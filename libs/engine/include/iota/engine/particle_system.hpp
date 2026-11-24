#pragma once

#include "iota/core/types.hpp"
#include "iota/rhi/rhi.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace iota::engine {

enum class ParticleEmitterType {
  Rectangle = 0,
  HorizontalLine = 1,
  Point = 2,
  Box3D = 3,
  Sphere = 4,
  Ring = 5,
  Grid = 6,
  LayerMask = 7,
};

enum class ParticleShapeType {
  Sprite = 0,
  Sphere = 1,
  GlowSphere = 2,
  Star = 3,
  BokehRing = 4,
  Streaklet = 5,
};

enum class ParticleCurvePreset {
  Linear = 0,
  FadeInOut = 1,
  EaseOut = 2,
  EaseIn = 3,
  Pulse = 4,
  Constant = 5,
};

enum class ParticleForceFieldType {
  None = 0,
  Vortex = 1,
  Attract = 2,
  Repel = 3,
  TurbulentVortex = 4,
};

enum class ParticleAuxMode {
  Off = 0,
  Continuously = 1,
  AtBounce = 2,
  AtDeath = 3,
};

[[nodiscard]] ParticleEmitterType particleEmitterTypeFromString(std::string_view s) noexcept;
[[nodiscard]] ParticleShapeType particleShapeTypeFromString(std::string_view s) noexcept;
[[nodiscard]] ParticleCurvePreset particleCurvePresetFromString(std::string_view s) noexcept;
[[nodiscard]] ParticleForceFieldType particleForceFieldFromString(std::string_view s) noexcept;
[[nodiscard]] ParticleAuxMode particleAuxModeFromString(std::string_view s) noexcept;

[[nodiscard]] float evaluateParticleCurve(ParticleCurvePreset preset, float lifeT, float fadeFrac) noexcept;

/// Deterministic 3D divergence-free curl noise velocity vector (smooth spatial turbulence).
[[nodiscard]] iota::core::Vec3 curlNoise3D(float x, float y, float z, int octaves = 2) noexcept;

struct EmitterSamplePoint {
  float u = 0.5f;  // Normalized [0, 1] left-to-right
  float v = 0.5f;  // Normalized [0, 1] top-to-bottom
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
  uint8_t a = 255;
};

struct ParticleSimConfig {
  // Emitter settings
  int maxParticles = 400;
  ParticleEmitterType emitterType = ParticleEmitterType::Rectangle;
  float spawnTime = 0.015f;
  float particlesPerSec = 0.0f;       // > 0 overrides 1 / spawnTime
  float emitRateMultiplier = 1.0f;
  float velocityFromMotion = 0.0f;
  float speedBase = 90.0f;
  float speedRandom = 40.0f;
  float overallSpeedMultiplier = 1.0f;
  float directionSpreadDeg = 35.0f;
  bool vectorRelativeToCenter = true;
  bool useRandomVector = false;
  float vectorAngleRad = 4.71238898f;  // 270 deg (upward in screen Y)
  float vectorAngleZ = 0.0f;
  float emitterSizeZ = 0.0f;
  float ringRadiusFrac = 0.7f;
  int gridCountX = 8;
  int gridCountY = 8;
  float preRunSec = 0.0f;

  // Particle appearance & over-life curves
  ParticleShapeType shapeType = ParticleShapeType::Sprite;
  float feather = 0.5f;
  float lifetime = 2.2f;
  float lifetimeRandom = 0.25f;
  float particleScale = 2.5f;
  float startSize = 1.0f;
  float fadeInSize = 1.0f;
  float endSize = 0.2f;
  float sizeRandom = 0.2f;
  float endSizeRandom = 0.0f;
  float endSizeZFactor = 0.0f;
  float sizeAspectRatio = 1.0f;
  float fadeInAndOutTime = 0.25f;
  ParticleCurvePreset sizeOverLife = ParticleCurvePreset::Linear;
  ParticleCurvePreset opacityOverLife = ParticleCurvePreset::FadeInOut;
  iota::core::ColorRgba baseTint{255, 255, 255, 255};
  iota::core::ColorHslaNorm colorFrom{0.55f, 0.80f, 0.65f, 0.85f};
  iota::core::ColorHslaNorm colorMid{0.70f, 0.90f, 0.65f, 0.90f};
  iota::core::ColorHslaNorm colorTo{0.85f, 0.90f, 0.55f, 0.00f};
  bool useMidColor = false;
  float colorRandomHue = 0.0f;
  float trailLength = 0.0f;
  bool velocityAngle = true;
  bool initialRotationRandom = false;
  float spinSpeedDeg = 0.0f;
  float spinRandomDeg = 0.0f;

  // Physics & environment
  iota::core::Vec2 gravity{0.0f, 0.0f};
  float gravityZ = 0.0f;
  iota::core::Vec3 wind{0.0f, 0.0f, 0.0f};
  float airResistance = 0.0f;
  float sideSineWaveFreq = 0.0f;
  float sideSineWaveFreqRandom = 0.0f;
  float sideSineWaveMag = 0.0f;
  float sideSineWaveMagRandom = 0.0f;
  ParticleForceFieldType forceField = ParticleForceFieldType::None;
  float forceFieldStrength = 60.0f;
  float forceFieldRadius = 400.0f;
  bool bounceEnabled = false;
  float bounceFloorY = 0.9f;  // Normalized [0, 1] viewport height
  float bounceRestitution = 0.6f;
  float bounceFriction = 0.2f;
  bool mirrorX = false;
  bool mirrorY = false;

  // 3D Curl Turbulence field
  float turbulenceAffectPos = 0.0f;
  float turbulenceAffectSize = 0.0f;
  float turbulenceAffectOpacity = 0.0f;
  float turbulenceScale = 1.5f;
  float turbulenceEvolution = 0.6f;
  int turbulenceOctaves = 2;
  float turbulenceFadeIn = 0.35f;

  // Aux system (secondary trailing / burst sub-particles)
  ParticleAuxMode auxMode = ParticleAuxMode::Off;
  float auxParticlesPerSec = 12.0f;
  int auxBurstCount = 6;
  float auxLifetime = 0.6f;
  float auxSizeScale = 0.45f;
  float auxVelocityScale = 0.08f;
  float auxColorFromMain = 0.75f;
  iota::core::ColorHslaNorm auxColor{0.12f, 0.95f, 0.65f, 0.80f};
  float auxOpacity = 0.65f;

  // 3D Camera & Depth of Field (DoF Bokeh)
  bool camera3DEnabled = false;
  float perspectiveDepth = 0.0f;
  float perspectiveFov = 110.0f;
  float cameraDistance = 420.0f;
  float cameraOrbitYawDeg = 0.0f;
  float cameraOrbitPitchDeg = 0.0f;
  float nearCameraFadeOutDistance = 40.0f;
  bool dofEnabled = false;
  float dofFocusDist = 420.0f;
  float dofAperture = 0.35f;
  float dofMaxBlur = 0.85f;
};

/// Trapcode Particular-inspired 3D particle system with divergence-free curl turbulence,
/// multiple 3D emitter geometries, LayerMask sampling, over-life curves, Aux sub-particles,
/// and 3D camera + Depth-of-Field projection.
class ParticleSystem {
 public:
  void reset() noexcept;

  void step(const ParticleSimConfig& cfg,
            const iota::core::RectF& emitterRect,
            int viewW,
            int viewH,
            float dtSec,
            std::span<const EmitterSamplePoint> layerSamples = {});

  void buildGeometry(const ParticleSimConfig& cfg,
                     const iota::core::RectF& emitterRect,
                     int viewW,
                     int viewH,
                     bool spriteIsFbo,
                     bool sortBackToFront,
                     std::vector<iota::rhi::VertexPosColUv>& outVerts,
                     std::vector<uint32_t>& outIndices) const;

  [[nodiscard]] int activeCount() const noexcept { return static_cast<int>(particles_.size()); }
  [[nodiscard]] int auxCount() const noexcept { return auxCount_; }

 private:
  struct Particle {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    float age = 0.0f;
    float maxAge = 2.0f;
    float sizeRand = 0.0f;
    float endSizeRand = 0.0f;
    float hueShift = 0.0f;
    float rotation = 0.0f;
    float spinSpeed = 0.0f;
    float waveFreq = 0.0f;
    float waveMag = 0.0f;
    float wavePhase = 0.0f;
    float turbSizeMod = 1.0f;
    float turbAlphaMod = 1.0f;
    float auxAccum = 0.0f;
    uint32_t customAbgr = 0xFFFFFFFFu;
    bool hasCustomColor = false;
    bool isAux = false;
  };

  void stepInternal(const ParticleSimConfig& cfg,
                    const iota::core::RectF& emitterRect,
                    int viewW,
                    int viewH,
                    float dtSec,
                    std::span<const EmitterSamplePoint> layerSamples);

  void spawnPrimary(const ParticleSimConfig& cfg,
                    const iota::core::RectF& emitterRect,
                    iota::core::Vec2 emitterVel,
                    std::span<const EmitterSamplePoint> layerSamples);

  void spawnAuxFrom(const ParticleSimConfig& cfg,
                    const Particle& parent,
                    float extraJitterSpeed,
                    int maxCap,
                    std::vector<Particle>& outAux);

  struct DrawItem {
    float camZ = 0.0f;
    float sx = 0.0f;
    float sy = 0.0f;
    float svx = 0.0f;
    float svy = 0.0f;
    float halfW = 1.0f;
    float halfH = 1.0f;
    float angle = 0.0f;
    float coc = 0.0f;
    uint32_t abgr = 0xFFFFFFFFu;
  };

  uint32_t rng_ = 1337u;
  float spawnAccum_ = 0.0f;
  float simTime_ = 0.0f;
  uint32_t gridCursor_ = 0;
  iota::core::Vec2 prevEmitterCenter_{};
  bool hasPrevEmitterPos_ = false;
  bool preRunDone_ = false;
  int auxCount_ = 0;
  std::vector<Particle> particles_;
  std::vector<Particle> pendingAux_;
  mutable std::vector<DrawItem> scratchDrawItems_;
};

}  // namespace iota::engine
