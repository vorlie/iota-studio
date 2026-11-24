#include "iota/engine/scene_renderer.hpp"

#include "iota/core/procedural_assets.hpp"
#include "iota/core/types.hpp"
#include "iota/core/utf8_path.hpp"
#include "iota/dsp/filters.hpp"
#include "iota/engine/builtin_shaders.hpp"
#include "iota/engine/layout.hpp"
#include "iota/engine/particle_system.hpp"
#include "iota/engine/shake_eval.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iota::engine {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

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

const iota::model::PropertyEntry* findPropCI(const iota::model::PropertyBag& bag,
                                             std::string_view name) noexcept {
  if (const auto* exact = bag.find(name)) {
    return exact;
  }
  for (const auto& [k, v] : bag.items()) {
    if (asciiEqualsIgnoreCase(k, name)) {
      return &v;
    }
  }
  return nullptr;
}

std::string getStringCI(const iota::model::PropertyBag& bag,
                        std::string_view name,
                        std::string_view fallback = {}) {
  if (const auto* e = findPropCI(bag, name)) {
    if (const auto* s = std::get_if<std::string>(&e->value)) {
      return *s;
    }
  }
  return std::string(fallback);
}

bool getBoolCI(const iota::model::PropertyBag& bag, std::string_view name, bool fallback = false) {
  if (const auto* e = findPropCI(bag, name)) {
    if (const auto* b = std::get_if<bool>(&e->value)) return *b;
    if (const auto* iv = std::get_if<int64_t>(&e->value)) return *iv != 0;
    if (const auto* dv = std::get_if<double>(&e->value)) return *dv != 0.0;
    if (const auto* sv = std::get_if<std::string>(&e->value)) return *sv == "1" || *sv == "true";
  }
  return fallback;
}

int getIntCI(const iota::model::PropertyBag& bag, std::string_view name, int fallback = 0) {
  if (const auto* e = findPropCI(bag, name)) {
    if (const auto* iv = std::get_if<int64_t>(&e->value)) return static_cast<int>(*iv);
    if (const auto* dv = std::get_if<double>(&e->value)) return static_cast<int>(std::llround(*dv));
    if (const auto* b = std::get_if<bool>(&e->value)) return *b ? 1 : 0;
  }
  return fallback;
}

float getFloatCI(const iota::model::PropertyBag& bag, std::string_view name, float fallback = 0.0f) {
  if (const auto* e = findPropCI(bag, name)) {
    if (const auto* dv = std::get_if<double>(&e->value)) return static_cast<float>(*dv);
    if (const auto* iv = std::get_if<int64_t>(&e->value)) return static_cast<float>(*iv);
    if (const auto* b = std::get_if<bool>(&e->value)) return *b ? 1.0f : 0.0f;
  }
  return fallback;
}

iota::core::ColorRgba getColorArgbCI(const iota::model::PropertyBag& bag,
                                     std::string_view name,
                                     iota::core::ColorRgba fallback) {
  if (const auto* e = findPropCI(bag, name)) {
    if (const auto* iv = std::get_if<int64_t>(&e->value)) {
      return iota::core::ColorRgba::fromArgbInt32(static_cast<int32_t>(*iv));
    }
    if (const auto* dv = std::get_if<double>(&e->value)) {
      return iota::core::ColorRgba::fromArgbInt32(static_cast<int32_t>(*dv));
    }
  }
  return fallback;
}

iota::core::ColorHslaNorm getColorHslaCI(const iota::model::PropertyBag& bag,
                                         std::string_view name,
                                         iota::core::ColorHslaNorm fallback) {
  if (const auto* e = findPropCI(bag, name)) {
    if (const auto* sv = std::get_if<std::string>(&e->value)) {
      if (auto parsed = iota::core::parseHsla4f(*sv)) {
        return *parsed;
      }
    }
  }
  return fallback;
}

float evalKeyframeOverride(const iota::model::Project& project,
                           int compIdx,
                           int elemIdx,
                           std::string_view propName,
                           double timeSec,
                           float baseValue) {
  for (const auto& track : project.desktopTracks) {
    if (track.compositionIndex == compIdx &&
        track.elementIndex == elemIdx &&
        asciiEqualsIgnoreCase(track.propertyPath, propName) &&
        !track.keyframes.empty()) {
      const auto& kfs = track.keyframes;
      if (timeSec <= kfs.front().timeSec) return kfs.front().value;
      if (timeSec >= kfs.back().timeSec) return kfs.back().value;
      for (size_t i = 0; i + 1 < kfs.size(); ++i) {
        if (timeSec >= kfs[i].timeSec && timeSec <= kfs[i + 1].timeSec) {
          const double span = std::max(1e-6, kfs[i + 1].timeSec - kfs[i].timeSec);
          const float t = static_cast<float>((timeSec - kfs[i].timeSec) / span);
          return kfs[i].value + (kfs[i + 1].value - kfs[i].value) * t;
        }
      }
    }
  }
  return baseValue;
}

LayoutSpec extractLayoutSpec(const iota::model::Project& project,
                             int compIdx,
                             int elemIdx,
                             const iota::model::Element& el,
                             iota::dsp::MVarEvaluator& mvarEval,
                             const iota::dsp::MVarEvalContext& mctx) {
  LayoutSpec spec;
  const auto& p = el.props;
  const int rid = el.runtimeId();

  spec.position = p.getVec2("position", {0.5f, 0.5f});
  spec.measurePosDelta = mvarEval.evaluateVec2(
      iota::dsp::MVarEvaluator::makeKey(rid, "MeasurePos"),
      p.getMVar("MeasurePos"),
      mctx);

  spec.anchorX = anchorFromString(getStringCI(p, "anchorX", "unset"));
  spec.anchorY = anchorFromString(getStringCI(p, "anchorY", "unset"));
  spec.alignmentPosition = p.getVec2("alignmentPosition", {0.5f, 0.5f});
  spec.scaleIsUniform = getBoolCI(p, "scaleIsUniform", false);
  spec.scale = p.getVec2("scale", {1.0f, 1.0f});
  spec.measureScaleDelta = mvarEval.evaluateVec2(
      iota::dsp::MVarEvaluator::makeKey(rid, "measureScale"),
      p.getMVar("measureScale"),
      mctx);

  const float baseRot = getFloatCI(p, "rotation", 0.0f);
  spec.rotationDeg = evalKeyframeOverride(project, compIdx, elemIdx, "rotation", mctx.trackPositionSec, baseRot);
  spec.measureRotTurns = mvarEval.evaluateScalar(
      iota::dsp::MVarEvaluator::makeKey(rid, "measureRot"),
      p.getMVar("measureRot"),
      mctx);

  // Allow position/scale keyframe overrides if user keyed them
  spec.position.x = evalKeyframeOverride(project, compIdx, elemIdx, "position.x", mctx.trackPositionSec, spec.position.x);
  spec.position.y = evalKeyframeOverride(project, compIdx, elemIdx, "position.y", mctx.trackPositionSec, spec.position.y);
  spec.scale.x = evalKeyframeOverride(project, compIdx, elemIdx, "scale.x", mctx.trackPositionSec, spec.scale.x);
  spec.scale.y = evalKeyframeOverride(project, compIdx, elemIdx, "scale.y", mctx.trackPositionSec, spec.scale.y);
  return spec;
}

std::optional<int> parseCompositionRef(std::string_view uri) noexcept {
  constexpr std::string_view prefix = "composition:";
  if (uri.size() <= prefix.size() || uri.substr(0, prefix.size()) != prefix) {
    return std::nullopt;
  }
  int idx = 0;
  for (char c : uri.substr(prefix.size())) {
    if (c < '0' || c > '9') return std::nullopt;
    idx = idx * 10 + (c - '0');
  }
  return idx;
}

void computeMaskUniforms(std::string_view mode,
                         float& addA,
                         float& mulA,
                         float& addL,
                         float& mulL) noexcept {
  if (mode == "TransparencyAndBlacks") {
    addA = 0.0f; mulA = 1.0f; addL = 0.0f; mulL = 1.0f;
  } else if (mode == "TransparencyAndWhites") {
    addA = 0.0f; mulA = 1.0f; addL = 1.0f; mulL = -1.0f;
  } else if (mode == "InvertedTransparency") {
    addA = 1.0f; mulA = -1.0f; addL = 1.0f; mulL = 0.0f;
  } else {
    // Transparency (default)
    addA = 0.0f; mulA = 1.0f; addL = 1.0f; mulL = 0.0f;
  }
}

void appendQuadVertices(iota::core::RectF r,
                        uint32_t abgr,
                        bool flipY,
                        std::vector<iota::rhi::VertexPosColUv>& verts,
                        std::vector<uint32_t>& indices,
                        float u0 = 0.0f,
                        float u1 = 1.0f) {
  const float vTop = flipY ? 1.0f : 0.0f;
  const float vBot = flipY ? 0.0f : 1.0f;
  const uint32_t base = static_cast<uint32_t>(verts.size());
  verts.push_back({r.left, r.top, 0.0f, abgr, u0, vTop});
  verts.push_back({r.right, r.top, 0.0f, abgr, u1, vTop});
  verts.push_back({r.right, r.bottom, 0.0f, abgr, u1, vBot});
  verts.push_back({r.left, r.bottom, 0.0f, abgr, u0, vBot});
  indices.push_back(base + 0);
  indices.push_back(base + 1);
  indices.push_back(base + 2);
  indices.push_back(base + 0);
  indices.push_back(base + 2);
  indices.push_back(base + 3);
}

void appendPolygonFanVertices(iota::core::RectF r,
                              int sides,
                              float radiusScale,
                              float gapNorm,
                              uint32_t abgr,
                              bool flipY,
                              std::vector<iota::rhi::VertexPosColUv>& verts,
                              std::vector<uint32_t>& indices) {
  const int n = std::clamp(sides, 3, 128);
  const float cx = r.centerX();
  const float cy = r.centerY();
  const float rx = r.width() * 0.5f * std::max(0.05f, radiusScale);
  const float ry = r.height() * 0.5f * std::max(0.05f, radiusScale);
  const float startAngle = -kPi * 0.5f + gapNorm * kPi;
  const float sweep = (1.0f - std::clamp(gapNorm, 0.0f, 0.95f)) * kTwoPi;

  auto toUv = [&](float x, float y) -> iota::core::Vec2 {
    const float u = (r.width() > 1e-4f) ? ((x - r.left) / r.width()) : 0.5f;
    float v = (r.height() > 1e-4f) ? ((y - r.top) / r.height()) : 0.5f;
    if (flipY) v = 1.0f - v;
    return {u, v};
  };

  const uint32_t centerIdx = static_cast<uint32_t>(verts.size());
  const auto centerUv = toUv(cx, cy);
  verts.push_back({cx, cy, 0.0f, abgr, centerUv.x, centerUv.y});

  const int ringCount = (gapNorm > 1e-4f) ? (n + 1) : n;
  for (int i = 0; i < ringCount; ++i) {
    const float frac = (gapNorm > 1e-4f)
                           ? (static_cast<float>(i) / static_cast<float>(n))
                           : (static_cast<float>(i) / static_cast<float>(n));
    const float ang = startAngle + frac * sweep;
    const float vx = cx + std::cos(ang) * rx;
    const float vy = cy + std::sin(ang) * ry;
    const auto uv = toUv(vx, vy);
    verts.push_back({vx, vy, 0.0f, abgr, uv.x, uv.y});
  }

  const int triCount = (gapNorm > 1e-4f) ? n : n;
  for (int i = 0; i < triCount; ++i) {
    const uint32_t i0 = centerIdx + 1 + static_cast<uint32_t>(i);
    const uint32_t i1 = (gapNorm > 1e-4f)
                            ? (centerIdx + 1 + static_cast<uint32_t>(i + 1))
                            : (centerIdx + 1 + static_cast<uint32_t>((i + 1) % n));
    indices.push_back(centerIdx);
    indices.push_back(i0);
    indices.push_back(i1);
  }
}

struct ShapePoint {
  float x = 0.0f;
  float y = 0.0f;
  float nx = 0.0f;
  float ny = -1.0f;
  float tangentAngle = 0.0f;
};

void buildShapePathPoints(const iota::model::PropertyEntry* shapeProp,
                          iota::core::RectF rect,
                          int count,
                          std::vector<ShapePoint>& outPoints) {
  outPoints.clear();
  if (count <= 0) return;

  const std::string kind = shapeProp
                               ? std::get<std::string>(std::get_if<std::string>(&shapeProp->value)
                                                           ? shapeProp->value
                                                           : iota::model::PropertyValue{std::string("Circle")})
                               : std::string("Circle");

  const float cx = rect.centerX();
  const float cy = rect.centerY();
  const float minDim = std::min(rect.width(), rect.height());

  auto childFloat = [&](std::string_view k, float def) -> float {
    if (shapeProp) {
      if (const auto* c = shapeProp->findChild(k)) {
        if (const auto* dv = std::get_if<double>(&c->value)) return static_cast<float>(*dv);
        if (const auto* iv = std::get_if<int64_t>(&c->value)) return static_cast<float>(*iv);
      }
    }
    return def;
  };
  auto childInt = [&](std::string_view k, int def) -> int {
    if (shapeProp) {
      if (const auto* c = shapeProp->findChild(k)) {
        if (const auto* iv = std::get_if<int64_t>(&c->value)) return static_cast<int>(*iv);
        if (const auto* dv = std::get_if<double>(&c->value)) return static_cast<int>(std::llround(*dv));
      }
    }
    return def;
  };

  if (kind == "HorizontalLine") {
    for (int i = 0; i < count; ++i) {
      const float t = (count > 1) ? (static_cast<float>(i) / static_cast<float>(count - 1)) : 0.5f;
      const float x = rect.left + t * rect.width();
      const float y = cy;
      outPoints.push_back({x, y, 0.0f, -1.0f, 0.0f});
    }
    return;
  }

  if (kind == "SidedPolygon") {
    const int sides = std::clamp(childInt("sides", 6), 3, 18);
    const float rad = minDim * 0.5f * std::max(0.05f, childFloat("radius", 0.7f));
    std::vector<iota::core::Vec2> verts(static_cast<size_t>(sides));
    for (int s = 0; s < sides; ++s) {
      const float a = -kPi * 0.5f + kTwoPi * (static_cast<float>(s) / static_cast<float>(sides));
      verts[static_cast<size_t>(s)] = {cx + std::cos(a) * rad, cy + std::sin(a) * rad};
    }
    for (int i = 0; i < count; ++i) {
      const float pos = (static_cast<float>(i) / static_cast<float>(count)) * static_cast<float>(sides);
      const int e = static_cast<int>(std::floor(pos)) % sides;
      const int eNext = (e + 1) % sides;
      const float localT = pos - std::floor(pos);
      const auto p0 = verts[static_cast<size_t>(e)];
      const auto p1 = verts[static_cast<size_t>(eNext)];
      const float x = p0.x + (p1.x - p0.x) * localT;
      const float y = p0.y + (p1.y - p0.y) * localT;
      const float ex = p1.x - p0.x;
      const float ey = p1.y - p0.y;
      const float len = std::max(1e-4f, std::hypot(ex, ey));
      // Outward normal for clockwise winding in Y-down screen space: (ey, -ex)
      outPoints.push_back({x, y, ey / len, -ex / len, std::atan2(ey, ex)});
    }
    return;
  }

  if (kind == "Letter" || kind == "Year") {
    // Stylized geometric triangle/A outline centered in rect
    const float rad = minDim * 0.45f * std::max(0.05f, childFloat("radius", 0.8f));
    const std::array<iota::core::Vec2, 6> poly = {{
        {cx - 0.65f * rad, cy + 0.85f * rad},
        {cx, cy - 0.90f * rad},
        {cx + 0.65f * rad, cy + 0.85f * rad},
        {cx + 0.35f * rad, cy + 0.25f * rad},
        {cx - 0.35f * rad, cy + 0.25f * rad},
        {cx - 0.65f * rad, cy + 0.85f * rad},
    }};
    const int segs = static_cast<int>(poly.size() - 1);
    for (int i = 0; i < count; ++i) {
      const float pos = (static_cast<float>(i) / static_cast<float>(count)) * static_cast<float>(segs);
      const int s = std::clamp(static_cast<int>(std::floor(pos)), 0, segs - 1);
      const float t = pos - static_cast<float>(s);
      const float x = poly[static_cast<size_t>(s)].x + (poly[static_cast<size_t>(s + 1)].x - poly[static_cast<size_t>(s)].x) * t;
      const float y = poly[static_cast<size_t>(s)].y + (poly[static_cast<size_t>(s + 1)].y - poly[static_cast<size_t>(s)].y) * t;
      const float dx = x - cx;
      const float dy = y - cy;
      const float dLen = std::max(1e-4f, std::hypot(dx, dy));
      outPoints.push_back({x, y, dx / dLen, dy / dLen, std::atan2(dy, dx) + kPi * 0.5f});
    }
    return;
  }

  // Circle (default)
  const float rad = minDim * 0.5f * std::max(0.05f, childFloat("radius", 0.7f));
  const float gap = std::clamp(childFloat("gap", 0.0f), 0.0f, 0.9f);
  const float startAngle = -kPi * 0.5f + gap * kPi;
  const float sweep = (1.0f - gap) * kTwoPi;

  for (int i = 0; i < count; ++i) {
    const float u = (count > 1 && gap > 1e-4f)
                        ? (static_cast<float>(i) / static_cast<float>(count - 1))
                        : (static_cast<float>(i) / static_cast<float>(count));
    const float ang = startAngle + u * sweep;
    const float nx = std::cos(ang);
    const float ny = std::sin(ang);
    outPoints.push_back({cx + nx * rad, cy + ny * rad, nx, ny, ang + kPi * 0.5f});
  }
}

bool hasGlslDecl(std::string_view code, bool isInput, std::string_view ident) {
  std::vector<std::string_view> toks;
  size_t i = 0;
  const size_t n = code.size();
  while (i < n) {
    const char c = code[i];
    if (c == '/' && i + 1 < n && code[i + 1] == '/') {
      i += 2;
      while (i < n && code[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && code[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(code[i] == '*' && code[i + 1] == '/')) ++i;
      i = std::min(i + 2, n);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      const size_t start = i;
      while (i < n && (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_')) {
        ++i;
      }
      toks.push_back(code.substr(start, i - start));
      continue;
    }
    if (c == '{' || c == '}' || c == '(' || c == ')' || c == ';') {
      toks.push_back(code.substr(i, 1));
    }
    ++i;
  }

  int braceDepth = 0;
  int parenDepth = 0;
  for (size_t k = 0; k < toks.size(); ++k) {
    const std::string_view tok = toks[k];
    if (tok == "{") {
      ++braceDepth;
      continue;
    }
    if (tok == "}") {
      braceDepth = std::max(0, braceDepth - 1);
      continue;
    }
    if (tok == "(") {
      ++parenDepth;
      continue;
    }
    if (tok == ")") {
      parenDepth = std::max(0, parenDepth - 1);
      continue;
    }
    if (braceDepth == 0 && parenDepth == 0 &&
        (isInput ? (tok == "in" || tok == "varying") : (tok == "out"))) {
      for (size_t j = k + 1; j < toks.size(); ++j) {
        const std::string_view tj = toks[j];
        if (tj == ";" || tj == "{" || tj == "}" || tj == "(" || tj == ")") break;
        if (tj == ident) return true;
      }
    }
  }
  return false;
}

std::string adaptCustomGlslTo450(std::string_view src, bool isFragment) {
  if (src.find("#version") != std::string_view::npos) {
    return std::string(src);
  }
  std::string out = "#version 450 core\n";
  if (!isFragment) {
    out += "layout(location = 0) in vec3 a_pos;\n";
    out += "layout(location = 1) in vec4 a_color;\n";
    out += "layout(location = 2) in vec2 a_uv;\n";
    out += "uniform mat4 u_projView;\n";
    out += "out vec4 v_color;\n";
    out += "out vec2 v_uv;\n";
    out += "void main() {\n";
    out += "  v_color = a_color;\n";
    out += "  v_uv = a_uv;\n";
    out += "  gl_Position = u_projView * vec4(a_pos, 1.0);\n";
    out += "}\n";
    return out;
  }

  std::string body(src);
  auto replaceAll = [&](std::string_view from, std::string_view to) {
    size_t pos = 0;
    while ((pos = body.find(from, pos)) != std::string::npos) {
      body.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  replaceAll("precision mediump float;", "");
  replaceAll("precision highp float;", "");
  replaceAll("precision lowp float;", "");
  replaceAll("varying ", "in ");
  replaceAll("texture2D(", "texture(");
  replaceAll("gl_FragColor", "iota_FragColor");

  if (body.find("iota_FragColor") != std::string::npos && !hasGlslDecl(body, false, "iota_FragColor")) {
    out += "out vec4 iota_FragColor;\n";
  }
  if (!hasGlslDecl(body, true, "v_uv") && !hasGlslDecl(body, true, "vTexCoord")) {
    out += "in vec2 v_uv;\n";
  }
  if (!hasGlslDecl(body, true, "v_color") && !hasGlslDecl(body, true, "vColor")) {
    out += "in vec4 v_color;\n";
  }
  out += body;
  return out;
}

}  // namespace

struct SceneRenderer::Impl {
  iota::rhi::IRenderDevice* device = nullptr;
  bool initialized = false;

  // Built-in shader programs
  iota::rhi::ShaderHandle shBuffer{};
  iota::rhi::ShaderHandle shFxLight{};
  iota::rhi::ShaderHandle shKawaseDown{};
  iota::rhi::ShaderHandle shKawaseUp{};
  iota::rhi::ShaderHandle shGaussianSep{};
  iota::rhi::ShaderHandle shRadialBlur{};
  iota::rhi::ShaderHandle shRgbSplit{};
  iota::rhi::ShaderHandle shMotionBlur{};
  iota::rhi::ShaderHandle shTemporalBlur{};
  iota::rhi::ShaderHandle shParticle{};
  iota::rhi::ShaderHandle shShake{};
  iota::rhi::ShaderHandle shBloomPrefilter{};
  iota::rhi::ShaderHandle shBloomDown{};
  iota::rhi::ShaderHandle shBloomUp{};
  iota::rhi::ShaderHandle shTonemap{};
  iota::rhi::ShaderHandle shFxaa{};
  struct CachedCustomShader {
    iota::rhi::ShaderHandle handle{};
    std::string error;
  };
  std::unordered_map<std::string, CachedCustomShader> customShaders;

  // Cached asset textures
  iota::rhi::TextureHandle texWhite{};
  iota::rhi::TextureHandle texBlack{};
  std::unordered_map<std::string, iota::rhi::TextureHandle> internalTexCache;
  std::unordered_map<std::string, iota::rhi::TextureHandle> localTexCache;

  FontAtlasCache fontCache;
  iota::dsp::MVarEvaluator mvarEval;
  iota::dsp::SoundKeysProcessor soundKeysProc;
  std::vector<iota::dsp::AudioAnalyzer> analyzers{1};
  std::unordered_map<int, ParticleSystem> particleSystems;

  struct MotionBlurPrevTransform {
    float centerX = 0.0f;
    float centerY = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
    float angleRad = 0.0f;
  };
  struct TemporalBlurHistory {
    iota::rhi::RenderTargetHandle rt[2]{};
    int readIdx = 0;
    bool initialized = false;
  };
  std::unordered_map<int, MotionBlurPrevTransform> motionBlurPrevTransforms;
  std::unordered_map<int, TemporalBlurHistory> temporalBlurHistories;

  // Render targets
  std::vector<iota::rhi::RenderTargetHandle> compTargets;  // Index matches Composition index [0..N-1]
  iota::rhi::RenderTargetHandle finalRt{};
  iota::rhi::RenderTargetHandle tonemapRt{};
  std::array<iota::rhi::RenderTargetHandle, 6> blurDownRts{};
  std::array<iota::rhi::RenderTargetHandle, 5> blurUpRts{};
  iota::rhi::RenderTargetHandle blurFullA{};
  iota::rhi::RenderTargetHandle blurFullB{};
  iota::rhi::RenderTargetHandle canvasSnapshotRt{};
  iota::rhi::RenderTargetHandle emitterSampleRt{};
  iota::rhi::RenderTargetHandle activeFinalRt{};
  iota::rhi::PixelFormat workingFormat = iota::rhi::PixelFormat::Rgba8;

  // Reusable CPU geometry / PCM scratch buffers
  std::vector<iota::rhi::VertexPosColUv> scratchVerts;
  std::vector<uint32_t> scratchIndices;
  std::vector<float> scratchPcm;
  std::vector<float> scratchLayerBars;
  std::vector<float> scratchInterpHeights;
  std::vector<ShapePoint> scratchShape;
  std::vector<uint8_t> emitterSampleRgba;
  std::vector<EmitterSamplePoint> scratchEmitterSamples;
  int width = 0;
  int height = 0;

  bool createBuiltinShaders(std::string* outError) {
    shBuffer = device->createShader(shaders::kBufferVert, shaders::kBufferFrag, outError);
    if (!shBuffer) return false;
    shFxLight = device->createShader(shaders::kBufferVert, shaders::kBufferFxLightFrag, outError);
    if (!shFxLight) return false;
    shKawaseDown = device->createShader(shaders::kBlurFullscreenVert, shaders::kKawaseDownFrag, outError);
    if (!shKawaseDown) return false;
    shKawaseUp = device->createShader(shaders::kBlurFullscreenVert, shaders::kKawaseUpFrag, outError);
    if (!shKawaseUp) return false;
    shGaussianSep = device->createShader(shaders::kBlurFullscreenVert, shaders::kGaussianSepFrag, outError);
    if (!shGaussianSep) return false;
    shRadialBlur = device->createShader(shaders::kBlurFullscreenVert, shaders::kRadialBlurFrag, outError);
    if (!shRadialBlur) return false;
    shRgbSplit = device->createShader(shaders::kBufferVert, shaders::kRgbSplitFrag, outError);
    if (!shRgbSplit) return false;
    shMotionBlur = device->createShader(shaders::kBufferVert, shaders::kMotionBlurFrag, outError);
    if (!shMotionBlur) return false;
    shTemporalBlur = device->createShader(shaders::kBlurFullscreenVert, shaders::kTemporalBlurFrag, outError);
    if (!shTemporalBlur) return false;
    shParticle = device->createShader(shaders::kParticleVert, shaders::kParticleFrag, outError);
    if (!shParticle) return false;
    shShake = device->createShader(shaders::kBufferVert, shaders::kShakeFrag, outError);
    if (!shShake) return false;
    shBloomPrefilter = device->createShader(shaders::kBlurFullscreenVert, shaders::kBloomPrefilterFrag, outError);
    if (!shBloomPrefilter) return false;
    shBloomDown = device->createShader(shaders::kBlurFullscreenVert, shaders::kBloomDownFrag, outError);
    if (!shBloomDown) return false;
    shBloomUp = device->createShader(shaders::kBlurFullscreenVert, shaders::kBloomUpFrag, outError);
    if (!shBloomUp) return false;
    shTonemap = device->createShader(shaders::kBlurFullscreenVert, shaders::kTonemapFrag, outError);
    if (!shTonemap) return false;
    shFxaa = device->createShader(shaders::kBlurFullscreenVert, shaders::kFxaaFrag, outError);
    if (!shFxaa) return false;

    for (auto sh : {shBuffer, shFxLight, shRgbSplit, shMotionBlur, shParticle, shShake}) {
      device->setUniformFloat(sh, "u_hdrGain", 1.0f);
    }
    return true;
  }

  iota::rhi::TextureHandle getInternalTexture(std::string_view uri) {
    if (uri.empty()) {
      uri = "internalres:placeholderart5";
    }
    std::string key(uri);
    auto it = internalTexCache.find(key);
    if (it != internalTexCache.end()) {
      return it->second;
    }

    auto img = iota::core::generateInternalResource(uri);
    if (!img) {
      img = iota::core::generateInternalResource("internalres:placeholderart5");
    }
    iota::rhi::TextureHandle h{};
    if (img && !img->pixels.empty()) {
      iota::rhi::TextureDesc td;
      td.width = img->width;
      td.height = img->height;
      td.rgbaPixels = img->pixels.data();
      td.filter = iota::rhi::TextureFilter::Linear;
      td.wrap = iota::rhi::TextureWrap::ClampToEdge;
      h = device->createTexture2D(td);
    }
    if (!h) h = texWhite;
    internalTexCache.emplace(std::move(key), h);
    return h;
  }

  struct ResolvedTexture {
    iota::rhi::TextureHandle tex{};
    int texW = 1;
    int texH = 1;
    bool isFbo = false;
  };

  ResolvedTexture resolveTextureUri(const iota::model::Project& project, std::string_view uri) {
    ResolvedTexture out;
    if (uri.empty()) {
      out.tex = getInternalTexture("internalres:placeholderart5");
      device->textureSize(out.tex, out.texW, out.texH);
      return out;
    }

    if (auto compIdx = parseCompositionRef(uri)) {
      const int idx = *compIdx;
      if (idx > 0 && idx < static_cast<int>(compTargets.size()) && compTargets[static_cast<size_t>(idx)]) {
        out.tex = device->renderTargetColorTexture(compTargets[static_cast<size_t>(idx)]);
        device->textureSize(out.tex, out.texW, out.texH);
        out.isFbo = true;
        return out;
      }
      out.tex = texBlack;
      device->textureSize(out.tex, out.texW, out.texH);
      return out;
    }

    if (uri.rfind("local:", 0) == 0) {
      const std::string assetName(uri.substr(6));
      auto cached = localTexCache.find(assetName);
      if (cached != localTexCache.end()) {
        out.tex = cached->second;
        device->textureSize(out.tex, out.texW, out.texH);
        return out;
      }
      auto mapIt = project.localAssets.find(assetName);
      if (mapIt != project.localAssets.end() && !mapIt->second.empty()) {
        int iw = 0, ih = 0, ch = 0;
        uint8_t* decoded = stbi_load_from_memory(mapIt->second.data(),
                                                 static_cast<int>(mapIt->second.size()),
                                                 &iw, &ih, &ch, 4);
        if (decoded != nullptr && iw > 0 && ih > 0) {
          iota::rhi::TextureDesc td;
          td.width = iw;
          td.height = ih;
          td.rgbaPixels = decoded;
          td.filter = iota::rhi::TextureFilter::Linear;
          td.wrap = iota::rhi::TextureWrap::ClampToEdge;
          iota::rhi::TextureHandle th = device->createTexture2D(td);
          stbi_image_free(decoded);
          if (th) {
            localTexCache.emplace(assetName, th);
            out.tex = th;
            out.texW = iw;
            out.texH = ih;
            return out;
          }
        } else if (decoded) {
          stbi_image_free(decoded);
        }
      }
    }

    if (uri.rfind("internalres:", 0) == 0) {
      out.tex = getInternalTexture(uri);
      device->textureSize(out.tex, out.texW, out.texH);
      return out;
    }

    // Desktop fallback: allow direct filesystem image path (wide Unicode-safe via pathFromUtf8)
    const std::filesystem::path diskPath = iota::core::pathFromUtf8(uri);
    if (iota::core::pathExistsNoThrow(diskPath)) {
      std::string key(uri);
      auto cached = localTexCache.find(key);
      if (cached != localTexCache.end()) {
        out.tex = cached->second;
        device->textureSize(out.tex, out.texW, out.texH);
        return out;
      }
      std::vector<uint8_t> imgBytes;
      {
        std::ifstream f(diskPath, std::ios::binary | std::ios::ate);
        if (f.is_open()) {
          const auto sz = f.tellg();
          if (sz > 0 &&
              static_cast<uint64_t>(sz) <= static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            imgBytes.resize(static_cast<size_t>(sz));
            f.seekg(0, std::ios::beg);
            if (!f.read(reinterpret_cast<char*>(imgBytes.data()), sz)) {
              imgBytes.clear();
            }
          }
        }
      }
      if (!imgBytes.empty()) {
        int iw = 0, ih = 0, ch = 0;
        uint8_t* decoded = stbi_load_from_memory(imgBytes.data(),
                                                 static_cast<int>(imgBytes.size()),
                                                 &iw,
                                                 &ih,
                                                 &ch,
                                                 4);
        if (decoded && iw > 0 && ih > 0) {
          iota::rhi::TextureDesc td;
          td.width = iw;
          td.height = ih;
          td.rgbaPixels = decoded;
          iota::rhi::TextureHandle th = device->createTexture2D(td);
          stbi_image_free(decoded);
          if (th) {
            localTexCache.emplace(std::move(key), th);
            out.tex = th;
            out.texW = iw;
            out.texH = ih;
            return out;
          }
        } else if (decoded) {
          stbi_image_free(decoded);
        }
      }
    }

    out.tex = getInternalTexture("internalres:placeholderart5");
    device->textureSize(out.tex, out.texW, out.texH);
    return out;
  }

  void ensureRenderTargets(int compCount, int w, int h, iota::rhi::PixelFormat desiredFormat) {
    const int newW = std::max(16, w);
    const int newH = std::max(16, h);
    const bool formatChanged = (desiredFormat != workingFormat);
    if (formatChanged) {
      for (auto rt : compTargets) device->destroyRenderTarget(rt);
      compTargets.clear();
      for (auto* rt : {&blurFullA, &blurFullB, &canvasSnapshotRt}) {
        if (*rt) {
          device->destroyRenderTarget(*rt);
          *rt = {};
        }
      }
      for (auto& rt : blurDownRts) {
        if (rt) {
          device->destroyRenderTarget(rt);
          rt = {};
        }
      }
      for (auto& rt : blurUpRts) {
        if (rt) {
          device->destroyRenderTarget(rt);
          rt = {};
        }
      }
      for (auto& [id, hist] : temporalBlurHistories) {
        for (int k = 0; k < 2; ++k) {
          if (hist.rt[k]) {
            device->destroyRenderTarget(hist.rt[k]);
            hist.rt[k] = {};
          }
        }
        hist.initialized = false;
      }
      workingFormat = desiredFormat;
    }

    const bool sizeChanged = (newW != width || newH != height);
    width = newW;
    height = newH;

    while (static_cast<int>(compTargets.size()) < compCount) {
      compTargets.push_back(device->createRenderTarget(width, height, iota::rhi::TextureFilter::Linear, workingFormat));
    }
    while (static_cast<int>(compTargets.size()) > compCount) {
      device->destroyRenderTarget(compTargets.back());
      compTargets.pop_back();
    }
    for (auto rt : compTargets) {
      device->resizeRenderTarget(rt, width, height);
    }

    auto ensureRt = [&](iota::rhi::RenderTargetHandle& rt, int rw, int rh, iota::rhi::PixelFormat fmt) {
      if (!rt) {
        rt = device->createRenderTarget(rw, rh, iota::rhi::TextureFilter::Linear, fmt);
      } else {
        device->resizeRenderTarget(rt, rw, rh);
      }
    };

    ensureRt(finalRt, width, height, iota::rhi::PixelFormat::Rgba8);
    ensureRt(tonemapRt, width, height, iota::rhi::PixelFormat::Rgba8);
    ensureRt(blurFullA, width, height, workingFormat);
    ensureRt(blurFullB, width, height, workingFormat);
    ensureRt(canvasSnapshotRt, width, height, workingFormat);
    if (!emitterSampleRt) {
      emitterSampleRt = device->createRenderTarget(64, 64, iota::rhi::TextureFilter::Linear, iota::rhi::PixelFormat::Rgba8);
    }

    int pw = width;
    int ph = height;
    for (size_t i = 0; i < blurDownRts.size(); ++i) {
      pw = std::max(2, pw / 2);
      ph = std::max(2, ph / 2);
      ensureRt(blurDownRts[i], pw, ph, workingFormat);
      if (i < blurUpRts.size()) {
        ensureRt(blurUpRts[i], pw, ph, workingFormat);
      }
    }

    if (sizeChanged && !formatChanged) {
      for (auto& [id, hist] : temporalBlurHistories) {
        for (int k = 0; k < 2; ++k) {
          if (hist.rt[k]) {
            device->resizeRenderTarget(hist.rt[k], width, height);
          }
        }
        hist.initialized = false;
      }
    }
  }

  std::vector<int> computeCompositionOrder(const iota::model::Project& project) const {
    const int n = static_cast<int>(project.compositions.size());
    std::vector<std::vector<int>> deps(static_cast<size_t>(n));
    for (int ci = 0; ci < n; ++ci) {
      for (const auto& el : project.compositions[static_cast<size_t>(ci)].elements) {
        if (el.type == iota::model::ElementType::BlurEffect) {
          const int srcIdx = getIntCI(el.props, "sourceCompositionIndex", 1);
          if (srcIdx > 0 && srcIdx < n && srcIdx != ci) {
            deps[static_cast<size_t>(ci)].push_back(srcIdx);
          }
        }
        for (const char* propName : {"customImage", "MaskImage", "targetImage", "emitterLayer"}) {
          const auto val = getStringCI(el.props, propName);
          if (auto ref = parseCompositionRef(val)) {
            if (*ref > 0 && *ref < n && *ref != ci) {
              deps[static_cast<size_t>(ci)].push_back(*ref);
            }
          }
        }
      }
    }

    std::vector<int> state(static_cast<size_t>(n), 0);  // 0=unvisited, 1=visiting, 2=done
    std::vector<int> order;
    auto dfs = [&](auto& self, int node) -> void {
      state[static_cast<size_t>(node)] = 1;
      for (int dep : deps[static_cast<size_t>(node)]) {
        if (state[static_cast<size_t>(dep)] == 0) {
          self(self, dep);
        }
      }
      state[static_cast<size_t>(node)] = 2;
      order.push_back(node);
    };

    for (int ci = 1; ci < n; ++ci) {
      if (state[static_cast<size_t>(ci)] == 0) {
        dfs(dfs, ci);
      }
    }
    if (n > 0) {
      order.push_back(0);
    }
    return order;
  }

  float evalElementHdrGain(const iota::model::Element& el,
                           const iota::dsp::MVarEvalContext& mctx) {
    const float baseGain = std::max(0.0f, getFloatCI(el.props, "hdrIntensity", 1.0f));
    float reactiveGain = 0.0f;
    if (el.props.contains("measureHdrIntensity")) {
      reactiveGain = std::max(0.0f, mvarEval.evaluateScalar(
          iota::dsp::MVarEvaluator::makeKey(el.runtimeId(), "measureHdrIntensity"),
          el.props.getMVar("measureHdrIntensity"),
          mctx));
    }
    return std::clamp(baseGain + reactiveGain, 0.0f, 32.0f);
  }

  void drawQuadWithBufferShader(iota::rhi::TextureHandle tex,
                                const glm::mat4& projView,
                                iota::core::RectF rect,
                                uint32_t abgr,
                                iota::core::BlendMode blend,
                                bool flipY,
                                RenderFrameStats* stats,
                                float hdrGain = 1.0f) {
    scratchVerts.clear();
    scratchIndices.clear();
    appendQuadVertices(rect, abgr, flipY, scratchVerts, scratchIndices);

    device->setBlendMode(blend);
    device->useShader(shBuffer);
    device->setUniformMat4(shBuffer, "u_projView", projView);
    device->bindTexture(0, tex);
    device->setUniformInt(shBuffer, "u_texture", 0);
    device->setUniformFloat(shBuffer, "u_hdrGain", hdrGain);
    device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
    if (stats) stats->drawCalls += 1;
  }

  iota::rhi::TextureHandle runBloomPipeline(iota::rhi::TextureHandle comp0Tex,
                                            float exposure,
                                            float threshold,
                                            float softKnee,
                                            float scatter,
                                            float anamorphic,
                                            RenderFrameStats* stats) {
    const auto& clipQuad = fullClipQuad();
    const auto& clipIdx = fullClipIdx();

    // Pass 0: Prefilter from full-res comp0Tex -> blurDownRts[0] (1/2 res)
    {
      int dstW = 2, dstH = 2;
      device->renderTargetSize(blurDownRts[0], dstW, dstH);
      device->bindRenderTarget(blurDownRts[0], dstW, dstH);
      device->setBlendMode(iota::core::BlendMode::Opaque);
      device->useShader(shBloomPrefilter);
      device->bindTexture(0, comp0Tex);
      device->setUniformInt(shBloomPrefilter, "u_texture", 0);
      device->setUniformVec2(shBloomPrefilter,
                             "u_texelSize",
                             1.0f / static_cast<float>(std::max(1, width)),
                             1.0f / static_cast<float>(std::max(1, height)));
      device->setUniformFloat(shBloomPrefilter, "u_exposure", exposure);
      device->setUniformFloat(shBloomPrefilter, "u_threshold", threshold);
      device->setUniformFloat(shBloomPrefilter, "u_softKnee", softKnee);
      device->setUniformFloat(shBloomPrefilter, "u_anamorphic", anamorphic);
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, clipQuad, clipIdx);
      if (stats) stats->drawCalls += 1;
    }

    // Passes 1..5: Progressive Jimenez 13-tap downsample blurDownRts[i-1] -> blurDownRts[i]
    for (size_t i = 1; i < blurDownRts.size(); ++i) {
      int srcW = 2, srcH = 2;
      int dstW = 2, dstH = 2;
      device->renderTargetSize(blurDownRts[i - 1], srcW, srcH);
      device->renderTargetSize(blurDownRts[i], dstW, dstH);
      device->bindRenderTarget(blurDownRts[i], dstW, dstH);
      device->setBlendMode(iota::core::BlendMode::Opaque);
      device->useShader(shBloomDown);
      device->bindTexture(0, device->renderTargetColorTexture(blurDownRts[i - 1]));
      device->setUniformInt(shBloomDown, "u_texture", 0);
      device->setUniformVec2(shBloomDown,
                             "u_texelSize",
                             1.0f / static_cast<float>(std::max(1, srcW)),
                             1.0f / static_cast<float>(std::max(1, srcH)));
      device->setUniformFloat(shBloomDown, "u_anamorphic", anamorphic);
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, clipQuad, clipIdx);
      if (stats) stats->drawCalls += 1;
    }

    // Upsample pyramid: i = 4 down to 0
    for (int i = static_cast<int>(blurUpRts.size()) - 1; i >= 0; --i) {
      const auto coarseRt =
          (i == static_cast<int>(blurUpRts.size()) - 1) ? blurDownRts[5] : blurUpRts[static_cast<size_t>(i + 1)];
      int coarseW = 2, coarseH = 2;
      int dstW = 2, dstH = 2;
      device->renderTargetSize(coarseRt, coarseW, coarseH);
      device->renderTargetSize(blurUpRts[static_cast<size_t>(i)], dstW, dstH);
      device->bindRenderTarget(blurUpRts[static_cast<size_t>(i)], dstW, dstH);
      device->setBlendMode(iota::core::BlendMode::Opaque);
      device->useShader(shBloomUp);
      device->bindTexture(0, device->renderTargetColorTexture(coarseRt));
      device->setUniformInt(shBloomUp, "u_coarseTex", 0);
      device->bindTexture(1, device->renderTargetColorTexture(blurDownRts[static_cast<size_t>(i)]));
      device->setUniformInt(shBloomUp, "u_fineTex", 1);
      device->setUniformVec2(shBloomUp,
                             "u_texelSize",
                             1.0f / static_cast<float>(std::max(1, coarseW)),
                             1.0f / static_cast<float>(std::max(1, coarseH)));
      device->setUniformFloat(shBloomUp, "u_scatter", scatter);
      device->setUniformFloat(shBloomUp, "u_anamorphic", anamorphic);
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, clipQuad, clipIdx);
      if (stats) stats->drawCalls += 1;
    }

    return device->renderTargetColorTexture(blurUpRts[0]);
  }

  static const std::array<iota::rhi::VertexPosColUv, 4>& fullClipQuad() noexcept {
    static const std::array<iota::rhi::VertexPosColUv, 4> kQuad = {{
        {-1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
        {+1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {+1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
        {-1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
    }};
    return kQuad;
  }

  static const std::array<uint32_t, 6>& fullClipIdx() noexcept {
    static const std::array<uint32_t, 6> kIdx = {0, 1, 2, 0, 2, 3};
    return kIdx;
  }

  static float wrapAngleDiffPi(float d) noexcept {
    while (d > kPi) d -= kTwoPi;
    while (d < -kPi) d += kTwoPi;
    return d;
  }

  iota::rhi::TextureHandle captureCanvasSnapshot(iota::rhi::RenderTargetHandle currentTargetRt,
                                                 RenderFrameStats* stats) {
    const iota::rhi::TextureHandle curTex = device->renderTargetColorTexture(currentTargetRt);
    device->bindRenderTarget(canvasSnapshotRt, width, height);
    drawQuadWithBufferShader(curTex,
                             makeOrthoProjView(width, height),
                             {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)},
                             0xFFFFFFFFu,
                             iota::core::BlendMode::Opaque,
                             true,
                             stats);
    return device->renderTargetColorTexture(canvasSnapshotRt);
  }

  std::pair<iota::rhi::TextureHandle, bool> runBlurPipeline(iota::rhi::TextureHandle srcTex,
                                                            bool srcIsFbo,
                                                            std::string_view blurType,
                                                            float radiusPx,
                                                            float angleDeg,
                                                            iota::core::Vec2 centerUv,
                                                            RenderFrameStats* stats) {
    if (!srcTex || radiusPx < 0.05f) {
      return {srcTex, srcIsFbo};
    }

    const auto& clipQuad = fullClipQuad();
    const auto& clipIdx = fullClipIdx();

    iota::rhi::TextureHandle workSrc = srcTex;
    bool workIsInFullB = false;
    if (!srcIsFbo) {
      device->bindRenderTarget(blurFullB, width, height);
      drawQuadWithBufferShader(srcTex,
                               makeOrthoProjView(width, height),
                               {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)},
                               0xFFFFFFFFu,
                               iota::core::BlendMode::Opaque,
                               false,
                               stats);
      workSrc = device->renderTargetColorTexture(blurFullB);
      workIsInFullB = true;
    }

    auto runSepPass = [&](iota::rhi::RenderTargetHandle dstRt,
                          iota::rhi::TextureHandle inTex,
                          float stepX,
                          float stepY,
                          float rPx,
                          int halfTaps) {
      int dstW = 2, dstH = 2;
      device->renderTargetSize(dstRt, dstW, dstH);
      device->bindRenderTarget(dstRt, dstW, dstH);
      device->setBlendMode(iota::core::BlendMode::Opaque);
      device->useShader(shGaussianSep);
      device->bindTexture(0, inTex);
      device->setUniformInt(shGaussianSep, "u_texture", 0);
      device->setUniformVec2(shGaussianSep, "u_stepUv", stepX, stepY);
      device->setUniformFloat(shGaussianSep, "u_radiusPx", rPx);
      device->setUniformInt(shGaussianSep, "u_halfTaps", halfTaps);
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, clipQuad, clipIdx);
      if (stats) stats->drawCalls += 1;
    };

    auto runKawaseDown = [&](iota::rhi::RenderTargetHandle dstRt,
                             iota::rhi::TextureHandle inTex,
                             float offset) {
      int dstW = 2, dstH = 2;
      device->renderTargetSize(dstRt, dstW, dstH);
      device->bindRenderTarget(dstRt, dstW, dstH);
      device->setBlendMode(iota::core::BlendMode::Opaque);
      device->useShader(shKawaseDown);
      device->bindTexture(0, inTex);
      device->setUniformInt(shKawaseDown, "u_texture", 0);
      device->setUniformVec2(shKawaseDown, "u_halfPixel",
                             0.5f / static_cast<float>(std::max(1, dstW)),
                             0.5f / static_cast<float>(std::max(1, dstH)));
      device->setUniformFloat(shKawaseDown, "u_offset", offset);
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, clipQuad, clipIdx);
      if (stats) stats->drawCalls += 1;
    };

    auto runKawaseUp = [&](iota::rhi::RenderTargetHandle dstRt,
                           iota::rhi::TextureHandle coarseTex,
                           iota::rhi::TextureHandle fineTex,
                           float offset,
                           float blend) {
      int dstW = 2, dstH = 2;
      device->renderTargetSize(dstRt, dstW, dstH);
      device->bindRenderTarget(dstRt, dstW, dstH);
      device->setBlendMode(iota::core::BlendMode::Opaque);
      device->useShader(shKawaseUp);
      device->bindTexture(0, coarseTex);
      device->setUniformInt(shKawaseUp, "u_texture", 0);
      device->bindTexture(1, fineTex);
      device->setUniformInt(shKawaseUp, "u_fineTexture", 1);
      device->setUniformVec2(shKawaseUp, "u_halfPixel",
                             0.5f / static_cast<float>(std::max(1, dstW)),
                             0.5f / static_cast<float>(std::max(1, dstH)));
      device->setUniformFloat(shKawaseUp, "u_offset", offset);
      device->setUniformFloat(shKawaseUp, "u_blend", blend);
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, clipQuad, clipIdx);
      if (stats) stats->drawCalls += 1;
    };

    if (asciiEqualsIgnoreCase(blurType, "Gaussian")) {
      if (radiusPx <= 18.0f) {
        const int taps = std::clamp(static_cast<int>(std::ceil(radiusPx)), 2, 18);
        const auto rtH = workIsInFullB ? blurFullA : blurFullB;
        const auto rtV = workIsInFullB ? blurFullB : blurFullA;
        runSepPass(rtH, workSrc, 1.0f / static_cast<float>(width), 0.0f, radiusPx, taps);
        runSepPass(rtV, device->renderTargetColorTexture(rtH), 0.0f, 1.0f / static_cast<float>(height), radiusPx, taps);
        return {device->renderTargetColorTexture(rtV), true};
      }

      const int dsLevels = (radiusPx > 48.0f) ? 2 : 1;
      iota::rhi::TextureHandle cur = workSrc;
      for (int i = 0; i < dsLevels; ++i) {
        runKawaseDown(blurDownRts[static_cast<size_t>(i)], cur, 1.0f);
        cur = device->renderTargetColorTexture(blurDownRts[static_cast<size_t>(i)]);
      }
      const size_t k = static_cast<size_t>(dsLevels - 1);
      int subW = 2, subH = 2;
      device->renderTargetSize(blurDownRts[k], subW, subH);
      const float scaledR = radiusPx / static_cast<float>(1 << dsLevels);
      const int taps = std::clamp(static_cast<int>(std::ceil(scaledR)), 3, 18);
      runSepPass(blurUpRts[k], device->renderTargetColorTexture(blurDownRts[k]),
                 1.0f / static_cast<float>(std::max(1, subW)), 0.0f, scaledR, taps);
      runSepPass(blurDownRts[k], device->renderTargetColorTexture(blurUpRts[k]),
                 0.0f, 1.0f / static_cast<float>(std::max(1, subH)), scaledR, taps);
      if (dsLevels == 2) {
        runKawaseUp(blurUpRts[0], device->renderTargetColorTexture(blurDownRts[1]),
                    device->renderTargetColorTexture(blurDownRts[1]), 1.0f, 1.0f);
        runKawaseUp(blurFullA, device->renderTargetColorTexture(blurUpRts[0]),
                    device->renderTargetColorTexture(blurUpRts[0]), 1.0f, 1.0f);
      } else {
        runKawaseUp(blurFullA, device->renderTargetColorTexture(blurDownRts[0]),
                    device->renderTargetColorTexture(blurDownRts[0]), 1.0f, 1.0f);
      }
      return {device->renderTargetColorTexture(blurFullA), true};
    }

    if (asciiEqualsIgnoreCase(blurType, "Directional")) {
      const float rad = angleDeg * kPi / 180.0f;
      const float stepX = std::cos(rad) / static_cast<float>(width);
      const float stepY = std::sin(rad) / static_cast<float>(height);
      const int taps = std::clamp(static_cast<int>(std::ceil(radiusPx)), 4, 28);
      runSepPass(blurFullA, workSrc, stepX, stepY, radiusPx, taps);
      return {device->renderTargetColorTexture(blurFullA), true};
    }

    if (asciiEqualsIgnoreCase(blurType, "Radial")) {
      const float minDim = static_cast<float>(std::max(1, std::min(width, height)));
      const float strength = std::clamp((radiusPx / minDim) * 8.0f, 0.0f, 0.85f);
      device->bindRenderTarget(blurFullA, width, height);
      device->setBlendMode(iota::core::BlendMode::Opaque);
      device->useShader(shRadialBlur);
      device->bindTexture(0, workSrc);
      device->setUniformInt(shRadialBlur, "u_texture", 0);
      device->setUniformVec2(shRadialBlur, "u_center",
                             std::clamp(centerUv.x, 0.0f, 1.0f),
                             std::clamp(1.0f - centerUv.y, 0.0f, 1.0f));
      device->setUniformFloat(shRadialBlur, "u_strength", strength);
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, clipQuad, clipIdx);
      if (stats) stats->drawCalls += 1;
      return {device->renderTargetColorTexture(blurFullA), true};
    }

    // DualKawase (default): continuous multi-octave downsample + tent upsample pyramid
    const float contLevel = std::clamp(std::log2(1.0f + radiusPx * 0.75f), 0.0f, 6.0f);
    const int deepest = std::clamp(static_cast<int>(std::ceil(contLevel)), 1, 6);
    const float frac = std::clamp(contLevel - static_cast<float>(deepest - 1), 0.0f, 1.0f);

    iota::rhi::TextureHandle curDown = workSrc;
    for (int i = 0; i < deepest; ++i) {
      runKawaseDown(blurDownRts[static_cast<size_t>(i)], curDown, 1.0f);
      curDown = device->renderTargetColorTexture(blurDownRts[static_cast<size_t>(i)]);
    }

    for (int lvl = deepest - 1; lvl >= 0; --lvl) {
      const auto dstRt = (lvl == 0) ? blurFullA : blurUpRts[static_cast<size_t>(lvl - 1)];
      const bool isDeepestPass = (lvl == deepest - 1);
      const iota::rhi::TextureHandle fineTex = isDeepestPass
          ? (lvl == 0 ? workSrc : device->renderTargetColorTexture(blurDownRts[static_cast<size_t>(lvl - 1)]))
          : curDown;
      const float blend = isDeepestPass ? frac : 1.0f;
      runKawaseUp(dstRt, curDown, fineTex, 1.0f, blend);
      curDown = device->renderTargetColorTexture(dstRt);
    }

    return {device->renderTargetColorTexture(blurFullA), true};
  }

  void renderImageElement(const iota::model::Project& project,
                          int compIdx,
                          int elemIdx,
                          const iota::model::Element& el,
                          iota::rhi::RenderTargetHandle currentTargetRt,
                          const iota::dsp::MVarEvalContext& mctx,
                          RenderFrameStats* stats) {
    const auto& p = el.props;
    const int rid = el.runtimeId();
    const std::string uri = getStringCI(p, "customImage", "internalres:placeholderart5");
    ResolvedTexture mainTex = resolveTextureUri(project, uri);

    if (getBoolCI(p, "blurEnabled", false)) {
      const int blurDivider = std::clamp(getIntCI(p, "blurDivider", 5), 0, 10);
      const int imgBlurRadius = std::clamp(getIntCI(p, "blurRadius", 7), 1, 25);
      const float resScale = static_cast<float>(std::min(width, height)) / 720.0f;
      const float radiusPx = static_cast<float>(imgBlurRadius) *
                             (0.6f + 0.35f * static_cast<float>(blurDivider)) * resScale;
      auto [bTex, bIsFbo] = runBlurPipeline(mainTex.tex,
                                            mainTex.isFbo,
                                            "DualKawase",
                                            radiusPx,
                                            0.0f,
                                            {0.5f, 0.5f},
                                            stats);
      device->bindRenderTarget(currentTargetRt, width, height);
      mainTex.tex = bTex;
      mainTex.isFbo = bIsFbo;
    }

    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    iota::core::RectF rect = computeElementRect(layout, width, height);
    rect = adjustAspectRect(rect,
                            mainTex.texW,
                            mainTex.texH,
                            getBoolCI(p, "keepAspectRatio", true),
                            getBoolCI(p, "keepAspectRatioAndCropToFit", false));
    const float angleRad = computeTotalRotationRadians(layout);
    const glm::mat4 projView = makeRotatedOrthoProjView(width, height, rect.centerX(), rect.centerY(), angleRad);

    // Color & saturation evaluation
    const auto cFrom = getColorHslaCI(p, "Color", {0.0f, 0.0f, 1.0f, 1.0f});
    const auto cTo = getColorHslaCI(p, "ColorTo", cFrom);
    const float colorBlend = std::clamp(mvarEval.evaluateScalar(
        iota::dsp::MVarEvaluator::makeKey(rid, "MeasureColorBlend"),
        p.getMVar("MeasureColorBlend"),
        mctx), 0.0f, 1.0f);
    const auto tintedHsla = iota::core::ColorHslaNorm::lerp(cFrom, cTo, colorBlend);
    const auto tintedRgba = tintedHsla.toRgba();

    const float sat = std::clamp(mvarEval.evaluateScalar(
        iota::dsp::MVarEvaluator::makeKey(rid, "saturation"),
        p.getMVar("saturation", iota::model::MVariableFloat::constant(1.0f)),
        mctx), 0.0f, 2.0f);

    const std::string maskUri = getStringCI(p, "MaskImage", "");
    const ResolvedTexture maskTex = maskUri.empty()
                                        ? ResolvedTexture{texWhite, 2, 2, mainTex.isFbo}
                                        : resolveTextureUri(project, maskUri);
    float maskAdd = 0.0f, maskMul = 1.0f, maskLAdd = 1.0f, maskLMul = 0.0f;
    computeMaskUniforms(getStringCI(p, "maskMode", "Transparency"), maskAdd, maskMul, maskLAdd, maskLMul);

    scratchVerts.clear();
    scratchIndices.clear();

    const auto* shapeProp = findPropCI(p, "Shape");
    const std::string shapeKind = shapeProp ? std::get<std::string>(
                                                  std::get_if<std::string>(&shapeProp->value)
                                                      ? shapeProp->value
                                                      : iota::model::PropertyValue{std::string("None")})
                                            : std::string("None");

    const uint32_t abgr = tintedRgba.toPackedAbgrU32();
    if (shapeKind == "Circle" || shapeKind == "SidedPolygon") {
      int sides = (shapeKind == "Circle") ? 48 : 6;
      float radScale = 1.0f;
      float gap = 0.0f;
      if (shapeProp) {
        if (const auto* sc = shapeProp->findChild("sides")) {
          if (const auto* iv = std::get_if<int64_t>(&sc->value)) sides = static_cast<int>(*iv);
        }
        if (const auto* rc = shapeProp->findChild("radius")) {
          if (const auto* dv = std::get_if<double>(&rc->value)) radScale = static_cast<float>(*dv);
        }
        if (const auto* gc = shapeProp->findChild("gap")) {
          if (const auto* dv = std::get_if<double>(&gc->value)) gap = static_cast<float>(*dv);
        }
      }
      appendPolygonFanVertices(rect, sides, radScale, gap, abgr, mainTex.isFbo, scratchVerts, scratchIndices);
    } else {
      appendQuadVertices(rect, abgr, mainTex.isFbo, scratchVerts, scratchIndices);
    }

    const auto blend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "Alpha"));
    device->setBlendMode(blend);
    device->useShader(shFxLight);
    device->setUniformMat4(shFxLight, "u_projView", projView);
    device->bindTexture(0, mainTex.tex);
    device->setUniformInt(shFxLight, "u_texture", 0);
    device->bindTexture(1, maskTex.tex);
    device->setUniformInt(shFxLight, "u_texture2", 1);
    device->setUniformFloat(shFxLight, "maskadd", maskAdd);
    device->setUniformFloat(shFxLight, "maskmul", maskMul);
    device->setUniformFloat(shFxLight, "mask_l_add", maskLAdd);
    device->setUniformFloat(shFxLight, "mask_l_mul", maskLMul);

    const bool flipMaskY = (mainTex.isFbo != maskTex.isFbo);
    device->setUniformFloat(shFxLight, "tex2_y_add", flipMaskY ? 1.0f : 0.0f);
    device->setUniformFloat(shFxLight, "tex2_y_mul", flipMaskY ? -1.0f : 1.0f);
    device->setUniformVec4(shFxLight, "Color2", 0.0f, 0.0f, 0.0f, 0.0f);
    device->setUniformFloat(shFxLight, "saturation", sat);
    device->setUniformFloat(shFxLight, "u_hdrGain", evalElementHdrGain(el, mctx));

    device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
    if (stats) stats->drawCalls += 1;
  }

  void renderSegment(const iota::model::PropertyEntry& segEntry,
                     const std::vector<ShapePoint>& pts,
                     const std::vector<float>& heightsPx,
                     float colorOffset,
                     bool closedShape,
                     iota::core::BlendMode blend) {
    const auto* segKind = std::get_if<std::string>(&segEntry.value);
    if (!segKind || *segKind == "None" || pts.size() < 2) {
      return;
    }

    auto childColor = [&](std::string_view k, iota::core::ColorRgba def) -> iota::core::ColorRgba {
      if (const auto* c = segEntry.findChild(k)) {
        if (const auto* iv = std::get_if<int64_t>(&c->value)) {
          return iota::core::ColorRgba::fromArgbInt32(static_cast<int32_t>(*iv));
        }
      }
      return def;
    };
    auto childFloat = [&](std::string_view k, float def) -> float {
      if (const auto* c = segEntry.findChild(k)) {
        if (const auto* dv = std::get_if<double>(&c->value)) return static_cast<float>(*dv);
        if (const auto* iv = std::get_if<int64_t>(&c->value)) return static_cast<float>(*iv);
      }
      return def;
    };
    auto childBool = [&](std::string_view k, bool def) -> bool {
      if (const auto* c = segEntry.findChild(k)) {
        if (const auto* b = std::get_if<bool>(&c->value)) return *b;
        if (const auto* iv = std::get_if<int64_t>(&c->value)) return *iv != 0;
      }
      return def;
    };

    const auto cFrom = childColor("colorFrom", {255, 255, 255, 255});
    const auto cTo = childColor("colorTo", cFrom);
    const float fixedH = childFloat("fixedHeight", 0.0f);
    const float heightMul = childFloat("barHeightMultiplier", 1.0f);
    const float barWidthScale = std::clamp(childFloat("barWidth", 0.6f), 0.05f, 3.0f);
    const bool mirror = childBool("mirror", false);
    const bool flipEveryOther = childBool("flipEveryOther", false);

    auto packColor = [&](int idx, int count) -> uint32_t {
      const float frac = std::fmod(
          static_cast<float>(idx) / static_cast<float>(std::max(1, count - 1)) + colorOffset,
          1.0f);
      auto col = iota::core::ColorRgba::lerp(cFrom, cTo, frac < 0.0f ? frac + 1.0f : frac);
      if (blend == iota::core::BlendMode::PreMulAlpha) {
        col.r = static_cast<uint8_t>((static_cast<uint32_t>(col.r) * col.a) / 255u);
        col.g = static_cast<uint8_t>((static_cast<uint32_t>(col.g) * col.a) / 255u);
        col.b = static_cast<uint8_t>((static_cast<uint32_t>(col.b) * col.a) / 255u);
      }
      return col.toPackedAbgrU32();
    };

    const int n = static_cast<int>(pts.size());
    scratchVerts.clear();
    scratchIndices.clear();

    if (*segKind == "Line") {
      // Continuous filled polygon wave strip
      const bool hasFixed = (std::abs(fixedH) > 1e-4f);
      for (int i = 0; i < n; ++i) {
        const auto& pt = pts[static_cast<size_t>(i)];
        float h = heightsPx[static_cast<size_t>(i)] * heightMul;
        if (flipEveryOther && (i % 2 == 1)) {
          h = -h;
        }
        const float hIn = hasFixed ? h : (mirror ? -h : 0.0f);
        const float hOut = hasFixed ? (h + fixedH) : h;
        const uint32_t abgr = packColor(i, n);

        scratchVerts.push_back({pt.x + pt.nx * hIn, pt.y + pt.ny * hIn, 0.0f, abgr, 0.5f, 0.5f});
        scratchVerts.push_back({pt.x + pt.nx * hOut, pt.y + pt.ny * hOut, 0.0f, abgr, 0.5f, 0.5f});
      }

      const int segLimit = closedShape ? n : (n - 1);
      for (int i = 0; i < segLimit; ++i) {
        const uint32_t i0 = static_cast<uint32_t>(i * 2);
        const uint32_t i1 = static_cast<uint32_t>(((i + 1) % n) * 2);
        scratchIndices.push_back(i0 + 0);
        scratchIndices.push_back(i0 + 1);
        scratchIndices.push_back(i1 + 1);
        scratchIndices.push_back(i0 + 0);
        scratchIndices.push_back(i1 + 1);
        scratchIndices.push_back(i1 + 0);
      }
      return;
    }

    // Compute average spacing between consecutive points for bar thickness
    float avgStep = 8.0f;
    if (n >= 2) {
      avgStep = std::max(2.0f, std::hypot(pts[1].x - pts[0].x, pts[1].y - pts[0].y));
    }
    const float halfWidth = std::max(1.0f, avgStep * 0.42f * barWidthScale);
    const bool isSharp = (*segKind == "SharpBars");
    const bool isRound = (*segKind == "RoundBars");
    const bool hasFixed = (std::abs(fixedH) > 1e-4f);

    for (int i = 0; i < n; ++i) {
      const auto& pt = pts[static_cast<size_t>(i)];
      const float rawH = heightsPx[static_cast<size_t>(i)] * heightMul;
      const float hIn = hasFixed ? rawH : (mirror ? -rawH : 0.0f);
      const float hOut = hasFixed ? (rawH + fixedH) : rawH;
      const uint32_t abgr = packColor(i, n);

      const float tx = -pt.ny * halfWidth;
      const float ty = pt.nx * halfWidth;

      const float bx0 = pt.x + pt.nx * hIn - tx;
      const float by0 = pt.y + pt.ny * hIn - ty;
      const float bx1 = pt.x + pt.nx * hIn + tx;
      const float by1 = pt.y + pt.ny * hIn + ty;

      if (isSharp) {
        const float tipX = pt.x + pt.nx * hOut;
        const float tipY = pt.y + pt.ny * hOut;
        const uint32_t base = static_cast<uint32_t>(scratchVerts.size());
        scratchVerts.push_back({bx0, by0, 0.0f, abgr, 0.5f, 0.5f});
        scratchVerts.push_back({bx1, by1, 0.0f, abgr, 0.5f, 0.5f});
        scratchVerts.push_back({tipX, tipY, 0.0f, abgr, 0.5f, 0.5f});
        scratchIndices.push_back(base + 0);
        scratchIndices.push_back(base + 1);
        scratchIndices.push_back(base + 2);
      } else {
        const float tipScale = isRound ? 0.65f : 1.0f;
        const float ox0 = pt.x + pt.nx * hOut - tx * tipScale;
        const float oy0 = pt.y + pt.ny * hOut - ty * tipScale;
        const float ox1 = pt.x + pt.nx * hOut + tx * tipScale;
        const float oy1 = pt.y + pt.ny * hOut + ty * tipScale;
        const uint32_t base = static_cast<uint32_t>(scratchVerts.size());
        scratchVerts.push_back({bx0, by0, 0.0f, abgr, 0.5f, 0.5f});
        scratchVerts.push_back({bx1, by1, 0.0f, abgr, 0.5f, 0.5f});
        scratchVerts.push_back({ox1, oy1, 0.0f, abgr, 0.5f, 0.5f});
        scratchVerts.push_back({ox0, oy0, 0.0f, abgr, 0.5f, 0.5f});
        scratchIndices.push_back(base + 0);
        scratchIndices.push_back(base + 1);
        scratchIndices.push_back(base + 2);
        scratchIndices.push_back(base + 0);
        scratchIndices.push_back(base + 2);
        scratchIndices.push_back(base + 3);
      }
    }
  }

  void renderBarsElement(const iota::model::Project& project,
                         int compIdx,
                         int elemIdx,
                         const iota::model::Element& el,
                         const iota::dsp::MVarEvalContext& mctx,
                         int analyzerCount,
                         RenderFrameStats* stats) {
    const auto& p = el.props;
    const int rid = el.runtimeId();
    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);
    const float angleRad = computeTotalRotationRadians(layout);
    const glm::mat4 projView = makeRotatedOrthoProjView(width, height, rect.centerX(), rect.centerY(), angleRad);

    const int providerIdx = std::clamp(getIntCI(p, "audioProviderIndex", 1) - 1, 0, std::max(0, analyzerCount - 1));
    const int delay = getIntCI(p, "reactionDelay", 0);
    const int accDelay = getIntCI(p, "reactionAccumulatedDelay", 0);
    const int softness = getIntCI(p, "softness", 0);

    analyzers[static_cast<size_t>(providerIdx)].sampleBarsForLayer(delay, accDelay, softness, scratchLayerBars);
    if (scratchLayerBars.empty()) {
      scratchLayerBars.assign(64, 0.0f);
    }

    const bool flipInput = getBoolCI(p, "flipInput", false);
    const float heightScale = evalKeyframeOverride(
        project, compIdx, elemIdx, "heightScale", mctx.trackPositionSec, getFloatCI(p, "heightScale", 2.0f));
    const float minHFrac = getFloatCI(p, "minHeightScale", 0.01f);
    const float maxHFrac = getFloatCI(p, "maxHeightScale", 1.0f);

    const float minDim = static_cast<float>(std::min(width, height));
    const float minH = minDim * minHFrac;
    const float maxH = std::max(0.0f, minDim * maxHFrac);
    const float hFactor = minDim * heightScale * 0.002f;
    const float hSign = (hFactor < 0.0f) ? -1.0f : 1.0f;
    const float hFactorAbs = std::abs(hFactor);

    const int barCount = static_cast<int>(scratchLayerBars.size());
    std::vector<float> heightsPx(static_cast<size_t>(barCount));
    for (int i = 0; i < barCount; ++i) {
      const int srcIdx = flipInput ? (barCount - 1 - i) : i;
      const float rawSample = std::max(0.0f, scratchLayerBars[static_cast<size_t>(srcIdx)]);
      heightsPx[static_cast<size_t>(i)] = (std::min(rawSample * hFactorAbs, maxH) * hSign) + minH;
    }

    const auto* shapeProp = findPropCI(p, "ShapePath");
    const std::string shapeKind = shapeProp && std::get_if<std::string>(&shapeProp->value)
                                      ? std::get<std::string>(shapeProp->value)
                                      : std::string("Circle");
    const bool closedShape = (shapeKind != "HorizontalLine");

    bool closedCurve = closedShape;
    if (closedCurve && shapeProp && shapeKind != "SidedPolygon" && shapeKind != "Letter" && shapeKind != "Year") {
      if (const auto* gapChild = shapeProp->findChild("gap")) {
        float gapVal = 0.0f;
        if (const auto* dv = std::get_if<double>(&gapChild->value)) gapVal = static_cast<float>(*dv);
        else if (const auto* iv = std::get_if<int64_t>(&gapChild->value)) gapVal = static_cast<float>(*iv);
        if (gapVal > 1e-4f) {
          closedCurve = false;
        }
      }
    }

    const auto curveMode =
        iota::dsp::curveInterpolationModeFromString(getStringCI(p, "curveInterpolation", "None"));
    const int curveSubs = std::clamp(getIntCI(p, "curveSubdivisions", 4), 1, 8);
    iota::dsp::interpolateBarCurve(heightsPx, curveMode, curveSubs, closedCurve, scratchInterpHeights);

    const int renderCount = static_cast<int>(scratchInterpHeights.size());
    buildShapePathPoints(shapeProp, rect, renderCount, scratchShape);

    const float colorBlendOffset = mvarEval.evaluateScalar(
        iota::dsp::MVarEvaluator::makeKey(rid, "colorBlendOffset"),
        p.getMVar("colorBlendOffset"),
        mctx);

    const auto blend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "Alpha"));
    device->setBlendMode(blend);
    device->useShader(shBuffer);
    device->setUniformMat4(shBuffer, "u_projView", projView);
    device->bindTexture(0, texWhite);
    device->setUniformInt(shBuffer, "u_texture", 0);
    device->setUniformFloat(shBuffer, "u_hdrGain", evalElementHdrGain(el, mctx));

    for (const char* segName : {"Segment1", "Segment2"}) {
      if (const auto* seg = findPropCI(p, segName)) {
        renderSegment(*seg, scratchShape, scratchInterpHeights, colorBlendOffset, closedShape, blend);
        if (!scratchVerticesEmpty()) {
          device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
          if (stats) stats->drawCalls += 1;
        }
      }
    }
  }

  bool scratchVerticesEmpty() const noexcept {
    return scratchVerts.empty() || scratchIndices.empty();
  }

  void renderParticlesElement(const iota::model::Project& project,
                              int compIdx,
                              int elemIdx,
                              const iota::model::Element& el,
                              iota::rhi::RenderTargetHandle currentTargetRt,
                              const iota::dsp::MVarEvalContext& mctx,
                              RenderFrameStats* stats) {
    const auto& p = el.props;
    const int rid = el.runtimeId();
    auto& ps = particleSystems[rid];

    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);

    ParticleSimConfig cfg;
    cfg.maxParticles = std::clamp(getIntCI(p, "CountLimit", 400), 1, 50000);
    cfg.spawnTime = std::clamp(getFloatCI(p, "spawnTime", 0.015f), 0.0005f, 0.5f);
    cfg.particlesPerSec = getFloatCI(p, "particlesPerSec", 0.0f);
    if (const auto* emitRateProp = findPropCI(p, "measureEmitRate")) {
      (void)emitRateProp;
      cfg.emitRateMultiplier = std::max(0.0f, mvarEval.evaluateScalar(
          iota::dsp::MVarEvaluator::makeKey(rid, "measureEmitRate"),
          p.getMVar("measureEmitRate", iota::model::MVariableFloat::constant(1.0f)),
          mctx));
    } else {
      cfg.emitRateMultiplier = 1.0f;
    }
    cfg.velocityFromMotion = getFloatCI(p, "velocityFromMotion", 0.0f);
    cfg.lifetime = std::clamp(getFloatCI(p, "lifetime", 2.2f), 0.05f, 30.0f);
    cfg.lifetimeRandom = std::clamp(getFloatCI(p, "lifetimeRandom", 0.25f), 0.0f, 0.95f);
    cfg.speedBase = getFloatCI(p, "Speed", 90.0f);
    cfg.speedRandom = getFloatCI(p, "speedRandom", 40.0f);
    cfg.directionSpreadDeg = std::clamp(getFloatCI(p, "directionSpread", 35.0f), 0.0f, 180.0f);
    cfg.overallSpeedMultiplier = std::max(0.0f, 1.0f + mvarEval.evaluateScalar(
        iota::dsp::MVarEvaluator::makeKey(rid, "MeasureOverallSpeed"),
        p.getMVar("MeasureOverallSpeed"),
        mctx));

    cfg.emitterSizeZ = getFloatCI(p, "emitterSizeZ", 0.0f);
    cfg.preRunSec = std::clamp(getFloatCI(p, "preRunSec", 0.0f), 0.0f, 10.0f);
    cfg.mirrorX = getBoolCI(p, "mirrorX", false);
    cfg.mirrorY = getBoolCI(p, "mirrorY", false);
    cfg.perspectiveDepth = std::max(0.0f, getFloatCI(p, "perspectiveDepth", 0.0f));
    cfg.perspectiveFov = std::clamp(getFloatCI(p, "perspectiveFov", 110.0f), 20.0f, 170.0f);

    const auto* spawnArea = findPropCI(p, "SpawnArea");
    const std::string spawnKind = spawnArea && std::get_if<std::string>(&spawnArea->value)
                                      ? std::get<std::string>(spawnArea->value)
                                      : std::string("Rectangle");
    cfg.emitterType = particleEmitterTypeFromString(spawnKind);
    auto spawnChildBool = [&](std::string_view k, bool def) -> bool {
      if (spawnArea) {
        if (const auto* c = spawnArea->findChild(k)) {
          if (const auto* b = std::get_if<bool>(&c->value)) return *b;
          if (const auto* iv = std::get_if<int64_t>(&c->value)) return *iv != 0;
        }
      }
      return def;
    };
    auto spawnChildFloat = [&](std::string_view k, float def) -> float {
      if (spawnArea) {
        if (const auto* c = spawnArea->findChild(k)) {
          if (const auto* dv = std::get_if<double>(&c->value)) return static_cast<float>(*dv);
          if (const auto* iv = std::get_if<int64_t>(&c->value)) return static_cast<float>(*iv);
        }
      }
      return def;
    };
    cfg.vectorRelativeToCenter = spawnChildBool("vectorRelativeToCenter", true);
    cfg.useRandomVector = spawnChildBool("useRandomVectorInstead", false);
    cfg.vectorAngleRad = spawnChildFloat("vectorAngle", 270.0f) * (kPi / 180.0f);
    cfg.vectorAngleZ = spawnChildFloat("vectorAngleZ", 0.0f);

    cfg.shapeType = particleShapeTypeFromString(getStringCI(p, "particleShape", "Sprite"));
    cfg.feather = std::clamp(getFloatCI(p, "feather", 0.5f), 0.0f, 1.0f);
    cfg.particleScale = getFloatCI(p, "particleScale", 2.5f);
    cfg.startSize = getFloatCI(p, "startSize", 1.0f);
    cfg.fadeInSize = getFloatCI(p, "fadeInSize", 1.0f);
    cfg.endSize = getFloatCI(p, "endSize", 0.2f);
    cfg.sizeRandom = std::clamp(getFloatCI(p, "sizeRandom", 0.2f), 0.0f, 1.0f);
    cfg.endSizeRandom = getFloatCI(p, "endSizeRandom", 0.0f);
    cfg.endSizeZFactor = getFloatCI(p, "endSizeZFactor", 0.0f);
    cfg.sizeAspectRatio = std::max(0.1f, getFloatCI(p, "sizeAspectRatio", 1.0f));
    cfg.fadeInAndOutTime = std::clamp(getFloatCI(p, "fadeInAndOutTime", 0.25f), 0.01f, 0.49f);
    cfg.sizeOverLife = particleCurvePresetFromString(getStringCI(p, "sizeOverLife", "Linear"));
    cfg.opacityOverLife = particleCurvePresetFromString(getStringCI(p, "opacityOverLife", "FadeInOut"));
    cfg.baseTint = getColorArgbCI(p, "color", {255, 255, 255, 255});
    cfg.colorFrom = getColorHslaCI(p, "ColorFrom", {0.55f, 0.8f, 0.65f, 0.85f});
    cfg.colorMid = getColorHslaCI(p, "ColorMid", {0.70f, 0.9f, 0.65f, 0.90f});
    cfg.colorTo = getColorHslaCI(p, "ColorTo", {0.85f, 0.9f, 0.55f, 0.0f});
    cfg.useMidColor = getBoolCI(p, "useMidColor", false);
    cfg.colorRandomHue = std::clamp(getFloatCI(p, "colorRandomHue", 0.0f), 0.0f, 1.0f);
    cfg.trailLength = std::max(0.0f, getFloatCI(p, "trailLength", 0.0f));
    cfg.velocityAngle = getBoolCI(p, "velocityAngle", true);
    cfg.initialRotationRandom = getBoolCI(p, "initialRotationRandom", false);
    cfg.spinSpeedDeg = getFloatCI(p, "spinSpeed", 0.0f);
    cfg.spinRandomDeg = getFloatCI(p, "spinRandom", 0.0f);

    cfg.gravity = p.getVec2("gravity", {0.0f, 0.0f});
    cfg.gravityZ = getFloatCI(p, "gravityZ", 0.0f);
    cfg.wind = {getFloatCI(p, "windX", 0.0f), getFloatCI(p, "windY", 0.0f), getFloatCI(p, "windZ", 0.0f)};
    cfg.airResistance = std::max(0.0f, getFloatCI(p, "airResistance", 0.0f));
    cfg.sideSineWaveFreq = getFloatCI(p, "sideSineWaveFreq", 0.0f);
    cfg.sideSineWaveFreqRandom = getFloatCI(p, "sideSineWaveFreqRandom", 0.0f);
    cfg.sideSineWaveMag = getFloatCI(p, "sideSineWaveMag", 0.0f);
    cfg.sideSineWaveMagRandom = getFloatCI(p, "sideSineWaveMagRandom", 0.0f);
    cfg.forceField = particleForceFieldFromString(getStringCI(p, "ForceField", "None"));
    cfg.forceFieldStrength = getFloatCI(p, "forceFieldStrength", 60.0f);
    cfg.forceFieldRadius = std::max(20.0f, getFloatCI(p, "forceFieldRadius", 400.0f));
    cfg.bounceEnabled = getBoolCI(p, "bounceEnabled", false);
    cfg.bounceFloorY = getFloatCI(p, "bounceFloorY", 0.9f);
    cfg.bounceRestitution = std::clamp(getFloatCI(p, "bounceRestitution", 0.6f), 0.0f, 1.0f);
    cfg.bounceFriction = std::clamp(getFloatCI(p, "bounceFriction", 0.2f), 0.0f, 1.0f);

    cfg.turbulenceAffectPos = getFloatCI(p, "turbulenceAffectPos", 0.0f);
    cfg.turbulenceAffectSize = getFloatCI(p, "turbulenceAffectSize", 0.0f);
    cfg.turbulenceAffectOpacity = getFloatCI(p, "turbulenceAffectOpacity", 0.0f);
    cfg.turbulenceScale = getFloatCI(p, "turbulenceScale", 1.5f);
    cfg.turbulenceEvolution = getFloatCI(p, "turbulenceEvolution", 0.6f);
    cfg.turbulenceOctaves = std::clamp(getIntCI(p, "turbulenceOctaves", 2), 1, 3);
    cfg.turbulenceFadeIn = std::max(0.0f, getFloatCI(p, "turbulenceFadeIn", 0.35f));

    cfg.auxMode = particleAuxModeFromString(getStringCI(p, "auxEmitMode", "Off"));
    cfg.auxParticlesPerSec = std::max(0.0f, getFloatCI(p, "auxParticlesPerSec", 12.0f));
    cfg.auxBurstCount = std::clamp(getIntCI(p, "auxBurstCount", 6), 1, 32);
    cfg.auxLifetime = std::clamp(getFloatCI(p, "auxLifetime", 0.6f), 0.05f, 10.0f);
    cfg.auxSizeScale = std::max(0.05f, getFloatCI(p, "auxSizeScale", 0.45f));
    cfg.auxVelocityScale = std::max(0.0f, getFloatCI(p, "auxVelocityScale", 0.08f));
    cfg.auxColorFromMain = std::clamp(getFloatCI(p, "auxColorFromMain", 0.75f), 0.0f, 1.0f);
    cfg.auxColor = getColorHslaCI(p, "auxColor", {0.12f, 0.95f, 0.65f, 0.80f});
    cfg.auxOpacity = std::clamp(getFloatCI(p, "auxOpacity", 0.65f), 0.0f, 1.0f);

    cfg.camera3DEnabled = getBoolCI(p, "camera3DEnabled", false);
    cfg.cameraDistance = std::max(40.0f, getFloatCI(p, "cameraDistance", 420.0f));
    cfg.cameraOrbitYawDeg = getFloatCI(p, "cameraOrbitYaw", 0.0f);
    cfg.cameraOrbitPitchDeg = getFloatCI(p, "cameraOrbitPitch", 0.0f);
    cfg.nearCameraFadeOutDistance = std::max(1.0f, getFloatCI(p, "nearCameraFadeOutDistance", 40.0f));
    cfg.dofEnabled = getBoolCI(p, "dofEnabled", false);
    cfg.dofFocusDist = std::max(25.0f, getFloatCI(p, "dofFocusDist", 420.0f));
    cfg.dofAperture = std::clamp(getFloatCI(p, "dofAperture", 0.35f), 0.0f, 3.0f);
    cfg.dofMaxBlur = std::clamp(getFloatCI(p, "dofMaxBlur", 0.85f), 0.0f, 1.0f);

    // Optional LayerMask GPU-to-64x64 sampling when emitterType == LayerMask
    scratchEmitterSamples.clear();
    if (cfg.emitterType == ParticleEmitterType::LayerMask && emitterSampleRt) {
      const std::string layerUri = getStringCI(p, "emitterLayer", "");
      const auto layerRef = parseCompositionRef(layerUri);
      const bool sampleCanvasBelow = layerUri.empty() ||
          (layerRef && (*layerRef <= 0 || *layerRef >= static_cast<int>(compTargets.size()) || *layerRef == compIdx));
      iota::rhi::TextureHandle srcLayerTex{};
      bool srcLayerIsFbo = true;
      if (sampleCanvasBelow) {
        srcLayerTex = captureCanvasSnapshot(currentTargetRt, stats);
        srcLayerIsFbo = true;
      } else {
        const auto resolvedLayer = resolveTextureUri(project, layerUri);
        srcLayerTex = resolvedLayer.tex;
        srcLayerIsFbo = resolvedLayer.isFbo;
      }
      if (srcLayerTex) {
        device->bindRenderTarget(emitterSampleRt, 64, 64);
        device->clear(0.0f, 0.0f, 0.0f, 0.0f);
        drawQuadWithBufferShader(srcLayerTex,
                                 makeOrthoProjView(64, 64),
                                 {0.0f, 0.0f, 64.0f, 64.0f},
                                 0xFFFFFFFFu,
                                 iota::core::BlendMode::Opaque,
                                 srcLayerIsFbo,
                                 stats);
        int sw = 0, sh = 0;
        if (device->readRenderTargetRgba(emitterSampleRt, sw, sh, emitterSampleRgba, true) &&
            sw == 64 && sh == 64 && emitterSampleRgba.size() == 64u * 64u * 4u) {
          for (int sy = 0; sy < 64; ++sy) {
            for (int sx = 0; sx < 64; ++sx) {
              const size_t idx = static_cast<size_t>((sy * 64 + sx) * 4);
              const uint8_t cr = emitterSampleRgba[idx + 0];
              const uint8_t cg = emitterSampleRgba[idx + 1];
              const uint8_t cb = emitterSampleRgba[idx + 2];
              const uint8_t ca = emitterSampleRgba[idx + 3];
              if (ca > 24 && (static_cast<int>(cr) + cg + cb) > 36) {
                scratchEmitterSamples.push_back({
                    (static_cast<float>(sx) + 0.5f) / 64.0f,
                    (static_cast<float>(sy) + 0.5f) / 64.0f,
                    cr, cg, cb, ca,
                });
              }
            }
          }
        }
        device->bindRenderTarget(currentTargetRt, width, height);
      }
    }

    ps.step(cfg, rect, width, height, mctx.dt, scratchEmitterSamples);
    if (stats) {
      stats->activeParticles += ps.activeCount();
      stats->activeAuxParticles += ps.auxCount();
    }

    const ResolvedTexture sprite = (cfg.shapeType == ParticleShapeType::Sprite)
                                       ? resolveTextureUri(project, getStringCI(p, "customImage", "internalres:particle_blur01"))
                                       : ResolvedTexture{texWhite, 1, 1, true};

    const auto blend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "Add"));
    const bool sortBackToFront =
        (blend == iota::core::BlendMode::Alpha || blend == iota::core::BlendMode::PreMulAlpha) &&
        (cfg.camera3DEnabled || cfg.perspectiveDepth > 0.0f);

    ps.buildGeometry(cfg, rect, width, height, sprite.isFbo, sortBackToFront, scratchVerts, scratchIndices);

    if (!scratchVerticesEmpty()) {
      const float premulRgb = (blend == iota::core::BlendMode::Add ||
                               blend == iota::core::BlendMode::Screen ||
                               blend == iota::core::BlendMode::PreMulAlpha)
                                  ? 1.0f
                                  : 0.0f;
      device->setBlendMode(blend);
      device->useShader(shParticle);
      device->setUniformMat4(shParticle, "u_projView", makeOrthoProjView(width, height));
      device->bindTexture(0, sprite.tex);
      device->setUniformInt(shParticle, "u_texture", 0);
      device->setUniformInt(shParticle, "u_particleShape", static_cast<int>(cfg.shapeType));
      device->setUniformFloat(shParticle, "u_feather", cfg.feather);
      device->setUniformFloat(shParticle, "u_premulRgb", premulRgb);
      device->setUniformFloat(shParticle, "u_hdrGain", evalElementHdrGain(el, mctx));
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
      if (stats) stats->drawCalls += 1;
    }
  }

  void renderTextElement(const iota::model::Project& project,
                         int compIdx,
                         int elemIdx,
                         const iota::model::Element& el,
                         const iota::audio::AudioEngine& audio,
                         const FrameTiming& timing,
                         const iota::dsp::MVarEvalContext& mctx,
                         RenderFrameStats* stats) {
    const auto& p = el.props;
    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);
    const float angleRad = computeTotalRotationRadians(layout);
    const glm::mat4 projView = makeRotatedOrthoProjView(width, height, rect.centerX(), rect.centerY(), angleRad);

    TextTokenContext tctx;
    tctx.timeCurrentSec = timing.isExporting ? mctx.trackPositionSec : audio.currentTimeSeconds();
    tctx.durationSec = audio.durationSeconds();
    tctx.artist = audio.currentArtist();
    tctx.title = audio.currentTitle();
    tctx.album = audio.currentAlbum();
    tctx.fps = timing.fpsStat;
    tctx.frameTimeMs = timing.dt * 1000.0f;
    tctx.flushCount = stats ? stats->drawCalls : 0;
    tctx.rtSwitchCount = stats ? stats->compositionsRendered : 0;

    std::string pattern = getStringCI(p, "text", "{ArtistAndTitle}");
    if (el.type == iota::model::ElementType::AppLogo) {
      pattern = "{MarkedArtistAndTitle}";
    } else if (el.type == iota::model::ElementType::StatText && pattern.empty()) {
      pattern = "{Fps} FPS";
    }
    const std::string resolved = expandTextTokens(pattern, tctx);
    const float resScale = static_cast<float>(std::min(width, height)) / 720.0f;
    const float fontSizePx = std::max(10.0f, static_cast<float>(getIntCI(p, "fontSize", 32)) * resScale);
    const auto col = getColorArgbCI(p, "color", {255, 255, 255, 255});
    const std::string face = getStringCI(p, "typeFace", "internal_2");

    TextStyle style;
    style.weight = fontWeightFromString(getStringCI(p, "fontWeight", "Default"));
    style.italic = getBoolCI(p, "fontItalic", false);
    style.alignment = textAlignmentFromString(getStringCI(p, "textAlign", "Center"));
    style.wordWrap = getBoolCI(p, "wordWrap", false);
    style.letterSpacingPx = getFloatCI(p, "letterSpacing", 0.0f) * resScale;
    style.lineSpacing = std::clamp(getFloatCI(p, "lineSpacing", 1.0f), 0.5f, 3.0f);
    style.outlineWidthPx = std::max(0.0f, getFloatCI(p, "outlineWidth", 0.0f)) * resScale;
    style.outlineColor = getColorArgbCI(p, "outlineColor", {0, 0, 0, 255});
    const auto shOff = p.getVec2("shadowOffset", {0.0f, 0.0f});
    style.shadowOffsetPx = {shOff.x * resScale, shOff.y * resScale};
    style.shadowColor = getColorArgbCI(p, "shadowColor", {0, 0, 0, 180});
    style.localAssets = &project.localAssets;

    scratchVerts.clear();
    scratchIndices.clear();
    const auto tex = fontCache.buildTextQuads(face, resolved, fontSizePx, rect, col, style, scratchVerts, scratchIndices);
    if (tex && !scratchVerticesEmpty()) {
      const auto blend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "PreMulAlpha"));
      device->setBlendMode(blend);
      device->useShader(shBuffer);
      device->setUniformMat4(shBuffer, "u_projView", projView);
      device->bindTexture(0, tex);
      device->setUniformInt(shBuffer, "u_texture", 0);
      device->setUniformFloat(shBuffer, "u_hdrGain", evalElementHdrGain(el, mctx));
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
      if (stats) stats->drawCalls += 1;
    }
  }

  void renderBlurEffect(const iota::model::Project& project,
                        int compIdx,
                        int elemIdx,
                        const iota::model::Element& el,
                        iota::rhi::RenderTargetHandle currentTargetRt,
                        const iota::dsp::MVarEvalContext& mctx,
                        RenderFrameStats* stats) {
    const auto& p = el.props;
    const int srcIdx = getIntCI(p, "sourceCompositionIndex", 1);
    const bool useCanvasBelow =
        (srcIdx <= 0 || srcIdx >= static_cast<int>(compTargets.size()) || srcIdx == compIdx);

    const iota::rhi::TextureHandle srcTex = useCanvasBelow
        ? captureCanvasSnapshot(currentTargetRt, stats)
        : device->renderTargetColorTexture(compTargets[static_cast<size_t>(srcIdx)]);
    const int rid = el.runtimeId();
    const float baseBlurRadius = getFloatCI(p, "blurRadius", 1.5f);
    const float reactiveBlur = mvarEval.evaluateScalar(
        iota::dsp::MVarEvaluator::makeKey(rid, "measureBlurRadius"),
        p.getMVar("measureBlurRadius"),
        mctx);
    const float blurRadius = std::clamp(baseBlurRadius + reactiveBlur, 0.0f, 12.0f);
    const float blurMult = std::clamp(getFloatCI(p, "blurMultiplier", 4.0f), 0.25f, 12.0f);
    const float resScale = static_cast<float>(std::min(width, height)) / 720.0f;
    const float radiusPx = blurRadius * blurMult * resScale * 3.0f;

    const std::string blurType = getStringCI(p, "blurType", "DualKawase");
    const float blurAngle = getFloatCI(p, "blurAngle", 0.0f);
    const auto blurCenter = p.getVec2("blurCenter", {0.5f, 0.5f});

    auto [blurredTex, blurredIsFbo] =
        runBlurPipeline(srcTex, true, blurType, radiusPx, blurAngle, blurCenter, stats);

    // Restore composition render target
    device->bindRenderTarget(currentTargetRt, width, height);

    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);
    const glm::mat4 projView = makeRotatedOrthoProjView(
        width, height, rect.centerX(), rect.centerY(), computeTotalRotationRadians(layout));

    const std::string maskUri = getStringCI(p, "MaskImage", "");
    const bool noMask = maskUri.empty() || asciiEqualsIgnoreCase(maskUri, "internalres:transparent");
    const ResolvedTexture maskTex = noMask
                                        ? ResolvedTexture{texWhite, 2, 2, true}
                                        : resolveTextureUri(project, maskUri);

    const auto contentBlend = iota::core::blendModeFromString(getStringCI(p, "blendModeContent", "Add"));
    const auto elemBlend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "Add"));

    auto drawLayer = [&](iota::rhi::TextureHandle tex,
                         bool isFbo,
                         iota::core::Vec2 layerScale,
                         iota::core::ColorRgba tint,
                         iota::core::ColorRgba color2Add,
                         iota::core::BlendMode blend,
                         bool invertMask,
                         float layerHdrGain) {
      if (layerScale.x <= 0.001f || layerScale.y <= 0.001f) {
        return;
      }
      const float hw = rect.width() * 0.5f * layerScale.x;
      const float hh = rect.height() * 0.5f * layerScale.y;
      const iota::core::RectF r{rect.centerX() - hw, rect.centerY() - hh, rect.centerX() + hw, rect.centerY() + hh};
      scratchVerts.clear();
      scratchIndices.clear();
      appendQuadVertices(r, tint.toPackedAbgrU32(), isFbo, scratchVerts, scratchIndices);

      device->setBlendMode(blend);
      device->useShader(shFxLight);
      device->setUniformMat4(shFxLight, "u_projView", projView);
      device->bindTexture(0, tex);
      device->setUniformInt(shFxLight, "u_texture", 0);
      device->bindTexture(1, maskTex.tex);
      device->setUniformInt(shFxLight, "u_texture2", 1);
      device->setUniformFloat(shFxLight, "maskadd", invertMask ? 1.0f : 0.0f);
      device->setUniformFloat(shFxLight, "maskmul", invertMask ? -1.0f : 1.0f);
      device->setUniformFloat(shFxLight, "mask_l_add", 1.0f);
      device->setUniformFloat(shFxLight, "mask_l_mul", 0.0f);
      const bool flipMaskY = (isFbo != maskTex.isFbo);
      device->setUniformFloat(shFxLight, "tex2_y_add", flipMaskY ? 1.0f : 0.0f);
      device->setUniformFloat(shFxLight, "tex2_y_mul", flipMaskY ? -1.0f : 1.0f);
      device->setUniformVec4(shFxLight,
                             "Color2",
                             color2Add.r / 255.0f,
                             color2Add.g / 255.0f,
                             color2Add.b / 255.0f,
                             color2Add.a / 255.0f);
      device->setUniformFloat(shFxLight, "saturation", 1.0f);
      device->setUniformFloat(shFxLight, "u_hdrGain", layerHdrGain);
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
      if (stats) stats->drawCalls += 1;
    };

    const float hdrGain = evalElementHdrGain(el, mctx);
    const auto mainCol = getColorArgbCI(p, "color", {0, 0, 0, 255});
    const iota::core::ColorRgba zeroColor2{0, 0, 0, 0};
    if (getBoolCI(p, "showUnblurredContentUnder", false)) {
      drawLayer(srcTex, true, {1.0f, 1.0f}, {255, 255, 255, 255}, zeroColor2, contentBlend, true, 1.0f);
    }

    const std::array<std::pair<const char*, const char*>, 3> blurLayers = {{
        {"1layerScale", "1layerColor"},
        {"2layerScale", "2layerColor"},
        {"3layerScale", "3layerColor"},
    }};
    for (const auto& [sKey, cKey] : blurLayers) {
      const auto ls = p.getVec2(sKey, {0.0f, 0.0f});
      const auto lc = getColorArgbCI(p, cKey, {255, 255, 255, 255});
      drawLayer(blurredTex, blurredIsFbo, ls, lc, mainCol, elemBlend, false, hdrGain);
    }

    if (getBoolCI(p, "showUnblurredContent", true)) {
      drawLayer(srcTex, true, {1.0f, 1.0f}, {255, 255, 255, 255}, zeroColor2, contentBlend, false, 1.0f);
    }
  }

  void renderRgbSplitEffect(const iota::model::Project& project,
                            int compIdx,
                            int elemIdx,
                            const iota::model::Element& el,
                            const iota::dsp::MVarEvalContext& mctx,
                            RenderFrameStats* stats) {
    const auto& p = el.props;
    const int rid = el.runtimeId();
    const auto target = resolveTextureUri(project, getStringCI(p, "targetImage", "composition:1"));

    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);
    const glm::mat4 projView = makeRotatedOrthoProjView(
        width, height, rect.centerX(), rect.centerY(), computeTotalRotationRadians(layout));

    const auto mainCol = getColorArgbCI(p, "color", {255, 255, 255, 255});
    const auto contentBlend = iota::core::blendModeFromString(getStringCI(p, "blendModeContent", "Add"));
    const auto elemBlend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "PreMulAlpha"));

    if (getBoolCI(p, "showUnblurredContentUnder", false)) {
      drawQuadWithBufferShader(target.tex, projView, rect, mainCol.toPackedAbgrU32(), contentBlend, target.isFbo, stats);
    }

    const float mult = mvarEval.evaluateScalar(
        iota::dsp::MVarEvaluator::makeKey(rid, "splitMultiplier"),
        p.getMVar("splitMultiplier", iota::model::MVariableFloat::constant(1.5f)),
        mctx);
    const auto amt = mvarEval.evaluateVec2(
        iota::dsp::MVarEvaluator::makeKey(rid, "splitAmount"),
        p.getMVar("splitAmount"),
        mctx);

    const auto c0 = getColorArgbCI(p, "splitColor0", {255, 0, 0, 255});
    const auto c1 = getColorArgbCI(p, "splitColor1", {0, 255, 0, 255});
    const auto c2 = getColorArgbCI(p, "splitColor2", {0, 0, 255, 255});

    scratchVerts.clear();
    scratchIndices.clear();
    appendQuadVertices(rect, mainCol.toPackedAbgrU32(), target.isFbo, scratchVerts, scratchIndices);

    device->setBlendMode(elemBlend);
    device->useShader(shRgbSplit);
    device->setUniformMat4(shRgbSplit, "u_projView", projView);
    device->bindTexture(0, target.tex);
    device->setUniformInt(shRgbSplit, "u_texture", 0);
    device->setUniformVec2(shRgbSplit, "dirAmount", amt.x * mult, amt.y * mult);
    device->setUniformVec3(shRgbSplit, "splitColor0", c0.r / 255.0f, c0.g / 255.0f, c0.b / 255.0f);
    device->setUniformVec3(shRgbSplit, "splitColor1", c1.r / 255.0f, c1.g / 255.0f, c1.b / 255.0f);
    device->setUniformVec3(shRgbSplit, "splitColor2", c2.r / 255.0f, c2.g / 255.0f, c2.b / 255.0f);
    device->setUniformFloat(shRgbSplit, "u_hdrGain", evalElementHdrGain(el, mctx));
    device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
    if (stats) stats->drawCalls += 1;

    if (getBoolCI(p, "showUnblurredContent", true)) {
      drawQuadWithBufferShader(target.tex, projView, rect, mainCol.toPackedAbgrU32(), contentBlend, target.isFbo, stats);
    }
  }

  void renderMotionBlurEffect(const iota::model::Project& project,
                              int compIdx,
                              int elemIdx,
                              const iota::model::Element& el,
                              iota::rhi::RenderTargetHandle currentTargetRt,
                              const iota::dsp::MVarEvalContext& mctx,
                              RenderFrameStats* stats) {
    const auto& p = el.props;
    const int rid = el.runtimeId();
    const std::string targetUri = getStringCI(p, "TargetImage", "composition:1");
    const auto compRef = parseCompositionRef(targetUri);
    const bool useCanvasBelow = targetUri.empty() ||
        (compRef && (*compRef <= 0 || *compRef >= static_cast<int>(compTargets.size()) || *compRef == compIdx));

    ResolvedTexture target{};
    if (useCanvasBelow) {
      target.tex = captureCanvasSnapshot(currentTargetRt, stats);
      target.texW = width;
      target.texH = height;
      target.isFbo = true;
      device->bindRenderTarget(currentTargetRt, width, height);
    } else {
      target = resolveTextureUri(project, targetUri);
    }

    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);
    const float angleRad = computeTotalRotationRadians(layout);
    const glm::mat4 projView = makeRotatedOrthoProjView(
        width, height, rect.centerX(), rect.centerY(), angleRad);

    const auto mainCol = getColorArgbCI(p, "color", {255, 255, 255, 255});
    const auto contentBlend = iota::core::blendModeFromString(getStringCI(p, "blendModeContent", "Add"));
    const auto elemBlend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "PreMulAlpha"));

    if (getBoolCI(p, "showUnblurredContentUnder", false)) {
      drawQuadWithBufferShader(target.tex, projView, rect, mainCol.toPackedAbgrU32(), contentBlend, target.isFbo, stats);
    }

    const float mult = std::max(0.0f, mvarEval.evaluateScalar(
        iota::dsp::MVarEvaluator::makeKey(rid, "blurAmountMultiplier"),
        p.getMVar("blurAmountMultiplier", iota::model::MVariableFloat::constant(1.0f)),
        mctx));

    const auto* motionSrcProp = findPropCI(p, "motionSource");
    const std::string motionSrcKind =
        motionSrcProp && std::get_if<std::string>(&motionSrcProp->value)
            ? std::get<std::string>(motionSrcProp->value)
            : std::string("EffectTransform");

    const float shutterAngle = std::clamp(getFloatCI(p, "shutterAngle", 180.0f), 0.0f, 720.0f);
    const float shutterPhase = std::clamp(getFloatCI(p, "shutterPhase", 0.0f), -0.5f, 0.5f);
    const int blurSamples = std::clamp(getIntCI(p, "blurSamples", 24), 4, 64);
    const auto blurCenter = p.getVec2("blurCenter", {0.5f, 0.5f});

    iota::core::Vec2 posAmt{};
    iota::core::Vec2 scaleAmt{};
    float rotAmt = 0.0f;

    const bool useTransform = asciiEqualsIgnoreCase(motionSrcKind, "EffectTransform") ||
                              asciiEqualsIgnoreCase(motionSrcKind, "Hybrid");
    const bool useManual = asciiEqualsIgnoreCase(motionSrcKind, "Manual");
    const bool useTemporal = asciiEqualsIgnoreCase(motionSrcKind, "Temporal") ||
                             asciiEqualsIgnoreCase(motionSrcKind, "Hybrid");

    if (useTransform) {
      if (auto prevIt = motionBlurPrevTransforms.find(rid); prevIt != motionBlurPrevTransforms.end()) {
        const auto& prev = prevIt->second;
        const float dx = (rect.centerX() - prev.centerX) / static_cast<float>(std::max(1, width));
        const float dyScreen = (rect.centerY() - prev.centerY) / static_cast<float>(std::max(1, height));
        const float dyUv = target.isFbo ? -dyScreen : dyScreen;
        const float dsx = (rect.width() - prev.width) / std::max(1.0f, prev.width);
        const float dsy = (rect.height() - prev.height) / std::max(1.0f, prev.height);
        const float dRot = wrapAngleDiffPi(angleRad - prev.angleRad);
        const float shutterScale =
            (shutterAngle / 360.0f) * ((1.0f / 60.0f) / std::max(mctx.dt, 1.0f / 500.0f)) * mult;

        posAmt.x = std::clamp(dx * shutterScale, -0.35f, 0.35f);
        posAmt.y = std::clamp(dyUv * shutterScale, -0.35f, 0.35f);
        scaleAmt.x = std::clamp(dsx * shutterScale, -0.60f, 0.60f);
        scaleAmt.y = std::clamp(dsy * shutterScale, -0.60f, 0.60f);
        rotAmt = std::clamp(dRot * shutterScale, -1.5f, 1.5f);
      }
      motionBlurPrevTransforms[rid] =
          MotionBlurPrevTransform{rect.centerX(), rect.centerY(), rect.width(), rect.height(), angleRad};
    } else if (useManual) {
      const bool hasStudioManual = (findPropCI(p, "manualDirectional") != nullptr) ||
                                   (findPropCI(p, "manualZoom") != nullptr) ||
                                   (findPropCI(p, "manualSpin") != nullptr);
      if (hasStudioManual) {
        const float dirRad = getFloatCI(p, "manualAngle", 0.0f) * kPi / 180.0f;
        const float manualDir = getFloatCI(p, "manualDirectional", 0.0f);
        const float manualZoom = getFloatCI(p, "manualZoom", 0.08f);
        const float manualSpin = getFloatCI(p, "manualSpin", 0.0f);
        posAmt.x = std::cos(dirRad) * manualDir * mult;
        posAmt.y = std::sin(dirRad) * manualDir * mult;
        scaleAmt = {manualZoom * mult, manualZoom * mult};
        rotAmt = (manualSpin * kPi / 180.0f) * mult;
      } else if (motionSrcProp != nullptr) {
        // Legacy preset Manual child MVars (posBlurAmount neutral at {0.5, 0.5}, scaleBlurAmount signed)
        auto readChildMVar = [&](std::string_view k, iota::core::Vec2 def) {
          if (const auto* child = motionSrcProp->findChild(k)) {
            if (const auto* sv = std::get_if<std::string>(&child->value)) {
              return iota::model::parseMVarString(*sv, iota::model::MVariableFloat::constant2(def.x, def.y));
            }
          }
          return iota::model::MVariableFloat::constant2(def.x, def.y);
        };
        const auto manualPos = mvarEval.evaluateVec2(
            iota::dsp::MVarEvaluator::makeKey(rid, "motionManualPos"),
            readChildMVar("posBlurAmount", {0.5f, 0.5f}),
            mctx);
        const auto manualScale = mvarEval.evaluateVec2(
            iota::dsp::MVarEvaluator::makeKey(rid, "motionManualScale"),
            readChildMVar("scaleBlurAmount", {0.0f, 0.0f}),
            mctx);
        posAmt = {(manualPos.x - 0.5f) * mult, (manualPos.y - 0.5f) * mult};
        scaleAmt = {manualScale.x * mult, manualScale.y * mult};
      }
      posAmt.x = std::clamp(posAmt.x, -0.45f, 0.45f);
      posAmt.y = std::clamp(posAmt.y, -0.45f, 0.45f);
      scaleAmt.x = std::clamp(scaleAmt.x, -0.75f, 0.75f);
      scaleAmt.y = std::clamp(scaleAmt.y, -0.75f, 0.75f);
      rotAmt = std::clamp(rotAmt, -2.0f, 2.0f);
    }

    if (useTemporal) {
      auto& hist = temporalBlurHistories[rid];
      for (int k = 0; k < 2; ++k) {
        if (!hist.rt[k]) {
          hist.rt[k] = device->createRenderTarget(width, height, iota::rhi::TextureFilter::Linear, workingFormat);
        }
      }
      const float trailDecayMs = std::clamp(getFloatCI(p, "trailDecayMs", 120.0f), 0.0f, 2500.0f);
      const float tauSec = (trailDecayMs * mult) / 1000.0f;
      const float keepFactor = (!hist.initialized || tauSec < 1e-4f)
          ? 0.0f
          : std::clamp(std::exp(-std::max(mctx.dt, 0.0f) / tauSec), 0.0f, 0.985f);

      const std::string blendStr = getStringCI(p, "trailBlend", "Mix");
      int trailBlendInt = 0;
      if (asciiEqualsIgnoreCase(blendStr, "Lighten")) trailBlendInt = 1;
      else if (asciiEqualsIgnoreCase(blendStr, "Screen")) trailBlendInt = 2;

      const int writeIdx = 1 - hist.readIdx;
      device->bindRenderTarget(hist.rt[writeIdx], width, height);
      device->setBlendMode(iota::core::BlendMode::Opaque);
      device->useShader(shTemporalBlur);
      device->bindTexture(0, target.tex);
      device->setUniformInt(shTemporalBlur, "u_currentTex", 0);
      device->bindTexture(1, device->renderTargetColorTexture(hist.rt[hist.readIdx]));
      device->setUniformInt(shTemporalBlur, "u_historyTex", 1);
      device->setUniformFloat(shTemporalBlur, "u_keepFactor", keepFactor);
      device->setUniformFloat(shTemporalBlur, "u_currFlipY", target.isFbo ? 0.0f : 1.0f);
      device->setUniformInt(shTemporalBlur, "u_trailBlend", trailBlendInt);
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, fullClipQuad(), fullClipIdx());
      if (stats) stats->drawCalls += 1;

      hist.readIdx = writeIdx;
      hist.initialized = true;
      target.tex = device->renderTargetColorTexture(hist.rt[hist.readIdx]);
      target.isFbo = true;
      device->bindRenderTarget(currentTargetRt, width, height);
    }

    const float aspect = static_cast<float>(std::max(1, width)) / static_cast<float>(std::max(1, height));
    const float centerV = target.isFbo ? std::clamp(1.0f - blurCenter.y, 0.0f, 1.0f) : std::clamp(blurCenter.y, 0.0f, 1.0f);

    scratchVerts.clear();
    scratchIndices.clear();
    appendQuadVertices(rect, mainCol.toPackedAbgrU32(), target.isFbo, scratchVerts, scratchIndices);

    device->setBlendMode(elemBlend);
    device->useShader(shMotionBlur);
    device->setUniformMat4(shMotionBlur, "u_projView", projView);
    device->bindTexture(0, target.tex);
    device->setUniformInt(shMotionBlur, "u_texture", 0);
    device->setUniformVec2(shMotionBlur, "u_posAmount", posAmt.x, posAmt.y);
    device->setUniformVec2(shMotionBlur, "u_scaleAmount", scaleAmt.x, scaleAmt.y);
    device->setUniformFloat(shMotionBlur, "u_rotAmount", rotAmt);
    device->setUniformFloat(shMotionBlur, "u_shutterPhase", shutterPhase);
    device->setUniformVec2(shMotionBlur, "u_center", std::clamp(blurCenter.x, 0.0f, 1.0f), centerV);
    device->setUniformFloat(shMotionBlur, "u_aspect", aspect);
    device->setUniformInt(shMotionBlur, "u_samples", blurSamples);
    device->setUniformFloat(shMotionBlur, "u_hdrGain", evalElementHdrGain(el, mctx));
    device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
    if (stats) stats->drawCalls += 1;

    if (getBoolCI(p, "showUnblurredContent", false)) {
      drawQuadWithBufferShader(target.tex, projView, rect, mainCol.toPackedAbgrU32(), contentBlend, target.isFbo, stats);
    }
  }

  void renderShakeEffect(const iota::model::Project& project,
                         int compIdx,
                         int elemIdx,
                         const iota::model::Element& el,
                         iota::rhi::RenderTargetHandle currentTargetRt,
                         const iota::dsp::MVarEvalContext& mctx,
                         RenderFrameStats* stats) {
    const auto& p = el.props;
    const int rid = el.runtimeId();
    const std::string targetUri = getStringCI(p, "TargetImage", "composition:0");
    const auto compRef = parseCompositionRef(targetUri);
    const bool useCanvasBelow = targetUri.empty() ||
        (compRef && (*compRef <= 0 || *compRef >= static_cast<int>(compTargets.size()) || *compRef == compIdx));

    ResolvedTexture target{};
    if (useCanvasBelow) {
      target.tex = captureCanvasSnapshot(currentTargetRt, stats);
      target.texW = width;
      target.texH = height;
      target.isFbo = true;
      device->bindRenderTarget(currentTargetRt, width, height);
    } else {
      target = resolveTextureUri(project, targetUri);
    }

    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);
    const glm::mat4 projView = makeRotatedOrthoProjView(
        width, height, rect.centerX(), rect.centerY(), computeTotalRotationRadians(layout));
    const auto mainCol = getColorArgbCI(p, "color", {255, 255, 255, 255});
    const auto elemBlend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "Opaque"));

    ShakeConfig cfg;
    cfg.style = shakeStyleFromString(getStringCI(p, "style", "Normal"));
    const float baseAmp = std::max(0.0f, getFloatCI(p, "amplitude", 0.35f));
    const float reactiveAmp = std::max(0.0f, mvarEval.evaluateScalar(
        iota::dsp::MVarEvaluator::makeKey(rid, "measureAmplitude"),
        p.getMVar("measureAmplitude"),
        mctx));
    cfg.amplitude = baseAmp + reactiveAmp;
    cfg.frequency = std::max(0.0f, getFloatCI(p, "frequency", 6.0f));
    cfg.stillness = std::clamp(getFloatCI(p, "stillness", 0.70f), 0.0f, 1.0f);
    cfg.twitchFreq = std::max(0.0f, getFloatCI(p, "twitchFreq", 2.0f));
    cfg.drift = std::clamp(getFloatCI(p, "drift", 0.30f), 0.0f, 1.0f);
    cfg.centerBias = std::max(0.0f, getFloatCI(p, "centerBias", 0.0f));
    cfg.zDist = std::clamp(getFloatCI(p, "zDist", 1.0f), 0.1f, 8.0f);
    cfg.phase = getFloatCI(p, "phase", 0.0f);
    cfg.seed = getFloatCI(p, "seed", 0.0f);
    cfg.still = getBoolCI(p, "still", false);
    cfg.octaves = std::clamp(getIntCI(p, "octaves", 3), 1, 10);

    cfg.xRandAmp = getFloatCI(p, "xRandAmp", 0.18f);
    cfg.xRandFreq = getFloatCI(p, "xRandFreq", 1.0f);
    cfg.xWaveAmp = getFloatCI(p, "xWaveAmp", 0.0f);
    cfg.xWaveFreq = getFloatCI(p, "xWaveFreq", 0.5f);
    cfg.xPhase = getFloatCI(p, "xPhase", 0.0f);
    cfg.yRandAmp = getFloatCI(p, "yRandAmp", 0.14f);
    cfg.yRandFreq = getFloatCI(p, "yRandFreq", 1.0f);
    cfg.yWaveAmp = getFloatCI(p, "yWaveAmp", 0.0f);
    cfg.yWaveFreq = getFloatCI(p, "yWaveFreq", 0.5f);
    cfg.yPhase = getFloatCI(p, "yPhase", 0.0f);
    cfg.zRandAmp = getFloatCI(p, "zRandAmp", 0.08f);
    cfg.zRandFreq = getFloatCI(p, "zRandFreq", 1.0f);
    cfg.zWaveAmp = getFloatCI(p, "zWaveAmp", 0.0f);
    cfg.zWaveFreq = getFloatCI(p, "zWaveFreq", 0.5f);
    cfg.zPhase = getFloatCI(p, "zPhase", 0.0f);
    cfg.tiltRandAmp = getFloatCI(p, "tiltRandAmp", 3.5f);
    cfg.tiltRandFreq = getFloatCI(p, "tiltRandFreq", 1.0f);
    cfg.tiltWaveAmp = getFloatCI(p, "tiltWaveAmp", 0.0f);
    cfg.tiltWaveFreq = getFloatCI(p, "tiltWaveFreq", 0.5f);
    cfg.tiltPhase = getFloatCI(p, "tiltPhase", 0.0f);
    cfg.twitchAmp = getFloatCI(p, "twitchAmp", 0.0f);

    cfg.redAmp = std::max(0.0f, getFloatCI(p, "redAmp", 1.0f));
    cfg.greenAmp = std::max(0.0f, getFloatCI(p, "greenAmp", 1.0f));
    cfg.blueAmp = std::max(0.0f, getFloatCI(p, "blueAmp", 1.0f));
    cfg.redPhase = getFloatCI(p, "redPhase", 0.0f);
    cfg.greenPhase = getFloatCI(p, "greenPhase", 0.0f);
    cfg.bluePhase = getFloatCI(p, "bluePhase", 0.0f);
    cfg.rgbRandomness = std::max(0.0f, getFloatCI(p, "rgbRandomness", 0.0f));
    cfg.rgbFrequency = std::max(0.0f, getFloatCI(p, "rgbFrequency", 2.0f));

    const bool moBlurOn = getBoolCI(p, "motionBlur", true);
    const float moBlurLen = std::clamp(getFloatCI(p, "moBlurLength", 0.65f), 0.0f, 4.0f);
    const float rgbPhase = std::clamp(getFloatCI(p, "rgbPhase", 0.0f), 0.0f, 1.0f);
    const float cropL = std::clamp(getFloatCI(p, "cropLeft", 0.0f), 0.0f, 0.45f);
    const float cropR = std::clamp(getFloatCI(p, "cropRight", 0.0f), 0.0f, 0.45f);
    const float cropT = std::clamp(getFloatCI(p, "cropTop", 0.0f), 0.0f, 0.45f);
    const float cropB = std::clamp(getFloatCI(p, "cropBottom", 0.0f), 0.0f, 0.45f);

    const double halfShutterSec = (moBlurOn && moBlurLen > 1e-4f)
                                      ? (0.5 * static_cast<double>(moBlurLen) / 60.0)
                                      : 0.0;
    const int samples = (moBlurOn && moBlurLen > 1e-4f && !cfg.still && cfg.amplitude > 1e-4f)
                            ? std::clamp(getIntCI(p, "blurSamples", 16), 4, 48)
                            : 1;

    const ShakeRgbPoses poses1 = evaluateCameraShakeRgb(cfg, mctx.totalTimeSec, rgbPhase);
    const ShakeRgbPoses poses0 =
        (halfShutterSec > 1e-6) ? evaluateCameraShakeRgb(cfg, mctx.totalTimeSec - halfShutterSec, rgbPhase) : poses1;
    const ShakeRgbPoses poses2 =
        (halfShutterSec > 1e-6) ? evaluateCameraShakeRgb(cfg, mctx.totalTimeSec + halfShutterSec, rgbPhase) : poses1;

    const int wrapX = static_cast<int>(shakeWrapModeFromString(getStringCI(p, "wrapX", "Reflect")));
    const int wrapY = static_cast<int>(shakeWrapModeFromString(getStringCI(p, "wrapY", "Reflect")));
    const float aspect = static_cast<float>(std::max(1, width)) / static_cast<float>(std::max(1, height));
    const float ySign = target.isFbo ? -1.0f : 1.0f;

    scratchVerts.clear();
    scratchIndices.clear();
    appendQuadVertices(rect, mainCol.toPackedAbgrU32(), target.isFbo, scratchVerts, scratchIndices);

    device->setBlendMode(elemBlend);
    device->useShader(shShake);
    device->setUniformMat4(shShake, "u_projView", projView);
    device->bindTexture(0, target.tex);
    device->setUniformInt(shShake, "u_texture", 0);
    device->setUniformVec4(shShake, "u_pose0", poses0.g.offsetX, poses0.g.offsetY * ySign, poses0.g.zDist, poses0.g.tiltRad);
    device->setUniformVec4(shShake, "u_pose1", poses1.g.offsetX, poses1.g.offsetY * ySign, poses1.g.zDist, poses1.g.tiltRad);
    device->setUniformVec4(shShake, "u_pose2", poses2.g.offsetX, poses2.g.offsetY * ySign, poses2.g.zDist, poses2.g.tiltRad);
    device->setUniformInt(shShake, "u_splitChannels", poses1.splitChannels ? 1 : 0);
    if (poses1.splitChannels) {
      device->setUniformVec4(shShake, "u_poseR0", poses0.r.offsetX, poses0.r.offsetY * ySign, poses0.r.zDist, poses0.r.tiltRad);
      device->setUniformVec4(shShake, "u_poseR1", poses1.r.offsetX, poses1.r.offsetY * ySign, poses1.r.zDist, poses1.r.tiltRad);
      device->setUniformVec4(shShake, "u_poseR2", poses2.r.offsetX, poses2.r.offsetY * ySign, poses2.r.zDist, poses2.r.tiltRad);
      device->setUniformVec4(shShake, "u_poseB0", poses0.b.offsetX, poses0.b.offsetY * ySign, poses0.b.zDist, poses0.b.tiltRad);
      device->setUniformVec4(shShake, "u_poseB1", poses1.b.offsetX, poses1.b.offsetY * ySign, poses1.b.zDist, poses1.b.tiltRad);
      device->setUniformVec4(shShake, "u_poseB2", poses2.b.offsetX, poses2.b.offsetY * ySign, poses2.b.zDist, poses2.b.tiltRad);
    }
    if (target.isFbo) {
      device->setUniformVec4(shShake, "u_crop", cropL, cropR, cropB, cropT);
    } else {
      device->setUniformVec4(shShake, "u_crop", cropL, cropR, cropT, cropB);
    }
    device->setUniformFloat(shShake, "u_aspect", aspect);
    device->setUniformInt(shShake, "u_wrapX", wrapX);
    device->setUniformInt(shShake, "u_wrapY", wrapY);
    device->setUniformInt(shShake, "u_samples", samples);
    device->setUniformFloat(shShake, "u_hdrGain", evalElementHdrGain(el, mctx));
    device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
    if (stats) stats->drawCalls += 1;
  }

  void renderMirrorEffect(const iota::model::Project& project,
                          int compIdx,
                          int elemIdx,
                          const iota::model::Element& el,
                          const iota::dsp::MVarEvalContext& mctx,
                          RenderFrameStats* stats) {
    const auto& p = el.props;
    const auto target = resolveTextureUri(project, getStringCI(p, "TargetImage", "composition:1"));
    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);
    const glm::mat4 projView = makeRotatedOrthoProjView(
        width, height, rect.centerX(), rect.centerY(), computeTotalRotationRadians(layout));

    const uint32_t abgr = getColorArgbCI(p, "color", {255, 255, 255, 255}).toPackedAbgrU32();
    const std::string mode = getStringCI(p, "mirrorMode", "Horizontal");
    const bool flip = getBoolCI(p, "flipMirror", false);

    const float vTop = target.isFbo ? 1.0f : 0.0f;
    const float vBot = target.isFbo ? 0.0f : 1.0f;
    const float vMid = 0.5f;

    scratchVerts.clear();
    scratchIndices.clear();
    auto pushUvQuad = [&](iota::core::RectF q, float u0, float v0, float u1, float v1) {
      const uint32_t b = static_cast<uint32_t>(scratchVerts.size());
      scratchVerts.push_back({q.left, q.top, 0.0f, abgr, u0, v0});
      scratchVerts.push_back({q.right, q.top, 0.0f, abgr, u1, v0});
      scratchVerts.push_back({q.right, q.bottom, 0.0f, abgr, u1, v1});
      scratchVerts.push_back({q.left, q.bottom, 0.0f, abgr, u0, v1});
      scratchIndices.push_back(b + 0);
      scratchIndices.push_back(b + 1);
      scratchIndices.push_back(b + 2);
      scratchIndices.push_back(b + 0);
      scratchIndices.push_back(b + 2);
      scratchIndices.push_back(b + 3);
    };

    const float cx = rect.centerX();
    const float cy = rect.centerY();
    const float ua = flip ? 0.0f : 1.0f;
    const float ub = 0.5f;
    const float va = flip ? vBot : vTop;
    const float vb = vMid;
    if (mode == "Vertical") {
      pushUvQuad({rect.left, rect.top, rect.right, cy}, 0.0f, va, 1.0f, vb);
      pushUvQuad({rect.left, cy, rect.right, rect.bottom}, 0.0f, vb, 1.0f, va);
    } else if (mode == "HorizontalAndVertical") {
      pushUvQuad({rect.left, rect.top, cx, cy}, ua, va, ub, vb);
      pushUvQuad({cx, rect.top, rect.right, cy}, ub, va, ua, vb);
      pushUvQuad({rect.left, cy, cx, rect.bottom}, ua, vb, ub, va);
      pushUvQuad({cx, cy, rect.right, rect.bottom}, ub, vb, ua, va);
    } else {
      pushUvQuad({rect.left, rect.top, cx, rect.bottom}, ua, vTop, ub, vBot);
      pushUvQuad({cx, rect.top, rect.right, rect.bottom}, ub, vTop, ua, vBot);
    }

    const auto blend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "PreMulAlpha"));
    device->setBlendMode(blend);
    device->useShader(shBuffer);
    device->setUniformMat4(shBuffer, "u_projView", projView);
    device->bindTexture(0, target.tex);
    device->setUniformInt(shBuffer, "u_texture", 0);
    device->setUniformFloat(shBuffer, "u_hdrGain", 1.0f);
    device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
    if (stats) stats->drawCalls += 1;
  }

  void renderCustomShaderElement(const iota::model::Project& project,
                                 int compIdx,
                                 int elemIdx,
                                 const iota::model::Element& el,
                                 const iota::dsp::MVarEvalContext& mctx,
                                 RenderFrameStats* stats) {
    const auto& p = el.props;
    const auto target = resolveTextureUri(project, getStringCI(p, "TargetImage", "composition:1"));
    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);
    const glm::mat4 projView = makeRotatedOrthoProjView(
        width, height, rect.centerX(), rect.centerY(), computeTotalRotationRadians(layout));

    const std::string vsRaw = getStringCI(p, "shaderVertex", "");
    const std::string fsRaw = getStringCI(p, "shaderFragment", "");

    iota::rhi::ShaderHandle sh = shBuffer;
    if (!fsRaw.empty()) {
      const std::string key = vsRaw + "\n---\n" + fsRaw;
      auto it = customShaders.find(key);
      if (it == customShaders.end()) {
        std::string err;
        auto created = device->createShader(adaptCustomGlslTo450(vsRaw, false),
                                            adaptCustomGlslTo450(fsRaw, true),
                                            &err);
        if (!created) {
          created = shBuffer;
          while (!err.empty() && err.back() == '\0') err.pop_back();
          if (err.empty()) err = "Custom shader compilation failed.";
          if (stats) stats->lastError = err;
          customShaders.emplace(key, CachedCustomShader{created, std::move(err)});
        } else {
          customShaders.emplace(key, CachedCustomShader{created, {}});
        }
        sh = created;
      } else {
        sh = it->second.handle;
        if (stats && !it->second.error.empty()) {
          stats->lastError = it->second.error;
        }
      }
    }

    scratchVerts.clear();
    scratchIndices.clear();
    appendQuadVertices(rect, 0xFFFFFFFFu, target.isFbo, scratchVerts, scratchIndices);

    device->setBlendMode(iota::core::blendModeFromString(getStringCI(p, "blendMode", "PreMulAlpha")));
    device->useShader(sh);
    device->setUniformMat4(sh, "u_projView", projView);
    device->bindTexture(0, target.tex);
    device->setUniformInt(sh, "u_texture", 0);
    device->setUniformFloat(sh, "u_time", static_cast<float>(mctx.totalTimeSec));
    device->setUniformVec2(sh, "u_resolution", static_cast<float>(width), static_cast<float>(height));
    device->setUniformFloat(sh, "u_beat", mctx.beat);
    device->setUniformVec4(sh, "u_soundKeys", mctx.soundKeys[0], mctx.soundKeys[1], mctx.soundKeys[2], mctx.beat);
    device->setUniformFloat(sh, "u_hdrGain", 1.0f);
    device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
    if (stats) stats->drawCalls += 1;
  }
};

SceneRenderer::SceneRenderer() : impl_(std::make_unique<Impl>()) {}
SceneRenderer::~SceneRenderer() { shutdown(); }

bool SceneRenderer::initialize(iota::rhi::IRenderDevice* device, std::string* outError) {
  if (!device) {
    if (outError) *outError = "Null IRenderDevice";
    return false;
  }
  impl_->device = device;
  impl_->fontCache.setDevice(device);

  if (!impl_->createBuiltinShaders(outError)) {
    return false;
  }

  const uint8_t whitePx[4] = {255, 255, 255, 255};
  const uint8_t blackPx[4] = {0, 0, 0, 255};
  impl_->texWhite = device->createTexture2D({1, 1, whitePx, iota::rhi::TextureFilter::Nearest,
                                             iota::rhi::TextureWrap::ClampToEdge, false},
                                            outError);
  impl_->texBlack = device->createTexture2D({1, 1, blackPx, iota::rhi::TextureFilter::Nearest,
                                             iota::rhi::TextureWrap::ClampToEdge, false},
                                            outError);
  impl_->initialized = (impl_->texWhite && impl_->texBlack);
  return impl_->initialized;
}

void SceneRenderer::shutdown() {
  if (!impl_ || !impl_->device) {
    return;
  }
  impl_->fontCache.shutdown();

  for (auto rt : impl_->compTargets) impl_->device->destroyRenderTarget(rt);
  impl_->compTargets.clear();
  for (auto* rt : {&impl_->finalRt, &impl_->tonemapRt, &impl_->blurFullA, &impl_->blurFullB,
                   &impl_->canvasSnapshotRt, &impl_->emitterSampleRt}) {
    if (*rt) {
      impl_->device->destroyRenderTarget(*rt);
      *rt = {};
    }
  }
  impl_->workingFormat = iota::rhi::PixelFormat::Rgba8;
  for (auto& rt : impl_->blurDownRts) {
    if (rt) {
      impl_->device->destroyRenderTarget(rt);
      rt = {};
    }
  }
  for (auto& rt : impl_->blurUpRts) {
    if (rt) {
      impl_->device->destroyRenderTarget(rt);
      rt = {};
    }
  }
  for (auto& [id, hist] : impl_->temporalBlurHistories) {
    for (int k = 0; k < 2; ++k) {
      if (hist.rt[k]) {
        impl_->device->destroyRenderTarget(hist.rt[k]);
        hist.rt[k] = {};
      }
    }
  }
  impl_->temporalBlurHistories.clear();
  impl_->activeFinalRt = {};

  for (auto& [k, t] : impl_->internalTexCache) {
    if (t != impl_->texWhite && t != impl_->texBlack) impl_->device->destroyTexture(t);
  }
  impl_->internalTexCache.clear();
  for (auto& [k, t] : impl_->localTexCache) {
    impl_->device->destroyTexture(t);
  }
  impl_->localTexCache.clear();

  if (impl_->texWhite) { impl_->device->destroyTexture(impl_->texWhite); impl_->texWhite = {}; }
  if (impl_->texBlack) { impl_->device->destroyTexture(impl_->texBlack); impl_->texBlack = {}; }

  for (auto& [k, entry] : impl_->customShaders) {
    if (entry.handle != impl_->shBuffer) impl_->device->destroyShader(entry.handle);
  }
  impl_->customShaders.clear();

  for (auto* sh : {&impl_->shBuffer, &impl_->shFxLight, &impl_->shKawaseDown, &impl_->shKawaseUp,
                   &impl_->shGaussianSep, &impl_->shRadialBlur, &impl_->shRgbSplit,
                   &impl_->shMotionBlur, &impl_->shTemporalBlur, &impl_->shParticle, &impl_->shShake,
                   &impl_->shBloomPrefilter, &impl_->shBloomDown, &impl_->shBloomUp,
                   &impl_->shTonemap, &impl_->shFxaa}) {
    if (*sh) {
      impl_->device->destroyShader(*sh);
      *sh = {};
    }
  }
  impl_->initialized = false;
  impl_->device = nullptr;
}

void SceneRenderer::resetSimulation() {
  impl_->mvarEval.reset();
  impl_->soundKeysProc.reset();
  for (auto& a : impl_->analyzers) {
    a.reset();
  }
  impl_->particleSystems.clear();
  impl_->motionBlurPrevTransforms.clear();
  for (auto& [id, hist] : impl_->temporalBlurHistories) {
    hist.initialized = false;
  }
}

void SceneRenderer::invalidateLocalAssets() {
  if (!impl_ || !impl_->device) return;
  for (auto& [k, t] : impl_->localTexCache) {
    impl_->device->destroyTexture(t);
  }
  impl_->localTexCache.clear();
  impl_->fontCache.invalidateCustomFonts();
}

AssetThumbnail SceneRenderer::resolveAssetPreview(const iota::model::Project& project,
                                                      std::string_view uri) {
  if (!impl_ || !impl_->initialized || !impl_->device) {
    return {};
  }
  const auto resolved = impl_->resolveTextureUri(project, uri);
  return {resolved.tex, resolved.texW, resolved.texH, resolved.isFbo};
}

iota::rhi::RenderTargetHandle SceneRenderer::renderFrame(const iota::model::Project& project,
                                                         const iota::audio::AudioEngine& audio,
                                                         const FrameTiming& timing,
                                                         int width,
                                                         int height,
                                                         iota::rhi::RenderTargetHandle outputTarget,
                                                         RenderFrameStats* outStats) {
  if (!impl_->initialized || project.compositions.empty()) {
    return {};
  }
  if (outStats) {
    *outStats = RenderFrameStats{};
  }

  const int compCount = static_cast<int>(project.compositions.size());
  const bool hdrEnabled = getBoolCI(project.compositions[0].props, "hdrPipeline", true);
  impl_->ensureRenderTargets(
      compCount,
      width,
      height,
      hdrEnabled ? iota::rhi::PixelFormat::Rgba16F : iota::rhi::PixelFormat::Rgba8);

  // Count and run all AudioProviders across the project (unlimited count)
  int providerCount = 0;
  for (const auto& comp : project.compositions) {
    for (const auto& el : comp.elements) {
      if (el.type == iota::model::ElementType::AudioProvider) {
        ++providerCount;
      }
    }
  }
  const size_t neededAnalyzers = static_cast<size_t>(std::max(1, providerCount));
  if (impl_->analyzers.size() < neededAnalyzers) {
    impl_->analyzers.resize(neededAnalyzers);
  }

  int providerSlot = 0;
  const double queryTime = timing.isExporting ? timing.playAccumTimeSec : audio.currentTimeSeconds();
  for (const auto& comp : project.compositions) {
    for (const auto& el : comp.elements) {
      if (el.type == iota::model::ElementType::AudioProvider) {
        const auto cfg = iota::dsp::spectrumConfigFromElement(el);
        impl_->analyzers[static_cast<size_t>(providerSlot)].setConfig(cfg);
        const int reqSamples = (cfg.kind == iota::dsp::SampleProviderKind::Spectrum2)
                                   ? std::clamp((cfg.audioDurationMs * audio.sampleRate()) / 1000, 64, 32768)
                                   : std::clamp(1 << cfg.inputSizePow, 32, 32768);
        audio.extractMonoWindow(queryTime, reqSamples, timing.visOffsetMs, impl_->scratchPcm);

        iota::dsp::AudioFrameInput fi;
        fi.monoPcm = impl_->scratchPcm;
        fi.sampleRate = audio.sampleRate();
        fi.dt = timing.dt;
        fi.songTimeSec = timing.totalTimeSec;
        fi.trackPositionSec = queryTime;
        fi.trackDurationSec = audio.durationSeconds();
        fi.isPlaying = audio.isPlaying();
        fi.isExporting = timing.isExporting;
        const auto& res = impl_->analyzers[static_cast<size_t>(providerSlot)].process(fi);
        if (providerSlot == 0 && outStats) {
          outStats->beat = res.beat;
          outStats->peakBarIndex = res.peakBarIndex;
          outStats->lastBars = res.smoothedBars;
        }
        ++providerSlot;
      }
    }
  }
  if (providerSlot == 0) {
    // Ensure analyzer slot 0 stays active even if preset omits AudioProvider
    impl_->analyzers[0].setConfig(iota::dsp::SpectrumConfig{});
    audio.extractMonoWindow(queryTime, 4096, timing.visOffsetMs, impl_->scratchPcm);
    iota::dsp::AudioFrameInput fi{impl_->scratchPcm, audio.sampleRate(), timing.dt, timing.totalTimeSec,
                                   queryTime, audio.durationSeconds(), audio.isPlaying(), timing.isExporting};
    const auto& res = impl_->analyzers[0].process(fi);
    if (outStats) {
      outStats->beat = res.beat;
      outStats->peakBarIndex = res.peakBarIndex;
      outStats->lastBars = res.smoothedBars;
    }
    providerSlot = 1;
  }

  const iota::model::Element* soundKeysEl = nullptr;
  for (const auto& comp : project.compositions) {
    for (const auto& el : comp.elements) {
      if (el.type == iota::model::ElementType::SoundKeys && getBoolCI(el.props, "visible", true)) {
        soundKeysEl = &el;
        break;
      }
    }
    if (soundKeysEl) {
      break;
    }
  }

  std::array<float, 3> soundKeysVals{0.0f, 0.0f, 0.0f};
  if (soundKeysEl) {
    const auto skCfg = iota::dsp::soundKeysConfigFromElement(*soundKeysEl);
    const int skSlot = std::clamp(skCfg.audioProviderIndex - 1, 0, std::max(0, providerSlot - 1));
    const auto& skAnalyzer = impl_->analyzers[static_cast<size_t>(skSlot)];
    const auto& skBars =
        skCfg.useRawSpectrum ? skAnalyzer.lastResult().rawBars : skAnalyzer.lastResult().smoothedBars;
    const auto& skRes = impl_->soundKeysProc.process(skBars, skCfg, timing.dt);
    soundKeysVals = skRes.outputs;
    if (outStats) {
      outStats->soundKeysOutputs = skRes.outputs;
      outStats->soundKeysRaw = skRes.rawNormalized;
      outStats->soundKeysSpectrum = skBars;
      outStats->soundKeysBarFreqsHz = iota::dsp::computeBarFrequencies(skAnalyzer.config());
      outStats->soundKeysCeiling = skRes.effectiveCeiling;
      outStats->soundKeysPeak = skRes.spectrumPeak;
    }
  } else if (outStats) {
    outStats->soundKeysOutputs = {};
    outStats->soundKeysRaw = {};
    outStats->soundKeysSpectrum = impl_->analyzers[0].lastResult().smoothedBars;
    outStats->soundKeysBarFreqsHz = iota::dsp::computeBarFrequencies(impl_->analyzers[0].config());
    float peak = 0.0f;
    for (float v : outStats->soundKeysSpectrum) {
      peak = std::max(peak, v);
    }
    outStats->soundKeysPeak = peak;
    outStats->soundKeysCeiling = std::max(50.0f, peak * 1.25f);
  }

  iota::dsp::MVarEvalContext mctx;
  mctx.dt = timing.dt;
  mctx.totalTimeSec = timing.totalTimeSec;
  mctx.playAccumTimeSec = timing.playAccumTimeSec;
  mctx.trackPositionSec = queryTime;
  mctx.trackDurationSec = audio.durationSeconds();
  mctx.beat = outStats ? outStats->beat : impl_->analyzers[0].lastResult().beat;
  mctx.peakBarIndex = outStats ? outStats->peakBarIndex : impl_->analyzers[0].lastResult().peakBarIndex;
  mctx.soundKeys = soundKeysVals;
  mctx.isPlaying = audio.isPlaying();
  mctx.isExporting = timing.isExporting;
  mctx.frameCounter = static_cast<uint32_t>(timing.totalTimeSec * 60.0);

  const std::vector<int> compOrder = impl_->computeCompositionOrder(project);
  for (int compIdx : compOrder) {
    const auto& comp = project.compositions[static_cast<size_t>(compIdx)];
    const auto rt = impl_->compTargets[static_cast<size_t>(compIdx)];
    impl_->device->bindRenderTarget(rt, impl_->width, impl_->height);
    if (compIdx == 0) {
      impl_->device->clear(0.03f, 0.03f, 0.045f, 1.0f);
    } else {
      impl_->device->clear(0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (outStats) outStats->compositionsRendered += 1;

    for (int elemIdx = 0; elemIdx < static_cast<int>(comp.elements.size()); ++elemIdx) {
      const auto& el = comp.elements[static_cast<size_t>(elemIdx)];
      if (!getBoolCI(el.props, "visible", true)) {
        continue;
      }

      switch (el.type) {
        case iota::model::ElementType::Image:
          impl_->renderImageElement(project, compIdx, elemIdx, el, rt, mctx, outStats);
          break;
        case iota::model::ElementType::Bars:
          impl_->renderBarsElement(project, compIdx, elemIdx, el, mctx, providerSlot, outStats);
          break;
        case iota::model::ElementType::Particles:
          impl_->renderParticlesElement(project, compIdx, elemIdx, el, rt, mctx, outStats);
          break;
        case iota::model::ElementType::Text:
        case iota::model::ElementType::StatText:
        case iota::model::ElementType::AppLogo:
          impl_->renderTextElement(project, compIdx, elemIdx, el, audio, timing, mctx, outStats);
          break;
        case iota::model::ElementType::BlurEffect:
          impl_->renderBlurEffect(project, compIdx, elemIdx, el, rt, mctx, outStats);
          break;
        case iota::model::ElementType::RgbSplitEffect:
          impl_->renderRgbSplitEffect(project, compIdx, elemIdx, el, mctx, outStats);
          break;
        case iota::model::ElementType::MotionBlurEffect:
          impl_->renderMotionBlurEffect(project, compIdx, elemIdx, el, rt, mctx, outStats);
          break;
        case iota::model::ElementType::ShakeEffect:
          impl_->renderShakeEffect(project, compIdx, elemIdx, el, rt, mctx, outStats);
          break;
        case iota::model::ElementType::MirrorEffect:
          impl_->renderMirrorEffect(project, compIdx, elemIdx, el, mctx, outStats);
          break;
        case iota::model::ElementType::CustomShader:
          impl_->renderCustomShaderElement(project, compIdx, elemIdx, el, mctx, outStats);
          break;
        default:
          break;
      }
    }

    // Intro / outro fade overlay on final composition 0
    if (compIdx == 0) {
      const float introDur = getFloatCI(comp.props, "introFadeDuration", 0.0f);
      const float outroDur = getFloatCI(comp.props, "outroFadeDuration", 0.0f);
      const double dur = audio.durationSeconds();
      float fadeAlpha = 0.0f;
      if (introDur > 0.001f && queryTime < introDur) {
        fadeAlpha = std::clamp(1.0f - static_cast<float>(queryTime / introDur), 0.0f, 1.0f);
      } else if (outroDur > 0.001f && dur > outroDur && queryTime > (dur - outroDur)) {
        fadeAlpha = std::clamp(static_cast<float>((queryTime - (dur - outroDur)) / outroDur), 0.0f, 1.0f);
      }
      if (fadeAlpha > 0.001f) {
        auto fadeCol = getColorArgbCI(comp.props, "introFadeColor", {0, 0, 0, 223});
        fadeCol.a = static_cast<uint8_t>(std::clamp(static_cast<float>(fadeCol.a) * fadeAlpha, 0.0f, 255.0f));
        const auto blend = iota::core::blendModeFromString(getStringCI(comp.props, "introBlendMode", "Alpha"));
        impl_->drawQuadWithBufferShader(impl_->texWhite,
                                        makeOrthoProjView(impl_->width, impl_->height),
                                        {0.0f, 0.0f, static_cast<float>(impl_->width), static_cast<float>(impl_->height)},
                                        fadeCol.toPackedAbgrU32(),
                                        blend,
                                        false,
                                        outStats);
      }
    }
  }

  const auto comp0Rt = impl_->compTargets[0];
  const auto dstRt = outputTarget ? outputTarget : impl_->finalRt;
  const auto& comp0Props = project.compositions[0].props;

  const float exposureEv = std::clamp(getFloatCI(comp0Props, "exposureEv", 0.0f), -6.0f, 6.0f);
  const float exposure = std::pow(2.0f, exposureEv);
  const bool bloomEnabled = getBoolCI(comp0Props, "bloomEnabled", false);
  const float bloomThreshold = std::max(0.0f, getFloatCI(comp0Props, "bloomThreshold", 0.90f));
  const float bloomSoftKnee = std::clamp(getFloatCI(comp0Props, "bloomSoftKnee", 0.50f), 0.0f, 1.0f);
  const float baseBloomIntensity = std::max(0.0f, getFloatCI(comp0Props, "bloomIntensity", 0.50f));
  float bloomAudioBoost = 0.0f;
  if (comp0Props.contains("measureBloomIntensity")) {
    bloomAudioBoost = std::max(0.0f, impl_->mvarEval.evaluateScalar(
        iota::dsp::MVarEvaluator::makeKey(-100, "comp0_measureBloomIntensity"),
        comp0Props.getMVar("measureBloomIntensity"),
        mctx));
  }
  const float effectiveBloom = bloomEnabled ? (baseBloomIntensity + bloomAudioBoost) : 0.0f;
  const float bloomRadius = std::clamp(getFloatCI(comp0Props, "bloomRadius", 0.72f), 0.05f, 0.98f);
  const float bloomAnamorphic = std::clamp(getFloatCI(comp0Props, "bloomAnamorphic", 0.0f), 0.0f, 1.0f);
  const auto bloomTint = getColorArgbCI(comp0Props, "bloomTint", {255, 255, 255, 255});

  const std::string tonemapStr = getStringCI(comp0Props, "tonemapMode", "None");
  int tonemapInt = 0;
  if (asciiEqualsIgnoreCase(tonemapStr, "ACES")) tonemapInt = 1;
  else if (asciiEqualsIgnoreCase(tonemapStr, "AgX")) tonemapInt = 2;
  else if (asciiEqualsIgnoreCase(tonemapStr, "PBRNeutral")) tonemapInt = 3;
  else if (asciiEqualsIgnoreCase(tonemapStr, "Reinhard")) tonemapInt = 4;

  const float contrast = std::clamp(getFloatCI(comp0Props, "contrast", 1.0f), 0.1f, 4.0f);
  const float saturation = std::clamp(getFloatCI(comp0Props, "saturation", 1.0f), 0.0f, 4.0f);
  const float gamma = std::clamp(getFloatCI(comp0Props, "gamma", 1.0f), 0.1f, 4.0f);
  const float vignette = std::clamp(getFloatCI(comp0Props, "vignette", 0.0f), 0.0f, 1.0f);

  const bool hdrPostActive =
      (effectiveBloom > 1e-4f) || (tonemapInt != 0) ||
      (std::abs(exposureEv) > 1e-4f) || (std::abs(contrast - 1.0f) > 1e-4f) ||
      (std::abs(saturation - 1.0f) > 1e-4f) || (std::abs(gamma - 1.0f) > 1e-4f) ||
      (vignette > 1e-4f);

  if (outStats) {
    outStats->hdrPipelineActive = (impl_->workingFormat == iota::rhi::PixelFormat::Rgba16F);
    outStats->bloomActive = (effectiveBloom > 1e-4f);
  }

  auto runFxaaPass = [&](iota::rhi::RenderTargetHandle srcRt, iota::rhi::RenderTargetHandle outRt) {
    impl_->device->bindRenderTarget(outRt, impl_->width, impl_->height);
    impl_->device->setBlendMode(iota::core::BlendMode::Opaque);
    impl_->device->useShader(impl_->shFxaa);
    impl_->device->bindTexture(0, impl_->device->renderTargetColorTexture(srcRt));
    impl_->device->setUniformInt(impl_->shFxaa, "u_texture", 0);
    impl_->device->setUniformFloat(impl_->shFxaa, "resolutionW", static_cast<float>(impl_->width));
    impl_->device->setUniformFloat(impl_->shFxaa, "resolutionH", static_cast<float>(impl_->height));
    impl_->device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles,
                               Impl::fullClipQuad(),
                               Impl::fullClipIdx());
    if (outStats) outStats->drawCalls += 1;
  };

  if (hdrPostActive) {
    const iota::rhi::TextureHandle comp0Tex = impl_->device->renderTargetColorTexture(comp0Rt);
    const iota::rhi::TextureHandle bloomTex = (effectiveBloom > 1e-4f)
        ? impl_->runBloomPipeline(comp0Tex,
                                  exposure,
                                  bloomThreshold,
                                  bloomSoftKnee,
                                  bloomRadius,
                                  bloomAnamorphic,
                                  outStats)
        : impl_->texBlack;

    const auto postTarget = timing.fxaaEnabled ? impl_->tonemapRt : dstRt;
    impl_->device->bindRenderTarget(postTarget, impl_->width, impl_->height);
    impl_->device->setBlendMode(iota::core::BlendMode::Opaque);
    impl_->device->useShader(impl_->shTonemap);
    impl_->device->bindTexture(0, comp0Tex);
    impl_->device->setUniformInt(impl_->shTonemap, "u_sceneTex", 0);
    impl_->device->bindTexture(1, bloomTex);
    impl_->device->setUniformInt(impl_->shTonemap, "u_bloomTex", 1);
    impl_->device->setUniformInt(impl_->shTonemap, "u_tonemapMode", tonemapInt);
    impl_->device->setUniformFloat(impl_->shTonemap, "u_exposure", exposure);
    impl_->device->setUniformFloat(impl_->shTonemap, "u_bloomAmount", effectiveBloom);
    impl_->device->setUniformVec3(impl_->shTonemap,
                                  "u_bloomTint",
                                  static_cast<float>(bloomTint.r) / 255.0f,
                                  static_cast<float>(bloomTint.g) / 255.0f,
                                  static_cast<float>(bloomTint.b) / 255.0f);
    impl_->device->setUniformFloat(impl_->shTonemap, "u_contrast", contrast);
    impl_->device->setUniformFloat(impl_->shTonemap, "u_saturation", saturation);
    impl_->device->setUniformFloat(impl_->shTonemap, "u_gamma", gamma);
    impl_->device->setUniformFloat(impl_->shTonemap, "u_vignette", vignette);
    impl_->device->setUniformFloat(impl_->shTonemap, "u_dither", timing.fxaaEnabled ? 0.0f : 1.0f);
    impl_->device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles,
                               Impl::fullClipQuad(),
                               Impl::fullClipIdx());
    if (outStats) outStats->drawCalls += 1;

    if (timing.fxaaEnabled) {
      runFxaaPass(impl_->tonemapRt, dstRt);
    }
    impl_->activeFinalRt = dstRt;
  } else if (timing.fxaaEnabled) {
    runFxaaPass(comp0Rt, dstRt);
    impl_->activeFinalRt = dstRt;
  } else if ((outputTarget && outputTarget != comp0Rt) ||
             impl_->workingFormat != iota::rhi::PixelFormat::Rgba8) {
    impl_->device->bindRenderTarget(dstRt, impl_->width, impl_->height);
    impl_->drawQuadWithBufferShader(impl_->device->renderTargetColorTexture(comp0Rt),
                                    makeOrthoProjView(impl_->width, impl_->height),
                                    {0.0f, 0.0f, static_cast<float>(impl_->width), static_cast<float>(impl_->height)},
                                    0xFFFFFFFFu,
                                    iota::core::BlendMode::Opaque,
                                    true,
                                    outStats,
                                    1.0f);
    impl_->activeFinalRt = dstRt;
  } else {
    impl_->activeFinalRt = comp0Rt;
  }

  if (outStats) {
    const auto fStats = impl_->fontCache.stats();
    outStats->cachedGlyphs = fStats.cachedGlyphs;
    outStats->fontUsingDirectWrite = fStats.usingDirectWrite;
  }

  impl_->device->bindRenderTarget({}, 0, 0);
  return impl_->activeFinalRt;
}

iota::rhi::RenderTargetHandle SceneRenderer::hdrSceneRenderTarget() const noexcept {
  if (!impl_ || impl_->compTargets.empty()) {
    return {};
  }
  return impl_->compTargets[0];
}

iota::rhi::RenderTargetHandle SceneRenderer::finalRenderTarget() const noexcept {
  return impl_->activeFinalRt;
}

iota::rhi::TextureHandle SceneRenderer::finalOutputTexture() const noexcept {
  if (!impl_->device || !impl_->activeFinalRt) {
    return {};
  }
  return impl_->device->renderTargetColorTexture(impl_->activeFinalRt);
}

}  // namespace iota::engine
