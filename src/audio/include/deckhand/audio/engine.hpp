#pragma once
// Real-time audio engine: decks, mixer, cue bus. The audio callback takes no locks,
// allocates nothing and does no I/O; everything reaches it through wait-free queues.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "deckhand/audio/track.hpp"

namespace deckhand {

constexpr int kMaxDecks = 4;
constexpr int kHotCues = 8;
constexpr int kFxSlots = 3;       // assignable effect slots per channel and on the master bus
constexpr int kSamplerSlots = 16;
constexpr int kFxMaster = 4;      // `deck` value that addresses the master effect bus in Fx commands

struct GridSection {
  double first_beat_frame = 0;
  double bpm = 0;
  int bar_beat = 1;
};

// Everything a deck needs for one track. Built off the audio thread, then handed over.
struct LoadedTrack {
  std::shared_ptr<const TrackAudio> audio;
  std::int64_t track_id = 0;
  double bpm = 0;
  std::vector<GridSection> grid;    // sorted by first_beat_frame
  std::vector<double> memory_cues;  // frames, sorted
  double hot_cue[kHotCues];
  double hot_loop_end[kHotCues];
  double cue_point = 0;
  LoadedTrack() {
    for (int i = 0; i < kHotCues; ++i) hot_cue[i] = hot_loop_end[i] = -1;
  }
};

enum class Op : std::uint8_t {
  Load,          // track
  Eject,
  PlayToggle,
  CueDown,
  CueUp,
  HotCueDown,    // i = slot
  HotCueUp,      // i = slot
  HotCueClear,   // i = slot
  Seek,          // d = frame
  JogTouch,      // i = 1 touched, 0 released
  JogTurn,       // d = revolutions (signed), i = 1 platter top (scratch when touched), 0 = side (bend)
  Tempo,         // f = -1..1 (positive = faster)
  TempoRange,    // f = fraction, e.g. 0.06
  SyncToggle,
  LoopIn,
  LoopOut,
  LoopToggle,    // f = beats used when no loop is active
  BeatLoop,      // f = beats
  Reloop,
  LoopScale,     // f = factor (0.5, 2)
  BeatJump,      // f = beats (signed)
  CueCall,       // i = -1 previous, +1 next (halves / doubles an active loop)
  Trim,          // f = 0..1 knob position (0.5 = unity) for this and the following
  EqHigh,
  EqMid,
  EqLow,
  Filter,        // 0.5 = off, below = low-pass, above = high-pass
  Fader,         // f = 0..1
  Pfl,           // i = 1 on, 0 off, -1 toggle (headphone cue)
  Crossfader,    // f = 0..1 (0 = left)
  HeadMix,       // f = 0 cue .. 1 master
  HeadLevel,     // f = 0..1
  MasterLevel,   // f = 0..1
  // ---- appended: key lock, quantise, slip, reverse, auto-gain, effects, sampler, recording ----
  KeyLock,       // i = 1 on, 0 off, -1 toggle (master tempo: tempo changes keep pitch)
  KeyShift,      // f = semitones, -12..+12 (works with key lock on or off)
  Quantize,      // i = 1 on, 0 off, -1 toggle (cues, loop in/out, beat loops and beat jumps snap to the grid)
  Slip,          // i = 1 on, 0 off, -1 toggle (shadow playhead during scratch / loop / hot-cue hold / reverse)
  Reverse,       // i = 1 on, 0 off, -1 toggle (play backwards while playing)
  LoopMove,      // f = beats (signed); moves the loop, keeping its length
  LoopExit,      // leaves the loop (Reloop re-enters it)
  AutoGain,      // f = dB offset (-24..+12) applied after the trim knob
  ColorFx,       // i = ColorFx value: which effect the filter knob drives on this channel
  FxSelect,      // deck = 0..3 channel or kFxMaster, i = slot 0..2, d = (double)EffectKind
  FxEnable,      // deck, i = slot, f = 1 on / 0 off (release effects fire while on)
  FxWet,         // deck, i = slot, f = 0..1 wet/dry (send level for tail effects)
  FxTime,        // deck, i = slot, f = beats (1/16..16); d > 0 = fixed seconds instead of beats
  FxParam,       // deck, i = slot, f = 0..1 secondary parameter (see EffectInfo::param_name)
  MasterBpm,     // f = BPM used for beat-synced master effects and sampler sync; 0 = follow the decks
  SamplerLoad,   // deck = slot 0..15, sample (use Engine::sampler_load)
  SamplerTrigger,// deck = slot, f = velocity 0..1 (0 = full); retriggers a playing slot
  SamplerStop,   // deck = slot, i = -1 stops every slot
  SamplerLevel,  // f = 0..1 sampler bus level
  SamplerSync,   // deck = slot, i = 1 follow master tempo, 0 = off
  SamplerLoop,   // deck = slot, i = 1 loop, 0 one-shot
  SamplerGain,   // deck = slot, f = 0..1 slot level (0.8 = unity)
  RecStart,      // f = 1 tap before the master soft clip, 0 after; needs Engine::record_open first
  RecStop,       // stops recording; the writer thread finalises the file
  StreamEnable,  // i = 1 on / 0 off, f = 1 pre-soft-clip tap; feeds Engine::stream_read
  // ---- internal (scheduling); never pass these to send() / schedule() ----
  SchedInsert,     // i = payload slot handed over by Engine::schedule
  SchedCancel,     // d = schedule id
  SchedCancelAll,
};

enum class ColorFx : std::uint8_t { Filter = 0, Noise, DubEcho, Crush, Pitch, Space, Count };

// Effect registry. Every effect is one entry in a table in src/audio/fx.cpp.
enum class EffectKind : std::uint8_t {
  None = 0,
  Echo, PingPong, Spiral, Reverb, Flanger, Phaser, LowCutEcho, FilterSweep, Roll, Transform, Pitch,
  Crusher, VinylBrake, Backspin,
  ColorNoise, ColorDubEcho, ColorCrush, ColorPitch, ColorSpace,  // colour-slot only (driven by the knob)
  Count
};

struct EffectInfo {
  EffectKind kind = EffectKind::None;
  const char* name = "";
  const char* param_name = "";  // what FxParam controls for this effect
  bool beat_synced = false;     // FxTime is a beat fraction
  bool tail = false;            // keeps ringing after it is switched off
  bool release = false;         // vinyl brake / backspin: acts while engaged, returns on release
  bool colour = false;          // only valid through Op::ColorFx
  float default_wet = 0.5f, default_param = 0.5f, default_beats = 1.0f;
};
int effect_count();                        // number of real effects (excludes None)
const EffectInfo& effect_info_at(int i);   // 0 .. effect_count()-1
const EffectInfo* effect_info(EffectKind k);

// Sample for a sampler slot. Built off the audio thread, then handed over.
struct LoadedSample {
  std::shared_ptr<const TrackAudio> audio;
  double bpm = 0;       // tempo of the sample, used when sync is on
  bool loop = false;
  bool sync = false;    // follow the master tempo (varispeed)
  float gain = 1.0f;
};

struct Command {
  Op op{};
  std::uint8_t deck = 0;
  std::int32_t i = 0;
  float f = 0;
  double d = 0;
  LoadedTrack* track = nullptr;
  LoadedSample* sample = nullptr;
};

// ScheduledFired: `sched_id` fired at engine clock frame `clock_frame` (the first output sample the command
// affected); `deck` = the command's deck, `slot` = the command's Op, `frame` = for deck-position schedules the
// trigger deck's playhead (track frames) when it fired, else 0.
// ScheduledDropped: the entry was discarded without running; `slot` = a DropReason, `clock_frame` = when.
enum class EventType : std::uint8_t { HotCueSet, HotCueCleared, CueSet, TrackEnded, ScheduledFired, ScheduledDropped };
enum class DropReason : std::int32_t { DeckUnloaded = 1, TableFull = 2, Unresolvable = 3 };  // Unresolvable: no track / grid

struct EngineEvent {
  EventType type{};
  std::uint8_t deck = 0;
  std::int32_t slot = 0;
  double frame = 0;
  std::int64_t track_id = 0;
  // appended
  std::uint64_t sched_id = 0;
  std::uint64_t clock_frame = 0;
};

// When a scheduled command runs. The command executes inside the audio callback on the exact output sample.
enum class ScheduleKind : std::uint8_t { ClockFrame, DeckFrame, DeckBeat };
struct Schedule {
  ScheduleKind kind = ScheduleKind::ClockFrame;
  std::uint64_t clock_frame = 0;  // ClockFrame: absolute engine frame (Engine::clock_frames() timeline)
  std::uint8_t deck = 0;          // DeckFrame / DeckBeat: the deck whose playhead triggers
  double frame = 0;               // DeckFrame: track position in the track's own frames
  double beat = 0;                // DeckBeat: absolute beat number (beat 0 = first grid beat; sections continue the count),
                                  //   or, when `relative`, beats ahead of the playhead (> 0; 1 = the next grid beat)
  bool relative = false;          // DeckBeat: `beat` is beats_from_now
  bool bar_aligned = false;       // DeckBeat: move on to the next bar line (grid beat whose bar_beat is 1)
  bool fire_if_passed = false;    // Deck kinds: also fire when the playhead is already past / jumps past the target
  static Schedule at_clock_frame(std::uint64_t f) {
    Schedule s;
    s.clock_frame = f;
    return s;
  }
  static Schedule at_deck_frame(int deck, double frame, bool fire_if_passed = false) {
    Schedule s;
    s.kind = ScheduleKind::DeckFrame;
    s.deck = static_cast<std::uint8_t>(deck);
    s.frame = frame;
    s.fire_if_passed = fire_if_passed;
    return s;
  }
  static Schedule at_deck_beat(int deck, double beat_number, bool bar_aligned = false, bool fire_if_passed = false) {
    Schedule s;
    s.kind = ScheduleKind::DeckBeat;
    s.deck = static_cast<std::uint8_t>(deck);
    s.beat = beat_number;
    s.bar_aligned = bar_aligned;
    s.fire_if_passed = fire_if_passed;
    return s;
  }
  static Schedule beats_from_now(int deck, double beats, bool bar_aligned = false) {
    Schedule s = at_deck_beat(deck, beats, bar_aligned);
    s.relative = true;
    return s;
  }
};

struct FxSlotView {
  EffectKind kind = EffectKind::None;
  bool on = false;
  float wet = 0, param = 0, beats = 1;
};

struct DeckView {
  bool loaded = false, playing = false, sync = false, pfl = false;
  bool loop_active = false, touching = false, at_cue = false;
  std::int64_t track_id = 0;
  double position = 0, length = 0, sample_rate = 44100;  // frames
  double speed = 0;         // current playback rate, 1.0 = normal
  double tempo = 0;         // tempo offset as a fraction (+0.02 = +2 %)
  double tempo_range = 0.06;
  double bpm = 0;           // effective BPM at the current tempo
  double cue = 0, loop_in = -1, loop_out = -1;
  double hot_cue[kHotCues] = {-1, -1, -1, -1, -1, -1, -1, -1};
  float level = 0;          // pre-fader peak, 0..1
  float trim = 0.5f, eq_high = 0.5f, eq_mid = 0.5f, eq_low = 0.5f, filter = 0.5f, fader = 1.0f;
  // appended
  bool key_lock = false, key_lock_active = false;  // active = the time-stretcher is engaged right now
  bool quantize = false, slip = false, slip_active = false, reverse = false;
  float key_shift = 0;          // semitones
  float auto_gain_db = 0;
  int color_fx = 0;             // ColorFx
  double slip_position = -1;    // shadow playhead (frames) while slip_active
  FxSlotView fx[kFxSlots];
};

struct MixerView {
  float crossfader = 0.5f, head_mix = 0.5f, head_level = 0.8f, master_level = 0.8f;
  float peak_left = 0, peak_right = 0;
  // appended
  FxSlotView fx[kFxSlots];     // master effect bus
  float sampler_level = 0.8f;
  double master_bpm = 0;       // BPM driving master effects and sampler sync (0 = unknown)
  bool stream_enabled = false;
};

struct SamplerView {
  bool loaded = false, playing = false, loop = false, sync = false;
  double position = 0, length = 0;  // frames
};

struct RecorderStatus {
  bool open = false;        // a file is open (armed or recording)
  bool recording = false;
  bool finished = false;    // the file was finalised
  bool error = false;
  double seconds = 0;       // audio written to disk
  std::uint64_t dropped_frames = 0;  // expected 0
  std::uint64_t bytes = 0;
};

struct OutputConfig {
  std::string device_hint;  // case-insensitive substring of the device name; empty = system default
  int master_channel = 0;   // first channel of the master pair (0-based)
  int phones_channel = 2;   // first channel of the headphone pair; ignored if the device is stereo
  std::uint32_t period_frames = 256;
};

class Engine {
 public:
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Opens the output device and starts the callback. Returns false and sets error on failure.
  bool start(const OutputConfig& config, std::string& error);
  void stop();
  bool running() const;
  std::string device_name() const;
  int output_channels() const;
  std::uint32_t sample_rate() const;
  // Names of the playback devices currently present.
  static std::vector<std::string> output_devices();

  // Thread-safe for any number of non-real-time callers.
  void send(const Command& c);
  void load(int deck, std::unique_ptr<LoadedTrack> track);

  DeckView deck(int index) const;
  MixerView mixer() const;
  SamplerView sampler(int slot) const;

  // Sampler: hand a sample to a slot (non-real-time caller).
  void sampler_load(int slot, std::unique_ptr<LoadedSample> sample);

  // Recording of the master bus to 24-bit WAV (RF64 beyond 4 GB; the header is refreshed about once a
  // second, so a crash still leaves a playable file). record_open opens the file and the writer thread and
  // starts recording (Op::RecStart is sent for you). Op::RecStop / record_stop ends it; the writer then
  // finalises the file. record_close waits for that and releases the writer (call before opening again).
  bool record_open(const std::string& path, bool pre_clip, std::string& error);
  void record_stop();
  void record_close();
  RecorderStatus recorder_status() const;

  // Stream bus: a second master tap for the broadcast module. Enable with Op::StreamEnable (or
  // stream_enable), then pull interleaved stereo float frames from one non-real-time thread.
  void stream_enable(bool on, bool pre_clip = false);
  std::size_t stream_read(float* interleaved_stereo, std::size_t max_frames);
  std::uint64_t stream_dropped_frames() const;

  // ---- engine clock and sample-accurate scheduling ----
  // Output frames rendered so far (monotonic, any thread; advances once per callback).
  std::uint64_t clock_frames() const;
  double clock_seconds() const;
  // Estimated engine frame being rendered at wall time `t` (extrapolated from the device callback timing; error is
  // about one callback jitter; add the device output latency yourself). Offline: just clock_frames().
  std::uint64_t clock_at_wall(std::chrono::steady_clock::time_point t) const;
  // Run `c` on the exact output sample given by `when`. Returns an id (> 0), or 0 when rejected: queue / table
  // pool full, Load / SamplerLoad (pointer commands), internal Ops, bad deck, or relative beats <= 0.
  // Beat / deck kinds are resolved on the audio thread when the entry is accepted (ClockFrame in the past runs at
  // the start of the next callback). Capacity 1024 live entries; beyond that the entry is dropped (event).
  std::uint64_t schedule(const Command& c, const Schedule& when);
  bool cancel(std::uint64_t id);      // true if it was still pending when called (no event is emitted)
  std::size_t scheduled_count() const;  // pending entries, including ones still in the command queue
  void cancel_all();

  // Call regularly from one non-real-time thread: drains events and frees retired tracks.
  bool poll(EngineEvent& out);
  void collect_garbage();

  // Offline rendering without a device (tests, tools). Interleaved float output.
  void configure_offline(std::uint32_t sample_rate, int channels);
  void render(float* out, std::uint32_t frames);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deckhand
