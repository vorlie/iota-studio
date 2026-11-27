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

#include <string_view>

namespace iota::engine {

enum class TwitchMode {
  Random = 0,
  Pulse = 1,
  Stutter = 2,
  Smooth = 3,
};

enum class TwitchSlideDirection {
  Both = 0,
  Horizontal = 1,
  Vertical = 2,
  Diagonal = 3,
};

enum class TwitchScaleDirection {
  ZoomIn = 0,
  ZoomOut = 1,
  Both = 2,
};

enum class TwitchLightBehavior {
  Brighten = 0,
  Darken = 1,
  Both = 2,
};

enum class TwitchColorMode {
  HueShift = 0,
  Tint = 1,
  Invert = 2,
  ChannelSwap = 3,
};

enum class TwitchBlurDirection {
  Directional = 0,
  RadialZoom = 1,
  Isotropic = 2,
};

[[nodiscard]] TwitchMode twitchModeFromString(std::string_view s) noexcept;
[[nodiscard]] TwitchSlideDirection twitchSlideDirectionFromString(std::string_view s) noexcept;
[[nodiscard]] TwitchScaleDirection twitchScaleDirectionFromString(std::string_view s) noexcept;
[[nodiscard]] TwitchLightBehavior twitchLightBehaviorFromString(std::string_view s) noexcept;
[[nodiscard]] TwitchColorMode twitchColorModeFromString(std::string_view s) noexcept;
[[nodiscard]] TwitchBlurDirection twitchBlurDirectionFromString(std::string_view s) noexcept;

struct TwitchConfig {
  float amount = 0.15f;
  float speed = 12.0f;
  TwitchMode mode = TwitchMode::Random;
  float threshold = 0.45f;
  float randomSeed = 0.0f;
  float timePhase = 0.0f;

  bool enableSlide = true;
  bool enableScale = true;
  bool enableLight = true;
  bool enableColor = false;
  bool enableBlur = true;
  bool enableSlice = false;

  float slideAmount = 0.22f;
  TwitchSlideDirection slideDirection = TwitchSlideDirection::Both;
  float slideRgbSplit = 0.35f;
  float slideRgbAngle = 0.0f;
  bool slideRgbAutoAngle = true;

  float scaleAmount = 0.18f;
  TwitchScaleDirection scaleDirection = TwitchScaleDirection::ZoomIn;
  float scaleRotation = 3.0f;

  float lightAmount = 0.55f;
  TwitchLightBehavior lightBehavior = TwitchLightBehavior::Brighten;

  float colorAmount = 0.55f;
  TwitchColorMode colorMode = TwitchColorMode::HueShift;

  float blurAmount = 0.35f;
  TwitchBlurDirection blurDirection = TwitchBlurDirection::Directional;
  float blurAngle = 0.0f;

  float sliceAmount = 0.25f;
  int sliceCount = 18;
  float sliceRgbSplit = 0.40f;
};

struct TwitchPose {
  float slideX = 0.0f;   // normalized horizontal shift
  float slideY = 0.0f;   // normalized vertical shift
  float scale = 1.0f;    // magnification factor (>1 zooms in, <1 zooms out)
  float tiltRad = 0.0f;  // roll angle in radians
};

struct TwitchFrameState {
  TwitchPose pose0{};
  TwitchPose pose1{};
  TwitchPose pose2{};

  float rgbSplitX = 0.0f;
  float rgbSplitY = 0.0f;

  float lightDelta = 0.0f;

  float colorMix = 0.0f;
  float colorHueShift = 0.0f;
  int colorSwapDir = 0;

  float blurRadius = 0.0f;
  float blurDirX = 1.0f;
  float blurDirY = 0.0f;

  float sliceStrength = 0.0f;
  float sliceSeed = 0.0f;
  float sliceRgbSplit = 0.0f;
};

[[nodiscard]] TwitchFrameState evaluateTwitch(const TwitchConfig& cfg,
                                              double timeSec,
                                              double halfShutterSec = 0.0) noexcept;

}  // namespace iota::engine
