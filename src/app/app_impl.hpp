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
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "deckhand/app/api_bridge.hpp"
#include "deckhand/app/capi.h"
#include "deckhand/app/controls.hpp"
#include "deckhand/app/stream_bridge.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/remote_engine.hpp"
#include "deckhand/gfx/draw.hpp"
#include "deckhand/i18n/i18n.hpp"
#include "deckhand/import/import.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/log/log.hpp"
#include "deckhand/midi/controller.hpp"
#include "deckhand/services/track_analysis.hpp"
#include "deckhand/services/usb_export.hpp"
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
  enum Kind { Load, Reattach, AddFile, Import, SaveHotCue, ClearHotCue, HistoryStart, HistoryUpdate, Export, RecFinish, LoadSample, Sync, SyncInfo, RestoreSamples } kind = Load;
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
};

// A single-line text field shared by the overlays: the focused one receives typed text.
struct Field {
  std::string text;
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
  void scroll(float dy) { raw_.scroll += dy / scale_; }
  void key(dh_key key, std::uint32_t mods);
  void text(const char* utf8);
  void open_file(const std::string& path, float x, float y);
  void audio_route() { audio_recheck_ = true; }
  const dh_frame* frame();

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

  // ---- browser state (app.cpp) ---------------------------------------------------
  void refresh_rows();
  void move_selection(int steps);
  void apply_tree_selection();
  void commit_rename();
  bool view_is_editable_playlist() const {
    return view_ == TreeItem::Playlist && sort_ == SortKey::Default && search_.empty();
  }

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
  void playlist_action(const std::string& which);
  Color deck_color(int di) const { return theme_.deck[std::clamp(di, 0, 3)]; }
  Rect inner_rect() const;

  // ---- overlays (app_panels.cpp) ----------------------------------------------------
  enum class Panel { None, Settings, Sampler, Learn, Tags, Relink, Export, AddTo };
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
  bool ui_button(const Rect& r, const std::string& label, bool lit = false, Color lit_color = 0);
  bool ui_toggle(const Rect& r, const std::string& label, bool on);
  void ui_field(const Rect& r, Field& f, const std::string& hint);
  void ui_heading(float x, float y, const std::string& s);
  void begin_export_menu();
  void learn_pick(const std::string& action);
  void save_setting(const std::string& key, const std::string& value) { lib_->set_setting(key, value); }
  bool field_key(dh_key key);
  void draw_sync_section(float x, float& y, float rw);
  void draw_controller_section(float x, float& y, float rw);
  bool choose_folder(const std::string& title, std::string& out);
  static std::string host_name();

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
  std::string status_, audio_label_, audio_choice_, audio_pref_;
  std::uint32_t audio_period_ = 256;
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
  bool rename_fresh_ = false;  // the first typed character replaces the suggested name
  std::int64_t confirm_delete_ = 0;
  float tree_scroll_ = 0;
  int sel_ = 0;
  bool focus_tree_ = false;
  float scroll_ = 0;
  bool follow_sel_ = false;
  std::string search_;
  bool search_focus_ = false;

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
  Field f_title_, f_artist_, f_key_, f_rating_, f_relink_, f_relink_old_, f_sync_folder_, f_sync_name_;
  Field* last_focus_ = nullptr;
  std::map<std::string, std::string> sv_;  // settings shown in the settings panel
  int tag_rating_ = 0;
  std::string tag_path_;
  float dev_scroll_ = 0, panel_content_h_ = 0;
  std::vector<std::string> output_devices_;
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
