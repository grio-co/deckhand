// ONNX Runtime backend for the StemModel slot. Compiled only with DECKHAND_WITH_ONNX; otherwise open_onnx() reports
// that no backend is built in. The tensor layout handling (input/output letters) is driven entirely by the manifest.
#include "deckhand/stems/manifest.hpp"
#include "internal.hpp"

#if defined(DECKHAND_WITH_ONNX) && DECKHAND_WITH_ONNX
#include <onnxruntime_cxx_api.h>

#include "deckhand/stems/demucs_dsp.hpp"

namespace deckhand::stems::detail {
namespace {

class OnnxStemModel final : public StemModel {
 public:
  OnnxStemModel(const ModelManifest& m, const std::string& path, int threads) : m_(m) {
    info_.id = m.id;
    info_.sample_rate = m.sample_rate;
    info_.segment_frames = m.segment_frames;
    info_.channels = m.channels;
    env_ = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "deckhand-stems");
    Ort::SessionOptions so;
    if (threads > 0) so.SetIntraOpNumThreads(threads);
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (m.execution_provider == "coreml") {
      try { so.AppendExecutionProvider("CoreML", {}); } catch (const Ort::Exception&) { /* CPU fallback */ }
    }
#if defined(_WIN32)
    std::wstring wp(path.begin(), path.end());
    session_ = Ort::Session(env_, wp.c_str(), so);
#else
    session_ = Ort::Session(env_, path.c_str(), so);
#endif
    Ort::AllocatorWithDefaultOptions alloc;
    bool have_in = false;
    for (std::size_t i = 0; i < session_.GetInputCount(); ++i)
      if (session_.GetInputNameAllocated(i, alloc).get() == m.input_name) have_in = true;
    if (!have_in) throw std::runtime_error("model has no input tensor named '" + m.input_name + "'");
    std::vector<std::string> outs;
    for (std::size_t i = 0; i < session_.GetOutputCount(); ++i) outs.emplace_back(session_.GetOutputNameAllocated(i, alloc).get());
    out_names_ = m.output_names.empty() ? std::vector<std::string>{m.output_name} : m.output_names;
    for (auto& n : out_names_)
      if (std::find(outs.begin(), outs.end(), n) == outs.end()) throw std::runtime_error("model has no output tensor named '" + n + "'");
    // model stem slot -> canonical stem
    for (int k = 0; k < kStemCount; ++k) canon_[std::size_t(k)] = stem_from_name(m.stems[std::size_t(k)]);
    const std::string& il = m.input_layout;
    std::vector<std::int64_t> dims;
    for (char c : il) dims.push_back(c == 'B' ? 1 : c == 'C' ? m.channels : std::int64_t(m.segment_frames));
    in_dims_ = dims;
    in_idx_ = make_layout(il, dims, "BCT");
    if (!in_idx_.ok) throw std::runtime_error(in_idx_.error);
  }

  const ModelInfo& info() const override { return info_; }

  bool run(const float* in, float* out, std::string* error) override {
    try {
      const std::size_t S = info_.segment_frames, C = std::size_t(info_.channels);
      in_buf_.assign(in_idx_.total, 0.f);
      for (std::size_t c = 0; c < C; ++c)
        for (std::size_t t = 0; t < S; ++t) in_buf_[c * in_idx_.stride['C'] + t * in_idx_.stride['T']] = in[c * S + t];
      Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      Ort::Value iv = Ort::Value::CreateTensor<float>(mem, in_buf_.data(), in_buf_.size(), in_dims_.data(), in_dims_.size());
      const char* in_names[] = {m_.input_name.c_str()};
      std::vector<const char*> on;
      for (auto& n : out_names_) on.push_back(n.c_str());
      auto res = session_.Run(Ort::RunOptions{nullptr}, in_names, &iv, 1, on.data(), on.size());
      for (std::size_t r = 0; r < res.size(); ++r) {
        if (!res[r].IsTensor()) { if (error) *error = "model output is not a tensor"; return false; }
        auto shape = res[r].GetTensorTypeAndShapeInfo().GetShape();
        LayoutIndex L = make_layout(m_.output_layout, shape, "BSCT");
        if (!L.ok) { if (error) *error = "model output: " + L.error; return false; }
        auto dim = [&](char ch) { std::size_t k = m_.output_layout.find(ch); return k == std::string::npos ? std::int64_t(1) : shape[k]; };
        if (dim('B') != 1 || dim('C') != std::int64_t(C) || dim('T') != std::int64_t(S) ||
            dim('S') != (m_.output_names.empty() ? std::int64_t(kStemCount) : std::int64_t(1))) {
          if (error) *error = "model output shape does not match the manifest";
          return false;
        }
        const float* p = res[r].GetTensorData<float>();
        for (int k = 0; k < kStemCount; ++k) {
          if (!m_.output_names.empty() && std::size_t(k) != r) continue;
          int dst = canon_[std::size_t(k)];
          for (std::size_t c = 0; c < C; ++c)
            for (std::size_t t = 0; t < S; ++t)
              out[(std::size_t(dst) * C + c) * S + t] = p[std::size_t(k) * L.stride['S'] * (m_.output_names.empty() ? 1 : 0) + c * L.stride['C'] + t * L.stride['T']];
        }
      }
      return true;
    } catch (const std::exception& e) {
      if (error) *error = std::string("onnxruntime: ") + e.what();
      return false;
    }
  }

 private:
  ModelManifest m_;
  ModelInfo info_;
  Ort::Env env_{nullptr};
  Ort::Session session_{nullptr};
  std::vector<std::string> out_names_;
  std::array<int, kStemCount> canon_{};
  std::vector<std::int64_t> in_dims_;
  LayoutIndex in_idx_;
  std::vector<float> in_buf_;
};

// Hybrid Transformer Demucs: the ONNX skeleton holds the network core (inputs "spec", "mix"; outputs "xspec", "xtime");
// the STFT, the mask and the inverse STFT around it run in C++ (DemucsDsp).
class DemucsCoreModel final : public StemModel {
 public:
  DemucsCoreModel(const ModelManifest& m, const std::string& path, int threads) : m_(m) {
    info_.id = m.id;
    info_.sample_rate = 44100;
    info_.segment_frames = 343980;  // 7.8 s, the length the network was trained and exported for
    info_.channels = 2;
    env_ = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "deckhand-stems");
    Ort::SessionOptions so;
    if (threads > 0) so.SetIntraOpNumThreads(threads);
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (m.execution_provider == "coreml") {
      try { so.AppendExecutionProvider("CoreML", {}); } catch (const Ort::Exception&) { /* CPU fallback */ }
    }
#if defined(_WIN32)
    std::wstring wp(path.begin(), path.end());
    session_ = Ort::Session(env_, wp.c_str(), so);
#else
    session_ = Ort::Session(env_, path.c_str(), so);
#endif
    for (int k = 0; k < kStemCount; ++k) canon_[std::size_t(k)] = stem_from_name(m.stems[std::size_t(k)]);
  }
  const ModelInfo& info() const override { return info_; }
  bool run(const float* in, float* out, std::string* error) override {
    try {
      const std::size_t S = info_.segment_frames, T = DemucsDsp::frames_for(S), Fq = DemucsDsp::kBins;
      DemucsDsp::analyse(in, S, spec_);
      Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      std::vector<float> mix(in, in + 2 * S);
      std::int64_t sd[4] = {1, 4, std::int64_t(Fq), std::int64_t(T)}, md[3] = {1, 2, std::int64_t(S)};
      Ort::Value ins[2] = {Ort::Value::CreateTensor<float>(mem, spec_.data(), spec_.size(), sd, 4),
                           Ort::Value::CreateTensor<float>(mem, mix.data(), mix.size(), md, 3)};
      const char* in_names[] = {"spec", "mix"};
      const char* out_names[] = {"xspec", "xtime"};
      auto res = session_.Run(Ort::RunOptions{nullptr}, in_names, ins, 2, out_names, 2);
      const float* xs = res[0].GetTensorData<float>();
      const float* xt = res[1].GetTensorData<float>();
      DemucsDsp::synthesise(xs, kStemCount, S, y_);
      for (int k = 0; k < kStemCount; ++k)
        for (std::size_t c = 0; c < 2; ++c)
          for (std::size_t i = 0; i < S; ++i)
            out[(std::size_t(canon_[std::size_t(k)]) * 2 + c) * S + i] = y_[(std::size_t(k) * 2 + c) * S + i] + xt[(std::size_t(k) * 2 + c) * S + i];
      return true;
    } catch (const std::exception& e) {
      if (error) *error = std::string("onnxruntime: ") + e.what();
      return false;
    }
  }
 private:
  ModelManifest m_;
  ModelInfo info_;
  Ort::Env env_{nullptr};
  Ort::Session session_{nullptr};
  std::array<int, kStemCount> canon_{};
  std::vector<float> spec_, y_;
};

}  // namespace

std::unique_ptr<StemModel> open_onnx(const ModelManifest& m, const std::string& path, int threads, std::string* error) {
  try {
    if (m.pipeline == "htdemucs-core") return std::make_unique<DemucsCoreModel>(m, path, threads);
    return std::make_unique<OnnxStemModel>(m, path, threads);
  } catch (const std::exception& e) {
    if (error) *error = std::string("cannot load model: ") + e.what();
    return nullptr;
  }
}

}  // namespace deckhand::stems::detail
#else
namespace deckhand::stems::detail {
std::unique_ptr<StemModel> open_onnx(const ModelManifest&, const std::string&, int, std::string* error) {
  if (error) *error = "built without ONNX Runtime";
  return nullptr;
}
}  // namespace deckhand::stems::detail
#endif
