#include "deckhand/capture/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

#if defined(_MSC_VER)
#define dh_fseeko _fseeki64
#define dh_ftello _ftelli64
#else
#define dh_fseeko fseeko
#define dh_ftello ftello
#endif

namespace deckhand::capture {
namespace {

void put32(unsigned char* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i)); }
void put64(unsigned char* p, std::uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i)); }
void put16(unsigned char* p, std::uint16_t v) { p[0] = static_cast<unsigned char>(v); p[1] = static_cast<unsigned char>(v >> 8); }
std::uint32_t get32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint64_t get64(const unsigned char* p) { return get32(p) | (static_cast<std::uint64_t>(get32(p + 4)) << 32); }
std::uint16_t get16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

bool printable_id(const unsigned char* p) {
  for (int i = 0; i < 4; ++i) if (p[i] < 0x20 || p[i] > 0x7e) return false;
  return true;
}

// True when the bytes from `pos` to `end` form a clean chain of chunks (ids printable, sizes landing exactly on `end`).
bool chunk_chain_ok(std::FILE* f, std::uint64_t pos, std::uint64_t end) {
  if (pos == end) return true;
  while (pos < end) {
    unsigned char h[8];
    if (end - pos < 8 || dh_fseeko(f, static_cast<long long>(pos), SEEK_SET) != 0 || std::fread(h, 1, 8, f) != 8) return false;
    if (!printable_id(h)) return false;
    const std::uint64_t sz = get32(h + 4);
    pos += 8 + sz + (sz & 1);
  }
  return pos == end || pos == end + 1;  // the final chunk's pad byte may be missing
}

}  // namespace

int seek64(std::FILE* f, std::uint64_t pos) { return dh_fseeko(f, static_cast<long long>(pos), SEEK_SET); }

std::uint64_t file_size(const std::string& path) {
  std::error_code ec;
  const auto n = std::filesystem::file_size(std::filesystem::path(path), ec);
  return ec ? 0 : static_cast<std::uint64_t>(n);
}

void pack_samples(const float* in, std::size_t n, SampleFormat f, unsigned char* out) {
  if (f == SampleFormat::Float32) {
    for (std::size_t i = 0; i < n; ++i) {
      std::uint32_t u;
      std::memcpy(&u, &in[i], 4);
      put32(out + i * 4, u);
    }
  } else if (f == SampleFormat::Pcm24) {
    for (std::size_t i = 0; i < n; ++i) {
      const float x = std::clamp(in[i], -1.0f, 1.0f);
      const std::int32_t v = static_cast<std::int32_t>(std::lrintf(x * 8388607.0f));
      out[i * 3] = static_cast<unsigned char>(v);
      out[i * 3 + 1] = static_cast<unsigned char>(v >> 8);
      out[i * 3 + 2] = static_cast<unsigned char>(v >> 16);
    }
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      const float x = std::clamp(in[i], -1.0f, 1.0f);
      const std::int32_t v = static_cast<std::int32_t>(std::lrintf(x * 32767.0f));
      put16(out + i * 2, static_cast<std::uint16_t>(v));
    }
  }
}

void unpack_samples(const unsigned char* in, std::size_t n, SampleFormat f, float* out) {
  if (f == SampleFormat::Float32) {
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint32_t u = get32(in + i * 4);
      std::memcpy(&out[i], &u, 4);
    }
  } else if (f == SampleFormat::Pcm24) {
    for (std::size_t i = 0; i < n; ++i) {
      std::int32_t v = in[i * 3] | (in[i * 3 + 1] << 8) | (in[i * 3 + 2] << 16);
      if (v & 0x800000) v -= 0x1000000;
      out[i] = static_cast<float>(v) / 8388607.0f;
    }
  } else {
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(static_cast<std::int16_t>(get16(in + i * 2))) / 32767.0f;
  }
}

// ---- tags --------------------------------------------------------------------------------------------------------------

namespace {

bool ascii_only(const std::string& s) {
  for (unsigned char c : s) if (c >= 0x80) return false;
  return true;
}

// Text frame payload: encoding byte, then ISO-8859-1 (ASCII only here) or UTF-16 with a byte-order mark.
std::string id3_text(const std::string& s) {
  if (ascii_only(s)) return std::string(1, '\0') + s;
  std::string out(1, '\x01');
  out += "\xFF\xFE";
  for (std::size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    std::uint32_t cp = c;
    int extra = 0;
    if (c >= 0xF0) { cp = c & 7; extra = 3; }
    else if (c >= 0xE0) { cp = c & 15; extra = 2; }
    else if (c >= 0xC0) { cp = c & 31; extra = 1; }
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    auto u16 = [&](std::uint32_t v) { out.push_back(static_cast<char>(v & 0xFF)); out.push_back(static_cast<char>(v >> 8)); };
    if (cp >= 0x10000) {
      cp -= 0x10000;
      u16(0xD800 + (cp >> 10));
      u16(0xDC00 + (cp & 0x3FF));
    } else {
      u16(cp);
    }
  }
  return out;
}

void id3_frame(std::string& body, const char* id, const std::string& payload) {
  if (payload.size() <= 1) return;
  body.append(id, 4);
  const std::uint32_t n = static_cast<std::uint32_t>(payload.size());
  for (int i = 3; i >= 0; --i) body.push_back(static_cast<char>(n >> (8 * i)));
  body.append(2, '\0');
  body += payload;
}

}  // namespace

std::string build_id3_tag(const TrackTags& t) {
  std::string body;
  id3_frame(body, "TIT2", id3_text(t.title));
  id3_frame(body, "TPE1", id3_text(t.artist));
  id3_frame(body, "TALB", id3_text(t.album));
  id3_frame(body, "TCON", id3_text(t.genre));
  id3_frame(body, "TYER", id3_text(t.year));
  if (!t.comment.empty()) {
    std::string c = id3_text(t.comment);
    c.insert(1, "eng");
    c.insert(4, std::string(c[0] == '\x01' ? 2 : 1, '\0'));  // empty description
    id3_frame(body, "COMM", c);
  }
  if (t.track_number > 0) {
    std::string n = std::to_string(t.track_number);
    if (t.track_total > 0) n += "/" + std::to_string(t.track_total);
    id3_frame(body, "TRCK", id3_text(n));
  }
  std::string tag = "ID3";
  tag.push_back(3);
  tag.push_back(0);
  tag.push_back(0);
  const std::uint32_t n = static_cast<std::uint32_t>(body.size());
  for (int i = 3; i >= 0; --i) tag.push_back(static_cast<char>((n >> (7 * i)) & 0x7F));
  return tag + body;
}

// ---- parsing -------------------------------------------------------------------------------------------------------------

bool read_wav_info(const std::string& path, WavInfo& info, std::string& error) {
  info = WavInfo{};
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { error = "could not open " + path; return false; }
  struct Closer { std::FILE* f; ~Closer() { std::fclose(f); } } closer{f};
  const std::uint64_t L = file_size(path);
  info.file_bytes = L;
  unsigned char h[12];
  if (std::fread(h, 1, 12, f) != 12 || (std::memcmp(h, "RIFF", 4) && std::memcmp(h, "RF64", 4)) || std::memcmp(h + 8, "WAVE", 4)) {
    error = "not a WAV file";
    return false;
  }
  info.rf64 = !std::memcmp(h, "RF64", 4);
  std::uint64_t ds64_data = 0;
  bool have_fmt = false;
  std::uint64_t off = 12;
  std::uint32_t data_decl32 = 0;
  while (off + 8 <= L) {
    unsigned char ch[8];
    if (dh_fseeko(f, static_cast<long long>(off), SEEK_SET) != 0 || std::fread(ch, 1, 8, f) != 8) break;
    const std::uint32_t sz = get32(ch + 4);
    const std::uint64_t body = off + 8;
    if (!std::memcmp(ch, "ds64", 4) && sz >= 28) {
      unsigned char d[28];
      if (std::fread(d, 1, 28, f) == 28) ds64_data = get64(d + 8);
    } else if (!std::memcmp(ch, "fmt ", 4) && sz >= 16) {
      unsigned char d[40] = {0};
      if (std::fread(d, 1, std::min<std::uint32_t>(sz, 40), f) < 16) break;
      std::uint16_t tag = get16(d);
      const std::uint16_t ch_n = get16(d + 2), bits = get16(d + 14);
      if (tag == 0xFFFE && sz >= 40) tag = get16(d + 24);
      info.fmt.sample_rate = get32(d + 4);
      info.fmt.channels = ch_n;
      if (tag == 1 && bits == 16) info.fmt.format = SampleFormat::Pcm16;
      else if (tag == 1 && bits == 24) info.fmt.format = SampleFormat::Pcm24;
      else if (tag == 3 && bits == 32) info.fmt.format = SampleFormat::Float32;
      else { error = "unsupported sample format"; return false; }
      have_fmt = ch_n > 0 && info.fmt.sample_rate > 0;
    } else if (!std::memcmp(ch, "data", 4)) {
      if (!have_fmt) { error = "data before fmt"; return false; }
      info.data_offset = body;
      data_decl32 = sz;
      std::uint64_t declared = sz;
      if (info.rf64 && sz == 0xFFFFFFFFu) declared = ds64_data;
      const std::uint64_t avail = L - body;
      if (declared > avail || (sz == 0xFFFFFFFFu && !info.rf64)) {
        info.data_bytes = avail;
        info.damaged = true;
      } else if (declared == avail) {
        info.data_bytes = declared;
      } else if (chunk_chain_ok(f, body + declared + (declared & 1), L)) {
        info.data_bytes = declared;
        info.trailer_bytes = L - (body + declared);
      } else {
        info.data_bytes = avail;  // growing file: the header is behind the data
        info.damaged = true;
      }
      if (info.damaged) info.data_bytes -= info.data_bytes % info.fmt.block_align();
      info.frames = info.data_bytes / info.fmt.block_align();
      info.our_layout = off == 72 && (info.rf64 ? true : true) && body == kWavHeaderBytes;
      if (info.our_layout) {
        unsigned char j[4];
        dh_fseeko(f, 12, SEEK_SET);
        info.our_layout = std::fread(j, 1, 4, f) == 4 && (!std::memcmp(j, "JUNK", 4) || !std::memcmp(j, "ds64", 4));
      }
      (void)data_decl32;
      return true;
    }
    off = body + sz + (sz & 1);
  }
  error = "no audio data in file";
  return false;
}

// ---- writer --------------------------------------------------------------------------------------------------------------

bool WavWriter::write_header(std::uint64_t trailer_bytes) {
  if (!file_) return false;
  const std::uint64_t data = frames_ * fmt_.block_align();
  const bool big = data > threshold_;
  unsigned char h[kWavHeaderBytes];
  std::memset(h, 0, sizeof(h));
  const std::uint64_t riff = kWavHeaderBytes - 8 + data + trailer_bytes;
  std::memcpy(h, big ? "RF64" : "RIFF", 4);
  put32(h + 4, big ? 0xFFFFFFFFu : static_cast<std::uint32_t>(riff));
  std::memcpy(h + 8, "WAVE", 4);
  std::memcpy(h + 12, big ? "ds64" : "JUNK", 4);
  put32(h + 16, 28);
  if (big) {
    put64(h + 20, riff);
    put64(h + 28, data);
    put64(h + 36, frames_);
  }
  std::memcpy(h + 48, "fmt ", 4);
  put32(h + 52, 16);
  const bool flt = fmt_.format == SampleFormat::Float32;
  put16(h + 56, flt ? 3 : 1);
  put16(h + 58, fmt_.channels);
  put32(h + 60, fmt_.sample_rate);
  put32(h + 64, fmt_.sample_rate * fmt_.block_align());
  put16(h + 68, static_cast<std::uint16_t>(fmt_.block_align()));
  put16(h + 70, static_cast<std::uint16_t>(bytes_per_sample(fmt_.format) * 8));
  std::memcpy(h + 72, "data", 4);
  put32(h + 76, big ? 0xFFFFFFFFu : static_cast<std::uint32_t>(data));
  if (dh_fseeko(file_, 0, SEEK_SET) != 0 || std::fwrite(h, 1, sizeof(h), file_) != sizeof(h)) return false;
  return dh_fseeko(file_, 0, SEEK_END) == 0;
}

bool WavWriter::create(const std::string& path, const WavFormat& fmt, std::string& error, std::uint64_t threshold) {
  close();
  if (fmt.channels == 0 || fmt.sample_rate == 0) { error = "bad format"; return false; }
  file_ = std::fopen(path.c_str(), "wb");
  if (!file_) { error = "could not create " + path; return false; }
  fmt_ = fmt;
  frames_ = 0;
  threshold_ = threshold;
  failed_ = false;
  if (!write_header(0)) { error = "could not write the header"; close(); return false; }
  std::fflush(file_);
  return true;
}

bool WavWriter::open_append(const std::string& path, std::string& error, std::uint64_t threshold) {
  close();
  WavInfo info;
  if (!read_wav_info(path, info, error)) return false;
  if (!info.our_layout) { error = "not a recording written by DeckHand"; return false; }
  std::error_code ec;
  std::filesystem::resize_file(std::filesystem::path(path), info.data_offset + info.data_bytes, ec);  // drops a partial frame and tags
  if (ec) { error = "could not truncate " + path; return false; }
  file_ = std::fopen(path.c_str(), "r+b");
  if (!file_) { error = "could not reopen " + path; return false; }
  fmt_ = info.fmt;
  frames_ = info.frames;
  threshold_ = threshold;
  failed_ = false;
  if (!write_header(0) || dh_fseeko(file_, 0, SEEK_END) != 0) { error = "could not update the header"; close(); return false; }
  return true;
}

bool WavWriter::write_raw(const unsigned char* bytes, std::uint64_t frames) {
  if (!file_) return false;
  const std::uint64_t bytes_n = frames * fmt_.block_align();
  if (std::fwrite(bytes, 1, static_cast<std::size_t>(bytes_n), file_) != bytes_n) { failed_ = true; return false; }
  frames_ += frames;
  return true;
}

bool WavWriter::write(const float* in, std::uint64_t frames) {
  if (!file_) return false;
  constexpr std::uint64_t kChunk = 4096;
  std::vector<unsigned char> buf(static_cast<std::size_t>(kChunk * fmt_.block_align()));
  for (std::uint64_t done = 0; done < frames;) {
    const std::uint64_t n = std::min(kChunk, frames - done);
    pack_samples(in + done * fmt_.channels, static_cast<std::size_t>(n * fmt_.channels), fmt_.format, buf.data());
    if (!write_raw(buf.data(), n)) return false;
    done += n;
  }
  return true;
}

bool WavWriter::flush() {
  if (!file_) return false;
  const bool ok = write_header(0) && std::fflush(file_) == 0;
  return ok && !failed_;
}

bool WavWriter::close(const TrackTags* tags) {
  if (!file_) return true;
  std::uint64_t trailer = 0;
  bool ok = !failed_;
  if (tags && !tags->empty()) {
    std::string trail;
    if ((frames_ * fmt_.block_align()) & 1) trail.push_back('\0');  // pad the data chunk
    auto chunk = [&](const char* id, const std::string& body) {
      unsigned char s[4];
      put32(s, static_cast<std::uint32_t>(body.size()));
      trail.append(id, 4);
      trail.append(reinterpret_cast<char*>(s), 4);
      trail += body;
      if (body.size() & 1) trail.push_back('\0');
    };
    std::string info = "INFO";
    auto sub = [&](const char* id, const std::string& v) {
      if (v.empty()) return;
      std::string z = v;
      z.push_back('\0');
      unsigned char s[4];
      put32(s, static_cast<std::uint32_t>(z.size()));
      info.append(id, 4);
      info.append(reinterpret_cast<char*>(s), 4);
      info += z;
      if (z.size() & 1) info.push_back('\0');
    };
    sub("INAM", tags->title);
    sub("IART", tags->artist);
    sub("IPRD", tags->album);
    sub("IGNR", tags->genre);
    sub("ICRD", tags->year);
    sub("ICMT", tags->comment);
    if (tags->track_number > 0) sub("ITRK", std::to_string(tags->track_number));
    chunk("LIST", info);
    chunk("id3 ", build_id3_tag(*tags));
    ok = ok && std::fwrite(trail.data(), 1, trail.size(), file_) == trail.size();
    trailer = trail.size();
  }
  ok = write_header(trailer) && ok;
  ok = std::fclose(file_) == 0 && ok;
  file_ = nullptr;
  return ok;
}

RepairResult repair_wav(const std::string& path) {
  RepairResult r;
  WavInfo info;
  if (!read_wav_info(path, info, r.error)) return r;
  if (!info.our_layout) { r.error = "not a recording written by DeckHand"; return r; }
  r.was_damaged = info.damaged;
  r.dropped_bytes = info.file_bytes - info.data_offset - info.data_bytes - info.trailer_bytes;
  WavWriter w;
  std::string err;
  if (info.trailer_bytes == 0 || info.damaged) {
    if (!w.open_append(path, err)) { r.error = err; return r; }
    r.frames = w.frames();
    r.ok = w.close();
  } else {
    r.frames = info.frames;
    r.ok = true;
  }
  return r;
}

// ---- reader --------------------------------------------------------------------------------------------------------------

bool WavReader::open(const std::string& path, std::string& error) {
  close();
  if (!read_wav_info(path, info_, error)) return false;
  file_ = std::fopen(path.c_str(), "rb");
  if (!file_) { error = "could not open " + path; return false; }
  return true;
}

void WavReader::close() {
  if (file_) std::fclose(file_);
  file_ = nullptr;
}

std::uint64_t WavReader::read(std::uint64_t start, std::uint64_t count, float* out) {
  if (!file_ || start >= info_.frames) return 0;
  count = std::min(count, info_.frames - start);
  const std::uint32_t ba = info_.fmt.block_align();
  constexpr std::uint64_t kChunk = 8192;
  buf_.resize(static_cast<std::size_t>(kChunk * ba));
  auto* raw = reinterpret_cast<unsigned char*>(buf_.data());
  std::uint64_t done = 0;
  while (done < count) {
    const std::uint64_t n = std::min(kChunk, count - done);
    if (dh_fseeko(file_, static_cast<long long>(info_.data_offset + (start + done) * ba), SEEK_SET) != 0) break;
    const std::size_t got = std::fread(raw, ba, static_cast<std::size_t>(n), file_);
    unpack_samples(raw, got * info_.fmt.channels, info_.fmt.format, out + done * info_.fmt.channels);
    done += got;
    if (got < n) break;
  }
  return done;
}

}  // namespace deckhand::capture
