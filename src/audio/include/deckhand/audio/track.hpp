#pragma once
// Decoded audio held in memory, plus its display waveform.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace deckhand {

struct WavePoint {
  std::uint8_t low = 0, mid = 0, high = 0;  // band peaks, 0..255
};

struct TrackAudio {
  std::vector<float> pcm;  // interleaved stereo
  std::uint32_t sample_rate = 44100;
  std::uint64_t frames = 0;
  std::vector<WavePoint> wave;  // wave_rate points per second
  double wave_rate = 150.0;
  // Audio that lives elsewhere (a shared-memory block mapped from another process): when set, `pcm` is empty and
  // `ext_hold` keeps the mapping alive. Read samples through data().
  const float* ext_pcm = nullptr;
  std::shared_ptr<const void> ext_hold;
  const float* data() const { return ext_pcm ? ext_pcm : pcm.data(); }
};

struct FileTags {
  std::string title, artist, album;
  std::int64_t duration_ms = 0;
};

// Implemented per platform (platform/<os>). Returns nullptr and sets error on failure.
std::shared_ptr<TrackAudio> decode_file(const std::string& path, std::string& error);
bool read_file_tags(const std::string& path, FileTags& out);

// Fills audio.wave from audio.pcm.
void build_waveform(TrackAudio& audio);

// Steady-tempo estimate from the waveform. Returns false when the track is too short or silent.
bool estimate_tempo(const TrackAudio& audio, double& bpm, double& first_beat_seconds);

}  // namespace deckhand
