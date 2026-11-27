#pragma once
// Named layouts of the track table: which columns, in which order and width, and the sort. A few are built in; the user's
// own are stored in the library settings ("browser.layouts"). Pure logic, no window.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "deckhand/library/library.hpp"

namespace deckhand {

struct BrowserLayout {
  std::string name;    // built-in: the id ("prepare", "perform", "tags"); user layouts: their own name
  bool builtin = false;
  std::vector<std::string> columns;
  std::map<std::string, float> widths;  // px, or weight for the flexible columns; missing = the column's default
  SortKey sort = SortKey::Default;
  bool descending = false;
};

inline const std::vector<std::string>& browser_column_ids() {  // keep in step with the table in app_browser.cpp
  static const std::vector<std::string> ids = {"title", "artist", "album", "genre", "bpm", "key", "time", "rating", "comment",
                                               "added", "plays", "played", "color", "format", "bitrate", "source"};
  return ids;
}

inline const char* sort_key_name(SortKey k) {
  switch (k) {
    case SortKey::Title: return "title";
    case SortKey::Artist: return "artist";
    case SortKey::Album: return "album";
    case SortKey::Genre: return "genre";
    case SortKey::Bpm: return "bpm";
    case SortKey::Key: return "key";
    case SortKey::Duration: return "time";
    case SortKey::Rating: return "rating";
    case SortKey::DateAdded: return "added";
    case SortKey::Comment: return "comment";
    case SortKey::Color: return "color";
    case SortKey::Format: return "format";
    case SortKey::Bitrate: return "bitrate";
    case SortKey::PlayCount: return "plays";
    case SortKey::LastPlayed: return "played";
    case SortKey::Default: break;
  }
  return "none";
}

inline SortKey sort_key_from_name(const std::string& n) {
  for (const SortKey k : {SortKey::Title, SortKey::Artist, SortKey::Album, SortKey::Genre, SortKey::Bpm, SortKey::Key, SortKey::Duration,
                          SortKey::Rating, SortKey::DateAdded, SortKey::Comment, SortKey::Color, SortKey::Format, SortKey::Bitrate,
                          SortKey::PlayCount, SortKey::LastPlayed})
    if (n == sort_key_name(k)) return k;
  return SortKey::Default;
}

inline std::vector<BrowserLayout> builtin_browser_layouts() {
  std::vector<BrowserLayout> v(3);
  v[0].name = "prepare";  // getting a set ready: tempo, key, genre, notes, newest first
  v[0].columns = {"title", "artist", "genre", "bpm", "key", "time", "rating", "comment", "added"};
  v[0].sort = SortKey::DateAdded;
  v[0].descending = true;
  v[1].name = "perform";  // on stage: few, readable columns, ordered by tempo
  v[1].columns = {"title", "artist", "bpm", "key", "time"};
  v[1].sort = SortKey::Bpm;
  v[2].name = "tags";  // cleaning up: everything the tag editor touches
  v[2].columns = {"title", "artist", "album", "genre", "comment", "rating", "color", "format", "bitrate"};
  v[2].sort = SortKey::Artist;
  for (BrowserLayout& l : v) l.builtin = true;
  return v;
}

// Text form of one layout: name|columns|widths|sort|d (d = 1 for descending). Names cannot hold '|', ';' or line breaks.
inline std::string clean_layout_name(std::string s) {
  for (char& c : s)
    if (c == '|' || c == ';' || c == '\n' || c == '\r' || c == ',' || c == '=') c = ' ';
  while (!s.empty() && s.back() == ' ') s.pop_back();
  std::size_t a = 0;
  while (a < s.size() && s[a] == ' ') ++a;
  s = s.substr(a);
  if (s.size() > 40) s.resize(40);
  return s;
}

inline std::string encode_layout(const BrowserLayout& l) {
  std::string cols, widths;
  for (const std::string& c : l.columns) cols += (cols.empty() ? "" : ",") + c;
  for (const auto& [k, w] : l.widths) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(w));
    widths += (widths.empty() ? "" : ",") + k + "=" + buf;
  }
  return l.name + "|" + cols + "|" + widths + "|" + sort_key_name(l.sort) + "|" + (l.descending ? "1" : "0");
}

inline std::vector<std::string> split_text(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::size_t a = 0;
  for (;;) {
    const std::size_t b = s.find(sep, a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

// False for a damaged line. Unknown and repeated columns are dropped; at least one column must remain.
inline bool decode_layout(const std::string& line, BrowserLayout& out) {
  const std::vector<std::string> f = split_text(line, '|');
  if (f.size() != 5) return false;
  BrowserLayout l;
  l.name = clean_layout_name(f[0]);
  if (l.name.empty()) return false;
  const auto& known = browser_column_ids();
  for (const std::string& c : split_text(f[1], ','))
    if (std::find(known.begin(), known.end(), c) != known.end() && std::find(l.columns.begin(), l.columns.end(), c) == l.columns.end())
      l.columns.push_back(c);
  if (l.columns.empty()) return false;
  for (const std::string& kv : split_text(f[2], ',')) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string id = kv.substr(0, eq);
    const float w = static_cast<float>(std::atof(kv.c_str() + eq + 1));
    if (std::find(known.begin(), known.end(), id) != known.end() && w > 0.05f && w < 2000) l.widths[id] = w;
  }
  l.sort = sort_key_from_name(f[3]);
  l.descending = f[4] == "1" && l.sort != SortKey::Default;
  out = std::move(l);
  return true;
}

// Built-in layouts followed by the user's, as stored. A user layout never shadows a built-in name.
class BrowserLayouts {
 public:
  explicit BrowserLayouts(Library& lib) : lib_(lib) { load(); }

  const std::vector<BrowserLayout>& all() const { return all_; }
  const BrowserLayout* find(const std::string& name) const {
    for (const BrowserLayout& l : all_)
      if (l.name == name) return &l;
    return nullptr;
  }
  std::string active() const { return lib_.setting("browser.layout_active", ""); }
  void set_active(const std::string& name) { lib_.set_setting("browser.layout_active", name); }

  // Stores (or replaces) a user layout; false for an empty name or the name of a built-in.
  bool save(BrowserLayout l) {
    l.name = clean_layout_name(l.name);
    l.builtin = false;
    if (l.name.empty() || l.columns.empty()) return false;
    for (const BrowserLayout& b : builtin_browser_layouts())
      if (b.name == l.name) return false;
    for (BrowserLayout& e : all_) {
      if (e.name == l.name && !e.builtin) {
        e = l;
        persist();
        return true;
      }
    }
    all_.push_back(l);
    persist();
    return true;
  }
  bool remove(const std::string& name) {
    for (std::size_t i = 0; i < all_.size(); ++i) {
      if (all_[i].name == name && !all_[i].builtin) {
        all_.erase(all_.begin() + static_cast<std::ptrdiff_t>(i));
        persist();
        if (active() == name) set_active("");
        return true;
      }
    }
    return false;
  }
  // The layout after / before `name` (wrapping); the first when `name` is unknown.
  const BrowserLayout& step(const std::string& name, int dir) const {
    std::size_t at = all_.size();
    for (std::size_t i = 0; i < all_.size(); ++i)
      if (all_[i].name == name) at = i;
    if (at == all_.size()) return all_.front();
    const std::size_t n = all_.size();
    return all_[(at + (dir >= 0 ? 1 : n - 1)) % n];
  }
  void reload() { load(); }

 private:
  void load() {
    all_ = builtin_browser_layouts();
    for (const std::string& line : split_text(lib_.setting("browser.layouts", ""), ';')) {
      BrowserLayout l;
      if (line.empty() || !decode_layout(line, l)) continue;
      bool dup = false;
      for (const BrowserLayout& e : all_) dup = dup || e.name == l.name;
      if (!dup) all_.push_back(std::move(l));
    }
  }
  void persist() {
    std::string text;
    for (const BrowserLayout& l : all_)
      if (!l.builtin) text += (text.empty() ? "" : ";") + encode_layout(l);
    lib_.set_setting("browser.layouts", text);
  }
  Library& lib_;
  std::vector<BrowserLayout> all_;
};

}  // namespace deckhand
