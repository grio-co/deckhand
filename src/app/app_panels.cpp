// Overlay panels drawn by the application, styled from the active skin's tokens (ui::Theme): settings,
// sampler, MIDI learn, track details, relink, USB export destinations and the add-to-playlist menu.
// They take the raw pointer (ov_) because the main UI is blocked while one is open.
#include "app_impl.hpp"

namespace deckhand {

namespace {
constexpr std::uint32_t kPeriods[] = {64, 128, 256, 512, 1024, 2048};

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  return s.substr(a);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}
}  // namespace

// ---- small widgets ----------------------------------------------------------------------------------------
bool App::ui_button(const Rect& r, const std::string& label, bool lit, Color lit_color) {
  const ui::Theme& th = theme_;
  const bool hover = r.contains(ov_.x, ov_.y);
  const Color fill = lit ? (lit_color ? lit_color : th.accent) : (hover ? mix(th.panel2, rgba(255, 255, 255), 0.12f) : th.panel2);
  dl_.rrect(r.x, r.y, r.w, r.h, 4, fill);
  dl_.rrect_outline(r.x, r.y, r.w, r.h, 4, lit ? fill : th.line, 1);
  const std::string t = skin_->fit(label, 12, 1, r.w - 10);
  dl_.text(r.x + r.w / 2, r.y + (r.h - dl_.text_height(t, 12, 1)) / 2, t, 12, lit ? th.bg : th.text, Align::Center, r.w - 6, 1);
  return ov_.pressed && hover;
}

bool App::ui_toggle(const Rect& r, const std::string& label, bool on) {
  const ui::Theme& th = theme_;
  const Rect pill{r.x, r.y + (r.h - 16) / 2, 30, 16};
  dl_.rrect(pill.x, pill.y, pill.w, pill.h, 8, on ? th.accent : th.panel2);
  dl_.rrect_outline(pill.x, pill.y, pill.w, pill.h, 8, th.line, 1);
  dl_.circle(pill.x + (on ? 22.0f : 8.0f), pill.y + 8, 6, on ? th.bg : th.dim);
  const std::string t = skin_->fit(label, 12, 0, r.w - 40);
  dl_.text(r.x + 40, r.y + (r.h - dl_.text_height(t, 12, 0)) / 2, t, 12, th.text, Align::Left, r.w - 40);
  return ov_.pressed && r.contains(ov_.x, ov_.y);
}

void App::ui_field(const Rect& r, Field& f, const std::string& hint) {
  const ui::Theme& th = theme_;
  if (ov_.pressed) {
    if (r.contains(ov_.x, ov_.y)) focus_ = &f;
    else if (focus_ == &f) focus_ = nullptr;
  }
  const bool focus = focus_ == &f;
  dl_.rrect(r.x, r.y, r.w, r.h, 4, rgba(10, 11, 14));
  dl_.rrect_outline(r.x, r.y, r.w, r.h, 4, focus ? th.accent : th.line, 1);
  const std::string shown = f.text.empty() && !focus ? hint : f.text + (focus ? "|" : "");
  const std::string t = skin_->fit(shown, 12, 0, r.w - 14);
  if (!t.empty()) dl_.text(r.x + 7, r.y + (r.h - dl_.text_height(t, 12, 0)) / 2, t, 12, f.text.empty() && !focus ? th.dim : th.text, Align::Left, r.w - 14);
}

void App::ui_heading(float x, float y, const std::string& s) { dl_.text(x, y, s, 11, theme_.accent, Align::Left, 0, 1); }

bool App::field_key(dh_key key) {
  if (!focus_) return false;
  if (key == DH_KEY_BACKSPACE) utf8_pop(focus_->text);
  else if (key == DH_KEY_ENTER || key == DH_KEY_ESCAPE || key == DH_KEY_TAB) focus_ = nullptr;
  return true;
}

// ---- panel management ----------------------------------------------------------------------------------------
void App::close_panel() {
  if (panel_ == Panel::Settings) commit_settings();
  if (panel_ == Panel::Learn) hub_->learn_cancel();
  panel_ = Panel::None;
  focus_ = nullptr;
}

void App::begin_export_menu() {
  export_targets_.clear();
  std::error_code ec;
#if defined(__APPLE__)
  const char* mounts[] = {"/Volumes"};
#else
  const char* mounts[] = {"/media", "/run/media", "/mnt"};
#endif
  for (const char* m : mounts) {
    for (std::filesystem::directory_iterator it(m, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_symlink(ec) || !it->is_directory(ec)) continue;  // the boot volume appears as a link
      export_targets_.push_back(it->path().string());
    }
  }
  if (const char* home = std::getenv("HOME")) export_targets_.push_back(std::string(home) + "/Music/DeckHand USB Export");
}

void App::open_panel(Panel p) {
  if (panel_ == Panel::Settings && p != Panel::Settings) commit_settings();
  panel_ = p;
  panel_fresh_ = true;
  panel_scroll_ = 0;
  focus_ = nullptr;
  last_focus_ = nullptr;
  confirm_remove_ = false;
  switch (p) {
    case Panel::Settings: {
      output_devices_ = engine_->devices();
      for (const auto& kv : std::vector<std::pair<const char*, const char*>>{{"api.enabled", "1"}, {"api.port", "7373"}, {"stream.files", "0"},
                                                                          {"stream.overlay", "0"}, {"obs.enabled", "0"}, {"obs.host", "127.0.0.1"},
                                                                          {"obs.port", "4455"}, {"obs.text_source", ""}})
        sv_[kv.first] = lib_->setting(kv.first, kv.second);
      f_port_.text = sv_["api.port"];
      f_obs_host_.text = sv_["obs.host"];
      f_obs_port_.text = sv_["obs.port"];
      f_obs_source_.text = sv_["obs.text_source"];
      break;
    }
    case Panel::Export: begin_export_menu(); break;
    case Panel::Learn: {
      learn_list_.clear();
      learn_action_.clear();
      learn_line_.clear();
      learn_note_.clear();
      learn_saved_ = false;
      f_search_learn_.text.clear();
      hub_->learn_cancel();
      const char* per_deck[] = {"play", "cue", "sync", "jog", "jog_touch", "tempo", "tempo_range", "loop_in", "loop_out", "loop_toggle", "reloop",
                                "loop_exit", "loop_halve", "loop_double", "cue_call", "trim", "eq_high", "eq_mid", "eq_low", "filter", "fader",
                                "pfl", "load", "key_lock", "key_shift", "quantize", "slip", "reverse", "seek_start", "pad_mode"};
      const char* per_slot[] = {"hotcue", "hotcue_clear", "sampler"};
      const char* global[] = {"crossfader", "head_mix", "head_level", "master_level", "sampler_level", "master_bpm", "browse", "browse_press", "sampler_stop"};
      const auto pretty = [](std::string a) {
        std::replace(a.begin(), a.end(), '_', ' ');
        return a;
      };
      for (const char* a : global) learn_list_.push_back({pretty(a), a});
      for (int d = 1; d <= 4; ++d) {
        for (const char* a : per_deck) {
          std::string action = std::string(a) + " deck=" + std::to_string(d);
          if (std::string(a) == "pad_mode") action += " mode=hotcue";
          learn_list_.push_back({pretty(a) + "  \xC2\xB7  " + T("learn.deck", {{"deck", d}}), action});
        }
        for (const char* a : per_slot) {
          const int n = std::string(a) == "sampler" ? 16 : kHotCues;
          if (std::string(a) == "sampler" && d > 1) continue;  // sampler slots are not per deck
          for (int s = 1; s <= n; ++s) {
            const std::string deck_part = std::string(a) == "sampler" ? "" : " deck=" + std::to_string(d);
            learn_list_.push_back({pretty(a) + "  \xC2\xB7  " + (deck_part.empty() ? "" : T("learn.deck", {{"deck", d}}) + "  \xC2\xB7  ") + T("learn.slot", {{"slot", s}}),
                                   std::string(a) + deck_part + " slot=" + std::to_string(s)});
          }
        }
      }
      break;
    }
    case Panel::Tags:
    case Panel::Relink: {
      const auto row = lib_->track(edit_track_);
      if (!row) {
        panel_ = Panel::None;
        break;
      }
      f_title_.text = row->title;
      f_artist_.text = row->artist;
      f_key_.text = row->key;
      tag_rating_ = row->rating;
      tag_path_ = row->path;
      if (p == Panel::Relink) {
        relink_old_ = std::filesystem::path(row->path).parent_path().string();
        f_relink_.text.clear();
        f_relink_old_.text = relink_old_;
      }
      break;
    }
    default: break;
  }
}

void App::commit_settings() {
  const auto numeric = [&](const char* key, Field& f) {
    const int n = std::atoi(trim(f.text).c_str());
    if (n < 1 || n > 65535) {
      f.text = sv_[key];
      return;
    }
    const std::string v = std::to_string(n);
    f.text = v;
    if (v != sv_[key]) {
      save_setting(key, v);
      sv_[key] = v;
    }
  };
  const auto text = [&](const char* key, Field& f, bool allow_empty) {
    const std::string v = trim(f.text);
    if (v.empty() && !allow_empty) {
      f.text = sv_[key];
      return;
    }
    if (v != sv_[key]) {
      save_setting(key, v);
      sv_[key] = v;
    }
  };
  numeric("api.port", f_port_);
  numeric("obs.port", f_obs_port_);
  text("obs.host", f_obs_host_, false);
  text("obs.text_source", f_obs_source_, true);
}

void App::draw_overlays() {
  ov_ = raw_;
  if (panel_fresh_) {  // the click that opened the panel must not reach it
    ov_.pressed = false;
    ov_.clicks = 0;
    panel_fresh_ = false;
  }
  if (panel_ == Panel::None) return;
  const ui::Theme& th = theme_;
  const float w = width_px_ / scale_, h = height_px_ / scale_;
  const bool popup = panel_ == Panel::Export || panel_ == Panel::AddTo;
  if (!popup) dl_.rect(0, 0, w, h, rgba(0, 0, 0, 120));

  Rect m;
  std::string title;
  switch (panel_) {
    case Panel::Settings: m = {0, 0, std::min(640.0f, w - 24), std::min(600.0f, h - 24)}; title = T("settings.title"); break;
    case Panel::Sampler: m = {0, 0, std::min(560.0f, w - 24), std::min(420.0f, h - 24)}; title = T("sampler.title"); break;
    case Panel::Learn: m = {0, 0, std::min(680.0f, w - 24), std::min(540.0f, h - 24)}; title = T("learn.title"); break;
    case Panel::Tags: m = {0, 0, std::min(520.0f, w - 24), std::min(400.0f, h - 24)}; title = T("tags.title"); break;
    case Panel::Relink: m = {0, 0, std::min(560.0f, w - 24), std::min(250.0f, h - 24)}; title = T("relink.title"); break;
    case Panel::Export: m = {0, 0, std::min(440.0f, w - 16), 38 + 26 * static_cast<float>(export_targets_.size())}; title = T("export.title", {{"what", export_what_()}}); break;
    default: break;
  }
  if (panel_ == Panel::AddTo) {
    int count = 0;
    {
      std::lock_guard<std::mutex> lock(browse_mutex_);
      for (const TreeItem& item : tree_) count += item.kind == TreeItem::Playlist;
    }
    m = {0, 0, std::min(300.0f, w - 16), std::min(h - 24, 44 + 26 * static_cast<float>(std::max(1, count)))};
    title = T("browser.add_to");
  }
  m.h = std::min(m.h, h - 12);
  if (panel_ == Panel::Export) {
    m.x = std::max(8.0f, w - m.w - 8 - inset_[2]);
    m.y = 34 + inset_[1];
  } else {
    m.x = (w - m.w) / 2;
    m.y = (h - m.h) / 2;
  }
  panel_rect_ = m;

  if (ov_.pressed && !m.contains(ov_.x, ov_.y)) {
    close_panel();
    return;
  }
  dl_.rrect(m.x - 1, m.y - 1, m.w + 2, m.h + 2, 8, with_alpha(th.accent, 160));
  dl_.rrect(m.x, m.y, m.w, m.h, 8, th.panel);
  const std::string head = skin_->fit(title, 14, 1, m.w - 70);
  dl_.text(m.x + 16, m.y + 12, head, 14, th.text, Align::Left, m.w - 70, 1);
  if (ui_button({m.x + m.w - 34, m.y + 8, 24, 24}, "x", false)) {
    close_panel();
    return;
  }
  if (panel_ == Panel::Export) draw_export_menu(m);
  else if (panel_ == Panel::AddTo) draw_add_menu(m);
  else if (panel_ == Panel::Settings) draw_settings(m);
  else if (panel_ == Panel::Sampler) draw_sampler(m);
  else if (panel_ == Panel::Learn) draw_learn(m);
  else if (panel_ == Panel::Tags) draw_tags(m);
  else if (panel_ == Panel::Relink) draw_relink(m);
}

std::string App::export_what_() {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  if (view_ == TreeItem::Playlist && tree_sel_ >= 0 && tree_sel_ < static_cast<int>(tree_.size())) return "\"" + tree_[static_cast<std::size_t>(tree_sel_)].name + "\"";
  return T("export.all_playlists");
}

// ---- export destinations and add-to menu ---------------------------------------------------------------------
void App::draw_export_menu(const Rect& m) {
  std::int64_t list = 0;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (view_ == TreeItem::Playlist) list = playlist_id_;
  }
  for (std::size_t i = 0; i < export_targets_.size(); ++i) {
    const Rect row{m.x + 6, m.y + 38 + 26 * static_cast<float>(i), m.w - 12, 24};
    if (row.contains(ov_.x, ov_.y)) dl_.rrect(row.x, row.y, row.w, row.h, 4, with_alpha(theme_.accent, 90));
    dl_.text(row.x + 8, row.y + 5, skin_->fit(export_targets_[i], 12, 0, row.w - 16), 12, theme_.text, Align::Left, row.w - 16);
    if (ov_.pressed && row.contains(ov_.x, ov_.y)) {
      Job j;
      j.kind = Job::Export;
      j.path = export_targets_[i];
      j.track_id = list;
      post(j);
      close_panel();
      return;
    }
  }
}

void App::draw_add_menu(const Rect& m) {
  std::vector<std::pair<std::int64_t, std::string>> lists;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    for (const TreeItem& item : tree_)
      if (item.kind == TreeItem::Playlist) lists.emplace_back(item.id, item.name);
  }
  if (lists.empty()) dl_.text(m.x + 16, m.y + 46, T("playlist.none"), 12, theme_.dim);
  dl_.push_clip(m.x, m.y + 38, m.w, m.h - 40);
  for (std::size_t i = 0; i < lists.size(); ++i) {
    const Rect row{m.x + 6, m.y + 42 + 26 * static_cast<float>(i), m.w - 12, 24};
    if (row.contains(ov_.x, ov_.y)) dl_.rrect(row.x, row.y, row.w, row.h, 4, with_alpha(theme_.accent, 90));
    dl_.text(row.x + 8, row.y + 5, skin_->fit(lists[i].second, 12, 0, row.w - 16), 12, theme_.text, Align::Left, row.w - 16);
    if (ov_.pressed && row.contains(ov_.x, ov_.y)) {
      lib_->add_to_playlist(lists[i].first, edit_track_);
      rows_dirty_ = true;
      status_note_ = T("status.added_to", {{"name", lists[i].second}});
      dl_.pop_clip();
      close_panel();
      return;
    }
  }
  dl_.pop_clip();
}

// ---- settings ----------------------------------------------------------------------------------------------------------
void App::draw_settings(const Rect& m) {
  const ui::Theme& th = theme_;
  if (last_focus_ && last_focus_ != focus_) commit_settings();  // a field lost focus: save it
  last_focus_ = focus_;
  const Rect body{m.x, m.y + 40, m.w, m.h - 48};
  if (body.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
  panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, panel_content_h_ - body.h));
  dl_.push_clip(body.x, body.y, body.w, body.h);
  const float x = m.x + 20, rw = m.w - 40;
  float y = body.y + 4 - panel_scroll_;
  const float line = 28;

  ui_heading(x, y, T("settings.language"));
  y += 18;
  {
    float bx = x;
    for (const i18n::LangInfo& l : i18n_.available()) {
      const float bw = std::min(150.0f, std::max(70.0f, dl_.text_width(l.name, 12, 1) + 24));
      if (bx + bw > x + rw) {
        bx = x;
        y += line;
      }
      if (ui_button({bx, y, bw, 24}, l.name, l.tag == i18n_.language()) && l.tag != i18n_.language()) {
        if (i18n_.set_language(l.tag)) {
          lib_->set_setting("ui.language", l.tag);
          rows_dirty_ = true;
        }
      }
      bx += bw + 6;
    }
    y += line + 6;
  }

  ui_heading(x, y, T("settings.skin"));
  y += 18;
  {
    float bx = x;
    for (const SkinEntry& s : skins_) {
      const float bw = std::min(150.0f, std::max(70.0f, dl_.text_width(s.name, 12, 1) + 24));
      if (bx + bw > x + rw) {
        bx = x;
        y += line;
      }
      if (ui_button({bx, y, bw, 24}, s.name, s.name == skin_name_) && s.name != skin_name_) use_skin(s.name, true);
      bx += bw + 6;
    }
    y += line + 6;
  }

  ui_heading(x, y, T("settings.audio"));
  y += 18;
  {
    std::string pref;
    std::uint32_t period;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      pref = audio_pref_;
      period = audio_period_;
    }
    const auto pick_device = [&](const std::string& name) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        audio_pref_ = name;
      }
      save_setting("audio.device", name);
      restart_audio();
    };
    const float row_h = 24;
    const std::size_t shown = std::min<std::size_t>(output_devices_.size() + 1, 6);
    const Rect list{x, y, rw, row_h * static_cast<float>(shown) + 4};
    dl_.rrect(list.x, list.y, list.w, list.h, 4, rgba(10, 11, 14));
    dl_.push_clip(list.x, list.y, list.w, list.h);
    for (std::size_t i = 0; i <= output_devices_.size(); ++i) {
      const std::string name = i == 0 ? std::string() : output_devices_[i - 1];
      const Rect row{list.x + 2, list.y + 2 + row_h * static_cast<float>(i) - dev_scroll_, list.w - 4, row_h};
      if (row.y + row.h < list.y || row.y > list.bottom()) continue;
      const bool sel = name == pref;
      if (sel) dl_.rrect(row.x, row.y, row.w, row.h, 3, with_alpha(th.accent, 110));
      else if (row.contains(ov_.x, ov_.y) && list.contains(ov_.x, ov_.y)) dl_.rrect(row.x, row.y, row.w, row.h, 3, with_alpha(th.text, 20));
      const std::string label = i == 0 ? T("settings.audio_follow") : name;
      dl_.text(row.x + 8, row.y + 5, skin_->fit(label, 12, 0, row.w - 16), 12, th.text, Align::Left, row.w - 16);
      if (ov_.pressed && row.contains(ov_.x, ov_.y) && list.contains(ov_.x, ov_.y) && !sel) pick_device(name);
    }
    dl_.pop_clip();
    const float total = row_h * static_cast<float>(output_devices_.size() + 1);
    if (list.contains(ov_.x, ov_.y) && total > list.h) {
      dev_scroll_ = std::clamp(dev_scroll_ - ov_.scroll, 0.0f, total - list.h + 4);
      panel_scroll_ += ov_.scroll;  // the list takes the wheel, not the panel
    }
    y += list.h + 10;

    dl_.text(x, y + 4, T("settings.buffer"), 12, th.dim);
    float bx = x + 130;
    for (const std::uint32_t p : kPeriods) {
      if (ui_button({bx, y, 54, 24}, std::to_string(p), p == period) && p != period) {
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          audio_period_ = p;
        }
        save_setting("audio.period", std::to_string(p));
        restart_audio();
      }
      bx += 58;
    }
    y += line;
    dl_.text(x + 130, y, T("settings.latency", {{"ms", static_cast<double>(period) / std::max(1.0, engine_->sample_rate() / 1000.0)}}), 11, th.dim);
    y += 24;
  }

  ui_heading(x, y, T("settings.api"));
  y += 18;
  {
    const bool on = sv_["api.enabled"] != "0";
    if (ui_toggle({x, y, 200, 24}, T("settings.api_enabled"), on)) {
      sv_["api.enabled"] = on ? "0" : "1";
      save_setting("api.enabled", sv_["api.enabled"]);
    }
    dl_.text(x + 230, y + 5, T("settings.port"), 12, th.dim);
    ui_field({x + 290, y, 90, 24}, f_port_, "7373");
    y += line;
    dl_.text(x, y, T("settings.next_start"), 11, th.dim);
    y += 24;
  }

  ui_heading(x, y, T("settings.streaming"));
  y += 18;
  {
    const struct {
      const char* key;
      const char* label;
    } toggles[] = {{"stream.files", "settings.stream_files"}, {"stream.overlay", "settings.stream_overlay"}, {"obs.enabled", "settings.obs"}};
    for (const auto& t : toggles) {
      const bool on = sv_[t.key] == "1";
      if (ui_toggle({x, y, rw, 24}, T(t.label), on)) {
        sv_[t.key] = on ? "0" : "1";
        save_setting(t.key, sv_[t.key]);
      }
      y += line;
    }
    dl_.text(x, y + 5, T("settings.obs_host"), 12, th.dim);
    ui_field({x + 90, y, 180, 24}, f_obs_host_, "127.0.0.1");
    dl_.text(x + 290, y + 5, T("settings.port"), 12, th.dim);
    ui_field({x + 340, y, 80, 24}, f_obs_port_, "4455");
    y += line;
    dl_.text(x, y + 5, T("settings.obs_source"), 12, th.dim);
    ui_field({x + 90, y, 330, 24}, f_obs_source_, "");
    y += line;
    dl_.text(x, y, T("settings.next_start"), 11, th.dim);
    y += 24;
  }
  dl_.pop_clip();
  panel_content_h_ = y + panel_scroll_ - body.y + 8;
}

// ---- sampler -----------------------------------------------------------------------------------------------------------
void App::draw_sampler_grid(const Rect& r, int cols, int rows, const ui::Input& in) {
  const ui::Theme& th = theme_;
  const float gap = 5, cw = (r.w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols),
              ch = (r.h - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows);
  std::string names[kSamplerSlots];
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (int i = 0; i < kSamplerSlots; ++i) names[i] = sampler_names_[i];
  }
  for (int k = 0; k < cols * rows && k < kSamplerSlots; ++k) {
    const Rect c{r.x + static_cast<float>(k % cols) * (cw + gap), r.y + static_cast<float>(k / cols) * (ch + gap), cw, ch};
    sampler_slot_rect_[k] = c;
    const SamplerView& sv = samplers_[k];
    const bool hover = c.contains(in.x, in.y);
    Color fill = sv.playing ? mix(th.panel2, th.accent, 0.55f) : sv.loaded ? th.panel2 : mix(th.panel, th.bg, 0.5f);
    if (hover) fill = mix(fill, rgba(255, 255, 255), 0.12f);
    dl_.rrect(c.x, c.y, c.w, c.h, 5, fill);
    dl_.rrect_outline(c.x, c.y, c.w, c.h, 5, sv.playing ? th.accent : th.line, 1);
    dl_.text(c.x + 6, c.y + 4, std::to_string(k + 1), 10, th.dim, Align::Left, 0, 1);
    const std::string label = sv.loaded ? (names[k].empty() ? T("sampler.loaded") : names[k]) : T("sampler.empty_slot");
    const std::string t = skin_->fit(label, 11, 0, c.w - 12);
    dl_.text(c.x + c.w / 2, c.y + (c.h - dl_.text_height(t, 11, 0)) / 2 + 2, t, 11, sv.loaded ? th.text : th.dim, Align::Center, c.w - 8);
    const Rect stop{c.right() - 18, c.y + 4, 14, 14};
    if (sv.loaded) {
      if (sv.length > 0) dl_.rect(c.x + 4, c.bottom() - 6, (c.w - 8) * static_cast<float>(std::clamp(sv.position / sv.length, 0.0, 1.0)), 3, th.accent);
      dl_.rect(stop.x + 3, stop.y + 3, 8, 8, sv.playing ? th.text : th.dim);  // stop zone
    }
    if (in.pressed && c.contains(in.x, in.y) && sv.loaded) {
      std::string error;
      const std::string base = "sampler." + std::to_string(k + 1) + ".";
      if (stop.contains(in.x, in.y) || in.shift) apply_control(*engine_, base + "stop", false, 0, error);
      else apply_control(*engine_, base + "trigger", false, 0, error);
    }
  }
}

void App::draw_sampler(const Rect& m) {
  const ui::Theme& th = theme_;
  const Rect grid{m.x + 16, m.y + 44, m.w - 32, m.h - 44 - 84};
  draw_sampler_grid(grid, 4, 4, ov_);
  float y = grid.bottom() + 12;
  dl_.text(m.x + 16, y + 5, T("sampler.level"), 12, th.dim);
  {  // level slider
    const ui::Target t = ui::map_binding("sampler.level");
    double v = 0;
    read_control(*engine_, t.name, v);
    const Rect tr{m.x + 90, y + 10, m.w - 90 - 150, 6};
    dl_.rrect(tr.x, tr.y, tr.w, tr.h, 3, rgba(10, 11, 14));
    dl_.rrect(tr.x, tr.y, tr.w * static_cast<float>(v), tr.h, 3, th.accent);
    dl_.circle(tr.x + tr.w * static_cast<float>(v), tr.y + 3, 7, th.text);
    const Rect hit{tr.x - 8, tr.y - 8, tr.w + 16, tr.h + 16};
    if (ov_.pressed && hit.contains(ov_.x, ov_.y)) held_ = 990;
    if (held_ == 990 && ov_.down) set_number(t, std::clamp((ov_.x - tr.x) / tr.w, 0.0f, 1.0f));
    if (held_ == 990 && !ov_.down) held_ = 0;
    if (ov_.pressed && hit.contains(ov_.x, ov_.y) && ov_.clicks >= 2) set_number(t, t.def);
  }
  if (ui_button({m.x + m.w - 130, y, 114, 26}, T("sampler.stop_all"), false)) {
    Command c;
    c.op = Op::SamplerStop;
    c.i = -1;
    engine_->send(c);
  }
  {  // the hint wraps onto a second line at a word boundary when it does not fit
    const std::string hint = T("sampler.hint");
    const float full = dl_.text_width(hint, 11, 0), room = m.w - 32;
    std::size_t cut = std::string::npos;
    if (full > room) cut = hint.rfind(' ', static_cast<std::size_t>(static_cast<float>(hint.size()) * room / full));
    if (cut == std::string::npos) {
      dl_.text(m.x + 16, y + 38, skin_->fit(hint, 11, 0, room), 11, th.dim, Align::Left, room);
    } else {
      dl_.text(m.x + 16, y + 38, skin_->fit(hint.substr(0, cut), 11, 0, room), 11, th.dim, Align::Left, room);
      dl_.text(m.x + 16, y + 53, skin_->fit(hint.substr(cut + 1), 11, 0, room), 11, th.dim, Align::Left, room);
    }
  }
}

// ---- MIDI learn ------------------------------------------------------------------------------------------------------------
void App::learn_pick(const std::string& action) {
  learn_action_ = action;
  learn_line_.clear();
  learn_saved_ = false;
  learn_note_.clear();
  if (!hub_->learn_begin(action)) learn_note_ = hub_->learn_error();
}

void App::draw_learn(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 16, rw = m.w - 32;
  const LearnState st = hub_->learn_state();
  const bool busy = st != LearnState::Idle && st != LearnState::Failed;

  ui_field({x, m.y + 44, rw, 26}, f_search_learn_, T("learn.search"));
  const std::string q = lower(trim(f_search_learn_.text));
  const Rect list{x, m.y + 78, rw, m.h - 78 - 128};
  dl_.rrect(list.x, list.y, list.w, list.h, 4, rgba(10, 11, 14));
  std::vector<const LearnEntry*> shown;
  for (const LearnEntry& e : learn_list_)
    if (q.empty() || lower(e.label).find(q) != std::string::npos || lower(e.action).find(q) != std::string::npos) shown.push_back(&e);
  const float row_h = 22, content = static_cast<float>(shown.size()) * row_h;
  if (list.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
  panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, content - list.h + 4));
  dl_.push_clip(list.x, list.y, list.w, list.h);
  for (std::size_t i = 0; i < shown.size(); ++i) {
    const Rect row{list.x + 2, list.y + 2 + static_cast<float>(i) * row_h - panel_scroll_, list.w - 4, row_h};
    if (row.bottom() < list.y || row.y > list.bottom()) continue;
    const bool sel = shown[i]->action == learn_action_;
    if (sel) dl_.rrect(row.x, row.y, row.w, row.h, 3, with_alpha(th.accent, 120));
    else if (row.contains(ov_.x, ov_.y) && list.contains(ov_.x, ov_.y)) dl_.rrect(row.x, row.y, row.w, row.h, 3, with_alpha(th.text, 20));
    dl_.text(row.x + 8, row.y + 4, skin_->fit(shown[i]->label, 12, 0, row.w * 0.62f), 12, th.text, Align::Left, row.w * 0.62f);
    dl_.text(row.right() - 8, row.y + 5, skin_->fit(shown[i]->action, 11, 0, row.w * 0.34f), 11, th.dim, Align::Right, row.w * 0.34f);
    if (ov_.pressed && row.contains(ov_.x, ov_.y) && list.contains(ov_.x, ov_.y)) learn_pick(shown[i]->action);
  }
  dl_.pop_clip();

  // ---- capture status
  float y = list.bottom() + 10;
  std::string line = hub_->learn_result(false);
  if (st == LearnState::Observing && line.empty()) line = hub_->learn_result(false);
  std::string status;
  Color status_color = th.dim;
  if (learn_action_.empty()) {
    status = T("learn.pick");
  } else if (st == LearnState::Waiting) {
    status = T("learn.waiting", {{"action", learn_action_}});
    status_color = th.text;
  } else if (st == LearnState::Observing) {
    status = T("learn.observing");
    status_color = th.text;
  } else if (st == LearnState::Done) {
    learn_line_ = line;
    status = T("learn.captured", {{"device", hub_->learn_device()}});
    status_color = th.good;
  } else if (st == LearnState::Failed) {
    status = hub_->learn_error().empty() ? learn_note_ : hub_->learn_error();
    status_color = th.warn;
  }
  if (learn_saved_) {
    status = learn_note_;
    status_color = th.good;
  }
  dl_.text(x, y, skin_->fit(status, 12, 1, rw), 12, status_color, Align::Left, rw, 1);
  y += 20;
  dl_.rrect(x, y, rw, 24, 4, rgba(10, 11, 14));
  const std::string shown_line = !learn_line_.empty() ? learn_line_ : line;
  if (!shown_line.empty()) dl_.text(x + 8, y + 5, skin_->fit(shown_line, 12, 0, rw - 16), 12, th.text, Align::Left, rw - 16);
  y += 34;
  const bool can_save = st == LearnState::Done && !learn_line_.empty() && !learn_saved_;
  if (ui_button({x, y, 150, 26}, T("learn.save"), can_save, th.good) && can_save) {
    const std::string device = hub_->learn_device();
    std::string error;
    if (hub_->learn_commit() && save_user_mapping(device, {learn_line_}, data_dir_ + "/controllers", &error)) {
      learn_saved_ = true;
      learn_note_ = T("learn.saved", {{"path", data_dir_ + "/controllers"}});
      learn_line_.clear();
      learn_action_.clear();
    } else {
      learn_note_ = error.empty() ? hub_->learn_error() : error;
    }
  }
  if (ui_button({x + 158, y, 120, 26}, T("learn.cancel"), false) && busy) {
    hub_->learn_cancel();
    learn_action_.clear();
    learn_line_.clear();
  }
  if (st == LearnState::Observing && ui_button({x + 286, y, 150, 26}, T("learn.done"), false)) hub_->learn_result(true);
}

// ---- track details -------------------------------------------------------------------------------------------------------------
void App::draw_tags(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 20, lw = 70, fx = x + lw, fw = m.w - 40 - lw;
  float y = m.y + 48;
  const struct {
    const char* label;
    Field* f;
  } rows[] = {{"tags.title_field", &f_title_}, {"tags.artist", &f_artist_}, {"tags.key", &f_key_}};
  for (const auto& r : rows) {
    dl_.text(x, y + 5, T(r.label), 12, th.dim, Align::Left, lw - 6);
    ui_field({fx, y, fw, 26}, *r.f, "");
    y += 34;
  }
  dl_.text(x, y + 5, T("tags.rating"), 12, th.dim, Align::Left, lw - 6);
  for (int s = 1; s <= 5; ++s) {
    if (ui_button({fx + static_cast<float>(s - 1) * 34, y, 30, 26}, std::to_string(s), s <= tag_rating_, th.warn) ) tag_rating_ = tag_rating_ == s ? s - 1 : s;
  }
  y += 36;
  const bool exists = std::filesystem::exists(tag_path_);
  dl_.text(x, y + 2, T("tags.file"), 12, th.dim, Align::Left, lw - 6);
  dl_.text(fx, y + 2, skin_->fit(tag_path_, 11, 0, fw), 11, exists ? th.text : th.warn, Align::Left, fw);
  if (!exists) dl_.text(fx, y + 18, T("tags.missing"), 11, th.warn);
  y += 40;
  if (ui_button({x, y, 130, 28}, T("tags.save"), true, th.good)) {
    TrackEdit e;
    e.title = trim(f_title_.text);
    e.artist = trim(f_artist_.text);
    e.key = trim(f_key_.text);
    e.rating = tag_rating_;
    if (e.title->empty()) e.title = std::filesystem::path(tag_path_).stem().string();
    lib_->edit_track(edit_track_, e);
    rows_dirty_ = true;
    status_note_ = T("tags.saved", {{"title", *e.title}});
    close_panel();
    return;
  }
  if (ui_button({x + 138, y, 150, 28}, T("tags.relink"), false)) {
    open_panel(Panel::Relink);
    return;
  }
  const bool sure = confirm_remove_;
  if (ui_button({x + 296, y, m.w - 40 - 296, 28}, sure ? T("tags.remove_confirm") : T("tags.remove"), sure, th.warn)) {
    if (sure) {
      const std::string title = f_title_.text;
      lib_->remove_track(edit_track_);
      rows_dirty_ = true;
      status_note_ = T("tags.removed", {{"title", title}});
      close_panel();
      return;
    }
    confirm_remove_ = true;
  }
}

void App::draw_relink(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 20, lw = 100, fx = x + lw, fw = m.w - 40 - lw;
  float y = m.y + 48;
  dl_.text(x, y + 5, T("relink.old"), 12, th.dim, Align::Left, lw - 6);
  ui_field({fx, y, fw, 26}, f_relink_old_, "");
  y += 34;
  dl_.text(x, y + 5, T("relink.new"), 12, th.dim, Align::Left, lw - 6);
  ui_field({fx, y, fw, 26}, f_relink_, "/Volumes/...");
  y += 38;
  dl_.text(x, y, skin_->fit(T("relink.help"), 11, 0, m.w - 40), 11, th.dim, Align::Left, m.w - 40);
  y += 26;
  const bool can = !trim(f_relink_old_.text).empty() && !trim(f_relink_.text).empty();
  if (ui_button({x, y, 130, 28}, T("relink.apply"), can, th.good) && can) {
    const int n = lib_->relink_prefix(trim(f_relink_old_.text), trim(f_relink_.text));
    rows_dirty_ = true;
    status_note_ = n > 0 ? T("relink.done", {{"count", n}}) : T("relink.none");
    close_panel();
    return;
  }
  if (ui_button({x + 138, y, 110, 28}, T("learn.cancel"), false)) open_panel(Panel::Tags);
}

}  // namespace deckhand
