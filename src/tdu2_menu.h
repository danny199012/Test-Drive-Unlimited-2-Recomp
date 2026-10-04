// tdu2 - F1 quick settings overlay.
//
// The SDK already ships a generic cvar editor on F4 (rex::ui::SettingsDialog)
// which can change anything. This is a small curated panel for the settings
// you actually reach for mid-game: render resolution, window size, fullscreen.
//
// Most of these are host-side presentation values that the runtime does not
// re-read while running, so changes are applied where they can be and the rest
// are flagged "restart to apply" rather than silently pretending to work.

#pragma once

#include <string>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>

class Tdu2MenuDialog : public rex::ui::ImGuiDialog {
 public:
  explicit Tdu2MenuDialog(rex::ui::ImGuiDrawer* drawer);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Preset render resolutions offered as buttons.
  struct Preset {
    const char* label;
    int width;
    int height;
  };

  static constexpr Preset kPresets[] = {
      {"640 x 480", 640, 480},   {"720p", 1280, 720},    {"1080p", 1920, 1080},
      {"1440p", 2560, 1440},    {"4K", 3840, 2160},
  };

  void ApplyPreset(int width, int height);
  void ApplyWindow(int width, int height);

  int GetInt(const char* name, int fallback) const;
  std::string GetString(const char* name) const;

  // Editable profile fields. Held as buffers because ImGui writes in place;
  // name_edited_ stops the cvar being re-read over the top while typing.
  char name_buf_[64] = {};
  char xuid_buf_[32] = {};
  bool name_edited_ = false;

  std::string status_;
};