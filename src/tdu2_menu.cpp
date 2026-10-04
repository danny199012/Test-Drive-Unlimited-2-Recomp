#include "tdu2_menu.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>

#include "tdu2_profile.h"

Tdu2MenuDialog::Tdu2MenuDialog(rex::ui::ImGuiDrawer* drawer)
    : rex::ui::ImGuiDialog(drawer) {}

int Tdu2MenuDialog::GetInt(const char* name, int fallback) const {
  std::string value = rex::cvar::GetFlagByName(name);
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stoi(value);
  } catch (const std::exception&) {
    return fallback;
  }
}

std::string Tdu2MenuDialog::GetString(const char* name) const {
  return rex::cvar::GetFlagByName(name);
}

void Tdu2MenuDialog::ApplyPreset(int width, int height) {
  // The guest video mode is what XGetVideoMode reports back to the game, so it
  // is the thing that actually changes the in-game render resolution. The
  // runtime reads it during setup, hence the restart note.
  rex::cvar::SetFlagByName("video_mode_width", std::to_string(width));
  rex::cvar::SetFlagByName("video_mode_height", std::to_string(height));
  status_ = "Render resolution set to " + std::to_string(width) + "x" +
            std::to_string(height) + ". Restart to apply.";
}

void Tdu2MenuDialog::ApplyWindow(int width, int height) {
  rex::cvar::SetFlagByName("window_width", std::to_string(width));
  rex::cvar::SetFlagByName("window_height", std::to_string(height));
  status_ = "Window set to " + std::to_string(width) + "x" +
            std::to_string(height) + ". Restart to apply.";
}

void Tdu2MenuDialog::OnDraw(ImGuiIO& io) {
  (void)io;

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->GetCenter().x - 190.0f, viewport->GetCenter().y - 190.0f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_Always);

  bool open = true;
  if (ImGui::Begin("TDU2 Settings  (F1 to close)", &open,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
    const int mode_w = GetInt("video_mode_width", 0);
    const int mode_h = GetInt("video_mode_height", 0);

    ImGui::TextUnformatted("Render resolution");
    ImGui::Separator();
    for (const auto& preset : kPresets) {
      const bool current = (preset.width == mode_w && preset.height == mode_h);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                          ImVec2(ImGui::GetFrameHeight() * 0.5f, 0.0f));
      if (ImGui::Button(preset.label, ImVec2(-1.0f, 0.0f))) {
        ApplyPreset(preset.width, preset.height);
      }
      ImGui::PopStyleVar();
      if (current) {
        ImGui::SameLine();
        ImGui::TextDisabled("(current)");
      }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Window size");
    ImGui::Separator();
    ImGui::BeginGroup();
    if (ImGui::Button("1280 x 720")) {
      ApplyWindow(1280, 720);
    }
    ImGui::SameLine();
    if (ImGui::Button("1600 x 900")) {
      ApplyWindow(1600, 900);
    }
    ImGui::SameLine();
    if (ImGui::Button("1920 x 1080")) {
      ApplyWindow(1920, 1080);
    }
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    bool fullscreen = GetString("fullscreen") == "true";
    if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
      rex::cvar::SetFlagByName("fullscreen", fullscreen ? "true" : "false");
      status_ = "Fullscreen updated. Restart to apply.";
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Profile");
    ImGui::Separator();

    // ReXGlue has no dashboard, so this identity is synthetic and local. The
    // guest only reads it through the overridden XamUser* imports, which read
    // the same cvars, so edits here take effect on the guest's next call.
    //
    // The name lives in a fixed buffer rather than a std::string: ImGui's
    // InputText needs a stable char* it can write into, and a string that is
    // re-read from the cvar every frame would be reset under the cursor.
    tdu2::Profile profile = tdu2::GetProfile();
    if (!name_edited_) {
      std::snprintf(name_buf_, sizeof(name_buf_), "%s", profile.name.c_str());
      std::snprintf(xuid_buf_, sizeof(xuid_buf_), "%llu",
                    static_cast<unsigned long long>(profile.xuid));
    }

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("Name", name_buf_, sizeof(name_buf_))) {
      name_edited_ = true;
      rex::cvar::SetFlagByName("profile_name", name_buf_);
      status_ = "Profile name updated.";
    }

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("XUID", xuid_buf_, sizeof(xuid_buf_))) {
      try {
        const unsigned long long parsed = std::stoull(xuid_buf_);
        rex::cvar::SetFlagByName("profile_xuid", std::to_string(parsed));
        status_ = "Profile XUID updated.";
      } catch (const std::exception&) {
        // Transient states while typing ("", "1_", "1844") are not errors worth
        // surfacing; the cvar keeps its last accepted value and the box is
        // re-seeded from it when the menu is reopened.
      }
    }

    bool signed_in = profile.signed_in;
    if (ImGui::Checkbox("Signed in", &signed_in)) {
      rex::cvar::SetFlagByName("profile_signed_in",
                               signed_in ? "true" : "false");
      status_ = signed_in ? "Profile reports signed in."
                          : "Profile reports signed out.";
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Local identity only - this is not an Xbox Live");
    ImGui::TextDisabled("account and does not authenticate to any server.");
    ImGui::TextDisabled("Saved to tdu2.toml when the game exits.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Every setting here needs a restart - the host reads");
    ImGui::TextDisabled("them at startup. F4 opens the full cvar editor.");
  }
  ImGui::End();

  if (!open) {
    Close();
  }
}