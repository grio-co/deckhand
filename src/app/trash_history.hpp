#pragma once
// Data behind the trash and edit-history panel: what the Library keeps, grouped for a list and acted on with one call each.
// Pure logic (no window), so it is tested headless.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"

namespace deckhand {

// One undoable step: the journal rows of a batch (one operation, possibly several fields or tracks).
struct HistoryStep {
  std::int64_t batch_id = 0;
  std::int64_t track_id = 0;  // the first row's track
  std::int64_t edited_at = 0;
  std::string source;                // user | api | import | sync | undo
  std::vector<std::string> fields;   // field names in the batch, each once
  std::string first_old, first_new;  // values of the first row, for a one-line preview ("" when NULL)
};

// Newest first. Rows without a batch (batch_id 0) each form a step of their own.
inline std::vector<HistoryStep> group_history(const std::vector<TrackEditRecord>& rows) {
  std::vector<HistoryStep> out;
  for (const TrackEditRecord& r : rows) {
    HistoryStep* s = nullptr;
    if (r.batch_id != 0)
      for (HistoryStep& h : out)
        if (h.batch_id == r.batch_id) s = &h;
    if (!s) {
      out.emplace_back();
      s = &out.back();
      s->batch_id = r.batch_id != 0 ? r.batch_id : r.id;
      s->track_id = r.track_id;
      s->edited_at = r.edited_at;
      s->source = r.source;
    }
    // rows arrive newest first, so the batch's first row is the one seen last: keep overwriting the preview
    s->first_old = r.old_value.value_or("");
    s->first_new = r.new_value.value_or("");
    s->track_id = r.track_id;
    if (std::find(s->fields.begin(), s->fields.end(), r.field) == s->fields.end()) s->fields.push_back(r.field);
  }
  return out;
}

inline std::vector<HistoryStep> track_history_steps(Library& lib, std::int64_t track_id, int limit = 100) {
  return group_history(lib.track_history(track_id, limit));
}
inline std::vector<HistoryStep> recent_history_steps(Library& lib, int limit = 200) { return group_history(lib.recent_edits(limit)); }

// Undo of a step. A step with batch_id 0 never exists (see above), so undo_batch works for every one.
inline UndoResult undo_step(Library& lib, const HistoryStep& s) { return lib.undo_batch(s.batch_id); }

inline int empty_trash(Library& lib) { return lib.purge_deleted(0); }

}  // namespace deckhand
