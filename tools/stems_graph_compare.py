#!/usr/bin/env python3
"""Compares the built-in executor's htdemucs core output with the reference runtime (PyPI onnxruntime, oracle only).

  1. Run the C++ test with DH_STEMS_WEIGHTS=<htdemucs.weights> DH_STEMS_DUMP_DIR=<dir>  (it writes spec/mix/xspec/xtime .bin)
  2. python tools/stems_graph_compare.py <dir holding htdemucs.onnx next to htdemucs.weights> <dump dir> [--fixture out.bin] [--weights file]

Prints max-abs and RMS error per output (and per stem for the time branch). With --fixture it also writes the small
reference file tests/test_stems_graph.cpp checks in CI-less environments with weights (tools/stems_test_models/htdemucs_core_ref.bin).
"""
import struct, sys, os
import numpy as np
import onnxruntime as ort

model_dir, dump = sys.argv[1], sys.argv[2]
if '--weights' in sys.argv:  # another weights file (e.g. one model of the htdemucs_ft bag) next to the same skeleton
    import tempfile, shutil
    tmp = tempfile.mkdtemp()
    shutil.copy(os.path.join(model_dir, 'htdemucs.onnx'), tmp)
    shutil.copy(sys.argv[sys.argv.index('--weights') + 1], os.path.join(tmp, 'htdemucs.weights'))  # the reference runtime refuses symlinks out of the folder
    model_dir = tmp
fixture = sys.argv[sys.argv.index('--fixture') + 1] if '--fixture' in sys.argv else None
spec = np.fromfile(os.path.join(dump, 'spec.bin'), np.float32).reshape(1, 4, 2048, 336)
mix = np.fromfile(os.path.join(dump, 'mix.bin'), np.float32).reshape(1, 2, 343980)
so = ort.SessionOptions()
so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
s = ort.InferenceSession(os.path.join(model_dir, 'htdemucs.onnx'), so, providers=['CPUExecutionProvider'])
rxs, rxt = s.run(['xspec', 'xtime'], {'spec': spec, 'mix': mix})
gxs = np.fromfile(os.path.join(dump, 'xspec.bin'), np.float32).reshape(rxs.shape)
gxt = np.fromfile(os.path.join(dump, 'xtime.bin'), np.float32).reshape(rxt.shape)
for name, r, g in (('xspec', rxs, gxs), ('xtime', rxt, gxt)):
    d = g.astype(np.float64) - r
    print(f'{name}: ref peak {np.abs(r).max():.5f} ref rms {np.sqrt((r.astype(np.float64)**2).mean()):.6f} | '
          f'max abs err {np.abs(d).max():.3e} rms err {np.sqrt((d**2).mean()):.3e} rel-to-peak {np.abs(d).max()/np.abs(r).max():.2e}')
for k in range(4):
    d = gxt[0, k].astype(np.float64) - rxt[0, k]
    dd = gxs[0, k].astype(np.float64) - rxs[0, k]
    print(f'  stem {k}: time-branch max abs {np.abs(d).max():.3e} rms {np.sqrt((d**2).mean()):.3e} | spectral max abs {np.abs(dd).max():.3e} rms {np.sqrt((dd**2).mean()):.3e}')
if fixture:
    n = 2048
    out = b'DHCR1\n' + struct.pack('<I', n)
    for r in (rxs, rxt):
        out += struct.pack('<ff', float(np.sqrt((r.astype(np.float64) ** 2).mean())), float(np.abs(r).max()))
    # header order is xs_rms, xs_max, xt_rms, xt_max, then the samples of xspec then xtime
    for r in (rxs, rxt):
        flat = r.reshape(-1)
        idx = [(k * 2654435761 + 12345) % flat.size for k in range(n)]
        out += flat[idx].astype(np.float32).tobytes()
    open(fixture, 'wb').write(out)
    print('wrote', fixture, len(out), 'bytes')
