#include "iota/engine/layout.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace iota::engine {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

float anchorOriginAxis(float pos, float extent, float minDim, AnchorMode mode, bool uniformScale) noexcept {
  if (mode == AnchorMode::Unset) {
    return pos * extent;
  }
  float anchorFrac = 0.0f;
  if (mode == AnchorMode::Center) {
    anchorFrac = 0.5f;
  } else if (mode == AnchorMode::End) {
    anchorFrac = 1.0f;
  }
  const float span = uniformScale ? minDim : extent;
  return anchorFrac * extent + pos * span;
}

}  // namespace

AnchorMode anchorFromString(std::string_view s) noexcept {
  if (s == "start") return AnchorMode::Start;
  if (s == "center") return AnchorMode::Center;
  if (s == "end") return AnchorMode::End;
  return AnchorMode::Unset;
}

iota::core::RectF computeElementRect(const LayoutSpec& spec,
                                     int viewportWidth,
                                     int viewportHeight) noexcept {
  const float W = static_cast<float>(std::max(1, viewportWidth));
  const float H = static_cast<float>(std::max(1, viewportHeight));
  const float M = std::min(W, H);

  const float sx = std::max(0.0f, spec.scale.x + spec.measureScaleDelta.x);
  const float sy = std::max(0.0f, spec.scale.y + spec.measureScaleDelta.y);
  const float w = sx * (spec.scaleIsUniform ? M : W);
  const float h = sy * (spec.scaleIsUniform ? M : H);

  const float px = spec.position.x + spec.measurePosDelta.x;
  const float py = spec.position.y + spec.measurePosDelta.y;
  const float originX = anchorOriginAxis(px, W, M, spec.anchorX, spec.scaleIsUniform);
  const float originY = anchorOriginAxis(py, H, M, spec.anchorY, spec.scaleIsUniform);

  const float left = originX - spec.alignmentPosition.x * w;
  const float top = originY - spec.alignmentPosition.y * h;
  return {left, top, left + w, top + h};
}

iota::core::RectF adjustAspectRect(iota::core::RectF rect,
                                 int texWidth,
                                 int texHeight,
                                 bool keepAspect,
                                 bool cropToFit) noexcept {
  if (!keepAspect || texWidth <= 0 || texHeight <= 0) {
    return rect;
  }
  const float rw = rect.width();
  const float rh = rect.height();
  if (rw <= 0.0f || rh <= 0.0f) {
    return rect;
  }
  const float boxAspect = rw / rh;
  const float texAspect = static_cast<float>(texWidth) / static_cast<float>(texHeight);

  float newW = rw;
  float newH = rh;
  if (cropToFit) {
    if (texAspect > boxAspect) {
      newW = rh * texAspect;
    } else {
      newH = rw / texAspect;
    }
  } else {
    if (texAspect > boxAspect) {
      newH = rw / texAspect;
    } else {
      newW = rh * texAspect;
    }
  }

  const float cx = rect.centerX();
  const float cy = rect.centerY();
  return {cx - newW * 0.5f, cy - newH * 0.5f, cx + newW * 0.5f, cy + newH * 0.5f};
}

float computeTotalRotationRadians(const LayoutSpec& spec) noexcept {
  const float turns = (spec.rotationDeg / 360.0f) + spec.measureRotTurns;
  return turns * kTwoPi;
}

glm::mat4 makeOrthoProjView(int viewportWidth, int viewportHeight) noexcept {
  const float W = static_cast<float>(std::max(1, viewportWidth));
  const float H = static_cast<float>(std::max(1, viewportHeight));
  const glm::mat4 proj = glm::ortho(0.0f, W, H, 0.0f, 0.01f, 100.0f);
  const glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f));
  return proj * view;
}

glm::mat4 makeRotatedOrthoProjView(int viewportWidth,
                                   int viewportHeight,
                                   float centerX,
                                   float centerY,
                                   float angleRadians) noexcept {
  if (std::abs(angleRadians) < 1e-6f) {
    return makeOrthoProjView(viewportWidth, viewportHeight);
  }
  const float W = static_cast<float>(std::max(1, viewportWidth));
  const float H = static_cast<float>(std::max(1, viewportHeight));
  const glm::mat4 proj = glm::ortho(0.0f, W, H, 0.0f, 0.01f, 100.0f);
  glm::mat4 view(1.0f);
  view = glm::translate(view, glm::vec3(centerX, centerY, -10.0f));
  view = glm::rotate(view, angleRadians, glm::vec3(0.0f, 0.0f, 1.0f));
  view = glm::translate(view, glm::vec3(-centerX, -centerY, 0.0f));
  return proj * view;
}

}  // namespace iota::engine
