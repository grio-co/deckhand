#include "deckhand/lighting/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "deckhand/broadcast/json.hpp"

namespace deckhand::lighting {

using deckhand::broadcast::Json;

namespace {
double steady_now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
std::string hex(const Rgb& c) {
  char b[16];
  std::snprintf(b, sizeof b, "#%02x%02x%02x", static_cast<int>(std::lround(std::clamp(c.r, 0.f, 1.f) * 255)),
                static_cast<int>(std::lround(std::clamp(c.g, 0.f, 1.f) * 255)), static_cast<int>(std::lround(std::clamp(c.b, 0.f, 1.f) * 255)));
  return b;
}
double num(const Json& j, const char* k, double d) { return j[k].is_number() ? j[k].as_number() : d; }
bool flag(const Json& j, const char* k, bool d) { return j[k].is_bool() ? j[k].as_bool() : d; }
std::string str(const Json& j, const char* k, const std::string& d) { return j[k].is_string() ? j[k].as_string() : d; }
}  // namespace

Json config_to_json(const LightingConfig& c) {
  Json j = Json::object();
  j.set("version", 1).set("enabled", c.enabled).set("refresh_hz", c.refresh_hz).set("keepalive_s", c.keepalive_s);
  const ShowConfig& s = c.show;
  Json sj = Json::object();
  sj.set("master", s.master).set("strobe_enabled", s.strobe_enabled).set("strobe_max_hz", s.strobe_max_hz)
      .set("latency_ms", s.latency_ms).set("crossfade_s", s.crossfade_s).set("stop_fade_s", s.stop_fade_s)
      .set("stale_s", s.stale_s).set("seed", static_cast<double>(s.seed)).set("auto_movement", s.auto_movement);
  Json pj = Json::object();
  pj.set("mode", palette_mode_name(s.palette.mode));
  Json cols = Json::array();
  for (const auto& col : s.palette.colours) cols.push(hex(col));
  pj.set("colours", std::move(cols));
  sj.set("palette", std::move(pj));
  j.set("show", std::move(sj));

  Json o = Json::object();
  Json a = Json::object();
  a.set("enabled", c.artnet.enabled).set("host", c.artnet.host).set("port", static_cast<int>(c.artnet.port))
      .set("bind_ip", c.artnet.bind_ip).set("listen_port", static_cast<int>(c.artnet.listen_port))
      .set("respond_to_poll", c.artnet.respond_to_poll).set("first_universe", c.artnet.first_universe)
      .set("node_ip", c.artnet.node_ip).set("short_name", c.artnet.short_name).set("long_name", c.artnet.long_name);
  Json e = Json::object();
  e.set("enabled", c.sacn.enabled).set("multicast", c.sacn.multicast).set("host", c.sacn.host).set("port", static_cast<int>(c.sacn.port))
      .set("bind_ip", c.sacn.bind_ip).set("interface_ip", c.sacn.interface_ip).set("priority", c.sacn.priority)
      .set("universe_offset", c.sacn.universe_offset).set("sync_universe", c.sacn.sync_universe).set("slots", c.sacn.slots)
      .set("source_name", c.sacn.source_name).set("cid", c.sacn.cid);
  Json r = Json::object();
  r.set("enabled", c.serial.enabled).set("device", c.serial.device).set("kind", c.serial.kind == SerialKind::Pro ? "pro" : "open")
      .set("universe", c.serial.universe);
  o.set("artnet", std::move(a)).set("sacn", std::move(e)).set("serial", std::move(r));
  j.set("outputs", std::move(o));

  Json patch = Json::array();
  for (const auto& f : c.patch) {
    Json p = Json::object();
    p.set("id", f.id).set("profile", f.profile).set("universe", f.universe).set("address", f.address).set("position", f.position);
    Json tags = Json::array();
    for (const auto& t : f.tags) tags.push(t);
    p.set("tags", std::move(tags));
    patch.push(std::move(p));
  }
  j.set("patch", std::move(patch));
  Json profs = Json::array();
  for (const auto& p : c.profiles) profs.push(profile_to_json(p));
  j.set("profiles", std::move(profs));
  Json files = Json::array();
  for (const auto& f : c.profile_files) files.push(f);
  j.set("profile_files", std::move(files));
  return j;
}

bool config_from_json(const Json& j, LightingConfig* out, std::string* err) {
  std::string e;
  if (!err) err = &e;
  if (!j.is_object()) { *err = "lighting config must be a JSON object"; return false; }
  LightingConfig c;
  c.enabled = flag(j, "enabled", false);
  c.refresh_hz = std::clamp(num(j, "refresh_hz", 40), 1.0, 60.0);
  c.keepalive_s = std::clamp(num(j, "keepalive_s", 1.0), 0.05, 10.0);
  const Json& s = j["show"];
  c.show.master = static_cast<float>(std::clamp(num(s, "master", 1.0), 0.0, 1.0));
  c.show.strobe_enabled = flag(s, "strobe_enabled", true);
  c.show.strobe_max_hz = static_cast<float>(std::clamp(num(s, "strobe_max_hz", 10.0), 0.0, 25.0));
  c.show.latency_ms = std::clamp(num(s, "latency_ms", 0), -200.0, 1000.0);
  c.show.crossfade_s = std::clamp(num(s, "crossfade_s", 1.5), 0.0, 8.0);
  c.show.stop_fade_s = std::clamp(num(s, "stop_fade_s", 0.6), 0.0, 8.0);
  c.show.stale_s = std::clamp(num(s, "stale_s", 2.0), 0.2, 60.0);
  c.show.seed = static_cast<std::uint64_t>(std::max(0.0, num(s, "seed", 1)));
  c.show.auto_movement = flag(s, "auto_movement", true);
  const Json& p = s["palette"];
  const std::string pm = str(p, "mode", "user");
  c.show.palette.mode = pm == "track" ? PaletteMode::Track : pm == "key" ? PaletteMode::Key : PaletteMode::User;
  for (std::size_t i = 0; i < p["colours"].size() && i < 8; ++i) {
    Rgb col;
    if (!parse_hex_colour(p["colours"].at(i).as_string(), &col.r, &col.g, &col.b)) { *err = "palette colours must be \"#rrggbb\""; return false; }
    c.show.palette.colours.push_back(col);
  }
  c.show.refresh_hz = c.refresh_hz;
  const Json& a = j["outputs"]["artnet"];
  c.artnet.enabled = flag(a, "enabled", false);
  c.artnet.host = str(a, "host", "");
  c.artnet.port = static_cast<std::uint16_t>(std::clamp(num(a, "port", kArtNetPort), 1.0, 65535.0));
  c.artnet.bind_ip = str(a, "bind_ip", "0.0.0.0");
  c.artnet.listen_port = static_cast<std::uint16_t>(std::clamp(num(a, "listen_port", kArtNetPort), 0.0, 65535.0));
  c.artnet.respond_to_poll = flag(a, "respond_to_poll", true);
  c.artnet.first_universe = static_cast<int>(std::clamp(num(a, "first_universe", 0), 0.0, 32000.0));
  c.artnet.node_ip = str(a, "node_ip", "");
  c.artnet.short_name = str(a, "short_name", "DeckHand");
  c.artnet.long_name = str(a, "long_name", "DeckHand lighting");
  const Json& e2 = j["outputs"]["sacn"];
  c.sacn.enabled = flag(e2, "enabled", false);
  c.sacn.multicast = flag(e2, "multicast", true);
  c.sacn.host = str(e2, "host", "");
  c.sacn.port = static_cast<std::uint16_t>(std::clamp(num(e2, "port", kSacnPort), 1.0, 65535.0));
  c.sacn.bind_ip = str(e2, "bind_ip", "0.0.0.0");
  c.sacn.interface_ip = str(e2, "interface_ip", "");
  c.sacn.priority = static_cast<int>(std::clamp(num(e2, "priority", 100), 0.0, 200.0));
  c.sacn.universe_offset = static_cast<int>(std::clamp(num(e2, "universe_offset", 0), 0.0, 60000.0));
  c.sacn.sync_universe = static_cast<int>(std::clamp(num(e2, "sync_universe", 0), 0.0, 63999.0));
  c.sacn.slots = static_cast<int>(std::clamp(num(e2, "slots", 512), 24.0, 512.0));
  c.sacn.source_name = str(e2, "source_name", "DeckHand");
  c.sacn.cid = str(e2, "cid", "");
  const Json& r = j["outputs"]["serial"];
  c.serial.enabled = flag(r, "enabled", false);
  c.serial.device = str(r, "device", "");
  c.serial.kind = str(r, "kind", "pro") == "open" ? SerialKind::OpenDmx : SerialKind::Pro;
  c.serial.universe = static_cast<int>(std::clamp(num(r, "universe", 1), 1.0, 32767.0));
  for (std::size_t i = 0; i < j["patch"].size(); ++i) {
    const Json& f = j["patch"].at(i);
    PatchEntry pe;
    pe.id = str(f, "id", "");
    pe.profile = str(f, "profile", "");
    pe.universe = static_cast<int>(num(f, "universe", 1));
    pe.address = static_cast<int>(num(f, "address", 1));
    pe.position = static_cast<int>(num(f, "position", static_cast<double>(i)));
    for (std::size_t k = 0; k < f["tags"].size(); ++k) pe.tags.push_back(f["tags"].at(k).as_string());
    c.patch.push_back(std::move(pe));
  }
  for (std::size_t i = 0; i < j["profiles"].size(); ++i) {
    Profile pr;
    if (!parse_profile(j["profiles"].at(i), &pr, err)) { *err = "profile " + std::to_string(i + 1) + ": " + *err; return false; }
    c.profiles.push_back(std::move(pr));
  }
  for (std::size_t i = 0; i < j["profile_files"].size(); ++i) c.profile_files.push_back(j["profile_files"].at(i).as_string());
  *out = std::move(c);
  return true;
}

bool load_config_file(const std::string& path, LightingConfig* out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { *out = LightingConfig{}; return true; }
  std::stringstream ss;
  ss << f.rdbuf();
  auto j = Json::parse(ss.str());
  if (!j) { if (err) *err = "lighting config is not valid JSON"; return false; }
  return config_from_json(*j, out, err);
}

bool save_config_file(const std::string& path, const LightingConfig& c, std::string* err) {
  namespace fs = std::filesystem;
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot write " + tmp; return false; }
    f << config_to_json(c).dump();
    if (!f.good()) { if (err) *err = "write failed"; return false; }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) { fs::remove(path, ec); fs::rename(tmp, path, ec); }
  if (ec) { if (err) *err = "cannot replace " + path; return false; }
  return true;
}

// ---- engine ----------------------------------------------------------------------------------------------------
LightingEngine::LightingEngine() {
  profiles_ = ProfileSet::with_builtins();
  show_.configure(ShowConfig{}, CompiledPatch{});
  pacer_.configure(40, 1.0);
}
LightingEngine::~LightingEngine() { stop(); }

bool LightingEngine::configure(const LightingConfig& in, std::string* err) {
  const bool was_running = running_.load();
  ProfileSet ps = ProfileSet::with_builtins();
  for (const auto& p : in.profiles) ps.add(p);
  std::string perr;
  for (const auto& path : in.profile_files) {
    Profile p;
    if (!load_profile_file(path, &p, &perr)) { if (err) *err = path + ": " + perr; return false; }
    ps.add(std::move(p));
  }
  CompiledPatch cp;
  std::vector<Issue> issues;
  if (!compile_patch(in.patch, ps, &cp, &issues)) {
    if (err) {
      *err = "patch rejected:";
      for (const auto& i : issues) if (i.error) *err += "\n  " + i.message;
    }
    return false;
  }
  if (was_running) stop();
  {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = in;
    cfg_.show.refresh_hz = cfg_.refresh_hz = std::clamp(in.refresh_hz, 1.0, 60.0);
    profiles_ = std::move(ps);
    issues_.clear();
    for (const auto& i : issues) issues_.push_back((i.error ? "error: " : "warning: ") + i.message);
    show_.configure(cfg_.show, std::move(cp));
    pacer_.configure(cfg_.refresh_hz, cfg_.keepalive_s);
    last_sent_.clear();
    have_sent_ = false;
  }
  if (was_running) return start(err);
  return true;
}

LightingConfig LightingEngine::config() const {
  std::lock_guard<std::mutex> lk(mu_);
  LightingConfig c = cfg_;
  c.show = show_.config();
  return c;
}

void LightingEngine::open_outputs_locked() {
  outputs_.clear();
  infos_.clear();
  auto add = [&](std::shared_ptr<Output> o) {
    OutputInfo info;
    info.name = o->name();
    std::string e;
    info.open = o->open(&e);
    info.error = e;
    infos_.push_back(info);
    outputs_.push_back(std::move(o));
  };
  if (cfg_.enabled) {
    if (cfg_.artnet.enabled) add(make_artnet_output(cfg_.artnet));
    if (cfg_.sacn.enabled) add(make_sacn_output(cfg_.sacn));
    if (cfg_.serial.enabled) add(make_serial_output(cfg_.serial));
  }
  for (auto& a : attached_) add(a);
}

void LightingEngine::close_outputs_locked(bool dark) {
  if (dark && have_sent_) {
    std::vector<UniverseFrame> z = last_sent_;
    for (auto& f : z) f.data.fill(0);
    for (auto& o : outputs_) o->send(z.data(), static_cast<int>(z.size()));
  }
  for (auto& o : outputs_) o->close();
  outputs_.clear();
}

bool LightingEngine::start(std::string* err, bool spawn_thread) {
  if (running_.load()) return true;
  {
    std::lock_guard<std::mutex> lk(mu_);
    open_outputs_locked();
    have_sent_ = false;
    next_due_ = 0;
    stop_ = false;
    running_ = true;
  }
  if (spawn_thread) thread_ = std::thread([this] { thread_main(); });
  if (err) err->clear();
  return true;
}

void LightingEngine::stop() {
  if (!running_.exchange(false)) return;
  {
    std::lock_guard<std::mutex> lk(wait_mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lk(mu_);
  close_outputs_locked(true);
}

void LightingEngine::attach_output(std::shared_ptr<Output> out) {
  std::lock_guard<std::mutex> lk(mu_);
  attached_.push_back(out);
  if (running_.load()) {
    OutputInfo info;
    info.name = out->name();
    std::string e;
    info.open = out->open(&e);
    info.error = e;
    infos_.push_back(info);
    outputs_.push_back(std::move(out));
  }
}
void LightingEngine::clear_attached_outputs() {
  std::lock_guard<std::mutex> lk(mu_);
  attached_.clear();
}

void LightingEngine::set_state(const MusicState& m) {
  std::lock_guard<std::mutex> lk(mu_);
  show_.set_music(m);
}

bool LightingEngine::tick(double now) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!pacer_.frame_due(now)) { next_due_ = pacer_.next_due(); return false; }
  show_.render(now);
  ++rendered_;
  for (auto& o : outputs_) o->service();
  const auto& fr = show_.frames();
  bool changed = !have_sent_ || last_sent_.size() != fr.size();
  for (std::size_t i = 0; !changed && i < fr.size(); ++i) changed = fr[i].data != last_sent_[i].data;
  if (!fr.empty() && pacer_.should_send(now, changed)) {
    for (auto& o : outputs_) o->send(fr.data(), static_cast<int>(fr.size()));
    last_sent_ = fr;  // same size after the first call: no allocation
    have_sent_ = true;
    ++sent_;
  }
  next_due_ = pacer_.next_due();
  return true;
}

double LightingEngine::next_due() const {
  std::lock_guard<std::mutex> lk(mu_);
  return next_due_;
}

void LightingEngine::thread_main() {
  while (true) {
    {
      std::lock_guard<std::mutex> lk(wait_mu_);
      if (stop_) return;
    }
    const double now = steady_now();
    tick(now);
    const double wait = std::max(0.001, next_due() - steady_now());
    std::unique_lock<std::mutex> lk(wait_mu_);
    cv_.wait_for(lk, std::chrono::duration<double>(wait), [this] { return stop_.load(); });
    if (stop_) return;
  }
}

void LightingEngine::set_blackout(bool on) { std::lock_guard<std::mutex> lk(mu_); show_.set_blackout(on); }
void LightingEngine::set_hold(bool on) { std::lock_guard<std::mutex> lk(mu_); show_.set_hold(on); }
void LightingEngine::set_master(float v) { std::lock_guard<std::mutex> lk(mu_); show_.set_master(v); }
void LightingEngine::set_strobe(bool e, float hz) { std::lock_guard<std::mutex> lk(mu_); show_.set_strobe(e, hz); }
void LightingEngine::set_latency_ms(double ms) {
  std::lock_guard<std::mutex> lk(mu_);
  ShowConfig c = show_.config();
  c.latency_ms = ms;
  show_.set_config(c);
}
void LightingEngine::set_palette(const PaletteConfig& p) {
  std::lock_guard<std::mutex> lk(mu_);
  ShowConfig c = show_.config();
  c.palette = p;
  show_.set_config(c);
}
void LightingEngine::set_override(const Override& o) { std::lock_guard<std::mutex> lk(mu_); show_.set_override(o); }
void LightingEngine::release_override() { std::lock_guard<std::mutex> lk(mu_); show_.clear_override(); }

LightingStatus LightingEngine::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  LightingStatus s;
  s.running = running_.load();
  s.outputs_enabled = cfg_.enabled;
  s.show = show_.status();
  s.issues = issues_;
  s.frames_rendered = rendered_;
  s.frames_sent = sent_;
  s.fixtures = static_cast<int>(show_.patch().fixtures.size());
  s.universes = show_.patch().universes;
  for (std::size_t i = 0; i < outputs_.size() && i < infos_.size(); ++i) {
    OutputInfo oi = infos_[i];
    oi.stats = outputs_[i]->stats();
    s.outputs.push_back(std::move(oi));
  }
  return s;
}

std::vector<UniverseFrame> LightingEngine::preview() const {
  std::lock_guard<std::mutex> lk(mu_);
  return show_.frames();
}
std::vector<Look> LightingEngine::preview_looks() const {
  std::lock_guard<std::mutex> lk(mu_);
  return show_.looks();
}

}  // namespace deckhand::lighting
