#pragma once
// The step sequencer's patterns, tempo and swing in the library settings (seq.pattern.1..8, seq.bpm, seq.swing,
// seq.current), so they survive a restart of the application and of the engine process.
#include "deckhand/audio/engine.hpp"
#include "deckhand/library/library.hpp"

namespace deckhand {

// Remembers what was last seen so a change can be noticed (and debounced by the caller).
struct SeqWatch {
  std::uint32_t rev = 0;
  float bpm = -1, swing = -1;
  int current = -1;
  bool locked = false;
  bool primed = false;
  // true when `v` differs from what was seen before (the first call only records it)
  bool changed(const StepSeqView& v);
};

// Writes what changed since the stored values (patterns as text; the tempo only while the sequencer runs free, otherwise
// it would store a deck's tempo).
void save_sequencer(Library& lib, const StepSeqView& v);
// Sends the stored patterns, tempo, swing and selected pattern to the engine. Returns how many patterns were sent.
int restore_sequencer(Library& lib, IEngine& engine);

}  // namespace deckhand
