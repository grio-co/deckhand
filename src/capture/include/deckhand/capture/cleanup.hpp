#pragma once
// Optional clean-up of a recorded piece, all off by default. Offline, in place, on interleaved float.
// Order: DC removal, rumble high-pass, click repair, loudness normalise, fades.
#include <cstdint>
#include <vector>

namespace deckhand::capture {

struct CleanupSettings {
  bool remove_dc = false;
  bool rumble_filter = false;      // zero-phase 8th-order (4th-order Butterworth forward and back) high-pass
  double rumble_hz = 45.0;
  bool click_repair = false;       // detect pops and crackle and interpolate over them
  double click_threshold = 8.0;    // detection level in robust standard deviations of the third difference
  double click_max_ms = 2.0;       // longer disturbances are left alone (they are music, not clicks)
  bool normalise = false;          // to the integrated loudness below, never past the ceiling
  double target_lufs = -14.0;
  double ceiling_dbfs = -1.0;
  double fade_in_ms = 0;           // short fades at the cuts; 0 = none
  double fade_out_ms = 0;
  bool any() const { return remove_dc || rumble_filter || click_repair || normalise || fade_in_ms > 0 || fade_out_ms > 0; }
};

struct ClickSpan {
  std::uint32_t channel = 0;
  std::uint64_t start = 0, length = 0;  // damaged samples [start, start + length)
};

struct CleanupReport {
  std::uint64_t clicks_repaired = 0, samples_repaired = 0;
  std::vector<double> dc_removed;  // per channel
  double gain_db = 0;              // applied by normalise (0 when off or measurement failed)
  double measured_lufs = -70;
};

std::vector<ClickSpan> detect_clicks(const float* pcm, std::uint64_t frames, std::uint32_t channels, std::uint32_t rate, const CleanupSettings& s);
CleanupReport apply_cleanup(float* pcm, std::uint64_t frames, std::uint32_t channels, std::uint32_t rate, const CleanupSettings& s);

}  // namespace deckhand::capture
