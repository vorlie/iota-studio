#include "iota/dsp/sound_keys.hpp"

#include "iota/dsp/filters.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace iota::dsp {

std::string_view soundKeysRangeModeToString(SoundKeysRangeMode mode) noexcept {
  switch (mode) {
    case SoundKeysRangeMode::Average: return "Average";
    case SoundKeysRangeMode::OnOff: return "OnOff";
    case SoundKeysRangeMode::Peak:
    default: return "Peak";
  }
}

SoundKeysRangeMode soundKeysRangeModeFromString(std::string_view str) noexcept {
  if (str == "Average") return SoundKeysRangeMode::Average;
  if (str == "OnOff") return SoundKeysRangeMode::OnOff;
  return SoundKeysRangeMode::Peak;
}

std::string_view soundKeysFalloffModeToString(SoundKeysFalloffMode mode) noexcept {
  switch (mode) {
    case SoundKeysFalloffMode::None: return "None";
    case SoundKeysFalloffMode::Linear: return "Linear";
    case SoundKeysFalloffMode::Integrate: return "Integrate";
    case SoundKeysFalloffMode::Exponential:
    default: return "Exponential";
  }
}

SoundKeysFalloffMode soundKeysFalloffModeFromString(std::string_view str) noexcept {
  if (str == "None") return SoundKeysFalloffMode::None;
  if (str == "Linear") return SoundKeysFalloffMode::Linear;
  if (str == "Integrate") return SoundKeysFalloffMode::Integrate;
  return SoundKeysFalloffMode::Exponential;
}

SoundKeysConfig soundKeysConfigFromElement(const iota::model::Element& el) {
  SoundKeysConfig cfg;
  const auto& p = el.props;

  cfg.audioProviderIndex = std::clamp(p.getInt("audioProviderIndex", 1), 1, 4);
  cfg.useRawSpectrum = p.getBool("useRawSpectrum", false);
  cfg.autoScaleCeiling = p.getBool("autoScaleCeiling", true);
  cfg.displayCeiling = std::clamp(p.getFloat("displayCeiling", 50.0f), 5.0f, 10000.0f);

  for (int i = 0; i < kSoundKeysRangeCount; ++i) {
    auto& dst = cfg.ranges[static_cast<size_t>(i)];
    const std::string prefix = "range" + std::to_string(i + 1);

    dst.enabled = p.getBool(prefix + "Enabled", true);
    const float rawF0 = std::clamp(p.getFloat(prefix + "MinFreq", dst.minFreq), 0.0f, 1.0f);
    const float rawF1 = std::clamp(p.getFloat(prefix + "MaxFreq", dst.maxFreq), 0.0f, 1.0f);
    dst.minFreq = std::min(rawF0, rawF1);
    dst.maxFreq = std::max(rawF0, rawF1);

    const float rawA0 = std::max(0.0f, p.getFloat(prefix + "MinAmp", dst.minAmplitude));
    const float rawA1 = std::max(0.0f, p.getFloat(prefix + "MaxAmp", dst.maxAmplitude));
    dst.minAmplitude = std::min(rawA0, rawA1);
    dst.maxAmplitude = std::max(dst.minAmplitude + 0.01f, std::max(rawA0, rawA1));

    dst.rangeMode = soundKeysRangeModeFromString(p.getString(prefix + "Mode", "Peak"));
    dst.falloffMode = soundKeysFalloffModeFromString(p.getString(prefix + "Falloff", "Exponential"));
    dst.attackMs = std::clamp(p.getFloat(prefix + "AttackMs", dst.attackMs), 0.0f, 500.0f);
    dst.falloffTimeSec = std::clamp(p.getFloat(prefix + "FalloffSec", dst.falloffTimeSec), 0.01f, 10.0f);
    dst.outputMin = p.getFloat(prefix + "OutputMin", 0.0f);
    dst.outputMax = p.getFloat(prefix + "OutputMax", 1.0f);
    dst.integrateSpeed = std::clamp(p.getFloat(prefix + "IntegrateSpeed", 1.0f), 0.01f, 20.0f);
  }
  return cfg;
}

void SoundKeysProcessor::reset() noexcept {
  env_.fill(0.0f);
  accum_.fill(0.0f);
  trackedCeiling_ = 0.0f;
  result_ = {};
}

const SoundKeysResult& SoundKeysProcessor::process(std::span<const float> bars,
                                                   const SoundKeysConfig& cfg,
                                                   float dtSec) noexcept {
  const float dt = std::clamp(dtSec, 1.0f / 1000.0f, 0.1f);
  const int n = static_cast<int>(bars.size());

  float framePeak = 0.0f;
  for (int i = 0; i < n; ++i) {
    framePeak = std::max(framePeak, std::max(0.0f, bars[static_cast<size_t>(i)]));
  }
  result_.spectrumPeak = framePeak;

  const float floorCeil = std::max(5.0f, cfg.displayCeiling);
  if (cfg.autoScaleCeiling) {
    const float targetCeil = std::max(floorCeil, framePeak * 1.25f);
    if (trackedCeiling_ <= 0.0f || targetCeil >= trackedCeiling_) {
      trackedCeiling_ = targetCeil;
    } else {
      const float decayAlpha = expSmoothingAlphaMs(dt, 4000.0f);
      trackedCeiling_ = std::max(floorCeil, trackedCeiling_ + (targetCeil - trackedCeiling_) * decayAlpha);
    }
    result_.effectiveCeiling = trackedCeiling_;
  } else {
    trackedCeiling_ = floorCeil;
    result_.effectiveCeiling = floorCeil;
  }

  for (int r = 0; r < kSoundKeysRangeCount; ++r) {
    const size_t idx = static_cast<size_t>(r);
    const auto& rcfg = cfg.ranges[idx];

    if (!rcfg.enabled || n <= 0) {
      result_.rawNormalized[idx] = 0.0f;
      if (rcfg.falloffMode == SoundKeysFalloffMode::Linear) {
        env_[idx] = std::max(0.0f, env_[idx] - dt / std::max(0.01f, rcfg.falloffTimeSec));
      } else if (rcfg.falloffMode == SoundKeysFalloffMode::Exponential) {
        const float relAlpha = expSmoothingAlphaMs(dt, std::max(0.0f, rcfg.falloffTimeSec) * 1000.0f);
        env_[idx] = std::max(0.0f, env_[idx] * (1.0f - relAlpha));
      } else if (rcfg.falloffMode == SoundKeysFalloffMode::None) {
        env_[idx] = 0.0f;
      }
      result_.outputs[idx] = rcfg.outputMin;
      continue;
    }

    const float fLo = std::clamp(std::min(rcfg.minFreq, rcfg.maxFreq), 0.0f, 1.0f);
    const float fHi = std::clamp(std::max(rcfg.minFreq, rcfg.maxFreq), fLo, 1.0f);
    const float maxIdxF = static_cast<float>(n - 1);
    const int i0 = std::clamp(static_cast<int>(std::floor(fLo * maxIdxF + 0.5f)), 0, n - 1);
    const int i1 = std::clamp(static_cast<int>(std::floor(fHi * maxIdxF + 0.5f)), i0, n - 1);

    const float ampLo = std::max(0.0f, rcfg.minAmplitude);
    const float ampSpan = std::max(0.01f, rcfg.maxAmplitude - ampLo);

    float peak = 0.0f;
    double sum = 0.0;
    for (int i = i0; i <= i1; ++i) {
      const float v = std::max(0.0f, bars[static_cast<size_t>(i)]);
      peak = std::max(peak, v);
      sum += static_cast<double>(v);
    }
    const float mean = static_cast<float>(sum / static_cast<double>(i1 - i0 + 1));

    float rawNorm = 0.0f;
    switch (rcfg.rangeMode) {
      case SoundKeysRangeMode::Average:
        rawNorm = std::clamp((mean - ampLo) / ampSpan, 0.0f, 1.0f);
        break;
      case SoundKeysRangeMode::OnOff:
        rawNorm = (peak > ampLo) ? 1.0f : 0.0f;
        break;
      case SoundKeysRangeMode::Peak:
      default:
        rawNorm = std::clamp((peak - ampLo) / ampSpan, 0.0f, 1.0f);
        break;
    }
    result_.rawNormalized[idx] = rawNorm;

    const float outSpan = rcfg.outputMax - rcfg.outputMin;
    switch (rcfg.falloffMode) {
      case SoundKeysFalloffMode::None:
        env_[idx] = rawNorm;
        result_.outputs[idx] = rcfg.outputMin + env_[idx] * outSpan;
        break;

      case SoundKeysFalloffMode::Linear:
        env_[idx] = (rawNorm >= env_[idx])
                        ? rawNorm
                        : std::max(rawNorm, env_[idx] - dt / std::max(0.01f, rcfg.falloffTimeSec));
        result_.outputs[idx] = rcfg.outputMin + env_[idx] * outSpan;
        break;

      case SoundKeysFalloffMode::Integrate:
        accum_[idx] += rawNorm * dt * std::max(0.01f, rcfg.integrateSpeed);
        env_[idx] = rawNorm;
        result_.outputs[idx] = rcfg.outputMin + accum_[idx] * outSpan;
        break;

      case SoundKeysFalloffMode::Exponential:
      default: {
        const float alpha =
            (rawNorm >= env_[idx])
                ? expSmoothingAlphaMs(dt, std::max(0.0f, rcfg.attackMs))
                : expSmoothingAlphaMs(dt, std::max(0.0f, rcfg.falloffTimeSec) * 1000.0f);
        env_[idx] = std::clamp(env_[idx] + (rawNorm - env_[idx]) * alpha, 0.0f, 1.0f);
        result_.outputs[idx] = rcfg.outputMin + env_[idx] * outSpan;
        break;
      }
    }
  }

  return result_;
}

}  // namespace iota::dsp
