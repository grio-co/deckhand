# Stem separation (vocals / drums / bass / other)

DeckHand separates **offline, ahead of time** (and in the background on first load), stores the four stems on disk, and
lets the engine later play four pre-rendered stems. There is no live inference cost and nothing has to be analysed from the
start of the track. This document covers Phase 1 (everything up to a tested offline separator and cache). The real-time
engine (`src/audio`) is untouched.

## Status at a glance

| Part | State |
|---|---|
| Chunking, overlap-add, resampling, exact mixture consistency (`separate`) | done, tested with test models |
| Disk cache (content-hash keys, LRU cap, atomic writes, verify) | done, tested |
| Background queue (one job, low priority, pause/resume/cancel) | done, tested |
| Model slot (`StemModel`) and manifest format (`*.dhstems.json`) | done, tested |
| ONNX Runtime backend (`OnnxStemModel`, `DemucsCoreModel`) | **written, not compiled or run**: the official runtime could not be downloaded (see below) |
| Catalogue + user-driven installer + checkpoint converter | done and tested offline; the real download path (system `curl`) is untested end to end |
| Demucs v4 `htdemucs` | skeleton, converter and C++ STFT verified against the reference; **no C++ end-to-end run** (no runtime) |
| Demucs v4 `htdemucs_ft` (bag of four) | catalogued, not installable yet (needs a bag runner) |
| Mel-Band RoFormer vocals | checkpoint downloaded and parsed; **no skeleton or C++ pipeline yet** |
| Engine playback of stems, UI, REST route | Phase 2 / other owners |

**Runtime blocker.** `github.com/microsoft/onnxruntime` (repository, releases and raw files) answered HTTP 404 from this
machine on 2026-11-26, although PyPI still lists the project. The PyPI wheel contains only `libonnxruntime.dylib`, no C/C++
headers, so nothing could be compiled against it. `scripts/fetch-stems-runtime.sh` is written and refuses to install
anything until `scripts/stems-runtime.sha256` has pinned hashes (it is empty on purpose). All ONNX code is guarded by
`DECKHAND_WITH_ONNX`, which turns itself off when `third_party/onnxruntime` is absent, so the default build and CI are
unaffected. The ONNX models and the Demucs core were validated with the PyPI `onnxruntime` 1.32.0 Python wheel instead.

## Licence position (why no model ships)

DeckHand ships, mirrors and re-hosts **no weights and nothing derived from them**. A model is installed on the user's own
machine, by the user's explicit action, straight from the upstream author's host, after the app shows the licence statement and
the user confirms that their use is within it.

| Candidate | Code licence | Weights licence (what was read) | Training data | Result |
|---|---|---|---|---|
| Demucs v4 (`htdemucs`, `htdemucs_ft`) | MIT (`gh api repos/facebookresearch/demucs/license`) | Not MIT. The Demucs author states in upstream issue 327 that the weights "are not covered by the MIT license, and are provided only for scientific purposes". README only says "Demucs is released under the MIT license". HF model cards under `adefossez/` carry no licence field. | MUSDB18-HQ (academic) + 800 songs | Research-only. Catalogued with a warning; user installs on own responsibility. |
| Mel-Band RoFormer vocals (Kimberley Jensen) | none stated in the author's GitHub repo; architecture by lucidrains (MIT) | Hugging Face model card of the author, `KimberleyJSN/melbandroformer`: `license: mit`. | Author's private dataset plus contributed material; undisclosed | Declared MIT. Catalogued with a note that training data is unknown. |
| Open-Unmix `umxl` | MIT | README: CC BY-NC-SA 4.0, non-commercial | private | Rejected |
| Open-Unmix `umxhq` / `umx` | MIT | README silent for these weights; Zenodo (not an allowed host) | MUSDB18 (academic) | Unclear, not used |
| Spleeter 2/4/5 stems | MIT | README says the *code* is MIT; weights not addressed; maintainers have not answered issues 898, 957 and 959 | Deezer internal | Unclear, not used |

**Warning.** Research-only weights (Demucs) may not cover paid gigs, streaming, or any commercial use, and "declared MIT" weights
trained on undisclosed data carry unknown risk. Whether your use is allowed is **your responsibility**; this is not legal
advice. DeckHand only moves the files onto your machine after you accept the statement.

## Model slot and manifest

`StemModel` (`stems.hpp`): `info()` returns id, sample rate, segment length, channels; `run(in, out)` takes planar stereo
`[2][segment]` and returns `[4][2][segment]` in canonical order vocals, drums, bass, other. Separation works with any
implementation, so tests use fixed-gain splitters.

A model is installed by placing `<name>.dhstems.json` plus its file(s) in `<data>/models/`:

```json
{
  "format": "deckhand-stems-model/1",
  "id": "my-model-1.0",              // [A-Za-z0-9._-], part of every cache key
  "name": "Display name",
  "file": "my-model.onnx",           // relative to the manifest; at most one sub-folder, no "..", no absolute path
  "licence": "licence string of the weights",
  "source_url": "https://where.the.weights.came.from/",
  "sha256": "<64 hex of the model file>",   // verified before every load; mismatch = refused
  "sample_rate": 44100,
  "segment_frames": 343980,          // or "segment_seconds"
  "overlap": 0.25,                   // optional, 0..0.5
  "channels": 2,
  "input":  { "name": "mix", "layout": "BCT" },            // letters B batch, C channel, T time (B optional)
  "output": { "name": "stems", "layout": "BSCT" },         // S = stem axis; or "names": [4 tensors] with a layout without S
  "stems": ["other","bass","drums","vocals"],              // the model's output order (a permutation of the four names)
  "execution_provider": "cpu"                              // or "coreml"
}
```

`"pipeline": "htdemucs-core"` replaces `input`/`output`: the ONNX file is the Demucs network core and the STFT work is done in C++.
Tensor-layout handling is tested with three generated models (`tools/make_stems_test_models.py`, 241 to 304 bytes each).

## Install flow (API for the app)

Header `deckhand/stems/model_install.hpp`; catalogue entries are metadata only (`resources/stem-models/*.json`).

```cpp
std::vector<CatalogueEntry> list_catalogue(dir);
InstallResult install_model(data_dir, entry, accept_licence, progress, cancel, hooks);  // refuses unless accept_licence
std::vector<InstalledModel> installed_models(data_dir);
bool remove_model(data_dir, id);
std::function<...> demucs_converter(data_dir, skeleton_dir);                              // InstallHooks::convert for Demucs
```

Flow: show `licence_summary` verbatim and the source link, require the user's confirmation, check the host against an allow-list
(`dl.fbaipublicfiles.com`, `huggingface.co`, `github.com`, https only), download to `*.part`, check size and SHA-256 (mandatory, from the
catalogue), rename, convert, write the manifest, delete the download. The default downloader runs the system `curl` (macOS,
Windows 10+, Linux) with TLS verification and https-only redirects: no new library. The repo's own TLS client does not verify
certificates, so it was not used. A native client (NSURLSession / WinHTTP) would be a better long-term home.

**Wiring needed (not mine to edit):** a settings screen listing `list_catalogue()` with the licence text and a checkbox, calling
`install_model` on a worker thread with progress/cancel; a REST route for the same; `stems_available()` gating (do not offer a
download when the build has no runtime); packaging `resources/stem-models` including `skeletons/`.

## No Python at the end user: skeletons

`tools/export_stems_skeleton.py` (developer-run) exports the Demucs network core to ONNX with every weight as an external-data
reference, and writes `htdemucs.layout.json` (checkpoint key, offset, length per tensor). Committed: `resources/stem-models/skeletons/`
`htdemucs.onnx` (6.6 MB, graph only) and the layout (50 KB). On the user's machine `convert_checkpoint()` reads the downloaded
PyTorch file with a hardened reader (`checkpoint.hpp`: zip, then a pickle interpreter with a fixed opcode and global allow-list
and size limits; nothing is imported or executed; tested against `os.system`, `eval`, `NEWOBJ`, truncation, traversal) and writes
`htdemucs.weights`. The reader's tensors were compared with `torch.load` for the RoFormer file (sums and first values equal). The
converter's output is byte-identical to the reference weights file (SHA-256 `98362ed7...799a`). The Demucs STFT, mask and inverse STFT
are reimplemented in `demucs_dsp.cpp` (max abs error 4.5e-8 against `torch`, full 7.8 s chunk).

Numeric check of the whole chain (Python ORT, skeleton + C++-written weights vs torch, random noise input): spectral branch max abs
error 2.2e-4 (peak 0.27), time branch 4.1e-6 (peak 0.17). One 7.8 s chunk took 2.5 s on this heavily loaded machine.

## Combining RoFormer and Demucs (design)

1. Run the RoFormer vocal model on the mix: `v` (vocals), `i = mix - v` (instrumental).
2. Run Demucs on **`i`**, take drums, bass and other from it (its vocals output is discarded or used as a residual check).
3. Mixture consistency at the end on the original mix: the existing proportional step distributes `mix - (v + d + b + o)` and then closes
   `other` exactly. The stems sum to the input, so "all on" is transparent.
Cost: two models per track (about 1.3x the time of Demucs alone with RoFormer's size, more on CPU). Not implemented: needs the RoFormer
skeleton and its mel-band STFT pipeline.

## Disk cache

`<data>/stems/<key[0..1]>/<key>.dhstems`, key = SHA-256 over the 16-bit-quantised decoded audio, its rate and length, and the model id
(not the path). 128-byte header, planar stem-major 16-bit PCM (24-bit optional; a float scale in the header prevents clipping of stems
louder than full scale), 16-byte trailer with CRC-32. Writes go to a temp name, are flushed and renamed; truncated files are removed on
rescan and never served; `verify()` checks the CRC. LRU by file mtime with a configurable byte cap (default 20 GiB); the entry just
stored is never evicted. Streaming reads (`StemReader::read`) are thread-safe; memory-mapping is a Phase 2 option.

Disk use (computed from the format, checked in tests): **42.3 MB per minute** of stereo 44.1 kHz audio at 16-bit (63.5 MB at 24-bit), so a
4-minute track is about 169 MB. A lossless codec could roughly halve this; not done.

## Speed

Measured only through Python ORT (no C++ runtime yet): htdemucs, one 7.8 s chunk in 2.5 s on a machine at load 100-150. With 25% overlap
that is roughly 2.3x real time; `htdemucs_ft` would be about four times slower. These numbers are pessimistic. C++ numbers are not measured.

## What Phase 2 needs in the engine

- Four-stem playback per deck, sample-locked to the main track: all four stems read from the cache at the deck's position by one
  prefetch thread into lock-free ring buffers (the SPSC types in `core`), same time-stretch/pitch path applied to each stem or to the sum.
- Per-stem mute/solo/volume (and FX sends), applied before the deck's EQ and fader; "all on, unity" must pass the mix through.
  When stems are active the deck can play the stem sum instead of the original file, which is transparent to within 16-bit rounding.
- IPC (`src/audio` protocol and `src/enginehost`): commands for stem gains/mutes per deck, a "stems ready" state and progress events.
- Library: store the cache key per track, queue separation on load when idle, pause the queue while a deck plays.
- A bag runner for `htdemucs_ft`, the RoFormer skeleton and the combination above.

## Third-party notices to ship

- ONNX Runtime (MIT, Microsoft): only once the runtime is bundled. Text in `third_party/onnxruntime/LICENSE` and `ThirdPartyNotices.txt` of the release.
- Demucs architecture code (MIT, Meta): the skeleton is generated from it. Weights are NOT part of DeckHand.
- lucidrains BS-RoFormer / Mel-Band RoFormer code (MIT), when its skeleton is added. See `THIRD-PARTY-NOTICES.md`.
