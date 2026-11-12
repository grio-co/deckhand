#include "format.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace deckhand::sync::detail {
namespace {

constexpr char kMagic[4] = {'D', 'H', 'L', 'G'};
constexpr std::uint32_t kMaxFrame = 16u << 20;

const std::uint32_t* crc_table() {
  static std::uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  return table;
}

void put_varint(std::string& o, std::uint64_t v) {
  while (v >= 0x80) {
    o.push_back(static_cast<char>(v | 0x80));
    v >>= 7;
  }
  o.push_back(static_cast<char>(v));
}
void put_zigzag(std::string& o, std::int64_t v) {
  put_varint(o, (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63));
}
void put_str(std::string& o, const std::string& s) {
  put_varint(o, s.size());
  o += s;
}
void put_u32(std::string& o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(static_cast<char>(v >> (8 * i)));
}
void put_f64(std::string& o, double d) {
  std::uint64_t b;
  std::memcpy(&b, &d, 8);
  for (int i = 0; i < 8; ++i) o.push_back(static_cast<char>(b >> (8 * i)));
}

struct Reader {
  const unsigned char* p;
  const unsigned char* e;
  bool ok = true;
  Reader(const std::string& s) : p(reinterpret_cast<const unsigned char*>(s.data())), e(p + s.size()) {}
  std::uint8_t u8() {
    if (p >= e) { ok = false; return 0; }
    return *p++;
  }
  std::uint64_t varint() {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      const std::uint8_t b = u8();
      if (!ok) return 0;
      v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
    }
    ok = false;
    return 0;
  }
  std::int64_t zigzag() {
    const std::uint64_t v = varint();
    return static_cast<std::int64_t>((v >> 1) ^ (~(v & 1) + 1));
  }
  std::string str() {
    const std::uint64_t n = varint();
    if (!ok || n > static_cast<std::uint64_t>(e - p)) { ok = false; return {}; }
    std::string s(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n));
    p += n;
    return s;
  }
  double f64() {
    if (e - p < 8) { ok = false; return 0; }
    std::uint64_t b = 0;
    for (int i = 0; i < 8; ++i) b |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    p += 8;
    double d;
    std::memcpy(&d, &b, 8);
    return d;
  }
};

std::uint32_t get_u32(const unsigned char* p) {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
         static_cast<std::uint32_t>(p[3]) << 24;
}

bool decode_header(const std::string& payload, FileHeader& h) {
  Reader r(payload);
  h.device_id = r.str();
  h.device_name = r.str();
  h.seq = r.varint();
  h.created_ms = r.zigzag();
  return r.ok && !h.device_id.empty();
}

bool decode_reg(const std::string& payload, RegRec& rec) {
  Reader r(payload);
  rec.kind = static_cast<char>(r.u8());
  rec.eid = r.str();
  rec.field = r.str();
  rec.st.wall = r.zigzag();
  rec.st.ctr = static_cast<std::uint32_t>(r.varint());
  rec.st.dev = r.str();
  const std::uint8_t t = r.u8();
  switch (t) {
    case Val::Null: rec.v = Val::null(); break;
    case Val::Int: rec.v = Val::integer(r.zigzag()); break;
    case Val::Real: rec.v = Val::real(r.f64()); break;
    case Val::Text: rec.v = Val::text(r.str()); break;
    default: return false;
  }
  return r.ok && !rec.st.dev.empty();
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t n, std::uint32_t seed) {
  const std::uint32_t* t = crc_table();
  std::uint32_t c = ~seed;
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

void Hash128::add(const void* data, std::size_t n) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    a_ = (a_ ^ p[i]) * 0x100000001b3ull;
    b_ = (b_ + p[i] + 1) * 0x9E3779B97F4A7C15ull;
    b_ ^= b_ >> 29;
  }
}

std::string Hash128::hex() const {
  const auto mix = [](std::uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    return x ^ (x >> 31);
  };
  char buf[33];
  std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(mix(a_)),
                static_cast<unsigned long long>(mix(b_ ^ a_)));
  return buf;
}

FileWriter::FileWriter(const FileHeader& h) {
  buf_.append(kMagic, 4);
  buf_.push_back(static_cast<char>(kFormatVersion));
  buf_.push_back(static_cast<char>(h.kind));
  buf_.push_back(static_cast<char>(0));  // flags: no encrypted payloads written by this version
  buf_.push_back(static_cast<char>(0));
  std::string p;
  put_str(p, h.device_id);
  put_str(p, h.device_name);
  put_varint(p, h.seq);
  put_zigzag(p, h.created_ms);
  frame(kRecHeader, p);
}

void FileWriter::frame(std::uint8_t type, const std::string& payload) {
  put_u32(buf_, static_cast<std::uint32_t>(payload.size() + 1));
  const std::size_t start = buf_.size();
  buf_.push_back(static_cast<char>(type));
  buf_ += payload;
  put_u32(buf_, crc32(buf_.data() + start, buf_.size() - start));
}

void FileWriter::add(const RegRec& r) {
  std::string p;
  p.push_back(r.kind);
  put_str(p, r.eid);
  put_str(p, r.field);
  put_zigzag(p, r.st.wall);
  put_varint(p, r.st.ctr);
  put_str(p, r.st.dev);
  p.push_back(static_cast<char>(r.v.t));
  switch (r.v.t) {
    case Val::Null: break;
    case Val::Int: put_zigzag(p, r.v.i); break;
    case Val::Real: put_f64(p, r.v.d); break;
    case Val::Text: put_str(p, r.v.s); break;
  }
  frame(kRecReg, p);
  ++count_;
}

std::string FileWriter::finish() {
  std::string p;
  put_varint(p, count_);
  put_u32(p, crc32(buf_.data(), buf_.size()));
  frame(kRecTrailer, p);
  return std::move(buf_);
}

bool parse_header(const std::string& s, FileHeader& out) {
  if (s.size() < 8 + 4 || std::memcmp(s.data(), kMagic, 4) != 0) return false;
  const auto* d = reinterpret_cast<const unsigned char*>(s.data());
  out.kind = d[5];
  const std::uint32_t len = get_u32(d + 8);
  if (len < 1 || len > kMaxFrame || s.size() < 12u + len + 4u || d[12] != kRecHeader) return false;
  if (crc32(d + 12, len) != get_u32(d + 12 + len)) return false;
  return decode_header(std::string(reinterpret_cast<const char*>(d + 13), len - 1), out);
}

ParsedFile parse_file(const std::string& bytes) {
  ParsedFile f;
  if (bytes.size() < 8 || std::memcmp(bytes.data(), kMagic, 4) != 0) {
    f.problem = "not a DeckHand sync file";
    return f;
  }
  const auto* d = reinterpret_cast<const unsigned char*>(bytes.data());
  if (d[4] != kFormatVersion) {
    f.unsupported = true;
    f.problem = "written by a newer DeckHand (format version " + std::to_string(d[4]) + ")";
    return f;
  }
  const bool may_encrypt = (d[6] & kFlagMayBeEncrypted) != 0;
  std::size_t pos = 8;
  while (pos < bytes.size()) {
    if (bytes.size() - pos < 4) { f.problem = "truncated record"; break; }
    const std::uint32_t len = get_u32(d + pos);
    if (len < 1 || len > kMaxFrame || bytes.size() - pos < 4u + len + 4u) { f.problem = "truncated record"; break; }
    const unsigned char* body = d + pos + 4;
    if (crc32(body, len) != get_u32(body + len)) { f.problem = "record checksum mismatch"; break; }
    const std::uint8_t type = body[0];
    const std::string payload(reinterpret_cast<const char*>(body + 1), len - 1);
    if (type == kRecHeader) {
      if (!decode_header(payload, f.header)) { f.problem = "bad header"; break; }
      f.header.kind = d[5];
      f.header_ok = true;
    } else if (!f.header_ok) {
      f.problem = "missing header";
      break;
    } else if (type == kRecReg) {
      RegRec r;
      if (decode_reg(payload, r)) {
        f.regs.push_back(std::move(r));
      } else {
        ++f.unknown_records;  // well-framed but unreadable: skip, keep going
      }
    } else if (type == kRecTrailer) {
      Reader r(payload);
      const std::uint64_t n = r.varint();
      if (!r.ok || r.e - r.p < 4) { f.problem = "bad trailer"; break; }
      const std::uint32_t crc = get_u32(r.p);
      if (n != f.regs.size() + f.unknown_records) { f.problem = "record count mismatch"; break; }
      if (crc != crc32(d, pos)) { f.problem = "file checksum mismatch"; break; }
      f.complete = true;
      return f;
    } else if (type == kRecEncrypted && may_encrypt) {
      ++f.encrypted_records;  // reserved hook: payload decryption is not implemented in this version
    } else {
      ++f.unknown_records;
    }
    pos += 4u + len + 4u;
  }
  if (f.problem.empty() && !f.complete) f.problem = "missing trailer (file incomplete)";
  return f;
}

bool read_file(const std::string& path, std::string& out, std::size_t max_bytes) {
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const auto n = in.tellg();
  if (n < 0 || static_cast<std::size_t>(n) > max_bytes) return false;
  in.seekg(0);
  out.resize(static_cast<std::size_t>(n));
  in.read(out.data(), static_cast<std::streamsize>(n));
  return static_cast<bool>(in) || n == 0;
}

// Platform specifics live here: flush the data to disk, atomically replace the target, flush the directory.
bool write_file_atomic(const std::string& path, const std::string& bytes, std::string& error) {
  const fs::path target(path);
  const fs::path tmp = target.parent_path() / ("." + target.filename().string() + ".tmp");
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
  if (!f) {
    error = "cannot create " + tmp.string();
    return false;
  }
  bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  ok = ok && std::fflush(f) == 0;
#ifdef _WIN32
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && ::fsync(::fileno(f)) == 0;
#endif
  ok = std::fclose(f) == 0 && ok;
  if (!ok) {
    error = "cannot write " + tmp.string();
    fs::remove(tmp, ec);
    return false;
  }
  fs::rename(tmp, target, ec);
  if (ec) {
    error = "cannot rename into place: " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
#ifndef _WIN32
  const int dfd = ::open(target.parent_path().string().c_str(), O_RDONLY);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
#endif
  return true;
}

}  // namespace deckhand::sync::detail
