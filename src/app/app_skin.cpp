// Skin-driven UI glue: skin discovery and switching, the SkinHost implementation (bindings -> engine,
// library and app actions) and the complex widgets (waveform, overview, jog, pads, playlist tree, track
// table, search field) that are drawn into the rectangle the skin gives them.
#include "app_impl.hpp"
#include "vocal_band.hpp"

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
    ++skin_gen_;
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
  if (t.name == "mic_level") {
    v = mixer_.mic_peak;
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
    } else if (n == "pad_bank") {
      out = pad_bank_[d] ? "9-16" : "1-8";
    } else if (n == "key_sync_status") {  // empty unless key sync is on or has just been used
      static const char* const ids[] = {"none", "ok", "too_far", "no_own_key", "no_ref_key", "leader"};
      out.clear();
      if (v.loaded && (v.key_sync_follow || v.key_sync_result != 0) && v.key_sync_result < 6 && v.key_sync_result != 0)
        out = T(std::string("deck.keysync_") + ids[v.key_sync_result]);
    } else if (n == "key_shift") {
      const long semis = std::lround(v.key_shift);
      out = semis == 0 ? "0" : fmt("%+ld", semis);
    } else if (n == "grid_bpm") {
      const GridSection* g = nullptr;
      for (const GridSection& sec : in.grid)
        if (sec.first_beat_frame <= v.position) g = &sec;
      if (!g && !in.grid.empty()) g = &in.grid.front();
      out = g ? fmt("%.2f", g->bpm) : "--.--";
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
  if (t.type == ui::Target::Type::Info && (n == "preview_title" || n == "preview_time")) {
    const PreviewView pv = engine_->preview();
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (n == "preview_title") {
      out = preview_.loading ? T("deck.loading") : (pv.loaded && preview_.id == pv.track_id ? preview_.title : T("preview.idle"));
    } else {
      out = pv.loaded && pv.sample_rate > 0 ? clock_text(pv.position / pv.sample_rate) + " / " + clock_text(pv.length / pv.sample_rate) : std::string();
    }
    return true;
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
    } else if (n == "controller_open") {
      const auto c = hub_->connected();
      out = c.empty() ? T("app.no_controller") : c.front();
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
    const bool on = read_control(*engine_, t.name, v) && v != 0;
    return t.invert ? !on : on;
  }
  const std::string& n = t.name;
  if (t.type == ui::Target::Type::Info) {
    if (n == "stems_available") return deck_ok && views_[d].stems_state == 2;
    if (n == "loaded") return deck_ok && views_[d].loaded;
    if (n == "has_import") return import_available();
    if (n == "multi_language") return i18n_.available().size() > 1;
    if (n == "multi_skin") return skins_.size() > 1;
    if (n == "has_four") return skin_ && skin_->has_window("four");
    if (n == "multi_window") return multi();
    if (n.compare(0, 7, "window:") == 0) return module_visible(n.substr(7));
    if (n == "any_grid_edit") {
      for (int i = 0; i < kMaxDecks; ++i)
        if (grid_edit_[i]) return true;
      return false;
    }
    if (n == "can_remove") {
      std::lock_guard<std::mutex> lock(browse_mutex_);
      return view_is_editable_playlist() && sel_ >= 0 && sel_ < static_cast<int>(rows_.size());
    }
    return false;
  }
  if (t.type == ui::Target::Type::Action) {
    if (n == "rec") return engine_->recorder_status().recording;
    if (n.compare(0, 7, "window.") == 0) {
      const std::size_t c = n.find(':');
      return c != std::string::npos && n.compare(0, 14, "window.toggle:") == 0 && module_visible(n.substr(c + 1));
    }
    if (n == "export") return panel_ == Panel::Export;
    if (n == "settings") return panel_ == Panel::Settings;
    if (n == "sampler") return panel_ == Panel::Sampler;
    if (n == "learn") return panel_ == Panel::Learn;
    if (n == "layout") return variant_ == "four";
    if (n == "columns") return panel_ == Panel::Columns;
    if (n == "trash") return panel_ == Panel::Trash;
    if (n == "chain") return panel_ == Panel::Chain;
    if (n == "layouts") return panel_ == Panel::Layouts;
    if (n == "locate") return panel_ == Panel::Locate;
    if (n == "capture") return panel_ == Panel::Capture;
    if (n == "split") return split_ != SplitMode::Off;
    if (n == "compatible") return compat_on_;
    if (n == "preview_play") return engine_->preview().playing != 0;
    if (n == "grid_edit") return deck_ok && grid_edit_[d];
    if (n == "grid_lock") return deck_ok && info_[d].grid_locked;
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
  if (t.name.size() > 10 && t.name.compare(t.name.size() - 10, 10, ".metronome") == 0 && engine_->output_channels() < 4)
    status_note_ = T("grid.no_cue_bus");  // the click goes to the headphone pair only; the switch still works
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
  } else if (n.compare(0, 7, "window.") == 0) {
    do_window_action(n);
  } else if (n == "sampler" && module_known("sampler")) {  // the skin has its own sampler window: that is the sampler
    set_module_visible("sampler", 2);
  } else if (n == "controller_open") {
    if (panel_ == Panel::Controller) close_panel();
    else open_panel(Panel::Controller);
  } else if (n == "settings" || n == "sampler" || n == "learn") {
    const Panel p = n == "settings" ? Panel::Settings : n == "sampler" ? Panel::Sampler : Panel::Learn;
    if (panel_ == p) close_panel();
    else open_panel(p);
  } else if (n == "load") {
    std::int64_t id = 0;
    {
      std::lock_guard<std::mutex> lock(browse_mutex_);
      if (sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) id = rows_.id(static_cast<std::size_t>(sel_));
    }
    if (id) request_load(t.deck, id);
  } else if (n == "add_to" || n == "edit_track") {
    bool have;
    {
      std::lock_guard<std::mutex> lock(browse_mutex_);
      have = sel_ >= 0 && sel_ < static_cast<int>(rows_.size());
      if (have) edit_track_ = rows_.id(static_cast<std::size_t>(sel_));
      add_ids_ = selected_ids();
    }
    if (have) open_panel(n == "add_to" ? Panel::AddTo : Panel::Tags);
  } else if (n == "columns") {
    if (panel_ == Panel::Columns) close_panel();
    else open_panel(Panel::Columns);
  } else if (n == "trash" || n == "layouts" || n == "locate") {
    const Panel p = n == "trash" ? Panel::Trash : n == "layouts" ? Panel::Layouts : Panel::Locate;
    if (panel_ == p) {
      close_panel();
    } else {
      open_panel(p);
      if (p == Panel::Locate) {
        loc_siblings_ = false;
        loc_scanned_ = false;
        loc_report_ = {};
        loc_note_.clear();
      }
      trash_dirty_ = true;
    }
  } else if (n == "capture") {
    if (panel_ == Panel::Capture) {
      close_panel();
    } else {
      open_panel(Panel::Capture);
      cap_ui_.devices = engine_->input_devices();
      cap_ui_.device = lib_->setting("capture.device");
      cap_ui_.channel = std::clamp(std::atoi(lib_->setting("capture.channel", "0").c_str()), 0, 7);
      cap_ui_.stereo = lib_->setting("capture.stereo", "1") != "0";
      cap_ui_.again = false;
      cap_ui_.note.clear();
    }
  } else if (n == "chain" || n.compare(0, 6, "chain.") == 0) {
    int target = t.deck >= 0 ? t.deck : kFxMaster;
    if (n.size() > 6) {
      int parsed = 0;
      if (parse_fx_target(n.substr(6), parsed)) target = parsed;
    } else if (panel_ == Panel::Chain && t.deck < 0) {
      target = chain_target_;
    }
    if (panel_ == Panel::Chain && chain_target_ == target) close_panel();
    else open_chain(target);
  } else if (n == "split") {
    toggle_split();
  } else if (n == "focus_pane") {
    focus_other_pane();
  } else if (n == "layout_next" || n == "layout_prev") {
    step_layout(n == "layout_next" ? 1 : -1);
  } else if (n == "compatible") {
    compat_on_ = !compat_on_;
    rows_dirty_ = true;
  } else if (n == "preview_play") {
    preview_toggle();
  } else if (n == "preview_stop") {
    Command c;
    c.op = Op::PreviewStop;
    engine_->send(c);
  } else if (n == "remove" || n.compare(0, 9, "playlist_") == 0) {
    playlist_action(n);
  } else if (n == "cue_memory_prev" || n == "cue_memory_next") {
    std::string error;
    apply_control(*engine_, "deck" + std::to_string(t.deck + 1) + ".cue_call", true, n == "cue_memory_prev" ? -1 : 1, error);
  } else if (n.compare(0, 5, "grid_") == 0 || n.compare(0, 4, "cue_") == 0) {
    static const struct { const char* id; const char* action; bool value; } kMap[] = {
        {"grid_edit", "grid.edit", false},           {"grid_shift", "grid.shift", true},       {"grid_first_beat", "grid.set_first_beat", false},
        {"grid_downbeat", "grid.downbeat", false},   {"grid_rotate", "grid.rotate", true},     {"grid_bpm", "grid.bpm_delta", true},
        {"grid_scale", "grid.scale", true},          {"grid_tap", "grid.tap", false},          {"grid_reanalyse", "grid.reanalyse", false},
        {"grid_lock", "grid.lock", false},           {"grid_undo", "grid.undo", false},        {"grid_add_section", "grid.add_section", false},
        {"grid_remove_section", "grid.remove_section", false}, {"cue_memory_set", "cue.memory_set", false},
        {"cue_memory_delete", "cue.memory_delete", false}, {"cue_loop_save", "cue.loop_save", false}, {"cue_loop_recall", "cue.loop_recall", false},
        {"cue_suggest", "cue.suggest", false}};
    for (const auto& m : kMap) {
      if (n != m.id) continue;
      PrepArgs a;
      if (m.value) {
        a.has_value = true;
        a.value = t.step;
      }
      std::string error;
      if (!prep(t.deck, m.action, a, error) && !error.empty()) status_note_ = T("status.error", {{"message", error}});
      break;
    }
  } else if (n == "pad_bank") {
    if (t.deck >= 0 && t.deck < kMaxDecks) pad_bank_[t.deck] ^= 1;
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
  } else if (which == "playlist_add_smart") {
    open_smart_editor(0);
  } else if (which == "playlist_rename" && cur && cur->kind == TreeItem::Smart) {
    open_smart_editor(cur->id);
  } else if (which == "playlist_delete" && cur && cur->kind == TreeItem::Smart) {
    open_smart_editor(cur->id);  // deleting a smart playlist asks inside its editor
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
  ui::SkinView* view = cur_win_ && cur_win_->view ? cur_win_->view.get() : skin_.get();
  const std::string win = cur_win_ && cur_win_->view ? cur_win_->name : (skin_->has_window(variant_) ? variant_ : "main");
  view->layout(win, w, h, inset_[0], inset_[1], inset_[2], inset_[3]);
  // Release bookkeeping for held controls started outside the skin widgets.
  view->frame(*this, in_, now_);
  if (in_.released && drag_deck_ >= 0 && !any_down_ && !in_.down) drag_deck_ = -1;
  if (jog_deck_ >= 0 && !in_.down && !any_down_) {  // the platter vanished mid-touch (hidden, skin switched, overlay opened): let go
    if (jog_top_) {
      Command c;
      c.op = Op::JogTouch;
      c.deck = static_cast<std::uint8_t>(jog_deck_);
      c.i = 0;
      engine_->send(c);
    }
    jog_deck_ = -1;
  }
}

void App::draw_custom(const skin::ResolvedWidget& w, const ui::Target& t, const skin::Style& st, const Rect& r) {
  const int d = t.deck;
  const bool deck_ok = d >= 0 && d < kMaxDecks;
  const Color base = st.fg == 0xE6E6EAFF ? deck_color(std::max(d, 0)) : ui::to_gfx(st.fg, st.opacity);
  switch (w.kind) {
    case skin::Kind::Waveform: {
      if (t.name == "preview_wave") {
        draw_preview_strip(r, base);
        return;
      }
      if (!deck_ok) return;
      const auto mode = w.props.find("mode");
      const auto ph = w.props.find("phrases");  // skin switch: phrases="off" keeps the bands off this widget
      wave_phrases_ = ph == w.props.end() || (ph->second != "off" && ph->second != "0" && ph->second != "false");
      if (mode != w.props.end() && mode->second == "overview") draw_overview(d, r, views_[d], info_[d], base, theme_);
      else draw_wave(d, r, views_[d], info_[d], base, theme_, true);
      wave_phrases_ = true;
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
      draw_browser_panes(r, theme_, cols != w.props.end() ? cols->second : "title,artist,bpm,key,time",
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

namespace {
Color rgb_of(int c) { return rgba((c >> 16) & 255, (c >> 8) & 255, c & 255); }
}  // namespace

namespace {
Color phrase_color(const std::string& label) {
  if (label == "intro" || label == "outro") return rgba(110, 140, 190);
  if (label == "build") return rgba(240, 180, 60);
  if (label == "drop" || label == "chorus") return rgba(235, 70, 110);
  if (label == "breakdown") return rgba(70, 200, 220);
  if (label == "verse") return rgba(120, 200, 120);
  if (label == "bridge") return rgba(180, 120, 230);
  return rgba(150, 150, 160);
}
}  // namespace

void App::draw_phrase_bands(const DeckInfo& info, float x0, float y, float w, float h, double ms0, double ms1, bool labels,
                            const ui::Theme& th) {
  if (!show_phrases_ || !wave_phrases_ || info.phrases.sections.empty() || ms1 <= ms0) return;
  const float per_ms = w / static_cast<float>(ms1 - ms0);
  for (const StoredPhraseSection& s : info.phrases.sections) {
    if (s.end_ms < ms0 || s.start_ms > ms1) continue;
    const float xa = x0 + static_cast<float>(s.start_ms - ms0) * per_ms, xb = x0 + static_cast<float>(s.end_ms - ms0) * per_ms;
    const float width = xb - xa;
    if (width < 1.0f) continue;
    const bool sure = s.label_confidence >= 0.5;  // a doubtful label is drawn lighter
    const Color c = phrase_color(s.label);
    const float bh = std::max(2.0f, h * (0.35f + 0.065f * static_cast<float>(std::clamp(s.energy, 1, 10))));
    dl_.rect(xa, y + h - bh, std::max(1.0f, width - 1.0f), bh, with_alpha(c, sure ? 200 : 80));
    // Boundary tick: stronger changes are taller.
    dl_.rect(xa, y, 1.0f, h * static_cast<float>(0.5 + 0.5 * std::clamp(s.boundary_confidence, 0.0, 1.0)), with_alpha(c, sure ? 230 : 110));
    if (labels && width > 46) {
      const std::string text = skin_->fit(T(("phrase.label." + s.label).c_str()) + " " + std::to_string(s.energy), 10, 1, width - 8);
      dl_.text(xa + 4, y + h + 1, text, 10, with_alpha(sure ? th.text : th.dim, sure ? 235 : 170), Align::Left, 0, 1);
    }
  }
}

void App::draw_wave(int di, const Rect& r, const DeckView& v, const DeckInfo& info, Color base, const ui::Theme& th, bool) {
  dl_.rect(r.x, r.y, 3, r.h, base);
  if (!info.audio || !v.loaded) {
    const std::string msg = info.loading ? T("deck.loading") : (!info.error.empty() ? info.error : T("deck.empty", {{"deck", deck_letter(di)}}));
    dl_.text(r.x + r.w / 2, r.y + r.h / 2 - 8, skin_ ? skin_->fit(msg, 13, 0, r.w - 40) : msg, 13, info.error.empty() ? th.dim : th.warn, Align::Center, r.w - 40);
    return;
  }
  const TrackAudio& a = *info.audio;
  const double sr = a.sample_rate, window = wave_zoom_[di];
  const bool edit = grid_edit_[di];
  const double pos = v.position / sr, pt_per_sec = r.w / window;
  const float cx = r.x + r.w / 2, mid = r.y + r.h / 2, half = r.h / 2 - 5;
  const auto x_of = [&](double seconds) { return cx + static_cast<float>((seconds - pos) * pt_per_sec); };
  const bool inside = r.contains(in_.x, in_.y);

  // Zoom: the wheel or a pinch over the waveform, and the two small buttons in its corner (remembered per deck).
  const Rect zout{r.x + r.w - 42, r.y + 2, 18, 16}, zin{r.x + r.w - 22, r.y + 2, 18, 16};
  bool on_button = false;
  if (inside && in_.scroll != 0) {
    set_wave_zoom(di, window * (in_.scroll > 0 ? 1.0 / 1.25 : 1.25));
    in_.scroll = 0;
  }
  if (inside && in_.magnify != 0) {
    set_wave_zoom(di, window / std::clamp(1.0 + static_cast<double>(in_.magnify), 0.5, 2.0));
    in_.magnify = 0;
  }
  if (clicked(zin)) { set_wave_zoom(di, window / 1.5); on_button = true; }
  if (clicked(zout)) { set_wave_zoom(di, window * 1.5); on_button = true; }

  dl_.push_clip(r.x + 3, r.y, r.w - 3, r.h);

  // Loop region behind the waveform.
  if (v.loop_in >= 0 && v.loop_out > v.loop_in) {
    const float x0 = x_of(v.loop_in / sr), x1 = x_of(v.loop_out / sr);
    dl_.rect(x0, r.y, x1 - x0, r.h, with_alpha(th.accent, v.loop_active ? 70 : 30));
  }
  // Saved loops and hot-cue loops, fainter.
  for (const MemCue& m : info.mem) {
    if (m.end <= m.frame) continue;
    const float x0 = x_of(m.frame / sr), x1 = x_of(m.end / sr);
    if (x1 < r.x || x0 > r.x + r.w) continue;
    const Color c = m.color >= 0 ? rgb_of(m.color) : th.warn;
    dl_.rect(x0, r.y + r.h - 6, x1 - x0, 6, with_alpha(c, 110));
  }
  const double t0 = pos - window / 2, t1 = pos + window / 2;
  // Waveform: columns of at least 1.5 points wide (several analysis points are merged when zoomed out), three bands.
  const double per_point = 1.0 / a.wave_rate;
  const float px_per_point = static_cast<float>(per_point * pt_per_sec);
  const long step = std::max(1L, static_cast<long>(std::floor(1.5f / std::max(px_per_point, 1e-4f))));
  const float col = std::max(px_per_point * static_cast<float>(step), 1.0f) + 0.5f;
  long i0 = std::max(0L, static_cast<long>(t0 * a.wave_rate)), i1 = std::min(static_cast<long>(a.wave.size()), static_cast<long>(t1 * a.wave_rate) + 2);
  i0 -= i0 % step;  // columns stay put while the track scrolls
  const Color c_low = mix(base, th.bg, 0.55f), c_high = mix(base, rgba(255, 255, 255), 0.7f);
  for (long i = i0; i < i1; i += step) {
    WavePoint p = a.wave[static_cast<std::size_t>(i)];
    for (long k = 1; k < step && i + k < static_cast<long>(a.wave.size()); ++k) {
      const WavePoint& q = a.wave[static_cast<std::size_t>(i + k)];
      p.low = std::max(p.low, q.low);
      p.mid = std::max(p.mid, q.mid);
      p.high = std::max(p.high, q.high);
    }
    const float x = x_of(static_cast<double>(i) * per_point);
    const float hl = half * p.low / 255.0f, hm = half * 0.8f * p.mid / 255.0f, hh = half * 0.55f * p.high / 255.0f;
    const float tall = std::max(hl, std::max(hm, hh));
    dl_.rect(x, mid - tall, col, tall * 2, c_low);
    if (hm > 0.5f) dl_.rect(x, mid - hm, col, hm * 2, base);
    if (hh > 0.5f) dl_.rect(x, mid - hh, col, hh * 2, c_high);
  }
  // Beat grid, over the waveform. In edit mode it is the point of the view: heavier lines, bar numbers, downbeats in colour.
  {
    std::vector<gridedit::BeatMark> marks;
    gridedit::beats_in(info.grid, t0 * sr, t1 * sr, static_cast<double>(a.frames), sr, marks);
    const float beat_px = info.grid.empty() ? 0.0f : static_cast<float>(60.0 / std::max(30.0, info.grid.front().bpm) * pt_per_sec);
    const std::size_t stride = beat_px < 3.0f ? 4 : 1;  // zoomed far out: bars only
    std::size_t n = 0;
    for (const gridedit::BeatMark& m : marks) {
      const bool bar = m.bar_pos == 1;
      if (stride > 1 && !bar) continue;
      ++n;
      const float x = x_of(m.frame / sr);
      if (edit) {
        dl_.rect(x - (bar ? 1.25f : 0.75f), r.y, bar ? 2.5f : 1.5f, r.h, bar ? with_alpha(th.warn, 230) : with_alpha(th.text, 150));
        dl_.triangle(x - 4, r.y, x + 4, r.y, x, r.y + 6, bar ? th.warn : th.text);
        if (bar && beat_px * 4 > 28) dl_.text(x + 4, r.y + r.h - 14, "1", 10, th.warn, Align::Left, 0, 1);
      } else {
        dl_.rect(x, r.y, 1, r.h, with_alpha(th.text, bar ? 120 : 45));
      }
    }
    (void)n;
  }
  if (r.h > 36) draw_phrase_bands(info, r.x, r.y + std::min(13.0f, r.h * 0.5f) + 2, r.w, 5, t0 * 1000.0, t1 * 1000.0, r.h >= 70, th);
  // Cue point, hot cues (colour and name), memory cues.
  const auto marker_frame = [&](int kind, int index, double frame) {
    return cue_drag_.deck == di && cue_drag_.kind == kind && cue_drag_.index == index ? cue_drag_.frame : frame;
  };
  const float xc = x_of(marker_frame(3, 0, v.cue) / sr);
  dl_.triangle(xc - 5, r.y, xc + 5, r.y, xc, r.y + 8, th.text);
  const float tag_h = std::min(13.0f, r.h * 0.5f);
  for (int h = 0; h < kHotCues; ++h) {
    if (v.hot_cue[h] < 0) continue;
    const Color hc = info.hot_color[h] >= 0 ? rgb_of(info.hot_color[h]) : kHot[h];
    const double frame = marker_frame(1, h, v.hot_cue[h]);
    const float x = x_of(frame / sr);
    if (v.hot_cue_end[h] > v.hot_cue[h]) {
      const float xe = x_of((frame + (v.hot_cue_end[h] - v.hot_cue[h])) / sr);
      dl_.rect(x, r.y, xe - x, r.h, with_alpha(hc, 38));
      dl_.rect(xe, r.y, 1.5f, r.h, with_alpha(hc, 190));
    }
    dl_.rect(x, r.y, 1.5f, r.h, hc);
    float tw = 14;
    dl_.rect(x, r.y, tw, tag_h, hc);
    if (tag_h > 9) dl_.text(x + 7, r.y, std::string(1, static_cast<char>('1' + h)), 10, th.bg, Align::Center, 0, 1);
    if (!info.hot_label[h].empty() && tag_h > 9) {
      const std::string lab = skin_->fit(info.hot_label[h], 10, 1, 120);
      const float lw = dl_.text_width(lab, 10, 1) + 8;
      dl_.rect(x + tw, r.y, lw, tag_h, with_alpha(hc, 215));
      dl_.text(x + tw + 4, r.y, lab, 10, th.bg, Align::Left, 0, 1);
    }
  }
  for (std::size_t m = 0; m < info.mem.size(); ++m) {
    const MemCue& c = info.mem[m];
    const double frame = marker_frame(2, static_cast<int>(m), c.frame);
    const Color mc = c.color >= 0 ? rgb_of(c.color) : th.warn;
    const float x = x_of(frame / sr);
    if (x < r.x - 100 || x > r.x + r.w + 4) continue;
    dl_.rect(x, r.y, 1, r.h, with_alpha(mc, c.end > 0 ? 90 : 110));
    dl_.triangle(x - 5, r.y + r.h, x + 5, r.y + r.h, x, r.y + r.h - 9, mc);
    if (!c.label.empty() && r.h > 30) dl_.text(x + 6, r.y + r.h - 24, skin_->fit(c.label, 10, 1, 110), 10, mc, Align::Left, 0, 1);
  }
  draw_link_marks(info, r, sr, x_of, th);  // linked mix points and the vocal band
  dl_.pop_clip();
  dl_.rect(cx - 1, r.y, 2, r.h, rgba(255, 255, 255));  // playhead

  // Zoom buttons and the visible span.
  const auto small_button = [&](const Rect& b, const char* label) {
    dl_.rrect(b.x, b.y, b.w, b.h, 3, with_alpha(th.panel2, b.contains(in_.x, in_.y) ? 255 : 200));
    dl_.text(b.x + b.w / 2, b.y + 1, label, 12, th.text, Align::Center, 0, 1);
  };
  small_button(zout, "-");
  small_button(zin, "+");
  dl_.text(zout.x - 6, r.y + 3, fmt("%.1f s", window), 10, th.dim, Align::Right);
  if (edit) dl_.text(r.x + 10, r.y + 3, T("grid.editing", {{"bpm", v.bpm}}), 10, th.warn, Align::Left, 0, 1);

  // Pointer: drag a cue marker (double-click names it); a paused deck is scrubbed by dragging the waveform.
  const auto pos_frame_at = [&](float x) {
    double f = (pos + static_cast<double>(x - cx) / pt_per_sec) * sr;
    f = std::clamp(f, 0.0, v.length);
    return v.quantize && !info.grid.empty() ? gridedit::nearest_beat(info.grid, f, sr) : f;
  };
  if (in_.pressed && inside && !on_button) {
    const CueHit hit = cue_hit_test(di, r, v, info, window, in_.x, in_.y);
    if (hit.kind != 0) {
      if (in_.clicks >= 2 && hit.kind == 1) open_cue_editor(di, hit.index, -1);
      else if (in_.clicks >= 2 && hit.kind == 2) open_cue_editor(di, -1, hit.index);
      else {
        cue_drag_ = {di, hit.kind, hit.index, hit.kind == 1 ? v.hot_cue[hit.index] : hit.kind == 2 ? info.mem[static_cast<std::size_t>(hit.index)].frame : v.cue, false};
        scrub_x0_ = in_.x;
      }
    } else if (!v.playing && !v.touching) {
      scrub_deck_ = di;
      scrub_x0_ = in_.x;
      scrub_pos0_ = v.position;
    }
  }
  if (cue_drag_.deck == di) {
    if (in_.down) {
      if (std::fabs(in_.x - scrub_x0_) > 3) cue_drag_.moved = true;
      if (cue_drag_.moved) cue_drag_.frame = pos_frame_at(in_.x);
    } else {
      if (cue_drag_.moved) {
        Command c;
        c.deck = static_cast<std::uint8_t>(di);
        if (cue_drag_.kind == 1) {
          c.op = Op::HotCueSetAt;
          c.i = cue_drag_.index;
          c.d = cue_drag_.frame;
          engine_->send(c);
        } else if (cue_drag_.kind == 3) {
          c.op = Op::CueSetAt;
          c.d = cue_drag_.frame;
          engine_->send(c);
        } else {
          int number = 0;  // the action counts memory cues, not loops
          for (int i = 0; i <= cue_drag_.index && i < static_cast<int>(info.mem.size()); ++i)
            if (info.mem[static_cast<std::size_t>(i)].end < 0) ++number;
          PrepArgs pa;
          pa.has_value = true;
          pa.value = cue_drag_.frame / sr;
          pa.slot = number;
          std::string error;
          prep(di, "cue.memory_move", pa, error);
        }
      }
      cue_drag_ = {};
    }
  }
  if (scrub_deck_ == di) {
    if (in_.down && !v.playing && cue_drag_.deck != di) {
      Command c;
      c.op = Op::Seek;
      c.deck = static_cast<std::uint8_t>(di);
      c.d = std::clamp(scrub_pos0_ - static_cast<double>(in_.x - scrub_x0_) / pt_per_sec * sr, 0.0, v.length);
      engine_->send(c);
    } else {
      scrub_deck_ = -1;
    }
  }
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
  const auto x_at = [&](double frame) { return ov.x + static_cast<float>(frame / v.length) * ov.w; };
  draw_phrase_bands(info, ov.x, ov.y, ov.w, std::min(4.0f, ov.h * 0.3f), 0, v.length / std::max(1u, info.audio ? info.audio->sample_rate : 1u) * 1000.0, false, th);
  if (info.has_vocals && v.sample_rate > 0) {  // the vocal band of the main waveform, along the top edge of the whole track
    const float bh = std::min(3.0f, ov.h * 0.25f);
    for (const VocalSpan& g : vocal_spans(info.vocals.regions, 0, v.length / v.sample_rate * 1000.0, ov.x, ov.w))
      dl_.rect(g.x0, ov.y, std::max(1.0f, g.x1 - g.x0), bh, with_alpha(rgba(120, 210, 255), 200));
  }
  if (v.loop_in >= 0 && v.loop_out > v.loop_in)
    dl_.rect(x_at(v.loop_in), ov.y, std::max(1.0f, x_at(v.loop_out) - x_at(v.loop_in)), ov.h, with_alpha(th.accent, v.loop_active ? 80 : 40));
  for (const MemCue& m : info.mem) {  // memory cues: a tick at the foot; saved loops: a band
    const Color mc = m.color >= 0 ? rgb_of(m.color) : th.warn;
    if (m.end > m.frame) dl_.rect(x_at(m.frame), ov.y + ov.h - 3, std::max(1.0f, x_at(m.end) - x_at(m.frame)), 3, with_alpha(mc, 170));
    else dl_.rect(x_at(m.frame) - 0.5f, ov.y + ov.h * 0.55f, 1.5f, ov.h * 0.45f, mc);
  }
  for (int h = 0; h < kHotCues; ++h) {
    if (v.hot_cue[h] < 0) continue;
    const Color hc = info.hot_color[h] >= 0 ? rgb_of(info.hot_color[h]) : kHot[h];
    if (v.hot_cue_end[h] > v.hot_cue[h]) dl_.rect(x_at(v.hot_cue[h]), ov.y, std::max(1.0f, x_at(v.hot_cue_end[h]) - x_at(v.hot_cue[h])), ov.h, with_alpha(hc, 60));
    dl_.rect(x_at(v.hot_cue[h]), ov.y, 2, ov.h, hc);
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
  const DeckInfo& pinfo = info_[di];
  const float gap = 3, cw = (r.w - gap * static_cast<float>(cols + 1)) / static_cast<float>(cols),
              ch = (r.h - gap * static_cast<float>(rows + 1)) / static_cast<float>(rows);
  const int id_base = (di + 1) * 100;
  const int cap = cols * rows;  // a grid smaller than the 16 slots shows one bank of them; the skin's "pad_bank" button switches
  const int base = cap < kHotCues ? std::clamp(pad_bank_[di], 0, (kHotCues - 1) / cap) * cap : 0;
  for (int pos = 0; pos < cap && base + pos < kHotCues; ++pos) {
    const int k = base + pos;
    const Rect pr{r.x + gap + static_cast<float>(pos % cols) * (cw + gap), r.y + gap + static_cast<float>(pos / cols) * (ch + gap), cw, ch};
    const bool set = v.hot_cue[k] >= 0;
    const bool hover = pr.contains(in_.x, in_.y);
    const Color slot_color = pinfo.hot_color[k] >= 0 ? rgb_of(pinfo.hot_color[k]) : kHot[k];
    Color fill = set ? slot_color : mix(theme_.panel2, rgba(255, 255, 255), 0.07f);
    if (hover) fill = mix(fill, rgba(255, 255, 255), 0.18f);
    dl_.rrect(pr.x, pr.y, pr.w, pr.h, std::min(st.radius, 5.0f), fill);
    const float fs = std::min(12.0f, std::max(8.0f, pr.h * 0.5f));
    const std::string pad_text = set && !pinfo.hot_label[k].empty() ? std::to_string(k + 1) + " " + pinfo.hot_label[k] : std::to_string(k + 1);
    dl_.text(pr.x + pr.w / 2, pr.y + (pr.h - fs * 1.25f) / 2, skin_->fit(pad_text, fs, 1, pr.w - 4), fs, set ? theme_.bg : theme_.dim, Align::Center, pr.w - 4, 1);
    if (clicked(pr) && set && raw_.shift) {  // shift-click on a set pad: name, colour, loop
      open_cue_editor(di, k, -1);
    } else if (clicked(pr)) {
      std::string error;
      apply_control(*engine_, "deck" + std::to_string(di + 1) + ".hotcue." + std::to_string(k + 1), true, 1, error);
      held_ = id_base + 10 + k;
    }
  }
  if (in_.released && held_ / 100 == di + 1 && held_ % 100 >= 10 && held_ % 100 < 30) {
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
    const bool drop_here = rdrag_.active && item.kind == TreeItem::Playlist && row.contains(in_.x, in_.y) && tree.contains(in_.x, in_.y);
    if (drop_here) {  // tracks dragged from the browser: the playlist under the pointer takes them on release
      dl_.rect(row.x, row.y, row.w, row.h, with_alpha(th.good, 80));
      dl_.rrect_outline(row.x + 1, row.y + 1, row.w - 2, row.h - 2, 3, th.good, 1.5f);
      if (!in_.down) {
        drop_rows_on_playlist(item.id);
        rdrag_ = {};
      }
    }
    const bool container = item.kind == TreeItem::Folder || item.kind == TreeItem::HistoryHeader || item.kind == TreeItem::DiskHeader;
    const float indent = 10 + 14.0f * static_cast<float>(item.depth) + (item.kind == TreeItem::Collection ? 0 : 6);
    const float ty = row.y + (row_h - 15) / 2;
    if (rename_id_ != 0 && item.id == rename_id_ && (item.kind == TreeItem::Playlist || item.kind == TreeItem::Folder)) {
      dl_.rect(row.x + indent - 4, row.y + 2, tree.w - indent - 4, row.h - 4, rgba(10, 11, 14));
      dl_.outline(row.x + indent - 4, row.y + 2, tree.w - indent - 4, row.h - 4, th.accent);
      dl_.text(row.x + indent, ty, rename_buf_ + "|", 12, th.text, Align::Left, tree.w - indent - 12);
    } else {
      if (item.kind == TreeItem::Smart) {  // a funnel: this list is a saved filter
        dl_.triangle(row.x + 6, ty + 3, row.x + 15, ty + 3, row.x + 10.5f, ty + 9, th.accent);
        dl_.rect(row.x + 9.5f, ty + 9, 2, 4, th.accent);
      }
      dl_.text(row.x + indent, ty, skin_->fit(item.name, 12, item.kind == TreeItem::Collection || container ? 1 : 0, tree.w - indent - 44), 12,
               container ? th.dim : th.text, Align::Left, tree.w - indent - 44, item.kind == TreeItem::Collection || container ? 1 : 0);
      if (!container) dl_.text(row.x + tree.w - 10, ty + 1, std::to_string(item.count), 10, th.dim, Align::Right);
    }
    if (clicked(row) && tree.contains(in_.x, in_.y)) {
      if (rename_id_ != 0 && item.id != rename_id_) commit_rename();
      tree_sel_ = i;
      focus_tree_ = false;
      apply_tree_selection();
      if (in_.clicks >= 2 && item.kind == TreeItem::Smart) {
        open_smart_editor(item.id);
      } else if (in_.clicks >= 2 && (item.kind == TreeItem::Playlist || item.kind == TreeItem::Folder)) {
        rename_id_ = item.id;
        rename_buf_ = item.name;
        rename_fresh_ = true;
      }
    }
  }
  dl_.pop_clip();
}

}  // namespace deckhand
