#pragma once

#include <algorithm>
#include <cmath>

namespace iota::dsp {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 6.28318530717958647692f;

/// Standard Hamming window w(n) = 0.54 - 0.46 cos(2*pi*n/(N-1)).
[[nodiscard]] inline float hammingWindow(int n, int count) noexcept {
  if (count <= 1) {
    return 1.0f;
  }
  const float phase = kTwoPi * static_cast<float>(n) / static_cast<float>(count - 1);
  return 0.54f - 0.46f * std::cos(phase);
}

/// Standard Hann window w(n) = 0.5 * (1 - cos(2*pi*n/(N-1))).
[[nodiscard]] inline float hannWindow(int n, int count) noexcept {
  if (count <= 1) {
    return 1.0f;
  }
  const float phase = kTwoPi * static_cast<float>(n) / static_cast<float>(count - 1);
  return 0.5f * (1.0f - std::cos(phase));
}

/// Zeroth-order modified Bessel function I0(x) using standard polynomial approximations.
[[nodiscard]] inline float besselI0(float x) noexcept {
  const float ax = std::abs(x);
  if (ax < 3.75f) {
    const float t = x / 3.75f;
    const float y = t * t;
    return 1.0f + y * (3.5156229f +
                       y * (3.0899424f +
                            y * (1.2067492f +
                                 y * (0.2659732f +
                                      y * (0.0360768f + y * 0.0045813f)))));
  }
  const float y = 3.75f / ax;
  const float poly = 0.39894228f +
                     y * (0.01328592f +
                          y * (0.00225319f +
                               y * (-0.00157565f +
                                    y * (0.00916281f +
                                         y * (-0.02057706f +
                                              y * (0.02635537f +
                                                   y * (-0.01647633f + y * 0.00392377f)))))));
  return (std::exp(ax) / std::sqrt(ax)) * poly;
}

/// Kaiser-Bessel window taper used by time-domain Waveform snapshots.
[[nodiscard]] inline float kaiserWindow(int n, int count, float alpha = 0.8f) noexcept {
  if (count <= 1) {
    return 1.0f;
  }
  const float denom = besselI0(kPi * alpha);
  if (denom <= 0.0f) {
    return 1.0f;
  }
  const float normPos = (2.0f * static_cast<float>(n) / static_cast<float>(count)) - 1.0f;
  const float inside = std::max(0.0f, 1.0f - normPos * normPos);
  return besselI0(kPi * alpha * std::sqrt(inside)) / denom;
}

}  // namespace iota::dsp
