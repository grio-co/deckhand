#!/usr/bin/env python3
"""Writes the tiny ONNX models used by tests/test_stems.cpp (fixed-gain 4-way splits; no weights, no licence issue).

Reproduce:  python3 -m venv v && v/bin/pip install onnx numpy onnxruntime && v/bin/python tools/make_stems_test_models.py
All three split a stereo segment of 4096 frames into four copies scaled 0.1 / 0.2 / 0.3 / 0.4 (they sum to the input),
in model-output order [other, bass, drums, vocals], and differ only in tensor layout:
  gain4_multi.onnx  input "mix" BCT [1,2,4096]; four outputs "s0".."s3", each BCT
  gain4_bsct.onnx   input "mix" BTC [1,4096,2]; one output "stems" BSCT [1,4,2,4096]
  gain4_btcs.onnx   input "mix" BCT [1,2,4096]; one output "stems" BTCS [1,4096,2,4]
"""
import sys, os
import numpy as np
import onnx
from onnx import helper, TensorProto as TP, numpy_helper

T, C = 4096, 2
GAINS = [0.1, 0.2, 0.3, 0.4]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stems_test_models")


def save(graph, name):
    m = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    m.ir_version = 8
    onnx.checker.check_model(m)
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(m.SerializeToString())


def vi(name, shape):
    return helper.make_tensor_value_info(name, TP.FLOAT, shape)


def multi():
    inits, nodes, outs = [], [], []
    for i, g in enumerate(GAINS):
        inits.append(numpy_helper.from_array(np.array([g], np.float32), f"g{i}"))
        nodes.append(helper.make_node("Mul", ["mix", f"g{i}"], [f"s{i}"]))
        outs.append(vi(f"s{i}", [1, C, T]))
    save(helper.make_graph(nodes, "gain4_multi", [vi("mix", [1, C, T])], outs, inits), "gain4_multi.onnx")


def stacked(in_perm, in_shape, out_perm, out_shape, name):
    gains = numpy_helper.from_array(np.array(GAINS, np.float32).reshape(1, 4, 1, 1), "gains")
    axes = numpy_helper.from_array(np.array([1], np.int64), "axes")
    nodes, cur = [], "mix"
    if in_perm:
        nodes.append(helper.make_node("Transpose", [cur], ["bct"], perm=in_perm)); cur = "bct"
    nodes.append(helper.make_node("Unsqueeze", [cur, "axes"], ["b1ct"]))
    nodes.append(helper.make_node("Mul", ["b1ct", "gains"], ["bsct" if out_perm else "stems"]))
    if out_perm:
        nodes.append(helper.make_node("Transpose", ["bsct"], ["stems"], perm=out_perm))
    save(helper.make_graph(nodes, name, [vi("mix", in_shape)], [vi("stems", out_shape)], [gains, axes]), name + ".onnx")


multi()
stacked([0, 2, 1], [1, T, C], None, [1, 4, C, T], "gain4_bsct")      # BTC in, BSCT out
stacked(None, [1, C, T], [0, 3, 2, 1], [1, T, C, 4], "gain4_btcs")  # BCT in, BTCS out

# Self-check with onnxruntime when it is installed.
try:
    import onnxruntime as ort
    x = np.random.default_rng(1).standard_normal((1, C, T)).astype(np.float32)
    s = ort.InferenceSession(os.path.join(OUT, "gain4_multi.onnx"))
    o = s.run(None, {"mix": x})
    for i, g in enumerate(GAINS): assert np.allclose(o[i], x * g, atol=1e-6)
    s = ort.InferenceSession(os.path.join(OUT, "gain4_bsct.onnx"))
    o = s.run(None, {"mix": x.transpose(0, 2, 1).copy()})[0]
    for i, g in enumerate(GAINS): assert np.allclose(o[0, i], x[0] * g, atol=1e-6)
    s = ort.InferenceSession(os.path.join(OUT, "gain4_btcs.onnx"))
    o = s.run(None, {"mix": x})[0]
    for i, g in enumerate(GAINS): assert np.allclose(o[0, :, :, i], (x[0] * g).T, atol=1e-6)
    print("verified with onnxruntime", ort.__version__)
except ImportError:
    print("onnxruntime not installed: models written, not verified")


# A tiny PyTorch state dict for the checkpoint reader test (needs torch; skipped when it is not installed).
try:
    import torch, collections
    sd = collections.OrderedDict()
    sd["a.weight"] = torch.arange(6, dtype=torch.float32).reshape(2, 3)
    sd["b.bias"] = torch.tensor([0.5, -1.0, 2.0, 0.25], dtype=torch.float16)
    sd["c.t"] = torch.arange(6, dtype=torch.float32).reshape(3, 2).t()  # non-contiguous view
    sd["d.idx"] = torch.tensor([7, -3], dtype=torch.int64)
    torch.save(sd, os.path.join(OUT, "tiny_state.pt"))
    print("wrote tiny_state.pt")
except ImportError:
    print("torch not installed: tiny_state.pt not written")
