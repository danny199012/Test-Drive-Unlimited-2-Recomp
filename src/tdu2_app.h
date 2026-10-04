// tdu2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

// Included (not forward-declared): OnPostSetup constructs Tdu2MenuDialog with
// std::make_unique, which needs the complete type.
#include "tdu2_menu.h"
#include "tdu2_profile.h"

class Tdu2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Tdu2App>(new Tdu2App(ctx, "tdu2",
        PPCImageConfig));
  }

  // TDU2 is a heavily GPU-driven game: it calls VdInitializeRingBuffer,
  // VdSetGraphicsInterruptCallback and VdEnableRingBufferRPtrWriteBack very
  // early during startup. With no GPU emulation loaded the runtime stubs those
  // out ("no GPU emulation loaded (gpu_plugin not set); call ignored") and the
  // game never gets past initialisation.
  //
  // The default is empty, so request the Xenos plugin here. The build stages
  // rexgpu-xenos.dll (Release) / rexgpu-xenosd.dll (Debug) next to the exe via
  // rexglue_setup_target(tdu2 GPU_PLUGINS xenos).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.gpu_plugin = "xenos";

    // The profile_* cvars must exist before the runtime parses tdu2.toml,
    // otherwise the file's profile lines are silently dropped. The macros
    // register at static-init time; this just logs and confirms the values.
    tdu2::RegisterProfileCVars();
  }

  // The game opens "UPDATE:\" early. That is the Xbox 360's alias for the
  // title update container ($SystemUpdate). Without an update_data_root the
  // VFS has no device mounted there, so every resolve fails:
  //     [error] [fs] ResolvePath(UPDATE:\) failed - device not found
  // Point it at the redump's $SystemUpdate folder when one is present, so
  // the guest sees the same layout it expects.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    namespace fs = std::filesystem;

    if (paths.update_data_root.empty()) {
      // Default to <game root>\$SystemUpdate when it exists; otherwise leave
      // it empty and the runtime logs the device-not-found error once.
      if (!paths.game_data_root.empty()) {
        fs::path candidate = paths.game_data_root / "$SystemUpdate";
        std::error_code ec;
        if (fs::is_directory(candidate, ec)) {
          paths.update_data_root = candidate;
        }
      }
    }
  }

  // F1 toggles the curated quick-settings panel (render resolution, window
  // size, fullscreen). F4, which the SDK registers by default, is the full cvar
  // editor and covers everything this does plus far more - this is just the
  // handful of things worth one keypress mid-game.
  void OnPostSetup() override {
    // Take over the guest's profile imports. This has to happen after the
    // modules have registered their function tables (OnPostLoadXexImage) but
    // before the guest starts calling them, so it is installed from
    // OnPreLaunchModule below.
    rex::ui::RegisterBind(
        "bind_tdu2_menu", "F1", "Toggle TDU2 quick settings menu",
        [this] {
          if (tdu2_menu_) {
            tdu2_menu_.reset();
          } else if (imgui_drawer()) {
            tdu2_menu_ = std::make_unique<Tdu2MenuDialog>(imgui_drawer());
          }
        });
  }

  // Installs the synthetic-profile overrides over the guest's XamUser* imports.
  //
  // The launcher registers its function table during startup, but the game DLL
  // is loaded by the guest a couple of seconds later, so hooking only once from
  // OnPreLaunchModule reaches just the launcher. This runs the retrying install
  // on its own thread instead, and joins it on shutdown.
  void OnPreLaunchModule() override {
    if (runtime() == nullptr) {
      return;
    }
    profile_hook_thread_ = std::thread([this] {
      tdu2::InstallProfileHooksWithRetry(runtime()->function_dispatcher(),
                                         std::chrono::milliseconds(500), 6);
    });
  }

  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_tdu2_menu");
    tdu2_menu_.reset();

    if (profile_hook_thread_.joinable()) {
      profile_hook_thread_.join();
    }

    // Persist any profile edits made in the F1 menu.
    tdu2::SaveProfileToConfig();
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;

 private:
  // Held by the app; the ImGui drawer draws it every frame.
  std::unique_ptr<Tdu2MenuDialog> tdu2_menu_;

  // Installs the profile import overrides once the game DLL has registered.
  std::thread profile_hook_thread_;
};
