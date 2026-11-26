#!/usr/bin/env python3
"""Compares the built-in executor's Mel-Band RoFormer pipeline with the reference implementation (bs-roformer 0.3.10, MIT) and the
reference runtime (PyPI onnxruntime, oracle only).

  1. python tools/export_roformer_skeleton.py <ckpt> <dir> --check     (dir then holds roformer.onnx + roformer.reference.weights)
     mv <dir>/roformer.reference.weights <dir>/roformer.weights
  2. DH_STEMS_DUMP_DIR=<dump> build-graph/test_stems_graph roformer_matches   (writes rf_mix/rf_x/rf_masks/rf_vocals .bin)
  3. python tools/stems_roformer_compare.py <ckpt> <dir> <dump> [--fixture tools/stems_test_models/roformer_ref.bin]

Compares: C++ STFT + band gather vs the library's own features; network masks (C++ vs onnxruntime vs the library's modules); final
vocals (C++ inverse STFT and mask averaging vs the library's complete forward()).
"""
import os, struct, sys
import numpy as np
import torch
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export_roformer_skeleton as ex

ckpt, model_dir, dump = sys.argv[1], sys.argv[2], sys.argv[3]
fixture = sys.argv[sys.argv.index('--fixture') + 1] if '--fixture' in sys.argv else None
S = 343980
torch.set_grad_enabled(False)
mix = np.fromfile(os.path.join(dump, 'rf_mix.bin'), np.float32).reshape(2, S)
x_cpp = np.fromfile(os.path.join(dump, 'rf_x.bin'), np.float32)
masks_cpp = np.fromfile(os.path.join(dump, 'rf_masks.bin'), np.float32)
vocals_cpp = np.fromfile(os.path.join(dump, 'rf_vocals.bin'), np.float32).reshape(2, S)
m, sd = ex.build(ckpt)
m.requires_grad_(False)
audio = torch.from_numpy(mix)[None]
_, x_ref = ex.features(m, audio)
x_ref = x_ref.numpy().reshape(-1)


def rep(name, got, ref):
    d = got.astype(np.float64) - ref.astype(np.float64)
    print(f'{name}: ref peak {np.abs(ref).max():.5f} rms {np.sqrt((ref.astype(np.float64)**2).mean()):.6f} | max abs err {np.abs(d).max():.3e} '
          f'rms err {np.sqrt((d**2).mean()):.3e} rel-to-peak {np.abs(d).max() / np.abs(ref).max():.2e}')


rep('features (C++ STFT + band gather vs library)', x_cpp, x_ref)
# the library's complete forward() with its own, unmodified attention and linear layers: the final vocals
vocals_ref = m(audio)[0].numpy()
rep('vocals (C++ pipeline vs library forward)', vocals_cpp, vocals_ref)
# the modules the skeleton was exported from (chunked MatMul attention, 2-D linear): same maths, checked here
import torch.nn as nn
import bs_roformer.attend as attend_mod
attend_mod.Attend.forward = ex.chunked_attend
nn.Linear.forward = ex.linear2d
core = ex.Core(m).eval()
masks_ref = core(torch.from_numpy(x_cpp).reshape(1, -1, 7916)).numpy().reshape(-1)
rep('masks (C++ executor vs library modules)', masks_cpp, masks_ref)
import onnxruntime as ort
so = ort.SessionOptions()
so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
sess = ort.InferenceSession(os.path.join(model_dir, 'roformer.onnx'), so, providers=['CPUExecutionProvider'])
masks_ort = sess.run(['masks'], {'x': x_cpp.reshape(1, -1, 7916)})[0].reshape(-1)
rep('masks (C++ executor vs onnxruntime)', masks_cpp, masks_ort)
rep('masks (onnxruntime vs library modules)', masks_ort, masks_ref)
if fixture:
    n = 2048
    out = b'DHRF1\n' + struct.pack('<I', n)
    for r in (masks_ref, vocals_ref.reshape(-1)):
        out += struct.pack('<ff', float(np.sqrt((r.astype(np.float64) ** 2).mean())), float(np.abs(r).max()))
    for r in (masks_ref, vocals_ref.reshape(-1)):
        idx = [(k * 2654435761 + 12345) % r.size for k in range(n)]
        out += r[idx].astype(np.float32).tobytes()
    open(fixture, 'wb').write(out)
    print('wrote', fixture, len(out), 'bytes')
