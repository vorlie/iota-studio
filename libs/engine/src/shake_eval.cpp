#include "iota/engine/shake_eval.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

namespace iota::engine {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr float kPiF = 3.14159265358979323846f;

bool asciiEqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

double hashUnit01(int64_t cell, uint32_t seed) noexcept {
  uint64_t z = static_cast<uint64_t>(cell) + 0x9e3779b97f4a7c15ULL +
               static_cast<uint64_t>(seed) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

double hashCell(int64_t cell, uint32_t seed) noexcept {
  return hashUnit01(cell, seed) * 2.0 - 1.0;
}

double smoothStep01(double edge0, double edge1, double x) noexcept {
  const double span = edge1 - edge0;
  if (std::abs(span) < 1e-12) {
    return (x >= edge1) ? 1.0 : 0.0;
  }
  const double t = std::clamp((x - edge0) / span, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

double smoothCurve1D(double x, uint32_t seed) noexcept {
  const double fl = std::floor(x);
  const int64_t i = static_cast<int64_t>(fl);
  const double t = x - fl;
  const double p0 = hashCell(i - 1, seed);
  const double p1 = hashCell(i + 0, seed);
  const double p2 = hashCell(i + 1, seed);
  const double p3 = hashCell(i + 2, seed);
  const double t2 = t * t;
  const double t3 = t2 * t;
  const double v = 0.5 * ((2.0 * p1) +
                          (-p0 + p2) * t +
                          (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                          (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
  return std::clamp(v, -1.0, 1.0);
}

double fbmShake1D(double t, int octaves, uint32_t seed) noexcept {
  const int count = std::clamp(octaves, 1, 10);
  double sum = 0.0;
  double amp = 1.0;
  double freq = 1.0;
  double weightSum = 0.0;
  for (int o = 0; o < count; ++o) {
    const double phaseShift = static_cast<double>(o) * 17.31;
    const uint32_t octSeed = seed ^ (static_cast<uint32_t>(o) * 0x9e3779b9u + 0x517cc1b7u);
    sum += amp * smoothCurve1D(t * freq + phaseShift, octSeed);
    weightSum += amp;
    amp *= 0.5;
    freq *= 2.0;
  }
  return (weightSum > 0.0) ? (sum / weightSum) : 0.0;
}

double jumpyCurve1D(double stepT,
                    float drift,
                    float centerBias,
                    uint32_t baseSeed,
                    uint32_t axisSeed) noexcept {
  const double fl = std::floor(stepT);
  const int64_t cell = static_cast<int64_t>(fl);
  const double frac = stepT - fl;
  const double cb = std::max(0.0, static_cast<double>(centerBias));
  const double d = std::clamp(static_cast<double>(drift), 0.0, 1.0);

  auto jumpTarget = [&](int64_t c) noexcept -> double {
    const double centerProb = cb / (1.0 + cb);
    if (cb > 1e-6 && hashUnit01(c, baseSeed ^ 0xCE7E4B1Au) < centerProb) {
      return 0.0;
    }
    const double raw = hashCell(c, axisSeed);
    return (cb > 1e-6) ? std::copysign(std::pow(std::abs(raw), 1.0 + 0.35 * cb), raw) : raw;
  };

  auto cellVal = [&](int64_t c, double f) noexcept -> double {
    const double base = jumpTarget(c);
    const double driftDelta =
        (d > 1e-6) ? (d * 0.45 * (f - 0.5) * hashCell(c, axisSeed ^ 0xD41F7001u)) : 0.0;
    return std::clamp(base + driftDelta, -1.0, 1.0);
  };

  if (frac < 0.04) {
    const double prevEnd = cellVal(cell - 1, 1.0);
    const double curStart = cellVal(cell, 0.04);
    const double alpha = smoothStep01(0.0, 0.04, frac);
    return prevEnd + (curStart - prevEnd) * alpha;
  }
  return cellVal(cell, frac);
}

double twitchPulse1D(double t, uint32_t seed) noexcept {
  const double n = smoothCurve1D(t * 2.0, seed);
  return std::copysign(std::pow(std::abs(n), 3.5) * 1.4, n);
}

double evalAxis(ShakeStyle style,
                double baseT,
                double globalFreq,
                int octaves,
                float drift,
                float centerBias,
                float randAmp,
                float randFreq,
                float waveAmp,
                float waveFreq,
                double wavePhaseTurns,
                float axisPhase,
                float twitchAmp,
                float twitchFreq,
                float twitchAxisScale,
                uint32_t baseSeed,
                uint32_t axisSeed) noexcept {
  const double axPhase = static_cast<double>(axisPhase);
  double r = 0.0;
  if (std::abs(randAmp) > 1e-6f) {
    const double randT = (baseT * globalFreq + axPhase) * std::max(0.0, static_cast<double>(randFreq));
    const double noise = (style == ShakeStyle::Jumpy)
                             ? jumpyCurve1D(randT, drift, centerBias, baseSeed, axisSeed)
                             : fbmShake1D(randT, octaves, axisSeed);
    r = static_cast<double>(randAmp) * noise;
  }
  const double w = (std::abs(waveAmp) > 1e-6f)
                       ? static_cast<double>(waveAmp) *
                             std::sin(kTwoPi * (baseT * globalFreq * std::max(0.0, static_cast<double>(waveFreq)) +
                                                wavePhaseTurns + axPhase))
                       : 0.0;
  const double tw = (std::abs(twitchAmp) > 1e-6f)
                        ? static_cast<double>(twitchAmp) * static_cast<double>(twitchAxisScale) *
                              twitchPulse1D(baseT * globalFreq * std::max(0.0, static_cast<double>(twitchFreq)),
                                            axisSeed ^ 0xA5A5F00Du)
                        : 0.0;
  return r + w + tw;
}

ShakePose evaluateChannelPose(const ShakeConfig& cfg,
                                double timeSec,
                                float chAmp,
                                float chPhase,
                                uint32_t chSalt) noexcept {
  ShakeConfig chCfg = cfg;
  chCfg.amplitude = std::max(0.0f, cfg.amplitude) * std::max(0.0f, chAmp);
  chCfg.phase = cfg.phase + chPhase;
  ShakePose p = evaluateCameraShake(chCfg, timeSec);

  if (cfg.rgbRandomness > 1e-5f && chCfg.amplitude > 1e-5f) {
    const uint32_t baseSeed =
        static_cast<uint32_t>(std::llround(static_cast<double>(chCfg.seed) * 1000.0));
    const double chBaseT = chCfg.still ? static_cast<double>(chCfg.phase)
                                       : (timeSec + static_cast<double>(chCfg.phase));
    const double rgbFreq = std::max(0.0, static_cast<double>(cfg.rgbFrequency));
    const int oct = std::clamp(cfg.octaves, 1, 10);
    const double scale =
        static_cast<double>(cfg.rgbRandomness) * static_cast<double>(chCfg.amplitude);
    const uint32_t rgbSeedBase = baseSeed ^ chSalt;

    const double xNoise =
        fbmShake1D(chBaseT * rgbFreq, oct, rgbSeedBase ^ 0x13572468u) *
        scale * static_cast<double>(std::max(cfg.xRandAmp, cfg.xWaveAmp));
    const double yNoise =
        fbmShake1D(chBaseT * rgbFreq, oct, rgbSeedBase ^ 0x24681357u) *
        scale * static_cast<double>(std::max(cfg.yRandAmp, cfg.yWaveAmp));
    const double zNoise =
        fbmShake1D(chBaseT * rgbFreq, oct, rgbSeedBase ^ 0x9B3C7E11u) *
        scale * static_cast<double>(std::max(cfg.zRandAmp, cfg.zWaveAmp));
    const double tiltNoiseDeg =
        fbmShake1D(chBaseT * rgbFreq, oct, rgbSeedBase ^ 0xD1E2F3A4u) *
        scale * static_cast<double>(std::max(cfg.tiltRandAmp, cfg.tiltWaveAmp));

    p.offsetX += static_cast<float>(xNoise);
    p.offsetY += static_cast<float>(yNoise);
    p.zDist = std::clamp(p.zDist * std::pow(2.0f, static_cast<float>(zNoise)), 0.10f, 8.0f);
    p.tiltRad += static_cast<float>(tiltNoiseDeg) * (kPiF / 180.0f);
  }
  return p;
}

}  // namespace

ShakeWrapMode shakeWrapModeFromString(std::string_view s) noexcept {
  if (asciiEqualsIgnoreCase(s, "No") || asciiEqualsIgnoreCase(s, "Transparent")) {
    return ShakeWrapMode::No;
  }
  if (asciiEqualsIgnoreCase(s, "Tile") || asciiEqualsIgnoreCase(s, "Repeat")) {
    return ShakeWrapMode::Tile;
  }
  if (asciiEqualsIgnoreCase(s, "Clamp")) {
    return ShakeWrapMode::Clamp;
  }
  return ShakeWrapMode::Reflect;
}

ShakeStyle shakeStyleFromString(std::string_view s) noexcept {
  if (asciiEqualsIgnoreCase(s, "Twitchy")) {
    return ShakeStyle::Twitchy;
  }
  if (asciiEqualsIgnoreCase(s, "Jumpy")) {
    return ShakeStyle::Jumpy;
  }
  return ShakeStyle::Normal;
}

ShakePose evaluateCameraShake(const ShakeConfig& cfg, double timeSec) noexcept {
  const uint32_t baseSeed = static_cast<uint32_t>(std::llround(static_cast<double>(cfg.seed) * 1000.0));
  const double baseT = cfg.still ? static_cast<double>(cfg.phase)
                                 : (timeSec + static_cast<double>(cfg.phase));
  const double globalFreq = std::max(0.0, static_cast<double>(cfg.frequency));
  const float masterAmp = std::max(0.0f, cfg.amplitude);
  const int oct = std::clamp(cfg.octaves, 1, 10);

  double xVal = evalAxis(cfg.style, baseT, globalFreq, oct, cfg.drift, cfg.centerBias,
                         cfg.xRandAmp, cfg.xRandFreq,
                         cfg.xWaveAmp, cfg.xWaveFreq, 0.0, cfg.xPhase,
                         cfg.twitchAmp, cfg.twitchFreq, 0.25f,
                         baseSeed, baseSeed ^ 0x13572468u);
  double yVal = evalAxis(cfg.style, baseT, globalFreq, oct, cfg.drift, cfg.centerBias,
                         cfg.yRandAmp, cfg.yRandFreq,
                         cfg.yWaveAmp, cfg.yWaveFreq, 0.25, cfg.yPhase,
                         cfg.twitchAmp, cfg.twitchFreq, 0.20f,
                         baseSeed, baseSeed ^ 0x24681357u);
  double zVal = evalAxis(cfg.style, baseT, globalFreq, oct, cfg.drift, cfg.centerBias,
                         cfg.zRandAmp, cfg.zRandFreq,
                         cfg.zWaveAmp, cfg.zWaveFreq, 0.50, cfg.zPhase,
                         cfg.twitchAmp, cfg.twitchFreq, 0.15f,
                         baseSeed, baseSeed ^ 0x9B3C7E11u);
  double tiltDeg = evalAxis(cfg.style, baseT, globalFreq, oct, cfg.drift, cfg.centerBias,
                            cfg.tiltRandAmp, cfg.tiltRandFreq,
                            cfg.tiltWaveAmp, cfg.tiltWaveFreq, 0.15, cfg.tiltPhase,
                            cfg.twitchAmp, cfg.twitchFreq, 6.0f,
                            baseSeed, baseSeed ^ 0xD1E2F3A4u);

  if (cfg.style == ShakeStyle::Twitchy) {
    const double burstRate = std::max(0.01, static_cast<double>(cfg.twitchFreq));
    const double u = baseT * burstRate;
    const double flU = std::floor(u);
    const int64_t cycleIdx = static_cast<int64_t>(flU);
    const double localU = u - flU;
    const double stillFrac = std::clamp(static_cast<double>(cfg.stillness), 0.0, 1.0);
    double twitchEnv = 1.0;
    if (stillFrac >= 0.999) {
      twitchEnv = 0.0;
    } else if (stillFrac > 1e-4) {
      const double activeFrac = 1.0 - stillFrac;
      const double start = hashUnit01(cycleIdx, baseSeed ^ 0x51A7E213u) * stillFrac;
      if (localU < start || localU > start + activeFrac) {
        twitchEnv = 0.0;
      } else {
        const double s = (localU - start) / activeFrac;
        twitchEnv = smoothStep01(0.0, 0.15, s) * smoothStep01(1.0, 0.80, s);
      }
    }
    xVal *= twitchEnv;
    yVal *= twitchEnv;
    zVal *= twitchEnv;
    tiltDeg *= twitchEnv;
  }

  ShakePose pose;
  pose.offsetX = static_cast<float>(xVal) * masterAmp;
  pose.offsetY = static_cast<float>(yVal) * masterAmp;
  pose.zDist = std::clamp(cfg.zDist * std::pow(2.0f, static_cast<float>(zVal) * masterAmp), 0.10f, 8.0f);
  pose.tiltRad = static_cast<float>(tiltDeg) * masterAmp * (kPiF / 180.0f);
  return pose;
}

ShakeRgbPoses evaluateCameraShakeRgb(const ShakeConfig& cfg,
                                     double timeSec,
                                     float legacyRgbPhase) noexcept {
  const bool split =
      std::abs(cfg.redAmp - 1.0f) > 1e-4f ||
      std::abs(cfg.greenAmp - 1.0f) > 1e-4f ||
      std::abs(cfg.blueAmp - 1.0f) > 1e-4f ||
      std::abs(cfg.redPhase) > 1e-4f ||
      std::abs(cfg.greenPhase) > 1e-4f ||
      std::abs(cfg.bluePhase) > 1e-4f ||
      cfg.rgbRandomness > 1e-4f ||
      legacyRgbPhase > 1e-4f;

  if (!split) {
    const ShakePose p = evaluateCameraShake(cfg, timeSec);
    return {p, p, p, false};
  }

  const float legacyShiftSec = legacyRgbPhase * 0.012f;
  ShakeRgbPoses out;
  out.r = evaluateChannelPose(cfg, timeSec, cfg.redAmp, cfg.redPhase + legacyShiftSec, 0x41C64E6Du);
  out.g = evaluateChannelPose(cfg, timeSec, cfg.greenAmp, cfg.greenPhase, 0x6C078965u);
  out.b = evaluateChannelPose(cfg, timeSec, cfg.blueAmp, cfg.bluePhase - legacyShiftSec, 0x85EBCA6Bu);
  out.splitChannels = true;
  return out;
}

}  // namespace iota::engine
