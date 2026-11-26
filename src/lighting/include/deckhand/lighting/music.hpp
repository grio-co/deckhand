#pragma once
// The musical state the show reacts to, and the helper that derives it from a track's beat grid and phrase analysis.
#include <cstdint>
#include <string>
#include <vector>

#include "deckhand/analysis/analysis.hpp"

namespace deckhand::lighting {

enum class Section : std::uint8_t { Unknown, Intro, Build, Drop, Breakdown, Verse, Chorus, Bridge, Outro };
const char* section_name(Section s);
// From PhraseSection::label (used when its confidence is at least 0.3) or else ::kind (intro, up, down, chorus, outro).
Section section_from_phrase(const std::string& label, double label_confidence, const std::string& kind);

// Sampled by the application at its tick (any rate from ~10 Hz up) for the deck that drives the lights.
struct MusicState {
  bool playing = false;            // the driving deck is playing and audible
  double stamp_s = 0;              // monotonic seconds at which this was sampled (same clock as LightingEngine::tick)
  double bpm = 0;                  // effective tempo including pitch/speed; <= 0 = no grid (the show free-runs at 120)
  double beat_pos = 0;             // beats since the grid's first beat, fractional: fract() is the beat phase
  int downbeat_phase = 0;          // beat numbers n with n % 4 == downbeat_phase are downbeats
  int section_index = -1;          // index in the phrase list, -1 = no phrase analysis
  Section section = Section::Unknown;
  int energy = 5;                  // 1..10
  double section_elapsed_s = 0;    // real seconds since the section began
  double time_to_boundary_s = -1;  // real seconds until the section ends, < 0 = unknown
  bool has_next = false;           // a following section exists
  Section next_section = Section::Unknown;
  int next_energy = 5;
  bool levels_valid = false;       // level/low/mid/high are real measurements
  float level = 0, low = 0, mid = 0, high = 0;  // 0..1
  std::uint32_t track_rgb = 0;     // 0xRRGGBB colour tag of the loaded track, 0 = none
  int key_index = -1;              // analysis key index 0..23 (12..23 = minor), -1 = unknown
};

// A loaded track's grid and sections, copied once at load; locate() is then allocation free.
class TrackTimeline {
 public:
  void set_track(const std::vector<double>& beats_ms, int downbeat_phase, const std::vector<deckhand::PhraseSection>& sections);
  void clear();
  bool has_grid() const { return beats_.size() >= 2; }
  // position_ms: playhead in track time. speed: playback rate (1 = normal). Fills the grid and section fields of `out`
  // (playing, stamp_s, levels, colour and key are the caller's).
  void locate(double position_ms, double speed, MusicState* out) const;

 private:
  struct Sec { double start_ms, end_ms; Section section; int energy; };
  std::vector<double> beats_;
  std::vector<Sec> secs_;
  int phase_ = 0;
};

// Camelot-wheel number (1..12) and mode of an analysis key index; false when out of range.
bool camelot_of_key(int key_index, int* number, bool* minor);

}  // namespace deckhand::lighting
