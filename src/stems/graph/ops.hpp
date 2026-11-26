// Operator interface of the built-in executor. An operator reads its inputs (null where an optional input is absent),
// calls ctx.alloc()/ctx.alias() for each output it produces, and writes the result. Failing loudly beats guessing:
// anything unsupported throws GraphError.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "kernels.hpp"
#include "onnx_proto.hpp"
#include "tensor.hpp"

namespace deckhand::stems::graph {

struct OpCtx {
  ThreadPool* pool = nullptr;
  std::vector<std::vector<float>>* scratch = nullptr;
  Tensor* outs = nullptr;
  int nouts = 0;
  const bool* wanted = nullptr;  // per output: false for omitted optional outputs
  void* user = nullptr;
  void (*alloc_fn)(OpCtx&, int idx, DType, const Shape&) = nullptr;

  bool want(int i) const { return i < nouts && wanted[i]; }
  void alloc(int i, DType dt, const Shape& s) { alloc_fn(*this, i, dt, s); }
  // Output i becomes a view of `src` with another shape (same element count); no copy, no new storage.
  void alias(int i, const Tensor& src, const Shape& s) {
    if (s.numel() != src.shape.numel()) throw GraphError("internal: alias with a different element count");
    outs[i].dt = src.dt; outs[i].shape = s; outs[i].data = src.data; outs[i].owner = src.owner;
  }
  float* scratch_f(int slot, std::size_t n) {
    auto& v = (*scratch)[std::size_t(slot)];
    if (v.size() < n) v.resize(n);
    return v.data();
  }
  void par(std::int64_t n, std::int64_t grain, const std::function<void(std::int64_t, std::int64_t)>& fn) {
    pool->run(n, grain, fn);
  }
};

using OpFn = void (*)(OpCtx&, const NodeProto&, const Tensor* const* in, int nin);
const OpFn* find_op(const std::string& op_type);       // null when unsupported
std::vector<std::string> supported_ops();
bool is_shape_arith_view(const std::string& op);

constexpr int kScratchSlots = 4;
// Tests shrink the convolution column tiles so tile borders are exercised by tiny tensors.
void set_conv_tiling_for_test(std::int64_t floats, std::int64_t min_cols);

// ---- helpers shared by the operator files ----
std::int64_t attr_int(const NodeProto& n, const char* name, std::int64_t def);
float attr_float(const NodeProto& n, const char* name, float def);
std::string attr_str(const NodeProto& n, const char* name, const std::string& def);
std::vector<std::int64_t> attr_ints(const NodeProto& n, const char* name, bool* present = nullptr);
inline int norm_axis(std::int64_t a, int rank, const char* what) {
  if (a < -rank || a >= rank) throw GraphError(std::string(what) + ": axis out of range");
  return int(a < 0 ? a + rank : a);
}
std::vector<std::int64_t> tensor_i64(const Tensor& t);  // reads an I64 tensor into a vector
const Tensor& need(const Tensor* const* in, int nin, int i, const char* what);

void register_basic_ops(std::vector<std::pair<std::string, OpFn>>& out);
void register_nn_ops(std::vector<std::pair<std::string, OpFn>>& out);

}  // namespace deckhand::stems::graph
