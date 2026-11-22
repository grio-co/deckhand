/* Force-included into the `dh_app` target on Windows (see CMakeLists.txt): src/app uses a few POSIX names that the
   Windows toolchains lack. Nothing here changes behaviour on other platforms; the real fixes belong in src/app. */
#ifndef DECKHAND_WINDOWS_COMPAT_H
#define DECKHAND_WINDOWS_COMPAT_H
#if defined(_WIN32)
#include <stddef.h>
#include <time.h>
#include <io.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef __cplusplus
#include <cstring> /* src/app relies on std::strlen arriving transitively */
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#endif

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#define __attribute__(x) /* GNU attributes (printf format checks) */
#define chmod _chmod
#ifdef __cplusplus
static inline struct tm* localtime_r(const time_t* t, struct tm* out) { return localtime_s(out, t) == 0 ? out : 0; }
#endif
#elif defined(__MINGW32__)
#if !defined(_POSIX_THREAD_SAFE_FUNCTIONS) && !defined(localtime_r)
#ifdef __cplusplus
static inline struct tm* localtime_r(const time_t* t, struct tm* out) { return localtime_s(out, t) == 0 ? out : 0; }
#endif
#endif
#endif
#endif /* _WIN32 */
#endif
