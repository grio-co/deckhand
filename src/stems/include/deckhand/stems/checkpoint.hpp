// Reader for PyTorch checkpoints (.th / .ckpt / .pt: a zip holding a pickle plus raw tensor storages), written to
// treat the file as hostile input: nothing is ever executed or imported. The pickle is run by a tiny interpreter that
// accepts a fixed set of opcodes and a fixed allow-list of globals, with hard size limits; anything else is rejected.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace deckhand::stems {

struct CkptTensor {
  std::string name;                // dotted state-dict key, e.g. "encoder.0.conv.weight"
  std::string dtype;               // "float32", "float16", "bfloat16", "float64", "int64", "int32", ...
  std::vector<std::int64_t> shape, stride;  // strides in elements
  std::string member;              // zip member holding the storage
  std::int64_t offset = 0;         // storage offset in elements
  std::uint64_t numel = 0;
};

struct Checkpoint {
  std::vector<CkptTensor> tensors;   // state dict entries in file order (the "state" dict when the file wraps one)
  std::string wrapper_class;         // e.g. "demucs.htdemucs.HTDemucs" for Demucs packages; empty for a bare state dict
};

struct CkptLimits {
  std::uint64_t max_pickle_bytes = 64u << 20;
  std::uint64_t max_tensors = 200000;
  std::uint64_t max_tensor_elements = 1ull << 31;
  std::uint64_t max_file_bytes = 4ull << 30;
};

bool read_checkpoint(const std::filesystem::path& file, Checkpoint& out, std::string* error, const CkptLimits& lim = {});
// Reads one tensor as contiguous row-major float32 (half / bfloat16 / double / integer storages are converted).
bool read_tensor_f32(const std::filesystem::path& file, const CkptTensor& t, std::vector<float>& out, std::string* error);

}  // namespace deckhand::stems
