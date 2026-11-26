#include "deckhand/lighting/music.hpp"

#include <algorithm>

namespace deckhand::lighting {

const char* section_name(Section s) {
  static const char* n[] = {"unknown", "intro", "build", "drop", "breakdown", "verse", "chorus", "bridge", "outro"};
  return n[static_cast<int>(s)];
}

Section section_from_phrase(const std::string& label, double conf, const std::string& kind) {
  if (conf >= 0.3) {
    for (int i = 1; i <= static_cast<int>(Section::Outro); ++i)
      if (label == section_name(static_cast<Section>(i))) return static_cast<Section>(i);
  }
  if (kind == "intro") return Section::Intro;
  if (kind == "up") return Section::Build;
  if (kind == "down") return Section::Breakdown;
  if (kind == "chorus") return Section::Chorus;
  if (kind == "outro") return Section::Outro;
  return Section::Unknown;
}

bool camelot_of_key(int k, int* number, bool* minor) {
  if (k < 0 || k > 23) return false;
  const bool mi = k >= 12;
  const int major_tonic = mi ? (k % 12 + 3) % 12 : k;  // the same rule as analysis::key_wheel
  if (number) *number = ((major_tonic * 7) % 12 + 7) % 12 + 1;
  if (minor) *minor = mi;
  return true;
}

void TrackTimeline::set_track(const std::vector<double>& beats_ms, int downbeat_phase, const std::vector<deckhand::PhraseSection>& sections) {
  beats_ = beats_ms;
  phase_ = ((downbeat_phase % 4) + 4) % 4;
  secs_.clear();
  for (const auto& s : sections)
    secs_.push_back({s.start_ms, s.end_ms, section_from_phrase(s.label, s.label_confidence, s.kind), std::clamp(s.energy, 1, 10)});
}

void TrackTimeline::clear() { beats_.clear(); secs_.clear(); }

void TrackTimeline::locate(double t, double speed, MusicState* o) const {
  speed = speed > 0.01 ? speed : 1.0;
  o->downbeat_phase = phase_;
  if (beats_.size() >= 2) {
    auto it = std::upper_bound(beats_.begin(), beats_.end(), t);
    std::size_t i = it == beats_.begin() ? 0 : static_cast<std::size_t>(it - beats_.begin()) - 1;
    if (i + 1 >= beats_.size()) i = beats_.size() - 2;
    const double iv = std::max(beats_[i + 1] - beats_[i], 1.0);
    o->beat_pos = static_cast<double>(i) + (t - beats_[i]) / iv;
    o->bpm = 60000.0 / iv * speed;
  } else {
    o->beat_pos = 0;
    o->bpm = 0;
  }
  o->section_index = -1;
  o->section = Section::Unknown;
  o->has_next = false;
  o->time_to_boundary_s = -1;
  o->section_elapsed_s = 0;
  if (secs_.empty()) return;
  std::size_t i = 0;
  while (i + 1 < secs_.size() && t >= secs_[i].end_ms) ++i;
  const Sec& s = secs_[i];
  o->section_index = static_cast<int>(i);
  o->section = s.section;
  o->energy = s.energy;
  o->section_elapsed_s = std::max(0.0, (t - s.start_ms) / 1000.0 / speed);
  o->time_to_boundary_s = std::max(0.0, (s.end_ms - t) / 1000.0 / speed);
  if (i + 1 < secs_.size()) {
    o->has_next = true;
    o->next_section = secs_[i + 1].section;
    o->next_energy = secs_[i + 1].energy;
  }
}

}  // namespace deckhand::lighting
