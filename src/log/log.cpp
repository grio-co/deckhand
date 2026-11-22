#include "deckhand/log/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace deckhand::log {
namespace {
std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;
std::FILE* g_file = nullptr;
}  // namespace

void set_level(Level l) { g_level.store(l, std::memory_order_relaxed); }
Level level() { return g_level.load(std::memory_order_relaxed); }
bool enabled(Level l) { return static_cast<int>(l) <= static_cast<int>(level()); }

void set_file(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) std::fclose(g_file);
  g_file = path.empty() ? nullptr : std::fopen(path.c_str(), "a");
}

void write(Level l, std::string_view m) {
  if (!enabled(l)) return;
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
  std::lock_guard<std::mutex> lock(g_mutex);
  for (std::FILE* f : {stderr, g_file}) {
    if (!f) continue;
    std::fprintf(f, "%s.%03d [%.*s] %.*s\n", stamp, ms, static_cast<int>(name(l).size()), name(l).data(),
                 static_cast<int>(m.size()), m.data());
    std::fflush(f);
  }
}

void writef(Level l, const char* fmt, ...) {
  if (!enabled(l)) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  write(l, buf);
}

}  // namespace deckhand::log
