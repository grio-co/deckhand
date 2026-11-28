// The mixer laws as application settings: loaded and sent to the engine at start-up and after every audio restart, changed from the
// settings panel and the REST routes, and handed to the automix service (so a transition's ideal curve matches the law).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app_impl.hpp"
#include "mixer_prefs.hpp"

namespace deckhand {

using namespace deckhand::detail;

void App::send_mixer_field(mixer::MixerField f, float v) {
  Command c;
  c.op = Op::MixerSetting;
  c.i = static_cast<int>(f);
  c.f = v;
  engine_->send(c);  // RemoteEngine keeps the last value of each field and replays it after a helper restart
}

void App::apply_mixer_settings() {
  const mixer::Settings s = mixer_settings_load(*lib_);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    laws_ = s;
  }
  for (const MixerPref& p : mixer_prefs()) send_mixer_field(p.field, mixer_value(s, p.field));
  if (automix_) automix_->set_mixer_laws(s);
}

void App::mixer_changed(const mixer::Settings& s) {
  mixer::Settings old;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    old = laws_;
    laws_ = s;
  }
  for (const MixerPref& p : mixer_prefs())
    if (mixer_value(s, p.field) != mixer_value(old, p.field)) send_mixer_field(p.field, mixer_value(s, p.field));
  if (automix_) automix_->set_mixer_laws(s);
}

void App::api_mixer_apply(const mixer::Settings& s) { mixer_changed(s); }

void App::set_mixer_pref(const MixerPref& p, float v) {
  mixer::Settings s;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    s = laws_;
  }
  mixer::set_field(s, p.field, v);
  mixer_setting_store(*lib_, p, mixer_value(s, p.field));
  mixer_changed(s);
}

void App::draw_mixer_section(float x, float& y, float rw) {
  const ui::Theme& th = theme_;
  const float line = 28;
  mixer::Settings s;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    s = laws_;
  }
  ui_heading(x, y, T("settings.mixer"));
  y += 18;
  for (const MixerPref& p : mixer_prefs()) {
    const std::string name = p.name;
    const float cur = mixer_value(s, p.field);
    dl_.text(x, y + 5, skin_->fit(T("mixer." + name), 12, 0, 166), 12, th.dim, Align::Left, 166);
    float bx = x + 176;
    if (p.choice) {
      for (int n = 0; n < p.choices; ++n) {
        const std::string label = T("mixer." + name + "." + std::to_string(n));
        const float bw = std::max(60.0f, dl_.text_width(label, 12, 1) + 20);
        if (bx + bw > x + rw) break;
        if (ui_button({bx, y, bw, 24}, label, static_cast<int>(cur) == n) && static_cast<int>(cur) != n) set_mixer_pref(p, static_cast<float>(n));
        bx += bw + 4;
      }
    } else {
      const bool pct = std::string(p.unit) == "%";
      const float shown = pct ? cur * 100.0f : cur;
      char b[48];
      if (p.field == mixer::MixerField::TrimCutDb && cur <= 0) std::snprintf(b, sizeof b, "%s", T("mixer.silence").c_str());
      else if (pct) std::snprintf(b, sizeof b, "%.1f %%", static_cast<double>(shown));
      else std::snprintf(b, sizeof b, "%g %s", static_cast<double>(shown), p.unit);
      if (ui_button({bx, y, 30, 24}, "-", false)) set_mixer_pref(p, cur - p.step);
      dl_.text(bx + 36, y + 5, b, 12, th.text, Align::Center, 90);
      if (ui_button({bx + 132, y, 30, 24}, "+", false)) set_mixer_pref(p, cur + p.step);
    }
    y += line;
  }
  if (ui_button({x, y, 200, 24}, T("mixer.reset"), false)) {
    for (const MixerPref& p : mixer_prefs()) lib_->set_setting(p.key, "");
    mixer_changed(mixer::Settings{});
  }
  y += line;
  dl_.text(x, y, skin_->fit(T("mixer.note"), 11, 0, rw), 11, th.dim, Align::Left, rw);
  y += 24;
}

}  // namespace deckhand
