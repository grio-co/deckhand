#pragma once
// Internal: fractional position keys. Strings over [0-9A-Za-z] compared bytewise; a key is a base-62 fraction.
#include <string>
#include <vector>

namespace deckhand::sync::detail {

inline int frac_idx(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  return 36 + (c - 'a');
}
inline char frac_chr(int i) { return static_cast<char>(i < 10 ? '0' + i : i < 36 ? 'A' + (i - 10) : 'a' + (i - 36)); }

// A key strictly between a and b ("" = open end). Requires a < b when both are non-empty.
inline std::string key_between(const std::string& a, const std::string& b) {
  if (!b.empty() && a >= b) return a + "V";  // invalid input; still returns something sortable after a
  if (b.empty() && !a.empty()) {             // appending: step the first digit, so runs stay short
    const int da = frac_idx(a[0]);
    if (da < 61) return std::string(1, frac_chr(da + 1));
    return std::string(1, a[0]) + key_between(a.substr(1), "");
  }
  std::string r;
  bool bounded = !b.empty();
  for (std::size_t i = 0;; ++i) {
    const int da = i < a.size() ? frac_idx(a[i]) : 0;
    const int db = bounded ? (i < b.size() ? frac_idx(b[i]) : 0) : 62;
    if (db - da > 1) {
      r += frac_chr((da + db) / 2);
      return r;
    }
    r += frac_chr(da);
    if (db != da) bounded = false;
  }
}

// n keys strictly between a and b, increasing, spread evenly (short keys even for long runs).
inline void keys_between(const std::string& a, const std::string& b, std::size_t n, std::vector<std::string>& out) {
  if (n == 0) return;
  const std::size_t left = (n - 1) / 2;
  const std::string m = key_between(a, b);
  keys_between(a, m, left, out);
  out.push_back(m);
  keys_between(m, b, n - 1 - left, out);
}

}  // namespace deckhand::sync::detail
