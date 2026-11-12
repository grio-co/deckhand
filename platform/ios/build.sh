#!/bin/bash
# Usage: platform/ios/build.sh [sim|device] [run]
# sim: builds for the iOS Simulator (ad-hoc simulator signing is inherent to the sim; no device identity applies)
# device: signs with the Apple Development identity from ~/.claude/CLAUDE.md. Requires a provisioning profile
#         (set DEVELOPMENT_TEAM) -- not verifiable without a physical device.
set -euo pipefail
cd "$(dirname "$0")"
mode=${1:-sim}
xcodegen generate --quiet
OUT="$PWD/build"
if [ "$mode" = sim ]; then
  xcodebuild -project DeckHand.xcodeproj -scheme DeckHand -configuration Debug -sdk iphonesimulator \
    -destination 'generic/platform=iOS Simulator' -derivedDataPath "$OUT" CODE_SIGNING_ALLOWED=NO build | tail -n 5
  APP="$OUT/Build/Products/Debug-iphonesimulator/DeckHand.app"
  if [ "${2:-}" = run ]; then
    DEV=${SIM_DEVICE:-$(xcrun simctl list devices available | grep -m1 'iPhone' | grep -o '[0-9A-F-]\{36\}')}
    xcrun simctl boot "$DEV" 2>/dev/null || true
    xcrun simctl terminate "$DEV" co.grio.deckhand 2>/dev/null || true
    xcrun simctl install "$DEV" "$APP"
    xcrun simctl launch "$DEV" co.grio.deckhand
  fi
else
  xcodebuild -project DeckHand.xcodeproj -scheme DeckHand -configuration Debug -sdk iphoneos \
    -destination 'generic/platform=iOS' -derivedDataPath "$OUT" \
    CODE_SIGN_IDENTITY="Apple Development: Grioghar Thomas-Baldwin (7QK88B7T66)" ${DEVELOPMENT_TEAM:+DEVELOPMENT_TEAM=$DEVELOPMENT_TEAM} \
    -allowProvisioningUpdates build | tail -n 5
fi
