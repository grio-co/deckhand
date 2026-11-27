// Plug-in hosting. macOS: Apple's built-in Audio Units (system delay, low-pass filter, peak limiter) found through the
// scanner, rendered offline on synthetic buffers: no audio device, no sound, no third-party plug-in is ever instantiated.
// Elsewhere: the stub compiles and reports "not supported".
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "deckhand/plugins/catalog.hpp"
#include "deckhand/plugins/effect.hpp"
#include "deckhand/plugins/plugin.hpp"
#include "deckhand/plugins/probe.hpp"

using namespace deckhand::plugins;

#if defined(__APPLE__) && !defined(DECKHAND_PLUGINS_DISABLED)

namespace fs = std::filesystem;
static constexpr double kSr = 48000;

static const PluginInfo* find_apple(const std::vector<PluginInfo>& all, const char* sub) {
  for (const auto& p : all)
    if (p.type == "aufx" && p.subtype == sub && p.manufacturer == "appl") return &p;
  return nullptr;
}

static int pid_by_name(PluginInstance& in, const char* needle) {
  for (const auto& p : in.params()) {
    std::string n = p.name;
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    if (n.find(needle) != std::string::npos) return (int)p.id;
  }
  return -1;
}

struct Buf {
  std::vector<float> l, r;
  explicit Buf(int n = 0) : l((std::size_t)n, 0.f), r((std::size_t)n, 0.f) {}
};

static void run(PluginInstance& in, const Buf& src, Buf& dst) {
  dst = Buf((int)src.l.size());
  const float* ip[2] = {src.l.data(), src.r.data()};
  float* op[2] = {dst.l.data(), dst.r.data()};
  in.process(ip, op, (int)src.l.size());
}

static Buf impulse(int n) {
  Buf b(n);
  b.l[0] = b.r[0] = 1.0f;
  return b;
}
static Buf sine(int n, double hz, float amp) {
  Buf b(n);
  for (int i = 0; i < n; ++i) b.l[(std::size_t)i] = b.r[(std::size_t)i] = amp * (float)std::sin(2 * M_PI * hz * i / kSr);
  return b;
}
static double rms(const std::vector<float>& v, std::size_t from = 0) {
  double s = 0;
  for (std::size_t i = from; i < v.size(); ++i) s += (double)v[i] * v[i];
  return std::sqrt(s / (double)(v.size() - from));
}
static double peak(const std::vector<float>& v) {
  double p = 0;
  for (float x : v) p = std::max(p, (double)std::fabs(x));
  return p;
}
static int peak_index(const std::vector<float>& v) {
  int k = 0;
  for (std::size_t i = 0; i < v.size(); ++i)
    if (std::fabs(v[i]) > std::fabs(v[(std::size_t)k])) k = (int)i;
  return k;
}

static std::unique_ptr<PluginInstance> open_apple(const std::vector<PluginInfo>& all, const char* sub, int block = 512) {
  const PluginInfo* p = find_apple(all, sub);
  CHECK(p != nullptr);
  std::string err;
  auto in = open_plugin(*p, kSr, block, &err);
  if (!in) std::fprintf(stderr, "open %s failed: %s\n", sub, err.c_str());
  CHECK(in != nullptr);
  return in;
}

static void set_delay(PluginInstance& d, double seconds, double feedback, double wet) {
  CHECK(d.set_plain((std::uint32_t)pid_by_name(d, "delay time"), seconds));
  CHECK(d.set_plain((std::uint32_t)pid_by_name(d, "feedback"), feedback));
  CHECK(d.set_plain((std::uint32_t)pid_by_name(d, "wet"), wet));
  d.flush_params();
}

static void test_scan_and_catalog() {
  CHECK(hosting_supported());
  const auto all = scan_installed();
  CHECK(!all.empty());
  for (const char* s : {"dely", "lpas", "lmtr"}) {
    const PluginInfo* p = find_apple(all, s);
    CHECK(p != nullptr);
    CHECK(!p->name.empty());
    CHECK(p->maker == "Apple");
    CHECK(p->version != 0);
    CHECK(p->id.rfind("au:", 0) == 0);
  }
  const fs::path dir = fs::temp_directory_path() / ("dh_plugins_test_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  const std::string cache = (dir / "plugins.cache").string();
  {
    PluginCatalog c(cache);
    CHECK(!c.load());          // no file yet
    CHECK(c.refresh());        // scanned and wrote it
    CHECK(c.plugins().size() == all.size());
  }
  {
    PluginCatalog c(cache);
    CHECK(!c.refresh());       // valid cache is reused
    CHECK(c.plugins().size() == all.size());
    const PluginInfo* d = find_apple(c.plugins(), "dely");
    CHECK(d && d->subtype_code == 0x64656c79 && d->has_midi == false);
    c.block(d->id, "test: crashed\tin probe");
    CHECK(c.blocked(d->id));
    CHECK(c.usable().size() == all.size() - 1);
  }
  {
    PluginCatalog c(cache);    // blocklist survives
    c.load();
    CHECK(c.blocklist().size() == 1);
    const PluginInfo* d = find_apple(c.plugins(), "dely");
    CHECK(d && d->blocked);
    c.unblock(d->id);
    CHECK(c.usable().size() == all.size());
  }
  {
    // A stale stamp rescans.
    std::string txt;
    {
      FILE* f = std::fopen(cache.c_str(), "rb");
      CHECK(f);
      char b[4096];
      std::size_t n;
      while ((n = std::fread(b, 1, sizeof b, f)) > 0) txt.append(b, n);
      std::fclose(f);
    }
    const std::size_t k = txt.find("stamp\t");
    CHECK(k != std::string::npos);
    txt.insert(k + 6, "stale-");
    FILE* f = std::fopen(cache.c_str(), "wb");
    std::fwrite(txt.data(), 1, txt.size(), f);
    std::fclose(f);
    PluginCatalog c(cache);
    CHECK(!c.load());
    CHECK(c.refresh());
    CHECK(c.plugins().size() == all.size());
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

static void test_delay_processing(const std::vector<PluginInfo>& all) {
  auto d = open_apple(all, "dely", 512);  // 512 max block, 8192-frame call: exercises internal chunking
  CHECK(d->params().size() >= 3);
  for (const auto& p : d->params()) CHECK(p.max >= p.min);
  const int pid = pid_by_name(*d, "delay time");
  CHECK(pid >= 0);
  set_delay(*d, 0.05, 0.0, 100.0);
  Buf out;
  run(*d, impulse(8192), out);
  CHECK(peak(out.l) > 0.3);
  const int k = peak_index(out.l);
  CHECK(std::abs(k - 2400) <= 96);               // 50 ms at 48 kHz
  CHECK(std::fabs(out.l[0]) < 0.05);             // wet only: nothing at time zero
  CHECK(d->render_errors() == 0);
  CHECK(d->latency_seconds() >= 0 && d->tail_seconds() >= 0);
  CHECK(d->latency_frames() >= 0);
}

static void test_params(const std::vector<PluginInfo>& all) {
  auto d = open_apple(all, "dely");
  const std::uint32_t id = (std::uint32_t)pid_by_name(*d, "delay time");
  const ParamInfo& p = d->params()[(std::size_t)d->param_index(id)];
  CHECK(!p.name.empty() && p.max > p.min && p.def >= p.min && p.def <= p.max);
  CHECK(d->set_plain(id, p.min + 0.25 * (p.max - p.min)));
  CHECK(std::fabs(d->get_norm(id) - 0.25) < 1e-3);
  CHECK(d->set_norm(id, 0.5));
  CHECK(std::fabs(d->get_plain(id) - (p.min + 0.5 * (p.max - p.min))) < 1e-3 * (p.max - p.min) + 1e-6);
  d->flush_params();
  CHECK(std::fabs(d->get_norm(id) - 0.5) < 1e-3);  // the unit itself holds it now
  CHECK(d->set_plain(id, p.max * 10));             // clamped
  CHECK(std::fabs(d->get_plain(id) - p.max) < 1e-4);
  CHECK(!d->set_plain(0xdeadbeef, 1));
  CHECK(d->param_index(0xdeadbeef) == -1);
  CHECK(!d->value_string(id, p.min).empty());
  // A discrete (indexed) parameter, if the low-pass has none this is skipped; the system EQ-style units usually do.
  bool saw_discrete = false;
  for (const auto& q : d->params()) saw_discrete = saw_discrete || q.steps > 0;
  (void)saw_discrete;
  // Thread-safe set while another thread renders.
  std::atomic<bool> stop{false};
  std::thread t([&] {
    double v = 0;
    while (!stop.load()) {
      d->set_norm(id, v);
      v += 0.01;
      if (v > 1) v = 0;
    }
  });
  Buf in = sine(512, 440, 0.2f), out;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < deadline) run(*d, in, out);
  stop = true;
  t.join();
  CHECK(d->render_errors() == 0);
}

static void test_lowpass(const std::vector<PluginInfo>& all) {
  auto lp = open_apple(all, "lpas");
  const int pid = pid_by_name(*lp, "cutoff");
  CHECK(pid >= 0);
  CHECK(lp->set_plain((std::uint32_t)pid, 500.0));
  lp->flush_params();
  Buf lo, hi;
  run(*lp, sine(8192, 100, 0.5f), lo);
  lp->reset();
  run(*lp, sine(8192, 8000, 0.5f), hi);
  CHECK(rms(hi.l, 4096) < 0.1 * rms(lo.l, 4096));
}

static void test_limiter(const std::vector<PluginInfo>& all) {
  auto lm = open_apple(all, "lmtr");
  Buf in = sine(24000, 220, 4.0f), out;  // far above full scale
  run(*lm, in, out);
  for (float v : out.l) CHECK(std::isfinite(v));
  CHECK(peak(out.l) < peak(in.l) * 0.6);  // clearly limited (settles once the look-ahead is full)
}

static void test_bypass_and_inplace(const std::vector<PluginInfo>& all) {
  auto d = open_apple(all, "dely");
  set_delay(*d, 0.01, 0.0, 100.0);
  Buf in = sine(1024, 330, 0.5f), out;
  d->set_bypass(true);
  CHECK(d->bypass());
  run(*d, in, out);
  CHECK(out.l == in.l && out.r == in.r);
  d->set_bypass(false);
  run(*d, in, out);
  CHECK(out.l != in.l);
  // in place
  Buf a = impulse(4096), b = a;
  set_delay(*d, 0.02, 0.0, 100.0);
  d->reset();
  Buf ref;
  run(*d, a, ref);
  d->reset();
  float* ch[2] = {b.l.data(), b.r.data()};
  d->process(ch, ch, 4096);
  CHECK(b.l == ref.l);
}

static void test_state_and_presets(const std::vector<PluginInfo>& all) {
  auto a = open_apple(all, "dely");
  set_delay(*a, 0.031, 0.0, 100.0);
  std::vector<std::uint8_t> blob;
  CHECK(a->save_state(blob));
  CHECK(blob.size() > 16);
  auto b = open_apple(all, "dely");
  set_delay(*b, 0.2, 40.0, 30.0);  // different settings
  CHECK(b->restore_state(blob.data(), blob.size()));
  const std::uint32_t id = (std::uint32_t)pid_by_name(*a, "delay time");
  CHECK(std::fabs(b->get_plain(id) - 0.031) < 1e-4);
  Buf ra, rb;
  run(*a, impulse(8192), ra);
  run(*b, impulse(8192), rb);
  double maxd = 0;
  for (std::size_t i = 0; i < ra.l.size(); ++i) maxd = std::max(maxd, (double)std::fabs(ra.l[i] - rb.l[i]));
  CHECK(maxd < 1e-6);
  CHECK(!b->restore_state(blob.data(), 5));  // garbage is refused
  std::vector<std::uint8_t> junk(64, 0x41);
  CHECK(!b->restore_state(junk.data(), junk.size()));
  for (const auto& p : a->presets()) CHECK(!p.name.empty());  // list only: may be empty
  auto pr = a->presets();
  if (!pr.empty()) CHECK(a->load_preset(pr[0].number));
}

static void test_reset(const std::vector<PluginInfo>& all) {
  auto ring = open_apple(all, "dely");
  auto clean = open_apple(all, "dely");
  for (auto* d : {ring.get(), clean.get()}) set_delay(*d, 0.02, 70.0, 100.0);
  Buf out, sil(512);
  for (auto* d : {ring.get(), clean.get()}) run(*d, impulse(512), out);
  clean->reset();
  double ring_peak = 0, clean_peak = 0;
  for (int i = 0; i < 40; ++i) {
    run(*ring, sil, out);
    ring_peak = std::max(ring_peak, peak(out.l));
    run(*clean, sil, out);
    clean_peak = std::max(clean_peak, peak(out.l));
  }
  CHECK(ring_peak > 0.01);     // the feedback tail rings on
  CHECK(clean_peak < 1e-6);    // reset cleared it
}

static void test_probe(const std::vector<PluginInfo>& all) {
  const PluginInfo* p = find_apple(all, "dely");
  CHECK(p);
  ProbeResult r = probe_plugin(*p);
  if (!r.ok) std::fprintf(stderr, "probe: %s\n", r.error.c_str());
  CHECK(r.ok && r.params >= 3);
  PluginInfo bogus = *p;
  bogus.subtype_code = 0x7a7a7a7a;
  bogus.id = "au:bogus";
  CHECK(!probe_plugin(bogus).ok);
#ifdef DH_PLUGIN_PROBE_EXE
  ProbeResult o = probe_out_of_process(DH_PLUGIN_PROBE_EXE, p->id, 20000);
  if (!o.ok) std::fprintf(stderr, "out-of-process probe: %s\n", o.error.c_str());
  CHECK(o.ok && o.params == r.params);
  CHECK(!probe_out_of_process(DH_PLUGIN_PROBE_EXE, "au:00000000:00000000:00000000", 20000).ok);
  CHECK(!probe_out_of_process("/nonexistent/probe", p->id, 1000).ok);
#endif
}

static void test_adapter(const std::vector<PluginInfo>& all) {
  PluginEffect fx;
  CHECK(!fx.busy());
  std::string err;
  CHECK(fx.load(*find_apple(all, "dely"), kSr, &err));
  CHECK(fx.param_count() >= 3);
  set_delay(*fx.instance(), 0.05, 0.0, 100.0);
  fx.set_wet(1.0f);
  fx.set_on(true);
  std::vector<float> buf(2 * 8192, 0.0f);
  buf[0] = buf[1] = 1.0f;
  fx.run(buf.data(), 8192);
  std::vector<float> l(8192);
  for (int i = 0; i < 8192; ++i) l[(std::size_t)i] = buf[(std::size_t)(2 * i)];
  // insert mode, mix ramps in over ~5 ms: the dry impulse is mostly gone by sample 0? (m starts at 0: dry passes first)
  CHECK(l[0] > 0.9f);
  double echo = 0;
  for (int i = 2200; i < 2600; ++i) echo = std::max(echo, (double)std::fabs(l[(std::size_t)i]));
  CHECK(echo > 0.3);
  CHECK(fx.busy());
  // switch off in insert mode: fades out then idles
  fx.set_on(false);
  std::vector<float> sil(2 * 512, 0.0f);
  for (int i = 0; i < 20; ++i) fx.run(sil.data(), 512);
  CHECK(!fx.busy());
  // send mode: rings out its tail then goes idle
  fx.set_send_mode(true);
  fx.reset();
  set_delay(*fx.instance(), 0.02, 50.0, 100.0);
  fx.set_on(true);
  std::vector<float> imp(2 * 512, 0.0f);
  imp[0] = imp[1] = 1.0f;
  fx.run(imp.data(), 512);
  fx.set_on(false);
  CHECK(fx.busy());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  int blocks = 0;
  while (fx.busy() && std::chrono::steady_clock::now() < deadline && blocks < 100000) {
    std::fill(sil.begin(), sil.end(), 0.0f);
    fx.run(sil.data(), 512);
    ++blocks;
  }
  CHECK(!fx.busy());
  // state round trip through the unit
  std::vector<std::uint8_t> st;
  CHECK(fx.save_state(st));
  PluginEffect fx2;
  CHECK(fx2.load(*find_apple(all, "dely"), kSr, &err));
  CHECK(fx2.restore_state(st.data(), st.size()));
  CHECK(!fx2.on() && fx2.wet() == 1.0f);
}

int main() {
  test_scan_and_catalog();
  const auto all = scan_installed();
  test_delay_processing(all);
  test_params(all);
  test_lowpass(all);
  test_limiter(all);
  test_bypass_and_inplace(all);
  test_state_and_presets(all);
  test_reset(all);
  test_probe(all);
  test_adapter(all);
  std::puts("test_plugins: ok");
  return 0;
}

#else  // stub platforms

int main() {
  CHECK(!hosting_supported());
  CHECK(scan_installed().empty());
  PluginInfo info;
  std::string err;
  CHECK(open_plugin(info, 48000, 512, &err) == nullptr && !err.empty());
  PluginEffect fx;
  CHECK(!fx.load(info, 48000, nullptr));
  float buf[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0, 0, 0, 0};
  fx.set_on(true);
  fx.run(buf, 4);  // unloaded: untouched
  CHECK(buf[0] == 0.1f && buf[3] == 0.4f);
  CHECK(!probe_plugin(info).ok);
  std::puts("test_plugins: ok (stub)");
  return 0;
}

#endif
