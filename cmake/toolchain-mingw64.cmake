# Cross-compiles DeckHand for Windows x86_64 with mingw-w64 (POSIX threads, GCC 13 or newer, or llvm-mingw).
# Dependencies (zlib, OpenSSL, SQLCipher) live under ${DECKHAND_WIN_PREFIX}, default /opt/win64 (scripts/windows/Dockerfile).
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(_triple x86_64-w64-mingw32)
if(NOT DEFINED DECKHAND_WIN_PREFIX)
  set(DECKHAND_WIN_PREFIX /opt/win64)
endif()
set(CMAKE_C_COMPILER ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)
set(CMAKE_RC_COMPILER ${_triple}-windres)
set(CMAKE_FIND_ROOT_PATH ${DECKHAND_WIN_PREFIX} /usr/${_triple})
set(CMAKE_PREFIX_PATH ${DECKHAND_WIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Tests run under Wine when the build script sets CMAKE_CROSSCOMPILING_EMULATOR (scripts/windows/docker-build.sh).
