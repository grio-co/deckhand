#pragma once
// Musical keys of tracks. Stored as the analysis writes them (wheel notation such as "8A") or as an imported library had
// them ("Am", "F#m", "Db"); these helpers read both, show either, and tell which keys mix well.
#include <string>

namespace deckhand {

struct KeyInfo {
  int number = 0;     // 1..12 on the wheel; 0 = not a key
  bool minor = false;  // wheel letter A = minor, B = major
  explicit operator bool() const { return number != 0; }
};

KeyInfo parse_key(const std::string& text);
std::string key_wheel_text(KeyInfo k);     // "8A"
std::string key_musical_text(KeyInfo k);   // "Am", "C", "Abm"
// The key as `raw` was stored, shown in musical (true) or wheel (false) notation; text that is no key comes back as is.
std::string display_key(const std::string& raw, bool musical);
// Same key, one step either way round the wheel, or the relative major / minor.
bool keys_compatible(KeyInfo a, KeyInfo b);
// The wheel keys compatible with `k` as a "8A,7A,9A,8B" list (for SmartRule::Key).
std::string compatible_key_list(KeyInfo k);

}  // namespace deckhand
