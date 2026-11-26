// Track preparation: beat grid editing, memory cues, saved loops, hot cue names and colours, waveform zoom. Everything
// that changes a loaded track goes through App::prep(), which the buttons, the REST API and the named controls share.
// The loaded deck changes at once (Engine::set_track_meta, hot cue ops) and the library follows on the loader thread.
#include "app_impl.hpp"

#include <cstring>

namespace deckhand {

namespace {

constexpr double kMinZoom = 1.5, kMaxZoom = 48.0;
constexpr std::size_t kUndoDepth = 32;

std::string json_text(const std::string& s) {
  std::string out = "\"";
  for (const unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      out += b;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

gridedit::Grid grid_of(const std::vector<GridSection>& g) { return g; }

std::vector<BeatSection> sections_ms(const gridedit::Grid& g, double sr) {
  std::vector<BeatSection> out;
  for (const GridSection& s : g) out.push_back({s.first_beat_frame / sr * 1000.0, s.bpm, s.bar_beat});
  return out;
}

}  // namespace

void App::load_prep_settings() {
  for (int i = 0; i < kMaxDecks; ++i) {
    const double z = std::atof(lib_->setting("ui.wave_zoom." + std::to_string(i + 1), "6").c_str());
    if (z >= kMinZoom && z <= kMaxZoom) wave_zoom_[i] = z;
  }
}

void App::set_wave_zoom(int deck, double seconds) {
  if (deck < 0 || deck >= kMaxDecks) return;
  seconds = std::clamp(seconds, kMinZoom, kMaxZoom);
  if (std::fabs(seconds - wave_zoom_[deck]) < 1e-9) return;
  wave_zoom_[deck] = seconds;
  lib_->set_setting("ui.wave_zoom." + std::to_string(deck + 1), fmt("%.3f", seconds));
}

// Sends the grid and memory cues of a deck to the engine (no audio is reloaded).
void App::send_meta(int deck, std::int64_t track_id, const std::vector<GridSection>& grid, const std::vector<MemCue>& mem, double bpm) {
  TrackMeta m;
  m.bpm = bpm;
  m.grid = grid;
  for (const MemCue& c : mem)
    if (c.end < 0) m.memory_cues.push_back(c.frame);
  std::sort(m.memory_cues.begin(), m.memory_cues.end());
  engine_->set_track_meta(deck, track_id, m);
}

// A hand-edited grid replaces the old one on the deck, in the library, and locks (analysis keeps its hands off).
void App::commit_grid(int deck, std::int64_t track_id, const gridedit::Grid& next, bool push_undo) {
  std::vector<MemCue> mem;
  double sr = 44100, bpm = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    DeckInfo& d = decks_[deck];
    if (!d.audio || d.row.id != track_id) return;
    sr = d.audio->sample_rate;
    if (push_undo) {
      grid_undo_[deck].push_back(grid_of(d.grid));
      if (grid_undo_[deck].size() > kUndoDepth) grid_undo_[deck].erase(grid_undo_[deck].begin());
    }
    d.grid = next;
    d.grid_locked = true;
    bpm = next.empty() ? d.row.bpm : next.front().bpm;
    d.row.bpm = bpm;
    mem = d.mem;
  }
  send_meta(deck, track_id, next, mem, bpm);
  Job j;
  j.kind = Job::SaveGrid;
  j.deck = deck;
  j.track_id = track_id;
  j.grid = sections_ms(next, sr);
  j.ms = bpm;
  j.flag = true;
  post(std::move(j));
  rows_dirty_ = true;
}

void App::commit_mem(int deck, std::int64_t track_id, std::vector<MemCue> mem) {
  std::sort(mem.begin(), mem.end(), [](const MemCue& a, const MemCue& b) { return a.frame < b.frame; });
  std::vector<GridSection> grid;
  double sr = 44100, bpm = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    DeckInfo& d = decks_[deck];
    if (!d.audio || d.row.id != track_id) return;
    sr = d.audio->sample_rate;
    d.mem = mem;
    grid = d.grid;
    bpm = d.row.bpm;
  }
  send_meta(deck, track_id, grid, mem, bpm);
  Job j;
  j.kind = Job::SaveMemCues;
  j.deck = deck;
  j.track_id = track_id;
  for (const MemCue& c : mem) {
    Cue cue;
    cue.kind = c.end > c.frame ? CueKind::Loop : CueKind::Memory;
    cue.start_ms = c.frame / sr * 1000.0;
    cue.end_ms = c.end > c.frame ? c.end / sr * 1000.0 : -1;
    cue.label = c.label;
    cue.color = c.color;
    j.cues.push_back(std::move(cue));
  }
  post(std::move(j));
}

bool App::prep(int deck, const std::string& action, const PrepArgs& a, std::string& error) {
  if (deck < 0 || deck >= kMaxDecks) {
    error = "no such deck";
    return false;
  }
  if (action == "grid.edit") {
    grid_edit_[deck] = a.has_value ? a.value != 0 : !grid_edit_[deck].load();
    return true;
  }
  if (action.compare(0, 5, "grid.") == 0) return prep_grid(deck, action, a, error);
  if (action.compare(0, 4, "cue.") == 0) return prep_cue(deck, action, a, error);
  if (action == "wave.zoom_in" || action == "wave.zoom_out" || action == "wave.zoom") {
    const double cur = wave_zoom_[deck];
    set_wave_zoom(deck, action == "wave.zoom" ? (a.has_value ? a.value : 6.0) : action == "wave.zoom_in" ? cur / 1.5 : cur * 1.5);
    return true;
  }
  error = "unknown action";
  return false;
}

bool App::prep_grid(int deck, const std::string& action, const PrepArgs& a, std::string& error) {
  const DeckView v = engine_->deck(deck);
  if (!v.loaded) {
    error = "no track loaded";
    return false;
  }
  gridedit::Grid cur;
  std::shared_ptr<const TrackAudio> audio;
  std::int64_t tid = 0;
  double row_bpm = 0;
  bool locked = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const DeckInfo& d = decks_[deck];
    if (!d.audio || d.row.id != v.track_id || d.loading) {
      error = "the track is still loading";
      return false;
    }
    cur = grid_of(d.grid);
    audio = d.audio;
    tid = d.row.id;
    row_bpm = d.row.bpm;
    locked = d.grid_locked;
  }
  const double sr = audio->sample_rate, at = v.position;
  const std::string op = action.substr(5);

  if (op == "lock") {
    const bool on = a.has_value ? a.value != 0 : !locked;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      decks_[deck].grid_locked = on;
    }
    Job j;
    j.kind = Job::SaveGrid;
    j.track_id = tid;
    j.flag = on;
    j.ms = -1;  // lock flag only
    post(std::move(j));
    return true;
  }
  if (op == "reanalyse") {
    if (locked) {
      error = "the grid is locked; unlock it first";
      set_status(T("grid.locked_note"));
      return false;
    }
    Job j;
    j.kind = Job::Reanalyse;
    j.deck = deck;
    j.track_id = tid;
    post(std::move(j));
    return true;
  }
  if (op == "undo") {
    gridedit::Grid prev;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (grid_undo_[deck].empty()) {
        error = "nothing to undo";
        return false;
      }
      prev = grid_undo_[deck].back();
      grid_undo_[deck].pop_back();
    }
    commit_grid(deck, tid, prev, false);
    return true;
  }

  // Edits that need a grid to start from: make a one-section grid at the playhead.
  const bool needs_base = op == "align" || op == "set_first_beat" || op == "set_bpm" || op == "bpm_delta" || op == "tap";
  if (cur.empty() && needs_base) cur = {{std::max(0.0, at), row_bpm > 0 ? row_bpm : 120.0, 1}};
  gridedit::Grid next = cur;
  if (op == "shift") {
    if (!a.has_value) {
      error = "value needed: milliseconds, signed";
      return false;
    }
    next = gridedit::shift(cur, a.value * sr / 1000.0, sr);
  } else if (op == "align") {
    next = gridedit::align(cur, at, sr);
  } else if (op == "set_first_beat") {
    next = gridedit::set_first_beat(cur, at, sr);
  } else if (op == "downbeat") {
    next = gridedit::set_downbeat(cur, at, sr);
  } else if (op == "rotate") {
    next = gridedit::rotate_downbeat(cur, a.has_value ? static_cast<int>(std::lround(a.value)) : 1);
  } else if (op == "bpm_delta") {
    if (!a.has_value) {
      error = "value needed: BPM change";
      return false;
    }
    next = gridedit::bpm_delta(cur, at, a.value, sr);
  } else if (op == "set_bpm") {
    if (!a.has_value || !(a.value >= gridedit::kMinBpm && a.value <= gridedit::kMaxBpm)) {
      error = "value needed: BPM between 30 and 300";
      return false;
    }
    next = gridedit::set_bpm(cur, at, a.value, sr);
  } else if (op == "scale") {
    if (!a.has_value || !(a.value == 0.5 || a.value == 2.0 || a.value == 1.5 || a.value == 2.0 / 3.0)) {
      error = "value must be 0.5 (halve) or 2 (double)";
      return false;
    }
    next = gridedit::scale_bpm(cur, a.value, sr);
  } else if (op == "tap") {
    double bpm = 0;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      tap_times_[deck].push_back(now_seconds());
      if (tap_times_[deck].size() > 16) tap_times_[deck].erase(tap_times_[deck].begin());
      bpm = gridedit::tap_bpm(tap_times_[deck]);
    }
    if (bpm <= 0) return true;  // keep tapping
    next = gridedit::set_bpm(cur, at, std::round(bpm * 100.0) / 100.0, sr);
  } else if (op == "add_section") {
    next = gridedit::add_section(cur, a.has_value ? a.value * sr : at, sr);
  } else if (op == "remove_section") {
    next = gridedit::remove_section(cur, a.has_value ? a.value * sr : at, sr);
  } else {
    error = "unknown grid action";
    return false;
  }
  if (gridedit::same(next, cur) && !cur.empty()) return true;  // nothing changed (limits, no grid)
  if (next.empty()) {
    error = "this track has no beat grid to edit";
    return false;
  }
  commit_grid(deck, tid, next, true);
  return true;
}

bool App::prep_cue(int deck, const std::string& action, const PrepArgs& a, std::string& error) {
  const DeckView v = engine_->deck(deck);
  if (!v.loaded) {
    error = "no track loaded";
    return false;
  }
  std::vector<MemCue> mem;
  gridedit::Grid grid;
  std::int64_t tid = 0;
  double sr = v.sample_rate;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const DeckInfo& d = decks_[deck];
    if (!d.audio || d.row.id != v.track_id || d.loading) {
      error = "the track is still loading";
      return false;
    }
    mem = d.mem;
    grid = grid_of(d.grid);
    tid = d.row.id;
    sr = d.audio->sample_rate;
  }
  const std::string op = action.substr(4);
  const auto snap = [&](double frame) { return v.quantize && !grid.empty() ? gridedit::nearest_beat(grid, frame, sr) : frame; };
  const auto seconds_or_playhead = [&] { return a.has_value ? std::clamp(a.value * sr, 0.0, v.length) : v.position; };
  const auto slot_of = [&](int& slot) {
    slot = a.slot - 1;
    if (slot < 0 || slot >= kHotCues) {
      error = "slot must be 1..8";
      return false;
    }
    return true;
  };

  if (op == "memory_set") {
    double at = a.has_value ? seconds_or_playhead() : snap(v.position);
    for (const MemCue& c : mem)
      if (c.end < 0 && std::fabs(c.frame - at) < sr * 0.01) {
        error = "there is a memory cue here already";
        return false;
      }
    MemCue c;
    c.frame = at;
    if (a.has_text) c.label = a.text;
    c.color = a.color;
    mem.push_back(c);
    commit_mem(deck, tid, mem);
    return true;
  }
  if (op == "memory_delete" || op == "memory_style") {
    int idx = -1;
    if (a.slot > 0) {  // 1-based index among the memory cues (not loops), in order
      int n = 0;
      for (std::size_t i = 0; i < mem.size(); ++i)
        if (mem[i].end < 0 && ++n == a.slot) idx = static_cast<int>(i);
    } else {  // nearest to the playhead, within a second
      double best = sr;
      for (std::size_t i = 0; i < mem.size(); ++i)
        if (mem[i].end < 0 && std::fabs(mem[i].frame - v.position) < best) {
          best = std::fabs(mem[i].frame - v.position);
          idx = static_cast<int>(i);
        }
    }
    if (idx < 0) {
      error = "no such memory cue";
      return false;
    }
    if (op == "memory_delete") {
      mem.erase(mem.begin() + idx);
    } else {
      if (a.has_text) mem[static_cast<std::size_t>(idx)].label = a.text;
      if (a.has_color) mem[static_cast<std::size_t>(idx)].color = a.color;
    }
    commit_mem(deck, tid, mem);
    return true;
  }
  if (op == "memory_move") {  // value = new position in seconds, slot = 1-based index among the memory cues
    int n = 0, idx = -1;
    for (std::size_t i = 0; i < mem.size(); ++i)
      if (mem[i].end < 0 && ++n == a.slot) idx = static_cast<int>(i);
    if (idx < 0 || !a.has_value) {
      error = "give slot (the memory cue's number) and value (seconds)";
      return false;
    }
    mem[static_cast<std::size_t>(idx)].frame = std::clamp(snap(a.value * sr), 0.0, v.length);
    commit_mem(deck, tid, mem);
    return true;
  }
  if (op == "loop_save") {
    if (!(v.loop_in >= 0 && v.loop_out > v.loop_in)) {
      error = "there is no loop to save";
      return false;
    }
    for (const MemCue& c : mem)
      if (c.end > 0 && std::fabs(c.frame - v.loop_in) < sr * 0.01 && std::fabs(c.end - v.loop_out) < sr * 0.01) {
        error = "this loop is saved already";
        return false;
      }
    MemCue c;
    c.frame = v.loop_in;
    c.end = v.loop_out;
    if (a.has_text) c.label = a.text;
    c.color = a.color;
    mem.push_back(c);
    commit_mem(deck, tid, mem);
    return true;
  }
  if (op == "loop_recall" || op == "loop_delete") {  // slot = 1-based index among the saved loops; none: the next one
    std::vector<std::size_t> loops;
    for (std::size_t i = 0; i < mem.size(); ++i)
      if (mem[i].end > mem[i].frame) loops.push_back(i);
    if (loops.empty()) {
      error = "no saved loops";
      return false;
    }
    std::size_t pick = 0;
    if (a.slot > 0) {
      if (static_cast<std::size_t>(a.slot) > loops.size()) {
        error = "no such saved loop";
        return false;
      }
      pick = static_cast<std::size_t>(a.slot - 1);
    } else {  // cycle: the first saved loop after the current loop start
      pick = 0;
      for (std::size_t k = 0; k < loops.size(); ++k)
        if (mem[loops[k]].frame > v.loop_in + 1) {
          pick = k;
          break;
        }
    }
    if (op == "loop_delete") {
      mem.erase(mem.begin() + static_cast<std::ptrdiff_t>(loops[pick]));
      commit_mem(deck, tid, mem);
      return true;
    }
    const MemCue& m = mem[loops[pick]];
    Command c;
    c.deck = static_cast<std::uint8_t>(deck);
    c.op = Op::LoopSetIn;
    c.d = m.frame;
    engine_->send(c);
    c.op = Op::LoopSetOut;
    c.d = m.end;
    engine_->send(c);
    if (v.position < m.frame || v.position >= m.end) {
      c.op = Op::Seek;
      c.d = m.frame;
      engine_->send(c);
    }
    return true;
  }
  if (op == "hot_set" || op == "hot_style" || op == "hot_loop" || op == "hot_delete") {
    int slot = 0;
    if (!slot_of(slot)) return false;
    Command c;
    c.deck = static_cast<std::uint8_t>(deck);
    c.i = slot;
    if (op == "hot_set") {
      c.op = Op::HotCueSetAt;
      c.d = a.has_value ? std::clamp(a.value * sr, 0.0, v.length) : snap(v.position);
      engine_->send(c);
      return true;
    }
    if (op == "hot_delete") {
      c.op = Op::HotCueSetAt;
      c.d = -1;
      engine_->send(c);
      return true;
    }
    if (v.hot_cue[slot] < 0) {
      error = "this hot cue is not set";
      return false;
    }
    if (op == "hot_loop") {
      // a hot cue as a loop: its length is the active loop (or `value` beats at the deck's tempo)
      double end = -1;
      if (v.loop_in >= 0 && v.loop_out > v.loop_in && v.loop_in >= v.hot_cue[slot] - sr * 0.01 && v.loop_in <= v.hot_cue[slot] + sr * 0.01)
        end = v.loop_out;
      else if (a.has_value && v.bpm > 0)
        end = v.hot_cue[slot] + a.value * 60.0 / (v.bpm / (1.0 + v.tempo)) * sr;
      else if (v.loop_in >= 0 && v.loop_out > v.loop_in)
        end = v.hot_cue[slot] + (v.loop_out - v.loop_in);
      else if (v.bpm > 0)
        end = v.hot_cue[slot] + 4.0 * 60.0 / (v.bpm / (1.0 + v.tempo)) * sr;  // four beats
      if (v.hot_cue_end[slot] > 0 && !a.has_value) end = -1;                   // already a loop: back to a plain cue
      end = end > v.hot_cue[slot] ? std::min(end, v.length) : -1;
      c.op = Op::HotCueEndAt;
      c.d = end;
      engine_->send(c);
      Job j;
      j.kind = Job::SaveHotCue;
      j.track_id = tid;
      j.slot = slot;
      j.ms = v.hot_cue[slot] / sr * 1000.0;
      j.ms_end = end > 0 ? end / sr * 1000.0 : -1;
      post(std::move(j));
      return true;
    }
    // hot_style: name and colour
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      DeckInfo& d = decks_[deck];
      if (a.has_text) d.hot_label[slot] = a.text.substr(0, 40);
      if (a.has_color) d.hot_color[slot] = a.color;
      Job j;
      j.kind = Job::SaveHotStyle;
      j.track_id = tid;
      j.slot = slot;
      j.text = d.hot_label[slot];
      j.color = d.hot_color[slot];
      j.ms = v.hot_cue[slot] / sr * 1000.0;
      j.ms_end = v.hot_cue_end[slot] > 0 ? v.hot_cue_end[slot] / sr * 1000.0 : -1;
      post(std::move(j));
    }
    return true;
  }
  if (op == "main_set") {
    Command c;
    c.op = Op::CueSetAt;
    c.deck = static_cast<std::uint8_t>(deck);
    c.d = a.has_value ? std::clamp(a.value * sr, 0.0, v.length) : snap(v.position);
    engine_->send(c);
    return true;
  }
  error = "unknown cue action";
  return false;
}

// Re-runs the analysis on the loaded track (loader thread). A locked grid is never replaced.
void App::reanalyse(Library& lib, int deck, std::int64_t track_id) {
  std::shared_ptr<const TrackAudio> audio;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (decks_[deck].row.id != track_id) return;
    audio = decks_[deck].audio;
  }
  if (!audio) return;
  if (lib.grid_locked(track_id)) {
    set_status(T("grid.locked_note"));
    return;
  }
  const TrackAnalysis an = analyse_track(*audio);
  EditScope analysis_scope("analysis");  // the BPM stored below is DeckHand's own result
  if (an.has_audible) lib.set_analysis_cues(track_id, an.suggested_cue_ms, an.first_audible_ms);
  if (!an.has_grid) {
    set_status(T("grid.no_result"));
    return;
  }
  const double sr = audio->sample_rate;
  gridedit::Grid g;
  for (const BeatSection& b : an.grid) g.push_back({b.first_beat_ms / 1000.0 * sr, b.bpm, b.bar_beat});
  g = gridedit::normalise(g, sr);
  std::vector<MemCue> mem;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    DeckInfo& d = decks_[deck];
    if (d.row.id != track_id) return;
    grid_undo_[deck].push_back(grid_of(d.grid));
    if (grid_undo_[deck].size() > kUndoDepth) grid_undo_[deck].erase(grid_undo_[deck].begin());
    d.grid = g;
    d.row.bpm = an.bpm;
    mem = d.mem;
  }
  lib.replace_beatgrid(track_id, an.grid);
  lib.set_bpm(track_id, an.bpm);
  send_meta(deck, track_id, g, mem, an.bpm);
  rows_dirty_ = true;
  set_status(T("grid.reanalysed", {{"bpm", an.bpm}}));
}

std::string App::api_deck_prep_json(int deck) {
  if (deck < 0 || deck >= kMaxDecks) return {};
  const DeckView v = engine_->deck(deck);
  if (!v.loaded) return {};
  std::lock_guard<std::mutex> lock(state_mutex_);
  const DeckInfo& d = decks_[deck];
  if (!d.audio || d.row.id != v.track_id) return {};
  const double sr = d.audio->sample_rate;
  std::string out = "{\"track_id\":" + std::to_string(d.row.id) + ",\"grid_locked\":" + (d.grid_locked ? "true" : "false") +
                    ",\"grid_edit\":" + (grid_edit_[deck].load() ? "true" : "false") + ",\"undo_depth\":" + std::to_string(grid_undo_[deck].size()) +
                    ",\"wave_zoom_seconds\":" + fmt("%.3f", wave_zoom_[deck]) + ",\"metronome\":" + (v.metronome ? "true" : "false") + ",\"grid\":[";
  for (std::size_t i = 0; i < d.grid.size(); ++i)
    out += (i ? "," : "") + fmt("{\"first_beat_s\":%.6f,\"bpm\":%.4f,\"bar_beat\":%d}", d.grid[i].first_beat_frame / sr, d.grid[i].bpm, d.grid[i].bar_beat);
  out += "],\"memory_cues\":[";
  bool first = true;
  for (const MemCue& c : d.mem) {
    out += (first ? "" : ",") + fmt("{\"time_s\":%.6f", c.frame / sr);
    if (c.end > c.frame) out += fmt(",\"end_s\":%.6f", c.end / sr);
    out += ",\"label\":" + json_text(c.label) + ",\"color\":" + std::to_string(c.color) + "}";
    first = false;
  }
  out += "],\"hot_cues\":[";
  first = true;
  for (int i = 0; i < kHotCues; ++i) {
    if (v.hot_cue[i] < 0) continue;
    out += (first ? "" : ",") + fmt("{\"slot\":%d,\"time_s\":%.6f", i + 1, v.hot_cue[i] / sr);
    if (v.hot_cue_end[i] > v.hot_cue[i]) out += fmt(",\"end_s\":%.6f", v.hot_cue_end[i] / sr);
    out += ",\"label\":" + json_text(d.hot_label[i]) + ",\"color\":" + std::to_string(d.hot_color[i]) + "}";
    first = false;
  }
  out += "],\"loop\":";
  out += v.loop_in >= 0 && v.loop_out > v.loop_in ? fmt("{\"in_s\":%.6f,\"out_s\":%.6f,\"active\":%s}", v.loop_in / sr, v.loop_out / sr, v.loop_active ? "true" : "false") : "null";
  return out + "}";
}

// ---- hot cue / memory cue editor (overlay) ----------------------------------------------------------------------------
void App::open_cue_editor(int deck, int slot, int mem_index) {
  const DeckView v = engine_->deck(deck);
  if (!v.loaded) return;
  cue_edit_deck_ = deck;
  cue_edit_slot_ = slot;
  cue_edit_mem_ = mem_index;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const DeckInfo& d = decks_[deck];
    if (slot >= 0) {
      if (v.hot_cue[slot] < 0) return;
      f_cue_name_.text = d.hot_label[slot];
      cue_edit_color_ = d.hot_color[slot];
    } else if (mem_index >= 0 && mem_index < static_cast<int>(d.mem.size())) {
      f_cue_name_.text = d.mem[static_cast<std::size_t>(mem_index)].label;
      cue_edit_color_ = d.mem[static_cast<std::size_t>(mem_index)].color;
    } else {
      return;
    }
  }
  open_panel(Panel::CueEdit);
  focus_ = &f_cue_name_;
}

void App::apply_cue_editor(bool remove) {
  PrepArgs a;
  a.has_text = true;
  a.text = f_cue_name_.text;
  a.color = cue_edit_color_;
  a.has_color = true;
  std::string error;
  if (cue_edit_slot_ >= 0) {
    a.slot = cue_edit_slot_ + 1;
    prep(cue_edit_deck_, remove ? "cue.hot_delete" : "cue.hot_style", a, error);
  } else {
    int n = 0;  // the editor knows the index into the whole list; the action counts memory cues only
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      const auto& mem = decks_[cue_edit_deck_].mem;
      for (int i = 0; i <= cue_edit_mem_ && i < static_cast<int>(mem.size()); ++i)
        if (mem[static_cast<std::size_t>(i)].end < 0) ++n;
    }
    a.slot = n;
    prep(cue_edit_deck_, remove ? "cue.memory_delete" : "cue.memory_style", a, error);
  }
  if (!error.empty()) set_status(T("status.error", {{"message", error}}));
}

void App::draw_cue_edit(const Rect& m) {
  const ui::Theme& th = theme_;
  const float x = m.x + 20, lw = 70, fx = x + lw, fw = m.w - 40 - lw;
  float y = m.y + 48;
  dl_.text(x, y + 5, T("cue.name"), 12, th.dim, Align::Left, lw - 6);
  ui_field({fx, y, fw, 26}, f_cue_name_, T("cue.name_hint"));
  y += 38;
  dl_.text(x, y + 5, T("cue.color"), 12, th.dim, Align::Left, lw - 6);
  const float sw = std::min(28.0f, (fw - 8.0f * 4) / 8.0f);
  for (int i = 0; i < kPaletteSize; ++i) {
    const Rect r{fx + static_cast<float>(i % 8) * (sw + 4), y + static_cast<float>(i / 8) * (sw + 4), sw, sw};
    const Color c = rgba(static_cast<int>((kPalette[i] >> 16) & 255), static_cast<int>((kPalette[i] >> 8) & 255), static_cast<int>(kPalette[i] & 255));
    dl_.rrect(r.x, r.y, r.w, r.h, 4, c);
    const bool sel = cue_edit_color_ == static_cast<int>(kPalette[i]);
    if (sel) dl_.rrect_outline(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 5, th.text, 2);
    if (ov_.pressed && r.contains(ov_.x, ov_.y)) cue_edit_color_ = static_cast<int>(kPalette[i]);
  }
  y += (sw + 4) * 2 + 8;
  if (ui_button({fx, y, 120, 26}, T("cue.default_color"), cue_edit_color_ < 0)) cue_edit_color_ = -1;
  y += 40;
  if (ui_button({x, y, 110, 28}, T("cue.save"), true, th.good)) {
    apply_cue_editor(false);
    close_panel();
    return;
  }
  if (cue_edit_slot_ >= 0 && ui_button({x + 118, y, 130, 28}, engine_->deck(cue_edit_deck_).hot_cue_end[cue_edit_slot_] > 0 ? T("cue.make_plain") : T("cue.make_loop"), false)) {
    PrepArgs a;
    a.slot = cue_edit_slot_ + 1;
    std::string error;
    prep(cue_edit_deck_, "cue.hot_loop", a, error);
    close_panel();
    return;
  }
  const bool sure = confirm_remove_;
  if (ui_button({m.x + m.w - 20 - 130, y, 130, 28}, sure ? T("cue.delete_confirm") : T("cue.delete"), sure, th.warn)) {
    if (sure) {
      apply_cue_editor(true);
      close_panel();
      return;
    }
    confirm_remove_ = true;
  }
}

// ---- waveform marker hit test --------------------------------------------------------------------------------------------
App::CueHit App::cue_hit_test(int di, const Rect& r, const DeckView& v, const DeckInfo& info, double window, float x, float y) const {
  CueHit hit;
  if (!v.loaded || !info.audio || !r.contains(x, y)) return hit;
  const double sr = info.audio->sample_rate, pos = v.position / sr, per = r.w / window;
  const float cx = r.x + r.w / 2;
  const auto x_of = [&](double frame) { return cx + static_cast<float>((frame / sr - pos) * per); };
  float best = 7;
  const float tag_h = std::min(13.0f, r.h * 0.5f);
  if (y <= r.y + tag_h + 2) {
    const float xc = x_of(v.cue);
    if (std::fabs(x - xc) < best && std::fabs(x - xc) <= 6) {
      best = std::fabs(x - xc);
      hit = {3, 0};
    }
    for (int h = 0; h < kHotCues; ++h) {
      if (v.hot_cue[h] < 0) continue;
      const float x0 = x_of(v.hot_cue[h]);
      const float d = x >= x0 && x <= x0 + 14 ? 0.0f : std::min(std::fabs(x - x0), std::fabs(x - (x0 + 14)));
      if (d < best) {
        best = d;
        hit = {1, h};
      }
    }
  }
  if (y >= r.y + r.h - tag_h - 2) {
    for (std::size_t i = 0; i < info.mem.size(); ++i) {
      const float d = std::fabs(x - x_of(info.mem[i].frame));
      if (d < best) {
        best = d;
        hit = {2, static_cast<int>(i)};
      }
    }
  }
  (void)di;
  return hit;
}

}  // namespace deckhand
