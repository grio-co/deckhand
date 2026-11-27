#include "deckhand/app/seq_store.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace deckhand {

bool SeqWatch::changed(const StepSeqView& v) {
  const bool same = primed && rev == v.rev && bpm == v.bpm && swing == v.swing && current == v.current && locked == (v.locked != 0);
  rev = v.rev;
  bpm = v.bpm;
  swing = v.swing;
  current = v.current;
  locked = v.locked != 0;
  const bool was = primed;
  primed = true;
  return was && !same;
}

namespace {
std::string number(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  return buf;
}
}  // namespace

void save_sequencer(Library& lib, const StepSeqView& v) {
  const std::string empty = seq_pattern_to_text(SeqPatternData{});
  for (int i = 0; i < kSeqPatterns; ++i) {
    const std::string key = "seq.pattern." + std::to_string(i + 1);
    std::string text = seq_pattern_to_text(v.pattern[i]);
    if (text == empty) text.clear();
    if (lib.setting(key) != text) lib.set_setting(key, text);
  }
  const auto put = [&](const char* key, const std::string& value) {
    if (lib.setting(key) != value) lib.set_setting(key, value);
  };
  if (!v.locked) put("seq.bpm", number(v.bpm));
  put("seq.swing", number(v.swing));
  put("seq.current", std::to_string(v.current + 1));
}

int restore_sequencer(Library& lib, IEngine& engine) {
  int sent = 0;
  for (int i = 0; i < kSeqPatterns; ++i) {
    const std::string text = lib.setting("seq.pattern." + std::to_string(i + 1));
    if (!text.empty() && engine.seq_pattern_set(i, text)) ++sent;
  }
  const std::string bpm = lib.setting("seq.bpm"), swing = lib.setting("seq.swing"), cur = lib.setting("seq.current");
  if (!bpm.empty()) {
    const double b = std::strtod(bpm.c_str(), nullptr);
    if (std::isfinite(b) && b >= 20 && b <= 400) engine.seq_bpm(b);
  }
  if (!swing.empty()) {
    const double s = std::strtod(swing.c_str(), nullptr);
    if (std::isfinite(s) && s >= 0 && s <= 0.75) engine.seq_swing(s);
  }
  const int c = std::atoi(cur.c_str());
  if (c >= 1 && c <= kSeqPatterns) engine.seq_select(c - 1, false);
  return sent;
}

}  // namespace deckhand
