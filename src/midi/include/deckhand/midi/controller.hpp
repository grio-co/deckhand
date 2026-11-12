#pragma once
// Generic controller support. A controller is described by a declarative mapping file
// (no code); any connected MIDI device whose name matches a mapping is driven by it.
#include <cstdint>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "deckhand/audio/engine.hpp"

namespace deckhand {

// ---- platform MIDI access (implemented in platform/<os>) ---------------------------
class MidiIO {
 public:
  using Receive = std::function<void(const std::string& device, const std::uint8_t* data, std::size_t size)>;
  using Changed = std::function<void()>;
  virtual ~MidiIO() = default;
  virtual std::vector<std::string> devices() = 0;  // names of connected input devices
  virtual void send(const std::string& device, const std::uint8_t* data, std::size_t size) = 0;
};
// Callbacks may arrive on any thread.
std::unique_ptr<MidiIO> create_midi_io(MidiIO::Receive receive, MidiIO::Changed changed);

// ---- mapping ----------------------------------------------------------------------
enum class InKind : std::uint8_t { Button, Abs, Abs14, Rel, Rel2 };

enum class Action : std::uint8_t {
  None, Play, Cue, Sync, TempoRange, SeekStart, JogTouch, Jog, Tempo, LoopIn, LoopOut, LoopToggle, Reloop,
  CueCall, BeatLoop, BeatJump, HotCue, HotCueClear, Trim, EqHigh, EqMid, EqLow, Filter, Fader, Crossfader,
  HeadMix, HeadLevel, MasterLevel, Pfl, Browse, BrowsePress, Load, PadMode,
  // appended
  KeyLock, KeyShift, Quantize, Slip, Reverse, LoopMove, LoopExit, LoopHalve, LoopDouble, AutoGain, ColorFx,
  FxSelect, FxOn, FxWet, FxParam, FxTime, Sampler, SamplerStop, SamplerLevel, MasterBpm,
  Keyboard,     // pad: key shift to a semitone, then play the hot cue pressed last
  KeyShiftPad,  // pad: set the key shift (default from the pad number)
  PadFx,        // pad: momentary effect (on while held)
  Modifier,     // held button that changes what rules with when=<name> do
};

enum class LedState : std::uint8_t {
  Playing, Cue, Sync, Pfl, Loop, LoopIn, HotCue, Loaded, PadMode, Level,
  // appended
  KeyLock, Quantize, Slip, Reverse, FxOn, SamplerPlaying, LoopBeats, KeyShift, Modifier,
};

struct InRule {
  InKind kind{};
  std::uint8_t status = 0, data1 = 0, data1_lsb = 0;
  Action action = Action::None;
  int deck = 0;         // 0-based
  int slot = 0;         // 0-based
  int mode = -1;        // pad mode the rule is limited to; -1 = any (pad_mode: the mode it selects)
  bool top = false;     // jog: platter top (scratches when touched)
  bool invert = false;
  float amount = 1;     // beats, direction or scale, depending on the action
  // appended
  bool has_deck = false, has_slot = false, has_beats = false, has_dir = false, has_value = false;
  bool has_range = false, master_unit = false, hold = false;
  float value = 0;                // semitones / BPM / dB / on-off value (value= or semitones=)
  float lo = 0, hi = 1;           // range an absolute control sweeps (lo= hi=)
  float deadzone = -1;            // -1 = mapping default
  int pickup = -1;                // -1 = mapping default, 0 off, 1 soft takeover
  int effect = -1;                // EffectKind (fx_select, padfx) or ColorFx (color_fx); -1 = none
  int pad = -1, offset = 0;       // sampler slot offset
  int when = -1;                  // modifier index the rule needs; -1 = none
  bool when_not = false;
  int modifier = -1;              // action modifier: which one it holds
};

struct OutRule {
  std::uint8_t status = 0, data1 = 0;
  LedState state{};
  int deck = 0, slot = 0, mode = -1;   // mode: pad mode the LED belongs to (pad_mode state: the mode it shows)
  std::uint8_t on = 0x7F, off = 0x00;  // for Level: on = maximum value, off = minimum lit value
  bool blink = false;
  // appended
  float amount = 1, value = 0;
  bool has_value = false, master_unit = false;
  int effect = -1, offset = 0, modifier = -1;
};

struct Mapping {
  std::string name;
  std::string match;        // case-insensitive substring of the MIDI device name
  std::string audio_match;  // substring of the audio device name; empty = no preference
  int master_channel = 0;   // 0-based first channel of each stereo pair
  int phones_channel = 2;
  double ticks_per_rev = 720;
  std::vector<std::vector<std::uint8_t>> init;  // messages sent when the device connects
  std::vector<InRule> in;
  std::vector<OutRule> out;
  // appended
  std::vector<std::string> modifiers;  // names used by modifier / when= / name=
  bool pickup_default = false;         // soft takeover for every absolute control
  float deadzone_default = 0;          // centre dead zone (0..1) for every absolute control
  bool generated = false;              // auto-created empty mapping for an unknown device (MIDI learn)
  std::multimap<std::uint16_t, std::size_t> index;  // (status << 8 | data1) -> rule
};

// Returns false and sets error (with a line number) when the text is not a valid mapping.
bool parse_mapping(const std::string& text, Mapping& out, std::string& error);
// Writes or updates <folder>/<device>.dhmap with the given "in"/"out" lines. A line replaces an existing
// one with the same control or the same action; everything else in the file is kept. A new file gets
// name and match lines for the device.
bool save_user_mapping(const std::string& device, const std::vector<std::string>& lines, const std::string& folder,
                       std::string* error = nullptr);
// Loads every *.dhmap in the given folders; later folders override earlier ones by name.
std::vector<Mapping> load_mappings(const std::vector<std::string>& folders);

// ---- where controller input goes -----------------------------------------------------
class ControlSink {
 public:
  virtual ~ControlSink() = default;
  virtual void command(const Command& c) = 0;
  virtual void browse(int steps) = 0;
  virtual void browse_press() = 0;
  virtual void load_selected(int deck) = 0;
};

enum class LearnState : std::uint8_t { Idle, Waiting, Observing, Done, Failed };

class ControllerHub {
 public:
  ControllerHub(ControlSink& sink, std::vector<Mapping> mappings);
  ~ControllerHub();

  void set_io(MidiIO* io);
  void devices_changed();  // rescan and (de)activate mappings
  void midi(const std::string& device, const std::uint8_t* data, std::size_t size);
  // LED and meter feedback; call regularly from one thread.
  void tick(const DeckView* decks, int deck_count, double now_seconds);
  // Same, with the mixer (master effects, sampler level) and sampler slot states for their LEDs.
  void tick(const DeckView* decks, int deck_count, const MixerView* mixer, const SamplerView* samplers,
            int sampler_count, double now_seconds);

  // ---- MIDI learn ---------------------------------------------------------------------
  // action = what the control should do, as written after the control in a mapping line, e.g.
  // "play deck=1" or "filter deck=2". The next control touched on any device is captured (and not
  // dispatched). Returns false (see learn_error) when the action is unknown.
  bool learn_begin(const std::string& action);
  void learn_cancel();
  LearnState learn_state() const;
  // The mapping line ("in abs14 B0 00 20 tempo deck=1"), or "" while still waiting. A note, or a CC
  // for a button action, is captured at once; other CCs are observed for a short sequence (12 messages,
  // 0.4 s after the last one, or finalize=true) to tell abs / abs14 / rel / rel2 apart.
  std::string learn_result(bool finalize = false);
  std::string learn_device() const;
  std::string learn_error() const;
  // Adds the captured rule to that device's live mapping so it works at once. Save it with
  // save_user_mapping to keep it.
  bool learn_commit();
  std::vector<std::string> connected() const;  // mapping names in use
  // Audio routing wanted by a connected controller, if any.
  bool audio_hint(std::string& device_match, int& master_channel, int& phones_channel) const;

 private:
  struct Active {
    std::string device;
    Mapping* map = nullptr;
    std::vector<std::uint8_t> msb, lsb;
    std::map<std::uint16_t, int> last_out;  // (status << 8 | data1) -> last byte sent
    int pad_mode[kMaxDecks] = {0, 0, 0, 0};
    int range_index[kMaxDecks] = {0, 0, 0, 0};
    double jog_ticks[kMaxDecks] = {0, 0, 0, 0};
    int last_cue[kMaxDecks] = {-1, -1, -1, -1};     // hot cue pressed last (keyboard pads)
    std::vector<std::array<bool, kMaxDecks + 1>> held;  // modifiers: [0] global, [1 + deck] per deck
    std::vector<char> down, engaged;                // per rule: pressed / soft takeover engaged
    std::vector<float> last_v;                      // per rule: previous absolute value (-1 = none)
    std::vector<int> aux;                           // per rule: cue held by a keyboard pad
  };
  struct Cache {  // last engine state seen in tick(), updated optimistically by our own commands
    DeckView deck[kMaxDecks];
    bool have_deck[kMaxDecks] = {false, false, false, false};
    MixerView mixer;
    bool have_mixer = false;
  };
  struct Learn;
  void fit(Active& a);
  bool allowed(const Active& a, const InRule& r) const;
  bool held(const Active& a, const std::string& name, int deck) const;
  void handle(Active& a, std::uint8_t status, std::uint8_t d1, std::uint8_t d2);
  void button(Active& a, std::size_t idx, const InRule& r, bool pressed);
  void value(Active& a, std::size_t idx, const InRule& r, float v);
  void delta(Active& a, const InRule& r, int steps);
  bool engine_value(const InRule& r, float& e) const;
  FxSlotView& fx_slot(bool master, int deck, int slot);
  bool learn_feed(const std::string& device, std::uint8_t status, std::uint8_t d1, std::uint8_t d2);
  void learn_finish();

  ControlSink& sink_;
  std::deque<Mapping> mappings_;  // stable addresses: Active points into it
  Cache cache_;
  std::unique_ptr<Learn> learn_;
  MidiIO* io_ = nullptr;
  mutable std::mutex mutex_;
  std::vector<Active> active_;
};

}  // namespace deckhand
