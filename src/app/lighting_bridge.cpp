#include "deckhand/app/lighting_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

#include "deckhand/app/next_tracks.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/broadcast/json.hpp"
#include "deckhand/log/log.hpp"

#ifndef DECKHAND_WITH_LIGHTING
#define DECKHAND_WITH_LIGHTING 1
#endif
#if DECKHAND_WITH_LIGHTING
#include "deckhand/lighting/engine.hpp"
#endif

namespace deckhand {

void lighting_beats_from_grid(const std::vector<LightingGridSection>& grid, double length_ms, std::vector<double>& beats_ms, int& downbeat_phase) {
  beats_ms.clear();
  downbeat_phase = 0;
  bool first = true;
  for (std::size_t s = 0; s < grid.size(); ++s) {
    if (grid[s].bpm <= 0) continue;
    const double interval = 60000.0 / grid[s].bpm;
    const double end = s + 1 < grid.size() ? grid[s + 1].first_beat_ms : length_ms;
    for (double ms = grid[s].first_beat_ms; ms < end - interval * 0.5; ms += interval) {
      if (first) {
        downbeat_phase = (4 - ((std::clamp(grid[s].bar_beat, 1, 4) - 1))) % 4;  // beat 0 is bar beat bar_beat
        first = false;
      }
      beats_ms.push_back(ms);
    }
  }
}

#if DECKHAND_WITH_LIGHTING

using deckhand::broadcast::Json;
namespace lt = deckhand::lighting;

namespace {
double steady_now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Objects merge key by key, everything else is replaced.
void merge_into(Json& base, const Json& over) {
  if (!over.is_object()) return;
  for (const std::string& k : over.keys()) {
    const Json& v = over[k];
    if (v.is_object() && base[k].is_object()) {
      Json sub = base[k];
      merge_into(sub, v);
      base.set(k, std::move(sub));
    } else {
      base.set(k, v);
    }
  }
}

bool parse_colour(const Json& j, lt::Rgb& c) {
  if (j.is_string()) return lt::parse_hex_colour(j.as_string(), &c.r, &c.g, &c.b);
  return false;
}
}  // namespace

struct LightingBridge::Impl {
  mutable std::mutex mu;
  lt::LightingEngine engine;
  std::string path;
  bool opened = false;
  lt::TrackTimeline timeline[kMaxDecks];
  std::int64_t track_id[kMaxDecks] = {};
  std::uint32_t rgb[kMaxDecks] = {};
  int key[kMaxDecks];
  int driving = -1;
  double last_tick = 0;

  Impl() {
    for (int& k : key) k = -1;
  }

  // Applies the config: configure, then start or stop depending on `enabled`.
  bool apply(const lt::LightingConfig& cfg, std::string& error) {
    if (!engine.configure(cfg, &error)) return false;
    if (cfg.enabled && !engine.running()) {
      std::string e2;
      engine.start(&e2);  // an output that cannot open shows in the status; the show still runs
    } else if (!cfg.enabled && engine.running()) {
      engine.stop();
    }
    return true;
  }

  Json to_json() const {
    const Impl& im = *this;
    const lt::LightingStatus st = im.engine.status();
    const lt::LightingConfig cfg = im.engine.config();
    Json j = Json::object();
    j.set("available", true);
    j.set("enabled", cfg.enabled);
    j.set("running", st.running);
    j.set("outputs_enabled", st.outputs_enabled);
    Json outs = Json::array();
    for (const lt::OutputInfo& o : st.outputs) {
      Json e = Json::object();
      e.set("name", o.name);
      e.set("open", o.open);
      e.set("frames", static_cast<double>(o.stats.frames));
      e.set("packets", static_cast<double>(o.stats.packets));
      e.set("errors", static_cast<double>(o.stats.errors));
      e.set("error", o.error);
      outs.push(std::move(e));
    }
    j.set("outputs", std::move(outs));
    Json issues = Json::array();
    for (const std::string& s : st.issues) issues.push(s);
    j.set("issues", std::move(issues));
    j.set("scene", lt::scene_name(st.show.scene));
    j.set("move", lt::move_name(st.show.move));
    j.set("section", lt::section_name(st.show.section));
    j.set("blackout", st.show.blackout);
    j.set("hold", st.show.hold);
    j.set("override", st.show.override_active);
    j.set("master", static_cast<double>(st.show.master));
    j.set("strobe_hz", static_cast<double>(st.show.strobe_hz));
    j.set("strobe_enabled", cfg.show.strobe_enabled);
    j.set("strobe_max_hz", static_cast<double>(cfg.show.strobe_max_hz));
    j.set("latency_ms", cfg.show.latency_ms);
    j.set("fixtures", st.fixtures);
    Json uni = Json::array();
    for (int u : st.universes) uni.push(u);
    j.set("universes", std::move(uni));
    j.set("frames_rendered", static_cast<double>(st.frames_rendered));
    j.set("frames_sent", static_cast<double>(st.frames_sent));
    j.set("driving_deck", im.driving >= 0 ? Json(im.driving + 1) : Json());
    return j;
  }
  bool save(const lt::LightingConfig& cfg, std::string& error) { return path.empty() || lt::save_config_file(path, cfg, &error); }
};

LightingBridge::LightingBridge() : impl_(std::make_unique<Impl>()) {}
LightingBridge::~LightingBridge() { close(); }
bool LightingBridge::compiled() { return true; }

void LightingBridge::open(const std::string& data_dir) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->path = data_dir + "/lighting.json";
  lt::LightingConfig cfg;
  std::string err;
  if (!lt::load_config_file(impl_->path, &cfg, &err)) {
    DH_LOG(Warn, "lighting: %s: using defaults (everything off)", err.c_str());
    cfg = lt::LightingConfig{};
  }
  if (!impl_->apply(cfg, err)) DH_LOG(Warn, "lighting: configuration rejected: %s", err.c_str());
  impl_->opened = true;
}

void LightingBridge::close() {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->engine.running()) impl_->engine.stop();
}

void LightingBridge::set_track(int deck, int key_index, LightingTrack t) {
  if (deck < 0 || deck >= kMaxDecks) return;
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->timeline[deck].set_track(t.beats_ms, t.downbeat_phase, t.phrases);
  impl_->track_id[deck] = t.track_id;
  impl_->rgb[deck] = t.rgb;
  impl_->key[deck] = key_index;
}

void LightingBridge::clear_track(int deck) {
  if (deck < 0 || deck >= kMaxDecks) return;
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->timeline[deck].clear();
  impl_->track_id[deck] = 0;
}

void LightingBridge::tick(const IEngine& engine) {
  if (!impl_->opened || !impl_->engine.running()) return;  // off: no work at all
  const double now = steady_now();
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (now - impl_->last_tick < 0.02) return;  // 50 Hz at most
    impl_->last_tick = now;
  }
  const int d = master_deck(engine);
  lt::MusicState m;
  m.stamp_s = now;
  if (d >= 0) {
    const DeckView v = engine.deck(d);
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->driving = d;
    m.playing = v.loaded && v.playing && v.fader > 0.02f;
    m.bpm = v.bpm;
    if (impl_->track_id[d] == v.track_id && impl_->timeline[d].has_grid() && v.sample_rate > 0)
      impl_->timeline[d].locate(v.position / v.sample_rate * 1000.0, v.speed > 0 ? v.speed : 1.0, &m);
    m.bpm = v.bpm > 0 ? v.bpm : m.bpm;
    m.levels_valid = true;  // the engine reports one level per deck, not bands: all four follow it
    m.level = m.low = m.mid = m.high = std::clamp(v.level, 0.0f, 1.0f);
    m.track_rgb = impl_->track_id[d] == v.track_id ? impl_->rgb[d] : 0;
    m.key_index = v.key_index;
    impl_->engine.set_state(m);
  } else {
    impl_->engine.set_state(m);  // nothing loaded: stopped
  }
}

std::string LightingBridge::status_json() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->to_json().dump();
}

std::string LightingBridge::config_json() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return lt::config_to_json(impl_->engine.config()).dump();
}

std::string LightingBridge::preview_json() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  Json j = Json::object();
  j.set("running", impl_->engine.running());
  Json fx = Json::array();
  for (const lt::Look& l : impl_->engine.preview_looks()) {
    Json e = Json::object();
    const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    char hex[16];
    std::snprintf(hex, sizeof hex, "#%02x%02x%02x", byte(l.r * l.intensity), byte(l.g * l.intensity), byte(l.b * l.intensity));
    e.set("colour", std::string(hex));
    e.set("intensity", static_cast<double>(l.intensity));
    e.set("strobe_hz", static_cast<double>(l.strobe_hz));
    e.set("pan", static_cast<double>(l.pan));
    e.set("tilt", static_cast<double>(l.tilt));
    fx.push(std::move(e));
  }
  j.set("fixtures", std::move(fx));
  Json uni = Json::array();
  for (const lt::UniverseFrame& f : impl_->engine.preview()) {
    Json u = Json::object();
    u.set("universe", f.universe);
    std::string hex;
    static const char* digits = "0123456789abcdef";
    int last = lt::kDmxSlots;
    while (last > 0 && f.data[static_cast<std::size_t>(last - 1)] == 0) --last;
    for (int i = 0; i < last; ++i) {
      hex += digits[f.data[static_cast<std::size_t>(i)] >> 4];
      hex += digits[f.data[static_cast<std::size_t>(i)] & 15];
    }
    u.set("channels", last);
    u.set("data_hex", hex);  // trailing zero channels are left off
    uni.push(std::move(u));
  }
  j.set("universes", std::move(uni));
  return j.dump();
}

bool LightingBridge::set_config_json(const std::string& text, std::string& error) {
  const auto over = Json::parse(text);
  if (!over || !over->is_object()) {
    error = "the body must be a JSON object";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  Json merged = lt::config_to_json(impl_->engine.config());
  merge_into(merged, *over);
  lt::LightingConfig cfg;
  if (!lt::config_from_json(merged, &cfg, &error)) return false;
  if (!impl_->apply(cfg, error)) return false;
  std::string save_error;
  if (!impl_->save(impl_->engine.config(), save_error)) DH_LOG(Warn, "lighting: configuration not saved: %s", save_error.c_str());
  return true;
}

bool LightingBridge::control(const std::string& action, bool has_value, double value, std::string& error) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const lt::LightingStatus st = impl_->engine.status();
  lt::LightingConfig cfg = impl_->engine.config();
  const auto flag = [&](bool current) { return has_value ? value != 0 : !current; };
  if (action == "blackout") {
    impl_->engine.set_blackout(flag(st.show.blackout));
  } else if (action == "hold") {
    impl_->engine.set_hold(flag(st.show.hold));
  } else if (action == "master") {
    if (!has_value || !(value >= 0 && value <= 1)) {
      error = "value needed: 0..1";
      return false;
    }
    impl_->engine.set_master(static_cast<float>(value));
  } else if (action == "strobe") {  // 1 = strobes allowed, 0 = no strobes
    impl_->engine.set_strobe(flag(cfg.show.strobe_enabled), cfg.show.strobe_max_hz);
  } else if (action == "enable") {
    cfg.enabled = flag(cfg.enabled);
    if (!impl_->apply(cfg, error)) return false;
    std::string e;
    impl_->save(impl_->engine.config(), e);
  } else {
    error = "unknown lighting control";
    return false;
  }
  return true;
}

bool LightingBridge::read_control(const std::string& action, double& value) const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const lt::LightingStatus st = impl_->engine.status();
  const lt::LightingConfig cfg = impl_->engine.config();
  if (action == "blackout") value = st.show.blackout;
  else if (action == "hold") value = st.show.hold;
  else if (action == "master") value = st.show.master;
  else if (action == "strobe") value = cfg.show.strobe_enabled;
  else if (action == "enable") value = cfg.enabled;
  else return false;
  return true;
}

bool LightingBridge::command_json(const std::string& text, std::string& error) {
  const auto j = Json::parse(text);
  if (!j || !j->is_object() || !(*j)["action"].is_string()) {
    error = "the body needs an \"action\"";
    return false;
  }
  const std::string a = (*j)["action"].as_string();
  const Json& v = (*j)["value"];
  if (a == "blackout" || a == "hold" || a == "strobe" || a == "enable" || a == "master") {
    if (a == "strobe" && (*j)["max_hz"].is_number()) {  // strobe {"enabled": bool, "max_hz": n}
      std::lock_guard<std::mutex> lock(impl_->mu);
      const lt::LightingConfig cfg = impl_->engine.config();
      const double hz = (*j)["max_hz"].as_number();
      if (!(hz >= 0 && hz <= 25)) {
        error = "max_hz is 0..25";
        return false;
      }
      impl_->engine.set_strobe((*j)["enabled"].is_bool() ? (*j)["enabled"].as_bool() : cfg.show.strobe_enabled, static_cast<float>(hz));
      return true;
    }
    bool has = !v.is_null();
    double val = v.is_bool() ? (v.as_bool() ? 1 : 0) : v.as_number();
    if (a == "strobe" && (*j)["enabled"].is_bool()) {
      has = true;
      val = (*j)["enabled"].as_bool() ? 1 : 0;
    }
    return control(a, has, val, error);
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (a == "release") {
    impl_->engine.release_override();
    return true;
  }
  if (a == "override") {
    lt::Override o;
    if ((*j)["scene"].is_string()) {
      if (!lt::scene_from_name((*j)["scene"].as_string(), &o.scene)) {
        error = "unknown scene";
        return false;
      }
      o.has_scene = true;
    }
    if ((*j)["move"].is_string()) {
      if (!lt::move_from_name((*j)["move"].as_string(), &o.move)) {
        error = "unknown move";
        return false;
      }
      o.has_move = true;
    }
    if (!(*j)["colour"].is_null()) {
      if (!parse_colour((*j)["colour"], o.colour)) {
        error = "colour is \"#rrggbb\"";
        return false;
      }
      o.has_colour = true;
    }
    if ((*j)["intensity"].is_number()) {
      o.intensity = static_cast<float>(std::clamp((*j)["intensity"].as_number(), 0.0, 1.0));
      o.has_intensity = true;
    }
    if ((*j)["strobe_hz"].is_number()) {
      o.strobe_hz = static_cast<float>(std::clamp((*j)["strobe_hz"].as_number(), 0.0, 25.0));
      o.has_strobe = true;
    }
    if (!o.any()) {
      error = "an override needs scene, move, colour, intensity or strobe_hz";
      return false;
    }
    impl_->engine.set_override(o);
    return true;
  }
  error = "unknown action (blackout, hold, master, strobe, enable, override, release)";
  return false;
}

bool LightingBridge::enabled() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->engine.config().enabled;
}

std::string LightingBridge::describe() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const lt::LightingStatus st = impl_->engine.status();
  if (!st.running) return {};
  return std::to_string(st.fixtures) + " fixtures";
}

LightingBridge::UiState LightingBridge::ui_state() const {
  UiState u;
  u.available = true;
  std::lock_guard<std::mutex> lock(impl_->mu);
  const lt::LightingStatus st = impl_->engine.status();
  const lt::LightingConfig cfg = impl_->engine.config();
  u.enabled = cfg.enabled;
  u.running = st.running;
  u.blackout = st.show.blackout;
  u.hold = st.show.hold;
  u.strobes = cfg.show.strobe_enabled;
  u.master = st.show.master;
  u.artnet = cfg.artnet.enabled;
  u.sacn = cfg.sacn.enabled;
  u.serial = cfg.serial.enabled;
  u.artnet_host = cfg.artnet.host;
  u.sacn_host = cfg.sacn.host;
  u.artnet_first_universe = cfg.artnet.first_universe;
  u.sacn_multicast = cfg.sacn.multicast;
  if (st.running) {
    for (const lt::Look& l : impl_->engine.preview_looks()) {
      if (u.swatches.size() >= 16) break;
      const auto byte = [](float v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
      u.swatches.push_back((byte(l.r * l.intensity) << 16) | (byte(l.g * l.intensity) << 8) | byte(l.b * l.intensity));
    }
  }
  return u;
}

bool LightingBridge::set_output(const std::string& which, bool on, const std::string& host, int universe, bool multicast, std::string& error) {
  Json o = Json::object();
  Json cfg = Json::object();
  if (which == "artnet") {
    cfg.set("enabled", on);
    if (!host.empty() || !on) cfg.set("host", host);
    cfg.set("first_universe", universe);
  } else if (which == "sacn") {
    cfg.set("enabled", on);
    cfg.set("multicast", multicast);
    if (!host.empty() || !on) cfg.set("host", host);
    cfg.set("universe_offset", universe);
  } else {
    error = "unknown output (artnet, sacn)";
    return false;
  }
  Json outputs = Json::object();
  outputs.set(which, std::move(cfg));
  o.set("outputs", std::move(outputs));
  return set_config_json(o.dump(), error);
}

#else  // ---- built without the lighting module: a stub ------------------------------------------------------------

struct LightingBridge::Impl {};
LightingBridge::LightingBridge() : impl_(std::make_unique<Impl>()) {}
LightingBridge::~LightingBridge() = default;
bool LightingBridge::compiled() { return false; }
void LightingBridge::open(const std::string&) {}
void LightingBridge::close() {}
void LightingBridge::set_track(int, int, LightingTrack) {}
void LightingBridge::clear_track(int) {}
void LightingBridge::tick(const IEngine&) {}
std::string LightingBridge::status_json() const { return "{\"available\":false,\"enabled\":false,\"running\":false}"; }
std::string LightingBridge::config_json() const { return "{}"; }
std::string LightingBridge::preview_json() const { return "{\"running\":false,\"fixtures\":[],\"universes\":[]}"; }
bool LightingBridge::set_config_json(const std::string&, std::string& error) {
  error = "this build has no lighting module";
  return false;
}
bool LightingBridge::command_json(const std::string&, std::string& error) {
  error = "this build has no lighting module";
  return false;
}
bool LightingBridge::control(const std::string&, bool, double, std::string& error) {
  error = "this build has no lighting module";
  return false;
}
bool LightingBridge::read_control(const std::string&, double&) const { return false; }
bool LightingBridge::enabled() const { return false; }
std::string LightingBridge::describe() const { return {}; }
LightingBridge::UiState LightingBridge::ui_state() const { return {}; }
bool LightingBridge::set_output(const std::string&, bool, const std::string&, int, bool, std::string& error) {
  error = "this build has no lighting module";
  return false;
}

#endif

}  // namespace deckhand
