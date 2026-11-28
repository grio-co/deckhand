// Playlist trash: a deleted playlist, folder (with everything inside) or smart playlist is kept in the trash_playlist_*
// tables until purged. Included at the end of library.cpp.
//
//   trash_playlist_entries  one row per deletion (the root: kind, name, old id and parent, rank among its siblings, rules)
//   trash_playlist_nodes    every playlist / folder of the deleted subtree (the root included) with its own place
//   trash_playlist_items    the tracks of each playlist in order
//   trash_playlist_import   import_map rows of the subtree
// The shared-playlist link (setting "shared.link.<id>", written by the sync library) is kept in the node row.

namespace {

std::string shared_link_key(std::int64_t playlist_id) { return "shared.link." + std::to_string(playlist_id); }

// "Name", then "Name (restored)", "Name (restored 2)" ... the first one not used by a sibling (case-insensitive).
std::string unclashed_name(Db& db, bool smart, std::int64_t parent, const std::string& name) {
  const auto used = [&](const std::string& n) {
    Stmt s(db, smart ? "SELECT 1 FROM smart_playlists WHERE name = ?1 COLLATE NOCASE"
                     : "SELECT 1 FROM playlists WHERE coalesce(parent_id, 0) = ?2 AND name = ?1 COLLATE NOCASE");
    s.bind(1, n);
    if (!smart) s.bind(2, parent);
    return s.step();
  };
  if (!used(name)) return name;
  // A name that already carries our suffix ("Mix (restored 2)") is counted on from its base, not suffixed again.
  std::string base = name;
  const std::size_t open = base.rfind(" (restored");
  if (open != std::string::npos && base.back() == ')') {
    const std::string mid = base.substr(open + 10, base.size() - open - 11);  // "" or " 2"
    if (mid.empty() || (mid[0] == ' ' && mid.size() > 1 && mid.find_first_not_of("0123456789", 1) == std::string::npos)) base.resize(open);
  }
  std::string candidate = base + " (restored)";
  for (int n = 2; used(candidate); ++n) candidate = base + " (restored " + std::to_string(n) + ")";
  return candidate;
}

bool row_exists(Db& db, const char* sql, std::int64_t id) {
  Stmt s(db, sql);
  s.bind(1, id);
  return s.step();
}

// An "in playlist" rule whose playlist is in the trash keeps a marker instead of the id: -(entry << 32 | old id). The id may
// be handed to another playlist while it is away; the marker cannot, and restore rewrites it to whatever id the
// playlist got back.
std::int64_t trash_marker(std::int64_t entry, std::int64_t old_id) { return -((entry << 32) | (old_id & 0xffffffffLL)); }

}  // namespace

void Library::rewrite_playlist_refs(const std::function<std::optional<std::int64_t>(std::int64_t)>& map) {
  const auto rewrite = [&](const std::string& json) -> std::optional<std::string> {
    std::vector<SmartRule> rules;
    if (!smart_rules_from_json(json, rules)) return std::nullopt;
    bool changed = false;
    for (SmartRule& r : rules) {
      if (r.field != SmartRule::Field::InPlaylist) continue;
      if (const auto to = map(r.id); to && *to != r.id) {
        r.id = *to;
        changed = true;
      }
    }
    if (!changed) return std::nullopt;
    return smart_rules_json(rules);
  };
  for (const char* table : {"smart_playlists", "trash_playlist_entries"}) {
    std::vector<std::pair<std::int64_t, std::string>> rows;
    {
      Stmt s(db_, (std::string("SELECT id, rules FROM ") + table + (table[0] == 's' ? "" : " WHERE kind = 'smart'")).c_str());
      while (s.step()) rows.emplace_back(s.i64(0), s.text(1));
    }
    for (const auto& [id, json] : rows) {
      const auto out = rewrite(json);
      if (!out) continue;
      Stmt u(db_, (std::string("UPDATE ") + table + " SET rules = ?1 WHERE id = ?2").c_str());
      u.bind(1, *out).bind(2, id);
      u.run();
    }
  }
}

std::int64_t Library::trash_playlist_node(std::int64_t id, bool smart, std::string* kind) {
  OptionalTx tx(db_);
  const std::int64_t now = unix_now();
  std::int64_t entry = 0;
  if (smart) {
    std::string name, rules;
    std::int64_t match_all = 1, rank = 0;
    {
      Stmt s(db_, "SELECT name, match_all, rules FROM smart_playlists WHERE id = ?1");
      s.bind(1, id);
      if (!s.step()) return 0;
      name = s.text(0);
      match_all = s.i64(1);
      rules = s.text(2);
    }
    {
      Stmt s(db_, "SELECT id FROM smart_playlists ORDER BY position, name COLLATE NOCASE, id");
      for (; s.step(); ++rank)
        if (s.i64(0) == id) break;
    }
    Stmt i(db_, "INSERT INTO trash_playlist_entries(kind, name, old_id, old_parent_id, position, deleted_at, match_all, rules)"
                " VALUES ('smart', ?1, ?2, 0, ?3, ?4, ?5, ?6)");
    i.bind(1, name).bind(2, id).bind(3, rank).bind(4, now).bind(5, match_all).bind(6, rules);
    i.run();
    entry = db_.last_insert_id();
    Stmt d(db_, "DELETE FROM smart_playlists WHERE id = ?1");
    d.bind(1, id);
    d.run();
    if (kind) *kind = "smart";
    tx.commit();
    return entry;
  }

  std::string name;
  std::int64_t parent = 0, rank = 0;
  bool folder = false;
  {
    Stmt s(db_, "SELECT name, coalesce(parent_id, 0), is_folder FROM playlists WHERE id = ?1");
    s.bind(1, id);
    if (!s.step()) return 0;
    name = s.text(0);
    parent = s.i64(1);
    folder = s.i64(2) != 0;
  }
  {
    Stmt s(db_, "SELECT id FROM playlists WHERE coalesce(parent_id, 0) = ?1 ORDER BY position, name");
    s.bind(1, parent);
    for (; s.step(); ++rank)
      if (s.i64(0) == id) break;
  }
  const char* sub = "WITH RECURSIVE sub(id) AS (SELECT ?1 UNION ALL SELECT p.id FROM playlists p JOIN sub ON p.parent_id = sub.id) ";
  {
    Stmt i(db_, "INSERT INTO trash_playlist_entries(kind, name, old_id, old_parent_id, position, deleted_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    i.bind(1, folder ? "folder" : "playlist").bind(2, name).bind(3, id).bind(4, parent).bind(5, rank).bind(6, now);
    i.run();
    entry = db_.last_insert_id();
  }
  const auto run = [&](const std::string& sql) {
    Stmt c(db_, (std::string(sub) + sql).c_str());
    c.bind(1, id).bind(2, entry);
    c.run();
  };
  run("INSERT INTO trash_playlist_nodes(entry_id, old_id, old_parent_id, name, is_folder, position)"
      " SELECT ?2, p.id, coalesce(p.parent_id, 0), p.name, p.is_folder, p.position FROM playlists p WHERE p.id IN (SELECT id FROM sub)");
  run("INSERT INTO trash_playlist_items(entry_id, playlist_old_id, position, track_id)"
      " SELECT ?2, t.playlist_id, t.position, t.track_id FROM playlist_tracks t WHERE t.playlist_id IN (SELECT id FROM sub)");
  run("INSERT INTO trash_playlist_import(entry_id, old_id, source, source_id)"
      " SELECT ?2, local_id, source, source_id FROM import_map WHERE entity = 'playlist' AND local_id IN (SELECT id FROM sub)");
  // The shared-playlist link goes with the playlist, so a reused id does not inherit it.
  std::vector<std::int64_t> nodes;
  {
    Stmt s(db_, "SELECT old_id FROM trash_playlist_nodes WHERE entry_id = ?1");
    s.bind(1, entry);
    while (s.step()) nodes.push_back(s.i64(0));
  }
  for (const auto node : nodes) {
    const std::string link = setting(shared_link_key(node));
    if (link.empty()) continue;
    Stmt u(db_, "UPDATE trash_playlist_nodes SET shared_link = ?1 WHERE entry_id = ?2 AND old_id = ?3");
    u.bind(1, link).bind(2, entry).bind(3, node);
    u.run();
    set_setting(shared_link_key(node), "");
  }
  {
    Stmt c(db_, (std::string(sub) + "DELETE FROM import_map WHERE entity = 'playlist' AND local_id IN (SELECT id FROM sub)").c_str());
    c.bind(1, id);
    c.run();
  }
  // Rules that name the playlist (or anything inside a folder) now name the trash entry instead of a reusable id.
  rewrite_playlist_refs([&](std::int64_t ref) -> std::optional<std::int64_t> {
    if (std::find(nodes.begin(), nodes.end(), ref) == nodes.end()) return std::nullopt;
    return trash_marker(entry, ref);
  });
  Stmt d(db_, "DELETE FROM playlists WHERE id = ?1");  // children and entries cascade
  d.bind(1, id);
  d.run();
  if (kind) *kind = folder ? "folder" : "playlist";
  tx.commit();
  return entry;
}

std::int64_t Library::restore_playlist_entry(std::int64_t entry, std::string* kind) {
  OptionalTx tx(db_);
  std::string k, name, rules;
  std::int64_t old_id = 0, old_parent = 0, rank = 0, match_all = 1;
  {
    Stmt s(db_, "SELECT kind, name, old_id, old_parent_id, position, match_all, rules FROM trash_playlist_entries WHERE id = ?1");
    s.bind(1, entry);
    if (!s.step()) return 0;
    k = s.text(0);
    name = s.text(1);
    old_id = s.i64(2);
    old_parent = s.i64(3);
    rank = s.i64(4);
    match_all = s.i64(5);
    rules = s.text(6);
  }
  if (kind) *kind = k;
  const auto drop = [&] {
    for (const char* t : {"trash_playlist_nodes", "trash_playlist_items", "trash_playlist_import"}) {
      Stmt d(db_, (std::string("DELETE FROM ") + t + " WHERE entry_id = ?1").c_str());
      d.bind(1, entry);
      d.run();
    }
    Stmt d(db_, "DELETE FROM trash_playlist_entries WHERE id = ?1");
    d.bind(1, entry);
    d.run();
  };
  // Inserts `id` (placeholder position) and then puts it at rank `rank` among its siblings, numbering them 0..n-1.
  const auto place = [&](std::int64_t id, bool smart, std::int64_t parent) {
    std::vector<std::int64_t> sibs;
    {
      Stmt s(db_, smart ? "SELECT id FROM smart_playlists WHERE id != ?2 ORDER BY position, name COLLATE NOCASE, id"
                        : "SELECT id FROM playlists WHERE coalesce(parent_id, 0) = ?1 AND id != ?2 ORDER BY position, name");
      if (!smart) s.bind(1, parent);
      s.bind(2, id);
      while (s.step()) sibs.push_back(s.i64(0));
    }
    const auto at = static_cast<std::size_t>(std::clamp<std::int64_t>(rank, 0, static_cast<std::int64_t>(sibs.size())));
    sibs.insert(sibs.begin() + static_cast<std::ptrdiff_t>(at), id);
    Stmt u(db_, smart ? "UPDATE smart_playlists SET position = ?1 WHERE id = ?2" : "UPDATE playlists SET position = ?1 WHERE id = ?2");
    int pos = 0;
    for (const auto sib : sibs) {
      u.bind(1, pos++).bind(2, sib);
      u.run();
    }
  };

  if (k == "smart") {
    const bool free_id = !row_exists(db_, "SELECT 1 FROM smart_playlists WHERE id = ?1", old_id);
    Stmt i(db_, "INSERT INTO smart_playlists(id, name, match_all, rules, position) VALUES (?1, ?2, ?3, ?4, 1000000)");
    if (free_id) i.bind(1, old_id);
    else i.bind_null(1);
    i.bind(2, unclashed_name(db_, true, 0, name)).bind(3, match_all).bind(4, rules);
    i.run();
    const std::int64_t id = free_id ? old_id : db_.last_insert_id();
    place(id, true, 0);
    drop();
    tx.commit();
    return id;
  }

  struct Node {
    std::int64_t old_id, old_parent, position;
    std::string name, link;
    bool folder;
  };
  std::vector<Node> nodes;
  {
    Stmt s(db_, "SELECT old_id, old_parent_id, position, name, shared_link, is_folder FROM trash_playlist_nodes WHERE entry_id = ?1");
    s.bind(1, entry);
    while (s.step()) nodes.push_back({s.i64(0), s.i64(1), s.i64(2), s.text(3), s.text(4), s.i64(5) != 0});
  }
  std::unordered_map<std::int64_t, std::vector<std::size_t>> kids;  // old parent -> node indexes
  std::size_t root = nodes.size();
  for (std::size_t n = 0; n < nodes.size(); ++n) {
    if (nodes[n].old_id == old_id) root = n;
    else kids[nodes[n].old_parent].push_back(n);
  }
  if (root == nodes.size()) {  // damaged entry: nothing to restore
    drop();
    tx.commit();
    return 0;
  }
  // Where it goes: the old folder when it is still there, else the top level.
  std::int64_t parent = 0;
  if (old_parent != 0) {
    Stmt s(db_, "SELECT 1 FROM playlists WHERE id = ?1 AND is_folder = 1");
    s.bind(1, old_parent);
    if (s.step()) parent = old_parent;
  }
  std::unordered_map<std::int64_t, std::int64_t> remap;  // old id -> new id
  std::vector<std::pair<std::size_t, std::int64_t>> stack{{root, parent}};
  while (!stack.empty()) {
    const auto [n, into] = stack.back();
    stack.pop_back();
    const Node& node = nodes[n];
    const bool free_id = !row_exists(db_, "SELECT 1 FROM playlists WHERE id = ?1", node.old_id);
    Stmt i(db_, "INSERT INTO playlists(id, parent_id, name, is_folder, position) VALUES (?1, ?2, ?3, ?4, ?5)");
    if (free_id) i.bind(1, node.old_id);
    else i.bind_null(1);
    if (into) i.bind(2, into);
    else i.bind_null(2);
    i.bind(3, n == root ? unclashed_name(db_, false, into, node.name) : node.name).bind(4, node.folder ? 1 : 0)
        .bind(5, n == root ? std::int64_t{1000000} : node.position);
    i.run();
    const std::int64_t id = free_id ? node.old_id : db_.last_insert_id();
    remap[node.old_id] = id;
    {
      // Tracks still in the library, in their old order.
      Stmt t(db_, "INSERT INTO playlist_tracks(playlist_id, position, track_id)"
                  " SELECT ?1, i.position, i.track_id FROM trash_playlist_items i"
                  " WHERE i.entry_id = ?2 AND i.playlist_old_id = ?3 AND EXISTS (SELECT 1 FROM tracks WHERE id = i.track_id)"
                  " ORDER BY i.position");
      t.bind(1, id).bind(2, entry).bind(3, node.old_id);
      t.run();
      std::int64_t kept = 0, expected = 0;
      Stmt c(db_, "SELECT count(*), (SELECT count(*) FROM trash_playlist_items WHERE entry_id = ?2 AND playlist_old_id = ?3)"
                  " FROM playlist_tracks WHERE playlist_id = ?1");
      c.bind(1, id).bind(2, entry).bind(3, node.old_id);
      if (c.step()) {
        kept = c.i64(0);
        expected = c.i64(1);
      }
      if (kept != expected) set_playlist_tracks(id, playlist_track_ids(id));  // close the gaps left by tracks that are gone
    }
    if (!node.link.empty() && setting(shared_link_key(id)).empty()) set_setting(shared_link_key(id), node.link);
    {
      Stmt m(db_, "INSERT OR IGNORE INTO import_map(source, entity, source_id, local_id)"
                  " SELECT source, 'playlist', source_id, ?1 FROM trash_playlist_import WHERE entry_id = ?2 AND old_id = ?3");
      m.bind(1, id).bind(2, entry).bind(3, node.old_id);
      m.run();
    }
    const auto ch = kids.find(node.old_id);
    if (ch != kids.end())
      for (const auto c : ch->second) stack.push_back({c, id});
  }
  place(remap[old_id], false, parent);
  rewrite_playlist_refs([&](std::int64_t ref) -> std::optional<std::int64_t> {
    if (ref >= 0 || ((-ref) >> 32) != entry) return std::nullopt;
    const auto it = remap.find((-ref) & 0xffffffffLL);
    if (it == remap.end()) return std::nullopt;
    return it->second;
  });
  drop();
  tx.commit();
  return remap[old_id];
}

std::vector<DeletedPlaylist> Library::deleted_playlists() {
  std::vector<DeletedPlaylist> out;
  Stmt s(db_, "SELECT e.id, e.kind, e.name, e.old_id, e.old_parent_id, e.deleted_at,"
              " (SELECT count(*) FROM trash_playlist_nodes n WHERE n.entry_id = e.id AND n.is_folder = 0),"
              " (SELECT count(*) FROM trash_playlist_nodes n WHERE n.entry_id = e.id AND n.is_folder = 1),"
              " (SELECT count(*) FROM trash_playlist_items i WHERE i.entry_id = e.id),"
              " (SELECT count(*) FROM trash_playlist_nodes n WHERE n.entry_id = e.id AND n.shared_link != '')"
              " FROM trash_playlist_entries e ORDER BY e.deleted_at DESC, e.id DESC");
  while (s.step()) {
    DeletedPlaylist d;
    d.id = s.i64(0);
    d.kind = s.text(1);
    d.name = s.text(2);
    d.old_id = s.i64(3);
    d.parent_id = s.i64(4);
    d.deleted_at = s.i64(5);
    d.playlists = d.kind == "smart" ? 1 : static_cast<int>(s.i64(6));
    d.folders = static_cast<int>(s.i64(7));
    d.tracks = static_cast<int>(s.i64(8));
    d.shared = s.i64(9) > 0;
    out.push_back(std::move(d));
  }
  return out;
}

bool Library::restore_playlist(std::int64_t trash_id, std::int64_t* restored) {
  EditScope scope("user");
  OptionalTx tx(db_);
  std::string kind;
  const std::int64_t id = restore_playlist_entry(trash_id, &kind);
  if (!id) return false;
  journal_record(0, "playlist_trash", "trash:" + std::to_string(trash_id), "live:" + std::to_string(id) + ":" + kind);
  tx.commit();
  if (restored) *restored = id;
  return true;
}

bool Library::purge_deleted_playlist(std::int64_t trash_id) {
  OptionalTx tx(db_);
  if (!row_exists(db_, "SELECT 1 FROM trash_playlist_entries WHERE id = ?1", trash_id)) return false;
  for (const char* t : {"trash_playlist_nodes", "trash_playlist_items", "trash_playlist_import"}) {
    Stmt d(db_, (std::string("DELETE FROM ") + t + " WHERE entry_id = ?1").c_str());
    d.bind(1, trash_id);
    d.run();
  }
  Stmt d(db_, "DELETE FROM trash_playlist_entries WHERE id = ?1");
  d.bind(1, trash_id);
  d.run();
  tx.commit();
  return true;
}

int Library::purge_deleted_playlists(int older_than_days) {
  const std::int64_t cutoff = unix_now() - static_cast<std::int64_t>(std::max(0, older_than_days)) * 86400;
  std::vector<std::int64_t> ids;
  {
    Stmt s(db_, "SELECT id FROM trash_playlist_entries WHERE deleted_at <= ?1");
    s.bind(1, cutoff);
    while (s.step()) ids.push_back(s.i64(0));
  }
  if (ids.empty()) return 0;
  OptionalTx tx(db_);
  for (const auto id : ids) purge_deleted_playlist(id);
  tx.commit();
  return static_cast<int>(ids.size());
}

// Undo / redo of a delete or restore: "live:<id>:<kind>" is a playlist that exists, "trash:<entry>" one in the trash.
int Library::undo_playlist_trash(const TrackEditRecord& e) {
  if (!e.old_value || !e.new_value) return 1;
  const std::string& now = *e.new_value;
  if (now.rfind("trash:", 0) == 0) {  // it was deleted: bring it back
    const std::int64_t entry = std::atoll(now.c_str() + 6);
    std::string kind;
    const std::int64_t id = restore_playlist_entry(entry, &kind);
    if (!id) return 2;
    journal_record(0, "playlist_trash", now, "live:" + std::to_string(id) + ":" + kind);
    return 0;
  }
  if (now.rfind("live:", 0) == 0) {  // it was restored: delete it again
    const std::int64_t id = std::atoll(now.c_str() + 5);
    const bool smart = now.size() > 6 && now.compare(now.size() - 6, 6, ":smart") == 0;
    std::string kind;
    const std::int64_t entry = trash_playlist_node(id, smart, &kind);
    if (!entry) return 2;
    journal_record(0, "playlist_trash", now, "trash:" + std::to_string(entry));
    return 0;
  }
  return 1;
}
