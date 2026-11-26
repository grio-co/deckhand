// Private helpers of the stems module (not installed): SHA-256, CRC-32, a small JSON reader, layout copies.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "deckhand/stems/stems.hpp"

namespace deckhand::stems::detail {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, std::size_t n);
  std::string finish_hex();
 private:
  void block(const std::uint8_t* p);
  std::uint32_t h_[8];
  std::uint8_t buf_[64];
  std::size_t fill_ = 0;
  std::uint64_t total_ = 0;
};

std::uint32_t crc32_update(std::uint32_t crc, const void* data, std::size_t n);  // start with 0

// Minimal JSON value (objects, arrays, strings, numbers, bool, null) for manifests and catalogues.
struct Json {
  enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;
  const Json* get(const std::string& k) const {
    for (auto& kv : o) if (kv.first == k) return &kv.second;
    return nullptr;
  }
  bool is_str() const { return t == T::Str; }
  bool is_num() const { return t == T::Num; }
};
bool parse_json(const std::string& text, Json& out, std::string* error);  // depth- and size-bounded

// Strided copies between a planar [ch][frames] buffer and a tensor described by layout letters.
// `dims` gives the extent of each letter in layout order; letters: B batch, S stem, C channel, T time.
struct LayoutIndex {
  bool ok = false;
  std::string error;
  std::size_t stride[256] = {0};  // by letter
  std::size_t total = 0;
};
LayoutIndex make_layout(const std::string& layout, const std::vector<std::int64_t>& dims, const std::string& allowed);

#if defined(DECKHAND_WITH_ONNX) && DECKHAND_WITH_ONNX
constexpr bool kHaveOnnx = true;
#else
constexpr bool kHaveOnnx = false;
#endif
// Built-in graph executor backend (src/stems/graph); the default for every installed model.
std::unique_ptr<StemModel> open_graph(const struct ModelManifest& m, const std::string& model_path, int threads, std::string* error);
std::unique_ptr<StemModel> open_recipe(const struct ModelManifest& m, int threads, std::string* error);
std::unique_ptr<StemModel> open_onnx(const struct ModelManifest& m, const std::string& model_path, int threads, std::string* error);

}  // namespace deckhand::stems::detail
