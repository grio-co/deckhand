// Matrix, convolution and normalisation operators (the numeric heavy lifting) plus the operator registry.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "ops.hpp"

namespace deckhand::stems::graph {
namespace {

constexpr std::int64_t kGrain = 1 << 14;
std::int64_t g_tile_floats = 4 << 20;  // work-buffer budget per tile (floats)
std::int64_t g_tile_min = 256;         // never fewer columns than this per tile

// sgemm that also uses the thread pool when the backend itself is single threaded.
void gemm_mt(OpCtx& c, bool ta, bool tb, int M, int N, int K, float alpha, const float* A, int lda, const float* B, int ldb,
             float beta, float* C, int ldc) {
  const bool backend_threads = std::string(kernel_backend_name()) != "portable";
  if (backend_threads || c.pool->threads() == 1 || std::int64_t(M) * N * K < (1 << 20) || M < 8) {
    sgemm(ta, tb, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
    return;
  }
  c.par(M, 4, [&](std::int64_t b, std::int64_t e) {
    sgemm(ta, tb, int(e - b), N, K, alpha, ta ? A + b : A + b * lda, lda, B, ldb, beta, C + b * ldc, ldc);
  });
}

void op_matmul(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "MatMul");
  const Tensor& b = need(in, nin, 1, "MatMul");
  if (a.dt != DType::F32 || b.dt != DType::F32) throw GraphError("MatMul: float operands only");
  if (a.shape.rank == 0 || b.shape.rank == 0) throw GraphError("MatMul: scalar operand");
  Shape as = a.shape, bs = b.shape;
  const bool a1 = as.rank == 1, b1 = bs.rank == 1;
  if (a1) { Shape t{1, as[0]}; as = t; }
  if (b1) { Shape t{bs[0], 1}; bs = t; }
  const std::int64_t M = as[as.rank - 2], K = as[as.rank - 1], N = bs[bs.rank - 1];
  if (bs[bs.rank - 2] != K) throw GraphError("MatMul: inner dimensions differ: " + a.shape.str() + " x " + b.shape.str());
  Shape ab, bb;
  for (int d = 0; d < as.rank - 2; ++d) ab.push(as[d]);
  for (int d = 0; d < bs.rank - 2; ++d) bb.push(bs[d]);
  int br = std::max(ab.rank, bb.rank);
  Shape ob;
  for (int d = 0; d < br; ++d) {
    int ia = d - (br - ab.rank), ib = d - (br - bb.rank);
    std::int64_t x = ia >= 0 ? ab[ia] : 1, y = ib >= 0 ? bb[ib] : 1;
    if (x != y && x != 1 && y != 1) throw GraphError("MatMul: batch dimensions do not broadcast");
    ob.push(x == 1 ? y : x);
  }
  Shape os = ob;
  if (!a1) os.push(M);
  if (!b1) os.push(N);
  c.alloc(0, DType::F32, os);
  float* O = c.outs[0].f();
  const float *A = a.f(), *B = b.f();
  const std::int64_t batches = ob.numel();
  if (batches == 0 || M * N == 0) return;
  if (K == 0) { std::fill_n(O, os.numel(), 0.f); return; }
  if (bb.numel() == 1 && (bb.rank == 0 || true) && bb.numel() == 1) {
    // B is shared by every batch: fold the batches of A into the rows of one product.
    if (ab.numel() == batches) {
      gemm_mt(c, false, false, int(batches * M), int(N), int(K), 1.f, A, int(K), B, int(N), 0.f, O, int(N));
      return;
    }
  }
  // General batched product with broadcast batch strides.
  std::int64_t sa[kMaxRank] = {0}, sb[kMaxRank] = {0};
  {
    std::int64_t ra = M * K, rb = K * N;
    for (int d = br - 1; d >= 0; --d) {
      int ia = d - (br - ab.rank), ib = d - (br - bb.rank);
      sa[d] = (ia >= 0 && ab[ia] != 1) ? ra : 0;
      sb[d] = (ib >= 0 && bb[ib] != 1) ? rb : 0;
      if (ia >= 0) ra *= ab[ia];
      if (ib >= 0) rb *= bb[ib];
    }
  }
  auto one = [&](std::int64_t q) {
    std::int64_t rem = q, oa = 0, ob_ = 0;
    for (int d = br - 1; d >= 0; --d) { std::int64_t i = rem % ob[d]; rem /= ob[d]; oa += i * sa[d]; ob_ += i * sb[d]; }
    sgemm(false, false, int(M), int(N), int(K), 1.f, A + oa, int(K), B + ob_, int(N), 0.f, O + q * M * N, int(N));
  };
  if (batches == 1) gemm_mt(c, false, false, int(M), int(N), int(K), 1.f, A, int(K), B, int(N), 0.f, O, int(N));
  else c.par(batches, 1, [&](std::int64_t b0, std::int64_t b1) { for (std::int64_t q = b0; q < b1; ++q) one(q); });
}

void op_gemm(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Gemm");
  const Tensor& b = need(in, nin, 1, "Gemm");
  if (a.dt != DType::F32 || b.dt != DType::F32 || a.shape.rank != 2 || b.shape.rank != 2) throw GraphError("Gemm: needs 2-D float operands");
  bool ta = attr_int(n, "transA", 0) != 0, tb = attr_int(n, "transB", 0) != 0;
  float alpha = attr_float(n, "alpha", 1.f), beta = attr_float(n, "beta", 1.f);
  std::int64_t M = ta ? a.shape[1] : a.shape[0], K = ta ? a.shape[0] : a.shape[1];
  std::int64_t N = tb ? b.shape[0] : b.shape[1], K2 = tb ? b.shape[1] : b.shape[0];
  if (K != K2) throw GraphError("Gemm: inner dimensions differ");
  c.alloc(0, DType::F32, Shape{M, N});
  float* O = c.outs[0].f();
  float bscale = 0.f;
  if (nin > 2 && in[2] && beta != 0.f) {
    const Tensor& cc = *in[2];
    if (cc.dt != DType::F32) throw GraphError("Gemm: bias must be float");
    // unidirectional broadcast of C to [M,N]
    std::int64_t cm = cc.shape.rank == 2 ? cc.shape[0] : 1, cn = cc.shape.rank >= 1 ? cc.shape[cc.shape.rank - 1] : 1;
    if ((cm != 1 && cm != M) || (cn != 1 && cn != N) || cc.shape.rank > 2) throw GraphError("Gemm: bias does not broadcast");
    for (std::int64_t i = 0; i < M; ++i)
      for (std::int64_t j = 0; j < N; ++j) O[i * N + j] = beta * cc.f()[(cm == 1 ? 0 : i) * cn + (cn == 1 ? 0 : j)];
    bscale = 1.f;
  }
  gemm_mt(c, ta, tb, int(M), int(N), int(K), alpha, a.f(), int(a.shape[1]), b.f(), int(b.shape[1]), bscale, O, int(N));
}

// ---- convolution ---------------------------------------------------------------------------------------------------
struct ConvGeom {
  std::int64_t N, C, H, W, M, kh, kw, sh, sw, dh, dw, pt, pl, pb, pr, g;
  bool one_d;
};

ConvGeom conv_geom(const NodeProto& n, const Tensor& x, const Tensor& w, bool transpose) {
  if (x.dt != DType::F32 || w.dt != DType::F32) throw GraphError(n.op_type + ": float operands only");
  if (attr_str(n, "auto_pad", "NOTSET") != "NOTSET") throw GraphError(n.op_type + ": auto_pad is not supported");
  ConvGeom g{};
  if (x.shape.rank != 3 && x.shape.rank != 4) throw GraphError(n.op_type + ": only 1-D and 2-D are supported, got input " + x.shape.str());
  if (w.shape.rank != x.shape.rank) throw GraphError(n.op_type + ": weight rank differs from input rank");
  g.one_d = x.shape.rank == 3;
  g.N = x.shape[0]; g.C = x.shape[1];
  g.H = g.one_d ? 1 : x.shape[2]; g.W = x.shape[x.shape.rank - 1];
  g.M = w.shape[0];
  g.kh = g.one_d ? 1 : w.shape[2]; g.kw = w.shape[w.shape.rank - 1];
  g.g = attr_int(n, "group", 1);
  if (g.g < 1) throw GraphError(n.op_type + ": bad group");
  auto ints = [&](const char* nm, std::int64_t def, std::size_t want) {
    bool has;
    auto v = attr_ints(n, nm, &has);
    if (!has) v.assign(want, def);
    if (v.size() != want) throw GraphError(n.op_type + ": attribute " + nm + " has the wrong length");
    for (auto e : v) if (e < 0 || (std::string(nm) != "pads" && e == 0)) throw GraphError(n.op_type + ": bad value in " + nm);
    return v;
  };
  const std::size_t sp = g.one_d ? 1 : 2;
  auto ks = attr_ints(n, "kernel_shape");
  if (!ks.empty()) {
    if (ks.size() != sp || ks[sp - 1] != g.kw || (!g.one_d && ks[0] != g.kh)) throw GraphError(n.op_type + ": kernel_shape disagrees with the weight");
  }
  auto st = ints("strides", 1, sp), di = ints("dilations", 1, sp), pd = ints("pads", 0, 2 * sp);
  if (g.one_d) { g.sh = 1; g.dh = 1; g.pt = g.pb = 0; g.sw = st[0]; g.dw = di[0]; g.pl = pd[0]; g.pr = pd[1]; }
  else { g.sh = st[0]; g.sw = st[1]; g.dh = di[0]; g.dw = di[1]; g.pt = pd[0]; g.pl = pd[1]; g.pb = pd[2]; g.pr = pd[3]; }
  if (const Attribute* op = n.attr("output_padding")) for (auto v : op->ints) if (v != 0) throw GraphError(n.op_type + ": output_padding is not supported");
  if (n.attr("output_shape")) throw GraphError(n.op_type + ": output_shape is not supported");
  if (!transpose) {
    if (g.C != w.shape[1] * g.g || g.M % g.g) throw GraphError("Conv: channel/group mismatch (input " + x.shape.str() + ", weight " + w.shape.str() + ")");
  } else {
    if (g.C != w.shape[0] || g.C % g.g) throw GraphError("ConvTranspose: channel/group mismatch");
  }
  return g;
}

void op_conv(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& x = need(in, nin, 0, "Conv");
  const Tensor& w = need(in, nin, 1, "Conv");
  const Tensor* bias = nin > 2 ? in[2] : nullptr;
  ConvGeom g = conv_geom(n, x, w, false);
  const std::int64_t Cg = g.C / g.g, Mg = g.M / g.g;
  const std::int64_t Ho = (g.H + g.pt + g.pb - g.dh * (g.kh - 1) - 1) / g.sh + 1;
  const std::int64_t Wo = (g.W + g.pl + g.pr - g.dw * (g.kw - 1) - 1) / g.sw + 1;
  if (Ho <= 0 || Wo <= 0 || g.H + g.pt + g.pb < g.dh * (g.kh - 1) + 1 || g.W + g.pl + g.pr < g.dw * (g.kw - 1) + 1)
    throw GraphError("Conv: kernel larger than the padded input");
  if (bias && (bias->dt != DType::F32 || bias->numel() != g.M)) throw GraphError("Conv: bad bias");
  Shape os = g.one_d ? Shape{g.N, g.M, Wo} : Shape{g.N, g.M, Ho, Wo};
  c.alloc(0, DType::F32, os);
  const std::int64_t K = Cg * g.kh * g.kw, cols = Ho * Wo;
  const bool direct = g.kh == 1 && g.kw == 1 && g.sh == 1 && g.sw == 1 && g.pt == 0 && g.pl == 0 && g.pb == 0 && g.pr == 0;
  // Work in column tiles so the im2col buffer stays small (about 16 MB) however large the output is.
  const std::int64_t tile = direct ? cols : std::min<std::int64_t>(cols, std::max<std::int64_t>(g_tile_min, g_tile_floats / std::max<std::int64_t>(1, K)));
  float* col = direct ? nullptr : c.scratch_f(0, std::size_t(K * tile));
  for (std::int64_t b = 0; b < g.N; ++b) {
    for (std::int64_t gr = 0; gr < g.g; ++gr) {
      const float* xin = x.f() + (b * g.C + gr * Cg) * g.H * g.W;
      float* out = c.outs[0].f() + (b * g.M + gr * Mg) * cols;
      float beta = 0.f;
      if (bias) {
        for (std::int64_t m = 0; m < Mg; ++m) std::fill_n(out + m * cols, cols, bias->f()[gr * Mg + m]);
        beta = 1.f;
      }
      if (direct) {
        gemm_mt(c, false, false, int(Mg), int(cols), int(K), 1.f, w.f() + gr * Mg * K, int(K), xin, int(cols), beta, out, int(cols));
        continue;
      }
      for (std::int64_t p0 = 0; p0 < cols; p0 += tile) {
        const std::int64_t tl = std::min(tile, cols - p0);
        c.par(K, std::max<std::int64_t>(1, kGrain / std::max<std::int64_t>(1, tl)), [&](std::int64_t r0, std::int64_t r1) {
          for (std::int64_t r = r0; r < r1; ++r) {
            std::int64_t kx = r % g.kw, ky = (r / g.kw) % g.kh, ch = r / (g.kw * g.kh);
            const float* plane = xin + ch * g.H * g.W;
            float* dst = col + r * tl;
            const std::int64_t off = kx * g.dw - g.pl;  // ix = ox*sw + off
            for (std::int64_t p = p0; p < p0 + tl;) {
              std::int64_t oy = p / Wo, ox0 = p % Wo, seg = std::min(Wo - ox0, p0 + tl - p);
              float* d = dst + (p - p0);
              std::int64_t iy = oy * g.sh - g.pt + ky * g.dh;
              if (iy < 0 || iy >= g.H) std::fill(d, d + seg, 0.f);
              else {
                const float* srow = plane + iy * g.W;
                for (std::int64_t k = 0; k < seg; ++k) {
                  std::int64_t ix = (ox0 + k) * g.sw + off;
                  d[k] = (ix >= 0 && ix < g.W) ? srow[ix] : 0.f;
                }
              }
              p += seg;
            }
          }
        });
        gemm_mt(c, false, false, int(Mg), int(tl), int(K), 1.f, w.f() + gr * Mg * K, int(K), col, int(tl), beta, out + p0, int(cols));
      }
    }
  }
}

void op_convtranspose(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& x = need(in, nin, 0, "ConvTranspose");
  const Tensor& w = need(in, nin, 1, "ConvTranspose");
  const Tensor* bias = nin > 2 ? in[2] : nullptr;
  ConvGeom g = conv_geom(n, x, w, true);
  const std::int64_t Cg = g.C / g.g, Mg = w.shape[1], Mout = Mg * g.g;
  const std::int64_t Ho = (g.H - 1) * g.sh - g.pt - g.pb + g.dh * (g.kh - 1) + 1;
  const std::int64_t Wo = (g.W - 1) * g.sw - g.pl - g.pr + g.dw * (g.kw - 1) + 1;
  if (Ho <= 0 || Wo <= 0) throw GraphError("ConvTranspose: empty output");
  if (bias && (bias->dt != DType::F32 || bias->numel() != Mout)) throw GraphError("ConvTranspose: bad bias");
  Shape os = g.one_d ? Shape{g.N, Mout, Wo} : Shape{g.N, Mout, Ho, Wo};
  c.alloc(0, DType::F32, os);
  const std::int64_t hw = g.H * g.W, kk = g.kh * g.kw, rows = Mg * kk;
  const std::int64_t tile = std::min<std::int64_t>(hw, std::max<std::int64_t>(g_tile_min, g_tile_floats / std::max<std::int64_t>(1, rows)));
  float* col = c.scratch_f(0, std::size_t(rows * tile));
  for (std::int64_t b = 0; b < g.N; ++b) {
    for (std::int64_t gr = 0; gr < g.g; ++gr) {
      const float* xin = x.f() + (b * g.C + gr * Cg) * hw;
      float* out = c.outs[0].f() + (b * Mout + gr * Mg) * Ho * Wo;
      c.par(Mg, 1, [&](std::int64_t m0, std::int64_t m1) {
        for (std::int64_t m = m0; m < m1; ++m) std::fill_n(out + m * Ho * Wo, Ho * Wo, bias ? bias->f()[gr * Mg + m] : 0.f);
      });
      for (std::int64_t p0 = 0; p0 < hw; p0 += tile) {
        const std::int64_t tl = std::min(tile, hw - p0);
        gemm_mt(c, true, false, int(rows), int(tl), int(Cg), 1.f, w.f() + gr * Cg * rows, int(rows), xin + p0, int(hw), 0.f, col, int(tl));
        c.par(Mg, 1, [&](std::int64_t m0, std::int64_t m1) {
          for (std::int64_t m = m0; m < m1; ++m) {
            float* o = out + m * Ho * Wo;
            for (std::int64_t ky = 0; ky < g.kh; ++ky)
              for (std::int64_t kx = 0; kx < g.kw; ++kx) {
                const float* src = col + ((m * g.kh + ky) * g.kw + kx) * tl;
                for (std::int64_t p = p0; p < p0 + tl;) {
                  std::int64_t iy = p / g.W, ix0 = p % g.W, seg = std::min(g.W - ix0, p0 + tl - p);
                  std::int64_t oy = iy * g.sh - g.pt + ky * g.dh;
                  if (oy >= 0 && oy < Ho) {
                    float* orow = o + oy * Wo;
                    const float* srow = src + (p - p0);
                    for (std::int64_t k = 0; k < seg; ++k) {
                      std::int64_t ox = (ix0 + k) * g.sw - g.pl + kx * g.dw;
                      if (ox >= 0 && ox < Wo) orow[ox] += srow[k];
                    }
                  }
                  p += seg;
                }
              }
          }
        });
      }
    }
  }
}

// ---- softmax and normalisations ----------------------------------------------------------------------------------------
void op_softmax(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Softmax");
  if (a.dt != DType::F32) throw GraphError("Softmax: float only");
  int ax = norm_axis(attr_int(n, "axis", -1), a.shape.rank, "Softmax");
  std::int64_t outer = 1, inner = 1, dim = a.shape[ax];
  for (int d = 0; d < ax; ++d) outer *= a.shape[d];
  for (int d = ax + 1; d < a.shape.rank; ++d) inner *= a.shape[d];
  c.alloc(0, DType::F32, a.shape);
  const float* X = a.f();
  float* O = c.outs[0].f();
  if (inner == 1) {  // rows are contiguous: subtract the maximum, exponentiate with the vector routine, normalise
    c.par(outer, std::max<std::int64_t>(1, kGrain / std::max<std::int64_t>(1, dim)), [&](std::int64_t b, std::int64_t e) {
      for (std::int64_t r = b; r < e; ++r) {
        const float* x = X + r * dim;
        float* y = O + r * dim;
        float mx = -INFINITY;
        for (std::int64_t k = 0; k < dim; ++k) mx = std::max(mx, x[k]);
        for (std::int64_t k = 0; k < dim; ++k) y[k] = x[k] - mx;
        vec_expf(y, y, int(dim));
        float s = 0.f;
        for (std::int64_t k = 0; k < dim; ++k) s += y[k];
        const float inv = 1.f / s;
        for (std::int64_t k = 0; k < dim; ++k) y[k] *= inv;
      }
    });
    return;
  }
  c.par(outer * inner, std::max<std::int64_t>(1, kGrain / std::max<std::int64_t>(1, dim)), [&](std::int64_t b, std::int64_t e) {
    for (std::int64_t q = b; q < e; ++q) {
      std::int64_t o = q / inner, i = q % inner;
      const float* x = X + o * dim * inner + i;
      float* y = O + o * dim * inner + i;
      float mx = -INFINITY;
      for (std::int64_t k = 0; k < dim; ++k) mx = std::max(mx, x[k * inner]);
      float s = 0.f;
      for (std::int64_t k = 0; k < dim; ++k) { float v = std::exp(x[k * inner] - mx); y[k * inner] = v; s += v; }
      float inv = 1.f / s;
      for (std::int64_t k = 0; k < dim; ++k) y[k * inner] *= inv;
    }
  });
}

void op_layernorm(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& x = need(in, nin, 0, "LayerNormalization");
  const Tensor& sc = need(in, nin, 1, "LayerNormalization");
  const Tensor* bi = nin > 2 ? in[2] : nullptr;
  if (x.dt != DType::F32 || sc.dt != DType::F32) throw GraphError("LayerNormalization: float only");
  int ax = norm_axis(attr_int(n, "axis", -1), x.shape.rank, "LayerNormalization");
  float eps = attr_float(n, "epsilon", 1e-5f);
  std::int64_t outer = 1, D = 1;
  for (int d = 0; d < ax; ++d) outer *= x.shape[d];
  for (int d = ax; d < x.shape.rank; ++d) D *= x.shape[d];
  if (sc.numel() != D || (bi && bi->numel() != D)) throw GraphError("LayerNormalization: scale/bias size does not match the normalised dimensions");
  c.alloc(0, DType::F32, x.shape);
  Shape ms = x.shape;
  for (int d = ax; d < ms.rank; ++d) ms[d] = 1;
  if (c.want(1)) c.alloc(1, DType::F32, ms);
  if (c.want(2)) c.alloc(2, DType::F32, ms);
  c.par(outer, std::max<std::int64_t>(1, kGrain / D), [&](std::int64_t b, std::int64_t e) {
    for (std::int64_t r = b; r < e; ++r) {
      const float* p = x.f() + r * D;
      double s = 0;
      for (std::int64_t i = 0; i < D; ++i) s += p[i];
      double mean = s / double(D), v = 0;
      for (std::int64_t i = 0; i < D; ++i) { double d = p[i] - mean; v += d * d; }
      float inv = float(1.0 / std::sqrt(v / double(D) + double(eps)));
      float m = float(mean);
      float* o = c.outs[0].f() + r * D;
      const float *g = sc.f(), *bb = bi ? bi->f() : nullptr;
      for (std::int64_t i = 0; i < D; ++i) o[i] = (p[i] - m) * inv * g[i] + (bb ? bb[i] : 0.f);
      if (c.want(1)) c.outs[1].f()[r] = m;
      if (c.want(2)) c.outs[2].f()[r] = inv;
    }
  });
}

void op_instancenorm(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& x = need(in, nin, 0, "InstanceNormalization");
  const Tensor& sc = need(in, nin, 1, "InstanceNormalization");
  const Tensor& bi = need(in, nin, 2, "InstanceNormalization");
  if (x.dt != DType::F32 || x.shape.rank < 3) throw GraphError("InstanceNormalization: needs a float tensor [N,C,...]");
  float eps = attr_float(n, "epsilon", 1e-5f);
  std::int64_t N = x.shape[0], C = x.shape[1], S = x.numel() / std::max<std::int64_t>(1, N * C);
  if (sc.numel() != C || bi.numel() != C) throw GraphError("InstanceNormalization: scale/bias size");
  c.alloc(0, DType::F32, x.shape);
  c.par(N * C, std::max<std::int64_t>(1, kGrain / std::max<std::int64_t>(1, S)), [&](std::int64_t b, std::int64_t e) {
    for (std::int64_t q = b; q < e; ++q) {
      const float* p = x.f() + q * S;
      double s = 0;
      for (std::int64_t i = 0; i < S; ++i) s += p[i];
      double mean = s / double(S), v = 0;
      for (std::int64_t i = 0; i < S; ++i) { double d = p[i] - mean; v += d * d; }
      float inv = float(1.0 / std::sqrt(v / double(S) + double(eps)));
      std::int64_t ch = q % C;
      float g = sc.f()[ch] * inv, o0 = bi.f()[ch] - float(mean) * g;
      float* o = c.outs[0].f() + q * S;
      for (std::int64_t i = 0; i < S; ++i) o[i] = p[i] * g + o0;
    }
  });
}

}  // namespace

void set_conv_tiling_for_test(std::int64_t floats, std::int64_t min_cols) { g_tile_floats = floats; g_tile_min = min_cols; }

void register_nn_ops(std::vector<std::pair<std::string, OpFn>>& v) {
  v.push_back({"MatMul", &op_matmul}); v.push_back({"Gemm", &op_gemm}); v.push_back({"Conv", &op_conv});
  v.push_back({"ConvTranspose", &op_convtranspose}); v.push_back({"Softmax", &op_softmax});
  v.push_back({"LayerNormalization", &op_layernorm}); v.push_back({"InstanceNormalization", &op_instancenorm});
}

namespace {
const std::unordered_map<std::string, OpFn>& registry() {
  static const std::unordered_map<std::string, OpFn> r = [] {
    std::vector<std::pair<std::string, OpFn>> v;
    register_basic_ops(v);
    register_nn_ops(v);
    return std::unordered_map<std::string, OpFn>(v.begin(), v.end());
  }();
  return r;
}
}  // namespace

const OpFn* find_op(const std::string& t) {
  auto& r = registry();
  auto it = r.find(t);
  return it == r.end() ? nullptr : &it->second;
}
std::vector<std::string> supported_ops() {
  std::vector<std::string> v;
  for (auto& kv : registry()) v.push_back(kv.first);
  std::sort(v.begin(), v.end());
  return v;
}

}  // namespace deckhand::stems::graph
