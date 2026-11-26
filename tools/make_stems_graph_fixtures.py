#!/usr/bin/env python3
"""Generates tools/stems_test_models/graph_ops.dhgf: tiny single-purpose ONNX models, their inputs and the outputs the
reference runtime (the PyPI onnxruntime wheel, used ONLY as an oracle) produced. tests/test_stems_graph.cpp replays
them through the built-in executor, so CI needs no Python.

  python tools/make_stems_graph_fixtures.py  tools/stems_test_models/graph_ops.dhgf

Container: "DHGF1\\n", u32 ncases, then per case: str name, u32 model_len, model bytes, f32 rtol, f32 atol, then a list of
inputs and a list of expected outputs. A list is u32 count then per tensor: str name, u8 dtype (0 f32, 1 i64, 2 bool),
u32 rank, rank x i64 dims, raw little-endian data. str = u32 length + bytes.
"""
import struct, sys
import numpy as np
import onnx
from onnx import helper as H, TensorProto as TP, numpy_helper as NH
import onnxruntime as ort

rng = np.random.default_rng(12345)
cases = []
F, I, B = TP.FLOAT, TP.INT64, TP.BOOL


def rnd(*shape, scale=1.0):
    return (rng.standard_normal(shape) * scale).astype(np.float32)


def add(name, nodes, inputs, outs, inits=None, opset=17, rtol=2e-4, atol=2e-5, otypes=None):
    inits = inits or {}
    otypes = otypes or {}
    gin = []
    for k, v in inputs.items():
        et = {np.dtype('float32'): F, np.dtype('int64'): I, np.dtype('bool'): B}[v.dtype]
        gin.append(H.make_tensor_value_info(k, et, list(v.shape)))
    gout = [H.make_tensor_value_info(o, otypes.get(o, F), None) for o in outs]
    g = H.make_graph(nodes, name, gin, gout, [NH.from_array(v, k) for k, v in inits.items()])
    m = H.make_model(g, opset_imports=[H.make_opsetid('', opset)])
    m.ir_version = 8
    data = m.SerializeToString()
    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    s = ort.InferenceSession(data, so, providers=['CPUExecutionProvider'])
    res = s.run(outs, dict(inputs))
    cases.append((name, data, rtol, atol, list(inputs.items()), list(zip(outs, res))))


N = H.make_node
# ---- elementwise, broadcasting ----
a, b = rnd(2, 3, 4), rnd(4)
for op in ['Add', 'Sub', 'Mul', 'Div']:
    bb = b + 3.0 if op == 'Div' else b
    add('bin_' + op.lower() + '_bcast', [N(op, ['a', 'b'], ['y'])], {'a': a, 'b': bb}, ['y'])
add('bin_mul_col', [N('Mul', ['a', 'b'], ['y'])], {'a': rnd(3, 1, 5), 'b': rnd(1, 4, 1)}, ['y'])
add('pow_scalar', [N('Pow', ['a', 'b'], ['y'])], {'a': np.abs(rnd(2, 5)) + 0.1, 'b': np.array(2.0, np.float32)}, ['y'])
for op in ['Erf', 'Sigmoid', 'Tanh', 'Relu', 'Sin', 'Cos', 'Exp', 'Neg', 'Abs', 'Floor', 'Ceil', 'Reciprocal']:
    add('un_' + op.lower(), [N(op, ['x'], ['y'])], {'x': rnd(3, 7, scale=2) + 0.05}, ['y'])
add('un_sqrt', [N('Sqrt', ['x'], ['y'])], {'x': np.abs(rnd(40)) + 0.01}, ['y'])
add('un_log', [N('Log', ['x'], ['y'])], {'x': np.abs(rnd(40)) + 0.01}, ['y'])
add('gelu', [N('Gelu', ['x'], ['y'])], {'x': rnd(50, scale=2)}, ['y'], opset=20)
add('clip', [N('Clip', ['x', 'lo', 'hi'], ['y'])], {'x': rnd(30, scale=2)}, ['y'],
    inits={'lo': np.array(-0.5, np.float32), 'hi': np.array(0.7, np.float32)})
ia, ib = np.array([[7, -7, 5], [-5, 9, 0]], np.int64), np.array([3, 3, -2], np.int64)
add('int_div_mod', [N('Div', ['a', 'b'], ['d']), N('Mod', ['a', 'b'], ['m'])], {'a': ia, 'b': ib}, ['d', 'm'],
    otypes={'d': I, 'm': I})
add('int_fmod', [N('Mod', ['a', 'b'], ['m'], fmod=1)], {'a': ia, 'b': ib}, ['m'], otypes={'m': I})
add('float_fmod', [N('Mod', ['a', 'b'], ['m'], fmod=1)], {'a': rnd(3, 4, scale=5), 'b': np.array([1.5], np.float32)}, ['m'])
add('compare', [N('Equal', ['a', 'b'], ['e']), N('Less', ['a', 'b'], ['l']), N('Greater', ['a', 'b'], ['g']),
                N('Not', ['e'], ['ne']), N('And', ['l', 'g'], ['lg']), N('Or', ['l', 'g'], ['lo'])],
    {'a': np.array([[1, 2, 3], [4, 5, 6]], np.int64), 'b': np.array([2, 2, 2], np.int64)}, ['e', 'l', 'g', 'ne', 'lg', 'lo'],
    otypes={k: B for k in ['e', 'l', 'g', 'ne', 'lg', 'lo']})
add('where', [N('Where', ['c', 'x', 'y'], ['o'])],
    {'c': np.array([[True], [False]]), 'x': rnd(2, 3), 'y': rnd(1, 3)}, ['o'])
add('cast', [N('Cast', ['x'], ['i'], to=I), N('Cast', ['i'], ['f'], to=F), N('Cast', ['x'], ['b'], to=B)],
    {'x': np.array([-2.7, -0.2, 0.0, 0.5, 3.9], np.float32)}, ['i', 'f', 'b'], otypes={'i': I, 'f': F, 'b': B})
add('minmax', [N('Max', ['a', 'b'], ['mx']), N('Min', ['a', 'b'], ['mn'])], {'a': rnd(4, 3), 'b': rnd(3)}, ['mx', 'mn'])

# ---- shape arithmetic and data movement ----
x = rnd(2, 3, 4, 5)
add('shape_chain',
    [N('Shape', ['x'], ['s']), N('Gather', ['s', 'i0'], ['d0']), N('Gather', ['s', 'i1'], ['d1']),
     N('Mul', ['d0', 'd1'], ['p']), N('Unsqueeze', ['p', 'ax'], ['pu']), N('Concat', ['pu', 'm1'], ['ns'], axis=0),
     N('Reshape', ['x', 'ns'], ['y']), N('Shape', ['y'], ['ys'], start=1, end=2)],
    {'x': x}, ['y', 'ys'], inits={'i0': np.array(0, np.int64), 'i1': np.array(1, np.int64), 'ax': np.array([0], np.int64),
                                  'm1': np.array([-1], np.int64)}, otypes={'ys': I})
add('reshape_zero', [N('Reshape', ['x', 'sh'], ['y'])], {'x': x}, ['y'], inits={'sh': np.array([0, -1, 5], np.int64)})
add('transpose', [N('Transpose', ['x'], ['y'], perm=[2, 0, 3, 1])], {'x': x}, ['y'])
add('transpose_default', [N('Transpose', ['x'], ['y'])], {'x': rnd(3, 4, 2)}, ['y'])
add('squeeze_unsqueeze', [N('Unsqueeze', ['x', 'ax'], ['u']), N('Squeeze', ['u', 'ax2'], ['s'])],
    {'x': rnd(3, 4)}, ['u', 's'], inits={'ax': np.array([0, 2], np.int64), 'ax2': np.array([2], np.int64)})
add('concat', [N('Concat', ['a', 'b', 'c'], ['y'], axis=1)], {'a': rnd(2, 1, 3), 'b': rnd(2, 2, 3), 'c': rnd(2, 3, 3)}, ['y'])
add('concat_neg', [N('Concat', ['a', 'b'], ['y'], axis=-1)], {'a': rnd(2, 3), 'b': rnd(2, 4)}, ['y'])
add('gather_axis1', [N('Gather', ['x', 'i'], ['y'], axis=1)], {'x': rnd(3, 5, 2), 'i': np.array([[4, 0], [-1, 2]], np.int64)}, ['y'])
add('slice_basic', [N('Slice', ['x', 's', 'e', 'a', 'st'], ['y'])], {'x': rnd(6, 7)}, ['y'],
    inits={'s': np.array([1, -3], np.int64), 'e': np.array([5, 100], np.int64), 'a': np.array([0, 1], np.int64), 'st': np.array([2, 1], np.int64)})
add('slice_neg_step', [N('Slice', ['x', 's', 'e', 'a', 'st'], ['y'])], {'x': rnd(8, 3)}, ['y'],
    inits={'s': np.array([-1], np.int64), 'e': np.array([-100], np.int64), 'a': np.array([0], np.int64), 'st': np.array([-3], np.int64)})
add('slice_noaxes', [N('Slice', ['x', 's', 'e'], ['y'])], {'x': rnd(5, 6)}, ['y'],
    inits={'s': np.array([1, 2], np.int64), 'e': np.array([4, 6], np.int64)})
add('split_equal', [N('Split', ['x'], ['a', 'b', 'c'], axis=1)], {'x': rnd(2, 6, 3)}, ['a', 'b', 'c'])
add('split_sizes', [N('Split', ['x', 'sz'], ['a', 'b'], axis=-1)], {'x': rnd(2, 7)}, ['a', 'b'], inits={'sz': np.array([2, 5], np.int64)})
add('expand', [N('Expand', ['x', 'sh'], ['y'])], {'x': rnd(3, 1, 4)}, ['y'], inits={'sh': np.array([2, 3, 5, 4], np.int64)})
add('tile', [N('Tile', ['x', 'r'], ['y'])], {'x': rnd(2, 3)}, ['y'], inits={'r': np.array([2, 3], np.int64)})
add('range_int', [N('Range', ['s', 'l', 'd'], ['y'])], {}, ['y'],
    inits={'s': np.array(3, np.int64), 'l': np.array(20, np.int64), 'd': np.array(4, np.int64)}, otypes={'y': I})
add('range_float', [N('Range', ['s', 'l', 'd'], ['y'])], {}, ['y'],
    inits={'s': np.array(0.5, np.float32), 'l': np.array(3.0, np.float32), 'd': np.array(0.25, np.float32)})
add('constant_of_shape', [N('ConstantOfShape', ['sh'], ['y'], value=H.make_tensor('v', F, [1], [2.5])),
                          N('ConstantOfShape', ['sh'], ['z'], value=H.make_tensor('w', I, [1], [7]))],
    {}, ['y', 'z'], inits={'sh': np.array([2, 3], np.int64)}, otypes={'z': I})
add('scatternd', [N('ScatterND', ['d', 'i', 'u'], ['y'])],
    {'d': rnd(4, 3), 'i': np.array([[1], [3]], np.int64), 'u': rnd(2, 3)}, ['y'])
add('scatternd_elem', [N('ScatterND', ['d', 'i', 'u'], ['y'])],
    {'d': np.arange(12, dtype=np.int64).reshape(3, 4), 'i': np.array([[0, 1], [2, 3]], np.int64), 'u': np.array([-1, -2], np.int64)}, ['y'],
    otypes={'y': I})
for mode in ['constant', 'reflect', 'edge']:
    add('pad_' + mode, [N('Pad', ['x', 'p'], ['y'], mode=mode)], {'x': rnd(2, 3, 6)}, ['y'],
        inits={'p': np.array([0, 1, 2, 0, 1, 3], np.int64)})
add('pad_const_value', [N('Pad', ['x', 'p', 'cv'], ['y'])], {'x': rnd(3, 4)}, ['y'],
    inits={'p': np.array([1, 0, 0, 2], np.int64), 'cv': np.array(-1.5, np.float32)})
add('pad_crop', [N('Pad', ['x', 'p'], ['y'])], {'x': rnd(3, 6)}, ['y'], inits={'p': np.array([0, -1, 0, -2], np.int64)})

# ---- reductions ----
x = rnd(3, 4, 5)
add('reducemean', [N('ReduceMean', ['x'], ['a'], axes=[1], keepdims=1), N('ReduceMean', ['x'], ['b'], axes=[0, 2], keepdims=0),
                   N('ReduceMean', ['x'], ['c'], keepdims=0)], {'x': x}, ['a', 'b', 'c'])
add('reducesum_max_min', [N('ReduceSum', ['x', 'ax'], ['s'], keepdims=0), N('ReduceMax', ['x'], ['mx'], axes=[-1]),
                          N('ReduceMin', ['x'], ['mn'], axes=[1], keepdims=0)], {'x': x}, ['s', 'mx', 'mn'],
    inits={'ax': np.array([1], np.int64)})
add('reduceprod_int', [N('ReduceProd', ['x'], ['p'], keepdims=0)], {'x': np.array([[2, 3], [4, 5]], np.int64)}, ['p'], otypes={'p': I})
add('softmax', [N('Softmax', ['x'], ['a']), N('Softmax', ['x'], ['b'], axis=1)], {'x': rnd(2, 5, 6, scale=3)}, ['a', 'b'])

# ---- matrices ----
add('matmul_2d', [N('MatMul', ['a', 'b'], ['y'])], {'a': rnd(5, 7), 'b': rnd(7, 3)}, ['y'])
add('matmul_batch_weight', [N('MatMul', ['a', 'b'], ['y'])], {'a': rnd(2, 3, 5, 7), 'b': rnd(7, 4)}, ['y'])
add('matmul_batch_bcast', [N('MatMul', ['a', 'b'], ['y'])], {'a': rnd(2, 1, 5, 7), 'b': rnd(3, 7, 4)}, ['y'])
add('matmul_vec', [N('MatMul', ['a', 'b'], ['y']), N('MatMul', ['b', 'v'], ['z'])], {'a': rnd(4), 'b': rnd(4, 3), 'v': rnd(3)}, ['y', 'z'])
add('gemm', [N('Gemm', ['a', 'b', 'c'], ['y'], alpha=0.5, beta=2.0, transB=1)], {'a': rnd(4, 6), 'b': rnd(5, 6), 'c': rnd(5)}, ['y'])
add('gemm_transa_nobias', [N('Gemm', ['a', 'b'], ['y'], transA=1)], {'a': rnd(6, 4), 'b': rnd(6, 5)}, ['y'])
add('matmul_large', [N('MatMul', ['a', 'b'], ['y'])], {'a': rnd(37, 129), 'b': rnd(129, 53)}, ['y'], rtol=1e-3, atol=1e-4)

# ---- norms ----
add('layernorm', [N('LayerNormalization', ['x', 's', 'b'], ['y'], axis=-1, epsilon=1e-5)],
    {'x': rnd(2, 3, 16, scale=2) + 1}, ['y'], inits={'s': rnd(16), 'b': rnd(16)})
add('layernorm_multi_axis', [N('LayerNormalization', ['x', 's'], ['y'], axis=1, epsilon=1e-6)],
    {'x': rnd(2, 3, 4)}, ['y'], inits={'s': rnd(3, 4)})
add('instancenorm', [N('InstanceNormalization', ['x', 's', 'b'], ['y'], epsilon=1e-5)],
    {'x': rnd(2, 3, 11, scale=2) + 0.5}, ['y'], inits={'s': rnd(3), 'b': rnd(3)})
add('instancenorm_2d', [N('InstanceNormalization', ['x', 's', 'b'], ['y'])],
    {'x': rnd(1, 4, 5, 6)}, ['y'], inits={'s': rnd(4), 'b': rnd(4)})

# ---- convolutions ----
add('conv1d_basic', [N('Conv', ['x', 'w', 'b'], ['y'], kernel_shape=[3], pads=[1, 1])], {'x': rnd(2, 4, 20)}, ['y'],
    inits={'w': rnd(6, 4, 3), 'b': rnd(6)})
add('conv1d_stride_dil', [N('Conv', ['x', 'w'], ['y'], kernel_shape=[3], strides=[2], dilations=[3], pads=[3, 2])],
    {'x': rnd(1, 3, 31)}, ['y'], inits={'w': rnd(5, 3, 3)})
add('conv1d_groups', [N('Conv', ['x', 'w', 'b'], ['y'], kernel_shape=[4], strides=[4], group=2)], {'x': rnd(2, 6, 32)}, ['y'],
    inits={'w': rnd(8, 3, 4), 'b': rnd(8)})
add('conv1d_1x1', [N('Conv', ['x', 'w', 'b'], ['y'], kernel_shape=[1])], {'x': rnd(1, 5, 17)}, ['y'], inits={'w': rnd(7, 5, 1), 'b': rnd(7)})
add('conv1d_depthwise', [N('Conv', ['x', 'w'], ['y'], kernel_shape=[3], pads=[1, 1], group=4)], {'x': rnd(1, 4, 9)}, ['y'],
    inits={'w': rnd(4, 1, 3)})
add('conv2d_k81', [N('Conv', ['x', 'w', 'b'], ['y'], kernel_shape=[8, 1], strides=[4, 1], pads=[2, 0, 2, 0])],
    {'x': rnd(1, 4, 32, 7)}, ['y'], inits={'w': rnd(6, 4, 8, 1), 'b': rnd(6)})
add('conv2d_general', [N('Conv', ['x', 'w', 'b'], ['y'], kernel_shape=[3, 2], strides=[2, 1], pads=[1, 0, 2, 1], dilations=[1, 2], group=2)],
    {'x': rnd(2, 4, 9, 8)}, ['y'], inits={'w': rnd(6, 2, 3, 2), 'b': rnd(6)})
add('conv2d_1x1', [N('Conv', ['x', 'w'], ['y'], kernel_shape=[1, 1])], {'x': rnd(1, 3, 4, 5)}, ['y'], inits={'w': rnd(4, 3, 1, 1)})
add('convt1d_basic', [N('ConvTranspose', ['x', 'w', 'b'], ['y'], kernel_shape=[4], strides=[2], pads=[1, 1])],
    {'x': rnd(2, 3, 10)}, ['y'], inits={'w': rnd(3, 5, 4), 'b': rnd(5)})
add('convt1d_dil_groups', [N('ConvTranspose', ['x', 'w'], ['y'], kernel_shape=[3], strides=[2], dilations=[2], group=2, pads=[2, 0])],
    {'x': rnd(1, 4, 7)}, ['y'], inits={'w': rnd(4, 3, 3)})
add('convt2d_k81', [N('ConvTranspose', ['x', 'w', 'b'], ['y'], kernel_shape=[8, 1], strides=[4, 1], pads=[2, 0, 2, 0])],
    {'x': rnd(1, 4, 6, 5)}, ['y'], inits={'w': rnd(4, 3, 8, 1), 'b': rnd(3)})
add('convt2d_general', [N('ConvTranspose', ['x', 'w'], ['y'], kernel_shape=[3, 3], strides=[2, 3], pads=[1, 1, 0, 2])],
    {'x': rnd(1, 2, 4, 3)}, ['y'], inits={'w': rnd(2, 3, 3, 3)})


def pack_list(items):
    out = struct.pack('<I', len(items))
    for name, arr in items:
        arr = np.asarray(arr, order="C")
        dt = {np.dtype('float32'): 0, np.dtype('int64'): 1, np.dtype('bool'): 2}[arr.dtype]
        nb = name.encode()
        out += struct.pack('<I', len(nb)) + nb + struct.pack('<BI', dt, arr.ndim) + b''.join(struct.pack('<q', d) for d in arr.shape)
        out += arr.tobytes()
    return out


if __name__ == '__main__':
    out = b'DHGF1\n' + struct.pack('<I', len(cases))
    for name, data, rtol, atol, ins, outs in cases:
        nb = name.encode()
        out += struct.pack('<I', len(nb)) + nb + struct.pack('<I', len(data)) + data + struct.pack('<ff', rtol, atol)
        out += pack_list(ins) + pack_list(outs)
    open(sys.argv[1], 'wb').write(out)
    print(len(cases), 'cases,', len(out), 'bytes')
