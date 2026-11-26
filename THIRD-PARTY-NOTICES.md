# Third-party notices

This file lists third-party material that DeckHand builds on or may download. SQLite/SQLCipher and miniaudio are documented in
`third_party/sqlcipher/` and the headers under `third_party/miniaudio/`.

## Stem separation (see docs/STEMS.md)

- **No runtime dependency.** Stem models run on DeckHand's own graph executor (`src/stems/graph`, MIT like the rest of the project, written from the
  published ONNX file format and operator specifications; no code from any ONNX runtime). On macOS it calls Apple's **Accelerate** framework for
  matrix products; Accelerate is a system framework (not redistributed). Elsewhere a portable fallback is used, and OpenBLAS (BSD-3-Clause) could be
  slotted in later, in which case its licence would have to be added here. ONNX Runtime is still supported as an optional, never-bundled backend
  (`DECKHAND_WITH_ONNX`, off by default); if that option is ever shipped, add its MIT licence and `ThirdPartyNotices.txt`.
- The Python `onnxruntime`, `onnx` and `torch` wheels were used only on a developer machine as a reference oracle to generate the small test
  fixtures in `tools/stems_test_models/` (`graph_ops.dhgf`, `htdemucs_*core_ref.bin`: numbers and tiny single-operator graphs, no weights).
  Nothing from them is linked or shipped.
- **Hybrid Transformer Demucs** architecture (MIT, Copyright Meta Platforms, Inc. and affiliates, https://github.com/facebookresearch/demucs).
  `resources/stem-models/skeletons/htdemucs.onnx` contains the network graph generated from that code, and no weights.
  The Demucs weights are **not** part of DeckHand and are not under the MIT licence (research use only per the upstream author).
  Users download them from `dl.fbaipublicfiles.com` at their own initiative after accepting the statement.
- **bs-roformer 0.3.10** (MIT, Phil Wang / lucidrains, PyPI) was used only on a developer machine, by `tools/export_roformer_skeleton.py`, to
  generate `resources/stem-models/skeletons/roformer.onnx` (graph only, no weights) and as the reference for the test fixtures. It is not linked or shipped.
- **Mel-Band RoFormer vocal model** by Kimberley Jensen (weights declared MIT on https://huggingface.co/KimberleyJSN/melbandroformer);
  architecture by lucidrains (MIT, https://github.com/lucidrains/BS-RoFormer). Downloaded by the user on request; not bundled.
