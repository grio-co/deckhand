// The metadata fill panel (ui.metadata): look up the selected tracks in the open music database on a worker thread (MetadataJob),
// review every proposal (current -> proposed, with confidence), accept or reject each, apply the accepted ones as one batch, undo it.
// Off until the user turns it on in the settings; the panel says what is sent.
#include <algorithm>
#include <cstdio>

#include "app_impl.hpp"
#include "deckhand/app/metadata_job.hpp"

namespace deckhand {

using namespace deckhand::detail;

void App::open_metadata_panel() {
  if (panel_ == Panel::Metadata) {
    close_panel();
    return;
  }
  std::vector<std::int64_t> ids;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    ids = selected_ids();
  }
  open_panel(Panel::Metadata);
  if (meta_ && !meta_->status().running) meta_sel_ = ids;  // the selection when the panel opened (a running lookup keeps its own)
  meta_note_.clear();
}

void App::draw_metadata_section(float x, float& y, float rw) {
  const ui::Theme& th = theme_;
  ui_heading(x, y, T("settings.metadata"));
  y += 18;
  const bool on = meta_ && meta_->enabled();
  if (ui_toggle({x, y, rw, 24}, T("settings.metadata_enable"), on) && meta_) meta_->set_enabled(!on);
  y += 28;
  for (const char* k : {"metadata.sent", "metadata.not_sent"}) {
    dl_.text(x, y, skin_->fit(T(k), 11, 0, rw), 11, th.dim, Align::Left, rw);
    y += 16;
  }
  y += 8;
}

void App::draw_metadata_panel(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 18, rw = m.w - 36;
  float y = m.y + 44;
  if (!meta_) {
    dl_.text(x, y, T("metadata.unavailable"), 12, th.dim);
    return;
  }
  const MetadataJob::Status st = meta_->status();
  if (!st.enabled) {
    dl_.text(x, y, skin_->fit(T("metadata.off"), 12, 0, rw), 12, th.text, Align::Left, rw);
    y += 20;
    dl_.text(x, y, skin_->fit(T("metadata.sent"), 11, 0, rw), 11, th.dim, Align::Left, rw);
    y += 28;
    if (ui_button({x, y, 220, 28}, T("metadata.open_settings"), false)) open_panel(Panel::Settings);
    return;
  }
  dl_.text(x, y, skin_->fit(T("metadata.sent"), 11, 0, rw), 11, th.dim, Align::Left, rw);
  y += 22;
  const bool has_results = !meta_->results().empty();
  if (!st.running) {
    const std::string label = T("metadata.lookup", {{"count", static_cast<double>(meta_sel_.size())}});
    if (ui_button({x, y, 260, 28}, label, false, th.good) && !meta_sel_.empty()) {
      std::string e;
      metadata::FillOptions fo;
      fo.overwrite_nonempty = meta_overwrite_;
      if (!meta_->start(meta_sel_, fo, e)) meta_note_ = e;
      else meta_note_.clear();
    }
    if (!st.network_built) dl_.text(x + 270, y + 8, skin_->fit(T("metadata.no_network"), 11, 0, rw - 270), 11, th.warn, Align::Left, rw - 270);
  } else {
    const std::string prog = T("metadata.progress", {{"done", static_cast<double>(st.done)}, {"total", static_cast<double>(st.total)}});
    dl_.text(x, y + 8, prog, 12, th.text);
    if (ui_button({x + 260, y, 120, 28}, st.cancelling ? T("metadata.cancelling") : T("metadata.cancel"), false) && !st.cancelling) meta_->cancel();
    const float frac = st.total ? static_cast<float>(st.done) / static_cast<float>(st.total) : 0.0f;
    dl_.rrect(x + 390, y + 10, rw - 390, 8, 3, rgba(10, 11, 14));
    dl_.rrect(x + 390, y + 10, (rw - 390) * std::clamp(frac, 0.0f, 1.0f), 8, 3, th.accent);
  }
  y += 36;
  if (ui_toggle({x, y, 330, 22}, T("metadata.overwrite"), meta_overwrite_)) meta_overwrite_ = !meta_overwrite_;
  y += 28;

  const float foot = 40;
  const Rect body{m.x, y, m.w, m.bottom() - y - foot};
  const std::vector<MetadataJob::Track> tracks = meta_->results();
  if (body.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
  panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, panel_content_h_ - body.h));
  dl_.push_clip(body.x, body.y, body.w, body.h);
  float cy = body.y - panel_scroll_;
  if (tracks.empty() && !st.running) {
    dl_.text(x, cy + 4, skin_->fit(meta_note_.empty() ? T("metadata.hint") : meta_note_, 12, 0, rw), 12, th.dim, Align::Left, rw);
    cy += 24;
  }
  for (const MetadataJob::Track& t : tracks) {
    char conf[32] = "";
    if (t.confidence > 0) std::snprintf(conf, sizeof conf, "  %.0f %%", t.confidence * 100);
    dl_.text(x, cy + 2, skin_->fit(t.label + "  -  " + T(std::string("metadata.status.") + metadata::status_name(t.status)) + conf, 12, 0, rw), 12, th.text, Align::Left, rw);
    cy += 20;
    if (t.rows.empty() && !t.message.empty()) {
      dl_.text(x + 12, cy, skin_->fit(t.message, 10, 0, rw - 12), 10, th.dim, Align::Left, rw - 12);
      cy += 16;
    }
    for (const MetadataJob::Row& r : t.rows) {
      const std::string cur = r.p.current.empty() ? T("metadata.empty") : r.p.current;
      char c2[32];
      std::snprintf(c2, sizeof c2, "  (%.0f %%)", r.p.confidence * 100);
      const std::string line = r.p.field + ":  " + cur + "  >  " + r.p.proposed + c2 + (r.p.overwrites ? "  " + T("metadata.replaces") : "") +
                               (r.p.info_only ? "  " + T("metadata.info_only") : "");
      if (r.p.info_only) {
        dl_.text(x + 12, cy + 4, skin_->fit(line, 11, 0, rw - 12), 11, th.dim, Align::Left, rw - 12);
      } else if (ui_toggle({x + 12, cy, rw - 12, 22}, skin_->fit(line, 11, 0, rw - 60), r.accepted)) {
        meta_->set_accepted(t.track_id, r.p.field, !r.accepted);
      }
      cy += 24;
    }
    cy += 6;
  }
  dl_.pop_clip();
  panel_content_h_ = cy + panel_scroll_ - body.y + 8;

  float fy = m.bottom() - foot + 6;
  float bx = x;
  std::size_t accepted = 0;
  for (const auto& t : tracks)
    for (const auto& r : t.rows) accepted += r.accepted && !r.p.info_only ? 1 : 0;
  if (has_results && !st.running) {
    if (ui_button({bx, fy, 100, 26}, T("metadata.accept_all"), false)) meta_->set_all_accepted(true);
    bx += 106;
    if (ui_button({bx, fy, 100, 26}, T("metadata.reject_all"), false)) meta_->set_all_accepted(false);
    bx += 112;
    if (accepted > 0 && ui_button({bx, fy, 190, 26}, T("metadata.apply", {{"count", static_cast<double>(accepted)}}), false, th.good)) {
      metadata::ApplyResult ar;
      std::string e;
      if (meta_->apply(meta_overwrite_, ar, e)) {
        meta_note_ = T("metadata.applied", {{"applied", static_cast<double>(ar.applied)}, {"skipped", static_cast<double>(ar.skipped)}});
        rows_dirty_ = true;
      } else {
        meta_note_ = e;
      }
    }
    bx += 196;
  }
  if (!st.running && st.last_batch > 0 && ui_button({bx, fy, 150, 26}, T("metadata.undo"), false)) {
    UndoResult ur;
    std::string e;
    if (meta_->undo_last(ur, e)) meta_note_ = ur.status == UndoStatus::Ok ? T("metadata.undone") : T("tools.undo_conflict");
    else meta_note_ = e;
    rows_dirty_ = true;
  }
  bx += 156;
  if (!meta_note_.empty() && !tracks.empty() && bx < x + rw)
    dl_.text(bx, fy + 6, skin_->fit(meta_note_, 11, 0, x + rw - bx), 11, th.text, Align::Left, x + rw - bx);
}

}  // namespace deckhand
