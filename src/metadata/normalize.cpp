// Normalisation and similarity of titles and artist names.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

#include "deckhand/metadata/metadata.hpp"

namespace deckhand::metadata {
namespace {

const char* const kLatin1[64] = {  // U+00C0..U+00FF, lower case
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i", "d", "n", "o", "o", "o", "o", "o", "x",
    "o", "u", "u", "u", "u", "y", "th", "ss", "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y"};

struct Run {
  const char* s;
  int n;
};
const Run kExtA[] = {{"a", 6}, {"c", 8}, {"d", 4}, {"e", 10}, {"g", 8}, {"h", 4}, {"i", 10}, {"ij", 2}, {"j", 2}, {"k", 3},
                     {"l", 10}, {"n", 7}, {"ng", 2}, {"o", 6}, {"oe", 2}, {"r", 6}, {"s", 8}, {"t", 6}, {"u", 12}, {"w", 2},
                     {"y", 3}, {"z", 6}, {"s", 1}};  // U+0100..U+017F

const char* fold_cp(std::uint32_t cp) {
  if (cp >= 0xC0 && cp <= 0xFF) return kLatin1[cp - 0xC0];
  if (cp >= 0x100 && cp <= 0x17F) {
    int i = static_cast<int>(cp - 0x100);
    for (const Run& r : kExtA) {
      if (i < r.n) return r.s;
      i -= r.n;
    }
  }
  if (cp == 0x2018 || cp == 0x2019 || cp == 0x2032) return "'";
  if (cp == 0x201C || cp == 0x201D) return "\"";
  if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) return "-";
  return nullptr;
}

// Lower case ASCII, diacritics folded, other non-ASCII sequences copied unchanged.
std::string fold(const std::string& in) {
  std::string out;
  for (std::size_t i = 0; i < in.size();) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (c < 0x80) {
      out += static_cast<char>(std::tolower(c));
      ++i;
      continue;
    }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (i + static_cast<std::size_t>(len) > in.size()) len = 1;
    std::uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(in[i + static_cast<std::size_t>(k)]) & 0x3Fu);
    const char* f = len > 1 ? fold_cp(cp) : nullptr;
    if (f) out += f;
    else out.append(in, i, static_cast<std::size_t>(len));
    i += static_cast<std::size_t>(len);
  }
  return out;
}

// Words only: punctuation becomes a space, "&" becomes "and", apostrophes vanish.
std::string words(const std::string& s) {
  std::string out;
  bool space = true;
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '\'') continue;
    if (c == '&') {
      out += space ? "and " : " and ";
      space = true;
      continue;
    }
    if (c >= 0x80 || std::isalnum(c)) {
      out += static_cast<char>(c);
      space = false;
    } else if (!space) {
      out += ' ';
      space = true;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool has_word(const std::string& w, const char* word) {
  const std::string pat = std::string(" ") + word + " ";
  return (" " + w + " ").find(pat) != std::string::npos;
}
bool starts_with_word(const std::string& w, const char* word) {
  const std::size_t n = std::strlen(word);
  return w.compare(0, n, word) == 0 && (w.size() == n || w[n] == ' ');
}

bool credit_word(const std::string& w) {
  return has_word(w, "feat") || has_word(w, "ft") || has_word(w, "featuring") || has_word(w, "with") || has_word(w, "prod");
}
bool ignorable(const std::string& w) {
  if (w.empty() || credit_word(w)) return true;
  if (w.find("remaster") != std::string::npos) return true;
  static const char* const kIgnore[] = {"original mix", "original version", "album version", "single version", "radio edit",
                                        "explicit", "clean", "bonus track", "mono", "stereo", "digital remaster", "original"};
  for (const char* k : kIgnore)
    if (w == k) return true;
  return false;
}
bool variant_word(const std::string& w) {
  static const char* const kWords[] = {"remix", "mix", "edit", "dub", "instrumental", "live", "acoustic", "version", "vip",
                                       "bootleg", "rework", "cover", "remaster", "remastered", "demo", "reprise"};
  for (const char* k : kWords)
    if (has_word(w, k)) return true;
  return false;
}

void strip_leading_the(std::string& s) {
  if (s.compare(0, 4, "the ") == 0 && s.size() > 4) s.erase(0, 4);
}

std::size_t levenshtein(const std::string& a, const std::string& b) {
  std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j)
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0u : 1u)});
    std::swap(prev, cur);
  }
  return prev[b.size()];
}
double ratio(const std::string& a, const std::string& b) {
  if (a.empty() && b.empty()) return 1;
  const std::size_t m = std::max(a.size(), b.size());
  if (m == 0) return 1;
  return 1.0 - static_cast<double>(levenshtein(a, b)) / static_cast<double>(m);
}
std::string sorted_tokens(const std::string& s) {
  std::istringstream is(s);
  std::vector<std::string> t;
  for (std::string w; is >> w;) t.push_back(w);
  std::sort(t.begin(), t.end());
  std::string out;
  for (const auto& w : t) out += (out.empty() ? "" : " ") + w;
  return out;
}
std::vector<std::string> split_bar(const std::string& s) {
  std::vector<std::string> out;
  std::size_t a = 0;
  while (a <= s.size()) {
    const std::size_t b = std::min(s.find('|', a), s.size());
    if (b > a) out.push_back(s.substr(a, b - a));
    a = b + 1;
  }
  return out;
}

}  // namespace

Normalized normalize_title(const std::string& title) {
  Normalized n;
  std::string t = fold(title);
  // bracketed groups are qualifiers
  std::string rest;
  for (std::size_t i = 0; i < t.size();) {
    const char open = t[i];
    if (open == '(' || open == '[') {
      const char close = open == '(' ? ')' : ']';
      const std::size_t e = t.find(close, i + 1);
      if (e != std::string::npos) {
        const std::string q = words(t.substr(i + 1, e - i - 1));
        if (!ignorable(q)) n.variant.insert(q);
        rest += ' ';
        i = e + 1;
        continue;
      }
    }
    rest += t[i++];
  }
  // " - qualifier" tail (a hyphen with spaces around it)
  const std::size_t dash = rest.rfind(" - ");
  if (dash != std::string::npos) {
    const std::string q = words(rest.substr(dash + 3));
    if (credit_word(q) || ignorable(q)) rest.erase(dash);
    else if (variant_word(q)) {
      n.variant.insert(q);
      rest.erase(dash);
    }
  }
  std::string w = words(rest);
  // "feat. X" without brackets: the rest is a credit
  for (const char* k : {"feat", "ft", "featuring"}) {
    const std::string pat = std::string(" ") + k + " ";
    const std::size_t p = (" " + w + " ").find(pat);
    if (p != std::string::npos && p > 0) w.erase(p - 1 < w.size() ? p - 1 : w.size());
  }
  strip_leading_the(w);
  n.base = w;
  return n;
}

std::string normalize_artist(const std::string& artist) {
  std::string t = fold(artist);
  // credits and joiners become '|'
  std::string w;
  for (std::size_t i = 0; i < t.size(); ++i) {
    const char c = t[i];
    if (c == ',' || c == ';' || c == '/' || c == '+') w += '|';
    else w += c;
  }
  const auto replace_all = [&](const std::string& from, const std::string& to) {
    for (std::size_t p = w.find(from); p != std::string::npos; p = w.find(from, p + to.size())) w.replace(p, from.size(), to);
  };
  for (const char* k : {" feat. ", " feat ", " ft. ", " ft ", " featuring ", " with ", " vs. ", " vs ", " and ", " & ", " x "})
    replace_all(k, "|");
  std::vector<std::string> parts;
  for (const auto& p : split_bar(w)) {
    std::string q = words(p);
    strip_leading_the(q);
    if (!q.empty()) parts.push_back(q);
  }
  std::sort(parts.begin(), parts.end());
  parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
  std::string out;
  for (const auto& p : parts) out += (out.empty() ? "" : "|") + p;
  return out;
}

double title_similarity(const std::string& a, const std::string& b) {
  const Normalized x = normalize_title(a), y = normalize_title(b);
  if (x.base.empty() || y.base.empty()) return 0;
  double s = std::max(ratio(x.base, y.base), ratio(sorted_tokens(x.base), sorted_tokens(y.base)));
  if (x.variant != y.variant) s *= 0.75;
  return s;
}

double artist_similarity(const std::string& a, const std::string& b) {
  const std::string x = normalize_artist(a), y = normalize_artist(b);
  if (x.empty() || y.empty()) return 0;
  if (x == y) return 1;
  const auto xs = split_bar(x), ys = split_bar(y);
  const bool x_in_y = std::all_of(xs.begin(), xs.end(), [&](const std::string& p) { return std::find(ys.begin(), ys.end(), p) != ys.end(); });
  const bool y_in_x = std::all_of(ys.begin(), ys.end(), [&](const std::string& p) { return std::find(xs.begin(), xs.end(), p) != xs.end(); });
  double best = std::max(ratio(x, y), 0.0);
  const auto& small = xs.size() <= ys.size() ? xs : ys;
  const auto& large = xs.size() <= ys.size() ? ys : xs;
  double sum = 0;
  for (const auto& p : small) {
    double m = 0;
    for (const auto& q : large) m = std::max(m, ratio(p, q));
    sum += m;
  }
  best = std::max(best, sum / static_cast<double>(large.size()));
  if (x_in_y || y_in_x) best = std::max(best, 0.9);
  return best;
}

std::string percent_encode(const std::string& s) {
  static const char* const hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += static_cast<char>(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

}  // namespace deckhand::metadata
