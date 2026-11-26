// Stem separation: chunking, mixture consistency, cache, queue, manifests, ONNX loader (when built in) and
// real models (when installed under third_party/models). Everything but the last two runs without any model.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <process.h>
#define DH_GETPID _getpid
#else
#include <unistd.h>
#define DH_GETPID ::getpid
#endif

#include "check.hpp"
#include "deckhand/stems/cache.hpp"
#include "deckhand/stems/checkpoint.hpp"
#include "deckhand/stems/demucs_dsp.hpp"
#include "deckhand/stems/model_convert.hpp"
#include "deckhand/stems/model_install.hpp"
#include "deckhand/stems/manifest.hpp"
#include "deckhand/stems/queue.hpp"
#include "deckhand/stems/stems.hpp"

using namespace deckhand::stems;
namespace fs = std::filesystem;

static int g_tests = 0;
#define TEST(name) static void name(); struct name##_reg { name##_reg() { run_list().push_back({#name, name}); } } name##_inst; static void name()
struct Case { const char* name; void (*fn)(); };
static std::vector<Case>& run_list() { static std::vector<Case> v; return v; }

static fs::path tmpdir(const char* tag) {
  static int n = 0;
  fs::path p = fs::temp_directory_path() / ("dh_stems_test_" + std::string(tag) + "_" + std::to_string(DH_GETPID()) + "_" + std::to_string(n++));
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

static Audio noise(std::size_t n, int rate, unsigned seed = 1, float amp = 0.5f) {
  std::mt19937 g(seed);
  std::uniform_real_distribution<float> d(-amp, amp);
  Audio a;
  a.sample_rate = rate;
  a.l.resize(n); a.r.resize(n);
  for (std::size_t i = 0; i < n; ++i) { a.l[i] = d(g); a.r[i] = d(g); }
  return a;
}
static Audio tone(std::size_t n, int rate, double hz, float amp) {
  Audio a;
  a.sample_rate = rate;
  a.l.resize(n); a.r.resize(n);
  for (std::size_t i = 0; i < n; ++i) { a.l[i] = amp * float(std::sin(2 * M_PI * hz * double(i) / rate)); a.r[i] = 0.7f * a.l[i]; }
  return a;
}

// Test doubles ---------------------------------------------------------------------------------------------------
// Fixed-gain split (sums to 1): the "identity" model.
struct GainModel : StemModel {
  ModelInfo mi;
  float g[4] = {0.4f, 0.3f, 0.2f, 0.1f};
  int calls = 0;
  std::function<void(int)> on_call;
  float edge_error = 0;  // corrupts the first/last 128 samples of every chunk (what real models do at borders)
  GainModel(std::size_t seg, int rate = 44100) { mi.id = "gain-test"; mi.sample_rate = rate; mi.segment_frames = seg; }
  const ModelInfo& info() const override { return mi; }
  bool run(const float* in, float* out, std::string*) override {
    ++calls;
    if (on_call) on_call(calls);
    const std::size_t S = mi.segment_frames;
    for (int s = 0; s < 4; ++s)
      for (int c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < S; ++i) {
          float v = g[s] * in[c * S + i];
          if (edge_error != 0 && (i < 128 || i >= S - 128)) v += edge_error * 0.1f;
          out[(std::size_t(s) * 2 + std::size_t(c)) * S + i] = v;
        }
    return true;
  }
};
// Returns stems whose sum is NOT the input (each is 0.5 x, sum = 2x).
struct BadSumModel : GainModel {
  using GainModel::GainModel;
  bool run(const float* in, float* out, std::string* e) override {
    for (int s = 0; s < 4; ++s) g[s] = 0.5f;
    return GainModel::run(in, out, e);
  }
};

static double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
  CHECK(a.size() == b.size());
  double m = 0;
  for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(double(a[i]) - b[i]));
  return m;
}
static double sum_error(const Audio& mix, const Stems& st) {
  double m = 0;
  for (std::size_t i = 0; i < mix.frames(); ++i) {
    double l = 0, r = 0;
    for (auto& s : st) { l += s.l[i]; r += s.r[i]; }
    m = std::max({m, std::fabs(l - mix.l[i]), std::fabs(r - mix.r[i])});
  }
  return m;
}

// ---- hashing & small utilities ---------------------------------------------------------------------------------
TEST(sha256_vectors) {
  CHECK(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::string m(1000000, 'a');
  CHECK(sha256_hex(m.data(), m.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(stem_names) {
  for (int i = 0; i < 4; ++i) CHECK(stem_from_name(stem_name(i)) == i);
  CHECK(stem_from_name("guitar") == -1);
}

TEST(resample_roundtrip) {
  const int a = 44100, b = 48000;
  Audio t = tone(44100, a, 1000, 0.5f);
  auto up = resample(t.l, a, b);
  CHECK(up.size() == 48000);
  auto back = resample(up, b, a);
  CHECK(back.size() == t.l.size());
  double err = 0;
  for (std::size_t i = 2000; i + 2000 < back.size(); ++i) err = std::max(err, std::fabs(double(back[i]) - t.l[i]));
  CHECK(err < 2e-3);
  // downsampling removes content above the new Nyquist
  Audio hi = tone(44100, a, 15000, 0.5f);
  auto lo = resample(hi.l, a, 22050);
  double peak = 0;
  for (std::size_t i = 500; i + 500 < lo.size(); ++i) peak = std::max(peak, std::fabs(double(lo[i])));
  CHECK(peak < 0.02);
  // DC gain is exactly one
  std::vector<float> dc(5000, 0.25f);
  auto d2 = resample(dc, 44100, 96000);
  CHECK(std::fabs(d2[2500] - 0.25f) < 1e-4);
  CHECK(resample({}, 44100, 48000).empty());
}

// ---- separation core -------------------------------------------------------------------------------------------
TEST(overlap_add_identity_reconstructs) {
  for (std::size_t n : {std::size_t(1), std::size_t(100), std::size_t(4096), std::size_t(4097), std::size_t(10000), std::size_t(33333)})
    for (float ov : {0.f, 0.25f, 0.5f}) {
      Audio in = noise(n, 44100, unsigned(n));
      GainModel m(4096);
      SeparateOptions o;
      o.overlap = ov;
      o.mixture = MixtureMode::None;
      auto r = separate(in, m, o);
      CHECK(r.status == SeparateResult::Status::Ok);
      for (int s = 0; s < 4; ++s) {
        CHECK(r.stems[std::size_t(s)].frames() == n);
        CHECK(max_abs_diff(r.stems[std::size_t(s)].l, [&] { auto v = in.l; for (auto& x : v) x *= m.g[s]; return v; }()) < 1e-6);
      }
      CHECK(sum_error(in, r.stems) < 1e-6);
    }
}

TEST(no_seams_at_chunk_borders) {
  // A model that is wrong in the first and last 128 samples of every chunk: with cross-fades the error at each
  // border must stay far below the error a hard seam would give, and no step may exceed the model's own error.
  Audio in = tone(60000, 44100, 220, 0.5f);
  GainModel m(8192);
  m.edge_error = 1.0f;  // 0.1 absolute error at the chunk edges
  SeparateOptions o;
  o.overlap = 0.25f;
  o.mixture = MixtureMode::None;
  auto r = separate(in, m, o);
  CHECK(r.status == SeparateResult::Status::Ok);
  const auto& v = r.stems[0].l;
  double worst_step = 0;
  for (std::size_t i = 300; i + 300 < v.size(); ++i) worst_step = std::max(worst_step, std::fabs(double(v[i]) - v[i - 1]) - 0.4 * std::fabs(double(in.l[i]) - in.l[i - 1]));
  std::fprintf(stderr, "worst_step %g\n", worst_step);
  CHECK(worst_step < 0.002);  // a seam would be a jump of ~0.04 (0.4 * 0.1)
  // with zero overlap the same model does produce the hard seams (the check above is meaningful)
  GainModel m2(8192);
  m2.edge_error = 1.0f;
  SeparateOptions o2;
  o2.overlap = 0;
  o2.mixture = MixtureMode::None;
  auto r2 = separate(in, m2, o2);
  double step2 = 0;
  for (std::size_t i = 300; i + 300 < r2.stems[0].l.size(); ++i) step2 = std::max(step2, std::fabs(double(r2.stems[0].l[i]) - r2.stems[0].l[i - 1]) - 0.4 * std::fabs(double(in.l[i]) - in.l[i - 1]));
  CHECK(step2 > 0.02);
}

TEST(mixture_consistency_exact) {
  Audio in = noise(30000, 44100, 7);
  for (MixtureMode mode : {MixtureMode::ToOther, MixtureMode::Proportional}) {
    BadSumModel m(4096);
    SeparateOptions o;
    o.mixture = mode;
    auto r = separate(in, m, o);
    CHECK(r.status == SeparateResult::Status::Ok);
    CHECK(sum_error(in, r.stems) < 2e-6);
  }
  BadSumModel m(4096);
  SeparateOptions o;
  o.mixture = MixtureMode::None;
  auto r = separate(in, m, o);
  CHECK(sum_error(in, r.stems) > 0.1);
  // proportional mode spreads the correction (vocals get part of it), ToOther leaves vocals untouched
  BadSumModel m2(4096), m3(4096);
  SeparateOptions po, to;
  po.mixture = MixtureMode::Proportional;
  to.mixture = MixtureMode::ToOther;
  auto rp = separate(in, m2, po), rt = separate(in, m3, to);
  CHECK(max_abs_diff(rp.stems[0].l, rt.stems[0].l) > 0.05);
}

TEST(resampled_path_sums_and_length) {
  for (int rate : {48000, 96000, 22050}) {
    Audio in = noise(40000, rate, 3, 0.3f);
    GainModel m(8192, 44100);
    auto r = separate(in, m);
    CHECK(r.status == SeparateResult::Status::Ok);
    CHECK(r.stems[2].frames() == in.frames() && r.stems[2].sample_rate == rate);
    CHECK(sum_error(in, r.stems) < 2e-6);
  }
}

TEST(edge_cases) {
  GainModel m(4096);
  Audio empty;
  empty.sample_rate = 44100;
  auto r = separate(empty, m);
  CHECK(r.status == SeparateResult::Status::Ok && r.stems[0].frames() == 0);
  Audio bad;
  bad.sample_rate = 0;
  bad.l = {0.f}; bad.r = {0.f};
  CHECK(separate(bad, m).status == SeparateResult::Status::Failed);
  Audio shorter = noise(10, 44100);
  CHECK(sum_error(shorter, separate(shorter, m).stems) < 1e-6);
}

TEST(progress_and_cancel) {
  Audio in = noise(100000, 44100);
  GainModel m(4096);
  std::vector<float> seen;
  SeparateOptions o;
  o.progress = [&](float f) { seen.push_back(f); };
  CHECK(separate(in, m, o).status == SeparateResult::Status::Ok);
  CHECK(seen.size() > 5 && seen.front() == 0.f && seen.back() == 1.f);
  CHECK(std::is_sorted(seen.begin(), seen.end()));
  // cancel mid-way
  std::atomic<bool> cancel{false};
  GainModel m2(4096);
  m2.on_call = [&](int n) { if (n == 3) cancel = true; };
  SeparateOptions o2;
  o2.cancel = &cancel;
  auto r = separate(in, m2, o2);
  CHECK(r.status == SeparateResult::Status::Cancelled);
  CHECK(m2.calls == 3);
  // failing model
  struct Fail : GainModel { using GainModel::GainModel; bool run(const float*, float*, std::string* e) override { *e = "boom"; return false; } } f(4096);
  auto rf = separate(in, f);
  CHECK(rf.status == SeparateResult::Status::Failed && rf.error == "boom");
}

TEST(pause_blocks_then_resumes) {
  Audio in = noise(50000, 44100);
  GainModel m(4096);
  std::atomic<bool> pause{true};
  SeparateOptions o;
  o.pause = &pause;
  std::atomic<bool> finished{false};
  std::thread t([&] { separate(in, m, o); finished = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK(!finished && m.calls == 0);
  pause = false;
  t.join();
  CHECK(finished && m.calls > 0);
  // cancel wins over pause
  pause = true;
  std::atomic<bool> cancel{false};
  o.cancel = &cancel;
  GainModel m2(4096);
  std::thread t2([&] { CHECK(separate(in, m2, o).status == SeparateResult::Status::Cancelled); });
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  cancel = true;
  t2.join();
}

// ---- WAV -------------------------------------------------------------------------------------------------------
TEST(wav_roundtrip) {
  fs::path d = tmpdir("wav");
  Audio a = noise(1000, 48000, 5, 0.8f);
  for (int bits : {16, 24, 32}) {
    std::string p = (d / ("t" + std::to_string(bits) + ".wav")).string();
    std::string err;
    CHECK(write_wav(p, a, bits, &err));
    Audio b;
    CHECK(read_wav(p, b, &err));
    CHECK(b.sample_rate == 48000 && b.frames() == 1000);
    CHECK(max_abs_diff(a.l, b.l) < (bits == 32 ? 1e-9 : bits == 24 ? 2e-7 : 4e-5));
  }
  Audio b;
  std::string err;
  CHECK(!read_wav((d / "missing.wav").string(), b, &err));
  { std::ofstream(d / "junk.wav") << "not a wav file at all, definitely not, no no no no no"; }
  CHECK(!read_wav((d / "junk.wav").string(), b, &err));
  fs::remove_all(d);
}

// ---- cache -----------------------------------------------------------------------------------------------------
static Stems fake_stems(std::size_t n, int rate, unsigned seed) {
  Stems s;
  for (int i = 0; i < 4; ++i) s[std::size_t(i)] = noise(n, rate, seed + unsigned(i), 0.9f);
  return s;
}

TEST(key_hashing) {
  Audio a = noise(5000, 44100, 1);
  std::string k = content_key(a, "m1");
  CHECK(k.size() == 64);
  CHECK(content_key(a, "m1") == k);
  CHECK(content_key(a, "m2") != k);       // model id is part of the key
  Audio b = a;
  b.l[2500] += 0.01f;
  CHECK(content_key(b, "m1") != k);        // content change
  Audio c = a;
  c.sample_rate = 48000;
  CHECK(content_key(c, "m1") != k);        // rate is part of the content
  Audio d = a;
  d.l[10] += 1e-7f;                        // sub-16-bit decoder noise does not change the key
  CHECK(content_key(d, "m1") == k);
  // incremental feeding in odd pieces gives the same key (no path involved anywhere)
  KeyHasher h("m1");
  h.add(a.l.data(), a.r.data(), 17);
  h.add(a.l.data() + 17, a.r.data() + 17, 4983);
  CHECK(h.finish(44100) == k);
}

TEST(cache_roundtrip_and_partial_reads) {
  for (int bits : {16, 24}) {
    fs::path d = tmpdir("cache");
    CacheConfig cfg;
    cfg.root = d;
    cfg.bits = bits;
    StemCache c(cfg);
    Stems s = fake_stems(12345, 44100, 10);
    std::string key = content_key(s[0], "m");
    CHECK(!c.has_stems(key));
    std::string err;
    CHECK(c.store(key, s, &err));
    CHECK(c.has_stems(key));
    CHECK(c.total_bytes() == StemCache::expected_size(12345, bits));
    CHECK(fs::file_size(c.path_for(key)) == c.total_bytes());
    Stems back;
    CHECK(c.load_stems(key, back, &err));
    const double tol = bits == 16 ? 1.5e-4 : 1e-6;
    for (int i = 0; i < 4; ++i) {
      CHECK(back[std::size_t(i)].sample_rate == 44100);
      CHECK(max_abs_diff(back[std::size_t(i)].l, s[std::size_t(i)].l) < tol);
      CHECK(max_abs_diff(back[std::size_t(i)].r, s[std::size_t(i)].r) < tol);
    }
    // random access and zero fill past the end
    auto rd = c.open(key);
    CHECK(rd && rd->frames() == 12345 && rd->bits() == bits);
    std::vector<float> buf(100);
    CHECK(rd->read(2, 1, 5000, 100, buf.data()));
    for (int i = 0; i < 100; ++i) CHECK(std::fabs(buf[std::size_t(i)] - s[2].r[5000 + std::size_t(i)]) < tol);
    CHECK(rd->read(0, 0, 12300, 100, buf.data()));
    CHECK(std::fabs(buf[10] - s[0].l[12310]) < tol && buf[60] == 0.f);
    CHECK(c.verify(key));
    // a new cache object over the same folder sees it (persisted index)
    StemCache c2(cfg);
    CHECK(c2.has_stems(key) && c2.count() == 1);
    rd.reset();  // Windows cannot delete a file that is still open
    CHECK(c.evict(key) && !c.has_stems(key) && c.total_bytes() == 0);
    fs::remove_all(d);
  }
}

TEST(cache_loud_stems_not_clipped) {
  fs::path d = tmpdir("loud");
  StemCache c(CacheConfig{d});
  Stems s = fake_stems(2000, 44100, 3);
  for (auto& a : s) for (auto& x : a.l) x *= 3.f;  // beyond full scale
  std::string key = content_key(s[0], "m");
  CHECK(c.store(key, s));
  Stems b;
  CHECK(c.load_stems(key, b));
  CHECK(max_abs_diff(b[1].l, s[1].l) < 3e-4);
  fs::remove_all(d);
}

TEST(cache_lru_eviction_and_cap) {
  fs::path d = tmpdir("lru");
  CacheConfig cfg;
  cfg.root = d;
  const std::uint64_t one = StemCache::expected_size(4000, 16);
  cfg.cap_bytes = one * 3 + one / 2;  // room for three
  StemCache c(cfg);
  std::vector<std::string> keys;
  for (int i = 0; i < 3; ++i) {
    Stems s = fake_stems(4000, 44100, unsigned(i) * 10);
    keys.push_back(content_key(s[0], "m"));
    CHECK(c.store(keys.back(), s));
  }
  CHECK(c.count() == 3 && c.total_bytes() == 3 * one);
  CHECK(c.open(keys[0]));  // touch the oldest: keys[1] is now least recently used
  Stems s4 = fake_stems(4000, 44100, 99);
  std::string k4 = content_key(s4[0], "m");
  CHECK(c.store(k4, s4));
  CHECK(c.count() == 3);
  CHECK(!c.has_stems(keys[1]) && c.has_stems(keys[0]) && c.has_stems(keys[2]) && c.has_stems(k4));
  CHECK(c.total_bytes() <= cfg.cap_bytes);
  auto e = c.entries();
  CHECK(e.size() == 3 && e.back().key == k4);  // most recent last
  c.set_cap_bytes(one);                           // shrinking the cap evicts down to it
  CHECK(c.count() == 1 && c.has_stems(k4) == true);
  // an entry bigger than the cap is kept when it is the one just stored
  c.set_cap_bytes(one / 2);
  Stems big = fake_stems(20000, 44100, 5);
  std::string kb = content_key(big[0], "m");
  CHECK(c.store(kb, big));
  CHECK(c.has_stems(kb));
  fs::remove_all(d);
}

TEST(cache_atomic_and_crash_safety) {
  fs::path d = tmpdir("atomic");
  CacheConfig cfg;
  cfg.root = d;
  StemCache c(cfg);
  Stems s = fake_stems(3000, 44100, 1);
  std::string key = content_key(s[0], "m");
  CHECK(c.store(key, s));
  // simulate a crash mid-write of a different key: a stray temp file and a truncated final file
  std::string k2(64, 'a');
  fs::create_directories(d / "aa");
  { std::ofstream f(d / "aa" / (k2 + ".4242.0.tmp"), std::ios::binary); f << "partial"; }
  fs::path good = c.path_for(key);
  std::vector<char> bytes(fs::file_size(good));
  { std::ifstream f(good, std::ios::binary); f.read(bytes.data(), std::streamsize(bytes.size())); }
  std::string k3(64, 'b');
  fs::create_directories(d / "bb");
  { std::ofstream f(d / "bb" / (k3 + ".dhstems"), std::ios::binary); f.write(bytes.data(), std::streamsize(bytes.size() / 2)); }
  StemCache c2(cfg);  // rescan on construction
  CHECK(c2.has_stems(key));
  CHECK(!c2.has_stems(k2) && !c2.has_stems(k3));
  CHECK(!fs::exists(d / "bb" / (k3 + ".dhstems")));  // truncated file removed, never served
  CHECK(fs::exists(d / "aa" / (k2 + ".4242.0.tmp")));  // young temp files may belong to a live writer
  c2.rescan(0);
  CHECK(!fs::exists(d / "aa" / (k2 + ".4242.0.tmp")));
  // a failed store leaves the previous file intact and no temp file behind
  Stems bad = s;
  bad[1].r.resize(10);
  std::string err;
  CHECK(!c2.store(key, bad, &err));
  CHECK(c2.has_stems(key) && c2.verify(key));
  for (auto& e : fs::recursive_directory_iterator(d)) CHECK(e.path().extension() != ".tmp");
  // bit rot is caught by verify() (and size changes by has_stems after a rescan)
  { std::fstream f(good, std::ios::in | std::ios::out | std::ios::binary); f.seekp(1000); char x = 0x55; f.write(&x, 1); }
  CHECK(!c2.verify(key));
  // concurrent stores of the same key leave exactly one valid file
  fs::remove(good);
  c2.rescan();
  std::vector<std::thread> th;
  for (int i = 0; i < 4; ++i) th.emplace_back([&] { CHECK(c2.store(key, s)); });
  for (auto& t : th) t.join();
  CHECK(c2.verify(key) && c2.count() == 1);
  int files = 0;
  for (auto& e : fs::recursive_directory_iterator(d)) if (e.is_regular_file()) ++files;
  CHECK(files == 1);
  CHECK(!c2.open("not a key"));
  fs::remove_all(d);
}

// ---- queue -----------------------------------------------------------------------------------------------------
static JobSpec job_of(const Audio& a, const std::string& label) {
  JobSpec j;
  j.label = label;
  j.load = [a](Audio& out, std::string*) { out = a; return true; };
  return j;
}

TEST(queue_runs_in_order_and_caches) {
  fs::path d = tmpdir("queue");
  StemCache cache(CacheConfig{d});
  int models_built = 0;
  QueueOptions fast;
  fast.low_priority = false;
  StemQueue q(cache, "gain-test", [&](std::string*) { ++models_built; return std::make_unique<GainModel>(4096); }, fast);
  Audio a = noise(20000, 44100, 1), b = noise(20000, 44100, 2);
  auto ia = q.enqueue(job_of(a, "a")), ib = q.enqueue(job_of(b, "b")), ia2 = q.enqueue(job_of(a, "a again"));
  CHECK(q.wait_idle(90000));
  CHECK(q.job(ia).state == JobState::Done && q.job(ib).state == JobState::Done);
  CHECK(q.job(ia2).state == JobState::AlreadyCached);  // same content, other label: no second separation
  CHECK(q.job(ia).progress == 1.f && q.job(ia).key == q.job(ia2).key && q.job(ia).key != q.job(ib).key);
  CHECK(cache.count() == 2 && cache.has_stems(q.job(ia).key));
  CHECK(models_built == 1);
  Stems s;
  CHECK(cache.load_stems(q.job(ia).key, s));
  CHECK(sum_error(a, s) < 3e-4);  // 16-bit storage of four stems
  CHECK(q.jobs().size() == 3);
  // failures
  JobSpec bad;
  bad.label = "bad";
  bad.load = [](Audio&, std::string* e) { *e = "decode failed"; return false; };
  auto ibad = q.enqueue(bad);
  CHECK(q.wait_idle(120000));
  CHECK(q.job(ibad).state == JobState::Failed && q.job(ibad).error == "decode failed");
  fs::remove_all(d);
}

TEST(queue_pause_cancel_and_callbacks) {
  fs::path d = tmpdir("queue2");
  StemCache cache(CacheConfig{d});
  QueueOptions fast;
  fast.low_priority = false;
  StemQueue q(cache, "gain-test", [&](std::string*) { return std::make_unique<GainModel>(4096); }, fast);
  std::atomic<int> changes{0};
  q.set_on_change([&](const JobInfo&) { ++changes; });
  q.pause();
  auto j1 = q.enqueue(job_of(noise(30000, 44100, 1), "1"));
  auto j2 = q.enqueue(job_of(noise(30000, 44100, 2), "2"));
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  CHECK(q.job(j1).state == JobState::Queued && q.paused() && q.pending() == 2);
  CHECK(q.cancel(j2));  // cancel while queued
  q.resume();
  CHECK(q.wait_idle(90000));
  CHECK(q.job(j1).state == JobState::Done && q.job(j2).state == JobState::Cancelled);
  CHECK(!q.cancel(j1));  // already finished
  CHECK(changes > 4);
  // cancel while running: a gate inside the model makes this deterministic (no sleeps, no polling)
  struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool entered = false, release = false;
  } gate;
  struct Gated : GainModel {
    Gate& g;
    Gated(std::size_t seg, Gate& gt) : GainModel(seg), g(gt) {}
    bool run(const float* i, float* o, std::string* e) override {
      {
        std::unique_lock<std::mutex> lk(g.m);
        g.entered = true;
        g.cv.notify_all();
        g.cv.wait(lk, [&] { return g.release; });
      }
      return GainModel::run(i, o, e);
    }
  };
  QueueOptions qo;
  qo.low_priority = false;
  StemQueue q2(cache, "gain-test", [&](std::string*) { return std::make_unique<Gated>(2048, gate); }, qo);
  auto j3 = q2.enqueue(job_of(noise(400000, 44100, 3), "gated"));
  {
    std::unique_lock<std::mutex> lk(gate.m);
    gate.cv.wait(lk, [&] { return gate.entered; });  // the model is inside its first chunk, so the job is Running
  }
  CHECK(q2.job(j3).state == JobState::Running);
  CHECK(q2.cancel(j3));
  {
    std::lock_guard<std::mutex> lk(gate.m);
    gate.release = true;
  }
  gate.cv.notify_all();
  CHECK(q2.wait_idle(120000));
  CHECK(q2.job(j3).state == JobState::Cancelled && !cache.has_stems(q2.job(j3).key));
  fs::remove_all(d);
}

TEST(queue_model_factory_failure) {
  fs::path d = tmpdir("queue3");
  StemCache cache(CacheConfig{d});
  QueueOptions fast;
  fast.low_priority = false;
  StemQueue q(cache, "x", [](std::string* e) -> std::unique_ptr<StemModel> { *e = "no model installed"; return nullptr; }, fast);
  auto j = q.enqueue(job_of(noise(1000, 44100), "t"));
  CHECK(q.wait_idle(120000));
  CHECK(q.job(j).state == JobState::Failed && q.job(j).error == "no model installed");
  fs::remove_all(d);
}

// ---- manifests -------------------------------------------------------------------------------------------------
static std::string manifest_json(const std::string& file, const std::string& sha, const char* mode) {
  std::string out = std::string("{\"format\":\"deckhand-stems-model/1\",\"id\":\"gain4-") + mode + "\",\"name\":\"Fixed gain split\",";
  out += "\"file\":\"" + file + "\",\"licence\":\"CC0 (generated by tools/make_stems_test_models.py)\",\"source_url\":\"https://example.invalid/none\",";
  out += "\"sha256\":\"" + sha + "\",\"sample_rate\":44100,\"segment_frames\":4096,\"overlap\":0.25,\"channels\":2,";
  if (!std::strcmp(mode, "multi")) out += R"J("input":{"name":"mix","layout":"BCT"},"output":{"names":["s0","s1","s2","s3"],"layout":"BCT"},)J";
  else if (!std::strcmp(mode, "bsct")) out += R"J("input":{"name":"mix","layout":"BTC"},"output":{"name":"stems","layout":"BSCT"},)J";
  else out += R"J("input":{"name":"mix","layout":"BCT"},"output":{"name":"stems","layout":"BTCS"},)J";
  out += R"J("stems":["other","bass","drums","vocals"]})J";
  return out;
}

TEST(manifest_parse_and_reject) {
  ModelManifest m;
  std::string err;
  std::string sha(64, 'a');
  CHECK(parse_manifest(manifest_json("x.onnx", sha, "multi"), m, &err));
  CHECK(m.id == "gain4-multi" && m.segment_frames == 4096 && m.output_names.size() == 4 && m.stems[0] == "other");
  CHECK(m.input_layout == "BCT" && m.licence.find("CC0") == 0);
  // each of these must be rejected with a message
  auto bad = [&](const std::string& from, const std::string& to) {
    std::string j = manifest_json("x.onnx", sha, "multi");
    auto p = j.find(from);
    CHECK(p != std::string::npos);
    j.replace(p, from.size(), to);
    ModelManifest mm;
    std::string e;
    bool ok = parse_manifest(j, mm, &e);
    if (ok) std::fprintf(stderr, "accepted: %s -> %s\n", from.c_str(), to.c_str());
    CHECK(!ok && !e.empty());
  };
  bad("deckhand-stems-model/1", "deckhand-stems-model/2");
  bad("\"x.onnx\"", "\"../x.onnx\"");
  bad("\"x.onnx\"", "\"a/b/x.onnx\"");
  bad("\"x.onnx\"", "\"/x.onnx\"");
  bad("\"x.onnx\"", "\"c:x.onnx\"");
  bad("\"x.onnx\"", "\"\"");
  bad(sha, std::string(63, 'a'));
  bad(sha, std::string(64, 'A'));
  bad("\"sample_rate\":44100", "\"sample_rate\":3");
  bad("\"segment_frames\":4096", "\"segment_frames\":5");
  bad("\"channels\":2", "\"channels\":1");
  bad("\"overlap\":0.25", "\"overlap\":0.9");
  bad("\"other\",\"bass\",\"drums\",\"vocals\"", "\"other\",\"bass\",\"drums\",\"drums\"");
  bad("\"layout\":\"BCT\"},\"output\"", "\"layout\":\"BXT\"},\"output\"");
  bad("\"id\":\"gain4-multi\"", "\"id\":\"has space\"");
  bad("\"licence\":\"CC0 (generated by tools/make_stems_test_models.py)\",", "");
  bad("\"names\":[\"s0\",\"s1\",\"s2\",\"s3\"],\"layout\":\"BCT\"", "\"names\":[\"s0\"],\"layout\":\"BCT\"");
  bad("\"layout\":\"BCT\"},\"stems\"", "\"layout\":\"BSCT\"},\"stems\"");  // S axis with per-stem tensors
  CHECK(!parse_manifest("{", m, &err));
  CHECK(!parse_manifest("[1,2]", m, &err));
  CHECK(!parse_manifest(std::string(10, '[') + std::string(10, ']'), m, &err));
  std::string deep(200, '['), tail(200, ']');
  CHECK(!parse_manifest(deep + tail, m, &err));
}

TEST(manifest_dir_listing_and_hash_check) {
  fs::path d = tmpdir("models");
  { std::ofstream(d / "m.onnx", std::ios::binary) << "pretend weights"; }
  std::string sha = sha256_hex("pretend weights", 15);
  { std::ofstream(d / "good.dhstems.json") << manifest_json("m.onnx", sha, "multi"); }
  { std::ofstream(d / "broken.dhstems.json") << "{ nope"; }
  { std::ofstream(d / "other.txt") << "x"; }
  std::vector<std::string> problems;
  auto v = list_models(d, &problems);
  for (auto& p : problems) std::fprintf(stderr, "problem: %s\n", p.c_str());
  CHECK(v.size() == 1 && problems.size() == 1 && v[0].dir == d);
  std::string err;
  CHECK(verify_model_file(v[0], &err));
  { std::ofstream(d / "m.onnx", std::ios::binary) << "tampered weights"; }
  CHECK(!verify_model_file(v[0], &err) && err.find("mismatch") != std::string::npos);
  auto m = open_model(v[0], 1, &err);  // never loads a file whose hash is wrong, backend or not
  CHECK(!m && !err.empty());
  CHECK(list_models(d / "nonexistent").empty());
  fs::remove_all(d);
}

// ---- ONNX loader (only when the runtime is built in) -------------------------------------------------------------
#ifdef DH_STEMS_TEST_DIR
static void onnx_case(const char* mode, const char* file) {
  fs::path d = tmpdir("onnx");
  fs::copy_file(fs::path(DH_STEMS_TEST_DIR) / file, d / file);
  std::string sha;
  CHECK(sha256_file(d / file, sha));
  { std::ofstream(d / "t.dhstems.json") << manifest_json(file, sha, mode); }
  auto v = list_models(d);
  CHECK(v.size() == 1);
  std::string err;
  auto model = open_model(v[0], 1, &err);
  if (!stems_available()) { CHECK(!model && !err.empty()); fs::remove_all(d); return; }
  if (!model) std::fprintf(stderr, "open_model: %s\n", err.c_str());
  CHECK(model);
  CHECK(model->info().segment_frames == 4096 && model->info().sample_rate == 44100);
  // the manifest lists the model's output order as other, bass, drums, vocals with gains 0.1..0.4:
  // canonical vocals must therefore carry gain 0.4, drums 0.3, bass 0.2, other 0.1
  Audio in = noise(30000, 44100, 8);
  auto r = separate(in, *model);
  CHECK(r.status == SeparateResult::Status::Ok);
  const float want[4] = {0.4f, 0.3f, 0.2f, 0.1f};
  for (int s = 0; s < 4; ++s) {
    auto ref = in.l;
    for (auto& x : ref) x *= want[s];
    CHECK(max_abs_diff(r.stems[std::size_t(s)].l, ref) < 1e-5);
  }
  CHECK(sum_error(in, r.stems) < 2e-6);
  // a wrong tensor name is reported, not crashed on
  ModelManifest bad = v[0];
  bad.input_name = "nope";
  CHECK(!open_model(bad, 1, &err) && !err.empty());
  // a manifest whose segment does not match the model's fixed shape fails at run time with a message
  ModelManifest wrong = v[0];
  wrong.segment_frames = 2048;
  auto wm = open_model(wrong, 1, &err);
  if (wm) { std::vector<float> i(2 * 2048), o(8 * 2048); std::string e; CHECK(!wm->run(i.data(), o.data(), &e) && !e.empty()); }
  fs::remove_all(d);
}
TEST(onnx_multi_output_bct) { onnx_case("multi", "gain4_multi.onnx"); }
TEST(onnx_single_output_btc_in_bsct_out) { onnx_case("bsct", "gain4_bsct.onnx"); }
TEST(onnx_single_output_btcs_layout) { onnx_case("btcs", "gain4_btcs.onnx"); }
#endif


// ---- checkpoint reader (hostile-input handling) ------------------------------------------------------------------
static void write_zip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& files) {
  std::string out, cd;
  auto w32 = [](std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s += char(v >> (8 * i)); };
  auto w16 = [](std::string& s, std::uint16_t v) { s += char(v); s += char(v >> 8); };
  for (auto& f : files) {
    std::uint32_t off = std::uint32_t(out.size());
    std::string crc_src = f.second;
    w32(out, 0x04034b50); w16(out, 20); w16(out, 0); w16(out, 0); w32(out, 0); w32(out, 0);
    w32(out, std::uint32_t(f.second.size())); w32(out, std::uint32_t(f.second.size())); w16(out, std::uint16_t(f.first.size())); w16(out, 0);
    out += f.first; out += f.second;
    w32(cd, 0x02014b50); w16(cd, 20); w16(cd, 20); w16(cd, 0); w16(cd, 0); w32(cd, 0); w32(cd, 0);
    w32(cd, std::uint32_t(f.second.size())); w32(cd, std::uint32_t(f.second.size())); w16(cd, std::uint16_t(f.first.size()));
    w16(cd, 0); w16(cd, 0); w16(cd, 0); w16(cd, 0); w32(cd, 0); w32(cd, off); cd += f.first;
  }
  std::uint32_t cdoff = std::uint32_t(out.size());
  out += cd;
  w32(out, 0x06054b50); w16(out, 0); w16(out, 0); w16(out, std::uint16_t(files.size())); w16(out, std::uint16_t(files.size()));
  w32(out, std::uint32_t(cd.size())); w32(out, cdoff); w16(out, 0);
  std::ofstream(p, std::ios::binary) << out;
}

TEST(checkpoint_reads_real_torch_file) {
  fs::path f = fs::path(DH_STEMS_TEST_DIR) / "tiny_state.pt";
  if (!fs::exists(f)) { std::printf("  (tiny_state.pt missing: skipped)\n"); return; }
  Checkpoint c;
  std::string err;
  CHECK(read_checkpoint(f, c, &err));
  CHECK(c.tensors.size() == 4 && c.tensors[0].name == "a.weight" && c.tensors[0].shape == std::vector<std::int64_t>({2, 3}));
  std::vector<float> v;
  CHECK(read_tensor_f32(f, c.tensors[0], v, &err) && v.size() == 6 && v[4] == 4.f);
  CHECK(read_tensor_f32(f, c.tensors[1], v, &err) && v.size() == 4 && v[0] == 0.5f && v[1] == -1.f && v[3] == 0.25f);  // float16
  CHECK(read_tensor_f32(f, c.tensors[2], v, &err) && v.size() == 6);  // transposed view: [[0,2,4],[1,3,5]]
  CHECK(v[0] == 0.f && v[1] == 2.f && v[2] == 4.f && v[3] == 1.f && v[5] == 5.f);
  CHECK(read_tensor_f32(f, c.tensors[3], v, &err) && v[0] == 7.f && v[1] == -3.f);
}

TEST(checkpoint_rejects_hostile_input) {
  fs::path d = tmpdir("ckpt");
  auto try_pickle = [&](const std::string& pk, const char* why) {
    write_zip(d / "x.pt", {{"archive/data.pkl", pk}});
    Checkpoint c;
    std::string err;
    bool ok = read_checkpoint(d / "x.pt", c, &err);
    if (ok) std::fprintf(stderr, "accepted hostile pickle: %s\n", why);
    CHECK(!ok && !err.empty());
  };
  const std::string P2 = "\x80\x02";
  try_pickle(P2 + "cos\nsystem\nX\x07\x00\x00\x00" "echo hi" "\x85R.", "os.system");
  try_pickle(P2 + "cbuiltins\neval\nX\x03\x00\x00\x00" "1+1" "\x85R.", "builtins.eval");
  try_pickle(P2 + "csubprocess\nPopen\n)R.", "subprocess");
  try_pickle(P2 + "ccollections\nOrderedDict\n)R}b.", "BUILD with a non-OrderedDict target is fine only for OrderedDict");  // target is an OrderedDict but then no state dict -> rejected below
  try_pickle(P2 + "(S'abc'\n.", "STRING opcode");
  try_pickle(P2 + "cposix\nsystem\n.", "bare global, no tensors");
  try_pickle(P2 + "N\x81.", "NEWOBJ opcode");
  try_pickle(P2 + "c__builtin__\nobject\n)\x81.", "NEWOBJ of object");
  try_pickle(P2 + "X\xff\xff\xff\x7f", "huge string");
  try_pickle(P2 + "N", "no STOP");
  try_pickle(std::string(), "empty pickle");
  try_pickle(P2 + "h\x05.", "memo get without put");
  try_pickle(P2 + "]]]]]].", "stack not unit at STOP");
  // no pickle at all / not a zip / compressed member / path traversal member
  { std::ofstream(d / "junk.pt", std::ios::binary) << "this is not a zip file at all"; Checkpoint c; std::string e; CHECK(!read_checkpoint(d / "junk.pt", c, &e)); }
  { write_zip(d / "nopkl.pt", {{"archive/data/0", "abcd"}}); Checkpoint c; std::string e; CHECK(!read_checkpoint(d / "nopkl.pt", c, &e)); }
  { write_zip(d / "trav.pt", {{"../evil/data.pkl", "."}}); Checkpoint c; std::string e; CHECK(!read_checkpoint(d / "trav.pt", c, &e)); }
  { Checkpoint c; std::string e; CHECK(!read_checkpoint(d / "missing.pt", c, &e)); }
  // a tensor whose view runs past its storage is rejected, not read
  fs::path tiny = fs::path(DH_STEMS_TEST_DIR) / "tiny_state.pt";
  if (fs::exists(tiny)) {
    Checkpoint c;
    std::string e;
    CHECK(read_checkpoint(tiny, c, &e));
    CkptTensor t = c.tensors[0];
    t.shape = {1000, 1000}; t.stride = {1000, 1}; t.numel = 1000000;
    std::vector<float> v;
    CHECK(!read_tensor_f32(tiny, t, v, &e));
    // a truncated copy of a valid checkpoint fails cleanly
    std::ifstream in(tiny, std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (std::size_t cut : {all.size() / 2, all.size() - 10, std::size_t(100)}) {
      std::ofstream(d / "cut.pt", std::ios::binary) << all.substr(0, cut);
      Checkpoint c2;
      CHECK(!read_checkpoint(d / "cut.pt", c2, &e));
    }
  }
  fs::remove_all(d);
}

TEST(checkpoint_reads_installed_models_if_present) {
  fs::path root = fs::path(DH_STEMS_MODELS_DIR);
  struct Case { const char* rel; const char* wrapper; std::uint64_t min_params; };
  const Case cases[] = {{"demucs/955717e8-8726e21a.th", "demucs.htdemucs HTDemucs", 30000000}, {"roformer/MelBandRoformer.ckpt", "", 50000000}};
  bool any = false;
  for (auto& c : cases) {
    fs::path f = root / c.rel;
    if (!fs::exists(f)) continue;
    any = true;
    Checkpoint ck;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    bool rok = read_checkpoint(f, ck, &err);
    if (!rok) std::fprintf(stderr, "  %s: %s\n", c.rel, err.c_str());
    CHECK(rok);
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::uint64_t n = 0;
    for (auto& t : ck.tensors) n += t.numel;
    std::printf("  %s: %zu tensors, %.1f M parameters, parsed in %.2f s (wrapper '%s')\n", c.rel, ck.tensors.size(), double(n) / 1e6, dt, ck.wrapper_class.c_str());
    CHECK(ck.wrapper_class == c.wrapper && n > c.min_params);
    std::vector<float> v;
    CHECK(read_tensor_f32(f, ck.tensors[0], v, &err));
    for (float x : v) CHECK(std::isfinite(x));
  }
  if (!any) std::printf("  (no downloaded checkpoints: skipped)\n");
}


// ---- Demucs signal processing and the checkpoint converter -------------------------------------------------------------
TEST(demucs_dsp_matches_reference_and_reconstructs) {
  const std::size_t n = 5000;
  std::vector<float> mix(2 * n);
  for (std::size_t i = 0; i < n; ++i) {
    float l = 0.3f * std::sin(2 * float(M_PI) * 440.f * float(i) / 44100.f) + 0.1f * std::sin(2 * float(M_PI) * 3000.f * float(i) / 44100.f);
    mix[i] = l; mix[n + i] = 0.5f * l;
  }
  std::vector<float> spec;
  DemucsDsp::analyse(mix.data(), n, spec);
  const std::size_t le = DemucsDsp::frames_for(n);
  CHECK(le == 5 && spec.size() == 4 * DemucsDsp::kBins * le);
  // reference values produced by the upstream implementation (torch.stft with demucs' padding) for this signal
  struct Ref { int c, b, f; float v; };
  const Ref refs[] = {{0, 10, 1, 0.000744187564f}, {1, 10, 1, -0.0243318006f}, {0, 70, 3, 0.00520384219f}, {2, 10, 4, -0.0526717193f}, {3, 350, 2, -3.54234288e-07f}, {1, 1, 0, -0.0946986601f}};
  for (auto& r : refs) CHECK(std::fabs(spec[(std::size_t(r.c) * DemucsDsp::kBins + std::size_t(r.b)) * le + std::size_t(r.f)] - r.v) < 2e-6);
  // analysis followed by synthesis reproduces the signal away from the borders (the first and last two frames are
  // dropped by the algorithm itself, exactly as upstream does)
  const std::size_t n2 = 30000;
  Audio rnd = tone(n2, 44100, 1234, 0.4f);  // band-limited: the algorithm drops the Nyquist bin, so white noise is not exact
  std::vector<float> m2(2 * n2), sp2, back;
  std::copy(rnd.l.begin(), rnd.l.end(), m2.begin());
  std::copy(rnd.r.begin(), rnd.r.end(), m2.begin() + long(n2));
  DemucsDsp::analyse(m2.data(), n2, sp2);
  DemucsDsp::synthesise(sp2.data(), 1, n2, back);
  CHECK(back.size() == 2 * n2);
  double err = 0;
  for (std::size_t c = 0; c < 2; ++c)
    for (std::size_t i = 4000; i + 4000 < n2; ++i) err = std::max(err, std::fabs(double(back[c * n2 + i]) - m2[c * n2 + i]));
  CHECK(err < 1e-5);
}

TEST(converter_writes_layout_exactly) {
  fs::path tiny = fs::path(DH_STEMS_TEST_DIR) / "tiny_state.pt";
  if (!fs::exists(tiny)) { std::printf("  (tiny_state.pt missing: skipped)\n"); return; }
  fs::path d = tmpdir("convert");
  // layout for the four tiny tensors: a.weight [2,3] at 0, b.bias [4] at 64, c.t [2,3] (transposed view) at 128, d.idx [2] at 192
  { std::ofstream(d / "l.json") << R"J({"format":"deckhand-stems-layout/1","total_bytes":256,"tensors":[
    {"name":"a.weight","offset":0,"length":24,"shape":[2,3]},{"name":"b.bias","offset":64,"length":16,"shape":[4]},
    {"name":"c.t","offset":128,"length":24,"shape":[2,3]},{"name":"d.idx","offset":192,"length":8,"shape":[2]}]})J"; }
  std::string err;
  CHECK(convert_checkpoint(tiny, d / "l.json", d / "w.bin", nullptr, &err));
  std::vector<float> v(64);
  {  // closed before fs::remove_all below: Windows cannot delete a file that is still open (the throw was the 0xc0000409 abort)
    std::ifstream f(d / "w.bin", std::ios::binary);
    f.read(reinterpret_cast<char*>(v.data()), 256);
  }
  CHECK(fs::file_size(d / "w.bin") == 256);
  CHECK(v[0] == 0.f && v[5] == 5.f);                         // a.weight
  CHECK(v[16] == 0.5f && v[17] == -1.f && v[19] == 0.25f);   // b.bias (float16 widened)
  CHECK(v[32] == 0.f && v[33] == 2.f && v[34] == 4.f && v[35] == 1.f);  // c.t contiguous copy of the transposed view
  CHECK(v[48] == 7.f && v[49] == -3.f);                      // d.idx converted from int64
  CHECK(v[10] == 0.f && v[40] == 0.f);                       // alignment gaps are zero
  // a layout that does not match the checkpoint is refused and leaves nothing behind
  { std::ofstream(d / "bad.json") << R"J({"format":"deckhand-stems-layout/1","total_bytes":64,"tensors":[{"name":"a.weight","offset":0,"length":24,"shape":[3,2]}]})J"; }
  CHECK(!convert_checkpoint(tiny, d / "bad.json", d / "w2.bin", nullptr, &err) && !fs::exists(d / "w2.bin") && !fs::exists(d / "w2.bin.part"));
  { std::ofstream(d / "bad2.json") << R"J({"format":"deckhand-stems-layout/1","total_bytes":64,"tensors":[{"name":"a.weight","offset":0,"length":24,"shape":[2,3]},
    {"name":"b.bias","offset":0,"length":16,"shape":[4]},{"name":"c.t","offset":64,"length":24,"shape":[2,3]},{"name":"d.idx","offset":128,"length":8,"shape":[2]}]})J"; }
  CHECK(!convert_checkpoint(tiny, d / "bad2.json", d / "w3.bin", nullptr, &err));  // overlapping placement
  CHECK(!convert_checkpoint(tiny, d / "missing.json", d / "w4.bin", nullptr, &err));
  fs::remove_all(d);
}

// ---- catalogue and installer -----------------------------------------------------------------------------------------
static CatalogueEntry test_entry(const std::string& content, bool supported) {
  CatalogueEntry e;
  e.id = "test-model"; e.name = "Test"; e.install_supported = supported; e.weights_licence = "TEST-ONLY"; e.licence_summary = "research only (test)";
  CatalogueFile f;
  f.name = "w.bin"; f.url = "https://huggingface.co/x/y/resolve/main/w.bin"; f.size = content.size(); f.sha256 = sha256_hex(content.data(), content.size());
  e.files.push_back(f);
  return e;
}
static DownloadFn fake_download(const std::string& content, int* calls = nullptr) {
  return [content, calls](const std::string&, const fs::path& dest, std::uint64_t, const std::function<void(std::uint64_t)>& cb, const std::atomic<bool>* cancel, std::string* err) {
    if (calls) ++*calls;
    if (cancel && cancel->load()) { *err = "cancelled"; return false; }
    std::ofstream(dest, std::ios::binary) << content;
    if (cb) cb(content.size());
    return true;
  };
}

TEST(catalogue_files_are_valid) {
  std::vector<std::string> problems;
  auto v = list_catalogue(fs::path(DH_STEMS_TEST_DIR) / ".." / ".." / "resources" / "stem-models", &problems);
  for (auto& p : problems) std::fprintf(stderr, "catalogue problem: %s\n", p.c_str());
  CHECK(problems.empty() && v.size() == 4);
  for (auto& e : v) {
    CHECK(!e.licence_summary.empty() && !e.licence_link.empty() && (!e.files.empty() || e.kind == "recipe"));
    for (auto& f : e.files) CHECK(url_host_allowed(f.url) && f.sha256.size() == 64 && f.size > 1000000);
  }
  CHECK(v[0].id == "demucs-htdemucs" && v[1].id == "demucs-htdemucs-ft" && v[1].files.size() == 4);
  CHECK(v[0].restriction == "research-only" && v[2].restriction == "permissive-declared");
  CHECK(v[0].licence_summary.find("not covered by the MIT licence") != std::string::npos);
  CHECK(v[3].id == "recipe-roformer-demucs" && v[3].kind == "recipe" && v[3].files.empty() && v[3].requires_ids.size() == 2 && v[3].install_supported);
  CHECK(v[2].install_supported && v[1].install_supported);
}

TEST(catalogue_rejects_bad_entries) {
  CatalogueEntry e;
  std::string err;
  auto good = R"J({"format":"deckhand-stems-catalogue/1","id":"m","name":"M","files":[{"name":"a.bin","url":"https://huggingface.co/a/b","size":10,"sha256":"__SHA__"}],"licence":{"summary":"x"}})J";
  std::string sha(64, 'a');
  auto mk = [&](std::string s) { auto p = s.find("__SHA__"); if (p != std::string::npos) s.replace(p, 7, sha); return s; };
  CHECK(parse_catalogue_entry(mk(good), e, &err));
  auto rep = [&](const char* a, const char* b) { std::string s = mk(good); auto p = s.find(a); CHECK(p != std::string::npos); s.replace(p, std::strlen(a), b); return s; };
  CHECK(!parse_catalogue_entry(rep("https://huggingface.co/a/b", "https://evil.example/a"), e, &err));
  CHECK(!parse_catalogue_entry(rep("https://huggingface.co/a/b", "http://huggingface.co/a/b"), e, &err));
  CHECK(!parse_catalogue_entry(rep("https://huggingface.co/a/b", "https://huggingface.co@evil.example/a"), e, &err));
  CHECK(!parse_catalogue_entry(rep("https://huggingface.co/a/b", "https://huggingface.co.evil.example/a"), e, &err));
  CHECK(!parse_catalogue_entry(rep("a.bin", "../a.bin"), e, &err));
  CHECK(!parse_catalogue_entry(rep("\"size\":10,", ""), e, &err));
  CHECK(!parse_catalogue_entry(rep("{\"summary\":\"x\"}", "{}"), e, &err));
  sha = "short";
  CHECK(!parse_catalogue_entry(mk(good), e, &err));
}

TEST(install_gate_verify_cancel_remove) {
  fs::path d = tmpdir("install");
  const std::string content = "weights bytes for the test";
  CatalogueEntry e = test_entry(content, true);
  int calls = 0;
  InstallHooks hk;
  hk.download = fake_download(content, &calls);
  // 1. licence not accepted: nothing downloaded
  auto r = install_model(d, e, false, nullptr, nullptr, hk);
  CHECK(!r.ok && calls == 0 && r.error.find("licence") != std::string::npos);
  // 2. entry whose conversion does not exist yet is refused before any download
  CatalogueEntry blocked = test_entry(content, false);
  blocked.blocked_by = "no skeleton";
  r = install_model(d, blocked, true, nullptr, nullptr, hk);
  CHECK(!r.ok && calls == 0 && r.error.find("no skeleton") != std::string::npos);
  // 3. wrong content: hash mismatch, nothing left behind
  InstallHooks wrong;
  wrong.download = fake_download("tampered");
  r = install_model(d, e, true, nullptr, nullptr, wrong);
  CHECK(!r.ok);
  for (auto& x : fs::recursive_directory_iterator(d)) CHECK(x.path().filename() != "w.bin" && x.path().extension() != ".part");
  // 4. host not on the list is refused whatever the catalogue says
  CatalogueEntry evil = e;
  evil.files[0].url = "https://evil.example/w.bin";
  r = install_model(d, evil, true, nullptr, nullptr, hk);
  CHECK(!r.ok && calls == 0);
  // 5. cancel
  std::atomic<bool> cancel{true};
  r = install_model(d, e, true, nullptr, &cancel, hk);
  CHECK(!r.ok && r.cancelled);
  // 6. success, with progress, a licence record, and a conversion hook
  std::vector<float> prog;
  bool converted = false;
  hk.convert = [&](const fs::path& dir, const CatalogueEntry&, std::string*) { converted = fs::exists(dir / "w.bin"); return true; };
  r = install_model(d, e, true, [&](float f) { prog.push_back(f); }, nullptr, hk);
  CHECK(r.ok && converted && !prog.empty() && prog.back() == 1.f && std::is_sorted(prog.begin(), prog.end()));
  auto inst = installed_models(d);
  CHECK(inst.size() == 1 && inst[0].id == "test-model" && inst[0].licence == "TEST-ONLY" && !inst[0].licence_accepted_at.empty() && !inst[0].usable);
  // 7. already-installed files are not downloaded again
  int before = calls;
  CHECK(install_model(d, e, true, nullptr, nullptr, hk).ok && calls == before);
  // 8. a failing conversion is reported
  hk.convert = [](const fs::path&, const CatalogueEntry&, std::string* err) { *err = "bad tensor"; return false; };
  r = install_model(d, e, true, nullptr, nullptr, hk);
  CHECK(!r.ok && r.error.find("bad tensor") != std::string::npos);
  // 9. removal, including path-traversal ids
  CHECK(!remove_model(d, "../x") && !remove_model(d, ""));
  CHECK(remove_model(d, "test-model") && installed_models(d).empty() && !fs::exists(models_dir(d) / "test-model"));
  fs::remove_all(d);
}


TEST(demucs_install_flow_offline) {
  // Real checkpoint + committed skeleton, with the download replaced by a local copy: download -> verify -> convert ->
  // manifest -> list. Skipped when the checkpoint has not been downloaded.
  fs::path ck = fs::path(DH_STEMS_MODELS_DIR) / "demucs" / "955717e8-8726e21a.th";
  fs::path res = fs::path(DH_STEMS_TEST_DIR) / ".." / ".." / "resources" / "stem-models";
  if (!fs::exists(ck)) { std::printf("  (checkpoint not downloaded: skipped)\n"); return; }
  auto cat = list_catalogue(res);
  const CatalogueEntry* e = nullptr;
  for (auto& c : cat) if (c.id == "demucs-htdemucs") e = &c;
  CHECK(e && e->install_supported);
  fs::path d = tmpdir("dinstall");
  InstallHooks hk;
  hk.download = [&](const std::string&, const fs::path& dest, std::uint64_t, const std::function<void(std::uint64_t)>& cb, const std::atomic<bool>*, std::string*) {
    fs::copy_file(ck, dest, fs::copy_options::overwrite_existing);
    if (cb) cb(fs::file_size(dest));
    return true;
  };
  hk.convert = demucs_converter(d, res / "skeletons");
  auto r = install_model(d, *e, true, nullptr, nullptr, hk);
  if (!r.ok) std::fprintf(stderr, "install: %s\n", r.error.c_str());
  CHECK(r.ok);
  auto models = list_models(models_dir(d));
  CHECK(models.size() == 1 && models[0].id == "demucs-htdemucs" && models[0].pipeline == "htdemucs-core" && models[0].segment_frames == 343980);
  std::string err;
  CHECK(verify_model_file(models[0], &err));
  CHECK(fs::file_size(models_dir(d) / "demucs-htdemucs" / "htdemucs.weights") == 167939968);
  CHECK(!fs::exists(models_dir(d) / "demucs-htdemucs" / "955717e8-8726e21a.th"));  // download deleted after conversion
  auto inst = installed_models(d);
  CHECK(inst.size() == 1 && inst[0].usable);
  CHECK(remove_model(d, "demucs-htdemucs") && list_models(models_dir(d)).empty());
  fs::remove_all(d);
}

// ---- stub / availability --------------------------------------------------------------------------------------
TEST(availability_reporting) {
  std::printf("backend: %s\n", backend_name());
  CHECK(stems_available() == (std::string(backend_name()) == "builtin"));
}

// ---- real models (skipped when none installed) -------------------------------------------------------------------
static void real_model_case(const ModelManifest& mm) {
  std::string err;
  auto model = open_model(mm, 0, &err);
  if (!model) { std::fprintf(stderr, "  real model %s not usable: %s\n", mm.id.c_str(), err.c_str()); CHECK(false); }
  const int rate = 44100;
  const std::size_t n = std::size_t(rate) * 20;
  Audio in;
  in.sample_rate = rate;
  in.l.assign(n, 0.f); in.r.assign(n, 0.f);
  std::mt19937 g(5);
  std::uniform_real_distribution<float> u(-1, 1);
  double bass_e = 0;
  for (std::size_t i = 0; i < n; ++i) {
    double t = double(i) / rate;
    float bass = 0.35f * float(std::sin(2 * M_PI * 55 * t) + 0.5 * std::sin(2 * M_PI * 110 * t)) * 0.7f;
    double beat = std::fmod(t, 0.5);
    float drum = beat < 0.06 ? u(g) * 0.5f * float(std::exp(-beat * 60)) : 0.f;
    double f0 = 220 + 20 * std::sin(2 * M_PI * 0.5 * t);
    float voc = 0;
    for (int h = 1; h <= 12; ++h) voc += float(std::sin(2 * M_PI * f0 * h * t) / h * std::exp(-std::pow((f0 * h - 700) / 500, 2)) * 0.25);
    in.l[i] = bass + drum + voc;
    in.r[i] = bass + 0.8f * drum + 0.9f * voc;
    bass_e += double(bass) * bass;
  }
  SeparateOptions o;
  o.overlap = mm.overlap;
  auto r = separate(in, *model, o);
  CHECK(r.status == SeparateResult::Status::Ok);
  const double speed = double(n) / rate / r.seconds;
  std::printf("  %s: %.1f s of audio in %.1f s (%.2fx real time)\n", mm.id.c_str(), double(n) / rate, r.seconds, speed);
  CHECK(sum_error(in, r.stems) < 2e-6);
  double e[4] = {0, 0, 0, 0};
  for (int s = 0; s < 4; ++s) for (std::size_t i = 0; i < n; ++i) e[s] += double(r.stems[std::size_t(s)].l[i]) * r.stems[std::size_t(s)].l[i];
  std::printf("  energy share  vocals %.2f drums %.2f bass %.2f other %.2f\n", e[0] / bass_e, e[1] / bass_e, e[2] / bass_e, e[3] / bass_e);
  CHECK(e[2] > 0.5 * (e[0] + e[1] + e[2] + e[3]) * 0.3);  // most of the bass-band energy lands in the bass stem
  // no discontinuities at the chunk borders: compare the step size at the hop positions with the typical step
  const std::size_t S = model->info().segment_frames, hop = std::size_t(S * (1 - mm.overlap));
  double typ = 0, border = 0; std::size_t cnt = 0;
  for (std::size_t i = 1; i < n; ++i) typ += std::fabs(double(r.stems[2].l[i]) - r.stems[2].l[i - 1]);
  typ /= double(n);
  for (std::size_t b = hop * 44100 / rate; b + 1 < n; b += hop) { border = std::max(border, std::fabs(double(r.stems[2].l[b]) - r.stems[2].l[b - 1])); ++cnt; }
  CHECK(cnt > 0 && border < 20 * typ + 1e-3);
}

TEST(real_models_if_installed) {
  auto models = list_models(DH_STEMS_MODELS_DIR);
  if (models.empty() || !stems_available()) { std::printf("  (no installed model or no runtime: real-model test skipped)\n"); return; }
  for (auto& m : models) real_model_case(m);
}

int main(int argc, char** argv) {
  int failed_filter = argc > 1;
  for (auto& c : run_list()) {
    if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
    std::printf("[ RUN ] %s\n", c.name);
    std::fflush(stdout);
    c.fn();
    ++g_tests;
  }
  std::printf("test_stems: %d tests passed (backend %s)%s\n", g_tests, backend_name(), failed_filter ? " (filtered)" : "");
  return 0;
}
