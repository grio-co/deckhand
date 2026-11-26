// dhgraph: a small float32/int64 tensor type used by the built-in ONNX graph executor. Tensors are plain views
// (pointer + shape); storage belongs to the executor (arena), to constants, or to the caller.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace deckhand::stems::graph {

struct GraphError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

enum class DType : std::uint8_t { F32 = 0, I64 = 1, Bool = 2 };
inline std::size_t dtype_size(DType t) { return t == DType::F32 ? 4 : t == DType::I64 ? 8 : 1; }

constexpr int kMaxRank = 8;

struct Shape {
  std::array<std::int64_t, kMaxRank> d{};
  int rank = 0;
  Shape() = default;
  Shape(std::initializer_list<std::int64_t> l) {
    for (auto v : l) push(v);
  }
  void push(std::int64_t v) {
    if (rank >= kMaxRank) throw GraphError("tensor rank above " + std::to_string(kMaxRank));
    d[std::size_t(rank++)] = v;
  }
  std::int64_t& operator[](int i) { return d[std::size_t(i)]; }
  std::int64_t operator[](int i) const { return d[std::size_t(i)]; }
  std::int64_t numel() const {
    std::int64_t n = 1;
    for (int i = 0; i < rank; ++i) n *= d[std::size_t(i)];
    return n;
  }
  bool operator==(const Shape& o) const {
    if (rank != o.rank) return false;
    for (int i = 0; i < rank; ++i)
      if (d[std::size_t(i)] != o.d[std::size_t(i)]) return false;
    return true;
  }
  bool operator!=(const Shape& o) const { return !(*this == o); }
  std::string str() const {
    std::string s = "[";
    for (int i = 0; i < rank; ++i) s += (i ? "," : "") + std::to_string(d[std::size_t(i)]);
    return s + "]";
  }
};

struct Tensor {
  DType dt = DType::F32;
  Shape shape;
  void* data = nullptr;
  std::shared_ptr<void> owner;  // set for tensors that own their storage (constants, folded values)

  std::int64_t numel() const { return shape.numel(); }
  std::size_t bytes() const { return std::size_t(numel()) * dtype_size(dt); }
  float* f() const { return static_cast<float*>(data); }
  std::int64_t* i() const { return static_cast<std::int64_t*>(data); }
  std::uint8_t* b() const { return static_cast<std::uint8_t*>(data); }
};

// Owning tensor with zeroed storage of the given type and shape.
Tensor make_owned(DType dt, const Shape& s);

}  // namespace deckhand::stems::graph
