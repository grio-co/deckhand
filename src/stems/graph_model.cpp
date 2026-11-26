// StemModel backed by the built-in graph executor (src/stems/graph). Replaces the ONNX Runtime classes for every model
// the app installs: plain tensor-in/tensor-out models (layouts from the manifest) and the Demucs core, around which the
// STFT / mask / inverse STFT run in C++ (demucs_dsp.cpp).
#include <algorithm>
#include <filesystem>
#include <thread>

#include "deckhand/stems/demucs_dsp.hpp"
#include "deckhand/stems/manifest.hpp"
#include "graph/executor.hpp"
#include "internal.hpp"

namespace deckhand::stems::detail {
namespace {

using graph::DType;
using graph::Executor;
using graph::InputBinding;
using graph::Shape;

int pick_threads(int t) {
  if (t > 0) return t;
  unsigned hw = std::thread::hardware_concurrency();
  return int(std::clamp<unsigned>(hw ? hw : 2, 1, 4));
}

class GraphPlainModel final : public StemModel {
 public:
  GraphPlainModel(const ModelManifest& m, const std::string& path, int threads) : m_(m) {
    info_.id = m.id;
    info_.sample_rate = m.sample_rate;
    info_.segment_frames = m.segment_frames;
    info_.channels = m.channels;
    std::string err;
    ex_ = Executor::open(path, "", pick_threads(threads), &err);
    if (!ex_) throw std::runtime_error(err);
    const auto& ins = ex_->input_names();
    if (std::find(ins.begin(), ins.end(), m.input_name) == ins.end()) throw std::runtime_error("model has no input tensor named '" + m.input_name + "'");
    out_names_ = m.output_names.empty() ? std::vector<std::string>{m.output_name} : m.output_names;
    for (auto& n : out_names_) {
      const auto& o = ex_->output_names();
      if (std::find(o.begin(), o.end(), n) == o.end()) throw std::runtime_error("model has no output tensor named '" + n + "'");
    }
    for (int k = 0; k < kStemCount; ++k) canon_[std::size_t(k)] = stem_from_name(m.stems[std::size_t(k)]);
    for (char c : m.input_layout) in_shape_.push(c == 'B' ? 1 : c == 'C' ? m.channels : std::int64_t(m.segment_frames));
    in_idx_ = make_layout(m.input_layout, std::vector<std::int64_t>(in_shape_.d.begin(), in_shape_.d.begin() + in_shape_.rank), "BCT");
    if (!in_idx_.ok) throw std::runtime_error(in_idx_.error);
  }
  const ModelInfo& info() const override { return info_; }
  bool run(const float* in, float* out, std::string* error) override {
    const std::size_t S = info_.segment_frames, C = std::size_t(info_.channels);
    in_buf_.assign(in_idx_.total, 0.f);
    for (std::size_t c = 0; c < C; ++c)
      for (std::size_t t = 0; t < S; ++t) in_buf_[c * in_idx_.stride['C'] + t * in_idx_.stride['T']] = in[c * S + t];
    InputBinding b;
    b.name = m_.input_name; b.data = in_buf_.data(); b.shape = in_shape_;
    if (!ex_->run({b}, error)) return false;
    for (std::size_t r = 0; r < out_names_.size(); ++r) {
      const graph::Tensor* t = ex_->output(out_names_[r]);
      if (!t || t->dt != DType::F32) { if (error) *error = "model output is not a float tensor"; return false; }
      std::vector<std::int64_t> shape(t->shape.d.begin(), t->shape.d.begin() + t->shape.rank);
      LayoutIndex L = make_layout(m_.output_layout, shape, "BSCT");
      if (!L.ok) { if (error) *error = "model output: " + L.error; return false; }
      auto dim = [&](char ch) { std::size_t k = m_.output_layout.find(ch); return k == std::string::npos ? std::int64_t(1) : shape[k]; };
      if (dim('B') != 1 || dim('C') != std::int64_t(C) || dim('T') != std::int64_t(S) ||
          dim('S') != (m_.output_names.empty() ? std::int64_t(kStemCount) : std::int64_t(1))) {
        if (error) *error = "model output shape does not match the manifest";
        return false;
      }
      const float* p = t->f();
      for (int k = 0; k < kStemCount; ++k) {
        if (!m_.output_names.empty() && std::size_t(k) != r) continue;
        int dst = canon_[std::size_t(k)];
        for (std::size_t c = 0; c < C; ++c)
          for (std::size_t i = 0; i < S; ++i)
            out[(std::size_t(dst) * C + c) * S + i] =
                p[(m_.output_names.empty() ? std::size_t(k) * L.stride['S'] : 0) + c * L.stride['C'] + i * L.stride['T']];
      }
    }
    return true;
  }

 private:
  ModelManifest m_;
  ModelInfo info_;
  std::unique_ptr<Executor> ex_;
  std::vector<std::string> out_names_;
  std::array<int, kStemCount> canon_{};
  Shape in_shape_;
  LayoutIndex in_idx_;
  std::vector<float> in_buf_;
};

// One Demucs core network (skeleton + weights) with the C++ STFT around it.
struct DemucsCore {
  std::unique_ptr<Executor> ex;
  std::vector<float> spec, y;
  static constexpr std::size_t S = 343980;

  // Runs the network; fills `res` with [kStemCount][2][S] in the network's own stem order.
  bool run(const float* in, std::vector<float>& res, std::string* error) {
    const std::size_t T = DemucsDsp::frames_for(S), Fq = DemucsDsp::kBins;
    DemucsDsp::analyse(in, S, spec);
    InputBinding bs, bm;
    bs.name = "spec"; bs.data = spec.data(); bs.shape = Shape{1, 4, std::int64_t(Fq), std::int64_t(T)};
    bm.name = "mix"; bm.data = in; bm.shape = Shape{1, 2, std::int64_t(S)};
    if (!ex->run({bs, bm}, error)) return false;
    const graph::Tensor* xs = ex->output("xspec");
    const graph::Tensor* xt = ex->output("xtime");
    if (!xs || !xt || xs->numel() != std::int64_t(kStemCount * 4 * Fq * T) || xt->numel() != std::int64_t(kStemCount * 2 * S)) {
      if (error) *error = "unexpected network output shapes";
      return false;
    }
    DemucsDsp::synthesise(xs->f(), kStemCount, S, y);
    res.resize(std::size_t(kStemCount) * 2 * S);
    for (std::size_t i = 0; i < res.size(); ++i) res[i] = y[i] + xt->f()[i];
    return true;
  }
};

class GraphDemucsModel final : public StemModel {
 public:
  GraphDemucsModel(const ModelManifest& m, const std::string& path, int threads) {
    info_.id = m.id;
    info_.sample_rate = 44100;
    info_.segment_frames = DemucsCore::S;  // 7.8 s, the length the network was trained and exported for
    info_.channels = 2;
    std::string err;
    core_.ex = Executor::open(path, "", pick_threads(threads), &err);
    if (!core_.ex) throw std::runtime_error(err);
    for (int k = 0; k < kStemCount; ++k) canon_[std::size_t(k)] = stem_from_name(m.stems[std::size_t(k)]);
  }
  const ModelInfo& info() const override { return info_; }
  bool run(const float* in, float* out, std::string* error) override {
    if (!core_.run(in, res_, error)) return false;
    const std::size_t S = DemucsCore::S;
    for (int k = 0; k < kStemCount; ++k)
      std::copy_n(res_.data() + std::size_t(k) * 2 * S, 2 * S, out + std::size_t(canon_[std::size_t(k)]) * 2 * S);
    return true;
  }
  Executor& executor() { return *core_.ex; }

 private:
  ModelInfo info_;
  DemucsCore core_;
  std::array<int, kStemCount> canon_{};
  std::vector<float> res_;
};

// htdemucs_ft: a bag of four fine-tuned models, one per source, all with the htdemucs architecture. They share one
// skeleton (the .onnx in the manifest) and differ only in weights: htdemucs.<stem>.weights next to it. Model k runs on the
// chunk and contributes only its own stem (bag weights are the identity matrix upstream). The four executors share one
// activation arena because they run one after another.
class GraphDemucsBagModel final : public StemModel {
 public:
  GraphDemucsBagModel(const ModelManifest& m, const std::string& path, int threads) {
    info_.id = m.id;
    info_.sample_rate = 44100;
    info_.segment_frames = DemucsCore::S;
    info_.channels = 2;
    auto arena = std::make_shared<graph::SharedArena>();
    const std::filesystem::path dir = std::filesystem::path(path).parent_path();
    for (int k = 0; k < kStemCount; ++k) {
      canon_[std::size_t(k)] = stem_from_name(m.stems[std::size_t(k)]);
      if (canon_[std::size_t(k)] < 0) throw std::runtime_error("manifest stem names are not the four canonical stems");
      std::string err;
      cores_[std::size_t(k)].ex = Executor::open(path, (dir / ("htdemucs." + m.stems[std::size_t(k)] + ".weights")).string(), pick_threads(threads), &err);
      if (!cores_[std::size_t(k)].ex) throw std::runtime_error(err);
      cores_[std::size_t(k)].ex->set_shared_arena(arena);
    }
  }
  const ModelInfo& info() const override { return info_; }
  bool run(const float* in, float* out, std::string* error) override {
    const std::size_t S = DemucsCore::S;
    for (int k = 0; k < kStemCount; ++k) {
      if (!cores_[std::size_t(k)].run(in, res_, error)) return false;
      std::copy_n(res_.data() + std::size_t(k) * 2 * S, 2 * S, out + std::size_t(canon_[std::size_t(k)]) * 2 * S);
    }
    return true;
  }

 private:
  ModelInfo info_;
  std::array<DemucsCore, kStemCount> cores_;
  std::array<int, kStemCount> canon_{};
  std::vector<float> res_;
};

}  // namespace

std::unique_ptr<StemModel> open_graph(const ModelManifest& m, const std::string& path, int threads, std::string* error) {
  try {
    if (m.pipeline == "htdemucs-core") return std::make_unique<GraphDemucsModel>(m, path, threads);
    if (m.pipeline == "htdemucs-ft-core") return std::make_unique<GraphDemucsBagModel>(m, path, threads);
    return std::make_unique<GraphPlainModel>(m, path, threads);
  } catch (const std::exception& e) {
    if (error) *error = std::string("cannot load model: ") + e.what();
    return nullptr;
  }
}

}  // namespace deckhand::stems::detail
