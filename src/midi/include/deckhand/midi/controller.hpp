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

// ---- platform HID access (implemented in platform/<os>; dormant until the app creates one) -----
// Which devices a mapping (or HID learn) wants. -1 = any. Only devices matching one entry are enumerated.
struct HidMatch {
  int vid = -1, pid = -1, usage_page = -1, usage = -1, interface_number = -1;
};
struct HidDeviceInfo {
  std::string id;  // stable while the device stays connected; opaque to callers
  std::string product, manufacturer;
  int vid = 0, pid = 0, usage_page = 0, usage = 0, interface_number = -1;
};
// Keyboards, keypads, mice and pointers are never opened (usage page 0x01 usages 1, 2, 6, 7 and page 0x07).
bool hid_usage_refused(int usage_page, int usage);
bool hid_matches(const HidMatch& m, const HidDeviceInfo& d);

// Report ids are passed separately: `data` never contains the report id byte (id 0 = the device has no
// numbered reports). Callbacks may arrive on any thread and are never made from inside a call to this
// interface. open/close/send_* are safe to call from the Changed callback and from any thread; close does
// not wait for a running Receive callback.
class HidIO {
 public:
  using Receive = std::function<void(const std::string& id, int report_id, const std::uint8_t* data, std::size_t size)>;
  using Changed = std::function<void()>;
  virtual ~HidIO() = default;
  // Restricts enumeration: only devices matching an entry are listed (an empty list lists nothing).
  // Nothing is opened by enumerating.
  virtual void set_matches(const std::vector<HidMatch>& matches) = 0;
  virtual std::vector<HidDeviceInfo> devices() = 0;
  virtual bool open(const std::string& id) = 0;  // refuses refused usages; starts delivering input reports
  virtual void close(const std::string& id) = 0;
  virtual bool send_output(const std::string& id, int report_id, const std::uint8_t* data, std::size_t size) = 0;
  virtual bool send_feature(const std::string& id, int report_id, const std::uint8_t* data, std::size_t size) = 0;
  virtual bool get_feature(const std::string& id, int report_id, std::vector<std::uint8_t>& out) = 0;
};
// nullptr where HID is unavailable (iOS). Hook up: hub.hid(id, report_id, data, size) as Receive and
// hub.devices_changed() as Changed, then hub.set_hid_io(io.get()).
std::unique_ptr<HidIO> create_hid_io(HidIO::Receive receive, HidIO::Changed changed);

// ---- mapping ----------------------------------------------------------------------
enum class InKind : std::uint8_t {
  Button, Abs, Abs14, Rel, Rel2,
  // HID report fields: HidButton (also `touch`), HidAbs (normalised 0..1), HidRel (wrapping running
  // position, delta with wrap handling), HidDelta (the field is itself a signed per-report delta)
  HidButton, HidAbs, HidRel, HidDelta,
};

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
  FxAssign,     // unit=N target=deck1..4|master: routes effect unit N to that channel
  MasterCue,    // headphone monitoring of the master (toggle; remembers and restores the head mix)
  MicOn, MicLevel, MicEqLow, MicEqHigh, MicTalkover,  // microphone channel (engine commands: DECKHAND_ENGINE_HAS_MIC)
};

enum class LedState : std::uint8_t {
  Playing, Cue, Sync, Pfl, Loop, LoopIn, HotCue, Loaded, PadMode, Level,
  // appended
  KeyLock, Quantize, Slip, Reverse, FxOn, SamplerPlaying, LoopBeats, KeyShift, Modifier,
  FxAssign, MasterCue, ColorFx,  // FxAssign: unit N is assigned to target; ColorFx: effect selected (default: any but filter)
  MicOn, MicTalkover,             // the hub's own view of the last mic command it sent
};

// Where a rule reads (or writes) in a HID report. `byte` counts from the first byte after the report id;
// the field is `size` bytes (1..4, `endian`), of which bits lo..hi (LSB = 0) are used.
struct HidField {
  int report = 0, byte = 0, size = 1, lo = 0, hi = 7;
  bool big = false, is_signed = false;
  bool has_min = false, has_max = false;
  double vmin = 0, vmax = 0;  // raw range of an absolute field (default: the full range of the width)
  int width() const { return hi - lo + 1; }
};

struct InRule {
  InKind kind{};
  HidField hid;               // HID kinds only
  double ticks_per_rev = 0;   // jog rules: counts per revolution; 0 = the mapping's jog ticks_per_rev
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
  int fx_unit = -1;               // effect unit (0-based) whose assignment the effect actions follow; -1 = deck= / unit=master
  int fx_target = -1;             // fx_assign: 0..3 deck, kFxMaster
};

struct OutRule {
  std::uint8_t status = 0, data1 = 0;
  bool hid = false;  // HID output field (hid_field); status/data1 unused
  HidField hid_field;
  LedState state{};
  int deck = 0, slot = 0, mode = -1;   // mode: pad mode the LED belongs to (pad_mode state: the mode it shows)
  int on = 0x7F, off = 0x00;  // (a byte for MIDI, up to the field width for HID) for Level: on = maximum value, off = minimum lit value
  bool blink = false;
  // appended
  float amount = 1, value = 0;
  bool has_value = false, master_unit = false;
  int effect = -1, offset = 0, modifier = -1;
  int fx_unit = -1, fx_target = -1;  // fx_on: follow unit N's assignment; fx_assign: lit while unit N is on target
};

// One output report of a HID device: the hub composes it from this template plus the out rules.
struct HidOutTemplate {
  int report = 0, size = 0;            // payload size (without the report id)
  std::vector<std::uint8_t> init;      // starting contents (zero-padded to size); bytes no rule claims keep them
  double interval = 0.02;              // seconds between sends of this report
};
struct HidInit {
  bool feature = false;
  int report = 0;
  std::vector<std::uint8_t> data;
};

struct Mapping {
  std::string name;
  std::string match;        // case-insensitive substring of the MIDI device name (empty for a HID-only mapping)
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
  // HID (a mapping may have a MIDI side, a HID side, or both)
  bool hid = false;
  HidMatch hid_match;
  std::vector<HidOutTemplate> hid_out;
  std::vector<HidInit> hid_init;
  std::map<int, std::vector<std::size_t>> hid_index;  // report id -> input rules
};

// Returns false and sets error (with a line number) when the text is not a valid mapping.
bool parse_mapping(const std::string& text, Mapping& out, std::string& error);
// Writes or updates <folder>/<device>.dhmap with the given "in"/"out" lines. A line replaces an existing
// one with the same control or the same action; everything else in the file is kept. A new file gets
// name and match lines for the device.
// header: extra lines for a new file instead of the match line (HID learn: ControllerHub::learn_header()).
bool save_user_mapping(const std::string& device, const std::vector<std::string>& lines, const std::string& folder,
                       std::string* error = nullptr, const std::string& header = std::string());
// Loads every *.dhmap in the given folders; later folders override earlier ones by name.
std::vector<Mapping> load_mappings(const std::vector<std::string>& folders);

// Effect units a mapping can assign (unit=1..kFxUnits). Unit N starts out on the master bus.
constexpr int kFxUnits = 4;

// One entry for a MIDI-learn picker: label "Play" in group "Deck 1" (English; the app translates groups).
struct LearnChoice {
  std::string label;   // e.g. "Play", "Hot cue 3"
  std::string action;  // text for ControllerHub::learn_begin, e.g. "play deck=1"
  std::string group;   // e.g. "Deck 1", "Mixer", "Effect unit 1"
};
std::vector<LearnChoice> learn_choices(int decks = 2);

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
  void set_hid_io(HidIO* io);  // optional; HID devices work like MIDI ones (connected(), audio hints, LEDs ...)
  void devices_changed();  // rescan and (de)activate mappings (MIDI and HID)
  void midi(const std::string& device, const std::uint8_t* data, std::size_t size);
  // One HID input report (data without the report id). Reports too short for a rule are ignored by it.
  void hid(const std::string& device, int report_id, const std::uint8_t* data, std::size_t size);
  // HID learn for devices no mapping names: off by default (no unmapped device is ever enumerated).
  // gamepads = also list joystick / game pad / multi-axis devices; hid_learn_allow adds one vid/pid.
  void hid_learn_gamepads(bool on);
  void hid_learn_allow(int vid, int pid);
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
  // HID: "hid vid=0x.... pid=0x...." for save_user_mapping's header argument ("" for a MIDI device).
  std::string learn_header() const;
  // Adds the captured rule to that device's live mapping so it works at once. Save it with
  // save_user_mapping to keep it.
  bool learn_commit();
  std::vector<std::string> connected() const;  // mapping names in use
  // Audio routing wanted by a connected controller, if any.
  bool audio_hint(std::string& device_match, int& master_channel, int& phones_channel) const;

 private:
  struct Active {
    std::string device;
    bool hid = false;
    Mapping* map = nullptr;
    std::vector<std::int64_t> hid_prev;             // per rule: last raw field value
    std::vector<char> hid_seen;
    std::vector<std::vector<std::uint8_t>> hid_sent;  // per output template: last report sent
    std::vector<double> hid_time;
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
  struct HidTrack;  // last report of a device/report id, and which of its bytes change all the time
  void fit(Active& a);
  bool allowed(const Active& a, const InRule& r) const;
  bool held(const Active& a, const std::string& name, int deck) const;
  void handle(Active& a, std::uint8_t status, std::uint8_t d1, std::uint8_t d2);
  void handle_hid(Active& a, int report_id, const std::uint8_t* data, std::size_t size);
  void dispatch_button(Active& a, std::size_t idx, const InRule& r, bool pressed);
  void midi_devices_changed(const std::vector<std::string>& names);
  void hid_devices_changed(const std::vector<HidDeviceInfo>& infos);
  void hid_set_matches();
  void hid_send_out(Active& a, const std::map<std::array<int, 6>, int>& want, double now);
  bool learn_feed_hid(const std::string& device, int report_id, const std::uint8_t* data, std::size_t size,
                      HidTrack& track);
  void learn_finish_hid();
  void button(Active& a, std::size_t idx, const InRule& r, bool pressed);
  void value(Active& a, std::size_t idx, const InRule& r, float v);
  void delta(Active& a, const InRule& r, int steps);
  bool engine_value(const InRule& r, float& e) const;
  FxSlotView& fx_slot(int target, int slot);        // target: 0..3 deck, kFxMaster
  int fx_target(const InRule& r) const;              // channel an effect rule addresses right now
  void fx_assign(int unit, int target);              // switch off on the old target, carry the settings over
  bool learn_feed(const std::string& device, std::uint8_t status, std::uint8_t d1, std::uint8_t d2);
  void learn_finish();

  ControlSink& sink_;
  std::deque<Mapping> mappings_;  // stable addresses: Active points into it
  Cache cache_;
  std::unique_ptr<Learn> learn_;
  MidiIO* io_ = nullptr;
  HidIO* hid_io_ = nullptr;
  std::map<std::pair<std::string, int>, std::shared_ptr<HidTrack>> hid_tracks_;
  bool learn_gamepads_ = false;
  std::vector<std::pair<int, int>> learn_allow_;
  std::vector<HidDeviceInfo> hid_known_;  // devices seen by the last scan (learn_header, names)
  bool mic_on_ = false, mic_talk_ = false;  // last mic command the hub sent (LED states mic_on / mic_talkover)
  mutable std::mutex mutex_;
  std::vector<Active> active_;
  int fx_assigned_[kFxUnits] = {kFxMaster, kFxMaster, kFxMaster, kFxMaster};
  bool master_cue_ = false;   // master_cue is on (head mix forced to master)
  float master_cue_saved_ = 0.5f;
};

}  // namespace deckhand
