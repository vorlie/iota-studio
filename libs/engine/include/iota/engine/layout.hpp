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
