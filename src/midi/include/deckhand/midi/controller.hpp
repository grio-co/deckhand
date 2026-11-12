#pragma once
// Generic controller support. A controller is described by a declarative mapping file
// (no code); any connected MIDI device whose name matches a mapping is driven by it.
#include <cstdint>
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
};

enum class LedState : std::uint8_t { Playing, Cue, Sync, Pfl, Loop, LoopIn, HotCue, Loaded, PadMode, Level };

struct InRule {
  InKind kind{};
  std::uint8_t status = 0, data1 = 0, data1_lsb = 0;
  Action action = Action::None;
  int deck = 0;         // 0-based
  int slot = 0;         // 0-based
  int mode = 0;
  bool top = false;     // jog: platter top (scratches when touched)
  bool invert = false;
  float amount = 1;     // beats, direction or scale, depending on the action
};

struct OutRule {
  std::uint8_t status = 0, data1 = 0;
  LedState state{};
  int deck = 0, slot = 0, mode = 0;
  std::uint8_t on = 0x7F, off = 0x00;  // for Level: on = maximum value, off = minimum lit value
  bool blink = false;
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
  std::multimap<std::uint16_t, std::size_t> index;  // (status << 8 | data1) -> rule
};

// Returns false and sets error (with a line number) when the text is not a valid mapping.
bool parse_mapping(const std::string& text, Mapping& out, std::string& error);
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

class ControllerHub {
 public:
  ControllerHub(ControlSink& sink, std::vector<Mapping> mappings);

  void set_io(MidiIO* io);
  void devices_changed();  // rescan and (de)activate mappings
  void midi(const std::string& device, const std::uint8_t* data, std::size_t size);
  // LED and meter feedback; call regularly from one thread.
  void tick(const DeckView* decks, int deck_count, double now_seconds);

  std::vector<std::string> connected() const;  // mapping names in use
  // Audio routing wanted by a connected controller, if any.
  bool audio_hint(std::string& device_match, int& master_channel, int& phones_channel) const;

 private:
  struct Active {
    std::string device;
    const Mapping* map = nullptr;
    std::vector<std::uint8_t> msb, lsb;
    std::vector<int> last_out;
    int pad_mode[kMaxDecks] = {0, 0, 0, 0};
    int range_index[kMaxDecks] = {0, 0, 0, 0};
    double jog_ticks[kMaxDecks] = {0, 0, 0, 0};
  };
  void handle(Active& a, std::uint8_t status, std::uint8_t d1, std::uint8_t d2);
  void button(Active& a, const InRule& r, bool pressed);
  void value(const InRule& r, float v);
  void delta(Active& a, const InRule& r, int steps);

  ControlSink& sink_;
  std::vector<Mapping> mappings_;
  MidiIO* io_ = nullptr;
  mutable std::mutex mutex_;
  std::vector<Active> active_;
};

}  // namespace deckhand
