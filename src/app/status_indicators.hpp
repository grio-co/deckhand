#pragma once
// What the status row shows while something is being recorded (vinyl recording from an input, recording of the mix), and
// what closing the application would lose. Pure logic over the services' status structs, so it is tested headless.
#include <cstdint>
#include <cstdio>
#include <string>

#include "deckhand/app/capture_service.hpp"
#include "deckhand/audio/engine.hpp"

namespace deckhand {

// "1:05" or "1:02:03".
inline std::string indicator_clock(double seconds) {
  const long t = seconds > 0 ? static_cast<long>(seconds) : 0;
  char b[32];
  if (t >= 3600) std::snprintf(b, sizeof b, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
  else std::snprintf(b, sizeof b, "%ld:%02ld", t / 60, t % 60);
  return b;
}

// The recording from an analogue input. `active` while it records or is paused; a finished side is no longer "recording".
struct CaptureIndicator {
  bool active = false;
  bool paused = false;
  bool warn = false;           // clipped, or samples dropped: worth a look at the panel
  std::uint64_t clips = 0, dropped = 0;
  std::string clock;           // elapsed time
};

inline CaptureIndicator capture_indicator(const CaptureService::Status& s) {
  CaptureIndicator i;
  i.active = s.state == CaptureService::State::Recording || s.state == CaptureService::State::Paused;
  if (!i.active) return i;
  i.paused = s.state == CaptureService::State::Paused;
  i.clips = s.clips;
  i.dropped = s.dropped;
  i.warn = s.clips > 0 || s.dropped > 0;
  i.clock = indicator_clock(s.seconds);
  return i;
}

// The recording of the master output (the REC button).
struct MixIndicator {
  bool active = false;
  bool warn = false;  // the file could not be written, or frames were dropped
  std::string clock;
};

inline MixIndicator mix_indicator(const RecorderStatus& r) {
  MixIndicator i;
  i.active = r.recording;
  if (!i.active) return i;
  i.warn = r.error || r.dropped_frames > 0;
  i.clock = indicator_clock(r.seconds);
  return i;
}

// What leaving the application now would cut short.
struct QuitRisk {
  bool capture = false;  // a recording from an input is running or paused
  bool mix = false;      // the mix is being recorded
  bool any() const { return capture || mix; }
};

inline QuitRisk quit_risk(const CaptureService::Status& c, const RecorderStatus& r) {
  QuitRisk q;
  q.capture = capture_indicator(c).active;
  q.mix = mix_indicator(r).active;
  return q;
}

}  // namespace deckhand
