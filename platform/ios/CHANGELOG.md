# iOS changelog
## Unreleased
- Bluetooth: native MIDI pairing sheet and audio route picker overlay; AirPlay allowed in the audio session
- Swift/UIKit shell: Metal draw-list renderer, CoreText rasteriser, multi-touch, keyboard, safe areas.
- AVAudioSession, CoreMIDI, document picker + bookmarks, background audio.
- XcodeGen project + `build.sh`; stub core to prove the shell until `capi.h` lands.
- Merged capi.h; real core linked for sim/device slices via CMake toolchain flags; removed stub and shell MIDI (core owns CoreMIDI); atlas region uploads, key events, text weight.
- Verified real core in the simulator: UI, import, load, playback. DEBUG-only DECKHAND_OPEN test hook.
- Device build signs via automatic provisioning with the Apple Development identity (needs DEVELOPMENT_TEAM).
