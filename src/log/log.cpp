#include "deckhand/log/log.hpp"
#include <atomic>
#include <cstdio>

namespace deckhand::log {
namespace { std::atomic<Level> g_level{Level::Info}; }

void set_level(Level l) { g_level.store(l, std::memory_order_relaxed); }
Level level() { return g_level.load(std::memory_order_relaxed); }
bool enabled(Level l) { return static_cast<int>(l) <= static_cast<int>(level()); }
void write(Level l, std::string_view m) {
  if (!enabled(l)) return;
  std::fprintf(stderr, "[%.*s] %.*s\n", static_cast<int>(name(l).size()), name(l).data(),
               static_cast<int>(m.size()), m.data());
}
}  // namespace deckhand::log
