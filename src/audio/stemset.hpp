#pragma once
// Four-stem audio of one loaded track (internal to the audio module), and the Source that the deck's readers use:
// either the plain track PCM or the sum of the stems (each times its gain), optionally blended with the plain PCM.
//
// Stems are held as 16-bit (or, for 24-bit cache files, 32-bit container) integers interleaved per frame:
// [vocals L, vocals R, drums L, drums R, bass L, bass R, other L, other R]. One frame is therefore one 16-byte
// (or 32-byte) read for all four stems, and a deck reads them sequentially like ordinary PCM.
// Memory: 16 bytes per frame = 2.54 MB per second = 42.3 MB per minute per deck at 16 bit (84.7 MB at 24 bit),
// on top of the 21 MB per minute of the float track PCM the deck keeps for the mix.
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp.hpp"

namespace deckhand {

constexpr int kStems = 4;  // vocals, drums, bass, other (the stem cache's canonical order)

struct StemSet {
  std::int64_t track_id = 0;
  int rate = 0;
  std::int64_t frames = 0;
  int bits = 16;
  float lsb = 1.0f / 32768.0f;  // value of one integer step (cache scale included)
  std::vector<std::int16_t> s16;
  std::vector<std::int32_t> s32;
  std::size_t bytes() const { return s16.size() * 2 + s32.size() * 4; }
  // Test / tool helper: quantises float stems (planar, `frames` each) to 16 bit with lsb = 1/32768 (inputs |x| < 1).
  void assign_from_float(const float* const l[kStems], const float* const r[kStems], std::int64_t n, int sample_rate);
};

// Reads a .dhstems cache file (format: src/stems/cache.hpp). Not real-time. False and `error` on failure.
bool stemset_load_file(const std::string& path, StemSet& out, std::string& error);

// What a deck reads its audio through. g[] are the final per-stem multipliers (gain times the integer step); w is the
// share of the stem sum in the output (1 = stems only, 0 = plain PCM only).
struct Source {
  const float* pcm = nullptr;
  std::int64_t frames = 0;
  const StemSet* st = nullptr;
  float g[kStems] = {0, 0, 0, 0};
  float w = 0;

  bool stems() const { return st != nullptr && w > 0.0f; }

  inline void stem_frame(std::int64_t k, float& l, float& r) const {
    if (k < 0 || k >= st->frames) {
      l = r = 0;
      return;
    }
    if (!st->s16.empty()) {
      const std::int16_t* p = &st->s16[static_cast<std::size_t>(k) * 8];
      l = g[0] * p[0] + g[1] * p[2] + g[2] * p[4] + g[3] * p[6];
      r = g[0] * p[1] + g[1] * p[3] + g[2] * p[5] + g[3] * p[7];
    } else {
      const std::int32_t* p = &st->s32[static_cast<std::size_t>(k) * 8];
      l = g[0] * static_cast<float>(p[0]) + g[1] * static_cast<float>(p[2]) + g[2] * static_cast<float>(p[4]) + g[3] * static_cast<float>(p[6]);
      r = g[0] * static_cast<float>(p[1]) + g[1] * static_cast<float>(p[3]) + g[2] * static_cast<float>(p[5]) + g[3] * static_cast<float>(p[7]);
    }
  }
  // Integer frame; silence outside the track.
  inline void frame(std::int64_t k, float& l, float& r) const {
    if (!stems()) {
      if (k < 0 || k >= frames) l = r = 0;
      else l = pcm[k * 2], r = pcm[k * 2 + 1];
      return;
    }
    stem_frame(k, l, r);
    if (w < 1.0f) {
      float ol = 0, orr = 0;
      if (k >= 0 && k < frames) ol = pcm[k * 2], orr = pcm[k * 2 + 1];
      l = ol * (1.0f - w) + l * w;
      r = orr * (1.0f - w) + r * w;
    }
  }
  inline float mono(std::int64_t k) const {
    float l, r;
    frame(k, l, r);
    return 0.5f * (l + r);
  }
  // 4-point Hermite at a fractional position.
  inline void hermite(double pos, float& l, float& r) const {
    if (!stems()) {
      hermite_frame(pcm, frames, pos, l, r);
      return;
    }
    const std::int64_t i = static_cast<std::int64_t>(std::floor(pos));
    if (i < -1 || i >= frames) {
      l = r = 0;
      return;
    }
    const float t = static_cast<float>(pos - static_cast<double>(i));
    float a[4][2];
    for (int j = 0; j < 4; ++j) frame(i - 1 + j, a[j][0], a[j][1]);
    float out[2];
    for (int c = 0; c < 2; ++c) {
      const float xm1 = a[0][c], x0 = a[1][c], x1 = a[2][c], x2 = a[3][c];
      const float c1 = 0.5f * (x1 - xm1);
      const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
      const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
      out[c] = ((c3 * t + c2) * t + c1) * t + x0;
    }
    l = out[0];
    r = out[1];
  }
};

}  // namespace deckhand
