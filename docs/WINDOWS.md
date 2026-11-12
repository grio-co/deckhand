# DeckHand on Windows

Windows 10 (1903 or later) and 11, x64. The port is the same C++20 core as the other platforms with a thin Win32
shell (`platform/windows/shell/`: window and input, Direct3D 11 renderer, DirectWrite text) and two platform files
(`platform/windows/decoder_windows.cpp`, `midi_windows.cpp`). Windows-specific core code is `src/audio/ipc_win32.cpp`
(engine-process IPC) and the Winsock paths in `src/api/net.cpp` and `src/broadcast/sock.cpp`.

**Status.** The code cross-compiles with mingw-w64 and its tests run under Wine in a container
(`scripts/windows/docker-build.sh`). Nothing of it has run on real Windows yet: WASAPI output, real MIDI hardware, the
MSVC build (`scripts/build-windows.ps1`), GPU drivers, per-monitor DPI changes and the IME are unverified.

## Build

### A. Cross-build with MinGW and test under Wine (verified)

```sh
scripts/windows/docker-build.sh               # image, build, every test under Wine, screenshot, artefacts
```

`scripts/windows/Dockerfile` (Ubuntu 24.04) holds the toolchain (mingw-w64 GCC 13, POSIX threads, Wine 9, Xvfb) and
the dependencies built for `x86_64-w64-mingw32` into `/opt/win64`, statically. The result is in
`build-windows-artifacts/` (git-ignored): `bin/*.exe` with `SHA256SUMS`, one log per test, `tests.txt`,
`shell_x1.png` / `shell_x2.png`. `cmake/toolchain-mingw64.cmake` is the toolchain file; use it directly with
`-DDECKHAND_WIN_PREFIX=<prefix with zlib, OpenSSL, SQLCipher>` outside Docker.

| Dependency | Version | Licence | Source of the sources |
|---|---|---|---|
| zlib | 1.3.1 | zlib | zlib.net, SHA-256 pinned in the Dockerfile |
| OpenSSL | 3.5.4 | Apache-2.0 | GitHub release, checked against the `.sha256` file the project publishes next to it |
| SQLCipher | 4.6.1 | BSD-3-Clause | GitHub tag archive (no published checksum; the build prints and stores the archive's SHA-256) |

SQLCipher is built from its amalgamation (`make sqlite3.c` natively, then one cross-compile with
`-DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_OPENSSL`) into a static `libsqlcipher.a` against OpenSSL's libcrypto.
No FFmpeg: decoding is Media Foundation plus built-in PCM readers.

### B. Native MSVC build with vcpkg (written, not run)

Prerequisites (nothing is installed by the script except vcpkg ports inside the vcpkg folder):
Visual Studio 2022 or Build Tools with "Desktop development with C++" and the CMake component, git, and a bootstrapped
vcpkg (`VCPKG_ROOT`).

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Test -Screenshot
```

It installs `openssl`, `zlib` and `sqlcipher` for the `x64-windows-static` triplet, configures with the Visual Studio
generator and the static runtime, builds `Release`, prints SHA-256 sums of the executables, and optionally runs ctest and
the screenshot. If vcpkg's `sqlcipher` port puts `sqlite3.h` somewhere other than `include\sqlcipher\`, point
`-DSQLCIPHER_ROOT=<prefix>` at a prefix that has it there.

## Run

```
deckhand.exe [--deck-a FILE] [--deck-b FILE] [FILE...] [--screenshot FILE.png [--frames N] [--size WxH]]
deckhandctl.exe devices | midi | import | list | export ...
```

* Data (library, settings, logs): `%APPDATA%\DeckHand`. **Portable mode:** an empty file named `portable` beside
  `deckhand.exe` keeps everything in `DeckHandData\` next to it.
* Resources (`controllers\`, `lang\`, `skins\`): `share\deckhand\` beside the exe (the build copies them to
  `<build>\share\deckhand`), then the exe's own folder, then `..\share\deckhand`; `DECKHAND_RESOURCE_DIR` overrides.
* `DECKHAND_LOG=error|warn|info|debug|trace` sets the log level (`deckhand.log` in the data folder).
* `--screenshot` renders N frames (default 30) into an offscreen Direct3D target and writes a PNG without opening a
  window; it uses the GPU or WARP (Microsoft's software rasteriser). `DECKHAND_SCALE=2` renders at 200 %.
* Keys: Ctrl takes the role of Command on macOS. Files can be dropped on the window (an elevated DeckHand still
  accepts drops). The source library of another DJ application is read from `%APPDATA%\Pioneer\rekordbox\master.db`.
* Narrow strings are UTF-8 process-wide: the executables carry a manifest with `activeCodePage=UTF-8` and
  `longPathAware` (Windows 10 1903+); wide Win32 calls are used wherever the code opens files itself.

## Design choices

* **Windowing: plain Win32** (`wWinMain`, per-monitor-v2 DPI awareness from the manifest, scale = DPI / 96,
  `WM_DPICHANGED`, size-move loop kept alive with a timer). Text input is `WM_CHAR` (surrogate pairs joined); the IME's
  composition window is placed at the last click and its result arrives as `WM_CHAR`. Drops are `WM_DROPFILES`;
  OLE drag and drop (virtual files) is future work.
* **Rendering: Direct3D 11**, flip-model swap chain (falls back to the discard model), one vertex/pixel shader pair
  compiled at start with `D3DCompile` from `d3dcompiler_47.dll` (loaded at run time; it ships with Windows 10),
  vertex colour times texel with A8 textures giving alpha only, a scissor rectangle per command. The device is the
  GPU, else WARP.
* **Text: DirectWrite** (`IDWriteTextLayout`, system font fallback) with glyph-run analysis into an A8 bitmap using the
  metrics conventions of the other shells (ceil(ascent)+1 baseline, ceil(w)+2 by ceil(a)+ceil(d)+2). Family is
  Segoe UI.
* **Audio output: miniaudio, WASAPI shared mode** (runtime-linked, so no import libraries). Exclusive mode and ASIO
  are future work; the period is the `audio.period` setting.
* **Decoding: Media Foundation** Source Reader for MP3, AAC/M4A, FLAC and the rest, float output at the file's own
  rate; **PCM WAV, AIFF and AIFC are read by built-in parsers** (Media Foundation has no AIFF; reading uncompressed audio
  directly is faster and independent of installed codecs). Ogg Vorbis and Opus are not available without FFmpeg
  (future work: bundle libvorbisfile / opus, or a Windows Store codec check).
* **MIDI: WinMM** (`midiIn*` / `midiOut*`). Inputs open as they appear, an output of the same name opens beside it.
  Hot-plug is a one-second poll of the device list. Limits of WinMM: device names are cut to 31 characters, a
  device can usually be opened by one process at a time, no BLE MIDI and no MIDI 2.0. WinRT MIDI (Windows.Devices.Midi)
  and Windows MIDI Services are future work.
* **Engine process IPC** (`ipc_win32.cpp`): named, pagefile-backed file mappings in the session namespace
  (`Local\dh<hash>.m`, `.bNN`); a lock file opened without sharing as the single-instance lock; a loopback TCP control
  channel whose port and a random 128-bit secret sit in a per-user file (`%LOCALAPPDATA%\DeckHand\ipc\<hash>.sock`, ACL
  restricted to the owner) and whose first message is the secret; `CreateProcessW` with an explicit inherited-handle list
  to start `deckhand-engine.exe` detached; the process handle table gives `reap_child`. TerminateProcess is the
  SIGKILL equivalent, a console control handler the SIGTERM one.
* **Token and key files** (`api-tokens.json`, the TLS key) are created with a protected ACL granting the owner and SYSTEM
  only. In portable mode on a FAT/exFAT stick there are no ACLs: keep the stick yours.
* **Winsock**: `src/api/sockport.hpp` and `src/broadcast/sock.cpp` hide the differences (WSAPoll, `closesocket`,
  `ioctlsocket`, a loopback socket pair instead of a pipe for wake-ups).

## Engine mode on Windows

`src/app/app.cpp` enables the separate engine process only under `__unix__`/`__APPLE__`, so a Windows build runs the
engine **in-process** until that guard learns `_WIN32` (see "Changes needed in src/app" below). The helper,
`deckhand-engine.exe`, is built and tested (`test_windows_ipc`).

## Changes needed in src/app (not done here: another agent owns that tree; line numbers are those of commit 08929d9)

* `app.cpp:10-11`: add `|| defined(_WIN32)` to the `DH_HAVE_ENGINE_PROCESS` condition (this is what turns the engine process on),
  keep the `<fcntl.h>/<signal.h>/<unistd.h>` includes POSIX-only, and give `start_signal_watch()` / `stop_signal_watch()`
  (`app.cpp:~228-250`, they use `pipe`, `sigaction`) a Windows branch: nothing is needed, because a GUI process is told to quit
  through `WM_CLOSE`, which the shell turns into `dh_app_destroy`, which stops the engine. The default mode string
  (`app.cpp:~135`, `"inprocess"` outside macOS) can become `"separate"` once WASAPI has run on a real machine.
* `app_impl.hpp:275-276` (`source_db()`): on Windows return `%APPDATA%\Pioneer\rekordbox\master.db`
  (`std::getenv("APPDATA")`), and the `share` folder beside it for the analysis files.
* `app_panels.cpp:124`: the `HOME`-based "Music" export target is empty on Windows (use `USERPROFILE`). Nothing else: the
  shell supplies `list_export_targets` (removable drives and non-system fixed drives, authoritative) and `choose_folder`
  (`IFileOpenDialog`); the `/media` scan at `:113-115` is skipped because the hook answers authoritatively.
* `app_panels.cpp:163` uses `std::strlen` without `<cstring>`; `app.cpp:746` and `app_impl.hpp:115` use `localtime_r`;
  `app_impl.hpp:54` uses `__attribute__((format))`; `api_bridge.cpp:759` calls `::chmod`. All four are papered over by the
  force-included `platform/windows/compat/dh_compat.h`; the real fixes are the include, `localtime_s`, a format-attribute
  macro, and guarding the `chmod` (token files are already owner-only through `src/api/fileio.cpp`).
* `app_panels.cpp:850` hint text `/Volumes/...` for the relink field is a macOS path shown on every platform.

## Tests

`ctest` runs every `tests/test_*.cpp` plus `test_windows_decoder`, `test_windows_midi`, `test_windows_text` and
`test_windows_ipc`. Under Wine two environment variables mark what Wine lacks: `DECKHAND_TEST_NO_MF=1` (skip the
Media Foundation codec checks) and `DECKHAND_TEST_LENIENT=1` (skip text samples for scripts without an installed font).
`test_engine_ipc` runs its portable parts only on Windows (its fork/SIGKILL parts have no equivalent);
`test_windows_ipc` covers the Windows IPC layer and the engine process, including an engine ended with TerminateProcess.

## Verification status

Cross-built with mingw-w64 GCC 13 (no warnings under `-Wall -Wextra`) and run under Wine 9.0 with Xvfb in the container:
all 18 suites pass (the 14 OS-independent ones, `test_engine_ipc` in its portable form, and the four Windows ones).
What Wine cannot show: Media Foundation codecs (MP3/AAC/FLAC decoding is **not exercised**; the PCM readers are), a WASAPI
device (the output falls back to miniaudio's null device), MIDI hardware (the list is empty), a real GPU (wined3d on Mesa
llvmpipe), per-monitor DPI changes, the IME. Wine has no Segoe UI, so text rendered with DejaVu Sans in the screenshots; the
font choice falls back through a preference list.

Wine needs a UTF-8 locale (`LANG=C.UTF-8`) to create non-ASCII file names; the container script sets it.

## Not done / future work

WASAPI exclusive mode and ASIO; WinRT / Windows MIDI Services and BLE MIDI; OLE drag and drop; Ogg Vorbis / Opus decoding;
an installer, code signing and automatic updates; high-contrast and dark-title-bar polish; an actual GPU / HiDPI
verification.
