#include "deckhand/app/cue_suggest.hpp"
#include "deckhand/app/automix.hpp"

#include <algorithm>
#include <cmath>

#include "deckhand/library/palette.hpp"
#include "deckhand/services/track_analysis.hpp"

namespace deckhand {

namespace {
constexpr double kSameCueMs = 30.0;
constexpr int kHotSlots = 8;
constexpr std::size_t kHabitTrackCap = 400;
}  // namespace

CueHabit cue_habit(Library& lib, HabitCache& cache) {
  std::int64_t tracks = 0, cues = 0;
  double sum = 0;
  {
    Stmt s(lib.db(), "SELECT count(DISTINCT p.track_id), count(c.track_id), coalesce(sum(c.start_ms), 0)"
                     " FROM track_phrases p JOIN cues c ON c.track_id = p.track_id");
    if (s.step()) {
      tracks = s.i64(0);
      cues = s.i64(1);
      sum = s.f64(2);
    }
  }
  std::lock_guard<std::mutex> lock(cache.m);
  if (cache.stamp_tracks == tracks && cache.stamp_cues == cues && std::fabs(cache.stamp_sum - sum) < 0.5) return cache.habit;
  std::vector<HabitSample> samples;
  for (const std::int64_t id : lib.tracks_with_phrases()) {
    if (samples.size() >= kHabitTrackCap) break;
    const std::vector<Cue> cs = lib.user_cues(id);  // suggestions nobody changed are not the user's habit
    if (cs.empty()) continue;
    const auto sp = lib.phrases(id);
    if (!sp) continue;
    HabitSample h;
    h.phrases = from_stored_phrases(*sp);
    for (const Cue& c : cs) h.cue_ms.push_back(c.start_ms);
    samples.push_back(std::move(h));
  }
  cache.habit = build_cue_habit(samples);
  cache.stamp_tracks = tracks;
  cache.stamp_cues = cues;
  cache.stamp_sum = sum;
  return cache.habit;
}

SuggestResult suggest_for_track(Library& lib, std::int64_t track_id, const SuggestOptions& o, HabitCache* cache) {
  SuggestResult r;
  const auto sp = lib.phrases(track_id);
  if (!sp || sp->sections.empty()) {
    r.note = "no phrase analysis for this track yet (it is analysed when it is loaded or re-analysed)";
    return r;
  }
  r.has_phrases = true;
  const PhraseAnalysis p = from_stored_phrases(*sp);
  const TrackCues tc = lib.track_cues(track_id);
  const auto row = lib.track(track_id);
  const double first = tc.first_sound_ms.value_or(p.sections.front().start_ms);
  double last = tc.last_sound_ms.value_or(0);
  if (last <= first) last = row ? static_cast<double>(row->duration_ms) : p.sections.back().end_ms;
  if (last <= first) last = p.sections.back().end_ms;
  CueOptions co;
  co.max_hot_cues = std::clamp(o.max_hot, 0, kHotSlots);
  co.mix_out_bars = o.mix_out_bars;
  co.memory_cues = o.memory;
  if (o.habit && cache) {
    co.use_habit = true;
    co.habit = cue_habit(lib, *cache);
    r.habit_samples = co.habit.samples;
  }
  r.items = suggest_cues(p, first, last, co);
  if (r.items.empty()) r.note = "the analysis found nothing to suggest";
  return r;
}

ApplyResult apply_suggestions(Library& lib, std::int64_t track_id, const SuggestOptions& o, HabitCache* cache, bool dry_run) {
  ApplyResult out;
  out.track_id = track_id;
  SuggestOptions so = o;
  so.max_hot = std::clamp(o.max_hot, 0, kHotSlots);
  const SuggestResult sr = suggest_for_track(lib, track_id, so, cache);
  out.has_phrases = sr.has_phrases;
  out.note = sr.note;
  if (sr.items.empty()) return out;
  std::vector<Cue> cues = lib.cues(track_id);
  bool used[kHotSlots] = {};
  for (const Cue& c : cues)
    if (c.kind != CueKind::Memory && c.slot >= 0 && c.slot < kHotSlots) used[c.slot] = true;
  const auto near_existing = [&](double ms, bool memory) {
    for (const Cue& c : cues)
      if ((c.kind == CueKind::Memory) == memory && std::fabs(c.start_ms - ms) < kSameCueMs) return true;
    return false;
  };
  std::vector<Cue> added;
  for (const CueSuggestion& s : sr.items) {
    if (s.memory && !o.memory) continue;
    if (near_existing(s.position_ms, s.memory)) {
      out.skipped.push_back({s, "cue_exists"});
      continue;
    }
    Cue c;
    c.start_ms = s.position_ms;
    c.label = s.name;
    c.color = static_cast<int>(kPalette[std::clamp(s.color, 0, kPaletteSize - 1)]);
    AppliedCue a;
    a.suggestion = s;
    if (s.memory) {
      c.kind = CueKind::Memory;
      c.slot = -1;
    } else {
      int slot = -1;
      for (int i = 0; i < kHotSlots; ++i)
        if (!used[i]) {
          slot = i;
          break;
        }
      if (slot < 0) {
        out.skipped.push_back({s, "no_free_slot"});
        continue;
      }
      used[slot] = true;
      c.kind = CueKind::Hot;
      c.slot = slot;
      a.slot = slot;
    }
    cues.push_back(c);
    added.push_back(c);
    out.written.push_back(std::move(a));
  }
  if (!dry_run && !added.empty()) {
    lib.mark_suggested_cues(track_id, added);  // first: a habit summary built in between must already see them as suggested
    lib.replace_all_cues(track_id, cues);
  }
  return out;
}

PhrasePoints phrase_mix_points(const StoredPhrases& phrases, double first_sound_ms, double last_sound_ms, int mix_out_bars) {
  PhrasePoints out;
  CueOptions o;
  o.max_hot_cues = 8;
  o.memory_cues = false;
  o.mix_out_bars = mix_out_bars;
  for (const CueSuggestion& c : suggest_cues(from_stored_phrases(phrases), first_sound_ms, last_sound_ms, o)) {
    if (c.type == "mix_in" && !out.mix_in_ms) out.mix_in_ms = c.position_ms;
    if (c.type == "mix_out" && !out.mix_out_ms) out.mix_out_ms = c.position_ms;
  }
  return out;
}

}  // namespace deckhand
