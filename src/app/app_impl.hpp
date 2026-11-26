#pragma once
// Private header shared by the App translation units (app.cpp, app_skin.cpp, app_panels.cpp, app_legacy.cpp).
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
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "deckhand/app/api_bridge.hpp"
#include "deckhand/app/capi.h"
#include "deckhand/app/controls.hpp"
#include "deckhand/app/stream_bridge.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/grid_edit.hpp"
#include "deckhand/audio/remote_engine.hpp"
#include "deckhand/gfx/draw.hpp"
#include "deckhand/i18n/i18n.hpp"
#include "deckhand/import/import.hpp"
#include "deckhand/library/keys.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/library/palette.hpp"
#include "deckhand/log/log.hpp"
#include "deckhand/skin/windows.hpp"
#include "deckhand/midi/controller.hpp"
#include "deckhand/services/track_analysis.hpp"
#include "deckhand/services/usb_export.hpp"
#include "deckhand/sync/sync.hpp"
#include "hub_api.hpp"
#include "skin_view.hpp"

namespace deckhand {
namespace detail {

using gfx::Align;
using gfx::Color;
using gfx::rgba;
using gfx::with_alpha;

static_assert(sizeof(gfx::Vertex) == sizeof(dh_vertex), "vertex layouts must match");

inline Color mix(Color a, Color b, float t) {
  const auto ch = [&](int shift) {
    const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
    return static_cast<Color>(std::lround(x + (y - x) * t)) << shift;
  };
  return ch(0) | ch(8) | ch(16) | (a & 0xFF000000u);
}
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
inline std::string fmt(const char* f, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

inline std::string clock_text(double seconds) {
  const bool neg = seconds < 0;
  const int total = static_cast<int>(std::fabs(seconds));
  return fmt("%s%d:%02d", neg ? "-" : "", total / 60, total % 60);
}

inline void utf8_pop(std::string& s) {
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  if (!s.empty()) s.pop_back();
}

inline double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A memory cue or a saved loop (end >= 0) of the loaded track, in frames. Persisted as library cues without a slot.
struct MemCue {
  double frame = 0, end = -1;
  std::string label;
  int color = -1;  // 0xRRGGBB, -1 = default colour
};

struct DeckInfo {
  TrackRow row;
  std::shared_ptr<const TrackAudio> audio;
  std::vector<GridSection> grid;
  std::vector<WavePoint> overview;
  bool loading = false;
  std::string error;
  double gain_db = 0;
  // track preparation: names and colours of the hot cues (positions live in the engine), memory cues, grid lock
  std::string hot_label[kHotCues];
  int hot_color[kHotCues];
  std::vector<MemCue> mem;
  bool grid_locked = false;
  DeckInfo() {
    for (int& c : hot_color) c = -1;
  }
};

struct Job {
  enum Kind { Load, Reattach, AddFile, Import, SaveHotCue, ClearHotCue, HistoryStart, HistoryUpdate, Export, RecFinish, LoadSample, Sync, SyncInfo, RestoreSamples,
              SaveGrid, SaveMemCues, SaveHotStyle, SaveTrackColor, Reanalyse, PreviewLoad } kind = Load;
  int deck = -1;
  std::int64_t track_id = 0;
  std::string path;
  int slot = 0;
  double ms = 0;
  // track preparation payloads
  double ms_end = -1;                 // SaveHotCue: loop end
  std::vector<BeatSection> grid;      // SaveGrid
  std::vector<Cue> cues;              // SaveMemCues
  std::string text;                   // SaveHotStyle: the name
  int color = -1;                     // SaveHotStyle / SaveTrackColor
  bool flag = false;                  // SaveGrid: lock the grid; PreviewLoad: start playing
};

struct TreeItem {
  enum Kind { Collection, Folder, Playlist, HistoryHeader, HistorySession, Smart } kind = Collection;
  std::int64_t id = 0;
  int depth = 0;
  std::string name;
  int count = 0;
};

inline std::int64_t wall_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline std::string date_text(std::int64_t ms) {
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

using Rect = skin::Rect;
constexpr int kUiDecks = 4;  // decks the UI can show (the engine runs kMaxDecks)

constexpr Color kHot[kHotCues] = {rgba(255, 92, 92),  rgba(255, 160, 60), rgba(250, 220, 70), rgba(110, 220, 90),
                                  rgba(60, 210, 200), rgba(80, 150, 255), rgba(170, 110, 255), rgba(255, 110, 200)};

struct SkinEntry {
  std::string name, path;  // name = file or folder stem
};

// What the settings panel shows about library sync. Written by the loader thread (it owns the SyncEngine's
// library connection), read by the UI thread; guarded by App::state_mutex_.
struct SyncSummary {
  bool running = false;        // a sync job is queued or running
  bool known = false;          // a status has been read at least once
  std::int64_t last_ms = 0;    // last completed sync (0 = never)
  bool last_ok = true;
  std::string error;           // last failure
  int sent = -1, received = -1;  // changes of the last sync run in this session (-1 = unknown)
  int other_devices = 0;       // devices seen in the folder besides this one
  int skipped = 0;             // files of other devices the last run could not use (passphrase mismatch)
};

// One window of the UI. In single-window mode these exist too (they hold the arrangement of the modules that are
// docked into the one window); with a multi-window shell each is a native window.
struct Win {
  std::uint32_t id = 0;
  std::string name;   // skin window id ("main", "four", "browser") or "panel"
  std::string role;   // main, browser, ... or the panel kind
  bool panel = false; // the application panel window (settings, ...)
  int skin_index = -1;
  skin::WinState st;  // persisted per skin: position, user scale, size, visibility, stacking, docked-to
  float pt_w = 0, pt_h = 0;               // content size the core wants, points
  std::uint32_t move_seq = 1;
  float px_w = 0, px_h = 0, backing = 1;  // as the shell reported them
  bool reported = false;                  // the shell told us where the window is
  bool placed = false;                    // the core chose or restored the position (a position the shell merely made up does not count)
  ui::Input raw;                          // pointer state of this window (points)
  std::unique_ptr<ui::SkinView> view;
  skin::WindowShape shape;
  bool shape_built = false;
  std::vector<dh_vertex> vtx;
  std::vector<std::uint32_t> idx;
  std::vector<dh_draw_cmd> cmds;
  dh_frame frame{};
  bool popup = false;                     // panel anchored to a control: closes when it loses focus
  bool has_anchor = false;
  float anchor_x = 0, anchor_y = 0;       // desktop point the pop-up hangs from
  bool focused = false;
  bool in_drag = false;                   // moved by a core-driven drag: ignore the shell's move echoes
  bool dirty = false;
  double dirty_at = 0;
  std::string title;
  float user_scale() const { return st.scale; }
};

// A single-line text field shared by the overlays: the focused one receives typed text.
struct Field {
  std::string text;
  bool secret = false;  // drawn as dots
};

}  // namespace detail

using namespace detail;

class App final : public ControlSink, public ApiHost, public ui::SkinHost {
 public:
  App(const dh_platform& platform, const dh_config& config);
  ~App() override;

  void set_display(float w, float h, float scale, float il, float it, float ir, float ib) {
    width_px_ = w;
    height_px_ = h;
    scale_ = scale > 0 ? scale : 1;
    inset_[0] = il / scale_;
    inset_[1] = it / scale_;
    inset_[2] = ir / scale_;
    inset_[3] = ib / scale_;
  }
  void set_modifiers(std::uint32_t mods) { mods_ = mods; }
  void pointer(dh_ptr_kind kind, float x, float y, int clicks);
  void scroll(float dy) { raw_.scroll += dy / (scale_ * fit_); }
  void key(dh_key key, std::uint32_t mods);
  void text(const char* utf8);
  void open_file(const std::string& path, float x, float y);
  void audio_route() { audio_recheck_ = true; }
  const dh_frame* frame();

  // ---- multi-window shells (app_windows.cpp) ----
  void set_capabilities(std::uint32_t caps) { caps_ = caps; }
  void begin_frame(double dt);
  std::uint32_t window_list(dh_window_info* out, std::uint32_t cap);
  void window_display(std::uint32_t id, float w_px, float h_px, float backing);
  const dh_frame* window_frame(std::uint32_t id);
  const dh_frame* end_frame();
  void window_pointer(std::uint32_t id, dh_ptr_kind kind, float x, float y, int clicks);
  void window_scroll(std::uint32_t id, float dy);
  void window_open_file(std::uint32_t id, const std::string& path, float x, float y);
  dh_hit hit_test(std::uint32_t id, float x, float y);
  void window_drag(std::uint32_t id, dh_drag_kind kind, dh_drag_phase phase, float gx, float gy, std::uint32_t mods);
  void window_event(std::uint32_t id, dh_window_event kind, float a, float b);
  void window_set_visible(std::uint32_t id, bool visible);
  void windows_reset();
  std::string api_debug_windows() override;
  std::string api_debug_hit(const std::string& window, float x, float y) override;

  // ApiHost (called from API threads)
  IEngine& api_engine() override { return *engine_; }
  bool api_load(int deck, std::int64_t track_id, std::string& error) override {
    if (engine_->deck(deck).playing) {
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
    j.deck = deck >= 0 && !engine_->deck(deck).playing ? deck : -1;
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
  bool api_prep(int deck, const std::string& action, const PrepArgs& args, std::string& error) override {
    return prep(deck, action, args, error);
  }
  std::string api_deck_prep_json(int deck) override;
  bool api_preview_load(std::int64_t track_id, bool play, std::string& error) override { return preview_start(track_id, play, error); }
  std::string api_preview_title() override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return preview_.title;
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
  void command(const Command& c) override { engine_->send(c); }
  void browse(int steps) override;
  void browse_press() override;
  void load_selected(int deck) override;

  // ui::SkinHost (UI thread)
  bool read_number(const ui::Target& t, double& v) override;
  bool read_text(const ui::Target& t, std::string& out) override;
  bool read_flag(const ui::Target& t) override;
  void press(const ui::Target& t) override;
  void release(const ui::Target& t) override;
  void set_number(const ui::Target& t, double v) override;
  double beat_age_ms(const ui::Target& t) override;
  void draw_custom(const skin::ResolvedWidget& w, const ui::Target& t, const skin::Style& st, const Rect& r) override;

 private:
  // ---- background work ----------------------------------------------------------
  void post(Job j) {
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      jobs_.push_back(std::move(j));
    }
    job_cv_.notify_one();
  }
  void post_kind(Job::Kind k) {
    Job j;
    j.kind = k;
    post(std::move(j));
  }
  void loader_main();
  void control_main();
  void run_job(Library& lib, const Job& j);
  void sync_job(Library& lib, bool run);
  void restore_sampler(Library& lib, bool reload);
  void load_track(Library& lib, int deck, std::int64_t track_id, bool attach_only = false);
  void setup_engine(const std::vector<std::string>& mapping_folders);
  void start_signal_watch();
  void stop_signal_watch();
  void start_audio();
  void restart_audio();
  void apply_input(bool force);  // microphone input from the settings (opens nothing while none is chosen)
  void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_ = s;
    DH_LOG(Info, "%s", s.c_str());
  }
  void request_load(int deck, std::int64_t track_id);
  bool record(bool start, std::string& path, std::string& error);
  std::string source_db() const {
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    return base ? std::string(base) + "\\Pioneer\\rekordbox\\master.db" : std::string();
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/Library/Pioneer/rekordbox/master.db" : std::string();
#endif
  }

  // ---- browser state (app.cpp) ---------------------------------------------------
  void refresh_rows();
  void move_selection(int steps);
  void apply_tree_selection();
  void commit_rename();
  bool view_is_editable_playlist() const {
    return view_ == TreeItem::Playlist && sort_ == SortKey::Default && search_.empty();
  }

  // ---- track preparation: grid editing, cues, loops, waveform zoom (app_prep.cpp) ---------------------------------
  bool prep(int deck, const std::string& action, const PrepArgs& a, std::string& error);  // any thread
  bool prep_grid(int deck, const std::string& action, const PrepArgs& a, std::string& error);
  bool prep_cue(int deck, const std::string& action, const PrepArgs& a, std::string& error);
  void commit_grid(int deck, std::int64_t track_id, const gridedit::Grid& next, bool push_undo);
  void commit_mem(int deck, std::int64_t track_id, std::vector<MemCue> mem);
  void send_meta(int deck, std::int64_t track_id, const std::vector<GridSection>& grid, const std::vector<MemCue>& mem, double bpm);
  void reanalyse(Library& lib, int deck, std::int64_t track_id);
  void open_cue_editor(int deck, int slot, int mem_index);
  void apply_cue_editor(bool remove);
  void set_wave_zoom(int deck, double seconds);
  void load_prep_settings();
  // Marker under the pointer on a zoomed waveform, for dragging and double-click editing: kind 1 hot cue, 2 memory cue, 3 main cue.
  struct CueHit {
    int kind = 0, index = -1;
  };
  CueHit cue_hit_test(int di, const Rect& r, const DeckView& v, const DeckInfo& info, double window, float x, float y) const;

  // ---- skins and layout (app_skin.cpp) ------------------------------------------------
  void scan_skins();
  bool use_skin(const std::string& name, bool persist);
  void cycle_skin();
  void cycle_language();
  void toggle_layout();
  void do_action(const ui::Target& t);
  void draw_skin_ui();
  void draw_wave(int di, const Rect& r, const DeckView& v, const DeckInfo& info, Color base, const ui::Theme& th, bool zoomed);
  void draw_overview(int di, const Rect& r, const DeckView& v, const DeckInfo& info, Color base, const ui::Theme& th);
  void draw_jog(int di, const Rect& r, const DeckView& v, Color base, const skin::Style& st);
  void draw_pads(const ui::Target& t, const skin::ResolvedWidget& w, const Rect& r, const skin::Style& st);
  void draw_sampler_grid(const Rect& r, int cols, int rows, const ui::Input& in);
  bool import_available();
  void draw_tree(const Rect& r, const ui::Theme& th);
  void draw_tracks(const Rect& r, const ui::Theme& th, const std::string& columns, float row_h);
  void draw_search(const Rect& r, const skin::Style& st, const std::string& hint);
  // ---- pre-listen (app_browser.cpp)
  bool preview_start(std::int64_t track_id, bool play, std::string& error);  // any thread
  void preview_toggle();                                                       // UI thread: the selected row
  void load_preview(Library& lib, std::int64_t track_id, bool play);           // loader thread
  void draw_preview_strip(const Rect& r, Color base);
  // ---- browser columns, selection, drag and drop, compatible filter (app_browser.cpp)
  void load_browser_settings(const std::string& skin_default);
  void save_browser_settings();
  std::vector<std::int64_t> selected_ids();                 // browse_mutex_ held
  bool file_missing(const TrackRow& t);                     // cached per refresh
  bool compat_reference(double& bpm, std::string& key);     // the track the compatible filter is about
  void drop_rows_on_playlist(std::int64_t playlist_id);     // browse_mutex_ held
  void playlist_action(const std::string& which);
  Color deck_color(int di) const { return theme_.deck[std::clamp(di, 0, 3)]; }
  Rect inner_rect() const;

  // ---- overlays (app_panels.cpp) ----------------------------------------------------
  enum class Panel { None, Settings, Sampler, Learn, Tags, Relink, Export, AddTo, CueEdit, Columns, Smart };
  bool overlay_open() const { return panel_ != Panel::None; }
  void open_panel(Panel p);
  void close_panel();
  void commit_settings();
  std::string export_what_();
  void draw_overlays();
  void draw_settings(const Rect& m);
  void draw_sampler(const Rect& m);
  void draw_learn(const Rect& m);
  void draw_tags(const Rect& m);
  void draw_relink(const Rect& m);
  void draw_export_menu(const Rect& m);
  void draw_add_menu(const Rect& m);
  void draw_cue_edit(const Rect& m);
  void draw_columns_panel(const Rect& m);
  void draw_smart_editor(const Rect& m);
  void open_smart_editor(std::int64_t id);  // 0 = a new smart playlist
  void save_smart_editor();
  bool ui_button(const Rect& r, const std::string& label, bool lit = false, Color lit_color = 0);
  bool ui_toggle(const Rect& r, const std::string& label, bool on);
  void ui_field(const Rect& r, Field& f, const std::string& hint);
  void ui_heading(float x, float y, const std::string& s);
  void begin_export_menu();
  void learn_pick(const std::string& action);
  void save_setting(const std::string& key, const std::string& value) { lib_->set_setting(key, value); }
  bool field_key(dh_key key);
  void draw_sync_section(float x, float& y, float rw);
  void draw_microphone_section(float x, float& y, float rw);
  void draw_controller_section(float x, float& y, float rw);
  bool choose_folder(const std::string& title, std::string& out);
  static std::string host_name();

  // ---- windows and modules (app_windows.cpp) -------------------------------------------
  bool multi() const { return (caps_ & DH_CAP_MULTI_WINDOW) != 0; }
  void sync_windows();
  Win* find_win(std::uint32_t id);
  Win* find_win_role(const std::string& role);
  void bind_window(Win& w, int skin_index);
  void load_win_state(Win& w);
  void save_win_state(Win& w);
  void mark_dirty(Win& w) {
    w.dirty = true;
    w.dirty_at = now_;
  }
  void place_modules();
  Rect win_frame_rect(const Win& w) const { return {w.st.x, w.st.y, w.pt_w, w.pt_h}; }
  float win_eff_scale(const Win& w) const;
  void ensure_shape(Win& w);
  void do_window_action(const std::string& action);
  void set_module_visible(const std::string& role, int how);  // 0 hide, 1 show, 2 toggle
  bool module_visible(const std::string& role) const;
  bool module_known(const std::string& role) const;
  static const char* panel_role(Panel p);
  void sync_panel_window();
  void draw_window_pass(Win& w);
  void draw_panel_window(Win& w);
  void compose_single();
  void serve_debug_requests();
  std::string run_on_ui(std::function<std::string()> fn);
  void prepare_frame();
  void collect_uploads(std::vector<dh_tex_upload>& uploads, std::vector<std::uint32_t>& releases);
  Rect panel_size(float w, float h, std::string& title);
  void draw_panel_body(const Rect& m, const std::string& title);

  // ---- legacy code-drawn fallback (app_legacy.cpp) ---------------------------------------
  void draw_legacy();
  bool legacy_button(const Rect& r, const std::string& label, bool lit, Color lit_color);

  bool clicked(const Rect& r) const { return in_.pressed && r.contains(in_.x, in_.y); }

  // Text in the active language. Safe from any thread.
  std::string T(std::string_view id) const { return i18n_.tr(id); }
  std::string T(std::string_view id, const i18n::Args& args) const { return i18n_.tr(id, args); }
  static std::string deck_letter(int deck) { return std::string(1, static_cast<char>('A' + deck)); }

  dh_platform platform_{};  // the shell's callbacks (optional members may be NULL)
  std::string data_dir_, resource_dir_;
  i18n::Catalog i18n_;
  std::unique_ptr<Library> lib_;
  std::unique_ptr<IEngine> engine_;       // in-process Engine or RemoteEngine (engine.process setting)
  RemoteEngine* remote_ = nullptr;        // engine_ when it runs in its own process
  std::unique_ptr<ControllerHub> local_hub_;  // in-process mode only; otherwise the hub lives in the engine process
  std::unique_ptr<MidiIO> midi_;
  std::unique_ptr<HidIO> hid_;
  std::unique_ptr<HubApi> hub_;
  std::thread signal_thread_;
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

  std::mutex audio_mutex_;  // serialises engine start/stop (UI thread and control thread)
  std::mutex state_mutex_;  // guards the members below, shared with the worker threads
  DeckInfo decks_[kMaxDecks];
  std::string status_, audio_label_, audio_choice_, audio_pref_, last_audio_error_;
  std::uint32_t audio_period_ = 256;
  std::string input_pref_, input_error_;  // audio.input_device ("" = no microphone) and the last open error
  int input_channel_ = 0;                 // audio.input_channel, 0-based
  bool input_stereo_ = false;             // audio.input_stereo
  float talk_depth_ = -12.0f;             // audio.mic_depth, dB
  bool importing_ = false, exporting_ = false;
  std::atomic<bool> rows_dirty_{true};
  std::atomic<bool> audio_recheck_{false};

  std::mutex browse_mutex_;  // guards the browse state, shared with the controller thread
  std::vector<TreeItem> tree_;
  // The track table: all ids, rows fetched lazily for the part on screen (so a 100k library does not build 100k rows
  // per refresh). id() touches no database and is safe under browse_mutex_ from any thread; operator[] reads the
  // library and is for the UI thread only.
  class TrackList {
   public:
    void set(Library* lib, std::vector<std::int64_t> ids) {
      lib_ = lib;
      ids_ = std::move(ids);
      cache_.clear();
      lo_ = 0;
    }
    std::size_t size() const { return ids_.size(); }
    bool empty() const { return ids_.empty(); }
    std::int64_t id(std::size_t i) const { return ids_[i]; }
    const TrackRow& operator[](std::size_t i) {
      if (i < lo_ || i >= lo_ + cache_.size()) {
        lo_ = i > 32 ? i - 32 : 0;
        const std::size_t hi = std::min(ids_.size(), lo_ + 160);
        cache_.clear();
        for (std::size_t k = lo_; k < hi; ++k) {
          std::optional<TrackRow> row = lib_ ? lib_->track(ids_[k]) : std::nullopt;
          if (!row) {
            row.emplace();  // removed meanwhile: keep the slot, the table is rebuilt on the next refresh
            row->id = ids_[k];
          }
          cache_.push_back(std::move(*row));
        }
      }
      return cache_[i - lo_];
    }

   private:
    Library* lib_ = nullptr;
    std::vector<std::int64_t> ids_;
    std::vector<TrackRow> cache_;  // rows [lo_, lo_ + cache_.size())
    std::size_t lo_ = 0;
  } rows_;
  TreeItem::Kind view_ = TreeItem::Collection;  // what the track table shows
  std::int64_t playlist_id_ = 0;                // playlist or history session, by view_
  int tree_sel_ = 0;                            // index into tree_
  SortKey sort_ = SortKey::Default;
  bool sort_desc_ = false;
  std::int64_t rename_id_ = 0;  // playlist being renamed in place
  std::string rename_buf_;
  bool rename_fresh_ = false;  // the first typed character replaces the suggested name
  std::int64_t confirm_delete_ = 0;
  float tree_scroll_ = 0;
  int sel_ = 0;
  bool focus_tree_ = false;
  float scroll_ = 0;
  bool follow_sel_ = false;
  std::string search_;
  bool search_focus_ = false;

  // ---- track preparation state
  std::atomic<bool> grid_edit_[kMaxDecks] = {};
  double wave_zoom_[kMaxDecks] = {6.0, 6.0, 6.0, 6.0};       // seconds visible in the scrolling waveform
  std::vector<gridedit::Grid> grid_undo_[kMaxDecks];         // state_mutex_
  std::vector<double> tap_times_[kMaxDecks];                 // state_mutex_
  struct CueDrag {
    int deck = -1, kind = 0, index = -1;
    double frame = 0;      // where the marker would land
    bool moved = false;
  } cue_drag_;
  int scrub_deck_ = -1;
  float scrub_x0_ = 0;
  double scrub_pos0_ = 0;
  int cue_edit_deck_ = -1, cue_edit_slot_ = -1, cue_edit_mem_ = -1, cue_edit_color_ = -1;
  Field f_cue_name_;

  // ---- browser (app_browser.cpp), UI thread
  std::vector<std::string> bcols_;                   // shown columns, in order
  std::map<std::string, float> bwidth_;              // user widths: px, or weight for the flexible columns
  bool bcols_loaded_ = false, key_musical_ = false, compat_on_ = false;
  std::int64_t compat_ref_id_ = 0;
  unsigned frame_count_ = 0;
  std::set<int> multi_;                              // selected row indices (browse_mutex_)
  int anchor_ = 0;
  std::map<std::int64_t, bool> exists_;              // file present, per track, cleared on refresh
  struct RowDrag {
    bool pending = false, active = false, collapse = false;
    float x0 = 0, y0 = 0;
    int row = -1;
  } rdrag_;
  int col_drag_ = -1;
  float col_drag_x_ = 0, col_drag_w_ = 0;
  struct PreviewInfo {
    std::int64_t id = 0;
    std::string title;
    std::vector<WavePoint> overview;
    bool loading = false;
  } preview_;                                        // state_mutex_
  std::atomic<bool> preview_master_{false};          // setting preview.master: also on the master output (default off)
  std::vector<std::int64_t> add_ids_;                // tracks the "add to playlist" menu will add
  struct RuleEdit {
    SmartRule rule;
    Field a, b;
  };
  std::deque<RuleEdit> sm_rules_;
  std::int64_t sm_id_ = 0;
  Field f_sm_name_;
  bool sm_all_ = true, sm_confirm_ = false;

  // ---- UI state (UI thread only)
  ui::Input raw_, in_, ov_;  // raw_ = what the shell sent; in_ = what the main UI sees (blocked under overlays)
  std::uint32_t mods_ = 0;
  int held_ = 0;        // id of the control being held with the pointer
  int drag_deck_ = -1;
  int jog_deck_ = -1;
  bool jog_top_ = false;
  float jog_angle_ = 0;
  bool panel_fresh_ = false;
  double import_checked_ = 0;
  bool import_ok_ = false;
  float width_px_ = 1280, height_px_ = 800, scale_ = 1, inset_[4] = {0, 0, 0, 0};
  bool touch_ = false;
  std::string status_note_;  // UI thread; shown via set_status on the next frame
  double now_ = 0;
  DeckView views_[kMaxDecks];
  MixerView mixer_;
  DeckInfo info_[kMaxDecks];
  SamplerView samplers_[kSamplerSlots];

  // ---- windows
  std::uint32_t caps_ = 0;  // DH_CAP_* the shell announced (0 = one rectangular window)
  std::vector<std::unique_ptr<Win>> wins_;
  std::uint32_t next_win_id_ = 1;
  int z_counter_ = 1;
  unsigned skin_gen_ = 0, arr_gen_ = ~0u;  // bumped by use_skin(); the arrangement follows
  std::string arr_variant_;
  Win* cur_win_ = nullptr;   // the window being drawn (null: the single window)
  float fit_ = 1;            // single-window fallback: uniform shrink of the docked canvas
  bool any_down_ = false;    // a pointer button is down in some window
  std::mutex dbg_mutex_;
  std::vector<std::function<void()>> dbg_queue_;
  dh_frame end_frame_{};
  std::vector<dh_tex_upload> end_uploads_;
  std::vector<std::uint32_t> end_releases_;
  bool arrangement_reset_ = false;         // windows_reset(): ignore the saved arrangement while rebuilding
  bool panel_anchor_valid_ = false;        // the open pop-up hangs from this desktop point
  float panel_anchor_x_ = 0, panel_anchor_y_ = 0;
  struct DragSession {                     // a core-driven window move or resize
    bool active = false, alone = false;
    std::uint32_t win = 0;
    dh_drag_kind kind = DH_DRAG_MOVE;
    float gx = 0, gy = 0, start_scale = 1;
    skin::Frame start;
    std::string corner, attach;
    std::vector<std::pair<std::uint32_t, skin::Point>> group;  // windows that move along, with their start positions
  } drag_;

  std::unique_ptr<ui::SkinView> skin_;
  ui::Theme theme_ = ui::Theme::defaults();
  std::vector<SkinEntry> skins_;
  std::string skin_name_, variant_ = "main";
  bool legacy_ = false;
  std::string skin_error_;

  // ---- overlay state
  Panel panel_ = Panel::None;
  Rect panel_rect_;
  std::vector<std::string> export_targets_;
  bool export_other_ = false;  // the last entry of the export menu is "Other folder..." (needs choose_folder)
  Field* focus_ = nullptr;  // focused overlay field
  Field f_period_, f_port_, f_obs_host_, f_obs_port_, f_obs_source_, f_search_learn_;
  Field f_title_, f_artist_, f_key_, f_rating_, f_relink_, f_relink_old_, f_sync_folder_, f_sync_name_, f_sync_pass_;
  Field* last_focus_ = nullptr;
  std::map<std::string, std::string> sv_;  // settings shown in the settings panel
  int tag_rating_ = 0;
  std::string tag_path_;
  float dev_scroll_ = 0, panel_content_h_ = 0;
  std::vector<std::string> output_devices_, input_devices_;
  float in_scroll_ = 0;
  float panel_scroll_ = 0;
  std::int64_t edit_track_ = 0;
  std::string relink_old_;
  bool confirm_remove_ = false;
  struct LearnEntry {
    std::string label, action, group;  // label = "Deck 1 > Play" (group name translated), group = header text
  };
  std::vector<LearnEntry> learn_list_;
  std::string learn_action_, learn_label_, learn_line_, learn_note_;
  bool learn_saved_ = false;
  int sampler_drop_ = -1;
  Rect sampler_slot_rect_[kSamplerSlots];
  std::string sampler_names_[kSamplerSlots];  // guarded by state_mutex_

  SyncSummary sync_;  // guarded by state_mutex_
  std::vector<dh_draw_cmd> cmds_out_;
  std::vector<dh_tex_upload> uploads_out_;
  std::vector<std::uint32_t> releases_out_;
  dh_tex_upload upload_{};
  dh_frame frame_{};
};

}  // namespace deckhand
