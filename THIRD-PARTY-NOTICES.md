# Third-party notices

This file lists third-party material that DeckHand builds on or may download. SQLite/SQLCipher and miniaudio are documented in
`third_party/sqlcipher/` and the headers under `third_party/miniaudio/`.

## Stem separation (see docs/STEMS.md)

- **ONNX Runtime** (MIT, Copyright Microsoft Corporation). Linked only when `third_party/onnxruntime` is present (not committed). Ship the
  release's `LICENSE` and `ThirdPartyNotices.txt` with the app when it is bundled.
- **Hybrid Transformer Demucs** architecture (MIT, Copyright Meta Platforms, Inc. and affiliates, https://github.com/facebookresearch/demucs).
  `resources/stem-models/skeletons/htdemucs.onnx` contains the network graph generated from that code, and no weights.
  The Demucs weights are **not** part of DeckHand and are not under the MIT licence (research use only per the upstream author).
  Users download them from `dl.fbaipublicfiles.com` at their own initiative after accepting the statement.
- **Mel-Band RoFormer vocal model** by Kimberley Jensen (weights declared MIT on https://huggingface.co/KimberleyJSN/melbandroformer);
  architecture by lucidrains (MIT, https://github.com/lucidrains/BS-RoFormer). Downloaded by the user on request; not bundled.
