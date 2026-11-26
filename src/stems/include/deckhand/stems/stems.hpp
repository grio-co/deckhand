// Offline four-stem separation (vocals / drums / bass / other).
//
// DeckHand separates ahead of time, caches the result on disk (cache.hpp) and lets the engine play four
// pre-rendered stems later, so there is no live inference cost. This header is the model-independent core:
// the pluggable model slot, chunking with overlap-add, resampling and the mixture-consistency step.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace deckhand::stems {

constexpr int kStemCount = 4;
enum Stem : int { Vocals = 0, Drums = 1, Bass = 2, Other = 3 };
const char* stem_name(int stem);          // "vocals", "drums", "bass", "other"
int stem_from_name(const std::string& n);  // -1 when unknown

// Planar stereo float PCM (nominally -1..1).
struct Audio {
  int sample_rate = 0;
  std::vector<float> l, r;
  std::size_t frames() const { return l.size(); }
};
using Stems = std::array<Audio, kStemCount>;  // canonical order: vocals, drums, bass, other

struct ModelInfo {
  std::string id;                  // names the model and version; part of every cache key
  int sample_rate = 44100;         // rate the model works at
  std::size_t segment_frames = 0;  // fixed chunk length in frames
  int channels = 2;                // only 2 is supported
};

// The pluggable model slot. Implementations: OnnxStemModel (manifest.hpp), test doubles.
class StemModel {
 public:
  virtual ~StemModel() = default;
  virtual const ModelInfo& info() const = 0;
  // in:  channels x segment_frames, planar (channel 0 then channel 1), at info().sample_rate
  // out: kStemCount x channels x segment_frames, planar, canonical stem order
  virtual bool run(const float* in, float* out, std::string* error) = 0;
};

enum class MixtureMode {
  None,          // keep the model output as is
  ToOther,       // residual goes entirely to "other" (other = mix - vocals - drums - bass)
  Proportional,  // residual shared by local stem energy, then "other" closed exactly (default)
};

using ProgressFn = std::function<void(float fraction)>;  // 0..1, called from the separating thread

struct SeparateOptions {
  float overlap = 0.25f;  // fraction of a segment shared by neighbouring chunks (0 .. 0.5)
  MixtureMode mixture = MixtureMode::Proportional;
  ProgressFn progress;
  const std::atomic<bool>* cancel = nullptr;  // set to true to abort at the next chunk boundary
  const std::atomic<bool>* pause = nullptr;   // while true, waits between chunks (cancel still works)
};

struct SeparateResult {
  enum class Status { Ok, Cancelled, Failed } status = Status::Failed;
  std::string error;
  Stems stems;          // at the input's sample rate and length
  double seconds = 0;   // wall time
};

// Separates `in` (any sample rate; resampled to the model rate and back). The four stems sum back to the input
// within float rounding unless mixture == None. Never produces seams at chunk borders: chunks are cross-faded
// with complementary windows and normalised by the accumulated window weight.
SeparateResult separate(const Audio& in, StemModel& model, const SeparateOptions& opt = {});

// True when a real model backend (ONNX Runtime) is compiled in and stems are not disabled at build time.
// Separation with a custom StemModel works either way.
bool stems_available();
const char* backend_name();  // "onnxruntime", or "none"

// Band-limited resampler (Kaiser-windowed sinc, DC gain 1). Output length is round(n * to / from).
std::vector<float> resample(const std::vector<float>& in, int from_rate, int to_rate);

// Plain WAV I/O for tools and tests (PCM 8/16/24/32 and float32 in; 16/24/32-float out). Mono input is duplicated.
bool read_wav(const std::string& path, Audio& out, std::string* error = nullptr);
bool write_wav(const std::string& path, const Audio& a, int bits /*16, 24 or 32 = float*/, std::string* error = nullptr);

}  // namespace deckhand::stems
