#include "deckhand/capture/export.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace deckhand::capture {
namespace fs = std::filesystem;
namespace {

void be32(std::string& s, std::uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back(static_cast<char>(v >> (8 * i))); }
void be16(std::string& s, std::uint16_t v) { s.push_back(static_cast<char>(v >> 8)); s.push_back(static_cast<char>(v)); }

// 80-bit IEEE extended for a whole sample rate.
std::string extended80(std::uint32_t rate) {
  int e = 31;
  while (e > 0 && !((rate >> e) & 1u)) --e;
  const std::uint16_t exp = static_cast<std::uint16_t>(16383 + e);
  const std::uint64_t mant = static_cast<std::uint64_t>(rate) << (63 - e);
  std::string s;
  be16(s, exp);
  for (int i = 7; i >= 0; --i) s.push_back(static_cast<char>(mant >> (8 * i)));
  return s;
}

bool write_aiff(std::FILE* f, SampleSource& src, std::uint64_t start, std::uint64_t frames, SampleFormat fmt, const TrackTags* tags, std::string& error) {
  const std::uint32_t ch = src.channels(), bps = bytes_per_sample(fmt);
  const std::uint64_t data = frames * ch * bps;
  const std::string id3 = tags && !tags->empty() ? build_id3_tag(*tags) : std::string();
  const std::uint64_t ssnd = 8 + data, id3_chunk = id3.empty() ? 0 : 8 + id3.size() + (id3.size() & 1);
  const std::uint64_t form = 4 + (8 + 18) + (8 + ssnd + (ssnd & 1)) + id3_chunk;
  if (form > 0xFFFFFFFFull - 8) { error = "too long for AIFF (4 GB limit)"; return false; }
  std::string h = "FORM";
  be32(h, static_cast<std::uint32_t>(form));
  h += "AIFFCOMM";
  be32(h, 18);
  be16(h, static_cast<std::uint16_t>(ch));
  be32(h, static_cast<std::uint32_t>(frames));
  be16(h, static_cast<std::uint16_t>(bps * 8));
  h += extended80(src.sample_rate());
  h += "SSND";
  be32(h, static_cast<std::uint32_t>(ssnd));
  be32(h, 0);
  be32(h, 0);
  if (std::fwrite(h.data(), 1, h.size(), f) != h.size()) { error = "write failed"; return false; }
  constexpr std::uint64_t kChunk = 8192;
  std::vector<float> buf(static_cast<std::size_t>(kChunk * ch));
  std::vector<unsigned char> raw(static_cast<std::size_t>(kChunk * ch * bps));
  for (std::uint64_t done = 0; done < frames;) {
    const std::uint64_t n = src.read(start + done, std::min(kChunk, frames - done), buf.data());
    if (n == 0) { error = "short read from the source"; return false; }
    pack_samples(buf.data(), static_cast<std::size_t>(n * ch), fmt, raw.data());
    for (std::size_t i = 0; i < n * ch; ++i) std::reverse(raw.begin() + static_cast<std::ptrdiff_t>(i * bps), raw.begin() + static_cast<std::ptrdiff_t>((i + 1) * bps));
    if (std::fwrite(raw.data(), 1, static_cast<std::size_t>(n * ch * bps), f) != n * ch * bps) { error = "write failed"; return false; }
    done += n;
  }
  std::string tail;
  if (ssnd & 1) tail.push_back('\0');
  if (!id3.empty()) {
    tail += "ID3 ";
    be32(tail, static_cast<std::uint32_t>(id3.size()));
    tail += id3;
    if (id3.size() & 1) tail.push_back('\0');
  }
  if (!tail.empty() && std::fwrite(tail.data(), 1, tail.size(), f) != tail.size()) { error = "write failed"; return false; }
  return true;
}

}  // namespace

bool flac_available() { return false; }

const char* container_extension(Container c) { return c == Container::Aiff ? "aiff" : c == Container::Flac ? "flac" : "wav"; }

std::string safe_filename(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c < 0x20 || std::strchr("<>:\"/\\|?*", c)) o.push_back(' ');
    else o.push_back(static_cast<char>(c));
  }
  std::string r;
  for (char c : o) if (!(c == ' ' && !r.empty() && r.back() == ' ')) r.push_back(c);
  while (!r.empty() && (r.back() == ' ' || r.back() == '.')) r.pop_back();
  std::size_t lead = 0;
  while (lead < r.size() && r[lead] == ' ') ++lead;
  r.erase(0, lead);
  if (r.size() > 150) r.resize(150);  // keep clear of path limits; cut on a character boundary
  while (!r.empty() && (static_cast<unsigned char>(r.back()) & 0xC0) == 0x80) r.pop_back();
  return r;
}

std::string piece_file_name(const TrackTags& t, int track_number, Container c) {
  char num[16];
  std::snprintf(num, sizeof(num), "%02d", track_number);
  std::string name = num;
  const std::string label = t.title.empty() ? "Track " + std::string(num) : (t.artist.empty() ? t.title : t.artist + " - " + t.title);
  name += " - " + label;
  std::string safe = safe_filename(name);
  return safe + "." + container_extension(c);
}

ExportResult export_range(SampleSource& src, std::uint64_t start, std::uint64_t end, const std::string& path, const ExportSettings& s, const TrackTags* tags) {
  ExportResult r;
  r.path = path;
  end = std::min(end, src.frames());
  if (end <= start) { r.error = "empty range"; return r; }
  r.start = start;
  r.end = end;
  r.frames = end - start;
  if (s.container == Container::Flac) { r.error = "FLAC encoding is not available: no encoder is part of this build"; return r; }
  if (s.container == Container::Aiff && s.format == SampleFormat::Float32) { r.error = "AIFF holds 16 or 24 bit; choose one of those"; return r; }
  std::error_code ec;
  const fs::path p(path);
  if (fs::exists(p, ec) && !s.overwrite) { r.error = "file exists: " + path; return r; }
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  const std::uint32_t ch = src.channels();
  std::vector<float> loaded;
  MemorySource mem(nullptr, 0, ch, src.sample_rate());
  SampleSource* eff = &src;
  std::uint64_t from = start;
  if (s.cleanup.any()) {
    loaded.resize(static_cast<std::size_t>(r.frames * ch));
    if (src.read(start, r.frames, loaded.data()) != r.frames) { r.error = "short read from the source"; return r; }
    r.cleanup = apply_cleanup(loaded.data(), r.frames, ch, src.sample_rate(), s.cleanup);
    mem = MemorySource(loaded.data(), r.frames, ch, src.sample_rate());
    eff = &mem;
    from = 0;
  }
  const std::string tmp = path + ".part";
  std::string err;
  bool ok = false;
  if (s.container == Container::Aiff) {
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { r.error = "could not create " + tmp; return r; }
    ok = write_aiff(f, *eff, from, r.frames, s.format, tags, err);
    ok = std::fclose(f) == 0 && ok;
  } else {
    WavWriter w;
    WavFormat f;
    f.sample_rate = src.sample_rate();
    f.channels = static_cast<std::uint16_t>(ch);
    f.format = s.format;
    if (!w.create(tmp, f, err)) { r.error = err; return r; }
    constexpr std::uint64_t kChunk = 16384;
    std::vector<float> buf(static_cast<std::size_t>(kChunk * ch));
    ok = true;
    for (std::uint64_t done = 0; done < r.frames && ok;) {
      const std::uint64_t n = eff->read(from + done, std::min(kChunk, r.frames - done), buf.data());
      if (n == 0) { err = "short read from the source"; ok = false; break; }
      ok = w.write(buf.data(), n);
      done += n;
    }
    if (!ok && err.empty()) err = "write failed";
    ok = w.close(tags) && ok;
  }
  if (!ok) {
    fs::remove(tmp, ec);
    r.error = err.empty() ? "write failed" : err;
    return r;
  }
  if (s.overwrite) fs::remove(p, ec);
  fs::rename(tmp, p, ec);
  if (ec) { fs::remove(tmp, ec); r.error = "could not move the finished file into place"; return r; }
  r.ok = true;
  return r;
}

std::vector<ExportResult> export_pieces(SampleSource& src, const std::vector<Piece>& pieces, const std::vector<TrackTags>& tags, const std::string& dir, const ExportSettings& s) {
  std::vector<ExportResult> out;
  int total = 0;
  for (const auto& p : pieces) if (!p.empty) ++total;
  int k = 0;
  for (const auto& p : pieces) {
    if (p.empty) continue;
    TrackTags t = static_cast<std::size_t>(k) < tags.size() ? tags[static_cast<std::size_t>(k)] : TrackTags{};
    ++k;
    if (t.track_number == 0) t.track_number = k;
    if (t.track_total == 0) t.track_total = total;
    const fs::path file = fs::path(dir) / piece_file_name(t, t.track_number, s.container);
    out.push_back(export_range(src, p.start, p.end, file.string(), s, &t));
  }
  return out;
}

ExportResult export_edit(SampleSource& src, const std::string& source_path, double start_s, double end_s, const std::string& out_path, const ExportSettings& s, const TrackTags* tags) {
  ExportResult r;
  r.path = out_path;
  std::error_code ec;
  if (!source_path.empty() && fs::exists(out_path, ec) && fs::exists(source_path, ec) && fs::equivalent(out_path, source_path, ec)) {
    r.error = "refusing to overwrite the original track";
    return r;
  }
  const double rate = src.sample_rate();
  const std::uint64_t a = static_cast<std::uint64_t>(std::max(0.0, start_s) * rate + 0.5);
  const std::uint64_t b = end_s > 0 ? static_cast<std::uint64_t>(end_s * rate + 0.5) : src.frames();
  return export_range(src, a, b, out_path, s, tags);
}

}  // namespace deckhand::capture
