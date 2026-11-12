#pragma once
// Real-time audio engine: decks, mixer, cue bus. The audio callback takes no locks,
// allocates nothing and does no I/O; everything reaches it through wait-free queues.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "deckhand/audio/track.hpp"

namespace deckhand {

constexpr int kMaxDecks = 4;
constexpr int kHotCues = 8;

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
};

struct Command {
  Op op{};
  std::uint8_t deck = 0;
  std::int32_t i = 0;
  float f = 0;
  double d = 0;
  LoadedTrack* track = nullptr;
};

enum class EventType : std::uint8_t { HotCueSet, HotCueCleared, CueSet, TrackEnded };

struct EngineEvent {
  EventType type{};
  std::uint8_t deck = 0;
  std::int32_t slot = 0;
  double frame = 0;
  std::int64_t track_id = 0;
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
};

struct MixerView {
  float crossfader = 0.5f, head_mix = 0.5f, head_level = 0.8f, master_level = 0.8f;
  float peak_left = 0, peak_right = 0;
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
