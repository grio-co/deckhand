// Vocal region detection: synthetic signals through the pure detector, through a real stem cache file, and the library
// storage (including selective removal).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/analysis/vocals.hpp"
#include "deckhand/library/library.hpp"
#include "deckhand/stems/cache.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

namespace {

constexpr int kRate = 8000;

struct Dir {
  fs::path p;
  explicit Dir(const char* name) {
    p = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
  }
  ~Dir() {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
};

struct Burst {
  double start_s, end_s, amp;
};

// A 440 Hz tone in the given bursts, a quiet noise floor everywhere (stem bleed).
std::vector<float> make_signal(double seconds, const std::vector<Burst>& bursts, double floor_amp = 0.0005) {
  const std::size_t n = static_cast<std::size_t>(seconds * kRate);
  std::vector<float> x(n);
  unsigned seed = 12345;
  for (std::size_t i = 0; i < n; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const double noise = (static_cast<double>(seed >> 8) / 8388608.0 - 1.0) * floor_amp;
    const double t = static_cast<double>(i) / kRate;
    double v = noise;
    for (const Burst& b : bursts)
      if (t >= b.start_s && t < b.end_s) v += b.amp * std::sin(2 * 3.14159265358979 * 440.0 * t);
    x[i] = static_cast<float>(v);
  }
  return x;
}

bool near_ms(double a, double b, double tol = 80) { return std::fabs(a - b) <= tol; }

void test_pure() {
  // A phrase with a breath inside, a separate line, and a stray syllable.
  const auto x = make_signal(30, {{5, 8, 0.3}, {8.25, 12, 0.3}, {15, 16.5, 0.2}, {20, 20.3, 0.3}});
  const auto r = detect_vocal_regions(x.data(), x.size(), 1, kRate);
  CHECK(r.size() == 2);
  CHECK(near_ms(r[0].start_ms, 5000) && near_ms(r[0].end_ms, 12000));  // the breath did not split it
  CHECK(near_ms(r[1].start_ms, 15000) && near_ms(r[1].end_ms, 16500));  // the stray 300 ms burst is gone

  // A real gap splits; a gap just under the merge distance does not.
  const auto y = make_signal(20, {{2, 4, 0.3}, {5.5, 7, 0.3}, {7.4, 9, 0.3}});
  const auto ry = detect_vocal_regions(y.data(), y.size(), 1, kRate);
  CHECK(ry.size() == 2 && near_ms(ry[0].end_ms, 4000) && near_ms(ry[1].start_ms, 5500) && near_ms(ry[1].end_ms, 9000));

  // Options: a longer minimum drops the short line; padding widens.
  VocalOptions o;
  o.min_length_ms = 2000;
  CHECK(detect_vocal_regions(x.data(), x.size(), 1, kRate, o).size() == 1);
  o = VocalOptions{};
  o.pad_ms = 200;
  const auto rp = detect_vocal_regions(x.data(), x.size(), 1, kRate, o);
  CHECK(rp.size() == 2 && near_ms(rp[0].start_ms, 4800) && near_ms(rp[0].end_ms, 12200));

  // Silence, near silence and noise only: no vocals. Quiet vocals relative to a loud stem still count (adaptive).
  CHECK(detect_vocal_regions(make_signal(10, {}, 0).data(), 10 * kRate, 1, kRate).empty());
  CHECK(detect_vocal_regions(make_signal(10, {{2, 5, 0.001}}, 0.0001).data(), 10 * kRate, 1, kRate).empty());
  const auto quiet = make_signal(10, {{2, 5, 0.02}}, 0.0001);
  const auto rq = detect_vocal_regions(quiet.data(), quiet.size(), 1, kRate);
  CHECK(rq.size() == 1 && near_ms(rq[0].start_ms, 2000) && near_ms(rq[0].end_ms, 5000));

  // Stereo interleaved gives the same answer; chunked streaming too.
  std::vector<float> st(x.size() * 2);
  for (std::size_t i = 0; i < x.size(); ++i) st[2 * i] = st[2 * i + 1] = x[i];
  const auto rs = detect_vocal_regions(st.data(), x.size(), 2, kRate);
  CHECK(rs.size() == 2 && near_ms(rs[0].start_ms, r[0].start_ms, 1) && near_ms(rs[1].end_ms, r[1].end_ms, 1));
  VocalDetector det(kRate);
  for (std::size_t at = 0; at < x.size(); at += 777) det.add(x.data() + at, std::min<std::size_t>(777, x.size() - at));
  const auto rc = det.finish();
  CHECK(rc.size() == 2 && near_ms(rc[0].start_ms, r[0].start_ms, 1));
  CHECK(detect_vocal_regions(nullptr, 0, 1, kRate).empty());
}

void test_cache() {
  Dir d("deckhand_test_vocal_cache");
  const std::string key(64, 'a'), missing(64, 'b');
  {
    stems::CacheConfig cfg;
    cfg.root = d.p;
    stems::StemCache cache(cfg);
    const auto v = make_signal(20, {{3, 9, 0.3}, {12, 14, 0.3}});
    const auto quiet = make_signal(20, {{0, 20, 0.2}});  // loud drums must not count as vocals
    stems::Stems s;
    for (int i = 0; i < stems::kStemCount; ++i) {
      s[static_cast<std::size_t>(i)].sample_rate = kRate;
      const auto& src = i == stems::Vocals ? v : quiet;
      s[static_cast<std::size_t>(i)].l = src;
      s[static_cast<std::size_t>(i)].r = src;
    }
    std::string err;
    CHECK(cache.store(key, s, &err));
  }
  std::vector<VocalRegion> r;
  std::string err;
  CHECK(detect_vocals_in_cache(d.p.string(), key, r, &err));
  CHECK(r.size() == 2 && near_ms(r[0].start_ms, 3000) && near_ms(r[0].end_ms, 9000) && near_ms(r[1].start_ms, 12000) && near_ms(r[1].end_ms, 14000));
  CHECK(!detect_vocals_in_cache(d.p.string(), missing, r, &err) && !err.empty() && r.empty());
  CHECK(!detect_vocals_in_cache(d.p.string(), "short", r, &err));
}

void test_library() {
  Dir d("deckhand_test_vocal_library");
  {
    Library lib((d.p / "library.db").string());
    NewTrack t;
    t.path = "/music/v.mp3";
    t.title = "v";
    const auto id = lib.upsert_track(t);
    t.path = "/music/w.mp3";
    t.title = "w";
    const auto id2 = lib.upsert_track(t);
    CHECK(!lib.vocals(id) && lib.tracks_with_vocals().empty());
    StoredVocals v;
    v.source = "key1";
    v.regions = {{9000, 12000}, {1000, 5000}, {4000, 6000}, {7, 7}, {-5, 100}};  // unsorted, overlapping, invalid ones
    lib.set_vocals(id, v);
    lib.set_vocals(id2, StoredVocals{});  // analysed, none found
    const auto got = lib.vocals(id);
    CHECK(got && got->version == kVocalAnalysisVersion && got->source == "key1" && got->regions.size() == 2);
    CHECK(got->regions[0].start_ms == 1000 && got->regions[0].end_ms == 6000 && got->regions[1].start_ms == 9000);
    CHECK(lib.vocals(id2) && lib.vocals(id2)->regions.empty() && lib.tracks_with_vocals().size() == 2);

    // Selective removal: a dry run counts, the real run clears vocals only (other analysis stays).
    lib.set_gain_db(id, -3.0);
    StripKinds k;
    k.vocals = true;
    const StripResult dry = lib.strip_analysis({id, id2}, k, false, true);
    CHECK(dry.dry_run && dry.cleared.at("vocals") == 2 && lib.vocals(id));
    const StripResult real = lib.strip_analysis({id}, k);
    CHECK(real.cleared.at("vocals") == 1 && !lib.vocals(id) && lib.vocals(id2) && lib.gain_db(id));
    CHECK(lib.strip_analysis({id}, k).cleared.count("vocals") == 0);  // nothing left to clear
  }
}

}  // namespace

int main() {
  test_pure();
  test_cache();
  test_library();
  std::puts("test_vocal_regions ok");
  return 0;
}
