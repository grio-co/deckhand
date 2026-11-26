// Elementwise, shape and data-movement operators.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

#include "ops.hpp"

namespace deckhand::stems::graph {

// ---- shared helpers -------------------------------------------------------------------------------------------------
std::int64_t attr_int(const NodeProto& n, const char* name, std::int64_t def) {
  const Attribute* a = n.attr(name);
  return a ? a->i : def;
}
float attr_float(const NodeProto& n, const char* name, float def) {
  const Attribute* a = n.attr(name);
  return a ? a->f : def;
}
std::string attr_str(const NodeProto& n, const char* name, const std::string& def) {
  const Attribute* a = n.attr(name);
  return a ? a->s : def;
}
std::vector<std::int64_t> attr_ints(const NodeProto& n, const char* name, bool* present) {
  const Attribute* a = n.attr(name);
  if (present) *present = a != nullptr;
  return a ? a->ints : std::vector<std::int64_t>{};
}
std::vector<std::int64_t> tensor_i64(const Tensor& t) {
  if (t.dt != DType::I64) throw GraphError("expected an int64 tensor");
  return std::vector<std::int64_t>(t.i(), t.i() + t.numel());
}
const Tensor& need(const Tensor* const* in, int nin, int i, const char* what) {
  if (i >= nin || !in[i]) throw GraphError(std::string(what) + ": missing input " + std::to_string(i));
  return *in[i];
}

namespace {

constexpr std::int64_t kGrain = 1 << 14;

// ---- strided iteration --------------------------------------------------------------------------------------------
struct BC {
  int n = 0, nop = 0;
  std::int64_t size[kMaxRank]{};
  std::int64_t st[3][kMaxRank]{};
};

// Builds a coalesced iteration over `out` where operand o advances by st[o][d] elements along dim d.
BC make_bc(const Shape& out, const std::int64_t* const* strides, int nop) {
  BC b;
  b.nop = nop;
  std::int64_t sz[kMaxRank], st[3][kMaxRank];
  int n = 0;
  for (int d = 0; d < out.rank; ++d) {
    if (out[d] == 1) continue;
    sz[n] = out[d];
    for (int o = 0; o < nop; ++o) st[o][n] = strides[o][d];
    ++n;
  }
  int m = 0;
  for (int d = 0; d < n; ++d) {
    if (m > 0) {
      bool merge = true;
      for (int o = 0; o < nop; ++o)
        if (b.st[o][m - 1] != st[o][d] * sz[d]) merge = false;
      if (merge) { b.size[m - 1] *= sz[d]; for (int o = 0; o < nop; ++o) b.st[o][m - 1] = st[o][d]; continue; }
    }
    b.size[m] = sz[d];
    for (int o = 0; o < nop; ++o) b.st[o][m] = st[o][d];
    ++m;
  }
  if (m == 0) { m = 1; b.size[0] = 1; for (int o = 0; o < nop; ++o) b.st[o][0] = 0; }
  b.n = m;
  return b;
}

// Broadcast strides (in elements) of an operand against `out`.
void bstrides(const Shape& in, const Shape& out, std::int64_t* st) {
  std::int64_t run = 1;
  for (int d = out.rank - 1; d >= 0; --d) {
    int id = d - (out.rank - in.rank);
    if (id < 0) { st[d] = 0; continue; }
    st[d] = in[id] == 1 ? 0 : run;
    run *= in[id];
  }
}

Shape bcast_shape(const Shape& a, const Shape& b) {
  Shape o;
  int r = std::max(a.rank, b.rank);
  for (int d = 0; d < r; ++d) {
    int ia = d - (r - a.rank), ib = d - (r - b.rank);
    std::int64_t x = ia >= 0 ? a[ia] : 1, y = ib >= 0 ? b[ib] : 1;
    if (x != y && x != 1 && y != 1) throw GraphError("cannot broadcast " + a.str() + " with " + b.str());
    o.push(x == 1 ? y : x);
  }
  return o;
}

// fn(out_offset, inner_len, element offsets per operand); inner strides are bc.st[o][n-1].
template <class Fn>
void bc_rows(OpCtx& c, const BC& b, Fn fn) {
  const int n = b.n;
  const std::int64_t inner = b.size[n - 1];
  std::int64_t rows = 1;
  for (int d = 0; d < n - 1; ++d) rows *= b.size[d];
  if (rows < 8 && inner >= 4 * kGrain) {  // few long rows: split each row across threads
    std::int64_t idx[kMaxRank] = {0}, off[3] = {0, 0, 0};
    for (std::int64_t row = 0; row < rows; ++row) {
      std::int64_t rem = row;
      for (int o = 0; o < b.nop; ++o) off[o] = 0;
      for (int d = n - 2; d >= 0; --d) { idx[d] = rem % b.size[d]; rem /= b.size[d]; for (int o = 0; o < b.nop; ++o) off[o] += idx[d] * b.st[o][d]; }
      c.par(inner, kGrain, [&](std::int64_t s0, std::int64_t s1) {
        std::int64_t o2[3];
        for (int o = 0; o < b.nop; ++o) o2[o] = off[o] + s0 * b.st[o][n - 1];
        fn(row * inner + s0, s1 - s0, o2);
      });
    }
    return;
  }
  std::int64_t grain = std::max<std::int64_t>(1, kGrain / std::max<std::int64_t>(1, inner));
  c.par(rows, grain, [&](std::int64_t r0, std::int64_t r1) {
    std::int64_t idx[kMaxRank] = {0}, off[3] = {0, 0, 0};
    std::int64_t r = r0;
    for (int d = n - 2; d >= 0; --d) {
      idx[d] = r % b.size[d];
      r /= b.size[d];
      for (int o = 0; o < b.nop; ++o) off[o] += idx[d] * b.st[o][d];
    }
    for (std::int64_t row = r0; row < r1; ++row) {
      fn(row * inner, inner, off);
      for (int d = n - 2; d >= 0; --d) {
        ++idx[d];
        for (int o = 0; o < b.nop; ++o) off[o] += b.st[o][d];
        if (idx[d] < b.size[d]) break;
        for (int o = 0; o < b.nop; ++o) off[o] -= b.st[o][d] * b.size[d];
        idx[d] = 0;
      }
    }
  });
}

// out (contiguous, shape `os`) = in viewed with element strides `st` and offset `base`.
template <class T>
void strided_copy(OpCtx& c, T* out, const Shape& os, const T* in, const std::int64_t* st) {
  const std::int64_t* sp[1] = {st};
  BC b = make_bc(os, sp, 1);
  const std::int64_t is = b.st[0][b.n - 1];
  bc_rows(c, b, [&](std::int64_t oo, std::int64_t len, const std::int64_t* off) {
    T* o = out + oo;
    const T* s = in + off[0];
    if (is == 1) std::memcpy(o, s, std::size_t(len) * sizeof(T));
    else if (is == 0) std::fill(o, o + len, *s);
    else for (std::int64_t i = 0; i < len; ++i) o[i] = s[i * is];
  });
}

void strides_of(const Shape& s, std::int64_t* st) {
  std::int64_t run = 1;
  for (int d = s.rank - 1; d >= 0; --d) { st[d] = run; run *= s[d]; }
}

// Dispatch on element size (4 or 8 bytes or 1) for pure data movement.
#define DH_BY_SIZE(dt, T, ...)                                       \
  switch (dtype_size(dt)) {                                             \
    case 4: { using T = std::uint32_t; __VA_ARGS__; break; }                   \
    case 8: { using T = std::uint64_t; __VA_ARGS__; break; }                   \
    default: { using T = std::uint8_t; __VA_ARGS__; break; }                   \
  }

// ---- elementwise binary ---------------------------------------------------------------------------------------------
struct AddF { template <class T> T operator()(T a, T b) const { return a + b; } };
struct SubF { template <class T> T operator()(T a, T b) const { return a - b; } };
struct MulF { template <class T> T operator()(T a, T b) const { return a * b; } };
struct DivF { template <class T> T operator()(T a, T b) const { return a / b; } };
struct MaxF { template <class T> T operator()(T a, T b) const { return a > b ? a : b; } };
struct MinF { template <class T> T operator()(T a, T b) const { return a < b ? a : b; } };

template <class TI, class TO, class F>
void binary_run(OpCtx& c, const Tensor& a, const Tensor& b, DType odt, F f) {
  Shape os = bcast_shape(a.shape, b.shape);
  c.alloc(0, odt, os);
  std::int64_t sa[kMaxRank], sb[kMaxRank];
  bstrides(a.shape, os, sa);
  bstrides(b.shape, os, sb);
  const std::int64_t* sp[2] = {sa, sb};
  BC bc = make_bc(os, sp, 2);
  const TI* A = reinterpret_cast<const TI*>(a.data);
  const TI* B = reinterpret_cast<const TI*>(b.data);
  TO* O = reinterpret_cast<TO*>(c.outs[0].data);
  const std::int64_t ia = bc.st[0][bc.n - 1], ib = bc.st[1][bc.n - 1];
  bc_rows(c, bc, [&](std::int64_t oo, std::int64_t len, const std::int64_t* off) {
    TO* o = O + oo;
    const TI* x = A + off[0];
    const TI* y = B + off[1];
    if (ia == 1 && ib == 1) for (std::int64_t i = 0; i < len; ++i) o[i] = f(x[i], y[i]);
    else if (ia == 1 && ib == 0) { TI v = *y; for (std::int64_t i = 0; i < len; ++i) o[i] = f(x[i], v); }
    else if (ia == 0 && ib == 1) { TI v = *x; for (std::int64_t i = 0; i < len; ++i) o[i] = f(v, y[i]); }
    else for (std::int64_t i = 0; i < len; ++i) o[i] = f(x[i * ia], y[i * ib]);
  });
}

template <class F>
void op_arith(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, n.op_type.c_str());
  const Tensor& b = need(in, nin, 1, n.op_type.c_str());
  if (nin != 2) throw GraphError(n.op_type + ": only two operands are supported");
  if (a.dt != b.dt) throw GraphError(n.op_type + ": operand types differ");
  if (a.dt == DType::F32) binary_run<float, float>(c, a, b, DType::F32, F{});
  else if (a.dt == DType::I64) {
    if (std::is_same_v<F, DivF>)
      for (std::int64_t i = 0; i < b.numel(); ++i)
        if (b.i()[i] == 0) throw GraphError("Div: integer division by zero");
    binary_run<std::int64_t, std::int64_t>(c, a, b, DType::I64, F{});
  } else throw GraphError(n.op_type + ": unsupported operand type");
}

void op_pow(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Pow");
  Tensor b = need(in, nin, 1, "Pow");
  if (a.dt != DType::F32) throw GraphError("Pow: only a float base is supported");
  if (b.dt == DType::I64) {
    Tensor t = make_owned(DType::F32, b.shape);
    for (std::int64_t i = 0; i < b.numel(); ++i) t.f()[i] = float(b.i()[i]);
    b = t;
  }
  binary_run<float, float>(c, a, b, DType::F32, [](float x, float y) { return std::pow(x, y); });
}

void op_mod(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Mod");
  const Tensor& b = need(in, nin, 1, "Mod");
  if (a.dt != b.dt) throw GraphError("Mod: operand types differ");
  bool fm = attr_int(n, "fmod", 0) != 0;
  if (a.dt == DType::F32) {
    if (!fm) throw GraphError("Mod: float operands need fmod=1");
    binary_run<float, float>(c, a, b, DType::F32, [](float x, float y) { return std::fmod(x, y); });
  } else if (a.dt == DType::I64) {
    for (std::int64_t i = 0; i < b.numel(); ++i)
      if (b.i()[i] == 0) throw GraphError("Mod: integer modulo by zero");
    if (fm) binary_run<std::int64_t, std::int64_t>(c, a, b, DType::I64, [](std::int64_t x, std::int64_t y) { return x % y; });
    else binary_run<std::int64_t, std::int64_t>(c, a, b, DType::I64, [](std::int64_t x, std::int64_t y) {
      std::int64_t r = x % y;
      return (r != 0 && ((r < 0) != (y < 0))) ? r + y : r;
    });
  } else throw GraphError("Mod: unsupported operand type");
}

struct EqF { template <class T> bool operator()(T a, T b) const { return a == b; } };
struct LtF { template <class T> bool operator()(T a, T b) const { return a < b; } };
struct GtF { template <class T> bool operator()(T a, T b) const { return a > b; } };
struct LeF { template <class T> bool operator()(T a, T b) const { return a <= b; } };
struct GeF { template <class T> bool operator()(T a, T b) const { return a >= b; } };
struct AndF { bool operator()(std::uint8_t a, std::uint8_t b) const { return a && b; } };
struct OrF { bool operator()(std::uint8_t a, std::uint8_t b) const { return a || b; } };

template <class F>
void op_compare(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, n.op_type.c_str());
  const Tensor& b = need(in, nin, 1, n.op_type.c_str());
  if (a.dt != b.dt) throw GraphError(n.op_type + ": operand types differ");
  if (a.dt == DType::F32) binary_run<float, std::uint8_t>(c, a, b, DType::Bool, F{});
  else if (a.dt == DType::I64) binary_run<std::int64_t, std::uint8_t>(c, a, b, DType::Bool, F{});
  else binary_run<std::uint8_t, std::uint8_t>(c, a, b, DType::Bool, F{});
}
template <class F>
void op_logic(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, n.op_type.c_str());
  const Tensor& b = need(in, nin, 1, n.op_type.c_str());
  if (a.dt != DType::Bool || b.dt != DType::Bool) throw GraphError(n.op_type + ": needs bool operands");
  binary_run<std::uint8_t, std::uint8_t>(c, a, b, DType::Bool, F{});
}

void op_where(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {
  const Tensor& cond = need(in, nin, 0, "Where");
  const Tensor& x = need(in, nin, 1, "Where");
  const Tensor& y = need(in, nin, 2, "Where");
  if (cond.dt != DType::Bool || x.dt != y.dt) throw GraphError("Where: bad operand types");
  Shape os = bcast_shape(bcast_shape(cond.shape, x.shape), y.shape);
  c.alloc(0, x.dt, os);
  std::int64_t s0[kMaxRank], s1[kMaxRank], s2[kMaxRank];
  bstrides(cond.shape, os, s0); bstrides(x.shape, os, s1); bstrides(y.shape, os, s2);
  const std::int64_t* sp[3] = {s0, s1, s2};
  BC bc = make_bc(os, sp, 3);
  const std::uint8_t* C = cond.b();
  DH_BY_SIZE(x.dt, T, {
    const T* X = static_cast<const T*>(x.data);
    const T* Y = static_cast<const T*>(y.data);
    T* O = static_cast<T*>(c.outs[0].data);
    const std::int64_t i0 = bc.st[0][bc.n - 1], i1 = bc.st[1][bc.n - 1], i2 = bc.st[2][bc.n - 1];
    bc_rows(c, bc, [&](std::int64_t oo, std::int64_t len, const std::int64_t* off) {
      for (std::int64_t i = 0; i < len; ++i) O[oo + i] = C[off[0] + i * i0] ? X[off[1] + i * i1] : Y[off[2] + i * i2];
    });
  });
}

// ---- unary ---------------------------------------------------------------------------------------------------------
template <class F>
void unary_f32(OpCtx& c, const Tensor& a, F f) {
  c.alloc(0, DType::F32, a.shape);
  const float* x = a.f();
  float* o = c.outs[0].f();
  c.par(a.numel(), kGrain, [&](std::int64_t b, std::int64_t e) { for (std::int64_t i = b; i < e; ++i) o[i] = f(x[i]); });
}
#define DH_UNARY(NAME, EXPR)                                                                      \
  void op_##NAME(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {                \
    const Tensor& a = need(in, nin, 0, #NAME);                                                    \
    if (a.dt != DType::F32) throw GraphError(std::string(#NAME) + ": needs a float tensor");      \
    unary_f32(c, a, [](float x) { return EXPR; });                                                \
  }
DH_UNARY(Erf, std::erf(x))
DH_UNARY(Sigmoid, 1.0f / (1.0f + std::exp(-x)))
DH_UNARY(Sqrt, std::sqrt(x))
DH_UNARY(Sin, std::sin(x))
DH_UNARY(Cos, std::cos(x))
DH_UNARY(Tanh, std::tanh(x))
DH_UNARY(Relu, x > 0.f ? x : 0.f)
DH_UNARY(Exp, std::exp(x))
DH_UNARY(Log, std::log(x))
DH_UNARY(Reciprocal, 1.0f / x)
DH_UNARY(Floor, std::floor(x))
DH_UNARY(Ceil, std::ceil(x))
DH_UNARY(GeluExact, 0.5f * x * (1.0f + std::erf(x * 0.70710678118654752f)))

void op_gelu(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Gelu");
  if (a.dt != DType::F32) throw GraphError("Gelu: needs a float tensor");
  if (attr_str(n, "approximate", "none") == "tanh")
    unary_f32(c, a, [](float x) { return 0.5f * x * (1.0f + std::tanh(0.7978845608028654f * (x + 0.044715f * x * x * x))); });
  else op_GeluExact(c, n, in, nin);
}
void op_neg(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Neg");
  if (a.dt == DType::F32) unary_f32(c, a, [](float x) { return -x; });
  else if (a.dt == DType::I64) { c.alloc(0, DType::I64, a.shape); for (std::int64_t i = 0; i < a.numel(); ++i) c.outs[0].i()[i] = -a.i()[i]; }
  else throw GraphError("Neg: unsupported type");
}
void op_abs(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Abs");
  if (a.dt == DType::F32) unary_f32(c, a, [](float x) { return std::fabs(x); });
  else if (a.dt == DType::I64) { c.alloc(0, DType::I64, a.shape); for (std::int64_t i = 0; i < a.numel(); ++i) c.outs[0].i()[i] = std::llabs(a.i()[i]); }
  else throw GraphError("Abs: unsupported type");
}
void op_not(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Not");
  if (a.dt != DType::Bool) throw GraphError("Not: needs bool");
  c.alloc(0, DType::Bool, a.shape);
  for (std::int64_t i = 0; i < a.numel(); ++i) c.outs[0].b()[i] = !a.b()[i];
}
void op_leakyrelu(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "LeakyRelu");
  float al = attr_float(n, "alpha", 0.01f);
  unary_f32(c, a, [al](float x) { return x >= 0.f ? x : al * x; });
}
void op_clip(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Clip");
  if (a.dt != DType::F32) throw GraphError("Clip: needs float");
  float lo = -INFINITY, hi = INFINITY;
  if (nin > 1 && in[1]) lo = in[1]->f()[0];
  if (nin > 2 && in[2]) hi = in[2]->f()[0];
  unary_f32(c, a, [lo, hi](float x) { return std::min(std::max(x, lo), hi); });
}

// ---- Cast / Constant / Shape / views ---------------------------------------------------------------------------------
void op_cast(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Cast");
  DType to;
  switch (attr_int(n, "to", 0)) {
    case 1: case 11: to = DType::F32; break;
    case 6: case 7: case 2: case 3: to = DType::I64; break;
    case 9: to = DType::Bool; break;
    default: throw GraphError("Cast: unsupported target type " + std::to_string(attr_int(n, "to", 0)));
  }
  if (to == a.dt) { c.alias(0, a, a.shape); return; }
  c.alloc(0, to, a.shape);
  const std::int64_t N = a.numel();
  for (std::int64_t i = 0; i < N; ++i) {
    if (a.dt == DType::F32) {
      float v = a.f()[i];
      if (to == DType::I64) c.outs[0].i()[i] = std::isfinite(v) ? std::int64_t(v) : 0; else c.outs[0].b()[i] = v != 0.f;
    } else if (a.dt == DType::I64) {
      if (to == DType::F32) c.outs[0].f()[i] = float(a.i()[i]); else c.outs[0].b()[i] = a.i()[i] != 0;
    } else {
      if (to == DType::F32) c.outs[0].f()[i] = a.b()[i]; else c.outs[0].i()[i] = a.b()[i];
    }
  }
}

void op_constant(OpCtx& c, const NodeProto& n, const Tensor* const*, int) {
  if (const Attribute* a = n.attr("value")) {
    Tensor t = decode_tensor(a->t, nullptr, 0);
    c.alloc(0, t.dt, t.shape);
    std::memcpy(c.outs[0].data, t.data, t.bytes());
  } else if (const Attribute* f = n.attr("value_float")) {
    c.alloc(0, DType::F32, Shape{}); c.outs[0].f()[0] = f->f;
  } else if (const Attribute* i = n.attr("value_int")) {
    c.alloc(0, DType::I64, Shape{}); c.outs[0].i()[0] = i->i;
  } else if (const Attribute* is = n.attr("value_ints")) {
    c.alloc(0, DType::I64, Shape{std::int64_t(is->ints.size())});
    for (std::size_t k = 0; k < is->ints.size(); ++k) c.outs[0].i()[k] = is->ints[k];
  } else throw GraphError("Constant: unsupported form");
}

void op_shape(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Shape");
  int r = a.shape.rank;
  std::int64_t s = attr_int(n, "start", 0), e = attr_int(n, "end", r);
  if (s < 0) s += r;
  if (e < 0) e += r;
  s = std::clamp<std::int64_t>(s, 0, r);
  e = std::clamp<std::int64_t>(e, 0, r);
  if (e < s) e = s;
  c.alloc(0, DType::I64, Shape{e - s});
  for (std::int64_t k = s; k < e; ++k) c.outs[0].i()[k - s] = a.shape[int(k)];
}

void op_identity(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Identity");
  c.alias(0, a, a.shape);
}

void op_reshape(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Reshape");
  auto dims = tensor_i64(need(in, nin, 1, "Reshape"));
  bool allowzero = attr_int(n, "allowzero", 0) != 0;
  if (dims.size() > std::size_t(kMaxRank)) throw GraphError("Reshape: rank above " + std::to_string(kMaxRank));
  Shape s;
  std::int64_t known = 1;
  int neg = -1;
  for (std::size_t k = 0; k < dims.size(); ++k) {
    std::int64_t d = dims[k];
    if (d == 0 && !allowzero) {
      if (int(k) >= a.shape.rank) throw GraphError("Reshape: zero copies a dimension that does not exist");
      d = a.shape[int(k)];
    }
    if (d == -1) { if (neg >= 0) throw GraphError("Reshape: more than one -1"); neg = int(k); d = 1; }
    else if (d < 0) throw GraphError("Reshape: negative dimension");
    s.push(d);
    known *= d;
  }
  std::int64_t total = a.numel();
  if (neg >= 0) {
    if (known == 0 || total % known) throw GraphError("Reshape: cannot infer -1 for " + a.shape.str());
    s[neg] = total / known;
  } else if (known != total) throw GraphError("Reshape: " + a.shape.str() + " into a different element count");
  c.alias(0, a, s);
}

void op_flatten(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Flatten");
  std::int64_t ax = attr_int(n, "axis", 1);
  if (ax < 0) ax += a.shape.rank;
  if (ax < 0 || ax > a.shape.rank) throw GraphError("Flatten: bad axis");
  std::int64_t d0 = 1, d1 = 1;
  for (int i = 0; i < a.shape.rank; ++i) (i < ax ? d0 : d1) *= a.shape[i];
  c.alias(0, a, Shape{d0, d1});
}

std::vector<std::int64_t> axes_arg(const NodeProto& n, const Tensor* const* in, int nin, int idx) {
  if (nin > idx && in[idx]) return tensor_i64(*in[idx]);
  return attr_ints(n, "axes");
}

void op_squeeze(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Squeeze");
  auto ax = axes_arg(n, in, nin, 1);
  std::vector<bool> drop(std::size_t(a.shape.rank), false);
  if (ax.empty()) { for (int i = 0; i < a.shape.rank; ++i) drop[std::size_t(i)] = a.shape[i] == 1; }
  else for (auto v : ax) {
    int k = norm_axis(v, a.shape.rank, "Squeeze");
    if (a.shape[k] != 1) throw GraphError("Squeeze: dimension is not 1");
    drop[std::size_t(k)] = true;
  }
  Shape s;
  for (int i = 0; i < a.shape.rank; ++i) if (!drop[std::size_t(i)]) s.push(a.shape[i]);
  c.alias(0, a, s);
}

void op_unsqueeze(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Unsqueeze");
  auto ax = axes_arg(n, in, nin, 1);
  int r = a.shape.rank + int(ax.size());
  if (r > kMaxRank) throw GraphError("Unsqueeze: rank above " + std::to_string(kMaxRank));
  std::vector<bool> ins(std::size_t(r), false);
  for (auto v : ax) {
    int k = norm_axis(v, r, "Unsqueeze");
    if (ins[std::size_t(k)]) throw GraphError("Unsqueeze: repeated axis");
    ins[std::size_t(k)] = true;
  }
  Shape s;
  int src = 0;
  for (int i = 0; i < r; ++i) s.push(ins[std::size_t(i)] ? 1 : a.shape[src++]);
  c.alias(0, a, s);
}

void op_transpose(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Transpose");
  int r = a.shape.rank;
  bool has;
  auto perm = attr_ints(n, "perm", &has);
  if (!has) { perm.resize(std::size_t(r)); for (int i = 0; i < r; ++i) perm[std::size_t(i)] = r - 1 - i; }
  if (int(perm.size()) != r) throw GraphError("Transpose: perm length");
  std::int64_t ist[kMaxRank];
  strides_of(a.shape, ist);
  Shape os;
  std::int64_t st[kMaxRank];
  std::vector<bool> seen(std::size_t(r), false);
  for (int i = 0; i < r; ++i) {
    int p = norm_axis(perm[std::size_t(i)], r, "Transpose");
    if (seen[std::size_t(p)]) throw GraphError("Transpose: perm repeats an axis");
    seen[std::size_t(p)] = true;
    os.push(a.shape[p]);
    st[i] = ist[p];
  }
  c.alloc(0, a.dt, os);
  DH_BY_SIZE(a.dt, T, strided_copy<T>(c, static_cast<T*>(c.outs[0].data), os, static_cast<const T*>(a.data), st));
}

void op_expand(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Expand");
  auto dims = tensor_i64(need(in, nin, 1, "Expand"));
  Shape ts;
  for (auto d : dims) { if (d < 0) throw GraphError("Expand: negative dimension"); ts.push(d); }
  Shape os = bcast_shape(a.shape, ts);
  c.alloc(0, a.dt, os);
  std::int64_t st[kMaxRank];
  bstrides(a.shape, os, st);
  DH_BY_SIZE(a.dt, T, strided_copy<T>(c, static_cast<T*>(c.outs[0].data), os, static_cast<const T*>(a.data), st));
}

void op_concat(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a0 = need(in, nin, 0, "Concat");
  int ax = norm_axis(attr_int(n, "axis", 0), a0.shape.rank, "Concat");
  Shape os = a0.shape;
  os[ax] = 0;
  for (int i = 0; i < nin; ++i) {
    const Tensor& t = need(in, nin, i, "Concat");
    if (t.dt != a0.dt || t.shape.rank != a0.shape.rank) throw GraphError("Concat: inputs differ in type or rank");
    for (int d = 0; d < t.shape.rank; ++d)
      if (d != ax && t.shape[d] != a0.shape[d]) throw GraphError("Concat: shapes " + a0.shape.str() + " and " + t.shape.str() + " do not match");
    os[ax] += t.shape[ax];
  }
  c.alloc(0, a0.dt, os);
  std::int64_t outer = 1, inner = 1;
  for (int d = 0; d < ax; ++d) outer *= os[d];
  for (int d = ax + 1; d < os.rank; ++d) inner *= os[d];
  const std::size_t es = dtype_size(a0.dt);
  std::uint8_t* O = static_cast<std::uint8_t*>(c.outs[0].data);
  std::int64_t pos = 0;
  for (int i = 0; i < nin; ++i) {
    const Tensor& t = *in[i];
    std::int64_t blk = t.shape[ax] * inner;
    const std::uint8_t* src = static_cast<const std::uint8_t*>(t.data);
    std::int64_t dstp = pos * inner;
    c.par(outer, std::max<std::int64_t>(1, kGrain / std::max<std::int64_t>(1, blk)), [&](std::int64_t b, std::int64_t e) {
      for (std::int64_t o = b; o < e; ++o)
        std::memcpy(O + (o * os[ax] * inner + dstp) * std::int64_t(es), src + o * blk * std::int64_t(es), std::size_t(blk) * es);
    });
    pos += t.shape[ax];
  }
}

void op_gather(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Gather");
  const Tensor& idx = need(in, nin, 1, "Gather");
  if (idx.dt != DType::I64) throw GraphError("Gather: indices must be int64");
  int ax = norm_axis(attr_int(n, "axis", 0), a.shape.rank, "Gather");
  Shape os;
  for (int d = 0; d < ax; ++d) os.push(a.shape[d]);
  for (int d = 0; d < idx.shape.rank; ++d) os.push(idx.shape[d]);
  for (int d = ax + 1; d < a.shape.rank; ++d) os.push(a.shape[d]);
  std::int64_t outer = 1, inner = 1, dim = a.shape[ax], ni = idx.numel();
  for (int d = 0; d < ax; ++d) outer *= a.shape[d];
  for (int d = ax + 1; d < a.shape.rank; ++d) inner *= a.shape[d];
  for (std::int64_t k = 0; k < ni; ++k) {
    std::int64_t v = idx.i()[k];
    if (v < -dim || v >= dim) throw GraphError("Gather: index " + std::to_string(v) + " out of range for dimension " + std::to_string(dim));
  }
  c.alloc(0, a.dt, os);
  const std::size_t es = dtype_size(a.dt) * std::size_t(inner);
  const std::uint8_t* src = static_cast<const std::uint8_t*>(a.data);
  std::uint8_t* dst = static_cast<std::uint8_t*>(c.outs[0].data);
  c.par(outer * ni, std::max<std::int64_t>(1, kGrain / std::max<std::int64_t>(1, inner)), [&](std::int64_t b, std::int64_t e) {
    for (std::int64_t k = b; k < e; ++k) {
      std::int64_t o = k / ni, q = k % ni, v = idx.i()[q];
      if (v < 0) v += dim;
      std::memcpy(dst + std::size_t(k) * es, src + std::size_t(o * dim + v) * es, es);
    }
  });
}

// Slice with already-normalised per-axis start/step/length.
void slice_into(OpCtx& c, const Tensor& a, int out_idx, const std::int64_t* start, const std::int64_t* step, const Shape& os) {
  c.alloc(out_idx, a.dt, os);
  std::int64_t ist[kMaxRank], st[kMaxRank], base = 0;
  strides_of(a.shape, ist);
  for (int d = 0; d < a.shape.rank; ++d) { st[d] = ist[d] * step[d]; base += ist[d] * start[d]; }
  DH_BY_SIZE(a.dt, T, strided_copy<T>(c, static_cast<T*>(c.outs[out_idx].data), os, static_cast<const T*>(a.data) + base, st));
}

void op_slice(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Slice");
  int r = a.shape.rank;
  std::vector<std::int64_t> starts, ends, axes, steps;
  if (nin >= 3) {
    starts = tensor_i64(need(in, nin, 1, "Slice"));
    ends = tensor_i64(need(in, nin, 2, "Slice"));
    if (nin > 3 && in[3]) axes = tensor_i64(*in[3]);
    if (nin > 4 && in[4]) steps = tensor_i64(*in[4]);
  } else {
    starts = attr_ints(n, "starts"); ends = attr_ints(n, "ends"); axes = attr_ints(n, "axes");
  }
  if (starts.size() != ends.size()) throw GraphError("Slice: starts/ends length differ");
  if (axes.empty()) { for (std::size_t i = 0; i < starts.size(); ++i) axes.push_back(std::int64_t(i)); }
  if (steps.empty()) steps.assign(starts.size(), 1);
  if (axes.size() != starts.size() || steps.size() != starts.size()) throw GraphError("Slice: argument lengths differ");
  std::int64_t st[kMaxRank], sp[kMaxRank];
  Shape os = a.shape;
  for (int d = 0; d < r; ++d) { st[d] = 0; sp[d] = 1; }
  for (std::size_t k = 0; k < axes.size(); ++k) {
    int d = norm_axis(axes[k], r, "Slice");
    std::int64_t dim = a.shape[d], step = steps[k], s = starts[k], e = ends[k];
    if (step == 0) throw GraphError("Slice: zero step");
    if (s < 0) s += dim;
    if (e < 0) e += dim;
    if (step > 0) { s = std::clamp<std::int64_t>(s, 0, dim); e = std::clamp<std::int64_t>(e, 0, dim); }
    else { s = std::clamp<std::int64_t>(s, 0, dim - 1); e = std::clamp<std::int64_t>(e, -1, dim - 1); }
    std::int64_t len = step > 0 ? (e > s ? (e - s + step - 1) / step : 0) : (s > e ? (s - e + (-step) - 1) / (-step) : 0);
    st[d] = s; sp[d] = step; os[d] = len;
  }
  slice_into(c, a, 0, st, sp, os);
}

void op_split(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Split");
  int ax = norm_axis(attr_int(n, "axis", 0), a.shape.rank, "Split");
  std::vector<std::int64_t> sizes;
  if (nin > 1 && in[1]) sizes = tensor_i64(*in[1]);
  else sizes = attr_ints(n, "split");
  int no = c.nouts;
  if (sizes.empty()) {
    std::int64_t dim = a.shape[ax], q = (dim + no - 1) / no;
    for (int i = 0; i < no; ++i) sizes.push_back(std::min<std::int64_t>(q, dim - std::int64_t(i) * q));
  }
  if (int(sizes.size()) != no) throw GraphError("Split: output count does not match split sizes");
  std::int64_t sum = 0;
  for (auto s : sizes) { if (s < 0) throw GraphError("Split: negative size"); sum += s; }
  if (sum != a.shape[ax]) throw GraphError("Split: sizes do not add up to the dimension");
  std::int64_t pos = 0;
  for (int i = 0; i < no; ++i) {
    std::int64_t st[kMaxRank] = {0}, sp[kMaxRank];
    for (int d = 0; d < a.shape.rank; ++d) sp[d] = 1;
    st[ax] = pos;
    Shape os = a.shape;
    os[ax] = sizes[std::size_t(i)];
    if (c.want(i)) slice_into(c, a, i, st, sp, os);
    pos += sizes[std::size_t(i)];
  }
}

void op_constantofshape(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  auto dims = tensor_i64(need(in, nin, 0, "ConstantOfShape"));
  Shape s;
  for (auto d : dims) { if (d < 0) throw GraphError("ConstantOfShape: negative dimension"); s.push(d); }
  Tensor val;
  const Attribute* a = n.attr("value");
  if (a) val = decode_tensor(a->t, nullptr, 0);
  else { val = make_owned(DType::F32, Shape{1}); }
  if (val.numel() != 1) throw GraphError("ConstantOfShape: value must have one element");
  c.alloc(0, val.dt, s);
  const std::int64_t N = s.numel();
  DH_BY_SIZE(val.dt, T, { T v = *static_cast<const T*>(val.data); std::fill_n(static_cast<T*>(c.outs[0].data), N, v); });
}

void op_range(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {
  const Tensor& s = need(in, nin, 0, "Range");
  const Tensor& l = need(in, nin, 1, "Range");
  const Tensor& d = need(in, nin, 2, "Range");
  if (s.dt == DType::I64) {
    std::int64_t a = s.i()[0], b = l.i()[0], st = d.i()[0];
    if (st == 0) throw GraphError("Range: zero delta");
    std::int64_t cnt = std::max<std::int64_t>(0, (b - a + st + (st > 0 ? -1 : 1)) / st);
    c.alloc(0, DType::I64, Shape{cnt});
    for (std::int64_t i = 0; i < cnt; ++i) c.outs[0].i()[i] = a + i * st;
  } else if (s.dt == DType::F32) {
    float a = s.f()[0], b = l.f()[0], st = d.f()[0];
    if (st == 0.f) throw GraphError("Range: zero delta");
    std::int64_t cnt = std::max<std::int64_t>(0, std::int64_t(std::ceil((b - a) / st)));
    c.alloc(0, DType::F32, Shape{cnt});
    for (std::int64_t i = 0; i < cnt; ++i) c.outs[0].f()[i] = a + float(i) * st;
  } else throw GraphError("Range: unsupported type");
}

void op_tile(OpCtx& c, const NodeProto&, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Tile");
  auto rep = tensor_i64(need(in, nin, 1, "Tile"));
  if (int(rep.size()) != a.shape.rank) throw GraphError("Tile: repeats length");
  Shape os;
  for (int d = 0; d < a.shape.rank; ++d) { if (rep[std::size_t(d)] < 0) throw GraphError("Tile: negative repeat"); os.push(a.shape[d] * rep[std::size_t(d)]); }
  c.alloc(0, a.dt, os);
  const int r = a.shape.rank;
  if (os.numel() == 0) return;
  if (r == 0) { std::memcpy(c.outs[0].data, a.data, a.bytes()); return; }
  std::int64_t ist[kMaxRank];
  strides_of(a.shape, ist);
  const std::int64_t inner_in = a.shape[r - 1], inner_out = os[r - 1];
  std::int64_t rows = os.numel() / inner_out;
  const std::size_t es = dtype_size(a.dt);
  const std::uint8_t* src = static_cast<const std::uint8_t*>(a.data);
  std::uint8_t* dst = static_cast<std::uint8_t*>(c.outs[0].data);
  c.par(rows, std::max<std::int64_t>(1, kGrain / inner_out), [&](std::int64_t b, std::int64_t e) {
    for (std::int64_t row = b; row < e; ++row) {
      std::int64_t rem = row, off = 0;
      for (int d = r - 2; d >= 0; --d) { off += (rem % os[d] % a.shape[d]) * ist[d]; rem /= os[d]; }
      const std::uint8_t* s = src + std::size_t(off) * es;
      std::uint8_t* o = dst + std::size_t(row * inner_out) * es;
      for (std::int64_t k = 0; k < rep[std::size_t(r - 1)]; ++k) std::memcpy(o + std::size_t(k * inner_in) * es, s, std::size_t(inner_in) * es);
    }
  });
}

void op_scatternd(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& data = need(in, nin, 0, "ScatterND");
  const Tensor& idx = need(in, nin, 1, "ScatterND");
  const Tensor& upd = need(in, nin, 2, "ScatterND");
  if (idx.dt != DType::I64 || upd.dt != data.dt) throw GraphError("ScatterND: bad types");
  if (attr_str(n, "reduction", "none") != "none") throw GraphError("ScatterND: only reduction=none is supported");
  int r = data.shape.rank, ir = idx.shape.rank;
  if (ir < 1) throw GraphError("ScatterND: indices rank");
  std::int64_t k = idx.shape[ir - 1];
  if (k < 1 || k > r) throw GraphError("ScatterND: index depth");
  std::int64_t nidx = idx.numel() / k, slice = 1;
  for (int d = int(k); d < r; ++d) slice *= data.shape[d];
  if (upd.numel() != nidx * slice) throw GraphError("ScatterND: updates shape");
  c.alloc(0, data.dt, data.shape);
  const std::size_t es = dtype_size(data.dt);
  std::memcpy(c.outs[0].data, data.data, data.bytes());
  std::int64_t ist[kMaxRank];
  strides_of(data.shape, ist);
  for (std::int64_t q = 0; q < nidx; ++q) {
    std::int64_t off = 0;
    for (std::int64_t j = 0; j < k; ++j) {
      std::int64_t v = idx.i()[q * k + j], dim = data.shape[int(j)];
      if (v < -dim || v >= dim) throw GraphError("ScatterND: index out of range");
      if (v < 0) v += dim;
      off += v * ist[j];
    }
    std::memcpy(static_cast<std::uint8_t*>(c.outs[0].data) + std::size_t(off) * es,
                static_cast<const std::uint8_t*>(upd.data) + std::size_t(q * slice) * es, std::size_t(slice) * es);
  }
}

void op_pad(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Pad");
  auto pads = tensor_i64(need(in, nin, 1, "Pad"));
  if (nin > 3 && in[3]) throw GraphError("Pad: the axes input is not supported");
  int r = a.shape.rank;
  if (int(pads.size()) != 2 * r) throw GraphError("Pad: pads length");
  std::string mode = attr_str(n, "mode", "constant");
  if (mode != "constant" && mode != "reflect" && mode != "edge") throw GraphError("Pad: unsupported mode " + mode);
  Shape os;
  for (int d = 0; d < r; ++d) {
    std::int64_t v = a.shape[d] + pads[std::size_t(d)] + pads[std::size_t(d + r)];
    if (v < 0) throw GraphError("Pad: negative output size");
    os.push(v);
  }
  c.alloc(0, a.dt, os);
  float cval = 0;
  std::int64_t ival = 0;
  if (nin > 2 && in[2]) { if (a.dt == DType::F32) cval = in[2]->f()[0]; else if (a.dt == DType::I64) ival = in[2]->i()[0]; }
  // Per-dimension source index maps (-1 = constant).
  std::vector<std::vector<std::int64_t>> map;
  map.resize(std::size_t(r));
  for (int d = 0; d < r; ++d) {
    auto& m = map[std::size_t(d)];
    m.resize(std::size_t(os[d]));
    std::int64_t dim = a.shape[d], lo = pads[std::size_t(d)];
    for (std::int64_t o = 0; o < os[d]; ++o) {
      std::int64_t s = o - lo;
      if (s < 0 || s >= dim) {
        if (mode == "constant") s = -1;
        else if (mode == "edge") s = s < 0 ? 0 : dim - 1;
        else {
          if (dim == 1) s = 0;
          else {
            std::int64_t per = 2 * (dim - 1);
            s = ((s % per) + per) % per;
            if (s >= dim) s = per - s;
          }
        }
      }
      m[std::size_t(o)] = s;
    }
  }
  std::int64_t ist[kMaxRank];
  strides_of(a.shape, ist);
  if (os.numel() == 0) return;
  const std::int64_t inner = os[r - 1];
  const std::int64_t rows = os.numel() / inner;
  const std::size_t es = dtype_size(a.dt);
  const std::uint8_t* src = static_cast<const std::uint8_t*>(a.data);
  std::uint8_t* dst = static_cast<std::uint8_t*>(c.outs[0].data);
  c.par(rows, std::max<std::int64_t>(1, kGrain / inner), [&](std::int64_t b, std::int64_t e) {
    for (std::int64_t row = b; row < e; ++row) {
      std::int64_t rem = row, off = 0;
      bool outside = false;
      for (int d = r - 2; d >= 0; --d) {
        std::int64_t s = map[std::size_t(d)][std::size_t(rem % os[d])];
        rem /= os[d];
        if (s < 0) outside = true; else off += s * ist[d];
      }
      std::uint8_t* o = dst + std::size_t(row * inner) * es;
      const auto& ml = map[std::size_t(r - 1)];
      for (std::int64_t i = 0; i < inner; ++i) {
        std::int64_t s = ml[std::size_t(i)];
        if (outside || s < 0) {
          if (a.dt == DType::F32) std::memcpy(o + std::size_t(i) * es, &cval, 4);
          else if (a.dt == DType::I64) std::memcpy(o + std::size_t(i) * es, &ival, 8);
          else o[i] = 0;
        } else std::memcpy(o + std::size_t(i) * es, src + std::size_t(off + s) * es, es);
      }
    }
  });
}

// ---- reductions ----------------------------------------------------------------------------------------------------------
enum class Red { Mean, Sum, Prod, Max, Min, L2 };
template <Red K>
void op_reduce(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  const Tensor& a = need(in, nin, 0, "Reduce");
  std::vector<std::int64_t> axes;
  if (nin > 1 && in[1]) axes = tensor_i64(*in[1]); else axes = attr_ints(n, "axes");
  bool keep = attr_int(n, "keepdims", 1) != 0;
  int r = a.shape.rank;
  std::vector<bool> red(std::size_t(r), axes.empty());
  for (auto v : axes) red[std::size_t(norm_axis(v, r, "Reduce"))] = true;
  Shape os;
  std::int64_t ist[kMaxRank];
  strides_of(a.shape, ist);
  std::vector<int> kept, redd;
  for (int d = 0; d < r; ++d) {
    if (red[std::size_t(d)]) { redd.push_back(d); if (keep) os.push(1); }
    else { kept.push_back(d); os.push(a.shape[d]); }
  }
  std::int64_t R = 1, Nk = 1;
  for (int d : redd) R *= a.shape[d];
  for (int d : kept) Nk *= a.shape[d];
  if (a.dt != DType::F32 && a.dt != DType::I64) throw GraphError("Reduce: unsupported type");
  if (R == 0 && (K == Red::Max || K == Red::Min)) throw GraphError("Reduce: empty reduction");
  c.alloc(0, a.dt, os);
  std::vector<std::int64_t> roff;
  roff.resize(std::size_t(R));
  for (std::int64_t q = 0; q < R; ++q) {
    std::int64_t rem = q, off = 0;
    for (int j = int(redd.size()) - 1; j >= 0; --j) { off += (rem % a.shape[redd[std::size_t(j)]]) * ist[redd[std::size_t(j)]]; rem /= a.shape[redd[std::size_t(j)]]; }
    roff[std::size_t(q)] = off;
  }
  auto body = [&](auto* in_p, auto* out_p) {
    using T = std::remove_pointer_t<decltype(in_p)>;
    c.par(Nk, std::max<std::int64_t>(1, kGrain / std::max<std::int64_t>(1, R)), [&](std::int64_t b, std::int64_t e) {
      for (std::int64_t o = b; o < e; ++o) {
        std::int64_t rem = o, base = 0;
        for (int j = int(kept.size()) - 1; j >= 0; --j) { base += (rem % a.shape[kept[std::size_t(j)]]) * ist[kept[std::size_t(j)]]; rem /= a.shape[kept[std::size_t(j)]]; }
        const T* p = in_p + base;
        if constexpr (K == Red::Max || K == Red::Min) {
          T v = p[roff[0]];
          for (std::int64_t q = 1; q < R; ++q) v = K == Red::Max ? std::max(v, p[roff[std::size_t(q)]]) : std::min(v, p[roff[std::size_t(q)]]);
          out_p[o] = v;
        } else if constexpr (K == Red::L2) {
          double sq = 0;
          for (std::int64_t q = 0; q < R; ++q) { double x = double(p[roff[std::size_t(q)]]); sq += x * x; }
          out_p[o] = T(std::sqrt(sq));
        } else if constexpr (K == Red::Prod) {
          T v = 1;
          for (std::int64_t q = 0; q < R; ++q) v *= p[roff[std::size_t(q)]];
          out_p[o] = v;
        } else {
          double s = 0;
          for (std::int64_t q = 0; q < R; ++q) s += double(p[roff[std::size_t(q)]]);
          if constexpr (K == Red::Mean) out_p[o] = T(s / double(R)); else out_p[o] = T(s);
        }
      }
    });
  };
  if (a.dt == DType::F32) body(a.f(), c.outs[0].f()); else body(a.i(), c.outs[0].i());
}


// Einsum: only the outer product "...,f->...f" (rotary embedding tables) is implemented; any other equation is refused.
void op_einsum(OpCtx& c, const NodeProto& n, const Tensor* const* in, int nin) {
  std::string eq;
  for (char ch : attr_str(n, "equation", "")) if (ch != ' ') eq += ch;
  if (eq != "...,f->...f" || nin != 2) throw GraphError("Einsum: unsupported equation '" + eq + "' (only ...,f->...f)");
  const Tensor& a = need(in, nin, 0, "Einsum");
  const Tensor& f = need(in, nin, 1, "Einsum");
  if (a.dt != DType::F32 || f.dt != DType::F32 || f.shape.rank != 1) throw GraphError("Einsum: needs a float tensor and a float vector");
  Shape os = a.shape;
  os.push(f.shape[0]);
  c.alloc(0, DType::F32, os);
  const std::int64_t na = a.numel(), nf = f.shape[0];
  for (std::int64_t i = 0; i < na; ++i) for (std::int64_t j = 0; j < nf; ++j) c.outs[0].f()[i * nf + j] = a.f()[i] * f.f()[j];
}

}  // namespace

void register_basic_ops(std::vector<std::pair<std::string, OpFn>>& v) {
  v.push_back({"Add", &op_arith<AddF>}); v.push_back({"Sub", &op_arith<SubF>}); v.push_back({"Mul", &op_arith<MulF>});
  v.push_back({"Div", &op_arith<DivF>}); v.push_back({"Max", &op_arith<MaxF>}); v.push_back({"Min", &op_arith<MinF>});
  v.push_back({"Pow", &op_pow}); v.push_back({"Mod", &op_mod});
  v.push_back({"Equal", &op_compare<EqF>}); v.push_back({"Less", &op_compare<LtF>}); v.push_back({"Greater", &op_compare<GtF>});
  v.push_back({"LessOrEqual", &op_compare<LeF>}); v.push_back({"GreaterOrEqual", &op_compare<GeF>});
  v.push_back({"And", &op_logic<AndF>}); v.push_back({"Or", &op_logic<OrF>}); v.push_back({"Not", &op_not});
  v.push_back({"Where", &op_where});
  v.push_back({"Erf", &op_Erf}); v.push_back({"Sigmoid", &op_Sigmoid}); v.push_back({"Sqrt", &op_Sqrt});
  v.push_back({"Sin", &op_Sin}); v.push_back({"Cos", &op_Cos}); v.push_back({"Tanh", &op_Tanh}); v.push_back({"Relu", &op_Relu});
  v.push_back({"Exp", &op_Exp}); v.push_back({"Log", &op_Log}); v.push_back({"Reciprocal", &op_Reciprocal});
  v.push_back({"Floor", &op_Floor}); v.push_back({"Ceil", &op_Ceil}); v.push_back({"Gelu", &op_gelu});
  v.push_back({"Neg", &op_neg}); v.push_back({"Abs", &op_abs}); v.push_back({"LeakyRelu", &op_leakyrelu}); v.push_back({"Clip", &op_clip});
  v.push_back({"Cast", &op_cast}); v.push_back({"Constant", &op_constant}); v.push_back({"Shape", &op_shape});
  v.push_back({"Identity", &op_identity}); v.push_back({"Reshape", &op_reshape}); v.push_back({"Flatten", &op_flatten});
  v.push_back({"Squeeze", &op_squeeze}); v.push_back({"Unsqueeze", &op_unsqueeze}); v.push_back({"Transpose", &op_transpose});
  v.push_back({"Expand", &op_expand}); v.push_back({"Concat", &op_concat}); v.push_back({"Gather", &op_gather});
  v.push_back({"Slice", &op_slice}); v.push_back({"Split", &op_split}); v.push_back({"ConstantOfShape", &op_constantofshape});
  v.push_back({"Range", &op_range}); v.push_back({"Tile", &op_tile}); v.push_back({"ScatterND", &op_scatternd}); v.push_back({"Pad", &op_pad});
  v.push_back({"ReduceMean", &op_reduce<Red::Mean>}); v.push_back({"ReduceSum", &op_reduce<Red::Sum>});
  v.push_back({"ReduceProd", &op_reduce<Red::Prod>}); v.push_back({"ReduceMax", &op_reduce<Red::Max>});
  v.push_back({"ReduceMin", &op_reduce<Red::Min>}); v.push_back({"ReduceL2", &op_reduce<Red::L2>});
  v.push_back({"Einsum", &op_einsum});
}

}  // namespace deckhand::stems::graph
