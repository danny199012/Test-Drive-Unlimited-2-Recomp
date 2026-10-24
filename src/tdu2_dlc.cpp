#include "tdu2_dlc.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

namespace tdu2 {

// X_ERROR_SUCCESS expands to an unqualified X_RESULT cast, so make the
// typedef visible here.
using rex::X_RESULT;

REXCVAR_DEFINE_STRING(tdu2_dlc_root, "", "TDU2",
                      "Folder of marketplace DLC packages (LIVE/CON/PIRS) to install into "
                      "the user data root at startup. Packages already extracted are skipped");

namespace {

// STFS package magics (XContentPackageType). Everything else in the folder is
// ignored, so the cvar can point at a mixed download directory.
bool IsStfsPackage(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  char magic[4] = {};
  file.read(magic, sizeof(magic));
  return std::memcmp(magic, "LIVE", 4) == 0 || std::memcmp(magic, "CON ", 4) == 0 ||
         std::memcmp(magic, "PIRS", 4) == 0;
}

// Directory entries sort by name; installing small packages first keeps the
// boot-time extraction window short and matches how the console layered
// unlock stubs over the big content packs.
struct PackageEntry {
  std::filesystem::path path;
  std::uintmax_t size;
};

}  // namespace

size_t InstallDlcPackages(rex::system::KernelState* kernel_state) {
  const std::string root = REXCVAR_GET(tdu2_dlc_root);
  if (root.empty() || kernel_state == nullptr || kernel_state->content_manager() == nullptr) {
    return 0;
  }

  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    REXLOG_WARN("TDU2 DLC: tdu2_dlc_root '{}' is not a directory; no DLC installed", root);
    return 0;
  }

  std::vector<PackageEntry> packages;
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (!entry.is_regular_file(ec) || !IsStfsPackage(entry.path())) {
      continue;
    }
    packages.push_back({entry.path(), entry.file_size(ec)});
  }
  if (packages.empty()) {
    REXLOG_WARN("TDU2 DLC: no STFS packages found in '{}'", root);
    return 0;
  }
  std::sort(packages.begin(), packages.end(),
            [](const PackageEntry& a, const PackageEntry& b) { return a.size < b.size; });

  // ContentExists resolves the same extracted-directory path the enumerator
  // scans, so "exists" covers every previous run regardless of which name the
  // display string used.
  size_t installed = 0;
  for (const auto& package : packages) {
    rex::system::xam::XCONTENT_AGGREGATE_DATA probe;
    probe.content_type = rex::system::XContentType::kMarketplaceContent;
    probe.title_id = rex::system::xam::kCurrentlyRunningTitleId;
    probe.xuid = 0;
    probe.set_file_name(rex::path_to_utf8(package.path.filename()));

    if (kernel_state->content_manager()->ContentExists(0, probe)) {
      REXLOG_INFO("TDU2 DLC: {} already installed", package.path.filename().string());
      continue;
    }

    REXLOG_INFO("TDU2 DLC: installing {} ({} MB)...", package.path.filename().string(),
                package.size / (1024 * 1024));
    const rex::X_RESULT result = kernel_state->content_manager()->InstallContent(package.path);
    if (result == X_ERROR_SUCCESS) {
      ++installed;
      REXLOG_INFO("TDU2 DLC: installed {}", package.path.filename().string());
    } else {
      REXLOG_ERROR("TDU2 DLC: install of {} failed with {:#x}", package.path.filename().string(),
                   result);
    }
  }

  REXLOG_INFO("TDU2 DLC: {} package(s) newly installed from {} ({} present)",
              installed, root, packages.size());
  return installed;
}

}  // namespace tdu2
