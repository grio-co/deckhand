// Relocating moved files and browsing by directory. Included at the end of library.cpp (shares its helpers).
namespace {

std::string slashes(std::string p) {
  for (char& c : p)
    if (c == '\\') c = '/';
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

std::string lower_ascii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string file_name_of(const std::string& path) {
  const std::string p = slashes(path);
  const auto at = p.find_last_of('/');
  return at == std::string::npos ? p : p.substr(at + 1);
}

std::string dir_of(const std::string& path) {
  const std::string p = slashes(path);
  const auto at = p.find_last_of('/');
  return at == std::string::npos ? std::string() : p.substr(0, at);
}

bool audio_extension(const std::string& name) {
  static const char* const exts[] = {"mp3", "flac", "wav", "wave", "aif", "aiff", "aifc", "m4a", "aac", "ogg", "oga", "opus", "wma", "alac", "mp4", "wv", "ape"};
  const auto dot = name.find_last_of('.');
  if (dot == std::string::npos) return false;
  const std::string e = lower_ascii(name.substr(dot + 1));
  for (const char* x : exts)
    if (e == x) return true;
  return false;
}

bool file_exists(const std::string& p) {
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::path(p), ec);
}

}  // namespace

RelocateReport Library::relocate_scan(const RelocateOptions& o) {
  namespace fs = std::filesystem;
  RelocateReport rep;
  struct Missing {
    std::int64_t id = 0;
    std::string path, name;
    std::int64_t size = 0, duration = 0;
    std::string hash;
  };
  std::vector<Missing> miss;
  {
    Stmt s(db_, "SELECT t.id, r.last_seen_path, t.rel_path, coalesce(t.file_size, 0), coalesce(t.content_hash, ''), coalesce(t.duration_ms, 0)"
                " FROM tracks t JOIN library_roots r ON r.id = t.root_id ORDER BY t.id");
    while (s.step()) {
      Missing m;
      m.id = s.i64(0);
      m.path = join_path(s.text(1), s.text(2));
      if (file_exists(m.path)) continue;
      m.name = lower_ascii(file_name_of(m.path));
      m.size = s.i64(3);
      m.hash = s.text(4);
      m.duration = s.i64(5);
      miss.push_back(std::move(m));
    }
  }
  rep.missing = static_cast<int>(miss.size());
  if (miss.empty()) return rep;

  struct File {
    std::string path;
    std::int64_t size = 0;
  };
  std::vector<File> files;
  std::map<std::string, std::vector<std::size_t>> by_name;
  std::map<std::int64_t, std::vector<std::size_t>> by_size;
  for (const std::string& folder : o.folders) {
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::path(folder), fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
      if (o.cancelled && o.cancelled()) return rep;
      std::error_code e2;
      if (it->is_regular_file(e2) && audio_extension(it->path().filename().string())) {
        File f;
        f.path = it->path().string();
        f.size = static_cast<std::int64_t>(it->file_size(e2));
        if (!e2) {
          files.push_back(f);
          by_name[lower_ascii(it->path().filename().string())].push_back(files.size() - 1);
          by_size[f.size].push_back(files.size() - 1);
        }
      }
      it.increment(ec);
    }
  }
  rep.scanned = static_cast<int>(files.size());
  std::map<std::size_t, std::string> hashes;  // by file index, computed when needed
  const auto hash_of = [&](std::size_t i) -> const std::string& {
    auto at = hashes.find(i);
    if (at == hashes.end()) at = hashes.emplace(i, hash_file(files[i].path)).first;
    return at->second;
  };

  std::map<std::string, int> claims;  // new path -> how many tracks want it
  std::vector<RelocateMatch> found;
  for (const Missing& m : miss) {
    if (o.cancelled && o.cancelled()) break;
    std::vector<std::size_t> hit;
    std::string how;
    bool weak = false;
    if (!m.hash.empty() && m.size > 0) {
      const auto sz = by_size.find(m.size);
      if (sz != by_size.end())
        for (const std::size_t i : sz->second)
          if (hash_of(i) == m.hash) hit.push_back(i);
      if (!hit.empty()) how = "hash";
    }
    if (hit.empty()) {
      const auto nm = by_name.find(m.name);
      if (nm != by_name.end()) {
        for (const std::size_t i : nm->second) {
          const bool size_ok = m.size > 0 && files[i].size == m.size;
          if (m.size > 0 && !size_ok) continue;  // a known size must agree
          std::int64_t d = 0;
          if (o.probe_duration && m.duration > 0) d = o.probe_duration(files[i].path);
          if (d > 0 && std::abs(static_cast<double>(d - m.duration)) > o.duration_tolerance_ms) continue;
          hit.push_back(i);
        }
        // Everything that survived agrees on name (and size / duration where they are known).
        if (!hit.empty()) {
          const bool size_known = m.size > 0;
          const bool dur_known = o.probe_duration && m.duration > 0;
          how = size_known && dur_known ? "name+size+duration" : size_known ? "name+size" : dur_known ? "name+duration" : "name";
          weak = !size_known && !dur_known;
        }
      }
    }
    if (hit.empty()) {
      rep.not_found.push_back(m.id);
    } else if (hit.size() > 1) {
      RelocateAmbiguous a;
      a.track_id = m.id;
      a.old_path = m.path;
      for (const std::size_t i : hit) a.candidates.push_back(files[i].path);
      rep.ambiguous.push_back(std::move(a));
    } else if (weak && !o.allow_name_only) {
      rep.not_found.push_back(m.id);  // only the name agrees: not enough to move a track
    } else {
      RelocateMatch r;
      r.track_id = m.id;
      r.old_path = m.path;
      r.new_path = files[hit[0]].path;
      r.how = how;
      r.weak = weak;
      ++claims[r.new_path];
      found.push_back(std::move(r));
    }
  }
  for (RelocateMatch& r : found) {
    if (claims[r.new_path] > 1) {  // two tracks want one file: neither is guessed
      RelocateAmbiguous a;
      a.track_id = r.track_id;
      a.old_path = r.old_path;
      a.candidates.push_back(r.new_path);
      rep.ambiguous.push_back(std::move(a));
    } else {
      rep.found.push_back(std::move(r));
    }
  }
  return rep;
}

RelocateResult Library::relocate_apply(const std::vector<RelocateMatch>& matches) {
  RelocateResult res;
  if (matches.empty()) return res;
  EditScope scope("user");
  OptionalTx tx(db_);
  for (const RelocateMatch& m : matches) {
    std::string old_path;
    {
      Stmt s(db_, "SELECT r.last_seen_path, t.rel_path FROM tracks t JOIN library_roots r ON r.id = t.root_id WHERE t.id = ?1");
      s.bind(1, m.track_id);
      if (!s.step()) {
        ++res.skipped;
        continue;
      }
      old_path = join_path(s.text(0), s.text(1));
    }
    if (!file_exists(m.new_path) || (m.how != "manual" && file_exists(old_path)) || slashes(old_path) == slashes(m.new_path)) {
      ++res.skipped;
      continue;
    }
    Stmt u(db_, "UPDATE OR IGNORE tracks SET root_id = ?1, rel_path = ?2, format = ?3 WHERE id = ?4");  // size and hash stay: same content
    u.bind(1, kSystemRoot).bind(2, rel_of(m.new_path)).bind(3, extension_of(m.new_path)).bind(4, m.track_id);
    u.run();
    if (sqlite3_changes(db_.handle()) == 0) {  // another track already has that path
      ++res.skipped;
      continue;
    }
    journal_record(m.track_id, "path", old_path, m.new_path);
    ++res.applied;
  }
  res.batch_id = scope.batch();
  tx.commit();
  missing_at_ = 0;
  return res;
}

RelocateResult Library::relink_track(std::int64_t track_id, const std::string& new_path) {
  RelocateMatch m;
  m.track_id = track_id;
  m.new_path = new_path;
  m.how = "manual";
  return relocate_apply({m});
}

RelocateReport Library::relink_siblings(std::int64_t track_id, const std::string& new_path) {
  RelocateReport rep;
  const auto row = track(track_id);
  if (!row) return rep;
  const std::string old_dir = dir_of(row->path), new_dir = dir_of(new_path);
  if (old_dir.empty() || new_dir.empty() || old_dir == new_dir) return rep;
  Stmt s(db_, "SELECT t.id, r.last_seen_path, t.rel_path FROM tracks t JOIN library_roots r ON r.id = t.root_id ORDER BY t.id");
  while (s.step()) {
    const std::int64_t id = s.i64(0);
    if (id == track_id) continue;
    const std::string p = join_path(s.text(1), s.text(2)), n = slashes(p);
    if (n.size() <= old_dir.size() + 1 || n.compare(0, old_dir.size() + 1, old_dir + "/") != 0) continue;
    if (file_exists(p)) continue;
    ++rep.missing;
    // Native separators, as every other stored path has.
    const std::string target = std::filesystem::path(new_dir + "/" + n.substr(old_dir.size() + 1)).make_preferred().string();
    if (!file_exists(target)) {
      rep.not_found.push_back(id);
      continue;
    }
    RelocateMatch m;
    m.track_id = id;
    m.old_path = p;
    m.new_path = target;
    m.how = "sibling";
    rep.found.push_back(std::move(m));
  }
  return rep;
}

// ---- folder views -----------------------------------------------------------------------------------------------------

std::vector<std::int64_t> Library::folder_track_ids(std::int64_t folder_id, const std::string& search, SortKey sort, bool descending,
                                                    std::map<std::int64_t, std::string>* sources) {
  const std::vector<PlaylistNode> nodes = playlists();
  std::vector<std::int64_t> order;  // tree order, each track once
  std::set<std::int64_t> seen;
  const auto walk = [&](auto&& self, const PlaylistNode& n, int depth) -> void {
    if (depth > 32) return;
    if (!n.is_folder) {
      for (const std::int64_t t : playlist_track_ids(n.id)) {
        if (!seen.insert(t).second) continue;
        order.push_back(t);
        if (sources) (*sources)[t] = n.name;
      }
      return;
    }
    for (const PlaylistNode& c : nodes)
      if (c.parent == n.id) self(self, c, depth + 1);
  };
  for (const PlaylistNode& n : nodes)
    if (n.id == folder_id) walk(walk, n, 0);
  const std::vector<std::int64_t> base = track_ids(0, search, sort, descending);
  if (sort != SortKey::Default) {
    std::vector<std::int64_t> out;
    for (const std::int64_t id : base)
      if (seen.count(id)) out.push_back(id);
    return out;
  }
  const std::set<std::int64_t> allowed(base.begin(), base.end());
  std::vector<std::int64_t> out;
  for (const std::int64_t id : order)
    if (allowed.count(id)) out.push_back(id);
  if (descending) std::reverse(out.begin(), out.end());
  return out;
}

std::vector<DiskFolder> Library::disk_folders(const std::string& dir_in) {
  std::vector<std::string> paths;
  for (const auto& [id, p] : track_paths(db_, "1=1", 2000000)) paths.push_back(slashes(p));
  std::string dir = slashes(dir_in);
  if (dir == "/") dir.clear();
  const auto children_of = [&](const std::string& d, int& direct) {
    std::map<std::string, DiskFolder> kids;
    direct = 0;
    for (const std::string& p : paths) {
      std::size_t start;
      if (d.empty()) start = !p.empty() && p[0] == '/' ? 1 : 0;
      else if (p.size() > d.size() + 1 && p.compare(0, d.size() + 1, d + "/") == 0) start = d.size() + 1;
      else continue;
      const auto slash = p.find('/', start);
      if (slash == std::string::npos) {
        ++direct;
        continue;
      }
      DiskFolder& f = kids[p.substr(0, slash)];
      f.path = p.substr(0, slash);
      f.name = p.substr(start, slash - start);
      ++f.total;
      if (p.find('/', slash + 1) == std::string::npos) ++f.tracks;
      else f.has_children = true;
    }
    std::vector<DiskFolder> out;
    for (auto& [k, v] : kids) out.push_back(std::move(v));
    return out;
  };
  int direct = 0;
  std::vector<DiskFolder> kids = children_of(dir, direct);
  if (dir.empty()) {  // skip the single-child chain at the top (/Users/name/Music -> start there)
    while (kids.size() == 1 && direct == 0 && kids[0].has_children) kids = children_of(kids[0].path, direct);
  }
  return kids;
}

std::vector<std::int64_t> Library::disk_folder_track_ids(const std::string& dir_in, bool recursive, const std::string& search, SortKey sort,
                                                         bool descending) {
  std::string dir = slashes(dir_in);
  if (dir == "/") dir.clear();
  std::set<std::int64_t> in;
  for (const auto& [id, p0] : track_paths(db_, "1=1", 2000000)) {
    const std::string p = slashes(p0);
    if (p.size() <= dir.size() + 1 || p.compare(0, dir.size() + 1, dir + "/") != 0) continue;
    if (recursive || p.find('/', dir.size() + 1) == std::string::npos) in.insert(id);
  }
  std::vector<std::int64_t> out;
  for (const std::int64_t id : track_ids(0, search, sort, descending))
    if (in.count(id)) out.push_back(id);
  return out;
}
