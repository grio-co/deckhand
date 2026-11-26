#pragma once
// Fixture profiles (.dhfix), the patch, and the mapping from an abstract look (colour, intensity, movement) to the
// channels a given fixture actually has.
#include <cstdint>
#include <string>
#include <vector>

#include "deckhand/lighting/packets.hpp"

namespace deckhand::broadcast { class Json; }

namespace deckhand::lighting {

enum class Role {
  Dimmer, Red, Green, Blue, White, Amber, UV, Strobe, Pan, PanFine, Tilt, TiltFine, ColorWheel, Gobo, Macro, Speed, Other
};
const char* role_name(Role r);
bool role_from_name(const std::string& s, Role* out);

struct WheelSlot {
  int value = 0;
  float r = 1, g = 1, b = 1;
  std::string name;
};

struct ChannelDef {
  Role role = Role::Other;
  std::string name;
  int default_value = 0;        // sent when nothing drives the channel
  int min = 0, max = 255;       // DMX range of a continuous control (dimmer, colours, ...)
  // strobe: `off` = no strobe; min..max = value range for hz_min..hz_max; `closed` (>=0) = shutter closed (blackout)
  int strobe_off = 0, strobe_min = 16, strobe_max = 250, closed = -1;
  double hz_min = 1, hz_max = 20;
  std::vector<WheelSlot> slots;  // colour wheel
};

struct Profile {
  std::string id, name;
  std::vector<ChannelDef> channels;  // channel i of the fixture = channels[i]
  double pan_range_deg = 540, tilt_range_deg = 270;
  // derived by finalize()
  bool has_dimmer = false, has_rgb = false, has_white = false, has_amber = false, has_uv = false, has_wheel = false;
  bool has_pan = false, has_tilt = false, pan16 = false, tilt16 = false;
  int channel_count() const { return static_cast<int>(channels.size()); }
  void finalize();
};

// Parse / write the .dhfix JSON format. Errors say what is wrong and where.
bool parse_profile(const deckhand::broadcast::Json& j, Profile* out, std::string* err);
bool parse_profile_text(const std::string& text, Profile* out, std::string* err);
bool load_profile_file(const std::string& path, Profile* out, std::string* err);
deckhand::broadcast::Json profile_to_json(const Profile& p);
bool parse_hex_colour(const std::string& s, float* r, float* g, float* b);

struct ProfileSet {
  std::vector<Profile> list;
  const Profile* find(const std::string& id) const;
  void add(Profile p);  // replaces a profile with the same id
  static ProfileSet with_builtins();
};
// generic-dimmer, generic-rgb, generic-rgbw, generic-rgba-par (dimmer, RGBA, strobe), generic-moving-head,
// generic-strobe, generic-uv.
const std::vector<Profile>& builtin_profiles();

struct PatchEntry {
  std::string id;       // unique label
  std::string profile;  // ProfileSet id
  int universe = 1;     // 1-based
  int address = 1;      // 1..512
  std::vector<std::string> tags;  // front, back, left, right, floor (others are kept but not used by the scenes)
  int position = 0;     // order for chases (low to high)
};

struct Issue {
  bool error = true;
  int fixture = -1;  // index into the patch, -1 = whole patch
  std::string message;
};
std::vector<Issue> validate_patch(const std::vector<PatchEntry>& patch, const ProfileSet& profiles);
inline bool has_errors(const std::vector<Issue>& v) {
  for (const auto& i : v) if (i.error) return true;
  return false;
}

constexpr std::uint32_t kTagFront = 1, kTagBack = 2, kTagLeft = 4, kTagRight = 8, kTagFloor = 16;
std::uint32_t tag_mask(const std::vector<std::string>& tags);

// What the renderer produces per fixture before channel mapping. Colour components are 0..1 and independent of
// intensity; pan/tilt 0..1.
struct Look {
  float r = 0, g = 0, b = 0;
  float intensity = 0;
  float strobe_hz = 0;
  float pan = 0.5f, tilt = 0.5f;
};
// Writes profile.channel_count() bytes. `dst` must have room for them. Allocation free.
void map_look(const Profile& p, const Look& look, std::uint8_t* dst);

struct CompiledFixture {
  int profile = 0;     // index into CompiledPatch::profiles
  int universe_slot = 0;
  int address0 = 0;    // 0-based
  float u = 0;         // 0..1 by position order, for chases and fans
  std::uint32_t tags = 0;
  int patch_index = 0;
};
struct CompiledPatch {
  std::vector<Profile> profiles;
  std::vector<CompiledFixture> fixtures;
  std::vector<int> universes;  // sorted, distinct universe numbers: slot i = universes[i]
};
// Fails (false) when validate_patch reports errors; the issues are returned either way.
bool compile_patch(const std::vector<PatchEntry>& patch, const ProfileSet& profiles, CompiledPatch* out,
                   std::vector<Issue>* issues);

}  // namespace deckhand::lighting
