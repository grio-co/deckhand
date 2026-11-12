#pragma once
// Skin-driven UI: loads a .dhskin, solves its layout, and draws and drives the generic widgets (panels,
// labels, buttons, knobs, faders, meters). Widgets that need application data or special interaction
// (waveforms, jog, pads, track browser, playlist tree, text fields) are handed to the SkinHost.
// Everything here runs on the UI thread. Coordinates are logical points (the layout is solved at scale 1
// for the window size in points; the DrawList applies the backing scale).
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "deckhand/gfx/draw.hpp"
#include "deckhand/i18n/i18n.hpp"
#include "deckhand/skin/skin.hpp"

namespace deckhand::ui {

struct Input {
  float x = 0, y = 0, scroll = 0;
  bool down = false, pressed = false, released = false;
  int clicks = 0;
  bool shift = false;
};

gfx::Color to_gfx(skin::Color c, float opacity = 1.0f);
gfx::Color lerp_color(gfx::Color a, gfx::Color b, float t);

// Colours the application's own overlays and complex widgets use. Built from the skin's tokens when it
// defines the usual names (bg panel panel2 line text dim accent ...), else defaults.
struct Theme {
  gfx::Color bg, panel, panel2, line, text, dim, accent, warn, good, hover;
  gfx::Color deck[4];
  static Theme defaults();
  static Theme from_skin(const skin::Skin& sk);
};

enum class Special { None, TempoRange, FxSelect, Step, SetValue };

// What a skin binding means to the application. The ONE place that spells the translation between skin
// names ("deck[1].play", "mixer.ch[2].eq.high") and control-table names ("deck1.play", "deck2.eq_high") or
// application actions is map_binding() in skin_view.cpp.
struct Target {
  enum class Type { None, Control, Action, Info } type = Type::None;
  std::string name;  // control-table name (Control), action id (Action), value id (Info)
  int deck = -1;     // 0-based deck for deck[N]. / mixer.ch[N]. bindings
  int index = -1;    // 0-based effect slot
  double lo = 0, hi = 1, def = 0;  // value range and the double-click default
  bool invert = false;             // fader runs the other way (tempo: faster towards the bottom)
  bool momentary = false;          // button acts on press and release (CUE)
  Special special = Special::None;
  double step = 0;
  explicit operator bool() const { return type != Type::None; }
};
Target map_binding(std::string_view skin_binding);

class SkinHost {
 public:
  virtual ~SkinHost() = default;
  virtual bool read_number(const Target&, double& v) = 0;
  virtual bool read_text(const Target&, std::string& out) = 0;
  virtual bool read_flag(const Target&) = 0;  // on-state of a toggle, or a `show=` condition
  virtual void press(const Target&) = 0;
  virtual void release(const Target&) = 0;
  virtual void set_number(const Target&, double v) = 0;
  // Seconds since the last beat of the deck the widget is bound to (negative if none).
  virtual double beat_age_ms(const Target&) = 0;
  virtual void draw_custom(const skin::ResolvedWidget&, const Target&, const skin::Style&, const skin::Rect&) = 0;
};

class SkinView {
 public:
  SkinView(gfx::DrawList& dl, const i18n::Catalog& catalog) : dl_(dl), catalog_(catalog) {}

  // Loads a skin file or directory. Returns false with the messages in `error` when it does not parse.
  bool load(const std::string& path, std::string& error);
  bool loaded() const { return skin_ != nullptr; }
  const skin::Skin* skin() const { return skin_.get(); }
  const Theme& theme() const { return theme_; }
  bool has_window(const std::string& id) const { return skin_ && skin_->find_window(id); }
  void set_touch(bool t) { touch_ = t; }

  // Solves the layout for the window in points, inside the safe-area insets. Cheap when nothing changed.
  void layout(const std::string& window, float w, float h, float il, float it, float ir, float ib);
  const skin::Layout& current() const { return layout_; }
  float min_w() const { return min_w_; }
  float min_h() const { return min_h_; }

  void frame(SkinHost& host, const Input& in, double now_s);

  // Resolved text of a widget (label/button), shared with the host for its own widgets.
  std::string text_of(const skin::ResolvedWidget& w, const Target& t, SkinHost& host, bool on);
  void draw_label(const skin::Rect& r, const std::string& s, const skin::Style& st, gfx::Align align, bool vcenter = true);
  bool bold_font(const skin::Style& st) const;
  std::string fit(const std::string& s, float size, int weight, float max_w);
  bool pointer_over_interactive(float x, float y) const;

 private:
  struct Slot {
    Target target, show;
    bool show_negate = false;
    skin::Style shown;
    bool shown_valid = false;
    double started[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  };
  int pick(float x, float y) const;
  skin::Rect hit_rect(const skin::ResolvedWidget& w) const;
  void draw_widget(int i, SkinHost& host, const Input& in, double now_s, double dt_ms, bool hidden);
  void draw_knob(const skin::ResolvedWidget& w, const Target& t, const skin::Style& st, const skin::Rect& r, double v, bool bipolar,
                 const std::string& label);
  void draw_fader(const skin::ResolvedWidget& w, const skin::Style& st, const skin::Rect& r, double n, bool bipolar);
  void draw_meter(const skin::ResolvedWidget& w, const skin::Style& st, const skin::Rect& r, double level);
  void handle_drag(SkinHost& host, const Input& in);

  gfx::DrawList& dl_;
  const i18n::Catalog& catalog_;
  std::shared_ptr<const skin::Skin> skin_;
  Theme theme_ = Theme::defaults();
  skin::Layout layout_;
  std::vector<Slot> slots_;
  std::vector<char> hidden_;
  std::string window_;
  float lw_ = 0, lh_ = 0, insets_[4] = {0, 0, 0, 0}, min_w_ = 0, min_h_ = 0;
  bool touch_ = false;
  int hot_ = -1, active_ = -1;
  float drag_y_ = 0, drag_x_ = 0;
  double drag_start_ = 0;
  bool drag_moved_ = false;
  double last_now_ = 0;
};

}  // namespace deckhand::ui
