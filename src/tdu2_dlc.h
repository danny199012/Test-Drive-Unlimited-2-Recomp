// tdu2 - Marketplace DLC installation.
//
// The retail DLC ships as signed STFS marketplace packages (LIVE/CON/PIRS,
// content type 00000002). The SDK's ContentManager already knows how to mount
// one and extract it into the layout the XAM content enumerator scans
// (user_data_root/0000000000000000/<title_id>/00000002/<file_name>/), plus
// write the .header file that carries the package's license bits - what it
// lacks is a caller. This module is that caller: point tdu2_dlc_root at a
// folder of packages and every one not yet installed is extracted at startup.

#pragma once

#include <cstddef>

namespace rex {
namespace system {
class KernelState;
}  // namespace system
}  // namespace rex

namespace tdu2 {

// Scans the tdu2_dlc_root cvar folder for STFS packages and installs any that
// the user data root does not already list. Skips non-package files and
// packages installed on a previous run, so it is idempotent and cheap to call
// every startup. Safe to call from any thread once the kernel state exists
// (OnPreLaunchModule); the guest may enumerate content concurrently, which at
// worst makes a half-extracted package visible a few seconds early.
//
// Returns the number of packages newly installed.
size_t InstallDlcPackages(rex::system::KernelState* kernel_state);

}  // namespace tdu2
