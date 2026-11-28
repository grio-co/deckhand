// Cue suggestions from stored phrases: free slots only, never overwriting, one undoable batch, dry run, the cached habit,
// and the phrase points automix uses as its automatic defaults.
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "check.hpp"
#include "deckhand/app/automix.hpp"
#include "deckhand/app/cue_suggest.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;

namespace {
StoredPhrases make_phrases() {
  StoredPhrases p;
  p.gridded = true;
  p.origin_ms = 500;
  p.bar_ms = 1875;  // 128 BPM
  p.total_bars = 128;
  for (int b = 0; b <= p.total_bars; ++b) p.bar_starts_ms.push_back(p.origin_ms + b * p.bar_ms);
  p.bar_energy.assign(static_cast<std::size_t>(p.total_bars), 0.5f);
  struct S { int a, b, energy; const char* label; const char* kind; };
  const S spec[] = {{0, 16, 5, "intro", "intro"}, {16, 32, 7, "build", "up"},     {32, 64, 9, "drop", "chorus"},
                    {64, 80, 3, "breakdown", "down"}, {80, 112, 9, "drop", "chorus"}, {112, 128, 5, "outro", "outro"}};
  for (const S& s : spec) {
    StoredPhraseSection x;
    x.start_bar = s.a;
    x.end_bar = s.b;
    x.bars = s.b - s.a;
    x.start_ms = p.bar_starts_ms[static_cast<std::size_t>(s.a)];
    x.end_ms = p.bar_starts_ms[static_cast<std::size_t>(s.b)];
    x.energy = s.energy;
    x.label = s.label;
    x.kind = s.kind;
    x.label_confidence = 0.9;
    x.boundary_confidence = 0.9;
    x.phrase_aligned = true;
    x.kick = s.energy >= 5 ? 1.0 : 0.0;
    p.sections.push_back(x);
  }
  return p;
}

std::int64_t add_track(Library& lib, const char* path, bool with_phrases) {
  NewTrack t;
  t.path = path;
  t.title = path;
  t.duration_ms = 250000;
  const std::int64_t id = lib.upsert_track(t);
  if (with_phrases) {
    lib.set_phrases(id, make_phrases());
    lib.set_analysis_cues(id, 500.0, 500.0);
    lib.set_last_sound_ms(id, 500 + 128 * 1875.0);
  }
  return id;
}
}  // namespace

int main() {
  const std::string path = (std::filesystem::temp_directory_path() / "deckhand_test_cue_suggest.db").string();
  for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
  Library lib(path);
  HabitCache cache;
  const std::int64_t a = add_track(lib, "/m/a.mp3", true), none = add_track(lib, "/m/none.mp3", false);

  // Suggestions: ranked, deterministic, none for a track without phrases.
  SuggestOptions o;
  const SuggestResult s1 = suggest_for_track(lib, a, o, &cache);
  CHECK(s1.has_phrases && !s1.items.empty());
  for (std::size_t i = 0; i < s1.items.size(); ++i) CHECK(s1.items[i].rank == static_cast<int>(i) + 1);
  const SuggestResult s2 = suggest_for_track(lib, a, o, &cache);
  CHECK(s1.items.size() == s2.items.size() && s1.items[0].position_ms == s2.items[0].position_ms);
  CHECK(!suggest_for_track(lib, none, o, &cache).has_phrases);
  CHECK(!apply_suggestions(lib, none, o, &cache, false).has_phrases && lib.cues(none).empty());

  // A dry run writes nothing; the real run fills free slots and leaves what is there alone.
  Cue mine;
  mine.kind = CueKind::Hot;
  mine.slot = 0;
  mine.start_ms = 12345;
  mine.label = "mine";
  Cue mine2 = mine;
  mine2.slot = 2;
  mine2.start_ms = 23456;
  lib.replace_all_cues(a, {mine, mine2});
  const ApplyResult dry = apply_suggestions(lib, a, o, &cache, true);
  CHECK(!dry.written.empty() && lib.cues(a).size() == 2);
  std::int64_t batch = 0;
  ApplyResult real;
  {
    EditScope scope("api");
    real = apply_suggestions(lib, a, o, &cache, false);
    batch = scope.batch();
  }
  CHECK(real.written.size() == dry.written.size() && batch != 0);
  const std::vector<Cue> after = lib.cues(a);
  CHECK(after.size() == 2 + real.written.size());
  int kept = 0;
  for (const Cue& c : after) {
    if (c.slot == 0) { CHECK(c.start_ms == 12345 && c.label == "mine"); ++kept; }
    if (c.slot == 2) { CHECK(c.start_ms == 23456 && c.label == "mine"); ++kept; }
  }
  CHECK(kept == 2);
  CHECK(lib.user_cues(a).size() == 2);  // the suggestions are not counted as the user's cues
  for (const AppliedCue& w : real.written) CHECK(w.slot >= 0 && w.slot != 0 && w.slot != 2 && w.slot < 8);
  // Applying again adds nothing (the positions are taken), and never touches the first batch.
  const ApplyResult again = apply_suggestions(lib, a, o, &cache, false);
  CHECK(again.written.empty() && lib.cues(a).size() == after.size());
  // One undo takes the whole batch back, the user's own cues stay.
  CHECK(lib.undo_batch(batch).status == UndoStatus::Ok);
  const std::vector<Cue> undone = lib.cues(a);
  CHECK(undone.size() == 2 && undone[0].label == "mine" && undone[1].label == "mine");

  // All eight slots taken: nothing is written, every suggestion is skipped with its reason.
  std::vector<Cue> full;
  for (int i = 0; i < 8; ++i) {
    Cue c;
    c.kind = CueKind::Hot;
    c.slot = i;
    c.start_ms = 1000.0 + i;
    full.push_back(c);
  }
  lib.replace_all_cues(a, full);
  const ApplyResult none_free = apply_suggestions(lib, a, o, &cache, false);
  CHECK(none_free.written.empty() && !none_free.skipped.empty() && none_free.skipped[0].reason == "no_free_slot" && lib.cues(a).size() == 8);

  // Memory cues only when asked for.
  lib.replace_all_cues(a, {});
  SuggestOptions with_memory;
  with_memory.memory = true;
  with_memory.max_hot = 2;
  const ApplyResult mem = apply_suggestions(lib, a, with_memory, &cache, false);
  int hot = 0, memory = 0;
  for (const AppliedCue& w : mem.written) (w.slot >= 0 ? hot : memory)++;
  CHECK(hot == 2 && memory > 0);

  // The habit summary: built from cued tracks that have phrases, cached, and refreshed when the cues change.
  const CueHabit h0 = cue_habit(lib, cache);
  const int before = h0.samples;
  CHECK(!lib.cues(a).empty() && lib.user_cues(a).empty());
  CHECK(before == 0);  // track a only holds suggestions nobody touched: they are not the user's habit
  const std::int64_t b = add_track(lib, "/m/b.mp3", true);
  Cue c;
  c.kind = CueKind::Hot;
  c.slot = 0;
  c.start_ms = 500 + 28 * 1875.0;  // four bars before the first drop at bar 32
  lib.replace_all_cues(b, {c});
  const CueHabit h1 = cue_habit(lib, cache);
  CHECK(h1.samples > before);
  CHECK(cue_habit(lib, cache).samples == h1.samples);  // cached
  CHECK(h1.samples == 1);
  // Moving a suggested cue makes it the user's own; the others stay suggestions.
  {
    std::vector<Cue> cs = lib.cues(a);
    cs.front().start_ms += 1000;
    lib.replace_all_cues(a, cs);
    CHECK(lib.user_cues(a).size() == 1 && std::fabs(lib.user_cues(a)[0].start_ms - cs.front().start_ms) < 0.01);
    CHECK(cue_habit(lib, cache).samples == 2);
  }

  // Automix's automatic mix points from the phrases.
  const StoredPhrases sp = make_phrases();
  const PhrasePoints pp = phrase_mix_points(sp, 500, 500 + 128 * 1875.0, 16);
  CHECK(pp.mix_in_ms && std::fabs(*pp.mix_in_ms - 500) < 1900);
  CHECK(pp.mix_out_ms && std::fabs(*pp.mix_out_ms - (500 + 112 * 1875.0)) < 1900);
  const PhrasePoints none_pts = phrase_mix_points(StoredPhrases{}, 0, 100000, 16);
  CHECK(!none_pts.mix_in_ms && !none_pts.mix_out_ms);
  TrackFacts f = read_track_facts(lib, a);
  CHECK(f.ok && !f.phrases.sections.empty());
  AutomixSettings st;
  const automix::ResolvedTrack rt = automix::resolve_track(f, AutomixPoints{}, st);
  CHECK(std::fabs(rt.mix_out_ms - (500 + 112 * 1875.0)) < 1900 && rt.mix_out_automatic);
  AutomixPoints own;
  own.mix_out_ms = 100000;
  CHECK(automix::resolve_track(f, own, st).mix_out_ms == 100000);  // the user's point wins
  f.phrases = StoredPhrases{};
  const automix::ResolvedTrack plain = automix::resolve_track(f, AutomixPoints{}, st);
  CHECK(std::fabs(plain.mix_out_ms - (plain.last_sound_ms - st.mix_out_bars * 1875.0 * 1.0)) < 5000);  // fallback: N bars before the end

  for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
  std::printf("test_cue_suggest passed\n");
  return 0;
}
