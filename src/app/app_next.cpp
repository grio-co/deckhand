// "What goes well after this?" (the Suggestions panel) and preparation mode (two headphone players on decks 3 and 4).
// The scoring is Library::suggest(); the preparation logic is PrepMode. This file is the screens and their keys.
#include <algorithm>
#include <cmath>

#include "app_impl.hpp"
#include "deckhand/app/next_tracks.hpp"
#include "deckhand/app/prep_mode.hpp"

namespace deckhand {

namespace {
std::string name_of(const TrackRow& r) { return r.artist.empty() ? r.title : r.artist + " - " + r.title; }
}  // namespace

std::int64_t App::selected_track_id() {
  std::lock_guard<std::mutex> lock(browse_mutex_);
  return sel_ >= 0 && sel_ < static_cast<int>(rows_.size()) ? rows_.id(static_cast<std::size_t>(sel_)) : 0;
}

// The deck a track is loaded on from the suggestions: not one that plays, not one on air; decks 1 and 2 first.
int App::free_deck() const {
  const int order[4] = {0, 1, 2, 3};
  for (const int d : order) {
    if (prep_ && prep_->active() && d >= 2) continue;  // borrowed by preparation mode
    const DeckView v = engine_->deck(d);
    if (!v.playing) return d;
  }
  return -1;
}

void App::next_load(std::int64_t id) {
  const int d = free_deck();
  if (d < 0 || id <= 0) {
    status_note_ = T("suggest.no_free_deck");
    return;
  }
  request_load(d, id);
}

void App::next_queue(std::int64_t id, bool play_next) {
  if (!queue_ || id <= 0) return;
  const QueueAdd r = play_next ? queue_->play_next({id}) : queue_->add_end({id});
  status_note_ = r.entry_ids.empty() && r.skipped > 0 ? T("queue.refused") : T(play_next ? "queue.play_next_added" : "queue.added", {{"count", "1"}});
  next_dirty_ = true;  // the queued track leaves the list
}

// The seed: the deck on air, or the browser selection (setting "suggest.follow").
std::int64_t App::next_seed() {
  if (next_set_.follow_master) {
    const int d = master_deck(*engine_);
    if (d >= 0) return engine_->deck(d).track_id;
  }
  return selected_track_id();
}

void App::next_refresh(bool force) {
  const std::int64_t seed = next_seed();
  const std::uint64_t rev = queue_ ? queue_->revision() : 0;
  if (!force && !next_dirty_ && seed == next_seed_ && rev == next_rev_) return;
  next_dirty_ = false;
  next_seed_ = seed;
  next_rev_ = rev;
  next_items_.clear();
  next_rows_.clear();
  if (next_sel_ < 0) next_sel_ = 0;
  if (seed <= 0) return;
  const NextTrackOptions o = next_options(*lib_, queue_.get(), next_set_, 0, 30);
  next_items_ = lib_->suggest(seed, o);
  for (const NextTrack& n : next_items_)
    if (const auto row = lib_->track(n.track_id)) next_rows_.push_back(*row);
  next_items_.resize(next_rows_.size());  // (a track removed meanwhile)
  next_sel_ = std::clamp(next_sel_, 0, std::max(0, static_cast<int>(next_items_.size()) - 1));
}

std::string App::next_reason(const NextTrack& n) {
  std::string out;
  const auto add = [&](const std::string& s) { out += (out.empty() ? "" : ", ") + s; };
  for (const NextTrackFactor& f : n.factors) {
    if (f.id == "tempo") {
      const std::string diff = fmt("%+.1f", f.detail);
      add(T(f.tag == "half" ? "suggest.r_tempo_half" : f.tag == "double" ? "suggest.r_tempo_double" : "suggest.r_tempo",
            {{"diff", diff}, {"pitch", fmt("%+.1f", n.pitch_change * 100.0)}}));
    } else if (f.id == "key") {
      if (f.tag != "none") add(T("suggest.r_key_" + f.tag));
    } else if (f.id == "energy") {
      add(T(f.detail > 0.15 ? "suggest.r_energy_up" : f.detail < -0.15 ? "suggest.r_energy_down" : "suggest.r_energy_same"));
    } else if (f.id == "genre") {
      if (!f.tag.empty()) add(T("suggest.r_genre", {{"genre", f.tag}}));
    } else if (f.id == "rating") {
      if (f.detail >= 4) add(T("suggest.r_rating", {{"stars", static_cast<int>(f.detail)}}));
    } else if (f.id == "history") {
      if (n.pair_count > 0) add(T("suggest.r_history", {{"count", n.pair_count}}));
      else add(T("suggest.r_history_back", {{"count", n.reverse_count}}));
    } else if (f.id == "explore") {
      add(T("suggest.r_new"));
    } else if (f.id == "link") {
      add(T("suggest.r_link", {{"count", static_cast<int>(f.detail)}}));
    } else if (f.id == "recent") {
      add(T("suggest.r_recent"));
    }
  }
  return out;
}

void App::draw_suggest(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 16, rw = m.w - 32;
  float y = m.y + 44;
  // Settings row: what the list follows and how strict it is.
  bool changed = false;
  if (ui_button({x, y, 210, 24}, T(next_set_.follow_master ? "suggest.follow_deck" : "suggest.follow_selection"), true)) {
    next_set_.follow_master = !next_set_.follow_master;
    changed = true;
  }
  if (ui_button({x + 216, y, 120, 24}, T("suggest.pitch", {{"percent", fmt("%g", next_set_.max_pitch * 100)}}), false)) {
    const double steps[] = {0.03, 0.06, 0.08, 0.10, 0.16};
    std::size_t i = 0;
    while (i < 5 && steps[i] <= next_set_.max_pitch + 1e-9) ++i;
    next_set_.max_pitch = steps[i % 5];
    changed = true;
  }
  if (ui_button({x + 342, y, 110, 24}, T("suggest.half_double"), next_set_.half_double)) {
    next_set_.half_double = !next_set_.half_double;
    changed = true;
  }
  if (ui_button({x + 458, y, 100, 24}, T("suggest.harmonic"), next_set_.harmonic_only)) {
    next_set_.harmonic_only = !next_set_.harmonic_only;
    changed = true;
  }
  if (ui_button({x + 564, y, std::max(60.0f, rw - 564), 24}, T("suggest.hide_queued"), next_set_.exclude_queued)) {
    next_set_.exclude_queued = !next_set_.exclude_queued;
    changed = true;
  }
  if (changed) {
    save_next_settings(*lib_, next_set_);
    next_dirty_ = true;
  }
  y += 32;
  next_refresh(false);
  if (const auto seed = next_seed_ > 0 ? lib_->track(next_seed_) : std::nullopt) {
    dl_.text(x, y, skin_->fit(T("suggest.after", {{"track", name_of(*seed)}}), 13, 1, rw), 13, th.text, Align::Left, rw, 1);
    dl_.text(x, y + 17, skin_->fit(fmt("%.1f BPM   %s   %s", seed->bpm, display_key(seed->key, key_musical_).c_str(), seed->genre.c_str()), 11, 0, rw), 11, th.dim, Align::Left, rw);
  } else {
    dl_.text(x, y + 6, skin_->fit(T("suggest.no_seed"), 12, 0, rw), 12, th.dim, Align::Left, rw);
  }
  y += 40;
  const Rect list{m.x + 8, y, m.w - 16, m.y + m.h - y - 30};
  if (list.contains(ov_.x, ov_.y)) panel_scroll_ -= ov_.scroll;
  const float row_h = 46;
  panel_content_h_ = row_h * static_cast<float>(next_items_.size()) + 4;
  panel_scroll_ = std::clamp(panel_scroll_, 0.0f, std::max(0.0f, panel_content_h_ - list.h));
  dl_.push_clip(list.x, list.y, list.w, list.h);
  float cy = list.y + 2 - panel_scroll_;
  if (next_items_.empty() && next_seed_ > 0) dl_.text(x, cy + 8, T("suggest.none"), 12, th.dim);
  const float bw = 62;
  for (std::size_t i = 0; i < next_items_.size(); ++i, cy += row_h) {
    if (cy + row_h < list.y || cy > list.y + list.h) continue;
    const NextTrack& n = next_items_[i];
    const TrackRow& r = next_rows_[i];
    const Rect row{list.x, cy, list.w, row_h - 2};
    const bool sel = static_cast<int>(i) == next_sel_;
    if (sel) dl_.rrect(row.x, row.y, row.w, row.h, 4, with_alpha(th.accent, 70));
    const float tx = x + 52, tw = rw - 52 - 3 * (bw + 6) - 6;
    dl_.text(x, cy + 6, fmt("%.0f", n.score), 16, n.score >= 70 ? th.good : th.text, Align::Left, 46, 1);
    dl_.text(tx, cy + 3, skin_->fit(name_of(r), 12, 1, tw), 12, th.text, Align::Left, tw, 1);
    dl_.text(tx, cy + 19, skin_->fit(fmt("%.1f  %s  ", r.bpm, display_key(r.key, key_musical_).c_str()) + next_reason(n), 10, 0, tw), 10, th.dim, Align::Left, tw);
    const float bx = x + rw - 3 * (bw + 6);
    if (ui_button({bx, cy + 8, bw, 26}, T("suggest.load"), false)) next_load(r.id);
    if (ui_button({bx + bw + 6, cy + 8, bw, 26}, T("suggest.next"), false)) next_queue(r.id, true);
    if (ui_button({bx + 2 * (bw + 6), cy + 8, bw, 26}, T("suggest.queue"), false)) next_queue(r.id, false);
    if (ov_.pressed && row.contains(ov_.x, ov_.y) && ov_.x < bx) {
      next_sel_ = static_cast<int>(i);
      if (ov_.clicks >= 2) next_load(r.id);
    }
  }
  dl_.pop_clip();
  dl_.text(x, m.y + m.h - 22, skin_->fit(T("suggest.keys"), 10, 0, rw), 10, th.dim, Align::Left, rw);
}

// Keys inside the Suggestions panel (the panel has the keyboard while it is open).
bool App::suggest_key(const std::string& key) {
  const std::int64_t id = next_sel_ >= 0 && next_sel_ < static_cast<int>(next_rows_.size()) ? next_rows_[static_cast<std::size_t>(next_sel_)].id : 0;
  if (key == "up") next_sel_ = std::max(0, next_sel_ - 1);
  else if (key == "down") next_sel_ = std::min(std::max(0, static_cast<int>(next_items_.size()) - 1), next_sel_ + 1);
  else if (key == "enter" || key == "return") next_load(id);
  else if (key == "q") next_queue(id, false);
  else if (key == "n") next_queue(id, true);
  else return false;
  return true;
}

void App::queue_top_suggestion() {
  if (!queue_) return;
  next_refresh(true);
  if (next_items_.empty()) {
    status_note_ = T("suggest.none");
    return;
  }
  next_queue(next_items_.front().track_id, true);
}

// ---------------------------------------------------------------------------------------------------- preparation mode

void App::prep_enter() {
  if (!prep_) return;
  std::string refusal, message;
  if (prep_->enter(refusal, message)) {
    prep_note_.clear();
    status_note_ = T("prep.on");
  } else {
    prep_note_ = T("prep.refused_" + refusal, {{"message", message}});
    status_note_ = prep_note_;
  }
}

void App::prep_leave() {
  if (!prep_) return;
  prep_->exit();
  status_note_ = T("prep.off");
}

void App::prep_load(int slot, std::int64_t id) {
  if (!prep_ || id <= 0) return;
  std::string error;
  if (!prep_->load(slot, id, error)) prep_note_ = error;
  else prep_note_.clear();
  prep_mix_[slot] = {};
  prep_facts_[slot] = TrackFacts{};
  prep_facts_for_[slot] = id;
  prep_facts_[slot] = read_track_facts(*lib_, id);
  const TrackFacts& f = prep_facts_[slot];
  const double first = f.cues.first_sound_ms.value_or(0), last = f.cues.last_sound_ms.value_or(f.duration_ms);
  if (!f.phrases.sections.empty()) prep_mix_[slot] = phrase_mix_points(f.phrases, first, last, 16);
}

void App::draw_prep_mode(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 16, rw = m.w - 32;
  float y = m.y + 44;
  const PrepStatus st = prep_ ? prep_->status() : PrepStatus{};
  if (!st.active) {
    for (const std::string& line : {T("prep.intro1"), T("prep.intro2"), T("prep.intro3")}) {
      dl_.text(x, y, skin_->fit(line, 12, 0, rw), 12, th.text, Align::Left, rw);
      y += 20;
    }
    y += 8;
    if (ui_button({x, y, 200, 30}, T("prep.enter"), true, th.accent)) prep_enter();
    if (!prep_note_.empty()) dl_.text(x, y + 42, skin_->fit(prep_note_, 12, 0, rw), 12, th.warn, Align::Left, rw);
    return;
  }
  dl_.rrect(x, y, rw, 24, 4, with_alpha(th.warn, 60));
  dl_.text(x + 8, y + 5, skin_->fit(T("prep.banner"), 11, 1, rw - 16), 11, th.text, Align::Left, rw - 16, 1);
  y += 32;
  const float colw = (rw - 12) / 2;
  const std::int64_t sel = selected_track_id();
  for (int s = 0; s < 2; ++s) {
    const float cx = x + s * (colw + 12);
    const int d = PrepMode::kDeck[s];
    const DeckInfo& info = info_[d];
    const DeckView v = engine_->deck(d);
    std::string title = st.loading[s] ? T("deck.loading") : T("prep.empty");
    if (st.loaded[s]) title = name_of(info.row);
    dl_.text(cx, y, skin_->fit(std::string(s ? "B  " : "A  ") + title, 13, 1, colw), 13, th.text, Align::Left, colw, 1);
    dl_.text(cx, y + 17, skin_->fit(st.loaded[s] ? fmt("%.1f BPM -> %.1f   %s", st.native_bpm[s], st.bpm[s], display_key(info.row.key, key_musical_).c_str()) : std::string(), 11, 0, colw), 11, th.dim, Align::Left, colw);
    const Rect wave{cx, y + 38, colw, 56};
    dl_.rect(wave.x, wave.y, wave.w, wave.h, rgba(12, 13, 16, 200));
    if (st.loaded[s] && !info.overview.empty() && v.length > 0) {
      const float played = static_cast<float>(v.position / v.length) * wave.w;
      const float cw = wave.w / static_cast<float>(info.overview.size());
      const Color base = s ? th.accent : th.good;
      for (std::size_t i = 0; i < info.overview.size(); ++i) {
        const WavePoint& p = info.overview[i];
        const float px = wave.x + static_cast<float>(i) * cw;
        const float h = std::max(1.0f, wave.h * std::max(p.low, std::max(p.mid, p.high)) / 255.0f);
        dl_.rect(px, wave.y + (wave.h - h) / 2, cw + 0.5f, h, px - wave.x < played ? mix(base, th.bg, 0.6f) : base);
      }
      dl_.rect(wave.x + played - 1, wave.y - 1, 2, wave.h + 2, rgba(255, 255, 255));
      draw_phrase_bands(info, wave.x, wave.y + wave.h + 2, wave.w, 5, 0, v.length / std::max(1.0, v.sample_rate) * 1000.0, false, th);
      if (ov_.pressed && wave.contains(ov_.x, ov_.y)) prep_->seek(s, (ov_.x - wave.x) / wave.w * st.length_ms[s]);
    }
    const float by = y + 110;
    const float bw = (colw - 18) / 4;
    if (ui_button({cx, by, bw, 26}, T(st.playing[s] ? "prep.pause" : "prep.play"), st.playing[s])) prep_->play(s, -1);
    if (ui_button({cx + bw + 6, by, bw, 26}, T("prep.cue"), false)) {
      prep_->cue(s, true);
      prep_->cue(s, false);
    }
    if (ui_button({cx + 2 * (bw + 6), by, bw, 26}, T("prep.use_selection"), false)) prep_load(s, sel);
    if (s == 1 && ui_button({cx + 3 * (bw + 6), by, bw, 26}, T("prep.use_playing"), false)) {
      const int md = master_deck(*engine_);
      if (md >= 0 && md < 2) prep_load(1, engine_->deck(md).track_id);
    }
    if (s == 0 && ui_button({cx + 3 * (bw + 6), by, bw, 26}, T("prep.use_playing"), false)) {
      const int md = master_deck(*engine_);
      if (md >= 0 && md < 2) prep_load(0, engine_->deck(md).track_id);
    }
  }
  y += 148;
  // Blend and tempo follow.
  const Rect slider{x + 90, y + 4, rw - 360, 16};
  dl_.text(x, y + 3, T("prep.blend"), 12, th.text);
  dl_.rrect(slider.x, slider.y + 6, slider.w, 4, 2, th.panel2);
  dl_.circle(slider.x + static_cast<float>(st.blend) * slider.w, slider.y + 8, 8, th.accent);
  if (ov_.pressed && Rect{slider.x - 8, slider.y - 6, slider.w + 16, 28}.contains(ov_.x, ov_.y)) drag_deck_ = 101;
  if (drag_deck_ == 101 && ov_.down) prep_->set_blend(std::clamp((ov_.x - slider.x) / slider.w, 0.0f, 1.0f));
  if (!ov_.down && drag_deck_ == 101) drag_deck_ = -1;
  if (ui_toggle({slider.x + slider.w + 16, y, 150, 24}, T("prep.tempo_follow"), st.tempo_match)) prep_->set_tempo_match(!st.tempo_match);
  std::string err;
  if (ui_button({slider.x + slider.w + 176, y - 1, 84, 26}, T("prep.swap"), st.swap_armed)) {
    if (!prep_->swap_at_next_bar(err)) prep_note_ = err;
  }
  y += 34;
  // The two tracks' phrase bands, lined up at the transition: A's mix-out against B's mix-in.
  dl_.text(x, y, T("prep.aligned"), 11, th.dim);
  y += 16;
  const float mid = x + rw / 2, half = rw / 2 - 4;
  const double span_ms = 90000;
  for (int s = 0; s < 2; ++s) {
    const DeckInfo& info = info_[PrepMode::kDeck[s]];
    if (!st.loaded[s] || prep_facts_for_[s] != st.track[s]) continue;
    const double anchor = s == 0 ? prep_mix_[0].mix_out_ms.value_or(st.length_ms[0]) : prep_mix_[1].mix_in_ms.value_or(0);
    if (s == 0) draw_phrase_bands(info, x + 4, y, half, 14, anchor - span_ms, anchor, true, th);
    else draw_phrase_bands(info, mid, y, half, 14, anchor, anchor + span_ms, true, th);
  }
  dl_.rect(mid - 1, y - 2, 2, 34, rgba(255, 255, 255, 200));
  y += 40;
  // Save the pair into the transition history.
  ui_field({x, y, rw - 150, 24}, f_prep_note_, T("prep.note_hint"));
  if (ui_button({x + rw - 140, y, 140, 24}, T("prep.save_pair"), false) && st.track[0] && st.track[1]) {
    lib_->save_transition(st.track[0], st.track[1], f_prep_note_.text);
    status_note_ = T("prep.pair_saved");
    f_prep_note_.text.clear();
  }
  y += 34;
  if (ui_button({x, y, 200, 28}, T("prep.leave"), false, th.warn)) prep_leave();
  const std::string note = !st.note.empty() ? st.note : prep_note_;
  if (!note.empty()) dl_.text(x + 214, y + 7, skin_->fit(note, 11, 0, rw - 214), 11, th.warn, Align::Left, rw - 214);
  dl_.text(x, m.y + m.h - 22, skin_->fit(T("prep.keys"), 10, 0, rw), 10, th.dim, Align::Left, rw);
}

bool App::prep_key(const std::string& key) {
  if (!prep_ || !prep_->active()) return false;
  std::string err;
  if (key == "1") prep_->play(0, -1);
  else if (key == "2") prep_->play(1, -1);
  else if (key == "s") {
    if (!prep_->swap_at_next_bar(err)) prep_note_ = err;
  } else if (key == "t") prep_->set_tempo_match(!prep_->status().tempo_match);
  else if (key == "left") prep_->set_blend(prep_->status().blend - 0.1);
  else if (key == "right") prep_->set_blend(prep_->status().blend + 0.1);
  else return false;
  return true;
}

// A badge on the main window while the decks are borrowed (drawn every frame, panel or not).
void App::draw_prep_badge() {
  if (!prep_ || !prep_->active()) return;
  const ui::Theme& th = theme_;
  const std::string label = T("prep.badge");
  const float w = dl_.text_width(label, 11) + 20;
  const float x = width_px_ / scale_ - w - 8 - inset_[2], y = 6 + inset_[1];
  dl_.rrect(x, y, w, 20, 4, th.warn);
  dl_.text(x + w / 2, y + 4, label, 11, th.bg, Align::Center, w, 1);
}

}  // namespace deckhand
