// FLAC encoder: bit-exact round trips through an independent test decoder, STREAMINFO / SEEKTABLE / tags, streaming, export
// integration and decode through the project's own decoder. Uses `ffmpeg` as an extra check when it happens to be on PATH.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "capture_synth.hpp"
#include "check.hpp"
#include "deckhand/audio/track.hpp"
#include "deckhand/capture/export.hpp"
#include "deckhand/capture/flac.hpp"
#include "flac_ref_decoder.hpp"

using namespace deckhand::capture;
namespace fs = std::filesystem;

namespace {

using Pcm = std::vector<std::int32_t>;

std::vector<std::uint8_t> slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

enum class Kind { Silence, Dc, Sine, Noise, Music, Wasted, Square, Extremes, Impulse };

Pcm make(Kind k, std::uint64_t frames, std::uint32_t ch, std::uint32_t bits, std::uint32_t rate, std::uint64_t seed = 3) {
  synth::Rng rng(seed);
  const std::int64_t hi = (1ll << (bits - 1)) - 1, lo = -hi - 1;
  Pcm x(static_cast<std::size_t>(frames * ch));
  for (std::uint64_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    for (std::uint32_t c = 0; c < ch; ++c) {
      double v = 0;
      switch (k) {
        case Kind::Silence: v = 0; break;
        case Kind::Dc: v = 0.25 * (c + 1) / ch; break;
        case Kind::Sine: v = std::sin(2 * synth::kPi * (440.0 + 3 * c) * t); break;
        case Kind::Noise: v = 2 * rng.uni() - 1; break;
        case Kind::Music: {
          const double env = 0.5 + 0.5 * std::sin(2 * synth::kPi * 0.7 * t);
          v = 0.25 * env * (std::sin(2 * synth::kPi * 220 * t) + 0.5 * std::sin(2 * synth::kPi * 440 * t + c) + 0.25 * std::sin(2 * synth::kPi * 660 * t) +
                            0.12 * std::sin(2 * synth::kPi * 1320 * t)) +
              0.1 * std::sin(2 * synth::kPi * (110 + 55 * std::floor(std::fmod(t, 4.0))) * t) + 0.002 * rng.gauss();
          if (c % 2) v = 0.8 * v + 0.1 * std::sin(2 * synth::kPi * 330 * t);
          break;
        }
        case Kind::Wasted: v = std::sin(2 * synth::kPi * 300 * t) * 0.6; break;
        case Kind::Square: v = std::fmod(t * 100, 1.0) < 0.5 ? 1.0 : -1.0; break;
        case Kind::Extremes: v = ((i + c) & 1) ? 1.0 : -1.0; break;
        case Kind::Impulse: v = (i % 1000 == 500) ? 1.0 : 0.0; break;
      }
      std::int64_t s = static_cast<std::int64_t>(std::llround(v * static_cast<double>(hi)));
      if (k == Kind::Extremes) s = ((i + c) & 1) ? hi : lo;
      if (k == Kind::Wasted) s = (s / 8) * 8;
      x[static_cast<std::size_t>(i * ch + c)] = static_cast<std::int32_t>(std::clamp(s, lo, hi));
    }
  }
  return x;
}

struct Encoded {
  std::vector<std::uint8_t> bytes;
};

Encoded encode(const std::string& path, const Pcm& x, std::uint32_t ch, std::uint32_t bits, std::uint32_t rate, int level = 5, std::uint32_t block = 4096,
               const TrackTags* tags = nullptr, std::size_t chunk = 0) {
  FlacWriter w;
  FlacParams p;
  p.sample_rate = rate;
  p.channels = ch;
  p.bits = bits;
  p.level = level;
  p.block_size = block;
  p.total_frames_hint = x.size() / ch;
  std::string e;
  CHECK(w.create(path, p, tags, e));
  const std::uint64_t frames = x.size() / ch;
  if (chunk == 0) chunk = static_cast<std::size_t>(frames ? frames : 1);
  for (std::uint64_t d = 0; d < frames; d += chunk) CHECK(w.write_int(x.data() + d * ch, std::min<std::uint64_t>(chunk, frames - d)));
  CHECK(w.close());
  CHECK(w.frames_written() == frames);
  Encoded r;
  r.bytes = slurp(path);
  CHECK(w.bytes_written() == r.bytes.size());
  return r;
}

flacref::Decoded roundtrip(const fs::path& dir, const Pcm& x, std::uint32_t ch, std::uint32_t bits, std::uint32_t rate, int level = 5, std::uint32_t block = 4096) {
  const std::string path = (dir / "rt.flac").string();
  const Encoded e = encode(path, x, ch, bits, rate, level, block);
  flacref::Decoded d;
  if (!flacref::decode(e.bytes, d)) { std::fprintf(stderr, "decode failed: %s (ch %u bits %u rate %u frames %zu block %u)\n", d.err.c_str(), ch, bits, rate, x.size() / ch, block); std::exit(1); }
  CHECK(d.info.rate == rate && d.info.channels == ch && d.info.bps == bits && d.info.total == x.size() / ch);
  if (d.pcm != x) {
    std::size_t i = 0;
    while (i < x.size() && d.pcm[i] == x[i]) ++i;
    std::fprintf(stderr, "sample mismatch at %zu (ch %u bits %u frames %zu)\n", i, ch, bits, x.size() / ch);
    std::exit(1);
  }
  return d;
}

bool have_ffmpeg() {
#ifdef _WIN32
  return false;
#else
  return std::system("ffmpeg -version >/dev/null 2>&1") == 0;
#endif
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "dh_test_flac";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  // ---- MD5 and CRC known answers (both implementations) --------------------------------------------------------------
  {
    const char* abc = "abc";
    std::uint8_t a[16], b[16];
    flacref::md5(reinterpret_cast<const std::uint8_t*>(abc), 3, a);
    Md5 m;
    m.update(abc, 3);
    const auto r = m.finish();
    CHECK(std::memcmp(a, r.data(), 16) == 0);
    CHECK(a[0] == 0x90 && a[1] == 0x01 && a[2] == 0x50 && a[3] == 0x98 && a[15] == 0x72);  // 900150983cd24fb0d6963f7d28e17f72
    flacref::md5(nullptr, 0, b);
    Md5 e;
    const auto re = e.finish();
    CHECK(std::memcmp(b, re.data(), 16) == 0 && b[0] == 0xd4 && b[15] == 0x7e);
    std::vector<std::uint8_t> big(100003);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 31 + (i >> 8));
    flacref::md5(big.data(), big.size(), a);
    Md5 g;
    for (std::size_t o = 0; o < big.size(); o += 777) g.update(big.data() + o, std::min<std::size_t>(777, big.size() - o));
    const auto rg = g.finish();
    CHECK(std::memcmp(a, rg.data(), 16) == 0);
  }

  // ---- signal x length x channels x depth x rate ----------------------------------------------------------------------
  {
    const Kind kinds[] = {Kind::Silence, Kind::Dc, Kind::Sine, Kind::Noise, Kind::Music, Kind::Wasted, Kind::Square, Kind::Extremes, Kind::Impulse};
    const std::uint64_t lengths[] = {0, 1, 3, 15, 16, 17, 100, 4095, 4096, 4097, 9000, 12288};
    const std::uint32_t rates[] = {44100, 48000, 96000};
    int n = 0;
    for (Kind k : kinds)
      for (std::uint32_t ch : {1u, 2u, 6u})
        for (std::uint32_t bits : {16u, 24u})
          for (std::uint64_t len : lengths) {
            const std::uint32_t rate = rates[(n++) % 3];
            if (ch == 6 && len > 9000) continue;
            roundtrip(dir, make(k, len, ch, bits, rate), ch, bits, rate);
          }
    // A few more shapes: other channel counts, depths, rates and block sizes, every level.
    for (std::uint32_t ch : {3u, 4u, 8u}) roundtrip(dir, make(Kind::Music, 5000, ch, 16, 44100), ch, 16, 44100);
    for (std::uint32_t bits : {8u, 12u, 20u}) roundtrip(dir, make(Kind::Music, 6000, 2, bits, 44100), 2, bits, 44100);
    for (std::uint32_t rate : {8000u, 22050u, 37800u, 88200u, 192000u, 1000000u}) roundtrip(dir, make(Kind::Sine, 3000, 2, 16, rate), 2, 16, rate);
    for (std::uint32_t block : {16u, 100u, 192u, 256u, 576u, 1000u, 1152u, 2048u, 4608u, 8192u, 65535u})
      for (int level : {0, 3, 8}) roundtrip(dir, make(Kind::Music, 20000, 2, 24, 44100), 2, 24, 44100, level, block);
  }

  // ---- what the encoder chose: stereo modes, subframe kinds, wasted bits ----------------------------------------------------
  {
    const auto d = roundtrip(dir, make(Kind::Music, 200000, 2, 24, 44100), 2, 24, 44100);
    CHECK(d.kinds[3] > 0);  // LPC used
    const auto z = roundtrip(dir, make(Kind::Silence, 20000, 2, 16, 44100), 2, 16, 44100);
    CHECK(z.kinds[0] > 0 && z.kinds[1] == 0);
    const auto w = roundtrip(dir, make(Kind::Wasted, 20000, 2, 16, 44100), 2, 16, 44100);
    CHECK(w.wasted_subframes > 0);
    const auto nz = roundtrip(dir, make(Kind::Noise, 20000, 2, 16, 44100), 2, 16, 44100);
    CHECK(nz.kinds[1] > 0 || nz.kinds[2] > 0);  // noise barely compresses: verbatim or low-order fixed
    // Correlated stereo picks a joint mode, an independent pair stays independent.
    Pcm cor(40000 * 2), ind(40000 * 2);
    synth::Rng r(9);
    for (std::size_t i = 0; i < 40000; ++i) {
      const double a = 12000 * std::sin(i * 0.01) + 500 * r.gauss();
      cor[i * 2] = static_cast<std::int32_t>(a);
      cor[i * 2 + 1] = static_cast<std::int32_t>(a + 3 * r.gauss());
      ind[i * 2] = static_cast<std::int32_t>(12000 * std::sin(i * 0.01) + 40 * r.gauss());
      ind[i * 2 + 1] = static_cast<std::int32_t>(9000 * std::sin(i * 0.037 + 1) + 40 * r.gauss());
    }
    const auto dc = roundtrip(dir, cor, 2, 16, 44100);
    CHECK(dc.modes[1] + dc.modes[2] + dc.modes[3] > 0);
    const auto di = roundtrip(dir, ind, 2, 16, 44100);
    CHECK(di.modes[0] > 0);
    // Left-only and right-only content exercises left/side and side/right.
    Pcm lo(40000 * 2), ro(40000 * 2);
    for (std::size_t i = 0; i < 40000; ++i) {
      const std::int32_t v = static_cast<std::int32_t>(10000 * std::sin(i * 0.02) + 200 * r.gauss());
      lo[i * 2] = v; lo[i * 2 + 1] = static_cast<std::int32_t>(r.gauss() * 3);
      ro[i * 2] = static_cast<std::int32_t>(r.gauss() * 3); ro[i * 2 + 1] = v;
    }
    roundtrip(dir, lo, 2, 16, 44100);
    roundtrip(dir, ro, 2, 16, 44100);
    // Full-scale opposite phase (side channel needs one extra bit).
    Pcm opp(5000 * 2);
    for (std::size_t i = 0; i < 5000; ++i) { opp[i * 2] = (i & 1) ? 32767 : -32768; opp[i * 2 + 1] = (i & 1) ? -32768 : 32767; }
    roundtrip(dir, opp, 2, 16, 44100);
    Pcm opp24(5000 * 2);
    for (std::size_t i = 0; i < 5000; ++i) { opp24[i * 2] = (i & 3) ? 8388607 : -8388608; opp24[i * 2 + 1] = (i & 3) ? -8388608 : 8388607; }
    roundtrip(dir, opp24, 2, 24, 48000);
  }

  // ---- STREAMINFO, SEEKTABLE, tags, padding ---------------------------------------------------------------------------
  {
    TrackTags t;
    t.artist = "Some Artist";
    t.title = "T\xC3\xADtulo \xE2\x80\x94 \xE6\x97\xA5\xE6\x9C\xAC";
    t.album = "An Album";
    t.track_number = 3;
    t.track_total = 9;
    t.year = "1999";
    const std::uint64_t frames = 44100ull * 35 + 123;
    const Pcm x = make(Kind::Music, frames, 2, 16, 44100);
    const std::string path = (dir / "meta.flac").string();
    const Encoded e = encode(path, x, 2, 16, 44100, 5, 4096, &t, 5000);
    flacref::Decoded d;
    CHECK(flacref::decode(e.bytes, d));
    CHECK(d.pcm == x);
    CHECK(d.info.min_bs == 4096 && d.info.max_bs == 4096 && d.info.total == frames);
    CHECK(d.info.min_fs > 0 && d.info.max_fs >= d.info.min_fs);
    auto has = [&](const std::string& s) { return std::find(d.comments.begin(), d.comments.end(), s) != d.comments.end(); };
    CHECK(has("ARTIST=Some Artist") && has("ALBUM=An Album") && has("TRACKNUMBER=3") && has("TRACKTOTAL=9") && has("DATE=1999"));
    CHECK(has(std::string("TITLE=") + t.title));
    CHECK(d.vendor == "DeckHand" && d.padding == 2048);
    // Seek points: about every 10 s, each at the start of a frame, at the recorded offset.
    CHECK(d.seek.size() == 4);  // 0, 10, 20, 30 s
    for (const auto& s : d.seek) {
      const auto it = std::find(d.frame_offsets.begin(), d.frame_offsets.end(), s.offset);
      CHECK(it != d.frame_offsets.end());
      const std::size_t fi = static_cast<std::size_t>(it - d.frame_offsets.begin());
      CHECK(d.frame_first_sample[fi] == s.sample && s.count == 4096);
    }
    CHECK(d.seek[0].sample == 0 && d.seek[0].offset == 0);
    // One chunking or another gives the same file.
    const Encoded e2 = encode((dir / "meta2.flac").string(), x, 2, 16, 44100, 5, 4096, &t, 0);
    CHECK(e.bytes == e2.bytes);
    const Encoded e3 = encode((dir / "meta3.flac").string(), x, 2, 16, 44100, 5, 4096, &t, 777);
    CHECK(e.bytes == e3.bytes);
    // A stream that is a single short block records that size as min and max.
    const auto one = roundtrip(dir, make(Kind::Music, 1000, 2, 16, 44100), 2, 16, 44100);
    CHECK(one.info.min_bs == 1000 && one.info.max_bs == 1000);
    // Empty stream.
    const auto empty = roundtrip(dir, Pcm{}, 2, 16, 44100);
    CHECK(empty.info.total == 0 && empty.pcm.empty());
    // Samples out of range are clamped and the result still decodes.
    FlacWriter w;
    FlacParams p;
    p.bits = 16;
    std::string er;
    CHECK(w.create((dir / "clamp.flac").string(), p, nullptr, er));
    const std::int32_t wild[4] = {70000, -70000, 5, -5};
    CHECK(w.write_int(wild, 2) && w.close());
    flacref::Decoded dc;
    CHECK(flacref::decode(slurp((dir / "clamp.flac").string()), dc) && dc.pcm[0] == 32767 && dc.pcm[1] == -32768 && dc.pcm[2] == 5);
    // Bad parameters are refused.
    FlacWriter bad;
    FlacParams bp;
    bp.channels = 9;
    CHECK(!bad.create((dir / "bad.flac").string(), bp, nullptr, er) && !er.empty());
    bp.channels = 2;
    bp.bits = 32;
    CHECK(!bad.create((dir / "bad.flac").string(), bp, nullptr, er));
  }

  // ---- compression and speed on a music-like signal ----------------------------------------------------------------------
  {
    const std::uint32_t rate = 44100;
    const std::uint64_t frames = static_cast<std::uint64_t>(rate) * 60;
    for (std::uint32_t bits : {16u, 24u}) {
      const Pcm x = make(Kind::Music, frames, 2, bits, rate);
      const std::string path = (dir / "speed.flac").string();
      const auto t0 = std::chrono::steady_clock::now();
      const Encoded e = encode(path, x, 2, bits, rate);
      const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      const double wav = static_cast<double>(frames) * 2 * (bits / 8);
      std::printf("music-like 60 s stereo %u-bit: %.1f%% of WAV (ratio %.2f), encode %.2f s = %.0fx real time\n", bits, 100.0 * e.bytes.size() / wav,
                  wav / e.bytes.size(), sec, 60.0 / std::max(sec, 1e-9));
      flacref::Decoded d;
      CHECK(flacref::decode(e.bytes, d) && d.pcm == x);
      CHECK(e.bytes.size() < wav * 0.8);
    }
  }

  // ---- export_range / export_pieces / export_edit ------------------------------------------------------------------------
  {
    const std::uint32_t rate = 44100;
    const std::uint64_t frames = rate * 20 + 77;
    std::vector<float> pcm(static_cast<std::size_t>(frames * 2));
    synth::Rng rng(5);
    for (std::uint64_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(i) / rate;
      pcm[i * 2] = static_cast<float>(0.3 * std::sin(2 * synth::kPi * 330 * t) + 0.05 * rng.gauss());
      pcm[i * 2 + 1] = static_cast<float>(0.2 * std::sin(2 * synth::kPi * 495 * t) + 0.05 * rng.gauss());
    }
    pcm[10] = 1.7f;   // over full scale: clamps like the WAV writer
    pcm[11] = -1.7f;
    MemorySource src(pcm.data(), frames, 2, rate);
    CHECK(flac_available());
    CHECK(std::string(container_extension(Container::Flac)) == "flac");

    for (SampleFormat fmt : {SampleFormat::Pcm16, SampleFormat::Pcm24, SampleFormat::Float32}) {
      ExportSettings s;
      s.container = Container::Flac;
      s.format = fmt;
      const std::string f = (dir / "one.flac").string(), w = (dir / "one.wav").string();
      s.overwrite = true;
      const ExportResult r = export_range(src, 0, frames, f, s);
      CHECK(r.ok && r.frames == frames);
      CHECK(!fs::exists(f + ".part"));
      ExportSettings sw = s;
      sw.container = Container::Wav;
      CHECK(export_range(src, 0, frames, w, sw).ok);
      flacref::Decoded d;
      CHECK(flacref::decode(slurp(f), d));
      const std::uint32_t bits = fmt == SampleFormat::Pcm16 ? 16 : 24;
      CHECK(d.info.bps == bits && d.pcm.size() == frames * 2);
      // Same conversion as the WAV writer, sample for sample.
      std::vector<unsigned char> raw(pcm.size() * (bits / 8));
      pack_samples(pcm.data(), pcm.size(), bits == 16 ? SampleFormat::Pcm16 : SampleFormat::Pcm24, raw.data());
      for (std::size_t i = 0; i < pcm.size(); ++i) {
        std::int32_t v = bits == 16 ? static_cast<std::int16_t>(raw[i * 2] | (raw[i * 2 + 1] << 8))
                                    : static_cast<std::int32_t>((raw[i * 3] | (raw[i * 3 + 1] << 8) | (raw[i * 3 + 2] << 16)) << 8) >> 8;
        CHECK(d.pcm[i] == v);
      }
    }
    ExportSettings ex;
    ex.container = Container::Flac;
    ex.format = SampleFormat::Pcm16;
    CHECK(!export_range(src, 0, frames, (dir / "one.flac").string(), ex).ok);  // exists, no overwrite

    // Pieces: contiguous, tagged, concatenate to the source samples.
    const std::vector<Piece> pieces = {{0, 100000, false, 1}, {100000, 100000, true, 1}, {100000, 400000, false, 1}, {400000, frames, false, 1}};
    std::vector<TrackTags> tags(3);
    tags[0].artist = "A"; tags[0].title = "One";
    tags[1].artist = "A"; tags[1].title = "Two";
    ExportSettings ps;
    ps.container = Container::Flac;
    ps.format = SampleFormat::Pcm24;
    const fs::path pdir = dir / "pieces";
    const auto res = export_pieces(src, pieces, tags, pdir.string(), ps);
    CHECK(res.size() == 3);
    Pcm joined;
    for (const auto& r : res) {
      CHECK(r.ok && fs::path(r.path).extension() == ".flac");
      flacref::Decoded d;
      CHECK(flacref::decode(slurp(r.path), d));
      CHECK(d.info.total == r.frames);
      joined.insert(joined.end(), d.pcm.begin(), d.pcm.end());
    }
    CHECK(joined.size() == frames * 2);
    for (std::size_t i = 0; i < joined.size(); ++i) CHECK(joined[i] == flac_quantise(pcm[i], 24));
    {
      flacref::Decoded d;
      CHECK(flacref::decode(slurp(res[0].path), d));
      auto has = [&](const std::string& s) { return std::find(d.comments.begin(), d.comments.end(), s) != d.comments.end(); };
      CHECK(has("TITLE=One") && has("TRACKNUMBER=1") && has("TRACKTOTAL=3"));
    }

    // Edited export refuses the original and trims.
    const std::string orig = (dir / "orig.flac").string();
    CHECK(export_range(src, 0, frames, orig, ps).ok);
    const auto before = slurp(orig);
    const ExportResult same = export_edit(src, orig, 0, 0, orig, ps);
    CHECK(!same.ok && slurp(orig) == before);
    const std::string cut = (dir / "cut.flac").string();
    const ExportResult cr = export_edit(src, orig, 1.0, 3.0, cut, ps);
    CHECK(cr.ok && cr.frames == rate * 2);
    flacref::Decoded dcut;
    CHECK(flacref::decode(slurp(cut), dcut) && dcut.pcm.size() == rate * 2 * 2);
    CHECK(dcut.pcm[0] == flac_quantise(pcm[rate * 2], 24));

    // More than 8 channels is an error, not a crash.
    std::vector<float> nine(9 * 100, 0.1f);
    MemorySource ns(nine.data(), 100, 9, rate);
    CHECK(!export_range(ns, 0, 100, (dir / "nine.flac").string(), ex).ok);
    // Six channels work.
    std::vector<float> six(6 * 5000);
    for (std::size_t i = 0; i < six.size(); ++i) six[i] = static_cast<float>(0.3 * std::sin(i * 0.01 * (1 + i % 6)));
    MemorySource ss(six.data(), 5000, 6, 48000);
    CHECK(export_range(ss, 0, 5000, (dir / "six.flac").string(), ps).ok);
    flacref::Decoded d6;
    CHECK(flacref::decode(slurp((dir / "six.flac").string()), d6) && d6.info.channels == 6 && d6.info.rate == 48000);
  }

  // ---- the project's own decoder reads the files back bit-exactly -------------------------------------------------------------
  {
    int checked = 0;
    for (std::uint32_t rate : {44100u, 48000u, 96000u})
      for (std::uint32_t bits : {16u, 24u}) {
        const Pcm x = make(Kind::Music, rate / 2 + 37, 2, bits, rate);
        const std::string path = (dir / "own.flac").string();
        encode(path, x, 2, bits, rate);
        std::string err;
        const auto t = deckhand::decode_file(path, err);
#ifdef __APPLE__
        if (!t) std::fprintf(stderr, "decode_file failed: %s\n", err.c_str());
        CHECK(t != nullptr);
#else
        if (!t) { std::printf("decode_file cannot read FLAC on this platform (%s); skipped\n", err.c_str()); continue; }
#endif
        CHECK(t->frames == x.size() / 2);
        CHECK(t->sample_rate == rate);
        const float* p = t->data();
        const double scale = static_cast<double>(1u << (bits - 1));
        for (std::size_t i = 0; i < x.size(); ++i) CHECK(static_cast<std::int32_t>(std::lrint(static_cast<double>(p[i]) * scale)) == x[i]);
        ++checked;
      }
    std::printf("project decoder: %d files bit-exact\n", checked);
  }

  // ---- ffmpeg, when it happens to be installed -------------------------------------------------------------------------------
  if (have_ffmpeg()) {
    int checked = 0;
    for (std::uint32_t ch : {1u, 2u, 6u})
      for (std::uint32_t bits : {16u, 24u}) {
        const Pcm x = make(Kind::Music, 30001, ch, bits, 48000);
        const std::string path = (dir / "ff.flac").string(), raw = (dir / "ff.raw").string();
        encode(path, x, ch, bits, 48000);
        const std::string cmd = "ffmpeg -v error -y -err_detect +crccheck+explode -i \"" + path + "\" -f s32le -acodec pcm_s32le \"" + raw + "\"";
        CHECK(std::system(cmd.c_str()) == 0);
        const auto b = slurp(raw);
        CHECK(b.size() == x.size() * 4);
        for (std::size_t i = 0; i < x.size(); ++i) {
          std::int32_t v;
          std::memcpy(&v, b.data() + i * 4, 4);
          CHECK((v >> (32 - bits)) == x[i]);
        }
        ++checked;
      }
    std::printf("ffmpeg: %d files bit-exact\n", checked);
  } else {
    std::printf("ffmpeg not found: skipped\n");
  }

  fs::remove_all(dir, ec);
  std::printf("flac ok\n");
  return 0;
}
