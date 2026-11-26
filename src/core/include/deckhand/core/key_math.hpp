// Musical-key arithmetic shared by the engine (key sync) and the UI. Header-only, no dependencies.
//
// A key is the analysis module's key_index: 0..11 major (C, C#, ... B), 12..23 minor (C, C#, ... B); -1 = unknown.
// The wheel is the Camelot / Open Key wheel: neighbours on one ring are a fifth apart, the relative major / minor
// (C major <-> A minor) share a number.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string>

namespace deckhand::keymath {

constexpr bool valid(int k) { return k >= 0 && k <= 23; }
constexpr bool is_minor(int k) { return k >= 12; }
constexpr int tonic(int k) { return k % 12; }
constexpr int make(int tonic_pc, bool minor) { return ((tonic_pc % 12) + 12) % 12 + (minor ? 12 : 0); }

// C major <-> A minor, and so on.
constexpr int relative(int k) { return valid(k) ? (is_minor(k) ? make(tonic(k) + 3, false) : make(tonic(k) + 9, true)) : -1; }
// `steps` places around the wheel on the same ring: +1 = a fifth up (8B C major -> 9B G major).
constexpr int wheel_step(int k, int steps) { return valid(k) ? make(tonic(k) + 7 * steps, is_minor(k)) : -1; }

// 1..12, 0 for an unknown key. C major = 8 (8B), A minor = 8 (8A).
constexpr int wheel_number(int k) {
  if (!valid(k)) return 0;
  const int major_tonic = is_minor(k) ? (tonic(k) + 3) % 12 : tonic(k);
  return ((major_tonic * 7) % 12 + 7) % 12 + 1;
}
inline std::string camelot(int k) { return valid(k) ? std::to_string(wheel_number(k)) + (is_minor(k) ? "A" : "B") : std::string(); }
// Open Key notation: C major = 1d, A minor = 1m.
inline std::string open_key(int k) {
  if (!valid(k)) return {};
  const int n = (wheel_number(k) - 8 + 12) % 12 + 1;
  return std::to_string(n) + (is_minor(k) ? "m" : "d");
}

// Parses what a library stores: "Am", "C#m", "Db", "F# minor", Camelot "8A" / "08B", Open Key "1d" / "1m". -1 when not a key.
inline int parse(const std::string& text) {
  std::string t;
  for (char c : text) if (c != ' ' && c != '\t') t += c;
  if (t.empty()) return -1;
  if (std::isdigit(static_cast<unsigned char>(t[0]))) {
    std::size_t i = 0;
    int n = 0;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])) && i < 3) n = n * 10 + (t[i++] - '0');
    if (i != t.size() - 1 || n < 1 || n > 12) return -1;
    const char m = static_cast<char>(std::tolower(static_cast<unsigned char>(t[i])));
    if (m == 'a' || m == 'b') {  // Camelot: major tonic from the wheel number
      const int major_tonic = ((n - 8 + 12) % 12 * 7) % 12;  // inverse of wheel_number
      return m == 'b' ? make(major_tonic, false) : make(major_tonic + 9, true);
    }
    if (m == 'd' || m == 'm') {  // Open Key: 1d = C major
      const int major_tonic = ((n - 1) * 7) % 12;
      return m == 'd' ? make(major_tonic, false) : make(major_tonic + 9, true);
    }
    return -1;
  }
  static const int pc[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
  const char l = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
  if (l < 'A' || l > 'G') return -1;
  int p = pc[l - 'A'];
  std::size_t i = 1;
  if (i < t.size() && (t[i] == '#' || t[i] == 'b')) p += t[i++] == '#' ? 1 : -1;
  std::string rest;
  for (; i < t.size(); ++i) rest += static_cast<char>(std::tolower(static_cast<unsigned char>(t[i])));
  if (rest.empty() || rest == "maj" || rest == "major") return make(p, false);
  if (rest == "m" || rest == "min" || rest == "minor") return make(p, true);
  return -1;
}

// Semitones a tempo ratio moves the pitch by when it is not key-locked (1.06 -> +1.0194).
inline double tempo_to_semitones(double ratio) { return ratio > 0 ? 12.0 * std::log2(ratio) : 0.0; }

// ---- key sync -------------------------------------------------------------------------------------------
enum class SyncMode : int { Match = 0, Compatible = 1 };
enum class SyncResult : int { None = 0, Ok = 1, TooFar = 2, NoOwnKey = 3, NoRefKey = 4, Leader = 5 };

struct SyncParams {
  int own_key = -1;        // the deck's key as analysed (without any shift)
  int ref_key = -1;        // the reference deck's analysed key
  double own_pitch = 0;    // semitones the own audio is already moved by apart from the shift (0 while key-locked)
  double ref_pitch = 0;    // semitones the reference is sounding above / below its analysed key
  SyncMode mode = SyncMode::Match;
  bool fine = false;       // land exactly (cents) instead of whole semitones
  double limit = 2.0;      // largest |shift| in semitones
  int prefer = -1;         // candidate chosen last time (hysteresis), -1 = none
};

struct SyncSolution {
  SyncResult result = SyncResult::None;
  double shift = 0;        // semitones to apply (whole, or in cents when `fine`); meaningful for Ok
  double raw = 0;          // the unrounded shift of the chosen candidate
  int candidate = -1;      // 0 same / relative, 1 one step up the wheel, 2 one step down
  int target_key = -1;     // the key index the deck is nominally in after the shift
};

constexpr double kCandidateBias = 0.3;  // semitones a rival must win by to displace the previous candidate

inline double wrap_semitones(double x) {  // into [-6, 6)
  x = std::fmod(x + 6.0, 12.0);
  if (x < 0) x += 12.0;
  return x - 6.0;
}

inline SyncSolution solve_sync(const SyncParams& p) {
  SyncSolution s;
  if (!valid(p.own_key)) { s.result = SyncResult::NoOwnKey; return s; }
  if (!valid(p.ref_key)) { s.result = SyncResult::NoRefKey; return s; }
  const bool same_ring = is_minor(p.own_key) == is_minor(p.ref_key);
  // The reference's tonic on our ring: itself when the modes agree, else the relative key.
  const int base = same_ring ? p.ref_key : relative(p.ref_key);
  const double ref_tonic = tonic(base) + p.ref_pitch;
  const double own_tonic = tonic(p.own_key) + p.own_pitch;
  const int ncand = (p.mode == SyncMode::Compatible && same_ring) ? 3 : 1;
  const int offsets[3] = {0, 7, -7};
  double best = 1e9;
  for (int c = 0; c < ncand; ++c) {
    const double raw = wrap_semitones(ref_tonic + offsets[c] - own_tonic);
    const double score = std::fabs(raw) - (c == p.prefer ? kCandidateBias : 0.0);
    if (score < best - 1e-9) {
      best = score;
      s.candidate = c;
      s.raw = raw;
    }
  }
  s.shift = p.fine ? std::round(s.raw * 100.0) / 100.0 : std::round(s.raw);
  if (s.shift == 0) s.shift = 0;  // no negative zero
  s.target_key = make(tonic(base) + offsets[s.candidate], is_minor(p.own_key));
  s.result = std::fabs(s.shift) <= std::max(0.0, p.limit) + 1e-9 ? SyncResult::Ok : SyncResult::TooFar;
  return s;
}

// Follow mode: true while the shift already applied is close enough to the new solution to leave alone.
inline bool hold_current(double current_shift, const SyncSolution& s, bool fine) {
  return std::fabs(s.raw - current_shift) < (fine ? 0.05 : 0.65);
}

}  // namespace deckhand::keymath
