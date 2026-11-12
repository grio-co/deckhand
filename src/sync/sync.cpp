#include "deckhand/sync/sync.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "deckhand/log/log.hpp"
#include "format.hpp"
#include "frac.hpp"
#include "state.hpp"

namespace fs = std::filesystem;

namespace deckhand::sync {
namespace {

using namespace detail;

struct Crash {
  std::string stage;
};

constexpr std::int64_t kSystemRoot = 1;

struct FieldDef {
  const char* name;
  Val::Type type;
};
// Track columns that sync, in SELECT order. Everything else about a track row is device-specific.
const FieldDef kTrackFields[] = {
    {"title", Val::Text},      {"artist", Val::Text},     {"album", Val::Text},      {"genre", Val::Text},
    {"label", Val::Text},      {"comment", Val::Text},    {"key", Val::Text},        {"format", Val::Text},
    {"year", Val::Int},        {"rating", Val::Int},      {"color", Val::Int},       {"duration_ms", Val::Int},
    {"bitrate", Val::Int},     {"sample_rate", Val::Int}, {"date_added", Val::Int},  {"bpm", Val::Real},
    {"gain_db", Val::Real}};
constexpr int kNumTrackFields = static_cast<int>(sizeof kTrackFields / sizeof kTrackFields[0]);

std::string join_path(const std::string& root, const std::string& rel) {
  if (root.empty() || root.back() == '/') return root + rel;
  return root + "/" + rel;
}
std::string rel_of(const std::string& abs) { return abs.size() > 1 && abs[0] == '/' ? abs.substr(1) : abs; }
std::string base_name(const std::string& path) {
  const auto s = path.find_last_of("/\\");
  return s == std::string::npos ? path : path.substr(s + 1);
}
std::string hex_of(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
  return b;
}

Val read_val(const Stmt& s, int col, Val::Type t) {
  if (s.is_null(col)) return Val::null();
  switch (t) {
    case Val::Int: return Val::integer(s.i64(col));
    case Val::Real: return Val::real(s.f64(col));
    default: return Val::text(s.text(col));
  }
}
void bind_val(Stmt& s, int col, const Val& v) {  // binds four consecutive parameters: type, int, real, text
  s.bind(col, static_cast<std::int64_t>(v.t));
  v.t == Val::Int ? s.bind(col + 1, v.i) : s.bind_null(col + 1);
  v.t == Val::Real ? s.bind(col + 2, v.d) : s.bind_null(col + 2);
  v.t == Val::Text ? s.bind(col + 3, std::string_view(v.s)) : s.bind_null(col + 3);
}
void bind_opt(Stmt& s, int col, const Val& v) {  // a column value: NULL, integer, real or text
  switch (v.t) {
    case Val::Null: s.bind_null(col); break;
    case Val::Int: s.bind(col, v.i); break;
    case Val::Real: s.bind(col, v.d); break;
    case Val::Text: s.bind(col, std::string_view(v.s)); break;
  }
}

std::string encode_grid(const std::vector<BeatSection>& g) {
  std::string out;
  char b[96];
  for (const auto& s : g) {
    std::snprintf(b, sizeof b, "%.17g,%.17g,%d;", s.first_beat_ms, s.bpm, s.bar_beat);
    out += b;
  }
  return out;
}
std::vector<BeatSection> decode_grid(const std::string& text) {
  std::vector<BeatSection> out;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const auto end = text.find(';', pos);
    if (end == std::string::npos) break;
    const std::string item = text.substr(pos, end - pos);
    pos = end + 1;
    char* p = nullptr;
    BeatSection s;
    s.first_beat_ms = std::strtod(item.c_str(), &p);
    if (*p != ',') continue;
    s.bpm = std::strtod(p + 1, &p);
    if (*p != ',') continue;
    s.bar_beat = static_cast<int>(std::strtol(p + 1, nullptr, 10));
    out.push_back(s);
  }
  return out;
}

bool setting_allowed(const std::string& key, const std::vector<std::string>& allow) {
  std::string lower = key;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  for (const char* deny : {"token", "password", "secret", "api", "audio", "midi", "device", "path", "sync."})
    if (lower.find(deny) != std::string::npos) return false;
  for (const auto& p : allow)
    if (key.compare(0, p.size(), p) == 0) return true;
  return false;
}

bool parse_name(const std::string& name, std::uint64_t& seq, bool& snapshot) {
  const auto dot = name.find('.');
  if (dot == std::string::npos || dot == 0 || dot > 18) return false;
  const std::string ext = name.substr(dot);
  if (ext != ".dhlog" && ext != ".dhsnap") return false;
  for (std::size_t i = 0; i < dot; ++i)
    if (name[i] < '0' || name[i] > '9') return false;
  seq = std::strtoull(name.substr(0, dot).c_str(), nullptr, 10);
  snapshot = ext == ".dhsnap";
  return true;
}
std::string file_name_for(std::uint64_t seq, bool snapshot) {
  char b[32];
  std::snprintf(b, sizeof b, "%08llu", static_cast<unsigned long long>(seq));
  return std::string(b) + (snapshot ? ".dhsnap" : ".dhlog");
}

// ---- order helpers ----------------------------------------------------------------------------------------

// Longest common subsequence (as index pairs) of two string sequences; trims common ends first.
std::vector<std::pair<int, int>> lcs_pairs(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  std::vector<std::pair<int, int>> out;
  const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
  int lo = 0;
  while (lo < n && lo < m && a[static_cast<std::size_t>(lo)] == b[static_cast<std::size_t>(lo)]) {
    out.emplace_back(lo, lo);
    ++lo;
  }
  int ha = n, hb = m;
  while (ha > lo && hb > lo && a[static_cast<std::size_t>(ha - 1)] == b[static_cast<std::size_t>(hb - 1)]) {
    --ha;
    --hb;
  }
  const std::size_t rows = static_cast<std::size_t>(ha - lo), cols = static_cast<std::size_t>(hb - lo);
  if (rows && cols && rows * cols <= 4u * 1024 * 1024) {
    std::vector<std::uint32_t> t((rows + 1) * (cols + 1), 0);
    const auto at = [&](std::size_t i, std::size_t j) -> std::uint32_t& { return t[i * (cols + 1) + j]; };
    for (std::size_t i = rows; i-- > 0;)
      for (std::size_t j = cols; j-- > 0;)
        at(i, j) = a[lo + i] == b[lo + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
    std::size_t i = 0, j = 0;
    while (i < rows && j < cols) {
      if (a[lo + i] == b[lo + j]) {
        out.emplace_back(lo + static_cast<int>(i), lo + static_cast<int>(j));
        ++i;
        ++j;
      } else if (at(i + 1, j) >= at(i, j + 1)) {
        ++i;
      } else {
        ++j;
      }
    }
  }
  for (int k = 0; k < n - ha; ++k) out.emplace_back(ha + k, hb + k);
  return out;
}

// Indexes (into ranks) of a longest strictly increasing subsequence, ignoring entries < 0.
std::vector<bool> lis_keep(const std::vector<int>& ranks) {
  std::vector<int> tails, tail_idx, prev(ranks.size(), -1);
  for (std::size_t i = 0; i < ranks.size(); ++i) {
    if (ranks[i] < 0) continue;
    const auto it = std::lower_bound(tails.begin(), tails.end(), ranks[i]);
    const auto pos = static_cast<std::size_t>(it - tails.begin());
    if (it == tails.end()) {
      tails.push_back(ranks[i]);
      tail_idx.push_back(static_cast<int>(i));
    } else {
      *it = ranks[i];
      tail_idx[pos] = static_cast<int>(i);
    }
    prev[i] = pos ? tail_idx[pos - 1] : -1;
  }
  std::vector<bool> keep(ranks.size(), false);
  for (int i = tail_idx.empty() ? -1 : tail_idx.back(); i >= 0; i = prev[static_cast<std::size_t>(i)])
    keep[static_cast<std::size_t>(i)] = true;
  return keep;
}

// Given the desired order, which items may keep their key (anchors), their current keys and ids, returns a new key
// for every item that must change ("" = unchanged). Items are ordered by (key, id), so equal keys are fine for
// anchors; one is only re-keyed when something has to be inserted between two equal keys.
std::vector<std::string> plan_keys(const std::vector<bool>& anchor, const std::vector<std::string>& keys,
                                   const std::vector<std::string>& ids) {
  const std::size_t n = anchor.size();
  std::vector<bool> keep(n, false);
  std::string last_key, last_id;
  for (std::size_t i = 0; i < n; ++i)
    if (anchor[i] && !keys[i].empty() && std::tie(keys[i], ids[i]) > std::tie(last_key, last_id)) {
      keep[i] = true;
      last_key = keys[i];
      last_id = ids[i];
    }
  std::vector<std::string> out(n);
  std::string prev;
  for (std::size_t i = 0; i < n;) {
    if (keep[i]) {
      prev = keys[i];
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < n && !keep[j]) ++j;
    while (j < n && prev == keys[j]) {  // no room between equal keys: re-key the right-hand one too
      keep[j] = false;
      ++j;
      while (j < n && !keep[j]) ++j;
    }
    const std::string next = j < n ? keys[j] : std::string();
    std::vector<std::string> fresh;
    keys_between(prev, next, j - i, fresh);
    for (std::size_t k = i; k < j; ++k) out[k] = fresh[k - i];
    if (!fresh.empty()) prev = fresh.back();
    i = j;
  }
  return out;
}

// ---- effective playlist tree --------------------------------------------------------------------------------

struct Tree {
  std::unordered_map<std::string, std::string> parent;  // effective parent ("" = top level); cycles broken
  std::unordered_set<std::string> dead;                  // deleted, or inside a deleted folder
  std::unordered_map<std::string, std::vector<std::string>> kids;  // alive children in order
  std::vector<std::string> order;                        // alive nodes, parents before children
};

Tree build_tree(const State& st) {
  const auto& nodes = st.ent[State::idx('P')];
  Tree t;
  for (const auto& [eid, e] : nodes) {
    (void)e;
    std::string p = st.text('P', eid, "parent");
    if (p == eid || !nodes.count(p)) p.clear();
    t.parent[eid] = p;
  }
  // Break cycles deterministically: the edge with the oldest stamp loses and its node goes to the top level.
  std::unordered_map<std::string, int> color;
  for (const auto& [eid, e] : nodes) {
    (void)e;
    for (;;) {
      std::vector<std::string> path;
      std::string cur = eid;
      bool cycle = false;
      while (!cur.empty() && color[cur] != 2) {
        if (color[cur] == 1) {
          const auto from = std::find(path.begin(), path.end(), cur);
          std::string victim = *from;
          for (auto it = from; it != path.end(); ++it) {
            const Reg* a = st.find('P', *it, "parent");
            const Reg* b = st.find('P', victim, "parent");
            if (a && b && a->st < b->st) victim = *it;
          }
          t.parent[victim].clear();
          for (const auto& p : path) color[p] = 0;
          cycle = true;
          break;
        }
        color[cur] = 1;
        path.push_back(cur);
        cur = t.parent[cur];
      }
      if (!cycle) {
        for (const auto& p : path) color[p] = 2;
        break;
      }
    }
  }
  std::unordered_map<std::string, int> memo;  // 1 dead, 2 alive
  const std::function<bool(const std::string&)> is_dead = [&](const std::string& n) -> bool {
    if (n.empty()) return false;
    int& m = memo[n];
    if (m) return m == 1;
    const bool d = !st.alive('P', n) || is_dead(t.parent[n]);
    memo[n] = d ? 1 : 2;
    return d;
  };
  for (const auto& [eid, e] : nodes) {
    (void)e;
    if (is_dead(eid)) {
      t.dead.insert(eid);
    } else {
      t.kids[t.parent[eid]].push_back(eid);
    }
  }
  for (auto& [p, list] : t.kids) {
    (void)p;
    std::sort(list.begin(), list.end(), [&](const std::string& x, const std::string& y) {
      const std::string px = st.text('P', x, "pos"), py = st.text('P', y, "pos");
      return px != py ? px < py : x < y;
    });
  }
  std::vector<std::string> queue{""};
  for (std::size_t i = 0; i < queue.size(); ++i) {
    const auto it = t.kids.find(queue[i]);
    if (it == t.kids.end()) continue;
    for (const auto& k : it->second) {
      t.order.push_back(k);
      queue.push_back(k);
    }
  }
  return t;
}

}  // namespace

std::vector<std::string> DefaultSettingsAllow() {
  return {"ui.language", "ui.skin", "analysis.", "cue.", "mix.", "stream.template", "obs.text_template"};
}

std::string SyncEngine::fingerprint_file(const std::string& path) {
  std::error_code ec;
  const auto size = fs::file_size(fs::path(path), ec);
  if (ec) return {};
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) return {};
  constexpr std::uintmax_t kWin = 64 * 1024;
  Hash128 h;
  std::vector<char> buf(kWin);
  const auto window = [&](std::uintmax_t at, std::uintmax_t len) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(at));
    in.read(buf.data(), static_cast<std::streamsize>(len));
    h.add(buf.data(), static_cast<std::size_t>(in.gcount()));
  };
  if (size <= 4 * kWin) {
    for (std::uintmax_t at = 0; at < size; at += kWin) window(at, std::min(kWin, size - at));
  } else {
    for (const std::uintmax_t at : {size / 4, size / 2, size / 4 * 3}) window(at, kWin);
  }
  return "f1-" + hex_of(size) + "-" + h.hex();
}

// ==== engine ==================================================================================================

struct SyncEngine::Impl {
  Library& lib;
  Db& db;
  SyncConfig cfg;
  State st;
  std::string dev, dev_name, init_error;
  std::int64_t hlc_wall = 0;
  std::uint32_t hlc_ctr = 0;
  std::uint64_t next_seq = 1, eid_ctr = 0;
  std::int64_t last_sync = 0;
  std::string last_error;
  int changes = 0;

  struct TC {
    std::string fp, path;
    std::int64_t size = 0, mtime = 0;
    bool missing = false;
  };
  struct IdMap {
    std::unordered_map<std::int64_t, std::string> l2e;
    std::unordered_map<std::string, std::int64_t> e2l;
  };

  Impl(Library& l, SyncConfig c) : lib(l), db(l.db()), cfg(std::move(c)) {
    if (cfg.settings_allow.empty()) cfg.settings_allow = DefaultSettingsAllow();
    if (cfg.device_name.empty()) cfg.device_name = "DeckHand";
    try {
      init();
    } catch (const std::exception& e) {
      init_error = e.what();
      DH_LOG(Error, "sync: init failed: %s", e.what());
    }
  }

  std::int64_t now() const {
    if (cfg.clock) return cfg.clock();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  void fault(const char* stage) {
    if (cfg.fault && cfg.fault(stage)) throw Crash{stage};
  }

  // ---- storage ----------------------------------------------------------------------------------------------
  void init() {
    Transaction tx(db);
    db.exec(
        "CREATE TABLE IF NOT EXISTS sync_meta(key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS sync_reg(kind TEXT NOT NULL, eid TEXT NOT NULL, field TEXT NOT NULL,"
        " wall INTEGER NOT NULL, ctr INTEGER NOT NULL, dev TEXT NOT NULL, vt INTEGER NOT NULL, vi INTEGER, vr REAL,"
        " vs TEXT, pending INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(kind, eid, field)) WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS sync_map(kind TEXT NOT NULL, local_id INTEGER NOT NULL, eid TEXT NOT NULL,"
        " PRIMARY KEY(kind, local_id));"
        "CREATE INDEX IF NOT EXISTS idx_sync_map_eid ON sync_map(kind, eid);"
        "CREATE TABLE IF NOT EXISTS sync_track(track_id INTEGER PRIMARY KEY, fp TEXT NOT NULL, path TEXT,"
        " size INTEGER NOT NULL DEFAULT 0, mtime INTEGER NOT NULL DEFAULT 0, missing INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_sync_track_fp ON sync_track(fp);"
        "CREATE TABLE IF NOT EXISTS sync_files(dev TEXT NOT NULL, name TEXT NOT NULL, size INTEGER NOT NULL,"
        " status INTEGER NOT NULL, PRIMARY KEY(dev, name));");
    tx.commit();
    dev = meta_get("device_id");
    if (dev.empty()) {
      std::random_device rd;
      const std::uint64_t id = (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
                               static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
      char b[20];
      std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(id));
      dev = b;
      meta_put("device_id", dev);
    }
    dev_name = cfg.device_name;
    meta_put("device_name", dev_name);
    reload();
  }

  std::string meta_get(const char* key) {
    Stmt s(db, "SELECT value FROM sync_meta WHERE key = ?1");
    s.bind(1, key);
    return s.step() ? s.text(0) : std::string();
  }
  void meta_put(const char* key, const std::string& value) {
    Stmt s(db, "INSERT INTO sync_meta(key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bind(1, key).bind(2, value);
    s.run();
  }
  void save_meta() {
    meta_put("hlc_wall", std::to_string(hlc_wall));
    meta_put("hlc_ctr", std::to_string(hlc_ctr));
    meta_put("next_seq", std::to_string(next_seq));
    meta_put("eid_ctr", std::to_string(eid_ctr));
    meta_put("last_sync_ms", std::to_string(last_sync));
  }
  void reload() {
    const auto num = [&](const char* k) {
      const std::string v = meta_get(k);
      return v.empty() ? 0ll : std::strtoll(v.c_str(), nullptr, 10);
    };
    hlc_wall = num("hlc_wall");
    hlc_ctr = static_cast<std::uint32_t>(num("hlc_ctr"));
    next_seq = static_cast<std::uint64_t>(std::max(1ll, num("next_seq")));
    eid_ctr = static_cast<std::uint64_t>(num("eid_ctr"));
    last_sync = num("last_sync_ms");
    for (auto& e : st.ent) e.clear();
    st.pending.clear();
    st.dirty.clear();
    Stmt s(db, "SELECT kind, eid, field, wall, ctr, dev, vt, vi, vr, vs, pending FROM sync_reg");
    while (s.step()) {
      const std::string kind = s.text(0);
      const int k = kind.size() == 1 ? State::idx(kind[0]) : -1;
      if (k < 0) continue;
      Reg& r = st.ent[k][s.text(1)].f[s.text(2)];
      r.st = {s.i64(3), static_cast<std::uint32_t>(s.i64(4)), s.text(5)};
      switch (static_cast<Val::Type>(s.i64(6))) {
        case Val::Int: r.v = Val::integer(s.i64(7)); break;
        case Val::Real: r.v = Val::real(s.f64(8)); break;
        case Val::Text: r.v = Val::text(s.text(9)); break;
        default: r.v = Val::null(); break;
      }
      r.pending = s.i64(10) != 0;
      if (r.pending) st.pending.insert(Key{kind[0], s.text(1), s.text(2)});
    }
  }
  void flush_regs() {
    if (st.dirty.empty()) return;
    Stmt s(db,
           "INSERT OR REPLACE INTO sync_reg(kind, eid, field, wall, ctr, dev, vt, vi, vr, vs, pending)"
           " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
    for (const auto& [kind, eid, field] : st.dirty) {
      const Reg* r = st.find(kind, eid, field);
      if (!r) continue;
      s.bind(1, std::string_view(&kind, 1)).bind(2, eid).bind(3, field);
      s.bind(4, r->st.wall).bind(5, static_cast<std::int64_t>(r->st.ctr)).bind(6, r->st.dev);
      bind_val(s, 7, r->v);
      s.bind(11, static_cast<std::int64_t>(r->pending ? 1 : 0));
      s.run();
    }
    st.dirty.clear();
  }

  IdMap load_map(char kind) {
    IdMap m;
    Stmt s(db, "SELECT local_id, eid FROM sync_map WHERE kind = ?1");
    s.bind(1, std::string_view(&kind, 1));
    while (s.step()) {
      m.l2e[s.i64(0)] = s.text(1);
      m.e2l[s.text(1)] = s.i64(0);
    }
    return m;
  }
  void map_put(IdMap& m, char kind, std::int64_t id, const std::string& eid) {
    Stmt s(db, "INSERT OR REPLACE INTO sync_map(kind, local_id, eid) VALUES (?1, ?2, ?3)");
    s.bind(1, std::string_view(&kind, 1)).bind(2, id).bind(3, eid);
    s.run();
    m.l2e[id] = eid;
    m.e2l[eid] = id;
  }
  void map_drop(IdMap& m, char kind, std::int64_t id) {
    Stmt s(db, "DELETE FROM sync_map WHERE kind = ?1 AND local_id = ?2");
    s.bind(1, std::string_view(&kind, 1)).bind(2, id);
    s.run();
    const auto it = m.l2e.find(id);
    if (it != m.l2e.end()) {
      m.e2l.erase(it->second);
      m.l2e.erase(it);
    }
  }

  // ---- clock and ids ------------------------------------------------------------------------------------------
  Stamp tick() {
    const std::int64_t t = now();
    if (t > hlc_wall) {
      hlc_wall = t;
      hlc_ctr = 0;
    } else {
      ++hlc_ctr;
    }
    return {hlc_wall, hlc_ctr, dev};
  }
  void observe(const Stamp& s) {
    if (s.wall > hlc_wall || (s.wall == hlc_wall && s.ctr > hlc_ctr)) {
      hlc_wall = s.wall;
      hlc_ctr = s.ctr;
    }
  }
  std::string new_eid() { return dev + ":" + hex_of(++eid_ctr); }

  // Emit a local register if the live value differs from what the merged state says.
  bool put(char kind, const std::string& eid, const std::string& field, const Val& v) {
    const Reg* r = st.find(kind, eid, field);
    if (r ? same(r->v, v) : v.empty()) return false;
    st.set_local(kind, eid, field, tick(), v);
    ++changes;
    return true;
  }

  fs::path root_dir() const { return fs::path(cfg.folder) / "DeckHandSync" / "v1"; }
  fs::path own_dir() const { return root_dir() / dev; }

  // ---- local track identity -------------------------------------------------------------------------------------
  std::string compute_fp(const std::string& path) {
    return cfg.fingerprint ? cfg.fingerprint(path) : SyncEngine::fingerprint_file(path);
  }
  std::unordered_map<std::int64_t, TC> load_track_cache() {
    std::unordered_map<std::int64_t, TC> m;
    Stmt s(db, "SELECT track_id, fp, path, size, mtime, missing FROM sync_track");
    while (s.step()) m[s.i64(0)] = {s.text(1), s.text(2), s.i64(3), s.i64(4), s.i64(5) != 0};
    return m;
  }
  void save_track_cache(std::int64_t id, const TC& c) {
    Stmt s(db, "INSERT OR REPLACE INTO sync_track(track_id, fp, path, size, mtime, missing) VALUES (?1,?2,?3,?4,?5,?6)");
    s.bind(1, id).bind(2, c.fp).bind(3, c.path).bind(4, c.size).bind(5, c.mtime).bind(6, static_cast<std::int64_t>(c.missing));
    s.run();
  }
  static std::int64_t mtime_of(const std::string& path, std::int64_t& size) {
    std::error_code ec;
    size = static_cast<std::int64_t>(fs::file_size(fs::path(path), ec));
    if (ec) return -1;
    const auto t = fs::last_write_time(fs::path(path), ec);
    return ec ? -1 : static_cast<std::int64_t>(t.time_since_epoch().count());
  }
  struct Maps {
    std::unordered_map<std::string, std::int64_t> fp2id;  // canonical local row per fingerprint
    std::unordered_map<std::int64_t, std::string> id2fp;
  };
  Maps track_maps() {
    Maps m;
    Stmt s(db, "SELECT s.fp, s.track_id, s.missing FROM sync_track s JOIN tracks t ON t.id = s.track_id ORDER BY s.missing, s.track_id");
    while (s.step()) {
      m.id2fp[s.i64(1)] = s.text(0);
      m.fp2id.emplace(s.text(0), s.i64(1));  // first (non-missing, lowest id) wins
    }
    return m;
  }
  static std::string placeholder_rel(const std::string& fp, const std::string& name) {
    std::string n = name.empty() ? "track" : name;
    for (char& c : n)
      if (c == '/' || c == '\\' || c == ':') c = '_';
    return ".deckhand-sync-missing/" + fp + "/" + n;
  }

  // ---- capture ----------------------------------------------------------------------------------------------------
  struct LiveTrack {
    std::int64_t id = 0;
    std::string path, fp;
    bool missing = false;
    std::vector<Val> vals;
  };

  void relink_by_hints() {
    if (!cfg.trust_path_hints) return;
    std::vector<std::pair<std::int64_t, std::string>> missing;
    {
      Stmt s(db, "SELECT s.track_id, s.fp FROM sync_track s JOIN tracks t ON t.id = s.track_id WHERE s.missing = 1");
      while (s.step()) missing.emplace_back(s.i64(0), s.text(1));
    }
    for (const auto& [id, fp] : missing) {
      const std::string hint = st.text('T', fp, "hint");
      std::error_code ec;
      if (hint.empty() || !fs::is_regular_file(fs::path(hint), ec)) continue;
      if (compute_fp(hint) == fp) link_track(id, fp, hint);
    }
  }
  bool link_track(std::int64_t id, const std::string& fp, const std::string& path) {
    {
      Stmt u(db, "UPDATE OR IGNORE tracks SET root_id = ?1, rel_path = ?2 WHERE id = ?3");
      u.bind(1, kSystemRoot).bind(2, rel_of(path)).bind(3, id);
      u.run();
    }
    Stmt chk(db, "SELECT 1 FROM tracks WHERE id = ?1 AND root_id = ?2 AND rel_path = ?3");
    chk.bind(1, id).bind(2, kSystemRoot).bind(3, rel_of(path));
    if (!chk.step()) return false;  // another row already owns that path
    TC c;
    c.fp = fp;
    c.path = path;
    c.mtime = mtime_of(path, c.size);
    save_track_cache(id, c);
    return true;
  }

  void capture_tracks(std::unordered_map<std::string, std::int64_t>& fp2id_out) {
    db.exec("DELETE FROM sync_track WHERE track_id NOT IN (SELECT id FROM tracks)");
    auto cache = load_track_cache();
    std::vector<LiveTrack> live;
    {
      std::string sql = "SELECT t.id, r.last_seen_path, t.rel_path";
      for (const auto& f : kTrackFields) sql += std::string(", t.") + f.name;
      sql += " FROM tracks t JOIN library_roots r ON r.id = t.root_id ORDER BY t.id";
      Stmt s(db, sql.c_str());
      while (s.step()) {
        LiveTrack t;
        t.id = s.i64(0);
        t.path = join_path(s.text(1), s.text(2));
        for (int k = 0; k < kNumTrackFields; ++k) t.vals.push_back(read_val(s, 3 + k, kTrackFields[k].type));
        live.push_back(std::move(t));
      }
    }
    // Identity.
    for (auto& t : live) {
      const auto it = cache.find(t.id);
      const bool have = it != cache.end();
      const bool same_path = have && it->second.path == t.path;
      if (same_path && it->second.missing) {
        t.fp = it->second.fp;
        t.missing = true;
        continue;
      }
      std::error_code ec;
      const bool exists = fs::is_regular_file(fs::path(t.path), ec);
      if (same_path && exists) {
        std::int64_t size = 0;
        const std::int64_t mt = mtime_of(t.path, size);
        if (size == it->second.size && mt == it->second.mtime) {
          t.fp = it->second.fp;
          continue;
        }
      }
      std::string fp;
      if (exists || cfg.fingerprint) fp = compute_fp(t.path);
      if (fp.empty() && have) fp = it->second.fp;  // offline drive: keep the identity we already know
      if (fp.empty()) continue;                      // never seen readable: not synced yet
      TC c;
      c.fp = fp;
      c.path = t.path;
      c.mtime = exists ? mtime_of(t.path, c.size) : 0;
      if (!exists && have && it->second.fp == fp) {  // keep the old size/mtime so a later stat still matches
        c.size = it->second.size;
        c.mtime = it->second.mtime;
      }
      save_track_cache(t.id, c);
      t.fp = fp;
    }
    // Group by fingerprint: the best row is canonical, the others are aliases of it.
    std::unordered_map<std::string, std::size_t> canon;
    for (std::size_t i = 0; i < live.size(); ++i) {
      if (live[i].fp.empty()) continue;
      const auto it = canon.find(live[i].fp);
      if (it == canon.end()) {
        canon[live[i].fp] = i;
      } else if (live[it->second].missing && !live[i].missing) {
        canon[live[i].fp] = i;
      }
    }
    // A "missing" placeholder next to a real row of the same file is redundant: fold it into the real one.
    for (auto& t : live) {
      if (t.fp.empty() || !t.missing) continue;
      const auto& c = live[canon[t.fp]];
      if (c.id == t.id || c.missing) continue;
      Stmt u(db, "UPDATE playlist_tracks SET track_id = ?1 WHERE track_id = ?2");
      u.bind(1, c.id).bind(2, t.id);
      u.run();
      Stmt d(db, "DELETE FROM tracks WHERE id = ?1");
      d.bind(1, t.id);
      d.run();
      Stmt d2(db, "DELETE FROM sync_track WHERE track_id = ?1");
      d2.bind(1, t.id);
      d2.run();
      t.fp.clear();
    }
    // Beat grids and cues of the canonical rows.
    std::unordered_map<std::int64_t, std::string> grids;
    {
      Stmt s(db, "SELECT track_id, first_beat_ms, bpm, bar_beat FROM beatgrids ORDER BY track_id, position");
      std::unordered_map<std::int64_t, std::vector<BeatSection>> raw;
      while (s.step()) raw[s.i64(0)].push_back({s.f64(1), s.f64(2), static_cast<int>(s.i64(3))});
      for (auto& [id, g] : raw) grids[id] = encode_grid(g);
    }
    struct LiveCue {
      std::string key;
      std::vector<Val> vals;  // kind, start_ms, end_ms, label, color
    };
    std::unordered_map<std::int64_t, std::vector<LiveCue>> cues;
    {
      Stmt s(db, "SELECT track_id, kind, slot, start_ms, end_ms, label, color FROM cues ORDER BY track_id, id");
      while (s.step()) {
        LiveCue c;
        const std::string kind = s.text(1);
        if (!s.is_null(2)) {
          c.key = "s" + std::to_string(s.i64(2));
        } else {
          char b[48];
          std::snprintf(b, sizeof b, "@%.3f", s.f64(3));
          c.key = std::string(kind == "loop" ? "l" : "m") + b;
        }
        c.vals = {Val::text(kind), Val::real(s.f64(3)), read_val(s, 4, Val::Real), read_val(s, 5, Val::Text),
                  read_val(s, 6, Val::Int)};
        cues[s.i64(0)].push_back(std::move(c));
      }
    }
    static const char* const kCueFields[] = {"kind", "start_ms", "end_ms", "label", "color"};

    std::unordered_set<std::string> live_fps;
    for (const auto& t : live) {
      if (t.fp.empty()) continue;
      fp2id_out.emplace(t.fp, t.id);
      if (canon[t.fp] != static_cast<std::size_t>(&t - live.data())) continue;
      live_fps.insert(t.fp);
      const std::string& fp = t.fp;
      if (!st.alive('T', fp)) put('T', fp, "_del", Val::integer(0));
      for (int k = 0; k < kNumTrackFields; ++k) put('T', fp, kTrackFields[k].name, t.vals[static_cast<std::size_t>(k)]);
      if (!st.find('T', fp, "hint") && !t.missing) {
        put('T', fp, "file_name", Val::text(base_name(t.path)));
        put('T', fp, "hint", Val::text(t.path));
      }
      const auto g = grids.find(t.id);
      put('T', fp, "grid", Val::text(g == grids.end() ? std::string() : g->second));
      // Cues.
      std::unordered_set<std::string> seen;
      const auto ci = cues.find(t.id);
      if (ci != cues.end()) {
        for (const auto& c : ci->second) {
          if (!seen.insert(c.key).second) continue;
          const std::string eid = fp + "#" + c.key;
          if (st.entity('C', eid) && !st.alive('C', eid)) put('C', eid, "_del", Val::integer(0));
          for (int k = 0; k < 5; ++k) put('C', eid, kCueFields[k], c.vals[static_cast<std::size_t>(k)]);
        }
      }
      const std::string prefix = fp + "#";
      const auto& cs = st.ent[State::idx('C')];
      for (auto it = cs.lower_bound(prefix); it != cs.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        if (!seen.count(it->first.substr(prefix.size())) && st.alive('C', it->first)) put('C', it->first, "_del", Val::integer(1));
      }
    }
    // Tracks that were alive in the merged state but are gone from this library.
    for (const auto& [fp, e] : st.ent[State::idx('T')]) {
      (void)e;
      if (!live_fps.count(fp) && st.alive('T', fp)) put('T', fp, "_del", Val::integer(1));
    }
  }

  void capture_playlists(const Maps& maps) {
    struct Row {
      std::int64_t id, parent;
      std::string name;
      bool folder;
      std::int64_t pos;
    };
    std::vector<Row> rows;
    {
      Stmt s(db, "SELECT id, coalesce(parent_id, 0), name, is_folder, position FROM playlists ORDER BY id");
      while (s.step()) rows.push_back({s.i64(0), s.i64(1), s.text(2), s.i64(3) != 0, s.i64(4)});
    }
    IdMap pm = load_map('P');
    std::unordered_set<std::int64_t> live_ids;
    for (const auto& r : rows) live_ids.insert(r.id);
    std::vector<std::int64_t> gone;
    for (const auto& [id, eid] : pm.l2e)
      if (!live_ids.count(id)) gone.push_back(id);
    for (const auto id : gone) {
      const std::string eid = pm.l2e[id];
      if (st.alive('P', eid)) put('P', eid, "_del", Val::integer(1));
      map_drop(pm, 'P', id);
    }
    for (const auto& r : rows)
      if (!pm.l2e.count(r.id)) map_put(pm, 'P', r.id, new_eid());
    for (const auto& r : rows) {
      const std::string& eid = pm.l2e[r.id];
      if (!st.alive('P', eid)) put('P', eid, "_del", Val::integer(0));
      put('P', eid, "name", Val::text(r.name));
      put('P', eid, "is_folder", Val::integer(r.folder ? 1 : 0));
      put('P', eid, "parent", Val::text(r.parent ? pm.l2e[r.parent] : std::string()));
    }
    // Order within each parent.
    Tree tree = build_tree(st);
    std::map<std::int64_t, std::vector<const Row*>> groups;
    for (const auto& r : rows) groups[r.parent].push_back(&r);
    for (auto& [parent, list] : groups) {
      std::stable_sort(list.begin(), list.end(), [](const Row* a, const Row* b) { return a->pos < b->pos; });
      const std::string peid = parent ? pm.l2e[parent] : std::string();
      std::unordered_map<std::string, int> rank;
      const auto ki = tree.kids.find(peid);
      if (ki != tree.kids.end())
        for (std::size_t i = 0; i < ki->second.size(); ++i) rank[ki->second[i]] = static_cast<int>(i);
      std::vector<int> ranks;
      for (const Row* r : list) {
        const auto it = rank.find(pm.l2e[r->id]);
        ranks.push_back(it == rank.end() ? -1 : it->second);
      }
      const auto keep = lis_keep(ranks);
      std::vector<std::string> keys;
      for (const Row* r : list) keys.push_back(st.text('P', pm.l2e[r->id], "pos"));
      std::vector<std::string> ids;
      for (const Row* r : list) ids.push_back(pm.l2e[r->id]);
      const auto fresh = plan_keys(keep, keys, ids);
      for (std::size_t i = 0; i < list.size(); ++i)
        if (!fresh[i].empty()) put('P', pm.l2e[list[i]->id], "pos", Val::text(fresh[i]));
    }
    capture_entries(pm, maps);
  }

  void capture_entries(IdMap& pm, const Maps& maps) {
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> live;
    {
      Stmt s(db, "SELECT playlist_id, track_id FROM playlist_tracks ORDER BY playlist_id, position");
      while (s.step()) live[s.i64(0)].push_back(s.i64(1));
    }
    std::unordered_map<std::string, std::vector<std::string>> by_pl;
    for (const auto& [eid, e] : st.ent[State::idx('E')]) {
      (void)e;
      const std::string pl = st.text('E', eid, "pl");
      if (!pl.empty()) by_pl[pl].push_back(eid);
    }
    Tree tree = build_tree(st);
    for (const auto& [id, peid] : pm.l2e) {
      if (tree.dead.count(peid)) continue;
      std::vector<std::string> live_fp;
      for (const auto tid : live[id]) {
        const auto it = maps.id2fp.find(tid);
        if (it != maps.id2fp.end()) live_fp.push_back(it->second);
      }
      struct Sh {
        std::string eid, fp, pos;
      };
      std::vector<Sh> sh;
      for (const auto& eid : by_pl[peid]) {
        if (!st.alive('E', eid)) continue;
        const std::string fp = st.text('E', eid, "trk");
        if (!st.entity('T', fp)) continue;
        sh.push_back({eid, fp, st.text('E', eid, "pos")});
      }
      std::sort(sh.begin(), sh.end(), [](const Sh& a, const Sh& b) { return a.pos != b.pos ? a.pos < b.pos : a.eid < b.eid; });
      std::vector<std::string> shadow_fp;
      for (const auto& x : sh) shadow_fp.push_back(x.fp);
      const auto pairs = lcs_pairs(live_fp, shadow_fp);
      std::vector<int> l2s(live_fp.size(), -1);
      std::vector<bool> used(sh.size(), false), anchor(live_fp.size(), false);
      for (const auto& [i, j] : pairs) {
        l2s[static_cast<std::size_t>(i)] = j;
        used[static_cast<std::size_t>(j)] = true;
        anchor[static_cast<std::size_t>(i)] = true;
      }
      // Leftovers with the same fingerprint are moves of the same entry.
      std::unordered_map<std::string, std::vector<int>> spare;
      for (std::size_t j = 0; j < sh.size(); ++j)
        if (!used[j]) spare[sh[j].fp].push_back(static_cast<int>(j));
      std::unordered_map<std::string, std::size_t> taken;
      for (std::size_t i = 0; i < live_fp.size(); ++i) {
        if (l2s[i] >= 0) continue;
        const auto it = spare.find(live_fp[i]);
        if (it == spare.end() || taken[live_fp[i]] >= it->second.size()) continue;
        const int j = it->second[taken[live_fp[i]]++];
        l2s[i] = j;
        used[static_cast<std::size_t>(j)] = true;
      }
      std::vector<std::string> eids(live_fp.size()), keys(live_fp.size());
      for (std::size_t i = 0; i < live_fp.size(); ++i) {
        if (l2s[i] >= 0) {
          eids[i] = sh[static_cast<std::size_t>(l2s[i])].eid;
          keys[i] = sh[static_cast<std::size_t>(l2s[i])].pos;
        } else {
          eids[i] = new_eid();
        }
      }
      const auto fresh = plan_keys(anchor, keys, eids);
      for (std::size_t i = 0; i < live_fp.size(); ++i) {
        if (l2s[i] < 0) {
          put('E', eids[i], "pl", Val::text(peid));
          put('E', eids[i], "trk", Val::text(live_fp[i]));
        }
        if (!fresh[i].empty()) put('E', eids[i], "pos", Val::text(fresh[i]));
      }
      for (std::size_t j = 0; j < sh.size(); ++j)
        if (!used[j]) put('E', sh[j].eid, "_del", Val::integer(1));
    }
  }

  void capture_history(const Maps& maps) {
    IdMap sm = load_map('S'), hm = load_map('H');
    {
      Stmt s(db, "SELECT id, started_at FROM history_sessions ORDER BY id");
      while (s.step()) {
        if (!sm.l2e.count(s.i64(0))) map_put(sm, 'S', s.i64(0), new_eid());
        put('S', sm.l2e[s.i64(0)], "started", Val::integer(s.i64(1)));
      }
    }
    Stmt s(db, "SELECT id, session_id, coalesce(track_id, 0), deck, started_at, played_ms FROM history ORDER BY id");
    while (s.step()) {
      const std::int64_t id = s.i64(0);
      const bool fresh = !hm.l2e.count(id);
      if (fresh) map_put(hm, 'H', id, new_eid());
      const std::string& eid = hm.l2e[id];
      if (fresh) {
        const auto se = sm.l2e.find(s.i64(1));
        const auto fp = maps.id2fp.find(s.i64(2));
        put('H', eid, "sess", Val::text(se == sm.l2e.end() ? std::string() : se->second));
        put('H', eid, "trk", Val::text(fp == maps.id2fp.end() ? std::string() : fp->second));
        put('H', eid, "deck", Val::text(s.text(3)));
        put('H', eid, "started", Val::integer(s.i64(4)));
      }
      put('H', eid, "played", Val::integer(s.i64(5)));
    }
  }

  void capture_settings() {
    Stmt s(db, "SELECT key, value FROM settings");
    while (s.step()) {
      const std::string key = s.text(0);
      if (setting_allowed(key, cfg.settings_allow)) put('G', key, "v", Val::text(s.text(1)));
    }
  }

  void do_capture() {
    relink_by_hints();
    std::unordered_map<std::string, std::int64_t> fp2id;
    capture_tracks(fp2id);
    const Maps maps = track_maps();
    capture_playlists(maps);
    capture_history(maps);
    capture_settings();
  }

  SyncResult run_capture(SyncResult& r) {
    changes = 0;
    {
      Transaction tx(db);
      do_capture();
      flush_regs();
      save_meta();
      fault("capture.commit");
      tx.commit();
    }
    r.changes_captured += changes;
    return r;
  }

  // ---- write ----------------------------------------------------------------------------------------------------
  std::uint64_t scan_max_seq(const fs::path& dir) {
    std::uint64_t mx = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      std::uint64_t seq;
      bool snap;
      if (parse_name(it->path().filename().string(), seq, snap)) mx = std::max(mx, seq);
    }
    return mx;
  }

  RegRec rec_of(const Key& k) const {
    const auto& [kind, eid, field] = k;
    const Reg* r = st.find(kind, eid, field);
    RegRec rec;
    rec.kind = kind;
    rec.eid = eid;
    rec.field = field;
    rec.st = r->st;
    rec.v = r->v;
    return rec;
  }

  void write_pending(SyncResult& r) {
    const fs::path dir = own_dir();
    std::error_code ec;
    const bool first = !fs::exists(dir, ec);
    if (st.pending.empty() && !first) return;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error("cannot create " + dir.string() + ": " + ec.message());
    next_seq = std::max(next_seq, scan_max_seq(dir) + 1);
    auto it = st.pending.begin();
    do {
      FileHeader h;
      h.device_id = dev;
      h.device_name = dev_name;
      h.seq = next_seq;
      h.created_ms = now();
      h.kind = kFileLog;
      FileWriter w(h);
      std::vector<Key> keys;
      while (it != st.pending.end() && w.size() < cfg.max_log_bytes) {
        w.add(rec_of(*it));
        keys.push_back(*it);
        ++it;
      }
      std::string err;
      if (!write_file_atomic((dir / file_name_for(next_seq, false)).string(), w.finish(), err)) throw std::runtime_error(err);
      ++next_seq;
      ++r.files_written;
      r.records_written += static_cast<int>(keys.size());
      fault("write.file");
      Transaction tx(db);
      for (const auto& k : keys) {
        const auto& [kind, eid, field] = k;
        st.ent[State::idx(kind)][eid].f[field].pending = false;
        st.pending.erase(k);
        st.dirty.insert(k);
      }
      flush_regs();
      save_meta();
      tx.commit();
    } while (it != st.pending.end());
  }

  // ---- read ------------------------------------------------------------------------------------------------------
  struct Incoming {
    std::string dev, name;
    std::size_t size = 0;
    ParsedFile pf;
  };

  void read_remote(std::vector<Incoming>& out, std::unordered_set<std::string>& present, SyncResult& r) {
    std::error_code ec;
    const fs::path root = root_dir();
    if (!fs::is_directory(root, ec)) return;
    std::map<std::pair<std::string, std::string>, std::pair<std::size_t, int>> known;
    {
      Stmt s(db, "SELECT dev, name, size, status FROM sync_files");
      while (s.step()) known[{s.text(0), s.text(1)}] = {static_cast<std::size_t>(s.i64(2)), static_cast<int>(s.i64(3))};
    }
    for (fs::directory_iterator d(root, ec), dend; !ec && d != dend; d.increment(ec)) {
      std::error_code e2;
      if (!d->is_directory(e2)) continue;
      const std::string device = d->path().filename().string();
      if (device == dev || device.empty() || device[0] == '.') continue;
      struct F {
        std::uint64_t seq;
        bool snap;
        std::string name;
        std::size_t size;
      };
      std::vector<F> files;
      for (fs::directory_iterator f(d->path(), e2), fend; !e2 && f != fend; f.increment(e2)) {
        std::uint64_t seq;
        bool snap;
        const std::string name = f->path().filename().string();
        if (!parse_name(name, seq, snap)) continue;
        std::error_code e3;
        const auto sz = fs::file_size(f->path(), e3);
        if (e3) continue;
        files.push_back({seq, snap, name, static_cast<std::size_t>(sz)});
        present.insert(device + "/" + name);
      }
      std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.seq < b.seq; });
      std::map<std::string, Incoming> parsed;
      const auto load = [&](const F& f) -> Incoming& {
        auto it = parsed.find(f.name);
        if (it != parsed.end()) return it->second;
        Incoming in;
        in.dev = device;
        in.name = f.name;
        in.size = f.size;
        std::string bytes;
        if (read_file((d->path() / f.name).string(), bytes)) {
          in.pf = parse_file(bytes);
          in.size = bytes.size();
        } else {
          in.pf.problem = "cannot read file";
        }
        ++r.files_read;
        return parsed.emplace(f.name, std::move(in)).first->second;
      };
      const auto done = [&](const F& f) {
        const auto k = known.find({device, f.name});
        return k != known.end() && k->second.first == f.size;
      };
      std::uint64_t cover = 0;  // logs older than the newest complete snapshot are covered by it
      for (auto it = files.rbegin(); it != files.rend(); ++it) {
        if (!it->snap) continue;
        const auto k = known.find({device, it->name});
        if (k != known.end() && k->second.first == it->size && k->second.second == 1) {
          cover = it->seq;
          break;
        }
        if (load(*it).pf.complete) {
          cover = it->seq;
          break;
        }
      }
      for (const auto& f : files) {
        if (!f.snap && f.seq < cover) continue;
        if (done(f)) continue;
        Incoming& in = load(f);
        if (in.pf.header_ok && in.pf.header.device_id != device) {
          r.warnings.push_back("ignored " + device + "/" + f.name + ": it claims to belong to another device");
          in.pf.regs.clear();
          in.pf.header_ok = false;
        }
        out.push_back(in);
      }
    }
  }

  // ---- apply ------------------------------------------------------------------------------------------------------
  void apply(std::vector<Incoming>& in, const std::unordered_set<std::string>& present, SyncResult& r) {
    Transaction tx(db);
    std::unordered_set<std::string> devices_seen;
    for (auto& f : in) {
      for (const auto& rec : f.pf.regs) {
        if (State::idx(rec.kind) < 0 || rec.field.empty()) continue;
        if (rec.kind == 'G' && !setting_allowed(rec.eid, cfg.settings_allow)) continue;
        observe(rec.st);
        if (rec.st.wall > now() + 24ll * 3600 * 1000 && devices_seen.insert(rec.st.dev).second)
          r.warnings.push_back("device " + rec.st.dev + " has a clock more than a day ahead");
        if (st.merge(rec.kind, rec.eid, rec.field, rec.st, rec.v)) ++r.records_applied;
      }
      if (f.pf.unsupported) r.warnings.push_back(f.dev + "/" + f.name + ": " + f.pf.problem);
      else if (!f.pf.complete) {
        ++r.files_corrupt;
        r.warnings.push_back(f.dev + "/" + f.name + ": " + f.pf.problem + "; " + std::to_string(f.pf.regs.size()) +
                             " valid records used");
      }
      if (f.pf.encrypted_records)
        r.warnings.push_back(f.dev + "/" + f.name + ": " + std::to_string(f.pf.encrypted_records) +
                             " encrypted records skipped (encryption is not supported by this version)");
      Stmt s(db, "INSERT OR REPLACE INTO sync_files(dev, name, size, status) VALUES (?1, ?2, ?3, ?4)");
      s.bind(1, f.dev).bind(2, f.name).bind(3, static_cast<std::int64_t>(f.size)).bind(4, f.pf.complete ? 1 : f.pf.unsupported ? 3 : 2);
      s.run();
    }
    {  // forget files that were compacted away
      std::vector<std::pair<std::string, std::string>> stale;
      Stmt s(db, "SELECT dev, name FROM sync_files");
      while (s.step())
        if (!present.count(s.text(0) + "/" + s.text(1))) stale.emplace_back(s.text(0), s.text(1));
      Stmt d(db, "DELETE FROM sync_files WHERE dev = ?1 AND name = ?2");
      for (const auto& [a, b] : stale) {
        d.bind(1, a).bind(2, b);
        d.run();
      }
    }
    materialize();
    flush_regs();
    save_meta();
    fault("apply.commit");
    tx.commit();
  }

  // ---- materialize: merged state -> live tables ---------------------------------------------------------------------
  void materialize() {
    std::set<std::string> dirty_t, dirty_c, dirty_e_pl;
    bool dirty_p = false, dirty_hs = false;
    std::set<std::string> dirty_g;
    for (const auto& [kind, eid, field] : st.dirty) {
      (void)field;
      switch (kind) {
        case 'T': dirty_t.insert(eid); break;
        case 'C': dirty_c.insert(eid); break;
        case 'P': dirty_p = true; break;
        case 'E': dirty_e_pl.insert(st.text('E', eid, "pl")); break;
        case 'H':
        case 'S': dirty_hs = true; break;
        case 'G': dirty_g.insert(eid); break;
      }
    }
    if (!dirty_t.empty()) materialize_tracks(dirty_t);
    Maps maps = track_maps();
    for (const auto& eid : dirty_c) materialize_cue(eid, maps);
    std::set<std::int64_t> fresh_playlists;
    if (dirty_p) materialize_playlists(fresh_playlists);
    if (dirty_p || !dirty_e_pl.empty() || !dirty_t.empty()) materialize_entries(dirty_e_pl, dirty_t, fresh_playlists, maps);
    if (dirty_hs) materialize_history(maps);
    for (const auto& key : dirty_g) {
      const Reg* r = st.find('G', key, "v");
      if (r && r->v.t == Val::Text) lib.set_setting(key, r->v.s);
    }
  }

  void materialize_tracks(const std::set<std::string>& fps) {
    Maps maps = track_maps();
    std::unordered_map<std::string, std::vector<std::int64_t>> all_ids;
    {
      Stmt s(db, "SELECT fp, track_id FROM sync_track ORDER BY track_id");
      while (s.step()) all_ids[s.text(0)].push_back(s.i64(1));
    }
    for (const auto& fp : fps) {
      const Entity* e = st.entity('T', fp);
      if (!e) continue;
      auto& ids = all_ids[fp];
      if (!st.alive('T', fp)) {
        for (const auto id : ids) {
          for (const char* sql : {"DELETE FROM tracks WHERE id = ?1", "DELETE FROM sync_track WHERE track_id = ?1",
                                  "DELETE FROM import_map WHERE entity = 'track' AND local_id = ?1"}) {
            Stmt d(db, sql);
            d.bind(1, id);
            d.run();
          }
        }
        continue;
      }
      std::int64_t id = 0;
      const auto canon = maps.fp2id.find(fp);
      if (canon != maps.fp2id.end()) id = canon->second;
      const auto field = [&](const char* name) -> const Val* {
        const auto it = e->f.find(name);
        return it == e->f.end() ? nullptr : &it->second.v;
      };
      if (!id) id = create_track_row(fp, e);
      if (!id) continue;
      std::string sql = "UPDATE tracks SET ";
      std::vector<const Val*> vals;
      for (const auto& f : kTrackFields) {
        const Val* v = field(f.name);
        if (!v) continue;
        sql += std::string(vals.empty() ? "" : ", ") + f.name + " = ?" + std::to_string(vals.size() + 1);
        vals.push_back(v);
      }
      if (!vals.empty()) {
        sql += " WHERE id = ?" + std::to_string(vals.size() + 1);
        Stmt u(db, sql.c_str());
        for (std::size_t i = 0; i < vals.size(); ++i) bind_opt(u, static_cast<int>(i + 1), *vals[i]);
        u.bind(static_cast<int>(vals.size() + 1), id);
        u.run();
      }
      if (const Val* g = field("grid")) {
        const auto want = decode_grid(g->t == Val::Text ? g->s : std::string());
        if (encode_grid(lib.beatgrid(id)) != encode_grid(want)) lib.replace_beatgrid(id, want);
      }
    }
  }

  std::int64_t create_track_row(const std::string& fp, const Entity* e) {
    const auto text = [&](const char* k) {
      const auto it = e->f.find(k);
      return it != e->f.end() && it->second.v.t == Val::Text ? it->second.v.s : std::string();
    };
    std::string hint = cfg.trust_path_hints ? text("hint") : std::string();
    std::string rel = placeholder_rel(fp, text("file_name"));
    bool linked = false;
    std::error_code ec;
    if (!hint.empty() && fs::is_regular_file(fs::path(hint), ec) && compute_fp(hint) == fp) {
      Stmt chk(db, "SELECT 1 FROM tracks WHERE root_id = ?1 AND rel_path = ?2");
      chk.bind(1, kSystemRoot).bind(2, rel_of(hint));
      if (!chk.step()) {
        rel = rel_of(hint);
        linked = true;
      }
    }
    Stmt i(db, "INSERT INTO tracks(root_id, rel_path, date_added) VALUES (?1, ?2, 0)");
    i.bind(1, kSystemRoot).bind(2, rel);
    i.run();
    const std::int64_t id = db.last_insert_id();
    TC c;
    c.fp = fp;
    c.path = "/" + rel;
    c.missing = !linked;
    if (linked) c.mtime = mtime_of(c.path, c.size);
    save_track_cache(id, c);
    return id;
  }

  void materialize_cue(const std::string& eid, const Maps& maps) {
    const auto hash = eid.find('#');
    if (hash == std::string::npos) return;
    const std::string fp = eid.substr(0, hash), key = eid.substr(hash + 1);
    const auto t = maps.fp2id.find(fp);
    if (t == maps.fp2id.end() || !st.alive('T', fp) || key.empty()) return;
    const std::int64_t tid = t->second;
    const Entity* e = st.entity('C', eid);
    if (!e) return;
    const auto get = [&](const char* k) {
      const auto it = e->f.find(k);
      return it == e->f.end() ? Val::null() : it->second.v;
    };
    const Val kind = get("kind"), start = get("start_ms"), end = get("end_ms"), label = get("label"), color = get("color");
    std::int64_t row = 0;
    {
      std::string sql = "SELECT id FROM cues WHERE track_id = ?1 AND ";
      if (key[0] == 's') sql += "slot = ?2 AND kind IN ('hot', 'loop')";
      else sql += "slot IS NULL AND kind = ?2 AND abs(start_ms - ?3) < 0.0006";
      Stmt s(db, sql.c_str());
      s.bind(1, tid);
      if (key[0] == 's') {
        s.bind(2, static_cast<std::int64_t>(std::strtoll(key.c_str() + 1, nullptr, 10)));
      } else {
        s.bind(2, key[0] == 'l' ? "loop" : "memory").bind(3, std::strtod(key.c_str() + 2, nullptr));
      }
      if (s.step()) row = s.i64(0);
    }
    if (!st.alive('C', eid)) {
      if (row) {
        Stmt d(db, "DELETE FROM cues WHERE id = ?1");
        d.bind(1, row);
        d.run();
      }
      return;
    }
    const std::string k = kind.t == Val::Text ? kind.s : key[0] == 'l' ? "loop" : key[0] == 's' ? "hot" : "memory";
    if (row) {
      Stmt u(db, "UPDATE cues SET kind = ?1, start_ms = ?2, end_ms = ?3, label = ?4, color = ?5 WHERE id = ?6");
      u.bind(1, k);
      bind_opt(u, 2, start.t == Val::Null ? Val::real(0) : start);
      bind_opt(u, 3, end);
      bind_opt(u, 4, label);
      bind_opt(u, 5, color);
      u.bind(6, row);
      u.run();
    } else {
      Stmt i(db, "INSERT INTO cues(track_id, kind, slot, start_ms, end_ms, label, color) VALUES (?1,?2,?3,?4,?5,?6,?7)");
      i.bind(1, tid).bind(2, k);
      if (key[0] == 's') i.bind(3, static_cast<std::int64_t>(std::strtoll(key.c_str() + 1, nullptr, 10)));
      else i.bind_null(3);
      bind_opt(i, 4, start.t == Val::Null ? Val::real(std::strtod(key.c_str() + 2, nullptr)) : start);
      bind_opt(i, 5, end);
      bind_opt(i, 6, label);
      bind_opt(i, 7, color);
      i.run();
    }
  }

  void materialize_playlists(std::set<std::int64_t>& created) {
    const Tree tree = build_tree(st);
    IdMap pm = load_map('P');
    struct Live {
      std::int64_t parent, pos;
      std::string name;
      bool folder;
    };
    std::unordered_map<std::int64_t, Live> live;
    {
      Stmt s(db, "SELECT id, coalesce(parent_id, 0), name, is_folder, position FROM playlists");
      while (s.step()) live[s.i64(0)] = {s.i64(1), s.i64(4), s.text(2), s.i64(3) != 0};
    }
    for (const auto& eid : tree.dead) {  // deleted here: remove the live row (children and entries cascade)
      const auto it = pm.e2l.find(eid);
      if (it == pm.e2l.end()) continue;
      Stmt d(db, "DELETE FROM playlists WHERE id = ?1");
      d.bind(1, it->second);
      d.run();
      live.erase(it->second);
    }
    for (const auto& eid : tree.order) {
      const std::string peid = tree.parent.at(eid);
      std::int64_t parent = 0;
      if (!peid.empty()) parent = pm.e2l.count(peid) ? pm.e2l[peid] : 0;
      const std::string name = st.text('P', eid, "name");
      const Reg* fr = st.find('P', eid, "is_folder");
      const bool folder = fr && fr->v.t == Val::Int && fr->v.i != 0;
      const auto& sibs = tree.kids.at(peid);
      const std::int64_t pos = std::find(sibs.begin(), sibs.end(), eid) - sibs.begin();
      auto e2l = pm.e2l.find(eid);
      if (e2l == pm.e2l.end() || !live.count(e2l->second)) {
        Stmt i(db, "INSERT INTO playlists(parent_id, name, is_folder, position) VALUES (?1, ?2, ?3, ?4)");
        parent ? i.bind(1, parent) : i.bind_null(1);
        i.bind(2, name).bind(3, folder ? 1 : 0).bind(4, pos);
        i.run();
        const std::int64_t id = db.last_insert_id();
        map_put(pm, 'P', id, eid);
        live[id] = {parent, pos, name, folder};
        created.insert(id);
        continue;
      }
      const std::int64_t id = e2l->second;
      Live& l = live[id];
      if (l.parent != parent || l.name != name || l.folder != folder || l.pos != pos) {
        Stmt u(db, "UPDATE playlists SET parent_id = ?1, name = ?2, is_folder = ?3, position = ?4 WHERE id = ?5");
        parent ? u.bind(1, parent) : u.bind_null(1);
        u.bind(2, name).bind(3, folder ? 1 : 0).bind(4, pos).bind(5, id);
        u.run();
        l = {parent, pos, name, folder};
      }
    }
    db.exec("DELETE FROM sync_map WHERE kind = 'P' AND local_id NOT IN (SELECT id FROM playlists)");
  }

  void materialize_entries(const std::set<std::string>& dirty_pl, const std::set<std::string>& dirty_t,
                           const std::set<std::int64_t>& created, const Maps& maps) {
    const Tree tree = build_tree(st);
    IdMap pm = load_map('P');
    std::set<std::string> todo = dirty_pl;
    std::unordered_map<std::string, std::vector<std::string>> by_pl;
    for (const auto& [eid, e] : st.ent[State::idx('E')]) {
      (void)e;
      const std::string pl = st.text('E', eid, "pl");
      if (pl.empty()) continue;
      by_pl[pl].push_back(eid);
      if (!dirty_t.empty() && dirty_t.count(st.text('E', eid, "trk"))) todo.insert(pl);
    }
    for (const auto id : created)
      if (pm.l2e.count(id)) todo.insert(pm.l2e[id]);
    for (const auto& peid : todo) {
      const auto pl = pm.e2l.find(peid);
      if (pl == pm.e2l.end() || tree.dead.count(peid)) continue;
      struct Sh {
        std::string pos, eid;
        std::int64_t track;
      };
      std::vector<Sh> want;
      for (const auto& eid : by_pl[peid]) {
        if (!st.alive('E', eid)) continue;
        const std::string fp = st.text('E', eid, "trk");
        const auto t = maps.fp2id.find(fp);
        if (t == maps.fp2id.end() || !st.alive('T', fp)) continue;
        want.push_back({st.text('E', eid, "pos"), eid, t->second});
      }
      std::sort(want.begin(), want.end(), [](const Sh& a, const Sh& b) { return a.pos != b.pos ? a.pos < b.pos : a.eid < b.eid; });
      std::vector<std::int64_t> target, current = lib.playlist_track_ids(pl->second);
      for (const auto& w : want) target.push_back(w.track);
      for (const auto tid : current)  // rows that are not synced (no fingerprint yet) stay, at the end
        if (!maps.id2fp.count(tid)) target.push_back(tid);
      if (target != current) lib.set_playlist_tracks(pl->second, target);
    }
  }

  void materialize_history(const Maps& maps) {
    IdMap sm = load_map('S'), hm = load_map('H');
    for (const auto& [eid, e] : st.ent[State::idx('S')]) {
      (void)e;
      if (sm.e2l.count(eid)) continue;
      const Reg* r = st.find('S', eid, "started");
      Stmt i(db, "INSERT INTO history_sessions(started_at) VALUES (?1)");
      i.bind(1, r && r->v.t == Val::Int ? r->v.i : 0);
      i.run();
      map_put(sm, 'S', db.last_insert_id(), eid);
    }
    for (const auto& [eid, e] : st.ent[State::idx('H')]) {
      (void)e;
      const Reg* pr = st.find('H', eid, "played");
      const std::int64_t played = pr && pr->v.t == Val::Int ? pr->v.i : 0;
      const auto have = hm.e2l.find(eid);
      if (have != hm.e2l.end()) {
        Stmt u(db, "UPDATE history SET played_ms = ?1 WHERE id = ?2 AND played_ms != ?1");
        u.bind(1, played).bind(2, have->second);
        u.run();
        continue;
      }
      const auto sess = sm.e2l.find(st.text('H', eid, "sess"));
      if (sess == sm.e2l.end()) continue;  // session not received yet; retried when it arrives
      const auto trk = maps.fp2id.find(st.text('H', eid, "trk"));
      const Reg* started = st.find('H', eid, "started");
      Stmt i(db, "INSERT INTO history(session_id, track_id, deck, started_at, played_ms) VALUES (?1,?2,?3,?4,?5)");
      i.bind(1, sess->second);
      trk == maps.fp2id.end() ? i.bind_null(2) : i.bind(2, trk->second);
      i.bind(3, st.text('H', eid, "deck")).bind(4, started && started->v.t == Val::Int ? started->v.i : 0).bind(5, played);
      i.run();
      map_put(hm, 'H', db.last_insert_id(), eid);
    }
  }

  // ---- public operations ------------------------------------------------------------------------------------------
  bool check_folder(SyncResult& r) {
    std::error_code ec;
    if (cfg.folder.empty() || !fs::is_directory(fs::path(cfg.folder), ec)) {
      r.error = "sync folder is not available: " + cfg.folder;
      return false;
    }
    fs::create_directories(root_dir(), ec);
    if (ec) {
      r.error = "cannot create " + root_dir().string() + ": " + ec.message();
      return false;
    }
    return true;
  }

  template <typename Fn>
  SyncResult guarded(Fn fn, bool needs_folder = true) {
    SyncResult r;
    try {
      if (!init_error.empty()) {
        r.error = init_error;
      } else if (needs_folder && !check_folder(r)) {
      } else {
        fn(r);
        r.ok = true;
      }
    } catch (const Crash& c) {
      r.error = std::string("simulated crash at ") + c.stage;
    } catch (const std::exception& e) {
      r.error = e.what();
    } catch (...) {
      r.error = "unknown error";
    }
    if (!r.ok) {
      DH_LOG(Warn, "sync: %s", r.error.c_str());
      try {
        reload();
      } catch (...) {
      }
    }
    last_error = r.error;
    try {
      r.tracks_missing = count_missing();
    } catch (...) {
    }
    return r;
  }

  int count_missing() {
    Stmt s(db, "SELECT count(*) FROM sync_track s JOIN tracks t ON t.id = s.track_id WHERE s.missing = 1");
    return s.step() ? static_cast<int>(s.i64(0)) : 0;
  }

  SyncResult sync_once() {
    return guarded([&](SyncResult& r) {
      run_capture(r);
      fault("capture.done");
      write_pending(r);
      std::vector<Incoming> in;
      std::unordered_set<std::string> present;
      read_remote(in, present, r);
      last_sync = now();
      apply(in, present, r);
    });
  }

  SyncResult capture_only() {
    return guarded([&](SyncResult& r) { run_capture(r); }, false);
  }

  SyncResult compact() {
    return guarded([&](SyncResult& r) {
      run_capture(r);
      write_pending(r);
      const fs::path dir = own_dir();
      next_seq = std::max(next_seq, scan_max_seq(dir) + 1);
      FileHeader h;
      h.device_id = dev;
      h.device_name = dev_name;
      h.seq = next_seq;
      h.created_ms = now();
      h.kind = kFileSnapshot;
      FileWriter w(h);
      for (const auto& ents : st.ent)
        for (const auto& [eid, e] : ents)
          for (const auto& [field, reg] : e.f)
            if (reg.st.dev == dev) {
              RegRec rec;
              rec.kind = kKinds[&ents - st.ent];
              rec.eid = eid;
              rec.field = field;
              rec.st = reg.st;
              rec.v = reg.v;
              w.add(rec);
            }
      std::string err;
      const std::uint64_t snap_seq = next_seq;
      r.records_written += static_cast<int>(w.count());
      if (!write_file_atomic((dir / file_name_for(snap_seq, true)).string(), w.finish(), err)) throw std::runtime_error(err);
      ++next_seq;
      ++r.files_written;
      fault("compact.snapshot");
      Transaction tx(db);
      save_meta();
      tx.commit();
      std::error_code ec;
      for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::uint64_t seq;
        bool snap;
        if (parse_name(it->path().filename().string(), seq, snap) && seq < snap_seq) {
          std::error_code e2;
          fs::remove(it->path(), e2);
        }
      }
    });
  }

  int relink_missing(const std::vector<std::string>& dirs) {
    int linked = 0;
    try {
      if (!init_error.empty()) return 0;
      std::unordered_map<std::int64_t, std::unordered_map<std::string, std::int64_t>> by_size;  // size -> fp -> track
      {
        Stmt s(db, "SELECT s.track_id, s.fp FROM sync_track s JOIN tracks t ON t.id = s.track_id WHERE s.missing = 1");
        while (s.step()) {
          const std::string fp = s.text(1);
          const auto a = fp.find('-'), b = fp.find('-', a + 1);
          if (a == std::string::npos || b == std::string::npos) continue;
          by_size[static_cast<std::int64_t>(std::strtoull(fp.substr(a + 1, b - a - 1).c_str(), nullptr, 16))][fp] = s.i64(0);
        }
      }
      if (by_size.empty()) return 0;
      Transaction tx(db);
      for (const auto& d : dirs) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(fs::path(d), fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
          std::error_code e2;
          if (!it->is_regular_file(e2)) continue;
          const auto size = static_cast<std::int64_t>(it->file_size(e2));
          const auto cand = by_size.find(size);
          if (e2 || cand == by_size.end()) continue;
          const std::string path = it->path().string();
          const std::string fp = compute_fp(path);
          const auto hit = cand->second.find(fp);
          if (hit == cand->second.end()) continue;
          if (link_track(hit->second, fp, path)) {
            ++linked;
            cand->second.erase(hit);
          }
        }
      }
      tx.commit();
    } catch (const std::exception& e) {
      DH_LOG(Warn, "sync: relink failed: %s", e.what());
    }
    return linked;
  }

  Status status() {
    Status s;
    s.device_id = dev;
    s.device_name = dev_name;
    s.folder = cfg.folder;
    s.pending_records = static_cast<int>(st.pending.size());
    s.last_sync_ms = last_sync;
    s.last_error = last_error.empty() ? init_error : last_error;
    try {
      s.tracks_missing = count_missing();
    } catch (...) {
    }
    return s;
  }

  std::vector<DeviceInfo> devices() {
    std::vector<DeviceInfo> out;
    std::error_code ec;
    for (fs::directory_iterator d(root_dir(), ec), dend; !ec && d != dend; d.increment(ec)) {
      std::error_code e2;
      if (!d->is_directory(e2)) continue;
      DeviceInfo info;
      info.id = d->path().filename().string();
      info.is_self = info.id == dev;
      std::string newest;
      for (fs::directory_iterator f(d->path(), e2), fend; !e2 && f != fend; f.increment(e2)) {
        std::uint64_t seq;
        bool snap;
        if (!parse_name(f->path().filename().string(), seq, snap)) continue;
        ++info.files;
        if (seq >= info.last_seq) {
          info.last_seq = seq;
          newest = f->path().string();
        }
      }
      if (info.files == 0) continue;
      std::string bytes;
      FileHeader h;
      if (read_file(newest, bytes, 1 << 20) && parse_header(bytes.substr(0, 4096), h)) {
        info.name = h.device_name;
        info.last_write_ms = h.created_ms;
      }
      out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const DeviceInfo& a, const DeviceInfo& b) { return a.id < b.id; });
    return out;
  }
};

SyncEngine::SyncEngine(Library& lib, SyncConfig cfg) : impl_(std::make_unique<Impl>(lib, std::move(cfg))) {}
SyncEngine::~SyncEngine() = default;
SyncResult SyncEngine::sync_once() { return impl_->sync_once(); }
SyncResult SyncEngine::capture() { return impl_->capture_only(); }
SyncResult SyncEngine::compact() { return impl_->compact(); }
int SyncEngine::relink_missing(const std::vector<std::string>& dirs) { return impl_->relink_missing(dirs); }
Status SyncEngine::status() { return impl_->status(); }
std::vector<DeviceInfo> SyncEngine::devices() { return impl_->devices(); }

}  // namespace deckhand::sync
