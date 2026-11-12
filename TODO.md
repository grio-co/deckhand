# TODO

Living list; milestones match [ROADMAP.md](ROADMAP.md). Tick items as they land and mirror
them in [CHANGELOG.md](CHANGELOG.md).

## Groundwork
- [x] Repository, CMake skeleton, module layout, licence
- [x] Draft library schema
- [ ] Test data (fake source library, royalty-free tracks)
- [ ] Choose test framework (Catch2 or doctest)
- [ ] CI on macOS / Windows / Linux (after the Mac version is solid)

## M1 Import + library
- [ ] Library storage layer (SQLite) from `docs/SCHEMA.sql`, with migrations
- [ ] Portable mode path layer (root + relative path, relink tool)
- [ ] Read-only importer for the source DJ library (works on a copy)
- [ ] `deckhandctl import` / `list`

## M2 UI shell
- [ ] GPU backend decision (WebGPU/Dawn vs native Metal/D3D12/Vulkan)
- [ ] gfx toolkit: renderer, text, layout, widgets
- [ ] Skin engine + built-in default skin
- [ ] i18n modules (`.dhlang`)
- [ ] Browser, playlists, search

## M3 Audio engine
- [ ] Separate engine process + API protocol, recording, history log

## Later
- [ ] M4 DDJ-FLX4 · M5 effects/mixer · M6 OBS streaming · M7 analysis · M8 CDJ export
- [ ] M9 CDJ-3000 test (needs reference USB export from the owner) · M10 cloud sync
- [ ] Windows and Linux shells (after the Mac version is solid)

## Open questions
- GPU backend; Swift/C++ interface design; packaging and updates
