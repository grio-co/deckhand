# DeckHand on Linux

The Linux port is the same C++20 core as the other platforms with a thin desktop shell
(`platform/linux/shell/`: SDL3 window, OpenGL 3.3 core or OpenGL ES 3.0 renderer, Pango text) and two
platform files (`platform/linux/decoder_linux.cpp`, `midi_linux.cpp`).

## Dependencies

| Need | Debian / Ubuntu | Fedora | Arch |
|---|---|---|---|
| Toolchain | `build-essential cmake ninja-build pkg-config` | `gcc-c++ cmake ninja-build pkgconf-pkg-config` | `base-devel cmake ninja` |
| SQLCipher | `libsqlcipher-dev` | `sqlcipher-devel` | `sqlcipher` |
| OpenSSL, zlib | `libssl-dev zlib1g-dev` | `openssl-devel zlib-devel` | `openssl zlib` |
| FFmpeg (decoding) | `libavformat-dev libavcodec-dev libavutil-dev libswresample-dev` | `ffmpeg-free-devel` (or RPM Fusion `ffmpeg-devel`) | `ffmpeg` |
| ALSA (MIDI) | `libasound2-dev` | `alsa-lib-devel` | `alsa-lib` |
| Text | `libpango1.0-dev libcairo2-dev` | `pango-devel cairo-devel` | `pango cairo` |
| Window and input | `libsdl3-dev` (Debian 13, Ubuntu 25.04 and later) | `SDL3-devel` | `sdl3` |
| Fonts (runtime) | `fonts-noto-core fonts-noto-cjk` | `google-noto-sans-fonts google-noto-sans-cjk-fonts` | `noto-fonts noto-fonts-cjk` |

Ubuntu 24.04 and older have no SDL3 package: build a release from <https://github.com/libsdl-org/SDL>
(`scripts/linux/Dockerfile` shows the exact steps). Audio output needs no development package, see below.
Without SDL3, Pango or a window system, build only the core and `deckhandctl` with
`-DDECKHAND_BUILD_SHELL=OFF`.

## Build

```sh
scripts/build-linux.sh --test            # build-linux/, runs ctest
scripts/build-linux.sh --install ~/.local
```

`BUILD_DIR` and `CMAKE_ARGS` override the build folder and add configure options. The shell is
`build-linux/deckhand`; its resources are copied to `build-linux/share/deckhand/` at build time.

Without a local toolchain, `scripts/linux/docker-build.sh` builds the Ubuntu 24.04 image
(`scripts/linux/Dockerfile`), copies the tree into a container (works with a remote or VM Docker daemon),
builds, runs every test and the Xvfb screenshot smoke test (`scripts/linux/smoke.sh`) and copies the
artefacts (screenshots, logs) to `build-linux-artifacts/`.

## Run

```sh
deckhand [--deck-a FILE] [--deck-b FILE] [FILE...]
```

* Data (library, settings, logs): `$XDG_DATA_HOME/DeckHand`, default `~/.local/share/DeckHand`.
* Portable mode: an empty file named `portable` beside the binary keeps everything in `DeckHandData/`
  next to it.
* Resources (`controllers/`, `lang/`, `skins/`) are read from `share/deckhand` beside the binary, then
  `../share/deckhand`, `/usr/local/share/deckhand`, `/usr/share/deckhand`; `DECKHAND_RESOURCE_DIR` overrides.
* `DECKHAND_LOG=error|warn|info|debug|trace` sets the log level (`deckhand.log` in the data folder).
* Debug: `--screenshot FILE.png [--frames N]` renders N frames, writes the window to a PNG and exits.
* Keys: Ctrl (or Super) takes the role of Command on macOS. Files can be dropped on the window. The
  window picks X11 or Wayland through SDL (`SDL_VIDEODRIVER=x11|wayland` forces one) and follows the
  desktop's display scale.

## Design choices

* **Windowing: SDL3** (zlib licence) covers X11 and Wayland, HiDPI, text input, drag and drop and OpenGL
  context creation with one code path. GL entry points are loaded through SDL, so no libGL is linked.
* **Rendering** mirrors the Metal shell: one shader (vertex colour, alpha times the texel), scissor per
  command, straight (non-sRGB) framebuffer values. It falls back to OpenGL ES 3.0 when 3.3 core is
  unavailable.
* **Text: Pango + Cairo** (Fontconfig, HarfBuzz and FreeType underneath) rather than hand-written
  FreeType/HarfBuzz: it gives shaping, bidirectional text and per-glyph font fallback (CJK, Arabic, Hebrew,
  Indic) in about 100 lines. Verified with Japanese, Korean, Arabic, Hebrew, Devanagari, Cyrillic and Greek.
* **Audio backends: miniaudio with runtime linking left on.** On Linux `MA_NO_RUNTIME_LINKING` is not
  defined (it stays on macOS), so miniaudio `dlopen`s PulseAudio (PipeWire's pulse server included), ALSA and
  JACK when the output opens. The binary needs no audio library at build or load time and works on any
  system that has one of them.
* **Decoding: FFmpeg** (`libavformat`, `libavcodec`, `libswresample`, dynamic): MP3, AAC/M4A, FLAC, WAV,
  AIFF, Ogg Vorbis, Opus. Output is interleaved stereo float at the file's own sample rate; mono is
  duplicated.
* **MIDI: ALSA sequencer.** Every readable port of every other client is subscribed; hot-plug follows the
  system announce port (150 ms settle time). A device is named by its ALSA client name (plus the port name
  if it has several ports), so `match =` lines in controller mappings work as on macOS. PipeWire and
  JACK expose their MIDI through the same sequencer when `pipewire-alsa`/`a2jmidid` bridges are present.

## Licences

DeckHand is MIT. It links, dynamically, FFmpeg (LGPL 2.1+ for a distribution build without GPL options;
check your distribution's build), SDL3 (zlib), Pango/Cairo/Fontconfig/FreeType/HarfBuzz (LGPL/MPL/FTL/MIT)
and the system C++ runtime. SQLCipher (BSD) and OpenSSL (Apache-2.0) come from the distribution.
Shipping binaries that bundle these libraries must keep them replaceable (LGPL) and carry their notices.

## Known gaps

* Verified only on Ubuntu 24.04 x86-64 in a container (X11 under Xvfb, Mesa llvmpipe, a null ALSA
  output, a fake ALSA sequencer client). Not verified: real audio output on PulseAudio, PipeWire or JACK,
  real USB controllers, Wayland, hardware GL drivers, fractional scaling, touchscreens.
* Plugging or removing an audio device does not call `dh_app_audio_route`; restart DeckHand to pick up a new
  default device.
* No Linux packaging (AppImage, Flatpak, `.deb`) and no desktop entry or icon yet.
* Window decorations, clipboard and an input-method preedit are those of SDL3's defaults; only committed
  text reaches the core.
