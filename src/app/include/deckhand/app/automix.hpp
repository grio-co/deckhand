#pragma once
// Automix: plays the play queue by itself. Two decks alternate; while one plays, the next queue entry is loaded on the
// other and cued at its mix-in point. A transition is planned and handed to the engine as scheduled commands, all
// anchored to the outgoing deck's beats (or its frames, for tracks without a grid), so the incoming track starts on
// the exact sample of a bar line and the engine, not this code's polling, carries the fade out. Talks to the engine
// only through IEngine (commands, state and scheduling), so it works with the engine in its own process.
//
// Threading: every method may be called from any thread; tick() must be called regularly (10 to 50 ms) from one.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "deckhand/app/play_queue.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/mixer_laws.hpp"
#include "deckhand/library/library.hpp"

namespace deckhand {

// Global automix settings (library settings `automix.*` and `queue.endless*`).
struct AutomixSettings {
  std::string transition_type = "crossfade";  // crossfade | cut | fade | echo_out
  std::string unsynced_type = "cut";          // used when the tempo gap is too big to sync, or a track has no beat grid
  double transition_beats = 16;               // default transition length
  double mix_out_bars = 16;                   // default mix-out: this many bars before the last audible sound
  double max_sync_gap_pct = 8;                // never match tempo across a bigger gap
  bool bass_swap = true;                      // crossfade: the incoming bass comes in (and the outgoing bass goes) halfway
  std::string override_mode = "yield";        // yield: a touched control is left alone until the transition ends | hands_off
  std::string on_empty = "finish";            // finish: the last track plays to its end | fade: it fades out at its mix-out
  double fade_seconds = 8;                    // length of a fade for tracks without a beat grid
  bool endless = false;                       // keep `endless_keep` entries queued from `endless_source`
  std::string endless_source;                 // "playlist:<id>" | "smart:<id>" | "search:<text>"
  int endless_keep = 5;
  std::string endless_order = "sequential";   // sequential | shuffle

  void load(Library& lib);
  void save(Library& lib) const;
  static bool valid_type(const std::string& t);
};

// What the automix engine needs to know about a track (the host reads it from the library).
struct TrackFacts {
  bool ok = false;
  std::int64_t id = 0;
  std::string title;
  double bpm = 0;
  std::vector<BeatSection> grid;
  double duration_ms = 0;
  TrackCues cues;
  AutomixPoints points;  // the track's own automix points
  StoredPhrases phrases;  // stored phrase analysis (no sections when the track has none); gives the default mix-in and mix-out
};

// The mix-in and mix-out the phrase analysis suggests (cue_suggest.cpp; either may be missing).
struct PhrasePoints {
  std::optional<double> mix_in_ms, mix_out_ms;
};
PhrasePoints phrase_mix_points(const StoredPhrases& phrases, double first_sound_ms, double last_sound_ms, int mix_out_bars);

enum class LoadState { Pending, Ready, Failed };

// Linked mix points: when the playing track and the next queued one (first row of the library's play queue) have a link
// that has not passed yet, it replaces the computed mix-out of the playing track and the mix-in of the next one (and the
// transition length and type, when the link sets them). Entry overrides of the queue still win, as they sit above a
// track's own points in the planner. Used by the application's host; tests drive it with a simulated one.
// Returns true when `f` was changed.
bool apply_mix_links(Library& lib, IEngine& engine, TrackFacts& f);

class AutomixHost {
 public:
  virtual ~AutomixHost() = default;
  virtual IEngine& engine() = 0;
  virtual TrackFacts facts(std::int64_t track_id) = 0;
  // Starts loading `track_id` on `deck` (which is stopped); load_state() reports the outcome.
  virtual bool request_load(int deck, std::int64_t track_id) = 0;
  virtual LoadState load_state(int deck, std::int64_t track_id) = 0;
  // Track ids of a source for endless mode and queue fills ("playlist:12", "smart:3", "search:text").
  virtual std::vector<std::int64_t> source_tracks(const std::string& source) { (void)source; return {}; }
  virtual std::int64_t now_ms();  // wall clock, for the history
};

struct AutomixStatus {
  std::string state;  // stopped | idle | playing | armed | transition | paused | finishing
  bool running = false, paused = false, resume_available = false;
  int current_deck = -1, next_deck = -1;
  std::int64_t current_track = 0, next_track = 0, current_entry = 0, next_entry = 0;
  double seconds_to_transition = -1;  // -1 unknown
  std::string transition_type;        // of the planned or running transition
  double transition_beats = 0;
  bool synced = false;                // tempo is matched
  std::string note;                   // last problem or reason for a pause
  std::uint64_t queue_revision = 0;
  std::int64_t planned_mix_out_ms = -1;  // of the playing track
};

struct AutomixEvent {
  std::string type;  // queue.changed | automix.state | automix.transition_start | automix.transition_end | automix.entry_failed | automix.finished
  std::map<std::string, std::string> text;
  std::map<std::string, double> num;
};

// Reads what automix needs about a track from the library (ok = false when the track does not exist).
TrackFacts read_track_facts(Library& lib, std::int64_t track_id);
// Track ids of "playlist:<id>", "smart:<id>" or "search:<text>"; empty for anything else.
std::vector<std::int64_t> resolve_track_source(Library& lib, const std::string& source);

namespace automix {

// The engine's beat arithmetic (beat 0 = first grid beat, sections continue the count), in the frames of one track.
struct BeatGrid {
  bool ok = false;
  double sr = 44100;
  std::vector<GridSection> sections;
  std::vector<double> base;  // beat number at the start of each section
  static BeatGrid make(const std::vector<BeatSection>& grid_ms, double fallback_bpm, double sample_rate);
  double frame_to_beat(double frame) const;
  double beat_to_frame(double beat) const;
  int bar_beat(double beat) const;      // 1..4
  double bpm_at_frame(double frame) const;
  double beat_frames_at(double frame) const;  // frames per beat in force at `frame`
  double bar_floor(double beat) const;  // the bar line at or before `beat` (a beat number)
  double bar_ceil(double beat) const;   // the bar line at or after `beat`
};

// A track's points once the unset ones are filled in.
struct ResolvedTrack {
  double mix_in_ms = 0, mix_out_ms = 0, duration_ms = 0, last_sound_ms = 0;
  bool mix_out_automatic = true;
  bool mix_out_limited_by_max_play = false;
  std::optional<double> beats;  // entry override, else the track's own, else unset
  std::string type;             // same ("" = unset)
};
// `entry` are the queue entry's overrides (win over the track's own points).
ResolvedTrack resolve_track(const TrackFacts& f, const AutomixPoints& entry, const AutomixSettings& st);

}  // namespace automix

class AutomixService {
 public:
  // `lib` (may be null) stores the settings and the "was running" hint; give it a connection of its own.
  AutomixService(AutomixHost& host, PlayQueue& queue, Library* lib = nullptr);
  ~AutomixService();

  // ---- control (any thread) ----
  bool start(std::string& error);   // plays from the queue (or continues what is already playing)
  void stop();                      // leaves the decks as they are (a running transition finishes first)
  void pause();                     // no new transition is started; what plays goes on
  void resume();
  bool skip();                      // start the next transition now (on the next bar)
  void set_listener(std::function<void(const AutomixEvent&)> cb);
  // The engine's mixer laws (Op::MixerSetting values), so a transition's ideal fader curve is judged against the right law.
  // Call whenever the settings change; the defaults match an engine that was never configured.
  void set_mixer_laws(const mixer::Settings& laws);

  AutomixSettings settings() const;
  void set_settings(const AutomixSettings& s);
  AutomixStatus status() const;

  // ---- called by the application ----
  void tick();                                  // every 10 to 50 ms
  void on_engine_event(const EngineEvent& e);   // ScheduledFired / ScheduledDropped
  void flush_events();                          // delivers queued events to the listener (tick() does it too)

  PlayQueue& queue() { return queue_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  PlayQueue& queue_;
};

}  // namespace deckhand
