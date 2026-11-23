#pragma once

#include <algorithm>
#include <cmath>

namespace iota::dsp {

/// IEC 61672 linear magnitude response R_A(f) before +2 dB 1 kHz normalization.
[[nodiscard]] inline double aWeightingRawRatio(double freqHz) noexcept {
  const double f = std::max(1.0, freqHz);
  const double f2 = f * f;
  constexpr double c1 = 20.598997 * 20.598997;      // ~424.32
  constexpr double c2 = 107.65265 * 107.65265;      // ~11589.09
  constexpr double c3 = 737.86223 * 737.86223;      // ~544440.67
  constexpr double c4 = 12194.217 * 12194.217;      // ~1.487e8

  const double num = c4 * f2 * f2;
  const double den = (f2 + c1) * std::sqrt((f2 + c2) * (f2 + c3)) * (f2 + c4);
  return den > 0.0 ? (num / den) : 0.0;
}

/// Normalized IEC 61672 A-weighting linear amplitude gain where gain(1000 Hz) = 1.0.
[[nodiscard]] inline float aWeightingNormalizedGain(double freqHz) noexcept {
  static const double kRef1000 = aWeightingRawRatio(1000.0);
  return static_cast<float>(aWeightingRawRatio(freqHz) / kRef1000);
}

/// Spectrum (FFT) A-weighting curve clamped flat above 1000 Hz, cubed per APK Spectrum math.
[[nodiscard]] inline float spectrumAWeightMultiplier(double freqHz, float aWeightStrength) noexcept {
  const float w = std::clamp(aWeightStrength, 0.0f, 1.0f);
  if (w <= 0.0f) {
    return 1.0f;
  }
  const float r = std::clamp(aWeightingNormalizedGain(std::min(freqHz, 1000.0)), 0.0f, 1.0f);
  return (1.0f - w) + w * (r * r * r);
}

/// Spectrum2 lower-frequency A-weighting factor clamped flat above 2600 Hz.
[[nodiscard]] inline float spectrum2LowerFreqMultiplier(double freqHz, float lowerFreqMagnitude) noexcept {
  const float amount = std::clamp(lowerFreqMagnitude, 0.0f, 1.0f);
  if (amount <= 0.0f) {
    return 1.0f;
  }
  const float gain = std::clamp(aWeightingNormalizedGain(std::min(freqHz, 2600.0)), 0.04f, 1.25f);
  return (1.0f - amount) + amount * gain;
}

}  // namespace iota::dsp
