#pragma once
// Reordering the track table's columns by dragging their headers.
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace deckhand {

// Where a column dropped at x goes: before the returned index (x0.size() = after the last one). The pointer passing a
// column's middle moves the slot past it.
inline std::size_t column_drop_slot(const std::vector<float>& x0, const std::vector<float>& x1, float x) {
  std::size_t slot = 0;
  while (slot < x0.size() && x > (x0[slot] + x1[slot]) / 2) ++slot;
  return slot;
}

// Moves `id` in front of `before` (empty: to the end). False when the order does not change.
inline bool move_column(std::vector<std::string>& cols, const std::string& id, const std::string& before) {
  std::vector<std::string> out = cols;
  const auto from = std::find(out.begin(), out.end(), id);
  if (from == out.end() || id == before) return false;
  out.erase(from);
  out.insert(before.empty() ? out.end() : std::find(out.begin(), out.end(), before), id);
  if (out == cols) return false;
  cols.swap(out);
  return true;
}

}  // namespace deckhand
