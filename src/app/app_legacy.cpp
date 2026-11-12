// Minimal code-drawn UI, used only when no skin can be loaded (not even the built-in default), so the app
// never shows a blank window. Transport, faders, library and the essential buttons; everything else lives
// in the skins.
#include "app_impl.hpp"

namespace deckhand {

bool App::legacy_button(const Rect& r, const std::string& label, bool lit, Color lit_color) {
  const bool hover = r.contains(in_.x, in_.y);
  dl_.rect(r.x, r.y, r.w, r.h, lit ? lit_color : (hover ? theme_.panel2 : theme_.panel));
  dl_.outline(r.x, r.y, r.w, r.h, lit ? lit_color : theme_.line);
  dl_.text(r.x + r.w / 2, r.y + (r.h - 15) / 2, skin_->fit(label, 11, 1, r.w - 6), 11, lit ? theme_.bg : theme_.text, Align::Center, r.w - 4, 1);
  return clicked(r);
}

void App::draw_legacy() {
  const float w = width_px_ / scale_, h = height_px_ / scale_;
  const ui::Theme& th = theme_;
  const float top_h = 30, wave_h = 80, gap = 2;

  // ---- top bar
  dl_.rect(0, 0, w, top_h, th.panel);
  dl_.text(12, 6, T("app.title"), 14, th.text, Align::Left, 0, 1);
  std::string status, audio;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status = status_;
    audio = audio_label_;
  }
  dl_.text(110, 8, skin_->fit(status, 12, 0, w * 0.3f), 12, th.dim, Align::Left, w * 0.3f);
  float right = w - 8;
  const float aw = dl_.text(right, 8, skin_->fit(audio, 12, 0, w * 0.25f), 12, th.dim, Align::Right);
  right -= aw + 16;
  const struct {
    const char* action;
    std::string label;
    float width;
  } buttons[] = {{"export", T("app.export_usb"), 96}, {"rec", T("app.rec"), 56}, {"settings", T("app.settings"), 80}, {"import", T("app.import_library"), 110}};
  for (const auto& b : buttons) {
    if (std::string(b.action) == "import" && !import_available()) continue;
    const ui::Target t = ui::map_binding(std::string("app.") + b.action);
    right -= b.width;
    if (legacy_button({right, 4, b.width, top_h - 8}, b.label, read_flag(t), th.accent)) press(t);
    right -= 6;
  }

  // ---- waveforms
  float y = top_h + gap;
  for (int di = 0; di < 2; ++di) {
    dl_.rect(0, y, w, wave_h, rgba(12, 13, 16));
    draw_wave(di, {0, y, w, wave_h}, views_[di], info_[di], deck_color(di), th, true);
    y += wave_h + gap;
  }

  // ---- decks and mixer
  const float deck_h = 150, mixer_w = 220, deck_w = (w - mixer_w - gap * 2) / 2;
  for (int di = 0; di < 2; ++di) {
    const Rect r{di == 0 ? 0 : deck_w + mixer_w + gap * 2, y, deck_w, deck_h};
    const DeckView& v = views_[di];
    dl_.rect(r.x, r.y, r.w, r.h, th.panel);
    dl_.text(r.x + 10, r.y + 8, deck_letter(di), 22, deck_color(di), Align::Left, 0, 1);
    std::string title;
    ui::Target tt = ui::map_binding("deck[" + std::to_string(di + 1) + "].title");
    read_text(tt, title);
    dl_.text(r.x + 40, r.y + 10, skin_->fit(title, 14, 1, r.w - 150), 14, th.text, Align::Left, r.w - 150, 1);
    dl_.text(r.x + r.w - 10, r.y + 8, v.bpm > 0 ? fmt("%.1f", v.bpm) : "--.-", 22, th.text, Align::Right, 0, 1);
    const char* names[] = {"cue", "play", "sync"};
    const std::string labels[] = {T("deck.cue"), v.playing ? T("deck.pause") : T("deck.play"), T("deck.sync")};
    for (int k = 0; k < 3; ++k) {
      const ui::Target t = ui::map_binding("deck[" + std::to_string(di + 1) + "]." + names[k]);
      const Rect b{r.x + 10 + 62.0f * static_cast<float>(k), r.y + 44, 56, 28};
      if (legacy_button(b, labels[k], read_flag(t), deck_color(di))) {
        press(t);
        if (t.momentary) held_ = (di + 1) * 100 + 1;
      }
    }
    if (in_.released && held_ == (di + 1) * 100 + 1) {
      release(ui::map_binding("deck[" + std::to_string(di + 1) + "].cue"));
      held_ = 0;
    }
    draw_overview(di, {r.x + 10, r.y + 84, r.w - 20, r.h - 94}, v, info_[di], deck_color(di), th);
  }
  {  // crossfader and channel faders: click or drag
    const Rect m{deck_w + gap, y, mixer_w, deck_h};
    dl_.rect(m.x, m.y, m.w, m.h, th.panel);
    const auto slider = [&](const Rect& tr, const std::string& bind, bool vertical, int id) {
      const ui::Target t = ui::map_binding(bind);
      double v = 0;
      read_number(t, v);
      const float n = static_cast<float>((v - t.lo) / (t.hi - t.lo));
      dl_.rect(tr.x, tr.y, tr.w, tr.h, rgba(10, 11, 14));
      if (vertical) dl_.rect(tr.x - 3, tr.y + tr.h * (1 - n) - 3, tr.w + 6, 6, th.text);
      else dl_.rect(tr.x + tr.w * n - 4, tr.y - 3, 8, tr.h + 6, th.text);
      if (clicked({tr.x - 6, tr.y - 6, tr.w + 12, tr.h + 12})) held_ = id;
      if (held_ == id && in_.down) {
        const float f = vertical ? 1 - (in_.y - tr.y) / tr.h : (in_.x - tr.x) / tr.w;
        set_number(t, t.lo + std::clamp(f, 0.0f, 1.0f) * (t.hi - t.lo));
      }
    };
    slider({m.x + 50, m.y + 14, 6, 90}, "mixer.ch[1].fader", true, 901);
    slider({m.x + m.w - 56, m.y + 14, 6, 90}, "mixer.ch[2].fader", true, 902);
    slider({m.x + 20, m.y + m.h - 24, m.w - 40, 6}, "mixer.crossfader", false, 903);
    if (in_.released && held_ >= 900) held_ = 0;
  }
  y += deck_h + gap;

  // ---- library
  const float tree_w = 220;
  dl_.rect(0, y, tree_w, h - y, th.panel);
  draw_tree({0, y, tree_w, h - y}, th);
  draw_tracks({tree_w + 1, y, w - tree_w - 1, h - y}, th, "title,artist,bpm,key,time", 22);
}

}  // namespace deckhand
