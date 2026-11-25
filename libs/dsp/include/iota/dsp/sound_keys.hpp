// Copyright (C) 2026 Vorlie
// This file is part of Iota Studio.
//
// Iota Studio is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Iota Studio is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
// See the LICENSE file for the full license text.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "iota/model/scene.hpp"

#include <array>
#include <span>
#include <string_view>

namespace iota::dsp {

inline constexpr int kSoundKeysRangeCount = 3;

enum class SoundKeysRangeMode {
  Average,
  Peak,
  OnOff,
};

[[nodiscard]] std::string_view soundKeysRangeModeToString(SoundKeysRangeMode mode) noexcept;
[[nodiscard]] SoundKeysRangeMode soundKeysRangeModeFromString(std::string_view str) noexcept;

enum class SoundKeysFalloffMode {
  None,
  Linear,
  Exponential,
  Integrate,
};

[[nodiscard]] std::string_view soundKeysFalloffModeToString(SoundKeysFalloffMode mode) noexcept;
[[nodiscard]] SoundKeysFalloffMode soundKeysFalloffModeFromString(std::string_view str) noexcept;

struct SoundKeysRangeConfig {
  bool enabled = true;
  float minFreq = 0.0f;         // Normalized [0, 1] spectrum bar start
  float maxFreq = 0.25f;        // Normalized [0, 1] spectrum bar end
  float minAmplitude = 2.0f;    // Floor threshold in spectrum units
  float maxAmplitude = 50.0f;   // Ceiling normalized to output = 1.0
  SoundKeysRangeMode rangeMode = SoundKeysRangeMode::Peak;
  SoundKeysFalloffMode falloffMode = SoundKeysFalloffMode::Exponential;
  float attackMs = 15.0f;
  float falloffTimeSec = 0.35f;
  float outputMin = 0.0f;
  float outputMax = 1.0f;
  float integrateSpeed = 1.0f;
};

struct SoundKeysConfig {
  int audioProviderIndex = 1;
  bool useRawSpectrum = false;
  bool autoScaleCeiling = true;
  float displayCeiling = 50.0f;
  std::array<SoundKeysRangeConfig, kSoundKeysRangeCount> ranges = {{
      {true, 0.00f, 0.20f, 2.0f, 70.0f, SoundKeysRangeMode::Peak, SoundKeysFalloffMode::Exponential,
       15.0f, 0.35f, 0.0f, 1.0f, 1.0f},
      {true, 0.25f, 0.55f, 2.0f, 55.0f, SoundKeysRangeMode::Peak, SoundKeysFalloffMode::Exponential,
       15.0f, 0.35f, 0.0f, 1.0f, 1.0f},
      {true, 0.60f, 0.95f, 2.0f, 45.0f, SoundKeysRangeMode::Peak, SoundKeysFalloffMode::Exponential,
       15.0f, 0.35f, 0.0f, 1.0f, 1.0f},
  }};
};

struct SoundKeysResult {
  std::array<float, kSoundKeysRangeCount> rawNormalized{};
  std::array<float, kSoundKeysRangeCount> outputs{};
  float effectiveCeiling = 50.0f;
  float spectrumPeak = 0.0f;
};

/// Reads a SoundKeysConfig from a SoundKeys Element's property bag.
SoundKeysConfig soundKeysConfigFromElement(const iota::model::Element& el);

/// Frame-rate-independent Trapcode Sound Keys equivalent: evaluates up to 3 frequency/amplitude
/// spectrum selection bands with Average/Peak/OnOff modes and None/Linear/Exponential/Integrate falloff.
class SoundKeysProcessor {
 public:
  void reset() noexcept;

  const SoundKeysResult& process(std::span<const float> bars,
                                 const SoundKeysConfig& cfg,
                                 float dtSec) noexcept;

  [[nodiscard]] const SoundKeysResult& lastResult() const noexcept { return result_; }

 private:
  std::array<float, kSoundKeysRangeCount> env_{};
  std::array<float, kSoundKeysRangeCount> accum_{};
  float trackedCeiling_ = 0.0f;
  SoundKeysResult result_{};
};

}  // namespace iota::dsp
