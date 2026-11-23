#include "doctest/doctest.h"

#include "iota/engine/particle_system.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

TEST_CASE("ParticleSystem 3D curl noise, emitter shapes, physics, DoF, and Aux trails") {
  using namespace iota::engine;

  const iota::core::RectF emitterRect{240.0f, 160.0f, 400.0f, 320.0f};
  constexpr int kW = 640;
  constexpr int kH = 480;

  SUBCASE("curlNoise3D is finite, non-zero, and varies smoothly in space") {
    const auto c0 = curlNoise3D(1.25f, -0.75f, 0.50f, 3);
    const auto c1 = curlNoise3D(1.252f, -0.75f, 0.50f, 3);
    const auto c2 = curlNoise3D(4.75f, 3.10f, -2.20f, 3);
    CHECK(std::isfinite(c0.x));
    CHECK(std::isfinite(c0.y));
    CHECK(std::isfinite(c0.z));
    CHECK(std::hypot(c0.x, std::hypot(c0.y, c0.z)) > 0.01f);
    const float nearDiff = std::hypot(c0.x - c1.x, std::hypot(c0.y - c1.y, c0.z - c1.z));
    const float farDiff = std::hypot(c0.x - c2.x, std::hypot(c0.y - c2.y, c0.z - c2.z));
    CHECK(nearDiff < 0.06f);
    CHECK(farDiff > 0.05f);
  }

  SUBCASE("Frame-rate-independent emission count and exponential air drag (60 FPS vs 144 FPS)") {
    auto runSim = [&](int steps, float dt, bool singleWithDrag) {
      ParticleSystem sys;
      ParticleSimConfig cfg;
      cfg.emitterType = ParticleEmitterType::Point;
      cfg.maxParticles = singleWithDrag ? 1 : 500;
      cfg.particlesPerSec = singleWithDrag ? 300.0f : 120.0f;
      cfg.lifetime = 5.0f;
      cfg.lifetimeRandom = 0.0f;
      cfg.speedBase = 240.0f;
      cfg.speedRandom = 0.0f;
      cfg.directionSpreadDeg = 0.0f;
      cfg.vectorRelativeToCenter = false;
      cfg.vectorAngleRad = 0.0f;  // +X rightward
      cfg.airResistance = singleWithDrag ? 2.0f : 0.0f;
      cfg.colorFrom = {0.5f, 1.0f, 0.5f, 1.0f};
      cfg.colorTo = {0.5f, 1.0f, 0.5f, 1.0f};
      cfg.opacityOverLife = ParticleCurvePreset::Constant;

      for (int i = 0; i < steps; ++i) {
        sys.step(cfg, emitterRect, kW, kH, dt);
      }
      std::vector<iota::rhi::VertexPosColUv> verts;
      std::vector<uint32_t> idx;
      sys.buildGeometry(cfg, emitterRect, kW, kH, false, false, verts, idx);
      float avgCenterX = 0.0f;
      if (!verts.empty()) {
        avgCenterX = 0.25f * (verts[0].x + verts[1].x + verts[2].x + verts[3].x);
      }
      return std::pair<int, float>{sys.activeCount(), avgCenterX};
    };

    // Emission count over 0.5 s: 30 steps @ 60 FPS vs 72 steps @ 144 FPS
    const auto [cnt60, unused60] = runSim(30, 1.0f / 60.0f, false);
    const auto [cnt144, unused144] = runSim(72, 1.0f / 144.0f, false);
    (void)unused60;
    (void)unused144;
    CHECK(cnt60 >= 58);
    CHECK(cnt60 <= 62);
    CHECK(std::abs(cnt60 - cnt144) <= 1);

    // Exponential airResistance = 2.0/s over T = 0.5s should match closely at 60 & 144 FPS
    const auto [c60, x60] = runSim(30, 1.0f / 60.0f, true);
    const auto [c144, x144] = runSim(72, 1.0f / 144.0f, true);
    REQUIRE(c60 == 1);
    REQUIRE(c144 == 1);
    const float disp60 = x60 - emitterRect.centerX();
    const float disp144 = x144 - emitterRect.centerX();
    // Undamped 240 * 0.5 = 120 px; damped (240/2)*(1 - e^-1) = ~75.8 px
    CHECK(disp60 > 65.0f);
    CHECK(disp60 < 90.0f);
    CHECK(std::abs(disp60 - disp144) < 3.0f);
  }

  SUBCASE("PreRun warm-up, LayerMask color inheritance, Aux trails, Bounce, and maxParticles cap") {
    ParticleSystem sys;
    ParticleSimConfig cfg;
    cfg.maxParticles = 350;
    cfg.emitterType = ParticleEmitterType::LayerMask;
    cfg.particlesPerSec = 180.0f;
    cfg.preRunSec = 0.8f;
    cfg.lifetime = 0.9f;
    cfg.speedBase = 160.0f;
    cfg.gravity = {0.0f, 350.0f};
    cfg.bounceEnabled = true;
    cfg.bounceFloorY = 0.72f;
    cfg.bounceRestitution = 0.7f;
    cfg.turbulenceAffectPos = 45.0f;
    cfg.turbulenceAffectSize = 0.25f;
    cfg.auxMode = ParticleAuxMode::Continuously;
    cfg.auxParticlesPerSec = 20.0f;
    cfg.auxLifetime = 0.45f;
    cfg.auxColorFromMain = 1.0f;
    cfg.colorFrom = {0.0f, 0.0f, 1.0f, 1.0f};  // White multiplier to preserve sample RGB
    cfg.colorTo = {0.0f, 0.0f, 1.0f, 1.0f};
    cfg.opacityOverLife = ParticleCurvePreset::Constant;

    // Bright green emitter mask samples
    const std::vector<EmitterSamplePoint> greenSamples = {
        {0.50f, 0.50f, 20, 240, 30, 255},
        {0.55f, 0.48f, 10, 250, 20, 255},
    };

    // Pre-run immediately populates particles + aux on step 0
    sys.step(cfg, emitterRect, kW, kH, 1.0f / 60.0f, greenSamples);
    CHECK(sys.activeCount() > 50);
    CHECK(sys.activeCount() <= cfg.maxParticles);
    CHECK(sys.auxCount() > 0);

    std::vector<iota::rhi::VertexPosColUv> verts;
    std::vector<uint32_t> idx;
    sys.buildGeometry(cfg, emitterRect, kW, kH, false, false, verts, idx);
    REQUIRE(!verts.empty());
    // Verify particles inherited green sample color (G >> R and G >> B)
    const uint32_t abgr = verts[0].abgr;
    const uint8_t r = static_cast<uint8_t>(abgr & 0xFFu);
    const uint8_t g = static_cast<uint8_t>((abgr >> 8) & 0xFFu);
    const uint8_t b = static_cast<uint8_t>((abgr >> 16) & 0xFFu);
    CHECK(g > r * 3);
    CHECK(g > b * 3);

    // AtDeath aux burst test
    ParticleSystem deathSys;
    ParticleSimConfig deathCfg;
    deathCfg.emitterType = ParticleEmitterType::Point;
    deathCfg.maxParticles = 64;
    deathCfg.particlesPerSec = 100.0f;
    deathCfg.lifetime = 0.06f;
    deathCfg.lifetimeRandom = 0.0f;
    deathCfg.auxMode = ParticleAuxMode::AtDeath;
    deathCfg.auxBurstCount = 4;
    deathCfg.auxLifetime = 0.5f;
    for (int f = 0; f < 6; ++f) {
      deathSys.step(deathCfg, emitterRect, kW, kH, 1.0f / 60.0f);
    }
    CHECK(deathSys.auxCount() > 0);
    CHECK(deathSys.activeCount() <= deathCfg.maxParticles);
  }

  SUBCASE("3D Camera orbit, DoF Circle-of-Confusion vertex softness, and streak elongation") {
    ParticleSystem sys;
    ParticleSimConfig cfg;
    cfg.emitterType = ParticleEmitterType::Box3D;
    cfg.emitterSizeZ = 600.0f;
    cfg.maxParticles = 120;
    cfg.particlesPerSec = 240.0f;
    cfg.preRunSec = 0.5f;
    cfg.speedBase = 180.0f;
    cfg.trailLength = 1.5f;
    cfg.camera3DEnabled = true;
    cfg.cameraDistance = 420.0f;
    cfg.cameraOrbitYawDeg = 25.0f;
    cfg.cameraOrbitPitchDeg = -10.0f;
    cfg.dofEnabled = true;
    cfg.dofFocusDist = 420.0f;
    cfg.dofAperture = 0.85f;
    cfg.dofMaxBlur = 0.90f;
    cfg.opacityOverLife = ParticleCurvePreset::Constant;

    sys.step(cfg, emitterRect, kW, kH, 1.0f / 60.0f);
    std::vector<iota::rhi::VertexPosColUv> verts;
    std::vector<uint32_t> idx;
    sys.buildGeometry(cfg, emitterRect, kW, kH, false, true, verts, idx);
    REQUIRE(verts.size() >= 16);

    float minCoc = 1.0f;
    float maxCoc = 0.0f;
    float maxAspectRatio = 0.0f;
    for (size_t i = 0; i + 3 < verts.size(); i += 4) {
      minCoc = std::min(minCoc, verts[i].z);
      maxCoc = std::max(maxCoc, verts[i].z);
      const float quadW = std::hypot(verts[i + 1].x - verts[i].x, verts[i + 1].y - verts[i].y);
      const float quadH = std::hypot(verts[i + 3].x - verts[i].x, verts[i + 3].y - verts[i].y);
      if (quadH > 0.1f) {
        maxAspectRatio = std::max(maxAspectRatio, quadW / quadH);
      }
    }
    CHECK(minCoc < 0.25f);
    CHECK(maxCoc > 0.10f);
    CHECK(maxAspectRatio > 1.3f);  // Velocity trail stretched quads longer along motion
  }
}
