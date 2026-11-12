// ANLZ analysis files (DAT / EXT / 2EX). Big-endian. Layout per the public DJ Link Ecosystem Analysis
// "ANLZ files" page; guessed fields are listed in docs/EXPORT.md.
#include <algorithm>
#include <cmath>

#include "anlz_writer.hpp"

namespace deckhand::usbexport::detail {
namespace {

void begin_tag(Bytes& b, const char* cc, std::uint32_t len_header) {
  for (int i = 0; i < 4; ++i) put_u8(b, static_cast<unsigned char>(cc[i]));
  put_be32(b, len_header);
  put_be32(b, 0);  // len_tag, patched by end_tag
}

// Patch len_tag of the tag that started at `start`.
void end_tag(Bytes& b, std::size_t start) { set_be32(b, start + 8, static_cast<std::uint32_t>(b.size() - start)); }

void put_utf16be_z(Bytes& b, const std::string& s) {
  for (char16_t c : utf8_to_utf16(s)) put_be16(b, c);
  put_be16(b, 0);
}

Bytes file_header_and_body(const Bytes& body) {
  Bytes b;
  for (char c : {'P', 'M', 'A', 'I'}) put_u8(b, static_cast<unsigned char>(c));
  put_be32(b, 0x1C);
  put_be32(b, static_cast<std::uint32_t>(0x1C + body.size()));
  put_be32(b, 1);  // guessed, see docs
  put_be32(b, 0x10000);
  put_be32(b, 0x10000);
  put_be32(b, 0);
  b.insert(b.end(), body.begin(), body.end());
  return b;
}

// Max-per-band resample of the 150/s waveform to `cols` columns.
std::vector<WaveSample> resample(const std::vector<WaveSample>& w, std::size_t cols) {
  std::vector<WaveSample> out(cols);
  const std::size_t n = w.size();
  if (n == 0) return out;
  for (std::size_t c = 0; c < cols; ++c) {
    std::size_t a = c * n / cols;
    std::size_t e = std::max((c + 1) * n / cols, a + 1);
    if (a >= n) a = n - 1;
    e = std::min(e, n);
    WaveSample m;
    for (std::size_t i = a; i < e; ++i) {
      m.low = std::max(m.low, w[i].low);
      m.mid = std::max(m.mid, w[i].mid);
      m.high = std::max(m.high, w[i].high);
    }
    out[c] = m;
  }
  return out;
}

unsigned peak(const WaveSample& s) { return std::max({s.low, s.mid, s.high}); }
unsigned height5(const WaveSample& s) { return (peak(s) * 31 + 127) / 255; }
unsigned height4(const WaveSample& s) { return (peak(s) * 15 + 127) / 255; }
// 3 bit "whiteness": share of the high band (guess; higher = less saturated blue).
unsigned whiteness3(const WaveSample& s) {
  const unsigned total = s.low + s.mid + s.high;
  return total == 0 ? 0 : (7u * s.high + total / 2) / total;
}
std::uint8_t pwav_byte(const WaveSample& s) { return static_cast<std::uint8_t>((whiteness3(s) << 5) | height5(s)); }

std::vector<WaveSample> detail_waveform(const ExportTrack& t) {
  if (!t.waveform.empty()) return t.waveform;
  return std::vector<WaveSample>(static_cast<std::size_t>(std::max<std::int64_t>(0, t.duration_ms) * 150 / 1000));
}

void tag_ppth(Bytes& b, const std::string& path) {
  const std::size_t s = b.size();
  begin_tag(b, "PPTH", 0x10);
  put_be32(b, static_cast<std::uint32_t>((utf8_to_utf16(path).size() + 1) * 2));
  put_utf16be_z(b, path);
  end_tag(b, s);
}

void tag_pvbr(Bytes& b, std::uint64_t file_size) {
  const std::size_t s = b.size();
  begin_tag(b, "PVBR", 0x10);
  put_be32(b, 0);  // unknown1
  // 400 seek entries; guessed as a linear byte-offset estimate (exact for constant bitrate).
  for (std::uint64_t i = 0; i < 400; ++i) put_be32(b, static_cast<std::uint32_t>(file_size * i / 400));
  end_tag(b, s);
}

void tag_pqtz(Bytes& b, const std::vector<ExportBeat>& beats) {
  const std::size_t s = b.size();
  begin_tag(b, "PQTZ", 0x18);
  put_be32(b, 0);
  put_be32(b, 0x00800000);
  put_be32(b, static_cast<std::uint32_t>(beats.size()));
  for (const auto& e : beats) {
    put_be16(b, static_cast<std::uint32_t>(std::clamp(e.beat_in_bar, 1, 4)));
    put_be16(b, static_cast<std::uint32_t>(std::clamp(std::lround(e.bpm * 100.0), 0L, 65535L)));
    put_be32(b, static_cast<std::uint32_t>(std::max<std::int64_t>(0, e.time_ms)));
  }
  end_tag(b, s);
}

void tag_pwav(Bytes& b, const std::vector<WaveSample>& w) {
  const std::size_t s = b.size();
  begin_tag(b, "PWAV", 0x14);
  put_be32(b, 400);
  put_be32(b, 0x00100000);
  for (const auto& c : resample(w, 400)) put_u8(b, pwav_byte(c));
  end_tag(b, s);
}

void tag_pwv2(Bytes& b, const std::vector<WaveSample>& w) {
  const std::size_t s = b.size();
  begin_tag(b, "PWV2", 0x14);
  put_be32(b, 100);
  put_be32(b, 0x00100000);
  for (const auto& c : resample(w, 100)) put_u8(b, height4(c));
  end_tag(b, s);
}

void tag_pcob(Bytes& b, bool hot, const std::vector<const ExportCue*>& cues) {
  const std::size_t s = b.size();
  begin_tag(b, "PCOB", 0x18);
  put_be32(b, hot ? 1 : 0);
  put_be16(b, 0);
  put_be16(b, static_cast<std::uint32_t>(cues.size()));
  put_be32(b, static_cast<std::uint32_t>(cues.size()));  // memory_count: meaning unknown, guessed
  for (std::size_t i = 0; i < cues.size(); ++i) {
    const ExportCue& c = *cues[i];
    const bool loop = c.loop_end_ms >= 0;
    const std::size_t e = b.size();
    begin_tag(b, "PCPT", 0x1C);
    set_be32(b, e + 8, 0x38);
    put_be32(b, hot ? static_cast<std::uint32_t>(c.slot) : 0);
    put_be32(b, 1);  // status: 1 = enabled (doc says 0 regular / 4 active loop; see docs/EXPORT.md)
    put_be32(b, 0x00100000);
    put_be16(b, i == 0 ? 0xFFFF : (i == 1 ? 0 : static_cast<std::uint32_t>(i)));
    put_be16(b, i + 1 == cues.size() ? 0xFFFF : static_cast<std::uint32_t>(i + 1));
    put_u8(b, loop ? 2 : 1);
    put_u8(b, 0);
    put_u8(b, 0x03);
    put_u8(b, 0xE8);
    put_be32(b, static_cast<std::uint32_t>(std::max<std::int64_t>(0, c.time_ms)));
    put_be32(b, loop ? static_cast<std::uint32_t>(c.loop_end_ms) : 0xFFFFFFFFu);
    while (b.size() < e + 0x38) put_u8(b, 0);
  }
  end_tag(b, s);
}

void tag_pco2(Bytes& b, bool hot, const std::vector<const ExportCue*>& cues) {
  const std::size_t s = b.size();
  begin_tag(b, "PCO2", 0x14);
  put_be32(b, hot ? 1 : 0);
  put_be16(b, static_cast<std::uint32_t>(cues.size()));
  put_be16(b, 0);
  for (const ExportCue* cp : cues) {
    const ExportCue& c = *cp;
    const bool loop = c.loop_end_ms >= 0;
    const std::size_t e = b.size();
    begin_tag(b, "PCP2", 0x10);
    put_be32(b, hot ? static_cast<std::uint32_t>(c.slot) : 0);
    put_u8(b, loop ? 2 : 1);
    put_u8(b, 0);
    put_u8(b, 0x03);
    put_u8(b, 0xE8);
    put_be32(b, static_cast<std::uint32_t>(std::max<std::int64_t>(0, c.time_ms)));
    put_be32(b, loop ? static_cast<std::uint32_t>(c.loop_end_ms) : 0xFFFFFFFFu);
    put_u8(b, hot ? 0 : c.color_id);
    put_u8(b, 1);
    for (int i = 0; i < 6; ++i) put_u8(b, 0);
    put_be16(b, 0);  // loop numerator (not quantized)
    put_be16(b, 0);  // loop denominator
    std::u16string lab = utf8_to_utf16(c.label);
    if (lab.size() > 120) lab.resize(120);
    put_be32(b, lab.empty() ? 0 : static_cast<std::uint32_t>((lab.size() + 1) * 2));
    if (!lab.empty()) {
      for (char16_t ch : lab) put_be16(b, ch);
      put_be16(b, 0);
    }
    // Hot cue colour: code (0 = default), r, g, b. Palette code mapping is not documented here.
    put_u8(b, 0);
    put_u8(b, c.has_rgb ? c.r : 0);
    put_u8(b, c.has_rgb ? c.g : 0);
    put_u8(b, c.has_rgb ? c.b : 0);
    for (int i = 0; i < 8; ++i) put_u8(b, 0);  // padding to the 0x38 minimum seen in practice (guess)
    set_be32(b, e + 8, static_cast<std::uint32_t>(b.size() - e));
  }
  end_tag(b, s);
}

void tag_pwv3(Bytes& b, const std::vector<WaveSample>& w) {
  const std::size_t s = b.size();
  begin_tag(b, "PWV3", 0x18);
  put_be32(b, 1);
  put_be32(b, static_cast<std::uint32_t>(w.size()));
  put_be32(b, 0x00960000);
  for (const auto& c : w) put_u8(b, pwav_byte(c));
  end_tag(b, s);
}

void tag_pwv4(Bytes& b, const std::vector<WaveSample>& w) {
  const std::size_t s = b.size();
  begin_tag(b, "PWV4", 0x18);
  put_be32(b, 6);
  put_be32(b, 1200);
  put_be32(b, 0);
  // Entry layout is NOT documented on the analysis page; this is a guess (see docs/EXPORT.md).
  for (const auto& c : resample(w, 1200)) {
    put_u8(b, 0);
    put_u8(b, 0);
    put_u8(b, c.low);
    put_u8(b, c.mid);
    put_u8(b, c.high);
    put_u8(b, peak(c));
  }
  end_tag(b, s);
}

void tag_pwv5(Bytes& b, const std::vector<WaveSample>& w) {
  const std::size_t s = b.size();
  begin_tag(b, "PWV5", 0x18);
  put_be32(b, 2);
  put_be32(b, static_cast<std::uint32_t>(w.size()));
  put_be32(b, 0x00960305);
  for (const auto& c : w) {
    const unsigned pk = peak(c);
    auto ch = [&](unsigned v) { return pk == 0 ? 0u : (7u * v + pk / 2) / pk; };
    const unsigned word = (ch(c.low) << 13) | (ch(c.mid) << 10) | (ch(c.high) << 7) | (height5(c) << 2);
    put_be16(b, word);
  }
  end_tag(b, s);
}

void tag_pwv6(Bytes& b, const std::vector<WaveSample>& w) {
  const std::size_t s = b.size();
  begin_tag(b, "PWV6", 0x14);
  put_be32(b, 3);
  put_be32(b, 1200);
  for (const auto& c : resample(w, 1200)) {
    put_u8(b, c.mid);
    put_u8(b, c.high);
    put_u8(b, c.low);
  }
  end_tag(b, s);
}

void tag_pwv7(Bytes& b, const std::vector<WaveSample>& w) {
  const std::size_t s = b.size();
  begin_tag(b, "PWV7", 0x18);
  put_be32(b, 3);
  put_be32(b, static_cast<std::uint32_t>(w.size()));
  put_be32(b, 0x00960000);
  for (const auto& c : w) {
    put_u8(b, c.mid);
    put_u8(b, c.high);
    put_u8(b, c.low);
  }
  end_tag(b, s);
}

}  // namespace

AnlzFiles build_anlz(const ExportTrack& t, const std::string& audio_db_path, std::uint64_t file_size,
                     const ExportOptions& opt) {
  const std::vector<WaveSample> detail = detail_waveform(t);
  std::vector<const ExportCue*> mem, hot;
  for (const auto& c : t.cues) (c.hot ? hot : mem).push_back(&c);
  auto by_time = [](const ExportCue* a, const ExportCue* b) { return a->time_ms < b->time_ms; };
  std::stable_sort(mem.begin(), mem.end(), by_time);
  std::stable_sort(hot.begin(), hot.end(), [](const ExportCue* a, const ExportCue* b) { return a->slot < b->slot; });

  AnlzFiles f;
  Bytes body;
  tag_ppth(body, audio_db_path);
  tag_pvbr(body, file_size);
  tag_pqtz(body, t.beats);
  tag_pwav(body, detail);
  tag_pwv2(body, detail);
  tag_pcob(body, false, mem);
  tag_pcob(body, true, hot);
  f.dat = file_header_and_body(body);

  body.clear();
  tag_ppth(body, audio_db_path);
  tag_pwv3(body, detail);
  tag_pco2(body, false, mem);
  tag_pco2(body, true, hot);
  if (opt.write_color_preview) tag_pwv4(body, detail);
  tag_pwv5(body, detail);
  f.ext = file_header_and_body(body);

  if (opt.write_3band) {
    body.clear();
    tag_ppth(body, audio_db_path);
    tag_pwv7(body, detail);
    tag_pwv6(body, detail);
    f.ex2 = file_header_and_body(body);
  }
  return f;
}

}  // namespace deckhand::usbexport::detail
