// The track browser: configurable columns, multi-select, drag and drop onto playlists and inside a playlist, rating and
// colour cells, the compatible-tracks filter, and the editors for columns and smart playlists.
#include "app_impl.hpp"
#include "column_order.hpp"

#include <cctype>
#include <cstring>

namespace deckhand {

namespace {

struct ColDef {
  const char* id;
  float w;       // flexible columns: weight; others: width in points
  bool flex, right;
  SortKey key;
};
const ColDef kCols[] = {
    {"title", 1.2f, true, false, SortKey::Title},    {"artist", 1.0f, true, false, SortKey::Artist},
    {"album", 0.8f, true, false, SortKey::Album},    {"genre", 96, false, false, SortKey::Genre},
    {"bpm", 58, false, true, SortKey::Bpm},          {"key", 52, false, false, SortKey::Key},
    {"time", 56, false, true, SortKey::Duration},    {"rating", 82, false, false, SortKey::Rating},
    {"comment", 0.8f, true, false, SortKey::Comment}, {"added", 90, false, false, SortKey::DateAdded},
    {"plays", 52, false, true, SortKey::PlayCount},  {"played", 90, false, false, SortKey::LastPlayed},
    {"color", 44, false, false, SortKey::Color},     {"format", 54, false, false, SortKey::Format},
    {"bitrate", 62, false, true, SortKey::Bitrate},
};
constexpr int kColCount = static_cast<int>(sizeof kCols / sizeof kCols[0]);

const ColDef* col_def(const std::string& id) {
  for (const ColDef& c : kCols)
    if (id == c.id) return &c;
  return nullptr;
}

std::string trim_copy(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  return s.substr(a);
}

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
  return s;
}

std::string date_only(std::int64_t seconds) {
  if (seconds <= 0) return {};
  const std::time_t t = static_cast<std::time_t>(seconds);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

Color palette_color(int rgb) { return rgba((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  for (std::size_t a = 0; a <= s.size();) {
    const std::size_t b = s.find(sep, a);
    std::string part = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (!part.empty()) out.push_back(std::move(part));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

}  // namespace

// ---- settings ---------------------------------------------------------------------------------------------------------
void App::load_browser_settings(const std::string& skin_default) {
  bcols_.clear();
  for (const std::string& id : split(lib_->setting("browser.columns", ""), ','))
    if (col_def(id) && std::find(bcols_.begin(), bcols_.end(), id) == bcols_.end()) bcols_.push_back(id);
  if (bcols_.empty())
    for (const std::string& id : split(skin_default, ','))
      if (col_def(id) && std::find(bcols_.begin(), bcols_.end(), id) == bcols_.end()) bcols_.push_back(id);
  if (bcols_.empty()) bcols_ = {"title", "artist", "bpm", "key", "time"};
  bwidth_.clear();
  for (const std::string& kv : split(lib_->setting("browser.widths", ""), ',')) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos || !col_def(kv.substr(0, eq))) continue;
    const float w = static_cast<float>(std::atof(kv.c_str() + eq + 1));
    if (w > 0.05f && w < 2000) bwidth_[kv.substr(0, eq)] = w;
  }
  key_musical_ = lib_->setting("browser.key_musical", "0") == "1";
  bcols_loaded_ = true;
}

void App::save_browser_settings() {
  std::string cols, widths;
  for (const std::string& id : bcols_) cols += (cols.empty() ? "" : ",") + id;
  for (const auto& kv : bwidth_) widths += (widths.empty() ? "" : ",") + kv.first + "=" + fmt("%.2f", kv.second);
  lib_->set_setting("browser.columns", cols);
  lib_->set_setting("browser.widths", widths);
}

// ---- selection and filters ---------------------------------------------------------------------------------------------
std::vector<std::int64_t> App::selected_ids() {
  std::vector<std::int64_t> ids;
  for (const int i : multi_)
    if (i >= 0 && i < static_cast<int>(rows_.size())) ids.push_back(rows_.id(static_cast<std::size_t>(i)));
  if (ids.empty() && sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) ids.push_back(rows_.id(static_cast<std::size_t>(sel_)));
  return ids;
}

bool App::file_missing(const TrackRow& t) {
  auto it = exists_.find(t.id);
  if (it == exists_.end()) {
    std::error_code ec;
    it = exists_.emplace(t.id, std::filesystem::exists(t.path, ec)).first;
  }
  return !it->second;
}

// The compatible filter is about the track on a playing deck, else a loaded deck, else the selected row.
bool App::compat_reference(double& bpm, std::string& key) {
  int pick = -1;
  for (int pass = 0; pass < 2 && pick < 0; ++pass)
    for (int d = 0; d < kMaxDecks; ++d)
      if (views_[d].loaded && (pass == 1 || views_[d].playing)) {
        pick = d;
        break;
      }
  if (pick >= 0) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (decks_[pick].row.id == views_[pick].track_id) {
      bpm = decks_[pick].row.bpm;
      key = decks_[pick].row.key;
      return bpm > 0 || !key.empty();
    }
  }
  if (sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) {
    const TrackRow& r = rows_[static_cast<std::size_t>(sel_)];
    bpm = r.bpm;
    key = r.key;
    return bpm > 0 || !key.empty();
  }
  return false;
}

void App::drop_rows_on_playlist(std::int64_t playlist_id) {
  const std::vector<std::int64_t> ids = selected_ids();
  for (const std::int64_t id : ids) lib_->add_to_playlist(playlist_id, id);
  rows_dirty_ = true;
  std::string name;
  for (const TreeItem& item : tree_)
    if (item.kind == TreeItem::Playlist && item.id == playlist_id) name = item.name;
  status_note_ = T("status.added_many_to", {{"count", static_cast<int>(ids.size())}, {"name", name}});
}

// ---- the track table ----------------------------------------------------------------------------------------------------
void App::draw_tracks(const Rect& table, const ui::Theme& th, const std::string& columns, float row_h) {
  if (settings_synced_.exchange(false)) bcols_loaded_ = false;
  if (!bcols_loaded_) load_browser_settings(columns);
  std::lock_guard<std::mutex> lock(browse_mutex_);
  struct Col {
    const ColDef* def;
    float w;  // points, or weight when flexible
  };
  std::vector<Col> cols;
  for (const std::string& id : bcols_) {
    const ColDef* d = col_def(id);
    if (!d) continue;
    const auto it = bwidth_.find(id);
    cols.push_back({d, it != bwidth_.end() ? it->second : d->w});
  }
  if (cols.empty()) return;
  const float badge = 30;
  float fixed = badge + 12, flex_sum = 0;
  for (const Col& c : cols) (c.def->flex ? flex_sum : fixed) += c.w;
  const float flex_w = std::max(40.0f, table.w - fixed);
  std::vector<float> x0(cols.size()), x1(cols.size());
  float x = table.x + badge;
  for (std::size_t i = 0; i < cols.size(); ++i) {
    x0[i] = x;
    x += cols[i].def->flex ? flex_w * cols[i].w / flex_sum : cols[i].w;
    x1[i] = x;
  }
  const float head_h = row_h + 2;
  dl_.rect(table.x, table.y, table.w, head_h, th.panel2);

  // Header: click to sort (ascending, descending, natural); drag it to move the column; drag its right edge to resize.
  for (std::size_t i = 0; i < cols.size(); ++i) {
    const ColDef& c = *cols[i].def;
    const bool active = sort_ == c.key && c.key != SortKey::Default;
    const std::string label = skin_->fit(T(std::string("browser.col.") + c.id) + (active ? (sort_desc_ ? " \xE2\x96\xBE" : " \xE2\x96\xB4") : ""), 10, 1, x1[i] - x0[i] - 12);
    const float ty = table.y + (head_h - 13) / 2;
    if (c.right) dl_.text(x1[i] - 8, ty, label, 10, active ? th.text : th.dim, Align::Right, 0, 1);
    else dl_.text(x0[i] + 4, ty, label, 10, active ? th.text : th.dim, Align::Left, 0, 1);
    const Rect grip{x1[i] - 3, table.y, 7, head_h};
    if (grip.contains(in_.x, in_.y)) dl_.rect(x1[i] - 1, table.y + 3, 2, head_h - 6, th.accent);
    if (clicked(grip)) {
      col_drag_ = static_cast<int>(i);
      col_drag_x_ = in_.x;
      col_drag_w_ = c.flex ? flex_w * cols[i].w / flex_sum : cols[i].w;
    } else if (clicked({x0[i], table.y, x1[i] - x0[i] - 4, head_h}) && col_drag_ < 0) {
      col_move_ = static_cast<int>(i);  // a click or the start of a move: decided on release
      col_move_x_ = in_.x;
      col_moving_ = false;
    }
  }
  if (col_move_ >= static_cast<int>(cols.size()) || in_.x < -1000) col_move_ = -1;  // columns changed, or an overlay opened
  if (col_move_ >= 0) {
    const ColDef& c = *cols[static_cast<std::size_t>(col_move_)].def;
    col_moving_ = col_moving_ || std::fabs(in_.x - col_move_x_) > 6;
    const std::size_t slot = column_drop_slot(x0, x1, in_.x);
    if (in_.down) {
      if (col_moving_)
        dl_.rect((slot < cols.size() ? x0[slot] : x1.back()) - 1, table.y + 1, 3, head_h - 2, th.accent);
    } else {
      if (col_moving_) {
        if (move_column(bcols_, c.id, slot < cols.size() ? cols[slot].def->id : "")) save_browser_settings();
      } else if (view_ != TreeItem::HistorySession) {  // history keeps its play order
        if (sort_ != c.key || c.key == SortKey::Default) { sort_ = c.key; sort_desc_ = false; }
        else if (!sort_desc_) sort_desc_ = true;
        else sort_ = SortKey::Default;
        rows_dirty_ = true;
      }
      col_move_ = -1;
    }
  }
  if (col_drag_ >= 0) {
    if (in_.down && col_drag_ < static_cast<int>(cols.size())) {
      const Col& c = cols[static_cast<std::size_t>(col_drag_)];
      const float px = std::clamp(col_drag_w_ + (in_.x - col_drag_x_), 36.0f, 700.0f);
      bwidth_[c.def->id] = c.def->flex ? std::max(0.1f, px / flex_w * flex_sum) : px;
    } else {
      col_drag_ = -1;
      save_browser_settings();
    }
  }

  const Rect body{table.x, table.y + head_h, table.w, table.h - head_h};
  const float content = static_cast<float>(rows_.size()) * row_h;
  if (body.contains(in_.x, in_.y)) scroll_ -= in_.scroll;
  if (follow_sel_) {
    const float top = static_cast<float>(sel_) * row_h;
    if (top < scroll_) scroll_ = top;
    if (top + row_h > scroll_ + body.h) scroll_ = top + row_h - body.h;
    follow_sel_ = false;
  }
  scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content - body.h));

  const bool cmd = (mods_ & (DH_MOD_COMMAND | DH_MOD_CONTROL)) != 0;
  int load_deck = -1;
  dl_.push_clip(body.x, body.y, body.w, body.h);
  const int first = static_cast<int>(scroll_ / row_h);
  const int last = std::min(static_cast<int>(rows_.size()), first + static_cast<int>(body.h / row_h) + 2);
  for (int i = first; i < last; ++i) {
    const TrackRow& t = rows_[static_cast<std::size_t>(i)];
    const Rect row{body.x, body.y + static_cast<float>(i) * row_h - scroll_, body.w, row_h};
    const bool chosen = multi_.count(i) != 0 || i == sel_;
    if (chosen) dl_.rect(row.x, row.y, row.w, row.h, with_alpha(th.accent, focus_tree_ ? 70 : (i == sel_ ? 150 : 100)));
    else if (i % 2) dl_.rect(row.x, row.y, row.w, row.h, with_alpha(th.text, 8));
    const float ty = row.y + (row_h - 15) / 2;
    const bool missing = file_missing(t);
    for (int di = 0; di < kMaxDecks; ++di) {
      if (views_[di].loaded && views_[di].track_id == t.id)
        dl_.text(row.x + 6 + 9.0f * static_cast<float>(di), ty + 1, deck_letter(di), 10, deck_color(di), Align::Left, 0, 1);
    }
    if (missing) dl_.text(row.x + badge - 12, ty, "!", 12, th.warn, Align::Left, 0, 1);
    bool cell_click = false;  // a click that belongs to a cell (rating, colour) does not select the row
    for (std::size_t c = 0; c < cols.size(); ++c) {
      const std::string id = cols[c].def->id;
      const float cw = x1[c] - x0[c];
      const Color main = missing ? with_alpha(th.text, 130) : th.text, soft = missing ? with_alpha(th.dim, 130) : th.dim;
      if (id == "title") dl_.text(x0[c], ty, skin_->fit(t.title, 12, 0, cw - 10), 12, main, Align::Left, cw - 10);
      else if (id == "artist") dl_.text(x0[c], ty, skin_->fit(t.artist, 12, 0, cw - 10), 12, soft, Align::Left, cw - 10);
      else if (id == "album") dl_.text(x0[c], ty, skin_->fit(t.album, 12, 0, cw - 10), 12, soft, Align::Left, cw - 10);
      else if (id == "genre") dl_.text(x0[c], ty, skin_->fit(t.genre, 12, 0, cw - 8), 12, soft, Align::Left, cw - 8);
      else if (id == "comment") dl_.text(x0[c], ty, skin_->fit(t.comment, 12, 0, cw - 10), 12, soft, Align::Left, cw - 10);
      else if (id == "bpm" && t.bpm > 0) dl_.text(x1[c] - 8, ty, fmt("%.1f", t.bpm), 12, main, Align::Right);
      else if (id == "key") dl_.text(x0[c] + 4, ty, display_key(t.key, key_musical_), 12, soft, Align::Left, cw - 6);
      else if (id == "time" && t.duration_ms > 0) dl_.text(x1[c] - 6, ty, clock_text(t.duration_ms / 1000.0), 12, soft, Align::Right);
      else if (id == "added") dl_.text(x0[c] + 4, ty, date_only(t.date_added), 12, soft, Align::Left, cw - 6);
      else if (id == "plays" && t.play_count > 0) dl_.text(x1[c] - 8, ty, std::to_string(t.play_count), 12, soft, Align::Right);
      else if (id == "played" && t.last_played_ms > 0) dl_.text(x0[c] + 4, ty, date_only(t.last_played_ms / 1000), 12, soft, Align::Left, cw - 6);
      else if (id == "format") dl_.text(x0[c] + 4, ty, upper(t.format), 12, soft, Align::Left, cw - 6);
      else if (id == "bitrate" && t.bitrate > 0) dl_.text(x1[c] - 8, ty, std::to_string(t.bitrate), 12, soft, Align::Right);
      else if (id == "rating") {  // five dots: click one to set the rating, click the last lit one again to clear
        const float dw = std::min(15.0f, (cw - 8) / 5.0f), cy = row.y + row_h / 2;
        for (int k = 1; k <= 5; ++k) {
          const float cx = x0[c] + 6 + dw * (static_cast<float>(k) - 0.5f);
          dl_.circle(cx, cy, k <= t.rating ? 3.2f : 1.8f, k <= t.rating ? th.warn : with_alpha(th.dim, 150));
        }
        const Rect cell{x0[c], row.y, dw * 5 + 8, row_h};
        if (in_.pressed && cell.contains(in_.x, in_.y) && body.contains(in_.x, in_.y)) {
          const int k = std::clamp(static_cast<int>(std::ceil((in_.x - x0[c] - 6) / dw)), 1, 5);
          TrackEdit e;
          e.rating = k == t.rating ? k - 1 : k;
          lib_->edit_track(t.id, e);
          rows_dirty_ = true;
          cell_click = true;
        }
      } else if (id == "color") {  // click to cycle the colour tag through the palette and back to none
        const Rect sw{x0[c] + 6, row.y + 3, std::min(cw - 12, 26.0f), row_h - 6};
        if (t.color > 0) dl_.rrect(sw.x, sw.y, sw.w, sw.h, 3, palette_color(t.color));
        else dl_.rrect_outline(sw.x, sw.y, sw.w, sw.h, 3, with_alpha(th.dim, 120), 1);
        if (in_.pressed && sw.contains(in_.x, in_.y) && body.contains(in_.x, in_.y)) {
          int next = 0;
          if (t.color > 0) {
            int cur = -1;
            for (int p = 0; p < kPaletteSize; ++p)
              if (static_cast<int>(kPalette[p]) == t.color) cur = p;
            next = cur >= 0 && cur + 1 < kPaletteSize ? static_cast<int>(kPalette[cur + 1]) : (cur < 0 ? static_cast<int>(kPalette[0]) : 0);
          } else {
            next = static_cast<int>(kPalette[0]);
          }
          lib_->set_track_color(t.id, next);
          rows_dirty_ = true;
          cell_click = true;
        }
      }
    }
    if (in_.pressed && !cell_click && row.contains(in_.x, in_.y) && body.contains(in_.x, in_.y) && col_drag_ < 0) {
      focus_tree_ = false;
      rdrag_ = {};
      rdrag_.row = i;
      rdrag_.x0 = in_.x;
      rdrag_.y0 = in_.y;
      if (in_.shift) {  // range from the anchor
        multi_.clear();
        for (int k = std::min(anchor_, i); k <= std::max(anchor_, i); ++k) multi_.insert(k);
      } else if (cmd) {  // toggle one
        if (!multi_.erase(i)) multi_.insert(i);
        anchor_ = i;
      } else {
        if (multi_.count(i) == 0) {
          multi_.clear();
          multi_.insert(i);
        } else if (multi_.size() > 1) {
          rdrag_.collapse = true;  // pressing inside a selection keeps it for a drag; a plain click collapses it on release
        }
        anchor_ = i;
      }
      sel_ = i;
      rdrag_.pending = true;
      if (in_.clicks >= 2 && !in_.shift && !cmd) {
        multi_ = {i};
        load_deck = views_[0].playing ? 1 : 0;
        rdrag_ = {};
      }
    }
  }
  if (rows_.empty()) {
    const std::string msg = search_.empty() ? T("browser.empty") : T("browser.no_matches");
    dl_.text(body.x + body.w / 2, body.y + 40, skin_->fit(msg, 13, 0, body.w - 20), 13, th.dim, Align::Center);
  }

  // Dragging rows: onto a playlist in the tree (handled there), or to another place in the playlist being viewed.
  int drop_at = -1;
  if (rdrag_.pending) {
    if (in_.down) {
      if (!rdrag_.active && std::fabs(in_.x - rdrag_.x0) + std::fabs(in_.y - rdrag_.y0) > 7) rdrag_.active = true;
    } else {
      if (rdrag_.active && body.contains(in_.x, in_.y) && view_is_editable_playlist()) {
        drop_at = std::clamp(static_cast<int>(std::floor((in_.y - body.y + scroll_) / row_h + 0.5f)), 0, static_cast<int>(rows_.size()));
      } else if (!rdrag_.active && rdrag_.collapse) {
        multi_ = {rdrag_.row};
      }
      if (!rdrag_.active || drop_at < 0) rdrag_.pending = false;
    }
  }
  if (rdrag_.active && in_.down && body.contains(in_.x, in_.y) && view_is_editable_playlist()) {  // insertion marker
    const int at = std::clamp(static_cast<int>(std::floor((in_.y - body.y + scroll_) / row_h + 0.5f)), 0, static_cast<int>(rows_.size()));
    dl_.rect(body.x, body.y + static_cast<float>(at) * row_h - scroll_ - 1, body.w, 2, th.accent);
  }
  dl_.pop_clip();
  if (drop_at >= 0) {  // move the selected rows so they sit before row `drop_at`
    std::vector<std::int64_t> ids = lib_->playlist_track_ids(playlist_id_);
    std::vector<std::int64_t> moved, rest;
    int before = 0;
    for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
      if (multi_.count(i)) {
        moved.push_back(ids[static_cast<std::size_t>(i)]);
        if (i < drop_at) ++before;
      } else {
        rest.push_back(ids[static_cast<std::size_t>(i)]);
      }
    }
    const int at = std::clamp(drop_at - before, 0, static_cast<int>(rest.size()));
    rest.insert(rest.begin() + at, moved.begin(), moved.end());
    lib_->set_playlist_tracks(playlist_id_, rest);
    multi_.clear();
    for (int k = 0; k < static_cast<int>(moved.size()); ++k) multi_.insert(at + k);
    sel_ = at;
    anchor_ = at;
    rdrag_ = {};
    rows_dirty_ = true;
  }
  if (rdrag_.active && in_.down) {  // the thing being dragged follows the pointer
    const std::string label = T("browser.drag_rows", {{"count", static_cast<int>(std::max<std::size_t>(1, multi_.size()))}});
    const float lw = dl_.text_width(label, 11, 1) + 16;
    dl_.rrect(in_.x + 12, in_.y + 8, lw, 22, 5, with_alpha(th.accent, 230));
    dl_.text(in_.x + 20, in_.y + 12, label, 11, th.bg, Align::Left, 0, 1);
  }
  if (content > body.h) {  // scroll bar
    const float bar_h = std::max(24.0f, body.h * body.h / content);
    const float bar_y = body.y + (body.h - bar_h) * scroll_ / (content - body.h);
    dl_.rect(body.x + body.w - 5, bar_y, 4, bar_h, th.line);
  }
  if (load_deck >= 0 && sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) {
    request_load(load_deck, rows_.id(static_cast<std::size_t>(sel_)));  // does not touch browse state
  }
}

// ---- columns panel ---------------------------------------------------------------------------------------------------------
void App::draw_columns_panel(const Rect& m) {
  const ui::Theme& th = theme_;
  float y = m.y + 46;
  const float x = m.x + 18;
  bool changed = false;
  // shown columns first (in order, with arrows), then the hidden ones
  std::vector<std::string> order = bcols_;
  for (const ColDef& c : kCols)
    if (std::find(order.begin(), order.end(), c.id) == order.end()) order.push_back(c.id);
  const float row_h = std::min(26.0f, (m.h - 100) / static_cast<float>(kColCount));
  for (std::size_t i = 0; i < order.size(); ++i) {
    const std::string& id = order[i];
    const bool shown = std::find(bcols_.begin(), bcols_.end(), id) != bcols_.end();
    const std::size_t pos = static_cast<std::size_t>(std::find(bcols_.begin(), bcols_.end(), id) - bcols_.begin());
    if (ui_toggle({x, y, 200, row_h - 2}, T("browser.col." + id), shown)) {
      if (shown) {
        if (bcols_.size() > 1) bcols_.erase(bcols_.begin() + static_cast<std::ptrdiff_t>(pos));
      } else {
        bcols_.push_back(id);
      }
      changed = true;
    }
    if (shown) {
      if (pos > 0 && ui_button({x + 230, y, 26, row_h - 2}, "\xE2\x86\x91", false)) {
        std::swap(bcols_[pos], bcols_[pos - 1]);
        changed = true;
      }
      if (pos + 1 < bcols_.size() && ui_button({x + 260, y, 26, row_h - 2}, "\xE2\x86\x93", false)) {
        std::swap(bcols_[pos], bcols_[pos + 1]);
        changed = true;
      }
      if (ui_button({x + 296, y, 26, row_h - 2}, "-", false) || ui_button({x + 326, y, 26, row_h - 2}, "+", false)) {
        const ColDef* d = col_def(id);
        const bool plus = ov_.x > x + 320;
        float& w = bwidth_.emplace(id, d->w).first->second;
        w = d->flex ? std::clamp(w * (plus ? 1.15f : 0.87f), 0.2f, 4.0f) : std::clamp(w + (plus ? 10.0f : -10.0f), 36.0f, 400.0f);
        changed = true;
      }
    }
    y += row_h;
  }
  y += 6;
  if (ui_toggle({x, y, 260, 24}, T("settings.key_musical"), key_musical_)) {
    key_musical_ = !key_musical_;
    lib_->set_setting("browser.key_musical", key_musical_ ? "1" : "0");
  }
  if (changed) save_browser_settings();
}

// ---- smart playlist editor -----------------------------------------------------------------------------------------------------
namespace {
const char* const kRuleNames[] = {"text", "bpm", "key", "genre", "rating", "color", "added", "plays", "playlist", "missing"};
constexpr int kEditableFields = 10;
}  // namespace

void App::open_smart_editor(std::int64_t id) {
  sm_rules_.clear();
  sm_id_ = id;
  sm_all_ = true;
  sm_confirm_ = false;
  f_sm_name_.text = T("smart.new_name");
  if (id) {
    if (const auto p = lib_->smart_playlist(id)) {
      f_sm_name_.text = p->name;
      sm_all_ = p->match_all;
      for (const SmartRule& r : p->rules) {
        if (r.field == SmartRule::Field::BpmNear) continue;
        RuleEdit e;
        e.rule = r;
        switch (r.field) {
          case SmartRule::Field::Text: case SmartRule::Field::Genre: case SmartRule::Field::Key: e.a.text = r.text; break;
          case SmartRule::Field::Bpm: e.a.text = r.lo > 0 ? fmt("%g", r.lo) : ""; e.b.text = r.hi > 0 ? fmt("%g", r.hi) : ""; break;
          case SmartRule::Field::AddedDays: e.a.text = fmt("%g", r.lo); break;
          case SmartRule::Field::Plays: e.a.text = fmt("%g", r.lo); e.b.text = r.hi >= 0 ? fmt("%g", r.hi) : ""; break;
          default: break;
        }
        sm_rules_.push_back(std::move(e));
      }
    }
  } else {
    RuleEdit e;
    e.rule.field = SmartRule::Field::Genre;
    sm_rules_.push_back(std::move(e));
  }
  open_panel(Panel::Smart);
  focus_ = &f_sm_name_;
}

void App::save_smart_editor() {
  SmartPlaylist p;
  p.id = sm_id_;
  p.name = trim_copy(f_sm_name_.text);
  if (p.name.empty()) p.name = T("smart.new_name");
  p.match_all = sm_all_;
  for (const RuleEdit& e : sm_rules_) {
    SmartRule r = e.rule;
    const auto num = [](const Field& f, double def) { return f.text.empty() ? def : std::atof(f.text.c_str()); };
    switch (r.field) {
      case SmartRule::Field::Text: case SmartRule::Field::Genre: case SmartRule::Field::Key:
        r.text = trim_copy(e.a.text);
        if (r.text.empty()) continue;  // an empty text rule matches everything: leave it out
        break;
      case SmartRule::Field::Bpm: r.lo = num(e.a, 0); r.hi = num(e.b, 0); break;
      case SmartRule::Field::AddedDays: r.lo = num(e.a, 30); break;
      case SmartRule::Field::Plays: r.lo = num(e.a, 0); r.hi = e.b.text.empty() ? -1 : num(e.b, -1); break;
      default: break;
    }
    p.rules.push_back(std::move(r));
  }
  if (p.id) lib_->update_smart_playlist(p);
  else p.id = lib_->create_smart_playlist(p.name, p.match_all, p.rules);
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    view_ = TreeItem::Smart;
    playlist_id_ = p.id;
    sel_ = 0;
    scroll_ = 0;
  }
  rows_dirty_ = true;
  status_note_ = T("smart.saved", {{"name", p.name}});
}

void App::draw_smart_editor(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 18;
  float y = m.y + 46;
  dl_.text(x, y + 5, T("smart.name"), 12, th.dim, Align::Left, 60);
  ui_field({x + 64, y, m.w - 36 - 64 - 130, 26}, f_sm_name_, T("smart.new_name"));
  if (ui_button({m.x + m.w - 18 - 126, y, 126, 26}, sm_all_ ? T("smart.match_all") : T("smart.match_any"), false)) sm_all_ = !sm_all_;
  y += 40;
  std::vector<std::pair<std::int64_t, std::string>> lists;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    for (const TreeItem& item : tree_)
      if (item.kind == TreeItem::Playlist) lists.emplace_back(item.id, item.name);
  }
  int remove = -1;
  const float row_h = 32;
  for (std::size_t i = 0; i < sm_rules_.size(); ++i) {
    RuleEdit& e = sm_rules_[i];
    SmartRule& r = e.rule;
    const int fi = std::min(static_cast<int>(r.field), kEditableFields - 1);
    const float ry = y + static_cast<float>(i) * row_h;
    if (ry + 28 > m.y + m.h - 70) break;
    if (ui_button({x, ry, 112, 26}, T(std::string("smart.rule.") + kRuleNames[fi]), false)) {  // next kind of rule
      r = SmartRule{};
      r.field = static_cast<SmartRule::Field>((fi + 1) % kEditableFields);
      if (r.field == SmartRule::Field::Rating) r.lo = 4;
      if (r.field == SmartRule::Field::AddedDays) r.lo = 30;
      if (r.field == SmartRule::Field::Plays) r.hi = -1;
      if (r.field == SmartRule::Field::InPlaylist && !lists.empty()) r.id = lists[0].first;
      e.a.text.clear();
      e.b.text.clear();
      if (r.field == SmartRule::Field::AddedDays) e.a.text = "30";
    }
    if (ui_button({x + 118, ry, 46, 26}, T("smart.not"), r.negate, th.warn)) r.negate = !r.negate;
    const float ex = x + 172, ew = m.w - 36 - 172 - 34;
    switch (r.field) {
      case SmartRule::Field::Text: case SmartRule::Field::Genre: case SmartRule::Field::Key:
        ui_field({ex, ry, ew, 26}, e.a, r.field == SmartRule::Field::Key ? T("smart.hint_keys") : T("smart.hint_text"));
        break;
      case SmartRule::Field::Bpm:
        ui_field({ex, ry, 80, 26}, e.a, T("smart.min"));
        dl_.text(ex + 90, ry + 6, T("smart.to"), 12, th.dim);
        ui_field({ex + 116, ry, 80, 26}, e.b, T("smart.max"));
        break;
      case SmartRule::Field::AddedDays:
        ui_field({ex, ry, 80, 26}, e.a, "30");
        dl_.text(ex + 90, ry + 6, T("smart.days"), 12, th.dim);
        break;
      case SmartRule::Field::Plays:
        ui_field({ex, ry, 70, 26}, e.a, T("smart.min"));
        dl_.text(ex + 78, ry + 6, T("smart.to"), 12, th.dim);
        ui_field({ex + 104, ry, 70, 26}, e.b, T("smart.max"));
        dl_.text(ex + 184, ry + 6, T("smart.plays_hint"), 11, th.dim, Align::Left, ew - 184);
        break;
      case SmartRule::Field::Rating:
        for (int k = 1; k <= 5; ++k)
          if (ui_button({ex + static_cast<float>(k - 1) * 32, ry, 28, 26}, std::to_string(k), k <= static_cast<int>(r.lo), th.warn)) r.lo = k;
        dl_.text(ex + 170, ry + 6, T("smart.or_more"), 12, th.dim);
        break;
      case SmartRule::Field::Color: {
        if (ui_button({ex, ry, 40, 26}, T("smart.none"), r.lo <= 0)) r.lo = 0;
        for (int p = 0; p < kPaletteSize; ++p) {
          const Rect sw{ex + 46 + static_cast<float>(p) * 20, ry + 3, 17, 20};
          dl_.rrect(sw.x, sw.y, sw.w, sw.h, 3, palette_color(static_cast<int>(kPalette[p])));
          if (static_cast<int>(r.lo) == static_cast<int>(kPalette[p])) dl_.rrect_outline(sw.x - 2, sw.y - 2, sw.w + 4, sw.h + 4, 4, th.text, 2);
          if (ov_.pressed && sw.contains(ov_.x, ov_.y)) r.lo = static_cast<double>(kPalette[p]);
        }
        break;
      }
      case SmartRule::Field::InPlaylist: {
        std::string name = T("smart.none");
        std::size_t at = 0;
        for (std::size_t k = 0; k < lists.size(); ++k)
          if (lists[k].first == r.id) {
            name = lists[k].second;
            at = k;
          }
        if (ui_button({ex, ry, std::min(260.0f, ew), 26}, name, false) && !lists.empty()) r.id = lists[(at + 1) % lists.size()].first;
        break;
      }
      case SmartRule::Field::Missing:
        dl_.text(ex, ry + 6, T(r.negate ? "smart.file_present" : "smart.file_missing"), 12, th.text);
        break;
      default: break;
    }
    if (ui_button({m.x + m.w - 18 - 26, ry, 26, 26}, "x", false)) remove = static_cast<int>(i);
  }
  if (remove >= 0) sm_rules_.erase(sm_rules_.begin() + remove);
  const float by = m.y + m.h - 46;
  if (sm_rules_.size() < 10 && ui_button({x, by, 120, 28}, T("smart.add_rule"), false)) {
    RuleEdit e;
    e.rule.field = SmartRule::Field::Text;
    sm_rules_.push_back(std::move(e));
  }
  if (ui_button({x + 130, by, 110, 28}, T("smart.save"), true, th.good)) {
    save_smart_editor();
    close_panel();
    return;
  }
  if (sm_id_ != 0) {
    const bool sure = sm_confirm_;
    if (ui_button({m.x + m.w - 18 - 150, by, 150, 28}, sure ? T("smart.delete_confirm") : T("smart.delete"), sure, th.warn)) {
      if (sure) {
        lib_->delete_smart_playlist(sm_id_);
        {
          std::lock_guard<std::mutex> lock(browse_mutex_);
          if (view_ == TreeItem::Smart && playlist_id_ == sm_id_) {
            view_ = TreeItem::Collection;
            playlist_id_ = 0;
          }
        }
        rows_dirty_ = true;
        status_note_ = T("smart.deleted");
        close_panel();
        return;
      }
      sm_confirm_ = true;
    }
  }
}

// ---- pre-listen ---------------------------------------------------------------------------------------------------------------
// Plays a library track in the headphones without loading it to a deck. With no headphone output the preview stays off
// (unless "preview on master" is on in the settings): a DJ must never blast the room by accident.
bool App::preview_start(std::int64_t track_id, bool play, std::string& error) {
  if (track_id == 0) {
    error = "no such track";
    return false;
  }
  preview_master_ = lib_->setting("preview.master", "0") == "1";  // also changed through the settings API
  if (engine_->output_channels() < 4 && !preview_master_) {
    error = T("preview.no_cue_bus");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    preview_.loading = true;
  }
  Job j;
  j.kind = Job::PreviewLoad;
  j.track_id = track_id;
  j.flag = play;
  post(std::move(j));
  return true;
}

void App::preview_toggle() {
  std::int64_t id = 0;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) id = rows_.id(static_cast<std::size_t>(sel_));
  }
  const PreviewView pv = engine_->preview();
  Command c;
  if (pv.loaded && (id == 0 || pv.track_id == id)) {  // the loaded one: play / pause
    c.op = Op::PreviewPlay;
    c.i = -1;
    engine_->send(c);
    return;
  }
  std::string error;
  if (!preview_start(id, true, error)) status_note_ = error.empty() ? std::string() : (error == "no such track" ? error : error);
}

void App::load_preview(Library& lib, std::int64_t track_id, bool play) {
  const auto row = lib.track(track_id);
  const auto fail = [&](const std::string& why) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      preview_.loading = false;
    }
    set_status(why);
  };
  if (!row) return fail(T("status.error", {{"message", "no such track"}}));
  std::string error;
  const std::shared_ptr<TrackAudio> audio = decode_file(row->path, error);
  if (!audio) return fail(std::filesystem::exists(row->path) ? T("status.error", {{"message", error}}) : T("deck.file_not_found", {{"path", row->path}}));
  PreviewInfo info;
  info.id = track_id;
  info.title = row->artist.empty() ? row->title : row->artist + " - " + row->title;
  const std::size_t columns = 600, points = audio->wave.size();
  info.overview.resize(columns);
  for (std::size_t c = 0; c < columns && points > 0; ++c) {
    const std::size_t a = c * points / columns, b = std::max(a + 1, (c + 1) * points / columns);
    WavePoint p;
    for (std::size_t i = a; i < b && i < points; ++i) {
      p.low = std::max(p.low, audio->wave[i].low);
      p.mid = std::max(p.mid, audio->wave[i].mid);
      p.high = std::max(p.high, audio->wave[i].high);
    }
    info.overview[c] = p;
  }
  auto t = std::make_unique<LoadedTrack>();
  t->audio = audio;
  t->track_id = track_id;
  t->path = row->path;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    preview_ = std::move(info);
  }
  Command m;
  m.op = Op::PreviewMaster;
  m.i = preview_master_ ? 1 : 0;
  engine_->send(m);
  engine_->preview_load(std::move(t));
  if (play) {
    Command c;
    c.op = Op::PreviewPlay;
    c.i = 1;
    engine_->send(c);
  }
}

void App::draw_preview_strip(const Rect& ov, Color base) {
  const ui::Theme& th = theme_;
  dl_.rect(ov.x, ov.y, ov.w, ov.h, rgba(12, 13, 16, 160));
  const PreviewView pv = engine_->preview();
  std::vector<WavePoint> wave;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (pv.loaded && preview_.id == pv.track_id) wave = preview_.overview;
  }
  if (!pv.loaded || wave.empty() || pv.length <= 0) {
    dl_.text(ov.x + ov.w / 2, ov.y + (ov.h - 13) / 2, skin_->fit(T("preview.hint"), 11, 0, ov.w - 16), 11, th.dim, Align::Center, ov.w - 16);
    return;
  }
  const float played = static_cast<float>(pv.position / pv.length) * ov.w;
  const float cw = ov.w / static_cast<float>(wave.size());
  for (std::size_t i = 0; i < wave.size(); ++i) {
    const WavePoint& p = wave[i];
    const float x = ov.x + static_cast<float>(i) * cw;
    const float h = std::max(1.0f, ov.h * std::max(p.low, std::max(p.mid, p.high)) / 255.0f);
    dl_.rect(x, ov.y + (ov.h - h) / 2, cw + 0.5f, h, x - ov.x < played ? mix(base, th.bg, 0.6f) : base);
  }
  dl_.rect(ov.x + played - 1, ov.y - 1, 2, ov.h + 2, rgba(255, 255, 255));
  if (clicked(ov)) drag_deck_ = 100;  // click or drag to seek
  if (drag_deck_ == 100 && in_.down) {
    Command c;
    c.op = Op::PreviewSeek;
    c.d = std::clamp((in_.x - ov.x) / ov.w, 0.0f, 1.0f) * pv.length;
    engine_->send(c);
  }
  if (!in_.down && drag_deck_ == 100) drag_deck_ = -1;
}

}  // namespace deckhand
