// Text rasteriser handed to the core: Pango (HarfBuzz shaping, Fontconfig fallback, bidi) draws any
// script into an 8-bit alpha bitmap through Cairo. The buffer is malloc'd here and freed by the core.
#pragma once
#include "deckhand/app/capi.h"

namespace deckhand::shell {

bool rasterise_text(void* user, const char* utf8, float px_size, int weight, dh_bitmap* out);

}  // namespace deckhand::shell
