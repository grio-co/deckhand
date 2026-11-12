// Beat grid reader for player analysis files, written from the publicly documented
// layout: a tagged big-endian container whose "PQTZ" section lists every beat as
// (beat-in-bar u16, tempo*100 u16, time-ms u32).
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

#include "deckhand/import/import.hpp"

namespace deckhand {
namespace {

std::uint32_t be32(const unsigned char* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
std::uint16_t be16(const unsigned char* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

}  // namespace

std::vector<BeatSection> read_analysis_beatgrid(const std::string& analysis_file) {
  std::vector<BeatSection> out;
  std::ifstream f(analysis_file, std::ios::binary);
  if (!f) return out;
  const std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 12 || std::memcmp(d.data(), "PMAI", 4) != 0) return out;

  std::size_t pos = be32(&d[4]);  // skip the file header
  while (pos + 12 <= d.size()) {
    const unsigned char* tag = &d[pos];
    const std::size_t header_len = be32(tag + 4);
    const std::size_t tag_len = be32(tag + 8);
    if (tag_len < 12 || pos + tag_len > d.size()) break;
    if (std::memcmp(tag, "PQTZ", 4) == 0 && header_len >= 24 && tag_len >= header_len) {
      const std::size_t count = be32(tag + 20);
      const unsigned char* e = tag + header_len;
      if (header_len + count * 8 > tag_len) break;
      double last_tempo = -1;
      for (std::size_t i = 0; i < count; ++i, e += 8) {
        const int bar_beat = be16(e);
        const double tempo = be16(e + 2) / 100.0;
        const double time_ms = be32(e + 4);
        if (tempo <= 0) continue;
        if (tempo != last_tempo) {
          out.push_back({time_ms, tempo, bar_beat >= 1 && bar_beat <= 4 ? bar_beat : 1});
          last_tempo = tempo;
        }
      }
      break;
    }
    pos += tag_len;
  }
  return out;
}

}  // namespace deckhand
