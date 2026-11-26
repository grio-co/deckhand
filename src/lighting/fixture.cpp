#include "deckhand/lighting/fixture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <set>
#include <sstream>

#include "deckhand/broadcast/json.hpp"

namespace deckhand::lighting {

using deckhand::broadcast::Json;

namespace {
const char* const kRoleNames[] = {"dimmer", "red", "green", "blue", "white", "amber", "uv", "strobe", "pan", "pan_fine",
                                  "tilt", "tilt_fine", "colorwheel", "gobo", "macro", "speed", "other"};
int clamp_dmx(double v) { return static_cast<int>(std::clamp(std::lround(v), 0L, 255L)); }
float c01(float v) { return std::clamp(v, 0.0f, 1.0f); }
}  // namespace

const char* role_name(Role r) { return kRoleNames[static_cast<int>(r)]; }
bool role_from_name(const std::string& s, Role* out) {
  for (int i = 0; i <= static_cast<int>(Role::Other); ++i)
    if (s == kRoleNames[i]) { *out = static_cast<Role>(i); return true; }
  return false;
}

void Profile::finalize() {
  has_dimmer = has_rgb = has_white = has_amber = has_uv = has_wheel = has_pan = has_tilt = pan16 = tilt16 = false;
  for (const auto& c : channels) {
    switch (c.role) {
      case Role::Dimmer: has_dimmer = true; break;
      case Role::Red: case Role::Green: case Role::Blue: has_rgb = true; break;
      case Role::White: has_white = true; break;
      case Role::Amber: has_amber = true; break;
      case Role::UV: has_uv = true; break;
      case Role::ColorWheel: has_wheel = true; break;
      case Role::Pan: has_pan = true; break;
      case Role::Tilt: has_tilt = true; break;
      case Role::PanFine: pan16 = true; break;
      case Role::TiltFine: tilt16 = true; break;
      default: break;
    }
  }
  pan16 = pan16 && has_pan;
  tilt16 = tilt16 && has_tilt;
}

bool parse_hex_colour(const std::string& s, float* r, float* g, float* b) {
  if (s.size() != 7 || s[0] != '#') return false;
  unsigned v = 0;
  for (std::size_t i = 1; i < 7; ++i) {
    const char ch = s[i];
    const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    if (d < 0) return false;
    v = v * 16 + static_cast<unsigned>(d);
  }
  *r = static_cast<float>((v >> 16) & 255) / 255.0f;
  *g = static_cast<float>((v >> 8) & 255) / 255.0f;
  *b = static_cast<float>(v & 255) / 255.0f;
  return true;
}

namespace {
bool range_int(const Json& j, const char* key, int lo, int hi, int* out, std::string* err, const std::string& where) {
  if (!j.has(key)) return true;
  const double v = j[key].as_number(-1e9);
  if (!j[key].is_number() || v < lo || v > hi || v != std::floor(v)) {
    *err = where + ": \"" + key + "\" must be an integer " + std::to_string(lo) + ".." + std::to_string(hi);
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}
}  // namespace

bool parse_profile(const Json& j, Profile* out, std::string* err) {
  std::string e;
  if (!err) err = &e;
  Profile p;
  if (!j.is_object()) { *err = "profile must be a JSON object"; return false; }
  p.id = j["id"].as_string();
  p.name = j["name"].as_string();
  if (p.id.empty()) { *err = "profile needs an \"id\""; return false; }
  if (p.name.empty()) p.name = p.id;
  p.pan_range_deg = j["pan_range"].as_number(540);
  p.tilt_range_deg = j["tilt_range"].as_number(270);
  const Json& ch = j["channels"];
  if (!ch.is_array() || ch.size() == 0) { *err = "profile needs a non-empty \"channels\" array"; return false; }
  if (ch.size() > static_cast<std::size_t>(kDmxSlots)) { *err = "more than 512 channels"; return false; }
  for (std::size_t i = 0; i < ch.size(); ++i) {
    const Json& c = ch.at(i);
    const std::string where = "channel " + std::to_string(i + 1);
    ChannelDef d;
    if (!c.is_object() || !role_from_name(c["role"].as_string(), &d.role)) {
      *err = where + ": unknown or missing \"role\"";
      return false;
    }
    d.name = c["name"].as_string(role_name(d.role));
    if (!range_int(c, "default", 0, 255, &d.default_value, err, where)) return false;
    if (!range_int(c, "min", 0, 255, &d.min, err, where) || !range_int(c, "max", 0, 255, &d.max, err, where)) return false;
    if (d.min > d.max) { *err = where + ": min is above max"; return false; }
    if (d.role == Role::Strobe) {
      d.min = 0; d.max = 255;
      if (!range_int(c, "off", 0, 255, &d.strobe_off, err, where) || !range_int(c, "closed", 0, 255, &d.closed, err, where) ||
          !range_int(c, "slow", 0, 255, &d.strobe_min, err, where) || !range_int(c, "fast", 0, 255, &d.strobe_max, err, where))
        return false;
      d.hz_min = c["hz_min"].as_number(1);
      d.hz_max = c["hz_max"].as_number(20);
      if (!(d.hz_min > 0) || !(d.hz_max > d.hz_min) || d.hz_max > 100) { *err = where + ": need 0 < hz_min < hz_max <= 100"; return false; }
    }
    if (d.role == Role::ColorWheel) {
      const Json& s = c["slots"];
      for (std::size_t k = 0; k < s.size(); ++k) {
        WheelSlot w;
        const Json& sj = s.at(k);
        if (!range_int(sj, "value", 0, 255, &w.value, err, where + " slot")) return false;
        if (!parse_hex_colour(sj["color"].as_string(), &w.r, &w.g, &w.b)) { *err = where + ": slot colour must be \"#rrggbb\""; return false; }
        w.name = sj["name"].as_string();
        d.slots.push_back(std::move(w));
      }
    }
    p.channels.push_back(std::move(d));
  }
  // Fine channels need their coarse partner somewhere in the profile.
  auto has_role = [&](Role r) { return std::any_of(p.channels.begin(), p.channels.end(), [&](const ChannelDef& c) { return c.role == r; }); };
  if (has_role(Role::PanFine) && !has_role(Role::Pan)) { *err = "pan_fine without pan"; return false; }
  if (has_role(Role::TiltFine) && !has_role(Role::Tilt)) { *err = "tilt_fine without tilt"; return false; }
  for (int r = 0; r <= static_cast<int>(Role::Other); ++r) {
    const Role role = static_cast<Role>(r);
    if (role == Role::Macro || role == Role::Gobo || role == Role::Speed || role == Role::Other) continue;
    if (std::count_if(p.channels.begin(), p.channels.end(), [&](const ChannelDef& c) { return c.role == role; }) > 1) {
      *err = std::string("role \"") + role_name(role) + "\" is used twice";
      return false;
    }
  }
  p.finalize();
  *out = std::move(p);
  return true;
}

bool parse_profile_text(const std::string& text, Profile* out, std::string* err) {
  auto j = Json::parse(text);
  if (!j) { if (err) *err = "not valid JSON"; return false; }
  return parse_profile(*j, out, err);
}

bool load_profile_file(const std::string& path, Profile* out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { if (err) *err = "cannot read " + path; return false; }
  std::stringstream ss;
  ss << f.rdbuf();
  return parse_profile_text(ss.str(), out, err);
}

Json profile_to_json(const Profile& p) {
  Json j = Json::object();
  j.set("format", "dhfix").set("version", 1).set("id", p.id).set("name", p.name);
  j.set("pan_range", p.pan_range_deg).set("tilt_range", p.tilt_range_deg);
  Json arr = Json::array();
  for (const auto& c : p.channels) {
    Json o = Json::object();
    o.set("role", role_name(c.role)).set("name", c.name).set("default", c.default_value);
    if (c.role == Role::Strobe) {
      o.set("off", c.strobe_off).set("slow", c.strobe_min).set("fast", c.strobe_max).set("hz_min", c.hz_min).set("hz_max", c.hz_max);
      if (c.closed >= 0) o.set("closed", c.closed);
    } else if (c.min != 0 || c.max != 255) {
      o.set("min", c.min).set("max", c.max);
    }
    if (!c.slots.empty()) {
      Json sl = Json::array();
      for (const auto& w : c.slots) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "#%02x%02x%02x", clamp_dmx(w.r * 255), clamp_dmx(w.g * 255), clamp_dmx(w.b * 255));
        Json s = Json::object();
        s.set("value", w.value).set("color", std::string(hex)).set("name", w.name);
        sl.push(std::move(s));
      }
      o.set("slots", std::move(sl));
    }
    arr.push(std::move(o));
  }
  j.set("channels", std::move(arr));
  return j;
}

// ---- built-in profiles, written in the same format users write ---------------------------------------------------
namespace {
const char* const kBuiltinJson[] = {
    R"({"id":"generic-dimmer","name":"Generic dimmer","channels":[{"role":"dimmer"}]})",
    R"({"id":"generic-rgb","name":"Generic RGB","channels":[{"role":"red"},{"role":"green"},{"role":"blue"}]})",
    R"({"id":"generic-rgbw","name":"Generic RGBW","channels":[{"role":"red"},{"role":"green"},{"role":"blue"},{"role":"white"}]})",
    R"({"id":"generic-rgba-par","name":"Generic RGBA par with dimmer and strobe","channels":[{"role":"dimmer"},{"role":"red"},{"role":"green"},{"role":"blue"},{"role":"amber"},{"role":"strobe","off":0,"slow":16,"fast":240,"hz_min":1,"hz_max":20}]})",
    R"({"id":"generic-moving-head","name":"Generic simple moving head","pan_range":540,"tilt_range":270,"channels":[{"role":"pan"},{"role":"pan_fine"},{"role":"tilt"},{"role":"tilt_fine"},{"role":"colorwheel","slots":[{"value":0,"color":"#ffffff","name":"white"},{"value":10,"color":"#ff0000","name":"red"},{"value":20,"color":"#ff8000","name":"orange"},{"value":30,"color":"#ffff00","name":"yellow"},{"value":40,"color":"#00ff00","name":"green"},{"value":50,"color":"#00ffff","name":"cyan"},{"value":60,"color":"#0000ff","name":"blue"},{"value":70,"color":"#ff00ff","name":"magenta"}]},{"role":"gobo","default":0},{"role":"strobe","off":0,"closed":0,"slow":16,"fast":240,"hz_min":1,"hz_max":20},{"role":"dimmer"},{"role":"macro","default":0}]})",
    R"({"id":"generic-strobe","name":"Generic strobe","channels":[{"role":"dimmer"},{"role":"strobe","off":0,"slow":16,"fast":240,"hz_min":1,"hz_max":20}]})",
    R"({"id":"generic-uv","name":"Generic UV","channels":[{"role":"uv"}]})",
};
}  // namespace

const std::vector<Profile>& builtin_profiles() {
  static const std::vector<Profile> v = [] {
    std::vector<Profile> r;
    for (const char* t : kBuiltinJson) {
      Profile p;
      std::string e;
      if (parse_profile_text(t, &p, &e)) r.push_back(std::move(p));
    }
    return r;
  }();
  return v;
}

const Profile* ProfileSet::find(const std::string& id) const {
  for (const auto& p : list) if (p.id == id) return &p;
  return nullptr;
}
void ProfileSet::add(Profile p) {
  for (auto& q : list) if (q.id == p.id) { q = std::move(p); return; }
  list.push_back(std::move(p));
}
ProfileSet ProfileSet::with_builtins() {
  ProfileSet s;
  s.list = builtin_profiles();
  return s;
}

std::uint32_t tag_mask(const std::vector<std::string>& tags) {
  std::uint32_t m = 0;
  for (const auto& t : tags) {
    if (t == "front") m |= kTagFront;
    else if (t == "back") m |= kTagBack;
    else if (t == "left") m |= kTagLeft;
    else if (t == "right") m |= kTagRight;
    else if (t == "floor") m |= kTagFloor;
  }
  return m;
}

std::vector<Issue> validate_patch(const std::vector<PatchEntry>& patch, const ProfileSet& profiles) {
  std::vector<Issue> out;
  auto add = [&](bool err, int i, std::string m) { out.push_back({err, i, std::move(m)}); };
  std::set<std::string> ids;
  std::set<int> universes;
  struct Span { int uni, a, b, idx; };
  std::vector<Span> spans;
  for (std::size_t i = 0; i < patch.size(); ++i) {
    const auto& f = patch[i];
    const int idx = static_cast<int>(i);
    const std::string who = "fixture \"" + f.id + "\"";
    if (f.id.empty()) add(true, idx, "fixture " + std::to_string(i + 1) + " has no id");
    else if (!ids.insert(f.id).second) add(true, idx, who + ": duplicate id");
    const Profile* p = profiles.find(f.profile);
    if (!p) { add(true, idx, who + ": unknown profile \"" + f.profile + "\""); continue; }
    if (f.universe < 1 || f.universe > 32767) { add(true, idx, who + ": universe out of range (1..32767)"); continue; }
    if (f.address < 1 || f.address > kDmxSlots) { add(true, idx, who + ": address out of range (1..512)"); continue; }
    const int last = f.address + p->channel_count() - 1;
    if (last > kDmxSlots) {
      add(true, idx, who + ": needs channels " + std::to_string(f.address) + ".." + std::to_string(last) + ", past 512");
      continue;
    }
    universes.insert(f.universe);
    spans.push_back({f.universe, f.address, last, idx});
  }
  for (std::size_t i = 0; i < spans.size(); ++i)
    for (std::size_t k = i + 1; k < spans.size(); ++k)
      if (spans[i].uni == spans[k].uni && spans[i].a <= spans[k].b && spans[k].a <= spans[i].b)
        add(true, spans[k].idx, "fixture \"" + patch[static_cast<std::size_t>(spans[k].idx)].id + "\" overlaps \"" +
                                    patch[static_cast<std::size_t>(spans[i].idx)].id + "\" in universe " + std::to_string(spans[i].uni));
  if (universes.size() > static_cast<std::size_t>(kMaxUniverses))
    add(true, -1, "more than " + std::to_string(kMaxUniverses) + " universes in use");
  std::set<int> positions;
  for (const auto& f : patch)
    if (!positions.insert(f.position).second) { add(false, -1, "two fixtures share chase position " + std::to_string(f.position)); break; }
  return out;
}

bool compile_patch(const std::vector<PatchEntry>& patch, const ProfileSet& profiles, CompiledPatch* out, std::vector<Issue>* issues) {
  auto iss = validate_patch(patch, profiles);
  const bool bad = has_errors(iss);
  if (issues) *issues = std::move(iss);
  if (bad) return false;
  CompiledPatch c;
  std::set<int> unis;
  for (const auto& f : patch) unis.insert(f.universe);
  c.universes.assign(unis.begin(), unis.end());
  std::vector<std::string> used;
  std::vector<int> order(patch.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return patch[static_cast<std::size_t>(a)].position < patch[static_cast<std::size_t>(b)].position; });
  std::vector<float> u(patch.size(), 0);
  for (std::size_t r = 0; r < order.size(); ++r) u[static_cast<std::size_t>(order[r])] = order.size() > 1 ? static_cast<float>(r) / static_cast<float>(order.size()) : 0.0f;
  for (std::size_t i = 0; i < patch.size(); ++i) {
    const auto& f = patch[i];
    auto it = std::find(used.begin(), used.end(), f.profile);
    if (it == used.end()) { used.push_back(f.profile); c.profiles.push_back(*profiles.find(f.profile)); it = used.end() - 1; }
    CompiledFixture cf;
    cf.profile = static_cast<int>(it - used.begin());
    cf.universe_slot = static_cast<int>(std::find(c.universes.begin(), c.universes.end(), f.universe) - c.universes.begin());
    cf.address0 = f.address - 1;
    cf.u = u[i];
    cf.tags = tag_mask(f.tags);
    cf.patch_index = static_cast<int>(i);
    c.fixtures.push_back(cf);
  }
  *out = std::move(c);
  return true;
}

// ---- look -> channels --------------------------------------------------------------------------------------------
namespace {
int scaled(const ChannelDef& c, float x) { return clamp_dmx(c.min + c01(x) * (c.max - c.min)); }
}  // namespace

void map_look(const Profile& p, const Look& look, std::uint8_t* dst) {
  const float I = c01(look.intensity);
  float r = c01(look.r), g = c01(look.g), b = c01(look.b);
  const float peak = std::max({r, g, b});
  // Colour that is carried by channels: scaled by intensity unless the fixture has a dimmer channel.
  const float k = p.has_dimmer ? 1.0f : I;
  float w = 0, a = 0;
  if (p.has_white) { w = std::min({r, g, b}); r -= w; g -= w; b -= w; }
  if (p.has_amber) {
    a = std::min({r, g / 0.6f, 1.0f});
    r -= a; g -= a * 0.6f;
    g = std::max(g, 0.0f);
  }
  const float uv = c01(look.b) * I;
  const bool colourless = !p.has_rgb && !p.has_white && !p.has_amber && !p.has_wheel && !p.has_uv;
  const float dimmer = colourless ? I * peak : I;
  const std::uint16_t pan16 = static_cast<std::uint16_t>(std::lround(c01(look.pan) * 65535.0f));
  const std::uint16_t tilt16 = static_cast<std::uint16_t>(std::lround(c01(look.tilt) * 65535.0f));
  for (std::size_t i = 0; i < p.channels.size(); ++i) {
    const ChannelDef& c = p.channels[i];
    int v = c.default_value;
    switch (c.role) {
      case Role::Dimmer: v = scaled(c, dimmer); break;
      case Role::Red: v = scaled(c, r * k); break;
      case Role::Green: v = scaled(c, g * k); break;
      case Role::Blue: v = scaled(c, b * k); break;
      case Role::White: v = scaled(c, w * k); break;
      case Role::Amber: v = scaled(c, a * k); break;
      case Role::UV: v = scaled(c, uv); break;
      case Role::Strobe:
        if (look.strobe_hz >= static_cast<float>(c.hz_min)) {
          const double t = std::clamp((look.strobe_hz - c.hz_min) / (c.hz_max - c.hz_min), 0.0, 1.0);
          v = clamp_dmx(c.strobe_min + t * (c.strobe_max - c.strobe_min));
        } else if (c.closed >= 0 && !p.has_dimmer && I <= 0.001f) {
          v = c.closed;  // no dimmer: close the shutter for blackout
        } else {
          v = c.strobe_off;
        }
        break;
      case Role::Pan: v = p.pan16 ? (pan16 >> 8) : clamp_dmx(c01(look.pan) * 255.0f); break;
      case Role::PanFine: v = pan16 & 255; break;
      case Role::Tilt: v = p.tilt16 ? (tilt16 >> 8) : clamp_dmx(c01(look.tilt) * 255.0f); break;
      case Role::TiltFine: v = tilt16 & 255; break;
      case Role::ColorWheel:
        if (!c.slots.empty()) {
          const float pk = peak > 0.001f ? peak : 1.0f;
          const float nr = look.r / pk, ng = look.g / pk, nb = look.b / pk;
          float best = 1e9f;
          for (const auto& s : c.slots) {
            const float d = (s.r - nr) * (s.r - nr) + (s.g - ng) * (s.g - ng) + (s.b - nb) * (s.b - nb);
            if (d < best) { best = d; v = s.value; }
          }
        }
        break;
      default: break;  // gobo, macro, speed, other: their defaults
    }
    dst[i] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
  }
}

}  // namespace deckhand::lighting
