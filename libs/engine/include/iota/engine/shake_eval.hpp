#pragma once

#include <string_view>

namespace iota::engine {

enum class ShakeWrapMode {
  No = 0,
  Tile = 1,
  Reflect = 2,
  Clamp = 3,
};

[[nodiscard]] ShakeWrapMode shakeWrapModeFromString(std::string_view s) noexcept;

struct ShakeConfig {
  float amplitude = 0.35f;
  float frequency = 6.0f;
  float zDist = 1.0f;
  float phase = 0.0f;
  float seed = 0.0f;
  bool still = false;
  int octaves = 3;

  float xRandAmp = 0.18f;
  float xRandFreq = 1.0f;
  float xWaveAmp = 0.0f;
  float xWaveFreq = 0.5f;

  float yRandAmp = 0.14f;
  float yRandFreq = 1.0f;
  float yWaveAmp = 0.0f;
  float yWaveFreq = 0.5f;

  float zRandAmp = 0.08f;
  float zRandFreq = 1.0f;
  float zWaveAmp = 0.0f;
  float zWaveFreq = 0.5f;

  float tiltRandAmp = 3.5f;
  float tiltRandFreq = 1.0f;
  float tiltWaveAmp = 0.0f;
  float tiltWaveFreq = 0.5f;

  float twitchAmp = 0.0f;
  float twitchFreq = 1.5f;
};

struct ShakePose {
  float offsetX = 0.0f;  // normalized frame-width shift
  float offsetY = 0.0f;  // normalized frame-height shift
  float zDist = 1.0f;    // optical Z distance (<1 zooms in, >1 zooms out)
  float tiltRad = 0.0f;  // roll angle in radians
};

[[nodiscard]] ShakePose evaluateCameraShake(const ShakeConfig& cfg, double timeSec) noexcept;

}  // namespace iota::engine
