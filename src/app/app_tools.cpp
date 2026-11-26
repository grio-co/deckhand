// "Library tools" panel: stem models, the trash, the edit history and duplicates. Small lists with one action each; the
// same operations are in the API. Runs on the UI thread (like the relink panel, it uses the UI's library handle).
#include <algorithm>

#include "app_impl.hpp"

namespace deckhand {

namespace {
std::vector<std::string> wrap(gfx::DrawList& dl, const std::string& text, float size, float width) {
  std::vector<std::string> lines;
  std::string line, word;
  const auto flush_word = [&]() {
    if (word.empty()) return;
    const std::string trial = line.empty() ? word : line + " " + word;
    if (!line.empty() && dl.text_width(trial, size) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = trial;
    }
    word.clear();
  };
  for (const char c : text) {
    if (c == ' ' || c == '\n') flush_word();
    else word += c;
  }
  flush_word();
  if (!line.empty()) lines.push_back(line);
  return lines;
}

std::string human_bytes(std::uint64_t b) {
  char buf[32];
  if (b >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(b) / (1ull << 30));
  else std::snprintf(buf, sizeof buf, "%.0f MB", static_cast<double>(b) / (1ull << 20));
  return buf;
}
}  // namespace

void App::tools_reload() {
  tools_licence_id_.clear();
  tools_dup_confirm_ = -1;
  switch (tools_tab_) {
    case 0:
      if (stems_) tools_models_ = stems_->models();
      break;
    case 1: tools_trash_ = lib_->deleted_tracks(); break;
    case 2: tools_edits_ = lib_->recent_edits(40); break;
    case 3: {
      const HashProgress hp = lib_->hash_pending(50);  // bounded work per visit
      tools_dup_left_ = hp.remaining;
      DuplicateOptions o;
      o.limit = 40;
      tools_dups_ = lib_->find_duplicates(o);
      break;
    }
  }
  panel_scroll_ = 0;
}

void App::draw_tools(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 20, rw = m.w - 40;
  float y = m.y + 46;
  const char* tabs[] = {"tools.tab_stems", "tools.tab_trash", "tools.tab_history", "tools.tab_dups", "tools.tab_queue"};
  const float tw = (rw - 24) / 5;
  for (int i = 0; i < 5; ++i) {
    if (ui_button({x + i * (tw + 6), y, tw, 26}, T(tabs[i]), tools_tab_ == i) && tools_tab_ != i) {
      tools_tab_ = i;
      tools_reload();
    }
  }
  y += 34;
  const Rect list{m.x + 8, y, m.w - 16, m.y + m.h - y - 8};
  if (list.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
  panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, panel_content_h_ - list.h));
  dl_.push_clip(list.x, list.y, list.w, list.h);
  float cy = list.y + 2 - panel_scroll_;
  const float row_h = 34, bw = 120;
  const float lx = x, lw = rw - bw - 10;
  const auto empty = [&]() { dl_.text(lx, cy + 6, T("tools.empty"), 12, th.dim); cy += row_h; };
  const auto name_of = [](const std::string& artist, const std::string& title) { return artist.empty() ? title : artist + " - " + title; };

  if (tools_tab_ == 0) {
    std::string why;
    const bool avail = stems_ && stems_->available(&why);
    const StemInstallState in = stems_ ? stems_->install_state() : StemInstallState{};
    if (in.state != tools_install_seen_) {  // finished or started: re-read what is installed
      tools_install_seen_ = in.state;
      if (stems_) tools_models_ = stems_->models();
      if (in.state == "failed") status_note_ = T("tools.install_failed", {{"message", in.error}});
    }
    if (!avail) {
      dl_.text(lx, cy + 4, skin_->fit(T("tools.stems_unavailable", {{"why", why}}), 11, 0, rw), 11, th.warn, Align::Left, rw);
      cy += 24;
    }
    if (!tools_licence_id_.empty()) {
      const StemModelRow* row = nullptr;
      for (const StemModelRow& r : tools_models_)
        if (r.entry.id == tools_licence_id_) row = &r;
      if (!row) {
        tools_licence_id_.clear();
      } else {
        dl_.text(lx, cy, row->entry.name, 13, th.text, Align::Left, rw, 1);
        cy += 22;
        dl_.text(lx, cy, T("tools.licence_note"), 11, th.dim, Align::Left, rw);
        cy += 20;
        for (const std::string& l : wrap(dl_, row->entry.licence_summary, 11, rw)) {
          dl_.text(lx, cy, l, 11, th.text, Align::Left, rw);
          cy += 15;
        }
        cy += 4;
        dl_.text(lx, cy, skin_->fit(row->entry.licence_source, 10, 0, rw), 10, th.dim, Align::Left, rw);
        cy += 24;
        if (ui_button({lx, cy, 200, 28}, T("tools.accept_install"), true, th.good) && avail) {
          std::string error;
          if (stems_->install(row->entry.id, true, error) != StemService::Result::Ok) status_note_ = T("tools.install_failed", {{"message", error}});
          tools_licence_id_.clear();
        }
        if (ui_button({lx + 208, cy, 110, 28}, T("learn.cancel"), false)) tools_licence_id_.clear();
        cy += 40;
      }
    } else {
      if (tools_models_.empty()) empty();
      for (const StemModelRow& r : tools_models_) {
        const std::string sub = r.installed ? T("tools.installed", {{"size", human_bytes(r.installed_bytes)}})
                                            : T("tools.download", {{"size", human_bytes(r.entry.total_bytes())}});
        dl_.text(lx, cy + 1, skin_->fit(r.entry.name, 12, 1, lw), 12, th.text, Align::Left, lw, 1);
        dl_.text(lx, cy + 17, skin_->fit(sub, 10, 0, lw), 10, th.dim, Align::Left, lw);
        const Rect b{x + rw - bw, cy + 3, bw, 26};
        if (r.installed) {
          if (ui_button(b, T("tools.remove"), false) && stems_) {
            stems_->remove(r.entry.id);
            tools_models_ = stems_->models();
            break;
          }
        } else if (in.state == "running" && in.id == r.entry.id) {
          if (ui_button(b, T("tools.installing", {{"percent", static_cast<int>(in.progress * 100)}}), true) && stems_) stems_->cancel_install();
        } else if (r.entry.install_supported && avail && in.state != "running") {
          if (ui_button(b, T("tools.install"), false)) tools_licence_id_ = r.entry.id;
        } else {
          dl_.text(b.x, b.y + 6, skin_->fit(r.entry.install_supported ? T("tools.busy") : T("tools.not_yet"), 11, 0, bw), 11, th.dim, Align::Left, bw);
        }
        cy += row_h;
      }
    }
  } else if (tools_tab_ == 1) {
    if (tools_trash_.empty()) empty();
    for (const DeletedTrack& d : tools_trash_) {
      dl_.text(lx, cy + 1, skin_->fit(name_of(d.artist, d.title), 12, 0, lw), 12, th.text, Align::Left, lw);
      dl_.text(lx, cy + 17, skin_->fit(d.path, 10, 0, lw), 10, th.dim, Align::Left, lw);
      if (ui_button({x + rw - bw, cy + 3, bw, 26}, T("tools.restore"), false)) {
        std::int64_t restored = 0;
        status_note_ = lib_->restore_track(d.id, &restored) ? T("tools.restored") : T("tools.restore_failed");
        rows_dirty_ = true;
        tools_trash_ = lib_->deleted_tracks();
        break;
      }
      cy += row_h;
    }
  } else if (tools_tab_ == 2) {
    if (tools_edits_.empty()) empty();
    for (const TrackEditRecord& e : tools_edits_) {
      std::string title;
      if (const auto row = lib_->track(e.track_id)) title = name_of(row->artist, row->title);
      const std::string change = e.field + ": " + e.old_value.value_or("") + " > " + e.new_value.value_or("");
      dl_.text(lx, cy + 1, skin_->fit(title.empty() ? std::to_string(e.track_id) : title, 12, 0, lw), 12, th.text, Align::Left, lw);
      dl_.text(lx, cy + 17, skin_->fit(change, 10, 0, lw), 10, th.dim, Align::Left, lw);
      if (e.source != "undo" && ui_button({x + rw - bw, cy + 3, bw, 26}, T("tools.undo"), false)) {
        const UndoResult u = lib_->undo_batch(e.batch_id ? e.batch_id : e.id);
        status_note_ = u.status == UndoStatus::Ok ? T("tools.undone") : T("tools.undo_conflict");
        rows_dirty_ = true;
        tools_edits_ = lib_->recent_edits(40);
        break;
      }
      cy += row_h;
    }
  } else if (tools_tab_ == 4) {
    // Play queue and automix: transport, transition, the upcoming entries.
    if (!queue_ || !automix_) {
      empty();
    } else {
      const AutomixStatus st = automix_->status();
      const AutomixSettings as = automix_->settings();
      std::string err;
      float bx = lx;
      const auto button = [&](const std::string& label, float w, bool on) {
        const bool hit = ui_button({bx, cy, w, 26}, label, on);
        bx += w + 6;
        return hit;
      };
      if (button(T(st.running ? "queue.stop" : "queue.start"), 96, st.running)) automix_control(st.running ? "stop" : "start", false, 0, err);
      if (button(T(st.paused ? "queue.resume" : "queue.pause"), 96, st.paused)) automix_control("pause", false, 0, err);
      if (button(T("queue.skip"), 96, false)) automix_control("skip", false, 0, err);
      cy += 32;
      bx = lx;
      if (button(T("queue.type", {{"type", as.transition_type}}), 180, false)) automix_control("next_type", false, 0, err);
      if (button(T("queue.length", {{"beats", fmt("%g", as.transition_beats)}}), 140, false)) automix_control("next_length", false, 0, err);
      cy += 32;
      bx = lx;
      if (button(T("queue.add_selected"), 170, false)) queue_selected(false);
      if (button(T("queue.play_next_selected"), 190, false)) queue_selected(true);
      cy += 34;
      dl_.text(lx, cy, skin_->fit(T("queue.state", {{"state", st.state}, {"count", static_cast<int>(queue_->size())}}), 11, 0, rw), 11, th.dim, Align::Left, rw);
      cy += 20;
      const std::vector<QueueEntry> up = queue_->upcoming();
      if (up.empty()) dl_.text(lx, cy + 4, T("queue.empty"), 12, th.dim);
      if (up.empty()) cy += row_h;
      const float small = 28, lw2 = rw - 3 * (small + 4) - 10;
      for (std::size_t i = 0; i < up.size() && i < 200; ++i) {
        std::string title = std::to_string(up[i].track_id), artist;
        if (const auto row = lib_->track(up[i].track_id)) {
          title = row->title;
          artist = row->artist;
        }
        dl_.text(lx, cy + 1, skin_->fit(std::to_string(i + 1) + "  " + title, 12, 0, lw2), 12, th.text, Align::Left, lw2);
        dl_.text(lx, cy + 17, skin_->fit(up[i].play_next ? T("queue.play_next_tag") + "  " + artist : artist, 10, 0, lw2), 10, th.dim, Align::Left, lw2);
        const float bx0 = x + rw - 3 * (small + 4);
        bool changed = false;
        if (i > 0 && ui_button({bx0, cy + 3, small, 26}, "^", false)) changed = queue_->move(up[i].id, static_cast<int>(i) - 1);
        else if (i + 1 < up.size() && ui_button({bx0 + small + 4, cy + 3, small, 26}, "v", false)) changed = queue_->move(up[i].id, static_cast<int>(i) + 1);
        else if (ui_button({bx0 + 2 * (small + 4), cy + 3, small, 26}, "x", false, th.warn)) changed = queue_->remove(up[i].id);
        if (changed) break;
        cy += row_h;
      }
    }
  } else {
    if (tools_dup_left_ > 0) {
      dl_.text(lx, cy + 2, T("tools.dup_hashing", {{"count", tools_dup_left_}}), 11, th.dim, Align::Left, rw);
      cy += 20;
    }
    if (tools_dups_.empty()) empty();
    for (std::size_t i = 0; i < tools_dups_.size(); ++i) {
      const DuplicateGroup& g = tools_dups_[i];
      std::string keep_name;
      for (const DuplicateMember& mem : g.members)
        if (mem.row.id == g.suggested_keep) keep_name = name_of(mem.row.artist, mem.row.title);
      dl_.text(lx, cy + 1, skin_->fit(keep_name + "  (" + std::to_string(g.members.size()) + ")", 12, 0, lw), 12, th.text, Align::Left, lw);
      dl_.text(lx, cy + 17, skin_->fit(g.kind + ": " + g.suggestion_reason, 10, 0, lw), 10, th.dim, Align::Left, lw);
      const bool confirming = tools_dup_confirm_ == static_cast<int>(i);
      if (ui_button({x + rw - bw, cy + 3, bw, 26}, confirming ? T("tools.dup_confirm") : T("tools.dup_merge"), confirming, th.warn)) {
        if (!confirming) {
          tools_dup_confirm_ = static_cast<int>(i);  // the second click is the confirmation
        } else {
          std::vector<std::int64_t> remove;
          for (const DuplicateMember& mem : g.members)
            if (mem.row.id != g.suggested_keep) remove.push_back(mem.row.id);
          const MergeResult res = lib_->merge_duplicates(g.suggested_keep, remove);
          status_note_ = res.ok ? T("tools.dup_merged", {{"count", static_cast<int>(res.removed.size())}}) : res.error;
          rows_dirty_ = true;
          tools_dup_confirm_ = -1;
          DuplicateOptions o;
          o.limit = 40;
          tools_dups_ = lib_->find_duplicates(o);
          break;
        }
      }
      cy += row_h;
    }
  }
  dl_.pop_clip();
  panel_content_h_ = cy + panel_scroll_ - list.y + 8;
}

}  // namespace deckhand
