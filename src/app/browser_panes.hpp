#pragma once
// The optional second track table: layout of the two slots, and how each pane's source / sort / search is stored.
// Pure logic (no window); the application swaps the live browser state with the other pane's snapshot.
#include <array>
#include <cstdlib>
#include <string>

#include "browser_layouts.hpp"

namespace deckhand {

enum class SplitMode { Off = 0, Side = 1, Stacked = 2 };

struct SlotRect {
  float x = 0, y = 0, w = 0, h = 0;
  bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

inline std::array<SlotRect, 2> split_slots(const SlotRect& r, SplitMode mode, float gap = 6) {
  if (mode == SplitMode::Side) {
    const float w = (r.w - gap) / 2;
    return {SlotRect{r.x, r.y, w, r.h}, SlotRect{r.x + w + gap, r.y, w, r.h}};
  }
  if (mode == SplitMode::Stacked) {
    const float h = (r.h - gap) / 2;
    return {SlotRect{r.x, r.y, r.w, h}, SlotRect{r.x, r.y + h + gap, r.w, h}};
  }
  return {r, SlotRect{}};
}

// -1 when the point is in neither slot (or the split is off and the point is outside).
inline int slot_at(const std::array<SlotRect, 2>& s, SplitMode mode, float x, float y) {
  if (s[0].contains(x, y)) return 0;
  if (mode != SplitMode::Off && s[1].contains(x, y)) return 1;
  return -1;
}

// What one pane shows. kind is the application's tree kind (an int here), `id` the playlist / folder / session id,
// `path` the directory of a disk folder.
struct PaneSnapshot {
  int kind = 0;
  std::int64_t id = 0;
  std::string path;
  SortKey sort = SortKey::Default;
  bool descending = false;
  std::string search;
  bool operator==(const PaneSnapshot& o) const {
    return kind == o.kind && id == o.id && path == o.path && sort == o.sort && descending == o.descending && search == o.search;
  }
};

// kind|id|sort|desc|path|search -- the path and the search come last and may hold anything but a line break; the path
// cannot hold '|' (such a folder simply is not remembered).
inline std::string encode_pane(const PaneSnapshot& p) {
  std::string path = p.path, search = p.search;
  for (char& c : search)
    if (c == '\n' || c == '\r') c = ' ';
  if (path.find('|') != std::string::npos || path.find('\n') != std::string::npos) path.clear();
  return std::to_string(p.kind) + "|" + std::to_string(p.id) + "|" + sort_key_name(p.sort) + "|" + (p.descending ? "1" : "0") + "|" + path + "|" + search;
}

inline bool decode_pane(const std::string& text, PaneSnapshot& out) {
  std::string f[5];
  std::size_t at = 0;
  for (int i = 0; i < 5; ++i) {
    const std::size_t bar = text.find('|', at);
    if (bar == std::string::npos) return false;
    f[i] = text.substr(at, bar - at);
    at = bar + 1;
  }
  PaneSnapshot p;
  char* end = nullptr;
  p.kind = static_cast<int>(std::strtol(f[0].c_str(), &end, 10));
  if (f[0].empty() || *end) return false;
  p.id = std::strtoll(f[1].c_str(), &end, 10);
  if (f[1].empty() || *end) return false;
  p.sort = sort_key_from_name(f[2]);
  p.descending = f[3] == "1" && p.sort != SortKey::Default;
  p.path = f[4];
  p.search = text.substr(at);
  out = std::move(p);
  return true;
}

// Settings: browser.split (0/1/2), browser.pane.0 / browser.pane.1 (the two slots) and browser.live_slot (the focused one).
struct PaneSettings {
  SplitMode mode = SplitMode::Off;
  int live_slot = 0;
  PaneSnapshot pane[2];
};

inline PaneSettings load_pane_settings(Library& lib) {
  PaneSettings s;
  const int m = std::atoi(lib.setting("browser.split", "0").c_str());
  s.mode = m == 1 ? SplitMode::Side : m == 2 ? SplitMode::Stacked : SplitMode::Off;
  s.live_slot = lib.setting("browser.live_slot", "0") == "1" ? 1 : 0;
  for (int i = 0; i < 2; ++i) {
    PaneSnapshot p;
    if (decode_pane(lib.setting("browser.pane." + std::to_string(i), ""), p)) s.pane[i] = p;
  }
  return s;
}

inline void save_pane_settings(Library& lib, const PaneSettings& s) {
  lib.set_setting("browser.split", std::to_string(static_cast<int>(s.mode)));
  lib.set_setting("browser.live_slot", std::to_string(s.live_slot));
  for (int i = 0; i < 2; ++i) lib.set_setting("browser.pane." + std::to_string(i), encode_pane(s.pane[i]));
}

inline SplitMode next_split_mode(SplitMode m) {  // off -> side by side -> stacked -> off
  return m == SplitMode::Off ? SplitMode::Side : m == SplitMode::Side ? SplitMode::Stacked : SplitMode::Off;
}

}  // namespace deckhand
