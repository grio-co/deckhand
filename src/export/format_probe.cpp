// Audio header sniffer. Bounded reads only (header areas, never the whole file).
#include "deckhand/export/format_probe.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace deckhand::usbexport {
namespace {

using U8 = std::uint8_t;
using U64 = std::uint64_t;

constexpr int kMaxChunks = 256;        // chunk/atom walk limit
constexpr U64 kMp3SyncScan = 8192;     // bytes scanned for the first MPEG frame after tags

std::uint32_t be16(const U8* p) { return (std::uint32_t(p[0]) << 8) | p[1]; }
std::uint32_t be32(const U8* p) { return (be16(p) << 16) | be16(p + 2); }
U64 be64(const U8* p) { return (U64(be32(p)) << 32) | be32(p + 4); }
std::uint32_t le16(const U8* p) { return p[0] | (std::uint32_t(p[1]) << 8); }
std::uint32_t le32(const U8* p) { return le16(p) | (le16(p + 2) << 16); }
U64 le64(const U8* p) { return le32(p) | (U64(le32(p + 4)) << 32); }

bool tag_is(const U8* p, const char* t) { return std::memcmp(p, t, 4) == 0; }
std::string fourcc(const U8* p) {
  std::string s;
  for (int i = 0; i < 4; ++i) s += (p[i] >= 32 && p[i] < 127) ? char(p[i]) : '?';
  return s;
}

// Bounded random-access reader.
class Reader {
 public:
  explicit Reader(const std::string& path) : f_(std::filesystem::path(path), std::ios::binary) {
    std::error_code ec;
    size_ = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) size_ = 0;
    ok_ = f_.good();
  }
  bool ok() const { return ok_; }
  U64 size() const { return size_; }
  // Reads up to n bytes at off; returns the count actually read (0 on failure / beyond the end).
  std::size_t read(U64 off, U8* buf, std::size_t n) {
    if (!ok_ || off >= size_) return 0;
    n = static_cast<std::size_t>(std::min<U64>(n, size_ - off));
    f_.clear();
    f_.seekg(static_cast<std::streamoff>(off));
    f_.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(n));
    const auto got = f_.gcount();
    return got > 0 ? static_cast<std::size_t>(got) : 0;
  }
  bool read_exact(U64 off, U8* buf, std::size_t n) { return read(off, buf, n) == n; }

 private:
  std::ifstream f_;
  U64 size_ = 0;
  bool ok_ = false;
};

void fail(FormatInfo& f, const char* why) {
  f.recognized = false;
  f.error = why;
}

// ---- WAV / RF64 ---------------------------------------------------------------------------------------------

bool probe_wav(Reader& r, FormatInfo& f, bool rf64) {
  f.container = rf64 ? Container::Rf64 : Container::Wav;
  bool got_fmt = false, got_data = false;
  U64 ds64_data = 0;
  bool have_ds64 = false;
  U64 pos = 12;
  int tag = 0;
  for (int i = 0; i < kMaxChunks && pos + 8 <= r.size(); ++i) {
    U8 h[8];
    if (!r.read_exact(pos, h, 8)) break;
    U64 sz = le32(h + 4);
    const U64 body = pos + 8;
    if (tag_is(h, "ds64")) {
      U8 d[24];
      if (r.read_exact(body, d, 24)) {
        ds64_data = le64(d + 8);
        have_ds64 = true;
      }
    } else if (tag_is(h, "fmt ")) {
      U8 b[40] = {};
      const std::size_t n = r.read(body, b, std::min<U64>(sz, 40));
      if (n < 16) break;
      tag = static_cast<int>(le16(b));
      f.channels = static_cast<int>(le16(b + 2));
      f.sample_rate = static_cast<int>(le32(b + 4));
      f.block_align = static_cast<int>(le16(b + 12));
      f.bit_depth = static_cast<int>(le16(b + 14));
      if (tag == 0xFFFE) {
        static const U8 kGuidTail[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
        if (n >= 40 && sz >= 40 && std::memcmp(b + 26, kGuidTail, 14) == 0)
          tag = static_cast<int>(le16(b + 24));
        else
          tag = -1;  // extensible with an unknown sub-format
      }
      got_fmt = true;
    } else if (tag_is(h, "data")) {
      if (rf64 && sz == 0xFFFFFFFFu && have_ds64) sz = ds64_data;
      f.data_offset = body;
      f.data_bytes = std::min<U64>(sz, body < r.size() ? r.size() - body : 0);  // streamed/truncated files
      got_data = true;
      if (sz == 0xFFFFFFFFu) break;  // size unknown: cannot skip over it
    }
    if (got_fmt && got_data) break;
    const U64 next = body + sz + (sz & 1);
    if (next <= pos) break;
    pos = next;
  }
  if (!got_fmt) {
    f.truncated = true;
    fail(f, "WAV header has no fmt chunk");
    return false;
  }
  if (!got_data) f.truncated = true;
  if (tag == 1) {
    f.codec = Codec::Pcm;
    f.codec_name = "PCM";
  } else if (tag == 3) {
    f.codec = Codec::Float;
    f.codec_name = "IEEE float";
    f.is_float = true;
  } else {
    f.codec = Codec::Other;
    f.codec_name = tag < 0 ? "WAVE extensible (unknown sub-format)" : "WAVE format " + std::to_string(tag);
  }
  if (f.channels <= 0 || f.sample_rate <= 0) {
    f.truncated = true;
    fail(f, "WAV fmt chunk is damaged");
    return false;
  }
  f.recognized = true;
  return true;
}

// ---- AIFF / AIFC --------------------------------------------------------------------------------------------

int extended80_to_int(const U8* p) {
  const int exp = static_cast<int>(be16(p) & 0x7FFF);
  const U64 mant = be64(p + 2);
  if (exp == 0 || exp == 0x7FFF || mant == 0) return 0;
  const double v = std::ldexp(static_cast<double>(mant), exp - 16383 - 63);
  if (!(v > 0 && v < 1e8)) return 0;
  return static_cast<int>(v + 0.5);
}

bool probe_aiff(Reader& r, FormatInfo& f, bool aifc) {
  f.container = aifc ? Container::Aifc : Container::Aiff;
  bool got_comm = false, got_ssnd = false;
  std::string comp = "NONE";
  U64 pos = 12;
  for (int i = 0; i < kMaxChunks && pos + 8 <= r.size(); ++i) {
    U8 h[8];
    if (!r.read_exact(pos, h, 8)) break;
    const U64 sz = be32(h + 4);
    const U64 body = pos + 8;
    if (tag_is(h, "COMM")) {
      U8 b[26] = {};
      const std::size_t n = r.read(body, b, std::min<U64>(sz, 26));
      if (n < 18) break;
      f.channels = static_cast<int>(be16(b));
      f.bit_depth = static_cast<int>(be16(b + 6));
      f.sample_rate = extended80_to_int(b + 8);
      if (aifc) {
        if (n < 22) break;
        comp = fourcc(b + 18);
      }
      got_comm = true;
    } else if (tag_is(h, "SSND")) {
      U8 b[8] = {};
      if (r.read(body, b, 8) == 8) {
        const U64 off = be32(b);
        f.data_offset = body + 8 + off;
        const U64 avail = r.size() > f.data_offset ? r.size() - f.data_offset : 0;
        const U64 declared = sz >= 8 + off ? sz - 8 - off : 0;
        f.data_bytes = std::min(avail, declared);
        got_ssnd = true;
      }
    }
    if (got_comm && got_ssnd) break;
    const U64 next = body + sz + (sz & 1);
    if (next <= pos) break;
    pos = next;
  }
  if (!got_comm) {
    f.truncated = true;
    fail(f, "AIFF header has no COMM chunk");
    return false;
  }
  if (!got_ssnd) f.truncated = true;
  f.big_endian = true;
  if (comp == "NONE" || comp == "twos" || comp == "in24" || comp == "in32") {
    f.codec = Codec::Pcm;
    f.codec_name = comp == "NONE" ? "PCM" : comp;
  } else if (comp == "sowt") {
    f.codec = Codec::Pcm;
    f.codec_name = "sowt";
    f.big_endian = false;
  } else if (comp == "fl32" || comp == "FL32") {
    f.codec = Codec::Float;
    f.codec_name = comp;
    f.is_float = true;
    f.bit_depth = 32;
  } else if (comp == "fl64" || comp == "FL64") {
    f.codec = Codec::Float;
    f.codec_name = comp;
    f.is_float = true;
    f.bit_depth = 64;
  } else {
    f.codec = Codec::Other;
    f.codec_name = comp;
  }
  if (f.channels <= 0 || f.sample_rate <= 0 || f.bit_depth <= 0) {
    f.truncated = true;
    fail(f, "AIFF COMM chunk is damaged");
    return false;
  }
  f.block_align = f.channels * ((f.bit_depth + 7) / 8);
  f.recognized = true;
  return true;
}

// ---- FLAC ---------------------------------------------------------------------------------------------------

bool probe_flac(Reader& r, FormatInfo& f, U64 base) {
  f.container = Container::Flac;
  U8 b[4 + 34];
  if (!r.read_exact(base, b, sizeof b)) {
    f.truncated = true;
    fail(f, "FLAC header is cut off");
    return false;
  }
  // b[0..3] "fLaC"; b[4] block header (type 0 = STREAMINFO), 3-byte length, then 34 bytes of STREAMINFO.
  if ((b[4] & 0x7F) != 0 || ((b[5] << 16) | (b[6] << 8) | b[7]) < 34) {
    f.truncated = true;
    fail(f, "FLAC STREAMINFO is missing");
    return false;
  }
  const U8* s = b + 8 + 10;  // 2+2+3+3 bytes of block/frame sizes
  const std::uint32_t v = (std::uint32_t(s[0]) << 12) | (std::uint32_t(s[1]) << 4) | (s[2] >> 4);
  f.sample_rate = static_cast<int>(v);
  f.channels = ((s[2] >> 1) & 7) + 1;
  f.bit_depth = (((s[2] & 1) << 4) | (s[3] >> 4)) + 1;
  f.codec = Codec::Flac;
  f.codec_name = "FLAC";
  if (f.sample_rate <= 0) {
    f.truncated = true;
    fail(f, "FLAC STREAMINFO is damaged");
    return false;
  }
  f.recognized = true;
  return true;
}

// ---- MP3 ----------------------------------------------------------------------------------------------------

struct Mp3Hdr {
  int version = 0, layer = 0, bitrate = 0, rate = 0, channels = 0, frame_len = 0, spf = 0;
  bool crc = false;
};

bool parse_mp3_header(const U8* p, Mp3Hdr& h) {
  if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
  const int ver_bits = (p[1] >> 3) & 3, layer_bits = (p[1] >> 1) & 3;
  if (ver_bits == 1 || layer_bits == 0) return false;
  const int br_idx = p[2] >> 4, sr_idx = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
  if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return false;
  h.version = ver_bits == 3 ? 1 : (ver_bits == 2 ? 2 : 25);
  h.layer = 4 - layer_bits;
  static const int kV1[3][14] = {{32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
                                 {32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
                                 {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}};
  static const int kV2L1[14] = {32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256};
  static const int kV2[14] = {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
  h.bitrate = h.version == 1 ? kV1[h.layer - 1][br_idx - 1] : (h.layer == 1 ? kV2L1[br_idx - 1] : kV2[br_idx - 1]);
  static const int kRates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
  h.rate = kRates[h.version == 1 ? 0 : (h.version == 2 ? 1 : 2)][sr_idx];
  h.channels = ((p[3] >> 6) & 3) == 3 ? 1 : 2;
  h.crc = (p[1] & 1) == 0;
  h.spf = h.layer == 1 ? 384 : (h.layer == 3 && h.version != 1 ? 576 : 1152);
  const long long br = static_cast<long long>(h.bitrate) * 1000;
  if (h.layer == 1)
    h.frame_len = static_cast<int>((12 * br / h.rate + pad) * 4);
  else
    h.frame_len = static_cast<int>((h.layer == 3 && h.version != 1 ? 72 : 144) * br / h.rate + pad);
  return h.frame_len >= 24;
}

bool probe_mp3(Reader& r, FormatInfo& f, U64 start) {
  f.container = Container::Mp3;
  const U64 end = std::min<U64>(r.size(), start + kMp3SyncScan);
  std::vector<U8> buf(static_cast<std::size_t>(end > start ? end - start : 0));
  const std::size_t got = r.read(start, buf.data(), buf.size());
  for (std::size_t i = 0; i + 4 <= got; ++i) {
    Mp3Hdr h;
    if (!parse_mp3_header(&buf[i], h)) continue;
    // Confidence: the next frame must start with a sync too, unless this frame runs to (or past) the end of file.
    const U64 frame_at = start + i;
    const U64 next_at = frame_at + static_cast<U64>(h.frame_len);
    U8 n4[4];
    Mp3Hdr h2;
    const bool ends_here = next_at + 4 > r.size();
    if (!ends_here) {
      if (!r.read_exact(next_at, n4, 4) || !parse_mp3_header(n4, h2) || h2.layer != h.layer || h2.rate != h.rate) continue;
    }
    f.codec = Codec::Mp3;
    f.codec_name = "MP3";
    f.mpeg_version = h.version;
    f.layer = h.layer;
    f.sample_rate = h.rate;
    f.channels = h.channels;
    f.bitrate_kbps = h.bitrate;
    // Xing / Info (offset after the side info) or VBRI (fixed offset 32 after the header).
    const bool mono = h.channels == 1;
    const std::size_t side = h.version == 1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    U8 x[40];
    const std::size_t xo = 4 + (h.crc ? 2 : 0) + side;
    if (h.layer == 3 && r.read(frame_at + xo, x, sizeof x) >= 16) {
      if (tag_is(x, "Xing") || tag_is(x, "Info")) {
        f.vbr = tag_is(x, "Xing");
        const std::uint32_t flags = be32(x + 4);
        std::size_t o = 8;
        std::uint32_t frames = 0, bytes = 0;
        if (flags & 1) { frames = be32(x + o); o += 4; }
        if (flags & 2) { bytes = be32(x + o); }
        if (f.vbr && frames > 0 && bytes > 0) {
          const double secs = double(frames) * h.spf / h.rate;
          if (secs > 0) f.bitrate_kbps = static_cast<int>(bytes * 8.0 / secs / 1000.0 + 0.5);
        }
      }
    }
    U8 v[16];
    if (r.read(frame_at + 36, v, 16) >= 4 && tag_is(v, "VBRI")) f.vbr = true;
    f.recognized = true;
    return true;
  }
  fail(f, "no MPEG audio frame found");
  return false;
}

// ---- MP4 / M4A ----------------------------------------------------------------------------------------------

struct Atom {
  U64 off = 0, size = 0, hdr = 8;
  U8 type[4] = {};
  U64 body() const { return off + hdr; }
  U64 end() const { return off + size; }
};

bool read_atom(Reader& r, U64 off, U64 limit, Atom& a) {
  U8 h[16];
  if (off + 8 > limit) return false;
  const std::size_t n = r.read(off, h, 16);
  if (n < 8) return false;
  U64 sz = be32(h);
  a.off = off;
  a.hdr = 8;
  std::memcpy(a.type, h + 4, 4);
  if (sz == 1) {
    if (n < 16) return false;
    sz = be64(h + 8);
    a.hdr = 16;
  } else if (sz == 0) {
    sz = limit - off;
  }
  if (sz < a.hdr) return false;
  a.size = std::min<U64>(sz, limit - off);
  return true;
}

bool find_child(Reader& r, const Atom& parent, const char* type, Atom& out, U64 skip = 0) {
  U64 pos = parent.body();
  U64 seen = 0;
  for (int i = 0; i < kMaxChunks; ++i) {
    Atom a;
    if (!read_atom(r, pos, parent.end(), a)) return false;
    if (tag_is(a.type, type) && seen++ >= skip) {
      out = a;
      return true;
    }
    if (a.end() <= pos) return false;
    pos = a.end();
  }
  return false;
}

bool probe_mp4(Reader& r, FormatInfo& f) {
  f.container = Container::Mp4;
  Atom root;
  root.off = 0;
  root.hdr = 0;
  root.size = r.size();
  Atom ftyp;
  if (read_atom(r, 0, r.size(), ftyp) && tag_is(ftyp.type, "ftyp")) {
    U8 b[8];
    if (r.read(ftyp.body(), b, 4) == 4 && tag_is(b, "M4P ")) f.drm = true;
  }
  Atom moov;
  if (!find_child(r, root, "moov", moov)) {
    f.truncated = true;
    fail(f, "MP4 has no moov atom");
    return false;
  }
  for (U64 t = 0; t < 32; ++t) {
    Atom trak, mdia, hdlr, minf, stbl, stsd;
    if (!find_child(r, moov, "trak", trak, t)) break;
    if (!find_child(r, trak, "mdia", mdia) || !find_child(r, mdia, "hdlr", hdlr)) continue;
    U8 hb[12];
    if (r.read(hdlr.body(), hb, 12) < 12 || !tag_is(hb + 8, "soun")) continue;
    if (!find_child(r, mdia, "minf", minf) || !find_child(r, minf, "stbl", stbl) || !find_child(r, stbl, "stsd", stsd))
      continue;
    U8 e[44] = {};
    const std::size_t n = r.read(stsd.body(), e, sizeof e);
    if (n < 8 + 36 || be32(e + 4) < 1) {
      f.truncated = true;
      fail(f, "MP4 sample description is cut off");
      return false;
    }
    const U8* en = e + 8;  // first sample entry
    const std::uint32_t esz = be32(en);
    f.channels = static_cast<int>(be16(en + 24));
    f.bit_depth = static_cast<int>(be16(en + 26));
    f.sample_rate = static_cast<int>(be16(en + 32));
    const U64 entry_off = stsd.body() + 8;
    const U64 entry_end = std::min<U64>(stsd.end(), entry_off + esz);
    if (tag_is(en + 4, "mp4a")) {
      f.codec = Codec::Aac;
      f.codec_name = "AAC";
      f.bit_depth = 0;
      // FairPlay-protected files keep mp4a but add a 'sinf' child.
      U64 pos = entry_off + 36;
      for (int i = 0; i < 16; ++i) {
        Atom c;
        if (!read_atom(r, pos, entry_end, c)) break;
        if (tag_is(c.type, "sinf")) f.drm = true;
        if (c.end() <= pos) break;
        pos = c.end();
      }
    } else if (tag_is(en + 4, "alac")) {
      f.codec = Codec::Alac;
      f.codec_name = "ALAC";
      Atom c;
      U8 k[36];
      if (read_atom(r, entry_off + 36, entry_end, c) && tag_is(c.type, "alac") && r.read(c.off, k, 36) == 36) {
        f.bit_depth = k[17];
        f.channels = k[21];
        f.sample_rate = static_cast<int>(be32(k + 32));
      }
    } else if (tag_is(en + 4, "drms") || tag_is(en + 4, "enca") || tag_is(en + 4, "drmi")) {
      f.codec = Codec::Drm;
      f.codec_name = fourcc(en + 4);
      f.drm = true;
    } else {
      f.codec = Codec::Other;
      f.codec_name = fourcc(en + 4);
    }
    if (f.drm && f.codec != Codec::Drm) f.codec_name += " (protected)";
    f.recognized = true;
    return true;
  }
  fail(f, "MP4 has no audio track");
  return false;
}

// ---- Ogg / ASF ----------------------------------------------------------------------------------------------

bool probe_ogg(Reader& r, FormatInfo& f) {
  f.container = Container::Ogg;
  U8 b[64] = {};
  const std::size_t n = r.read(0, b, sizeof b);
  f.recognized = true;
  f.codec = Codec::Other;
  f.codec_name = "Ogg";
  if (n >= 44 && std::memcmp(b + 28, "OpusHead", 8) == 0) {
    f.codec = Codec::Opus;
    f.codec_name = "Opus";
    f.channels = b[37];
    f.sample_rate = static_cast<int>(le32(b + 40));
  } else if (n >= 44 && std::memcmp(b + 28, "\x01vorbis", 7) == 0) {
    f.codec = Codec::Vorbis;
    f.codec_name = "Vorbis";
    f.channels = b[39];
    f.sample_rate = static_cast<int>(le32(b + 40));
  } else if (n >= 33 && std::memcmp(b + 28, "\x7f" "FLAC", 5) == 0) {
    f.codec_name = "Ogg FLAC";
  }
  return true;
}

bool probe_asf(FormatInfo& f) {
  f.container = Container::Asf;
  f.codec = Codec::Wma;
  f.codec_name = "WMA";
  f.recognized = true;
  return true;
}

}  // namespace

const char* container_name(Container c) {
  switch (c) {
    case Container::Wav: return "WAV";
    case Container::Rf64: return "RF64 WAV";
    case Container::Aiff: return "AIFF";
    case Container::Aifc: return "AIFF-C";
    case Container::Flac: return "FLAC";
    case Container::Mp3: return "MP3";
    case Container::Mp4: return "MP4/M4A";
    case Container::Ogg: return "Ogg";
    case Container::Asf: return "ASF/WMA";
    default: return "unknown";
  }
}

FormatInfo probe_format(const std::string& path) {
  FormatInfo f;
  try {
    Reader r(path);
    f.file_size = r.size();
    if (!r.ok()) {
      fail(f, "cannot open file");
      return f;
    }
    if (r.size() == 0) {
      fail(f, "empty file");
      return f;
    }
    U8 h[16] = {};
    const std::size_t n = r.read(0, h, sizeof h);
    if (n < 4) {
      fail(f, "file too short to be audio");
      return f;
    }
    U64 base = 0;  // after an ID3v2 tag (MP3, and FLAC files that carry one)
    if (n >= 10 && std::memcmp(h, "ID3", 3) == 0) {
      const U64 sz = (U64(h[6] & 0x7F) << 21) | (U64(h[7] & 0x7F) << 14) | (U64(h[8] & 0x7F) << 7) | (h[9] & 0x7F);
      base = 10 + sz + ((h[5] & 0x10) ? 10 : 0);
      if (base >= r.size()) {
        fail(f, "only an ID3 tag, no audio");
        return f;
      }
      U8 m[4];
      if (r.read_exact(base, m, 4) && tag_is(m, "fLaC")) {
        probe_flac(r, f, base);
        return f;
      }
      probe_mp3(r, f, base);
      return f;
    }
    if (n >= 12 && (tag_is(h, "RIFF") || tag_is(h, "RF64") || tag_is(h, "BW64")) && tag_is(h + 8, "WAVE")) {
      probe_wav(r, f, !tag_is(h, "RIFF"));
    } else if (n >= 12 && tag_is(h, "FORM") && (tag_is(h + 8, "AIFF") || tag_is(h + 8, "AIFC"))) {
      probe_aiff(r, f, tag_is(h + 8, "AIFC"));
    } else if (tag_is(h, "fLaC")) {
      probe_flac(r, f, 0);
    } else if (tag_is(h, "OggS")) {
      probe_ogg(r, f);
    } else if (n >= 16 && h[0] == 0x30 && h[1] == 0x26 && h[2] == 0xB2 && h[3] == 0x75 && h[4] == 0x8E && h[5] == 0x66) {
      probe_asf(f);
    } else if (n >= 12 && tag_is(h + 4, "ftyp")) {
      probe_mp4(r, f);
    } else if (!probe_mp3(r, f, 0)) {
      f.container = Container::Unknown;
      f.error = "unknown format";
    }
  } catch (...) {
    f = FormatInfo{};
    f.error = "probe failed";
  }
  return f;
}

std::string describe_format(const FormatInfo& f) {
  if (!f.recognized) return f.error.empty() ? "unrecognized file" : f.error;
  std::string s;
  const std::string depth = f.bit_depth > 0 ? std::to_string(f.bit_depth) + "-bit " : std::string();
  switch (f.codec) {
    case Codec::Float: s = depth + "float " + container_name(f.container); break;
    case Codec::Pcm: s = depth + container_name(f.container); break;
    case Codec::Mp3:
      s = "MPEG-" + std::string(f.mpeg_version == 1 ? "1" : (f.mpeg_version == 2 ? "2" : "2.5")) + " layer " +
          std::to_string(f.layer) + " MP3";
      break;
    case Codec::Flac:
    case Codec::Alac: s = depth + f.codec_name; break;
    case Codec::Aac:
    case Codec::Other: s = f.codec_name + " in " + container_name(f.container); break;
    default: s = f.codec_name; break;
  }
  if (f.sample_rate > 0) s += ", " + std::to_string(f.sample_rate) + " Hz";
  if (f.channels > 0) s += ", " + std::to_string(f.channels) + " ch";
  if (f.bitrate_kbps > 0) s += ", " + std::to_string(f.bitrate_kbps) + " kbps" + (f.vbr ? " VBR" : "");
  return s;
}

}  // namespace deckhand::usbexport
