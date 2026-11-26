#pragma once
// The application's side of the lighting module (src/lighting): owns the LightingEngine, keeps its configuration file in
// the data folder, and feeds it the music state of the deck on air. With DECKHAND_WITH_LIGHTING=OFF the same class is a
// stub that reports "not available" and does nothing, so the rest of the application needs no #if.
//
// Everything is off by default: nothing is opened or sent until the user enables lighting AND one of its outputs.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "deckhand/analysis/analysis.hpp"

namespace deckhand {

class IEngine;

// What the show needs to know about a loaded track (copied at load).
struct LightingTrack {
  std::int64_t track_id = 0;
  std::vector<double> beats_ms;       // every beat of the grid
  int downbeat_phase = 0;             // beat numbers n with n % 4 == phase are downbeats
  std::vector<PhraseSection> phrases;
  std::uint32_t rgb = 0;              // the track's colour tag 0xRRGGBB, 0 = none
};

// Beat times and the downbeat phase from a grid given as sections (first beat ms, bpm, bar beat 1..4 of the first beat).
struct LightingGridSection {
  double first_beat_ms = 0, bpm = 0;
  int bar_beat = 1;
};
void lighting_beats_from_grid(const std::vector<LightingGridSection>& grid, double length_ms, std::vector<double>& beats_ms, int& downbeat_phase);

class LightingBridge {
 public:
  LightingBridge();
  ~LightingBridge();
  LightingBridge(const LightingBridge&) = delete;
  LightingBridge& operator=(const LightingBridge&) = delete;

  static bool compiled();  // false in a build without the lighting module

  // Reads <data_dir>/lighting.json (missing = defaults, everything off) and starts the engine only when it says enabled.
  void open(const std::string& data_dir);
  void close();

  void set_track(int deck, int key_index, LightingTrack track);
  void clear_track(int deck);
  // Samples the deck on air and gives the show its state. Call from the application's tick (about 30 Hz); cheap.
  void tick(const IEngine& engine);

  // JSON text. status: what runs and what it sends; config: the whole configuration; preview: one swatch per fixture.
  std::string status_json() const;
  std::string config_json() const;
  std::string preview_json() const;
  // Merges the given (possibly partial) object into the configuration, validates, saves and applies it.
  bool set_config_json(const std::string& text, std::string& error);
  // {"action": "blackout|hold|master|strobe|enable|override|release", ...}
  bool command_json(const std::string& text, std::string& error);

  // Controls lighting.blackout / .hold / .master / .strobe / .enable. has_value false = toggle (or press).
  bool control(const std::string& action, bool has_value, double value, std::string& error);
  // The value a control shows, false when unknown.
  bool read_control(const std::string& action, double& value) const;
  bool enabled() const;
  // Short text for a status row ("Lighting: Art-Net 3 universes", "" when off).
  std::string describe() const;
  // The same sentence as plain data for the UI: blackout / hold / master / strobe allowed.
  struct UiState {
    bool available = false, enabled = false, running = false, blackout = false, hold = false, strobes = true;
    float master = 1;
    bool artnet = false, sacn = false, serial = false;
    std::string artnet_host, sacn_host;
    int artnet_first_universe = 0;
    bool sacn_multicast = true;
    std::vector<unsigned> swatches;  // 0xRRGGBB, at most 16 fixtures, for the preview row
  };
  UiState ui_state() const;
  // Output and strobe settings for the Tools tab (partial config changes through the same path as set_config_json).
  bool set_output(const std::string& which, bool on, const std::string& host, int universe, bool multicast, std::string& error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deckhand
