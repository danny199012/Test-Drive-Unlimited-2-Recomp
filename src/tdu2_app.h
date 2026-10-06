// tdu2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
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

  // Dumps the render-to-texture settings, which govern whether content drawn
  // into an offscreen target (the in-game photo, the licence portrait) survives
  // long enough to be displayed. These live in the prebuilt GPU plugin, so
  // their defaults cannot be read from source here - logging them at startup is
  // the only way to see what is actually in force.
  //
  // Must run from OnPostSetup, not OnPreSetup: the GPU plugin registers its
  // cvars when it loads during runtime setup, so at OnPreSetup every one of
  // them still reads "<unregistered>".
  //
  // Uses the string-based accessor deliberately: the typed REXCVAR_DECLARE
  // accessors live in rex/graphics/flags.h and would tie this header to the
  // graphics module for no benefit.
  static void LogTextureCVars() {
    static const char* const kNames[] = {
        "direct_host_resolve",
        "texture_cache_memory_limit_render_to_texture",
        "texture_cache_memory_limit_soft",
        "gpu_allow_invalid_fetch_constants",
        "gpu_3d_to_2d_texture",
        "anisotropic_override",
    };
    for (const char* name : kNames) {
      const std::string value = rex::cvar::GetFlagByName(name);
      REXLOG_INFO("cvar {} = '{}'", name, value.empty() ? "<unregistered>" : value);
    }
  }

  // Logs the cvars that decide how vertex/index data, memexport writes and
  // shader pipelines reach the GPU. They are the candidate knobs for the
  // intro-scene skinned-mesh corruption (shredded triangles / black wedges on
  // the character): a wrong value here shows up as garbage vertex positions,
  // never as a log error, so the value and its compiled-in default have to be
  // readable from the log.
  //
  // Runs after LogTextureCVars for the same reason: the GPU plugin registers
  // these when it loads during runtime setup.
  static void LogVertexCorruptionCVars() {
    static const char* const kNames[] = {
        // GPU-written guest memory (memexport / resolve readback). If these
        // writes are not coherent the CPU and the GPU see different vertex or
        // matrix data for the same frame.
        "readback_memexport",
        "readback_memexport_fast",
        "d3d12_readback_memexport",
        "d3d12_readback_resolve",
        "clear_memory_page_state",
        // Vertex/index fetch and primitive processing.
        "gpu_allow_invalid_fetch_constants",
        "vfetch_full",
        "vfetch_mini",
        "xe_vertex_index_offset",
        "primitive_processor_cache_min_indices",
        "execute_unclipped_draw_vs_on_cpu",
        "execute_unclipped_draw_vs_on_cpu_with_scissor",
        "force_convert_quad_lists_to_triangle_lists",
        "force_convert_triangle_fans_to_lists",
        // Shader pipeline / translation and presentation geometry.
        "async_shader_compilation",
        "dump_shaders",
        "d3d12_dxbc_disasm",
        "half_pixel_offset",
        "resolution_scale",
        "direct_host_resolve",
    };
    for (const char* name : kNames) {
      const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
      if (info == nullptr) {
        REXLOG_INFO("cvar {} = <unregistered>", name);
        continue;
      }
      REXLOG_INFO("cvar {} = '{}' (default '{}', source {})", name,
                  rex::cvar::GetFlagByName(name), info->default_value,
                  SourceName(rex::cvar::GetFlagSource(name)));
    }

    // Full registry dump next to the exe: which cvars exist, what they hold
    // and what they would hold with no config at all. Written every run so a
    // stale file cannot be mistaken for the current one.
    std::ofstream dump("cvars_full.txt", std::ios::trunc);
    if (dump) {
      for (const rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
        dump << entry.name << " = '" << entry.getter() << "' (default '"
             << entry.default_value << "')\n";
      }
    }
  }

  // Maps cvar::Source to a printable token. A local helper rather than a cast
  // so the log reads "config" instead of "1".
  static const char* SourceName(rex::cvar::Source source) {
    switch (source) {
      case rex::cvar::Source::kDefault:
        return "default";
      case rex::cvar::Source::kConfig:
        return "config";
      case rex::cvar::Source::kEnvironment:
        return "env";
      case rex::cvar::Source::kCommandLine:
        return "cmdline";
      case rex::cvar::Source::kRuntime:
        return "runtime";
    }
    return "unknown";
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
    // The GPU plugin has loaded by now, so its cvars exist and their real
    // values can be read.
    LogTextureCVars();
    LogVertexCorruptionCVars();

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
