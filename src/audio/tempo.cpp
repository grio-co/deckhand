// Basic tempo and beat-phase estimate for tracks that arrive without analysis.
// Assumes a steady tempo; a full analyser (variable tempo, downbeats, key) comes later.
#include <algorithm>
#include <cmath>
#include <vector>

#include "deckhand/audio/track.hpp"

namespace deckhand {
namespace {

// Onset strength sampled at a fractional index.
inline float at(const std::vector<float>& e, double i) {
  if (i < 0) return 0;
  const std::size_t k = static_cast<std::size_t>(i);
  if (k + 1 >= e.size()) return 0;
  const float t = static_cast<float>(i - static_cast<double>(k));
  return e[k] + (e[k + 1] - e[k]) * t;
}

// Best alignment of a beat comb with the given period (in points): returns score, sets phase.
double comb(const std::vector<float>& e, double period, int phases, double& best_phase) {
  double best = -1;
  for (int p = 0; p < phases; ++p) {
    const double phase = period * p / phases;
    double sum = 0;
    int count = 0;
    for (double i = phase; i < static_cast<double>(e.size()) - 1; i += period, ++count) sum += at(e, i);
    const double score = count ? sum / count : 0;
    if (score > best) {
      best = score;
      best_phase = phase;
    }
  }
  return best;
}

}  // namespace

bool estimate_tempo(const TrackAudio& a, double& bpm, double& first_beat_seconds) {
  const double rate = a.wave_rate;
  const std::size_t n = a.wave.size();
  if (n < static_cast<std::size_t>(rate * 8)) return false;

  // Onset envelope: positive change in band energy, low end weighted most.
  std::vector<float> e(n, 0.0f);
  float prev = 0, total = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const float s = 2.0f * a.wave[i].low + a.wave[i].mid + 0.5f * a.wave[i].high;
    e[i] = std::max(0.0f, s - prev);
    total += e[i];
    prev = s;
  }
  if (total <= 0) return false;

  // Coarse: autocorrelation over lags for 78..176 BPM.
  const int lag_min = static_cast<int>(rate * 60.0 / 176.0), lag_max = static_cast<int>(rate * 60.0 / 78.0);
  double best_r = -1;
  int best_lag = lag_min;
  for (int lag = lag_min; lag <= lag_max; ++lag) {
    double r = 0;
    const std::size_t m = n - static_cast<std::size_t>(lag);
    for (std::size_t i = 0; i < m; ++i) r += static_cast<double>(e[i]) * e[i + static_cast<std::size_t>(lag)];
    r /= static_cast<double>(m);
    if (r > best_r) {
      best_r = r;
      best_lag = lag;
    }
  }

  // Fine: slide a beat comb over the whole track around the coarse tempo.
  const double coarse = rate * 60.0 / best_lag;
  double best_score = -1, best_bpm = coarse, best_phase = 0;
  for (double b = coarse * 0.97; b <= coarse * 1.03; b += 0.01) {
    double phase = 0;
    const double score = comb(e, rate * 60.0 / b, 24, phase);
    if (score > best_score) {
      best_score = score;
      best_bpm = b;
      best_phase = phase;
    }
  }
  if (std::fabs(best_bpm - std::round(best_bpm)) < 0.04) best_bpm = std::round(best_bpm);
  const double period = rate * 60.0 / best_bpm;
  comb(e, period, 240, best_phase);

  bpm = std::round(best_bpm * 100.0) / 100.0;
  // The envelope marks the rise into each beat one point early; centre it.
  first_beat_seconds = std::fmod((best_phase - 0.5) / rate + 60.0 / bpm, 60.0 / bpm);
  return true;
}

}  // namespace deckhand
