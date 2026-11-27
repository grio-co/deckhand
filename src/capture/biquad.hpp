#pragma once
// Internal: Butterworth high-pass cascade (4th order = two biquads).
#include <cmath>
#include <vector>

namespace deckhand::capture {

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  double process(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void reset() { z1 = z2 = 0; }
  static Biquad highpass(double fc, double rate, double q) {
    constexpr double kPi = 3.14159265358979323846;
    const double w = 2 * kPi * fc / rate, c = std::cos(w), a = std::sin(w) / (2 * q);
    const double a0 = 1 + a;
    Biquad b;
    b.b0 = (1 + c) / 2 / a0;
    b.b1 = -(1 + c) / a0;
    b.b2 = (1 + c) / 2 / a0;
    b.a1 = -2 * c / a0;
    b.a2 = (1 - a) / a0;
    return b;
  }
};

// Two cascaded sections: a 4th-order Butterworth high-pass.
struct HighPass4 {
  Biquad s1, s2;
  HighPass4() = default;
  HighPass4(double fc, double rate) : s1(Biquad::highpass(fc, rate, 0.5411961)), s2(Biquad::highpass(fc, rate, 1.3065630)) {}
  double process(double x) { return s2.process(s1.process(x)); }
  void reset() { s1.reset(); s2.reset(); }
};

}  // namespace deckhand::capture
