#include "deckhand/library/keys.hpp"

#include <cctype>
#include <cstdlib>

namespace deckhand {
namespace {

// Pitch class (C = 0) of the tonic for wheel numbers 1..12.
const int kMajorPc[12] = {11, 6, 1, 8, 3, 10, 5, 0, 7, 2, 9, 4};
const int kMinorPc[12] = {8, 3, 10, 5, 0, 7, 2, 9, 4, 11, 6, 1};
const char* const kMajorName[12] = {"B", "F#", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E"};
const char* const kMinorName[12] = {"Abm", "Ebm", "Bbm", "Fm", "Cm", "Gm", "Dm", "Am", "Em", "Bm", "F#m", "C#m"};

int letter_pc(char c) {
  switch (std::toupper(static_cast<unsigned char>(c))) {
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    case 'A': return 9;
    case 'B': return 11;
    default: return -1;
  }
}

}  // namespace

KeyInfo parse_key(const std::string& text) {
  KeyInfo k;
  std::size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i >= text.size()) return k;
  if (std::isdigit(static_cast<unsigned char>(text[i]))) {  // wheel: 1..12 then A or B
    const int n = std::atoi(text.c_str() + i);
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    if (n < 1 || n > 12 || i >= text.size()) return k;
    const char l = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    if ((l != 'A' && l != 'B') || i + 1 < text.size()) return k;
    k.number = n;
    k.minor = l == 'A';
    return k;
  }
  int pc = letter_pc(text[i]);
  if (pc < 0) return k;
  ++i;
  for (; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '#') pc = (pc + 1) % 12;
    else if (c == 'b' && (i + 1 >= text.size() || text[i + 1] != 'a')) pc = (pc + 11) % 12;
    else if (c == 0xE2 && i + 2 < text.size()) {  // UTF-8 sharp (E2 99 AF) / flat (E2 99 AD)
      if (static_cast<unsigned char>(text[i + 2]) == 0xAF) pc = (pc + 1) % 12;
      else if (static_cast<unsigned char>(text[i + 2]) == 0xAD) pc = (pc + 11) % 12;
      i += 2;
    } else break;
  }
  std::string rest;
  for (; i < text.size(); ++i) rest += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
  bool minor = false;
  if (rest.empty() || rest == "maj" || rest == "major") minor = false;
  else if (rest == "m" || rest == "min" || rest == "minor") minor = true;
  else return k;
  for (int n = 1; n <= 12; ++n) {
    if ((minor ? kMinorPc : kMajorPc)[n - 1] == pc) {
      k.number = n;
      k.minor = minor;
      return k;
    }
  }
  return k;
}

std::string key_wheel_text(KeyInfo k) { return k ? std::to_string(k.number) + (k.minor ? "A" : "B") : std::string(); }

std::string key_musical_text(KeyInfo k) { return k ? std::string((k.minor ? kMinorName : kMajorName)[k.number - 1]) : std::string(); }

std::string display_key(const std::string& raw, bool musical) {
  const KeyInfo k = parse_key(raw);
  if (!k) return raw;
  return musical ? key_musical_text(k) : key_wheel_text(k);
}

bool keys_compatible(KeyInfo a, KeyInfo b) {
  if (!a || !b) return false;
  if (a.minor == b.minor) {
    const int d = std::abs(a.number - b.number);
    return d == 0 || d == 1 || d == 11;
  }
  return a.number == b.number;
}

std::string compatible_key_list(KeyInfo k) {
  std::string out;
  if (!k) return out;
  for (int n = 1; n <= 12; ++n) {
    for (int m = 0; m < 2; ++m) {
      KeyInfo c;
      c.number = n;
      c.minor = m == 0;
      if (keys_compatible(k, c)) out += (out.empty() ? "" : ",") + key_wheel_text(c);
    }
  }
  return out;
}

}  // namespace deckhand
