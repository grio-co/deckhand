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
| Mel-Band RoFormer vocals | skeleton (`skeletons/roformer.*`), converter, C++ STFT / band gather / mask averaging / inverse STFT, installer: **runs end to end, verified against the reference implementation** (below) |
| Recipe: RoFormer vocals + Demucs on the remainder | catalogue entry `recipe-roformer-demucs` (needs both models installed), manifest pipeline `recipe-vocals-then-demucs`; stems sum to the input to 4e-8 |
| Portable matrix kernel (Windows/Linux fallback) | register-tiled, checked against Accelerate at size; 2% of Accelerate's speed on this Mac (see Runtime) |
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
  Pad (constant, reflect, edge), ReduceMean/Sum/Prod/Max/Min/L2, Einsum (only the outer product `...,f->...f`), Softmax, LayerNormalization, InstanceNormalization, MatMul (batched,
  broadcast), Gemm, Conv and ConvTranspose (1-D and 2-D, strides, pads, dilations, groups, bias). Anything else stops at load with
  `unsupported op X at node N (name)`; shape and type mismatches stop the run with the node named, never a guess. The committed
  `htdemucs` skeleton uses 37 operator types (4,483 nodes, 533 external weights), the RoFormer skeleton 31 (11,339 nodes); all are implemented.
  External weights may be float32, int64 or float16 (widened at load); double constants are narrowed to float.
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

**Portable kernel.** Without a system BLAS, `sgemm_portable` (4 x 16 register tiles over cache blocks, auto-vectorised, threaded by splitting
rows) is used. On this Mac one thread does 56 GFLOP/s for a 768^3 product against 1,900-2,500 GFLOP/s for Accelerate (2-3%; Accelerate uses
the AMX units, so the gap is larger than it would be against a CPU BLAS). `test_stems_graph` checks it against the system kernel at sizes up to
1000 x 700 x 300 with every transpose combination, runs all operator fixtures and size-scaled 1-D/2-D convolutions (strides, dilations,
groups, forced tiny tiles) on it, and prints the speed ratio without asserting on timing.

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

## Mel-Band RoFormer and the combination recipe

**Reference implementation.** `bs-roformer` 0.3.10 on PyPI (MIT, Phil Wang / lucidrains, https://github.com/lucidrains/BS-RoFormer) loads
`MelBandRoformer.ckpt` with strict key matching (dim 384, depth 6, stereo, one stem, time/freq transformer depth 1, 60 mel bands, 8 heads
of 64, mask-estimator depth 2, STFT 2048 / hop 441; inferred from the tensor shapes; releases 0.4 and later use other module layouts and
do not load it). `tools/export_roformer_skeleton.py` exports the network between the band-split features and the masks. The library's
attention is rewritten as MatMul/Softmax chunked over the batch (a 781 x 781 score matrix for 60 bands would otherwise be 1.2 GB) and
`nn.Linear` as 2-D Gemm; both are checked equal to the library. The graph has 11,339 nodes and 673 weights; the skeleton is 6.6 MB (about
0.38 MB gzip-compressed, which is what git stores) plus a 79 kB layout file that also lists the 60 mel bands as STFT-bin ranges, so no mel
filter bank is needed at run time.

**Pipeline** (`roformer_dsp.cpp`, `graph_model.cpp`): STFT with reflect padding and a periodic Hann window, gather of the band bins into the
network's 7,916 features per frame (bin, channel, real/imag), network, overlap-averaged complex masks, inverse STFT. A chunk is 343,980
samples (781 frames), the same length as Demucs'. The vocal model alone fills `vocals` and puts `mix - vocals` in `other`.

**Installer.** Catalogue entry `melband-roformer-vocals` is installable: `demucs_converter()` (now a dispatcher on the entry's kind:
`demucs`, `roformer`, `recipe`) copies the skeleton and layout and converts the 913 MB checkpoint into `roformer.weights` in skeleton order.
The checkpoint's per-layer rotary tables are shared in the exported graph; the layout lists the unused copies under `"ignore"`.

**Recipe.** `recipe-roformer-demucs` downloads nothing and refuses to install until `melband-roformer-vocals` and `demucs-htdemucs` are
installed; it writes a manifest with `"pipeline": "recipe-vocals-then-demucs"` and `"parts"`. At run time:
1. RoFormer on the mix gives `v`; `i = mix - v`.
2. Demucs on `i` gives drums, bass, other (its vocals output is discarded).
3. `separate()`'s existing mixture-consistency step closes the small residual `mix - (v + d + b + o)`, so the four stems sum to the input.
The two networks share one activation arena. The `htdemucs_ft` bag can be the second part as well.

| Check | Result |
|---|---|
| STFT + band gather (C++) vs the library | max abs error 1.5e-5 on peak 135 (1e-7 of peak) |
| Masks, C++ executor vs the library's modules / vs onnxruntime | 8e-6 / 7.6e-6 of peak (max abs 1.1e-5 on peak 1.36) |
| Final vocals, C++ pipeline (mask averaging + inverse STFT) vs the library's own `forward()` | max abs error 1.1e-7 on peak 7.1e-3 (1.5e-5 of peak) |
| Vocal model through the manifest | vocals + other equals the input to 3e-8; drums and bass are exactly zero |
| Recipe on 10 s of voice-like audio (two chunks, through `separate()`) | max abs sum of the four stems minus the input 3.7e-8 |

Test signals matter: the network normalises every band, so a signal with digital silence in some bands makes the masks hypersensitive to
float rounding in the STFT (1e-7 differences in the features gave 0.45 differences in the masks). The test signal therefore carries a
-50 dB noise floor. Fixtures: `tools/stems_test_models/roformer_ref.bin` (16 kB of sampled values, regenerated with
`tools/stems_roformer_compare.py`); all committed fixtures together are 222 kB.

**Half-precision weights.** `export_roformer_skeleton.py --half` stores the weights as IEEE half (the converter writes `"dtype": "float16"`
tensors, the executor widens them at load; compute stays float32), halving the install from 913 MB to 456 MB. Measured against float32:
masks max abs error 8.6e-4 of peak, final vocals 0.6% of the vocal peak (about -44 dB). That is small but not identical, so the committed
skeleton is the float32 one; `--half` is there for a later decision (the loader and converter are tested at small scale).

Speed and memory (this Mac, load average 15-30, 4 threads, 7.8 s chunk): first run 15-17 s (it includes the planning run), steady state
about 8 s per chunk, so roughly 1x real time per chunk and 0.7x with 25% overlap. Where the steady-state time goes (profile): Softmax
about 2 s (1.8 G scores per chunk), Gemm 1.2 s, Erf (GELU) 1.1 s, MatMul 1.0 s, Concat 0.8 s. Memory: 913 MB of weights, 1,080 MB activation
arena, about 2.2 GB peak resident. The recipe: 10 s of audio in 29-43 s depending on machine load (two chunks, one planning run each); the Demucs half adds about
1.7 s per chunk, so a steady-state recipe chunk takes about 10 s.

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
- **Implemented (engine, protocol 6; Phase 2 playback).** The engine reads the `.dhstems` file itself (the helper process too:
  `IEngine::stems_attach(deck, track_id, path)`, request `kReqStemsAttach`) on a loader thread and holds it as interleaved int16
  (16 bytes per frame: 42.3 MB per minute per deck, 84.7 MB for 24-bit files), next to the float PCM of the mix. The deck reads
  through one `Source` (plain PCM, or the sum of stems x gain, blended): the sum is made before the varispeed read and before the one
  shared key-lock stretcher, so mutes and levels cost one 4-way mix per sample. A stem whose effect chain (`deckN.stemM.chain`,
  M 1 to 4 = vocals, drums, bass, other) is running is split out onto its own stretcher and chain (an extra stretch per such stem
  while key lock is engaged), crossfaded in over 256 samples, and summed back before trim, EQ and fader. Gains ramp in 10 ms,
  attach and detach blend over 20 ms without moving the playhead. With every stem at unity the output equals normal playback
  (exactly, in the tests). Controls `deckN.stem.<name>.<mute|solo|isolate|level>`, `deckN.stems.<acapella|instrumental|drums_only|no_drums|reset|enable|detach>`,
  REST `/v1/decks/{deck}/stems`, setting `stems.auto_attach`. Not done: a stems pad mode on controllers (FLX4 uses all eight pad modes).
- Library: store the cache key per track, queue separation on load when idle, pause the queue while a deck plays.
- The RoFormer skeleton and the combination above (the bag runner for `htdemucs_ft` is done).

## Third-party notices to ship

- No runtime is bundled any more (the built-in executor is DeckHand's own); ONNX Runtime only if the optional `DECKHAND_WITH_ONNX` backend is ever shipped.
- Demucs architecture code (MIT, Meta): the skeleton is generated from it. Weights are NOT part of DeckHand.
- lucidrains BS-RoFormer / Mel-Band RoFormer code (MIT), when its skeleton is added. See `THIRD-PARTY-NOTICES.md`.
