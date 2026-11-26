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
| Built-in graph executor (`src/stems/graph`, "dhgraph") + `GraphStemModel` | **done, verified against a reference runtime** (see Runtime). `stems_available()` is true, backend name `builtin` |
| ONNX Runtime backend (`OnnxStemModel`, `DemucsCoreModel`) | kept, compiled out behind `DECKHAND_WITH_ONNX` (off); never used by default, never linked |
| Catalogue + user-driven installer + checkpoint converter | done and tested offline; the real download path (system `curl`) is untested end to end |
| Demucs v4 `htdemucs` | skeleton, converter, C++ STFT and executor verified against the reference; **runs end to end in C++** (synthetic 30 s test, see Runtime) |
| Demucs v4 `htdemucs_ft` (bag of four) | bag runner (`pipeline: htdemucs-ft-core`) and installer written; each of the four models verified against the reference; runs end to end in C++ |
| Mel-Band RoFormer vocals | checkpoint parsed; reference implementation identified (below); **no skeleton, no C++ pipeline yet** |
| Engine playback of stems, UI, REST route | Phase 2 / other owners |

## Runtime: the built-in executor

The ONNX Runtime upstream repository is no longer published at its official GitHub location, so there is no official C++ package to
link. DeckHand therefore runs its models on its own small interpreter, `src/stems/graph` ("dhgraph"), with no third-party dependency
(Accelerate on macOS is a system framework).

- **Reader** (`onnx_proto.*`): hand-written protobuf wire-format parser for ModelProto/GraphProto (nodes, initializers with
  external-data location/offset/length, value infos, attributes). Every length, count, depth (16) and size is bounds-checked; unknown
  fields are skipped; sparse initializers and sub-graphs are refused. External-data paths may not be absolute or contain `..`.
  Tested with truncated and corrupted copies of the real skeleton and hand-built hostile files.
- **Operators** (float32, int64, bool; numpy broadcasting; rank up to 8): Add Sub Mul Div Pow Mod Max Min, Equal Less Greater LessOrEqual
  GreaterOrEqual And Or Not Where, Erf Sigmoid Sqrt Sin Cos Tanh Relu Exp Log Reciprocal Floor Ceil Gelu LeakyRelu Clip Neg Abs, Cast
  Constant ConstantOfShape Shape Range Identity, Reshape Flatten Squeeze Unsqueeze Transpose Expand Tile Concat Split Slice Gather ScatterND
  Pad (constant, reflect, edge), ReduceMean/Sum/Prod/Max/Min, Softmax, LayerNormalization, InstanceNormalization, MatMul (batched,
  broadcast), Gemm, Conv and ConvTranspose (1-D and 2-D, strides, pads, dilations, groups, bias). Anything else stops at load with
  `unsupported op X at node N (name)`; shape and type mismatches stop the run with the node named, never a guess. The committed
  `htdemucs` skeleton uses 37 operator types (4,483 nodes, 533 external weights); all are implemented.
- **Speed design**: the first run executes the whole graph, folds everything that depends only on constants and shapes (3,000 of 4,483
  nodes for htdemucs: all the shape arithmetic), records each remaining value's size and last use, and lays all activations out in one
  arena by liveness (views such as Reshape/Unsqueeze share storage). Later runs touch no allocator. A changed input shape re-plans.
  Convolutions are im2col + SGEMM in column tiles (16 MB work buffer), matmuls are SGEMM (`cblas_sgemm` from Accelerate, a portable blocked
  fallback elsewhere; one function in `kernels.cpp`, so OpenBLAS can replace it), elementwise/normalisation/data-movement operators run on a
  small thread pool. The thread cap is the `threads` argument of `open_model()` (0 = min(4, cores)); the system BLAS manages its own threads.
- **Models** (`graph_model.cpp`): `GraphStemModel` for plain tensor models (layouts from the manifest) and for `htdemucs-core` (C++ STFT,
  mask and inverse STFT around the network); `htdemucs-ft-core` runs the four fine-tuned models of the bag (one skeleton, files
  `htdemucs.drums.weights`, `.bass`, `.other`, `.vocals`, converted from the four checkpoints in upstream bag order) and takes stem k
  from model k only, as upstream's bag weights do. The four executors share one activation arena.

**Verification** (all against the PyPI `onnxruntime` Python wheel used only as an oracle on a developer machine; `tools/make_stems_graph_fixtures.py`
and `tools/stems_graph_compare.py` regenerate the fixtures):

| Check | Result |
|---|---|
| 81 single-purpose operator models (all operator families, broadcasting, dtypes, tile borders, 1 and 4 threads, planned and re-run) | match the reference (rtol 2e-4) |
| htdemucs core, 7.8 s synthetic chunk, spectral output | max abs error 5.5e-4 on peak 1.69 (3e-4 of peak), RMS error 1.3e-5 on RMS 0.020 |
| htdemucs core, time-branch output | max abs error 2.3e-5 on peak 0.26, RMS error 3.8e-6 |
| htdemucs_ft models 0-3 (spectral / time) | max abs error 1.8e-4 / 2.2e-5, 7.3e-5 / 2.3e-5, 1.0e-4 / 2.8e-5, 5.9e-5 / 1.7e-5 |
| ft bag vs four single models, per stem | bit-identical (max difference 0) |

The test suite (`test_stems_graph`) compares against committed fixtures of 216 kB in total (no Python needed); the tests that need real
weights run only when `third_party/models` holds the checkpoints (or `DH_STEMS_WEIGHTS` / `DH_STEMS_FT_DIR` point at converted weights)
and skip otherwise.

**End to end** (`deckhand-stems separate`, a synthesised 30 s stereo file: plucked harmonic bass, noise-burst drums, formant tone; the Mac was
under load average 15-35 the whole time, 8 cores, thread cap 4):

| Model | Speed | Peak memory | Result |
|---|---|---|---|
| htdemucs | 30 s of audio in 9.5-10.3 s = 2.9-3.2x real time (about 1.7 s per 7.8 s chunk once planned) | 1.26 GB resident | stems sum to the input within 1e-9; correlation of bass / drums / vocals stems with the true parts 0.995 / 0.987 / 0.998; bass band (below 120 Hz) 100% in the bass stem; largest step at chunk borders about 3x the mean step (no seams) |
| htdemucs_ft | 30 s in 45 s = 0.66x real time (four networks per chunk; load average 35-70) | 2.3 GB resident | sums to the input within 2e-9; see the note below |

Memory: htdemucs needs 168 MB of weights, a 595 MB activation arena and 34 MB of convolution work buffer; the bag holds four weight sets
(672 MB) and one shared arena. The synthetic formant tone is not recognised as "vocals" by the fine-tuned vocals model (the same model, run
alone, files it under "other"), so the ft numbers on this file say nothing about its quality; stem k is verified to come from model k.
Nothing here has been listened to.

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

`"pipeline": "htdemucs-core"` replaces `input`/`output`: the ONNX file is the Demucs network core and the STFT work is done in C++. `"htdemucs-ft-core"` is the same with four weight files next to it (`htdemucs.<stem>.weights`, stems in the manifest order).
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
`install_model` on a worker thread with progress/cancel; a REST route for the same; packaging `resources/stem-models` including
`skeletons/`. `stems_available()` is now true in every build with stems enabled and `backend_name()` reports `builtin`. The app should
use `demucs_converter()` for both Demucs entries (it now handles the four-checkpoint `htdemucs_ft` entry; the catalogue no longer blocks it).

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
**Reference implementation found (not yet used for an export).** `bs-roformer` 0.3.10 on PyPI (MIT, Phil Wang / lucidrains,
https://github.com/lucidrains/BS-RoFormer) loads `MelBandRoformer.ckpt` with `strict` matching and no missing or unexpected keys when built
with dim 384, depth 6, stereo, 1 stem, time/freq transformer depth 1, 60 bands, 8 heads of 64, mask-estimator depth 2, STFT 2048/441
(config inferred from the tensor shapes and key names; newer releases, 0.4 and later, use other module layouts and do not load it).
Plan: export the part between the band-split input and the mask output (STFT, band gather, mask averaging and inverse STFT in C++, like
Demucs), with the library's attention rewritten as MatMul/Softmax so no Einsum is needed; expected extra operators: ReduceL2 (RMS norm),
Clip/Max. The model is 228 M parameters (about 0.9 GB of weights), so it is slow on CPU. None of this is implemented yet.
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

Measured with the built-in executor on a loaded 8-core Apple Silicon Mac (see Runtime): htdemucs about 1.7 s per 7.8 s chunk (steady state),
2.9-3.2x real time including overlap and resampling; htdemucs_ft about 0.66x real time. The first chunk of every model pays about 2.3 s for the
planning run. An idle machine will be faster; Windows and Linux use the portable matrix kernel and will be slower until a BLAS is slotted into
`kernels.cpp`.

## What Phase 2 needs in the engine

- Four-stem playback per deck, sample-locked to the main track: all four stems read from the cache at the deck's position by one
  prefetch thread into lock-free ring buffers (the SPSC types in `core`), same time-stretch/pitch path applied to each stem or to the sum.
- Per-stem mute/solo/volume (and FX sends), applied before the deck's EQ and fader; "all on, unity" must pass the mix through.
  When stems are active the deck can play the stem sum instead of the original file, which is transparent to within 16-bit rounding.
- IPC (`src/audio` protocol and `src/enginehost`): commands for stem gains/mutes per deck, a "stems ready" state and progress events.
- Library: store the cache key per track, queue separation on load when idle, pause the queue while a deck plays.
- The RoFormer skeleton and the combination above (the bag runner for `htdemucs_ft` is done).

## Third-party notices to ship

- No runtime is bundled any more (the built-in executor is DeckHand's own); ONNX Runtime only if the optional `DECKHAND_WITH_ONNX` backend is ever shipped.
- Demucs architecture code (MIT, Meta): the skeleton is generated from it. Weights are NOT part of DeckHand.
- lucidrains BS-RoFormer / Mel-Band RoFormer code (MIT), when its skeleton is added. See `THIRD-PARTY-NOTICES.md`.
