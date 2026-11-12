# DeckHand roadmap

CDJ-3000 hardware test is deliberately last (M9). Until then, export is checked offline
against a reference export plus a read-back parser.

Strategy: macOS first, made robust; then Windows, then Linux. The core stays platform-neutral
(no platform headers outside platform/; audio, MIDI, paths behind interfaces). At the end of
each milestone, build and run tests on the user's Windows and Linux machines. CI and the Win/Linux
shells come after the Mac version is solid.

Status: a first working macOS build exists (library import, engine, generic controllers with a
DDJ-FLX4 mapping, core-drawn UI). It took two shortcuts, both tracked in TODO.md: the engine runs
in-process, and the default layout is drawn in code until the skin engine lands. Hardware testing
with the FLX4 is the next step. Controller support is general (mapping files); the FLX4 is simply
the first device that can be tested.

1. M1 Import + library: read-only import of a copy of the source DB into DeckHand's SQLite; portable path layer.
2. M2 UI shell: custom GPU toolkit (gfx), skin engine, built-in default skin, i18n, browser, playlists, search.
3. M3 Audio engine: separate process, two decks, waveforms, cues, loops, sync; API protocol; recording; history log.
4. M4 DDJ-FLX4: MIDI mapping, built-in audio interface (home hardware).
5. M5 Effects + mixer: full parity and beyond, one effect at a time, tested offline.
6. M6 Streaming: OBS WebSocket, now-playing, stream output bus.
7. M7 Analysis: own BPM, beatgrid, key, waveform.
8. M8 CDJ export: PDB / exportExt.pdb / ANLZ writer, verified offline + read-back.
9. M9 CDJ-3000 test: the only hardware check of the export.
10. M10 Cloud sync.

Settled: shared C++ core with Swift (macOS) / C++ (Win, Linux) shells; custom GPU UI; MIT;
Familiar-layout default skin (original assets) plus fully skinnable UI; all formats (MP3, FLAC,
WAV, AIFF, AAC); portable mode; DeckHand-owned library; REST + WebSocket API; 4+1 log levels.
Still open: GPU backend choice (WebGPU/Dawn vs native Metal/D3D12/Vulkan), at M2.
