# iOS changelog
## Unreleased
- Swift/UIKit shell: Metal draw-list renderer, CoreText rasteriser, multi-touch, keyboard, safe areas.
- AVAudioSession, CoreMIDI, document picker + bookmarks, background audio.
- XcodeGen project + `build.sh`; stub core to prove the shell until `capi.h` lands.
- Merged capi.h; real core linked for sim/device slices via CMake toolchain flags; removed stub and shell MIDI (core owns CoreMIDI); atlas region uploads, key events, text weight.
