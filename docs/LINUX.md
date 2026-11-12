# DeckHand on Linux

The Linux port is the same C++20 core as the other platforms with a thin desktop shell
(`platform/linux/shell/`: SDL3 window, OpenGL 3.3 core or OpenGL ES 3.0 renderer, Pango text) and two
platform files (`platform/linux/decoder_linux.cpp`, `midi_linux.cpp`). The audio engine runs in a helper process,
`deckhand-engine`, installed beside `deckhand` (see "Engine process" below).

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
scripts/build-linux.sh --package         # also writes build-linux-artifacts/deckhand-linux-<arch>.tar.gz
```

`BUILD_DIR` and `CMAKE_ARGS` override the build folder and add configure options. The shell is
`build-linux/deckhand`, with `deckhand-engine` and `deckhandctl` beside it; resources are copied to
`build-linux/share/deckhand/` at build time.

Without a local toolchain, `scripts/linux/docker-build.sh` builds the Ubuntu 24.04 image
(`scripts/linux/Dockerfile`), copies the tree into a container (works with a remote or VM Docker daemon),
builds, runs every test, the Xvfb screenshot smoke test (`scripts/linux/smoke.sh`: default skin with two and
four decks, the example skin, German), the engine-process check (`scripts/linux/engine-check.sh`) and the
packaging step, and copies the artefacts (screenshots, logs, tarball) to `build-linux-artifacts/`. The build
container gets `--shm-size=1g` (the engine IPC test keeps a 300 s track in `/dev/shm`; Docker's default of 64 MB
is too small) and `--init`. `DECKHAND_SRC=<dir>` builds another tree, for example a `git archive` of a commit.

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
* Engine process: see below. `DECKHAND_ENGINE_PROCESS=separate|inprocess` overrides the setting
  `engine.process`.
* Keys: Ctrl (or Super) takes the role of Command on macOS. Files can be dropped on the window. The
  window picks X11 or Wayland through SDL (`SDL_VIDEODRIVER=x11|wayland` forces one) and follows the
  desktop's display scale.

## Engine process

The output device, recorder and controller (MIDI) hub live in `deckhand-engine`, started by `deckhand` from its own
folder, so the music keeps playing if the window crashes or hangs, and a restarted window re-attaches (ARCHITECTURE.md,
"Engine process"). Linux specifics, all in `src/audio/ipc_posix.cpp`:

* Shared memory: POSIX `shm_open` names `/dh<hash>.m` (main segment) and `/dh<hash>.b<NN>` (one per loaded track,
  unlinked as soon as the engine maps it), i.e. files in `/dev/shm`. Segments are reserved with `posix_fallocate`, so a
  full `/dev/shm` is a refused load ("no room in /dev/shm") and not a SIGBUS in the app. Budget about 21 MB of
  `/dev/shm` per minute of loaded audio per deck (decoded float stereo at 44.1 kHz); a default Linux `/dev/shm` is half
  of RAM, a default Docker one is 64 MB.
* Control socket and instance lock: a Unix domain socket and a lock file in `$XDG_RUNTIME_DIR/deckhand/`, falling
  back to `/tmp/dh-<uid>/` when the session has no runtime folder; `DECKHAND_IPC_DIR` overrides. The folder must be
  owned by the user (mode 0700). Names are hashed from the data folder, so two data folders (or a portable copy)
  run two independent engines.
* The helper is started with every inherited descriptor above stderr closed (Linux has no spawn-time close-on-exec
  default) and in its own session. A helper killed while nobody reaps it (a container whose PID 1 is not an init)
  is detected through `/proc/<pid>/stat`, not only `kill(pid, 0)`.
* A normal quit (window close, SIGTERM, SIGINT, SIGHUP) stops the helper; `kill -9` of the window does not (setting
  `engine.keep_alive_on_quit` also keeps it on a normal quit while a deck plays). A helper that dies is restarted with
  back-off; its decks come back empty.

## Audio output

* No sound card, no sound server, or a device that will not open (containers, headless boxes): the engine reports
  "No audio output" (status line, `deckhand.log`) but keeps running on miniaudio's null backend, which is paced like
  a real device. Decks play silently, the clock advances, the API and every test of the rest of the app work. The
  periodic device check (every 2 s, and whenever the shell calls `dh_app_audio_route`) retries the real device and
  switches over without a restart when one opens; while none does, the null stream is left untouched.
* The shell watches for audio devices coming and going every 2 s (ALSA card list plus the output device names the
  backends report, compared off the UI thread) and calls `dh_app_audio_route` on a change.
* The shell reports the held modifier keys with `dh_app_modifiers` (before pointer and key events, and cleared when
  the window loses focus), so Shift gives fine control on knobs and faders and Shift-click stops a sampler slot.

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

## Packaging

`scripts/build-linux.sh --install PREFIX` (or `cmake --install`) produces:

    bin/deckhand  bin/deckhand-engine  bin/deckhandctl
    share/deckhand/{controllers,lang,skins}
    share/applications/deckhand.desktop      share/icons/hicolor/scalable/apps/deckhand.svg

`deckhand-engine` must stay in the same folder as `deckhand`. The desktop entry and the icon (a plain placeholder in
`platform/linux/packaging/`) are original to this project. `scripts/linux/package.sh` (run by `--package` and by the
Docker build) writes `build-linux-artifacts/deckhand-linux-x86_64.tar.gz`: that layout plus `README-linux.txt`,
which explains portable mode (an empty file `bin/portable`; data then lives in `bin/DeckHandData/`).

The tarball is **not** a static or self-contained build and not an AppImage: it is linked against the build
machine's shared libraries (Ubuntu 24.04 in the Docker build) and needs these from the distribution: SDL3 (3.2+),
Pango, PangoCairo and Cairo, FFmpeg's `libavformat`/`libavcodec`/`libavutil`/`libswresample` (same major version as
at build time), `libasound2`, SQLCipher, OpenSSL 3 (`libssl3`), zlib and the C++ runtime; Noto fonts at run time. Audio
backends are opened with `dlopen` (PulseAudio/PipeWire, ALSA, JACK). The packaging script prints the exact `ldd` list.
A distribution-independent AppImage or Flatpak, and `.deb`/`.rpm` packages, are future work.

## Licences

DeckHand is MIT. It links, dynamically, FFmpeg (LGPL 2.1+ for a distribution build without GPL options;
check your distribution's build), SDL3 (zlib), Pango/Cairo/Fontconfig/FreeType/HarfBuzz (LGPL/MPL/FTL/MIT)
and the system C++ runtime. SQLCipher (BSD) and OpenSSL (Apache-2.0) come from the distribution.
Shipping binaries that bundle these libraries must keep them replaceable (LGPL) and carry their notices.

## Known gaps

* Verified only on Ubuntu 24.04 x86-64 in a container (X11 under Xvfb, Mesa llvmpipe, no sound card so
  the null-output fallback, a fake ALSA sequencer client). Not verified: real audio output on PulseAudio, PipeWire or JACK,
  real USB controllers, Wayland, hardware GL drivers, fractional scaling, touchscreens.
* Device changes reach the engine process through its own 2 s device check, which follows a controller's audio
  interface or reopens a device that went away; a changed default sink while a device is working is not followed by
  DeckHand itself (PulseAudio and PipeWire normally move the stream for a stream on the default device; with plain
  ALSA choose the device in the audio panel). `dh_app_audio_route` only drives the in-process engine.
* Packaging is a tarball and an install layout; no AppImage, Flatpak or distribution packages yet.
* Window decorations, clipboard and an input-method preedit are those of SDL3's defaults; only committed
  text reaches the core.
