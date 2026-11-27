#pragma once
// Internal: text form of the analysis data that travels in sync (phrases, vocal regions, analysis cue points).
// Every encoded value starts with "v<N>\n", N being the analysis algorithm version it was produced by: a reader never
// replaces data with data of a lower version (docs/SYNC.md, "Analysis data").
#include <optional>
#include <string>

#include "deckhand/library/library.hpp"

namespace deckhand::sync::detail {

struct CueTimes {
  std::optional<double> load_cue_ms, first_sound_ms, last_sound_ms;
  bool operator==(const CueTimes& o) const {
    return load_cue_ms == o.load_cue_ms && first_sound_ms == o.first_sound_ms && last_sound_ms == o.last_sound_ms;
  }
  bool any() const { return load_cue_ms || first_sound_ms || last_sound_ms; }
};

std::string encode_phrases(const StoredPhrases& p);
bool decode_phrases(const std::string& text, StoredPhrases& out);
std::string encode_vocals(const StoredVocals& v);
bool decode_vocals(const std::string& text, StoredVocals& out);
std::string encode_cues(const CueTimes& c);
bool decode_cues(const std::string& text, CueTimes& out);
// The leading version of an encoded value, or -1 for empty / malformed text.
int encoded_version(const std::string& text);

}  // namespace deckhand::sync::detail
