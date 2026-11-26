#!/usr/bin/env python3
"""Generates the weight-less ONNX "skeleton" of Hybrid Transformer Demucs (htdemucs) plus its tensor layout file.

Run once by a developer (never on an end user's machine):
  python3 -m venv v && v/bin/pip install torch onnx onnxruntime numpy einops julius pyyaml tqdm omegaconf diffq \
      treetable hydra-core submitit retrying openunmix lameenc && v/bin/pip install --no-deps demucs==4.0.1 dora-search
  v/bin/python tools/export_stems_skeleton.py <path to htdemucs .th> <out dir> [--check]

The Demucs architecture code is MIT-licensed upstream; the checkpoint is only used here to size the graph and to build
the reference for --check. The output contains NO weight values:
  htdemucs.onnx          graph only; every weight is an ONNX external-data reference into "htdemucs.weights"
  htdemucs.layout.json   for each weight: checkpoint key, byte offset and length in that file, shape (float32, row-major)
The model core takes the STFT magnitude (complex-as-channels) and the waveform, and returns the stem spectrogram and the
time-branch waveform; the STFT, mask and inverse STFT around it are done in C++ (see docs/STEMS.md).
"""
import json, os, sys
import numpy as np
import onnx
import torch
from onnx import external_data_helper as edh
from demucs.htdemucs import HTDemucs


class Core(torch.nn.Module):
    """HTDemucs.forward between the STFT and the inverse STFT (same ops, copied from demucs 4.0.1)."""
    def __init__(s, m):
        super().__init__(); s.m = m

    def forward(s, x, mix):
        m = s.m
        B, C, Fq, T = x.shape
        mean = x.mean(dim=(1, 2, 3), keepdim=True); std = x.std(dim=(1, 2, 3), keepdim=True)
        x = (x - mean) / (1e-5 + std)
        xt = mix; meant = xt.mean(dim=(1, 2), keepdim=True); stdt = xt.std(dim=(1, 2), keepdim=True)
        xt = (xt - meant) / (1e-5 + stdt)
        saved, saved_t, lengths, lengths_t = [], [], [], []
        for idx, encode in enumerate(m.encoder):
            lengths.append(x.shape[-1]); inject = None
            if idx < len(m.tencoder):
                lengths_t.append(xt.shape[-1]); tenc = m.tencoder[idx]; xt = tenc(xt)
                if not tenc.empty: saved_t.append(xt)
                else: inject = xt
            x = encode(x, inject)
            if idx == 0 and m.freq_emb is not None:
                frs = torch.arange(x.shape[-2]); emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x); x = x + m.freq_emb_scale * emb
            saved.append(x)
        if m.bottom_channels:
            b, c, f, t = x.shape
            x = x.reshape(b, c, f * t); x = m.channel_upsampler(x); x = x.reshape(b, -1, f, t); xt = m.channel_upsampler_t(xt)
        x, xt = m.crosstransformer(x, xt)
        if m.bottom_channels:
            x = x.reshape(b, x.shape[1], f * t); x = m.channel_downsampler(x); x = x.reshape(b, -1, f, t); xt = m.channel_downsampler_t(xt)
        for idx, decode in enumerate(m.decoder):
            skip = saved.pop(-1); x, pre = decode(x, skip, lengths.pop(-1))
            offset = m.depth - len(m.tdecoder)
            if idx >= offset:
                tdec = m.tdecoder[idx - offset]; length_t = lengths_t.pop(-1)
                if tdec.empty: pre = pre[:, :, 0]; xt, _ = tdec(pre, None, length_t)
                else: skip = saved_t.pop(-1); xt, _ = tdec(xt, skip, length_t)
        S = len(m.sources)
        x = x.view(B, S, -1, Fq, T) * std[:, None] + mean[:, None]
        xt = xt.view(B, S, -1, mix.shape[-1]) * stdt[:, None] + meant[:, None]
        return x, xt


def main():
    ckpt, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    pk = torch.load(ckpt, map_location="cpu", weights_only=False)
    m = HTDemucs(*pk["args"], **pk["kwargs"]); m.load_state_dict(pk["state"]); m.eval()
    assert m.cac and m.nfft == 4096 and m.hop_length == 1024 and m.samplerate == 44100
    L = int(m.segment * m.samplerate)
    mix = torch.randn(1, 2, L) * 0.1
    spec = m._magnitude(m._spec(mix))
    core = Core(m).eval()
    tmp = os.path.join(out, "_tmp.onnx")
    torch.onnx.export(core, (spec, mix), tmp, input_names=["spec", "mix"], output_names=["xspec", "xtime"],
                      opset_version=17, dynamo=False, do_constant_folding=False)
    mo = onnx.load(tmp); os.remove(tmp)
    sd = {k: v for k, v in pk["state"].items()}
    layout, off = [], 0
    for init in mo.graph.initializer:
        key = init.name[2:] if init.name.startswith("m.") else init.name
        assert key in sd, key
        arr = sd[key]
        n = int(arr.numel()) * 4
        assert init.data_type == onnx.TensorProto.FLOAT and list(init.dims) == list(arr.shape), key
        layout.append({"name": key, "offset": off, "length": n, "shape": list(arr.shape)})
        init.ClearField("raw_data"); init.ClearField("float_data")
        init.data_location = onnx.TensorProto.EXTERNAL
        del init.external_data[:]
        for k, v in (("location", "htdemucs.weights"), ("offset", str(off)), ("length", str(n))):
            e = init.external_data.add(); e.key = k; e.value = v
        off += n
        off = (off + 63) // 64 * 64  # keep every tensor 64-byte aligned
    assert len(layout) == len(sd), (len(layout), len(sd))
    onnx.save_model(mo, os.path.join(out, "htdemucs.onnx"))
    json.dump({"format": "deckhand-stems-layout/1", "weights_file": "htdemucs.weights", "total_bytes": off, "dtype": "float32",
               "alignment": 64, "tensors": layout}, open(os.path.join(out, "htdemucs.layout.json"), "w"), separators=(",", ":"))
    print("skeleton:", os.path.getsize(os.path.join(out, "htdemucs.onnx")), "bytes;", len(layout), "tensors;", off, "bytes of weights expected")
    if "--check" in sys.argv:  # reference weights file written by this script, for comparison with the C++ installer's
        with open(os.path.join(out, "htdemucs.reference.weights"), "wb") as f:
            for t in layout:
                f.seek(t["offset"]); f.write(sd[t["name"]].float().contiguous().numpy().tobytes())
            f.truncate(off)
        torch.save({"spec": spec, "mix": mix}, os.path.join(out, "check_inputs.pt"))


if __name__ == "__main__":
    main()
