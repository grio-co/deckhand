#include "deckhand/services/track_analysis.hpp"

#include <cmath>

#include "deckhand/analysis/analysis.hpp"

namespace deckhand {

StoredPhrases to_stored_phrases(const PhraseAnalysis& p) {
  StoredPhrases s;
  s.version = kPhraseAnalysisVersion;
  s.gridded = p.gridded;
  s.time_based = p.time_based;
  s.origin_ms = p.origin_ms;
  s.pickup_ms = p.pickup_ms;
  s.bar_ms = p.bar_ms;
  s.total_bars = p.total_bars;
  s.bar_starts_ms = p.bar_starts_ms;
  s.bar_energy = p.bar_energy;
  for (const PhraseSection& x : p.sections) {
    StoredPhraseSection o;
    o.start_ms = x.start_ms;
    o.end_ms = x.end_ms;
    o.start_bar = x.start_bar;
    o.end_bar = x.end_bar;
    o.bars = x.bars;
    o.energy = x.energy;
    o.label = x.label;
    o.kind = x.kind;
    o.label_confidence = x.label_confidence;
    o.boundary_confidence = x.boundary_confidence;
    o.phrase_aligned = x.phrase_aligned;
    o.kick = x.kick;
    s.sections.push_back(std::move(o));
  }
  return s;
}

PhraseAnalysis from_stored_phrases(const StoredPhrases& s) {
  PhraseAnalysis p;
  p.ok = !s.sections.empty();
  p.gridded = s.gridded;
  p.time_based = s.time_based;
  p.origin_ms = s.origin_ms;
  p.pickup_ms = s.pickup_ms;
  p.bar_ms = s.bar_ms;
  p.total_bars = s.total_bars;
  p.bar_starts_ms = s.bar_starts_ms;
  p.bar_energy = s.bar_energy;
  for (const StoredPhraseSection& x : s.sections) {
    PhraseSection o;
    o.start_ms = x.start_ms;
    o.end_ms = x.end_ms;
    o.start_bar = x.start_bar;
    o.end_bar = x.end_bar;
    o.bars = x.bars;
    o.energy = x.energy;
    o.label = x.label;
    o.kind = x.kind;
    o.label_confidence = x.label_confidence;
    o.boundary_confidence = x.boundary_confidence;
    o.phrase_aligned = x.phrase_aligned;
    o.kick = x.kick;
    p.sections.push_back(std::move(o));
  }
  return p;
}

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
  out.has_audible = r.has_audible;
  out.first_audible_ms = r.first_audible_ms;
  out.last_audible_ms = r.last_audible_ms;
  out.suggested_cue_ms = r.suggested_cue_ms;
  if (r.phrases.ok && !r.phrases.sections.empty()) {
    out.phrases = to_stored_phrases(r.phrases);
    out.has_phrases = true;
  }
  if (r.loudness.ok) {
    out.gain_db = auto_gain_db(r.loudness);
    out.has_gain = true;
  }
  return out;
}

double initial_cue_ms(Library& lib, std::int64_t track_id, const TrackAudio& audio) {
  TrackCues c = lib.track_cues(track_id);
  if (c.main_cue_ms) return *c.main_cue_ms;
  const std::string mode = lib.setting("load_cue_mode", "first_downbeat");
  if (mode == "file_start") return 0;
  if (!c.first_sound_ms && !c.load_cue_ms && audio.frames > 0 && !audio.pcm.empty()) {
    const AudibleRange r = find_audible_range(audio.pcm.data(), audio.frames, audio.sample_rate);
    if (r.ok) {
      c.first_sound_ms = r.first_ms;
      lib.set_analysis_cues(track_id, std::nullopt, r.first_ms);
    }
  }
  if (mode == "first_sound") return c.first_sound_ms.value_or(0);
  return c.load_cue_ms ? *c.load_cue_ms : c.first_sound_ms.value_or(0);
}

}  // namespace deckhand
