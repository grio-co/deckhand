// Text rasteriser handed to the core: DirectWrite shapes any script (HarfBuzz-class shaping, bidi, system font
// fallback) and its glyph run analysis rasterises into an 8-bit alpha bitmap. The buffer is malloc'd here and freed
// by the core. Call from one thread (the UI thread).
#pragma once
#include "deckhand/app/capi.h"

namespace deckhand::shell {

bool rasterise_text(void* user, const char* utf8, float px_size, int weight, dh_bitmap* out);

}  // namespace deckhand::shell
