#pragma once
// Internal: register values, hybrid-logical-clock stamps and the in-memory merged state.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace deckhand::sync::detail {

// A dynamically typed value (what a SQLite column holds).
struct Val {
  enum Type : std::uint8_t { Null = 0, Int = 1, Real = 2, Text = 3 };
  Type t = Null;
  std::int64_t i = 0;
  double d = 0;
  std::string s;

  static Val null() { return {}; }
  static Val integer(std::int64_t v) { Val x; x.t = Int; x.i = v; return x; }
  static Val real(double v) { Val x; x.t = Real; x.d = v; return x; }
  static Val text(std::string v) { Val x; x.t = Text; x.s = std::move(v); return x; }

  bool empty() const {  // the "absent" default: NULL, 0, 0.0 or ''
    return t == Null || (t == Int && i == 0) || (t == Real && d == 0) || (t == Text && s.empty());
  }
  bool operator==(const Val& o) const {
    if (t != o.t) return false;
    switch (t) {
      case Null: return true;
      case Int: return i == o.i;
      case Real: return d == o.d || (d != d && o.d != o.d);
      case Text: return s == o.s;
    }
    return false;
  }
};

// Two values that mean the same: equal, or both "empty".
inline bool same(const Val& a, const Val& b) { return (a.empty() && b.empty()) || a == b; }

// Hybrid logical clock stamp; the device id breaks ties so stamps are totally ordered.
struct Stamp {
  std::int64_t wall = 0;
  std::uint32_t ctr = 0;
  std::string dev;
  bool operator<(const Stamp& o) const { return std::tie(wall, ctr, dev) < std::tie(o.wall, o.ctr, o.dev); }
  bool operator==(const Stamp& o) const { return wall == o.wall && ctr == o.ctr && dev == o.dev; }
};

struct Reg {
  Stamp st;
  Val v;
  bool pending = false;  // authored here, not yet written to a log file
};

using Key = std::tuple<char, std::string, std::string>;  // kind, entity id, field

struct Entity {
  std::map<std::string, Reg> f;
};

// Entity kinds: T track (id = fingerprint), C cue (fp#slot), P playlist/folder, E playlist entry,
// H history row, S history session, G setting (id = key).
constexpr const char* kKinds = "TCPEHSG";

struct State {
  std::map<std::string, Entity> ent[7];
  std::set<Key> pending;  // regs to export
  std::set<Key> dirty;    // regs changed since the last flush to SQLite

  static int idx(char kind) {
    for (int i = 0; kKinds[i]; ++i)
      if (kKinds[i] == kind) return i;
    return -1;
  }
  const Reg* find(char kind, const std::string& eid, const std::string& field) const {
    const int k = idx(kind);
    if (k < 0) return nullptr;
    const auto e = ent[k].find(eid);
    if (e == ent[k].end()) return nullptr;
    const auto f = e->second.f.find(field);
    return f == e->second.f.end() ? nullptr : &f->second;
  }
  const Entity* entity(char kind, const std::string& eid) const {
    const int k = idx(kind);
    if (k < 0) return nullptr;
    const auto e = ent[k].find(eid);
    return e == ent[k].end() ? nullptr : &e->second;
  }
  bool alive(char kind, const std::string& eid) const {
    const Reg* r = find(kind, eid, "_del");
    return !r || r->v.empty();
  }
  std::string text(char kind, const std::string& eid, const std::string& field) const {
    const Reg* r = find(kind, eid, field);
    return r && r->v.t == Val::Text ? r->v.s : std::string();
  }
  // Last-writer-wins merge of a remote register. Returns true when the state changed.
  bool merge(char kind, const std::string& eid, const std::string& field, const Stamp& st, const Val& v) {
    const int k = idx(kind);
    if (k < 0) return false;
    Reg& r = ent[k][eid].f[field];
    const bool fresh = r.st.dev.empty();
    if (!fresh && !(r.st < st)) return false;
    r.st = st;
    r.v = v;
    r.pending = false;
    pending.erase(Key{kind, eid, field});
    dirty.insert(Key{kind, eid, field});
    return true;
  }
  void set_local(char kind, const std::string& eid, const std::string& field, const Stamp& st, const Val& v) {
    const int k = idx(kind);
    Reg& r = ent[k][eid].f[field];
    r.st = st;
    r.v = v;
    r.pending = true;
    pending.insert(Key{kind, eid, field});
    dirty.insert(Key{kind, eid, field});
  }
};

}  // namespace deckhand::sync::detail
