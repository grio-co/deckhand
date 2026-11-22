#pragma once
// Four public levels plus a hidden TRACE (symbol tracing, enabled only by explicit flag).
// The audio thread must never log directly; it posts to a lock-free ring buffer.
#include <string>
#include <string_view>

namespace deckhand::log {

enum class Level { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

void set_level(Level);
Level level();
bool enabled(Level);
// Also append log lines to this file (empty path disables).
void set_file(const std::string& path);
void write(Level, std::string_view message);
#if defined(__MINGW32__)
void writef(Level, const char* fmt, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 2, 3)));
#elif defined(__GNUC__) || defined(__clang__)
void writef(Level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#else
void writef(Level, const char* fmt, ...);
#endif

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

#define DH_LOG(lvl, ...)                                                        \
  do {                                                                          \
    if (::deckhand::log::enabled(::deckhand::log::Level::lvl))                  \
      ::deckhand::log::writef(::deckhand::log::Level::lvl, __VA_ARGS__);        \
  } while (0)
