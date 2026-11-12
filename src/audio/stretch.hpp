#pragma once
// Time-stretcher for key lock: waveform-similarity overlap-add (WSOLA) reading straight from the
// in-memory track, so it adds no input latency. Original implementation.
#include <cstdint>
#include <vector>

namespace deckhand {

class Stretcher {
 public:
  void prepare(double sample_rate);  // allocates; not real-time
  void reset();                      // drop all content (state is rebuilt on the next call to next())
  void flush();                      // the play position jumped: fade the old audio out, restart there

  // One output frame. `pos` is the deck's play position (source frames), `v` the source frames the position
  // advances per output frame, `ratio` source-rate / output-rate and `pitch` the resampling factor applied
  // after stretching (1 = none; 2^(semitones/12) for key shift). The grain centre is aligned to `pos`, so the
  // reported position needs no latency correction.
  void next(const float* pcm, std::int64_t frames, double ratio, double pos, double v, double pitch, float& l,
            float& r);

 private:
  void produce(const float* pcm, std::int64_t frames, double ratio, double pos_w, double adv);
  int search(const float* pcm, std::int64_t frames, double ratio, double a);

  int n_ = 0, hs_ = 0, range_ = 0;
  std::int64_t mask_ = 0;
  std::vector<float> ring_, acc_, win_, tgt_, tgt2_, seg_;
  std::int64_t w_ = 0;
  double rpos_ = 0, cont_ = 0;
  bool have_ = false, jump_ = true;
  int keep_ = 0;
};

}  // namespace deckhand
