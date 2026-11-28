// The recording panel (ui.capture): pick an input, record a side with a level meter, then see the side's envelope with
// draggable split points, name the tracks, choose the clean-up and export. The work is CaptureService's; this is the view.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "app_impl.hpp"

namespace deckhand {

using namespace deckhand::detail;

namespace {
std::string trim_ws(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string cap_clock(double s) {
  const int t = static_cast<int>(s);
  char b[32];
  if (t >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
  else std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
  return b;
}
float meter_pos(float db) { return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f); }  // -60 .. 0 dB
}  // namespace

void App::draw_capture_panel(const Rect& m) {
  const ui::Theme& th = theme_;
  CaptureUi& u = cap_ui_;
  CaptureService& svc = *capture_;
  const float x = m.x + 18, rw = m.w - 36;
  float y = m.y + 44;
  CaptureService::Status st = svc.status();
  const bool live = st.state == CaptureService::State::Recording || st.state == CaptureService::State::Paused;
  const bool side = st.state == CaptureService::State::Stopped && !u.again;
  const auto fail = [&](const std::string& e) { u.note = T("capture.error", {{"message", e}}); };

  if (!side) {
    // ---- input and recorder ----
    if (!live) {
      dl_.text(x, y + 2, T("capture.input"), 12, th.dim);
      y += 20;
      const float row_h = 24;
      const std::size_t shown = std::min<std::size_t>(u.devices.size() + 1, 4);
      const Rect list{x, y, rw, row_h * static_cast<float>(shown) + 4};
      dl_.rrect(list.x, list.y, list.w, list.h, 4, rgba(10, 11, 14));
      dl_.push_clip(list.x, list.y, list.w, list.h);
      for (std::size_t i = 0; i <= u.devices.size(); ++i) {
        const std::string name = i == 0 ? std::string() : u.devices[i - 1];
        const Rect row{list.x + 2, list.y + 2 + row_h * static_cast<float>(i) - u.in_scroll, list.w - 4, row_h};
        if (row.bottom() < list.y || row.y > list.bottom()) continue;
        const bool sel = name == u.device;
        if (sel) dl_.rrect(row.x, row.y, row.w, row.h, 3, with_alpha(th.accent, 110));
        else if (row.contains(ov_.x, ov_.y) && list.contains(ov_.x, ov_.y)) dl_.rrect(row.x, row.y, row.w, row.h, 3, with_alpha(th.text, 20));
        dl_.text(row.x + 8, row.y + 5, skin_->fit(i == 0 ? T("capture.default_input") : name, 12, 0, row.w - 16), 12, th.text, Align::Left, row.w - 16);
        if (ov_.pressed && row.contains(ov_.x, ov_.y) && list.contains(ov_.x, ov_.y)) {
          u.device = name;
          lib_->set_setting("capture.device", name);
        }
      }
      dl_.pop_clip();
      const float total = row_h * static_cast<float>(u.devices.size() + 1);
      if (list.contains(ov_.x, ov_.y) && total > list.h) {
        u.in_scroll = std::clamp(u.in_scroll - ov_.scroll, 0.0f, total - list.h + 4);
        panel_scroll_ += ov_.scroll;
      }
      y += list.h + 10;
      dl_.text(x, y + 5, T("settings.mic_channel"), 12, th.dim);
      float bx = x + 130;
      for (int c = 0; c < 8; ++c) {
        if (ui_button({bx, y, 30, 24}, std::to_string(c + 1), c == u.channel) && c != u.channel) {
          u.channel = c;
          lib_->set_setting("capture.channel", std::to_string(c));
        }
        bx += 34;
      }
      if (ui_toggle({bx + 8, y, std::max(120.0f, x + rw - bx - 8), 24}, T("settings.mic_stereo"), u.stereo)) {
        u.stereo = !u.stereo;
        lib_->set_setting("capture.stereo", u.stereo ? "1" : "0");
      }
      y += 34;
      dl_.text(x, y + 5, T("capture.file_name"), 12, th.dim);
      ui_field({x + 130, y, rw - 130, 24}, u.name, T("capture.file_hint"));
      y += 38;
    } else {
      dl_.text(x, y, skin_->fit(T("capture.recording_from", {{"input", st.input}}), 12, 0, rw), 12, th.dim, Align::Left, rw);
      y += 24;
    }
    // ---- level meter ----
    const Rect bar{x, y, rw, 28};
    dl_.rrect(bar.x, bar.y, bar.w, bar.h, 4, rgba(10, 11, 14));
    const float lv = meter_pos(st.peak_db), hold = meter_pos(st.hold_db);
    const Color mc = st.hold_db > -1.0f ? th.warn : st.hold_db > -12.0f ? mix(th.good, th.warn, 0.5f) : th.good;
    dl_.rect(bar.x + 2, bar.y + 3, (bar.w - 4) * lv, bar.h - 6, mc);
    dl_.rect(bar.x + 2 + (bar.w - 4) * hold - 2, bar.y + 3, 3, bar.h - 6, th.text);
    for (int db = -60; db <= 0; db += 12) {
      const float tx = bar.x + 2 + (bar.w - 4) * meter_pos(static_cast<float>(db));
      dl_.text(tx, bar.bottom() + 2, std::to_string(db), 9, th.dim, Align::Center);
    }
    y += 46;
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.1f dB", static_cast<double>(std::max(st.hold_db, -99.9f)));
    dl_.text(x, y, T("capture.peak", {{"db", std::string(buf)}}), 12, th.text);
    dl_.text(x + 170, y, T("capture.clips", {{"count", static_cast<int>(st.clips)}}), 12, st.clips > 0 ? th.warn : th.text);
    dl_.text(x + 330, y, cap_clock(st.seconds), 12, th.text);
    if (st.dropped > 0) dl_.text(x + 420, y, T("capture.dropped", {{"count", static_cast<int>(st.dropped)}}), 12, th.warn);
    y += 30;
    if (!live) {
      if (ui_button({x, y, 130, 30}, T("capture.record"), false, th.good)) {
        CaptureService::StartArgs a;
        a.device = u.device;
        a.channel = u.channel;
        a.stereo = u.stereo;
        a.name = trim_ws(u.name.text);
        a.format = u.bits == 16 ? "pcm16" : "pcm24";
        std::string e;
        if (capture_->start(a, e)) {
          u.again = false;
          u.seen_stopped = false;
          u.env_dirty = true;
          u.sel = -1;
          u.note.clear();
        } else {
          fail(e);
        }
      }
      if (st.state == CaptureService::State::Stopped && ui_button({x + 140, y, 170, 30}, T("capture.back_to_side"), false)) u.again = false;
    } else {
      const bool paused = st.state == CaptureService::State::Paused;
      std::string e;
      if (ui_button({x, y, 130, 30}, T(paused ? "capture.resume" : "capture.pause"), paused) && !capture_->set_paused(!paused, e)) fail(e);
      if (ui_button({x + 140, y, 130, 30}, T("capture.stop"), false, th.warn)) {
        if (capture_->stop(e)) u.seen_stopped = false;
        else fail(e);
      }
    }
    y += 42;
    if (!st.path.empty() && live) dl_.text(x, y, skin_->fit(st.path, 10, 0, rw), 10, th.dim, Align::Left, rw);
    if (!u.note.empty()) dl_.text(x, m.bottom() - 28, skin_->fit(u.note, 11, 0, rw), 11, th.warn, Align::Left, rw);
    return;
  }

  // ---- the finished side ----
  if (!u.seen_stopped) {
    u.seen_stopped = true;
    u.env_dirty = true;
    u.sel = -1;
    capture::SplitSettings set;
    set.min_gap_s = u.min_gap;
    set.min_depth_db = u.min_depth;
    capture_->analyse_async(set);
  }
  dl_.text(x, y, skin_->fit(st.path, 11, 0, rw - 150), 11, th.dim, Align::Left, rw - 150);
  if (ui_button({x + rw - 140, y - 6, 140, 24}, T("capture.record_another"), false)) {
    u.again = true;
    u.devices = engine_->input_devices();
  }
  y += 24;
  if (st.analysing || !st.analysed) {
    dl_.text(x, y + 20, T(st.analysing ? "capture.analysing" : "capture.not_analysed"), 12, th.dim);
    if (!st.analysing && ui_button({x, y + 50, 140, 28}, T("capture.analyse"), false)) u.seen_stopped = false;
    if (!u.note.empty()) dl_.text(x, m.bottom() - 28, skin_->fit(u.note, 11, 0, rw), 11, th.warn, Align::Left, rw);
    return;
  }
  // envelope with the split points
  const Rect env{x, y, rw, 110};
  const int buckets = std::max(1, static_cast<int>(env.w) - 4);
  if (u.env_dirty || u.env_w != buckets) {
    u.env = svc.envelope(buckets);
    u.env_w = buckets;
    u.env_dirty = false;
  }
  dl_.rrect(env.x, env.y, env.w, env.h, 4, rgba(10, 11, 14));
  for (std::size_t i = 0; i < u.env.size(); ++i) {
    const float h = std::clamp((u.env[i] + 80.0f) / 80.0f, 0.0f, 1.0f) * (env.h - 8);
    dl_.rect(env.x + 2 + static_cast<float>(i) * (env.w - 4) / static_cast<float>(u.env.size()), env.bottom() - 4 - h, std::max(1.0f, (env.w - 4) / static_cast<float>(u.env.size())), h, with_alpha(th.accent, 190));
  }
  const double total = std::max(1.0, svc.side_seconds() * svc.side_rate());
  const auto frame_x = [&](std::uint64_t f) { return env.x + 2 + static_cast<float>(static_cast<double>(f) / total) * (env.w - 4); };
  const auto x_frame = [&](float px) { return static_cast<std::uint64_t>(std::clamp((static_cast<double>(px) - env.x - 2) / (env.w - 4), 0.0, 1.0) * total); };
  const std::vector<CaptureService::SplitPointInfo> pts = svc.points();
  if (u.sel >= static_cast<int>(pts.size())) u.sel = -1;
  if (ov_.pressed && env.contains(ov_.x, ov_.y)) {
    u.drag = -1;
    float best = 7;
    for (std::size_t i = 0; i < pts.size(); ++i) {
      const float d = std::fabs(frame_x(pts[i].frame) - ov_.x);
      if (d < best) best = d, u.drag = static_cast<int>(i);
    }
    if (u.drag >= 0) {
      u.sel = u.drag;
      u.drag_frame = pts[static_cast<std::size_t>(u.drag)].frame;
    } else if (ov_.shift) {  // shift-click on the side adds a point
      std::size_t at = 0;
      std::string e;
      if (capture_->add_point(x_frame(ov_.x), &at, e)) u.sel = static_cast<int>(at);
    }
  }
  if (u.drag >= 0) {
    if (ov_.down) u.drag_frame = x_frame(ov_.x);
    if (ov_.released || !ov_.down) {
      std::string e;
      if (u.drag_frame > 0 && capture_->move_point(static_cast<std::size_t>(u.drag), u.drag_frame, e)) u.sel = u.drag;
      u.drag = -1;
    }
  }
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const bool dragging = u.drag == static_cast<int>(i);
    const float px = frame_x(dragging ? u.drag_frame : pts[i].frame);
    const Color c = static_cast<int>(i) == u.sel ? th.warn : pts[i].user ? th.good : th.text;
    dl_.line(px, env.y + 2, px, env.bottom() - 2, 2, c);
    dl_.rect(px - 4, env.y + 2, 8, 8, c);
  }
  y += env.h + 6;
  dl_.text(x, y, skin_->fit(T("capture.edit_hint"), 10, 0, rw - 170), 10, th.dim, Align::Left, rw - 170);
  if (ui_button({x + rw - 160, y - 4, 160, 22}, T("capture.remove_point"), false) && u.sel >= 0) {
    std::string e;
    capture_->remove_point(static_cast<std::size_t>(u.sel), e);
    u.sel = -1;
  }
  y += 24;
  {  // sensitivity: how long a gap and how deep
    char b[48];
    std::snprintf(b, sizeof b, "%.1f s", u.min_gap);
    dl_.text(x, y + 5, T("capture.min_gap", {{"value", std::string(b)}}), 12, th.dim);
    bool changed = false;
    if (ui_button({x + 190, y, 24, 24}, "-", false)) u.min_gap = std::max(0.3, u.min_gap - 0.2), changed = true;
    if (ui_button({x + 218, y, 24, 24}, "+", false)) u.min_gap = std::min(5.0, u.min_gap + 0.2), changed = true;
    std::snprintf(b, sizeof b, "%.0f dB", u.min_depth);
    dl_.text(x + 270, y + 5, T("capture.min_depth", {{"value", std::string(b)}}), 12, th.dim);
    if (ui_button({x + 480, y, 24, 24}, "-", false)) u.min_depth = std::max(6.0, u.min_depth - 1), changed = true;
    if (ui_button({x + 508, y, 24, 24}, "+", false)) u.min_depth = std::min(40.0, u.min_depth + 1), changed = true;
    if (changed) {
      capture::SplitSettings set = svc.settings();
      set.min_gap_s = u.min_gap;
      set.min_depth_db = u.min_depth;
      std::string e;
      svc.rerun(set, e);  // the user's own points stay
    }
  }
  y += 32;
  // names
  dl_.text(x, y + 5, T("capture.artist"), 12, th.dim);
  ui_field({x + 70, y, 200, 24}, u.artist, "");
  dl_.text(x + 290, y + 5, T("capture.album"), 12, th.dim);
  ui_field({x + 350, y, rw - 350, 24}, u.album, "");
  y += 32;
  const std::vector<CaptureService::PieceInfo> pieces = svc.pieces(false);
  std::vector<const CaptureService::PieceInfo*> real;
  for (const auto& p : pieces)
    if (!p.empty) real.push_back(&p);
  if (u.titles.size() != real.size()) {
    std::vector<std::string> keep = svc.titles();
    focus_ = nullptr;
    u.titles.assign(real.size(), Field{});
    for (std::size_t i = 0; i < real.size() && i < keep.size(); ++i) u.titles[i].text = keep[i];
  }
  const float foot = 118;
  const Rect list{x, y, rw, std::max(60.0f, m.bottom() - y - foot)};
  if (list.contains(ov_.x, ov_.y)) u.list_scroll -= ov_.scroll;
  const float row_h = 28;
  u.list_scroll = std::clamp(u.list_scroll, 0.0f, std::max(0.0f, row_h * static_cast<float>(real.size()) - list.h));
  dl_.rrect(list.x, list.y, list.w, list.h, 4, rgba(10, 11, 14));
  dl_.push_clip(list.x, list.y, list.w, list.h);
  if (real.empty()) dl_.text(list.x + 8, list.y + 8, T("capture.no_tracks"), 12, th.dim);
  for (std::size_t i = 0; i < real.size(); ++i) {
    const float ry = list.y + 2 + row_h * static_cast<float>(i) - u.list_scroll;
    if (ry + row_h < list.y || ry > list.bottom()) continue;
    char b[64];
    std::snprintf(b, sizeof b, "%02d  %s - %s", static_cast<int>(i) + 1, cap_clock(real[i]->start_s).c_str(), cap_clock(real[i]->end_s).c_str());
    dl_.text(list.x + 8, ry + 6, b, 11, th.dim);
    ui_field({list.x + 200, ry + 1, list.w - 208, 24}, u.titles[i], T("capture.track_hint", {{"n", static_cast<int>(i) + 1}}));
  }
  dl_.pop_clip();
  y = list.bottom() + 8;
  // clean-up and format
  const float tw = (rw - 12) / 4;
  if (ui_toggle({x, y, tw, 22}, T("capture.dc"), u.dc)) u.dc = !u.dc;
  if (ui_toggle({x + tw + 4, y, tw, 22}, T("capture.rumble"), u.rumble)) u.rumble = !u.rumble;
  if (ui_toggle({x + 2 * (tw + 4), y, tw, 22}, T("capture.declick"), u.declick)) u.declick = !u.declick;
  if (ui_toggle({x + 3 * (tw + 4), y, tw, 22}, T("capture.normalise"), u.normalise)) u.normalise = !u.normalise;
  y += 26;
  if (ui_toggle({x, y, tw, 22}, T("capture.fades"), u.fades)) u.fades = !u.fades;
  const bool can_flac = capture::flac_available();
  if (!can_flac) u.flac = false;
  const float fw = (tw - 8) / (can_flac ? 3.0f : 2.0f);
  if (ui_button({x + tw + 4, y, fw, 22}, "WAV", !u.aiff && !u.flac)) u.aiff = u.flac = false;
  if (ui_button({x + tw + 4 + fw + 4, y, fw, 22}, "AIFF", u.aiff)) {
    u.aiff = true;
    u.flac = false;
  }
  if (can_flac && ui_button({x + tw + 4 + 2 * (fw + 4), y, fw, 22}, "FLAC", u.flac)) {  // 16 or 24 bit only, like the 16 / 24 chooser
    u.flac = true;
    u.aiff = false;
  }
  if (ui_button({x + 2 * (tw + 4), y, tw / 2 - 2, 22}, "16", u.bits == 16)) u.bits = 16;
  if (ui_button({x + 2 * (tw + 4) + tw / 2 + 2, y, tw / 2 - 2, 22}, "24", u.bits == 24)) u.bits = 24;
  y += 30;
  if (ui_button({x, y, 180, 30}, T("capture.export"), false, th.good) && !real.empty()) {
    std::vector<std::string> titles;
    for (const Field& f : u.titles) titles.push_back(trim_ws(f.text));
    capture_->set_titles(titles);
    CaptureService::AlbumTags a;
    a.artist = trim_ws(u.artist.text);
    a.album = trim_ws(u.album.text);
    capture_->set_album(a);
    capture::ExportSettings es;
    es.container = u.flac ? capture::Container::Flac : u.aiff ? capture::Container::Aiff : capture::Container::Wav;
    es.format = u.bits == 16 ? capture::SampleFormat::Pcm16 : capture::SampleFormat::Pcm24;
    es.cleanup.remove_dc = u.dc;
    es.cleanup.rumble_filter = u.rumble;
    es.cleanup.click_repair = u.declick;
    es.cleanup.normalise = u.normalise;
    if (u.fades) es.cleanup.fade_in_ms = es.cleanup.fade_out_ms = 20;
    const std::string stem = std::filesystem::path(std::filesystem::u8path(st.path)).stem().string();
    const std::string dir = capture_->folder() + "/" + stem + "-tracks";
    std::string e;
    const auto res = capture_->export_pieces(dir, es, false, e);
    int ok = 0;
    for (const auto& r : res) ok += r.ok ? 1 : 0;
    if (res.empty()) fail(e.empty() ? T("capture.nothing") : e);
    else u.note = T("capture.exported", {{"count", ok}, {"folder", dir}});
  }
  if (!u.note.empty()) dl_.text(x + 190, y + 8, skin_->fit(u.note, 11, 0, rw - 190), 11, th.text, Align::Left, rw - 190);
}

}  // namespace deckhand
