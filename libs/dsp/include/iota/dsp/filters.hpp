#pragma once

#include "iota/dsp/windows.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>
#include <vector>

namespace iota::dsp {

/// Continuous exponential smoothing alpha from time constant in milliseconds: `1 - exp(-dt / tau)`.
[[nodiscard]] inline float expSmoothingAlphaMs(float dtSec, float timeConstantMs) noexcept {
  if (dtSec <= 0.0f) {
    return 0.0f;
  }
  if (timeConstantMs <= 0.0f) {
    return 1.0f;
  }
  const float tauSec = timeConstantMs * 0.001f;
  const float ratio = dtSec / tauSec;
  return std::clamp(1.0f - std::exp(-ratio), 0.0f, 1.0f);
}

/// Converts a per-frame smoothing factor tuned at `refFps` (default 60 Hz) to an equivalent
/// frame-rate-independent alpha for arbitrary `dtSec`: `1 - (1 - alphaRef)^(dtSec * refFps)`.
[[nodiscard]] inline float frameRateIndependentAlpha(float alphaAtRefFps,
                                                     float dtSec,
                                                     float refFps = 60.0f) noexcept {
  if (dtSec <= 0.0f || alphaAtRefFps <= 0.0f) {
    return 0.0f;
  }
  if (alphaAtRefFps >= 1.0f) {
    return 1.0f;
  }
  const float decay = 1.0f - alphaAtRefFps;
  const float steps = std::max(0.0f, dtSec * refFps);
  return std::clamp(1.0f - std::pow(decay, steps), 0.0f, 1.0f);
}

/// High-pass / sharpening spatial 1D bar kernel with outer weights `-strength` and center `2*R`.
class SharpenKernel {
 public:
  void configure(int radius, float strength) {
    const int r = std::clamp(radius, 0, 16);
    if (configured_ && r == rawRadius_ && std::abs(strength - rawStrength_) < 1e-6f) {
      return;
    }
    configured_ = true;
    rawRadius_ = r;
    rawStrength_ = strength;
    if (r == 0 || strength <= 0.0001f) {
      radius_ = 0;
      weights_ = {1.0f};
      return;
    }
    radius_ = r;
    const int len = 2 * r + 1;
    weights_.assign(static_cast<size_t>(len), -strength);
    weights_[static_cast<size_t>(r)] = 2.0f * static_cast<float>(r);

    float sum = 0.0f;
    for (float w : weights_) {
      sum += w;
    }
    if (std::abs(sum) > 1e-4f) {
      for (float& w : weights_) {
        w /= sum;
      }
    } else {
      weights_.assign(static_cast<size_t>(len), 0.0f);
      weights_[static_cast<size_t>(r)] = 1.0f;
    }
  }

  void apply(std::span<const float> input, std::vector<float>& output) const {
    const int n = static_cast<int>(input.size());
    output.resize(static_cast<size_t>(n));
    if (n == 0) {
      return;
    }
    if (radius_ <= 0) {
      std::copy(input.begin(), input.end(), output.begin());
      return;
    }
    for (int i = 0; i < n; ++i) {
      float acc = 0.0f;
      for (int k = -radius_; k <= radius_; ++k) {
        const int idx = std::clamp(i + k, 0, n - 1);
        acc += input[static_cast<size_t>(idx)] * weights_[static_cast<size_t>(k + radius_)];
      }
      output[static_cast<size_t>(i)] = std::max(0.0f, acc);
    }
  }

 private:
  bool configured_ = false;
  int rawRadius_ = -1;
  float rawStrength_ = -1.0f;
  int radius_ = 0;
  std::vector<float> weights_{1.0f};
};

/// Gaussian-impulse spatial filter used by Spectrum2 (`(1 - f) * impulse + f * gaussian`).
class GaussianImpulseKernel {
 public:
  void configure(float radius, float blendStrength) {
    const float f = std::clamp(blendStrength, 0.0f, 1.0f);
    if (configured_ && std::abs(radius - rawRadius_) < 1e-6f && std::abs(f - rawBlend_) < 1e-6f) {
      return;
    }
    configured_ = true;
    rawRadius_ = radius;
    rawBlend_ = f;
    const int half = std::clamp(static_cast<int>(std::ceil(2.57f * std::max(0.0f, radius))), 0, 32);
    if (half <= 0 || radius <= 0.01f || f <= 0.0001f) {
      halfWidth_ = 0;
      weights_ = {1.0f};
      return;
    }
    halfWidth_ = half;
    const int len = 2 * half + 1;
    weights_.resize(static_cast<size_t>(len));

    const float sigma = std::max(0.25f, radius);
    const float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);
    float gaussSum = 0.0f;
    for (int k = -half; k <= half; ++k) {
      const float g = std::exp(-static_cast<float>(k * k) * invTwoSigma2);
      weights_[static_cast<size_t>(k + half)] = g;
      gaussSum += g;
    }

    float l1Sum = 0.0f;
    for (int k = -half; k <= half; ++k) {
      float& w = weights_[static_cast<size_t>(k + half)];
      const float gaussNorm = gaussSum > 0.0f ? (w / gaussSum) : (k == 0 ? 1.0f : 0.0f);
      const float impulse = (k == 0) ? 1.0f : 0.0f;
      w = (1.0f - f) * impulse + f * gaussNorm;
      l1Sum += std::abs(w);
    }
    if (l1Sum > 1e-5f) {
      for (float& w : weights_) {
        w /= l1Sum;
      }
    }
  }

  void apply(std::span<const float> input, std::vector<float>& output) const {
    const int n = static_cast<int>(input.size());
    output.resize(static_cast<size_t>(n));
    if (n == 0) {
      return;
    }
    if (halfWidth_ <= 0) {
      std::copy(input.begin(), input.end(), output.begin());
      return;
    }
    for (int i = 0; i < n; ++i) {
      float acc = 0.0f;
      for (int k = -halfWidth_; k <= halfWidth_; ++k) {
        const int idx = std::clamp(i + k, 0, n - 1);
        acc += input[static_cast<size_t>(idx)] * weights_[static_cast<size_t>(k + halfWidth_)];
      }
      output[static_cast<size_t>(i)] = std::max(0.0f, acc);
    }
  }

 private:
  bool configured_ = false;
  float rawRadius_ = -1.0f;
  float rawBlend_ = -1.0f;
  int halfWidth_ = 0;
  std::vector<float> weights_{1.0f};
};

/// Spatial / inter-bin DSP frequency smoothing mode (separate from temporal attack/release).
enum class FrequencySmoothMode {
  None,
  MovingAverage,
  Gaussian,
  SavitzkyGolay,
};

[[nodiscard]] inline std::string_view frequencySmoothModeToString(FrequencySmoothMode mode) noexcept {
  switch (mode) {
    case FrequencySmoothMode::MovingAverage: return "MovingAverage";
    case FrequencySmoothMode::Gaussian: return "Gaussian";
    case FrequencySmoothMode::SavitzkyGolay: return "SavitzkyGolay";
    case FrequencySmoothMode::None:
    default: return "None";
  }
}

[[nodiscard]] inline FrequencySmoothMode frequencySmoothModeFromString(std::string_view str) noexcept {
  if (str == "MovingAverage") return FrequencySmoothMode::MovingAverage;
  if (str == "Gaussian") return FrequencySmoothMode::Gaussian;
  if (str == "SavitzkyGolay") return FrequencySmoothMode::SavitzkyGolay;
  return FrequencySmoothMode::None;
}

/// Configurable 1D spatial frequency-bin smoother with cached normalized kernel weights.
class FrequencySmoother {
 public:
  void configure(FrequencySmoothMode mode, float radius, float strength) {
    const float clampedStrength = std::clamp(strength, 0.0f, 1.0f);
    const float clampedRadius = std::clamp(radius, 0.0f, 16.0f);
    if (configured_ && mode == mode_ &&
        std::abs(clampedRadius - radius_) < 1e-5f &&
        std::abs(clampedStrength - strength_) < 1e-5f) {
      return;
    }

    configured_ = true;
    mode_ = mode;
    radius_ = clampedRadius;
    strength_ = clampedStrength;
    halfWidth_ = 0;
    weights_ = {1.0f};

    if (mode_ == FrequencySmoothMode::None || strength_ <= 1e-4f || radius_ < 0.05f) {
      return;
    }

    if (mode_ == FrequencySmoothMode::MovingAverage) {
      halfWidth_ = std::clamp(static_cast<int>(std::lround(radius_)), 1, 16);
      const int len = 2 * halfWidth_ + 1;
      weights_.assign(static_cast<size_t>(len), 1.0f / static_cast<float>(len));
    } else if (mode_ == FrequencySmoothMode::Gaussian) {
      const float sigma = std::clamp(radius_, 0.25f, 16.0f);
      halfWidth_ = std::clamp(static_cast<int>(std::ceil(2.5f * sigma)), 1, 24);
      const int len = 2 * halfWidth_ + 1;
      weights_.resize(static_cast<size_t>(len));
      const float invTwoSigSq = 1.0f / (2.0f * sigma * sigma);
      float sum = 0.0f;
      for (int k = -halfWidth_; k <= halfWidth_; ++k) {
        const float w = std::exp(-(static_cast<float>(k * k)) * invTwoSigSq);
        weights_[static_cast<size_t>(k + halfWidth_)] = w;
        sum += w;
      }
      if (sum > 1e-6f) {
        for (float& w : weights_) {
          w /= sum;
        }
      }
    } else if (mode_ == FrequencySmoothMode::SavitzkyGolay) {
      // Closed-form quadratic/cubic Savitzky-Golay polynomial smoother weights
      halfWidth_ = std::clamp(static_cast<int>(std::lround(radius_)), 2, 12);
      const int len = 2 * halfWidth_ + 1;
      weights_.resize(static_cast<size_t>(len));
      const float m = static_cast<float>(halfWidth_);
      const float c0 = 3.0f * m * m + 3.0f * m - 1.0f;
      float sum = 0.0f;
      for (int k = -halfWidth_; k <= halfWidth_; ++k) {
        const float w = c0 - 5.0f * static_cast<float>(k * k);
        weights_[static_cast<size_t>(k + halfWidth_)] = w;
        sum += w;
      }
      if (std::abs(sum) > 1e-6f) {
        for (float& w : weights_) {
          w /= sum;
        }
      }
    }
  }

  void apply(std::span<const float> input, std::vector<float>& output) const {
    const int n = static_cast<int>(input.size());
    output.resize(static_cast<size_t>(n));
    if (n <= 0) {
      return;
    }
    if (mode_ == FrequencySmoothMode::None || halfWidth_ <= 0 || strength_ <= 1e-4f) {
      std::copy(input.begin(), input.end(), output.begin());
      return;
    }

    for (int i = 0; i < n; ++i) {
      float accum = 0.0f;
      for (int k = -halfWidth_; k <= halfWidth_; ++k) {
        const int src = std::clamp(i + k, 0, n - 1);
        accum += input[static_cast<size_t>(src)] * weights_[static_cast<size_t>(k + halfWidth_)];
      }
      const float raw = input[static_cast<size_t>(i)];
      const float blended = raw + (accum - raw) * strength_;
      output[static_cast<size_t>(i)] = std::max(0.0f, blended);
    }
  }

 private:
  bool configured_ = false;
  FrequencySmoothMode mode_ = FrequencySmoothMode::None;
  float radius_ = 0.0f;
  float strength_ = 0.0f;
  int halfWidth_ = 0;
  std::vector<float> weights_{1.0f};
};

/// Time-indexed bar history ring buffer for frame-rate-independent per-layer delay and softness.
class BarHistoryRing {
 public:
  explicit BarHistoryRing(int capacity = 64)
      : frames_(static_cast<size_t>(std::max(1, capacity))) {}

  void clear() {
    for (auto& f : frames_) {
      f.timeSec = 0.0;
      f.bars.clear();
    }
    newestIndex_ = -1;
    count_ = 0;
    currentTimeSec_ = 0.0;
  }

  void pushFrame(std::span<const float> bars, float dtSec = 1.0f / 60.0f) {
    if (frames_.empty()) {
      return;
    }
    currentTimeSec_ += static_cast<double>(std::max(0.0f, dtSec));
    newestIndex_ = (newestIndex_ + 1) % static_cast<int>(frames_.size());
    auto& slot = frames_[static_cast<size_t>(newestIndex_)];
    slot.timeSec = currentTimeSec_;
    slot.bars.assign(bars.begin(), bars.end());
    count_ = std::min(count_ + 1, static_cast<int>(frames_.size()));
  }

  void sampleBars(int reactionDelay,
                  int reactionAccumulatedDelay,
                  int softness,
                  std::vector<float>& outBars) const {
    if (count_ <= 0 || newestIndex_ < 0) {
      outBars.clear();
      return;
    }
    const int cap = static_cast<int>(frames_.size());
    const auto& newestFrame = frames_[static_cast<size_t>(newestIndex_)];
    const int barCount = static_cast<int>(newestFrame.bars.size());
    if (barCount == 0) {
      outBars.clear();
      return;
    }

    const int d0 = std::clamp(reactionDelay, 0, 24);
    const int accSteps = std::clamp(reactionAccumulatedDelay, 0, 24);

    if (d0 == 0 && accSteps == 0) {
      tempAvg_ = newestFrame.bars;
    } else {
      tempAvg_.assign(static_cast<size_t>(barCount), 0.0f);
      const int tapCount = accSteps + 1;
      const float tapWeight = 1.0f / static_cast<float>(tapCount);

      for (int k = 0; k <= accSteps; ++k) {
        const double delaySec = static_cast<double>(d0 + k) / 60.0;
        const double targetTime = newestFrame.timeSec - delaySec;
        accumulateAtTime(targetTime, barCount, tapWeight, cap);
      }
    }

    const int soft = std::clamp(softness, 0, 24);
    if (soft <= 0) {
      outBars = tempAvg_;
      return;
    }

    const int kernelLen = 2 * soft + 1;
    outBars.resize(static_cast<size_t>(barCount));
    for (int b = 0; b < barCount; ++b) {
      float weightedSum = 0.0f;
      float weightTotal = 0.0f;
      for (int k = -soft; k <= soft; ++k) {
        const float w = std::max(hammingWindow(soft + 1 + k, kernelLen + 2) - 0.05f, 0.0f);
        const int src = std::clamp(b + k, 0, barCount - 1);
        weightedSum += tempAvg_[static_cast<size_t>(src)] * w;
        weightTotal += w;
      }
      outBars[static_cast<size_t>(b)] = (weightTotal > 1e-6f) ? (weightedSum / weightTotal)
                                                              : tempAvg_[static_cast<size_t>(b)];
    }
  }

 private:
  struct TimedFrame {
    double timeSec = 0.0;
    std::vector<float> bars;
  };

  void accumulateAtTime(double targetTime, int barCount, float weight, int cap) const {
    int prevValidOffset = -1;
    for (int off = 0; off < count_; ++off) {
      const int idx = (newestIndex_ - off + cap * 2) % cap;
      const auto& frame = frames_[static_cast<size_t>(idx)];
      if (static_cast<int>(frame.bars.size()) != barCount) {
        continue;
      }
      if (prevValidOffset < 0 && targetTime >= frame.timeSec - 1e-9) {
        for (int b = 0; b < barCount; ++b) {
          tempAvg_[static_cast<size_t>(b)] += frame.bars[static_cast<size_t>(b)] * weight;
        }
        return;
      }
      if (targetTime >= frame.timeSec - 1e-9 && prevValidOffset >= 0) {
        const int newerIdx = (newestIndex_ - prevValidOffset + cap * 2) % cap;
        const auto& newer = frames_[static_cast<size_t>(newerIdx)];
        const double span = newer.timeSec - frame.timeSec;
        const float u = (span > 1e-9)
                            ? std::clamp(static_cast<float>((targetTime - frame.timeSec) / span), 0.0f, 1.0f)
                            : 1.0f;
        const float invU = 1.0f - u;
        for (int b = 0; b < barCount; ++b) {
          const float interp = frame.bars[static_cast<size_t>(b)] * invU +
                               newer.bars[static_cast<size_t>(b)] * u;
          tempAvg_[static_cast<size_t>(b)] += interp * weight;
        }
        return;
      }
      prevValidOffset = off;
    }

    if (prevValidOffset >= 0) {
      const int oldestIdx = (newestIndex_ - prevValidOffset + cap * 2) % cap;
      const auto& oldest = frames_[static_cast<size_t>(oldestIdx)];
      for (int b = 0; b < barCount; ++b) {
        tempAvg_[static_cast<size_t>(b)] += oldest.bars[static_cast<size_t>(b)] * weight;
      }
    }
  }

  std::vector<TimedFrame> frames_;
  mutable std::vector<float> tempAvg_;
  int newestIndex_ = -1;
  int count_ = 0;
  double currentTimeSec_ = 0.0;
};

/// Visual curve interpolation mode applied to Bars geometry at render time (decoupled from DSP).
enum class CurveInterpolationMode {
  None,
  Linear,
  CatmullRom,
  CubicBSpline,
};

[[nodiscard]] inline std::string_view curveInterpolationModeToString(CurveInterpolationMode mode) noexcept {
  switch (mode) {
    case CurveInterpolationMode::Linear: return "Linear";
    case CurveInterpolationMode::CatmullRom: return "CatmullRom";
    case CurveInterpolationMode::CubicBSpline: return "CubicBSpline";
    case CurveInterpolationMode::None:
    default: return "None";
  }
}

[[nodiscard]] inline CurveInterpolationMode curveInterpolationModeFromString(std::string_view str) noexcept {
  if (str == "Linear") return CurveInterpolationMode::Linear;
  if (str == "CatmullRom") return CurveInterpolationMode::CatmullRom;
  if (str == "CubicBSpline") return CurveInterpolationMode::CubicBSpline;
  return CurveInterpolationMode::None;
}

/// Subdivides discrete bar heights into a smooth visual curve (`N * subs` closed, `(N-1)*subs + 1` open).
inline void interpolateBarCurve(std::span<const float> inHeights,
                                CurveInterpolationMode mode,
                                int subdivisions,
                                bool closed,
                                std::vector<float>& outHeights) {
  const int n = static_cast<int>(inHeights.size());
  const int subs = std::clamp(subdivisions, 1, 16);
  if (n < 2 || mode == CurveInterpolationMode::None || subs <= 1) {
    outHeights.assign(inHeights.begin(), inHeights.end());
    return;
  }

  auto sampleAt = [&](int idx) noexcept -> float {
    if (closed) {
      const int wrapped = ((idx % n) + n) % n;
      return inHeights[static_cast<size_t>(wrapped)];
    }
    return inHeights[static_cast<size_t>(std::clamp(idx, 0, n - 1))];
  };

  auto evalSegment = [&](int i, float t) noexcept -> float {
    const float p1 = sampleAt(i);
    const float p2 = sampleAt(i + 1);
    if (mode == CurveInterpolationMode::Linear) {
      return p1 + (p2 - p1) * t;
    }
    const float p0 = sampleAt(i - 1);
    const float p3 = sampleAt(i + 2);
    const float t2 = t * t;
    const float t3 = t2 * t;
    if (mode == CurveInterpolationMode::CatmullRom) {
      return 0.5f * ((2.0f * p1) +
                     (-p0 + p2) * t +
                     (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                     (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    }
    // CubicBSpline: convex C2 blend, no overshoot
    const float omt = 1.0f - t;
    const float b0 = (omt * omt * omt) / 6.0f;
    const float b1 = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
    const float b2 = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
    const float b3 = t3 / 6.0f;
    return p0 * b0 + p1 * b1 + p2 * b2 + p3 * b3;
  };

  const int segCount = closed ? n : (n - 1);
  outHeights.clear();
  outHeights.reserve(static_cast<size_t>(segCount * subs + (closed ? 0 : 1)));
  for (int i = 0; i < segCount; ++i) {
    for (int s = 0; s < subs; ++s) {
      const float t = static_cast<float>(s) / static_cast<float>(subs);
      outHeights.push_back(evalSegment(i, t));
    }
  }
  if (!closed) {
    outHeights.push_back(evalSegment(n - 2, 1.0f));
  }
}

}  // namespace iota::dsp
