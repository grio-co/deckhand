// Analysis data in library sync: phrases, vocal regions and analysis cue points travel by content fingerprint,
// only when the switch is on, never replace a newer algorithm version, and stay small.
#include <cmath>

#include "sync_dev.hpp"

using namespace syncdev;

namespace {

// A four minute track at 128 BPM: 128 bars, twelve sections, per-bar energy.
StoredPhrases phrases(int seed, int version) {
  StoredPhrases p;
  p.version = version;
  p.gridded = true;
  p.origin_ms = 120.37 + seed;
  p.pickup_ms = 0;
  p.bar_ms = 1875.0 + seed * 0.013;  // 128 bpm
  p.total_bars = 128;
  unsigned r = static_cast<unsigned>(seed) * 2654435761u + 12345u;
  const auto rnd = [&] {
    r = r * 1664525u + 1013904223u;
    return static_cast<double>((r >> 8) & 0xFFFF) / 65536.0;
  };
  for (int i = 0; i < p.total_bars; ++i) {
    p.bar_starts_ms.push_back(p.origin_ms + i * p.bar_ms + (i % 29 == 0 ? rnd() * 0.4 : 0));
    p.bar_energy.push_back(static_cast<float>(std::round(rnd() * 1000.0) / 1000.0));
  }
  const char* labels[] = {"intro", "build", "drop", "break", "drop", "outro"};
  for (int i = 0; i < 12; ++i) {
    StoredPhraseSection s;
    s.start_bar = i * 10;
    s.end_bar = i * 10 + 10;
    s.bars = 10;
    s.start_ms = p.bar_starts_ms[static_cast<std::size_t>(s.start_bar)];
    s.end_ms = p.bar_starts_ms[static_cast<std::size_t>(std::min(127, s.end_bar))];
    s.energy = 1 + (i * 7) % 10;
    s.label = labels[i % 6];
    s.kind = i % 2 ? "phrase" : "section";
    s.label_confidence = 0.8123;
    s.boundary_confidence = 0.6;
    s.kick = 0.25;
    s.phrase_aligned = i % 3 != 0;
    p.sections.push_back(s);
  }
  return p;
}
StoredVocals vocals(int version) {
  StoredVocals v;
  v.version = version;
  v.source = "stems:abc|123";
  for (int i = 0; i < 8; ++i) v.regions.push_back({10000.0 + i * 20000, 18000.5 + i * 20000});
  return v;
}

bool near_equal(const StoredPhrases& a, const StoredPhrases& b) {
  if (a.version != b.version || a.gridded != b.gridded || a.total_bars != b.total_bars || a.sections.size() != b.sections.size() ||
      a.bar_starts_ms.size() != b.bar_starts_ms.size() || a.bar_energy.size() != b.bar_energy.size())
    return false;
  if (std::fabs(a.origin_ms - b.origin_ms) > 0.01 || std::fabs(a.bar_ms - b.bar_ms) > 0.01) return false;
  for (std::size_t i = 0; i < a.bar_starts_ms.size(); ++i)
    if (std::fabs(a.bar_starts_ms[i] - b.bar_starts_ms[i]) > 0.01) return false;
  for (std::size_t i = 0; i < a.bar_energy.size(); ++i)
    if (std::fabs(a.bar_energy[i] - b.bar_energy[i]) > 0.0006) return false;
  for (std::size_t i = 0; i < a.sections.size(); ++i) {
    const auto &x = a.sections[i], &y = b.sections[i];
    if (x.label != y.label || x.kind != y.kind || x.energy != y.energy || x.bars != y.bars || x.phrase_aligned != y.phrase_aligned ||
        std::fabs(x.start_ms - y.start_ms) > 0.01 || std::fabs(x.label_confidence - y.label_confidence) > 0.0001)
      return false;
  }
  return true;
}

Dev& device(Scenario& s, const char* name, bool analysis) {
  Dev& d = s.add(name, false);
  d.cfg.trust_path_hints = false;
  d.cfg.sync_analysis = analysis;
  d.start();
  return d;
}

std::uint64_t folder_bytes(const std::string& folder) {
  std::uint64_t n = 0;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(folder, ec))
    if (e.is_regular_file(ec)) n += static_cast<std::uint64_t>(e.file_size(ec));
  return n;
}

}  // namespace

int main() {
  const std::string fp1 = fp_of(1), fp2 = fp_of(2);
  {
    Scenario s("analysis");
    Dev& a = device(s, "A", true);
    Dev& b = device(s, "B", true);
    Dev& c = device(s, "C", false);
    const auto t1 = add_track(a, 1, "One"), t2 = add_track(a, 2, "Two");
    a.lib->set_phrases(t1, phrases(1, 1));
    a.lib->set_vocals(t2, vocals(1));
    a.lib->set_analysis_cues(t1, 1234.5, 800.25);
    a.lib->set_last_sound_ms(t1, 230000.5);
    s.sync_all(3);

    // Applied on the other device, to its own row for the same audio.
    const auto b1 = row_of(b, fp1), b2 = row_of(b, fp2);
    CHECK(b1 && b2);
    const auto pb = b.lib->phrases(b1);
    CHECK(pb && near_equal(*pb, phrases(1, 1)));
    const auto vb = b.lib->vocals(b2);
    CHECK(vb && vb->version == 1 && vb->regions.size() == 8 && vb->source == "stems:abc|123");
    CHECK(vb->regions[3].start_ms == 70000 && vb->regions[3].end_ms == 78000.5);
    const auto cb = b.lib->track_cues(b1);
    CHECK(cb.load_cue_ms && *cb.load_cue_ms == 1234.5 && cb.first_sound_ms && *cb.first_sound_ms == 800.25);
    CHECK(cb.last_sound_ms && *cb.last_sound_ms == 230000.5);
    // The switch is off on C: the data is kept in the merged state but not written to its library.
    const auto c1 = row_of(c, fp1);
    CHECK(c1 && !c.lib->phrases(c1) && !c.lib->vocals(row_of(c, fp2)));
    // Switched on later, the next sync writes it.
    c.cfg.sync_analysis = true;
    c.restart();
    const auto rc = c.sync();
    CHECK(rc.analysis_applied == 3);
    CHECK(c.lib->phrases(c1) && near_equal(*c.lib->phrases(c1), phrases(1, 1)));
    // Nothing is rewritten when repeated.
    CHECK(c.sync().analysis_applied == 0 && a.sync().analysis_applied == 0);

    // A newer algorithm version replaces the old one everywhere.
    adv(1000);
    b.lib->set_phrases(b1, phrases(1, 2));
    s.sync_all(3);
    for (Dev* d : {&a, &b, &c}) CHECK(d->lib->phrases(row_of(*d, fp1))->version == 2);

    // An older version written later (clock far ahead) never wins: the newer data is sent again and overrides it.
    const std::string fp3 = fp_of(3);
    const auto t3 = add_track(a, 3, "Three");
    a.lib->set_phrases(t3, phrases(3, 2));
    a.sync();  // published, not yet seen by B
    const auto t3b = add_track(b, 3, "Three");
    b.skew = 3600 * 1000;
    b.lib->set_phrases(t3b, phrases(3, 1));
    b.sync();  // B's version 1 carries a later stamp than A's version 2
    s.sync_all(4);
    for (Dev* d : {&a, &b, &c}) {
      const auto p = d->lib->phrases(row_of(*d, fp3));
      CHECK(p && p->version == 2 && near_equal(*p, phrases(3, 2)));
    }
    b.skew = 0;
  }
  {  // Size: bytes per track in the folder, with and without analysis data.
    std::uint64_t with = 0, without = 0;
    const int kTracks = 20;
    for (const bool on : {false, true}) {
      Scenario s("size");
      Dev& a = device(s, "A", on);
      for (int i = 1; i <= kTracks; ++i) {
        const auto id = add_track(a, 100 + i, "T" + std::to_string(i), 6000);
        a.lib->set_phrases(id, phrases(i, 1));
        a.lib->set_vocals(id, vocals(1));
        a.lib->set_analysis_cues(id, 500.0 + i, 400.0);
        a.lib->set_last_sound_ms(id, 200000.0 + i);
      }
      a.sync();
      (on ? with : without) = folder_bytes(s.folder);
    }
    const std::uint64_t per = (with - without) / kTracks;
    std::printf("analysis data in the folder: %llu bytes per track (library data without it: %llu bytes per track)\n",
                static_cast<unsigned long long>(per), static_cast<unsigned long long>(without / kTracks));
    CHECK(per > 200 && per < 4000);
  }
  std::puts("test_sync_analysis: all checks passed");
  return 0;
}
