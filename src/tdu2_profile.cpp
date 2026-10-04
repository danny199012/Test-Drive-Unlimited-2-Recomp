#include "tdu2_profile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>

namespace tdu2 {
namespace {

// Cvars. Defined here (not in a header) so the category shows up in the SDK's F4
// editor alongside the runtime's own settings, and so the TOML values bind to
// them during startup parsing.
REXCVAR_DEFINE_STRING(profile_name, "ReXGlue Player", "TDU2",
                     "Gamer tag the game sees for the synthetic profile");
REXCVAR_DEFINE_UINT64(profile_xuid, 2533274793222573ull, "TDU2",
                       "XUID the game sees for the synthetic profile");
REXCVAR_DEFINE_BOOL(profile_signed_in, true, "TDU2",
                    "Report the synthetic profile as signed in");

// Guest memory helpers. These mirror the REX_STORE_* macros in the generated PCH
// but are written out here so this file does not depend on generated headers,
// which are regenerated on every build. The PowerPC guest is big-endian, hence
// the byte swaps, and the 0xE0000000+ region needs a 0x1000 host bias.
inline uint8_t* GuestPtr(uint8_t* base, uint32_t addr) {
  const uint32_t bias = (addr >= 0xE0000000u) ? 0x1000u : 0u;
  return base + addr + bias;
}

void StoreU32(uint8_t* base, uint32_t addr, uint32_t value) {
  *reinterpret_cast<volatile uint32_t*>(GuestPtr(base, addr)) =
      __builtin_bswap32(value);
}

void StoreU64(uint8_t* base, uint32_t addr, uint64_t value) {
  *reinterpret_cast<volatile uint64_t*>(GuestPtr(base, addr)) =
      __builtin_bswap64(value);
}

// UTF-16LE store, one code unit at a time. XamUserGetName takes a wchar_t buffer
// and a capacity in characters, and null-terminates.
void StoreU16String(uint8_t* base, uint32_t addr, const std::string& utf8,
                    uint32_t capacity) {
  uint32_t written = 0;
  // Minimal UTF-8 -> UTF-16 conversion. Names are short and effectively ASCII;
  // anything outside that is passed through as U+FFFD rather than dropped.
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = static_cast<uint8_t>(utf8[i]);
    uint16_t unit = 0xFFFD;
    size_t len = 1;
    if (c < 0x80) {
      unit = c;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      unit = static_cast<uint16_t>(((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F));
      len = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
      unit = static_cast<uint16_t>(((c & 0x0F) << 12) |
                                   ((utf8[i + 1] & 0x3F) << 6) |
                                   (utf8[i + 2] & 0x3F));
      len = 3;
    }
    i += len;
    if (written + 1 >= capacity) {
      break;  // leave room for the terminator
    }
    *reinterpret_cast<volatile uint16_t*>(
        GuestPtr(base, addr + written * 2)) = unit;
    ++written;
  }
  *reinterpret_cast<volatile uint16_t*>(GuestPtr(base, addr + written * 2)) = 0;
}

// X_STATUS return codes. X_STATUS_SUCCESS is 0 on the 360; the result is
// carried in r3.
constexpr int32_t kStatusSuccess = 0;
constexpr int32_t kStatusInvalidParameter = static_cast<int32_t>(0xC000000D);
constexpr int32_t kStatusNoSuchUser = static_cast<int32_t>(0xC00002E5);

// XamSigninState: 0 = signed out, 1 = signed in with a profile selected. The
// guest compares the low byte against 1 to decide whether to show the dashboard
// sign-in flow, so a signed-in synthetic profile must report 1.
constexpr uint32_t kSigninStateSignedIn = 1;
constexpr uint32_t kSigninStateSignedOut = 0;

// --- Guest ABI shims --------------------------------------------------------
//
// Each replaces a runtime built-in import at its thunk address (see the thunk
// namespace in tdu2_profile.h). The PPC ABI returns its result in r3.
//

// XamUserGetSigninState(uint32_t user_index) -> X_STATUS. arg0 = r3.
void PPC_XamUserGetSigninState(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const uint32_t user_index = static_cast<uint32_t>(ctx.r3.u64);
  if (user_index > 0) {
    ctx.r3.s64 = kStatusNoSuchUser;
    return;
  }
  ctx.r3.s64 =
      GetProfile().signed_in ? kSigninStateSignedIn : kSigninStateSignedOut;
}

// XamUserGetXUID(uint32_t user_index, bool* online, bool* offline) -> X_STATUS.
// arg0 = r3 (user index), arg1 = r4 (online ptr), arg2 = r5 (offline ptr).
void PPC_XamUserGetXUID(PPCContext& ctx, uint8_t* base) {
  const uint32_t user_index = static_cast<uint32_t>(ctx.r3.u64);
  const uint32_t online_ptr = static_cast<uint32_t>(ctx.r4.u64);
  const uint32_t offline_ptr = static_cast<uint32_t>(ctx.r5.u64);

  const Profile profile = GetProfile();
  if (user_index > 0 || (!profile.signed_in && profile.xuid == 0)) {
    ctx.r3.s64 = kStatusNoSuchUser;
    return;
  }

  // A null flag pointer is legal: the guest passes null when it only wants the
  // status code back.
  if (online_ptr != 0) {
    StoreU32(base, online_ptr, profile.signed_in ? 1u : 0u);
  }
  if (offline_ptr != 0) {
    StoreU32(base, offline_ptr, 0u);
  }
  ctx.r3.s64 = kStatusSuccess;
}

// XamUserGetName(uint32_t user_index, wchar_t* buffer, uint32_t capacity).
// arg0 = r3 (user index), arg1 = r4 (buffer), arg2 = r5 (capacity in chars).
void PPC_XamUserGetName(PPCContext& ctx, uint8_t* base) {
  const uint32_t user_index = static_cast<uint32_t>(ctx.r3.u64);
  const uint32_t buffer = static_cast<uint32_t>(ctx.r4.u64);
  const uint32_t capacity = static_cast<uint32_t>(ctx.r5.u64);

  const Profile profile = GetProfile();
  if (user_index > 0 || buffer == 0 || capacity == 0) {
    ctx.r3.s64 = kStatusInvalidParameter;
    return;
  }
  // Empty name when signed out, matching a real dashboard read.
  StoreU16String(base, buffer, profile.signed_in ? profile.name : std::string(),
                 capacity);
  ctx.r3.s64 = kStatusSuccess;
}

// XamUserGetSigninInfo(uint32_t user_index, X_SIGNIN_INFO* info).
// arg0 = r3 (user index), arg1 = r4 (info pointer).
//
// X_SIGNIN_INFO is 0x18 bytes: flags, xuid, sign-in state, name (16 chars),
// and a state-reason field. Only the identity fields are filled in; the rest are
// left zero, which reads as "no privileges, no live connection".
void PPC_XamUserGetSigninInfo(PPCContext& ctx, uint8_t* base) {
  const uint32_t user_index = static_cast<uint32_t>(ctx.r3.u64);
  const uint32_t info = static_cast<uint32_t>(ctx.r4.u64);

  const Profile profile = GetProfile();
  if (user_index > 0 || info == 0) {
    ctx.r3.s64 = kStatusInvalidParameter;
    return;
  }

  StoreU32(base, info + 0x00, profile.signed_in ? 1u : 0u);  // flags
  StoreU64(base, info + 0x04, profile.xuid);                // xuid
  StoreU32(base, info + 0x0C, profile.signed_in ? kSigninStateSignedIn
                                                : kSigninStateSignedOut);
  if (profile.signed_in) {
    StoreU16String(base, info + 0x10, profile.name, 16);
  }
  StoreU32(base, info + 0x14, 0);  // state reason
  ctx.r3.s64 = kStatusSuccess;
}

}  // namespace

void RegisterProfileCVars() {
  // The REXCVAR_DEFINE_* macros register during static init; this call site
  // exists so the app has an explicit hook, and the log line confirms the
  // cvars are present (they are what the TOML binds to and what F4 lists).
  REXLOG_INFO("TDU2 profile cvars registered (name={}, xuid={}, signed_in={})",
              REXCVAR_GET(profile_name), REXCVAR_GET(profile_xuid),
              REXCVAR_GET(profile_signed_in));
}

Profile GetProfile() {
  Profile profile;
  profile.name = REXCVAR_GET(profile_name);
  profile.xuid = REXCVAR_GET(profile_xuid);
  profile.signed_in = REXCVAR_GET(profile_signed_in);
  return profile;
}

namespace {

// Replaces the value of a flat `key = ...` line, preserving everything else in
// the file including comments and spacing around the `=`. Returns false when
// the key was not present.
//
// The separator is matched with optional surrounding whitespace: a hand-written
// config normally reads `key = value`, while this function used to write
// `key=value`. Matching the bare `key=` prefix silently missed the former and
// appended a duplicate key instead of replacing it.
bool ReplaceTomlValue(std::vector<std::string>& lines, const std::string& key,
                      const std::string& value, bool quoted) {
  for (auto& line : lines) {
    if (line.rfind(key, 0) != 0) {
      continue;
    }
    size_t pos = key.size();
    while (pos < line.size() &&
           (line[pos] == ' ' || line[pos] == '\t')) {
      ++pos;
    }
    if (pos >= line.size() || line[pos] != '=') {
      continue;  // a different key that merely starts with the same text
    }
    line = key + " = " + (quoted ? "\"" + value + "\"" : value);
    return true;
  }
  return false;
}

}  // namespace

bool SaveProfileToConfig() {
  namespace fs = std::filesystem;
  std::error_code ec;

  // The runtime reads tdu2.toml from next to the exe, so that is the file that
  // has to change for the edit to survive a restart.
  const fs::path config = fs::current_path(ec) / "tdu2.toml";
  if (ec) {
    REXLOG_WARN("Could not resolve current directory: {}", ec.message());
    return false;
  }
  if (!fs::exists(config, ec)) {
    REXLOG_WARN("No tdu2.toml next to the exe; profile not saved");
    return false;
  }

  std::ifstream in(config, std::ios::binary);
  if (!in) {
    REXLOG_WARN("Could not open tdu2.toml for reading");
    return false;
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();  // avoid accumulating CR in CRLF files
    }
    lines.push_back(line);
  }

  const Profile profile = GetProfile();
  const std::string xuid_text = std::to_string(profile.xuid);
  const std::string signed_text = profile.signed_in ? "true" : "false";

  // Append any key that was missing, so the profile always round-trips even
  // against a config written before these settings existed.
  struct KeyValue {
    const char* key;
    const std::string value;
    bool quoted;
  };
  const KeyValue keys[] = {
      {"profile_name", profile.name, true},
      {"profile_xuid", xuid_text, false},
      {"profile_signed_in", signed_text, false},
  };
  for (const auto& kv : keys) {
    if (!ReplaceTomlValue(lines, kv.key, kv.value, kv.quoted)) {
      lines.push_back(std::string(kv.key) + "=" +
                      (kv.quoted ? "\"" + kv.value + "\"" : kv.value));
    }
  }

  std::ofstream out(config, std::ios::binary | std::ios::trunc);
  if (!out) {
    REXLOG_WARN("Could not open tdu2.toml for writing");
    return false;
  }
  for (const auto& l : lines) {
    out << l << "\n";
  }
  out.close();
  if (!out) {
    REXLOG_WARN("Failed while writing tdu2.toml");
    return false;
  }

  REXLOG_INFO("Saved profile to tdu2.toml (name={}, xuid={})", profile.name,
              profile.xuid);
  return true;
}

namespace {

struct Hook {
  uint32_t address;
  PPCFunc* func;
  const char* name;
};

// Both modules import these. The Default.xex thunks are listed as well so a
// direct launch of the launcher still reports a coherent identity.
const Hook kHooks[] = {
    {thunk::kDefaultXamUserGetXUID, PPC_XamUserGetXUID, "XamUserGetXUID"},
    {thunk::kDefaultXamUserGetSigninInfo, PPC_XamUserGetSigninInfo,
     "XamUserGetSigninInfo"},
    {thunk::kDllXamUserGetXUID, PPC_XamUserGetXUID, "XamUserGetXUID"},
    {thunk::kDllXamUserGetSigninState, PPC_XamUserGetSigninState,
     "XamUserGetSigninState"},
    {thunk::kDllXamUserGetName, PPC_XamUserGetName, "XamUserGetName"},
    {thunk::kDllXamUserGetSigninInfo, PPC_XamUserGetSigninInfo,
     "XamUserGetSigninInfo"},
};
constexpr size_t kHookCount = sizeof(kHooks) / sizeof(kHooks[0]);

// Replaces each thunk that currently resolves. Safe to call repeatedly: a
// thunk that is already ours is rewritten to the same function, and one whose
// module has not registered yet is simply skipped.
size_t InstallProfileHooksOn(rex::runtime::FunctionDispatcher* dispatcher) {
  size_t installed = 0;
  for (const auto& hook : kHooks) {
    // SetFunction reports false for an address outside every registered module
    // range, which is the expected case for a module that has not loaded yet.
    if (dispatcher->SetFunction(hook.address, hook.func)) {
      ++installed;
    }
  }
  return installed;
}

}  // namespace

size_t InstallProfileHooks(void* dispatcher_ptr) {
  auto* dispatcher =
      static_cast<rex::runtime::FunctionDispatcher*>(dispatcher_ptr);
  if (dispatcher == nullptr) {
    REXLOG_WARN("InstallProfileHooks: null dispatcher");
    return 0;
  }
  return InstallProfileHooksOn(dispatcher);
}

// Retries until every hook lands or the attempts run out.
//
// Why this is needed: Default.xex registers its function table during startup,
// but the game DLL is loaded by the guest afterwards and registers its own
// table a couple of seconds later. A single attempt from OnPreLaunchModule only
// ever reaches the launcher's imports and leaves the game reading the runtime's
// built-in behaviour. SetFunction is safe to call from another thread (the
// dispatcher takes an internal lock), so retrying off the UI thread is fine.
size_t InstallProfileHooksWithRetry(void* dispatcher_ptr,
                                   std::chrono::milliseconds initial_delay,
                                   int max_attempts) {
  auto* dispatcher =
      static_cast<rex::runtime::FunctionDispatcher*>(dispatcher_ptr);
  if (dispatcher == nullptr) {
    REXLOG_WARN("InstallProfileHooksWithRetry: null dispatcher");
    return 0;
  }

  std::chrono::milliseconds delay = initial_delay;
  size_t best = 0;
  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    std::this_thread::sleep_for(delay);
    const size_t installed = InstallProfileHooksOn(dispatcher);
    best = std::max(best, installed);
    if (installed >= kHookCount) {
      REXLOG_INFO("All {} profile hooks active after {} attempt(s)",
                  kHookCount, attempt);
      return installed;
    }
    // Back off gently; the DLL normally appears within the first few seconds.
    delay = std::min(delay * 2, std::chrono::milliseconds(4000));
  }

  REXLOG_WARN("Only {}/{} profile hooks active after {} attempts; the guest "
              "will read the runtime's built-in identity for the rest",
              best, kHookCount, max_attempts);
  return best;
}

}  // namespace tdu2