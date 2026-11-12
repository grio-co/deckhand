#pragma once
// Four public levels plus a hidden TRACE (symbol tracing, enabled only by explicit flag).
// The audio thread must never log directly; it posts to a lock-free ring buffer.
#include <string_view>

namespace deckhand::log {

enum class Level { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

void set_level(Level);
Level level();
bool enabled(Level);
void write(Level, std::string_view message);

constexpr std::string_view name(Level l) {
  switch (l) {
    case Level::Error: return "ERROR";
    case Level::Warn: return "WARN";
    case Level::Info: return "INFO";
    case Level::Debug: return "DEBUG";
    case Level::Trace: return "TRACE";
  }
  return "?";
}

}  // namespace deckhand::log
