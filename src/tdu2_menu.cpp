#include "tdu2_menu.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include <rex/cvar.h>

#include "tdu2_config.h"
#include "tdu2_profile.h"

namespace {

// Every cvar the menu can change, for the startup snapshot.
constexpr const char* kTrackedCvars[] = {
    "video_mode_width",
    "video_mode_height",
    "window_width",
    "window_height",
    "fullscreen",
    "present_letterbox",
    "resolution_scale",
    "anisotropic_override",
    "swap_post_effect",
    "present_effect",
    "present_dither",
    "d3d12_allow_variable_refresh_rate_and_tearing",
    "async_shader_compilation",
    "readback_resolve",
    "d3d12_readback_resolve",
};

std::map<std::string, std::string>& StartupValues() {
  static std::map<std::string, std::string> values;
  return values;
}

bool NeedsRestartToApply(std::string_view cvar) {
  // Registered as hot-reloadable, but only read when the swap chain is created.
  if (cvar == "d3d12_allow_variable_refresh_rate_and_tearing") {
    return true;
  }
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(cvar);
  return info != nullptr && info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart;
}

bool IsPendingRestart(const std::string& cvar) {
  if (!NeedsRestartToApply(cvar)) {
    return false;
  }
  const auto& startup = StartupValues();
  auto it = startup.find(cvar);
  return it != startup.end() && it->second != rex::cvar::GetFlagByName(cvar);
}

int GetInt(const char* cvar, int fallback) {
  try {
    return std::stoi(rex::cvar::GetFlagByName(cvar));
  } catch (const std::exception&) {
    return fallback;
  }
}

// TDU2's orange, for the accents.
constexpr ImVec4 kAccent(0.95f, 0.55f, 0.13f, 1.00f);
constexpr ImVec4 kAccentHover(1.00f, 0.65f, 0.25f, 1.00f);
constexpr ImVec4 kAccentDim(0.55f, 0.32f, 0.08f, 1.00f);

constexpr float kLabelColumnWidth = 210.0f;

}  // namespace

Tdu2MenuDialog::Tdu2MenuDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> on_close)
    : rex::ui::ImGuiDialog(drawer), on_close_(std::move(on_close)) {}

void Tdu2MenuDialog::CaptureStartupValues() {
  auto& startup = StartupValues();
  for (const char* cvar : kTrackedCvars) {
    startup[cvar] = rex::cvar::GetFlagByName(cvar);
  }
}

void Tdu2MenuDialog::OnClose() {
  if (on_close_) {
    on_close_();
  }
}

bool Tdu2MenuDialog::Apply(std::initializer_list<std::pair<const char*, std::string>> changes) {
  std::vector<std::string> saved;
  for (const auto& [cvar, value] : changes) {
    if (!rex::cvar::SetFlagByName(cvar, value)) {
      status_ = std::string("Could not set ") + cvar + " to " + value + ".";
      return false;
    }
    saved.emplace_back(cvar);
  }
  if (!tdu2::SaveCvarsToConfig(saved)) {
    status_ = "Applied, but tdu2.toml could not be written.";
    return true;
  }
  bool restart = false;
  for (const auto& cvar : saved) {
    restart |= NeedsRestartToApply(cvar);
  }
  status_ = restart ? "Saved. Restart the game to apply." : "Applied and saved.";
  return true;
}

void Tdu2MenuDialog::RowLabel(const char* label, std::initializer_list<const char*> cvars) {
  bool pending = false;
  for (const char* cvar : cvars) {
    cvar_labels_[cvar] = label;
    pending |= IsPendingRestart(cvar);
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (pending) {
    ImGui::SameLine();
    ImGui::TextColored(kAccent, "(restart)");
  }
  ImGui::SameLine(kLabelColumnWidth);
  ImGui::SetNextItemWidth(-1.0f);
}

void Tdu2MenuDialog::RowHelp(const char* help) {
  if (help == nullptr) {
    return;
  }
  ImGui::Indent(kLabelColumnWidth);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("%s", help);
  ImGui::PopTextWrapPos();
  ImGui::Unindent(kLabelColumnWidth);
  ImGui::Spacing();
}

bool Tdu2MenuDialog::ChoiceRow(const char* label, const char* cvar,
                               std::span<const Choice> choices, const char* help) {
  const std::string current = rex::cvar::GetFlagByName(cvar);
  const char* preview = current.c_str();
  for (const auto& choice : choices) {
    if (current == choice.value) {
      preview = choice.label;
    }
  }
  bool changed = false;
  ImGui::PushID(cvar);
  RowLabel(label, {cvar});
  if (ImGui::BeginCombo("##value", preview)) {
    for (const auto& choice : choices) {
      const bool selected = current == choice.value;
      if (ImGui::Selectable(choice.label, selected) && !selected) {
        changed = Apply({{cvar, choice.value}});
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  RowHelp(help);
  return changed;
}

bool Tdu2MenuDialog::CheckboxRow(const char* label, const char* cvar, const char* help) {
  bool value = rex::cvar::GetFlagByName(cvar) == "true";
  bool changed = false;
  ImGui::PushID(cvar);
  RowLabel(label, {cvar});
  if (ImGui::Checkbox("##value", &value)) {
    changed = Apply({{cvar, value ? "true" : "false"}});
  }
  ImGui::PopID();
  RowHelp(help);
  return changed;
}

bool Tdu2MenuDialog::SizePresetRow(const char* label, const char* width_cvar,
                                   const char* height_cvar, std::span<const SizePreset> presets,
                                   const char* help) {
  const int width = GetInt(width_cvar, 0);
  const int height = GetInt(height_cvar, 0);
  char custom[32];
  std::snprintf(custom, sizeof(custom), "%d x %d", width, height);
  const char* preview = custom;
  for (const auto& preset : presets) {
    if (preset.width == width && preset.height == height) {
      preview = preset.label;
    }
  }
  bool changed = false;
  ImGui::PushID(width_cvar);
  RowLabel(label, {width_cvar, height_cvar});
  if (ImGui::BeginCombo("##value", preview)) {
    for (const auto& preset : presets) {
      const bool selected = preset.width == width && preset.height == height;
      if (ImGui::Selectable(preset.label, selected) && !selected) {
        changed = Apply({{width_cvar, std::to_string(preset.width)},
                         {height_cvar, std::to_string(preset.height)}});
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  RowHelp(help);
  return changed;
}

void Tdu2MenuDialog::DrawDisplayTab() {
  static constexpr SizePreset kGameResolutions[] = {
      {"1280 x 720 (native)", 1280, 720}, {"1600 x 900", 1600, 900},
      {"1920 x 1080", 1920, 1080},        {"2560 x 1440", 2560, 1440},
      {"3840 x 2160", 3840, 2160},
  };
  SizePresetRow("Game resolution", "video_mode_width", "video_mode_height", kGameResolutions,
                "The resolution the game believes it is rendering at. 720p is what the "
                "console used; higher is sharper but some effects and UI may not scale.");

  static constexpr SizePreset kWindowSizes[] = {
      {"1280 x 720", 1280, 720},   {"1600 x 900", 1600, 900},   {"1920 x 1080", 1920, 1080},
      {"2560 x 1440", 2560, 1440}, {"3840 x 2160", 3840, 2160},
  };
  SizePresetRow("Window size", "window_width", "window_height", kWindowSizes,
                "Size of the window when not fullscreen.");

  CheckboxRow("Fullscreen", "fullscreen", "Borderless fullscreen. Applies immediately.");
  CheckboxRow("Letterbox", "present_letterbox",
              "Keep the 16:9 picture with black bars when the window has a different "
              "shape, instead of stretching it.");
}

void Tdu2MenuDialog::DrawGraphicsTab() {
  static constexpr Choice kResolutionScales[] = {
      {"1x (native)", "1"},
      {"2x", "2"},
      {"3x", "3"},
  };
  ChoiceRow("Resolution scale", "resolution_scale", kResolutionScales,
            "Renders the 3D scene at a multiple of the game resolution. Much sharper, "
            "much heavier on the GPU; some post effects may shift slightly.");

  static constexpr Choice kAnisotropic[] = {
      {"Game default", "-1"}, {"Off", "0"}, {"1x", "1"}, {"2x", "2"},
      {"4x", "3"},            {"8x", "4"},  {"16x", "5"},
  };
  ChoiceRow("Anisotropic filtering", "anisotropic_override", kAnisotropic,
            "Keeps roads and ground textures sharp at shallow angles. Cheap on modern "
            "GPUs. Applies immediately.");

  static constexpr Choice kPostEffects[] = {
      {"Off", "none"},
      {"FXAA", "fxaa"},
      {"FXAA (strong)", "fxaa_extreme"},
  };
  ChoiceRow("Anti-aliasing", "swap_post_effect", kPostEffects,
            "Smooths jagged edges on the final image.");

  // The upscalers are only compiled in with the FidelityFX SDK, so offer what
  // this build actually has.
  static const std::pair<std::string_view, const char*> kEffectLabels[] = {
      {"bilinear", "Bilinear"},
      {"cas", "AMD CAS (sharpen)"},
      {"fsr", "AMD FSR 1"},
      {"fsr2", "AMD FSR 2"},
      {"fsr3", "AMD FSR 3"},
  };
  std::vector<std::string> allowed;
  if (const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo("present_effect")) {
    allowed = info->constraints.allowed_values;
  }
  std::vector<Choice> effects;
  for (const auto& value : allowed) {
    const char* label = value.c_str();
    for (const auto& [key, text] : kEffectLabels) {
      if (key == value) {
        label = text;
      }
    }
    effects.push_back({label, value.c_str()});
  }
  if (effects.size() > 1) {
    ChoiceRow("Upscaling", "present_effect", effects,
              "How the picture is scaled to the window.");
  } else {
    RowLabel("Upscaling", {"present_effect"});
    ImGui::TextDisabled("Bilinear only in this build");
    RowHelp("AMD CAS / FSR need the SDK built with REXGLUE_ENABLE_FIDELITYFX.");
  }

  CheckboxRow("Dithering", "present_dither",
              "Adds faint noise to the final image to hide colour banding in skies.");
}

void Tdu2MenuDialog::DrawPerformanceTab() {
  CheckboxRow("Variable refresh / tearing", "d3d12_allow_variable_refresh_rate_and_tearing",
              "Lets G-Sync / FreeSync displays show frames as soon as they are ready.");

  CheckboxRow("Background shader compiling", "async_shader_compilation",
              "Compiles new shaders without pausing the game: less stutter, but objects "
              "can be missing for a moment the first time they appear. Applies "
              "immediately.");

  // readback_resolve decides when the CPU sees render-to-texture results;
  // d3d12_readback_resolve has to be on for the D3D12 path to copy them at all,
  // so the two are always set together.
  static constexpr Choice kReadback[] = {
      {"Off (fastest)", "none"},
      {"Fast (one frame late)", "fast"},
      {"Balanced (on cache miss)", "some"},
      {"Full (accurate, slowest)", "full"},
  };
  const std::string readback = rex::cvar::GetFlagByName("readback_resolve");
  const char* preview = readback.c_str();
  for (const auto& choice : kReadback) {
    if (readback == choice.value) {
      preview = choice.label;
    }
  }
  ImGui::PushID("readback_resolve");
  RowLabel("Photo / licence images", {"readback_resolve", "d3d12_readback_resolve"});
  if (ImGui::BeginCombo("##value", preview)) {
    for (const auto& choice : kReadback) {
      const bool selected = readback == choice.value;
      if (ImGui::Selectable(choice.label, selected) && !selected) {
        const bool on = std::string_view(choice.value) != "none";
        Apply({{"readback_resolve", choice.value},
               {"d3d12_readback_resolve", on ? "true" : "false"}});
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  RowHelp(
      "TDU2 draws in-game photos and the licence portrait on the GPU and reads them "
      "back. Off leaves them blank; Full is exact but costs the most frame rate in "
      "busy scenes. Applies immediately.");
}

void Tdu2MenuDialog::DrawProfileTab() {
  // ReXGlue has no dashboard, so this identity is synthetic and local. The
  // guest only reads it through the overridden XamUser* imports, which read
  // the same cvars, so edits here take effect on the guest's next call.
  tdu2::Profile profile = tdu2::GetProfile();
  if (!name_edited_) {
    std::snprintf(name_buf_, sizeof(name_buf_), "%s", profile.name.c_str());
    std::snprintf(xuid_buf_, sizeof(xuid_buf_), "%llu",
                  static_cast<unsigned long long>(profile.xuid));
  }

  RowLabel("Name", {"profile_name"});
  if (ImGui::InputText("##name", name_buf_, sizeof(name_buf_))) {
    name_edited_ = true;
    Apply({{"profile_name", name_buf_}});
  }

  RowLabel("XUID", {"profile_xuid"});
  if (ImGui::InputText("##xuid", xuid_buf_, sizeof(xuid_buf_))) {
    try {
      const unsigned long long parsed = std::stoull(xuid_buf_);
      Apply({{"profile_xuid", std::to_string(parsed)}});
    } catch (const std::exception&) {
      // Transient states while typing ("", "1_", "1844") are not errors worth
      // surfacing; the cvar keeps its last accepted value and the box is
      // re-seeded from it when the menu is reopened.
    }
  }

  CheckboxRow("Signed in", "profile_signed_in", nullptr);

  ImGui::Spacing();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled(
      "Local identity only - this is not an Xbox Live account and does not "
      "authenticate to any server. Typing needs a keyboard.");
  ImGui::PopTextWrapPos();
}

void Tdu2MenuDialog::DrawFooter() {
  ImGui::Separator();

  std::string pending;
  for (const char* cvar : kTrackedCvars) {
    if (!IsPendingRestart(cvar)) {
      continue;
    }
    auto it = cvar_labels_.find(cvar);
    const std::string label = it != cvar_labels_.end() ? it->second : cvar;
    if (pending.find(label) == std::string::npos) {
      pending += pending.empty() ? label : ", " + label;
    }
  }
  ImGui::PushTextWrapPos(0.0f);
  if (!pending.empty()) {
    ImGui::TextColored(kAccent, "Restart to apply: %s", pending.c_str());
  }
  if (!status_.empty()) {
    ImGui::TextUnformatted(status_.c_str());
  }
  ImGui::PopTextWrapPos();

  ImGui::TextDisabled("LB / RB: tabs    D-pad: move    A: change    B: close");
  ImGui::TextDisabled("F1 or Back + Start opens this menu. F4: every setting.");
  if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
    Close();
  }
}

void Tdu2MenuDialog::OnDraw(ImGuiIO& io) {
  (void)io;

  // Tab switching on the shoulder buttons, like the game's own menus.
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
    requested_tab_ = (current_tab_ + kTabCount - 1) % kTabCount;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
    requested_tab_ = (current_tab_ + 1) % kTabCount;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  // Anchored by the top edge: tabs differ in height, and a centered window
  // would move the tab row every time the tab changes.
  ImGui::SetNextWindowPos(
      ImVec2(viewport->GetCenter().x, viewport->Pos.y + viewport->Size.y * 0.18f),
      ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Always);
  if (first_frame_) {
    ImGui::SetNextWindowFocus();
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kAccentDim);
  ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.20f, 0.20f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TabHovered, kAccentHover);
  ImGui::PushStyleColor(ImGuiCol_TabSelected, kAccent);
  ImGui::PushStyleColor(ImGuiCol_CheckMark, kAccent);
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kAccentDim);
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, kAccent);
  ImGui::PushStyleColor(ImGuiCol_NavCursor, kAccent);

  bool open = true;
  if (ImGui::Begin("TDU2 Settings", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                               ImGuiWindowFlags_NoSavedSettings)) {
    if (ImGui::BeginTabBar("tabs")) {
      static constexpr const char* kTabNames[kTabCount] = {"Display", "Graphics",
                                                           "Performance", "Profile"};
      for (int tab = 0; tab < kTabCount; ++tab) {
        ImGuiTabItemFlags flags =
            requested_tab_ == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(kTabNames[tab], nullptr, flags)) {
          current_tab_ = tab;
          ImGui::Spacing();
          switch (tab) {
            case kTabDisplay:
              DrawDisplayTab();
              break;
            case kTabGraphics:
              DrawGraphicsTab();
              break;
            case kTabPerformance:
              DrawPerformanceTab();
              break;
            case kTabProfile:
              DrawProfileTab();
              break;
          }
          ImGui::EndTabItem();
        }
      }
      requested_tab_ = -1;
      ImGui::EndTabBar();
    }
    DrawFooter();

    // B (or Escape) backs out of the menu, unless it was closing a combo.
    const bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (!popup_open && !popup_was_open_ && !ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
      open = false;
    }
    popup_was_open_ = popup_open;
  }
  ImGui::End();

  ImGui::PopStyleColor(8);
  ImGui::PopStyleVar(3);

  first_frame_ = false;
  if (!open) {
    Close();
  }
}
