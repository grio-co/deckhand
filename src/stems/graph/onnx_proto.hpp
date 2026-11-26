// Hand-written reader for the subset of the ONNX protobuf wire format the executor needs (ModelProto -> GraphProto:
// nodes, initializers with external-data references, value infos, attributes). No generated code; every length,
// count, nesting depth and size is bounds-checked, so a hostile file produces an error, never a crash.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tensor.hpp"

namespace deckhand::stems::graph {

struct TensorProto {
  std::string name;
  std::vector<std::int64_t> dims;
  int data_type = 0;  // ONNX TensorProto.DataType (1 float, 6 int32, 7 int64, 9 bool, ...)
  // Inline data: raw little-endian bytes (points into the model buffer) or typed arrays.
  const std::uint8_t* raw = nullptr;
  std::size_t raw_len = 0;
  std::vector<float> float_data;
  std::vector<std::int64_t> int_data;  // int32 / int64 / bool / uint8 ... repeated fields
  bool external = false;
  std::string location;
  std::int64_t offset = 0, length = -1;
};

struct Attribute {
  std::string name;
  int type = 0;  // 1 float, 2 int, 3 string, 4 tensor, 6 floats, 7 ints, 8 strings, 5 graph
  float f = 0;
  std::int64_t i = 0;
  std::string s;
  TensorProto t;
  std::vector<float> floats;
  std::vector<std::int64_t> ints;
  bool has_graph = false;
};

struct NodeProto {
  std::string name, op_type, domain;
  std::vector<std::string> inputs, outputs;
  std::vector<Attribute> attrs;
  const Attribute* attr(const char* n) const {
    for (auto& a : attrs) if (a.name == n) return &a;
    return nullptr;
  }
};

struct ValueInfo {
  std::string name;
  int elem_type = 0;
  std::vector<std::int64_t> dims;  // -1 where symbolic or unknown
};

struct ModelProto {
  std::shared_ptr<std::vector<std::uint8_t>> buffer;  // keeps TensorProto::raw valid
  std::int64_t ir_version = 0, opset = 0;
  std::string producer;
  std::vector<NodeProto> nodes;
  std::vector<TensorProto> initializers;
  std::vector<ValueInfo> inputs, outputs;
};

struct ParseLimits {
  std::size_t max_nodes = 1u << 21, max_initializers = 1u << 20, max_attr_values = 1u << 22, max_string = 1u << 20;
  int max_depth = 16;
};

// Parses a complete model from memory (the buffer is moved into the result). Throws GraphError on malformed input.
ModelProto parse_model(std::shared_ptr<std::vector<std::uint8_t>> buffer, const ParseLimits& lim = {});
ModelProto load_model_file(const std::string& path, const ParseLimits& lim = {});

// Materialises an initializer / Constant tensor into owned storage (F32, I64 or Bool). `external` data is not read here.
Tensor decode_tensor(const TensorProto& t, const std::uint8_t* external_bytes /*null unless external*/, std::size_t external_len);

}  // namespace deckhand::stems::graph
