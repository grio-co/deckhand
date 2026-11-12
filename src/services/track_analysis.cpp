#include "deckhand/services/track_analysis.hpp"

#include <cmath>

#include "deckhand/analysis/analysis.hpp"

namespace deckhand {

TrackAnalysis analyse_track(const TrackAudio& audio) {
  TrackAnalysis out;
  if (audio.frames == 0 || audio.pcm.empty()) return out;
  const AnalysisResult r = analyse(audio.pcm.data(), audio.frames, audio.sample_rate);
  const double length_ms = static_cast<double>(audio.frames) / audio.sample_rate * 1000.0;

  if (r.beats.ok && !r.beats.sections.empty()) {
    const auto& sections = r.beats.sections;
    // Sections that agree on the tempo are one steady track whose pulse dropped out for a while
    // (a breakdown): keep the longest one and extend it over the whole track.
    bool steady = true;
    std::size_t longest = 0;
    double longest_ms = -1;
    for (std::size_t i = 0; i < sections.size(); ++i) {
      if (std::fabs(sections[i].bpm - r.beats.bpm) > 0.15) steady = false;
      const double span = (i + 1 < sections.size() ? sections[i + 1].first_beat_ms : length_ms) - sections[i].first_beat_ms;
      if (span > longest_ms) {
        longest_ms = span;
        longest = i;
      }
    }
    if (steady) {
      const TempoSection& s = sections[longest];
      const double interval = 60000.0 / s.bpm;
      const long back = static_cast<long>(std::floor(s.first_beat_ms / interval));
      const int bar = static_cast<int>(((s.bar_beat - 1 - back) % 4 + 4) % 4) + 1;
      out.grid.push_back({s.first_beat_ms - static_cast<double>(back) * interval, s.bpm, bar});
    } else {
      for (const TempoSection& s : sections) out.grid.push_back({s.first_beat_ms, s.bpm, s.bar_beat});
    }
    out.bpm = r.beats.bpm;
    out.has_grid = true;
  }
  if (r.key.ok && r.key.confidence >= 0.15) {
    out.key = r.key.wheel;
    out.has_key = true;
  }
  if (r.loudness.ok) {
    out.gain_db = auto_gain_db(r.loudness);
    out.has_gain = true;
  }
  return out;
}

}  // namespace deckhand
