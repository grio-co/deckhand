#include "analysis_codec.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace deckhand::sync::detail {
namespace {

std::string fmt(double v, const char* spec) {
  char b[48];
  std::snprintf(b, sizeof b, spec, v);
  std::string s = b;
  if (s.find('.') != std::string::npos && s.find('e') == std::string::npos) {  // "12.50" -> "12.5", "3.00" -> "3"
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s.empty() || s == "-0" ? "0" : s;
}
std::string ms(double v) { return fmt(v, "%.2f"); }
std::string ratio(double v) { return fmt(v, "%.4g"); }

std::string esc(const std::string& s) {
  std::string o;
  for (const char c : s) {
    if (c == '|' || c == '\\' || c == '\n' || c == ',' || c == ';') {
      char b[8];
      std::snprintf(b, sizeof b, "\\%02x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      o += b;
    } else {
      o.push_back(c);
    }
  }
  return o;
}
std::string unesc(const std::string& s) {
  std::string o;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 2 < s.size()) {
      o.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
      i += 2;
    } else {
      o.push_back(s[i]);
    }
  }
  return o;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (true) {
    const std::size_t n = s.find(sep, at);
    if (n == std::string::npos) {
      out.push_back(s.substr(at));
      break;
    }
    out.push_back(s.substr(at, n - at));
    at = n + 1;
  }
  return out;
}
double num(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
int inum(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 10)); }

}  // namespace

int encoded_version(const std::string& text) {
  if (text.size() < 2 || text[0] != 'v') return -1;
  char* end = nullptr;
  const long v = std::strtol(text.c_str() + 1, &end, 10);
  return end == text.c_str() + 1 || *end != '\n' || v < 0 ? -1 : static_cast<int>(v);
}

std::string encode_phrases(const StoredPhrases& p) {
  std::string o = "v" + std::to_string(p.version) + "\n";
  o += "p|" + std::string(p.gridded ? "1" : "0") + "|" + (p.time_based ? "1" : "0") + "|" + ms(p.origin_ms) + "|" +
       ms(p.pickup_ms) + "|" + ms(p.bar_ms) + "|" + std::to_string(p.total_bars) + "\n";
  // Bar starts: the first one, then each as a whole number of 0.01 ms off "previous + bar length", repeats run-length coded.
  // The offsets are measured against the value a reader will rebuild, so rounding never adds up.
  o += "b|";
  {
    std::vector<long long> deltas;
    double prev = 0;
    const double bm = std::llround(p.bar_ms * 100.0) / 100.0;  // as written in the "p" line
    for (std::size_t i = 0; i < p.bar_starts_ms.size(); ++i) {
      if (i == 0) {
        prev = std::llround(p.bar_starts_ms[0] * 100.0) / 100.0;
        o += ms(prev);
        continue;
      }
      const long long d = std::llround((p.bar_starts_ms[i] - prev - bm) * 100.0);
      deltas.push_back(d);
      prev = prev + bm + static_cast<double>(d) / 100.0;
    }
    for (std::size_t i = 0; i < deltas.size();) {
      std::size_t n = 1;
      while (i + n < deltas.size() && deltas[i + n] == deltas[i]) ++n;
      o += "," + std::to_string(deltas[i]);
      if (n >= 3) o += "*" + std::to_string(n);
      else if (n == 2) o += "," + std::to_string(deltas[i]);
      i += n;
    }
  }
  o += "\ne|";  // bar energy in thousandths
  for (std::size_t i = 0; i < p.bar_energy.size(); ++i) o += (i ? "," : "") + std::to_string(std::llround(p.bar_energy[i] * 1000.0));
  o += "\n";
  for (const auto& s : p.sections)
    o += "s|" + ms(s.start_ms) + "|" + ms(s.end_ms) + "|" + std::to_string(s.start_bar) + "|" + std::to_string(s.end_bar) + "|" +
         std::to_string(s.bars) + "|" + std::to_string(s.energy) + "|" + esc(s.label) + "|" + esc(s.kind) + "|" +
         ratio(s.label_confidence) + "|" + ratio(s.boundary_confidence) + "|" + ratio(s.kick) + "|" + (s.phrase_aligned ? "1" : "0") +
         "\n";
  return o;
}

bool decode_phrases(const std::string& text, StoredPhrases& out) {
  const int ver = encoded_version(text);
  if (ver < 0) return false;
  StoredPhrases p;
  p.version = ver;
  bool header = false;
  for (const std::string& line : split(text.substr(text.find('\n') + 1), '\n')) {
    if (line.empty()) continue;
    const auto f = split(line, '|');
    if (f[0] == "p" && f.size() == 7) {
      p.gridded = f[1] == "1";
      p.time_based = f[2] == "1";
      p.origin_ms = num(f[3]);
      p.pickup_ms = num(f[4]);
      p.bar_ms = num(f[5]);
      p.total_bars = inum(f[6]);
      header = true;
    } else if (f[0] == "b" && f.size() == 2) {
      if (!f[1].empty()) {
        const auto toks = split(f[1], ',');
        double prev = num(toks[0]);
        p.bar_starts_ms.push_back(prev);
        for (std::size_t i = 1; i < toks.size(); ++i) {
          const auto star = toks[i].find('*');
          const long long d = std::strtoll(toks[i].c_str(), nullptr, 10);
          const long long n = star == std::string::npos ? 1 : std::strtoll(toks[i].c_str() + star + 1, nullptr, 10);
          for (long long k = 0; k < n && k < 1000000; ++k) {
            prev = prev + p.bar_ms + static_cast<double>(d) / 100.0;
            p.bar_starts_ms.push_back(prev);
          }
        }
      }
    } else if (f[0] == "e" && f.size() == 2) {
      if (!f[1].empty())
        for (const auto& v : split(f[1], ',')) p.bar_energy.push_back(static_cast<float>(num(v) / 1000.0));
    } else if (f[0] == "s" && f.size() == 13) {
      StoredPhraseSection s;
      s.start_ms = num(f[1]);
      s.end_ms = num(f[2]);
      s.start_bar = inum(f[3]);
      s.end_bar = inum(f[4]);
      s.bars = inum(f[5]);
      s.energy = inum(f[6]);
      s.label = unesc(f[7]);
      s.kind = unesc(f[8]);
      s.label_confidence = num(f[9]);
      s.boundary_confidence = num(f[10]);
      s.kick = num(f[11]);
      s.phrase_aligned = f[12] == "1";
      p.sections.push_back(std::move(s));
    }  // other line types: a newer writer's, ignored
  }
  if (!header) return false;
  out = std::move(p);
  return true;
}

std::string encode_vocals(const StoredVocals& v) {
  std::string o = "v" + std::to_string(v.version) + "\nsrc|" + esc(v.source) + "\nr|";
  for (std::size_t i = 0; i < v.regions.size(); ++i) o += (i ? ";" : "") + ms(v.regions[i].start_ms) + "," + ms(v.regions[i].end_ms);
  o += "\n";
  return o;
}

bool decode_vocals(const std::string& text, StoredVocals& out) {
  const int ver = encoded_version(text);
  if (ver < 0) return false;
  StoredVocals v;
  v.version = ver;
  bool regions = false;
  for (const std::string& line : split(text.substr(text.find('\n') + 1), '\n')) {
    const auto f = split(line, '|');
    if (f[0] == "src" && f.size() == 2) {
      v.source = unesc(f[1]);
    } else if (f[0] == "r" && f.size() == 2) {
      regions = true;
      if (!f[1].empty())
        for (const auto& r : split(f[1], ';')) {
          const auto se = split(r, ',');
          if (se.size() == 2) v.regions.push_back({num(se[0]), num(se[1])});
        }
    }
  }
  if (!regions) return false;
  out = std::move(v);
  return true;
}

std::string encode_cues(const CueTimes& c) {
  const auto one = [](const std::optional<double>& v) { return v ? ms(*v) : std::string(); };
  return "v1\nc|" + one(c.load_cue_ms) + "|" + one(c.first_sound_ms) + "|" + one(c.last_sound_ms) + "\n";
}

bool decode_cues(const std::string& text, CueTimes& out) {
  if (encoded_version(text) < 0) return false;
  for (const std::string& line : split(text.substr(text.find('\n') + 1), '\n')) {
    const auto f = split(line, '|');
    if (f[0] == "c" && f.size() == 4) {
      CueTimes c;
      if (!f[1].empty()) c.load_cue_ms = num(f[1]);
      if (!f[2].empty()) c.first_sound_ms = num(f[2]);
      if (!f[3].empty()) c.last_sound_ms = num(f[3]);
      out = c;
      return true;
    }
  }
  return false;
}

}  // namespace deckhand::sync::detail
