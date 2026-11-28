#pragma once
// The question after a grid edit that leaves cues on the old grid ("Move the cues with the grid?"). Built from the commit
// (what the library proposed) and shown with the reusable confirmation; "move" hands the track and the journal batch of
// the grid edit back to `on_move`, which applies the follow inside that batch (Library::apply_cue_follow).
#include <cstdint>
#include <functional>
#include <string>

#include "confirm_dialog.hpp"
#include "deckhand/i18n/i18n.hpp"
#include "deckhand/library/library.hpp"

namespace deckhand {

// True when the commit proposes moves and the setting says "ask": nothing else needs a question.
inline bool cue_follow_needs_question(CueFollow mode, const GridCommit& c) { return mode == CueFollow::Ask && !c.proposal.moves.empty(); }

inline ConfirmRequest cue_follow_request(const i18n::Catalog& tr, std::int64_t track_id, const GridCommit& c,
                                         std::function<void(std::int64_t track, std::int64_t batch)> on_move) {
  ConfirmRequest q;
  q.id = "cues.follow." + std::to_string(track_id);
  q.title = tr.tr("cues.follow_title");
  q.message = tr.tr("cues.follow_ask", {{"count", static_cast<int>(c.proposal.moves.size())}});
  if (c.mix_links_moved > 0) q.details.push_back(tr.tr("cues.follow_links", {{"count", c.mix_links_moved}}));
  if (!c.proposal.off_grid.empty()) q.details.push_back(tr.tr("cues.follow_off_grid", {{"count", static_cast<int>(c.proposal.off_grid.size())}}));
  q.details.push_back(tr.tr("cues.follow_undo"));
  q.ok_label = tr.tr("cues.follow_move");
  q.cancel_label = tr.tr("cues.follow_keep");
  const std::int64_t batch = c.batch_id;
  q.on_ok = [on_move = std::move(on_move), track_id, batch] {
    if (on_move) on_move(track_id, batch);
  };
  return q;
}

}  // namespace deckhand
