// DeckHand application core: owns the library, engine and controllers, and draws the UI.
// The default layout is drawn in code for now; it becomes a skin once the skin engine lands.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "deckhand/app/api_bridge.hpp"
#include "deckhand/app/capi.h"
#include "deckhand/app/stream_bridge.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/gfx/draw.hpp"
#include "deckhand/i18n/i18n.hpp"
#include "deckhand/import/import.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/log/log.hpp"
#include "deckhand/midi/controller.hpp"
#include "deckhand/services/track_analysis.hpp"
#include "deckhand/services/usb_export.hpp"

namespace deckhand {
namespace {

using gfx::Align;
using gfx::Color;
using gfx::rgba;
using gfx::with_alpha;

static_assert(sizeof(gfx::Vertex) == sizeof(dh_vertex), "vertex layouts must match");

constexpr int kUiDecks = 2;  // decks shown; the engine itself runs kMaxDecks

constexpr Color kBg = rgba(16, 18, 22), kPanel = rgba(25, 28, 34), kPanelHi = rgba(35, 39, 47);
constexpr Color kLine = rgba(52, 57, 66), kText = rgba(232, 234, 238), kDim = rgba(136, 143, 154);
constexpr Color kAccent = rgba(70, 130, 255), kWarn = rgba(255, 96, 96), kGood = rgba(80, 210, 120);
constexpr Color kDeck[kUiDecks] = {rgba(46, 211, 183), rgba(255, 176, 46)};
constexpr Color kHot[kHotCues] = {rgba(255, 92, 92),  rgba(255, 160, 60), rgba(250, 220, 70), rgba(110, 220, 90),
                                  rgba(60, 210, 200), rgba(80, 150, 255), rgba(170, 110, 255), rgba(255, 110, 200)};

Color mix(Color a, Color b, float t) {
  const auto ch = [&](int shift) {
    const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
    return static_cast<Color>(std::lround(x + (y - x) * t)) << shift;
  };
  return ch(0) | ch(8) | ch(16) | (a & 0xFF000000u);
}

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

std::string clock_text(double seconds) {
  const bool neg = seconds < 0;
  const int total = static_cast<int>(std::fabs(seconds));
  return fmt("%s%d:%02d", neg ? "-" : "", total / 60, total % 60);
}

void utf8_pop(std::string& s) {
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  if (!s.empty()) s.pop_back();
}

double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct DeckInfo {
  TrackRow row;
  std::shared_ptr<const TrackAudio> audio;
  std::vector<GridSection> grid;
  std::vector<WavePoint> overview;
  bool loading = false;
  std::string error;
  double gain_db = 0;
};

struct Job {
  enum Kind { Load, AddFile, Import, SaveHotCue, ClearHotCue, HistoryStart, HistoryUpdate, Export, RecFinish, LoadSample } kind = Load;
  int deck = -1;
  std::int64_t track_id = 0;
  std::string path;
  int slot = 0;
  double ms = 0;
};

struct TreeItem {
  enum Kind { Collection, Folder, Playlist, HistoryHeader, HistorySession } kind = Collection;
  std::int64_t id = 0;
  int depth = 0;
  std::string name;
  int count = 0;
};

std::int64_t wall_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string date_text(std::int64_t ms) {
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

}  // namespace

class App final : public ControlSink, public ApiHost {
 public:
  App(const dh_platform& platform, const dh_config& config);
  ~App() override;

  void set_display(float w, float h, float scale) {
    width_px_ = w;
    height_px_ = h;
    scale_ = scale > 0 ? scale : 1;
  }
  void pointer(dh_ptr_kind kind, float x, float y, int clicks);
  void scroll(float dy) { in_.scroll += dy / scale_; }
  void key(dh_key key, std::uint32_t mods);
  void text(const char* utf8);
  void open_file(const std::string& path, float x, float y);
  void audio_route() { audio_recheck_ = true; }
  const dh_frame* frame();

  // ApiHost (called from API threads)
  Engine& api_engine() override { return engine_; }
  bool api_load(int deck, std::int64_t track_id, std::string& error) override {
    if (engine_.deck(deck).playing) {
      error = "deck is playing; stop it before loading";
      return false;
    }
    request_load(deck, track_id);
    return true;
  }
  void api_add_file(const std::string& path, int deck) override {
    Job j;
    j.kind = Job::AddFile;
    j.path = path;
    j.deck = deck >= 0 && !engine_.deck(deck).playing ? deck : -1;
    post(j);
  }
  bool api_export(const std::string& destination, std::int64_t playlist_id, std::string& error) override {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (exporting_) {
        error = "an export is already running";
        return false;
      }
      exporting_ = true;
    }
    Job j;
    j.kind = Job::Export;
    j.path = destination;
    j.track_id = playlist_id;
    post(j);
    return true;
  }
  void api_library_changed() override { rows_dirty_ = true; }
  bool api_record(bool start, std::string& path, std::string& error) override { return record(start, path, error); }
  void api_load_sample(int slot, const std::string& path) override {
    Job j;
    j.kind = Job::LoadSample;
    j.slot = slot;
    j.path = path;
    post(j);
  }
  std::string api_status() override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return status_;
  }
  std::string api_audio_device() override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return audio_label_;
  }

  // ControlSink (called from the controller thread)
  void command(const Command& c) override { engine_.send(c); }
  void browse(int steps) override;
  void browse_press() override;
  void load_selected(int deck) override;

 private:
  // ---- background work ----------------------------------------------------------
  void post(Job j) {
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      jobs_.push_back(std::move(j));
    }
    job_cv_.notify_one();
  }
  void loader_main();
  void control_main();
  void run_job(Library& lib, const Job& j);
  void load_track(Library& lib, int deck, std::int64_t track_id);
  void start_audio();
  void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_ = s;
    DH_LOG(Info, "%s", s.c_str());
  }
  void request_load(int deck, std::int64_t track_id);
  bool record(bool start, std::string& path, std::string& error);
  std::string source_db() const {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/Library/Pioneer/rekordbox/master.db" : std::string();
  }

  // ---- UI ------------------------------------------------------------------------
  void refresh_rows();
  void move_selection(int steps);
  void apply_tree_selection();
  void commit_rename();
  bool view_is_editable_playlist() const {
    return view_ == TreeItem::Playlist && sort_ == SortKey::Default && search_.empty();
  }
  bool clicked(const Rect& r) const { return in_.pressed && r.contains(in_.x, in_.y); }
  bool button(const Rect& r, const std::string& label, bool lit, Color lit_color);
  void draw_top(const Rect& r);
  void draw_export_menu(float right, float top);
  void draw_wave(int di, const Rect& r, const DeckView& v, const DeckInfo& info);
  void draw_deck(int di, const Rect& r, const DeckView& v, const DeckInfo& info);
  void draw_mixer(const Rect& r, const DeckView* v, const MixerView& m);
  void draw_browser(const Rect& r, const DeckView* v);
  void meter(const Rect& r, float level);
  void knob_bar(float x, float y, float w, const char* label, float value, bool centred);

  // Text in the active language. Safe from any thread.
  std::string T(std::string_view id) const { return i18n_.tr(id); }
  std::string T(std::string_view id, const i18n::Args& args) const { return i18n_.tr(id, args); }
  static std::string deck_letter(int deck) { return std::string(1, static_cast<char>('A' + deck)); }

  std::string data_dir_, resource_dir_;
  i18n::Catalog i18n_;
  std::unique_ptr<Library> lib_;
  Engine engine_;
  std::unique_ptr<ControllerHub> hub_;
  std::unique_ptr<MidiIO> midi_;
  std::unique_ptr<ApiBridge> api_;
  std::unique_ptr<StreamBridge> stream_;
  gfx::DrawList dl_;

  std::atomic<bool> quit_{false};
  std::thread loader_, control_;
  std::mutex job_mutex_;
  std::condition_variable job_cv_;
  std::deque<Job> jobs_;
  std::int64_t history_session_ = 0, history_row_[kMaxDecks] = {};  // loader thread only

  std::mutex record_mutex_;
  std::string record_path_;

  std::mutex state_mutex_;  // guards the members below, shared with the worker threads
  DeckInfo decks_[kMaxDecks];
  std::string status_, audio_label_, audio_choice_;
  bool importing_ = false, exporting_ = false;
  std::atomic<bool> rows_dirty_{true};
  std::atomic<bool> audio_recheck_{false};

  std::mutex browse_mutex_;  // guards the browse state, shared with the controller thread
  std::vector<TreeItem> tree_;
  std::vector<TrackRow> rows_;
  TreeItem::Kind view_ = TreeItem::Collection;  // what the track table shows
  std::int64_t playlist_id_ = 0;                // playlist or history session, by view_
  int tree_sel_ = 0;                            // index into tree_
  SortKey sort_ = SortKey::Default;
  bool sort_desc_ = false;
  std::int64_t rename_id_ = 0;  // playlist being renamed in place
  std::string rename_buf_;
  std::int64_t confirm_delete_ = 0;
  bool add_menu_ = false;
  float tree_scroll_ = 0;
  int sel_ = 0;
  bool focus_tree_ = false;
  float scroll_ = 0;
  bool follow_sel_ = false;
  std::string search_;
  bool search_focus_ = false;

  struct Input {
    float x = 0, y = 0, scroll = 0;
    bool down = false, pressed = false, released = false;
    int clicks = 0;
  } in_;
  int held_ = 0;        // id of the control being held with the pointer
  int drag_deck_ = -1;
  float width_px_ = 1280, height_px_ = 800, scale_ = 1;
  float rows_view_h_ = 0, top_buttons_right_ = 0;
  Rect add_menu_rect_;
  bool export_menu_ = false;
  std::vector<std::string> export_targets_;
  std::string status_note_;  // UI thread; shown via set_status on the next frame

  std::vector<dh_draw_cmd> cmds_out_;
  dh_tex_upload upload_{};
  dh_frame frame_{};
};

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

  std::vector<std::string> folders;
  if (!resource_dir_.empty()) folders.push_back(resource_dir_ + "/controllers");
  folders.push_back(data_dir_ + "/controllers");
  hub_ = std::make_unique<ControllerHub>(*this, load_mappings(folders));
  midi_ = create_midi_io(
      [this](const std::string& device, const std::uint8_t* data, std::size_t n) { hub_->midi(device, data, n); },
      [this] {
        hub_->devices_changed();
        audio_recheck_ = true;
      });
  hub_->set_io(midi_.get());

  start_audio();
  loader_ = std::thread([this] { loader_main(); });
  control_ = std::thread([this] { control_main(); });

  api_ = std::make_unique<ApiBridge>(*this, data_dir_);
  stream_ = std::make_unique<StreamBridge>(engine_, data_dir_, [this](int deck, TrackRow& row) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!decks_[deck].audio) return false;
    row = decks_[deck].row;
    return true;
  });

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
  hub_->set_io(nullptr);
  midi_.reset();
  engine_.stop();
}

void App::start_audio() {
  OutputConfig c;
  std::string match;
  int master = 0, phones = 2;
  if (hub_->audio_hint(match, master, phones)) {
    for (const std::string& name : Engine::output_devices()) {
      std::string low = name;
      std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return std::tolower(ch); });
      if (low.find(match) != std::string::npos) {
        c.device_hint = match;
        c.master_channel = master;
        c.phones_channel = phones;
      }
    }
  }
  std::string error;
  const bool ok = engine_.start(c, error);
  std::lock_guard<std::mutex> lock(state_mutex_);
  audio_choice_ = c.device_hint;
  if (ok) {
    audio_label_ = fmt("%s  %d ch  %.1f kHz", engine_.device_name().c_str(), engine_.output_channels(),
                       engine_.sample_rate() / 1000.0);
  } else {
    audio_label_ = T("app.no_audio");
    status_ = error;
    DH_LOG(Error, "%s", error.c_str());
  }
}

void App::control_main() {
  double last_audio_check = now_seconds(), last_tick = last_audio_check;
  struct Played {
    std::int64_t track_id = 0;
    double seconds = 0, saved = 0;
    bool logged = false;
  } played[kMaxDecks];
  while (!quit_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EngineEvent e;
    while (engine_.poll(e)) {
      if (e.track_id == 0) continue;
      Job j;
      j.track_id = e.track_id;
      j.slot = e.slot;
      if (e.type == EventType::HotCueSet) {
        j.kind = Job::SaveHotCue;
        j.ms = e.frame / engine_.deck(e.deck).sample_rate * 1000.0;
        post(j);
      } else if (e.type == EventType::HotCueCleared) {
        j.kind = Job::ClearHotCue;
        post(j);
      }
    }
    DeckView views[kMaxDecks];
    for (int i = 0; i < kMaxDecks; ++i) views[i] = engine_.deck(i);
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
      const MixerView mixer = engine_.mixer();
      SamplerView samplers[kSamplerSlots];
      for (int i = 0; i < kSamplerSlots; ++i) samplers[i] = engine_.sampler(i);
      hub_->tick(views, kMaxDecks, &mixer, samplers, kSamplerSlots, now);
    }
    engine_.collect_garbage();

    // Follow controllers being plugged in or removed: use their audio interface when present.
    if (audio_recheck_.exchange(false) || now - last_audio_check > 2.0) {
      last_audio_check = now;
      std::string match, want;
      int master = 0, phones = 2;
      if (hub_->audio_hint(match, master, phones)) {
        for (const std::string& name : Engine::output_devices()) {
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
      if (want != have || !engine_.running()) start_audio();
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
      engine_.record_close();  // waits for the writer to finalise the file
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
      engine_.sampler_load(j.slot, std::move(sample));
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

void App::load_track(Library& lib, int deck, std::int64_t track_id) {
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
  engine_.load(deck, std::move(loaded));
  {  // loudness trim from analysis, kept within a sane range
    Command g;
    g.op = Op::AutoGain;
    g.deck = static_cast<std::uint8_t>(deck);
    g.f = static_cast<float>(std::clamp(gain.value_or(0.0), -12.0, 12.0));
    engine_.send(g);
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
  const RecorderStatus st = engine_.recorder_status();
  if (start) {
    if (st.recording) {
      error = "already recording";
      path = record_path_;
      return false;
    }
    if (st.open) engine_.record_close();
    const std::string dir = data_dir_ + "/Recordings";
    std::filesystem::create_directories(dir);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char name[64];
    std::strftime(name, sizeof name, "DeckHand %Y-%m-%d %H.%M.%S.wav", &tm);
    record_path_ = dir + "/" + name;
    if (!engine_.record_open(record_path_, false, error)) return false;
    path = record_path_;
    set_status(T("status.recording", {{"path", record_path_}}));
    return true;
  }
  if (!st.recording) {
    error = "not recording";
    return false;
  }
  engine_.record_stop();
  path = record_path_;
  Job j;
  j.kind = Job::RecFinish;
  j.path = record_path_;
  post(j);
  return true;
}

void App::request_load(int deck, std::int64_t track_id) {
  if (deck < 0 || deck >= kMaxDecks || track_id == 0) return;
  if (engine_.deck(deck).playing) {  // never replace a track that is playing
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
  in_.x = x / scale_;
  in_.y = y / scale_;
  if (kind == DH_PTR_DOWN) {
    in_.down = in_.pressed = true;
    in_.clicks = clicks;
  } else if (kind == DH_PTR_UP || kind == DH_PTR_CANCEL) {
    in_.down = false;
    in_.released = true;
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
  std::int64_t load_id = 0;
  int load_deck = -1;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    const bool have = sel_ >= 0 && sel_ < static_cast<int>(rows_.size());
    if (rename_id_ != 0) {  // the name field takes the keyboard
      if (key == DH_KEY_ENTER) commit_rename();
      else if (key == DH_KEY_ESCAPE) rename_id_ = 0;
      else if (key == DH_KEY_BACKSPACE) utf8_pop(rename_buf_);
      return;
    }
    add_menu_ = false;
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
          load_deck = engine_.deck(0).playing ? 1 : 0;
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
  std::lock_guard<std::mutex> lock(browse_mutex_);
  if (rename_id_ != 0) {
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
  Job j;
  j.kind = Job::AddFile;
  j.path = path;
  // Dropped on the upper (deck) area: load it there, left half = deck A.
  const float px = x / scale_, py = y / scale_;
  if (x >= 0 && y >= 0 && py < 430) j.deck = px < (width_px_ / scale_) / 2 ? 0 : 1;
  if (j.deck >= 0 && engine_.deck(j.deck).playing) j.deck = -1;
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

// ---- drawing ----------------------------------------------------------------------------
bool App::button(const Rect& r, const std::string& label, bool lit, Color lit_color) {
  const bool hover = r.contains(in_.x, in_.y);
  dl_.rect(r.x, r.y, r.w, r.h, lit ? lit_color : (hover ? kPanelHi : kPanel));
  dl_.outline(r.x, r.y, r.w, r.h, lit ? lit_color : kLine);
  dl_.text(r.x + r.w / 2, r.y + (r.h - 15) / 2, label, 11, lit ? kBg : kText, Align::Center, r.w - 4, 1);
  return clicked(r);
}

void App::meter(const Rect& r, float level) {
  dl_.rect(r.x, r.y, r.w, r.h, rgba(10, 11, 14));
  // Peak in dB mapped over a 48 dB range.
  const float db = 20.0f * std::log10(std::max(level, 1e-5f));
  const float t = std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f);
  const float h = r.h * t;
  const Color c = level > 0.98f ? kWarn : (t > 0.85f ? rgba(250, 210, 70) : kGood);
  dl_.rect(r.x, r.y + r.h - h, r.w, h, c);
}

void App::knob_bar(float x, float y, float w, const char* label, float value, bool centred) {
  dl_.text(x, y - 2, label, 9, kDim);
  const float bx = x + 30, bw = w - 30;
  dl_.rect(bx, y + 3, bw, 4, rgba(10, 11, 14));
  if (centred) {
    const float mid = bx + bw / 2, px = bx + bw * value;
    dl_.rect(std::min(mid, px), y + 3, std::fabs(px - mid), 4, kText);
    dl_.rect(mid - 0.5f, y + 2, 1, 6, kDim);
  } else {
    dl_.rect(bx, y + 3, bw * value, 4, kText);
  }
}

void App::draw_top(const Rect& r) {
  dl_.rect(r.x, r.y, r.w, r.h, kPanel);
  dl_.text(r.x + 12, r.y + 6, T("app.title"), 14, kText, Align::Left, 0, 1);
  std::string status, audio;
  bool importing, exporting;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status = status_;
    audio = audio_label_;
    importing = importing_;
    exporting = exporting_;
  }
  dl_.text(r.x + 110, r.y + 8, status, 12, kDim, Align::Left, r.w * 0.4f);
  const auto controllers = hub_->connected();
  std::string right = controllers.empty() ? T("app.no_controller") : controllers.front();
  const float aw = dl_.text(r.x + r.w - 12, r.y + 8, audio, 12, kDim, Align::Right);
  const float cw = dl_.text(r.x + r.w - 30 - aw, r.y + 8, right, 12, controllers.empty() ? kDim : kGood, Align::Right);
  top_buttons_right_ = r.x + r.w - 60 - aw - cw;
  {  // language: click to step through the installed packs
    const auto langs = i18n_.available();
    const Rect b{top_buttons_right_ - 40, r.y + 4, 40, r.h - 8};
    if (langs.size() > 1 && button(b, i18n_.language(), false, kAccent)) {
      std::size_t at = 0;
      for (std::size_t i = 0; i < langs.size(); ++i)
        if (langs[i].tag == i18n_.language()) at = i;
      const std::string next = langs[(at + 1) % langs.size()].tag;
      if (i18n_.set_language(next)) {
        lib_->set_setting("ui.language", next);
        rows_dirty_ = true;
      }
    }
    if (langs.size() > 1) top_buttons_right_ -= 46;
  }
  {
    const RecorderStatus rec = engine_.recorder_status();
    const Rect b{top_buttons_right_ - 86, r.y + 4, 86, r.h - 8};
    const std::string label = rec.recording ? T("app.rec") + " " + clock_text(rec.seconds) : T("app.rec");
    if (button(b, label, rec.recording, kWarn)) {
      std::string path, error;
      if (!record(!rec.recording, path, error)) status_note_ = T("status.error", {{"message", error}});
    }
    top_buttons_right_ -= 92;
  }
  if (!exporting) {
    const Rect b{top_buttons_right_ - 96, r.y + 4, 96, r.h - 8};
    if (button(b, T("app.export_usb"), export_menu_, kAccent)) {
      export_menu_ = !export_menu_;
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
  }
  if (import_v6_available() && !importing && std::filesystem::exists(source_db())) {
    const Rect b{top_buttons_right_ - 96 - 116, r.y + 4, 110, r.h - 8};
    if (button(b, T("app.import_library"), false, kAccent)) {
      Job j;
      j.kind = Job::Import;
      post(j);
    }
  }
}

void App::draw_export_menu(float right, float top) {
  if (!export_menu_) return;
  std::string what = T("export.all_playlists");
  std::int64_t list = 0;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (view_ == TreeItem::Playlist && tree_sel_ >= 0 && tree_sel_ < static_cast<int>(tree_.size())) {
      list = playlist_id_;
      what = "\"" + tree_[static_cast<std::size_t>(tree_sel_)].name + "\"";
    }
  }
  const float row_h = 24, w = 420;
  const Rect m{right - w, top, w, 30 + row_h * static_cast<float>(export_targets_.size()) + 6};
  dl_.rect(m.x, m.y, m.w, m.h, kPanelHi);
  dl_.outline(m.x, m.y, m.w, m.h, kAccent);
  dl_.text(m.x + 10, m.y + 8, T("export.title", {{"what", what}}), 12, kText, Align::Left, m.w - 20, 1);
  for (std::size_t i = 0; i < export_targets_.size(); ++i) {
    const Rect row{m.x, m.y + 30 + static_cast<float>(i) * row_h, m.w, row_h};
    if (row.contains(in_.x, in_.y)) dl_.rect(row.x, row.y, row.w, row.h, with_alpha(kAccent, 90));
    dl_.text(row.x + 10, row.y + 4, export_targets_[i], 12, kText, Align::Left, m.w - 20);
    if (clicked(row)) {
      Job j;
      j.kind = Job::Export;
      j.path = export_targets_[i];
      j.track_id = list;
      post(j);
      export_menu_ = false;
    }
  }
  if (in_.pressed && !m.contains(in_.x, in_.y) && in_.y > top) export_menu_ = false;
}

void App::draw_wave(int di, const Rect& r, const DeckView& v, const DeckInfo& info) {
  dl_.rect(r.x, r.y, r.w, r.h, rgba(12, 13, 16));
  const Color base = kDeck[di];
  dl_.rect(r.x, r.y, 3, r.h, base);
  if (!info.audio || !v.loaded) {
    const std::string msg = info.loading ? T("deck.loading") : (!info.error.empty() ? info.error : T("deck.empty", {{"deck", deck_letter(di)}}));
    dl_.text(r.x + r.w / 2, r.y + r.h / 2 - 8, msg, 13, info.error.empty() ? kDim : kWarn, Align::Center, r.w - 40);
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
    dl_.rect(x0, r.y, x1 - x0, r.h, with_alpha(kAccent, v.loop_active ? 70 : 30));
  }
  const double t0 = pos - window / 2, t1 = pos + window / 2;
  // Waveform: one column per analysis point, three bands layered.
  const double per_point = 1.0 / a.wave_rate;
  const float col = static_cast<float>(per_point * pt_per_sec) + 0.5f;
  const long i0 = std::max(0L, static_cast<long>(t0 * a.wave_rate)),
             i1 = std::min(static_cast<long>(a.wave.size()), static_cast<long>(t1 * a.wave_rate) + 2);
  const Color c_low = mix(base, kBg, 0.55f), c_high = mix(base, rgba(255, 255, 255), 0.7f);
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
      dl_.rect(x_of(t), r.y, 1, r.h, with_alpha(kText, bar ? 120 : 45));
    }
  }
  // Cue point and hot cues.
  const float xc = x_of(v.cue / sr);
  dl_.triangle(xc - 5, r.y, xc + 5, r.y, xc, r.y + 8, kText);
  for (int h = 0; h < kHotCues; ++h) {
    if (v.hot_cue[h] < 0) continue;
    const float x = x_of(v.hot_cue[h] / sr);
    dl_.rect(x, r.y, 1.5f, r.h, kHot[h]);
    dl_.rect(x, r.y, 14, 13, kHot[h]);
    dl_.text(x + 7, r.y, std::string(1, static_cast<char>('1' + h)), 10, kBg, Align::Center, 0, 1);
  }
  dl_.pop_clip();
  dl_.rect(cx - 1, r.y, 2, r.h, rgba(255, 255, 255));  // playhead
}

void App::draw_deck(int di, const Rect& r, const DeckView& v, const DeckInfo& info) {
  dl_.rect(r.x, r.y, r.w, r.h, kPanel);
  const Color base = kDeck[di];
  dl_.rect(r.x, r.y, r.w, 2, base);
  const float pad = 12, right_w = 150;
  dl_.text(r.x + pad, r.y + 10, std::string(1, static_cast<char>('A' + di)), 22, base, Align::Left, 0, 1);
  const float tx = r.x + pad + 26, tw = r.w - pad * 2 - 26 - right_w;
  if (v.loaded) {
    dl_.text(tx, r.y + 9, info.row.title, 15, kText, Align::Left, tw, 1);
    dl_.text(tx, r.y + 31, info.row.artist, 12, kDim, Align::Left, tw);
  } else {
    dl_.text(tx, r.y + 18, info.loading ? T("deck.loading") : T("deck.no_track"), 13, kDim, Align::Left, tw);
  }
  // Tempo read-outs.
  const float rx = r.x + r.w - pad;
  dl_.text(rx, r.y + 6, v.bpm > 0 ? fmt("%.1f", v.bpm) : "--.-", 24, kText, Align::Right, 0, 1);
  dl_.text(rx, r.y + 36, fmt("%+.2f%%   \xC2\xB1%d%%", v.tempo * 100.0, static_cast<int>(std::lround(v.tempo_range * 100))), 11,
           kDim, Align::Right);
  const double seconds = v.loaded ? v.position / v.sample_rate : 0, total = v.loaded ? v.length / v.sample_rate : 0;
  dl_.text(rx, r.y + 52, clock_text(seconds) + "  " + clock_text(seconds - total), 12, kText, Align::Right);
  if (v.loaded && !info.row.key.empty()) dl_.text(rx - 120, r.y + 14, info.row.key, 12, kDim, Align::Right);

  // Transport buttons. CUE acts on press and release, like the hardware button.
  float bx = r.x + pad;
  const float by = r.y + 72, bh = 26;
  const int id_base = (di + 1) * 100;
  const auto cmd = [&](Op op, int i = 0) {
    Command c;
    c.op = op;
    c.deck = static_cast<std::uint8_t>(di);
    c.i = i;
    engine_.send(c);
  };
  if (button({bx, by, 54, bh}, T("deck.cue"), v.loaded && (v.at_cue || held_ == id_base + 1), base)) {
    cmd(Op::CueDown);
    held_ = id_base + 1;
  }
  bx += 60;
  if (button({bx, by, 54, bh}, v.playing ? T("deck.pause") : T("deck.play"), v.playing, base)) cmd(Op::PlayToggle);
  bx += 60;
  if (button({bx, by, 54, bh}, T("deck.sync"), v.sync, kAccent)) cmd(Op::SyncToggle);
  bx += 68;
  for (int h = 0; h < kHotCues; ++h) {
    const Rect pr{bx, by, 22, bh};
    const bool set = v.hot_cue[h] >= 0;
    dl_.rect(pr.x, pr.y, pr.w, pr.h, set ? kHot[h] : kPanelHi);
    dl_.text(pr.x + pr.w / 2, pr.y + 6, std::string(1, static_cast<char>('1' + h)), 11, set ? kBg : kDim, Align::Center, 0, 1);
    if (clicked(pr)) {
      cmd(Op::HotCueDown, h);
      held_ = id_base + 10 + h;
    }
    bx += 25;
  }
  {  // key lock, quantise, slip
    const float tw3 = std::max(30.0f, std::min(44.0f, (r.x + r.w - pad - bx - 6) / 3 - 3));
    bx += 6;
    if (button({bx, by, tw3, bh}, T("deck.key_lock"), v.key_lock, kAccent)) cmd(Op::KeyLock, -1);
    bx += tw3 + 3;
    if (button({bx, by, tw3, bh}, T("deck.quantize"), v.quantize, kAccent)) cmd(Op::Quantize, -1);
    bx += tw3 + 3;
    if (button({bx, by, tw3, bh}, T("deck.slip"), v.slip, kAccent)) cmd(Op::Slip, -1);
  }
  bx = tx;
  if (v.loop_active) {
    const double beats = (v.loop_out - v.loop_in) / (60.0 / std::max(1.0, v.bpm / (1.0 + v.tempo)) * v.sample_rate);
    dl_.text(bx, r.y + 50, beats >= 1 ? T("deck.loop", {{"beats", std::lround(beats)}}) : T("deck.loop_fraction", {{"divisor", std::lround(1.0 / beats)}}), 11, kAccent, Align::Left, 0, 1);
  }
  if (in_.released && held_ / 100 == di + 1) {
    const int which = held_ % 100;
    if (which == 1) cmd(Op::CueUp);
    else if (which >= 10 && which < 20) cmd(Op::HotCueUp, which - 10);
    held_ = 0;
  }

  // Whole-track overview; click or drag to seek.
  const Rect ov{r.x + pad, r.y + 108, r.w - pad * 2, r.h - 118 - 30};
  dl_.rect(ov.x, ov.y, ov.w, ov.h, rgba(12, 13, 16));
  if (v.loaded && !info.overview.empty() && v.length > 0) {
    const float played = static_cast<float>(v.position / v.length) * ov.w;
    const std::size_t n = info.overview.size();
    const float cw = ov.w / static_cast<float>(n);
    for (std::size_t i = 0; i < n; ++i) {
      const WavePoint& p = info.overview[i];
      const float x = ov.x + static_cast<float>(i) * cw;
      const float h = std::max(1.0f, ov.h * std::max(p.low, std::max(p.mid, p.high)) / 255.0f);
      dl_.rect(x, ov.y + (ov.h - h) / 2, cw + 0.5f, h, x - ov.x < played ? mix(base, kBg, 0.6f) : base);
    }
    for (int h = 0; h < kHotCues; ++h) {
      if (v.hot_cue[h] >= 0) dl_.rect(ov.x + static_cast<float>(v.hot_cue[h] / v.length) * ov.w, ov.y, 2, ov.h, kHot[h]);
    }
    dl_.rect(ov.x + played - 1, ov.y - 2, 2, ov.h + 4, rgba(255, 255, 255));
    if (clicked(ov)) drag_deck_ = di;
    if (drag_deck_ == di && in_.down) {
      Command c;
      c.op = Op::Seek;
      c.deck = static_cast<std::uint8_t>(di);
      c.d = std::clamp((in_.x - ov.x) / ov.w, 0.0f, 1.0f) * v.length;
      engine_.send(c);
    }
  }
  if (!in_.down && drag_deck_ == di) drag_deck_ = -1;

  // Effect slots: click the name to step through the effects, ON to engage, drag the bar for level.
  const float fy = r.y + r.h - 30, fw = (r.w - pad * 2 + 6) / kFxSlots;
  for (int slot = 0; slot < kFxSlots; ++slot) {
    const FxSlotView& f = v.fx[slot];
    const float fx0 = r.x + pad + fw * static_cast<float>(slot);
    const EffectInfo* fi = effect_info(f.kind);
    const float name_w = fw * 0.5f - 4, on_w = 30, bar_w = fw - name_w - on_w - 16;
    Command c;
    c.deck = static_cast<std::uint8_t>(di);
    c.i = slot;
    if (button({fx0, fy, name_w, 22}, fi ? fi->name : "FX " + std::to_string(slot + 1), false, kAccent)) {
      int next = static_cast<int>(f.kind);
      do {
        next = (next + 1) % static_cast<int>(EffectKind::Count);
      } while (next != 0 && effect_info(static_cast<EffectKind>(next)) && effect_info(static_cast<EffectKind>(next))->colour);
      c.op = Op::FxSelect;
      c.d = next;
      engine_.send(c);
    }
    if (button({fx0 + name_w + 3, fy, on_w, 22}, T("fx.on"), f.on, base) && fi) {
      c.op = Op::FxEnable;
      c.f = f.on ? 0.0f : 1.0f;
      engine_.send(c);
    }
    const Rect bar{fx0 + name_w + on_w + 9, fy + 9, bar_w, 4};
    dl_.rect(bar.x, bar.y, bar.w, bar.h, rgba(10, 11, 14));
    dl_.rect(bar.x, bar.y, bar.w * f.wet, bar.h, fi ? kText : kLine);
    const int id = id_base + 30 + slot;
    if (clicked({bar.x - 4, fy, bar.w + 8, 22})) held_ = id;
    if (held_ == id && in_.down) {
      c.op = Op::FxWet;
      c.f = std::clamp((in_.x - bar.x) / bar.w, 0.0f, 1.0f);
      engine_.send(c);
    }
    if (held_ == id && !in_.down) held_ = 0;
  }
}

void App::draw_mixer(const Rect& r, const DeckView* v, const MixerView& m) {
  dl_.rect(r.x, r.y, r.w, r.h, kPanel);
  dl_.rect(r.x, r.y, r.w, 2, kLine);
  const float strip = (r.w - 40) / 2;
  for (int di = 0; di < kUiDecks; ++di) {
    const float x = di == 0 ? r.x + 10 : r.x + r.w - 10 - strip;
    const DeckView& d = v[di];
    float y = r.y + 12;
    knob_bar(x, y, strip - 34, T("mixer.trim").c_str(), d.trim, true);
    knob_bar(x, y += 14, strip - 34, T("mixer.eq.high").c_str(), d.eq_high, true);
    knob_bar(x, y += 14, strip - 34, T("mixer.eq.mid").c_str(), d.eq_mid, true);
    knob_bar(x, y += 14, strip - 34, T("mixer.eq.low").c_str(), d.eq_low, true);
    knob_bar(x, y += 14, strip - 34, T("mixer.filter").c_str(), d.filter, true);
    // Channel fader (draggable) and level meter.
    const Rect f{x + strip - 26, r.y + 10, 10, 84};
    dl_.rect(f.x + 4, f.y, 2, f.h, rgba(10, 11, 14));
    dl_.rect(f.x - 2, f.y + f.h * (1 - d.fader) - 3, 14, 6, kText);
    const Rect hit{f.x - 6, f.y - 4, 22, f.h + 8};
    if (clicked(hit)) held_ = 900 + di;
    if (held_ == 900 + di && in_.down) {
      Command c;
      c.op = Op::Fader;
      c.deck = static_cast<std::uint8_t>(di);
      c.f = std::clamp(1.0f - (in_.y - f.y) / f.h, 0.0f, 1.0f);
      engine_.send(c);
    }
    meter({x + strip - 10, r.y + 10, 6, 84}, d.level);
    const Rect pfl{x, r.y + 84, 44, 16};
    if (button(pfl, T("mixer.cue"), d.pfl, kDeck[di])) {
      Command c;
      c.op = Op::Pfl;
      c.deck = static_cast<std::uint8_t>(di);
      c.i = -1;
      engine_.send(c);
    }
  }
  // Master meters in the middle.
  meter({r.x + r.w / 2 - 8, r.y + 10, 6, 84}, m.peak_left);
  meter({r.x + r.w / 2 + 2, r.y + 10, 6, 84}, m.peak_right);
  // Crossfader (draggable).
  const Rect xf{r.x + 20, r.y + r.h - 30, r.w - 40, 14};
  dl_.rect(xf.x, xf.y + 6, xf.w, 2, rgba(10, 11, 14));
  dl_.rect(xf.x + xf.w / 2 - 0.5f, xf.y + 2, 1, 10, kDim);
  dl_.rect(xf.x + xf.w * m.crossfader - 5, xf.y, 10, 14, kText);
  const Rect hit{xf.x - 6, xf.y - 5, xf.w + 12, xf.h + 10};
  if (clicked(hit)) held_ = 950;
  if (held_ == 950 && in_.down) {
    Command c;
    c.op = Op::Crossfader;
    c.f = std::clamp((in_.x - xf.x) / xf.w, 0.0f, 1.0f);
    engine_.send(c);
  }
  if (in_.released && held_ >= 900) held_ = 0;
}

void App::draw_browser(const Rect& r, const DeckView* v) {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  dl_.rect(r.x, r.y, r.w, r.h, kBg);
  const float tree_w = 230, row_h = 24, head_h = 30;

  // ---- playlist tree -------------------------------------------------------------
  const float bar_h = 28;
  const Rect tree{r.x, r.y, tree_w, r.h - bar_h};
  dl_.rect(tree.x, tree.y, tree.w, r.h, kPanel);
  const float tree_content = static_cast<float>(tree_.size()) * row_h + 12;
  if (tree.contains(in_.x, in_.y)) tree_scroll_ -= in_.scroll;
  tree_scroll_ = std::clamp(tree_scroll_, 0.0f, std::max(0.0f, tree_content - tree.h));
  dl_.push_clip(tree.x, tree.y, tree.w, tree.h);
  float y = tree.y + 6 - tree_scroll_;
  for (int i = 0; i < static_cast<int>(tree_.size()); ++i) {
    const TreeItem& item = tree_[static_cast<std::size_t>(i)];
    const Rect row{tree.x, y, tree.w, row_h};
    y += row_h;
    if (row.y + row.h < tree.y || row.y > tree.y + tree.h) continue;
    if (i == tree_sel_) dl_.rect(row.x, row.y, row.w, row.h, with_alpha(kAccent, focus_tree_ ? 150 : 70));
    const bool container = item.kind == TreeItem::Folder || item.kind == TreeItem::HistoryHeader;
    const float indent = 12 + 14.0f * static_cast<float>(item.depth) + (item.kind == TreeItem::Collection ? 0 : 6);
    if (rename_id_ != 0 && item.id == rename_id_ && (item.kind == TreeItem::Playlist || item.kind == TreeItem::Folder)) {
      dl_.rect(row.x + indent - 4, row.y + 2, tree.w - indent - 4, row.h - 4, rgba(10, 11, 14));
      dl_.outline(row.x + indent - 4, row.y + 2, tree.w - indent - 4, row.h - 4, kAccent);
      dl_.text(row.x + indent, row.y + 4, rename_buf_ + "|", 12, kText, Align::Left, tree.w - indent - 12);
    } else {
      dl_.text(row.x + indent, row.y + 4, item.name, 12, container ? kDim : kText, Align::Left, tree.w - indent - 44,
               item.kind == TreeItem::Collection || container ? 1 : 0);
      if (!container) dl_.text(row.x + tree.w - 10, row.y + 5, std::to_string(item.count), 10, kDim, Align::Right);
    }
    if (clicked(row) && tree.contains(in_.x, in_.y)) {
      if (rename_id_ != 0 && item.id != rename_id_) commit_rename();
      tree_sel_ = i;
      focus_tree_ = false;
      apply_tree_selection();
      if (in_.clicks >= 2 && (item.kind == TreeItem::Playlist || item.kind == TreeItem::Folder)) {
        rename_id_ = item.id;
        rename_buf_ = item.name;
      }
    }
  }
  dl_.pop_clip();
  {  // playlist actions
    const TreeItem* cur = tree_sel_ >= 0 && tree_sel_ < static_cast<int>(tree_.size()) ? &tree_[static_cast<std::size_t>(tree_sel_)] : nullptr;
    const bool editable = cur && (cur->kind == TreeItem::Playlist || cur->kind == TreeItem::Folder);
    const std::int64_t parent = cur && cur->kind == TreeItem::Folder ? cur->id : 0;
    const float by = r.y + r.h - bar_h + 3, bw = (tree_w - 10) / 4 - 2;
    float bx = r.x + 5;
    dl_.rect(r.x, r.y + r.h - bar_h, tree_w, 1, kLine);
    if (button({bx, by, bw, bar_h - 6}, T("playlist.add_list"), false, kAccent)) {
      rename_id_ = lib_->create_playlist(parent, T("playlist.new_list"), false);
      rename_buf_ = T("playlist.new_list");
      view_ = TreeItem::Playlist;
      playlist_id_ = rename_id_;
      rows_dirty_ = true;
    }
    bx += bw + 2;
    if (button({bx, by, bw, bar_h - 6}, T("playlist.add_folder"), false, kAccent)) {
      rename_id_ = lib_->create_playlist(parent, T("playlist.new_folder"), true);
      rename_buf_ = T("playlist.new_folder");
      rows_dirty_ = true;
    }
    bx += bw + 2;
    if (button({bx, by, bw, bar_h - 6}, T("playlist.rename"), false, kAccent) && editable) {
      rename_id_ = cur->id;
      rename_buf_ = cur->name;
    }
    bx += bw + 2;
    const bool sure = editable && confirm_delete_ == cur->id;
    if (button({bx, by, bw, bar_h - 6}, sure ? T("playlist.confirm") : T("playlist.delete"), sure, kWarn) && editable) {
      if (sure) {
        lib_->delete_playlist(cur->id);
        confirm_delete_ = 0;
        view_ = TreeItem::Collection;
        playlist_id_ = 0;
        rows_dirty_ = true;
      } else {
        confirm_delete_ = cur->id;
      }
    }
  }

  // ---- header: search and load buttons ----------------------------------------------
  const Rect table{r.x + tree_w + 1, r.y, r.w - tree_w - 1, r.h};
  dl_.rect(table.x, table.y, table.w, head_h, kPanel);
  const Rect search{table.x + 8, table.y + 4, 260, head_h - 8};
  dl_.rect(search.x, search.y, search.w, search.h, rgba(10, 11, 14));
  dl_.outline(search.x, search.y, search.w, search.h, search_focus_ ? kAccent : kLine);
  dl_.text(search.x + 8, search.y + 3, search_.empty() ? T("browser.search") : search_, 12, search_.empty() ? kDim : kText,
           Align::Left, search.w - 16);
  if (in_.pressed) search_focus_ = search.contains(in_.x, in_.y);
  dl_.text(search.x + search.w + 12, table.y + 8, T("browser.tracks", {{"count", rows_.size()}}), 11, kDim);

  const bool have = sel_ >= 0 && sel_ < static_cast<int>(rows_.size());
  int load_deck = -1;
  {  // playlist membership of the selected track
    const float bx = table.x + table.w - 8 - 76.0f * kUiDecks - 170;
    if (button({bx, table.y + 4, 80, head_h - 8}, T("browser.add_to"), add_menu_, kAccent) && have) add_menu_ = !add_menu_;
    else if (in_.pressed && add_menu_ && !add_menu_rect_.contains(in_.x, in_.y)) add_menu_ = false;
    if (view_is_editable_playlist() && button({bx + 84, table.y + 4, 76, head_h - 8}, T("browser.remove"), false, kWarn) && have) {
      lib_->remove_from_playlist(playlist_id_, sel_);
      rows_dirty_ = true;
    }
  }
  for (int di = 0; di < kUiDecks; ++di) {
    const Rect b{table.x + table.w - 8 - 76.0f * static_cast<float>(kUiDecks - di), table.y + 4, 70, head_h - 8};
    if (button(b, T("browser.load", {{"deck", deck_letter(di)}}), false, kDeck[di]) && have) load_deck = di;
  }

  // ---- column header ---------------------------------------------------------------------
  const float cy = table.y + head_h;
  const float w_bpm = 64, w_key = 56, w_time = 60, w_badge = 30;
  const float flex = table.w - w_bpm - w_key - w_time - w_badge - 24;
  const float x_title = table.x + w_badge, x_artist = x_title + flex * 0.55f, x_bpm = x_artist + flex * 0.45f;
  const float x_key = x_bpm + w_bpm, x_time = x_key + w_key;
  dl_.rect(table.x, cy, table.w, row_h, kPanelHi);
  {  // click a column to sort: ascending, descending, then back to the natural order
    struct Col { float x0, x1; const char* label; SortKey key; bool right; };
    const Col cols[] = {{x_title, x_artist, "browser.col.title", SortKey::Title, false}, {x_artist, x_bpm, "browser.col.artist", SortKey::Artist, false},
                        {x_bpm, x_key, "browser.col.bpm", SortKey::Bpm, true},           {x_key, x_time, "browser.col.key", SortKey::Key, false},
                        {x_time, x_time + w_time, "browser.col.time", SortKey::Duration, true}};
    for (const Col& c : cols) {
      const bool active = sort_ == c.key;
      const std::string label = T(c.label) + (active ? (sort_desc_ ? " â¾" : " â´") : "");
      if (c.right) dl_.text(c.x1 - 8, cy + 5, label, 10, active ? kText : kDim, Align::Right, 0, 1);
      else dl_.text(c.x0 + (c.key == SortKey::Key ? 6 : 0), cy + 5, label, 10, active ? kText : kDim, Align::Left, 0, 1);
      if (clicked({c.x0, cy, c.x1 - c.x0, row_h}) && view_ != TreeItem::HistorySession) {
        if (!active) { sort_ = c.key; sort_desc_ = false; }
        else if (!sort_desc_) sort_desc_ = true;
        else sort_ = SortKey::Default;
        rows_dirty_ = true;
      }
    }
  }

  // ---- rows ------------------------------------------------------------------------------
  const Rect body{table.x, cy + row_h, table.w, table.h - head_h - row_h};
  rows_view_h_ = body.h;
  const float content = static_cast<float>(rows_.size()) * row_h;
  if (body.contains(in_.x, in_.y)) scroll_ -= in_.scroll;
  if (follow_sel_) {
    const float top = static_cast<float>(sel_) * row_h;
    if (top < scroll_) scroll_ = top;
    if (top + row_h > scroll_ + body.h) scroll_ = top + row_h - body.h;
    follow_sel_ = false;
  }
  scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content - body.h));

  dl_.push_clip(body.x, body.y, body.w, body.h);
  const int first = static_cast<int>(scroll_ / row_h);
  const int last = std::min(static_cast<int>(rows_.size()), first + static_cast<int>(body.h / row_h) + 2);
  for (int i = first; i < last; ++i) {
    const TrackRow& t = rows_[static_cast<std::size_t>(i)];
    const Rect row{body.x, body.y + static_cast<float>(i) * row_h - scroll_, body.w, row_h};
    if (i == sel_) dl_.rect(row.x, row.y, row.w, row.h, with_alpha(kAccent, focus_tree_ ? 70 : 150));
    else if (i % 2) dl_.rect(row.x, row.y, row.w, row.h, rgba(21, 23, 28));
    for (int di = 0; di < kUiDecks; ++di) {
      if (v[di].loaded && v[di].track_id == t.id)
        dl_.text(row.x + 6 + 12.0f * static_cast<float>(di), row.y + 5, std::string(1, static_cast<char>('A' + di)), 11, kDeck[di],
                 Align::Left, 0, 1);
    }
    dl_.text(x_title, row.y + 4, t.title, 12, kText, Align::Left, x_artist - x_title - 12);
    dl_.text(x_artist, row.y + 4, t.artist, 12, kDim, Align::Left, x_bpm - x_artist - 12);
    if (t.bpm > 0) dl_.text(x_bpm + w_bpm - 10, row.y + 4, fmt("%.1f", t.bpm), 12, kText, Align::Right);
    dl_.text(x_key + 6, row.y + 4, t.key, 12, kDim, Align::Left, w_key - 8);
    if (t.duration_ms > 0) dl_.text(x_time + w_time - 6, row.y + 4, clock_text(t.duration_ms / 1000.0), 12, kDim, Align::Right);
    if (in_.pressed && !add_menu_ && row.contains(in_.x, in_.y) && body.contains(in_.x, in_.y)) {
      sel_ = i;
      focus_tree_ = false;
      if (in_.clicks >= 2) load_deck = v[0].playing ? 1 : 0;
    }
  }
  if (rows_.empty()) {
    dl_.text(body.x + body.w / 2, body.y + 40, search_.empty() ? T("browser.empty") : T("browser.no_matches"),
             13, kDim, Align::Center);
  }
  dl_.pop_clip();
  if (content > body.h) {  // scroll bar
    const float bar_h = std::max(24.0f, body.h * body.h / content);
    const float bar_y = body.y + (body.h - bar_h) * scroll_ / (content - body.h);
    dl_.rect(body.x + body.w - 5, bar_y, 4, bar_h, kLine);
  }
  if (add_menu_ && have) {  // pop-up list of playlists, drawn last so it sits on top
    std::vector<const TreeItem*> lists;
    for (const TreeItem& item : tree_)
      if (item.kind == TreeItem::Playlist) lists.push_back(&item);
    const float mw = 240, mh = std::min(body.h - 8, static_cast<float>(std::max<std::size_t>(1, lists.size())) * row_h + 8);
    add_menu_rect_ = {table.x + table.w - 8 - 76.0f * kUiDecks - 170, table.y + head_h, mw, mh};
    const Rect& m = add_menu_rect_;
    dl_.rect(m.x, m.y, m.w, m.h, kPanelHi);
    dl_.outline(m.x, m.y, m.w, m.h, kAccent);
    dl_.push_clip(m.x, m.y, m.w, m.h);
    if (lists.empty()) dl_.text(m.x + 10, m.y + 8, T("playlist.none"), 12, kDim);
    for (std::size_t i = 0; i < lists.size(); ++i) {
      const Rect row{m.x, m.y + 4 + static_cast<float>(i) * row_h, m.w, row_h};
      if (row.contains(in_.x, in_.y)) dl_.rect(row.x, row.y, row.w, row.h, with_alpha(kAccent, 90));
      dl_.text(row.x + 10, row.y + 4, lists[i]->name, 12, kText, Align::Left, m.w - 20);
      if (clicked(row)) {
        lib_->add_to_playlist(lists[i]->id, rows_[static_cast<std::size_t>(sel_)].id);
        add_menu_ = false;
        rows_dirty_ = true;
        status_note_ = T("status.added_to", {{"name", lists[i]->name}});
      }
    }
    dl_.pop_clip();
  }
  if (load_deck >= 0 && sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) {
    const std::int64_t id = rows_[static_cast<std::size_t>(sel_)].id;
    // request_load does not touch browse state, so calling it under the lock is safe.
    request_load(load_deck, id);
  }
}

const dh_frame* App::frame() {
  if (!status_note_.empty()) {
    set_status(status_note_);
    status_note_.clear();
  }
  if (rows_dirty_.exchange(false)) refresh_rows();
  const float w = width_px_ / scale_, h = height_px_ / scale_;
  dl_.begin(width_px_, height_px_, scale_);
  dl_.rect(0, 0, w, h, kBg);

  DeckView views[kMaxDecks];
  for (int i = 0; i < kMaxDecks; ++i) views[i] = engine_.deck(i);
  const MixerView mixer = engine_.mixer();
  DeckInfo info[kUiDecks];
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (int i = 0; i < kUiDecks; ++i) info[i] = decks_[i];
  }

  const float top_h = 30, wave_h = 92, deck_h = 180, gap = 2, mixer_w = 260;
  draw_top({0, 0, w, top_h});
  float y = top_h + gap;
  for (int di = 0; di < kUiDecks; ++di) {
    draw_wave(di, {0, y, w, wave_h}, views[di], info[di]);
    y += wave_h + gap;
  }
  const float deck_w = (w - mixer_w - gap * 2) / 2;
  draw_deck(0, {0, y, deck_w, deck_h}, views[0], info[0]);
  draw_mixer({deck_w + gap, y, mixer_w, deck_h}, views, mixer);
  draw_deck(1, {deck_w + mixer_w + gap * 2, y, deck_w, deck_h}, views[1], info[1]);
  y += deck_h + gap;
  draw_browser({0, y, w, h - y}, views);
  draw_export_menu(top_buttons_right_, top_h);

  in_.pressed = in_.released = false;
  in_.clicks = 0;
  in_.scroll = 0;

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
  frame_.clear_rgba[0] = 16 / 255.0f;
  frame_.clear_rgba[1] = 18 / 255.0f;
  frame_.clear_rgba[2] = 22 / 255.0f;
  frame_.clear_rgba[3] = 1;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    frame_.wants_text_input = search_focus_ || rename_id_ != 0;
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
void dh_app_set_display(dh_app* app, float w, float h, float scale, float, float, float, float) {
  app->app.set_display(w, h, scale);
}
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
