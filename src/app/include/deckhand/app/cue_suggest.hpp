#pragma once
// Cue suggestions from the stored phrase analysis: what to suggest, and writing it into FREE cue slots only (never
// overwriting a cue). The user's own cueing habit (from tracks they already cued) is summarised once and cached.
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "deckhand/analysis/analysis.hpp"
#include "deckhand/library/library.hpp"

namespace deckhand {

struct SuggestOptions {
  int max_hot = 8;        // hot cues suggested (and written by apply)
  int mix_out_bars = 16;  // 8, 16 or 32
  bool memory = false;    // memory-cue candidates at phrase boundaries (apply writes them only when set)
  bool habit = false;     // bias toward where this user usually cues
};

struct HabitCache {
  std::mutex m;
  CueHabit habit;
  std::int64_t stamp_tracks = -1, stamp_cues = -1;
  double stamp_sum = -1;
};

// Library connection must belong to the calling thread. Refreshes the cache lazily when the cued tracks changed.
CueHabit cue_habit(Library& lib, HabitCache& cache);

struct SuggestResult {
  bool has_phrases = false;
  std::string note;  // why there are no suggestions
  std::vector<CueSuggestion> items;
  int habit_samples = 0;  // cues the habit summary rests on (0 when habit was not used)
};
SuggestResult suggest_for_track(Library& lib, std::int64_t track_id, const SuggestOptions& o, HabitCache* cache);

struct AppliedCue {
  CueSuggestion suggestion;
  int slot = -1;  // hot slot (0-based) or -1 for memory
};
struct SkippedCue {
  CueSuggestion suggestion;
  std::string reason;  // no_free_slot | cue_exists
};
struct ApplyResult {
  std::int64_t track_id = 0;
  bool has_phrases = false;
  std::string note;
  std::vector<AppliedCue> written;
  std::vector<SkippedCue> skipped;
};
// Writes (or, with dry_run, only reports) the suggestions as cues. The caller holds the EditScope, so a batch over many
// tracks is one undo_batch.
ApplyResult apply_suggestions(Library& lib, std::int64_t track_id, const SuggestOptions& o, HabitCache* cache, bool dry_run);

}  // namespace deckhand
