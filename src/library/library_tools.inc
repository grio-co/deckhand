// Library tools: cues that follow the grid, selective analysis removal, duplicate finder. Included at the end of
// library.cpp (inside namespace deckhand) so it shares that file's helpers (OptionalTx, the edit context, unix_now ...).

// ---- text forms the journal stores ----------------------------------------------------------------------------------

namespace {
std::string num(double v) {
  char b[40];
  std::snprintf(b, sizeof b, "%.15g", v);
  return b;
}

std::string escape_field(const std::string& s) {
  std::string o;
  for (const char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\t') o += "\\t";
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o;
}

std::string unescape_field(const std::string& s) {
  std::string o;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char n = s[++i];
      o += n == 't' ? '\t' : n == 'n' ? '\n' : n;
    } else {
      o += s[i];
    }
  }
  return o;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::size_t from = 0;
  while (true) {
    const auto at = s.find(sep, from);
    out.push_back(s.substr(from, at == std::string::npos ? std::string::npos : at - from));
    if (at == std::string::npos) break;
    from = at + 1;
  }
  return out;
}

void insert_cue(Db& db, std::int64_t track_id, const Cue& c) {
  Stmt i(db, "INSERT INTO cues(track_id, kind, slot, start_ms, end_ms, label, color) VALUES (?1,?2,?3,?4,?5,?6,?7)");
  i.bind(1, track_id).bind(2, c.kind == CueKind::Hot ? "hot" : c.kind == CueKind::Loop ? "loop" : "memory");
  if (c.slot >= 0) i.bind(3, c.slot);
  else i.bind_null(3);
  i.bind(4, c.start_ms);
  if (c.end_ms >= 0) i.bind(5, c.end_ms);
  else i.bind_null(5);
  if (c.label.empty()) i.bind_null(6);
  else i.bind(6, c.label);
  if (c.color < 0) i.bind_null(7);
  else i.bind(7, c.color);
  i.run();
}

bool same_cue(const Cue& a, const Cue& b) {
  return a.kind == b.kind && a.slot == b.slot && std::fabs(a.start_ms - b.start_ms) < 1e-6 && std::fabs(a.end_ms - b.end_ms) < 1e-6;
}
}  // namespace

std::string grid_text(const std::vector<BeatSection>& grid) {
  std::string o;
  for (const BeatSection& g : grid) o += num(g.first_beat_ms) + "," + num(g.bpm) + "," + std::to_string(g.bar_beat) + ";";
  return o;
}

std::vector<BeatSection> grid_from_text(const std::string& text) {
  std::vector<BeatSection> out;
  for (const std::string& part : split(text, ';')) {
    const auto f = split(part, ',');
    if (f.size() != 3) continue;
    out.push_back({std::strtod(f[0].c_str(), nullptr), std::strtod(f[1].c_str(), nullptr), std::atoi(f[2].c_str())});
  }
  return out;
}

std::string cues_text(const std::vector<Cue>& cues) {
  std::string o;
  for (const Cue& c : cues)
    o += std::string(c.kind == CueKind::Hot ? "h" : c.kind == CueKind::Loop ? "l" : "m") + "\t" + std::to_string(c.slot) + "\t" +
         num(c.start_ms) + "\t" + (c.end_ms >= 0 ? num(c.end_ms) : "") + "\t" + std::to_string(c.color) + "\t" + escape_field(c.label) + "\n";
  return o;
}

std::vector<Cue> cues_from_text(const std::string& text) {
  std::vector<Cue> out;
  for (const std::string& line : split(text, '\n')) {
    const auto f = split(line, '\t');
    if (f.size() != 6) continue;
    Cue c;
    c.kind = f[0] == "h" ? CueKind::Hot : f[0] == "l" ? CueKind::Loop : CueKind::Memory;
    c.slot = std::atoi(f[1].c_str());
    c.start_ms = std::strtod(f[2].c_str(), nullptr);
    c.end_ms = f[3].empty() ? -1 : std::strtod(f[3].c_str(), nullptr);
    c.color = std::atoi(f[4].c_str());
    c.label = unescape_field(f[5]);
    out.push_back(std::move(c));
  }
  return out;
}

CueFollow cue_follow_from(const std::string& text, CueFollow fallback) {
  if (text == "ask") return CueFollow::Ask;
  if (text == "always") return CueFollow::Always;
  if (text == "never") return CueFollow::Never;
  return fallback;
}

const char* cue_follow_name(CueFollow f) { return f == CueFollow::Always ? "always" : f == CueFollow::Never ? "never" : "ask"; }

// ---- cues follow the grid ---------------------------------------------------------------------------------------------

namespace {
std::vector<BeatSection> sorted_grid(std::vector<BeatSection> g) {
  g.erase(std::remove_if(g.begin(), g.end(), [](const BeatSection& s) { return !(s.bpm > 0); }), g.end());
  std::stable_sort(g.begin(), g.end(), [](const BeatSection& a, const BeatSection& b) { return a.first_beat_ms < b.first_beat_ms; });
  return g;
}

// The section whose beat lattice covers `ms` (the first one extends backwards).
std::size_t section_of(const std::vector<BeatSection>& g, double ms) {
  std::size_t at = 0;
  for (std::size_t k = 1; k < g.size(); ++k) {
    if (ms >= g[k].first_beat_ms - 1e-6) at = k;
    else break;
  }
  return at;
}

// The same lattice under another name: the tempo became half or double (or a quarter / four times).
bool relabel_ratio(double ratio) {
  for (const double n : {0.25, 0.5, 2.0, 4.0})
    if (std::fabs(ratio / n - 1) < 0.003) return true;
  return false;
}
}  // namespace

CueFollowProposal propose_cue_follow(const std::vector<Cue>& cues, const std::vector<BeatSection>& old_in,
                                     const std::vector<BeatSection>& new_in, double tolerance_ms) {
  CueFollowProposal p;
  p.tolerance_ms = tolerance_ms;
  const auto old_grid = sorted_grid(old_in), new_grid = sorted_grid(new_in);
  if (old_grid.empty() || new_grid.empty()) {
    p.no_grid = true;
    p.off_grid = cues;
    return p;
  }
  for (const Cue& c : cues) {
    const std::size_t i = section_of(old_grid, c.start_ms);
    const BeatSection& o = old_grid[i];
    const double beats = (c.start_ms - o.first_beat_ms) * o.bpm / 60000.0;
    const double quarter = beats * 4.0;
    const double off_ms = std::fabs(quarter - std::round(quarter)) / 4.0 * 60000.0 / o.bpm;  // to a beat, half or quarter beat
    if (off_ms > tolerance_ms) {
      p.off_grid.push_back(c);
      continue;
    }
    std::size_t j = i;
    if (new_grid.size() != old_grid.size()) {  // sections were added or removed: the one that starts nearest
      double best = 1e300;
      for (std::size_t k = 0; k < new_grid.size(); ++k) {
        const double d = std::fabs(new_grid[k].first_beat_ms - o.first_beat_ms);
        if (d < best) best = d, j = k;
      }
    }
    const BeatSection& n = new_grid[j];
    CueMove m;
    m.before = m.after = c;
    m.beats = beats;
    if (relabel_ratio(n.bpm / o.bpm)) {
      const double delta = n.first_beat_ms - o.first_beat_ms;
      m.after.start_ms = c.start_ms + delta;
      if (c.end_ms >= 0) m.after.end_ms = c.end_ms + delta;
    } else {
      m.after.start_ms = n.first_beat_ms + beats * 60000.0 / n.bpm;
      if (c.end_ms >= 0) m.after.end_ms = m.after.start_ms + (c.end_ms - c.start_ms) * o.bpm / n.bpm;  // same length in beats
    }
    const bool same = std::fabs(m.after.start_ms - c.start_ms) < 0.005 && (c.end_ms < 0 || std::fabs(m.after.end_ms - c.end_ms) < 0.005);
    if (same) p.unchanged.push_back(c);
    else p.moves.push_back(std::move(m));
  }
  return p;
}

void Library::replace_all_cues(std::int64_t track_id, const std::vector<Cue>& list) {
  EditScope scope("user");
  OptionalTx tx(db_);
  const std::vector<Cue> before = cues(track_id);
  Stmt d(db_, "DELETE FROM cues WHERE track_id = ?1");
  d.bind(1, track_id);
  d.run();
  for (const Cue& c : list) insert_cue(db_, track_id, c);
  const std::string a = cues_text(before), b = cues_text(cues(track_id));
  if (a != b) journal_record(track_id, "cues", a, b);
  tx.commit();
}

void Library::mark_suggested_cues(std::int64_t track_id, const std::vector<Cue>& added) {
  if (added.empty()) return;
  const std::string key = "cues.suggested." + std::to_string(track_id);
  std::string text = setting(key);
  for (const Cue& c : added) {
    const std::string line = cues_text({c});
    if (text.find(line) == std::string::npos) text += line;
  }
  set_setting(key, text);
}

std::vector<Cue> Library::user_cues(std::int64_t track_id) {
  std::vector<Cue> all = cues(track_id);
  const std::string text = setting("cues.suggested." + std::to_string(track_id));
  if (text.empty()) return all;
  std::vector<Cue> out;
  for (Cue& c : all) {
    const std::string line = cues_text({c});
    // a whole line only: the text is a list of lines ending in a newline
    std::size_t at = 0;
    bool suggested = false;
    while ((at = text.find(line, at)) != std::string::npos) {
      if (at == 0 || text[at - 1] == '\n') {
        suggested = true;
        break;
      }
      ++at;
    }
    if (!suggested) out.push_back(std::move(c));
  }
  return out;
}

void Library::apply_cue_moves(std::int64_t track_id, const std::vector<Cue>& before, const CueFollowProposal& p) {
  std::vector<Cue> next = before;
  std::vector<bool> used(p.moves.size(), false);
  for (Cue& c : next) {
    for (std::size_t k = 0; k < p.moves.size(); ++k) {
      if (used[k] || !same_cue(c, p.moves[k].before)) continue;
      used[k] = true;
      c.start_ms = p.moves[k].after.start_ms;
      c.end_ms = p.moves[k].after.end_ms;
      break;
    }
  }
  replace_all_cues(track_id, next);
}

GridCommit Library::commit_grid(std::int64_t track_id, const std::vector<BeatSection>& grid, std::optional<double> bpm,
                                std::optional<bool> lock, CueFollow mode, double tolerance_ms, bool dry_run) {
  GridCommit r;
  r.mode = mode;
  EditScope scope("user");
  OptionalTx tx(db_);
  if (dry_run && !tx.own) throw std::runtime_error("a dry run cannot join an open transaction");
  const std::vector<BeatSection> old_grid = beatgrid(track_id);
  const std::vector<Cue> old_cues = cues(track_id);
  const FieldValues before = edit_snapshot(track_id);
  replace_beatgrid(track_id, grid);
  if (bpm) {
    Stmt u(db_, "UPDATE tracks SET bpm = ?1 WHERE id = ?2");
    u.bind(1, *bpm).bind(2, track_id);
    u.run();
  }
  if (lock) {
    Stmt u(db_, "UPDATE tracks SET grid_locked = ?1 WHERE id = ?2");
    u.bind(1, *lock ? 1 : 0).bind(2, track_id);
    u.run();
  }
  edit_journal(track_id, before);
  const std::string a = grid_text(old_grid), b = grid_text(grid);
  r.grid_changed = a != b;
  if (r.grid_changed) journal_record(track_id, "grid", a, b);
  if (mode != CueFollow::Never && r.grid_changed) {
    r.proposal = propose_cue_follow(old_cues, old_grid, grid, tolerance_ms);
    if (mode == CueFollow::Always && !r.proposal.moves.empty()) {
      apply_cue_moves(track_id, old_cues, r.proposal);
      r.applied = true;
    }
    r.mix_links_moved = follow_mix_links(track_id, old_grid, grid, tolerance_ms, mode == CueFollow::Always);
  }
  r.batch_id = scope.batch();
  if (dry_run) tx.rollback();
  else tx.commit();
  return r;
}

FollowResult Library::apply_cue_follow(std::int64_t track_id, std::int64_t batch_id, double tolerance_ms) {
  FollowResult r;
  r.batch_id = batch_id;
  std::optional<TrackEditRecord> grid_row;
  bool has_cues = false;
  for (const TrackEditRecord& e : read_edits("WHERE batch_id = ?1", batch_id, 1000000)) {
    if (e.field == "mix_links") has_cues = true;  // links already followed (their journal row sits on the from-track)
    if (e.track_id != track_id) continue;
    if (e.field == "grid" && !grid_row) grid_row = e;
    if (e.field == "cues") has_cues = true;
  }
  if (!grid_row) return r;
  if (has_cues) {
    r.status = FollowStatus::AlreadyApplied;
    return r;
  }
  if (grid_text(beatgrid(track_id)) != grid_row->new_value.value_or("")) {
    r.status = FollowStatus::Conflict;  // the grid was edited again since
    return r;
  }
  const std::vector<Cue> now = cues(track_id);
  r.proposal = propose_cue_follow(now, grid_from_text(grid_row->old_value.value_or("")), grid_from_text(grid_row->new_value.value_or("")), tolerance_ms);
  {
    EditScope scope("api", ResumeBatch{}, batch_id);
    OptionalTx tx(db_);
    if (!r.proposal.moves.empty()) apply_cue_moves(track_id, now, r.proposal);
    r.mix_links_moved = follow_mix_links(track_id, grid_from_text(grid_row->old_value.value_or("")), grid_from_text(grid_row->new_value.value_or("")), tolerance_ms, true);
    tx.commit();
  }
  r.status = FollowStatus::Ok;
  return r;
}

// Undo of a "grid" or "cues" journal row: only while the track still holds what that edit wrote.
int Library::undo_whole(const TrackEditRecord& e) {
  {
    Stmt s(db_, "SELECT 1 FROM tracks WHERE id = ?1");
    s.bind(1, e.track_id);
    if (!s.step()) return 2;
  }
  if (e.field == "mix_links") return undo_mix_links(e);
  const bool grid = e.field == "grid";
  const std::string current = grid ? grid_text(beatgrid(e.track_id)) : cues_text(cues(e.track_id));
  if (current != e.new_value.value_or("")) return 1;
  if (grid) {
    replace_beatgrid(e.track_id, grid_from_text(e.old_value.value_or("")));
    journal_record(e.track_id, "grid", current, e.old_value.value_or(""));
  } else {
    replace_all_cues(e.track_id, cues_from_text(e.old_value.value_or("")));
  }
  return 0;
}

// ---- selective analysis removal ---------------------------------------------------------------------------------------

StripResult Library::strip_analysis(const std::vector<std::int64_t>& ids, const StripKinds& k, bool force, bool dry_run) {
  StripResult r;
  r.dry_run = dry_run;
  EditScope scope("user");
  OptionalTx tx(db_);
  if (dry_run && !tx.own) throw std::runtime_error("a dry run cannot join an open transaction");
  std::set<std::int64_t> seen;
  const auto exec1 = [&](const char* sql, std::int64_t id) {
    Stmt u(db_, sql);
    u.bind(1, id);
    u.run();
  };
  for (const std::int64_t id : ids) {
    if (!seen.insert(id).second) continue;
    std::optional<double> bpm, gain;
    bool locked = false, has_key = false, has_sound = false, has_wave = false, has_phrases = false;
    {
      Stmt s(db_, "SELECT bpm, gain_db, grid_locked, coalesce(key, '') != '', load_cue_ms IS NOT NULL OR first_sound_ms IS NOT NULL,"
                  " EXISTS(SELECT 1 FROM waveforms w WHERE w.track_id = tracks.id) FROM tracks WHERE id = ?1");
      s.bind(1, id);
      if (!s.step()) continue;
      if (!s.is_null(0) && s.f64(0) > 0) bpm = s.f64(0);
      if (!s.is_null(1)) gain = s.f64(1);
      locked = s.i64(2) != 0;
      has_key = s.i64(3) != 0;
      has_sound = s.i64(4) != 0;
      has_wave = s.i64(5) != 0;
    }
    {
      Stmt s(db_, "SELECT 1 FROM track_phrases WHERE track_id = ?1");
      s.bind(1, id);
      has_phrases = s.step();
    }
    ++r.tracks;
    const std::vector<BeatSection> old_grid = beatgrid(id);
    const std::set<std::string> user_fields = protected_fields(id);
    const FieldValues before = edit_snapshot(id);
    bool changed = false;
    const auto count = [&](const char* kind) {
      ++r.cleared[kind];
      changed = true;
    };
    if (k.grid && (bpm || !old_grid.empty())) {
      if (locked && !force) {
        ++r.skipped_grid_locked;
      } else {
        count("grid");
        if (!dry_run) {
          exec1("DELETE FROM beatgrids WHERE track_id = ?1", id);
          exec1(locked ? "UPDATE tracks SET bpm = NULL, grid_locked = 0 WHERE id = ?1" : "UPDATE tracks SET bpm = NULL WHERE id = ?1", id);
          if (!old_grid.empty()) journal_record(id, "grid", grid_text(old_grid), "");
        }
      }
    }
    if (k.key && has_key) {
      if (user_fields.count("key") && !force) {
        ++r.skipped_user_key;
      } else {
        count("key");
        if (!dry_run) exec1("UPDATE tracks SET key = NULL WHERE id = ?1", id);
      }
    }
    if (k.loudness && gain) {
      count("loudness");
      if (!dry_run) exec1("UPDATE tracks SET gain_db = NULL WHERE id = ?1", id);
    }
    if (k.waveform && has_wave) {
      count("waveform");
      if (!dry_run) exec1("DELETE FROM waveforms WHERE track_id = ?1", id);
    }
    if (k.first_sound && has_sound) {
      count("first_sound");
      if (!dry_run) exec1("UPDATE tracks SET load_cue_ms = NULL, first_sound_ms = NULL, last_sound_ms = NULL WHERE id = ?1", id);
    }
    if (k.phrases && has_phrases) {
      count("phrases");
      if (!dry_run) clear_phrases(id);
    }
    if (k.vocals) {
      Stmt hv(db_, "SELECT 1 FROM track_vocals WHERE track_id = ?1");
      hv.bind(1, id);
      if (hv.step()) {
        count("vocals");
        if (!dry_run) clear_vocals(id);
      }
    }
    if (!dry_run) edit_journal(id, before);
    if (changed) ++r.tracks_changed;
  }
  if (k.stems) r.notes.push_back("stems: not available; nothing cleared");
  r.notes.push_back("undo (edit batch) restores grid, tempo, key and grid lock; loudness, waveform cache, phrases, vocal regions and first-sound / load cue are not undoable, the next analysis re-creates them");
  r.batch_id = scope.batch();
  if (dry_run) tx.rollback();
  else tx.commit();
  return r;
}

// ---- duplicates -------------------------------------------------------------------------------------------------------

namespace {
// SHA-256 (FIPS 180-4), streamed.
struct Sha256 {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  unsigned char buf[64];
  std::size_t fill = 0;
  std::uint64_t total = 0;
  static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const unsigned char* p) {
    static const std::uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) | (std::uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const unsigned char* p, std::size_t n) {
    total += n;
    while (n) {
      const std::size_t take = std::min(n, sizeof buf - fill);
      std::copy(p, p + take, buf + fill);
      fill += take, p += take, n -= take;
      if (fill == 64) block(buf), fill = 0;
    }
  }
  std::string hex() {
    const std::uint64_t bits = total * 8;
    const unsigned char one = 0x80, zero = 0;
    update(&one, 1);
    while (fill != 56) update(&zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    update(len, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
  }
};

// Empty on a read error.
std::string hash_file(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  Sha256 sha;
  std::vector<unsigned char> chunk(1 << 20);
  std::size_t n;
  while ((n = std::fread(chunk.data(), 1, chunk.size(), f)) > 0) sha.update(chunk.data(), n);
  const bool bad = std::ferror(f) != 0;
  std::fclose(f);
  return bad ? std::string() : sha.hex();
}

bool is_lossless(const std::string& format) {
  static const char* const names[] = {"flac", "wav", "wave", "aif", "aiff", "alac", "ape", "wv", "aifc"};
  for (const char* n : names)
    if (format == n) return true;
  return false;
}

// Lower case, punctuation dropped, whitespace collapsed; bytes of multi-byte characters are kept.
std::string normalise_name(const std::string& s) {
  std::string o;
  bool gap = false;
  for (const unsigned char c : s) {
    if (std::isalnum(c) || c >= 0x80) {
      if (gap && !o.empty()) o += ' ';
      gap = false;
      o += c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    } else {
      gap = true;
    }
  }
  return o;
}

std::vector<std::pair<std::int64_t, std::string>> track_paths(Db& db, const std::string& where, int limit) {
  std::vector<std::pair<std::int64_t, std::string>> out;
  Stmt s(db, ("SELECT t.id, r.last_seen_path, t.rel_path FROM tracks t JOIN library_roots r ON r.id = t.root_id WHERE " + where +
              " ORDER BY t.id LIMIT ?1").c_str());
  s.bind(1, limit);
  while (s.step()) out.emplace_back(s.i64(0), join_path(s.text(1), s.text(2)));
  return out;
}

const char* const kUnhashed =
    "t.content_hash IS NULL AND t.file_size > 0 AND t.file_size IN (SELECT file_size FROM tracks WHERE file_size > 0 GROUP BY file_size HAVING count(*) > 1)";
}  // namespace

HashProgress Library::hash_pending(int limit) {
  HashProgress p;
  for (const auto& [id, path] : track_paths(db_, "t.file_size IS NULL", std::max(0, limit))) {  // limit 0: only count
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    Stmt u(db_, "UPDATE tracks SET file_size = ?1 WHERE id = ?2");
    u.bind(1, ec ? std::int64_t(-1) : static_cast<std::int64_t>(size)).bind(2, id);
    u.run();
    if (ec) ++p.missing;
    else ++p.sized;
  }
  // Only files that share a size with another track can be duplicates of it, so only those are read.
  const int budget = std::max(0, limit - p.sized - p.missing);
  for (const auto& [id, path] : track_paths(db_, kUnhashed, budget)) {
    const std::string hash = hash_file(path);  // "" = unreadable: kept so it is not retried on every call
    Stmt u(db_, "UPDATE tracks SET content_hash = ?1 WHERE id = ?2");
    u.bind(1, hash).bind(2, id);
    u.run();
    ++p.hashed;
  }
  {
    Stmt s(db_, (std::string("SELECT (SELECT count(*) FROM tracks WHERE file_size IS NULL), (SELECT count(*) FROM tracks t WHERE ") + kUnhashed + ")").c_str());
    if (s.step()) p.remaining = static_cast<int>(s.i64(0) + s.i64(1));
  }
  return p;
}

std::vector<DuplicateGroup> Library::find_duplicates(const DuplicateOptions& o) {
  std::vector<DuplicateMember> all;
  std::vector<std::string> hashes;
  {
    const std::string sql = std::string("SELECT ") + kTrackColumns +
                            ", t.sample_rate, t.file_size, t.content_hash, (SELECT count(*) FROM cues c WHERE c.track_id = t.id),"
                            " EXISTS(SELECT 1 FROM beatgrids g WHERE g.track_id = t.id) FROM tracks t JOIN library_roots r ON r.id = t.root_id ORDER BY t.id";
    Stmt s(db_, sql.c_str());
    while (s.step()) {
      DuplicateMember m;
      m.row = read_track(s);
      m.sample_rate = s.is_null(19) ? 0 : static_cast<int>(s.i64(19));
      m.file_size = s.is_null(20) ? 0 : std::max<std::int64_t>(0, s.i64(20));
      hashes.push_back(s.is_null(21) ? std::string() : s.text(21));
      m.cue_count = static_cast<int>(s.i64(22));
      m.has_grid = s.i64(23) != 0;
      m.lossless = is_lossless(m.row.format);
      all.push_back(std::move(m));
    }
  }
  std::vector<DuplicateGroup> out;
  std::map<std::string, std::vector<std::size_t>> by_hash;
  std::map<std::size_t, std::string> hash_of;
  for (std::size_t i = 0; i < all.size(); ++i)
    if (!hashes[i].empty()) by_hash[hashes[i]].push_back(i), hash_of[i] = hashes[i];

  const auto finish = [&](DuplicateGroup g) {
    const auto& m = g.members;
    const auto differs = [&](auto get) {
      for (std::size_t i = 1; i < m.size(); ++i)
        if (get(m[i], m[0])) return true;
      return false;
    };
    if (differs([](const DuplicateMember& a, const DuplicateMember& b) { return a.row.format != b.row.format; })) g.differences.push_back("format");
    if (differs([](const DuplicateMember& a, const DuplicateMember& b) { return a.row.bitrate != b.row.bitrate; })) g.differences.push_back("bitrate");
    if (differs([](const DuplicateMember& a, const DuplicateMember& b) { return a.sample_rate != b.sample_rate; })) g.differences.push_back("sample_rate");
    if (differs([](const DuplicateMember& a, const DuplicateMember& b) { return std::llabs(a.row.duration_ms - b.row.duration_ms) > 250; })) g.differences.push_back("duration");
    if (differs([](const DuplicateMember& a, const DuplicateMember& b) { return a.file_size != b.file_size; })) g.differences.push_back("size");
    if (differs([](const DuplicateMember& a, const DuplicateMember& b) { return std::fabs(a.row.bpm - b.row.bpm) > 0.5; })) g.differences.push_back("bpm");
    if (differs([](const DuplicateMember& a, const DuplicateMember& b) { return a.row.key != b.row.key; })) g.differences.push_back("key");
    // Suggestion: lossless, bitrate, sample rate, has cues, has a grid, most played, rating, oldest entry.
    const auto better = [](const DuplicateMember& a, const DuplicateMember& b, const char** why) {
      const auto cmp = [&](auto x, auto y, const char* reason) -> int {
        if (x == y) return 0;
        *why = reason;
        return x > y ? 1 : -1;
      };
      int c;
      if ((c = cmp(a.lossless, b.lossless, "lossless over lossy"))) return c > 0;
      if ((c = cmp(a.row.bitrate, b.row.bitrate, "higher bitrate"))) return c > 0;
      if ((c = cmp(a.sample_rate, b.sample_rate, "higher sample rate"))) return c > 0;
      if ((c = cmp(a.cue_count > 0, b.cue_count > 0, "has cues"))) return c > 0;
      if ((c = cmp(a.has_grid, b.has_grid, "has a beat grid"))) return c > 0;
      if ((c = cmp(a.row.play_count, b.row.play_count, "most played"))) return c > 0;
      if ((c = cmp(a.row.rating, b.row.rating, "higher rating"))) return c > 0;
      *why = "oldest entry";
      return a.row.id < b.row.id;
    };
    std::size_t best = 0;
    const char* why = "oldest entry";
    for (std::size_t i = 1; i < m.size(); ++i) {
      const char* w = "";
      if (better(m[i], m[best], &w)) best = i, why = w;
    }
    // The reason: the highest-priority criterion on which the pick beats any of the others.
    static const char* const order[] = {"lossless over lossy", "higher bitrate", "higher sample rate", "has cues", "has a beat grid",
                                        "most played", "higher rating", "oldest entry"};
    std::size_t rank = 99;
    for (std::size_t i = 0; i < m.size(); ++i) {
      if (i == best) continue;
      const char* w = "";
      better(m[best], m[i], &w);
      for (std::size_t k = 0; k < 8; ++k)
        if (std::string(w) == order[k]) rank = std::min(rank, k);
    }
    why = rank < 8 ? order[rank] : "oldest entry";
    g.suggested_keep = m[best].row.id;
    g.suggestion_reason = why;
    out.push_back(std::move(g));
  };

  if (o.exact) {
    for (auto& [hash, idx] : by_hash) {
      if (idx.size() < 2) continue;
      DuplicateGroup g;
      g.kind = "exact";
      g.confidence = 1.0;
      for (const std::size_t i : idx) g.members.push_back(all[i]);
      finish(std::move(g));
    }
  }
  if (o.likely) {
    struct Cluster { std::vector<std::size_t> idx; };
    std::map<std::string, std::vector<Cluster>> clusters;
    std::vector<std::size_t> order(all.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return all[a].row.duration_ms < all[b].row.duration_ms; });
    for (const std::size_t i : order) {
      const TrackRow& t = all[i].row;
      const std::string title = normalise_name(t.title);
      if (title.empty() || t.duration_ms <= 0) continue;
      auto& list = clusters[normalise_name(t.artist) + "\x1f" + title];
      bool placed = false;
      for (Cluster& c : list) {
        const TrackRow& a = all[c.idx.front()].row;
        if (std::fabs(static_cast<double>(t.duration_ms - a.duration_ms)) > o.duration_tolerance_ms) continue;
        if (o.same_bpm && !(t.bpm > 0 && a.bpm > 0 && std::fabs(t.bpm - a.bpm) <= 0.5)) continue;
        if (o.same_key && (t.key.empty() || t.key != a.key)) continue;
        c.idx.push_back(i);
        placed = true;
        break;
      }
      if (!placed) list.push_back({{i}});
    }
    for (auto& [name, list] : clusters) {
      for (Cluster& c : list) {
        if (c.idx.size() < 2) continue;
        bool one_exact = true;  // every member has the same content hash: reported as an exact group
        for (const std::size_t i : c.idx)
          if (!hash_of.count(i) || hash_of[i] != hash_of[c.idx[0]]) one_exact = false;
        if (one_exact && o.exact) continue;  // already reported as an exact group
        DuplicateGroup g;
        g.kind = "likely";
        double dmin = 1e18, dmax = 0;
        bool bpm_close = true, key_same = true, album_same = true;
        const TrackRow& a = all[c.idx[0]].row;
        for (const std::size_t i : c.idx) {
          const TrackRow& t = all[i].row;
          dmin = std::min<double>(dmin, static_cast<double>(t.duration_ms));
          dmax = std::max<double>(dmax, static_cast<double>(t.duration_ms));
          bpm_close = bpm_close && t.bpm > 0 && a.bpm > 0 && std::fabs(t.bpm - a.bpm) <= 0.5;
          key_same = key_same && !t.key.empty() && t.key == a.key;
          album_same = album_same && !t.album.empty() && normalise_name(t.album) == normalise_name(a.album);
          g.members.push_back(all[i]);
        }
        g.confidence = 0.7 + (dmax - dmin <= 500 ? 0.1 : 0) + (bpm_close ? 0.1 : 0) + (key_same ? 0.05 : 0) + (album_same ? 0.05 : 0);
        g.confidence = std::min(0.95, std::round(g.confidence * 100) / 100);
        finish(std::move(g));
      }
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const DuplicateGroup& a, const DuplicateGroup& b) {
    if (a.confidence != b.confidence) return a.confidence > b.confidence;
    return a.members.size() > b.members.size();
  });
  if (o.limit && out.size() > o.limit) out.resize(o.limit);
  return out;
}

MergeResult Library::merge_duplicates(std::int64_t keep_id, const std::vector<std::int64_t>& remove_in, const MergeOptions& opt) {
  MergeResult r;
  r.dry_run = opt.dry_run;
  r.kept = keep_id;
  const auto exists = [&](std::int64_t id) {
    Stmt s(db_, "SELECT 1 FROM tracks WHERE id = ?1");
    s.bind(1, id);
    return s.step();
  };
  if (!exists(keep_id)) {
    r.error = "no such track to keep";
    return r;
  }
  std::vector<std::int64_t> removed;
  for (const std::int64_t id : remove_in) {
    if (id == keep_id) {
      r.error = "the kept track cannot also be removed";
      return r;
    }
    if (!exists(id)) {
      r.error = "no such track: " + std::to_string(id);
      return r;
    }
    if (std::find(removed.begin(), removed.end(), id) == removed.end()) removed.push_back(id);
  }
  if (removed.empty()) {
    r.error = "nothing to merge";
    return r;
  }
  EditScope scope("user");
  OptionalTx tx(db_);
  if (opt.dry_run && !tx.own) throw std::runtime_error("a dry run cannot join an open transaction");

  // Where the removed tracks sit in playlists (the trash keeps this too, for restore).
  std::map<std::int64_t, int> first_place;  // playlist -> smallest position
  for (const std::int64_t id : removed) {
    Stmt s(db_, "SELECT playlist_id, min(position) FROM playlist_tracks WHERE track_id = ?1 GROUP BY playlist_id");
    s.bind(1, id);
    while (s.step()) {
      const auto at = first_place.find(s.i64(0));
      if (at == first_place.end() || s.i64(1) < at->second) first_place[s.i64(0)] = static_cast<int>(s.i64(1));
    }
  }

  // Tags and analysis values the kept track lacks.
  if (opt.fill_metadata) {
    const FieldValues before = edit_snapshot(keep_id);
    const auto fill = [&](const char* column, bool text) {
      for (const std::int64_t id : removed) {
        const std::string c = column;
        const std::string empty = text ? "(" + c + " IS NULL OR " + c + " = '')" : "(" + c + " IS NULL OR " + c + " = 0)";
        const std::string has = text ? "(SELECT " + c + " FROM tracks WHERE id = ?2) != ''" : "(SELECT " + c + " FROM tracks WHERE id = ?2) != 0";
        Stmt u(db_, ("UPDATE tracks SET " + c + " = (SELECT " + c + " FROM tracks WHERE id = ?2) WHERE id = ?1 AND " + empty +
                     " AND (SELECT " + c + " FROM tracks WHERE id = ?2) IS NOT NULL AND " + has).c_str());
        u.bind(1, keep_id).bind(2, id);
        u.run();
        if (sqlite3_changes(db_.handle()) > 0) {
          r.fields_filled.push_back(c);
          break;
        }
      }
    };
    for (const char* c : {"title", "artist", "album", "genre", "comment", "key"}) fill(c, true);
    for (const char* c : {"rating", "year", "color", "bpm", "gain_db", "main_cue_ms", "load_cue_ms", "first_sound_ms"}) fill(c, false);
    edit_journal(keep_id, before);
  }

  // Cues, loops and the grid.
  if (opt.cues != MergeOptions::Cues::None) {
    const bool all = opt.cues == MergeOptions::Cues::All;
    std::vector<Cue> mine = cues(keep_id);
    const std::vector<Cue> mine_before = mine;
    const auto have = [&](CueKind k) {
      for (const Cue& c : mine_before)
        if (c.slot < 0 && c.kind == k) return true;
      return false;
    };
    const bool had_memory = have(CueKind::Memory), had_loops = have(CueKind::Loop);
    std::int64_t memory_from = 0, loops_from = 0;  // Missing: a whole set comes from one removed track
    for (const std::int64_t id : removed) {
      for (const Cue& c : cues(id)) {
        if (c.slot >= 0) {  // hot cue or hot loop: free slots only
          const bool taken = std::any_of(mine.begin(), mine.end(), [&](const Cue& m) { return m.slot == c.slot; });
          if (taken) continue;
          mine.push_back(c);
          ++r.hot_cues_copied;
          continue;
        }
        const bool loop = c.kind == CueKind::Loop;
        if (!all) {
          if (loop ? had_loops : had_memory) continue;
          std::int64_t& from = loop ? loops_from : memory_from;
          if (from == 0) from = id;
          if (from != id) continue;
        } else if (std::any_of(mine.begin(), mine.end(), [&](const Cue& m) {
                     return m.slot < 0 && m.kind == c.kind && std::fabs(m.start_ms - c.start_ms) < 1 && std::fabs(m.end_ms - c.end_ms) < 1;
                   })) {
          continue;
        }
        mine.push_back(c);
        if (loop) ++r.loops_copied;
        else ++r.memory_cues_copied;
      }
    }
    if (mine.size() != mine_before.size()) {
      std::stable_sort(mine.begin(), mine.end(), [](const Cue& a, const Cue& b) { return a.start_ms < b.start_ms; });
      replace_all_cues(keep_id, mine);
    }
    const std::vector<BeatSection> my_grid = beatgrid(keep_id);
    const bool my_locked = grid_locked(keep_id);
    for (const std::int64_t id : removed) {
      const std::vector<BeatSection> g = beatgrid(id);
      if (g.empty()) continue;
      const bool locked = grid_locked(id);
      if (my_grid.empty() || (all && locked && !my_locked)) {
        const FieldValues before = edit_snapshot(keep_id);
        replace_beatgrid(keep_id, g);
        if (locked) {
          Stmt u(db_, "UPDATE tracks SET grid_locked = 1 WHERE id = ?1");
          u.bind(1, keep_id);
          u.run();
        }
        edit_journal(keep_id, before);
        journal_record(keep_id, "grid", grid_text(my_grid), grid_text(g));
        r.grid_copied = true;
        break;
      }
    }
    if (r.grid_copied) {
      Stmt s(db_, "SELECT bpm FROM tracks WHERE id = ?1");
      s.bind(1, keep_id);
      if (s.step() && (s.is_null(0) || s.f64(0) <= 0)) {
        const FieldValues before = edit_snapshot(keep_id);
        Stmt u(db_, "UPDATE tracks SET bpm = ?1 WHERE id = ?2");
        u.bind(1, beatgrid(keep_id).front().bpm).bind(2, keep_id);
        u.run();
        edit_journal(keep_id, before);
      }
    }
  }

  // Play history moves to the kept track before the removed rows go (their history would otherwise be orphaned).
  if (opt.combine_history) {
    for (const std::int64_t id : removed) {
      Stmt u(db_, "UPDATE history SET track_id = ?1 WHERE track_id = ?2");
      u.bind(1, keep_id).bind(2, id);
      u.run();
      r.history_moved += sqlite3_changes(db_.handle());
    }
  }

  // Linked mix points of the removed tracks now belong to the kept track (a link between two of them would point at itself:
  // dropped; so are links that become exact copies of one the kept track already had).
  for (const std::int64_t id : removed) {
    for (const char* sql : {"UPDATE mix_links SET from_track = ?1 WHERE from_track = ?2", "UPDATE mix_links SET to_track = ?1 WHERE to_track = ?2"}) {
      Stmt u(db_, sql);
      u.bind(1, keep_id).bind(2, id);
      u.run();
      r.mix_links_moved += sqlite3_changes(db_.handle());
    }
  }
  if (r.mix_links_moved > 0) {
    Stmt d1(db_, "DELETE FROM mix_links WHERE from_track = ?1 AND to_track = ?1");
    d1.bind(1, keep_id);
    d1.run();
    Stmt d2(db_, "DELETE FROM mix_links WHERE (from_track = ?1 OR to_track = ?1) AND id NOT IN "
                 "(SELECT min(id) FROM mix_links WHERE from_track = ?1 OR to_track = ?1 GROUP BY from_track, to_track, round(from_ms), round(to_ms))");
    d2.bind(1, keep_id);
    d2.run();
  }

  for (const std::int64_t id : removed) remove_track(id);  // to the trash: restorable, files untouched

  // Playlists: where a removed track was, the kept track now stands, unless that playlist already has it.
  for (const auto& [list, pos] : first_place) {
    Stmt e(db_, "SELECT 1 FROM playlists WHERE id = ?1");
    e.bind(1, list);
    if (!e.step()) continue;
    auto ids = playlist_track_ids(list);
    if (std::find(ids.begin(), ids.end(), keep_id) != ids.end()) {
      ++r.playlists_already_had;
      continue;
    }
    ids.insert(ids.begin() + std::min<std::ptrdiff_t>(pos, static_cast<std::ptrdiff_t>(ids.size())), keep_id);
    set_playlist_tracks(list, ids);
    ++r.playlists_repointed;
  }
  r.removed = removed;
  r.batch_id = scope.batch();
  r.ok = true;
  if (opt.dry_run) tx.rollback();
  else tx.commit();
  return r;
}
