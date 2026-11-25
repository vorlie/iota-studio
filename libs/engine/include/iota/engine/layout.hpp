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

#include "iota/core/types.hpp"

#include <glm/mat4x4.hpp>

#include <string_view>

namespace iota::engine {

enum class AnchorMode {
  Unset = 0,
  Start = 1,
  Center = 2,
  End = 3,
};

[[nodiscard]] AnchorMode anchorFromString(std::string_view s) noexcept;

struct LayoutSpec {
  iota::core::Vec2 position{0.5f, 0.5f};
  iota::core::Vec2 measurePosDelta{};
  AnchorMode anchorX = AnchorMode::Unset;
  AnchorMode anchorY = AnchorMode::Unset;
  iota::core::Vec2 alignmentPosition{0.5f, 0.5f};
  bool scaleIsUniform = false;
  iota::core::Vec2 scale{1.0f, 1.0f};
  iota::core::Vec2 measureScaleDelta{};
  float rotationDeg = 0.0f;
  float measureRotTurns = 0.0f;
};

/// Computes pixel-space Y-down bounding rectangle from normalized layout parameters.
[[nodiscard]] iota::core::RectF computeElementRect(const LayoutSpec& spec,
                                                   int viewportWidth,
                                                   int viewportHeight) noexcept;

/// Adjusts bounding rect to preserve texture aspect ratio (fit or crop-to-fill).
[[nodiscard]] iota::core::RectF adjustAspectRect(iota::core::RectF rect,
                                               int texWidth,
                                               int texHeight,
                                               bool keepAspect,
                                               bool cropToFit) noexcept;

/// Computes total rotation in radians `(rotationDeg / 360 + measureRotTurns) * 2*pi`.
[[nodiscard]] float computeTotalRotationRadians(const LayoutSpec& spec) noexcept;

/// Builds 2D Y-down orthographic view-projection matrix `[0..W] x [H..0]`.
[[nodiscard]] glm::mat4 makeOrthoProjView(int viewportWidth, int viewportHeight) noexcept;

/// Builds 2D Y-down orthographic view-projection matrix rotated around `(centerX, centerY)`.
[[nodiscard]] glm::mat4 makeRotatedOrthoProjView(int viewportWidth,
                                                 int viewportHeight,
                                                 float centerX,
                                                 float centerY,
                                                 float angleRadians) noexcept;

}  // namespace iota::engine
