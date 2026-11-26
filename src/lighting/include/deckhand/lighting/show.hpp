#pragma once
// The show engine: turns MusicState into DMX frames. Single threaded (LightingEngine serialises access). After
// configure() the render path does no heap allocation.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "deckhand/lighting/fixture.hpp"
#include "deckhand/lighting/music.hpp"
#include "deckhand/lighting/output.hpp"

namespace deckhand::lighting {

enum class SceneId : std::uint8_t { Wash, Pulse, Chase, ColourCycle, BuildRamp, Calm, Blackout };
enum class MoveId : std::uint8_t { None, Circle, TiltSweep, PanFan };
const char* scene_name(SceneId s);
bool scene_from_name(const std::string& n, SceneId* out);
const char* move_name(MoveId m);
bool move_from_name(const std::string& n, MoveId* out);

struct Rgb { float r = 0, g = 0, b = 0; };
enum class PaletteMode : std::uint8_t { User, Track, Key };
const char* palette_mode_name(PaletteMode m);

struct PaletteConfig {
  PaletteMode mode = PaletteMode::User;  // Track: from the track's colour tag, else its key, else the user colours
  std::vector<Rgb> colours;              // 1..8 colours; empty = the built-in default palette
};

struct ShowConfig {
  float master = 1.0f;            // 0..1 global dimmer
  bool strobe_enabled = true;     // false = no strobing at all, from any scene or override
  float strobe_max_hz = 10.0f;    // hard cap, 0..25 (never above refresh_hz / 4)
  double latency_ms = 0;          // the whole show runs this far ahead to hide output/fixture latency
  double crossfade_s = 1.5;       // soft scene changes; hard hits (drops) cut
  double stop_fade_s = 0.6;       // fade to black when the music stops
  double stale_s = 2.0;           // no new MusicState for this long = treated as stopped
  std::uint64_t seed = 1;         // the director's variety is a pure function of seed and the track's phrase order
  double refresh_hz = 40;
  bool auto_movement = true;
  PaletteConfig palette;
};

struct Override {
  bool has_scene = false, has_move = false, has_colour = false, has_intensity = false, has_strobe = false;
  SceneId scene = SceneId::Wash;
  MoveId move = MoveId::None;
  Rgb colour{1, 1, 1};
  float intensity = 1;     // 0..1
  float strobe_hz = 0;     // requested; still capped
  bool any() const { return has_scene || has_move || has_colour || has_intensity || has_strobe; }
};

struct ShowStatus {
  SceneId scene = SceneId::Blackout;
  MoveId move = MoveId::None;
  Section section = Section::Unknown;
  int section_index = -1;
  int phrase_counter = 0;
  float crossfade = 1;  // 0..1 progress of the current scene change (1 = settled)
  bool active = false, blackout = false, hold = false, override_active = false, hit = false;
  float strobe_hz = 0;  // highest strobe rate in the last frame, after the cap
  float master = 1;
  double beat_pos = 0;
};

class ShowEngine {
 public:
  // Allocates. Resets the show.
  void configure(const ShowConfig& cfg, CompiledPatch patch);
  // Cheap runtime change of the settings (palette vector assignment may allocate; do not call per frame).
  void set_config(const ShowConfig& cfg);
  const ShowConfig& config() const { return cfg_; }

  void set_music(const MusicState& m);
  // Renders the frame for time `now_s` (same clock as MusicState::stamp_s).
  void render(double now_s);

  void set_blackout(bool on) { blackout_ = on; }
  void set_hold(bool on);
  void set_master(float v);
  void set_strobe(bool enabled, float max_hz);
  void set_override(const Override& o) { ovr_ = o; }
  void clear_override() { ovr_ = Override{}; }
  const Override& override_state() const { return ovr_; }

  const std::vector<UniverseFrame>& frames() const { return frames_; }
  const std::vector<Look>& looks() const { return final_; }
  ShowStatus status() const { return st_; }
  const CompiledPatch& patch() const { return patch_; }
  Rgb palette_colour(int i) const { return pal_[static_cast<std::size_t>(((i % pal_n_) + pal_n_) % pal_n_)]; }

 private:
  struct Selection {
    SceneId scene = SceneId::Blackout;
    MoveId move = MoveId::None;
    bool hit = false;
    float intensity = 0.7f;
  };
  struct Ctx;
  void update_palette();
  Selection choose(Section s, int energy, int counter) const;
  void begin_fade(double dur_s);
  void eval_scene(SceneId s, const Ctx& c, std::vector<Look>& out) const;
  void eval_move(MoveId m, const Ctx& c, std::vector<Look>& out) const;

  ShowConfig cfg_;
  CompiledPatch patch_;
  std::vector<UniverseFrame> frames_;
  std::vector<Look> cur_, from_, mix_, held_, final_;
  std::vector<float> sphase_;
  std::vector<char> has_strobe_;
  std::array<Rgb, 8> pal_{};
  int pal_n_ = 1;
  std::uint32_t pal_rgb_ = 0xffffffffu;
  int pal_key_ = -2;
  PaletteMode pal_mode_ = PaletteMode::User;
  std::size_t pal_cfg_hash_ = 0;

  MusicState music_;
  bool have_music_ = false;
  Selection sel_;
  SceneId last_eff_scene_ = SceneId::Blackout;
  int applied_ = -3;  // section index (or -3 = stopped) the selection was last made for
  int counter_ = 0;
  double fade_dur_ = 0, fade_t_ = 0;
  bool fading_ = false;
  double last_now_ = 0, free_beats_ = 0;
  bool started_ = false;
  bool blackout_ = false, hold_ = false, hold_pending_ = false;
  Override ovr_;
  ShowStatus st_;
};

}  // namespace deckhand::lighting
