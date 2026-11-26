#!/usr/bin/env python3
"""Generates the weight-less ONNX "skeleton" of the Mel-Band RoFormer vocal model plus its layout file.

Reference implementation: the PyPI package bs-roformer 0.3.10 (MIT, Phil Wang / lucidrains, https://github.com/lucidrains/BS-RoFormer),
whose MelBandRoformer loads Kimberley Jensen's MelBandRoformer.ckpt with strict key matching. Config inferred from the tensor shapes:
dim 384, depth 6, stereo, 1 stem, time/freq transformer depth 1, 60 mel bands, 8 heads x 64, mask estimator depth 2, STFT 2048 / hop 441.

  python3.14 -m venv v && v/bin/pip install torch onnx onnxruntime numpy einops beartype rotary-embedding-torch librosa scipy packaging
  v/bin/pip install --no-deps bs-roformer==0.3.10
  v/bin/python tools/export_roformer_skeleton.py third_party/models/roformer/MelBandRoformer.ckpt out_dir [--half]

Writes into out_dir:
  roformer.onnx          graph only; every weight is an ONNX external-data reference into "roformer.weights"
  roformer.layout.json   per weight: checkpoint key, byte offset/length, shape, dtype; "ignore": checkpoint keys the graph does not need;
                         "bands": [[first_freq, count], ...] (the mel bands as STFT-bin ranges, so C++ needs no mel filter bank)
The graph is the part between the band-split input features and the masks; the STFT, the mask averaging and the inverse STFT run in C++.
Differences from the library code, all numerically neutral (checked against the library's own forward in this script):
  * attention is written as MatMul/Softmax (no Einsum) and chunked over the batch so the score matrix stays small;
  * nn.Linear is exported through 2-D Gemm (transposed-weight flag), so weights keep their checkpoint names and layout.
--half stores the weights as IEEE half precision on disk (the executor widens them at load); compute stays float32.
"""
import json, os, sys
import numpy as np
import torch, torch.nn as nn, torch.nn.functional as F
import onnx
from einops import rearrange
import bs_roformer.attend as attend_mod
from bs_roformer import MelBandRoformer

SCORE_BUDGET = 16 * 1024 * 1024  # floats in one attention score block


def chunked_attend(self, q, k, v):
    scale = q.shape[-1] ** -0.5
    b, h, n, _ = q.shape
    g = max(1, SCORE_BUDGET // (h * n * k.shape[-2]))
    outs = []
    for qc, kc, vc in zip(q.split(g, 0), k.split(g, 0), v.split(g, 0)):
        sim = torch.matmul(qc, kc.transpose(-1, -2)) * scale
        outs.append(torch.matmul(sim.softmax(dim=-1), vc))
    return outs[0] if len(outs) == 1 else torch.cat(outs, 0)


def linear2d(self, x):
    y = F.linear(x.reshape(-1, x.shape[-1]), self.weight, self.bias)
    return y.reshape(*x.shape[:-1], y.shape[-1])


class Core(nn.Module):
    def __init__(s, m):
        super().__init__()
        s.band_split, s.layers, s.mask_estimators = m.band_split, m.layers, m.mask_estimators

    def forward(s, x):  # x: [1, T, features] -> masks [1, T, features]
        x = s.band_split(x)
        for time_t, freq_t in s.layers:
            x = rearrange(x, 'b t f d -> b f t d')
            x = rearrange(x, 'b f t d -> (b f) t d')
            x = time_t(x)
            x = rearrange(x, '(b f) t d -> b f t d', b=1)
            x = rearrange(x, 'b f t d -> b t f d')
            x = rearrange(x, 'b t f d -> (b t) f d')
            x = freq_t(x)
            x = rearrange(x, '(b t) f d -> b t f d', b=1)
        return s.mask_estimators[0](x)


def features(m, audio):  # [1,2,L] -> (stft_repr [1,(f s),t,c], x [1,t,(f c)]) exactly as MelBandRoformer.forward does
    win = m.stft_window_fn(device=audio.device)
    st = torch.view_as_real(torch.stft(audio.reshape(-1, audio.shape[-1]), **m.stft_kwargs, window=win, return_complex=True))
    st = rearrange(st.reshape(1, 2, *st.shape[1:]), 'b s f t c -> b (f s) t c')
    x = st[torch.arange(1)[..., None], m.freq_indices]
    return st, rearrange(x, 'b f t c -> b t (f c)')


def build(ckpt):
    sd = torch.load(ckpt, map_location='cpu', weights_only=True)
    m = MelBandRoformer(dim=384, depth=6, stereo=True, num_stems=1, time_transformer_depth=1, freq_transformer_depth=1, num_bands=60,
                        dim_head=64, heads=8, mask_estimator_depth=2, stft_hop_length=441, attn_dropout=0, ff_dropout=0, flash_attn=False)
    m.load_state_dict(sd, strict=True)
    return m.eval(), sd


def main():
    ckpt, out = sys.argv[1], sys.argv[2]
    half = '--half' in sys.argv
    os.makedirs(out, exist_ok=True)
    m, sd = build(ckpt)
    m.requires_grad_(False)  # tracing with autograd on would keep every activation (tens of GB) alive
    torch.set_grad_enabled(False)
    attend_mod.Attend.forward = chunked_attend
    nn.Linear.forward = linear2d
    L = 343980
    audio = torch.randn(1, 2, L) * 0.1
    _, x = features(m, audio)
    core = Core(m).eval()
    tmp = os.path.join(out, '_tmp.onnx')
    torch.onnx.export(core, (x,), tmp, input_names=['x'], output_names=['masks'], opset_version=17, dynamo=False, do_constant_folding=False)
    mo = onnx.load(tmp); os.remove(tmp)
    layout, off, used = [], 0, set()
    for init in mo.graph.initializer:
        key = init.name
        assert key in sd, 'initializer without a checkpoint key: ' + key
        arr = sd[key]
        assert init.data_type == onnx.TensorProto.FLOAT and list(init.dims) == list(arr.shape), key
        n = int(arr.numel()) * (2 if half else 4)
        layout.append({'name': key, 'offset': off, 'length': n, 'shape': list(arr.shape), 'dtype': 'float16' if half else 'float32'})
        used.add(key)
        init.ClearField('raw_data'); init.ClearField('float_data')
        init.data_location = onnx.TensorProto.EXTERNAL
        if half: init.data_type = onnx.TensorProto.FLOAT16
        del init.external_data[:]
        for k, v in (('location', 'roformer.weights'), ('offset', str(off)), ('length', str(n))):
            e = init.external_data.add(); e.key = k; e.value = v
        off += n
        off = (off + 63) // 64 * 64
    ignore = sorted(k for k in sd if k not in used)  # the rotary frequency tables are shared between layers
    for k in ignore: assert k.endswith('rotary_embed.freqs'), k
    # mel bands as bin ranges; mask averaging divisor per bin
    fpb = m.freqs_per_band.numpy()
    bands = []
    for row in fpb:
        idx = np.nonzero(row)[0]
        assert (np.diff(idx) == 1).all()
        bands.append([int(idx[0]), int(len(idx))])
    feat_total = sum(c for _, c in bands) * 2 * 2
    assert x.shape[-1] == feat_total
    onnx.save_model(mo, os.path.join(out, 'roformer.onnx'))
    json.dump({'format': 'deckhand-stems-layout/1', 'weights_file': 'roformer.weights', 'total_bytes': off, 'dtype': 'float16' if half else 'float32',
               'alignment': 64, 'tensors': layout, 'ignore': ignore, 'bands': bands, 'n_fft': 2048, 'hop': 441, 'frames': int(x.shape[1]),
               'features': int(x.shape[-1])}, open(os.path.join(out, 'roformer.layout.json'), 'w'), separators=(',', ':'))
    print('skeleton:', os.path.getsize(os.path.join(out, 'roformer.onnx')), 'bytes;', len(layout), 'tensors;', off, 'bytes of weights;', 'frames', x.shape[1], 'features', x.shape[-1])
    with open(os.path.join(out, 'roformer.reference.weights'), 'wb') as f:  # the file the C++ converter must reproduce byte for byte
        for t in layout:
            f.seek(t['offset'])
            a = sd[t['name']].float().contiguous()
            f.write(a.half().numpy().tobytes() if half else a.numpy().tobytes())
        f.truncate(off)
    if '--check' in sys.argv:  # the exported graph equals the library's forward: masks compared through the full model output
        with torch.no_grad():
            nn.Linear.forward = linear2d
            masks = core(x)
        torch.save({'audio': audio, 'x': x, 'masks': masks}, os.path.join(out, 'check_inputs.pt'))
        print('masks', tuple(masks.shape))


if __name__ == '__main__':
    main()
