// Track suggestions and the transition history (see suggest.hpp). The score is a plain weighted sum, so every number
// in a result can be shown to the user.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "deckhand/library/keys.hpp"
#include "deckhand/library/library.hpp"
#include <sqlite3.h>

namespace deckhand {
namespace {

// A transaction that joins one already open.
struct SuggestTx {
  explicit SuggestTx(Db& d) : db(d), own(sqlite3_get_autocommit(d.handle()) != 0) {
    if (own) db.exec("BEGIN IMMEDIATE");
  }
  void commit() {
    if (!own) return;
    db.exec("COMMIT");
    own = false;
  }
  ~SuggestTx() {
    if (!own) return;
    try {
      db.exec("ROLLBACK");
    } catch (...) {
    }
  }
  Db& db;
  bool own;
};

double mean(const std::vector<float>& v, std::size_t from, std::size_t to) {
  if (to <= from) return 0;
  double s = 0;
  for (std::size_t i = from; i < to; ++i) s += v[i];
  return s / static_cast<double>(to - from);
}

std::vector<std::string> genre_tokens(const std::string& g) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    while (!cur.empty() && cur.back() == ' ') cur.pop_back();
    if (!cur.empty()) out.push_back(cur);
    cur.clear();
  };
  for (const char c : g) {
    if (c == ',' || c == ';' || c == '/' || c == '&' || c == '|') flush();
    else if (c == ' ' && cur.empty()) continue;
    else cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  flush();
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// Overlap of two genre lists, 0..1 (shared tokens over the smaller list); `shared` names the first shared genre.
double genre_overlap(const std::vector<std::string>& a, const std::vector<std::string>& b, std::string& shared) {
  if (a.empty() || b.empty()) return -1;
  int n = 0;
  for (const std::string& x : a)
    if (std::binary_search(b.begin(), b.end(), x)) {
      if (!n) shared = x;
      ++n;
    }
  return static_cast<double>(n) / static_cast<double>(std::min(a.size(), b.size()));
}

enum class KeyRel { None, Same, Relative, Adjacent, Boost };

KeyRel key_relation(KeyInfo a, KeyInfo b) {
  if (a.number == b.number) return a.minor == b.minor ? KeyRel::Same : KeyRel::Relative;
  if (a.minor == b.minor) {
    const int d = ((b.number - a.number) % 12 + 12) % 12;
    if (d == 1 || d == 11) return KeyRel::Adjacent;
    if (d == 2) return KeyRel::Boost;
  }
  return KeyRel::None;
}

const char* key_rel_name(KeyRel r) {
  switch (r) {
    case KeyRel::Same: return "same";
    case KeyRel::Relative: return "relative";
    case KeyRel::Adjacent: return "adjacent";
    case KeyRel::Boost: return "boost";
    default: return "none";
  }
}

double key_rel_value(KeyRel r) {
  switch (r) {
    case KeyRel::Same: return 1.0;
    case KeyRel::Relative: return 0.8;
    case KeyRel::Adjacent: return 0.8;
    case KeyRel::Boost: return 0.4;
    default: return 0;
  }
}

struct Cand {
  std::int64_t id = 0;
  double bpm = 0;
  int rating = 0;
  double intro = -1;  // -1 = no energy data
  std::string key, genre;
};

}  // namespace

std::optional<EnergySummary> energy_of_phrases(const StoredPhrases& p) {
  EnergySummary e;
  if (!p.bar_energy.empty()) {
    const std::size_t n = p.bar_energy.size();
    const std::size_t k = std::min<std::size_t>(16, std::max<std::size_t>(1, n / 4));
    e.avg = mean(p.bar_energy, 0, n);
    e.intro = mean(p.bar_energy, 0, k);
    e.outro = mean(p.bar_energy, n - k, n);
    return e;
  }
  if (p.sections.empty()) return std::nullopt;
  double total = 0, weighted = 0;
  for (const StoredPhraseSection& s : p.sections) {
    const double w = std::max(1.0, s.end_ms - s.start_ms);
    total += w;
    weighted += w * s.energy / 10.0;
  }
  e.avg = total > 0 ? weighted / total : 0;
  e.intro = p.sections.front().energy / 10.0;
  e.outro = p.sections.back().energy / 10.0;
  return e;
}

std::optional<EnergySummary> Library::energy_summary(std::int64_t track_id) {
  {
    Stmt s(db_, "SELECT energy_avg, energy_intro, energy_outro FROM track_phrases WHERE track_id = ?1");
    s.bind(1, track_id);
    if (!s.step()) return std::nullopt;
    if (!s.is_null(0) && !s.is_null(1) && !s.is_null(2)) return EnergySummary{s.f64(0), s.f64(1), s.f64(2)};
  }
  const auto p = phrases(track_id);
  if (!p) return std::nullopt;
  const auto e = energy_of_phrases(*p);
  if (!e) return std::nullopt;
  Stmt u(db_, "UPDATE track_phrases SET energy_avg = ?1, energy_intro = ?2, energy_outro = ?3 WHERE track_id = ?4");
  u.bind(1, e->avg).bind(2, e->intro).bind(3, e->outro).bind(4, track_id);
  u.run();
  return e;
}

// ---------------------------------------------------------------- transition history

void Library::add_transition(std::int64_t from_id, std::int64_t to_id, int count, std::int64_t at_ms) {
  if (from_id <= 0 || to_id <= 0 || from_id == to_id || count <= 0) return;
  Stmt u(db_, "INSERT INTO transitions(from_id, to_id, count, last_ms) VALUES (?1, ?2, ?3, ?4)"
              " ON CONFLICT(from_id, to_id) DO UPDATE SET count = count + ?3, last_ms = max(last_ms, ?4)");
  u.bind(1, from_id).bind(2, to_id).bind(3, count).bind(4, at_ms);
  u.run();
}

int Library::record_transition(std::int64_t session_id, std::int64_t track_id, std::int64_t at_ms) {
  std::int64_t prev = 0;
  {
    // The newest entry is the one just added: the one before it is what this track follows.
    Stmt s(db_, "SELECT track_id FROM history WHERE session_id = ?1 AND track_id > 0 ORDER BY started_at DESC, id DESC LIMIT 1 OFFSET 1");
    s.bind(1, session_id);
    if (s.step()) prev = s.i64(0);
  }
  if (prev <= 0 || prev == track_id) return 0;
  add_transition(prev, track_id, 1, at_ms);
  return 1;
}

void Library::rebuild_transitions() {
  SuggestTx tx(db_);
  db_.exec("UPDATE transitions SET count = 0, last_ms = 0");
  std::map<std::pair<std::int64_t, std::int64_t>, std::pair<int, std::int64_t>> pairs;
  {
    Stmt s(db_, "SELECT session_id, track_id, started_at FROM history WHERE track_id IS NOT NULL AND track_id > 0"
                " ORDER BY session_id, started_at, id");
    std::int64_t session = -1, prev = 0;
    while (s.step()) {
      const std::int64_t sess = s.i64(0), tr = s.i64(1), at = s.i64(2);
      if (sess == session && prev > 0 && prev != tr) {
        auto& e = pairs[{prev, tr}];
        ++e.first;
        e.second = std::max(e.second, at);
      }
      session = sess;
      prev = tr;
    }
  }
  for (const auto& [k, v] : pairs) add_transition(k.first, k.second, v.first, v.second);
  db_.exec("DELETE FROM transitions WHERE count = 0 AND saved = 0");
  tx.commit();
}

std::optional<TransitionRow> Library::transition(std::int64_t from_id, std::int64_t to_id) {
  Stmt s(db_, "SELECT count, last_ms, coalesce(note, ''), saved FROM transitions WHERE from_id = ?1 AND to_id = ?2");
  s.bind(1, from_id).bind(2, to_id);
  if (!s.step()) return std::nullopt;
  TransitionRow r;
  r.from_id = from_id;
  r.to_id = to_id;
  r.count = static_cast<int>(s.i64(0));
  r.last_ms = s.i64(1);
  r.note = s.text(2);
  r.saved = s.i64(3) != 0;
  return r;
}

std::vector<TransitionRow> Library::transitions_from(std::int64_t from_id, std::size_t limit) {
  std::vector<TransitionRow> out;
  Stmt s(db_, "SELECT to_id, count, last_ms, coalesce(note, ''), saved FROM transitions WHERE from_id = ?1"
              " ORDER BY count DESC, last_ms DESC, to_id LIMIT ?2");
  s.bind(1, from_id).bind(2, static_cast<std::int64_t>(limit));
  while (s.step()) {
    TransitionRow r;
    r.from_id = from_id;
    r.to_id = s.i64(0);
    r.count = static_cast<int>(s.i64(1));
    r.last_ms = s.i64(2);
    r.note = s.text(3);
    r.saved = s.i64(4) != 0;
    out.push_back(std::move(r));
  }
  return out;
}

void Library::save_transition(std::int64_t from_id, std::int64_t to_id, const std::string& note) {
  if (from_id <= 0 || to_id <= 0 || from_id == to_id) return;
  Stmt u(db_, "INSERT INTO transitions(from_id, to_id, count, last_ms, note, saved) VALUES (?1, ?2, 0, 0, ?3, 1)"
              " ON CONFLICT(from_id, to_id) DO UPDATE SET note = ?3, saved = 1");
  u.bind(1, from_id).bind(2, to_id).bind(3, note);
  u.run();
}

// ---------------------------------------------------------------- suggestions

NextTrackWeights Library::suggest_weights() {
  NextTrackWeights w;
  auto get = [&](const char* name, double& v) {
    const std::string t = setting(std::string("suggest.w.") + name);
    if (t.empty()) return;
    char* end = nullptr;
    const double x = std::strtod(t.c_str(), &end);
    if (end != t.c_str() && x >= 0 && x <= 1000) v = x;
  };
  get("tempo", w.tempo);
  get("key", w.key);
  get("energy", w.energy);
  get("genre", w.genre);
  get("rating", w.rating);
  get("history", w.history);
  get("explore", w.explore);
  get("recent", w.recent);
  return w;
}

std::vector<NextTrack> Library::suggest(std::int64_t seed_id, const NextTrackOptions& opt) {
  std::vector<NextTrack> out;
  double seed_bpm = 0;
  std::string seed_key, seed_genre;
  {
    Stmt s(db_, "SELECT coalesce(bpm, 0), coalesce(key, ''), coalesce(genre, '') FROM tracks WHERE id = ?1");
    s.bind(1, seed_id);
    if (!s.step()) return out;
    seed_bpm = s.f64(0);
    seed_key = s.text(1);
    seed_genre = s.text(2);
  }
  const NextTrackWeights& W = opt.weights;
  const double mp = std::clamp(opt.max_pitch, 0.001, 0.5);
  const KeyInfo seed_ki = parse_key(seed_key);
  const std::vector<std::string> seed_genres = genre_tokens(seed_genre);
  const auto seed_energy = energy_summary(seed_id);

  // Candidates: the tempo ranges (direct, half time, double time) come straight from the bpm index.
  std::unordered_set<std::int64_t> in_playlist;
  if (opt.playlist_id > 0) {
    for (const std::int64_t id : playlist_track_ids(opt.playlist_id)) in_playlist.insert(id);
    if (in_playlist.empty()) return out;
  }
  std::vector<Cand> cands;
  {
    std::string sql = "SELECT t.id, coalesce(t.bpm, 0), coalesce(t.key, ''), coalesce(t.genre, ''), coalesce(t.rating, 0),"
                      " p.energy_intro FROM tracks t LEFT JOIN track_phrases p ON p.track_id = t.id WHERE t.id <> ?1";
    const bool tempo = seed_bpm > 0;
    if (tempo) {
      sql += " AND (t.bpm BETWEEN ?2 AND ?3";
      if (opt.half_double) sql += " OR t.bpm BETWEEN ?4 AND ?5 OR t.bpm BETWEEN ?6 AND ?7";
      sql += ")";
    }
    Stmt s(db_, sql.c_str());
    s.bind(1, seed_id);
    if (tempo) {
      s.bind(2, seed_bpm / (1 + mp)).bind(3, seed_bpm / (1 - mp));
      if (opt.half_double) {
        s.bind(4, seed_bpm * 0.5 / (1 + mp)).bind(5, seed_bpm * 0.5 / (1 - mp));
        s.bind(6, seed_bpm * 2 / (1 + mp)).bind(7, seed_bpm * 2 / (1 - mp));
      }
    }
    while (s.step()) {
      const std::int64_t id = s.i64(0);
      if (opt.exclude.count(id)) continue;
      if (!in_playlist.empty() && !in_playlist.count(id)) continue;
      Cand c;
      c.id = id;
      c.bpm = s.f64(1);
      c.key = s.text(2);
      c.genre = s.text(3);
      c.rating = static_cast<int>(s.i64(4));
      c.intro = s.is_null(5) ? -1 : s.f64(5);
      cands.push_back(std::move(c));
    }
  }

  // History: pairs, tracks ever played, tracks played in this session.
  std::unordered_map<std::int64_t, int> fwd, rev;
  {
    Stmt s(db_, "SELECT to_id, count FROM transitions WHERE from_id = ?1 AND count > 0");
    s.bind(1, seed_id);
    while (s.step()) fwd[s.i64(0)] = static_cast<int>(s.i64(1));
    Stmt r(db_, "SELECT from_id, count FROM transitions WHERE to_id = ?1 AND count > 0");
    r.bind(1, seed_id);
    while (r.step()) rev[r.i64(0)] = static_cast<int>(r.i64(1));
  }
  std::unordered_set<std::int64_t> played;
  if (W.explore != 0) {
    Stmt s(db_, "SELECT DISTINCT track_id FROM history WHERE track_id IS NOT NULL");
    while (s.step()) played.insert(s.i64(0));
  }
  std::vector<std::int64_t> recent = opt.recent;
  if (recent.empty() && opt.recent_from_history && W.recent != 0) {
    Stmt s(db_, "SELECT track_id FROM history WHERE track_id > 0 AND session_id ="
                " (SELECT id FROM history_sessions ORDER BY started_at DESC, id DESC LIMIT 1)"
                " ORDER BY started_at DESC, id DESC LIMIT 60");
    while (s.step()) recent.push_back(s.i64(0));
  }
  std::unordered_map<std::int64_t, int> recent_pos;
  for (std::size_t i = 0; i < recent.size(); ++i) recent_pos.emplace(recent[i], static_cast<int>(i));

  std::unordered_map<std::string, KeyInfo> key_cache;
  std::unordered_map<std::string, std::pair<double, std::string>> genre_cache;
  const double base_total = W.tempo + W.key + W.energy + W.genre + W.rating;
  const double targets[3] = {seed_bpm, seed_bpm * 0.5, seed_bpm * 2};
  const char* const target_tag[3] = {"direct", "half", "double"};
  const double seed_outro = seed_energy ? seed_energy->outro : -1;

  std::vector<NextTrack> all;
  all.reserve(cands.size());
  for (const Cand& c : cands) {
    NextTrack sg;
    sg.track_id = c.id;
    double wsum = 0, wval = 0;
    NextTrackFactor f_tempo, f_key, f_energy, f_genre, f_rating;
    bool h_tempo = false, h_key = false, h_energy = false, h_genre = false, h_rating = false;

    if (seed_bpm > 0 && c.bpm > 0) {
      int best = -1;
      double best_p = 1e9;
      for (int k = 0; k < (opt.half_double ? 3 : 1); ++k) {
        const double p = targets[k] / c.bpm - 1;
        if (std::fabs(p) < std::fabs(best_p) - 1e-12) {
          best_p = p;
          best = k;
        }
      }
      if (best < 0 || std::fabs(best_p) > mp + 1e-9) continue;
      sg.pitch_change = best_p;
      f_tempo.id = "tempo";
      f_tempo.value = (1 - std::fabs(best_p) / mp) * (best == 0 ? 1.0 : 0.9);
      f_tempo.detail = c.bpm - targets[best];
      f_tempo.tag = target_tag[best];
      h_tempo = true;
    }
    if (seed_ki) {
      auto it = key_cache.find(c.key);
      if (it == key_cache.end()) it = key_cache.emplace(c.key, parse_key(c.key)).first;
      if (it->second) {
        const KeyRel rel = key_relation(seed_ki, it->second);
        if (rel == KeyRel::None && opt.harmonic_only) continue;
        f_key.id = "key";
        f_key.value = key_rel_value(rel);
        f_key.tag = key_rel_name(rel);
        h_key = true;
      }
    }
    if (seed_outro >= 0 && c.intro >= 0) {
      f_energy.id = "energy";
      f_energy.value = std::max(0.0, 1 - std::fabs(seed_outro - c.intro));
      f_energy.detail = c.intro - seed_outro;
      h_energy = true;
    }
    if (!seed_genres.empty() && !c.genre.empty()) {
      auto it = genre_cache.find(c.genre);
      if (it == genre_cache.end()) {
        std::string shared;
        const double v = genre_overlap(seed_genres, genre_tokens(c.genre), shared);
        it = genre_cache.emplace(c.genre, std::make_pair(v, shared)).first;
      }
      if (it->second.first >= 0) {
        f_genre.id = "genre";
        f_genre.value = it->second.first;
        f_genre.tag = it->second.first > 0 ? it->second.second : std::string();
        h_genre = true;
      }
    }
    if (c.rating > 0) {
      f_rating.id = "rating";
      f_rating.value = std::min(1.0, c.rating / 5.0);
      f_rating.detail = c.rating;
      h_rating = true;
    }
    auto take = [&](bool has, NextTrackFactor& f, double w) {
      if (!has) return;
      wsum += w;
      wval += w * f.value;
      f.points = w;  // scaled below
    };
    take(h_tempo, f_tempo, W.tempo);
    take(h_key, f_key, W.key);
    take(h_energy, f_energy, W.energy);
    take(h_genre, f_genre, W.genre);
    take(h_rating, f_rating, W.rating);
    const double scale = wsum > 0 ? base_total / wsum : 0;
    double score = 0;
    for (auto [has, f] : {std::pair<bool, NextTrackFactor*>{h_tempo, &f_tempo}, {h_key, &f_key}, {h_energy, &f_energy},
                          {h_genre, &f_genre}, {h_rating, &f_rating}}) {
      if (!has) continue;
      f->points = f->points * f->value * scale;
      score += f->points;
      sg.factors.push_back(*f);
    }
    (void)wval;

    const auto fi = fwd.find(c.id);
    const auto ri = rev.find(c.id);
    sg.pair_count = fi == fwd.end() ? 0 : fi->second;
    sg.reverse_count = ri == rev.end() ? 0 : ri->second;
    if (sg.pair_count > 0 || sg.reverse_count > 0) {
      NextTrackFactor h;
      h.id = "history";
      h.value = std::min(1.0, std::log2(1.0 + sg.pair_count + 0.4 * sg.reverse_count) / 2.0);
      h.detail = sg.pair_count;
      h.tag = sg.pair_count > 0 ? "forward" : "reverse";
      h.points = W.history * h.value;
      score += h.points;
      sg.factors.push_back(h);
    }
    const auto rp = recent_pos.find(c.id);
    if (rp != recent_pos.end()) {
      NextTrackFactor r;
      r.id = "recent";
      r.value = std::max(0.25, 1.0 - rp->second * 0.05);
      r.detail = rp->second;
      r.points = -W.recent * r.value;
      score += r.points;
      sg.factors.push_back(r);
    } else if (W.explore != 0 && !played.count(c.id)) {
      NextTrackFactor e;
      e.id = "explore";
      e.value = 1;
      e.points = W.explore;
      score += e.points;
      sg.factors.push_back(e);
    }
    sg.score = score;
    all.push_back(std::move(sg));
  }
  const std::size_t n = opt.limit == 0 ? all.size() : std::min(opt.limit, all.size());
  auto better = [](const NextTrack& a, const NextTrack& b) { return a.score != b.score ? a.score > b.score : a.track_id < b.track_id; };
  std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n), all.end(), better);
  all.resize(n);
  return all;
}

}  // namespace deckhand
