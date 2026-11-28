// Confirmations (a reusable question panel), the quit guard, the status-row recording indicators, the compatibility report
// shown before a USB export, and the shared-playlist panel. The logic lives in confirm_dialog.hpp, status_indicators.hpp,
// export_report.hpp and shared_playlist_ui.hpp (all tested headless); this is the window part.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app_impl.hpp"

namespace deckhand {

using namespace deckhand::detail;

namespace {
std::vector<std::string> wrap_lines(gfx::DrawList& dl, const std::string& text, float size, float width) {
  std::vector<std::string> lines;
  std::string line;
  std::size_t at = 0;
  while (at <= text.size()) {
    std::size_t sp = text.find_first_of(" \n", at);
    if (sp == std::string::npos) sp = text.size();
    const std::string word = text.substr(at, sp - at);
    const bool hard = sp < text.size() && text[sp] == '\n';
    const std::string probe = line.empty() ? word : line + " " + word;
    if (!line.empty() && dl.text_width(probe, size) > width) {
      lines.push_back(line);
      line = word;
    } else {
      line = probe;
    }
    if (hard) {
      lines.push_back(line);
      line.clear();
    }
    at = sp + 1;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}
}  // namespace

// ---- the question panel ---------------------------------------------------------------------------------------------
void App::pump_dialogs() {
  if (confirm_.open() && panel_ != Panel::Confirm) confirm_.cancel();  // something else took the panel: the question lapses
  if (!confirm_.open() && panel_ != Panel::Confirm) {
    ConfirmRequest r;
    if (confirm_q_.take(r)) {
      // Declining returns to the panel that was open (a pop-up menu is not worth coming back to).
      confirm_return_ = panel_ == Panel::Export || panel_ == Panel::AddTo || panel_ == Panel::Confirm ? Panel::None : panel_;
      confirm_.show(std::move(r));
      open_panel(Panel::Confirm);
    }
  }
  bool report = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    report = export_ui_.pending_open;
    export_ui_.pending_open = false;
  }
  if (report && panel_ != Panel::Confirm) {
    panel_scroll_ = 0;
    open_panel(Panel::ExportReport);
  }
}

void App::answer_confirm(bool ok) {
  const Panel back = confirm_return_;
  confirm_return_ = Panel::None;
  panel_ = Panel::None;  // the panel goes first: the answer may open another one
  focus_ = nullptr;
  if (ok) confirm_.accept();
  else confirm_.cancel();
  if (!ok && back != Panel::None && panel_ == Panel::None) open_panel(back);
}

void App::draw_confirm_panel(const Rect& m) {
  const ui::Theme& th = theme_;
  if (!confirm_.open()) {
    close_panel();
    return;
  }
  const ConfirmRequest r = confirm_.request();  // a copy: the answer clears it
  const float x = m.x + 20, rw = m.w - 40;
  float y = m.y + 46;
  for (const std::string& l : wrap_lines(dl_, r.message, 13, rw)) {
    dl_.text(x, y, l, 13, th.text, Align::Left, rw);
    y += 18;
  }
  y += 6;
  for (const std::string& d : r.details)
    for (const std::string& l : wrap_lines(dl_, d, 11, rw)) {
      dl_.text(x, y, l, 11, th.dim, Align::Left, rw);
      y += 15;
    }
  const float by = m.y + m.h - 44, bw = 150;
  const std::string ok = r.ok_label.empty() ? T("confirm.ok") : r.ok_label;
  const std::string no = r.cancel_label.empty() ? T("confirm.cancel") : r.cancel_label;
  if (ui_button({m.x + m.w - 20 - bw, by, bw, 30}, ok, true, r.danger ? th.warn : th.good)) {
    answer_confirm(true);
    return;
  }
  if (ui_button({m.x + m.w - 20 - bw * 2 - 10, by, bw, 30}, no, false)) answer_confirm(false);
}

// ---- the quit guard -------------------------------------------------------------------------------------------------
bool App::may_quit() {
  if (quit_ok_) return true;
  const QuitRisk risk = quit_risk(capture_ ? capture_->status() : CaptureService::Status{}, engine_->recorder_status());
  if (!risk.any()) return true;
  ConfirmRequest q;
  q.id = "quit";
  q.title = T("quit.title");
  q.message = T(risk.capture && risk.mix ? "quit.both" : risk.capture ? "quit.capture" : "quit.mix");
  q.details.push_back(T("quit.detail"));
  q.ok_label = T("quit.anyway");
  q.cancel_label = T("quit.stay");
  q.danger = true;
  q.on_ok = [this] {
    quit_ok_ = true;
    quit_asked_ = true;
    if (platform_.request_quit) platform_.request_quit(platform_.user);
  };
  ask_confirm(std::move(q));
  return false;
}

// ---- the status-row indicators ----------------------------------------------------------------------------------------
std::string App::capture_indicator_text() {
  if (!capture_) return {};
  const CaptureIndicator i = capture_indicator(capture_->status());
  if (!i.active) return {};
  if (i.clips > 0) return T("capture.indicator_clip", {{"time", i.clock}, {"count", static_cast<int>(i.clips)}});
  return T(i.paused ? "capture.indicator_paused" : "capture.indicator", {{"time", i.clock}});
}

std::string App::mix_indicator_text() {
  const MixIndicator i = mix_indicator(engine_->recorder_status());
  if (!i.active) return {};
  return T(i.warn ? "rec.indicator_warn" : "rec.indicator", {{"time", i.clock}});
}

// ---- the compatibility report before an export ------------------------------------------------------------------------
void App::request_export(const std::string& destination, std::int64_t list) {
  Job j;
  j.kind = Job::ExportCheck;
  j.path = destination;
  j.track_id = list;
  post(j);
  set_status(T("status.export_checking"));
}

void App::export_check_job(Library& lib, const Job& j) {
  UsbExportRequest req;
  req.destination = j.path;
  req.convert_incompatible = lib.setting("export.convert_incompatible") == "1";
  req.player_profile = lib.setting("export.player_profile");
  if (j.track_id > 0) req.playlist_ids.push_back(j.track_id);
  else if (j.track_id < 0) req.smart_playlist_ids.push_back(-j.track_id);
  ExportReport rep;
  try {
    rep = check_export(lib, req);
  } catch (const std::exception& e) {
    DH_LOG(Warn, "export check failed, exporting without the report: %s", e.what());
  }
  if (rep.clean() && !j.flag) {  // nothing to tell: the export just starts
    Job go;
    go.kind = Job::Export;
    go.path = j.path;
    go.track_id = j.track_id;
    post(go);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    export_ui_.report = std::move(rep);
    export_ui_.destination = j.path;
    export_ui_.list = j.track_id;
    export_ui_.pending_open = true;
  }
  set_status(T("status.export_report_ready"));
}

namespace {
struct ReportLine {
  std::string text;
  float size = 11;
  int weight = 0;
  int tone = 0;  // 0 normal, 1 dim, 2 warning, 3 good
  float gap = 0;
};
}  // namespace

void App::draw_export_report(const Rect& m) {
  const ui::Theme& th = theme_;
  ExportReport rep;
  std::string dest;
  std::int64_t list = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    rep = export_ui_.report;
    dest = export_ui_.destination;
    list = export_ui_.list;
  }
  const float x = m.x + 20, rw = m.w - 40;
  float y = m.y + 44;
  dl_.text(x, y, skin_->fit(T("export.report_summary", {{"tracks", static_cast<int>(rep.tracks)},
                                                          {"will_not_play", static_cast<int>(rep.will_not_play)},
                                                          {"fixable", static_cast<int>(rep.fixable)},
                                                          {"warnings", static_cast<int>(rep.warnings)}}), 12, 0, rw), 12,
           rep.will_not_play > 0 ? th.warn : th.text, Align::Left, rw);
  y += 18;
  if (rep.cues_total > 0) {
    dl_.text(x, y, skin_->fit(T("export.report_cues", {{"cues", static_cast<int>(rep.cues_total)}, {"tracks", static_cast<int>(rep.cue_tracks)}}), 11, 0, rw), 11,
             th.dim, Align::Left, rw);
    y += 16;
  }
  dl_.text(x, y, skin_->fit(T("export.report_to", {{"path", dest}}), 11, 0, rw), 11, th.dim, Align::Left, rw);
  y += 22;

  // The rows: title, then the problems, what the conversion does, and the cues that stay behind.
  const Rect list_r{m.x + 8, y, m.w - 16, m.y + m.h - y - 100};
  std::vector<std::vector<ReportLine>> rows;
  float total = 0;
  for (const ExportReportRow& r : rep.rows) {
    std::vector<ReportLine> lines;
    lines.push_back({(r.artist.empty() ? r.title : r.artist + " - " + r.title), 12, 1, r.verdict == usbexport::Verdict::WillNotPlay ? 2 : 0, 0});
    for (const std::string& p : r.problems) lines.push_back({p, 11, 0, 1, 0});
    switch (r.fix) {
      case ExportReportRow::Fix::Convert: lines.push_back({T("export.report_fix_convert", {{"detail", r.fix_detail}}), 11, 0, 3, 0}); break;
      case ExportReportRow::Fix::ConvertOff: lines.push_back({T("export.report_fix_off"), 11, 0, 2, 0}); break;
      case ExportReportRow::Fix::CannotFix: lines.push_back({T("export.report_fix_none"), 11, 0, 2, 0}); break;
      case ExportReportRow::Fix::None: break;
    }
    if (r.cues_beyond > 0) lines.push_back({T("export.report_row_cues", {{"count", r.cues_beyond}}), 11, 0, 1, 0});
    // wrap every line to the width of the list
    std::vector<ReportLine> wrapped;
    for (const ReportLine& l : lines)
      for (const std::string& w : wrap_lines(dl_, l.text, l.size, list_r.w - 24)) {
        ReportLine c = l;
        c.text = w;
        wrapped.push_back(c);
      }
    total += 15.0f * static_cast<float>(wrapped.size()) + 10.0f;
    rows.push_back(std::move(wrapped));
  }
  if (list_r.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
  panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, total - list_r.h));
  dl_.rrect(list_r.x, list_r.y, list_r.w, list_r.h, 4, rgba(10, 11, 14));
  dl_.push_clip(list_r.x, list_r.y, list_r.w, list_r.h);
  float cy = list_r.y + 4 - panel_scroll_;
  for (const auto& lines : rows) {
    for (const ReportLine& l : lines) {
      if (cy + 15 >= list_r.y && cy <= list_r.bottom()) {
        const Color c = l.tone == 1 ? th.dim : l.tone == 2 ? th.warn : l.tone == 3 ? th.good : th.text;
        dl_.text(list_r.x + 12, cy, l.text, l.size, c, Align::Left, list_r.w - 24, l.weight);
      }
      cy += 15;
    }
    cy += 10;
  }
  dl_.pop_clip();

  // The conversion switch, and the answer.
  const float ty = m.y + m.h - 82;
  if (ui_toggle({x, ty, rw, 24}, T("export.convert_option"), rep.convert)) {
    save_setting("export.convert_incompatible", rep.convert ? "0" : "1");
    Job j;
    j.kind = Job::ExportCheck;
    j.path = dest;
    j.track_id = list;
    j.flag = true;  // show the report again with the new setting
    post(j);
  }
  const float by = m.y + m.h - 44, bw = 170;
  const bool risky = rep.will_not_play > 0 && (!rep.convert || rep.fixable < rep.will_not_play);
  if (ui_button({m.x + m.w - 20 - bw, by, bw, 30}, T(risky ? "export.report_go_anyway" : "export.report_go"), true, risky ? th.warn : th.good)) {
    Job j;
    j.kind = Job::Export;
    j.path = dest;
    j.track_id = list;
    post(j);
    close_panel();
    return;
  }
  if (ui_button({m.x + m.w - 20 - bw * 2 - 10, by, bw, 30}, T("export.report_cancel"), false)) {
    set_status(T("status.export_cancelled"));
    close_panel();
  }
}

// ---- shared playlists --------------------------------------------------------------------------------------------------
void App::open_shared_panel() {
  std::int64_t id = 0;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (tree_sel_ >= 0 && tree_sel_ < static_cast<int>(tree_.size())) {
      const TreeItem& it = tree_[static_cast<std::size_t>(tree_sel_)];
      if (it.kind == TreeItem::Playlist) {
        id = it.id;
        name = it.name;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    shared_ui_.playlist = id;
    shared_ui_.playlist_name = name;
    shared_ui_.status = {};
    shared_ui_.choices.clear();
    shared_ui_.note.clear();
  }
  open_panel(Panel::Shared);
  queue_shared("refresh", id);
}

void App::queue_shared(const std::string& op, std::int64_t playlist, const std::string& shared_id) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    shared_ui_.busy = true;
  }
  Job j;
  j.kind = Job::SharedOp;
  j.text = op;
  j.track_id = playlist;
  j.path = shared_id;
  post(j);
}

void App::shared_job(Library& lib, const Job& j) {
  const sync::SharedConfig cfg = shared_settings_config(lib, host_name());
  std::int64_t playlist = j.track_id;
  std::string note;
  bool changed = false;
  if (j.text == "share" || j.text == "join" || j.text == "sync") {
    if (cfg.folder.empty()) {
      note = T("shared.no_folder");
    } else {
      SharedOutcome o;
      if (j.text == "share") o = shared_share(lib, playlist, cfg);
      else if (j.text == "join") o = shared_join(lib, j.path, 0, cfg);
      else o = shared_sync(lib, playlist, cfg);
      if (!o.ok) {
        note = T("shared.failed", {{"message", o.error}});
      } else {
        if (o.playlist_id > 0) playlist = o.playlist_id;
        changed = true;
        note = T(j.text == "share" ? "shared.shared" : j.text == "join" ? "shared.joined" : "shared.synced",
                 {{"sent", o.sent}, {"received", o.received}, {"missing", o.missing}});
      }
    }
  } else if (j.text == "leave") {
    shared_leave(lib, playlist);
    note = T("shared.left");
  }
  SharedStatus st;
  std::vector<JoinChoice> choices;
  if (playlist > 0) st = shared_status(lib, playlist, cfg);
  choices = shared_join_choices(lib, cfg);
  std::string name;
  for (const PlaylistNode& n : lib.playlists())
    if (n.id == playlist) name = n.name;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    shared_ui_.playlist = playlist;
    if (!name.empty() || playlist == 0) shared_ui_.playlist_name = name;
    shared_ui_.status = st;
    shared_ui_.choices = std::move(choices);
    if (!note.empty()) shared_ui_.note = note;
    shared_ui_.busy = false;
  }
  if (changed) rows_dirty_ = true;
  if (!note.empty()) set_status(note);
}

void App::draw_shared_panel(const Rect& m) {
  const ui::Theme& th = theme_;
  SharedUi u;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    u = shared_ui_;
  }
  const float x = m.x + 20, rw = m.w - 40;
  float y = m.y + 44;
  for (const std::string& l : wrap_lines(dl_, T("shared.help"), 11, rw)) {
    dl_.text(x, y, l, 11, th.dim, Align::Left, rw);
    y += 15;
  }
  y += 8;
  // the folder both sides replicate
  const std::string folder = shared_folder(*lib_);
  dl_.text(x, y + 5, T("shared.folder"), 12, th.dim);
  dl_.text(x + 90, y + 5, skin_->fit(folder.empty() ? T("shared.no_folder_short") : folder, 12, 0, rw - 90 - 130), 12, folder.empty() ? th.warn : th.text, Align::Left, rw - 90 - 130);
  if (platform_.choose_folder && ui_button({x + rw - 120, y, 120, 24}, T("shared.choose"), false)) {
    std::string dest;
    if (choose_folder(T("shared.choose_title"), dest)) {
      save_setting("shared.folder", dest);
      queue_shared("refresh", u.playlist);
    }
  }
  y += 36;

  // this playlist
  ui_heading(x, y, T("shared.this_playlist"));
  y += 20;
  if (u.playlist <= 0) {
    dl_.text(x, y, skin_->fit(T("shared.select_playlist"), 12, 0, rw), 12, th.dim, Align::Left, rw);
    y += 24;
  } else {
    dl_.text(x, y, skin_->fit(u.playlist_name, 13, 1, rw), 13, th.text, Align::Left, rw, 1);
    y += 22;
    if (u.status.linked) {
      std::string who;
      for (const std::string& p : u.status.participants) who += (who.empty() ? "" : ", ") + p;
      dl_.text(x, y, skin_->fit(T("shared.status_linked", {{"entries", u.status.entries}, {"missing", u.status.missing}, {"with", who.empty() ? T("shared.nobody") : who}}), 12, 0, rw),
               12, u.status.folder_ok ? th.text : th.warn, Align::Left, rw);
      y += 20;
      if (!u.status.folder_ok) {
        dl_.text(x, y, skin_->fit(T("shared.folder_unreachable"), 11, 0, rw), 11, th.warn, Align::Left, rw);
        y += 16;
      }
      if (ui_button({x, y, 150, 28}, T("shared.sync_now"), false, th.good) && !u.busy) queue_shared("sync", u.playlist);
      if (ui_button({x + 160, y, 180, 28}, T("shared.leave"), false) && !u.busy) {
        ConfirmRequest q;
        q.id = "shared.leave";
        q.title = T("shared.leave_title");
        q.message = T("shared.leave_ask", {{"name", u.playlist_name}});
        q.details.push_back(T("shared.leave_detail"));
        q.ok_label = T("shared.leave");
        q.cancel_label = T("confirm.cancel");
        const std::int64_t id = u.playlist;
        q.on_ok = [this, id] {
          open_panel(Panel::Shared);  // back to the panel; the answer closed the question
          queue_shared("leave", id);
        };
        ask_confirm(std::move(q));
      }
      y += 38;
    } else {
      dl_.text(x, y, skin_->fit(T("shared.status_not_shared"), 12, 0, rw), 12, th.dim, Align::Left, rw);
      y += 20;
      if (ui_button({x, y, 220, 28}, T("shared.share"), false, th.good) && !u.busy) queue_shared("share", u.playlist);
      y += 38;
    }
  }

  // playlists others put in the folder
  ui_heading(x, y, T("shared.join_heading"));
  y += 20;
  const Rect list{m.x + 8, y, m.w - 16, m.y + m.h - y - 52};
  const float row_h = 34;
  if (u.choices.empty()) {
    dl_.text(x, y + 4, skin_->fit(T(folder.empty() ? "shared.no_folder" : "shared.nothing_to_join"), 12, 0, rw), 12, th.dim, Align::Left, rw);
  } else {
    if (list.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
    const float total = row_h * static_cast<float>(u.choices.size());
    panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, total - list.h));
    dl_.push_clip(list.x, list.y, list.w, list.h);
    float cy = list.y - panel_scroll_;
    for (const JoinChoice& c : u.choices) {
      std::string who;
      for (const std::string& p : c.participants) who += (who.empty() ? "" : ", ") + p;
      dl_.text(x, cy + 2, skin_->fit(c.name.empty() ? c.shared_id : c.name, 12, 1, rw - 120), 12, th.text, Align::Left, rw - 120, 1);
      dl_.text(x, cy + 18, skin_->fit(who, 11, 0, rw - 120), 11, th.dim, Align::Left, rw - 120);
      if (cy + row_h > list.y && cy < list.bottom() && ui_button({x + rw - 100, cy + 3, 100, 26}, T("shared.join"), false) && !u.busy)
        queue_shared("join", u.playlist, c.shared_id);
      cy += row_h;
    }
    dl_.pop_clip();
  }
  const std::string line = u.busy ? T("shared.working") : u.note;
  if (!line.empty()) dl_.text(x, m.y + m.h - 34, skin_->fit(line, 12, 0, rw), 12, u.busy ? th.dim : th.accent, Align::Left, rw);
}

}  // namespace deckhand
