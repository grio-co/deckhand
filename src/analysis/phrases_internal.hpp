#pragma once
// Internal: phrase detection on an already prepared mono signal (<= ~24.5 kHz).
#include <vector>

#include "deckhand/analysis/analysis.hpp"

namespace deckhand::detail {
PhraseAnalysis phrases_from_mono(const std::vector<float>& mono, double rate, const BeatGrid& grid, double first_ms,
                                 double last_ms, double duration_ms, const AnalysisOptions& options);
}
