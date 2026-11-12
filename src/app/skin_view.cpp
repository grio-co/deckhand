#include "skin_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace deckhand::ui {

namespace {

constexpr float kPi = 3.14159265358979f;

gfx::Color gfx_rgba(int r, int g, int b, int a = 255) { return gfx::rgba(r, g, b, a); }

struct Seg {
  std::string name;
  int idx = -1;  // 0-based; -1 = none
};

std::vector<Seg> split_binding(std::string_view n) {
  std::vector<Seg> out;
  std::size_t i = 0;
  while (i <= n.size()) {
    std::size_t dot = n.find('.', i);
    std::string_view part = n.substr(i, dot == std::string_view::npos ? std::string_view::npos : dot - i);
    Seg s;
    const std::size_t br = part.find('[');
    if (br == std::string_view::npos) {
      s.name = std::string(part);
    } else {
      s.name = std::string(part.substr(0, br));
      s.idx = std::atoi(std::string(part.substr(br + 1)).c_str()) - 1;
      if (s.idx < 0) s.idx = -2;  // [0] is not a valid 1-based index
    }
    out.push_back(std::move(s));
    if (dot == std::string_view::npos) break;
    i = dot + 1;
  }
  return out;
}

Target control(const std::string& name, int deck = -1, double lo = 0, double hi = 1, double def = 0) {
  Target t;
  t.type = Target::Type::Control;
  t.name = name;
  t.deck = deck;
  t.lo = lo;
  t.hi = hi;
  t.def = def;
  return t;
}
Target action(const std::string& id, int deck = -1) {
  Target t;
  t.type = Target::Type::Action;
  t.name = id;
  t.deck = deck;
  return t;
}
Target info(const std::string& id, int deck = -1) {
  Target t;
  t.type = Target::Type::Info;
  t.name = id;
  t.deck = deck;
  return t;
}

std::string number_text(double v, const std::string& format) {
  char buf[64];
  if (format.size() == 6 && format.compare(0, 5, "fixed") == 0) {
    std::snprintf(buf, sizeof buf, "%.*f", format[5] - '0', v);
  } else if (format == "int") {
    std::snprintf(buf, sizeof buf, "%.0f", v);
  } else if (format == "time") {
    const int total = static_cast<int>(std::fabs(v));
    std::snprintf(buf, sizeof buf, "%s%d:%02d", v < 0 ? "-" : "", total / 60, total % 60);
  } else if (format == "percent") {
    std::snprintf(buf, sizeof buf, "%.0f%%", v * 100);
  } else if (format == "signed") {
    std::snprintf(buf, sizeof buf, "%+.1f", v);
  } else if (format == "db") {
    std::snprintf(buf, sizeof buf, "%.1f dB", v);
  } else {
    std::snprintf(buf, sizeof buf, "%g", v);
  }
  return buf;
}

}  // namespace

// ---- the one binding translation table ---------------------------------------------------------------
Target map_binding(std::string_view n) {
  const std::vector<Seg> s = split_binding(n);
  if (s.empty()) return {};
  const std::string& g = s[0].name;
  const auto rest = [&](std::size_t from) {
    std::string r;
    for (std::size_t i = from; i < s.size(); ++i) r += (r.empty() ? "" : ".") + s[i].name;
    return r;
  };

  if (g == "deck" && s[0].idx >= 0 && s.size() >= 2) {
    const int d = s[0].idx;
    const std::string dn = "deck" + std::to_string(d + 1);
    const std::string k = rest(1);
    for (const char* tog : {"play", "sync", "key_lock", "quantize", "slip", "reverse"})
      if (k == tog) return control(dn + "." + k, d);
    if (k == "cue") {
      Target t = control(dn + ".cue", d);
      t.momentary = true;
      return t;
    }
    if (k == "tempo") {
      Target t = control(dn + ".tempo", d, -1, 1, 0);
      t.invert = true;  // like the hardware: pull down to speed up
      return t;
    }
    if (k == "tempo_range") {
      Target t = control(dn + ".tempo_range", d);
      t.special = Special::TempoRange;
      return t;
    }
    if (k == "key_shift") return info("key_shift", d);
    if (k == "key_shift.minus" || k == "key_shift.plus") {
      Target t = control(dn + ".key_shift", d, -12, 12, 0);
      t.special = Special::Step;
      t.step = k == "key_shift.plus" ? 1 : -1;
      return t;
    }
    if (k == "key_shift.reset") {
      Target t = control(dn + ".key_shift", d, -12, 12, 0);
      t.special = Special::SetValue;
      return t;
    }
    if (s[1].name == "fx" && s[1].idx >= 0 && s.size() == 3) {
      const int slot = s[1].idx;
      const std::string fx = dn + ".fx" + std::to_string(slot + 1) + ".";
      Target t;
      if (s[2].name == "select") {
        t = control(fx + "select", d, 0, 14, 0);
        t.special = Special::FxSelect;
      } else if (s[2].name == "on") {
        t = control(fx + "on", d);
      } else if (s[2].name == "wet") {
        t = control(fx + "wet", d, 0, 1, 0);
      } else if (s[2].name == "name") {
        t = info("fx_name", d);
      } else {
        return {};
      }
      t.index = slot;
      return t;
    }
    if (k == "load") return action("load", d);
    if (k == "eject") return control(dn + ".eject", d);
    for (const char* i : {"waveform", "jog", "pads", "letter", "title", "artist", "bpm", "key", "time", "loop", "loaded", "elapsed", "remaining"})
      if (k == i) return info(i, d);
    return {};
  }

  if (g == "mixer") {
    if (s.size() == 3 && s[1].name == "ch" && s[1].idx >= 0) {
      const int d = s[1].idx;
      const std::string dn = "deck" + std::to_string(d + 1) + ".";
      const std::string k = s[2].name;
      if (k == "trim") return control(dn + "trim", d, 0, 1, 0.5);
      if (k == "filter") return control(dn + "filter", d, 0, 1, 0.5);
      if (k == "fader") return control(dn + "fader", d, 0, 1, 1.0);
      if (k == "cue") return control(dn + "pfl", d);
      if (k == "level") return info("ch_level", d);
    }
    if (s.size() == 4 && s[1].name == "ch" && s[1].idx >= 0 && s[2].name == "eq") {
      const int d = s[1].idx;
      const std::string k = s[3].name;
      if (k == "high" || k == "mid" || k == "low") return control("deck" + std::to_string(d + 1) + ".eq_" + k, d, 0, 1, 0.5);
    }
    const std::string k = rest(1);
    if (k == "crossfader") return control("mixer.crossfader", -1, 0, 1, 0.5);
    if (k == "headphone.volume") return control("mixer.head_level", -1, 0, 1, 0.8);
    if (k == "headphone.mix") return control("mixer.head_mix", -1, 0, 1, 0.5);
    if (k == "master.level") return info("master_level");
    if (k == "master.volume") return control("mixer.master_level", -1, 0, 1, 0.8);
    if (k == "sampler.level") return control("mixer.sampler_level", -1, 0, 1, 0.8);
    return {};
  }

  if (g == "app" && s.size() == 2) {
    const std::string& k = s[1].name;
    for (const char* a : {"import", "rec", "export", "language", "skin", "layout", "settings", "sampler", "learn"})
      if (k == a) return action(a);
    for (const char* i : {"controller", "audio", "status", "has_import", "multi_language", "multi_skin", "has_four", "rec_time", "tracks_loading"})
      if (k == i) return info(i);
    return {};
  }

  if (g == "browser") {
    const std::string k = rest(1);
    if (k == "tree" || k == "tracks" || k == "search" || k == "count") return info(k);
    if (k == "add_to") return action("add_to");
    if (k == "remove") return action("remove");
    if (k == "can_remove") return info("can_remove");
    if (k == "playlist.add_list") return action("playlist_add_list");
    if (k == "playlist.add_folder") return action("playlist_add_folder");
    if (k == "playlist.rename") return action("playlist_rename");
    if (k == "playlist.delete") return action("playlist_delete");
    if (k == "edit") return action("edit_track");
    return {};
  }

  if (g == "sampler") {
    const std::string k = rest(1);
    if (k == "pads") return info("sampler_pads");
    if (k == "level") return control("mixer.sampler_level", -1, 0, 1, 0.8);
    if (k == "stop") return action("sampler_stop");
    return {};
  }
  return {};
}

// ---- colours -----------------------------------------------------------------------------------------
gfx::Color to_gfx(skin::Color c, float opacity) {
  const int a = static_cast<int>(std::lround(static_cast<float>(c & 0xFF) * std::clamp(opacity, 0.0f, 1.0f)));
  return gfx::rgba(static_cast<int>((c >> 24) & 0xFF), static_cast<int>((c >> 16) & 0xFF), static_cast<int>((c >> 8) & 0xFF), a);
}

gfx::Color lerp_color(gfx::Color a, gfx::Color b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  gfx::Color out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
    out |= static_cast<gfx::Color>(std::lround(x + (y - x) * t)) << shift;
  }
  return out;
}

Theme Theme::defaults() {
  Theme t;
  t.bg = gfx_rgba(16, 18, 22);
  t.panel = gfx_rgba(25, 28, 34);
  t.panel2 = gfx_rgba(35, 39, 47);
  t.line = gfx_rgba(52, 57, 66);
  t.text = gfx_rgba(232, 234, 238);
  t.dim = gfx_rgba(136, 143, 154);
  t.accent = gfx_rgba(70, 130, 255);
  t.warn = gfx_rgba(255, 96, 96);
  t.good = gfx_rgba(80, 210, 120);
  t.hover = gfx_rgba(255, 255, 255, 20);
  t.deck[0] = gfx_rgba(46, 211, 183);
  t.deck[1] = gfx_rgba(255, 176, 46);
  t.deck[2] = gfx_rgba(110, 150, 255);
  t.deck[3] = gfx_rgba(255, 110, 200);
  return t;
}

Theme Theme::from_skin(const skin::Skin& sk) {
  Theme t = defaults();
  const auto pick = [&](gfx::Color& out, std::initializer_list<const char*> names) {
    for (const char* n : names) {
      const auto it = sk.colors.find(n);
      if (it != sk.colors.end()) {
        out = to_gfx(it->second);
        return;
      }
    }
  };
  pick(t.bg, {"bg", "void"});
  pick(t.panel, {"panel", "glass"});
  t.panel2 = lerp_color(t.panel | 0xFF000000u, gfx_rgba(255, 255, 255), 0.05f);  // derived unless the skin names one
  pick(t.panel2, {"panel2"});
  pick(t.line, {"line", "rim"});
  pick(t.text, {"text", "ink"});
  pick(t.dim, {"dim"});
  pick(t.accent, {"accent", "violet", "cyan"});
  pick(t.warn, {"bad", "red", "magenta"});
  pick(t.good, {"good", "cyan"});
  pick(t.hover, {"hover"});
  pick(t.deck[0], {"deck1", "cyan"});
  pick(t.deck[1], {"deck2", "magenta"});
  pick(t.deck[2], {"deck3"});
  pick(t.deck[3], {"deck4"});
  // Overlays need opaque panels even when the skin's panel colour is translucent glass.
  t.panel = lerp_color(t.bg | 0xFF000000u, t.panel | 0xFF000000u, 0.85f);
  t.panel2 |= 0xFF000000u;
  t.bg |= 0xFF000000u;
  return t;
}

// ---- loading and layout ------------------------------------------------------------------------------
bool SkinView::load(const std::string& path, std::string& error) {
  skin::ParseResult r = skin::load_skin(path);
  if (!r.ok()) {
    error = r.describe();
    return false;
  }
  skin_ = r.skin;
  theme_ = Theme::from_skin(*skin_);
  layout_ = skin::Layout{};
  slots_.clear();
  window_.clear();
  active_ = hot_ = -1;
  return true;
}

void SkinView::layout(const std::string& window, float w, float h, float il, float it, float ir, float ib) {
  if (!skin_ || skin_->windows.empty()) return;
  if (window == window_ && w == lw_ && h == lh_ && il == insets_[0] && it == insets_[1] && ir == insets_[2] && ib == insets_[3] &&
      !layout_.widgets.empty())
    return;
  std::size_t index = 0;
  for (std::size_t i = 0; i < skin_->windows.size(); ++i)
    if (skin_->windows[i].id == window) index = i;
  window_ = window;
  lw_ = w;
  lh_ = h;
  insets_[0] = il;
  insets_[1] = it;
  insets_[2] = ir;
  insets_[3] = ib;
  const skin::Window& win = skin_->windows[index];
  min_w_ = win.min_w;
  min_h_ = win.min_h;
  const float iw = std::max(64.0f, w - il - ir), ih = std::max(64.0f, h - it - ib);
  layout_ = skin::solve_layout(*skin_, index, iw, ih, 1.0f);
  for (skin::ResolvedWidget& rw : layout_.widgets) {
    rw.rect.x += il;
    rw.rect.y += it;
  }
  slots_.assign(layout_.widgets.size(), Slot{});
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    const skin::ResolvedWidget& rw = layout_.widgets[i];
    if (!rw.bind.empty()) slots_[i].target = map_binding(rw.bind);
    if (!rw.show.empty()) {
      slots_[i].show_negate = rw.show[0] == '!';
      slots_[i].show = map_binding(slots_[i].show_negate ? rw.show.substr(1) : rw.show);
    }
  }
  hidden_.assign(layout_.widgets.size(), 0);
  active_ = hot_ = -1;
}

bool SkinView::bold_font(const skin::Style& st) const {
  if (!skin_ || st.font.empty()) return false;
  const auto it = skin_->fonts.find(st.font);
  return it != skin_->fonts.end() && it->second.bold;
}

std::string SkinView::fit(const std::string& s, float size, int weight, float max_w) {
  if (s.empty() || max_w <= 0) return s;
  const float full = dl_.text_width(s, size, weight);
  if (full <= max_w) return s;
  std::vector<std::size_t> starts;  // byte offset of every code point
  for (std::size_t i = 0; i < s.size(); ++i)
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) starts.push_back(i);
  const float room = std::max(0.0f, max_w - size * 0.8f);
  std::size_t keep = static_cast<std::size_t>(static_cast<float>(starts.size()) * room / full);
  keep = std::min(keep, starts.size());
  for (int attempt = 0; attempt < 4; ++attempt) {
    const std::string cand = s.substr(0, keep < starts.size() ? starts[keep] : s.size()) + "\xE2\x80\xA6";
    if (keep == 0 || dl_.text_width(cand, size, weight) <= max_w) return keep == 0 ? std::string("\xE2\x80\xA6") : cand;
    --keep;
  }
  return s.substr(0, starts[keep]) + "\xE2\x80\xA6";
}

void SkinView::draw_label(const skin::Rect& r, const std::string& s, const skin::Style& st, gfx::Align align, bool vcenter) {
  if (s.empty() || r.w < 2) return;
  const int weight = bold_font(st) ? 1 : 0;
  const std::string t = fit(s, st.font_size, weight, r.w);
  const float th = dl_.text_height(t, st.font_size, weight);
  const float y = vcenter ? r.y + (r.h - th) / 2 : r.y;
  const float x = align == gfx::Align::Left ? r.x : align == gfx::Align::Center ? r.x + r.w / 2 : r.right();
  dl_.text(x, y, t, st.font_size, to_gfx(st.fg, st.opacity), align, r.w, weight);
}

std::string SkinView::text_of(const skin::ResolvedWidget& w, const Target& t, SkinHost& host, bool on) {
  std::string spec = w.text;
  if (on) {
    const auto it = w.props.find("text_on");
    if (it != w.props.end()) spec = it->second;
  }
  if (!spec.empty()) {
    i18n::Args args;
    const auto a = w.props.find("args");
    if (a != w.props.end()) args = skin::parse_text_args(a->second);
    double v = 0;
    if (t && host.read_number(t, v)) {
      args.emplace_back("value", i18n::Value(v));
      args.emplace_back("count", i18n::Value(v));
    }
    return skin::resolve_text(*skin_, catalog_, spec, args);
  }
  if (t) {
    std::string s;
    if (host.read_text(t, s)) return s;
    double v = 0;
    if (host.read_number(t, v)) return number_text(v, w.format);
  }
  return {};
}

// ---- picking and input -------------------------------------------------------------------------------
skin::Rect SkinView::hit_rect(const skin::ResolvedWidget& w) const {
  skin::Rect r = w.rect;
  if (touch_ && (w.kind == skin::Kind::Button || w.kind == skin::Kind::Knob || w.kind == skin::Kind::Fader)) {
    const float min = 44;
    if (r.w < min) { r.x -= (min - r.w) / 2; r.w = min; }
    if (r.h < min) { r.y -= (min - r.h) / 2; r.h = min; }
  }
  return r;
}

int SkinView::pick(float x, float y) const {
  const auto editable = [](const skin::ResolvedWidget& w) {
    if (w.kind != skin::Kind::Label) return false;
    const auto it = w.props.find("editable");
    return it != w.props.end() && it->second == "true";
  };
  for (int pass = 0; pass < (touch_ ? 2 : 1); ++pass) {
    for (int i = static_cast<int>(layout_.widgets.size()) - 1; i >= 0; --i) {
      const skin::ResolvedWidget& w = layout_.widgets[static_cast<std::size_t>(i)];
      if (hidden_[static_cast<std::size_t>(i)] || !(w.interactive || editable(w))) continue;
      if ((pass == 0 ? w.rect : hit_rect(w)).contains(x, y)) return i;
    }
  }
  return -1;
}

void SkinView::handle_drag(SkinHost& host, const Input& in) {
  if (active_ < 0) return;
  const skin::ResolvedWidget& w = layout_.widgets[static_cast<std::size_t>(active_)];
  const Target& t = slots_[static_cast<std::size_t>(active_)].target;
  if (!t || t.type != Target::Type::Control) return;
  const double span = t.hi - t.lo;
  if (w.kind == skin::Kind::Knob) {
    const float dy = in.y - drag_y_;
    drag_y_ = in.y;
    drag_start_ = std::clamp(drag_start_ - dy / (in.shift ? 1600.0f : 160.0f), 0.0, 1.0);
    host.set_number(t, t.lo + (t.invert ? 1.0 - drag_start_ : drag_start_) * span);
  } else if (w.kind == skin::Kind::Fader) {
    const bool vertical = !(w.props.count("orientation") && w.props.at("orientation") == "horizontal");
    const float len = std::max(8.0f, (vertical ? w.rect.h : w.rect.w) - 12);
    const float d = vertical ? -(in.y - drag_y_) : (in.x - drag_x_);
    drag_y_ = in.y;
    drag_x_ = in.x;
    drag_start_ = std::clamp(drag_start_ + d / len * (in.shift ? 0.1f : 1.0f), 0.0, 1.0);
    host.set_number(t, t.lo + (t.invert ? 1.0 - drag_start_ : drag_start_) * span);
  }
}

// ---- drawing ---------------------------------------------------------------------------------------
void SkinView::draw_meter(const skin::ResolvedWidget& w, const skin::Style& st, const skin::Rect& r, double level) {
  dl_.rect(r.x, r.y, r.w, r.h, gfx_rgba(10, 11, 14, static_cast<int>(200 * st.opacity)));
  const float db = 20.0f * std::log10(std::max(static_cast<float>(level), 1e-5f));
  const float n = std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f);
  const bool vertical = !(w.props.count("orientation") && w.props.at("orientation") == "horizontal");
  // Segmented bar: the colour follows the level, like a hardware LED ladder.
  const int segs = std::max(6, static_cast<int>((vertical ? r.h : r.w) / 4));
  for (int i = 0; i < segs; ++i) {
    const float lo = static_cast<float>(i) / static_cast<float>(segs);
    if (lo >= n) break;
    const gfx::Color c = lo > 0.9f ? theme_.warn : (lo > 0.75f ? gfx_rgba(250, 210, 70) : theme_.good);
    if (vertical) {
      const float sh = r.h / static_cast<float>(segs);
      dl_.rect(r.x, r.bottom() - sh * static_cast<float>(i + 1) + 0.5f, r.w, std::max(1.0f, sh - 1.0f), c);
    } else {
      const float sw = r.w / static_cast<float>(segs);
      dl_.rect(r.x + sw * static_cast<float>(i), r.y, std::max(1.0f, sw - 1.0f), r.h, c);
    }
  }
}

void SkinView::draw_fader(const skin::ResolvedWidget& w, const skin::Style& st, const skin::Rect& r, double n, bool bipolar) {
  const bool vertical = !(w.props.count("orientation") && w.props.at("orientation") == "horizontal");
  const gfx::Color fg = to_gfx(st.fg, st.opacity);
  const gfx::Color groove = gfx_rgba(10, 11, 14, 230);
  const float cap = 10;  // thumb thickness along the travel axis
  if (vertical) {
    const float cx = r.x + r.w / 2, top = r.y + cap / 2, len = std::max(1.0f, r.h - cap);
    dl_.rrect(cx - 2, r.y + 2, 4, r.h - 4, 2, groove);
    const float ty = top + len * static_cast<float>(1.0 - n);
    const float from = bipolar ? top + len * 0.5f : r.bottom() - 3;
    dl_.rect(cx - 1, std::min(from, ty), 2, std::fabs(from - ty), lerp_color(fg, groove, 0.35f));
    if (bipolar) dl_.rect(cx - 6, top + len * 0.5f - 0.5f, 12, 1, theme_.dim);
    const float tw = std::min(r.w - 2, 26.0f);
    dl_.rrect(cx - tw / 2, ty - cap / 2, tw, cap, 2, fg);
    dl_.rect(cx - tw / 2 + 3, ty - 0.5f, tw - 6, 1, groove);
  } else {
    const float cy = r.y + r.h / 2, left = r.x + cap / 2, len = std::max(1.0f, r.w - cap);
    dl_.rrect(r.x + 2, cy - 2, r.w - 4, 4, 2, groove);
    const float tx = left + len * static_cast<float>(n);
    const float from = bipolar ? left + len * 0.5f : r.x + 3;
    dl_.rect(std::min(from, tx), cy - 1, std::fabs(from - tx), 2, lerp_color(fg, groove, 0.35f));
    if (bipolar) dl_.rect(left + len * 0.5f - 0.5f, cy - 6, 1, 12, theme_.dim);
    const float th = std::min(r.h - 2, 22.0f);
    dl_.rrect(tx - cap / 2, cy - th / 2, cap, th, 2, fg);
    dl_.rect(tx - 0.5f, cy - th / 2 + 3, 1, th - 6, groove);
  }
}

void SkinView::draw_knob(const skin::ResolvedWidget& w, const Target&, const skin::Style& st, const skin::Rect& r, double n, bool bipolar,
                         const std::string& label) {
  // Wide, short cells put the label beside the dial; otherwise it goes underneath.
  const bool beside = !label.empty() && r.w >= r.h * 2.2f;
  const float label_h = label.empty() || beside ? 0 : st.font_size * 1.25f + 1;
  const float d = std::max(8.0f, std::min(beside ? r.h : r.w, r.h - label_h) - 2);
  const float cx = beside ? r.x + d / 2 + 2 : r.x + r.w / 2, cy = r.y + (r.h - label_h) / 2, rad = d / 2;
  const gfx::Color fg = to_gfx(st.fg, st.opacity);
  const float a0 = 0.75f * kPi, a1 = 2.25f * kPi, thick = std::clamp(d / 9.0f, 2.0f, 5.0f);
  dl_.circle(cx, cy, rad - thick, gfx_rgba(10, 11, 14, 150));
  dl_.arc(cx, cy, rad - thick / 2, a0, a1, thick, gfx_rgba(10, 11, 14, 230));
  const float ang = a0 + (a1 - a0) * static_cast<float>(std::clamp(n, 0.0, 1.0));
  if (bipolar) dl_.arc(cx, cy, rad - thick / 2, std::min(ang, 1.5f * kPi), std::max(ang, 1.5f * kPi), thick, fg);
  else dl_.arc(cx, cy, rad - thick / 2, a0, ang, thick, fg);
  const float ir = rad - thick - 1;
  dl_.line(cx + std::cos(ang) * ir * 0.35f, cy + std::sin(ang) * ir * 0.35f, cx + std::cos(ang) * ir, cy + std::sin(ang) * ir, 2, theme_.text);
  (void)w;
  if (label.empty()) return;
  const int weight = bold_font(st) ? 1 : 0;
  const gfx::Color lc = lerp_color(theme_.dim, fg, 0.25f);
  if (beside) {
    const float lx = cx + rad + 6, room = r.right() - lx;
    const std::string t = fit(label, st.font_size, weight, room);
    dl_.text(lx, r.y + (r.h - dl_.text_height(t, st.font_size, weight)) / 2, t, st.font_size, lc, gfx::Align::Left, room, weight);
  } else {
    const std::string t = fit(label, st.font_size, weight, r.w);
    dl_.text(cx, r.bottom() - label_h, t, st.font_size, lc, gfx::Align::Center, r.w, weight);
  }
}

void SkinView::draw_widget(int i, SkinHost& host, const Input& in, double now_s, double dt_ms, bool hidden) {
  const skin::ResolvedWidget& w = layout_.widgets[static_cast<std::size_t>(i)];
  Slot& slot = slots_[static_cast<std::size_t>(i)];
  const Target& t = slot.target;
  if (hidden) return;

  // ---- state and style
  const bool pressed = active_ == i && in.down;
  const bool hover = !pressed && (active_ < 0 ? hot_ == i : active_ == i);
  bool on = false;
  if (w.kind == skin::Kind::Button && t) on = host.read_flag(t);
  skin::State state = skin::State::Normal;
  if (pressed) state = skin::State::Pressed;
  else if (on) state = skin::State::On;
  else if (hover) state = skin::State::Hover;
  skin::Style st = w.style[static_cast<int>(state)];
  if (w.transition_ms > 0 && slot.shown_valid) {
    const float k = std::min(1.0f, static_cast<float>(dt_ms) / w.transition_ms);
    const auto mixc = [&](skin::Color a, skin::Color b) {  // 0xRRGGBBAA, per channel
      skin::Color out = 0;
      for (int sh = 0; sh < 32; sh += 8) {
        const float x = static_cast<float>((a >> sh) & 0xFF), y = static_cast<float>((b >> sh) & 0xFF);
        out |= static_cast<skin::Color>(std::lround(x + (y - x) * k)) << sh;
      }
      return out;
    };
    skin::Style cur = slot.shown;
    cur.fg = mixc(cur.fg, st.fg);
    cur.bg = mixc(cur.bg, st.bg);
    cur.border = mixc(cur.border, st.border);
    cur.glow = mixc(cur.glow, st.glow);
    cur.border_w += (st.border_w - cur.border_w) * k;
    cur.radius = st.radius;
    cur.opacity += (st.opacity - cur.opacity) * k;
    cur.glow_r += (st.glow_r - cur.glow_r) * k;
    cur.font = st.font;
    cur.font_size = st.font_size;
    cur.align = st.align;
    st = cur;
  }
  slot.shown = st;
  slot.shown_valid = true;

  // ---- animations
  skin::Rect r = w.rect;
  float alpha = 1, off_x = 0, off_y = 0, scale = st.scale, rotate = w.rotation, glow = 1;
  bool glow_anim = false;
  for (std::size_t k = 0; k < w.animations.size() && k < 8; ++k) {
    const skin::Animation& a = skin_->animations[static_cast<std::size_t>(w.animations[k])];
    float t_ms = 0;
    if (a.trigger == "always") {
      t_ms = static_cast<float>(now_s * 1000.0);
    } else if (a.trigger == "beat") {
      const double age = host.beat_age_ms(t);
      t_ms = age >= 0 ? static_cast<float>(age) : 1e9f;
    } else {
      const bool active = (a.trigger == "hover" && (hover || pressed)) || (a.trigger == "pressed" && pressed) || (a.trigger == "on" && on);
      if (active && slot.started[k] < 0) slot.started[k] = now_s;
      if (!active) slot.started[k] = -1;
      t_ms = slot.started[k] < 0 ? 0 : static_cast<float>((now_s - slot.started[k]) * 1000.0);
    }
    const float v = skin::animation_value(a, t_ms);
    if (a.property == "opacity") alpha *= v;
    else if (a.property == "scale") scale *= v;
    else if (a.property == "offset_x") off_x += v * 1.0f;
    else if (a.property == "offset_y") off_y += v * 1.0f;
    else if (a.property == "rotate") rotate += v;
    else if (a.property == "glow") { glow = v; glow_anim = true; }
  }
  (void)glow_anim;
  if (scale != 1) {
    const float nw = r.w * scale, nh = r.h * scale;
    r.x -= (nw - r.w) / 2;
    r.y -= (nh - r.h) / 2;
    r.w = nw;
    r.h = nh;
  }
  r.x += off_x;
  r.y += off_y;
  st.opacity *= alpha;
  (void)rotate;

  // ---- background, border, glow
  const bool draws_box = w.drawable && w.kind != skin::Kind::Spacer;
  if (draws_box) {
    if ((st.glow & 0xFF) && st.glow_r > 0 && glow > 0.01f) {
      for (int k = 3; k >= 1; --k) {
        const float e = st.glow_r * static_cast<float>(k) / 3.0f;
        dl_.rrect(r.x - e, r.y - e, r.w + 2 * e, r.h + 2 * e, st.radius + e, to_gfx(st.glow, st.opacity * glow * 0.12f));
      }
    }
    if (st.bg & 0xFF) dl_.rrect(r.x, r.y, r.w, r.h, st.radius, to_gfx(st.bg, st.opacity));
    if ((st.border & 0xFF) && st.border_w > 0) dl_.rrect_outline(r.x, r.y, r.w, r.h, st.radius, to_gfx(st.border, st.opacity), st.border_w);
  }

  // ---- by kind
  switch (w.kind) {
    case skin::Kind::Label: {
      const auto ed = w.props.find("editable");
      if (ed != w.props.end() && ed->second == "true") {
        host.draw_custom(w, t, st, r);
      } else {
        const gfx::Align al = st.align == skin::TextAlign::Center ? gfx::Align::Center : st.align == skin::TextAlign::Right ? gfx::Align::Right : gfx::Align::Left;
        draw_label(r, text_of(w, t, host, false), st, al);
      }
      break;
    }
    case skin::Kind::Button: {
      skin::Rect tr = r;
      tr.x += 3;
      tr.w -= 6;
      draw_label(tr, text_of(w, t, host, on), st, gfx::Align::Center);
      break;
    }
    case skin::Kind::Knob:
    case skin::Kind::Fader: {
      double v = 0, n = 0;
      if (t && host.read_number(t, v)) n = std::clamp((v - t.lo) / std::max(1e-9, t.hi - t.lo), 0.0, 1.0);
      if (t.invert) n = 1.0 - n;
      const auto bp = w.props.find("bipolar");
      const bool bipolar = (bp != w.props.end() && bp->second == "true") || (t && t.lo < 0 && t.hi > 0);
      if (active_ == i && in.down) n = drag_start_;
      if (w.kind == skin::Kind::Knob) draw_knob(w, t, st, r, n, bipolar, text_of(w, Target{}, host, false));
      else draw_fader(w, st, r, n, bipolar);
      break;
    }
    case skin::Kind::Meter: {
      double v = 0;
      if (t) host.read_number(t, v);
      draw_meter(w, st, r, v);
      break;
    }
    case skin::Kind::Waveform:
    case skin::Kind::Jog:
    case skin::Kind::Pads:
    case skin::Kind::Browser:
    case skin::Kind::Tree: host.draw_custom(w, t, st, r); break;
    default: break;
  }
}

void SkinView::frame(SkinHost& host, const Input& in, double now_s) {
  if (!skin_ || layout_.widgets.empty()) return;
  const double dt_ms = last_now_ > 0 ? std::clamp((now_s - last_now_) * 1000.0, 0.0, 100.0) : 16.0;
  last_now_ = now_s;

  // Visibility first: a hidden container hides its children.
  for (std::size_t i = 0; i < layout_.widgets.size(); ++i) {
    const skin::ResolvedWidget& w = layout_.widgets[i];
    bool hide = w.parent >= 0 && hidden_[static_cast<std::size_t>(w.parent)];
    if (!hide && slots_[i].show) hide = host.read_flag(slots_[i].show) == slots_[i].show_negate;
    hidden_[i] = hide ? 1 : 0;
  }

  // Pointer: press picks the widget; a release ends the drag.
  if (active_ >= 0 && in.released) {
    const skin::ResolvedWidget& aw = layout_.widgets[static_cast<std::size_t>(active_)];
    if (aw.kind == skin::Kind::Button && slots_[static_cast<std::size_t>(active_)].target) host.release(slots_[static_cast<std::size_t>(active_)].target);
    active_ = -1;
  }
  if (in.pressed) {
    const int p = pick(in.x, in.y);
    if (p >= 0) {
      const skin::ResolvedWidget& pw = layout_.widgets[static_cast<std::size_t>(p)];
      const Target& pt = slots_[static_cast<std::size_t>(p)].target;
      if (pw.kind == skin::Kind::Button && pt) {
        active_ = p;
        host.press(pt);
      } else if ((pw.kind == skin::Kind::Knob || pw.kind == skin::Kind::Fader) && pt.type == Target::Type::Control) {
        active_ = p;
        double v = 0;
        host.read_number(pt, v);
        drag_start_ = std::clamp((v - pt.lo) / std::max(1e-9, pt.hi - pt.lo), 0.0, 1.0);
        if (pt.invert) drag_start_ = 1.0 - drag_start_;
        drag_y_ = in.y;
        drag_x_ = in.x;
        if (in.clicks >= 2) {
          host.set_number(pt, pt.def);  // double-click resets
          drag_start_ = std::clamp((pt.def - pt.lo) / std::max(1e-9, pt.hi - pt.lo), 0.0, 1.0);
          if (pt.invert) drag_start_ = 1.0 - drag_start_;
        } else if (pw.kind == skin::Kind::Fader) {
          // Pressing off the thumb jumps there, then the drag stays relative.
          const bool vertical = !(pw.props.count("orientation") && pw.props.at("orientation") == "horizontal");
          const float len = std::max(8.0f, (vertical ? pw.rect.h : pw.rect.w) - 12);
          const float pos = vertical ? 1.0f - (in.y - pw.rect.y - 6) / len : (in.x - pw.rect.x - 6) / len;
          if (std::fabs(pos - drag_start_) > 0.06) {
            drag_start_ = std::clamp(static_cast<double>(pos), 0.0, 1.0);
            host.set_number(pt, pt.lo + (pt.invert ? 1.0 - drag_start_ : drag_start_) * (pt.hi - pt.lo));
          }
        }
      }
    }
  }
  if (active_ >= 0 && in.down && !in.pressed) handle_drag(host, in);
  hot_ = in.down ? -1 : pick(in.x, in.y);

  for (std::size_t i = 0; i < layout_.widgets.size(); ++i) draw_widget(static_cast<int>(i), host, in, now_s, dt_ms, hidden_[i] != 0);
}

}  // namespace deckhand::ui
