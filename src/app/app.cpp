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
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "deckhand/app/capi.h"
#include "deckhand/audio/engine.hpp"
#include "deckhand/gfx/draw.hpp"
#include "deckhand/import/import.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/log/log.hpp"
#include "deckhand/midi/controller.hpp"

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
};

struct Job {
  enum Kind { Load, AddFile, Import, SaveHotCue, ClearHotCue } kind = Load;
  int deck = -1;
  std::int64_t track_id = 0;
  std::string path;
  int slot = 0;
  double ms = 0;
};

}  // namespace

class App final : public ControlSink {
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
  std::string source_db() const {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/Library/Pioneer/rekordbox/master.db" : std::string();
  }

  // ---- UI ------------------------------------------------------------------------
  void refresh_rows();
  void move_selection(int steps);
  void ensure_visible();
  bool clicked(const Rect& r) const { return in_.pressed && r.contains(in_.x, in_.y); }
  bool button(const Rect& r, const std::string& label, bool lit, Color lit_color);
  void draw_top(const Rect& r);
  void draw_wave(int di, const Rect& r, const DeckView& v, const DeckInfo& info);
  void draw_deck(int di, const Rect& r, const DeckView& v, const DeckInfo& info);
  void draw_mixer(const Rect& r, const DeckView* v, const MixerView& m);
  void draw_browser(const Rect& r, const DeckView* v);
  void meter(const Rect& r, float level);
  void knob_bar(float x, float y, float w, const char* label, float value, bool centred);

  std::string data_dir_, resource_dir_;
  std::unique_ptr<Library> lib_;
  Engine engine_;
  std::unique_ptr<ControllerHub> hub_;
  std::unique_ptr<MidiIO> midi_;
  gfx::DrawList dl_;

  std::atomic<bool> quit_{false};
  std::thread loader_, control_;
  std::mutex job_mutex_;
  std::condition_variable job_cv_;
  std::deque<Job> jobs_;

  std::mutex state_mutex_;  // guards the members below, shared with the worker threads
  DeckInfo decks_[kMaxDecks];
  std::string status_, audio_label_, audio_choice_;
  bool importing_ = false;
  std::atomic<bool> rows_dirty_{true};
  std::atomic<bool> audio_recheck_{false};

  std::mutex browse_mutex_;  // guards the browse state, shared with the controller thread
  std::vector<PlaylistNode> playlists_;
  std::vector<TrackRow> rows_;
  std::int64_t playlist_id_ = 0;
  int tree_sel_ = 0;  // 0 = whole collection, n = playlists_[n-1]
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
  float rows_view_h_ = 0;

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

  if (lib_->track_count() == 0 && import_v6_available() && std::filesystem::exists(source_db())) {
    Job j;
    j.kind = Job::Import;
    post(j);
  }
}

App::~App() {
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
    audio_label_ = "No audio output";
    status_ = error;
    DH_LOG(Error, "%s", error.c_str());
  }
}

void App::control_main() {
  double last_audio_check = now_seconds();
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
    hub_->tick(views, kMaxDecks, now);
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
      set_status(std::string("Error: ") + e.what());
      std::lock_guard<std::mutex> lock(state_mutex_);
      importing_ = false;
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
      set_status("Added " + t.title);
      if (j.deck >= 0) load_track(lib, j.deck, id);
      break;
    }
    case Job::Import: {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        importing_ = true;
        status_ = "Importing library...";
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
      set_status(fmt("Imported %d new and %d existing tracks, %d playlists", s.tracks_added, s.tracks_updated,
                     s.playlists));
      break;
    }
    case Job::SaveHotCue: lib.set_hot_cue(j.track_id, j.slot, j.ms); break;
    case Job::ClearHotCue: lib.clear_hot_cue(j.track_id, j.slot); break;
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
    decks_[deck].error = std::filesystem::exists(row->path) ? error : "File not found: " + row->path;
    DH_LOG(Warn, "deck %d: %s", deck + 1, decks_[deck].error.c_str());
    return;
  }
  const double sr = audio->sample_rate;
  auto loaded = std::make_unique<LoadedTrack>();
  loaded->audio = audio;
  loaded->track_id = track_id;
  loaded->bpm = row->bpm;
  std::vector<BeatSection> grid = lib.beatgrid(track_id);
  if (grid.empty()) {  // no analysis yet: estimate once and keep it
    double bpm = 0, first = 0;
    if (estimate_tempo(*audio, bpm, first)) {
      grid.push_back({first * 1000.0, bpm, 1});
      lib.replace_beatgrid(track_id, grid);
      lib.set_bpm(track_id, bpm);
      loaded->bpm = bpm;
      rows_dirty_ = true;
      DH_LOG(Info, "estimated tempo for %s: %.2f BPM", row->title.c_str(), bpm);
    }
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
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    decks_[deck] = std::move(info);
  }
  DH_LOG(Info, "deck %d loaded: %s - %s (%.1f s, %u Hz)", deck + 1, row->artist.c_str(), row->title.c_str(),
         static_cast<double>(audio->frames) / sr, audio->sample_rate);
}

void App::request_load(int deck, std::int64_t track_id) {
  if (deck < 0 || deck >= kMaxDecks || track_id == 0) return;
  if (engine_.deck(deck).playing) {  // never replace a track that is playing
    set_status(fmt("Deck %c is playing; stop it before loading", 'A' + deck));
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
    tree_sel_ = std::clamp(tree_sel_ + steps, 0, static_cast<int>(playlists_.size()));
    playlist_id_ = tree_sel_ == 0 ? 0 : playlists_[static_cast<std::size_t>(tree_sel_ - 1)].id;
    sel_ = 0;
    rows_dirty_ = true;
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
    switch (key) {
      case DH_KEY_UP: move_selection(-1); break;
      case DH_KEY_DOWN: move_selection(1); break;
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
  if (x >= 0 && y >= 0 && py < 400) j.deck = px < (width_px_ / scale_) / 2 ? 0 : 1;
  if (j.deck >= 0 && engine_.deck(j.deck).playing) j.deck = -1;
  post(j);
}

void App::refresh_rows() {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  playlists_ = lib_->playlists();
  tree_sel_ = std::clamp(tree_sel_, 0, static_cast<int>(playlists_.size()));
  rows_ = lib_->tracks(playlist_id_, search_);
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
  dl_.text(r.x + 12, r.y + 6, "DeckHand", 14, kText, Align::Left, 0, 1);
  std::string status, audio;
  bool importing;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status = status_;
    audio = audio_label_;
    importing = importing_;
  }
  dl_.text(r.x + 110, r.y + 8, status, 12, kDim, Align::Left, r.w * 0.4f);
  const auto controllers = hub_->connected();
  std::string right = controllers.empty() ? "No controller" : controllers.front();
  const float aw = dl_.text(r.x + r.w - 12, r.y + 8, audio, 12, kDim, Align::Right);
  const float cw = dl_.text(r.x + r.w - 30 - aw, r.y + 8, right, 12, controllers.empty() ? kDim : kGood, Align::Right);
  if (import_v6_available() && !importing && std::filesystem::exists(source_db())) {
    const Rect b{r.x + r.w - 60 - aw - cw - 110, r.y + 4, 110, r.h - 8};
    if (button(b, "Import library", false, kAccent)) {
      Job j;
      j.kind = Job::Import;
      post(j);
    }
  }
}

void App::draw_wave(int di, const Rect& r, const DeckView& v, const DeckInfo& info) {
  dl_.rect(r.x, r.y, r.w, r.h, rgba(12, 13, 16));
  const Color base = kDeck[di];
  dl_.rect(r.x, r.y, 3, r.h, base);
  if (!info.audio || !v.loaded) {
    const std::string msg = info.loading ? "Loading..." : (!info.error.empty() ? info.error : fmt("Deck %c is empty", 'A' + di));
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
    dl_.text(tx, r.y + 18, info.loading ? "Loading..." : "No track loaded", 13, kDim, Align::Left, tw);
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
  if (button({bx, by, 54, bh}, "CUE", v.loaded && (v.at_cue || held_ == id_base + 1), base)) {
    cmd(Op::CueDown);
    held_ = id_base + 1;
  }
  bx += 60;
  if (button({bx, by, 54, bh}, v.playing ? "PAUSE" : "PLAY", v.playing, base)) cmd(Op::PlayToggle);
  bx += 60;
  if (button({bx, by, 54, bh}, "SYNC", v.sync, kAccent)) cmd(Op::SyncToggle);
  bx += 68;
  for (int h = 0; h < kHotCues; ++h) {
    const Rect pr{bx, by, 24, bh};
    const bool set = v.hot_cue[h] >= 0;
    dl_.rect(pr.x, pr.y, pr.w, pr.h, set ? kHot[h] : kPanelHi);
    dl_.text(pr.x + pr.w / 2, pr.y + 6, std::string(1, static_cast<char>('1' + h)), 11, set ? kBg : kDim, Align::Center, 0, 1);
    if (clicked(pr)) {
      cmd(Op::HotCueDown, h);
      held_ = id_base + 10 + h;
    }
    bx += 28;
  }
  if (v.loop_active) {
    const double beats = (v.loop_out - v.loop_in) / (60.0 / std::max(1.0, v.bpm / (1.0 + v.tempo)) * v.sample_rate);
    dl_.text(bx + 8, by + 6, beats >= 1 ? fmt("LOOP %.0f", beats) : fmt("LOOP 1/%.0f", 1.0 / beats), 11, kAccent, Align::Left, 0, 1);
  }
  if (in_.released && held_ / 100 == di + 1) {
    const int which = held_ % 100;
    if (which == 1) cmd(Op::CueUp);
    else if (which >= 10) cmd(Op::HotCueUp, which - 10);
    held_ = 0;
  }

  // Whole-track overview; click or drag to seek.
  const Rect ov{r.x + pad, r.y + 108, r.w - pad * 2, r.h - 118};
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
}

void App::draw_mixer(const Rect& r, const DeckView* v, const MixerView& m) {
  dl_.rect(r.x, r.y, r.w, r.h, kPanel);
  dl_.rect(r.x, r.y, r.w, 2, kLine);
  const float strip = (r.w - 40) / 2;
  for (int di = 0; di < kUiDecks; ++di) {
    const float x = di == 0 ? r.x + 10 : r.x + r.w - 10 - strip;
    const DeckView& d = v[di];
    float y = r.y + 12;
    knob_bar(x, y, strip - 34, "TRIM", d.trim, true);
    knob_bar(x, y += 14, strip - 34, "HI", d.eq_high, true);
    knob_bar(x, y += 14, strip - 34, "MID", d.eq_mid, true);
    knob_bar(x, y += 14, strip - 34, "LOW", d.eq_low, true);
    knob_bar(x, y += 14, strip - 34, "FILT", d.filter, true);
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
    if (button(pfl, "CUE", d.pfl, kDeck[di])) {
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
  const Rect tree{r.x, r.y, tree_w, r.h};
  dl_.rect(tree.x, tree.y, tree.w, tree.h, kPanel);
  dl_.push_clip(tree.x, tree.y, tree.w, tree.h);
  float y = tree.y + 6;
  for (int i = 0; i <= static_cast<int>(playlists_.size()); ++i) {
    const bool all = i == 0;
    const PlaylistNode* node = all ? nullptr : &playlists_[static_cast<std::size_t>(i - 1)];
    const Rect row{tree.x, y, tree.w, row_h};
    if (i == tree_sel_) dl_.rect(row.x, row.y, row.w, row.h, with_alpha(kAccent, focus_tree_ ? 150 : 70));
    const float indent = all ? 12 : (node->parent ? 40 : 26);
    dl_.text(row.x + indent, row.y + 4, all ? "Collection" : node->name, 12, kText, Align::Left, tree.w - indent - 44,
             all || node->is_folder ? 1 : 0);
    if (!all && !node->is_folder)
      dl_.text(row.x + tree.w - 10, row.y + 5, std::to_string(node->track_count), 10, kDim, Align::Right);
    if (clicked(row) && (all || !node->is_folder)) {
      tree_sel_ = i;
      playlist_id_ = all ? 0 : node->id;
      sel_ = 0;
      scroll_ = 0;
      rows_dirty_ = true;
      focus_tree_ = false;
    }
    y += row_h;
  }
  dl_.pop_clip();

  // ---- header: search and load buttons ----------------------------------------------
  const Rect table{r.x + tree_w + 1, r.y, r.w - tree_w - 1, r.h};
  dl_.rect(table.x, table.y, table.w, head_h, kPanel);
  const Rect search{table.x + 8, table.y + 4, 260, head_h - 8};
  dl_.rect(search.x, search.y, search.w, search.h, rgba(10, 11, 14));
  dl_.outline(search.x, search.y, search.w, search.h, search_focus_ ? kAccent : kLine);
  dl_.text(search.x + 8, search.y + 3, search_.empty() ? "Search" : search_, 12, search_.empty() ? kDim : kText,
           Align::Left, search.w - 16);
  if (in_.pressed) search_focus_ = search.contains(in_.x, in_.y);
  dl_.text(search.x + search.w + 12, table.y + 8, fmt("%zu tracks", rows_.size()), 11, kDim);

  const bool have = sel_ >= 0 && sel_ < static_cast<int>(rows_.size());
  int load_deck = -1;
  for (int di = 0; di < kUiDecks; ++di) {
    const Rect b{table.x + table.w - 8 - 76.0f * static_cast<float>(kUiDecks - di), table.y + 4, 70, head_h - 8};
    if (button(b, fmt("Load %c", 'A' + di), false, kDeck[di]) && have) load_deck = di;
  }

  // ---- column header ---------------------------------------------------------------------
  const float cy = table.y + head_h;
  const float w_bpm = 64, w_key = 56, w_time = 60, w_badge = 30;
  const float flex = table.w - w_bpm - w_key - w_time - w_badge - 24;
  const float x_title = table.x + w_badge, x_artist = x_title + flex * 0.55f, x_bpm = x_artist + flex * 0.45f;
  const float x_key = x_bpm + w_bpm, x_time = x_key + w_key;
  dl_.rect(table.x, cy, table.w, row_h, kPanelHi);
  dl_.text(x_title, cy + 5, "TITLE", 10, kDim, Align::Left, 0, 1);
  dl_.text(x_artist, cy + 5, "ARTIST", 10, kDim, Align::Left, 0, 1);
  dl_.text(x_bpm + w_bpm - 10, cy + 5, "BPM", 10, kDim, Align::Right, 0, 1);
  dl_.text(x_key + 6, cy + 5, "KEY", 10, kDim, Align::Left, 0, 1);
  dl_.text(x_time + w_time - 6, cy + 5, "TIME", 10, kDim, Align::Right, 0, 1);

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
    if (in_.pressed && row.contains(in_.x, in_.y) && body.contains(in_.x, in_.y)) {
      sel_ = i;
      focus_tree_ = false;
      if (in_.clicks >= 2) load_deck = v[0].playing ? 1 : 0;
    }
  }
  if (rows_.empty()) {
    dl_.text(body.x + body.w / 2, body.y + 40, search_.empty() ? "No tracks yet. Drop audio files here, or use Import library." : "No matches",
             13, kDim, Align::Center);
  }
  dl_.pop_clip();
  if (content > body.h) {  // scroll bar
    const float bar_h = std::max(24.0f, body.h * body.h / content);
    const float bar_y = body.y + (body.h - bar_h) * scroll_ / (content - body.h);
    dl_.rect(body.x + body.w - 5, bar_y, 4, bar_h, kLine);
  }
  if (load_deck >= 0 && sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) {
    const std::int64_t id = rows_[static_cast<std::size_t>(sel_)].id;
    // request_load does not touch browse state, so calling it under the lock is safe.
    request_load(load_deck, id);
  }
}

const dh_frame* App::frame() {
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

  const float top_h = 30, wave_h = 92, deck_h = 150, gap = 2, mixer_w = 260;
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
    frame_.wants_text_input = search_focus_;
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
