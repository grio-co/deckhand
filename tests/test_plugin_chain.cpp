// Plug-ins in effect chains: Apple's built-in units only (delay, low-pass, peak limiter, found by the scanner), through
// the in-process Engine and through the engine helper (null audio). No third-party plug-in is ever instantiated, no audio
// device is opened and nothing is played. Elsewhere (or with plug-ins off) the test passes as a stub.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "deckhand/audio/engine.hpp"
#include "deckhand/audio/ipc.hpp"
#include "deckhand/audio/plugin_host.hpp"
#include "deckhand/audio/remote_engine.hpp"
#include "deckhand/plugins/catalog.hpp"
#include "deckhand/plugins/effect.hpp"
#include "deckhand/plugins/plugin.hpp"

#if defined(__APPLE__) && !defined(DECKHAND_PLUGINS_DISABLED)
#include <signal.h>

using namespace deckhand;
namespace fs = std::filesystem;
namespace {
constexpr double kSr = 48000;
const double kPi = 3.14159265358979323846;

bool wait_for(const std::function<bool()>& p, double seconds) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < end) {
    if (p()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return p();
}

std::string find_id(const char* sub) {
  for (const auto& p : plugins::scan_installed())
    if (p.type == "aufx" && p.subtype == sub && p.manufacturer == "appl") return p.id;
  return {};
}

std::unique_ptr<LoadedTrack> mix_track() {
  auto audio = std::make_shared<TrackAudio>();
  audio->sample_rate = static_cast<std::uint32_t>(kSr);
  audio->frames = static_cast<std::uint64_t>(30 * kSr);
  audio->pcm.resize(audio->frames * 2);
  for (std::uint64_t i = 0; i < audio->frames; ++i) {
    const double t = static_cast<double>(i) / kSr;
    audio->pcm[i * 2] = audio->pcm[i * 2 + 1] =
        static_cast<float>(0.2 * std::sin(2 * kPi * 110 * t) + 0.15 * std::sin(2 * kPi * 1300 * t) + 0.1 * std::sin(2 * kPi * 6000 * t));
  }
  build_waveform(*audio);
  auto tr = std::make_unique<LoadedTrack>();
  tr->audio = audio;
  tr->bpm = 120;
  tr->grid.push_back({0, 120, 1});
  return tr;
}

double rms_of(Engine& e, double seconds) {
  std::vector<float> out(static_cast<std::size_t>(seconds * kSr) * 2);
  for (std::size_t done = 0; done < out.size() / 2;) {
    const std::size_t n = std::min<std::size_t>(256, out.size() / 2 - done);
    e.render(out.data() + done * 2, static_cast<std::uint32_t>(n));
    done += n;
  }
  double sum = 0;
  for (std::size_t i = out.size() / 4; i < out.size() / 2; ++i) {
    const double v = out[i * 2];
    CHECK(std::isfinite(v));
    sum += v * v;
  }
  return std::sqrt(sum / (static_cast<double>(out.size()) / 4));
}

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

int insert_plugin(IEngine& e, int target, const std::string& id, bool on, std::string* err = nullptr) {
  FxChainEdit ed;
  ed.verb = FxChainVerb::Insert;
  ed.target = target;
  ed.kind = EffectKind::Plugin;
  ed.text = id;
  ed.value = on ? 1 : 0;
  FxChainResult r;
  const bool ok = e.fx_chain_edit(ed, r);
  if (err) *err = r.error;
  return ok ? r.id : 0;
}
std::string info_json(IEngine& e, int target, int id) {
  PluginOp op;
  op.target = target;
  op.id = id;
  PluginOpResult r;
  return e.plugin_op(op, r) ? r.data : std::string();
}
bool set_param(IEngine& e, int target, int id, int index, double norm) {
  FxChainEdit ed;
  ed.verb = FxChainVerb::Set;
  ed.target = target;
  ed.id = id;
  ed.field = kFxFieldPluginParam + index;
  ed.value = norm;
  FxChainResult r;
  return e.fx_chain_edit(ed, r);
}

void test_in_process(const std::string& dir) {
  plugin_host_set_dir(dir + "/inproc");
  plugin_host_set_probe_exe(DH_PLUGIN_PROBE_EXE);
  const std::string lp = find_id("lpas"), delay = find_id("dely");
  CHECK(!lp.empty() && !delay.empty());

  Engine e;
  e.configure_offline(static_cast<std::uint32_t>(kSr), 2);
  e.load(0, mix_track());
  Command c;
  c.op = Op::PlayToggle;
  c.deck = 0;
  e.send(c);
  const double dry = rms_of(e, 1.0);

  std::string err;
  CHECK(insert_plugin(e, 0, "au:aufx:nope:nope", true, &err) == 0 && !err.empty());  // unknown id: refused, chain untouched
  CHECK(e.fx_chain(0).count == 0);
  const int id = insert_plugin(e, 0, lp, true, &err);
  CHECK(id > 0);
  CHECK(plugin_host_is_vetted(lp));  // the probe ran and was remembered
  const FxChainInfo ci = e.fx_chain(0);
  CHECK(ci.count == 1 && ci.unit[0].kind == EffectKind::Plugin && ci.unit[0].on);
  const std::string info = info_json(e, 0, id);
  CHECK(contains(info, "\"loaded\":true") && contains(info, "\"params\":[{"));
  CHECK(info_json(e, 0, 999).empty());
  CHECK(set_param(e, 0, id, 0, 0.0));  // lowest cut-off
  CHECK(!set_param(e, 0, id, 500, 0.5));
  CHECK(contains(info_json(e, 0, id), "\"norm\":0"));
  const double wet = rms_of(e, 1.5);
  CHECK(wet < dry * 0.8);  // the filter took the high tones away

  // preset with state: save, clear, load, the parameter is back
  const std::string full = fx_chain_save_full(e, 0);
  CHECK(contains(full, "\"type\":\"plugin\"") && contains(full, lp) && contains(full, "\"state\":\""));
  CHECK(e.fx_chain_clear(0) && e.fx_chain(0).count == 0);
  std::string lerr;
  CHECK(fx_chain_load_full(e, 0, full, &lerr));
  const FxChainInfo back = e.fx_chain(0);
  CHECK(back.count == 1 && back.unit[0].kind == EffectKind::Plugin);
  CHECK(rms_of(e, 1.5) < dry * 0.8);
  // the engine's own preset names the plug-in, without the state
  const std::string plain = e.fx_chain_save(0);
  CHECK(contains(plain, lp) && !contains(plain, "\"state\""));

  // second plug-in, reorder, remove with its tail, replace
  const int id2 = insert_plugin(e, 0, delay, true);
  CHECK(id2 > 0 && e.fx_chain(0).count == 2);
  CHECK(e.fx_move(0, id2, 0));
  CHECK(e.fx_chain(0).unit[0].id == id2);
  CHECK(rms_of(e, 0.5) >= 0);
  CHECK(e.fx_remove(0, id2));
  CHECK(e.fx_replace(0, e.fx_chain(0).unit[0].id, EffectKind::Echo) > 0);
  CHECK(e.fx_chain(0).unit[0].kind == EffectKind::Echo);
  rms_of(e, 0.3);

  // a preset that names a plug-in that is gone keeps the unit as a pass-through and says so
  std::string gone = "{\"v\":1,\"fx\":[{\"type\":\"plugin\",\"plugin\":\"au:aufx:xxxx:yyyy\",\"on\":true,\"wet\":1}]}";
  FxChainEdit ld;
  ld.verb = FxChainVerb::Load;
  ld.target = 1;
  ld.text = gone;
  FxChainResult lr;
  CHECK(e.fx_chain_edit(ld, lr) && lr.info.count == 1 && !lr.error.empty());
  rms_of(e, 0.2);

  // blocklisted: refused before anything is opened
  {
    plugin_host_set_dir(dir + "/blocked");
    fs::create_directories(dir + "/blocked");
    plugins::PluginCatalog cat(dir + "/blocked/plugins.cache");
    cat.block(lp, "test");
    std::string berr;
    CHECK(insert_plugin(e, 2, lp, false, &berr) == 0 && contains(berr, "blocked"));
    plugin_host_set_dir(dir + "/inproc");
  }
  // wrong probe helper: the unit is refused, nothing loads
  {
    plugin_host_set_dir(dir + "/noprobe");
    plugin_host_set_probe_exe(dir + "/does-not-exist");
    std::string perr;
    CHECK(insert_plugin(e, 2, delay, false, &perr) == 0 && contains(perr, "probe"));
    plugin_host_set_probe_exe(DH_PLUGIN_PROBE_EXE);
    plugin_host_set_dir(dir + "/inproc");
  }
}

// Host tempo and beat position reach the unit through the format's host callbacks (Apple's delay only; nothing is
// played). The unit reads them back through the same property a tempo-synced plug-in would use.
void test_host_transport() {
  const std::string delay = find_id("dely");
  CHECK(!delay.empty());
  plugins::PluginInfo info;
  for (const auto& p : plugins::scan_installed())
    if (p.id == delay) info = p;
  plugins::PluginEffect fx;
  std::string err;
  CHECK(fx.load(info, kSr, &err));
  plugins::HostTransport t;
  CHECK(fx.instance()->query_host_transport(t) == false);  // no tempo told yet: the callbacks refuse
  fx.set_on(true);
  fx.set_transport(128.0, 16.0, true);
  std::vector<float> buf(1024 * 2, 0.0f);
  fx.run(buf.data(), 1024);
  fx.set_transport(128.0, 16.0, true);
  fx.instance()->set_transport({128.0, 16.0, true});
  CHECK(fx.instance()->query_host_transport(t));
  CHECK(std::fabs(t.tempo_bpm - 128.0) < 1e-9 && t.playing && std::fabs(t.beat - 16.0) < 1e-9);
  // the beat advances with the frames rendered: one second at 120 bpm is two beats
  fx.set_transport(120.0, 4.0, true);
  std::vector<float> one(static_cast<std::size_t>(kSr) * 2, 0.0f);
  fx.run(one.data(), static_cast<int>(kSr));
  CHECK(fx.instance()->query_host_transport(t) && std::fabs(t.tempo_bpm - 120.0) < 1e-9 && t.playing);
  std::printf("beat read back after one second: %.4f\n", t.beat);
  CHECK(t.beat > 5.98 && t.beat < 5.99);  // start of the last 384-frame block of the second (93 x 512 frames in)
  std::printf("host transport read back: %.1f bpm, beat %.3f\n", t.tempo_bpm, t.beat);
}

void test_helper(const std::string& dir, const std::string& helper) {
  const std::string data = dir + "/helper";
  fs::create_directories(data);
  RemoteEngine::Options o;
  o.data_dir = data;
  o.helper_path = helper;
  o.null_audio = true;
  o.log_level = 2;
  o.backoff_ms = 100;
  RemoteEngine eng(o);
  std::vector<RemoteEngine::Status> statuses;
  std::mutex mu;
  eng.set_status_callback([&](RemoteEngine::Status s, const std::string&) {
    std::lock_guard<std::mutex> lock(mu);
    statuses.push_back(s);
  });
  std::string err;
  CHECK(eng.connect(err));
  OutputConfig oc;
  CHECK(eng.start(oc, err));

  const std::string lp = find_id("lpas");
  std::string ierr;
  CHECK(insert_plugin(eng, 0, "au:aufx:nope:nope", false, &ierr) == 0 && !ierr.empty());
  const int id = insert_plugin(eng, 0, lp, true, &ierr);
  CHECK(id > 0);
  const std::string info = info_json(eng, 0, id);
  CHECK(contains(info, "\"loaded\":true") && contains(info, "\"params\":[{"));
  CHECK(set_param(eng, 0, id, 0, 0.0));
  const std::string full = fx_chain_save_full(eng, 0);
  CHECK(contains(full, lp) && contains(full, "\"state\":\""));
  // a state blob that is larger than a chain edit's text still travels (side channel)
  PluginOp big;
  big.kind = PluginOpKind::SetState;
  big.target = 0;
  big.id = id;
  big.data.assign(200 * 1024, 'x');
  PluginOpResult br;
  CHECK(!eng.plugin_op(big, br));  // garbage state is refused by the plug-in, not by the wire
  CHECK(!br.error.empty());
  CHECK(eng.fx_chain(0).count == 1);

  // the engine process dies: the chain, with the plug-in and its parameter, comes back with the new one
  eng.snapshot_chains_now();  // otherwise the monitor thread takes it a moment after the last edit
  const std::uint32_t old_pid = eng.engine_pid();
  kill(static_cast<pid_t>(old_pid), SIGKILL);
  CHECK(wait_for([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (auto s : statuses)
      if (s == RemoteEngine::Status::Restarted) return true;
    return false;
  }, 20));
  CHECK(eng.engine_pid() != old_pid);
  CHECK(wait_for([&] { return eng.fx_chain(0).count == 1; }, 20));
  const FxChainInfo back = eng.fx_chain(0);
  CHECK(back.unit[0].kind == EffectKind::Plugin);
  const std::string info2 = info_json(eng, 0, back.unit[0].id);
  CHECK(contains(info2, "\"loaded\":true") && contains(info2, lp));
  eng.shutdown_remote(0);
}
}  // namespace

int main(int, char** argv) {
  const std::string dir = (fs::temp_directory_path() / ("dh-plugin-chain-" + std::to_string(ipc::current_pid()))).string();
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  test_in_process(dir);
  test_host_transport();
  std::string helper = (fs::absolute(argv[0]).parent_path() / "deckhand-engine").string();
  if (const char* h = std::getenv("DECKHAND_ENGINE_BIN")) helper = h;
  if (fs::exists(helper, ec)) test_helper(dir, helper);
  else std::printf("deckhand-engine not built: skipping the helper part\n");
  fs::remove_all(dir, ec);
  std::printf("plugin chain ok\n");
  return 0;
}

#else
int main() {
  std::printf("plug-in chains: not supported on this platform / build (stub)\n");
  return 0;
}
#endif
