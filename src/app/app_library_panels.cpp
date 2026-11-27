// Second track table, browser layouts, trash / edit history and the "find moved files" panel (the UI glue over
// browser_panes.hpp, browser_layouts.hpp, trash_history.hpp and the library's relocation calls).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

#include "app_impl.hpp"

namespace deckhand {

using namespace deckhand::detail;

namespace {
std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::vector<std::string> wrap(gfx::DrawList& dl, const std::string& text, float size, float width) {
  std::vector<std::string> lines;
  std::string line;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t sp = text.find(' ', at);
    if (sp == std::string::npos) sp = text.size();
    const std::string word = text.substr(at, sp - at);
    const std::string probe = line.empty() ? word : line + " " + word;
    if (!line.empty() && dl.text_width(probe, size) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = probe;
    }
    at = sp + 1;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}
}  // namespace

// ---- the two panes ---------------------------------------------------------------------------------------------
PaneSnapshot App::snapshot_live() const {
  PaneSnapshot p;
  p.kind = static_cast<int>(view_);
  p.id = playlist_id_;
  p.path = disk_path_;
  p.sort = sort_;
  p.descending = sort_desc_;
  p.search = search_;
  return p;
}

void App::restore_live(const PaneSnapshot& p) {
  view_ = static_cast<TreeItem::Kind>(p.kind);
  playlist_id_ = p.id;
  disk_path_ = p.path;
  sort_ = p.sort;
  sort_desc_ = p.descending;
  search_ = p.search;
  sel_ = 0;
  scroll_ = 0;
  multi_.clear();
  rows_dirty_ = true;
}

void App::swap_pane() {  // browse_mutex_ held
  std::swap(view_, pane2_.view);
  std::swap(playlist_id_, pane2_.playlist_id);
  std::swap(disk_path_, pane2_.disk_path);
  std::swap(search_, pane2_.search);
  std::swap(sort_, pane2_.sort);
  std::swap(sort_desc_, pane2_.desc);
  std::swap(sel_, pane2_.sel);
  std::swap(anchor_, pane2_.anchor);
  std::swap(scroll_, pane2_.scroll);
  std::swap(follow_sel_, pane2_.follow);
  std::swap(multi_, pane2_.multi);
  std::swap(rows_, pane2_.rows);
  std::swap(sources_, pane2_.sources);
}

void App::save_panes() {  // browse_mutex_ held (or the UI thread alone)
  if (!lib_ || !panes_loaded_) return;
  PaneSettings s;
  s.mode = split_;
  s.live_slot = live_slot_;
  PaneSnapshot other;
  other.kind = static_cast<int>(pane2_.view);
  other.id = pane2_.playlist_id;
  other.path = pane2_.disk_path;
  other.sort = pane2_.sort;
  other.descending = pane2_.desc;
  other.search = pane2_.search;
  s.pane[live_slot_] = snapshot_live();
  s.pane[1 - live_slot_] = other;
  save_pane_settings(*lib_, s);
}

void App::load_panes() {  // browse_mutex_ held
  if (panes_loaded_ || !lib_) return;
  panes_loaded_ = true;
  const PaneSettings s = load_pane_settings(*lib_);
  split_ = s.mode;
  live_slot_ = s.live_slot;
  const bool reload = reload_panes_;
  reload_panes_ = false;
  if (split_ == SplitMode::Off) {  // the single table keeps what the tree restored, except after an API edit
    if (reload) {
      restore_live(s.pane[live_slot_]);
      view_ = static_cast<TreeItem::Kind>(std::clamp(s.pane[live_slot_].kind, 0, static_cast<int>(TreeItem::DiskFolder)));
    }
    return;
  }
  const PaneSnapshot& mine = s.pane[live_slot_];
  const PaneSnapshot& other = s.pane[1 - live_slot_];
  if (mine.kind >= 0 && mine.kind <= static_cast<int>(TreeItem::DiskFolder)) restore_live(mine);
  pane2_.view = static_cast<TreeItem::Kind>(std::clamp(other.kind, 0, static_cast<int>(TreeItem::DiskFolder)));
  pane2_.playlist_id = other.id;
  pane2_.disk_path = other.path;
  pane2_.sort = other.sort;
  pane2_.desc = other.descending;
  pane2_.search = other.search;
  rows_dirty_ = true;
}

void App::toggle_split() {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  panes_loaded_ = true;
  const SplitMode next = next_split_mode(split_);
  if (split_ == SplitMode::Off) {  // the new pane starts on what the live one shows
    pane2_ = PaneState{};
    pane2_.view = view_;
    pane2_.playlist_id = playlist_id_;
    pane2_.disk_path = disk_path_;
    pane2_.sort = sort_;
    pane2_.desc = sort_desc_;
    pane2_.search = search_;
  }
  split_ = next;
  if (split_ == SplitMode::Off) live_slot_ = 0;
  status_note_ = T(split_ == SplitMode::Off ? "browser.split_off" : split_ == SplitMode::Side ? "browser.split_side" : "browser.split_stacked");
  rows_dirty_ = true;
  save_panes();
}

void App::focus_other_pane() {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  if (split_ == SplitMode::Off) return;
  swap_pane();
  live_slot_ = 1 - live_slot_;
  focus_tree_ = false;
  rows_dirty_ = true;
  save_panes();
}

void App::draw_browser_panes(const Rect& r, const ui::Theme& th, const std::string& columns, float row_h) {
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (browser_reload_.exchange(false)) {  // the API wrote layouts, the split or a pane's source
      layouts_.reset();
      panes_loaded_ = false;
      reload_panes_ = true;
    }
    load_panes();
  }
  if (split_ == SplitMode::Off) {
    draw_tracks(r, th, columns, row_h);
    return;
  }
  const auto slots = split_slots({r.x, r.y, r.w, r.h}, split_, 6);
  const auto rect_of = [&](int s) { return Rect{slots[static_cast<std::size_t>(s)].x, slots[static_cast<std::size_t>(s)].y, slots[static_cast<std::size_t>(s)].w, slots[static_cast<std::size_t>(s)].h}; };
  if (in_.pressed) {  // a click in the other table focuses it first
    const int hit = slot_at(slots, split_, in_.x, in_.y);
    if (hit >= 0 && hit != live_slot_) focus_other_pane();
  }
  const int live = live_slot_;
  draw_tracks(rect_of(live), th, columns, row_h);
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    swap_pane();
  }
  const bool had_press = in_.pressed;
  in_.pressed = false;  // the unfocused table only shows; clicks in it were turned into a focus change above
  draw_tracks(rect_of(1 - live), th, columns, row_h);
  in_.pressed = had_press;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    swap_pane();
  }
  const Rect f = rect_of(live);
  dl_.rect(f.x, f.y, f.w, 2, th.accent);
}

// ---- layouts ---------------------------------------------------------------------------------------------------
BrowserLayouts& App::layouts() {
  if (!layouts_) layouts_.emplace(*lib_);
  return *layouts_;
}

void App::apply_layout(const BrowserLayout& l) {
  std::vector<std::string> cols;
  for (const std::string& id : l.columns)
    if (std::find(browser_column_ids().begin(), browser_column_ids().end(), id) != browser_column_ids().end()) cols.push_back(id);
  if (cols.empty()) return;
  bcols_ = cols;
  bwidth_.clear();
  for (const auto& [k, w] : l.widths) bwidth_[k] = w;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (view_ != TreeItem::HistorySession) {
      sort_ = l.sort;
      sort_desc_ = l.descending;
    }
    rows_dirty_ = true;
    save_panes();
  }
  bcols_loaded_ = true;
  save_browser_settings();
  layouts().set_active(l.name);
}

void App::step_layout(int dir) {
  BrowserLayouts& ls = layouts();
  if (ls.all().empty()) return;
  const BrowserLayout next = ls.step(ls.active(), dir);
  apply_layout(next);
  status_note_ = T("layouts.switched", {{"name", next.builtin ? T("layouts.builtin." + next.name) : next.name}});
}

void App::draw_layouts_panel(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 18, rw = m.w - 36;
  float y = m.y + 44;
  dl_.text(x, y, skin_->fit(T("layouts.help"), 11, 0, rw), 11, th.dim, Align::Left, rw);
  y += 22;
  BrowserLayouts& ls = layouts();
  const std::string active = ls.active();
  std::string apply_name, remove_name;
  const std::size_t n = ls.all().size();
  const float row_h = 28;
  const float list_h = std::max(60.0f, m.y + m.h - y - 90);
  const Rect list{m.x + 8, y, m.w - 16, list_h};
  if (list.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
  panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, row_h * static_cast<float>(n) - list.h));
  dl_.push_clip(list.x, list.y, list.w, list.h);
  float cy = list.y - panel_scroll_;
  for (const BrowserLayout& l : ls.all()) {
    const std::string label = l.builtin ? T("layouts.builtin." + l.name) : l.name;
    if (ui_button({x, cy, rw - (l.builtin ? 0.0f : 34.0f), row_h - 3}, label, l.name == active)) apply_name = l.name;
    if (!l.builtin && ui_button({x + rw - 30, cy, 30, row_h - 3}, "x", false)) remove_name = l.name;
    cy += row_h;
  }
  dl_.pop_clip();
  y = list.y + list.h + 8;
  dl_.text(x, y + 5, T("layouts.name"), 12, th.dim);
  ui_field({x + 70, y, rw - 70, 24}, f_layout_name_, T("layouts.name"));
  y += 32;
  if (ui_button({x, y, rw, 28}, T("layouts.save"), false)) {
    BrowserLayout cur;
    cur.name = trim(f_layout_name_.text);
    cur.columns = bcols_;
    for (const auto& kv : bwidth_) cur.widths[kv.first] = kv.second;
    {
      std::lock_guard<std::mutex> lock(browse_mutex_);
      cur.sort = sort_;
      cur.descending = sort_desc_;
    }
    if (ls.save(cur)) {
      status_note_ = T("layouts.saved", {{"name", clean_layout_name(cur.name)}});
      ls.set_active(clean_layout_name(cur.name));
      f_layout_name_.text.clear();
    } else {
      status_note_ = T("layouts.not_saved");
    }
  }
  if (!apply_name.empty())
    if (const BrowserLayout* l = ls.find(apply_name)) {
      const BrowserLayout copy = *l;
      apply_layout(copy);
      status_note_ = T("layouts.switched", {{"name", copy.builtin ? T("layouts.builtin." + copy.name) : copy.name}});
    }
  if (!remove_name.empty()) ls.remove(remove_name);
}

// ---- trash and edit history --------------------------------------------------------------------------------------
void App::draw_trash_panel(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 20, rw = m.w - 40;
  float y = m.y + 46;
  std::int64_t sel_track = 0;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) sel_track = rows_.id(static_cast<std::size_t>(sel_));
  }
  const char* tabs[] = {"trash.tab_trash", "trash.tab_track", "trash.tab_recent"};
  const float tw = (rw - 12) / 3;
  for (int i = 0; i < 3; ++i)
    if (ui_button({x + static_cast<float>(i) * (tw + 6), y, tw, 26}, T(tabs[i]), trash_tab_ == i) && trash_tab_ != i) {
      trash_tab_ = i;
      trash_dirty_ = true;
      panel_scroll_ = 0;
      trash_confirm_empty_ = false;
    }
  y += 34;
  if (trash_dirty_) {
    trash_dirty_ = false;
    trash_cache_.clear();
    hist_cache_.clear();
    if (trash_tab_ == 0) {
      trash_cache_ = lib_->deleted_tracks();
    } else {
      const std::vector<HistoryStep> steps = trash_tab_ == 1 ? (sel_track ? track_history_steps(*lib_, sel_track) : std::vector<HistoryStep>{})
                                                              : recent_history_steps(*lib_, 100);
      for (const HistoryStep& s : steps) {
        HistRow h;
        h.step = s;
        if (const auto row = lib_->track(s.track_id)) h.title = row->artist.empty() ? row->title : row->artist + " - " + row->title;
        hist_cache_.push_back(std::move(h));
      }
    }
  }
  const float foot = trash_tab_ == 0 ? 40.0f : 8.0f;
  const Rect list{m.x + 8, y, m.w - 16, m.y + m.h - y - foot};
  if (list.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
  const float row_h = 34, bw = 110, lw = rw - bw - 10;
  const std::size_t count = trash_tab_ == 0 ? trash_cache_.size() : hist_cache_.size();
  panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, row_h * static_cast<float>(count) - list.h));
  dl_.push_clip(list.x, list.y, list.w, list.h);
  float cy = list.y + 2 - panel_scroll_;
  if (count == 0) dl_.text(x, cy + 6, T(trash_tab_ == 0 ? "trash.empty" : "trash.no_history"), 12, th.dim);
  bool reload = false;
  if (trash_tab_ == 0) {
    for (const DeletedTrack& d : trash_cache_) {
      const std::string name = d.artist.empty() ? d.title : d.artist + " - " + d.title;
      dl_.text(x, cy + 1, skin_->fit(name, 12, 0, lw), 12, th.text, Align::Left, lw);
      dl_.text(x, cy + 17, skin_->fit(d.path, 10, 0, lw), 10, th.dim, Align::Left, lw);
      if (ui_button({x + rw - bw, cy + 3, bw, 26}, T("trash.restore"), false)) {
        std::int64_t restored = 0;
        status_note_ = lib_->restore_track(d.id, &restored) ? T("trash.restored", {{"title", name}}) : T("trash.cannot_restore");
        rows_dirty_ = true;
        reload = true;
        break;
      }
      cy += row_h;
    }
  } else {
    for (const HistRow& h : hist_cache_) {
      std::string fields;
      for (const std::string& f : h.step.fields) fields += (fields.empty() ? "" : ", ") + f;
      const std::string change = fields + ": " + h.step.first_old + " > " + h.step.first_new + "  (" + h.step.source + ")";
      dl_.text(x, cy + 1, skin_->fit(h.title.empty() ? T("trash.gone") : h.title, 12, 0, lw), 12, th.text, Align::Left, lw);
      dl_.text(x, cy + 17, skin_->fit(change, 10, 0, lw), 10, th.dim, Align::Left, lw);
      if (h.step.source != "undo" && ui_button({x + rw - bw, cy + 3, bw, 26}, T("trash.undo"), false)) {
        const UndoResult u = undo_step(*lib_, h.step);
        status_note_ = u.status == UndoStatus::Ok ? T("trash.undone") : T("trash.conflict");
        rows_dirty_ = true;
        reload = true;
        break;
      }
      cy += row_h;
    }
  }
  dl_.pop_clip();
  if (reload) trash_dirty_ = true;
  if (trash_tab_ == 0 && !trash_cache_.empty()) {
    const std::string label = trash_confirm_empty_ ? T("trash.empty_confirm", {{"count", static_cast<int>(trash_cache_.size())}}) : T("trash.empty_button");
    if (ui_button({x, m.y + m.h - 36, rw, 28}, label, trash_confirm_empty_, th.warn)) {
      if (trash_confirm_empty_) {
        status_note_ = T("trash.emptied", {{"count", empty_trash(*lib_)}});
        trash_confirm_empty_ = false;
        trash_dirty_ = true;
      } else {
        trash_confirm_empty_ = true;
      }
    }
  }
}

// ---- find moved files -------------------------------------------------------------------------------------------
void App::start_relocate_scan() {
  if (loc_future_.valid()) return;
  RelocateOptions o;
  for (const std::string& f : split_text(f_locate_.text, ';')) {
    const std::string t = trim(f);
    if (!t.empty()) o.folders.push_back(t);
  }
  if (o.folders.empty()) return;
  loc_cancel_ = false;
  o.cancelled = [this] { return loc_cancel_.load(); };
  Library* lib = lib_.get();
  loc_future_ = std::async(std::launch::async, [lib, o]() { return lib->relocate_scan(o); });
  loc_scanned_ = false;
}

void App::draw_locate_panel(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 20, rw = m.w - 40;
  float y = m.y + 44;
  if (loc_future_.valid() && loc_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    loc_report_ = loc_future_.get();
    loc_scanned_ = true;
  }
  const bool scanning = loc_future_.valid();
  for (const std::string& l : wrap(dl_, T("locate.help"), 11, rw)) {
    dl_.text(x, y, l, 11, th.dim, Align::Left, rw);
    y += 15;
  }
  y += 6;
  dl_.text(x, y + 5, T("locate.choose_title"), 12, th.dim);
  ui_field({x + 150, y, rw - 150 - 110, 24}, f_locate_, "/Volumes/Music");
  if (ui_button({x + rw - 100, y, 100, 24}, T(scanning ? "locate.scanning" : "locate.scan"), scanning) && !scanning) start_relocate_scan();
  y += 32;
  if (loc_scanned_) {
    dl_.text(x, y, skin_->fit(T("locate.summary", {{"found", static_cast<int>(loc_report_.found.size())},
                                                    {"ambiguous", static_cast<int>(loc_report_.ambiguous.size())},
                                                    {"missing", loc_report_.missing},
                                                    {"scanned", loc_report_.scanned}}), 12, 0, rw), 12, th.text, Align::Left, rw);
    y += 24;
    const Rect list{m.x + 8, y, m.w - 16, m.y + m.h - y - 52};
    if (list.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
    const float row_h = 30;
    const float total = row_h * static_cast<float>(loc_report_.found.size() + loc_report_.ambiguous.size());
    panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, total - list.h));
    dl_.push_clip(list.x, list.y, list.w, list.h);
    float cy = list.y - panel_scroll_;
    for (const RelocateMatch& f : loc_report_.found) {
      dl_.text(x, cy + 1, skin_->fit(f.old_path, 11, 0, rw), 11, th.dim, Align::Left, rw);
      dl_.text(x, cy + 14, skin_->fit(f.new_path + "  (" + f.how + (f.weak ? ", weak" : "") + ")", 11, 0, rw), 11, f.weak ? th.warn : th.text, Align::Left, rw);
      cy += row_h;
    }
    for (const RelocateAmbiguous& a : loc_report_.ambiguous) {
      dl_.text(x, cy + 8, skin_->fit(T("locate.ambiguous_row", {{"name", a.old_path}, {"count", static_cast<int>(a.candidates.size())}}), 11, 0, rw), 11, th.warn, Align::Left, rw);
      cy += row_h;
    }
    dl_.pop_clip();
    const float by = m.y + m.h - 42;
    if (!loc_report_.found.empty() &&
        ui_button({x, by, 260, 28}, T("locate.apply", {{"count", static_cast<int>(loc_report_.found.size())}}), true, th.good)) {
      const RelocateResult r = lib_->relocate_apply(loc_report_.found);
      loc_last_batch_ = r.batch_id;
      status_note_ = T("locate.applied", {{"count", r.applied}, {"skipped", r.skipped}});
      loc_report_ = {};
      loc_scanned_ = false;
      rows_dirty_ = true;
    }
    if (loc_last_batch_ != 0 && ui_button({x + 270, by, 200, 28}, T("locate.undo_last"), false)) {
      const UndoResult u = lib_->undo_batch(loc_last_batch_);
      status_note_ = T("locate.undone", {{"count", u.applied}});
      loc_last_batch_ = 0;
      rows_dirty_ = true;
    }
  } else if (loc_last_batch_ != 0) {
    if (ui_button({x, y, 200, 28}, T("locate.undo_last"), false)) {
      const UndoResult u = lib_->undo_batch(loc_last_batch_);
      status_note_ = T("locate.undone", {{"count", u.applied}});
      loc_last_batch_ = 0;
      rows_dirty_ = true;
    }
  }
}

}  // namespace deckhand
