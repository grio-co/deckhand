// Signal processing around the Mel-Band RoFormer network (the part the ONNX skeleton does not hold): STFT (n_fft 2048, hop 441,
// periodic Hann, centred with reflect padding), gathering the STFT bins of the 60 mel bands into the network's feature vector,
// the overlap-averaged complex mask, and the inverse STFT. Written from the published algorithm of the reference implementation
// (bs-roformer 0.3.10, MIT); the band edges come from the layout file written by tools/export_roformer_skeleton.py.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace deckhand::stems {

struct RoformerDsp {
  static constexpr std::size_t kNfft = 2048, kHop = 441, kBins = 1025;
  std::vector<std::pair<int, int>> bands;  // per mel band: first STFT bin, bin count (contiguous ranges)
  std::size_t features = 0;                // network feature width = 4 * sum of counts (bin, channel, re/im)

  // Bands from the layout json ("bands": [[first, count], ...]). Fails on gaps, overlaps with bins outside 0..1024, or empty input.
  bool set_bands(const std::vector<std::pair<int, int>>& b, std::string* error);
  static std::size_t frames_for(std::size_t length) { return 1 + length / kHop; }

  // mix: 2 x length planar. stft: [2][kBins][T][2] (kept for synthesise); x: [T][features] network input.
  void analyse(const float* mix, std::size_t length, std::vector<float>& stft, std::vector<float>& x) const;
  // masks: [T][features] network output. out: 2 x (hop * (T - 1)) planar (equals `length` for lengths that are multiples of the hop).
  void synthesise(const std::vector<float>& stft, const float* masks, std::size_t length, std::vector<float>& out) const;
};

}  // namespace deckhand::stems
