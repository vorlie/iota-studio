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

double hashCell(int64_t cell, uint32_t seed) noexcept {
  uint64_t z = static_cast<uint64_t>(cell) + 0x9e3779b97f4a7c15ULL +
               static_cast<uint64_t>(seed) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  const double u = static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
  return u * 2.0 - 1.0;
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

double twitchPulse1D(double t, uint32_t seed) noexcept {
  const double n = smoothCurve1D(t * 2.0, seed);
  return std::copysign(std::pow(std::abs(n), 3.5) * 1.4, n);
}

double evalAxis(double baseT,
                double globalFreq,
                int octaves,
                float randAmp,
                float randFreq,
                float waveAmp,
                float waveFreq,
                double wavePhaseTurns,
                float twitchAmp,
                float twitchFreq,
                float twitchAxisScale,
                uint32_t axisSeed) noexcept {
  const double r = (std::abs(randAmp) > 1e-6f)
                       ? static_cast<double>(randAmp) *
                             fbmShake1D(baseT * globalFreq * std::max(0.0, static_cast<double>(randFreq)),
                                        octaves,
                                        axisSeed)
                       : 0.0;
  const double w = (std::abs(waveAmp) > 1e-6f)
                       ? static_cast<double>(waveAmp) *
                             std::sin(kTwoPi * (baseT * globalFreq * std::max(0.0, static_cast<double>(waveFreq)) +
                                                wavePhaseTurns))
                       : 0.0;
  const double tw = (std::abs(twitchAmp) > 1e-6f)
                        ? static_cast<double>(twitchAmp) * static_cast<double>(twitchAxisScale) *
                              twitchPulse1D(baseT * globalFreq * std::max(0.0, static_cast<double>(twitchFreq)),
                                            axisSeed ^ 0xA5A5F00Du)
                        : 0.0;
  return r + w + tw;
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

ShakePose evaluateCameraShake(const ShakeConfig& cfg, double timeSec) noexcept {
  const uint32_t baseSeed = static_cast<uint32_t>(std::llround(static_cast<double>(cfg.seed) * 1000.0));
  const double baseT = cfg.still ? static_cast<double>(cfg.phase)
                                 : (timeSec + static_cast<double>(cfg.phase));
  const double globalFreq = std::max(0.0, static_cast<double>(cfg.frequency));
  const float masterAmp = std::max(0.0f, cfg.amplitude);
  const int oct = std::clamp(cfg.octaves, 1, 10);

  const double xVal = evalAxis(baseT, globalFreq, oct,
                               cfg.xRandAmp, cfg.xRandFreq,
                               cfg.xWaveAmp, cfg.xWaveFreq, 0.0,
                               cfg.twitchAmp, cfg.twitchFreq, 0.25f,
                               baseSeed ^ 0x13572468u);
  const double yVal = evalAxis(baseT, globalFreq, oct,
                               cfg.yRandAmp, cfg.yRandFreq,
                               cfg.yWaveAmp, cfg.yWaveFreq, 0.25,
                               cfg.twitchAmp, cfg.twitchFreq, 0.20f,
                               baseSeed ^ 0x24681357u);
  const double zVal = evalAxis(baseT, globalFreq, oct,
                               cfg.zRandAmp, cfg.zRandFreq,
                               cfg.zWaveAmp, cfg.zWaveFreq, 0.50,
                               cfg.twitchAmp, cfg.twitchFreq, 0.15f,
                               baseSeed ^ 0x9B3C7E11u);
  const double tiltDeg = evalAxis(baseT, globalFreq, oct,
                                  cfg.tiltRandAmp, cfg.tiltRandFreq,
                                  cfg.tiltWaveAmp, cfg.tiltWaveFreq, 0.15,
                                  cfg.twitchAmp, cfg.twitchFreq, 6.0f,
                                  baseSeed ^ 0xD1E2F3A4u);

  ShakePose pose;
  pose.offsetX = static_cast<float>(xVal) * masterAmp;
  pose.offsetY = static_cast<float>(yVal) * masterAmp;
  pose.zDist = std::clamp(cfg.zDist * std::pow(2.0f, static_cast<float>(zVal) * masterAmp), 0.10f, 8.0f);
  pose.tiltRad = static_cast<float>(tiltDeg) * masterAmp * (kPiF / 180.0f);
  return pose;
}

}  // namespace iota::engine
