#pragma once
// Marker for modules that map the microphone commands (Op::MicOn ...).
#define DECKHAND_ENGINE_HAS_MIC 1
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

// Grid and memory cues of a loaded track, replaced as a whole while it plays (Engine::set_track_meta).
struct TrackMeta {
  double bpm = 0;                   // fallback tempo when `grid` is empty
  std::vector<GridSection> grid;    // sorted by first_beat_frame
  std::vector<double> memory_cues;  // frames, sorted
};

// Everything a deck needs for one track. Built off the audio thread, then handed over.
struct LoadedTrack {
  std::shared_ptr<const TrackAudio> audio;
  std::int64_t track_id = 0;
  std::string path;                 // source file; the engine process keeps it so a restarted UI can rebuild its decks
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
  RecStart,      // f = 1 tap before the master soft clip, 0 after, i bit 0 = 1 leaves the microphone out; needs Engine::record_open first
  RecStop,       // stops recording; the writer thread finalises the file
  StreamEnable,  // i bit 0 = 1 on / 0 off, bit 1 = 1 leaves the microphone out; f = 1 pre-soft-clip tap; feeds Engine::stream_read
  // ---- microphone channel (needs an input opened with open_input; routed to the master bus) ----
  MicOn,            // i = 1 on, 0 off, -1 toggle
  MicLevel,         // f = 0..1 (0.8 = unity)
  MicEqLow,         // f = 0..1 (0.5 = flat, +-12 dB)
  MicEqHigh,        // f = 0..1 (0.5 = flat, +-12 dB)
  MicTalkover,      // i = 1 on (ducks the music while the microphone is above the threshold), 0 off, -1 toggle on/off,
                    //   2 = always duck while set
  MicTalkoverDepth, // f = ducking in dB, 0 .. -24
  MicMonitor,       // i = 1 on, 0 off, -1 toggle (microphone also on the headphone cue bus)
  MicGate,          // f = noise gate threshold in dB, -80 .. -20; <= -80 = gate off
  // ---- track preparation: cues, loops, metronome (protocol 3) ----
  HotCueSetAt,      // i = slot, d = frame: set or move a hot cue (d < 0 deletes it); keeps a hot loop's length
  HotCueEndAt,      // i = slot, d = end frame: makes the hot cue a loop (d <= its start = plain cue again)
  CueSetAt,         // d = frame: move the main cue point
  LoopSetIn,        // d = frame: loop start (no quantise); the loop is not activated
  LoopSetOut,       // d = frame: loop end (no quantise); the loop becomes active
  Metronome,        // i = 1 on, 0 off, -1 toggle: a click on every grid beat of this deck, on the headphone bus only
  MetronomeLevel,   // f = 0..1 click level (global)
  ReplaceMeta,      // internal pointer command (Engine::set_track_meta): new grid and memory cues, audio untouched
  // ---- pre-listen player (protocol 4): one extra track on the headphone bus, never on a deck's channel ----
  PreviewPlay,      // i = 1 play, 0 pause, -1 toggle (a finished preview restarts)
  PreviewStop,      // unloads the preview track
  PreviewSeek,      // d = frame
  PreviewLevel,     // f = 0..1 (0.7 = default)
  PreviewMaster,    // i = 1 also on the master bus (off by default: a DJ must never blast the room by accident), 0 = headphones only
  PreviewLoad,      // internal pointer command (IEngine::preview_load)
  // ---- internal (scheduling); never pass these to send() / schedule() ----
  SchedInsert,     // i = payload slot handed over by Engine::schedule
  SchedCancel,     // d = schedule id
  SchedCancelAll,
  // ---- effect chains (protocol 5); internal: chain edits go through IEngine::fx_chain_edit, never over the command ring ----
  FxChainSwap,     // deck = chain target, Command::chain = prepared chain state (built by fx_chain_edit)
  FxUnitSet,       // deck = chain target, i = (FxField << 16) | unit id, f / d = value
  FxChainSet,      // deck = chain target, i = 0 bypass (f > 0.5) / 1 chain wet (f 0..1)
  Count,           // number of ops; not a command
};

enum class ColorFx : std::uint8_t { Filter = 0, Noise, DubEcho, Crush, Pitch, Space, Count };

// Effect registry. Every effect is one entry in a table in src/audio/fx.cpp.
enum class EffectKind : std::uint8_t {
  None = 0,
  Echo, PingPong, Spiral, Reverb, Flanger, Phaser, LowCutEcho, FilterSweep, Roll, Transform, Pitch,
  Crusher, VinylBrake, Backspin,
  ColorNoise, ColorDubEcho, ColorCrush, ColorPitch, ColorSpace,  // colour-slot (driven by the knob); in a chain they are plain units
  // appended (protocol 5)
  Isolator,   // 3-band isolator: param = low, param2 = mid, param3 = high (0.5 = unity, 0 = kill, 1 = +6 dB)
  Slicer,     // rhythmic slicer: param = density (hits per 8 steps), param2 = rotation, param3 = gate length
  BuildUp,    // one macro knob (param): rising high-pass + echo feedback + noise riser; released on the next beat
  Filter,     // one-knob DJ filter: param 0.5 = off, below = low-pass, above = high-pass; param2 = resonance
  Count
};

struct EffectInfo {
  EffectKind kind = EffectKind::None;
  const char* name = "";
  const char* param_name = "";  // what FxParam controls for this effect
  bool beat_synced = false;     // FxTime is a beat fraction
  bool tail = false;            // keeps ringing after it is switched off
  bool release = false;         // vinyl brake / backspin: acts while engaged, returns on release
  bool colour = false;          // the filter-knob (Op::ColorFx) kind; in an effect chain it is an ordinary unit
  float default_wet = 0.5f, default_param = 0.5f, default_beats = 1.0f;
  // appended (protocol 5)
  const char* param2_name = "";  // what FxField::Param2 controls ("" = unused)
  const char* param3_name = "";
  const char* slug = "";         // stable lower-case identifier used in chain presets
};
int effect_count();                        // number of real effects (excludes None)
const EffectInfo& effect_info_at(int i);   // 0 .. effect_count()-1
const EffectInfo* effect_info(EffectKind k);


// ---- effect chains (protocol 5) ---------------------------------------------------------------------------------
// A chain is an ordered list of up to kFxChainMax effect units, processed in series. Every bus that can carry effects
// owns exactly one chain, addressed by a "target":
//   0..3                 channel (deck) pre-fader chain          (kFxMaster = 4: master bus)
//   kFxTargetMic         microphone channel, before it joins the master bus
//   kFxTargetSampler     the sampler bus, before it joins the master bus
//   fx_target_stem(d, s) stem s (0..7) of deck d (addressing only until the stem engine feeds it)
// The three fixed slots (Op::FxSelect / FxEnable / FxWet / FxTime / FxParam, DeckView::fx, MixerView::fx) are a
// view onto the default chain of a deck or of the master bus: slot i is the unit tagged legacy_slot == i.
constexpr int kFxChainMax = 8;
constexpr int kFxTargetMic = 5;
constexpr int kFxTargetSampler = 6;
constexpr int kFxTargetStemBase = 16;
constexpr int kFxStemsPerDeck = 8;
constexpr int kFxTargetCount = kFxTargetStemBase + kMaxDecks * kFxStemsPerDeck;  // 48
constexpr int fx_target_stem(int deck, int stem) { return kFxTargetStemBase + deck * kFxStemsPerDeck + stem; }
constexpr bool fx_target_valid(int t) {
  return (t >= 0 && t <= kFxTargetSampler) || (t >= kFxTargetStemBase && t < kFxTargetCount);
}

struct FxChainState;  // prepared off the audio thread, swapped in at a block boundary (engine-internal)

// Fields of one unit that FxChainVerb::Set can change.
enum class FxField : std::uint8_t { On = 0, Wet, Param, Beats, Seconds, Param2, Param3, Count };

struct FxUnitInfo {
  std::int32_t id = 0;          // stable for the unit's life, unique within its chain
  EffectKind kind = EffectKind::None;
  std::uint8_t on = 0;
  std::int8_t legacy_slot = -1; // 0..2: this unit is fixed slot i of the default chain; -1 = user unit
  std::uint8_t pad = 0;
  float wet = 0.5f, param = 0.5f, param2 = 0.5f, param3 = 0.5f, beats = 1.0f, pad2 = 0;
  double seconds = 0;           // > 0: fixed time instead of beats
};
struct FxChainInfo {
  std::uint8_t exists = 0;      // the chain has ever held a unit
  std::uint8_t bypass = 0;
  std::int16_t pad = 0;
  std::int32_t count = 0;
  float wet = 1.0f;             // chain wet / dry
  std::uint32_t rev = 0;        // counts structural edits
  FxUnitInfo unit[kFxChainMax];
};

enum class FxChainVerb : std::uint8_t {
  Get,        // target
  Insert,     // target, pos (-1 = end), kind, value > 0.5: start on        -> id
  Remove,     // target, id                                                  (tails of delay / reverb units ring out)
  Move,       // target, id, pos (new index, clamped)
  Replace,    // target, id, kind (keeps position, id and on-state; other fields reset to the new effect's defaults)
  Set,        // target, id, field, value
  SetChain,   // target, field = 0 bypass / 1 wet, value
  Clear,      // target
  Load,       // target, text = preset (see fx_chain_to_json)               replaces the whole chain
  Save,       // target                                                      -> text
  Count
};
struct FxChainEdit {
  FxChainVerb verb = FxChainVerb::Get;
  int target = 0;
  int id = 0;
  int pos = -1;
  EffectKind kind = EffectKind::None;
  int field = 0;                // FxField (Set) or 0 / 1 (SetChain)
  double value = 0;
  std::string text;             // Load
};
struct FxChainResult {
  bool ok = false;
  int id = 0;                   // Insert: the new unit
  std::string text;             // Save: preset JSON
  std::string error;
  FxChainInfo info;             // the chain after the edit
};

// Chain presets: a small JSON blob, e.g.
//   {"v":1,"bypass":false,"wet":1,"fx":[{"type":"echo","on":true,"wet":0.5,"param":0.5,"p2":0.5,"p3":0.5,"beats":0.5,"seconds":0}]}
// Function level only; the app decides where it is stored. Unit ids and legacy tags are not part of a preset.
std::string fx_chain_to_json(const FxChainInfo& chain);
bool fx_chain_from_json(const std::string& text, FxChainInfo& out, std::string& error);
const char* effect_slug(EffectKind k);                // "ping_pong"
EffectKind effect_from_slug(const std::string& slug); // EffectKind::None when unknown

// Per-block CPU cost of effects, for tests and diagnostics (offline; allocates).
double fx_bench_ns_per_block(EffectKind kind, int block_frames, double sample_rate, int blocks);

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
  FxChainState* chain = nullptr;  // Op::FxChainSwap only (internal)
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
  // appended (protocol 3)
  double hot_cue_end[kHotCues] = {-1, -1, -1, -1, -1, -1, -1, -1};  // hot loop ends, -1 = plain cue
  std::uint32_t meta_rev = 0;   // counts grid / memory-cue replacements of the loaded track
  std::uint32_t metronome = 0;  // 1 = the deck's click is on
};

// The pre-listen player (IEngine::preview()).
struct PreviewView {
  std::uint32_t loaded = 0, playing = 0, on_master = 0, pad = 0;
  double position = 0, length = 0, sample_rate = 44100;  // frames
  std::int64_t track_id = 0;
  float level = 0.7f, pad2 = 0;
};

struct MixerView {
  float crossfader = 0.5f, head_mix = 0.5f, head_level = 0.8f, master_level = 0.8f;
  float peak_left = 0, peak_right = 0;
  // appended
  FxSlotView fx[kFxSlots];     // master effect bus
  float sampler_level = 0.8f;
  double master_bpm = 0;       // BPM driving master effects and sampler sync (0 = unknown)
  bool stream_enabled = false;
  // appended: microphone channel
  bool mic_on = false, mic_monitor = false;
  bool mic_present = false;    // an input is open and feeding the channel
  bool mic_ducking = false;    // the music is being ducked right now
  std::uint8_t mic_talkover = 0;  // 0 off, 1 on (voice-activated), 2 always
  float mic_level = 0.8f, mic_eq_low = 0.5f, mic_eq_high = 0.5f;
  float mic_depth_db = -12.0f;    // talkover ducking, 0 .. -24
  float mic_gate_db = -80.0f;     // <= -80: off
  float mic_peak = 0;             // post-level peak, 0..1
  float mic_duck_db = 0;          // current gain applied to the music, <= 0
  float mic_pad = 0;
  // appended (protocol 3)
  float metro_level = 0.5f;       // click level on the headphone bus
  float pad2 = 0;
};

// Microphone channel tuning that is not a command.
constexpr double kMicTalkoverAttack = 0.025;   // seconds (time constant of the duck gain when it goes down)
constexpr double kMicTalkoverRelease = 0.5;    // seconds (time constant when it recovers)
constexpr float kMicTalkoverThresholdDb = -40.0f;  // voice detection: microphone peak above this (after level and gate)
constexpr float kMicHighPassHz = 80.0f;

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
  bool follow_controller = false;  // separate engine process: re-pick the device when a controller with its own audio appears
  bool null_backend = false;       // tests: miniaudio's null device (paced like a real one, produces no sound)
};

// Capture device for the microphone channel. Without one the channel is silent and no capture device is ever opened.
struct InputConfig {
  std::string device_hint;  // case-insensitive substring of the device name; empty = system default input
  int channel = 0;          // first device channel (0-based)
  bool stereo = false;      // use channel and channel + 1 as a stereo pair; false = mono, sent to both sides
  bool null_backend = false;  // tests: miniaudio's null capture device (paced like a real one, delivers silence); the hint is ignored
};

struct InputStatus {
  bool open = false;        // a capture device (or an offline feed) is open and feeding the channel
  bool lost = false;        // the device went away; the engine keeps trying to reopen it
  std::string name;
  std::uint32_t sample_rate = 0;   // of the capture device
  double latency_ms = 0;           // buffered between the capture and the output clocks
  std::uint64_t underruns = 0, overruns = 0;  // clock-drift buffer: expected 0
};

// The engine as the application sees it. Implemented by the in-process Engine below and by RemoteEngine
// (deckhand/audio/remote_engine.hpp), a proxy for the engine running in its own process.
class IEngine {
 public:
  virtual ~IEngine() = default;

  virtual bool start(const OutputConfig& config, std::string& error) = 0;
  virtual void stop() = 0;
  virtual bool running() const = 0;
  virtual std::string device_name() const = 0;
  virtual int output_channels() const = 0;
  virtual std::uint32_t sample_rate() const = 0;
  virtual std::vector<std::string> devices() = 0;  // playback devices currently present

  virtual void send(const Command& c) = 0;
  virtual void load(int deck, std::unique_ptr<LoadedTrack> track) = 0;
  // Replaces the beat grid and memory cues of the track on `deck` without reloading its audio: the playhead, loops,
  // hot cues and play state stay. Quantise, sync and loops use the new grid from the next audio block on. Ignored
  // when the deck holds a different track than `track_id` (0 = whatever is loaded).
  virtual void set_track_meta(int deck, std::int64_t track_id, const TrackMeta& meta) = 0;
  // Pre-listen: hands a decoded track (audio only) to the preview player, which replaces its current track and starts
  // paused at the beginning. It plays on the headphone bus only, never on a deck or the master (unless Op::PreviewMaster).
  virtual void preview_load(std::unique_ptr<LoadedTrack> track) = 0;
  virtual PreviewView preview() const = 0;
  virtual DeckView deck(int index) const = 0;
  virtual MixerView mixer() const = 0;
  virtual SamplerView sampler(int slot) const = 0;
  virtual void sampler_load(int slot, std::unique_ptr<LoadedSample> sample) = 0;

  // include_mic: the microphone channel is part of the recording / stream tap (default).
  virtual bool record_open(const std::string& path, bool pre_clip, std::string& error, bool include_mic = true) = 0;
  virtual void record_stop() = 0;
  virtual void record_close() = 0;
  virtual RecorderStatus recorder_status() const = 0;
  virtual std::string recorder_path() const { return {}; }  // file being written, when known

  virtual void stream_enable(bool on, bool pre_clip = false, bool include_mic = true) = 0;
  virtual std::size_t stream_read(float* interleaved_stereo, std::size_t max_frames) = 0;
  virtual std::uint64_t stream_dropped_frames() const = 0;

  // Microphone input. Nothing is opened until open_input; a second capture device runs on its own clock and a
  // drift-compensating resampler joins it to the output clock. Safe from any non-real-time thread.
  virtual bool open_input(const InputConfig& config, std::string& error) = 0;
  virtual void close_input() = 0;
  virtual std::vector<std::string> input_devices() = 0;  // capture devices currently present
  virtual InputStatus input_status() const = 0;

  virtual std::uint64_t clock_frames() const = 0;
  virtual double clock_seconds() const = 0;
  virtual std::uint64_t clock_at_wall(std::chrono::steady_clock::time_point t) const = 0;
  virtual std::uint64_t schedule(const Command& c, const Schedule& when) = 0;
  virtual bool cancel(std::uint64_t id) = 0;
  virtual std::size_t scheduled_count() const = 0;
  virtual void cancel_all() = 0;

  virtual bool poll(EngineEvent& out) = 0;
  virtual void collect_garbage() = 0;

  // ---- effect chains (protocol 5) ----
  // One entry point for every structural or parameter edit (see FxChainVerb); thread-safe, never blocks the audio
  // thread. Defaults make an engine that predates chains answer "unsupported".
  virtual bool fx_chain_edit(const FxChainEdit& edit, FxChainResult& result) {
    (void)edit;
    result = FxChainResult{};
    result.error = "effect chains are not supported by this engine";
    return false;
  }
  // The chain at `target` (count 0 / exists false when it never held a unit).
  virtual FxChainInfo fx_chain(int target) const {
    (void)target;
    return FxChainInfo{};
  }
  // Convenience wrappers over fx_chain_edit.
  int fx_insert(int target, EffectKind kind, int pos = -1, bool on = false);  // unit id, 0 on failure
  bool fx_remove(int target, int id);
  bool fx_move(int target, int id, int pos);
  int fx_replace(int target, int id, EffectKind kind);
  bool fx_set(int target, int id, FxField field, double value);
  bool fx_chain_bypass(int target, bool bypass);
  bool fx_chain_wet(int target, double wet);
  bool fx_chain_clear(int target);
  std::string fx_chain_save(int target);
  bool fx_chain_load(int target, const std::string& json, std::string* error = nullptr);
};

class Engine final : public IEngine {
 public:
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Opens the output device and starts the callback. Returns false and sets error on failure.
  bool start(const OutputConfig& config, std::string& error) override;
  void stop() override;
  bool running() const override;
  std::string device_name() const override;
  int output_channels() const override;
  std::uint32_t sample_rate() const override;
  // Names of the playback devices currently present.
  static std::vector<std::string> output_devices();
  std::vector<std::string> devices() override { return output_devices(); }

  // Thread-safe for any number of non-real-time callers.
  void send(const Command& c) override;
  void load(int deck, std::unique_ptr<LoadedTrack> track) override;
  void set_track_meta(int deck, std::int64_t track_id, const TrackMeta& meta) override;
  void preview_load(std::unique_ptr<LoadedTrack> track) override;
  PreviewView preview() const override;

  DeckView deck(int index) const override;
  MixerView mixer() const override;
  SamplerView sampler(int slot) const override;

  // Sampler: hand a sample to a slot (non-real-time caller).
  void sampler_load(int slot, std::unique_ptr<LoadedSample> sample) override;

  // Recording of the master bus to 24-bit WAV (RF64 beyond 4 GB; the header is refreshed about once a
  // second, so a crash still leaves a playable file). record_open opens the file and the writer thread and
  // starts recording (Op::RecStart is sent for you). Op::RecStop / record_stop ends it; the writer then
  // finalises the file. record_close waits for that and releases the writer (call before opening again).
  bool record_open(const std::string& path, bool pre_clip, std::string& error, bool include_mic = true) override;
  void record_stop() override;
  void record_close() override;
  RecorderStatus recorder_status() const override;

  // Stream bus: a second master tap for the broadcast module. Enable with Op::StreamEnable (or
  // stream_enable), then pull interleaved stereo float frames from one non-real-time thread.
  void stream_enable(bool on, bool pre_clip = false, bool include_mic = true) override;
  std::size_t stream_read(float* interleaved_stereo, std::size_t max_frames) override;
  std::uint64_t stream_dropped_frames() const override;

  // ---- microphone input ----
  // Opens the capture device `config` names (replacing an open one). A device that goes away is closed without
  // touching the output and reopened when it returns (see InputStatus::lost). Error text in `error` on failure.
  bool open_input(const InputConfig& config, std::string& error) override;
  void close_input() override;
  std::vector<std::string> input_devices() override;
  InputStatus input_status() const override;

  // ---- engine clock and sample-accurate scheduling ----
  // Output frames rendered so far (monotonic, any thread; advances once per callback).
  std::uint64_t clock_frames() const override;
  double clock_seconds() const override;
  // Estimated engine frame being rendered at wall time `t` (extrapolated from the device callback timing; error is
  // about one callback jitter; add the device output latency yourself). Offline: just clock_frames().
  std::uint64_t clock_at_wall(std::chrono::steady_clock::time_point t) const override;
  // Run `c` on the exact output sample given by `when`. Returns an id (> 0), or 0 when rejected: queue / table
  // pool full, Load / SamplerLoad (pointer commands), internal Ops, bad deck, or relative beats <= 0.
  // Beat / deck kinds are resolved on the audio thread when the entry is accepted (ClockFrame in the past runs at
  // the start of the next callback). Capacity 1024 live entries; beyond that the entry is dropped (event).
  std::uint64_t schedule(const Command& c, const Schedule& when) override;
  bool cancel(std::uint64_t id) override;      // true if it was still pending when called (no event is emitted)
  std::size_t scheduled_count() const override;  // pending entries, including ones still in the command queue
  void cancel_all() override;

  // Call regularly from one non-real-time thread: drains events and frees retired tracks.
  bool poll(EngineEvent& out) override;
  void collect_garbage() override;
  bool fx_chain_edit(const FxChainEdit& edit, FxChainResult& result) override;
  FxChainInfo fx_chain(int target) const override;

  // Offline rendering without a device (tests, tools). Interleaved float output.
  void configure_offline(std::uint32_t sample_rate, int channels);
  void render(float* out, std::uint32_t frames);
  // Offline microphone: `in` is `frames` interleaved stereo frames at the output rate, used as the microphone input
  // of this block exactly (no device, no resampling). Counts as an open input.
  void render_with_input(float* out, const float* in, std::uint32_t frames);
  // Offline microphone on its own clock: opens a device-less input of `input_sample_rate` Hz that feed_input fills
  // (as the capture callback would); render() then resamples it to the output clock like a real device.
  void configure_offline_input(std::uint32_t input_sample_rate);
  std::size_t feed_input(const float* interleaved_stereo, std::size_t frames);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deckhand
