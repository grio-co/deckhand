#!/bin/bash
# Compiles the shared C++ core for iOS from the unmodified root CMakeLists.txt (no edits to it).
# Usage: build-core.sh sim|device   -> platform/ios/build/core-<slice>/ (static libs)
# UNTESTED until src/app/include/deckhand/app/capi.h lands on main.
set -euo pipefail
cd "$(dirname "$0")/../../.."
slice=${1:-sim}
sdk=iphonesimulator; [ "$slice" = device ] && sdk=iphoneos
cmake -S . -B "platform/ios/build/core-$slice" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sdk -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release
cmake --build "platform/ios/build/core-$slice"
