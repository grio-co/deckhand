// Keyboard shortcuts in the application: feeding key events to the keymap, carrying out what it asks for (named controls and
// "ui.*" actions), and the shortcuts panel (search, rebind, conflicts, reset, cheat sheet).
#include "app_impl.hpp"

#include <cctype>
#include <cstring>

namespace deckhand {

namespace {

std::string lower_s(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string utf8_of(std::uint32_t cp) {
  std::string s;
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s += static_cast<char>(0xF0 | (cp >> 18));
    s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return s;
}

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  return o + "\"";
}

// "deck2.hotcue.3" -> deck 1, slot 3 etc. for ui actions named <word>.<number>
bool trailing_number(const std::string& n, const char* prefix, int& out) {
  const std::size_t len = std::strlen(prefix);
  if (n.compare(0, len, prefix) != 0 || n.size() == len) return false;
  char* end = nullptr;
  const long v = std::strtol(n.c_str() + len, &end, 10);
  if (!end || *end != 0 || v < 1 || v > kMaxDecks) return false;
  out = static_cast<int>(v);
  return true;
}

}  // namespace

// ---- loading and saving -----------------------------------------------------------------------------------------------

void App::load_keymap() {
  keymap_user_path_ = data_dir_ + "/keymap.dhkeys";
  const std::string def = resource_dir_.empty() ? std::string() : resource_dir_ + "/keymaps/default.dhkeys";
  std::vector<std::string> warnings;
  {
    std::lock_guard<std::mutex> lock(keymap_mutex_);
    warnings = keymap_.load_files(def, keymap_user_path_);
    if (keymap_.bindings().empty()) DH_LOG(Warn, "keymap: no shortcuts loaded (default file: %s)", def.c_str());
  }
  keymap_warnings_.clear();
  for (const std::string& w : warnings) {
    DH_LOG(Warn, "keymap: %s", w.c_str());
    keymap_warnings_ += w + "\n";
  }
}

bool App::save_keymap() {
  std::string error;
  bool ok;
  {
    std::lock_guard<std::mutex> lock(keymap_mutex_);
    ok = keymap_.save_user(keymap_user_path_, error);
  }
  if (!ok) {
    DH_LOG(Warn, "keymap: %s", error.c_str());
    status_note_ = T("status.error", {{"message", error}});
  }
  return ok;
}

std::string App::keymap_json() {
  std::lock_guard<std::mutex> lock(keymap_mutex_);
  std::string o = "{\"user_file\":" + json_str(keymap_user_path_) + ",\"bindings\":[";
  bool first = true;
  for (const KeyBinding& b : keymap_.bindings()) {
    if (!first) o += ",";
    first = false;
    char num[40];
    o += "{\"chord\":" + json_str(chord_text(b.chord)) + ",\"key\":" + json_str(b.chord.key) + ",\"mods\":" + std::to_string(b.chord.mods) +
         ",\"context\":" + json_str(key_context_name(b.context)) + ",\"control\":" + json_str(b.action.control) + ",\"mode\":" +
         json_str(key_mode_name(b.action.mode));
    if (b.action.has_value) {
      std::snprintf(num, sizeof num, "%g", b.action.value);
      o += std::string(",\"value\":") + num;
    }
    o += ",\"desc\":" + json_str(b.desc) + ",\"user\":" + (b.user ? "true" : "false") + "}";
  }
  o += "],\"warnings\":[";
  std::size_t at = 0;
  first = true;
  while (at < keymap_warnings_.size()) {
    std::size_t nl = keymap_warnings_.find('\n', at);
    if (nl == std::string::npos) nl = keymap_warnings_.size();
    if (nl > at) {
      if (!first) o += ",";
      first = false;
      o += json_str(keymap_warnings_.substr(at, nl - at));
    }
    at = nl + 1;
  }
  return o + "]}";
}

bool App::keymap_set_user(const std::string& text, std::vector<std::string>& warnings, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(keymap_mutex_);
    warnings = keymap_.load_user(text, "keymap.dhkeys");
  }
  keys_ui_.note.clear();
  if (!save_keymap()) {
    error = "cannot write the keymap file";
    return false;
  }
  return true;
}

// ---- key events -------------------------------------------------------------------------------------------------------------

bool App::text_has_keyboard() {
  if (focus_) return true;
  std::lock_guard<std::mutex> lock(browse_mutex_);
  return search_focus_ || rename_id_ != 0;
}

bool App::key_event(std::uint32_t physical, std::uint32_t character, std::uint32_t mods, bool down, bool repeat) {
  mods_ = mods;
  if (down) swallow_text_.clear();
  KeyEvent ev;
  ev.physical = physical;
  ev.character = character;
  ev.mods = mods;
  ev.down = down;
  ev.repeat = repeat;
  if (panel_ == Panel::Shortcuts && !keys_ui_.capture_control.empty()) {  // rebinding: the next key is the new shortcut
    if (!down) return true;
    const KeyChord c = Keymap::chord_of(ev);
    if (c.key == "escape" && c.mods == 0) keys_ui_.capture_control.clear();
    else if (!c.key.empty() && !repeat) keys_commit_capture(c);
    return true;
  }
  if (down && !text_has_keyboard() && ev.mods == 0 && (panel_ == Panel::Suggest || panel_ == Panel::PrepMode)) {
    const KeyChord c = Keymap::chord_of(ev);  // the panel's own keys (listed at its foot)
    if (panel_ == Panel::Suggest ? suggest_key(c.key) : prep_key(c.key)) return true;
  }
  const KeyFocus focus = text_has_keyboard() ? KeyFocus::Text : panel_ != Panel::None ? KeyFocus::Panel : KeyFocus::Browser;
  std::vector<KeyCall> calls;
  bool taken;
  {
    std::lock_guard<std::mutex> lock(keymap_mutex_);
    taken = keymap_.handle(ev, focus, calls);
  }
  for (const KeyCall& c : calls) run_key_call(c);
  if (taken && down && character) swallow_text_ = utf8_of(character);
  return taken;
}

void App::key_release_all() {
  std::vector<KeyCall> calls;
  {
    std::lock_guard<std::mutex> lock(keymap_mutex_);
    keymap_.release_all(calls);
  }
  for (const KeyCall& c : calls) run_key_call(c);
}

void App::run_key_call(const KeyCall& c) {
  if (c.control.compare(0, 3, "ui.") == 0) {
    do_ui_action(c.control, c.has_value, c.value);
    return;
  }
  int deck = 0, slot = 0;
  std::string action, error;
  if (parse_automix_control(c.control, action)) {
    if (!automix_control(action, c.has_value, c.value, error) && !error.empty()) status_note_ = error;
    return;
  }
  if (parse_lighting_control(c.control, action)) {
    if (!lighting_ || !lighting_->control(action, c.has_value, c.value, error)) status_note_ = error.empty() ? std::string("lighting is not available") : error;
    return;
  }
  if (parse_prep_control(c.control, deck, action, slot)) {
    PrepArgs a;
    a.has_value = c.has_value;
    a.value = c.value;
    a.slot = slot;
    if (!prep(deck, action, a, error) && !error.empty()) status_note_ = error;
    return;
  }
  if (!apply_control(*engine_, c.control, c.has_value, c.value, error)) status_note_ = error;
}

void App::do_ui_action(const std::string& name, bool has_value, double value) {
  const std::string n = name.substr(3);
  int deck = 0;
  const auto action = [&](const char* id, int d = -1) {
    ui::Target t;
    t.type = ui::Target::Type::Action;
    t.name = id;
    t.deck = d;
    do_action(t);
  };
  const auto selected_id = [&]() -> std::int64_t {  // browse_mutex_ is taken here
    std::lock_guard<std::mutex> lock(browse_mutex_);
    return sel_ >= 0 && sel_ < static_cast<int>(rows_.size()) ? rows_.id(static_cast<std::size_t>(sel_)) : 0;
  };
  if (trailing_number(n, "load.", deck)) {
    const std::int64_t id = selected_id();
    if (id) request_load(deck - 1, id);
  } else if (n == "load_free") {
    const std::int64_t id = selected_id();
    if (id) request_load(engine_->deck(0).playing ? 1 : 0, id);
  } else if (n == "browse_up") {
    browse(-1);
  } else if (n == "browse_down") {
    browse(1);
  } else if (n == "browse_page_up") {
    browse(-10);
  } else if (n == "browse_page_down") {
    browse(10);
  } else if (n == "browse_top") {
    browse(-1000000);
  } else if (n == "browse_bottom") {
    browse(1000000);
  } else if (n == "browse_switch") {
    browse_press();
  } else if (n == "playlist_next" || n == "playlist_prev") {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    const int step = n == "playlist_next" ? 1 : -1;
    for (int i = tree_sel_ + step; i >= 0 && i < static_cast<int>(tree_.size()); i += step) {
      const TreeItem::Kind k = tree_[static_cast<std::size_t>(i)].kind;
      if (k == TreeItem::Playlist || k == TreeItem::Smart || k == TreeItem::Collection) {
        tree_sel_ = i;
        apply_tree_selection();
        break;
      }
    }
  } else if (n == "focus_search") {
    if (panel_ != Panel::None) close_panel();
    std::lock_guard<std::mutex> lock(browse_mutex_);
    search_focus_ = true;
  } else if (n == "clear_search") {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    search_.clear();
    search_focus_ = false;
    sel_ = 0;
    rows_dirty_ = true;
  } else if (n == "preview_toggle") {
    preview_toggle();
  } else if (n == "preview_stop") {
    Command c;
    c.op = Op::PreviewStop;
    engine_->send(c);
  } else if (n == "preview_seek") {
    const PreviewView pv = engine_->preview();
    if (pv.loaded && has_value) {
      std::string error;
      const double now_s = pv.position / std::max(1.0, pv.sample_rate);
      apply_control(*engine_, "preview.seek", true, std::max(0.0, now_s + value), error);
    }
  } else if (n == "queue_add" || n == "queue_play_next") {
    queue_selected(n == "queue_play_next");
  } else if (n == "add_to_prepare") {
    std::int64_t target = 0;
    for (const PlaylistNode& p : lib_->playlists())
      if (!p.is_folder && lower_s(p.name) == "prepare") {
        target = p.id;
        break;
      }
    std::vector<std::int64_t> ids;
    {
      std::lock_guard<std::mutex> lock(browse_mutex_);
      ids = selected_ids();
    }
    if (!target) {
      status_note_ = T("keys.no_prepare");
    } else if (!ids.empty()) {
      for (const std::int64_t id : ids) lib_->add_to_playlist(target, id);
      rows_dirty_ = true;
      status_note_ = T("keys.added_prepare", {{"count", static_cast<double>(ids.size())}});
    }
  } else if (n == "add_to") {
    action("add_to");
  } else if (n == "rate" || n == "rate_up" || n == "rate_down") {
    std::vector<std::int64_t> ids;
    int current = 0;
    {
      std::lock_guard<std::mutex> lock(browse_mutex_);
      ids = selected_ids();
      if (sel_ >= 0 && sel_ < static_cast<int>(rows_.size())) current = rows_[static_cast<std::size_t>(sel_)].rating;
    }
    const int stars = n == "rate" ? static_cast<int>(std::lround(value)) : n == "rate_up" ? current + 1 : current - 1;
    TrackEdit e;
    e.rating = std::clamp(stars, 0, 5);
    for (const std::int64_t id : ids) lib_->edit_track(id, e);
    if (!ids.empty()) rows_dirty_ = true;
  } else if (n == "undo_edit") {
    for (const TrackEditRecord& r : lib_->recent_edits(20)) {
      if (r.source == "undo") continue;
      const UndoResult u = lib_->undo_batch(r.batch_id);
      status_note_ = u.status == UndoStatus::Ok ? T("keys.undone") : T("keys.undo_failed");
      rows_dirty_ = true;
      break;
    }
  } else if (n == "edit_track") {
    action("edit_track");
  } else if (n == "compatible") {
    action("compatible");
  } else if (n == "settings") {
    action("settings");
  } else if (n == "controller") {
    if (panel_ == Panel::Controller) close_panel();
    else open_panel(Panel::Controller);
  } else if (n == "shortcuts") {
    if (panel_ == Panel::Shortcuts) close_panel();
    else open_panel(Panel::Shortcuts);
  } else if (n == "suggestions") {
    if (panel_ == Panel::Suggest) close_panel();
    else open_panel(Panel::Suggest);
  } else if (n == "prep") {
    if (panel_ == Panel::PrepMode) close_panel();
    else open_panel(Panel::PrepMode);
  } else if (n == "queue_suggestion") {
    queue_top_suggestion();
  } else if (n == "sampler") {
    action("sampler");
  } else if (n == "learn") {
    action("learn");
  } else if (n == "columns") {
    action("columns");
  } else if (n == "close_panel") {
    key(DH_KEY_ESCAPE, 0);  // closes the panel, or clears the search
  } else if (n == "window.browser") {
    action("window.toggle:browser");
  } else if (n == "window.sampler") {
    action("window.toggle:sampler");
  } else if (n == "layout") {
    action("layout");
  } else if (n == "skin_next") {
    action("skin");
  } else if (n == "cheat_sheet") {
    keys_export_cheat_sheet();
  } else if (trailing_number(n, "nudge.", deck)) {
    if (has_value) {
      Command c;
      c.op = Op::JogTurn;
      c.deck = static_cast<std::uint8_t>(deck - 1);
      c.d = value;
      c.i = 0;  // the side of the platter: pitch bend
      engine_->send(c);
    }
  } else if (trailing_number(n, "key_shift.", deck)) {
    const std::string ctl = "deck" + std::to_string(deck) + ".key_shift";
    double cur = 0;
    std::string error;
    if (has_value && read_control(*engine_, ctl, cur)) apply_control(*engine_, ctl, true, std::clamp(cur + value, -12.0, 12.0), error);
  } else if (trailing_number(n, "tempo_step.", deck)) {
    const std::string ctl = "deck" + std::to_string(deck) + ".tempo";
    double cur = 0;
    std::string error;
    if (has_value && read_control(*engine_, ctl, cur)) apply_control(*engine_, ctl, true, std::clamp(cur + value, -1.0, 1.0), error);
  } else {
    status_note_ = T("keys.unknown_action", {{"name", name}});
  }
}

void App::keys_export_cheat_sheet() {
  std::string html, text;
  {
    std::lock_guard<std::mutex> lock(keymap_mutex_);
    html = keymap_.cheat_sheet(Keymap::Format::Html);
    text = keymap_.cheat_sheet(Keymap::Format::Text);
  }
  const std::string base = data_dir_ + "/shortcuts";
  std::ofstream h(base + ".html", std::ios::binary | std::ios::trunc), t(base + ".txt", std::ios::binary | std::ios::trunc);
  h << html;
  t << text;
  h.flush();
  t.flush();
  status_note_ = h && t ? T("keys.exported", {{"path", base + ".html"}}) : T("status.error", {{"message", base + ".html"}});
  keys_ui_.note = status_note_;
}

// ---- the shortcuts panel -------------------------------------------------------------------------------------------------------

namespace {
struct KeysRow {
  KeyAction action;
  KeyContext context = KeyContext::Global;
  std::string group, label, control, chords;
  bool changed = false;
};
}  // namespace

void App::keys_commit_capture(const KeyChord& chord) {
  const KeyAction a = keys_ui_.capture_action;
  KeyContext ctx = KeyContext::Global;
  {
    std::lock_guard<std::mutex> lock(keymap_mutex_);
    const auto have = keymap_.for_action(a);
    if (!have.empty()) ctx = have.front()->context;
    if (ctx == KeyContext::Text) ctx = KeyContext::Global;
    if (const KeyBinding* other = keymap_.conflict(chord, ctx, a)) {
      keys_ui_.have_pending = true;
      keys_ui_.pending = chord;
      keys_ui_.pending_ctx = ctx;
      keys_ui_.pending_other = other->action.control + (other->desc.empty() ? "" : " (" + other->desc + ")");
      keys_ui_.capture_control.clear();
      return;
    }
    if (!keys_ui_.capture_add) keymap_.unbind_action(a);
    keymap_.bind(chord, ctx, a);
  }
  keys_ui_.capture_control.clear();
  keys_ui_.note = chord_label(chord);
  save_keymap();
}

void App::draw_shortcuts(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 16, rw = m.w - 32;
  ui_field({x, m.y + 44, rw, 26}, keys_ui_.search, T("keys.search"));
  const std::string q = lower_s(keys_ui_.search.text);

  // ---- the rows: every binding's action, then every control that has none
  std::vector<KeysRow> rows;
  {
    std::lock_guard<std::mutex> lock(keymap_mutex_);
    std::set<std::string> bound_controls;
    for (const KeyBinding& b : keymap_.bindings()) {
      bound_controls.insert(b.action.control);
      const auto it = std::find_if(rows.begin(), rows.end(), [&](const KeysRow& r) { return r.action == b.action && r.context == b.context; });
      const std::string label = chord_label(b.chord);
      if (it != rows.end()) {
        it->chords += ", " + label;
        continue;
      }
      KeysRow r;
      r.action = b.action;
      r.context = b.context;
      r.group = key_control_group(b.action.control);
      r.control = b.action.control;
      r.label = b.desc;
      r.chords = label;
      rows.push_back(std::move(r));
    }
    const std::vector<KeyControlInfo> all = list_key_controls();
    std::map<std::string, const KeyControlInfo*> info;
    for (const KeyControlInfo& c : all) info[c.name] = &c;
    for (KeysRow& r : rows) {
      if (r.label.empty()) r.label = info.count(r.control) ? info[r.control]->help : r.control;
      r.changed = keymap_.action_changed(r.action);
    }
    for (const KeyControlInfo& c : all) {
      if (bound_controls.count(c.name)) continue;
      KeysRow r;
      r.action.control = c.name;
      r.action.mode = c.kind == "button" ? KeyMode::Hold : KeyMode::Press;
      r.group = key_control_group(c.name);
      r.control = c.name;
      r.label = c.help;
      r.changed = keymap_.action_changed(r.action);
      rows.push_back(std::move(r));
    }
  }
  std::stable_sort(rows.begin(), rows.end(), [](const KeysRow& a, const KeysRow& b) { return a.group < b.group; });
  if (!q.empty()) {
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](const KeysRow& r) {
                                return lower_s(r.label).find(q) == std::string::npos && lower_s(r.control).find(q) == std::string::npos &&
                                       lower_s(r.chords).find(q) == std::string::npos && lower_s(r.group).find(q) == std::string::npos;
                              }),
               rows.end());
  }

  const float foot = 76;
  const Rect list{x, m.y + 78, rw, m.h - 78 - foot};
  dl_.rrect(list.x, list.y, list.w, list.h, 4, rgba(10, 11, 14));
  const float row_h = 24, head_h = 22;
  float content = 0;
  {
    const std::string* last = nullptr;
    for (const KeysRow& r : rows) {
      if (!last || *last != r.group) content += head_h;
      last = &r.group;
      content += row_h;
    }
  }
  if (list.contains(ov_.x, ov_.y)) keys_ui_.scroll -= ov_.scroll;
  keys_ui_.scroll = std::clamp(keys_ui_.scroll, 0.0f, std::max(0.0f, content - list.h + 4));
  const bool capturing = !keys_ui_.capture_control.empty();
  const float bw = 44, gap = 4, buttons_w = bw * 3 + gap * 3 + 50;
  const float chords_x = list.x + list.w * 0.40f;
  dl_.push_clip(list.x, list.y, list.w, list.h);
  float y = list.y + 2 - keys_ui_.scroll;
  const std::string* last_group = nullptr;
  bool changed = false;
  for (const KeysRow& r : rows) {
    if (!last_group || *last_group != r.group) {
      if (y + head_h >= list.y && y <= list.bottom()) {
        dl_.rect(list.x + 2, y, list.w - 4, head_h, with_alpha(th.accent, 38));
        dl_.text(list.x + 10, y + 5, skin_->fit(r.group, 11, 1, list.w - 20), 11, th.accent, Align::Left, list.w - 20, 1);
      }
      y += head_h;
      last_group = &r.group;
    }
    const Rect row{list.x + 2, y, list.w - 4, row_h};
    y += row_h;
    if (row.bottom() < list.y || row.y > list.bottom()) continue;
    const bool mine = capturing && keys_ui_.capture_action == r.action && keys_ui_.capture_control == r.control;
    if (mine) dl_.rrect(row.x, row.y, row.w, row.h, 3, with_alpha(th.accent, 90));
    else if (row.contains(ov_.x, ov_.y) && list.contains(ov_.x, ov_.y)) dl_.rrect(row.x, row.y, row.w, row.h, 3, with_alpha(th.text, 20));
    dl_.text(row.x + 8, row.y + 5, skin_->fit(r.label, 12, 0, chords_x - row.x - 12), 12, th.text, Align::Left, chords_x - row.x - 12);
    const float bx = row.x + row.w - buttons_w;
    const std::string shown = mine ? T("keys.press") : r.chords.empty() ? T("keys.unbound") : r.chords;
    dl_.text(chords_x, row.y + 5, skin_->fit(shown, 12, 0, bx - chords_x - 6), 12, mine ? th.text : r.chords.empty() ? th.dim : th.good, Align::Left, bx - chords_x - 6);
    const auto tiny = [&](float at, float w, const std::string& label) { return ui_button({bx + at, row.y + 2, w, row_h - 4}, label); };
    if (tiny(0, bw, T("keys.set")) && list.contains(ov_.x, ov_.y)) {
      keys_ui_.capture_control = r.control;
      keys_ui_.capture_action = r.action;
      keys_ui_.capture_add = false;
      keys_ui_.have_pending = false;
    }
    if (tiny(bw + gap, bw, T("keys.add")) && list.contains(ov_.x, ov_.y)) {
      keys_ui_.capture_control = r.control;
      keys_ui_.capture_action = r.action;
      keys_ui_.capture_add = true;
      keys_ui_.have_pending = false;
    }
    if (!r.chords.empty() && tiny((bw + gap) * 2, bw, T("keys.clear")) && list.contains(ov_.x, ov_.y)) {
      std::lock_guard<std::mutex> lock(keymap_mutex_);
      keymap_.unbind_action(r.action);
      changed = true;
    }
    if (r.changed && tiny((bw + gap) * 3, 50, T("keys.reset")) && list.contains(ov_.x, ov_.y)) {
      std::lock_guard<std::mutex> lock(keymap_mutex_);
      keymap_.reset_action(r.action);
      changed = true;
    }
    if (changed) break;  // the list is stale: redraw next frame
  }
  dl_.pop_clip();
  if (changed) save_keymap();

  // ---- status line: conflict, hint or note
  float fy = list.bottom() + 8;
  if (keys_ui_.have_pending) {
    dl_.text(x, fy + 4, skin_->fit(T("keys.conflict", {{"key", chord_label(keys_ui_.pending)}, {"action", keys_ui_.pending_other}}), 12, 0, rw - 170), 12, th.warn,
             Align::Left, rw - 170);
    if (ui_button({x + rw - 160, fy, 76, 24}, T("keys.replace"), true, th.warn)) {
      const KeyAction a = keys_ui_.capture_action;
      {
        std::lock_guard<std::mutex> lock(keymap_mutex_);
        for (KeyContext c : {KeyContext::Global, KeyContext::Browser}) {  // whatever has the chord loses it
          const KeyBinding* o = keymap_.find(keys_ui_.pending, c);
          if (o && !(o->action == a)) keymap_.unbind(keys_ui_.pending, c);
        }
        if (!keys_ui_.capture_add) keymap_.unbind_action(a);
        keymap_.bind(keys_ui_.pending, keys_ui_.pending_ctx, a);
      }
      keys_ui_.have_pending = false;
      keys_ui_.note = chord_label(keys_ui_.pending);
      save_keymap();
    }
    if (ui_button({x + rw - 78, fy, 78, 24}, T("keys.cancel"))) keys_ui_.have_pending = false;
  } else if (capturing) {
    dl_.text(x, fy + 4, skin_->fit(T("keys.capture_hint"), 12, 0, rw), 12, th.text, Align::Left, rw);
  } else {
    dl_.text(x, fy + 4, skin_->fit(keys_ui_.note.empty() ? T("keys.hint") : keys_ui_.note, 12, 0, rw), 12, th.dim, Align::Left, rw);
  }
  fy += 32;
  const bool confirm = keys_ui_.confirm_reset;
  if (ui_button({x, fy, 150, 26}, confirm ? T("keys.reset_all_sure") : T("keys.reset_all"), confirm, th.warn)) {
    if (confirm) {
      {
        std::lock_guard<std::mutex> lock(keymap_mutex_);
        keymap_.reset_all();
      }
      keys_ui_.note = T("keys.was_reset");
      keys_ui_.confirm_reset = false;
      save_keymap();
    } else {
      keys_ui_.confirm_reset = true;
    }
  }
  if (confirm) dl_.text(x + 160, fy + 6, T("keys.reset_all_warn"), 12, th.warn);
  if (ui_button({x + rw - 210, fy, 210, 26}, T("keys.export"))) keys_export_cheat_sheet();
}

}  // namespace deckhand
