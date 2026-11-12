// Skin-driven UI glue: skin discovery and switching, the SkinHost implementation (bindings -> engine,
// library and app actions) and the complex widgets (waveform, overview, jog, pads, playlist tree, track
// table, search field) that are drawn into the rectangle the skin gives them.
#include "app_impl.hpp"

namespace deckhand {

namespace {
constexpr float kPi = 3.14159265358979f;
const double kRanges[] = {0.06, 0.10, 0.16, 1.0};

}  // namespace

// ---- skins ----------------------------------------------------------------------------------------
void App::scan_skins() {
  skins_.clear();
  const auto scan = [&](const std::string& dir) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      const std::filesystem::path p = it->path();
      const bool file = p.extension() == ".dhskin" && it->is_regular_file(ec);
      const bool folder = it->is_directory(ec) && std::filesystem::exists(p / "skin.dhs", ec);
      if (!file && !folder) continue;
      SkinEntry e{p.stem().string(), p.string()};
      bool replaced = false;
      for (SkinEntry& old : skins_)
        if (old.name == e.name) {
          old = e;  // a user skin overrides a built-in one of the same name
          replaced = true;
        }
      if (!replaced) skins_.push_back(std::move(e));
    }
  };
  if (!resource_dir_.empty()) scan(resource_dir_ + "/skins");
  scan(data_dir_ + "/skins");
  std::sort(skins_.begin(), skins_.end(), [](const SkinEntry& a, const SkinEntry& b) {
    if ((a.name == "default") != (b.name == "default")) return a.name == "default";
    return a.name < b.name;
  });
}

bool App::use_skin(const std::string& name, bool persist) {
  const SkinEntry* entry = nullptr;
  for (const SkinEntry& e : skins_)
    if (e.name == name) entry = &e;
  if (!entry) {
    skin_error_ = "skin not found: " + name;
  } else if (skin_->load(entry->path, skin_error_)) {
    skin_error_.clear();
    legacy_ = false;
    theme_ = skin_->theme();
    skin_name_ = name;
    if (!skin_->has_window(variant_)) variant_ = "main";
    if (persist) lib_->set_setting("ui.skin", name);
    return true;
  }
  DH_LOG(Warn, "skin '%s': %s", name.c_str(), skin_error_.c_str());
  const std::string first = skin_error_.substr(0, skin_error_.find('\n'));
  status_note_ = T("status.skin_error", {{"name", name}, {"message", first}});
  if (!skin_->loaded()) {  // not even the built-in skin: keep the code-drawn fallback
    legacy_ = true;
    theme_ = ui::Theme::defaults();
  }
  return false;
}

void App::cycle_skin() {
  if (skins_.size() < 2) return;
  std::size_t at = 0;
  for (std::size_t i = 0; i < skins_.size(); ++i)
    if (skins_[i].name == skin_name_) at = i;
  for (std::size_t step = 1; step <= skins_.size(); ++step) {  // skip skins that fail to load
    const SkinEntry& next = skins_[(at + step) % skins_.size()];
    if (use_skin(next.name, true)) {
      status_note_ = T("status.skin_changed", {{"name", next.name}});
      return;
    }
  }
}

void App::cycle_language() {
  const auto langs = i18n_.available();
  if (langs.size() < 2) return;
  std::size_t at = 0;
  for (std::size_t i = 0; i < langs.size(); ++i)
    if (langs[i].tag == i18n_.language()) at = i;
  const std::string next = langs[(at + 1) % langs.size()].tag;
  if (i18n_.set_language(next)) {
    lib_->set_setting("ui.language", next);
    rows_dirty_ = true;
  }
}

void App::toggle_layout() {
  if (!skin_->has_window("four")) return;
  variant_ = variant_ == "four" ? "main" : "four";
  lib_->set_setting("ui.layout", variant_);
}

bool App::import_available() {
  if (now_ - import_checked_ > 2.0) {
    import_checked_ = now_;
    import_ok_ = import_v6_available() && std::filesystem::exists(source_db());
  }
  return import_ok_;
}

// ---- the skin host ------------------------------------------------------------------------------------
bool App::read_number(const ui::Target& t, double& v) {
  if (t.type == ui::Target::Type::Control) return read_control(*engine_, t.name, v);
  if (t.type != ui::Target::Type::Info) return false;
  const int d = t.deck;
  if (t.name == "ch_level" && d >= 0 && d < kMaxDecks) {
    v = views_[d].level;
    return true;
  }
  if (t.name == "master_level") {
    v = std::max(mixer_.peak_left, mixer_.peak_right);
    return true;
  }
  if (t.name == "count") {
    v = static_cast<double>(rows_.size());
    return true;
  }
  return false;
}

bool App::read_text(const ui::Target& t, std::string& out) {
  if (t.type == ui::Target::Type::None) return false;
  const int d = t.deck;
  const bool deck_ok = d >= 0 && d < kMaxDecks;
  const std::string& n = t.name;
  if (deck_ok && t.type == ui::Target::Type::Info) {
    const DeckView& v = views_[d];
    const DeckInfo& in = info_[d];
    if (n == "letter") out = deck_letter(d);
    else if (n == "title") out = v.loaded ? in.row.title : (in.loading ? T("deck.loading") : (!in.error.empty() ? in.error : T("deck.no_track")));
    else if (n == "artist") out = v.loaded ? in.row.artist : std::string();
    else if (n == "key") out = v.loaded ? in.row.key : std::string();
    else if (n == "bpm") out = v.bpm > 0 ? fmt("%.1f", v.bpm) : "--.-";
    else if (n == "time" || n == "elapsed" || n == "remaining") {
      const double sec = v.loaded ? v.position / v.sample_rate : 0, total = v.loaded ? v.length / v.sample_rate : 0;
      out = n == "elapsed" ? clock_text(sec) : n == "remaining" ? clock_text(sec - total) : clock_text(sec) + "  " + clock_text(sec - total);
    } else if (n == "loop") {
      out.clear();
      if (v.loop_active) {
        const double beats = (v.loop_out - v.loop_in) / (60.0 / std::max(1.0, v.bpm / (1.0 + v.tempo)) * v.sample_rate);
        out = beats >= 1 ? T("deck.loop", {{"beats", std::lround(beats)}}) : T("deck.loop_fraction", {{"divisor", std::lround(1.0 / std::max(beats, 0.001))}});
      }
    } else if (n == "key_shift") {
      const long semis = std::lround(v.key_shift);
      out = semis == 0 ? "0" : fmt("%+ld", semis);
    } else if (n == "fx_name" && t.index >= 0 && t.index < kFxSlots) {
      const EffectInfo* fi = effect_info(v.fx[t.index].kind);
      out = fi && v.fx[t.index].kind != EffectKind::None ? fi->name : "FX " + std::to_string(t.index + 1);
    } else return false;
    return true;
  }
  if (deck_ok && t.type == ui::Target::Type::Control) {
    const DeckView& v = views_[d];
    if (n.size() > 6 && n.compare(n.size() - 6, 6, ".tempo") == 0) {
      out = fmt("%+.2f%%", v.tempo * 100.0);
      return true;
    }
    if (t.special == ui::Special::TempoRange) {
      out = fmt("\xC2\xB1%d%%", static_cast<int>(std::lround(v.tempo_range * 100)));
      return true;
    }
    if (t.special == ui::Special::FxSelect && t.index >= 0 && t.index < kFxSlots) {
      const EffectInfo* fi = effect_info(v.fx[t.index].kind);
      out = fi && v.fx[t.index].kind != EffectKind::None ? fi->name : "FX " + std::to_string(t.index + 1);
      return true;
    }
    return false;
  }
  if (n == "controller" && t.type == ui::Target::Type::Info) {
    const auto c = hub_->connected();
    out = c.empty() ? T("app.no_controller") : c.front();
  } else if (n == "audio" && t.type == ui::Target::Type::Info) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    out = audio_label_;
  } else if (n == "status" && t.type == ui::Target::Type::Info) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    out = status_;
  } else if (n == "search" && t.type == ui::Target::Type::Info) {
    out = search_;
  } else if (t.type == ui::Target::Type::Action) {
    if (n == "rec") {
      const RecorderStatus rec = engine_->recorder_status();
      out = rec.recording ? T("app.rec") + " " + clock_text(rec.seconds) : T("app.rec");
    } else if (n == "language") {
      out = i18n_.language();
    } else if (n == "skin") {
      out = skin_name_;
    } else if (n == "layout") {
      out = variant_ == "four" ? "4" : "2";
    } else {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

bool App::read_flag(const ui::Target& t) {
  const int d = t.deck;
  const bool deck_ok = d >= 0 && d < kMaxDecks;
  if (t.type == ui::Target::Type::Control) {
    if (t.special == ui::Special::TempoRange || t.special == ui::Special::FxSelect || t.special == ui::Special::Step ||
        t.special == ui::Special::SetValue)
      return false;
    double v = 0;
    return read_control(*engine_, t.name, v) && v != 0;
  }
  const std::string& n = t.name;
  if (t.type == ui::Target::Type::Info) {
    if (n == "loaded") return deck_ok && views_[d].loaded;
    if (n == "has_import") return import_available();
    if (n == "multi_language") return i18n_.available().size() > 1;
    if (n == "multi_skin") return skins_.size() > 1;
    if (n == "has_four") return skin_ && skin_->has_window("four");
    if (n == "can_remove") {
      std::lock_guard<std::mutex> lock(browse_mutex_);
      return view_is_editable_playlist() && sel_ >= 0 && sel_ < static_cast<int>(rows_.size());
    }
    return false;
  }
  if (t.type == ui::Target::Type::Action) {
    if (n == "rec") return engine_->recorder_status().recording;
    if (n == "export") return panel_ == Panel::Export;
    if (n == "settings") return panel_ == Panel::Settings;
    if (n == "sampler") return panel_ == Panel::Sampler;
    if (n == "learn") return panel_ == Panel::Learn;
    if (n == "layout") return variant_ == "four";
  }
  return false;
}

void App::press(const ui::Target& t) {
  std::string error;
  if (t.type == ui::Target::Type::Action) {
    do_action(t);
    return;
  }
  if (t.type != ui::Target::Type::Control) return;
  double cur = 0;
  switch (t.special) {
    case ui::Special::TempoRange: {
      read_control(*engine_, t.name, cur);
      std::size_t next = 0;
      for (std::size_t i = 0; i < std::size(kRanges); ++i)
        if (std::fabs(kRanges[i] - cur) < 0.005) next = (i + 1) % std::size(kRanges);
      apply_control(*engine_, t.name, true, kRanges[next], error);
      return;
    }
    case ui::Special::FxSelect: {
      read_control(*engine_, t.name, cur);
      int next = static_cast<int>(cur);
      do {
        next = (next + 1) % static_cast<int>(EffectKind::Count);
      } while (next != 0 && effect_info(static_cast<EffectKind>(next)) && effect_info(static_cast<EffectKind>(next))->colour);
      apply_control(*engine_, t.name, true, next, error);
      return;
    }
    case ui::Special::Step:
      read_control(*engine_, t.name, cur);
      apply_control(*engine_, t.name, true, std::clamp(std::round(cur) + t.step, t.lo, t.hi), error);
      return;
    case ui::Special::SetValue: apply_control(*engine_, t.name, true, t.def, error); return;
    case ui::Special::None: break;
  }
  if (t.momentary) apply_control(*engine_, t.name, true, 1, error);
  else apply_control(*engine_, t.name, false, 0, error);  // toggles flip
}

void App::release(const ui::Target& t) {
  if (t.type == ui::Target::Type::Control && t.momentary) {
    std::string error;
    apply_control(*engine_, t.name, true, 0, error);
  }
}

void App::set_number(const ui::Target& t, double v) {
  if (t.type != ui::Target::Type::Control) return;
  std::string error;
  apply_control(*engine_, t.name, true, std::clamp(v, t.lo, t.hi), error);
}

double App::beat_age_ms(const ui::Target& t) {
  if (t.deck < 0 || t.deck >= kMaxDecks) return -1;
  const DeckView& v = views_[t.deck];
  const DeckInfo& in = info_[t.deck];
  if (!v.loaded || !v.playing || in.grid.empty()) return -1;
  const GridSection* g = nullptr;
  for (const GridSection& s : in.grid)
    if (s.first_beat_frame <= v.position) g = &s;
  if (!g || g->bpm <= 0) return -1;
  const double interval = 60.0 / g->bpm * v.sample_rate;
  const double beats = (v.position - g->first_beat_frame) / interval;
  const double frac = beats - std::floor(beats);
  return frac * interval / v.sample_rate / std::max(0.2, v.speed) * 1000.0;
}

void App::do_action(const ui::Target& t) {
  const std::string& n = t.name;
  if (n == "import") {
    Job j;
    j.kind = Job::Import;
    post(j);
  } else if (n == "rec") {
    const RecorderStatus rec = engine_->recorder_status();
    std::string path, error;
    if (!record(!rec.recording, path, error)) status_note_ = T("status.error", {{"message", error}});
  } else if (n == "export") {
    bool busy;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      busy = exporting_;
    }
    if (!busy) {
      if (panel_ == Panel::Export) close_panel();
      else open_panel(Panel::Export);
    }
  } else if (n == "language") {
    cycle_language();
  } else if (n == "skin") {
    cycle_skin();
  } else if (n == "layout") {
    toggle_layout();
  } else if (n == "settings" || n == "sampler" || n == "learn") {
    const Panel p = n == "settings" ? Panel::Settings : n == "sampler" ? Panel::Sampler : Panel::Learn;
    if (panel_ == p) close_panel();
    else open_panel(p);
  } else if (n == "load") {
    std::int64_t id = 0;
    {
      std::lock_guard<std::mutex> lock(browse_mutex_);
      if (sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) id = rows_[static_cast<std::size_t>(sel_)].id;
    }
    if (id) request_load(t.deck, id);
  } else if (n == "add_to" || n == "edit_track") {
    bool have;
    {
      std::lock_guard<std::mutex> lock(browse_mutex_);
      have = sel_ >= 0 && sel_ < static_cast<int>(rows_.size());
      if (have) edit_track_ = rows_[static_cast<std::size_t>(sel_)].id;
    }
    if (have) open_panel(n == "add_to" ? Panel::AddTo : Panel::Tags);
  } else if (n == "remove" || n.compare(0, 9, "playlist_") == 0) {
    playlist_action(n);
  } else if (n == "sampler_stop") {
    Command c;
    c.op = Op::SamplerStop;
    c.i = -1;
    engine_->send(c);
  }
}

void App::playlist_action(const std::string& which) {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  const TreeItem* cur = tree_sel_ >= 0 && tree_sel_ < static_cast<int>(tree_.size()) ? &tree_[static_cast<std::size_t>(tree_sel_)] : nullptr;
  const bool editable = cur && (cur->kind == TreeItem::Playlist || cur->kind == TreeItem::Folder);
  const std::int64_t parent = cur && cur->kind == TreeItem::Folder ? cur->id : 0;
  if (which == "playlist_add_list") {
    rename_id_ = lib_->create_playlist(parent, T("playlist.new_list"), false);
    rename_buf_ = T("playlist.new_list");
    rename_fresh_ = true;
    view_ = TreeItem::Playlist;
    playlist_id_ = rename_id_;
    rows_dirty_ = true;
  } else if (which == "playlist_add_folder") {
    rename_id_ = lib_->create_playlist(parent, T("playlist.new_folder"), true);
    rename_buf_ = T("playlist.new_folder");
    rename_fresh_ = true;
    rows_dirty_ = true;
  } else if (which == "playlist_rename" && editable) {
    rename_id_ = cur->id;
    rename_buf_ = cur->name;
    rename_fresh_ = true;
  } else if (which == "playlist_delete" && editable) {
    if (confirm_delete_ == cur->id) {
      lib_->delete_playlist(cur->id);
      confirm_delete_ = 0;
      view_ = TreeItem::Collection;
      playlist_id_ = 0;
      rows_dirty_ = true;
    } else {
      confirm_delete_ = cur->id;
      status_note_ = T("playlist.confirm_delete", {{"name", cur->name}});
    }
  } else if (which == "remove" && view_is_editable_playlist() && sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) {
    lib_->remove_from_playlist(playlist_id_, sel_);
    rows_dirty_ = true;
  }
}

// ---- frame ------------------------------------------------------------------------------------------------
void App::draw_skin_ui() {
  const float w = width_px_ / scale_, h = height_px_ / scale_;
  skin_->layout(skin_->has_window(variant_) ? variant_ : "main", w, h, inset_[0], inset_[1], inset_[2], inset_[3]);
  // Release bookkeeping for held controls started outside the skin widgets.
  skin_->frame(*this, in_, now_);
  if (in_.released && drag_deck_ >= 0 && !in_.down) drag_deck_ = -1;
}

void App::draw_custom(const skin::ResolvedWidget& w, const ui::Target& t, const skin::Style& st, const Rect& r) {
  const int d = t.deck;
  const bool deck_ok = d >= 0 && d < kMaxDecks;
  const Color base = st.fg == 0xE6E6EAFF ? deck_color(std::max(d, 0)) : ui::to_gfx(st.fg, st.opacity);
  switch (w.kind) {
    case skin::Kind::Waveform: {
      if (!deck_ok) return;
      const auto mode = w.props.find("mode");
      if (mode != w.props.end() && mode->second == "overview") draw_overview(d, r, views_[d], info_[d], base, theme_);
      else draw_wave(d, r, views_[d], info_[d], base, theme_, true);
      break;
    }
    case skin::Kind::Jog:
      if (deck_ok) draw_jog(d, r, views_[d], base, st);
      break;
    case skin::Kind::Pads: draw_pads(t, w, r, st); break;
    case skin::Kind::Tree: draw_tree(r, theme_); break;
    case skin::Kind::Browser: {
      const auto cols = w.props.find("columns");
      const auto rh = w.props.find("row_h");
      draw_tracks(r, theme_, cols != w.props.end() ? cols->second : "title,artist,bpm,key,time",
                  rh != w.props.end() ? std::clamp(static_cast<float>(std::atof(rh->second.c_str())), 14.0f, 48.0f) : 22.0f);
      break;
    }
    case skin::Kind::Label: draw_search(r, st, skin_->text_of(w, t, *this, false)); break;
    default: break;
  }
}

// ---- complex widgets ----------------------------------------------------------------------------------------
void App::draw_search(const Rect& r, const skin::Style& st, const std::string& hint) {
  const bool inside = r.contains(in_.x, in_.y);
  if (in_.pressed) search_focus_ = inside;
  if (search_focus_) dl_.rrect_outline(r.x, r.y, r.w, r.h, st.radius, theme_.accent, 1);
  const std::string& shown = search_.empty() ? hint : search_;
  const std::string text = skin_->fit(shown + (search_focus_ && !search_.empty() ? "|" : ""), st.font_size, 0, r.w - 12);
  const float th = dl_.text_height(text, st.font_size, 0);
  dl_.text(r.x + 6, r.y + (r.h - th) / 2, text, st.font_size, search_.empty() ? theme_.dim : ui::to_gfx(st.fg, st.opacity), Align::Left, r.w - 12, 0);
}

void App::draw_wave(int di, const Rect& r, const DeckView& v, const DeckInfo& info, Color base, const ui::Theme& th, bool) {
  dl_.rect(r.x, r.y, 3, r.h, base);
  if (!info.audio || !v.loaded) {
    const std::string msg = info.loading ? T("deck.loading") : (!info.error.empty() ? info.error : T("deck.empty", {{"deck", deck_letter(di)}}));
    dl_.text(r.x + r.w / 2, r.y + r.h / 2 - 8, skin_ ? skin_->fit(msg, 13, 0, r.w - 40) : msg, 13, info.error.empty() ? th.dim : th.warn, Align::Center, r.w - 40);
    return;
  }
  const TrackAudio& a = *info.audio;
  const double sr = a.sample_rate, window = 6.0;
  const double pos = v.position / sr, pt_per_sec = r.w / window;
  const float cx = r.x + r.w / 2, mid = r.y + r.h / 2, half = r.h / 2 - 5;
  const auto x_of = [&](double seconds) { return cx + static_cast<float>((seconds - pos) * pt_per_sec); };
  dl_.push_clip(r.x + 3, r.y, r.w - 3, r.h);

  // Loop region behind the waveform.
  if (v.loop_in >= 0 && v.loop_out > v.loop_in) {
    const float x0 = x_of(v.loop_in / sr), x1 = x_of(v.loop_out / sr);
    dl_.rect(x0, r.y, x1 - x0, r.h, with_alpha(th.accent, v.loop_active ? 70 : 30));
  }
  const double t0 = pos - window / 2, t1 = pos + window / 2;
  // Waveform: one column per analysis point, three bands layered.
  const double per_point = 1.0 / a.wave_rate;
  const float col = static_cast<float>(per_point * pt_per_sec) + 0.5f;
  const long i0 = std::max(0L, static_cast<long>(t0 * a.wave_rate)),
             i1 = std::min(static_cast<long>(a.wave.size()), static_cast<long>(t1 * a.wave_rate) + 2);
  const Color c_low = mix(base, th.bg, 0.55f), c_high = mix(base, rgba(255, 255, 255), 0.7f);
  for (long i = i0; i < i1; ++i) {
    const WavePoint& p = a.wave[static_cast<std::size_t>(i)];
    const float x = x_of(static_cast<double>(i) * per_point);
    const float hl = half * p.low / 255.0f, hm = half * 0.8f * p.mid / 255.0f, hh = half * 0.55f * p.high / 255.0f;
    const float tall = std::max(hl, std::max(hm, hh));
    dl_.rect(x, mid - tall, col, tall * 2, c_low);
    if (hm > 0.5f) dl_.rect(x, mid - hm, col, hm * 2, base);
    if (hh > 0.5f) dl_.rect(x, mid - hh, col, hh * 2, c_high);
  }
  // Beat grid, over the waveform.
  for (std::size_t s = 0; s < info.grid.size(); ++s) {
    const GridSection& g = info.grid[s];
    if (g.bpm <= 0) continue;
    const double first = g.first_beat_frame / sr, interval = 60.0 / g.bpm;
    const double end = s + 1 < info.grid.size() ? info.grid[s + 1].first_beat_frame / sr : a.frames / sr;
    if (end < t0 || first > t1) continue;
    long n = std::max(0L, static_cast<long>(std::ceil((t0 - first) / interval)));
    for (double t = first + n * interval; t < std::min(t1, end - interval * 0.5); t = first + ++n * interval) {
      const bool bar = (n + g.bar_beat - 1) % 4 == 0;
      dl_.rect(x_of(t), r.y, 1, r.h, with_alpha(th.text, bar ? 120 : 45));
    }
  }
  // Cue point and hot cues.
  const float xc = x_of(v.cue / sr);
  dl_.triangle(xc - 5, r.y, xc + 5, r.y, xc, r.y + 8, th.text);
  const float tag_h = std::min(13.0f, r.h * 0.5f);
  for (int h = 0; h < kHotCues; ++h) {
    if (v.hot_cue[h] < 0) continue;
    const float x = x_of(v.hot_cue[h] / sr);
    dl_.rect(x, r.y, 1.5f, r.h, kHot[h]);
    dl_.rect(x, r.y, 14, tag_h, kHot[h]);
    if (tag_h > 9) dl_.text(x + 7, r.y, std::string(1, static_cast<char>('1' + h)), 10, th.bg, Align::Center, 0, 1);
  }
  dl_.pop_clip();
  dl_.rect(cx - 1, r.y, 2, r.h, rgba(255, 255, 255));  // playhead
}

void App::draw_overview(int di, const Rect& ov, const DeckView& v, const DeckInfo& info, Color base, const ui::Theme& th) {
  dl_.rect(ov.x, ov.y, ov.w, ov.h, rgba(12, 13, 16, 160));
  if (!v.loaded || info.overview.empty() || v.length <= 0) return;
  const float played = static_cast<float>(v.position / v.length) * ov.w;
  const std::size_t n = info.overview.size();
  const float cw = ov.w / static_cast<float>(n);
  for (std::size_t i = 0; i < n; ++i) {
    const WavePoint& p = info.overview[i];
    const float x = ov.x + static_cast<float>(i) * cw;
    const float h = std::max(1.0f, ov.h * std::max(p.low, std::max(p.mid, p.high)) / 255.0f);
    dl_.rect(x, ov.y + (ov.h - h) / 2, cw + 0.5f, h, x - ov.x < played ? mix(base, th.bg, 0.6f) : base);
  }
  for (int h = 0; h < kHotCues; ++h) {
    if (v.hot_cue[h] >= 0) dl_.rect(ov.x + static_cast<float>(v.hot_cue[h] / v.length) * ov.w, ov.y, 2, ov.h, kHot[h]);
  }
  dl_.rect(ov.x + played - 1, ov.y - 1, 2, ov.h + 2, rgba(255, 255, 255));
  if (clicked(ov)) drag_deck_ = di;  // click or drag to seek
  if (drag_deck_ == di && in_.down) {
    Command c;
    c.op = Op::Seek;
    c.deck = static_cast<std::uint8_t>(di);
    c.d = std::clamp((in_.x - ov.x) / ov.w, 0.0f, 1.0f) * v.length;
    engine_->send(c);
  }
  if (!in_.down && drag_deck_ == di) drag_deck_ = -1;
}

void App::draw_jog(int di, const Rect& r, const DeckView& v, Color base, const skin::Style& st) {
  const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rad = std::max(4.0f, std::min(r.w, r.h) / 2 - 3);
  const Color disc = (st.bg & 0xFF) ? ui::to_gfx(st.bg, st.opacity) : theme_.panel2;
  dl_.circle(cx, cy, rad, disc);
  dl_.arc(cx, cy, rad - 1, 0, 2 * kPi, 2, (st.border & 0xFF) ? ui::to_gfx(st.border, st.opacity) : theme_.line);
  const double sec = v.loaded ? v.position / v.sample_rate : 0;
  if (v.loaded && v.length > 0) {  // played part of the track as a ring
    const float frac = static_cast<float>(std::clamp(v.position / v.length, 0.0, 1.0));
    dl_.arc(cx, cy, rad - 5, -0.5f * kPi, -0.5f * kPi + 2 * kPi * frac, 4, with_alpha(base, 200));
  }
  dl_.circle(cx, cy, rad * 0.62f, mix(disc, theme_.bg, 0.35f));
  const float ang = static_cast<float>(sec * 0.5556 * 2 * kPi) - 0.5f * kPi;  // 33 1/3 rpm
  dl_.line(cx + std::cos(ang) * rad * 0.2f, cy + std::sin(ang) * rad * 0.2f, cx + std::cos(ang) * rad * 0.6f, cy + std::sin(ang) * rad * 0.6f, 3, v.playing ? base : theme_.dim);
  if (v.loaded && v.at_cue) dl_.circle(cx, cy, 3, theme_.warn);

  // Drag on the platter scratches while the deck is touched; the outer ring bends the pitch.
  const float dx = in_.x - cx, dy = in_.y - cy, dist = std::sqrt(dx * dx + dy * dy);
  const float pa = std::atan2(dy, dx);
  Command c;
  c.deck = static_cast<std::uint8_t>(di);
  if (in_.pressed && dist <= rad && v.loaded) {
    jog_deck_ = di;
    jog_top_ = dist < rad * 0.78f;
    jog_angle_ = pa;
    if (jog_top_) {
      c.op = Op::JogTouch;
      c.i = 1;
      engine_->send(c);
    }
  }
  if (jog_deck_ == di && in_.down) {
    float da = pa - jog_angle_;
    if (da > kPi) da -= 2 * kPi;
    if (da < -kPi) da += 2 * kPi;
    jog_angle_ = pa;
    if (std::fabs(da) > 1e-4f) {
      c.op = Op::JogTurn;
      c.d = da / (2 * kPi);
      c.i = jog_top_ ? 1 : 0;
      engine_->send(c);
    }
  }
  if (jog_deck_ == di && !in_.down) {
    if (jog_top_) {
      c.op = Op::JogTouch;
      c.i = 0;
      engine_->send(c);
    }
    jog_deck_ = -1;
  }
}

void App::draw_pads(const ui::Target& t, const skin::ResolvedWidget& w, const Rect& r, const skin::Style& st) {
  int cols = 4, rows = 2;
  if (w.props.count("cols")) cols = std::clamp(std::atoi(w.props.at("cols").c_str()), 1, 8);
  if (w.props.count("rows")) rows = std::clamp(std::atoi(w.props.at("rows").c_str()), 1, 8);
  if (t.name == "sampler_pads") {
    draw_sampler_grid(r, cols, rows, in_);
    return;
  }
  const int di = t.deck;
  if (di < 0 || di >= kMaxDecks) return;
  const DeckView& v = views_[di];
  const float gap = 3, cw = (r.w - gap * static_cast<float>(cols + 1)) / static_cast<float>(cols),
              ch = (r.h - gap * static_cast<float>(rows + 1)) / static_cast<float>(rows);
  const int id_base = (di + 1) * 100;
  for (int k = 0; k < cols * rows && k < kHotCues; ++k) {
    const Rect pr{r.x + gap + static_cast<float>(k % cols) * (cw + gap), r.y + gap + static_cast<float>(k / cols) * (ch + gap), cw, ch};
    const bool set = v.hot_cue[k] >= 0;
    const bool hover = pr.contains(in_.x, in_.y);
    Color fill = set ? kHot[k] : mix(theme_.panel2, rgba(255, 255, 255), 0.07f);
    if (hover) fill = mix(fill, rgba(255, 255, 255), 0.18f);
    dl_.rrect(pr.x, pr.y, pr.w, pr.h, std::min(st.radius, 5.0f), fill);
    const float fs = std::min(12.0f, std::max(8.0f, pr.h * 0.5f));
    dl_.text(pr.x + pr.w / 2, pr.y + (pr.h - fs * 1.25f) / 2, std::to_string(k + 1), fs, set ? theme_.bg : theme_.dim, Align::Center, 0, 1);
    if (clicked(pr)) {
      std::string error;
      apply_control(*engine_, "deck" + std::to_string(di + 1) + ".hotcue." + std::to_string(k + 1), true, 1, error);
      held_ = id_base + 10 + k;
    }
  }
  if (in_.released && held_ / 100 == di + 1 && held_ % 100 >= 10 && held_ % 100 < 20) {
    std::string error;
    apply_control(*engine_, "deck" + std::to_string(di + 1) + ".hotcue." + std::to_string(held_ % 100 - 10 + 1), true, 0, error);
    held_ = 0;
  }
}

void App::draw_tree(const Rect& r, const ui::Theme& th) {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  const float row_h = 22;
  const Rect tree = r;
  const float content = static_cast<float>(tree_.size()) * row_h + 12;
  if (tree.contains(in_.x, in_.y)) tree_scroll_ -= in_.scroll;
  tree_scroll_ = std::clamp(tree_scroll_, 0.0f, std::max(0.0f, content - tree.h));
  dl_.push_clip(tree.x, tree.y, tree.w, tree.h);
  float y = tree.y + 6 - tree_scroll_;
  for (int i = 0; i < static_cast<int>(tree_.size()); ++i) {
    const TreeItem& item = tree_[static_cast<std::size_t>(i)];
    const Rect row{tree.x, y, tree.w, row_h};
    y += row_h;
    if (row.y + row.h < tree.y || row.y > tree.y + tree.h) continue;
    if (i == tree_sel_) dl_.rect(row.x, row.y, row.w, row.h, with_alpha(th.accent, focus_tree_ ? 150 : 70));
    const bool container = item.kind == TreeItem::Folder || item.kind == TreeItem::HistoryHeader;
    const float indent = 10 + 14.0f * static_cast<float>(item.depth) + (item.kind == TreeItem::Collection ? 0 : 6);
    const float ty = row.y + (row_h - 15) / 2;
    if (rename_id_ != 0 && item.id == rename_id_ && (item.kind == TreeItem::Playlist || item.kind == TreeItem::Folder)) {
      dl_.rect(row.x + indent - 4, row.y + 2, tree.w - indent - 4, row.h - 4, rgba(10, 11, 14));
      dl_.outline(row.x + indent - 4, row.y + 2, tree.w - indent - 4, row.h - 4, th.accent);
      dl_.text(row.x + indent, ty, rename_buf_ + "|", 12, th.text, Align::Left, tree.w - indent - 12);
    } else {
      dl_.text(row.x + indent, ty, skin_->fit(item.name, 12, item.kind == TreeItem::Collection || container ? 1 : 0, tree.w - indent - 44), 12,
               container ? th.dim : th.text, Align::Left, tree.w - indent - 44, item.kind == TreeItem::Collection || container ? 1 : 0);
      if (!container) dl_.text(row.x + tree.w - 10, ty + 1, std::to_string(item.count), 10, th.dim, Align::Right);
    }
    if (clicked(row) && tree.contains(in_.x, in_.y)) {
      if (rename_id_ != 0 && item.id != rename_id_) commit_rename();
      tree_sel_ = i;
      focus_tree_ = false;
      apply_tree_selection();
      if (in_.clicks >= 2 && (item.kind == TreeItem::Playlist || item.kind == TreeItem::Folder)) {
        rename_id_ = item.id;
        rename_buf_ = item.name;
        rename_fresh_ = true;
      }
    }
  }
  dl_.pop_clip();
}

void App::draw_tracks(const Rect& table, const ui::Theme& th, const std::string& columns, float row_h) {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  // Column set from the skin: title, artist, bpm, key, time. Fixed widths for the short ones.
  struct Col {
    std::string name;
    float w;
    SortKey key;
    bool right, flex;
  };
  std::vector<Col> cols;
  for (std::size_t a = 0; a <= columns.size();) {
    const std::size_t c = columns.find(',', a);
    const std::string n = columns.substr(a, c == std::string::npos ? std::string::npos : c - a);
    if (n == "title") cols.push_back({n, 1.2f, SortKey::Title, false, true});
    else if (n == "artist") cols.push_back({n, 1.0f, SortKey::Artist, false, true});
    else if (n == "bpm") cols.push_back({n, 58, SortKey::Bpm, true, false});
    else if (n == "key") cols.push_back({n, 52, SortKey::Key, false, false});
    else if (n == "time") cols.push_back({n, 56, SortKey::Duration, true, false});
    if (c == std::string::npos) break;
    a = c + 1;
  }
  if (cols.empty()) return;
  const float badge = 30;
  float fixed = badge + 12, flex_sum = 0;
  for (const Col& c : cols) (c.flex ? flex_sum : fixed) += c.flex ? c.w : c.w;
  const float flex_w = std::max(40.0f, table.w - fixed);
  std::vector<float> x0(cols.size()), x1(cols.size());
  float x = table.x + badge;
  for (std::size_t i = 0; i < cols.size(); ++i) {
    x0[i] = x;
    x += cols[i].flex ? flex_w * cols[i].w / flex_sum : cols[i].w;
    x1[i] = x;
  }
  const float head_h = row_h + 2;
  dl_.rect(table.x, table.y, table.w, head_h, th.panel2);
  for (std::size_t i = 0; i < cols.size(); ++i) {  // click a column to sort: ascending, descending, natural
    const Col& c = cols[i];
    const bool active = sort_ == c.key;
    const std::string label = skin_->fit(T("browser.col." + c.name) + (active ? (sort_desc_ ? " \xE2\x96\xBE" : " \xE2\x96\xB4") : ""), 10, 1, x1[i] - x0[i] - 10);
    const float ty = table.y + (head_h - 13) / 2;
    if (c.right) dl_.text(x1[i] - 8, ty, label, 10, active ? th.text : th.dim, Align::Right, 0, 1);
    else dl_.text(x0[i] + 4, ty, label, 10, active ? th.text : th.dim, Align::Left, 0, 1);
    if (clicked({x0[i], table.y, x1[i] - x0[i], head_h}) && view_ != TreeItem::HistorySession) {
      if (!active) { sort_ = c.key; sort_desc_ = false; }
      else if (!sort_desc_) sort_desc_ = true;
      else sort_ = SortKey::Default;
      rows_dirty_ = true;
    }
  }

  const Rect body{table.x, table.y + head_h, table.w, table.h - head_h};
  const float content = static_cast<float>(rows_.size()) * row_h;
  if (body.contains(in_.x, in_.y)) scroll_ -= in_.scroll;
  if (follow_sel_) {
    const float top = static_cast<float>(sel_) * row_h;
    if (top < scroll_) scroll_ = top;
    if (top + row_h > scroll_ + body.h) scroll_ = top + row_h - body.h;
    follow_sel_ = false;
  }
  scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content - body.h));

  int load_deck = -1;
  dl_.push_clip(body.x, body.y, body.w, body.h);
  const int first = static_cast<int>(scroll_ / row_h);
  const int last = std::min(static_cast<int>(rows_.size()), first + static_cast<int>(body.h / row_h) + 2);
  for (int i = first; i < last; ++i) {
    const TrackRow& t = rows_[static_cast<std::size_t>(i)];
    const Rect row{body.x, body.y + static_cast<float>(i) * row_h - scroll_, body.w, row_h};
    if (i == sel_) dl_.rect(row.x, row.y, row.w, row.h, with_alpha(th.accent, focus_tree_ ? 70 : 150));
    else if (i % 2) dl_.rect(row.x, row.y, row.w, row.h, with_alpha(th.text, 8));
    const float ty = row.y + (row_h - 15) / 2;
    for (int di = 0; di < kMaxDecks; ++di) {
      if (views_[di].loaded && views_[di].track_id == t.id)
        dl_.text(row.x + 6 + 9.0f * static_cast<float>(di), ty + 1, deck_letter(di), 10, deck_color(di), Align::Left, 0, 1);
    }
    for (std::size_t c = 0; c < cols.size(); ++c) {
      const std::string& n = cols[c].name;
      const float cw = x1[c] - x0[c];
      if (n == "title") dl_.text(x0[c], ty, skin_->fit(t.title, 12, 0, cw - 10), 12, th.text, Align::Left, cw - 10);
      else if (n == "artist") dl_.text(x0[c], ty, skin_->fit(t.artist, 12, 0, cw - 10), 12, th.dim, Align::Left, cw - 10);
      else if (n == "bpm" && t.bpm > 0) dl_.text(x1[c] - 8, ty, fmt("%.1f", t.bpm), 12, th.text, Align::Right);
      else if (n == "key") dl_.text(x0[c] + 4, ty, t.key, 12, th.dim, Align::Left, cw - 6);
      else if (n == "time" && t.duration_ms > 0) dl_.text(x1[c] - 6, ty, clock_text(t.duration_ms / 1000.0), 12, th.dim, Align::Right);
    }
    if (in_.pressed && row.contains(in_.x, in_.y) && body.contains(in_.x, in_.y)) {
      sel_ = i;
      focus_tree_ = false;
      if (in_.clicks >= 2) load_deck = views_[0].playing ? 1 : 0;
    }
  }
  if (rows_.empty()) {
    const std::string msg = search_.empty() ? T("browser.empty") : T("browser.no_matches");
    dl_.text(body.x + body.w / 2, body.y + 40, skin_->fit(msg, 13, 0, body.w - 20), 13, th.dim, Align::Center);
  }
  dl_.pop_clip();
  if (content > body.h) {  // scroll bar
    const float bar_h = std::max(24.0f, body.h * body.h / content);
    const float bar_y = body.y + (body.h - bar_h) * scroll_ / (content - body.h);
    dl_.rect(body.x + body.w - 5, bar_y, 4, bar_h, th.line);
  }
  if (load_deck >= 0 && sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) {
    request_load(load_deck, rows_[static_cast<std::size_t>(sel_)].id);  // does not touch browse state
  }
}

}  // namespace deckhand
