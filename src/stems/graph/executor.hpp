// dhgraph executor: loads an ONNX graph (optionally with external-data weights), folds the shape arithmetic on the
// first run, plans a liveness-based buffer arena, and then runs the remaining nodes with no per-node allocation.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "onnx_proto.hpp"
#include "ops.hpp"

namespace deckhand::stems::graph {

struct InputBinding {
  std::string name;
  const void* data = nullptr;
  Shape shape;
  DType dt = DType::F32;
};

// Activation memory that several executors of the same architecture can share (they run one after another): the arena
// only grows, and every executor re-reads its base address at the start of each run.
class SharedArena {
 public:
  std::uint8_t* ensure(std::size_t bytes);
  std::uint8_t* data() const { return base_; }
  std::size_t size() const { return size_; }

 private:
  std::shared_ptr<void> owner_;
  std::uint8_t* base_ = nullptr;
  std::size_t size_ = 0;
};

class Executor {
 public:
  // `weights_override` (when non-empty) replaces the location of every external-data tensor with this file; otherwise
  // locations are resolved relative to the model's folder. Returns null and fills `err` on failure.
  static std::unique_ptr<Executor> open(const std::string& model_path, const std::string& weights_override, int threads,
                                        std::string* err);
  static std::unique_ptr<Executor> from_proto(ModelProto&& m, const std::string& weights_dir,
                                              const std::string& weights_override, int threads, std::string* err);
  ~Executor();

  // Must be called before the first run().
  void set_shared_arena(std::shared_ptr<SharedArena> arena);
  void set_threads(int threads);
  int threads() const;
  // Debug aid: never reuse buffers, so every intermediate value stays readable through value() after run().
  void set_keep_all(bool keep);

  bool run(const std::vector<InputBinding>& inputs, std::string* err);
  // Valid until the next run().
  const Tensor* output(const std::string& name) const;
  const Tensor* value(const std::string& name) const;

  const std::vector<std::string>& input_names() const;
  const std::vector<std::string>& output_names() const;
  std::map<std::string, int> op_histogram() const;
  std::size_t arena_bytes() const;    // activation memory planned for dynamic values
  std::size_t scratch_bytes() const;  // convolution work buffers
  std::size_t weights_bytes() const;  // initializer storage
  std::size_t node_count() const;
  std::size_t scheduled_nodes() const;  // nodes left after shape folding (valid after the first run)

 private:
  struct Impl;
  explicit Executor(std::unique_ptr<Impl> i);
  std::unique_ptr<Impl> p_;
};

}  // namespace deckhand::stems::graph
