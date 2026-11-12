#!/bin/bash
# Compiles the shared C++ core for iOS from the unmodified root CMakeLists.txt.
# Usage: build-core.sh sim|device  -> platform/ios/build/core-<slice>/ (static libs; see CORE_LIBS)
set -euo pipefail
cd "$(dirname "$0")/../../.."
slice=${1:-sim}
sdk=iphonesimulator; [ "$slice" = device ] && sdk=iphoneos
B="platform/ios/build/core-$slice"
cmake -S . -B "$B" \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sdk -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release  -DDECKHAND_WITH_SQLCIPHER=OFF -DCMAKE_C_FLAGS="-x objective-c" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY >/dev/null
cmake --build "$B" -j --target dh_app dh_platform 2>&1 | grep -E "error|Error|FAILED" || true
ls "$B"/libdh_app.a >/dev/null
