#include "deckhand/capture/source.hpp"

#include <algorithm>
#include <cstring>

namespace deckhand::capture {

std::uint64_t MemorySource::read(std::uint64_t start, std::uint64_t count, float* out) {
  if (start >= frames_) return 0;
  count = std::min(count, frames_ - start);
  std::memcpy(out, data_ + start * ch_, static_cast<std::size_t>(count * ch_ * sizeof(float)));
  return count;
}

}  // namespace deckhand::capture
