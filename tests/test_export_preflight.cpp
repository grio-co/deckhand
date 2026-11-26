// Export compatibility check: header probe, preflight report and the 24-bit WAV fix (docs/EXPORT.md).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "deckhand/export/export_check.hpp"
#include "deckhand/export/export_preflight.hpp"

using namespace deckhand::usbexport;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

void le16(Bytes& b, unsigned v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
void le32(Bytes& b, std::uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); }
void be16(Bytes& b, unsigned v) { b.push_back((v >> 8) & 255); b.push_back(v & 255); }
void be32(Bytes& b, std::uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }
void str(Bytes& b, const char* s) { while (*s) b.push_back(static_cast<std::uint8_t>(*s++)); }
void append(Bytes& b, const Bytes& o) { b.insert(b.end(), o.begin(), o.end()); }

Bytes slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  CHECK(f.good());
  return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
void spit(const fs::path& p, const Bytes& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  CHECK(f.good());
}

// WAV: tag 1 = PCM, 3 = float; extensible wraps the tag in a sub-format GUID.
Bytes wav(int tag, int ch, int rate, int bits, const Bytes& data, bool extensible = false, bool rf64 = false) {
  Bytes fmt;
  le16(fmt, extensible ? 0xFFFE : static_cast<unsigned>(tag));
  le16(fmt, ch);
  le32(fmt, rate);
  le32(fmt, rate * ch * bits / 8);
  le16(fmt, ch * bits / 8);
  le16(fmt, bits);
  if (extensible) {
    le16(fmt, 22);
    le16(fmt, bits);
    le32(fmt, 3);
    le16(fmt, tag);
    const std::uint8_t tail[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    fmt.insert(fmt.end(), tail, tail + 14);
  }
  Bytes b;
  str(b, rf64 ? "RF64" : "RIFF");
  le32(b, rf64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(4 + 8 + fmt.size() + 8 + data.size()));
  str(b, "WAVE");
  if (rf64) {
    str(b, "ds64");
    le32(b, 28);
    le32(b, 0), le32(b, 0);                                // riff size
    le32(b, static_cast<std::uint32_t>(data.size())), le32(b, 0);  // data size
    le32(b, 0), le32(b, 0);                                // sample count
    le32(b, 0);                                            // table length
  }
  str(b, "fmt ");
  le32(b, static_cast<std::uint32_t>(fmt.size()));
  append(b, fmt);
  str(b, "data");
  le32(b, rf64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(data.size()));
  append(b, data);
  return b;
}

void ext80(Bytes& b, int rate) {
  int e = 0;
  while ((std::uint64_t(1) << (e + 1)) <= static_cast<std::uint64_t>(rate)) ++e;
  be16(b, 16383 + e);
  const std::uint64_t mant = static_cast<std::uint64_t>(rate) << (63 - e);
  be32(b, static_cast<std::uint32_t>(mant >> 32));
  be32(b, static_cast<std::uint32_t>(mant & 0xFFFFFFFFu));
}

// AIFF (comp == nullptr) or AIFC with a compression type.
Bytes aiff(const char* comp, int ch, int rate, int bits, const Bytes& data) {
  Bytes comm;
  be16(comm, ch);
  be32(comm, static_cast<std::uint32_t>(data.size() / (ch * ((bits + 7) / 8))));
  be16(comm, bits);
  ext80(comm, rate);
  if (comp) {
    str(comm, comp);
    comm.push_back(0);  // empty pstring
    comm.push_back(0);
  }
  Bytes b;
  str(b, "FORM");
  be32(b, static_cast<std::uint32_t>(4 + 8 + comm.size() + 8 + 8 + data.size()));
  str(b, comp ? "AIFC" : "AIFF");
  str(b, "COMM");
  be32(b, static_cast<std::uint32_t>(comm.size()));
  append(b, comm);
  str(b, "SSND");
  be32(b, static_cast<std::uint32_t>(8 + data.size()));
  be32(b, 0), be32(b, 0);
  append(b, data);
  return b;
}

Bytes flac(int rate, int ch, int bits) {
  Bytes b;
  str(b, "fLaC");
  b.push_back(0x80);  // last block, STREAMINFO
  b.push_back(0), b.push_back(0), b.push_back(34);
  be16(b, 4096), be16(b, 4096);
  for (int i = 0; i < 6; ++i) b.push_back(0);  // min/max frame size
  b.push_back(static_cast<std::uint8_t>(rate >> 12));
  b.push_back(static_cast<std::uint8_t>(rate >> 4));
  b.push_back(static_cast<std::uint8_t>(((rate & 15) << 4) | ((ch - 1) << 1) | ((bits - 1) >> 4)));
  b.push_back(static_cast<std::uint8_t>(((bits - 1) & 15) << 4));
  for (int i = 0; i < 4; ++i) b.push_back(0);
  for (int i = 0; i < 16; ++i) b.push_back(0);  // md5
  b.resize(b.size() + 100, 0);
  return b;
}

// MPEG-1 layer 3, 128 kbps, 44.1 kHz, stereo: frames are 417 bytes. `xing` makes the first frame a Xing VBR header.
Bytes mp3(int frames, bool id3, bool xing = false, std::uint8_t b2 = 0x90) {
  Bytes b;
  if (id3) {
    str(b, "ID3");
    b.push_back(3), b.push_back(0), b.push_back(0);
    b.push_back(0), b.push_back(0), b.push_back(0), b.push_back(20);  // 20 bytes of tag
    b.resize(b.size() + 20, 0);
  }
  for (int i = 0; i < frames; ++i) {
    Bytes f(417, 0);
    f[0] = 0xFF, f[1] = 0xFB, f[2] = b2, f[3] = 0x00;  // 0x90 = 128 kbps / 44.1 kHz; 0x94 = 128 kbps / 48 kHz (frame 384*..)
    if (i == 0 && xing) {
      std::memcpy(&f[4 + 32], "Xing", 4);
      f[4 + 32 + 7] = 3;  // frames + bytes present
      f[4 + 32 + 11] = 100;                       // 100 frames
      const std::uint32_t bytes = 100 * 417 / 2;  // average = 64 kbps
      f[4 + 32 + 12] = static_cast<std::uint8_t>(bytes >> 24);
      f[4 + 32 + 13] = static_cast<std::uint8_t>(bytes >> 16);
      f[4 + 32 + 14] = static_cast<std::uint8_t>(bytes >> 8);
      f[4 + 32 + 15] = static_cast<std::uint8_t>(bytes);
    }
    append(b, f);
  }
  return b;
}

Bytes atom(const char* type, const Bytes& payload) {
  Bytes b;
  be32(b, static_cast<std::uint32_t>(8 + payload.size()));
  str(b, type);
  append(b, payload);
  return b;
}

// M4A with one audio track whose sample entry is `entry_type`. For alac the inner cookie carries depth and rate.
Bytes m4a(const char* entry_type, int rate, int ch, bool sinf = false, const char* brand = "M4A ") {
  Bytes entry;
  for (int i = 0; i < 6; ++i) entry.push_back(0);
  be16(entry, 1);
  be16(entry, 0), be16(entry, 0), be32(entry, 0);
  be16(entry, ch), be16(entry, 16), be16(entry, 0), be16(entry, 0);
  be32(entry, (static_cast<std::uint32_t>(rate) & 0xFFFF) << 16);
  if (std::strcmp(entry_type, "alac") == 0) {
    Bytes k;
    be32(k, 0);
    be32(k, 4096);
    k.push_back(0), k.push_back(24), k.push_back(40), k.push_back(10), k.push_back(14), k.push_back(static_cast<std::uint8_t>(ch));
    be16(k, 255), be32(k, 0), be32(k, 0), be32(k, static_cast<std::uint32_t>(rate));
    append(entry, atom("alac", k));
  }
  if (sinf) append(entry, atom("sinf", Bytes(8, 0)));
  Bytes stsd;
  be32(stsd, 0), be32(stsd, 1);
  append(stsd, atom(entry_type, entry));
  Bytes hdlr;
  be32(hdlr, 0), be32(hdlr, 0);
  str(hdlr, "soun");
  hdlr.resize(hdlr.size() + 12, 0);
  const Bytes stbl = atom("stbl", atom("stsd", stsd));
  const Bytes minf = atom("minf", stbl);
  Bytes mdia_p = atom("hdlr", hdlr);
  append(mdia_p, minf);
  const Bytes moov = atom("moov", atom("trak", atom("mdia", mdia_p)));
  Bytes ftyp;
  str(ftyp, brand);
  be32(ftyp, 0);
  str(ftyp, "isom");
  Bytes b = atom("ftyp", ftyp);
  append(b, moov);
  append(b, atom("mdat", Bytes(64, 0)));
  return b;
}

Bytes ogg(const char* head, int ch, int rate) {
  Bytes b;
  str(b, "OggS");
  b.resize(28, 0);
  str(b, head);
  while (b.size() < 37) b.push_back(1);
  if (std::strcmp(head, "OpusHead") == 0) {
    b.push_back(static_cast<std::uint8_t>(ch));
    b.push_back(0), b.push_back(0);
    le32(b, rate);
  } else {
    b.resize(39, 0);
    b.push_back(static_cast<std::uint8_t>(ch));
    le32(b, rate);
  }
  b.resize(b.size() + 20, 0);
  return b;
}

Bytes f32le(const std::vector<float>& v) {
  Bytes b;
  for (float x : v) {
    std::uint32_t u;
    std::memcpy(&u, &x, 4);
    le32(b, u);
  }
  return b;
}

int s24_at(const Bytes& b, std::size_t o) {
  int v = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16);
  if (v & 0x800000) v -= 0x1000000;
  return v;
}

const TrackPreflight* find(const PreflightReport& r, int id) {
  for (const auto& t : r.tracks)
    if (t.track_id == id) return &t;
  return nullptr;
}
bool has_issue(const TrackPreflight& t, const std::string& id) {
  for (const auto& i : t.issues)
    if (i.id == id) return true;
  return false;
}

ExportTrack make_track(int id, const fs::path& p, const std::string& title) {
  ExportTrack t;
  t.id = id;
  t.source_path = p.string();
  t.title = title;
  t.artist = "Artist";
  t.album = "Album";
  t.bpm = 120;
  t.duration_ms = 1000;
  t.sample_rate = 44100;
  t.bitrate = 1411;
  for (int b = 0; b < 8; ++b) t.beats.push_back({std::int64_t(b) * 500, 120.0, b % 4 + 1});
  return t;
}

FormatInfo probe_bytes(const fs::path& dir, const Bytes& b) {
  const fs::path p = dir / "probe.bin";
  spit(p, b);
  return probe_format(p.string());
}

}  // namespace

int main() {
  const fs::path tmp = fs::temp_directory_path() / ("deckhand_preflight_test_" + std::to_string(std::random_device{}()));
  fs::remove_all(tmp);
  fs::create_directories(tmp / "src");
  const fs::path src = tmp / "src";

  // ---- probe -----------------------------------------------------------------------------------------------
  const std::vector<float> samples = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, std::nanf(""), 0.25f, -0.25f};  // 4 stereo frames
  const Bytes fdata = f32le(samples);
  {
    FormatInfo f = probe_bytes(src, wav(3, 2, 44100, 32, fdata));
    CHECK(f.recognized && f.container == Container::Wav && f.codec == Codec::Float && f.is_float);
    CHECK(f.bit_depth == 32 && f.channels == 2 && f.sample_rate == 44100 && f.data_bytes == fdata.size());
    CHECK(describe_format(f).find("32-bit float WAV") == 0);
    f = probe_bytes(src, wav(1, 2, 48000, 16, Bytes(40, 0)));
    CHECK(f.recognized && f.codec == Codec::Pcm && !f.is_float && f.bit_depth == 16 && f.sample_rate == 48000);
    f = probe_bytes(src, wav(3, 2, 96000, 32, fdata, true));  // WAVE_FORMAT_EXTENSIBLE float
    CHECK(f.recognized && f.codec == Codec::Float && f.bit_depth == 32 && f.sample_rate == 96000);
    f = probe_bytes(src, wav(1, 2, 44100, 24, Bytes(60, 0), true));
    CHECK(f.recognized && f.codec == Codec::Pcm && f.bit_depth == 24);
    f = probe_bytes(src, wav(3, 2, 44100, 32, fdata, false, true));  // RF64, data size from ds64
    CHECK(f.recognized && f.container == Container::Rf64 && f.codec == Codec::Float && f.data_bytes == fdata.size());
    f = probe_bytes(src, wav(0x11, 2, 44100, 4, Bytes(40, 0)));  // IMA ADPCM
    CHECK(f.recognized && f.codec == Codec::Other);
    f = probe_bytes(src, aiff(nullptr, 2, 44100, 16, Bytes(32, 0)));
    CHECK(f.recognized && f.container == Container::Aiff && f.codec == Codec::Pcm && f.big_endian && f.sample_rate == 44100 &&
          f.bit_depth == 16 && f.channels == 2);
    f = probe_bytes(src, aiff("fl32", 2, 48000, 32, fdata));
    CHECK(f.recognized && f.container == Container::Aifc && f.codec == Codec::Float && f.bit_depth == 32 && f.sample_rate == 48000);
    f = probe_bytes(src, aiff("fl64", 2, 44100, 64, Bytes(64, 0)));
    CHECK(f.recognized && f.codec == Codec::Float && f.bit_depth == 64);
    f = probe_bytes(src, aiff("sowt", 2, 44100, 16, Bytes(32, 0)));
    CHECK(f.recognized && f.codec == Codec::Pcm && !f.big_endian);
    f = probe_bytes(src, flac(96000, 2, 24));
    CHECK(f.recognized && f.container == Container::Flac && f.sample_rate == 96000 && f.channels == 2 && f.bit_depth == 24);
    f = probe_bytes(src, flac(44100, 1, 16));
    CHECK(f.recognized && f.sample_rate == 44100 && f.channels == 1 && f.bit_depth == 16);
    f = probe_bytes(src, mp3(3, false));
    CHECK(f.recognized && f.codec == Codec::Mp3 && f.mpeg_version == 1 && f.layer == 3 && f.sample_rate == 44100 &&
          f.bitrate_kbps == 128 && !f.vbr && f.channels == 2);
    f = probe_bytes(src, mp3(3, true, true));  // ID3v2 skipped, Xing average bitrate
    CHECK(f.recognized && f.codec == Codec::Mp3 && f.vbr && f.bitrate_kbps > 60 && f.bitrate_kbps < 68);
    f = probe_bytes(src, m4a("mp4a", 44100, 2));
    CHECK(f.recognized && f.container == Container::Mp4 && f.codec == Codec::Aac && f.sample_rate == 44100 && !f.drm);
    f = probe_bytes(src, m4a("alac", 96000, 2));
    CHECK(f.recognized && f.codec == Codec::Alac && f.sample_rate == 96000 && f.bit_depth == 24);
    f = probe_bytes(src, m4a("mp4a", 44100, 2, true));
    CHECK(f.recognized && f.drm);
    f = probe_bytes(src, m4a("mp4a", 44100, 2, false, "M4P "));
    CHECK(f.recognized && f.drm);
    f = probe_bytes(src, ogg("OpusHead", 2, 48000));
    CHECK(f.recognized && f.container == Container::Ogg && f.codec == Codec::Opus && f.channels == 2);
    f = probe_bytes(src, ogg("\x01vorbis", 2, 44100));
    CHECK(f.recognized && f.codec == Codec::Vorbis && f.sample_rate == 44100);
    // Hostile input: empty, tiny, garbage, truncated headers, huge declared sizes, random bytes.
    f = probe_bytes(src, Bytes());
    CHECK(!f.recognized);
    f = probe_bytes(src, Bytes{1, 2, 3});
    CHECK(!f.recognized);
    Bytes garbage(5000);
    std::mt19937 rng(7);
    for (auto& c : garbage) c = static_cast<std::uint8_t>(rng());
    f = probe_bytes(src, garbage);
    CHECK(!f.recognized && f.container == Container::Unknown);
    const Bytes w = wav(3, 2, 44100, 32, fdata);
    for (std::size_t n : {12u, 16u, 20u, 30u, 36u, 40u}) {
      f = probe_bytes(src, Bytes(w.begin(), w.begin() + static_cast<std::ptrdiff_t>(n)));
      CHECK(!f.recognized || f.truncated || f.data_bytes == 0);
    }
    f = probe_bytes(src, Bytes(w.begin(), w.end() - 8));  // cut in the sample data: still readable, data clamped
    CHECK(f.recognized && f.data_bytes == fdata.size() - 8);
    Bytes hostile = w;
    hostile[16] = hostile[17] = hostile[18] = hostile[19] = 0xFF;  // fmt chunk claims 4 GB
    f = probe_bytes(src, hostile);
    CHECK(!f.recognized || f.truncated || f.sample_rate > 0);
    const Bytes aif = aiff("fl32", 2, 48000, 32, fdata);
    for (std::size_t n : {12u, 16u, 24u, 30u, 40u}) probe_bytes(src, Bytes(aif.begin(), aif.begin() + static_cast<std::ptrdiff_t>(n)));
    const Bytes mp4 = m4a("alac", 96000, 2);
    for (std::size_t n = 0; n < mp4.size(); n += 7) probe_bytes(src, Bytes(mp4.begin(), mp4.begin() + static_cast<std::ptrdiff_t>(n)));
    const Bytes fl = flac(44100, 2, 16);
    for (std::size_t n = 0; n < 50; ++n) probe_bytes(src, Bytes(fl.begin(), fl.begin() + static_cast<std::ptrdiff_t>(n)));
    Bytes mp4bad = mp4;  // atom sizes that point far outside the file
    mp4bad[0] = mp4bad[1] = 0xFF;
    probe_bytes(src, mp4bad);
    CHECK(!probe_format((tmp / "nonexistent.wav").string()).recognized);
  }

  // ---- preflight report ------------------------------------------------------------------------------------
  std::vector<ExportTrack> tracks;
  auto add = [&](int id, const char* name, const Bytes& b) {
    const fs::path p = src / name;
    spit(p, b);
    tracks.push_back(make_track(id, p, name));
  };
  add(1, "float32.wav", wav(3, 2, 44100, 32, fdata));
  add(2, "pcm16.wav", wav(1, 2, 44100, 16, Bytes(40, 0)));
  add(3, "song.flac", flac(44100, 2, 24));
  add(4, "voice.opus", ogg("OpusHead", 2, 48000));
  add(5, "int32.aif", aiff(nullptr, 2, 44100, 32, [] {
        Bytes d;
        for (std::uint32_t v : {0x40000000u, 0xC0000000u, 0x7FFFFFFFu, 0x80000000u, 0x00000080u, 0x0000007Fu, 0u, 0u}) be32(d, v);
        return d;
      }()));
  add(6, "float192.wav", wav(3, 2, 192000, 32, fdata));
  add(7, "song.mp3", mp3(3, false));
  add(8, "empty.wav", Bytes());
  add(9, "protected.m4p", m4a("mp4a", 44100, 2, true, "M4P "));
  add(10, "junk.mp3", [] { Bytes g(3000); std::mt19937 r(3); for (auto& c : g) c = static_cast<std::uint8_t>(r()); return g; }());
  add(11, "wrongname.mp3", wav(1, 2, 44100, 16, Bytes(40, 0)));
  add(12, "alac96.m4a", m4a("alac", 96000, 2));
  add(13, "aifc_float.aifc", aiff("fl32", 2, 48000, 32, fdata));
  add(14, "pcm16_192k.wav", wav(1, 2, 192000, 16, Bytes(40, 0)));
  tracks.push_back(make_track(15, src / "gone.wav", "gone"));
  {
    ExportTrack t = make_track(16, src / "pcm16.wav", "long");
    t.artist = std::string(300, 'a');
    t.album = std::string(300, 'b');
    tracks.push_back(t);
  }
  {
    spit(src / "we:ird?.wav", wav(1, 2, 44100, 16, Bytes(40, 0)));
    tracks.push_back(make_track(17, src / "we:ird?.wav", "odd"));
  }
  const PlayerProfile& club = default_player_profile();
  CHECK(club.id == "club" && find_player_profile("club_legacy") && !find_player_profile("nope"));
  const PreflightReport rep = preflight_export(tracks, club);
  CHECK(rep.tracks.size() == tracks.size());
  auto T = [&](int id) -> const TrackPreflight& {
    const TrackPreflight* t = find(rep, id);
    CHECK(t != nullptr);
    return *t;
  };
  CHECK(T(1).verdict == Verdict::WillNotPlay && has_issue(T(1), "pcm.float") && T(1).fixable);
  CHECK(T(1).issues[0].message.find("32-bit float WAV: club players cannot read this") == 0);
  CHECK(T(1).issues[0].source == RuleSource::Documented);
  CHECK(T(2).verdict == Verdict::Ok && T(2).issues.empty());
  CHECK(T(3).verdict == Verdict::Warning && has_issue(T(3), "flac.ok") && T(3).issues[0].message.find("older players cannot") != std::string::npos);
  CHECK(T(4).verdict == Verdict::WillNotPlay && has_issue(T(4), "opus.unsupported") && !T(4).fixable);
  CHECK(T(5).verdict == Verdict::WillNotPlay && has_issue(T(5), "pcm.int32") && T(5).fixable);
  CHECK(T(6).verdict == Verdict::WillNotPlay && !T(6).fixable && T(6).issues[0].message.find("resampling") != std::string::npos);
  CHECK(T(7).verdict == Verdict::Ok);
  CHECK(T(8).verdict == Verdict::WillNotPlay && has_issue(T(8), "source.empty"));
  CHECK(T(9).verdict == Verdict::WillNotPlay && has_issue(T(9), "drm.protected"));
  CHECK(T(10).verdict == Verdict::WillNotPlay && has_issue(T(10), "file.unrecognized"));
  CHECK(T(11).verdict == Verdict::Warning && has_issue(T(11), "ext.mismatch"));
  CHECK(T(12).verdict == Verdict::Warning && has_issue(T(12), "alac.ok"));
  CHECK(T(13).verdict == Verdict::WillNotPlay && has_issue(T(13), "pcm.float") && T(13).fixable);
  CHECK(T(14).verdict == Verdict::Warning && has_issue(T(14), "pcm.rate") && T(14).issues[0].source == RuleSource::Unverified);
  CHECK(T(15).verdict == Verdict::WillNotPlay && has_issue(T(15), "source.missing"));
  CHECK(T(16).verdict == Verdict::Ok);  // names are capped (60/60/255), so the 500-byte limit cannot be reached
  {
    PlayerProfile tight = club;  // a profile with a short path limit exercises the check
    tight.max_path_bytes = 100;
    const PreflightReport pr = preflight_export({tracks[15]}, tight);
    CHECK(pr.tracks[0].verdict == Verdict::WillNotPlay && has_issue(pr.tracks[0], "path.too_long"));
  }
  CHECK(T(17).verdict == Verdict::Warning && has_issue(T(17), "name.changed"));
  CHECK(rep.fixable == 3 && !rep.all_ok());
  CHECK(rep.ok + rep.warnings + rep.will_not_play == tracks.size());
  CHECK(rep.text().find("[pcm.float, documented]") != std::string::npos);
  const std::string json = rep.to_json();
  CHECK(json.find("\"will_not_play\":") != std::string::npos && json.find("\"id\":\"pcm.float\"") != std::string::npos &&
        json.front() == '{' && json.back() == '}');
  // The older-generation profile rejects FLAC/ALAC outright.
  {
    const PreflightReport old = preflight_export({tracks[2]}, *find_player_profile("club_legacy"));
    CHECK(old.tracks[0].verdict == Verdict::WillNotPlay);
  }
  // Higher than 24-bit / 96 kHz FLAC.
  {
    spit(src / "hi.flac", flac(192000, 2, 24));
    const PreflightReport hi = preflight_export({make_track(1, src / "hi.flac", "hi")}, club);
    CHECK(hi.tracks[0].verdict == Verdict::WillNotPlay && has_issue(hi.tracks[0], "flac.rate") && !hi.tracks[0].fixable);
  }

  // ---- conversion ------------------------------------------------------------------------------------------
  {
    ConversionPlan plan;
    std::string why;
    CHECK(plan_pcm24_conversion(T(1).format, club, plan, &why) && plan.frames == 4 && plan.out_bytes == 44 + 4 * 2 * 3);
    CHECK(!plan_pcm24_conversion(T(6).format, club, plan, &why) && !why.empty());
    CHECK(!plan_pcm24_conversion(T(3).format, club, plan, &why));
    const std::string dst = (tmp / "direct.wav").string();
    std::string err;
    CHECK(plan_pcm24_conversion(T(1).format, club, plan) && convert_to_pcm24_wav(tracks[0].source_path, T(1).format, plan, dst, err));
    const FormatInfo o = probe_format(dst);
    CHECK(o.recognized && o.codec == Codec::Pcm && o.bit_depth == 24 && o.sample_rate == 44100 && o.channels == 2);
  }

  // ---- export: default (no conversion) still proceeds and carries the report -------------------------------
  const Bytes src_float = slurp(tracks[0].source_path);
  const Bytes src_int = slurp(tracks[4].source_path);
  std::vector<ExportTrack> sel = {tracks[0], tracks[1], tracks[4], tracks[3]};
  {
    const fs::path stick = tmp / "stick_plain";
    ExportOptions o;
    o.destination = stick.string();
    ExportResult res;
    std::string err;
    CHECK(export_usb(sel, {}, o, err, res));
    CHECK(res.converted == 0 && res.report.will_not_play == 3 && res.remaining_will_not_play() == 3);
    CHECK(slurp(stick / "Contents/Artist/Album/float32.wav") == src_float);  // copied unchanged
    ValidationReport vr;
    CHECK(validate_export(stick.string(), vr));
    CHECK(!export_usb(sel, {}, [&] { ExportOptions b = o; b.player_profile = "nope"; return b; }(), err, res) && !err.empty());
  }
  // ---- export with convert_incompatible -------------------------------------------------------------------
  {
    const fs::path stick = tmp / "stick_conv";
    ExportOptions o;
    o.destination = stick.string();
    o.convert_incompatible = true;
    ExportResult res;
    std::string err;
    CHECK(export_usb(sel, {}, o, err, res));
    CHECK(res.converted == 2 && res.remaining_will_not_play() == 1);  // the Opus file cannot be fixed
    CHECK(find(res.report, 1)->converted && find(res.report, 5)->converted && !find(res.report, 4)->converted);
    CHECK(slurp(tracks[0].source_path) == src_float && slurp(tracks[4].source_path) == src_int);  // sources untouched
    // Float WAV: same file name (already .wav), now 24-bit PCM with the right sample values.
    const Bytes out = slurp(stick / "Contents/Artist/Album/float32.wav");
    CHECK(out.size() == 44 + 8 * 3 && std::memcmp(out.data(), "RIFF", 4) == 0 && std::memcmp(out.data() + 8, "WAVEfmt ", 8) == 0);
    CHECK(out[20] == 1 && out[22] == 2 && out[24] == 0x44 && out[25] == 0xAC && out[34] == 24);  // PCM, stereo, 44100, 24 bit
    CHECK(out[40] == 24 && out[41] == 0 && out[42] == 0 && out[43] == 0);                        // data chunk size
    const int expect[8] = {4194304, -4194304, 8388607, -8388608, 8388607, 0, 2097152, -2097152};
    for (int i = 0; i < 8; ++i) CHECK(s24_at(out, 44 + 3 * i) == expect[i]);
    // 32-bit integer AIFF: new .wav name next to the (absent) original, big-endian source decoded correctly.
    const Bytes out2 = slurp(stick / "Contents/Artist/Album/int32.wav");
    CHECK(out2.size() == 44 + 8 * 3);
    const int expect2[8] = {0x400000, -0x400000, 8388607, -8388608, 1, 0, 0, 0};
    for (int i = 0; i < 8; ++i) CHECK(s24_at(out2, 44 + 3 * i) == expect2[i]);
    CHECK(!fs::exists(stick / "Contents/Artist/Album/int32.aif"));
    CHECK(fs::exists(stick / "Contents/Artist/Album/voice.opus"));  // unfixable file still copied unchanged
    // The exported database describes the converted files.
    ReadExport rx;
    CHECK(read_export(stick.string(), rx, err));
    bool saw_float = false, saw_int = false, saw_opus = false;
    for (const auto& t : rx.tracks) {
      if (t.track.id == 1) {
        saw_float = true;
        CHECK(t.file_type == 0x0B && t.track.sample_rate == 44100 && t.track.bitrate == 2117);  // 44100*24*2/1000
        CHECK(t.file_path == "/Contents/Artist/Album/float32.wav");
      }
      if (t.track.id == 5) {
        saw_int = true;
        CHECK(t.file_type == 0x0B && t.file_path == "/Contents/Artist/Album/int32.wav");
      }
      if (t.track.id == 4) saw_opus = true;
    }
    CHECK(saw_float && saw_int && saw_opus);
    ValidationReport vr;
    CHECK(validate_export(stick.string(), vr));
    CHECK(vr.ok());
    // Re-export onto the same stick is still valid.
    CHECK(export_usb(sel, {}, o, err, res) && validate_export(stick.string(), vr));
    // The file the stick holds probes as 24-bit PCM.
    const FormatInfo fi = probe_format((stick / "Contents/Artist/Album/float32.wav").string());
    CHECK(fi.recognized && fi.codec == Codec::Pcm && fi.bit_depth == 24);
  }
  // ---- hot cues 9-16: the stick format carries 8; the rest is reported, never an error ------------------------
  {
    ExportTrack t = tracks[1];  // a playable file (the old-entry-point block below exports it too)
    for (int slot = 1; slot <= 12; ++slot) {
      ExportCue c;
      c.hot = true;
      c.slot = slot;
      c.time_ms = 50 * slot;
      t.cues.push_back(c);
    }
    ExportCue mem;  // a memory cue is not a hot cue and is kept
    mem.time_ms = 700;
    t.cues.push_back(mem);
    CHECK(hot_cues_beyond_format(t) == 4);
    const PreflightReport pr = preflight_export({t}, default_player_profile());
    bool noted = false;
    for (const auto& i : pr.tracks.at(0).issues) noted = noted || (i.id == "cues.not_exported" && i.verdict == Verdict::Warning);
    CHECK(noted);
    const fs::path stick = tmp / "stick_cues";
    ExportOptions o;
    o.destination = stick.string();
    ExportResult res;
    std::string err;
    CHECK(export_usb({t}, {}, o, err, res));
    CHECK(res.cues_not_exported == 4 && res.tracks_with_cues_not_exported == 1);
    ValidationReport vr;
    CHECK(validate_export(stick.string(), vr) && vr.ok());
    ReadExport rx;
    CHECK(read_export(stick.string(), rx, err) && rx.tracks.size() == 1);
    int hot = 0, memory = 0, highest = 0;
    for (const ExportCue& c : rx.tracks[0].track.cues) {
      if (c.hot) {
        ++hot;
        highest = std::max(highest, c.slot);
      } else {
        ++memory;
      }
    }
    CHECK(hot == 8 && highest == 8 && memory == 1);
  }
  // The old entry point still works and never converts.
  {
    const fs::path stick = tmp / "stick_old";
    ExportOptions o;
    o.destination = stick.string();
    std::string err;
    CHECK(export_usb({tracks[1]}, {}, o, err));
  }
  fs::remove_all(tmp);
  std::puts("test_export_preflight: ok");
  return 0;
}
