#include "iota/engine/scene_renderer.hpp"

#include "iota/core/procedural_assets.hpp"
#include "iota/core/types.hpp"
#include "iota/dsp/filters.hpp"
#include "iota/engine/builtin_shaders.hpp"
#include "iota/engine/layout.hpp"

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
#include <numeric>
#include <string>
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

uint32_t xorshift32(uint32_t& state) noexcept {
  uint32_t x = state ? state : 0x6D2B79F5u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return x;
}

float rand01(uint32_t& state) noexcept {
  return static_cast<float>(xorshift32(state) & 0x00FFFFFFu) / 16777215.0f;
}

struct ParticleInstance {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  float vx = 0.0f, vy = 0.0f, vz = 0.0f;
  float age = 0.0f;
  float maxAge = 2.0f;
  float sizeRand = 0.0f;
  float waveFreq = 1.0f;
  float waveMag = 1.0f;
  float wavePhase = 0.0f;
  bool alive = false;
};

struct ParticlePool {
  uint32_t rng = 1337u;
  float spawnAccum = 0.0f;
  std::vector<ParticleInstance> pool;
};

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

  if (body.find("iota_FragColor") != std::string::npos) {
    out += "out vec4 iota_FragColor;\n";
  }
  if (body.find("v_uv") == std::string::npos && body.find("vTexCoord") == std::string::npos) {
    out += "in vec2 v_uv;\n";
  }
  if (body.find("v_color") == std::string::npos && body.find("vColor") == std::string::npos) {
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
  iota::rhi::ShaderHandle shBlurStack{};
  iota::rhi::ShaderHandle shRgbSplit{};
  iota::rhi::ShaderHandle shMotionBlur{};
  iota::rhi::ShaderHandle shFxaa{};
  std::unordered_map<std::string, iota::rhi::ShaderHandle> customShaders;

  // Cached asset textures
  iota::rhi::TextureHandle texWhite{};
  iota::rhi::TextureHandle texBlack{};
  std::unordered_map<std::string, iota::rhi::TextureHandle> internalTexCache;
  std::unordered_map<std::string, iota::rhi::TextureHandle> localTexCache;

  FontAtlasCache fontCache;
  iota::dsp::MVarEvaluator mvarEval;
  std::vector<iota::dsp::AudioAnalyzer> analyzers{1};
  std::unordered_map<int, ParticlePool> particlePools;
  std::unordered_map<int, iota::core::RectF> motionBlurPrevRects;

  // Render targets
  std::vector<iota::rhi::RenderTargetHandle> compTargets;  // Index matches Composition index [0..N-1]
  iota::rhi::RenderTargetHandle finalRt{};
  std::array<iota::rhi::RenderTargetHandle, 6> blurPyramidRts{};
  iota::rhi::RenderTargetHandle activeFinalRt{};

  // Reusable CPU geometry / PCM scratch buffers
  std::vector<iota::rhi::VertexPosColUv> scratchVerts;
  std::vector<uint32_t> scratchIndices;
  std::vector<float> scratchPcm;
  std::vector<float> scratchLayerBars;
  std::vector<float> scratchInterpHeights;
  std::vector<ShapePoint> scratchShape;
  int width = 0;
  int height = 0;

  bool createBuiltinShaders(std::string* outError) {
    shBuffer = device->createShader(shaders::kBufferVert, shaders::kBufferFrag, outError);
    if (!shBuffer) return false;
    shFxLight = device->createShader(shaders::kBufferVert, shaders::kBufferFxLightFrag, outError);
    if (!shFxLight) return false;
    shBlurStack = device->createShader(shaders::kBlurFullscreenVert, shaders::kBlurStackFrag, outError);
    if (!shBlurStack) return false;
    shRgbSplit = device->createShader(shaders::kBufferVert, shaders::kRgbSplitFrag, outError);
    if (!shRgbSplit) return false;
    shMotionBlur = device->createShader(shaders::kBufferVert, shaders::kMotionBlurFrag, outError);
    if (!shMotionBlur) return false;
    shFxaa = device->createShader(shaders::kBlurFullscreenVert, shaders::kFxaaFrag, outError);
    return static_cast<bool>(shFxaa);
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

    // Desktop fallback: allow direct filesystem image path
    if (std::filesystem::exists(uri)) {
      std::string key(uri);
      auto cached = localTexCache.find(key);
      if (cached != localTexCache.end()) {
        out.tex = cached->second;
        device->textureSize(out.tex, out.texW, out.texH);
        return out;
      }
      int iw = 0, ih = 0, ch = 0;
      uint8_t* decoded = stbi_load(key.c_str(), &iw, &ih, &ch, 4);
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

    out.tex = getInternalTexture("internalres:placeholderart5");
    device->textureSize(out.tex, out.texW, out.texH);
    return out;
  }

  void ensureRenderTargets(int compCount, int w, int h) {
    width = std::max(16, w);
    height = std::max(16, h);

    while (static_cast<int>(compTargets.size()) < compCount) {
      compTargets.push_back(device->createRenderTarget(width, height));
    }
    while (static_cast<int>(compTargets.size()) > compCount) {
      device->destroyRenderTarget(compTargets.back());
      compTargets.pop_back();
    }
    for (auto rt : compTargets) {
      device->resizeRenderTarget(rt, width, height);
    }

    if (!finalRt) {
      finalRt = device->createRenderTarget(width, height);
    } else {
      device->resizeRenderTarget(finalRt, width, height);
    }

    int pw = width;
    int ph = height;
    for (size_t i = 0; i < blurPyramidRts.size(); ++i) {
      pw = std::max(2, pw / 2);
      ph = std::max(2, ph / 2);
      if (!blurPyramidRts[i]) {
        blurPyramidRts[i] = device->createRenderTarget(pw, ph);
      } else {
        device->resizeRenderTarget(blurPyramidRts[i], pw, ph);
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
        for (const char* propName : {"customImage", "MaskImage", "targetImage"}) {
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

  void drawQuadWithBufferShader(iota::rhi::TextureHandle tex,
                                const glm::mat4& projView,
                                iota::core::RectF rect,
                                uint32_t abgr,
                                iota::core::BlendMode blend,
                                bool flipY,
                                RenderFrameStats* stats) {
    scratchVerts.clear();
    scratchIndices.clear();
    appendQuadVertices(rect, abgr, flipY, scratchVerts, scratchIndices);

    device->setBlendMode(blend);
    device->useShader(shBuffer);
    device->setUniformMat4(shBuffer, "u_projView", projView);
    device->bindTexture(0, tex);
    device->setUniformInt(shBuffer, "u_texture", 0);
    device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
    if (stats) stats->drawCalls += 1;
  }

  void renderImageElement(const iota::model::Project& project,
                          int compIdx,
                          int elemIdx,
                          const iota::model::Element& el,
                          const iota::dsp::MVarEvalContext& mctx,
                          RenderFrameStats* stats) {
    const auto& p = el.props;
    const int rid = el.runtimeId();
    const std::string uri = getStringCI(p, "customImage", "internalres:placeholderart5");
    const auto mainTex = resolveTextureUri(project, uri);

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
                              const iota::dsp::MVarEvalContext& mctx,
                              RenderFrameStats* stats) {
    const auto& p = el.props;
    const int rid = el.runtimeId();
    auto& ppool = particlePools[rid];

    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);

    const int countLimit = std::clamp(getIntCI(p, "CountLimit", 400), 1, 5000);
    const float spawnTime = std::clamp(getFloatCI(p, "spawnTime", 0.015f), 0.002f, 0.5f);
    const float lifetime = std::clamp(getFloatCI(p, "lifetime", 2.2f), 0.05f, 20.0f);
    const float speedBase = getFloatCI(p, "Speed", 90.0f);
    const float speedRand = getFloatCI(p, "speedRandom", 40.0f);
    const float overallMul = std::max(0.0f, 1.0f + mvarEval.evaluateScalar(
        iota::dsp::MVarEvaluator::makeKey(rid, "MeasureOverallSpeed"),
        p.getMVar("MeasureOverallSpeed"),
        mctx));

    const auto gravity = p.getVec2("gravity", {0.0f, 0.0f});
    const float sineFreq = getFloatCI(p, "sideSineWaveFreq", 0.0f);
    const float sineMag = getFloatCI(p, "sideSineWaveMag", 0.0f);
    const bool vortex = (getStringCI(p, "ForceField", "None") == "Vortex");
    const bool mirrorX = getBoolCI(p, "mirrorX", false);
    const bool mirrorY = getBoolCI(p, "mirrorY", false);

    const float perspDepth = std::max(0.0f, getFloatCI(p, "perspectiveDepth", 0.0f));
    const float perspFov = std::clamp(getFloatCI(p, "perspectiveFov", 110.0f), 20.0f, 170.0f);

    if (static_cast<int>(ppool.pool.size()) < countLimit) {
      ppool.pool.resize(static_cast<size_t>(countLimit));
    }

    const auto* spawnArea = findPropCI(p, "SpawnArea");
    const std::string spawnKind = spawnArea && std::get_if<std::string>(&spawnArea->value)
                                      ? std::get<std::string>(spawnArea->value)
                                      : std::string("Rectangle");
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

    const bool relCenter = spawnChildBool("vectorRelativeToCenter", true);
    const bool rndDir = spawnChildBool("useRandomVectorInstead", false);
    const float vecAngleRad = spawnChildFloat("vectorAngle", 270.0f) * (kPi / 180.0f);
    const float vecAngleZ = spawnChildFloat("vectorAngleZ", 0.0f);

    const float dt = std::clamp(mctx.dt, 0.0f, 0.05f);
    ppool.spawnAccum += dt;
    while (ppool.spawnAccum >= spawnTime) {
      ppool.spawnAccum -= spawnTime;
      for (int i = 0; i < countLimit; ++i) {
        auto& pt = ppool.pool[static_cast<size_t>(i)];
        if (pt.alive) continue;

        pt.alive = true;
        pt.age = 0.0f;
        pt.maxAge = lifetime * (0.75f + 0.5f * rand01(ppool.rng));
        pt.sizeRand = rand01(ppool.rng);
        pt.waveFreq = sineFreq + getFloatCI(p, "sideSineWaveFreqRandom", 0.0f) * (rand01(ppool.rng) - 0.5f);
        pt.waveMag = sineMag + getFloatCI(p, "sideSineWaveMagRandom", 0.0f) * (rand01(ppool.rng) - 0.5f);
        pt.wavePhase = rand01(ppool.rng) * kTwoPi;

        if (spawnKind == "HorizontalLine") {
          pt.x = rect.left + rand01(ppool.rng) * rect.width();
          pt.y = rect.centerY();
          pt.z = perspDepth > 0.0f ? (rand01(ppool.rng) * perspDepth) : 0.0f;
        } else {
          pt.x = rect.left + rand01(ppool.rng) * rect.width();
          pt.y = rect.top + rand01(ppool.rng) * rect.height();
          pt.z = perspDepth > 0.0f ? (rand01(ppool.rng) * perspDepth) : 0.0f;
        }

        float dx = std::cos(vecAngleRad);
        float dy = std::sin(vecAngleRad);
        float dz = vecAngleZ;
        if (rndDir) {
          const float a = rand01(ppool.rng) * kTwoPi;
          dx = std::cos(a);
          dy = std::sin(a);
          dz = (rand01(ppool.rng) - 0.5f) * 0.5f;
        } else if (spawnKind != "HorizontalLine" && relCenter) {
          const float rx = pt.x - rect.centerX();
          const float ry = pt.y - rect.centerY();
          const float len = std::hypot(rx, ry);
          if (len > 1e-3f) {
            dx = rx / len;
            dy = ry / len;
          }
        }
        const float spd = speedBase + (rand01(ppool.rng) - 0.5f) * speedRand;
        pt.vx = dx * spd;
        pt.vy = dy * spd;
        pt.vz = dz * spd;
        break;
      }
    }

    const float scalePx = getFloatCI(p, "particleScale", 2.5f) * (std::min(width, height) * 0.004f);
    const float startSize = getFloatCI(p, "startSize", 1.0f);
    const float fadeSize = getFloatCI(p, "fadeInSize", 1.0f);
    const float endSize = getFloatCI(p, "endSize", 0.2f);
    const float fadeTimeFrac = std::clamp(getFloatCI(p, "fadeInAndOutTime", 0.25f), 0.01f, 0.49f);
    const float aspect = std::max(0.1f, getFloatCI(p, "sizeAspectRatio", 1.0f));

    const auto baseTint = getColorArgbCI(p, "color", {255, 255, 255, 255});
    const auto colFrom = getColorHslaCI(p, "ColorFrom", {0.55f, 0.8f, 0.65f, 0.85f});
    const auto colTo = getColorHslaCI(p, "ColorTo", {0.85f, 0.9f, 0.55f, 0.0f});

    const auto sprite = resolveTextureUri(project, getStringCI(p, "customImage", "internalres:particle_blur01"));
    const float nearFadeDist = std::max(1.0f, getFloatCI(p, "nearCameraFadeOutDistance", 40.0f));
    const float focalScale = 1.0f / std::max(0.15f, std::tan((perspFov * 0.5f) * (kPi / 180.0f)));

    scratchVerts.clear();
    scratchIndices.clear();

    for (int i = 0; i < countLimit; ++i) {
      auto& pt = ppool.pool[static_cast<size_t>(i)];
      if (!pt.alive) continue;

      pt.age += dt;
      if (pt.age >= pt.maxAge) {
        pt.alive = false;
        continue;
      }

      if (vortex) {
        const float rx = pt.x - rect.centerX();
        const float ry = pt.y - rect.centerY();
        const float inv = 1.0f / std::max(20.0f, std::hypot(rx, ry));
        pt.vx += (-ry * inv) * 60.0f * dt;
        pt.vy += (rx * inv) * 60.0f * dt;
      }
      pt.vx += gravity.x * dt;
      pt.vy += gravity.y * dt;

      const float lateral = std::sin(pt.age * pt.waveFreq * kTwoPi + pt.wavePhase) * pt.waveMag * 20.0f;
      const float spdLen = std::max(1e-3f, std::hypot(pt.vx, pt.vy));
      const float perpX = -pt.vy / spdLen;
      const float perpY = pt.vx / spdLen;

      pt.x += (pt.vx * overallMul + perpX * lateral) * dt;
      pt.y += (pt.vy * overallMul + perpY * lateral) * dt;
      pt.z += pt.vz * overallMul * dt;

      const float lifeT = std::clamp(pt.age / pt.maxAge, 0.0f, 1.0f);
      float envAlpha = 1.0f;
      if (lifeT < fadeTimeFrac) {
        envAlpha = lifeT / fadeTimeFrac;
      } else if (lifeT > (1.0f - fadeTimeFrac)) {
        envAlpha = (1.0f - lifeT) / fadeTimeFrac;
      }

      float szCurve = (lifeT < fadeTimeFrac)
                          ? (startSize + (fadeSize - startSize) * (lifeT / fadeTimeFrac))
                          : (fadeSize + (endSize - fadeSize) * ((lifeT - fadeTimeFrac) / (1.0f - fadeTimeFrac)));
      float perspFactor = 1.0f;
      if (perspDepth > 0.0f) {
        const float camZ = std::max(10.0f, pt.z + 120.0f);
        perspFactor = std::clamp((focalScale * 180.0f) / camZ, 0.1f, 3.5f);
        if (camZ < nearFadeDist) {
          envAlpha *= (camZ / nearFadeDist);
        }
      }

      auto hsla = iota::core::ColorHslaNorm::lerp(colFrom, colTo, lifeT);
      auto col = hsla.toRgba();
      col.r = static_cast<uint8_t>((static_cast<uint32_t>(col.r) * baseTint.r) / 255u);
      col.g = static_cast<uint8_t>((static_cast<uint32_t>(col.g) * baseTint.g) / 255u);
      col.b = static_cast<uint8_t>((static_cast<uint32_t>(col.b) * baseTint.b) / 255u);
      col.a = static_cast<uint8_t>(std::clamp(static_cast<float>(col.a) * (baseTint.a / 255.0f) * envAlpha, 0.0f, 255.0f));
      const uint32_t abgr = col.toPackedAbgrU32();

      const float halfW = std::max(1.0f, scalePx * szCurve * perspFactor);
      const float halfH = halfW * aspect;

      auto emitCopy = [&](float sx, float sy) {
        appendQuadVertices({sx - halfW, sy - halfH, sx + halfW, sy + halfH},
                           abgr,
                           sprite.isFbo,
                           scratchVerts,
                           scratchIndices);
      };
      emitCopy(pt.x, pt.y);
      if (mirrorX) emitCopy(rect.left + rect.right - pt.x, pt.y);
      if (mirrorY) emitCopy(pt.x, rect.top + rect.bottom - pt.y);
      if (mirrorX && mirrorY) emitCopy(rect.left + rect.right - pt.x, rect.top + rect.bottom - pt.y);
    }

    if (!scratchVerticesEmpty()) {
      const auto blend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "Add"));
      device->setBlendMode(blend);
      device->useShader(shBuffer);
      device->setUniformMat4(shBuffer, "u_projView", makeOrthoProjView(width, height));
      device->bindTexture(0, sprite.tex);
      device->setUniformInt(shBuffer, "u_texture", 0);
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
    const float fontSizePx = std::max(10.0f, static_cast<float>(getIntCI(p, "fontSize", 32)) * (static_cast<float>(std::min(width, height)) / 720.0f));
    const auto col = getColorArgbCI(p, "color", {255, 255, 255, 255});
    const std::string face = getStringCI(p, "typeFace", "internal_2");

    scratchVerts.clear();
    scratchIndices.clear();
    const auto tex = fontCache.buildTextQuads(face, resolved, fontSizePx, rect, col, scratchVerts, scratchIndices);
    if (tex && !scratchVerticesEmpty()) {
      const auto blend = iota::core::blendModeFromString(getStringCI(p, "blendMode", "PreMulAlpha"));
      device->setBlendMode(blend);
      device->useShader(shBuffer);
      device->setUniformMat4(shBuffer, "u_projView", projView);
      device->bindTexture(0, tex);
      device->setUniformInt(shBuffer, "u_texture", 0);
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
    if (srcIdx <= 0 || srcIdx >= static_cast<int>(compTargets.size())) {
      return;
    }

    const iota::rhi::TextureHandle srcTex = device->renderTargetColorTexture(compTargets[static_cast<size_t>(srcIdx)]);
    const float blurRadius = std::clamp(getFloatCI(p, "blurRadius", 1.5f), 0.0f, 3.0f);
    const int levels = std::clamp(static_cast<int>(std::lround(getFloatCI(p, "blurMultiplier", 3.0f))),
                                  1, static_cast<int>(blurPyramidRts.size()));

    // Downsample blur pyramid passes
    static const std::array<iota::rhi::VertexPosColUv, 4> kClipQuad = {{
        {-1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
        {+1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {+1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
        {-1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
    }};
    static const std::array<uint32_t, 6> kClipIdx = {0, 1, 2, 0, 2, 3};

    iota::rhi::TextureHandle prevTex = srcTex;
    int prevW = width;
    int prevH = height;

    device->setBlendMode(iota::core::BlendMode::Opaque);
    device->useShader(shBlurStack);

    for (int lvl = 0; lvl < levels; ++lvl) {
      auto pyrRt = blurPyramidRts[static_cast<size_t>(lvl)];
      int dstW = 2, dstH = 2;
      device->renderTargetSize(pyrRt, dstW, dstH);
      device->bindRenderTarget(pyrRt, dstW, dstH);
      device->bindTexture(0, prevTex);
      device->setUniformInt(shBlurStack, "u_texture", 0);
      device->setUniformFloat(shBlurStack, "blurWH", blurRadius * 2.0f);
      device->setUniformVec2(shBlurStack, "resolution", 1.0f / static_cast<float>(prevW),
                             1.0f / static_cast<float>(prevH));
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, kClipQuad, kClipIdx);
      if (stats) stats->drawCalls += 1;

      prevTex = device->renderTargetColorTexture(pyrRt);
      prevW = dstW;
      prevH = dstH;
    }

    // Restore composition render target
    device->bindRenderTarget(currentTargetRt, width, height);

    const LayoutSpec layout = extractLayoutSpec(project, compIdx, elemIdx, el, mvarEval, mctx);
    const iota::core::RectF rect = computeElementRect(layout, width, height);
    const glm::mat4 projView = makeRotatedOrthoProjView(
        width, height, rect.centerX(), rect.centerY(), computeTotalRotationRadians(layout));

    const std::string maskUri = getStringCI(p, "MaskImage", "");
    const ResolvedTexture maskTex = maskUri.empty()
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
                         bool invertMask) {
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
      device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
      if (stats) stats->drawCalls += 1;
    };

    const auto mainCol = getColorArgbCI(p, "color", {0, 0, 0, 255});
    const iota::core::ColorRgba zeroColor2{0, 0, 0, 0};
    if (getBoolCI(p, "showUnblurredContentUnder", false)) {
      drawLayer(srcTex, true, {1.0f, 1.0f}, {255, 255, 255, 255}, zeroColor2, contentBlend, true);
    }

    const std::array<std::pair<const char*, const char*>, 3> blurLayers = {{
        {"1layerScale", "1layerColor"},
        {"2layerScale", "2layerColor"},
        {"3layerScale", "3layerColor"},
    }};
    for (const auto& [sKey, cKey] : blurLayers) {
      const auto ls = p.getVec2(sKey, {0.0f, 0.0f});
      const auto lc = getColorArgbCI(p, cKey, {255, 255, 255, 255});
      drawLayer(prevTex, true, ls, lc, mainCol, elemBlend, false);
    }

    if (getBoolCI(p, "showUnblurredContent", true)) {
      drawLayer(srcTex, true, {1.0f, 1.0f}, {255, 255, 255, 255}, zeroColor2, contentBlend, false);
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
                              const iota::dsp::MVarEvalContext& mctx,
                              RenderFrameStats* stats) {
    const auto& p = el.props;
    const int rid = el.runtimeId();
    const auto target = resolveTextureUri(project, getStringCI(p, "TargetImage", "composition:1"));

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
        iota::dsp::MVarEvaluator::makeKey(rid, "blurAmountMultiplier"),
        p.getMVar("blurAmountMultiplier", iota::model::MVariableFloat::constant(0.5f)),
        mctx);

    iota::core::RectF trackedRect = rect;
    const auto* motionSrcProp = findPropCI(p, "motionSource");
    const std::string motionSrcKind =
        motionSrcProp && std::get_if<std::string>(&motionSrcProp->value)
            ? std::get<std::string>(motionSrcProp->value)
            : std::string("EffectTransform");
    if (motionSrcKind == "Manual" && motionSrcProp != nullptr) {
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
      LayoutSpec manualSpec;
      manualSpec.position = manualPos;
      manualSpec.scale = manualScale;
      trackedRect = computeElementRect(manualSpec, width, height);
    }

    iota::core::Vec2 posAmt{};
    iota::core::Vec2 scaleAmt{};
    if (auto prevIt = motionBlurPrevRects.find(rid); prevIt != motionBlurPrevRects.end()) {
      const auto& prev = prevIt->second;
      posAmt.x = std::clamp((trackedRect.centerX() - prev.centerX()) / static_cast<float>(width), -0.15f, 0.15f);
      posAmt.y = std::clamp((trackedRect.centerY() - prev.centerY()) / static_cast<float>(height), -0.15f, 0.15f);
      scaleAmt.x = std::clamp((trackedRect.width() - prev.width()) / static_cast<float>(width), -0.15f, 0.15f);
      scaleAmt.y = std::clamp((trackedRect.height() - prev.height()) / static_cast<float>(height), -0.15f, 0.15f);
    }
    motionBlurPrevRects[rid] = trackedRect;

    scratchVerts.clear();
    scratchIndices.clear();
    appendQuadVertices(rect, mainCol.toPackedAbgrU32(), target.isFbo, scratchVerts, scratchIndices);

    device->setBlendMode(elemBlend);
    device->useShader(shMotionBlur);
    device->setUniformMat4(shMotionBlur, "u_projView", projView);
    device->bindTexture(0, target.tex);
    device->setUniformInt(shMotionBlur, "u_texture", 0);
    device->setUniformVec2(shMotionBlur, "posAmount", posAmt.x * mult * 0.25f, posAmt.y * mult * 0.25f);
    device->setUniformVec2(shMotionBlur, "scaleAmount", scaleAmt.x * mult * 0.25f, scaleAmt.y * mult * 0.25f);
    device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, scratchVerts, scratchIndices);
    if (stats) stats->drawCalls += 1;

    if (getBoolCI(p, "showUnblurredContent", false)) {
      drawQuadWithBufferShader(target.tex, projView, rect, mainCol.toPackedAbgrU32(), contentBlend, target.isFbo, stats);
    }
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
          if (stats && !err.empty()) stats->lastError = err;
        }
        customShaders.emplace(key, created);
        sh = created;
      } else {
        sh = it->second;
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
  if (impl_->finalRt) {
    impl_->device->destroyRenderTarget(impl_->finalRt);
    impl_->finalRt = {};
  }
  for (auto& rt : impl_->blurPyramidRts) {
    if (rt) {
      impl_->device->destroyRenderTarget(rt);
      rt = {};
    }
  }
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

  for (auto& [k, sh] : impl_->customShaders) {
    if (sh != impl_->shBuffer) impl_->device->destroyShader(sh);
  }
  impl_->customShaders.clear();

  for (auto* sh : {&impl_->shBuffer, &impl_->shFxLight, &impl_->shBlurStack,
                   &impl_->shRgbSplit, &impl_->shMotionBlur, &impl_->shFxaa}) {
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
  for (auto& a : impl_->analyzers) {
    a.reset();
  }
  impl_->particlePools.clear();
  impl_->motionBlurPrevRects.clear();
}

void SceneRenderer::invalidateLocalAssets() {
  if (!impl_ || !impl_->device) return;
  for (auto& [k, t] : impl_->localTexCache) {
    impl_->device->destroyTexture(t);
  }
  impl_->localTexCache.clear();
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
  impl_->ensureRenderTargets(compCount, width, height);

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
                                   ? std::clamp((cfg.audioDurationMs * audio.sampleRate()) / 1000 + 64, 512, 32768)
                                   : std::clamp(1 << cfg.inputSizePow, 512, 32768);
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

  iota::dsp::MVarEvalContext mctx;
  mctx.dt = timing.dt;
  mctx.totalTimeSec = timing.totalTimeSec;
  mctx.playAccumTimeSec = timing.playAccumTimeSec;
  mctx.trackPositionSec = queryTime;
  mctx.trackDurationSec = audio.durationSeconds();
  mctx.beat = outStats ? outStats->beat : impl_->analyzers[0].lastResult().beat;
  mctx.peakBarIndex = outStats ? outStats->peakBarIndex : impl_->analyzers[0].lastResult().peakBarIndex;
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
          impl_->renderImageElement(project, compIdx, elemIdx, el, mctx, outStats);
          break;
        case iota::model::ElementType::Bars:
          impl_->renderBarsElement(project, compIdx, elemIdx, el, mctx, providerSlot, outStats);
          break;
        case iota::model::ElementType::Particles:
          impl_->renderParticlesElement(project, compIdx, elemIdx, el, mctx, outStats);
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
          impl_->renderMotionBlurEffect(project, compIdx, elemIdx, el, mctx, outStats);
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
  if (timing.fxaaEnabled) {
    static const std::array<iota::rhi::VertexPosColUv, 4> kClipQuad = {{
        {-1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
        {+1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {+1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
        {-1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
    }};
    static const std::array<uint32_t, 6> kClipIdx = {0, 1, 2, 0, 2, 3};

    impl_->device->bindRenderTarget(dstRt, impl_->width, impl_->height);
    impl_->device->setBlendMode(iota::core::BlendMode::Opaque);
    impl_->device->useShader(impl_->shFxaa);
    impl_->device->bindTexture(0, impl_->device->renderTargetColorTexture(comp0Rt));
    impl_->device->setUniformInt(impl_->shFxaa, "u_texture", 0);
    impl_->device->setUniformFloat(impl_->shFxaa, "resolutionW", static_cast<float>(impl_->width));
    impl_->device->setUniformFloat(impl_->shFxaa, "resolutionH", static_cast<float>(impl_->height));
    impl_->device->drawIndexed(iota::rhi::PrimitiveTopology::Triangles, kClipQuad, kClipIdx);
    if (outStats) outStats->drawCalls += 1;
    impl_->activeFinalRt = dstRt;
  } else if (outputTarget && outputTarget != comp0Rt) {
    impl_->device->bindRenderTarget(outputTarget, impl_->width, impl_->height);
    impl_->drawQuadWithBufferShader(impl_->device->renderTargetColorTexture(comp0Rt),
                                    makeOrthoProjView(impl_->width, impl_->height),
                                    {0.0f, 0.0f, static_cast<float>(impl_->width), static_cast<float>(impl_->height)},
                                    0xFFFFFFFFu,
                                    iota::core::BlendMode::Opaque,
                                    true,
                                    outStats);
    impl_->activeFinalRt = outputTarget;
  } else {
    impl_->activeFinalRt = comp0Rt;
  }

  impl_->device->bindRenderTarget({}, 0, 0);
  return impl_->activeFinalRt;
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
