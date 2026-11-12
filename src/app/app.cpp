// DeckHand application core: owns the library, engine and controllers, and runs the UI loop.
// The UI itself is drawn from a skin (app_skin.cpp, skin_view.cpp); overlays live in app_panels.cpp and the
// code-drawn fallback in app_legacy.cpp.
#include "deckhand/sync/sync.hpp"
#include "app_impl.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#define DH_HAVE_ENGINE_PROCESS 1
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace deckhand {

// =================================================================================
App::App(const dh_platform& platform, const dh_config& config)
    : data_dir_(config.data_dir ? config.data_dir : "."),
      resource_dir_(config.resource_dir ? config.resource_dir : ""),
      dl_([platform](const std::string& s, float px, int weight, gfx::Bitmap& out) {
        if (!platform.rasterise_text) return false;
        dh_bitmap b{};
        if (!platform.rasterise_text(platform.user, s.c_str(), px, weight, &b) || !b.alpha) return false;
        out.width = static_cast<int>(b.w);
        out.height = static_cast<int>(b.h);
        out.ascent = b.ascent;
        out.alpha.assign(b.alpha, b.alpha + static_cast<std::size_t>(b.w) * b.h);
        std::free(b.alpha);
        return true;
      }) {
  std::filesystem::create_directories(data_dir_);
  log::set_level(static_cast<log::Level>(std::clamp(config.log_level, 0, 4)));
  log::set_file(data_dir_ + "/deckhand.log");
  DH_LOG(Info, "DeckHand starting; data in %s", data_dir_.c_str());

  lib_ = std::make_unique<Library>(data_dir_ + "/library.db");

  if (!resource_dir_.empty()) {
    i18n_.load_builtin(resource_dir_ + "/lang/en.dhlang");
    i18n_.load_directory(resource_dir_ + "/lang");
  }
  i18n_.load_directory(data_dir_ + "/lang");  // packs the user installed
  i18n_.set_language(lib_->setting("ui.language", "en"));
  touch_ = config.touch;
  audio_pref_ = lib_->setting("audio.device", "");
  audio_period_ = static_cast<std::uint32_t>(std::clamp(std::atoi(lib_->setting("audio.period", "256").c_str()), 64, 2048));
  variant_ = lib_->setting("ui.layout", "main") == "four" ? "four" : "main";

  std::vector<std::string> folders;
  if (!resource_dir_.empty()) folders.push_back(resource_dir_ + "/controllers");
  folders.push_back(data_dir_ + "/controllers");
  setup_engine(folders);

  scan_skins();
  skin_ = std::make_unique<ui::SkinView>(dl_, i18n_);
  skin_->set_touch(touch_);
  if (!use_skin(lib_->setting("ui.skin", "default"), false)) use_skin("default", false);

  if (remote_ && remote_->attached_existing() && engine_->running()) {
    // Re-attached to an engine that kept playing: leave the output alone and adopt its state.
    std::lock_guard<std::mutex> lock(state_mutex_);
    audio_label_ = fmt("%s  %d ch  %.1f kHz", engine_->device_name().c_str(), engine_->output_channels(),
                       engine_->sample_rate() / 1000.0);
    audio_choice_ = audio_pref_.empty() ? std::string() : "pref:" + audio_pref_;
  } else {
    start_audio();
  }
  loader_ = std::thread([this] { loader_main(); });
  control_ = std::thread([this] { control_main(); });
  if (remote_ && remote_->attached_existing()) {
    for (int i = 0; i < kMaxDecks; ++i) {  // rebuild the deck displays (waveforms) from what the engine holds
      std::int64_t id = 0;
      std::string path;
      if (!remote_->deck_info(i, id, path) || id == 0) continue;
      Job j;
      j.kind = Job::Reattach;
      j.deck = i;
      j.track_id = id;
      post(j);
    }
    set_status("Re-attached to the running audio engine");
  }

  api_ = std::make_unique<ApiBridge>(*this, data_dir_);
  stream_ = std::make_unique<StreamBridge>(*engine_, data_dir_, [this](int deck, TrackRow& row) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!decks_[deck].audio) return false;
    row = decks_[deck].row;
    return true;
  });

  if (!lib_->setting("sync.folder").empty()) {  // bring in other devices' changes first
    Job j;
    j.kind = Job::Sync;
    post(j);
  }
  if (lib_->track_count() == 0 && import_v6_available() && std::filesystem::exists(source_db())) {
    Job j;
    j.kind = Job::Import;
    post(j);
  }
}

App::~App() {
  api_.reset();  // stop outside callers first
  stream_.reset();
  quit_ = true;
  job_cv_.notify_all();
  if (loader_.joinable()) loader_.join();
  if (control_.joinable()) control_.join();
  stop_signal_watch();
  if (local_hub_) local_hub_->set_io(nullptr);
  midi_.reset();
  if (remote_) {
    // Normal quit stops the engine process too. With engine.keep_alive_on_quit it stays, but only while a deck is
    // playing; a crash or kill always leaves it running.
    bool playing = false;
    for (int i = 0; i < kMaxDecks; ++i) playing = playing || engine_->deck(i).playing;
    if (lib_->setting("engine.keep_alive_on_quit", "0") == "1" && playing) remote_->detach();
    else remote_->shutdown_remote(120);
  } else {
    engine_->stop();
  }
}

// Chooses where the engine runs: its own process (engine.process = separate, the default on desktop macOS) or
// inside this one. If the helper cannot be started the app falls back to in-process audio rather than none.
void App::setup_engine(const std::vector<std::string>& mapping_folders) {
#if defined(DH_HAVE_ENGINE_PROCESS) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#if defined(__APPLE__)
  const char* default_mode = "separate";
#else
  const char* default_mode = "inprocess";  // the Linux packaging does not install the helper yet
#endif
  const char* forced = std::getenv("DECKHAND_ENGINE_PROCESS");
  const std::string mode = forced && *forced ? forced : lib_->setting("engine.process", default_mode);
  if (mode == "separate") {
    RemoteEngine::Options o;
    o.data_dir = data_dir_;
    o.resource_dir = resource_dir_;
    o.log_level = static_cast<int>(log::level());
    auto remote = std::make_unique<RemoteEngine>(o);
    std::string error;
    if (remote->connect(error)) {
      remote_ = remote.get();
      remote->set_status_callback([this](RemoteEngine::Status s, const std::string& message) {
        switch (s) {
          case RemoteEngine::Status::Died:
            set_status("The audio engine stopped; restarting it...");
            break;
          case RemoteEngine::Status::Restarted: {
            {
              std::lock_guard<std::mutex> lock(state_mutex_);
              for (DeckInfo& d : decks_) d = DeckInfo{};  // the new engine has empty decks
            }
            start_audio();
            set_status("The audio engine was restarted; your decks are empty, load your tracks again");
            break;
          }
          case RemoteEngine::Status::GaveUp:
            set_status("The audio engine keeps failing and was not restarted again. Restart DeckHand.");
            break;
          case RemoteEngine::Status::LoadRejected: set_status(message); break;
          default: break;
        }
      });
      remote->set_ui_request_handler([this](const RemoteEngine::UiRequest& r) {
        switch (r.kind) {
          case RemoteEngine::UiRequest::Browse: browse(r.value); break;
          case RemoteEngine::UiRequest::BrowsePress: browse_press(); break;
          case RemoteEngine::UiRequest::LoadSelected: load_selected(r.value); break;
        }
      });
      hub_ = std::make_unique<RemoteHub>(*remote);
      engine_ = std::move(remote);
      start_signal_watch();
      return;
    }
    DH_LOG(Error, "engine process unavailable (%s); using in-process audio", error.c_str());
    set_status("Could not start the audio engine process (" + error + "); using in-process audio");
  }
#endif
  engine_ = std::make_unique<Engine>();
  local_hub_ = std::make_unique<ControllerHub>(*this, load_mappings(mapping_folders));
  midi_ = create_midi_io(
      [this](const std::string& device, const std::uint8_t* data, std::size_t n) { local_hub_->midi(device, data, n); },
      [this] {
        local_hub_->devices_changed();
        audio_recheck_ = true;
      });
  local_hub_->set_io(midi_.get());
  hub_ = std::make_unique<LocalHub>(*local_hub_);
}

// A normal quit stops the engine process. SIGTERM / SIGHUP / SIGINT count as normal; a crash or SIGKILL does not.
#if defined(DH_HAVE_ENGINE_PROCESS)
namespace {
int g_signal_pipe[2] = {-1, -1};
void on_quit_signal(int s) {
  const char c = static_cast<char>(s);
  if (g_signal_pipe[1] >= 0) (void)!write(g_signal_pipe[1], &c, 1);
}
}  // namespace
#endif

void App::start_signal_watch() {
#if defined(DH_HAVE_ENGINE_PROCESS)
  if (pipe(g_signal_pipe) != 0) return;
  for (const int fd : g_signal_pipe) fcntl(fd, F_SETFD, FD_CLOEXEC);
  struct sigaction sa {};
  sa.sa_handler = on_quit_signal;
  sigemptyset(&sa.sa_mask);
  for (const int sig : {SIGTERM, SIGHUP, SIGINT}) sigaction(sig, &sa, nullptr);
  signal_thread_ = std::thread([this] {
    char c = 0;
    if (read(g_signal_pipe[0], &c, 1) != 1 || c == 0) return;
    DH_LOG(Info, "signal %d: stopping the audio engine and quitting", static_cast<int>(c));
    if (remote_) remote_->shutdown_remote(120);
    signal(c, SIG_DFL);
    raise(c);
  });
#endif
}

void App::stop_signal_watch() {
#if defined(DH_HAVE_ENGINE_PROCESS)
  if (!signal_thread_.joinable()) return;
  const char zero = 0;
  (void)!write(g_signal_pipe[1], &zero, 1);
  signal_thread_.join();
  for (const int sig : {SIGTERM, SIGHUP, SIGINT}) signal(sig, SIG_DFL);
  for (int& fd : g_signal_pipe) {
    close(fd);
    fd = -1;
  }
#endif
}

void App::start_audio() {
  std::lock_guard<std::mutex> audio_lock(audio_mutex_);
  OutputConfig c;
  std::string pref;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    pref = audio_pref_;
    c.period_frames = audio_period_;
  }
  std::string match;
  int master = 0, phones = 2;
  const auto lower = [](std::string x) {
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return x;
  };
  if (hub_->audio_hint(match, master, phones)) {
    for (const std::string& name : engine_->devices()) {
      if (lower(name).find(match) != std::string::npos) {
        c.device_hint = match;
        c.master_channel = master;
        c.phones_channel = phones;
      }
    }
  }
  const std::string followed = c.device_hint;  // what following the controller would pick
  c.follow_controller = pref.empty();
  if (!pref.empty()) c.device_hint = lower(pref);  // an explicit choice wins over the controller
  std::string error;
  const bool ok = engine_->start(c, error);  // stops a running stream first
  std::lock_guard<std::mutex> lock(state_mutex_);
  audio_choice_ = pref.empty() ? followed : "pref:" + pref;
  if (ok) {
    audio_label_ = fmt("%s  %d ch  %.1f kHz", engine_->device_name().c_str(), engine_->output_channels(),
                       engine_->sample_rate() / 1000.0);
  } else {
    audio_label_ = T("app.no_audio");
    status_ = error;
    DH_LOG(Error, "%s", error.c_str());
  }
}

void App::restart_audio() { start_audio(); }

void App::control_main() {
  double last_audio_check = now_seconds(), last_tick = last_audio_check, last_sync = last_audio_check;
  struct Played {
    std::int64_t track_id = 0;
    double seconds = 0, saved = 0;
    bool logged = false;
  } played[kMaxDecks];
  while (!quit_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (remote_) remote_->heartbeat();  // tells the engine process this UI is alive
    EngineEvent e;
    while (engine_->poll(e)) {
      if (e.type == EventType::ScheduledFired || e.type == EventType::ScheduledDropped) {
        if (api_) api_->engine_event(e);  // only ever produced by API requests, so api_ exists
        continue;
      }
      if (e.track_id == 0) continue;
      Job j;
      j.track_id = e.track_id;
      j.slot = e.slot;
      if (e.type == EventType::HotCueSet) {
        j.kind = Job::SaveHotCue;
        j.ms = e.frame / engine_->deck(e.deck).sample_rate * 1000.0;
        post(j);
      } else if (e.type == EventType::HotCueCleared) {
        j.kind = Job::ClearHotCue;
        post(j);
      }
    }
    DeckView views[kMaxDecks];
    for (int i = 0; i < kMaxDecks; ++i) views[i] = engine_->deck(i);
    const double now = now_seconds();
    // Play history: a track counts once it has been audible for a few seconds.
    for (int i = 0; i < kMaxDecks; ++i) {
      Played& p = played[i];
      const DeckView& v = views[i];
      if (v.track_id != p.track_id) p = Played{v.track_id, 0, 0, false};
      if (v.loaded && v.playing && v.fader > 0.05f) p.seconds += now - last_tick;
      if (p.track_id != 0 && !p.logged && p.seconds >= 8.0) {
        p.logged = true;
        Job j;
        j.kind = Job::HistoryStart;
        j.deck = i;
        j.track_id = p.track_id;
        j.ms = static_cast<double>(wall_ms()) - p.seconds * 1000.0;
        post(j);
      }
      if (p.logged && p.seconds - p.saved >= 15.0) {
        p.saved = p.seconds;
        Job j;
        j.kind = Job::HistoryUpdate;
        j.deck = i;
        j.track_id = p.track_id;
        j.ms = p.seconds * 1000.0;
        post(j);
      }
    }
    last_tick = now;
    {
      const MixerView mixer = engine_->mixer();
      SamplerView samplers[kSamplerSlots];
      for (int i = 0; i < kSamplerSlots; ++i) samplers[i] = engine_->sampler(i);
      if (local_hub_) local_hub_->tick(views, kMaxDecks, &mixer, samplers, kSamplerSlots, now);
    }
    engine_->collect_garbage();

    // Follow controllers being plugged in or removed: use their audio interface when present.
    // (with the engine in its own process it follows controllers and reopens lost devices by itself)
    if (now - last_sync > 300.0) {  // library sync every five minutes when a folder is set
      last_sync = now;
      Job j;
      j.kind = Job::Sync;
      post(j);
    }
    if (!remote_ && (audio_recheck_.exchange(false) || now - last_audio_check > 2.0)) {
      last_audio_check = now;
      std::string match, want;
      int master = 0, phones = 2;
      if (hub_->audio_hint(match, master, phones)) {
        for (const std::string& name : engine_->devices()) {
          std::string low = name;
          std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return std::tolower(ch); });
          if (low.find(match) != std::string::npos) want = match;
        }
      }
      std::string have;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        have = audio_choice_;
      }
      bool explicit_pref;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        explicit_pref = !audio_pref_.empty();
      }
      if ((!explicit_pref && want != have) || !engine_->running()) start_audio();
    }
  }
}

void App::loader_main() {
  Library lib(data_dir_ + "/library.db");  // own connection for background work
  while (!quit_) {
    Job j;
    {
      std::unique_lock<std::mutex> lock(job_mutex_);
      job_cv_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
      if (quit_) return;
      j = std::move(jobs_.front());
      jobs_.pop_front();
    }
    try {
      run_job(lib, j);
    } catch (const std::exception& e) {
      set_status(T("status.error", {{"message", e.what()}}));
      std::lock_guard<std::mutex> lock(state_mutex_);
      importing_ = exporting_ = false;
      if (j.deck >= 0 && j.deck < kMaxDecks) decks_[j.deck].loading = false;
    }
  }
}

void App::run_job(Library& lib, const Job& j) {
  switch (j.kind) {
    case Job::Load: load_track(lib, j.deck, j.track_id); break;
    case Job::Reattach: load_track(lib, j.deck, j.track_id, true); break;
    case Job::AddFile: {
      FileTags tags;
      read_file_tags(j.path, tags);
      NewTrack t;
      t.path = j.path;
      t.title = tags.title.empty() ? std::filesystem::path(j.path).stem().string() : tags.title;
      t.artist = tags.artist;
      t.album = tags.album;
      t.duration_ms = tags.duration_ms;
      const auto existing = lib.track_id_for_path(j.path);
      const std::int64_t id = existing ? *existing : lib.upsert_track(t);
      rows_dirty_ = true;
      set_status(T("status.added", {{"title", t.title}}));
      if (j.deck >= 0) load_track(lib, j.deck, id);
      break;
    }
    case Job::Sync: {
      const std::string folder = lib.setting("sync.folder");
      if (folder.empty()) break;
      deckhand::sync::SyncConfig cfg;
      cfg.folder = folder;
      cfg.device_name = lib.setting("sync.device_name", "DeckHand");
      deckhand::sync::SyncEngine engine(lib, cfg);
      const deckhand::sync::SyncResult r = engine.sync_once();
      for (const std::string& w : r.warnings) DH_LOG(Warn, "sync: %s", w.c_str());
      if (!r.ok) {
        set_status(T("status.sync_failed", {{"message", r.error}}));
      } else if (r.records_written > 0 || r.records_applied > 0) {
        rows_dirty_ = true;
        set_status(T("status.synced", {{"sent", r.records_written}, {"files", r.records_applied}}));
      }
      break;
    }
    case Job::Import: {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        importing_ = true;
        status_ = T("status.importing");
      }
      ImportOptions o;
      o.source_db = source_db();
      o.analysis_root = std::filesystem::path(o.source_db).parent_path().string() + "/share";
      o.scratch_dir = data_dir_ + "/scratch";
      std::ifstream key(data_dir_ + "/source-v6.key");
      std::getline(key, o.key);
      while (!o.key.empty() && std::isspace(static_cast<unsigned char>(o.key.back()))) o.key.pop_back();
      if (o.key.empty()) throw std::runtime_error("import needs the key file " + data_dir_ + "/source-v6.key");
      const ImportStats s = import_library_v6(lib, o);
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        importing_ = false;
      }
      rows_dirty_ = true;
      set_status(T("status.imported", {{"added", s.tracks_added}, {"updated", s.tracks_updated}, {"playlists", s.playlists}}));
      break;
    }
    case Job::SaveHotCue: lib.set_hot_cue(j.track_id, j.slot, j.ms); break;
    case Job::ClearHotCue: lib.clear_hot_cue(j.track_id, j.slot); break;
    case Job::Export: {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        exporting_ = true;
      }
      UsbExportRequest req;
      req.destination = j.path;
      if (j.track_id) req.playlist_ids.push_back(j.track_id);
      UsbExportResult res;
      std::string error;
      const bool ok = export_library_to_usb(lib, req, res, error, [&](int done, int total, const std::string& title) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status_ = T("status.exporting", {{"done", done}, {"total", total}, {"title", title}});
        return !quit_.load();
      });
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        exporting_ = false;
      }
      if (!ok) set_status(T("status.export_failed", {{"message", error}}));
      else if (res.missing_files) set_status(T("status.exported_missing", {{"tracks", res.tracks}, {"playlists", res.playlists}, {"missing", res.missing_files}}));
      else set_status(T("status.exported", {{"tracks", res.tracks}, {"playlists", res.playlists}, {"path", j.path}}));
      break;
    }
    case Job::RecFinish:
      engine_->record_close();  // waits for the writer to finalise the file
      set_status(T("status.recording_saved", {{"path", j.path}}));
      break;
    case Job::LoadSample: {
      std::string error;
      const std::shared_ptr<TrackAudio> audio = decode_file(j.path, error);
      if (!audio) {
        set_status(T("status.error", {{"message", error}}));
        break;
      }
      auto sample = std::make_unique<LoadedSample>();
      sample->audio = audio;
      engine_->sampler_load(j.slot, std::move(sample));
      if (j.slot >= 0 && j.slot < kSamplerSlots) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        sampler_names_[j.slot] = std::filesystem::path(j.path).stem().string();
      }
      set_status(T("status.sample_loaded", {{"slot", j.slot + 1}, {"title", std::filesystem::path(j.path).stem().string()}}));
      break;
    }
    case Job::HistoryStart:
      if (history_session_ == 0) history_session_ = lib.begin_history_session();
      history_row_[j.deck] = lib.add_history(history_session_, j.track_id, std::string(1, static_cast<char>('A' + j.deck)),
                                             static_cast<std::int64_t>(j.ms));
      rows_dirty_ = true;
      break;
    case Job::HistoryUpdate:
      if (history_row_[j.deck]) lib.set_history_played(history_row_[j.deck], static_cast<std::int64_t>(j.ms));
      break;
  }
}

void App::load_track(Library& lib, int deck, std::int64_t track_id, bool attach_only) {
  const auto row = lib.track(track_id);
  if (!row || deck < 0 || deck >= kMaxDecks) return;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    decks_[deck].loading = true;
    decks_[deck].error.clear();
  }
  std::string error;
  const std::shared_ptr<TrackAudio> audio = decode_file(row->path, error);
  if (!audio) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    decks_[deck].loading = false;
    decks_[deck].error = std::filesystem::exists(row->path) ? error : T("deck.file_not_found", {{"path", row->path}});
    DH_LOG(Warn, "deck %d: %s", deck + 1, decks_[deck].error.c_str());
    return;
  }
  const double sr = audio->sample_rate;
  auto loaded = std::make_unique<LoadedTrack>();
  loaded->audio = audio;
  loaded->track_id = track_id;
  loaded->path = row->path;
  loaded->bpm = row->bpm;
  std::vector<BeatSection> grid = lib.beatgrid(track_id);
  std::string key_text = row->key;
  std::optional<double> gain = lib.gain_db(track_id);
  if (grid.empty() || !gain) {  // not analysed yet: do it once and keep the result
    const TrackAnalysis a = analyse_track(*audio);
    if (grid.empty() && a.has_grid) {
      grid = a.grid;
      lib.replace_beatgrid(track_id, grid);
      lib.set_bpm(track_id, a.bpm);
      loaded->bpm = a.bpm;
    }
    if (key_text.empty() && a.has_key) {
      key_text = a.key;
      lib.set_key(track_id, key_text);
    }
    if (a.has_gain) {
      gain = a.gain_db;
      lib.set_gain_db(track_id, a.gain_db);
    }
    rows_dirty_ = true;
    DH_LOG(Info, "analysed %s: %.2f BPM, key %s, gain %+.1f dB", row->title.c_str(), a.bpm, a.key.c_str(), a.gain_db);
  }
  for (const BeatSection& b : grid) {
    loaded->grid.push_back({b.first_beat_ms / 1000.0 * sr, b.bpm, b.bar_beat});
  }
  for (const Cue& c : lib.cues(track_id)) {
    const double frame = c.start_ms / 1000.0 * sr;
    if (c.kind == CueKind::Memory) {
      loaded->memory_cues.push_back(frame);
    } else if (c.slot >= 0 && c.slot < kHotCues) {
      loaded->hot_cue[c.slot] = frame;
      loaded->hot_loop_end[c.slot] = c.kind == CueKind::Loop ? c.end_ms / 1000.0 * sr : -1;
    }
  }
  std::sort(loaded->memory_cues.begin(), loaded->memory_cues.end());

  DeckInfo info;
  info.row = *row;
  info.row.key = key_text;
  if (loaded->bpm > 0) info.row.bpm = loaded->bpm;
  info.gain_db = gain.value_or(0.0);
  info.audio = audio;
  info.grid = loaded->grid;
  const std::size_t columns = 800, points = audio->wave.size();
  info.overview.resize(columns);
  for (std::size_t c = 0; c < columns && points > 0; ++c) {
    const std::size_t a = c * points / columns, b = std::max(a + 1, (c + 1) * points / columns);
    WavePoint p;
    for (std::size_t i = a; i < b && i < points; ++i) {
      p.low = std::max(p.low, audio->wave[i].low);
      p.mid = std::max(p.mid, audio->wave[i].mid);
      p.high = std::max(p.high, audio->wave[i].high);
    }
    info.overview[c] = p;
  }
  if (attach_only) {  // the engine already holds this track: only rebuild what the UI shows
    if (engine_->deck(deck).track_id != track_id) return;
  } else {
    engine_->load(deck, std::move(loaded));
    Command g;  // loudness trim from analysis, kept within a sane range
    g.op = Op::AutoGain;
    g.deck = static_cast<std::uint8_t>(deck);
    g.f = static_cast<float>(std::clamp(gain.value_or(0.0), -12.0, 12.0));
    engine_->send(g);
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    decks_[deck] = std::move(info);
  }
  DH_LOG(Info, "deck %d loaded: %s - %s (%.1f s, %u Hz)", deck + 1, row->artist.c_str(), row->title.c_str(),
         static_cast<double>(audio->frames) / sr, audio->sample_rate);
}

// Starts or stops recording the master output. Safe from any thread.
bool App::record(bool start, std::string& path, std::string& error) {
  std::lock_guard<std::mutex> lock(record_mutex_);
  const RecorderStatus st = engine_->recorder_status();
  if (start) {
    if (st.recording) {
      error = "already recording";
      path = record_path_;
      return false;
    }
    if (st.open) engine_->record_close();
    const std::string dir = data_dir_ + "/Recordings";
    std::filesystem::create_directories(dir);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char name[64];
    std::strftime(name, sizeof name, "DeckHand %Y-%m-%d %H.%M.%S.wav", &tm);
    record_path_ = dir + "/" + name;
    if (!engine_->record_open(record_path_, false, error)) return false;
    path = record_path_;
    set_status(T("status.recording", {{"path", record_path_}}));
    return true;
  }
  if (!st.recording) {
    error = "not recording";
    return false;
  }
  engine_->record_stop();
  if (record_path_.empty()) record_path_ = engine_->recorder_path();  // recording started by a previous UI
  path = record_path_;
  Job j;
  j.kind = Job::RecFinish;
  j.path = record_path_;
  post(j);
  return true;
}

void App::request_load(int deck, std::int64_t track_id) {
  if (deck < 0 || deck >= kMaxDecks || track_id == 0) return;
  if (engine_->deck(deck).playing) {  // never replace a track that is playing
    set_status(T("deck.playing_stop_first", {{"deck", deck_letter(deck)}}));
    return;
  }
  Job j;
  j.kind = Job::Load;
  j.deck = deck;
  j.track_id = track_id;
  post(j);
}

// ---- controller thread entry points ---------------------------------------------------
void App::browse(int steps) {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  if (focus_tree_) {
    tree_sel_ = std::clamp(tree_sel_ + steps, 0, std::max(0, static_cast<int>(tree_.size()) - 1));
    apply_tree_selection();
  } else {
    move_selection(steps);
  }
}

void App::browse_press() {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  focus_tree_ = !focus_tree_;
}

void App::load_selected(int deck) {
  std::int64_t id = 0;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (rows_dirty_ || sel_ < 0 || sel_ >= static_cast<int>(rows_.size())) return;
    id = rows_[static_cast<std::size_t>(sel_)].id;
  }
  request_load(deck, id);
}

// ---- input ----------------------------------------------------------------------------
void App::pointer(dh_ptr_kind kind, float x, float y, int clicks) {
  raw_.x = x / scale_;
  raw_.y = y / scale_;
  if (kind == DH_PTR_DOWN) {
    raw_.down = raw_.pressed = true;
    raw_.clicks = clicks;
  } else if (kind == DH_PTR_UP || kind == DH_PTR_CANCEL) {
    raw_.down = false;
    raw_.released = true;
  }
}

void App::apply_tree_selection() {  // browse_mutex_ held
  if (tree_sel_ < 0 || tree_sel_ >= static_cast<int>(tree_.size())) return;
  const TreeItem& item = tree_[static_cast<std::size_t>(tree_sel_)];
  confirm_delete_ = 0;
  if (item.kind == TreeItem::Folder || item.kind == TreeItem::HistoryHeader) return;  // containers show nothing
  view_ = item.kind;
  playlist_id_ = item.id;
  sel_ = 0;
  scroll_ = 0;
  rows_dirty_ = true;
}

void App::commit_rename() {  // browse_mutex_ held, UI thread
  rename_fresh_ = false;
  if (rename_id_ != 0 && !rename_buf_.empty()) lib_->rename_playlist(rename_id_, rename_buf_);
  rename_id_ = 0;
  rename_buf_.clear();
  rows_dirty_ = true;
}

void App::move_selection(int steps) {  // browse_mutex_ held
  if (rows_.empty()) return;
  sel_ = std::clamp(sel_ + steps, 0, static_cast<int>(rows_.size()) - 1);
  follow_sel_ = true;
}

void App::key(dh_key key, std::uint32_t mods) {
  if (focus_ && field_key(key)) return;  // an overlay text field has the keyboard
  if (panel_ != Panel::None) {
    if (key == DH_KEY_ESCAPE) close_panel();
    return;
  }
  std::int64_t load_id = 0;
  int load_deck = -1;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    const bool have = sel_ >= 0 && sel_ < static_cast<int>(rows_.size());
    if (rename_id_ != 0) {  // the name field takes the keyboard
      if (key == DH_KEY_ENTER) commit_rename();
      else if (key == DH_KEY_ESCAPE) rename_id_ = 0;
      else if (key == DH_KEY_BACKSPACE) {
        if (rename_fresh_) rename_buf_.clear();
        else utf8_pop(rename_buf_);
        rename_fresh_ = false;
      }
      return;
    }
    switch (key) {
      case DH_KEY_UP:
      case DH_KEY_DOWN: {
        const int step = key == DH_KEY_UP ? -1 : 1;
        if (focus_tree_) {
          tree_sel_ = std::clamp(tree_sel_ + step, 0, std::max(0, static_cast<int>(tree_.size()) - 1));
          apply_tree_selection();
        } else if ((mods & DH_MOD_ALT) && have && view_is_editable_playlist()) {
          const int to = sel_ + step;  // reorder within the playlist
          if (to >= 0 && to < static_cast<int>(rows_.size())) {
            lib_->move_in_playlist(playlist_id_, sel_, to);
            sel_ = to;
            follow_sel_ = true;
            rows_dirty_ = true;
          }
        } else {
          move_selection(step);
        }
        break;
      }
      case DH_KEY_LEFT:
      case DH_KEY_RIGHT:
        if ((mods & (DH_MOD_SHIFT | DH_MOD_COMMAND)) && have) {
          load_deck = key == DH_KEY_LEFT ? 0 : 1;
          load_id = rows_[static_cast<std::size_t>(sel_)].id;
        }
        break;
      case DH_KEY_ENTER:
        if (search_focus_) {
          search_focus_ = false;
        } else if (have) {
          load_deck = engine_->deck(0).playing ? 1 : 0;
          load_id = rows_[static_cast<std::size_t>(sel_)].id;
        }
        break;
      case DH_KEY_ESCAPE:
        if (!search_.empty()) rows_dirty_ = true;
        search_.clear();
        search_focus_ = false;
        break;
      case DH_KEY_BACKSPACE:
        if (!search_.empty()) {
          utf8_pop(search_);
          sel_ = 0;
          rows_dirty_ = true;
        }
        break;
      case DH_KEY_TAB: focus_tree_ = !focus_tree_; break;
    }
  }
  if (load_deck >= 0) request_load(load_deck, load_id);
}

void App::text(const char* utf8) {
  if (!utf8 || !*utf8 || static_cast<unsigned char>(*utf8) < 0x20) return;
  if (focus_) {
    if (focus_->text.size() < 200) focus_->text += utf8;
    return;
  }
  if (panel_ != Panel::None) return;
  std::lock_guard<std::mutex> lock(browse_mutex_);
  if (rename_id_ != 0) {
    if (rename_fresh_) rename_buf_.clear();
    rename_fresh_ = false;
    if (rename_buf_.size() < 120) rename_buf_ += utf8;
    return;
  }
  search_ += utf8;  // typing anywhere searches the library
  search_focus_ = true;
  sel_ = 0;
  scroll_ = 0;
  rows_dirty_ = true;
}

void App::open_file(const std::string& path, float x, float y) {
  const float px = x / scale_, py = y / scale_;
  if (panel_ == Panel::Sampler && x >= 0) {  // dropped on a sampler slot
    for (int i = 0; i < kSamplerSlots; ++i) {
      if (sampler_slot_rect_[i].contains(px, py)) {
        Job j;
        j.kind = Job::LoadSample;
        j.slot = i;
        j.path = path;
        post(j);
        return;
      }
    }
  }
  Job j;
  j.kind = Job::AddFile;
  j.path = path;
  if (x >= 0 && y >= 0) {
    if (skin_ && skin_->loaded() && !legacy_) {  // the deck under the drop point, by the widgets' bindings
      const auto& ws = skin_->current().widgets;
      for (std::size_t i = ws.size(); i-- > 0;) {
        if (ws[i].bind.empty() || !ws[i].rect.contains(px, py)) continue;
        const ui::Target t = ui::map_binding(ws[i].bind);
        if (t.deck >= 0 && t.deck < kMaxDecks) {
          j.deck = t.deck;
          break;
        }
      }
    }
    // Before the first layout, or off any deck widget in the upper part of the window: left half = deck A.
    if (j.deck < 0 && py < (height_px_ / scale_) * 0.5f && (!skin_ || skin_->current().widgets.empty()))
      j.deck = px < (width_px_ / scale_) / 2 ? 0 : 1;
  }
  if (j.deck >= 0 && engine_->deck(j.deck).playing) j.deck = -1;
  post(j);
}

void App::refresh_rows() {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  // Flatten the playlist tree depth-first, then append the play history.
  const std::vector<PlaylistNode> nodes = lib_->playlists();
  tree_.clear();
  tree_.push_back({TreeItem::Collection, 0, 0, T("browser.collection"), static_cast<int>(lib_->track_count())});
  const auto add_children = [&](auto&& self, std::int64_t parent, int depth) -> void {
    if (depth > 16) return;
    for (const PlaylistNode& n : nodes) {
      if (n.parent != parent) continue;
      tree_.push_back({n.is_folder ? TreeItem::Folder : TreeItem::Playlist, n.id, depth, n.name, n.track_count});
      if (n.is_folder) self(self, n.id, depth + 1);
    }
  };
  add_children(add_children, 0, 0);
  const auto sessions = lib_->history_sessions();
  if (!sessions.empty()) {
    tree_.push_back({TreeItem::HistoryHeader, 0, 0, T("browser.history"), 0});
    for (std::size_t i = 0; i < sessions.size() && i < 30; ++i)
      tree_.push_back({TreeItem::HistorySession, sessions[i].id, 1, date_text(sessions[i].started_at_ms), sessions[i].entries});
  }
  // Keep the tree selection on the item being viewed, even when the tree changed.
  tree_sel_ = 0;
  bool found = view_ == TreeItem::Collection;
  for (std::size_t i = 0; i < tree_.size(); ++i) {
    if (tree_[i].kind == view_ && tree_[i].id == playlist_id_) {
      tree_sel_ = static_cast<int>(i);
      found = true;
    }
  }
  if (!found) {
    view_ = TreeItem::Collection;
    playlist_id_ = 0;
  }
  if (view_ == TreeItem::HistorySession) {
    rows_.clear();
    for (const HistoryEntry& e : lib_->history(playlist_id_)) {
      if (auto row = lib_->track(e.track_id)) rows_.push_back(std::move(*row));
    }
  } else {
    rows_ = lib_->tracks(view_ == TreeItem::Playlist ? playlist_id_ : 0, search_, sort_, sort_desc_);
  }
  sel_ = rows_.empty() ? 0 : std::clamp(sel_, 0, static_cast<int>(rows_.size()) - 1);
}

const dh_frame* App::frame() {
  now_ = now_seconds();
  if (!status_note_.empty()) {
    set_status(status_note_);
    status_note_.clear();
  }
  if (rows_dirty_.exchange(false)) refresh_rows();
  const float w = width_px_ / scale_, h = height_px_ / scale_;
  dl_.begin(width_px_, height_px_, scale_);
  dl_.rect(0, 0, w, h, theme_.bg);

  // One consistent snapshot of the engine and deck state for the whole frame.
  for (int i = 0; i < kMaxDecks; ++i) views_[i] = engine_->deck(i);
  mixer_ = engine_->mixer();
  for (int i = 0; i < kSamplerSlots; ++i) samplers_[i] = engine_->sampler(i);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (int i = 0; i < kMaxDecks; ++i) info_[i] = decks_[i];
  }

  raw_.shift = (mods_ & DH_MOD_SHIFT) != 0;
  in_ = raw_;
  if (overlay_open()) {  // the overlay owns the pointer: the main UI sees it far away and never pressed
    in_.x = in_.y = -1e4f;
    in_.pressed = false;
    in_.clicks = 0;
    in_.scroll = 0;
  }

  if (skin_ && skin_->loaded() && !legacy_) draw_skin_ui();
  else draw_legacy();
  draw_overlays();

  raw_.pressed = raw_.released = false;
  raw_.clicks = 0;
  raw_.scroll = 0;

  // ---- hand the frame to the shell ----------------------------------------------------------
  cmds_out_.clear();
  for (const gfx::DrawCmd& c : dl_.commands()) {
    if (c.index_count == 0) continue;
    dh_draw_cmd o{};
    o.texture_id = 1;
    std::copy(std::begin(c.clip), std::end(c.clip), std::begin(o.clip));
    o.idx_offset = c.index_offset;
    o.idx_count = c.index_count;
    cmds_out_.push_back(o);
  }
  frame_ = dh_frame{};
  frame_.vtx = reinterpret_cast<const dh_vertex*>(dl_.vertices().data());
  frame_.vtx_count = static_cast<std::uint32_t>(dl_.vertices().size());
  frame_.idx = dl_.indices().data();
  frame_.idx_count = static_cast<std::uint32_t>(dl_.indices().size());
  frame_.cmds = cmds_out_.data();
  frame_.cmd_count = static_cast<std::uint32_t>(cmds_out_.size());
  int y0 = 0, y1 = 0;
  if (dl_.atlas_dirty(y0, y1)) {
    upload_.id = 1;
    upload_.tex_w = gfx::DrawList::kAtlasWidth;
    upload_.tex_h = gfx::DrawList::kAtlasHeight;
    upload_.fmt = DH_TEX_A8;
    upload_.x = 0;
    upload_.y = static_cast<std::uint32_t>(y0);
    upload_.w = gfx::DrawList::kAtlasWidth;
    upload_.h = static_cast<std::uint32_t>(y1 - y0);
    upload_.stride = gfx::DrawList::kAtlasWidth;
    upload_.pixels = dl_.atlas() + static_cast<std::size_t>(y0) * gfx::DrawList::kAtlasWidth;
    frame_.uploads = &upload_;
    frame_.upload_count = 1;
    dl_.clear_atlas_dirty();
  }
  frame_.clear_rgba[0] = static_cast<float>(theme_.bg & 0xFF) / 255.0f;
  frame_.clear_rgba[1] = static_cast<float>((theme_.bg >> 8) & 0xFF) / 255.0f;
  frame_.clear_rgba[2] = static_cast<float>((theme_.bg >> 16) & 0xFF) / 255.0f;
  frame_.clear_rgba[3] = 1;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    frame_.wants_text_input = search_focus_ || rename_id_ != 0 || focus_ != nullptr;
  }
  return &frame_;
}

}  // namespace deckhand

// ---- C interface -----------------------------------------------------------------------------
struct dh_app {
  deckhand::App app;
  dh_app(const dh_platform& p, const dh_config& c) : app(p, c) {}
};

extern "C" {

dh_app* dh_app_create(const dh_platform* platform, const dh_config* config) {
  if (!platform || !config) return nullptr;
  try {
    return new dh_app(*platform, *config);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "DeckHand failed to start: %s\n", e.what());
    return nullptr;
  }
}
void dh_app_destroy(dh_app* app) { delete app; }
void dh_app_set_display(dh_app* app, float w, float h, float scale, float il, float it, float ir, float ib) {
  app->app.set_display(w, h, scale, il, it, ir, ib);
}
void dh_app_modifiers(dh_app* app, uint32_t mods) { app->app.set_modifiers(mods); }
void dh_app_pointer(dh_app* app, dh_ptr_kind kind, uint32_t, float x, float y, int clicks) {
  app->app.pointer(kind, x, y, clicks);
}
void dh_app_scroll(dh_app* app, float, float, float, float dy) { app->app.scroll(dy); }
void dh_app_key(dh_app* app, dh_key key, uint32_t mods) { app->app.key(key, mods); }
void dh_app_text(dh_app* app, const char* utf8) { app->app.text(utf8); }
void dh_app_open_file(dh_app* app, const char* path, float x, float y) {
  if (path) app->app.open_file(path, x, y);
}
void dh_app_audio_route(dh_app* app) { app->app.audio_route(); }
const dh_frame* dh_app_frame(dh_app* app, double) { return app->app.frame(); }

}  // extern "C"
