// tdu2 - F1 settings menu.
//
// A curated, tabbed settings screen for the options worth changing from inside
// the game: display, graphics, performance/accuracy and the synthetic profile.
// The SDK's F4 editor still exposes every cvar; this is the friendly subset.
//
// - Opens with F1, or Back + Start on a controller, and is fully navigable
//   with the controller (LB/RB switch tabs, B closes).
// - Every change is written to tdu2.toml straight away, keeping the file's
//   comments.
// - Settings the runtime only reads at startup are applied to the cvar and
//   listed as "restart to apply" until the game is restarted, rather than
//   pretending to have taken effect.

#pragma once

#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>

class Tdu2MenuDialog : public rex::ui::ImGuiDialog {
 public:
  // on_close runs when the menu closes itself (B, Escape, the Close button or
  // the window's X), just before the dialog deletes itself, so the owner can
  // drop its pointer without deleting it a second time.
  Tdu2MenuDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> on_close);

  // Records the values the tracked settings had when the game started, which
  // is what "restart to apply" is measured against. Call once after the GPU
  // plugin has registered its cvars, before the menu is first opened.
  static void CaptureStartupValues();

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  struct Choice {
    const char* label;
    const char* value;
  };
  // A preset that sets a width / height cvar pair.
  struct SizePreset {
    const char* label;
    int width;
    int height;
  };

  enum Tab : int { kTabDisplay, kTabGraphics, kTabPerformance, kTabProfile, kTabCount };

  void DrawDisplayTab();
  void DrawGraphicsTab();
  void DrawPerformanceTab();
  void DrawProfileTab();
  void DrawFooter();

  // One labelled row per setting. Each applies the change, saves it to
  // tdu2.toml and returns true when the value changed.
  bool ChoiceRow(const char* label, const char* cvar, std::span<const Choice> choices,
                 const char* help);
  bool CheckboxRow(const char* label, const char* cvar, const char* help);
  bool SizePresetRow(const char* label, const char* width_cvar, const char* height_cvar,
                     std::span<const SizePreset> presets, const char* help);
  void RowLabel(const char* label, std::initializer_list<const char*> cvars);
  void RowHelp(const char* help);

  // Applies and saves a group of cvar changes made together.
  bool Apply(std::initializer_list<std::pair<const char*, std::string>> changes);

  std::function<void()> on_close_;

  int current_tab_ = kTabDisplay;
  int requested_tab_ = -1;
  bool first_frame_ = true;
  // Popups (open combos) eat B themselves; only close the menu on a B press
  // that did not also close a popup this frame.
  bool popup_was_open_ = false;

  // Labels of rows by cvar, for the "restart to apply" footer.
  std::map<std::string, std::string> cvar_labels_;

  // Editable profile fields. Held as buffers because ImGui writes in place;
  // name_edited_ stops the cvar being re-read over the top while typing.
  char name_buf_[64] = {};
  char xuid_buf_[32] = {};
  bool name_edited_ = false;

  std::string status_;
};
