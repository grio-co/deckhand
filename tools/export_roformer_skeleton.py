#!/usr/bin/env python3
"""Exports the neural core of the Mel-Band RoFormer vocal model as a weight-less ONNX "skeleton" plus its layout file.

Reference implementation: the PyPI package `bs-roformer` 0.3.10 (MIT, Phil Wang / lucidrains, https://github.com/lucidrains/BS-RoFormer).
Of all PyPI releases only 0.3.10 matches the key names of Kimberley Jensen's checkpoint exactly (0 missing, 0 unexpected keys with
`strict=False`); newer releases changed the module layout (hyper-connections, value residuals). The model settings below are read from the
checkpoint's tensor shapes (dim 384, 6 layers, 60 mel bands, 8 heads of 64, mask MLP depth 2) and the hop of 441 samples that the
public configuration of this model uses; the 8 s chunk of 352800 samples is that configuration's too.

  pip install --no-deps --target v0310 bs-roformer==0.3.10        (the other requirements: torch, einops, rotary-embedding-torch, librosa)
  PYTHONPATH=v0310 python tools/export_roformer_skeleton.py MelBandRoformer.ckpt out_dir

The skeleton covers band-split, the axial time/frequency transformers and the mask estimator. The STFT, the gather of the band
frequencies, the averaging of overlapping band masks, the complex multiplication and the inverse STFT are not in it (the C++ side does
them); `roformer.bands.json` records the tables it needs (frequency index per band entry, bands per frequency).
Writes: roformer.onnx (graph only, weights as external-data references into roformer.weights), roformer.layout.json, roformer.bands.json.
The attention is written out as MatMul/Softmax (the library's own non-flash formula) so the executor needs no Einsum.
"""
import json, os, sys
import torch, onnx
from einops import rearrange

CHUNK = 352800
HOP = 441


def main():
    ckpt, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    import bs_roformer
    from bs_roformer import MelBandRoformer
    from bs_roformer.attend import Attend

    def attend(self, q, k, v):  # same maths as Attend.forward without flash attention, minus the einsum
        scale = q.shape[-1] ** -0.5
        attn = ((q @ k.transpose(-2, -1)) * scale).softmax(dim=-1)
        return attn @ v
    Attend.forward = attend

    sd = torch.load(ckpt, map_location="cpu", weights_only=True)
    m = MelBandRoformer(dim=384, depth=6, stereo=True, num_stems=1, time_transformer_depth=1, freq_transformer_depth=1, num_bands=60,
                        dim_head=64, heads=8, mask_estimator_depth=2, stft_hop_length=HOP, attn_dropout=0, ff_dropout=0, flash_attn=False)
    r = m.load_state_dict(sd, strict=False)
    assert not r.missing_keys and not r.unexpected_keys, (r.missing_keys[:3], r.unexpected_keys[:3])
    m.eval()

    class Core(torch.nn.Module):
        def __init__(s, m):
            super().__init__(); s.m = m
        def forward(s, x):  # x: [1, T, features] -> masks [1, T, 2 * entries]
            m = s.m
            x = m.band_split(x)
            for tt, ft in m.layers:
                x = rearrange(x, 'b t f d -> b f t d')
                b, f, t, d = x.shape
                x = tt(x.reshape(b * f, t, d)).reshape(b, f, t, d)
                x = rearrange(x, 'b f t d -> b t f d')
                b, t, f, d = x.shape
                x = ft(x.reshape(b * t, f, d)).reshape(b, t, f, d)
            return m.mask_estimators[0](x)

    frames = 1 + CHUNK // HOP
    feat = sum(m.band_split.dim_inputs)
    x = torch.randn(1, frames, feat) * 0.1
    core = Core(m).eval()
    with torch.no_grad():
        y = core(x)
    print("core input", tuple(x.shape), "output", tuple(y.shape))
    tmp = os.path.join(out, "_tmp.onnx")
    torch.onnx.export(core, (x,), tmp, input_names=["bands"], output_names=["masks"], opset_version=17, dynamo=False,
                      do_constant_folding=False)
    mo = onnx.load(tmp); os.remove(tmp)
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
        for k, v in (("location", "roformer.weights"), ("offset", str(off)), ("length", str(n))):
            e = init.external_data.add(); e.key = k; e.value = v
        off += n
        off = (off + 63) // 64 * 64
    used = {l["name"] for l in layout}
    missing = [k for k in sd if k not in used]
    print("checkpoint tensors not used by the exported core (folded into constants or unused):", len(missing), missing[:4])
    onnx.save_model(mo, os.path.join(out, "roformer.onnx"))
    json.dump({"format": "deckhand-stems-layout/1", "weights_file": "roformer.weights", "total_bytes": off, "dtype": "float32",
               "alignment": 64, "tensors": layout, "unused_checkpoint_keys": missing},
              open(os.path.join(out, "roformer.layout.json"), "w"), separators=(",", ":"))
    json.dump({"chunk": CHUNK, "hop": HOP, "n_fft": 2048, "frames": frames, "features": feat, "stereo": True,
               "freq_indices": m.freq_indices.tolist(), "num_bands_per_freq": m.num_bands_per_freq.tolist(),
               "num_freqs_per_band": m.num_freqs_per_band.tolist()}, open(os.path.join(out, "roformer.bands.json"), "w"), separators=(",", ":"))
    print("skeleton:", os.path.getsize(os.path.join(out, "roformer.onnx")), "bytes;", len(layout), "tensors;", off, "bytes of weights")
    if "--check" in sys.argv:
        with open(os.path.join(out, "roformer.reference.weights"), "wb") as f:
            for t in layout:
                f.seek(t["offset"]); f.write(sd[t["name"]].float().contiguous().numpy().tobytes())
            f.truncate(off)
        torch.save({"x": x, "y": y}, os.path.join(out, "check_io.pt"))


if __name__ == "__main__":
    main()
