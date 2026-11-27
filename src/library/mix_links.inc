// Linked mix points and vocal regions (schema 10). Included at the end of library.cpp (inside namespace deckhand) so it
// shares that file's helpers (OptionalTx, EditScope, num, escape_field, bind_opt ...).

namespace {
constexpr const char* kLinkCols = "m.id, m.from_track, m.from_ms, m.to_track, m.to_ms, m.transition_beats, m.transition_type, m.label, m.created_at";

MixLink read_link_row(Stmt& s) {
  MixLink l;
  l.id = s.i64(0);
  l.from_track = s.i64(1);
  l.from_ms = s.f64(2);
  l.to_track = s.i64(3);
  l.to_ms = s.f64(4);
  if (!s.is_null(5)) l.transition_beats = s.f64(5);
  if (!s.is_null(6)) l.transition_type = s.text(6);
  l.label = s.text(7);
  l.created_at = s.i64(8);
  return l;
}

bool valid_link_type(const std::string& t) { return t == "crossfade" || t == "cut" || t == "fade" || t == "echo_out"; }

// One line per link: id, from position, to track, to position, beats, type, label, created (tab separated).
std::string links_to_text(const std::vector<MixLink>& links) {
  std::string o;
  for (const MixLink& l : links)
    o += std::to_string(l.id) + "\t" + num(l.from_ms) + "\t" + std::to_string(l.to_track) + "\t" + num(l.to_ms) + "\t" +
         (l.transition_beats ? num(*l.transition_beats) : "") + "\t" + (l.transition_type ? *l.transition_type : "") + "\t" +
         escape_field(l.label) + "\t" + std::to_string(l.created_at) + "\n";
  return o;
}

std::vector<MixLink> links_from_text(std::int64_t from_track, const std::string& text) {
  std::vector<MixLink> out;
  for (const std::string& line : split(text, '\n')) {
    const auto f = split(line, '\t');
    if (f.size() != 8) continue;
    MixLink l;
    l.id = std::atoll(f[0].c_str());
    l.from_track = from_track;
    l.from_ms = std::strtod(f[1].c_str(), nullptr);
    l.to_track = std::atoll(f[2].c_str());
    l.to_ms = std::strtod(f[3].c_str(), nullptr);
    if (!f[4].empty()) l.transition_beats = std::strtod(f[4].c_str(), nullptr);
    if (!f[5].empty()) l.transition_type = f[5];
    l.label = unescape_field(f[6]);
    l.created_at = std::atoll(f[7].c_str());
    out.push_back(std::move(l));
  }
  return out;
}

// Every stored link out of the track, whatever happened to the other end (the journal records the whole set).
std::vector<MixLink> raw_links_from(Db& db, std::int64_t from_track) {
  std::vector<MixLink> out;
  Stmt s(db, (std::string("SELECT ") + kLinkCols + " FROM mix_links m WHERE m.from_track = ?1 ORDER BY m.from_ms, m.id").c_str());
  s.bind(1, from_track);
  while (s.step()) out.push_back(read_link_row(s));
  return out;
}

bool track_live(Db& db, std::int64_t id) {
  Stmt s(db, "SELECT 1 FROM tracks WHERE id = ?1");
  s.bind(1, id);
  return s.step();
}

void check_link_values(const MixLink& link) {
  if (!(link.from_ms >= 0) || !(link.to_ms >= 0)) throw std::invalid_argument("mix link positions must be 0 or later");
  if (link.transition_beats && !(*link.transition_beats >= 1 && *link.transition_beats <= 64))
    throw std::invalid_argument("transition length is 1 to 64 beats");
  if (link.transition_type && !valid_link_type(*link.transition_type)) throw std::invalid_argument("unknown transition type");
}
}  // namespace

// `where` uses ?1 (and ?2 when `two` is set).
std::vector<MixLink> Library::read_links(const char* where, std::int64_t a, std::int64_t b) {
  std::vector<MixLink> out;
  const std::string w = where;
  const std::string sql = std::string("SELECT ") + kLinkCols +
                          " FROM mix_links m WHERE EXISTS(SELECT 1 FROM tracks t WHERE t.id = m.from_track)"
                          " AND EXISTS(SELECT 1 FROM tracks t WHERE t.id = m.to_track) AND " + w +
                          " ORDER BY m.from_track, m.from_ms, m.id";
  Stmt s(db_, sql.c_str());
  s.bind(1, a);
  if (w.find("?2") != std::string::npos) s.bind(2, b);
  while (s.step()) out.push_back(read_link_row(s));
  return out;
}

void Library::write_link_set(std::int64_t from_track, const std::vector<MixLink>& links) {
  {
    Stmt d(db_, "DELETE FROM mix_links WHERE from_track = ?1");
    d.bind(1, from_track);
    d.run();
  }
  for (const MixLink& l : links) {
    Stmt i(db_, "INSERT INTO mix_links(id, from_track, from_ms, to_track, to_ms, transition_beats, transition_type, label, created_at)"
                " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
    if (l.id > 0) i.bind(1, l.id);
    else i.bind_null(1);
    i.bind(2, from_track).bind(3, l.from_ms).bind(4, l.to_track).bind(5, l.to_ms);
    bind_opt(i, 6, l.transition_beats);
    bind_opt(i, 7, l.transition_type);
    i.bind(8, l.label).bind(9, l.created_at);
    i.run();
  }
}

std::int64_t Library::add_mix_link(const MixLink& link) {
  if (link.from_track == link.to_track) throw std::invalid_argument("a mix link needs two different tracks");
  check_link_values(link);
  EditScope scope("user");
  OptionalTx tx(db_);
  if (!track_live(db_, link.from_track) || !track_live(db_, link.to_track)) throw std::invalid_argument("no such track");
  const std::vector<MixLink> existing = raw_links_from(db_, link.from_track);
  const std::string before = links_to_text(existing);
  std::int64_t id = 0;
  for (const MixLink& e : existing)
    if (e.to_track == link.to_track && std::fabs(e.from_ms - link.from_ms) < 1.0 && std::fabs(e.to_ms - link.to_ms) < 1.0) id = e.id;
  if (id > 0) {
    Stmt u(db_, "UPDATE mix_links SET from_ms = ?1, to_ms = ?2, transition_beats = ?3, transition_type = ?4, label = ?5 WHERE id = ?6");
    u.bind(1, link.from_ms).bind(2, link.to_ms);
    bind_opt(u, 3, link.transition_beats);
    bind_opt(u, 4, link.transition_type);
    u.bind(5, link.label).bind(6, id);
    u.run();
  } else {
    Stmt i(db_, "INSERT INTO mix_links(from_track, from_ms, to_track, to_ms, transition_beats, transition_type, label, created_at)"
                " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    i.bind(1, link.from_track).bind(2, link.from_ms).bind(3, link.to_track).bind(4, link.to_ms);
    bind_opt(i, 5, link.transition_beats);
    bind_opt(i, 6, link.transition_type);
    i.bind(7, link.label).bind(8, unix_now());
    i.run();
    id = db_.last_insert_id();
  }
  const std::string after = links_to_text(raw_links_from(db_, link.from_track));
  if (before != after) journal_record(link.from_track, "mix_links", before, after);
  tx.commit();
  return id;
}

std::optional<MixLink> Library::mix_link(std::int64_t id) {
  Stmt s(db_, (std::string("SELECT ") + kLinkCols + " FROM mix_links m WHERE m.id = ?1").c_str());
  s.bind(1, id);
  if (!s.step()) return std::nullopt;
  return read_link_row(s);
}

bool Library::update_mix_link(const MixLink& link) {
  const auto cur = mix_link(link.id);
  if (!cur) return false;
  check_link_values(link);
  EditScope scope("user");
  OptionalTx tx(db_);
  const std::string before = links_to_text(raw_links_from(db_, cur->from_track));
  Stmt u(db_, "UPDATE mix_links SET from_ms = ?1, to_ms = ?2, transition_beats = ?3, transition_type = ?4, label = ?5 WHERE id = ?6");
  u.bind(1, link.from_ms).bind(2, link.to_ms);
  bind_opt(u, 3, link.transition_beats);
  bind_opt(u, 4, link.transition_type);
  u.bind(5, link.label).bind(6, link.id);
  u.run();
  const std::string after = links_to_text(raw_links_from(db_, cur->from_track));
  if (before != after) journal_record(cur->from_track, "mix_links", before, after);
  tx.commit();
  return true;
}

bool Library::remove_mix_link(std::int64_t id) {
  const auto cur = mix_link(id);
  if (!cur) return false;
  EditScope scope("user");
  OptionalTx tx(db_);
  const std::string before = links_to_text(raw_links_from(db_, cur->from_track));
  Stmt d(db_, "DELETE FROM mix_links WHERE id = ?1");
  d.bind(1, id);
  d.run();
  journal_record(cur->from_track, "mix_links", before, links_to_text(raw_links_from(db_, cur->from_track)));
  tx.commit();
  return true;
}

int Library::remove_mix_links(std::int64_t track_id) {
  EditScope scope("user");
  OptionalTx tx(db_);
  std::set<std::int64_t> owners;
  int n = 0;
  {
    Stmt s(db_, "SELECT DISTINCT from_track FROM mix_links WHERE from_track = ?1 OR to_track = ?1");
    s.bind(1, track_id);
    while (s.step()) owners.insert(s.i64(0));
  }
  for (const std::int64_t owner : owners) {
    const std::string before = links_to_text(raw_links_from(db_, owner));
    Stmt d(db_, "DELETE FROM mix_links WHERE from_track = ?1 AND (from_track = ?2 OR to_track = ?2)");
    d.bind(1, owner).bind(2, track_id);
    d.run();
    n += sqlite3_changes(db_.handle());
    const std::string after = links_to_text(raw_links_from(db_, owner));
    if (before != after && track_live(db_, owner)) journal_record(owner, "mix_links", before, after);
  }
  tx.commit();
  return n;
}

std::vector<MixLink> Library::mix_links_from(std::int64_t track_id) { return read_links("m.from_track = ?1", track_id, 0); }
std::vector<MixLink> Library::mix_links_to(std::int64_t track_id) { return read_links("m.to_track = ?1", track_id, 0); }
std::vector<MixLink> Library::mix_links_between(std::int64_t from_track, std::int64_t to_track) {
  return read_links("m.from_track = ?1 AND m.to_track = ?2", from_track, to_track);
}

std::optional<MixLink> Library::mix_link_for_transition(std::int64_t from_track, std::int64_t to_track, double position_ms, double slack_ms) {
  const std::vector<MixLink> all = mix_links_between(from_track, to_track);  // by position
  if (all.empty()) return std::nullopt;
  for (const MixLink& l : all)
    if (l.from_ms >= position_ms - slack_ms) return l;
  return all.back();
}

std::map<std::int64_t, int> Library::mix_link_targets(std::int64_t from_track) {
  std::map<std::int64_t, int> out;
  for (const MixLink& l : mix_links_from(from_track)) ++out[l.to_track];
  return out;
}

// Link ends that sit on `track_id`'s grid follow it (from positions of links out of it, to positions of links into it).
int Library::follow_mix_links(std::int64_t track_id, const std::vector<BeatSection>& old_grid, const std::vector<BeatSection>& new_grid,
                              double tolerance_ms, bool apply) {
  struct End {
    MixLink link;
    bool is_from = false;
  };
  std::vector<End> ends;
  std::vector<Cue> pseudo;
  for (const MixLink& l : mix_links_from(track_id)) ends.push_back({l, true});
  for (const MixLink& l : mix_links_to(track_id)) ends.push_back({l, false});
  if (ends.empty()) return 0;
  for (std::size_t i = 0; i < ends.size(); ++i) {
    Cue c;
    c.kind = CueKind::Memory;
    c.slot = static_cast<int>(i);
    c.start_ms = ends[i].is_from ? ends[i].link.from_ms : ends[i].link.to_ms;
    pseudo.push_back(c);
  }
  const CueFollowProposal p = propose_cue_follow(pseudo, old_grid, new_grid, tolerance_ms);
  if (!apply) return static_cast<int>(p.moves.size());
  std::set<std::int64_t> owners;
  std::map<std::int64_t, std::string> before;
  for (const CueMove& m : p.moves) {
    const std::int64_t owner = ends[static_cast<std::size_t>(m.before.slot)].link.from_track;
    if (owners.insert(owner).second) before[owner] = links_to_text(raw_links_from(db_, owner));
  }
  for (const CueMove& m : p.moves) {
    const End& e = ends[static_cast<std::size_t>(m.before.slot)];
    Stmt u(db_, e.is_from ? "UPDATE mix_links SET from_ms = ?1 WHERE id = ?2" : "UPDATE mix_links SET to_ms = ?1 WHERE id = ?2");
    u.bind(1, m.after.start_ms).bind(2, e.link.id);
    u.run();
  }
  for (const std::int64_t owner : owners) {
    const std::string after = links_to_text(raw_links_from(db_, owner));
    if (after != before[owner]) journal_record(owner, "mix_links", before[owner], after);
  }
  return static_cast<int>(p.moves.size());
}

// ---- vocal regions ----------------------------------------------------------------------------------------------------

std::optional<StoredVocals> Library::vocals(std::int64_t track_id) {
  StoredVocals v;
  {
    Stmt s(db_, "SELECT version, source FROM track_vocals WHERE track_id = ?1");
    s.bind(1, track_id);
    if (!s.step()) return std::nullopt;
    v.version = static_cast<int>(s.i64(0));
    v.source = s.text(1);
  }
  Stmt r(db_, "SELECT start_ms, end_ms FROM vocal_regions WHERE track_id = ?1 ORDER BY idx");
  r.bind(1, track_id);
  while (r.step()) v.regions.push_back({r.f64(0), r.f64(1)});
  return v;
}

void Library::clear_vocals(std::int64_t track_id) {
  OptionalTx tx(db_);
  for (const char* sql : {"DELETE FROM track_vocals WHERE track_id = ?1", "DELETE FROM vocal_regions WHERE track_id = ?1"}) {
    Stmt d(db_, sql);
    d.bind(1, track_id);
    d.run();
  }
  tx.commit();
}

void Library::set_vocals(std::int64_t track_id, const StoredVocals& v) {
  std::vector<StoredVocalRegion> regions;
  for (const StoredVocalRegion& r : v.regions)
    if (r.start_ms >= 0 && r.end_ms > r.start_ms) regions.push_back(r);
  std::sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) { return a.start_ms < b.start_ms; });
  std::vector<StoredVocalRegion> merged;
  for (const StoredVocalRegion& r : regions) {
    if (!merged.empty() && r.start_ms <= merged.back().end_ms) merged.back().end_ms = std::max(merged.back().end_ms, r.end_ms);
    else merged.push_back(r);
  }
  OptionalTx tx(db_);
  clear_vocals(track_id);
  {
    Stmt u(db_, "INSERT INTO track_vocals(track_id, version, source, analysed_at) VALUES (?1, ?2, ?3, ?4)");
    u.bind(1, track_id).bind(2, v.version).bind(3, v.source).bind(4, unix_now());
    u.run();
  }
  int idx = 0;
  for (const StoredVocalRegion& r : merged) {
    Stmt i(db_, "INSERT INTO vocal_regions(track_id, idx, start_ms, end_ms) VALUES (?1, ?2, ?3, ?4)");
    i.bind(1, track_id).bind(2, idx++).bind(3, r.start_ms).bind(4, r.end_ms);
    i.run();
  }
  tx.commit();
}

std::vector<std::int64_t> Library::tracks_with_vocals() {
  std::vector<std::int64_t> ids;
  Stmt s(db_, "SELECT track_id FROM track_vocals ORDER BY track_id");
  while (s.step()) ids.push_back(s.i64(0));
  return ids;
}

// Undo of a "mix_links" journal row: only while the track still holds the set that edit wrote.
int Library::undo_mix_links(const TrackEditRecord& e) {
  const std::string now = links_to_text(raw_links_from(db_, e.track_id));
  if (now != e.new_value.value_or("")) return 1;
  OptionalTx tx(db_);
  write_link_set(e.track_id, links_from_text(e.track_id, e.old_value.value_or("")));
  journal_record(e.track_id, "mix_links", now, e.old_value.value_or(""));
  tx.commit();
  return 0;
}
