#include "deckhand/sync/shared_playlist.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <tuple>
#include <unordered_map>

#include "frac.hpp"

namespace deckhand::sync {
namespace fs = std::filesystem;

namespace {

constexpr const char* kMagic = "DHSP1";

struct Stamp {
  std::int64_t wall = 0;
  std::int64_t counter = 0;
  std::string who;
  bool operator<(const Stamp& o) const { return std::tie(wall, counter, who) < std::tie(o.wall, o.counter, o.who); }
};

struct Rec {
  std::string id;
  Stamp stamp;
  bool del = false;
  std::string pos;
  SharedTrackRef track;
};

struct FileState {
  std::string playlist, participant, label;
  Stamp name_stamp;
  std::string name;
  std::map<std::string, Rec> recs;
};

std::string esc(const std::string& s) {
  std::string o;
  for (const char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\t') o += "\\t";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}

std::string unesc(const std::string& s) {
  std::string o;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char n = s[++i];
      o += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    } else {
      o += s[i];
    }
  }
  return o;
}

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> f;
  std::size_t a = 0;
  for (;;) {
    const std::size_t b = line.find('\t', a);
    if (b == std::string::npos) {
      f.push_back(line.substr(a));
      return f;
    }
    f.push_back(line.substr(a, b - a));
    a = b + 1;
  }
}

std::int64_t to_i(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }

bool read_file(const fs::path& path, FileState& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::string line;
  if (!std::getline(in, line) || line != kMagic) return false;
  bool complete = false;
  FileState st;
  while (std::getline(in, line)) {
    const std::vector<std::string> f = split_tabs(line);
    if (f.empty()) continue;
    if (f[0] == "playlist" && f.size() >= 2) st.playlist = f[1];
    else if (f[0] == "participant" && f.size() >= 3) {
      st.participant = f[1];
      st.label = unesc(f[2]);
    } else if (f[0] == "name" && f.size() >= 5) {
      st.name_stamp = {to_i(f[1]), to_i(f[2]), f[3]};
      st.name = unesc(f[4]);
    } else if (f[0] == "e" && f.size() >= 12) {
      Rec r;
      r.id = f[1];
      r.stamp = {to_i(f[2]), to_i(f[3]), f[4]};
      r.del = f[5] == "1";
      r.pos = f[6];
      r.track.hash = f[7];
      r.track.duration_ms = to_i(f[9]);
      r.track.artist = unesc(f[10]);
      r.track.title = unesc(f[11]);
      st.recs[r.id] = std::move(r);
    } else if (f[0] == "end") {
      complete = true;  // a file without its last line was cut off: ignored until it is whole
    }
  }
  if (!complete || st.participant.empty()) return false;
  out = std::move(st);
  return true;
}

bool write_file(const fs::path& path, const FileState& st) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.parent_path() / ("." + path.filename().string() + ".tmp");
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << kMagic << '\n' << "playlist\t" << st.playlist << '\n' << "participant\t" << st.participant << '\t' << esc(st.label) << '\n';
    o << "name\t" << st.name_stamp.wall << '\t' << st.name_stamp.counter << '\t' << st.name_stamp.who << '\t' << esc(st.name) << '\n';
    for (const auto& kv : st.recs) {
      const Rec& r = kv.second;
      o << "e\t" << r.id << '\t' << r.stamp.wall << '\t' << r.stamp.counter << '\t' << r.stamp.who << '\t' << (r.del ? 1 : 0) << '\t'
        << r.pos << '\t' << r.track.hash << "\t0\t" << r.track.duration_ms << '\t' << esc(r.track.artist) << '\t' << esc(r.track.title) << '\n';
    }
    o << "end\n";
    o.flush();
    if (!o) return false;
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

fs::path dir_of(const SharedConfig& cfg, const std::string& sid) { return fs::path(cfg.folder) / "DeckHandShared" / sid; }

std::vector<FileState> read_all(const SharedConfig& cfg, const std::string& sid) {
  std::vector<FileState> out;
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir_of(cfg, sid), ec)) {
    const std::string n = e.path().filename().string();
    if (!n.empty() && n[0] != '.' && e.path().extension() == ".dhsp") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const fs::path& p : files) {
    FileState st;
    if (read_file(p, st) && st.playlist == sid) out.push_back(std::move(st));
  }
  return out;
}

struct Merged {
  std::map<std::string, Rec> recs;
  std::string name;
  Stamp name_stamp;
  std::vector<std::string> labels;
  std::vector<const Rec*> live() const {
    std::vector<const Rec*> v;
    for (const auto& kv : recs)
      if (!kv.second.del) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const Rec* a, const Rec* b) { return a->pos != b->pos ? a->pos < b->pos : a->id < b->id; });
    return v;
  }
};

Merged merge(const std::vector<FileState>& files) {
  Merged m;
  for (const FileState& f : files) {
    m.labels.push_back(f.label);
    if (m.name.empty() || m.name_stamp < f.name_stamp) {
      m.name = f.name;
      m.name_stamp = f.name_stamp;
    }
    for (const auto& kv : f.recs) {
      auto it = m.recs.find(kv.first);
      if (it == m.recs.end() || it->second.stamp < kv.second.stamp) m.recs[kv.first] = kv.second;
    }
  }
  return m;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!s.empty() && s.back() == ' ') s.pop_back();
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  return s;
}

SharedTrackRef ref_of(Library& lib, std::int64_t id) {
  SharedTrackRef r;
  Stmt s(lib.db(), "SELECT coalesce(artist, ''), coalesce(title, ''), coalesce(duration_ms, 0), coalesce(content_hash, '') FROM tracks WHERE id = ?1");
  s.bind(1, id);
  if (s.step()) {
    r.artist = s.text(0);
    r.title = s.text(1);
    r.duration_ms = s.i64(2);
    r.hash = s.text(3);
  }
  return r;
}

// The local track that is this one: same content hash when both are known, else same artist and title and a duration
// within two seconds (an unknown duration matches any).
std::int64_t match_local(Library& lib, const SharedTrackRef& t) {
  if (!t.hash.empty()) {
    Stmt s(lib.db(), "SELECT id FROM tracks WHERE content_hash = ?1 ORDER BY id LIMIT 1");
    s.bind(1, t.hash);
    if (s.step()) return s.i64(0);
  }
  const std::string title = lower(t.title), artist = lower(t.artist);
  if (title.empty()) return 0;
  Stmt s(lib.db(), "SELECT id, coalesce(artist, ''), coalesce(duration_ms, 0), coalesce(content_hash, '') FROM tracks WHERE lower(title) = ?1 ORDER BY id");
  s.bind(1, title);
  while (s.step()) {
    if (lower(s.text(1)) != artist) continue;
    const std::string h = s.text(3);
    if (!t.hash.empty() && !h.empty()) continue;  // both hashed and different: another recording
    const std::int64_t d = s.i64(2);
    if (t.duration_ms > 0 && d > 0 && std::llabs(d - t.duration_ms) > 2000) continue;
    return s.i64(0);
  }
  return 0;
}

// ---- the local link (a library setting per local playlist)

struct Link {
  std::string shared_id, participant, name_synced;
  std::int64_t counter = 0;
  std::vector<std::pair<std::string, std::int64_t>> base;  // entry id, local track id (0 = missing here), as last applied
};

std::string link_key(std::int64_t playlist_id) { return "shared.link." + std::to_string(playlist_id); }

std::optional<Link> load_link(Library& lib, std::int64_t playlist_id) {
  const std::string text = lib.setting(link_key(playlist_id));
  if (text.empty()) return std::nullopt;
  Link l;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const auto f = split_tabs(line);
    if (f.size() >= 2 && f[0] == "shared") l.shared_id = f[1];
    else if (f.size() >= 2 && f[0] == "participant") l.participant = f[1];
    else if (f.size() >= 2 && f[0] == "counter") l.counter = to_i(f[1]);
    else if (f.size() >= 2 && f[0] == "name") l.name_synced = unesc(f[1]);
    else if (f.size() >= 3 && f[0] == "b") l.base.push_back({f[1], to_i(f[2])});
  }
  if (l.shared_id.empty() || l.participant.empty()) return std::nullopt;
  return l;
}

void save_link(Library& lib, std::int64_t playlist_id, const Link& l) {
  std::ostringstream o;
  o << "shared\t" << l.shared_id << "\nparticipant\t" << l.participant << "\ncounter\t" << l.counter << "\nname\t" << esc(l.name_synced) << '\n';
  for (const auto& b : l.base) o << "b\t" << b.first << '\t' << b.second << '\n';
  lib.set_setting(link_key(playlist_id), o.str());
}

std::string playlist_name(Library& lib, std::int64_t id) {
  for (const PlaylistNode& n : lib.playlists())
    if (n.id == id) return n.name;
  return {};
}

std::int64_t now_of(const SharedConfig& cfg) {
  if (cfg.clock) return cfg.clock();
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string new_id_of(const SharedConfig& cfg) {
  if (cfg.new_id) return cfg.new_id();
  std::random_device rd;
  std::mt19937_64 g((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(g()));
  return buf;
}

struct Core {
  Library& lib;
  const SharedConfig& cfg;
  std::int64_t playlist_id;
  Link link;
};

// Sends the local changes and applies the merged result.
SharedResult run_sync(Core& c, bool first) {
  SharedResult res;
  res.shared_id = c.link.shared_id;
  res.playlist_id = c.playlist_id;
  if (c.cfg.folder.empty()) {
    res.error = "no shared folder is set";
    return res;
  }
  Link& L = c.link;
  const fs::path own_path = dir_of(c.cfg, L.shared_id) / (L.participant + ".dhsp");
  std::vector<FileState> files = read_all(c.cfg, L.shared_id);
  FileState own;
  bool have_own = false;
  for (const FileState& f : files)
    if (f.participant == L.participant) {
      own = f;
      have_own = true;
    }
  if (!have_own) {
    own.playlist = L.shared_id;
    own.participant = L.participant;
    own.label = c.cfg.label.empty() ? "DeckHand" : c.cfg.label;
  }
  own.label = c.cfg.label.empty() ? own.label : c.cfg.label;
  Merged before = merge(files);

  // ---- what changed locally since the last sync
  const std::vector<std::int64_t> local = c.lib.playlist_track_ids(c.playlist_id);
  std::unordered_map<std::int64_t, std::vector<std::size_t>> queue;  // track id -> unmatched base indices, in order
  for (std::size_t i = 0; i < L.base.size(); ++i)
    if (L.base[i].second > 0) queue[L.base[i].second].push_back(i);
  std::vector<std::size_t> matched_base(local.size(), static_cast<std::size_t>(-1));  // local index -> base index
  std::vector<bool> base_used(L.base.size(), false);
  for (std::size_t i = 0; i < local.size(); ++i) {
    auto it = queue.find(local[i]);
    if (it == queue.end() || it->second.empty()) continue;
    // Take the first unused entry that has not been passed by an earlier match when possible (keeps order stable).
    matched_base[i] = it->second.front();
    base_used[it->second.front()] = true;
    it->second.erase(it->second.begin());
  }
  // A track that was missing here and has since been added to this library (and to the playlist by hand) is that entry.
  for (std::size_t i = 0; i < local.size(); ++i) {
    if (matched_base[i] != static_cast<std::size_t>(-1)) continue;
    for (std::size_t b = 0; b < L.base.size(); ++b) {
      if (base_used[b] || L.base[b].second != 0) continue;
      const auto rit = before.recs.find(L.base[b].first);
      if (rit == before.recs.end() || rit->second.del || match_local(c.lib, rit->second.track) != local[i]) continue;
      matched_base[i] = b;
      base_used[b] = true;
      L.base[b].second = local[i];
      break;
    }
  }
  std::vector<std::string> removed_ids;
  for (std::size_t i = 0; i < L.base.size(); ++i)
    if (L.base[i].second > 0 && !base_used[i]) removed_ids.push_back(L.base[i].first);

  // Order check: entries whose base index increases along the local order stay; the others moved.
  std::vector<std::size_t> seq;  // local indices of matched items
  for (std::size_t i = 0; i < local.size(); ++i)
    if (matched_base[i] != static_cast<std::size_t>(-1)) seq.push_back(i);
  std::vector<int> best_len(seq.size(), 1), prev(seq.size(), -1);
  int best_end = -1;
  for (std::size_t a = 0; a < seq.size(); ++a) {
    for (std::size_t b = 0; b < a; ++b)
      if (matched_base[seq[b]] < matched_base[seq[a]] && best_len[b] + 1 > best_len[a]) {
        best_len[a] = best_len[b] + 1;
        prev[a] = static_cast<int>(b);
      }
    if (best_end < 0 || best_len[a] > best_len[static_cast<std::size_t>(best_end)]) best_end = static_cast<int>(a);
  }
  std::vector<bool> anchor(local.size(), false);
  for (int a = best_end; a >= 0; a = prev[static_cast<std::size_t>(a)]) anchor[seq[static_cast<std::size_t>(a)]] = true;

  // The positions of the local items: anchors keep their merged key; adds and moves get a key between their neighbours.
  const auto merged_pos = [&](const std::string& id) -> std::string {
    const auto it = before.recs.find(id);
    return it == before.recs.end() ? std::string() : it->second.pos;
  };
  std::vector<std::string> keep_key(local.size());
  for (std::size_t i = 0; i < local.size(); ++i)
    if (anchor[i]) keep_key[i] = merged_pos(L.base[matched_base[i]].first);
  const std::int64_t wall = now_of(c.cfg);
  const auto stamp = [&]() { return Stamp{wall, ++L.counter, L.participant}; };
  std::string last_key;
  bool changed_own = !have_own;
  for (std::size_t i = 0; i < local.size(); ++i) {
    if (anchor[i]) {
      last_key = keep_key[i];
      continue;
    }
    std::string next_key;
    for (std::size_t j = i + 1; j < local.size() && next_key.empty(); ++j)
      if (anchor[j]) next_key = keep_key[j];
    std::string key = (!next_key.empty() && last_key >= next_key) ? last_key + "V" : detail::key_between(last_key, next_key);
    last_key = key;
    if (matched_base[i] != static_cast<std::size_t>(-1)) {  // moved
      const std::string id = L.base[matched_base[i]].first;
      Rec r = before.recs.count(id) ? before.recs.at(id) : Rec{};
      r.id = id;
      r.pos = key;
      r.del = false;
      r.stamp = stamp();
      own.recs[id] = r;
      ++res.moved;
    } else {  // added
      Rec r;
      r.id = L.participant + ":" + std::to_string(L.counter + 1);
      r.pos = key;
      r.track = ref_of(c.lib, local[i]);
      r.stamp = stamp();
      own.recs[r.id] = r;
      ++res.added;
    }
    changed_own = true;
  }
  for (const std::string& id : removed_ids) {
    Rec r = before.recs.count(id) ? before.recs.at(id) : Rec{};
    r.id = id;
    r.del = true;
    r.stamp = stamp();
    own.recs[id] = r;
    ++res.removed;
    changed_own = true;
  }
  const std::string local_name = playlist_name(c.lib, c.playlist_id);
  if (first || (!local_name.empty() && local_name != L.name_synced)) {
    own.name = local_name;
    own.name_stamp = stamp();
    changed_own = true;
  } else if (!have_own) {
    own.name = before.name;
  }
  if (changed_own) {
    if (!write_file(own_path, own)) {
      res.error = "could not write into the shared folder";
      return res;
    }
  }

  // ---- apply the merged state
  files = read_all(c.cfg, L.shared_id);
  const Merged m = merge(files);
  std::vector<std::pair<std::string, std::int64_t>> base;
  std::vector<std::int64_t> list;
  for (const Rec* r : m.live()) {
    const std::int64_t id = match_local(c.lib, r->track);
    base.push_back({r->id, id});
    if (id > 0) list.push_back(id);
    else ++res.missing;
  }
  if (list != local) c.lib.set_playlist_tracks(c.playlist_id, list);
  {  // what the other side changed since the last sync
    std::map<std::string, bool> old_ids;
    for (const auto& b : L.base) old_ids[b.first] = true;
    std::map<std::string, bool> live_ids;
    for (const Rec* r : m.live()) {
      live_ids[r->id] = true;
      if (!old_ids.count(r->id) && r->stamp.who != L.participant) ++res.received;
    }
    for (const auto& b : L.base) {
      const auto it = m.recs.find(b.first);
      if (!live_ids.count(b.first) && it != m.recs.end() && it->second.stamp.who != L.participant) ++res.received;
    }
  }
  if (!m.name.empty() && m.name != local_name && m.name_stamp.who != L.participant) c.lib.rename_playlist(c.playlist_id, m.name);
  L.base = std::move(base);
  L.name_synced = m.name.empty() ? local_name : m.name;
  save_link(c.lib, c.playlist_id, L);
  res.ok = true;
  return res;
}

}  // namespace

SharedResult share_playlist(Library& lib, std::int64_t playlist_id, const SharedConfig& cfg) {
  SharedResult res;
  try {
    if (load_link(lib, playlist_id)) {
      res.error = "this playlist is already shared";
      return res;
    }
    bool found = false;
    for (const PlaylistNode& n : lib.playlists())
      if (n.id == playlist_id && !n.is_folder) found = true;
    if (!found) {
      res.error = "no such playlist";
      return res;
    }
    Core c{lib, cfg, playlist_id, {}};
    c.link.shared_id = new_id_of(cfg);
    c.link.participant = new_id_of(cfg);
    c.link.name_synced.clear();
    c.link.base.clear();
    res = run_sync(c, true);
  } catch (const std::exception& e) {
    res.ok = false;
    res.error = e.what();
  }
  return res;
}

SharedResult join_shared_playlist(Library& lib, const std::string& shared_id, const SharedConfig& cfg, std::int64_t parent) {
  SharedResult res;
  try {
    const std::vector<FileState> files = read_all(cfg, shared_id);
    if (files.empty()) {
      res.error = "no such shared playlist in the folder";
      return res;
    }
    const Merged m = merge(files);
    const std::int64_t pl = lib.create_playlist(parent, m.name.empty() ? "Shared playlist" : m.name, false);
    Core c{lib, cfg, pl, {}};
    c.link.shared_id = shared_id;
    c.link.participant = new_id_of(cfg);
    c.link.name_synced = m.name.empty() ? std::string("Shared playlist") : m.name;  // the name is theirs: not sent back
    res = run_sync(c, false);
    if (!res.ok) lib.purge_deleted_playlist(lib.delete_playlist(pl));  // a failed join leaves nothing behind, not even a trash entry
  } catch (const std::exception& e) {
    res.ok = false;
    res.error = e.what();
  }
  return res;
}

SharedResult sync_shared_playlist(Library& lib, std::int64_t playlist_id, const SharedConfig& cfg) {
  SharedResult res;
  try {
    auto link = load_link(lib, playlist_id);
    if (!link) {
      res.error = "this playlist is not shared";
      return res;
    }
    Core c{lib, cfg, playlist_id, *link};
    res = run_sync(c, false);
  } catch (const std::exception& e) {
    res.ok = false;
    res.error = e.what();
  }
  return res;
}

void unlink_shared_playlist(Library& lib, std::int64_t playlist_id) { lib.set_setting(link_key(playlist_id), ""); }

std::vector<SharedEntry> shared_playlist_entries(Library& lib, std::int64_t playlist_id, const SharedConfig& cfg) {
  std::vector<SharedEntry> out;
  const auto link = load_link(lib, playlist_id);
  if (!link) return out;
  const Merged m = merge(read_all(cfg, link->shared_id));
  for (const Rec* r : m.live()) {
    SharedEntry e;
    e.entry_id = r->id;
    e.track = r->track;
    e.local_track_id = match_local(lib, r->track);
    e.missing = e.local_track_id == 0;
    out.push_back(std::move(e));
  }
  return out;
}

std::vector<SharedPlaylistInfo> list_shared_playlists(const SharedConfig& cfg) {
  std::vector<SharedPlaylistInfo> out;
  std::error_code ec;
  const fs::path root = fs::path(cfg.folder) / "DeckHandShared";
  std::vector<std::string> ids;
  for (const auto& e : fs::directory_iterator(root, ec))
    if (e.is_directory(ec)) ids.push_back(e.path().filename().string());
  std::sort(ids.begin(), ids.end());
  for (const std::string& id : ids) {
    const std::vector<FileState> files = read_all(cfg, id);
    if (files.empty()) continue;
    SharedPlaylistInfo i;
    i.shared_id = id;
    const Merged m = merge(files);
    i.name = m.name;
    i.participants = m.labels;
    out.push_back(std::move(i));
  }
  return out;
}

}  // namespace deckhand::sync
