# DeckHand on iOS

Thin Swift shell (`platform/ios/`) over the shared C++ core. iOS 17+, iPhone and iPad, arm64
(device and simulator). Same contract as the macOS shell: the core owns audio, library, mapping and
draws the whole UI into a draw list; the shell renders it with Metal and supplies platform services.

## Build and run
```bash
platform/ios/build.sh sim run      # xcodegen -> xcodebuild -> install + launch in a booted/first iPhone simulator
platform/ios/build.sh device       # signed with "Apple Development: Grioghar Thomas-Baldwin (7QK88B7T66)"
```
`SIM_DEVICE=<udid>` picks the simulator; `DEVELOPMENT_TEAM=<id>` is needed for device builds (provisioning profile).
The Xcode project is generated from `platform/ios/project.yml` (needs `brew install xcodegen`); it is not checked in.
`platform/ios/scripts/build-core.sh sim|device` builds the core for iOS via a CMake toolchain invocation (root CMake untouched).

## Design
- `Renderer.swift`: one Metal pipeline (MSL compiled at runtime from a string, so no `.metal` build step). Vertices in
  pixels, RGBA8 colour, 32-bit indices, per-command scissor, alpha-blended. A8 textures are swizzled to white+alpha.
- `TextRasteriser.swift`: CoreText callback: UTF-8 + pixel size -> 8-bit alpha bitmap (all scripts, RTL, font fallback).
- `DeckViewController.swift`: `MTKView` at 60 fps, multi-touch -> pointer down/move/up/cancel (ids 0-9, pixels), safe-area and
  scale passed to the core, keyboard via `UIKeyInput` shown while the core sets `wants_text_input`.
- `AudioSessionManager.swift`: `.playback`, 5 ms preferred buffer, interruption resume, route changes; requests 4 output
  channels when the route offers them and reports and calls `dh_app_audio_route` so the core reopens its output.
- MIDI: the core opens CoreMIDI itself (`platform/apple/midi_apple.cpp`); the shell does nothing. Bluetooth MIDI would need `CABTMIDICentralViewController` (CoreAudioKit) for pairing UI; the plist already has the Bluetooth usage string.
- `FileAccess.swift`: document picker (audio + folders) with security-scoped bookmarks persisted and restored; Documents folder
  exposed in Files (`UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`). Background audio via `UIBackgroundModes: audio`.
- `CoreBridge.swift` is the only file touching the C API.

## Differs from macOS
Touch not mouse; no windowing or menus; scene lifecycle (required by the current SDK); security-scoped file access; audio
session instead of device enumeration (routes follow the OS); no separate engine process (iOS forbids helper processes).
DRM-protected streaming tracks cannot be read (apps do not expose them as files). 

## Status
The stub core is gone; the real core is linked (`scripts/build-core.sh`: CMake with `-DDECKHAND_WITH_SQLCIPHER=OFF`, `CMAKE_C_FLAGS=-x objective-c` for miniaudio, static libs, no CMake edits). Earlier verification with the stub: rendering, text incl. CJK/Cyrillic/Arabic, touch
buttons, drag slider, keyboard text entry; device-slice compile (unsigned). Not verified: real core, audio output, MIDI,
document picker, signing/install on a physical device, iPad layout, Bluetooth MIDI.

## Asks to the Mac session
Sent: (1) the contract I stubbed (see `platform/ios/Stub/deckhand_capi_stub.h`) so `capi.h` can match; (2) touch-first layout
variant / hit-target sizing, (3) safe-area insets in the display call, (4) audio route call (channel count, master/cue pairs) and
a way to hand the core an audio device config instead of miniaudio opening one itself (CoreAudio on iOS needs the session set first).

## Current blocker
Simulator launch aborts in core audio init: miniaudio forces the PlayAndRecord session category on iOS (RemoteIO then hangs in the sim). Asked the Mac session to pass `sessionCategory=none, noAudioSessionActivate` to `ma_context_init` when the shell manages audio.
