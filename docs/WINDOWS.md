# DeckHand on Windows

Windows 10 (1903 or later) and 11, x64. The port is the same C++20 core as the other platforms with a thin Win32
shell (`platform/windows/shell/`: window and input, Direct3D 11 renderer, DirectWrite text) and two platform files
(`platform/windows/decoder_windows.cpp`, `midi_windows.cpp`). Windows-specific core code is `src/audio/ipc_win32.cpp`
(engine-process IPC) and the Winsock paths in `src/api/net.cpp` and `src/broadcast/sock.cpp`.

**Status.** The code builds with MSVC in CI and cross-compiles with mingw-w64 and its tests run under Wine in a container
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
`-DDECKHAND_WIN_PREFIX=<prefix with OpenSSL>` outside Docker.

| Dependency | Version | Licence | Source of the sources |
|---|---|---|---|
| OpenSSL | 3.5.4 | Apache-2.0 | GitHub release, checked against the `.sha256` file the project publishes next to it |
| SQLCipher | 4.20.0 | BSD-3-Clause | vendored in `third_party/sqlcipher` (not fetched by the build), compiled by CMake with FTS5 against OpenSSL's libcrypto |

SQLCipher is the project's own pinned amalgamation (`third_party/sqlcipher/README.md`), the same on every platform and built
with FTS5, so library search has its index here (the vcpkg and distribution packages lack FTS5). `-DDECKHAND_SYSTEM_SQLITE=ON`
switches back to a prefix-provided SQLCipher (`-DSQLCIPHER_ROOT=<prefix>`, header in `include\sqlcipher\`). zlib is not needed.
No FFmpeg: decoding is Media Foundation plus built-in PCM readers.

### B. Native MSVC build with vcpkg (written, not run)

Prerequisites (nothing is installed by the script except vcpkg ports inside the vcpkg folder):
Visual Studio 2022 or Build Tools with "Desktop development with C++" and the CMake component, git, and a bootstrapped
vcpkg (`VCPKG_ROOT`).

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Test -Screenshot
```

It installs `openssl` for the `x64-windows-static` triplet, configures with the Visual Studio
generator and the static runtime, builds `Release`, prints SHA-256 sums of the executables, and optionally runs ctest and
the screenshot.

## Run

```
deckhand.exe [--deck-a FILE] [--deck-b FILE] [FILE...] [--screenshot FILE.png [--frames N] [--size WxH]]
deckhandctl.exe devices | midi | hid | import | list | export ...
```

* Data (library, settings, logs): `%APPDATA%\DeckHand`. **Portable mode:** an empty file named `portable` beside
  `deckhand.exe` keeps everything in `DeckHandData\` next to it.
* Resources (`controllers\`, `lang\`, `skins\`): `share\deckhand\` beside the exe (the build copies them to
  `<build>\share\deckhand`), then the exe's own folder, then `..\share\deckhand`; `DECKHAND_RESOURCE_DIR` overrides.
* `DECKHAND_LOG=error|warn|info|debug|trace` sets the log level (`deckhand.log` in the data folder).
* `--screenshot` renders N frames (default 30) into an offscreen Direct3D target and writes a PNG without opening a
  window; it uses the GPU or WARP (Microsoft's software rasteriser). `DECKHAND_SCALE=2` renders at 200 %: `--size` is the logical
  size, the PNG is size x scale pixels (see HiDPI).
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

## Engine process on Windows

`engine.process = separate` (the default on Windows, `DECKHAND_ENGINE_PROCESS` overrides) runs the audio engine in
`deckhand-engine.exe` beside the app, as on macOS and Linux: if the UI crashes, is killed or hangs, the music keeps
playing, and a restarted UI re-attaches to the running set (decks, mixer, recording). If the helper cannot start the app
falls back to in-process audio. `ARCHITECTURE.md` ("Engine process") describes the design; the Windows specifics:

* **Starting.** `spawn_detached` (`ipc_win32.cpp`) calls `CreateProcessW` with `DETACHED_PROCESS` (no console, so no
  window flashes up; the helper is a console-subsystem exe that never gets one), `CREATE_NEW_PROCESS_GROUP`, and
  `CREATE_BREAKAWAY_FROM_JOB` when the UI's job allows it (a launcher that wraps the UI in a kill-on-close job must not take
  the engine along; a job that refuses breakaway gets the plain call). There is deliberately **no job object of our own**:
  the helper's life is not tied to the UI's. Handles are not inherited except an explicit list of two (NUL as stdin and the
  helper's log file as stdout/stderr, `<data>\deckhand-engine.out`), through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`: the UI's
  sockets, mappings and lock file never leak into the helper. Sockets and the lock handle are also marked non-inheritable.
* **Crash vs quit.** A UI ended with `TerminateProcess` (Task Manager "End task" on a hung window, `Stop-Process -Force`, a
  fault) leaves the helper playing. A normal quit tells the helper to fade out and exit: closing the window (`WM_CLOSE`),
  `WM_ENDSESSION` on logoff or shutdown (the shell destroys the app, which stops the helper, then exits), and Ctrl+C,
  Ctrl+Break or a console close when the app was started from a console (`ipc::set_quit_handler`, the SIGTERM/SIGINT/SIGHUP
  analogue in `ipc_win32.cpp`). `engine.keep_alive_on_quit` keeps it while a deck plays, as elsewhere.
* **Helper side.** `SetConsoleCtrlHandler` turns Ctrl+C, logoff and shutdown into the same fade-out and exit; an open
  recording is finalised (the WAV header is patched) on every orderly exit, including the quit request of a UI that is
  closing. There is no named "quit" event: the control channel's quit request does that job, and an outside tool that wants
  the helper to stop uses the same request (or `taskkill /F`, which counts as a crash and leaves a WAV playable up to the
  last once-a-second header refresh).
* **One engine per data folder.** The single-instance lock is a file opened with no sharing, in
  `%LOCALAPPDATA%\DeckHand\ipc\<hash>.lock`, named from the user and the lower-cased canonical data folder; the OS drops it
  when the helper dies however it dies. (A named mutex would be equivalent; the lock file also carries `DELETE` access so a
  clean exit removes it.) Two portable copies on different folders do not collide.
* **Stale objects.** Windows removes a named file mapping when its last handle closes, so a killed UI or helper cannot leave
  segments behind; a stale lock or socket file (a port and secret) is simply replaced by the next helper. A helper
  restarted by the UI re-creates everything.
* **Limits.** The deck contents of a *restarted* helper are empty (a crash of the helper itself loses the loaded tracks, as
  elsewhere). If the helper is killed while the UI is gone, nobody restarts it until the next UI starts.

### Checking it

* `test_windows_ipc` (`platform/windows/tests/ipc_test.cpp`) starts the real `deckhand-engine.exe` on the null audio device:
  shared-memory load, a second client attaching, `TerminateProcess` of the helper and its restart, **a UI process that plays
  and records and is killed with `TerminateProcess` while the helper keeps going, a new client attaching and finding the
  position advanced by the wall-clock gap (within 0.75 s), the recording that spanned the kill closing as a valid WAV, and an
  orderly quit that finalises an open recording and leaves no segment, socket file or lock**.
* `scripts/windows/engine-check.ps1` runs the **real `deckhand.exe`** with its window against a portable data folder and
  drives it through the REST API (token from `<data>\api-admin-token.txt`, master muted first): helper started and windowless,
  UI killed, helper survives, relaunched UI re-attaches with the position continued, killed helper restarted, `WM_CLOSE`
  stops the helper and `deckhand-engine --probe` shows no object left. It runs in the CI job "Windows (MSVC)". With no sound
  device the engine runs on miniaudio's null device (paced like a real one). `scripts/windows/engine-check-wine.sh` is the same
  under Wine in the Docker build (`docker-build.sh` runs it after the tests).

Measured: on the GitHub `windows-latest` runner (MSVC Release, real Windows, no audio device) `engine-check.ps1` passes every
step with the default mode (position continued +4.6 s across a 4 s UI kill, helper restarted in under 3 s, quit stops it);
under Wine the same steps pass and `test_windows_ipc` reports a deck that advanced 2.52 s over a 2.53 s kill gap, a 3.95 s WAV
that spanned the kill and a 1.56 s WAV finalised by the orderly quit. What only a real PC can show: a WASAPI device (and the
Remote Desktop "Remote Audio" device, which appears and disappears with the session), real MIDI/HID hardware, and sleep/resume.

## Changes needed in src/app (done unless listed)

* Done: `DH_HAVE_ENGINE_PROCESS` is on for `_WIN32` (the POSIX signal pipe stays POSIX-only; Windows uses
  `ipc::set_quit_handler`), `host_name()` no longer calls `gethostname` there, and `RemoteEngine` finds
  `deckhand-engine.exe` beside the app.
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

## HiDPI (per-monitor v2)

Reviewed against the Win32 rules; **none of it can be exercised on CI or under Wine** (one monitor, 96 DPI): it needs a
real PC with a scaled display and, for `WM_DPICHANGED`, two monitors with different scaling (or changing the scale in
Settings while the app runs).

* Awareness comes from the manifest (`PerMonitorV2`) and `SetProcessDpiAwarenessContext(-4)` as a fallback; the scale is
  `GetDpiForWindow / 96`. The core lays out in logical units and multiplies by the scale; text is rasterised by DirectWrite at
  `size x scale` pixels (the glyph cache is keyed by that pixel size, so a scale change draws fresh glyphs).
* `WM_DPICHANGED`: the stored scale is updated first, then the window is moved and resized to the rectangle Windows
  suggests (so the monitor under the pointer keeps the same logical size); the resulting `WM_SIZE` resizes the swap chain
  (`ResizeBuffers`) and the next frame is laid out at the new scale. Non-client areas scale automatically under v2;
  outer sizes are computed with `AdjustWindowRectExForDpi`.
* Fixed in this round: the minimum window size (1100x700 DIPs) is now computed from the window's *current* DPI inside
  `WM_GETMINMAXINFO` (which arrives before `WM_DPICHANGED` when a window crosses monitors) and is capped to the monitor's work
  area (at 150 % on a 1080p panel the minimum used to be taller than the screen); `WM_ENDSESSION` quits cleanly.
* `--screenshot` with `DECKHAND_SCALE=2` now renders a canvas of **logical size x scale** (`--size 1440x900` gives
  2880x1800 pixels), like a real window on a 200 % display; before, the canvas stayed 1440x900 pixels and the layout was
  cramped. The Linux shell has no such debug scale (its screenshot is the real window's pixel size), so it is unaffected.

## Tests

`ctest` runs every `tests/test_*.cpp` plus `test_windows_decoder`, `test_windows_midi`, `test_windows_hid`,
`test_windows_text` and `test_windows_ipc`. `test_windows_hid` exercises the HID backend without a device (enumeration with
criteria that match nothing queries and opens nothing, the keyboard/mouse refusal rules, matching, the device-path parser,
unknown ids failing cleanly); it needs no hardware and runs on CI and under Wine. `deckhandctl hid` lists the HID devices
that are not keyboards or mice (vid:pid, usage page/usage, product); it opens nothing.

`test_analysis` times a 6-minute synthetic track as a sanity bound, not a benchmark: it must run at least 30x faster than
real time (under 12 s), and the measured time and factor are printed. The old fixed 3 s limit failed at 3.3 s on a desktop PC
under MSVC Release while every accuracy check passed. MSVC Release already builds with `/O2 /Ob2` (the equivalent of the
`-O3` of the other platforms) and no compiler in this project uses fast-math, so no flags changed. Under Wine two environment variables mark what Wine lacks: `DECKHAND_TEST_NO_MF=1` (skip the
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
