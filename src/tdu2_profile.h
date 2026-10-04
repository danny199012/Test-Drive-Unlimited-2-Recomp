// tdu2 - Synthetic dashboard profile.
//
// ReXGlue emulates no dashboard and no user accounts. The guest's profile-related
// imports (XamLoaderGetLaunchData, XamUserGetSigninState/GetXUID/GetName,
// XamUserReadProfileSettings) resolve into rexruntime.lib, which is a prebuilt
// binary we cannot patch from here. What we CAN do is own the identity values the
// guest observes, by registering our own host function over the generated import
// thunk address - the dispatcher's SetFunction is a plain map insert, so a later
// write for the same guest address wins.
//
// Scope, stated plainly: this gives the game a stable local identity that
// survives restarts. It is NOT an Xbox Live account and authenticates to
// nothing. Online play needs licensing, friends and auth layered on top.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace tdu2 {

// Guest-visible profile identity. Defaults match config/tdu2.toml.template.
struct Profile {
  std::string name;
  uint64_t xuid = 0;
  bool signed_in = false;
};

// Defines the profile_* cvars. Must run before the cvar TOML is parsed so the
// file's values bind to them - the app calls this from OnPreSetup.
void RegisterProfileCVars();

// Current profile, as read from the cvars.
Profile GetProfile();

// Writes the current profile back to tdu2.toml next to the exe, so edits made
// in the F1 menu survive a restart. Returns false if the file is not writable.
bool SaveProfileToConfig();

// Guest addresses of the profile-related import thunks in each module, as
// emitted by codegen. Overriding these is how the guest's identity queries are
// answered.
namespace thunk {
inline constexpr uint32_t kDefaultXamUserGetXUID = 0x8203A01C;
inline constexpr uint32_t kDefaultXamUserGetSigninInfo = 0x8203A03C;
inline constexpr uint32_t kDefaultXamLoaderGetLaunchData = 0x82039EFC;

inline constexpr uint32_t kDllXamLoaderGetLaunchData = 0x88FE6B74;
inline constexpr uint32_t kDllXamUserGetXUID = 0x88FE6FF4;
inline constexpr uint32_t kDllXamUserGetSigninState = 0x88FE7054;
inline constexpr uint32_t kDllXamUserGetName = 0x88FE7394;
inline constexpr uint32_t kDllXamUserGetSigninInfo = 0x88FE75A4;
}  // namespace thunk

// Installs host implementations for the profile imports listed above.
// `dispatcher` is a rex::runtime::FunctionDispatcher*. Returns the number of
// thunks successfully claimed; a low count means the guest still sees the
// runtime's built-in behaviour for the rest.
//
// Call after module registration has populated the function table.
size_t InstallProfileHooks(void* dispatcher);

// Same, but retries with a backoff until every hook lands or max_attempts is
// exhausted. This is the form the app uses: the launcher registers its function
// table during startup, but the game DLL is loaded by the guest afterwards and
// registers its own a couple of seconds later, so a single early attempt only
// ever reaches the launcher.
//
// Blocks for up to roughly (max_attempts * delay); call it off the UI thread.
size_t InstallProfileHooksWithRetry(void* dispatcher,
                                   std::chrono::milliseconds initial_delay,
                                   int max_attempts);

}  // namespace tdu2