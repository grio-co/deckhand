// Linked mix points and vocal regions in the application: what decks draw, the "link the two playheads" action, the list in
// the track details panel, and the background vocal analysis of separated stems.
#include "app_impl.hpp"
#include "deckhand/analysis/vocals.hpp"

namespace deckhand {

namespace {
std::string link_time_text(double ms) {
  const long s = static_cast<long>(std::max(0.0, ms) / 1000.0);
  return fmt("%ld:%02ld", s / 60, s % 60);
}
}  // namespace

// What a deck draws besides the audio: links out of and into the track, and its vocal regions.
void App::deck_extras_load(Library& lib, DeckInfo& info, std::int64_t track_id) {
  info.links_out = lib.mix_links_from(track_id);
  info.links_in = lib.mix_links_to(track_id);
  info.has_vocals = false;
  info.vocals = StoredVocals{};
  if (auto v = lib.vocals(track_id)) {
    info.vocals = std::move(*v);
    info.has_vocals = true;
  }
}

// Control thread: when links or vocal regions changed anywhere, the decks that hold tracks reload them.
void App::links_refresh_loop() {
  const int rev = links_rev_.load();
  if (rev == links_seen_) return;
  links_seen_ = rev;
  for (int d = 0; d < kMaxDecks; ++d) {
    std::int64_t id = 0;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      id = decks_[d].audio ? decks_[d].row.id : 0;
    }
    if (id <= 0) continue;
    Job j;
    j.kind = Job::RefreshLinks;
    j.deck = d;
    j.track_id = id;
    post(j);
  }
  if (panel_ == Panel::Tags) tags_links_stale_ = true;
}

// "Link the playhead of deck A to the playhead of deck B": when A reaches this spot, B starts from its own.
bool App::link_decks(int from, int to) {
  if (from < 0 || to < 0 || from >= kMaxDecks || to >= kMaxDecks || from == to) return false;
  const DeckView a = engine_->deck(from), b = engine_->deck(to);
  const std::int64_t ta = api_deck_track(from), tb = api_deck_track(to);
  if (ta <= 0 || tb <= 0 || a.sample_rate <= 0 || b.sample_rate <= 0) {
    status_note_ = T("link.need_tracks");
    return false;
  }
  if (ta == tb) {
    status_note_ = T("link.same_track");
    return false;
  }
  MixLink l;
  l.from_track = ta;
  l.to_track = tb;
  l.from_ms = a.position / a.sample_rate * 1000.0;
  l.to_ms = b.position / b.sample_rate * 1000.0;
  if (lib_->add_mix_link(l) <= 0) {
    status_note_ = T("link.failed");
    return false;
  }
  ++links_rev_;
  status_note_ = T("link.added", {{"from", deck_letter(from)}, {"to", deck_letter(to)}, {"at", link_time_text(l.from_ms)}, {"into", link_time_text(l.to_ms)}});
  return true;
}

// ---- waveform ----------------------------------------------------------------------------------------------------------
// Markers: a flag pointing forward at the start of a link out of this track, one pointing back at the end of a link into it;
// the vocal regions are a thin band along the top edge.
void App::draw_link_marks(const DeckInfo& info, const Rect& r, double sr, const std::function<float(double)>& x_of, const ui::Theme& th) {
  (void)sr;
  const float xl = r.x, xr = r.x + r.w;
  if (info.has_vocals) {
    for (const StoredVocalRegion& g : info.vocals.regions) {
      const float x0 = std::max(xl, x_of(g.start_ms / 1000.0)), x1 = std::min(xr, x_of(g.end_ms / 1000.0));
      if (x1 > x0) dl_.rect(x0, r.y, x1 - x0, 3, with_alpha(rgba(120, 210, 255), 200));
    }
  }
  for (const MixLink& l : info.links_out) {
    const float x = x_of(l.from_ms / 1000.0);
    if (x < xl - 8 || x > xr + 8) continue;
    dl_.rect(x - 0.5f, r.y + 3, 1.5f, r.h - 3, with_alpha(th.good, 190));
    dl_.triangle(x, r.y + 3, x, r.y + 15, x + 9, r.y + 9, th.good);
  }
  for (const MixLink& l : info.links_in) {
    const float x = x_of(l.to_ms / 1000.0);
    if (x < xl - 8 || x > xr + 8) continue;
    dl_.rect(x - 0.5f, r.y + 3, 1.5f, r.h - 3, with_alpha(th.warn, 190));
    dl_.triangle(x, r.y + 3, x, r.y + 15, x - 9, r.y + 9, th.warn);
  }
}

// ---- track details panel ----------------------------------------------------------------------------------------------------
void App::tag_links_refresh() {
  tag_links_out_ = lib_->mix_links_from(edit_track_);
  tag_links_in_ = lib_->mix_links_to(edit_track_);
  tag_link_titles_.clear();
  for (const auto* v : {&tag_links_out_, &tag_links_in_})
    for (const MixLink& l : *v)
      for (const std::int64_t id : {l.from_track, l.to_track})
        if (id != edit_track_ && !tag_link_titles_.count(id))
          if (const auto row = lib_->track(id)) tag_link_titles_[id] = row->artist.empty() ? row->title : row->artist + " - " + row->title;
  tags_links_stale_ = false;
}

// A list of the track's linked mix points (out first, then in) with a nudge for the position on this track and a delete,
// and one button per other loaded deck that links this track's playhead with that deck's.
void App::draw_tag_links(const Rect& r) {
  const ui::Theme& th = theme_;
  if (tags_links_stale_) tag_links_refresh();
  float y = r.y;
  dl_.text(r.x, y, T("link.title"), 12, th.text);
  y += 20;
  // the decks this track and another track are loaded on
  int mine = -1;
  for (int d = 0; d < kMaxDecks; ++d)
    if (api_deck_track(d) == edit_track_) mine = d;
  if (mine >= 0) {
    float bx = r.x;
    for (int d = 0; d < kMaxDecks; ++d) {
      const std::int64_t other = api_deck_track(d);
      if (d == mine || other <= 0 || other == edit_track_) continue;
      if (ui_button({bx, y, 150, 22}, T("link.to_deck", {{"deck", deck_letter(d)}}), false)) link_decks(mine, d);
      bx += 156;
      if (ui_button({bx, y, 150, 22}, T("link.from_deck", {{"deck", deck_letter(d)}}), false)) link_decks(d, mine);
      bx += 156;
      if (bx + 150 > r.right()) break;
    }
    y += 28;
  }
  struct Row {
    const MixLink* l;
    bool out;
  };
  std::vector<Row> rows;
  for (const MixLink& l : tag_links_out_) rows.push_back({&l, true});
  for (const MixLink& l : tag_links_in_) rows.push_back({&l, false});
  if (rows.empty()) {
    dl_.text(r.x, y, skin_->fit(T("link.none"), 11, 0, r.w), 11, th.dim, Align::Left, r.w);
    return;
  }
  const float rh = 24;
  for (const Row& row : rows) {
    if (y + rh > r.bottom()) {
      dl_.text(r.x, y, "...", 11, th.dim);
      break;
    }
    const MixLink& l = *row.l;
    const std::int64_t other = row.out ? l.to_track : l.from_track;
    const auto t = tag_link_titles_.find(other);
    const std::string name = t == tag_link_titles_.end() ? std::to_string(other) : t->second;
    const double mine_ms = row.out ? l.from_ms : l.to_ms, theirs_ms = row.out ? l.to_ms : l.from_ms;
    const std::string text = T(row.out ? "link.row_out" : "link.row_in", {{"title", name}, {"at", link_time_text(mine_ms)}, {"there", link_time_text(theirs_ms)}});
    dl_.text(r.x, y + 4, skin_->fit(text, 11, 0, r.w - 120), 11, th.text, Align::Left, r.w - 120);
    MixLink edit = l;
    double& pos = row.out ? edit.from_ms : edit.to_ms;
    bool changed = false;
    if (ui_button({r.right() - 112, y, 30, 22}, "-1s", false)) pos = std::max(0.0, pos - 1000), changed = true;
    if (ui_button({r.right() - 78, y, 30, 22}, "+1s", false)) pos += 1000, changed = true;
    const bool del = ui_button({r.right() - 44, y, 44, 22}, T("link.delete"), false, th.warn);
    if (changed) {
      lib_->update_mix_link(edit);
      ++links_rev_;
      tag_links_refresh();
      return;
    }
    if (del) {
      lib_->remove_mix_link(l.id);
      ++links_rev_;
      tag_links_refresh();
      return;
    }
    y += rh;
  }
}

// ---- vocal regions ------------------------------------------------------------------------------------------------------------
// Queues the analysis of a track's cached vocals stem unless a current one is stored (force: a separation just finished).
void App::queue_vocals(Library& lib, std::int64_t track_id, bool force) {
  if (!stems_ || track_id <= 0) return;
  if (!force) {
    const auto v = lib.vocals(track_id);
    if (v && v->version >= kVocalAnalysisVersion) return;
  }
  VocalWork w;
  w.track_id = track_id;
  if (!stems_->cache_ref(track_id, w.root, w.key)) return;
  {
    std::lock_guard<std::mutex> lock(vocals_mutex_);
    if (!vocals_pending_.insert(track_id).second) return;
    vocals_queue_.push_back(std::move(w));
  }
  vocals_cv_.notify_one();
}

// One thread of its own: reading a whole stem file takes a while and must not hold up track loads (or the UI).
void App::vocals_main() {
  std::unique_ptr<Library> lib;
  while (true) {
    VocalWork w;
    {
      std::unique_lock<std::mutex> lock(vocals_mutex_);
      vocals_cv_.wait(lock, [&] { return quit_ || !vocals_queue_.empty(); });
      if (quit_) return;
      w = std::move(vocals_queue_.front());
      vocals_queue_.pop_front();
    }
    std::vector<VocalRegion> regions;
    std::string error;
    const bool ok = detect_vocals_in_cache(w.root, w.key, regions, &error);
    if (ok && !quit_) {
      try {
        if (!lib) lib = std::make_unique<Library>(data_dir_ + "/library.db");
        StoredVocals v;
        v.source = w.key;
        for (const VocalRegion& g : regions) v.regions.push_back({g.start_ms, g.end_ms});
        lib->set_vocals(w.track_id, v);
        ++links_rev_;
      } catch (const std::exception& e) {
        DH_LOG(Warn, "vocal regions of track %lld not stored: %s", static_cast<long long>(w.track_id), e.what());
      }
    } else if (!ok) {
      DH_LOG(Debug, "vocal regions of track %lld: %s", static_cast<long long>(w.track_id), error.c_str());
    }
    std::lock_guard<std::mutex> lock(vocals_mutex_);
    vocals_pending_.erase(w.track_id);
  }
}

}  // namespace deckhand
