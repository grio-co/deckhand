// The "Controller" panel: one hardware controller, its mapping and its settings. Opened by clicking the controller's
// name in the status row (skin action app.controller.open), or by the ui.controller shortcut. Settings are stored per
// controller (controller_prefs.hpp) and applied by the hub (live, also when the hub runs in the engine process).
#include <algorithm>

#include "app_impl.hpp"
#include "deckhand/app/controller_prefs.hpp"
#include "deckhand/broadcast/json.hpp"

namespace deckhand {

namespace {
std::string joined(const std::vector<std::string>& v, const char* sep = ", ") {
  std::string out;
  for (const std::string& s : v) out += (out.empty() ? "" : sep) + s;
  return out;
}
std::string joined_ints(const std::vector<int>& v) {
  std::string out;
  for (const int d : v) out += (out.empty() ? "" : "/") + std::to_string(d);
  return out;
}
bool contains_ci(std::string hay, std::string needle) {
  std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return std::tolower(c); });
  std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return std::tolower(c); });
  return !needle.empty() && hay.find(needle) != std::string::npos;
}
}  // namespace

// Names shown in the panel: what is connected now, then controllers seen before.
void App::controller_reload() {
  ctl_files_ = scan_mapping_files(mapping_folders_);
  ctl_names_ = hub_ ? hub_->connected() : std::vector<std::string>();
  const std::size_t connected = ctl_names_.size();
  for (const std::string& s : seen_controllers(*lib_))
    if (std::find(ctl_names_.begin(), ctl_names_.end(), s) == ctl_names_.end()) ctl_names_.push_back(s);
  ctl_connected_count_ = connected;
  if (ctl_sel_ < 0 || ctl_sel_ >= static_cast<int>(ctl_names_.size())) ctl_sel_ = 0;
  ctl_measure_ = false;
  ctl_note_.clear();
  if (!ctl_names_.empty()) {
    ctl_prefs_ = load_controller_prefs(*lib_, ctl_names_[static_cast<std::size_t>(ctl_sel_)]);
    f_ctl_ticks_.text = ctl_prefs_.ticks_per_rev > 0 ? fmt("%g", ctl_prefs_.ticks_per_rev) : std::string();
  }
}

// Pushes the stored preferences of every connected controller to the hub (and remembers the controller). Called about
// once a second from the control thread; sends only when the set of controllers changed or every 10 seconds.
void App::controller_sync(double now) {
  if (!hub_ || now - ctl_last_sync_ < 1.0) return;
  ctl_last_sync_ = now;
  if (ctl_resync_.exchange(false)) ctl_synced_.clear();
  const std::vector<std::string> names = hub_->connected();
  if (names == ctl_synced_) return;  // a restarted engine process clears ctl_synced_ and gets everything again
  ctl_synced_ = names;
  for (const std::string& n : names) note_seen_controller(*lib_, n);
  const std::vector<std::string> prefer = preferred_mappings(*lib_, names);
  if (!prefer.empty()) hub_->prefer_mappings(prefer);
  for (const std::string& n : names) hub_->set_options(n, to_device_options(load_controller_prefs(*lib_, n)));
}

std::string App::controllers_json(Library& lib) {
  using broadcast::Json;
  Json root = Json::object();
  const std::vector<std::string> now = hub_ ? hub_->connected() : std::vector<std::string>();
  const std::vector<MappingFileInfo> files = scan_mapping_files(mapping_folders_);
  std::vector<std::string> all = now;
  for (const std::string& s : seen_controllers(lib))
    if (std::find(all.begin(), all.end(), s) == all.end()) all.push_back(s);
  Json list = Json::array();
  for (const std::string& n : all) {
    Json c = Json::object();
    c.set("name", n);
    c.set("connected", std::find(now.begin(), now.end(), n) != now.end());
    const ControllerPrefs p = load_controller_prefs(lib, n);
    Json s = Json::object();
    s.set("jog_sensitivity", p.jog_scale);
    s.set("ticks_per_rev", p.ticks_per_rev);  // 0 = the mapping's own
    s.set("soft_takeover", p.pickup < 0 ? Json("mapping") : Json(p.pickup ? "on" : "off"));
    s.set("leds", p.leds);
    s.set("mapping", p.mapping);
    c.set("settings", std::move(s));
    c.set("settings_prefix", "controller." + controller_slug(n) + ".");
    for (const MappingFileInfo& f : files) {
      if (f.name != n) continue;
      Json m = Json::object();
      m.set("name", f.name);
      m.set("file", f.path);
      m.set("match", f.match);
      m.set("ticks_per_rev", f.ticks_per_rev);
      m.set("soft_takeover_default", f.pickup_default);
      m.set("audio_match", f.audio_match);
      Json decks = Json::array();
      for (const int d : f.decks) decks.push(d);
      m.set("decks", std::move(decks));
      Json modes = Json::array();
      for (const std::string& pm : f.pad_modes) modes.push(pm);
      m.set("pad_modes", std::move(modes));
      Json alts = Json::array();
      for (const std::string& pm : f.alt_modes) alts.push(pm);
      m.set("alternate_pad_modes", std::move(alts));
      c.set("mapping", std::move(m));
    }
    list.push(std::move(c));
  }
  root.set("controllers", std::move(list));
  return root.dump();
}

void App::controller_select(int index) {
  if (index < 0 || index >= static_cast<int>(ctl_names_.size())) return;
  ctl_sel_ = index;
  ctl_measure_ = false;
  ctl_prefs_ = load_controller_prefs(*lib_, ctl_names_[static_cast<std::size_t>(index)]);
  f_ctl_ticks_.text = ctl_prefs_.ticks_per_rev > 0 ? fmt("%g", ctl_prefs_.ticks_per_rev) : std::string();
}

void App::controller_apply() {
  if (ctl_names_.empty()) return;
  const std::string& name = ctl_names_[static_cast<std::size_t>(ctl_sel_)];
  save_controller_prefs(*lib_, name, ctl_prefs_);
  if (hub_ && ctl_sel_ < static_cast<int>(ctl_connected_count_)) hub_->set_options(name, to_device_options(ctl_prefs_));
}

void App::draw_controller(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 20, rw = m.w - 40;
  float y = m.y + 46;
  const Rect list{m.x + 8, y, m.w - 16, m.y + m.h - y - 8};
  if (list.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
  panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, panel_content_h_ - list.h));
  dl_.push_clip(list.x, list.y, list.w, list.h);
  float cy = list.y + 2 - panel_scroll_;
  const auto line = [&](const std::string& s, Color c, float size = 12, float h = 18) {
    dl_.text(x, cy, skin_->fit(s, size, 0, rw), size, c, Align::Left, rw);
    cy += h;
  };

  if (ctl_names_.empty()) {  // nothing connected and nothing seen before
    line(T("controller.none"), th.text, 13, 24);
    line(T("controller.none_hint"), th.dim, 11, 22);
    if (ui_button({x, cy, 180, 28}, T("controller.learn"), false)) open_panel(Panel::Learn);
    cy += 36;
  } else {
    // the controllers: connected ones first, then the ones seen before
    float bx = x;
    for (std::size_t i = 0; i < ctl_names_.size(); ++i) {
      const float w = std::min(rw, dl_.text_width(ctl_names_[i], 12) + 28);
      if (bx + w > x + rw) {
        bx = x;
        cy += 32;
      }
      if (ui_button({bx, cy, w, 26}, ctl_names_[i], static_cast<int>(i) == ctl_sel_, i < ctl_connected_count_ ? th.good : 0)) controller_select(static_cast<int>(i));
      bx += w + 6;
    }
    cy += 36;
    const std::string name = ctl_names_[static_cast<std::size_t>(ctl_sel_)];
    const bool connected = ctl_sel_ < static_cast<int>(ctl_connected_count_);
    const MappingFileInfo* file = nullptr;
    for (const MappingFileInfo& f : ctl_files_)
      if (f.name == name) file = &f;

    dl_.text(x, cy, skin_->fit(name, 15, 1, rw), 15, th.text, Align::Left, rw, 1);
    cy += 24;
    line(connected ? T("controller.connected") : T("controller.not_connected"), connected ? th.good : th.warn);
    if (file) {
      line(T("controller.mapping_file", {{"path", file->path}}), th.dim, 11);
      line(T("controller.decks", {{"decks", joined_ints(file->decks)}}), th.dim, 11);
      if (!file->pad_modes.empty()) line(T("controller.pad_modes", {{"modes", joined(file->pad_modes)}}), th.dim, 11);
      for (const std::string& alt : file->alt_modes) line(T("controller.pad_alt", {{"modes", alt}}), th.dim, 11);
      if (!file->audio_match.empty())
        line(T("controller.audio_interface", {{"name", file->audio_match}, {"master", file->master_channel + 1}, {"phones", file->phones_channel + 1}}), th.dim, 11);
      line(T("controller.ports", {{"kind", file->hid ? "HID" : "MIDI"}, {"match", file->match}}), th.dim, 11);
      if (contains_ci(name, "flx4") || contains_ci(file->match, "flx4")) line(T("controller.checklist"), th.dim, 11);
    } else {
      line(T("controller.no_mapping"), th.warn, 11);
    }
    cy += 6;

    // several mappings can match one device: choose which one wins (needs a reload to take effect)
    std::vector<const MappingFileInfo*> rivals;
    if (file) rivals = competing_mappings(ctl_files_, file->match);
    if (rivals.size() > 1) {
      line(T("controller.choose_mapping"), th.text, 12, 20);
      float rx = x;
      for (const MappingFileInfo* r : rivals) {
        const float w = std::min(rw, dl_.text_width(r->name, 12) + 28);
        if (rx + w > x + rw) {
          rx = x;
          cy += 32;
        }
        const bool chosen = ctl_prefs_.mapping == r->name || (ctl_prefs_.mapping.empty() && r == rivals.front());
        if (ui_button({rx, cy, w, 26}, r->name, chosen)) {
          ctl_prefs_.mapping = r->name;
          controller_apply();
          const std::vector<std::string> prefer = preferred_mappings(*lib_, ctl_names_);
          if (hub_) {
            hub_->prefer_mappings(prefer);
            hub_->reload_mappings(load_mappings(mapping_folders_));
          }
          ctl_note_ = T("controller.reloaded");
        }
        rx += w + 6;
      }
      cy += 36;
    }

    // settings
    line(T("controller.settings"), th.text, 13, 24);
    dl_.text(x, cy + 5, T("controller.jog_sensitivity", {{"value", fmt("%.2f", ctl_prefs_.jog_scale)}}), 12, th.text);
    if (ui_button({x + 250, cy, 30, 26}, "-", false)) {
      ctl_prefs_.jog_scale = std::max(0.25, std::round((ctl_prefs_.jog_scale - 0.25) * 100) / 100);
      controller_apply();
    }
    if (ui_button({x + 286, cy, 30, 26}, "+", false)) {
      ctl_prefs_.jog_scale = std::min(4.0, std::round((ctl_prefs_.jog_scale + 0.25) * 100) / 100);
      controller_apply();
    }
    if (ui_button({x + 322, cy, 70, 26}, T("controller.reset"), false)) {
      ctl_prefs_.jog_scale = 1.0;
      controller_apply();
    }
    cy += 34;
    dl_.text(x, cy + 5, T("controller.ticks", {{"mapping", file ? fmt("%g", file->ticks_per_rev) : std::string("-")}}), 12, th.text);
    ui_field({x + 250, cy, 80, 26}, f_ctl_ticks_, file ? fmt("%g", file->ticks_per_rev) : std::string());
    if (ui_button({x + 338, cy, 70, 26}, T("controller.apply"), false)) {
      const double v = std::atof(f_ctl_ticks_.text.c_str());
      ctl_prefs_.ticks_per_rev = v >= 16 && v <= 100000 ? v : 0;
      controller_apply();
    }
    if (connected && ui_button({x + 414, cy, 150, 26}, T(ctl_measure_ ? "controller.measure_stop" : "controller.measure"), ctl_measure_)) {
      ctl_measure_ = !ctl_measure_;
      ctl_measured_ = 0;
    }
    cy += 34;
    if (ctl_measure_) {
      const double t = now_seconds();
      if (t - ctl_measure_poll_ > 0.25) {  // one request every quarter second, not every frame
        ctl_measure_poll_ = t;
        if (hub_) ctl_measured_ = hub_->jog_ticks(name);
      }
      line(T("controller.measure_hint"), th.dim, 11);
      if (ctl_measured_ > 0) {
        dl_.text(x, cy + 5, T("controller.measured", {{"ticks", std::lround(ctl_measured_)}}), 12, th.text);
        if (ui_button({x + 250, cy, 200, 26}, T("controller.use_measured"), false)) {
          ctl_prefs_.ticks_per_rev = std::round(ctl_measured_);
          f_ctl_ticks_.text = fmt("%g", ctl_prefs_.ticks_per_rev);
          controller_apply();
        }
        cy += 32;
      }
    }
    const char* pickup_label = ctl_prefs_.pickup < 0 ? "controller.takeover_mapping" : ctl_prefs_.pickup ? "controller.takeover_on" : "controller.takeover_off";
    dl_.text(x, cy + 5, T("controller.takeover"), 12, th.text);
    if (ui_button({x + 250, cy, 200, 26}, T(pickup_label), false)) {
      ctl_prefs_.pickup = ctl_prefs_.pickup < 0 ? 1 : ctl_prefs_.pickup == 1 ? 0 : -1;
      controller_apply();
    }
    cy += 34;
    if (ui_toggle({x, cy, rw, 24}, T("controller.leds"), ctl_prefs_.leds)) {
      ctl_prefs_.leds = !ctl_prefs_.leds;
      controller_apply();
    }
    cy += 32;

    // actions
    float ax = x;
    const auto act = [&](const char* label, float w) {
      const bool hit = ui_button({ax, cy, w, 28}, T(label), false);
      ax += w + 8;
      return hit;
    };
    if (act("controller.learn", 170)) open_panel(Panel::Learn);
    if (act("controller.reload", 150)) {
      if (hub_) hub_->reload_mappings(load_mappings(mapping_folders_));
      const std::size_t keep = static_cast<std::size_t>(ctl_sel_);
      controller_reload();
      ctl_sel_ = static_cast<int>(std::min(keep, ctl_names_.empty() ? std::size_t(0) : ctl_names_.size() - 1));
      controller_sync(now_seconds() + 100);
      ctl_note_ = T("controller.reloaded");
    }
    if (file && !file->audio_match.empty() && connected && act("controller.use_audio", 230)) {
      std::string dev;
      for (const std::string& d : engine_->devices())
        if (contains_ci(d, file->audio_match)) dev = d;
      if (dev.empty()) {
        ctl_note_ = T("controller.audio_missing", {{"name", file->audio_match}});
      } else {
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          audio_pref_ = dev;
        }
        save_setting("audio.device", dev);
        restart_audio();
        ctl_note_ = T("controller.audio_set", {{"name", dev}});
      }
    }
    cy += 38;
    if (file) {
      line(T("controller.file_hint", {{"path", file->path}}), th.dim, 11);  // no file manager hook in the core: the path is shown
    }
  }
  if (!ctl_note_.empty()) line(ctl_note_, th.good, 12, 20);
  dl_.pop_clip();
  panel_content_h_ = cy + panel_scroll_ - list.y + 8;
}

}  // namespace deckhand
