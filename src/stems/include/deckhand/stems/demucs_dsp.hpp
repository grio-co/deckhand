// The signal processing Hybrid Transformer Demucs does around its neural core (STFT with reflect padding, complex-as-
// channels packing, the mask and the inverse STFT), reimplemented in C++ from the published algorithm so the ONNX
// skeleton only has to hold the network itself. n_fft 4096, hop 1024, periodic Hann window, "normalized" scaling.
#pragma once
#include <cstddef>
#include <vector>

namespace deckhand::stems {

struct DemucsDsp {
  static constexpr std::size_t kNfft = 4096, kHop = 1024, kBins = 2048;
  static std::size_t frames_for(std::size_t length) { return (length + kHop - 1) / kHop; }
  // mix: 2 x length planar -> spec: 4 x kBins x frames (channels: left re, left im, right re, right im)
  static void analyse(const float* mix, std::size_t length, std::vector<float>& spec);
  // xspec: S x 4 x kBins x frames (the network's spectral output) -> out: S x 2 x length (spectral branch only; the
  // caller adds the network's time-branch output)
  static void synthesise(const float* xspec, int stems, std::size_t length, std::vector<float>& out);
};

}  // namespace deckhand::stems
