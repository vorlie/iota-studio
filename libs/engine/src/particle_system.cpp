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

#include "iota/engine/particle_system.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace iota::engine {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

bool eqCI(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
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

float randSym(uint32_t& state) noexcept {
  return rand01(state) * 2.0f - 1.0f;
}

float smooth5(float t) noexcept {
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float smooth5Deriv(float t) noexcept {
  const float u = t - 1.0f;
  return 30.0f * t * t * u * u;
}

struct GradSample3D {
  float val = 0.0f;
  float gx = 0.0f;
  float gy = 0.0f;
  float gz = 0.0f;
};

GradSample3D hashGrad3DWithDeriv(int ix, int iy, int iz, float fx, float fy, float fz) noexcept {
  uint32_t h = static_cast<uint32_t>(ix) * 374761393u +
               static_cast<uint32_t>(iy) * 668265263u +
               static_cast<uint32_t>(iz) * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= (h >> 16);
  const int g = static_cast<int>(h & 15u);
  const float su = ((g & 1) == 0) ? 1.0f : -1.0f;
  const float sv = ((g & 2) == 0) ? 1.0f : -1.0f;

  float gx = 0.0f;
  float gy = 0.0f;
  float gz = 0.0f;
  if (g < 8) {
    gx += su;
  } else {
    gy += su;
  }
  if (g < 4) {
    gy += sv;
  } else if (g == 12 || g == 14) {
    gx += sv;
  } else {
    gz += sv;
  }
  return {gx * fx + gy * fy + gz * fz, gx, gy, gz};
}

struct NoiseDeriv3D {
  float dx = 0.0f;
  float dy = 0.0f;
  float dz = 0.0f;
};

NoiseDeriv3D gradientNoise3DDeriv(float x, float y, float z) noexcept {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const int iz = static_cast<int>(std::floor(z));
  const float fx = x - static_cast<float>(ix);
  const float fy = y - static_cast<float>(iy);
  const float fz = z - static_cast<float>(iz);

  const float ux = smooth5(fx);
  const float uy = smooth5(fy);
  const float uz = smooth5(fz);
  const float dux = smooth5Deriv(fx);
  const float duy = smooth5Deriv(fy);
  const float duz = smooth5Deriv(fz);

  const auto c000 = hashGrad3DWithDeriv(ix, iy, iz, fx, fy, fz);
  const auto c100 = hashGrad3DWithDeriv(ix + 1, iy, iz, fx - 1.0f, fy, fz);
  const auto c010 = hashGrad3DWithDeriv(ix, iy + 1, iz, fx, fy - 1.0f, fz);
  const auto c110 = hashGrad3DWithDeriv(ix + 1, iy + 1, iz, fx - 1.0f, fy - 1.0f, fz);
  const auto c001 = hashGrad3DWithDeriv(ix, iy, iz + 1, fx, fy, fz - 1.0f);
  const auto c101 = hashGrad3DWithDeriv(ix + 1, iy, iz + 1, fx - 1.0f, fy, fz - 1.0f);
  const auto c011 = hashGrad3DWithDeriv(ix, iy + 1, iz + 1, fx, fy - 1.0f, fz - 1.0f);
  const auto c111 = hashGrad3DWithDeriv(ix + 1, iy + 1, iz + 1, fx - 1.0f, fy - 1.0f, fz - 1.0f);

  const float x00 = c000.val + (c100.val - c000.val) * ux;
  const float x10 = c010.val + (c110.val - c010.val) * ux;
  const float x01 = c001.val + (c101.val - c001.val) * ux;
  const float x11 = c011.val + (c111.val - c011.val) * ux;

  const float dx00_dx = c000.gx + (c100.gx - c000.gx) * ux + (c100.val - c000.val) * dux;
  const float dx10_dx = c010.gx + (c110.gx - c010.gx) * ux + (c110.val - c010.val) * dux;
  const float dx01_dx = c001.gx + (c101.gx - c001.gx) * ux + (c101.val - c001.val) * dux;
  const float dx11_dx = c011.gx + (c111.gx - c011.gx) * ux + (c111.val - c011.val) * dux;

  const float dx00_dy = c000.gy + (c100.gy - c000.gy) * ux;
  const float dx10_dy = c010.gy + (c110.gy - c010.gy) * ux;
  const float dx01_dy = c001.gy + (c101.gy - c001.gy) * ux;
  const float dx11_dy = c011.gy + (c111.gy - c011.gy) * ux;

  const float dx00_dz = c000.gz + (c100.gz - c000.gz) * ux;
  const float dx10_dz = c010.gz + (c110.gz - c010.gz) * ux;
  const float dx01_dz = c001.gz + (c101.gz - c001.gz) * ux;
  const float dx11_dz = c011.gz + (c111.gz - c011.gz) * ux;

  const float y0 = x00 + (x10 - x00) * uy;
  const float y1 = x01 + (x11 - x01) * uy;

  const float dy0_dx = dx00_dx + (dx10_dx - dx00_dx) * uy;
  const float dy1_dx = dx01_dx + (dx11_dx - dx01_dx) * uy;

  const float dy0_dy = dx00_dy + (dx10_dy - dx00_dy) * uy + (x10 - x00) * duy;
  const float dy1_dy = dx01_dy + (dx11_dy - dx01_dy) * uy + (x11 - x01) * duy;

  const float dy0_dz = dx00_dz + (dx10_dz - dx00_dz) * uy;
  const float dy1_dz = dx01_dz + (dx11_dz - dx01_dz) * uy;

  return {
      dy0_dx + (dy1_dx - dy0_dx) * uz,
      dy0_dy + (dy1_dy - dy0_dy) * uz,
      dy0_dz + (dy1_dz - dy0_dz) * uz + (y1 - y0) * duz,
  };
}

iota::core::Vec3 normalizeSafe3(iota::core::Vec3 v, iota::core::Vec3 fallback = {0.0f, -1.0f, 0.0f}) noexcept {
  const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len <= 1e-5f) return fallback;
  const float inv = 1.0f / len;
  return {v.x * inv, v.y * inv, v.z * inv};
}

iota::core::Vec3 perturbDirectionCone(iota::core::Vec3 baseDir, float spreadRad, uint32_t& rng) noexcept {
  if (spreadRad <= 1e-4f) {
    return baseDir;
  }
  const iota::core::Vec3 w = normalizeSafe3(baseDir);
  const iota::core::Vec3 ref = (std::abs(w.x) > 0.85f)
                                   ? iota::core::Vec3{0.0f, 1.0f, 0.0f}
                                   : iota::core::Vec3{1.0f, 0.0f, 0.0f};
  // u = normalize(cross(ref, w))
  const iota::core::Vec3 u = normalizeSafe3({
      ref.y * w.z - ref.z * w.y,
      ref.z * w.x - ref.x * w.z,
      ref.x * w.y - ref.y * w.x,
  });
  // v = cross(w, u)
  const iota::core::Vec3 v{
      w.y * u.z - w.z * u.y,
      w.z * u.x - w.x * u.z,
      w.x * u.y - w.y * u.x,
  };

  // Uniform cone angle sampling up to spreadRad
  const float cosMax = std::cos(std::clamp(spreadRad, 0.0f, kPi));
  const float cosTheta = 1.0f - rand01(rng) * (1.0f - cosMax);
  const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
  const float phi = rand01(rng) * kTwoPi;
  const float ux = std::cos(phi) * sinTheta;
  const float vy = std::sin(phi) * sinTheta;

  return normalizeSafe3({
      u.x * ux + v.x * vy + w.x * cosTheta,
      u.y * ux + v.y * vy + w.y * cosTheta,
      u.z * ux + v.z * vy + w.z * cosTheta,
  });
}

iota::core::Vec3 rotateYawPitch(iota::core::Vec3 p, float cy, float sy, float cp, float sp) noexcept {
  const float x1 = p.x * cy + p.z * sy;
  const float z1 = -p.x * sy + p.z * cy;
  const float y1 = p.y;
  return {
      x1,
      y1 * cp - z1 * sp,
      y1 * sp + z1 * cp,
  };
}

}  // namespace

ParticleEmitterType particleEmitterTypeFromString(std::string_view s) noexcept {
  if (eqCI(s, "HorizontalLine")) return ParticleEmitterType::HorizontalLine;
  if (eqCI(s, "Point")) return ParticleEmitterType::Point;
  if (eqCI(s, "Box3D")) return ParticleEmitterType::Box3D;
  if (eqCI(s, "Sphere")) return ParticleEmitterType::Sphere;
  if (eqCI(s, "Ring")) return ParticleEmitterType::Ring;
  if (eqCI(s, "Grid")) return ParticleEmitterType::Grid;
  if (eqCI(s, "LayerMask")) return ParticleEmitterType::LayerMask;
  return ParticleEmitterType::Rectangle;
}

ParticleShapeType particleShapeTypeFromString(std::string_view s) noexcept {
  if (eqCI(s, "Sphere")) return ParticleShapeType::Sphere;
  if (eqCI(s, "GlowSphere")) return ParticleShapeType::GlowSphere;
  if (eqCI(s, "Star")) return ParticleShapeType::Star;
  if (eqCI(s, "BokehRing")) return ParticleShapeType::BokehRing;
  if (eqCI(s, "Streaklet")) return ParticleShapeType::Streaklet;
  return ParticleShapeType::Sprite;
}

ParticleCurvePreset particleCurvePresetFromString(std::string_view s) noexcept {
  if (eqCI(s, "FadeInOut")) return ParticleCurvePreset::FadeInOut;
  if (eqCI(s, "EaseOut")) return ParticleCurvePreset::EaseOut;
  if (eqCI(s, "EaseIn")) return ParticleCurvePreset::EaseIn;
  if (eqCI(s, "Pulse")) return ParticleCurvePreset::Pulse;
  if (eqCI(s, "Constant")) return ParticleCurvePreset::Constant;
  if (eqCI(s, "HoldFadeOut")) return ParticleCurvePreset::HoldFadeOut;
  return ParticleCurvePreset::Linear;
}

ParticleForceFieldType particleForceFieldFromString(std::string_view s) noexcept {
  if (eqCI(s, "Vortex")) return ParticleForceFieldType::Vortex;
  if (eqCI(s, "Attract")) return ParticleForceFieldType::Attract;
  if (eqCI(s, "Repel")) return ParticleForceFieldType::Repel;
  if (eqCI(s, "TurbulentVortex")) return ParticleForceFieldType::TurbulentVortex;
  return ParticleForceFieldType::None;
}

ParticleAuxMode particleAuxModeFromString(std::string_view s) noexcept {
  if (eqCI(s, "Continuously")) return ParticleAuxMode::Continuously;
  if (eqCI(s, "AtBounce")) return ParticleAuxMode::AtBounce;
  if (eqCI(s, "AtDeath")) return ParticleAuxMode::AtDeath;
  return ParticleAuxMode::Off;
}

float evaluateParticleCurve(ParticleCurvePreset preset, float lifeT, float fadeFrac) noexcept {
  const float t = std::clamp(lifeT, 0.0f, 1.0f);
  const float f = std::clamp(fadeFrac, 0.01f, 0.49f);
  switch (preset) {
    case ParticleCurvePreset::Constant:
      return 1.0f;
    case ParticleCurvePreset::HoldFadeOut: {
      const float tail = std::clamp(fadeFrac, 0.01f, 0.95f);
      const float startFade = 1.0f - tail;
      if (t <= startFade) return 1.0f;
      const float u = std::clamp((t - startFade) / tail, 0.0f, 1.0f);
      return 1.0f - u * u * (3.0f - 2.0f * u);
    }
    case ParticleCurvePreset::FadeInOut:
      if (t < f) return t / f;
      if (t > (1.0f - f)) return (1.0f - t) / f;
      return 1.0f;
    case ParticleCurvePreset::EaseOut:
      return 1.0f - t * t;
    case ParticleCurvePreset::EaseIn: {
      const float r = 1.0f - t;
      return 1.0f - r * r;
    }
    case ParticleCurvePreset::Pulse:
      return std::sin(t * kPi);
    case ParticleCurvePreset::Linear:
    default:
      return 1.0f - t;
  }
}

iota::core::Vec3 curlNoise3D(float x, float y, float z, int octaves) noexcept {
  const int oct = std::clamp(octaves, 1, 3);
  iota::core::Vec3 curlAcc{};
  float amp = 1.0f;
  float freq = 1.0f;
  float norm = 0.0f;
  for (int o = 0; o < oct; ++o) {
    const float px = x * freq;
    const float py = y * freq;
    const float pz = z * freq;
    const auto dPx = gradientNoise3DDeriv(px, py, pz);
    const auto dPy = gradientNoise3DDeriv(px + 31.416f, py - 17.321f, pz + 47.853f);
    const auto dPz = gradientNoise3DDeriv(px - 19.732f, py + 53.197f, pz - 11.415f);
    const float scale = amp * freq;
    curlAcc.x += (dPz.dy - dPy.dz) * scale;
    curlAcc.y += (dPx.dz - dPz.dx) * scale;
    curlAcc.z += (dPy.dx - dPx.dy) * scale;
    norm += amp;
    amp *= 0.5f;
    freq *= 2.03f;
  }
  const float inv = norm > 0.0f ? (1.0f / norm) : 1.0f;
  return curlAcc * inv;
}

void ParticleSystem::reset() noexcept {
  rng_ = 1337u;
  spawnAccum_ = 0.0f;
  simTime_ = 0.0f;
  gridCursor_ = 0;
  prevEmitterCenter_ = {};
  hasPrevEmitterPos_ = false;
  preRunDone_ = false;
  auxCount_ = 0;
  particles_.clear();
  pendingAux_.clear();
}

void ParticleSystem::spawnPrimary(const ParticleSimConfig& cfg,
                                  const iota::core::RectF& emitterRect,
                                  iota::core::Vec2 emitterVel,
                                  std::span<const EmitterSamplePoint> layerSamples) {
  const int maxCap = std::clamp(cfg.maxParticles, 1, 50000);
  if (static_cast<int>(particles_.size()) >= maxCap) {
    return;
  }

  Particle pt;
  const float baseLife = std::clamp(cfg.lifetime, 0.05f, 30.0f);
  const float lifeRandAmt = std::clamp(cfg.lifetimeRandom, 0.0f, 0.95f);
  pt.maxAge = std::max(0.05f, baseLife * (1.0f + randSym(rng_) * lifeRandAmt));
  pt.sizeRand = rand01(rng_);
  pt.endSizeRand = rand01(rng_);
  pt.opacityRand = rand01(rng_);
  pt.hueShift = randSym(rng_) * std::clamp(cfg.colorRandomHue, 0.0f, 1.0f) * 0.5f;
  pt.rotation = cfg.initialRotationRandom ? (rand01(rng_) * kTwoPi) : 0.0f;
  pt.spinSpeed = (cfg.spinSpeedDeg + randSym(rng_) * cfg.spinRandomDeg) * (kPi / 180.0f);
  pt.waveFreq = cfg.sideSineWaveFreq + cfg.sideSineWaveFreqRandom * randSym(rng_) * 0.5f;
  pt.waveMag = cfg.sideSineWaveMag + cfg.sideSineWaveMagRandom * randSym(rng_) * 0.5f;
  pt.wavePhase = rand01(rng_) * kTwoPi;
  if (cfg.orbitalSpinAmp > 1e-4f) {
    const float cz = randSym(rng_);
    const float phi = rand01(rng_) * kTwoPi;
    const float rxy = std::sqrt(std::max(0.0f, 1.0f - cz * cz));
    const iota::core::Vec3 n{rxy * std::cos(phi), rxy * std::sin(phi), cz};
    const iota::core::Vec3 ref = (std::abs(n.x) > 0.85f)
                                     ? iota::core::Vec3{0.0f, 1.0f, 0.0f}
                                     : iota::core::Vec3{1.0f, 0.0f, 0.0f};
    pt.orbitAxisU = normalizeSafe3({
        ref.y * n.z - ref.z * n.y,
        ref.z * n.x - ref.x * n.z,
        ref.x * n.y - ref.y * n.x,
    });
    pt.orbitAxisV = normalizeSafe3({
        n.y * pt.orbitAxisU.z - n.z * pt.orbitAxisU.y,
        n.z * pt.orbitAxisU.x - n.x * pt.orbitAxisU.z,
        n.x * pt.orbitAxisU.y - n.y * pt.orbitAxisU.x,
    });
    pt.orbitPhase = rand01(rng_) * kTwoPi;
  }

  const float cx = emitterRect.centerX();
  const float cy = emitterRect.centerY();
  const float halfW = std::max(1.0f, emitterRect.width() * 0.5f);
  const float halfH = std::max(1.0f, emitterRect.height() * 0.5f);
  const float depthSpan = (cfg.emitterSizeZ > 1e-3f) ? cfg.emitterSizeZ : cfg.perspectiveDepth;
  const float halfZ = (depthSpan > 1e-3f) ? (depthSpan * 0.5f) : (std::min(halfW, halfH) * 0.5f);

  auto legacyZ = [&]() -> float {
    if (cfg.emitterSizeZ > 1e-3f) return randSym(rng_) * (cfg.emitterSizeZ * 0.5f);
    return (cfg.perspectiveDepth > 0.0f) ? (rand01(rng_) * cfg.perspectiveDepth) : 0.0f;
  };

  switch (cfg.emitterType) {
    case ParticleEmitterType::Point: {
      pt.x = cx;
      pt.y = cy;
      pt.z = 0.0f;
      break;
    }
    case ParticleEmitterType::HorizontalLine: {
      pt.x = emitterRect.left + rand01(rng_) * emitterRect.width();
      pt.y = cy;
      pt.z = legacyZ();
      break;
    }
    case ParticleEmitterType::Box3D: {
      pt.x = cx + randSym(rng_) * halfW;
      pt.y = cy + randSym(rng_) * halfH;
      pt.z = randSym(rng_) * halfZ;
      break;
    }
    case ParticleEmitterType::Sphere: {
      const float cz = randSym(rng_);
      const float phi = rand01(rng_) * kTwoPi;
      const float rxy = std::sqrt(std::max(0.0f, 1.0f - cz * cz));
      const float rad = std::cbrt(rand01(rng_));
      pt.x = cx + rxy * std::cos(phi) * rad * halfW;
      pt.y = cy + rxy * std::sin(phi) * rad * halfH;
      pt.z = cz * rad * halfZ;
      break;
    }
    case ParticleEmitterType::Ring: {
      const float ang = rand01(rng_) * kTwoPi;
      const float rFrac = std::clamp(cfg.ringRadiusFrac, 0.05f, 2.0f) * (0.94f + 0.12f * rand01(rng_));
      pt.x = cx + std::cos(ang) * halfW * rFrac;
      pt.y = cy + std::sin(ang) * halfH * rFrac;
      pt.z = randSym(rng_) * (depthSpan > 1e-3f ? depthSpan * 0.1f : 4.0f);
      break;
    }
    case ParticleEmitterType::Grid: {
      const int gx = std::max(1, cfg.gridCountX);
      const int gy = std::max(1, cfg.gridCountY);
      const uint32_t idx = gridCursor_++ % static_cast<uint32_t>(gx * gy);
      const int ix = static_cast<int>(idx % static_cast<uint32_t>(gx));
      const int iy = static_cast<int>(idx / static_cast<uint32_t>(gx));
      const float fx = (gx > 1) ? (static_cast<float>(ix) / static_cast<float>(gx - 1)) : 0.5f;
      const float fy = (gy > 1) ? (static_cast<float>(iy) / static_cast<float>(gy - 1)) : 0.5f;
      pt.x = emitterRect.left + fx * emitterRect.width();
      pt.y = emitterRect.top + fy * emitterRect.height();
      pt.z = randSym(rng_) * (cfg.emitterSizeZ * 0.5f);
      break;
    }
    case ParticleEmitterType::LayerMask: {
      if (!layerSamples.empty()) {
        const size_t sIdx = static_cast<size_t>(xorshift32(rng_) % static_cast<uint32_t>(layerSamples.size()));
        const auto& s = layerSamples[sIdx];
        pt.x = emitterRect.left + s.u * emitterRect.width();
        pt.y = emitterRect.top + s.v * emitterRect.height();
        pt.z = randSym(rng_) * (depthSpan * 0.5f);
        pt.hasCustomColor = true;
        pt.customAbgr = (static_cast<uint32_t>(s.a) << 24) |
                        (static_cast<uint32_t>(s.b) << 16) |
                        (static_cast<uint32_t>(s.g) << 8) |
                        static_cast<uint32_t>(s.r);
        break;
      }
      [[fallthrough]];
    }
    case ParticleEmitterType::Rectangle:
    default: {
      pt.x = emitterRect.left + rand01(rng_) * emitterRect.width();
      pt.y = emitterRect.top + rand01(rng_) * emitterRect.height();
      pt.z = legacyZ();
      break;
    }
  }

  iota::core::Vec3 dir{
      std::cos(cfg.vectorAngleRad),
      std::sin(cfg.vectorAngleRad),
      cfg.vectorAngleZ,
  };
  bool usedRandomFullSphere = false;
  if (cfg.useRandomVector) {
    const float cz = randSym(rng_);
    const float phi = rand01(rng_) * kTwoPi;
    const float rxy = std::sqrt(std::max(0.0f, 1.0f - cz * cz));
    dir = {rxy * std::cos(phi), rxy * std::sin(phi), cz * 0.5f};
    usedRandomFullSphere = true;
  } else if (cfg.vectorRelativeToCenter &&
             cfg.emitterType != ParticleEmitterType::HorizontalLine &&
             cfg.emitterType != ParticleEmitterType::Point) {
    const iota::core::Vec3 rel{pt.x - cx, pt.y - cy, cfg.camera3DEnabled ? pt.z : 0.0f};
    const float len = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
    if (len > 1e-3f) {
      dir = {rel.x / len, rel.y / len, rel.z / len};
    }
  } else {
    dir = normalizeSafe3(dir);
  }

  if (!usedRandomFullSphere && cfg.directionSpreadDeg > 0.05f) {
    const float spreadRad = std::clamp(cfg.directionSpreadDeg, 0.0f, 180.0f) * (kPi / 180.0f);
    dir = perturbDirectionCone(dir, spreadRad, rng_);
  }

  const float spd = cfg.speedBase + randSym(rng_) * (cfg.speedRandom * 0.5f);
  pt.vx = dir.x * spd + emitterVel.x * cfg.velocityFromMotion;
  pt.vy = dir.y * spd + emitterVel.y * cfg.velocityFromMotion;
  pt.vz = dir.z * spd;

  particles_.push_back(pt);
}

void ParticleSystem::spawnAuxFrom(const ParticleSimConfig& cfg,
                                  const Particle& parent,
                                  float extraJitterSpeed,
                                  int maxCap,
                                  std::vector<Particle>& outAux) {
  if (parent.isAux || static_cast<int>(particles_.size() + outAux.size()) >= maxCap) {
    return;
  }

  Particle aux;
  aux.isAux = true;
  aux.x = parent.x;
  aux.y = parent.y;
  aux.z = parent.z;
  aux.maxAge = std::clamp(cfg.auxLifetime, 0.05f, 10.0f) * (0.85f + 0.3f * rand01(rng_));
  aux.sizeRand = parent.sizeRand;
  aux.endSizeRand = parent.endSizeRand;
  aux.hueShift = parent.hueShift;
  aux.rotation = rand01(rng_) * kTwoPi;
  aux.spinSpeed = parent.spinSpeed * 0.5f;
  aux.hasCustomColor = parent.hasCustomColor;
  aux.customAbgr = parent.customAbgr;

  const float cz = randSym(rng_);
  const float phi = rand01(rng_) * kTwoPi;
  const float rxy = std::sqrt(std::max(0.0f, 1.0f - cz * cz));
  aux.vx = parent.vx * cfg.auxVelocityScale + rxy * std::cos(phi) * extraJitterSpeed;
  aux.vy = parent.vy * cfg.auxVelocityScale + rxy * std::sin(phi) * extraJitterSpeed;
  aux.vz = parent.vz * cfg.auxVelocityScale + cz * extraJitterSpeed;

  outAux.push_back(aux);
}

void ParticleSystem::step(const ParticleSimConfig& cfg,
                          const iota::core::RectF& emitterRect,
                          int viewW,
                          int viewH,
                          float dtSec,
                          std::span<const EmitterSamplePoint> layerSamples) {
  if (!preRunDone_) {
    preRunDone_ = true;
    const float warmSec = std::clamp(cfg.preRunSec, 0.0f, 10.0f);
    if (warmSec > 1e-4f) {
      constexpr float kWarmDt = 1.0f / 60.0f;
      const int steps = std::min(600, std::max(1, static_cast<int>(std::ceil(warmSec / kWarmDt))));
      for (int s = 0; s < steps; ++s) {
        stepInternal(cfg, emitterRect, viewW, viewH, kWarmDt, layerSamples);
      }
    }
  }
  stepInternal(cfg, emitterRect, viewW, viewH, dtSec, layerSamples);
}

void ParticleSystem::stepInternal(const ParticleSimConfig& cfg,
                                  const iota::core::RectF& emitterRect,
                                  int viewW,
                                  int viewH,
                                  float dtSec,
                                  std::span<const EmitterSamplePoint> layerSamples) {
  const float wallDt = std::clamp(dtSec, 0.0f, 0.05f);
  if (wallDt <= 0.0f) {
    return;
  }

  const iota::core::Vec2 curCenter{emitterRect.centerX(), emitterRect.centerY()};
  iota::core::Vec2 emitterVel{};
  if (hasPrevEmitterPos_) {
    emitterVel = {
        (curCenter.x - prevEmitterCenter_.x) / std::max(wallDt, 1e-4f),
        (curCenter.y - prevEmitterCenter_.y) / std::max(wallDt, 1e-4f),
    };
  }
  prevEmitterCenter_ = curCenter;
  hasPrevEmitterPos_ = true;

  const float ptf = std::clamp(cfg.physicsTimeFactor, 0.0f, 30.0f);
  const float totalPhysDt = wallDt * ptf;
  const int subSteps =
      (totalPhysDt > 0.045f) ? std::min(16, static_cast<int>(std::ceil(totalPhysDt / 0.04f))) : 1;
  const float subWallDt = wallDt / static_cast<float>(subSteps);
  const float subPhysDt = totalPhysDt / static_cast<float>(subSteps);

  for (int s = 0; s < subSteps; ++s) {
    stepSubstep(cfg, emitterRect, viewW, viewH, subWallDt, subPhysDt, emitterVel, layerSamples);
  }
}

void ParticleSystem::stepSubstep(const ParticleSimConfig& cfg,
                                 const iota::core::RectF& emitterRect,
                                 int viewW,
                                 int viewH,
                                 float wallDt,
                                 float physDt,
                                 iota::core::Vec2 emitterVel,
                                 std::span<const EmitterSamplePoint> layerSamples) {
  (void)viewW;
  const iota::core::Vec2 curCenter{emitterRect.centerX(), emitterRect.centerY()};

  const int maxCap = std::clamp(cfg.maxParticles, 1, 50000);
  if (static_cast<int>(particles_.size()) > maxCap) {
    particles_.resize(static_cast<size_t>(maxCap));
  }

  // Primary emission rate stays anchored to wall-clock time (matching Trapcode Particular)
  const float baseRate = (cfg.particlesPerSec > 0.0f)
                             ? cfg.particlesPerSec
                             : (1.0f / std::max(0.0005f, cfg.spawnTime));
  const float emitRate = std::clamp(baseRate * std::max(0.0f, cfg.emitRateMultiplier), 0.0f, 20000.0f);
  spawnAccum_ += emitRate * wallDt;
  const int maxSpawnsThisStep = std::min(2000, maxCap);
  int spawned = 0;
  while (spawnAccum_ >= 1.0f && spawned < maxSpawnsThisStep) {
    spawnAccum_ -= 1.0f;
    if (static_cast<int>(particles_.size()) < maxCap) {
      spawnPrimary(cfg, emitterRect, emitterVel, layerSamples);
    }
    ++spawned;
  }
  if (spawnAccum_ > 10.0f) {
    spawnAccum_ = 10.0f;
  }

  if (physDt <= 0.0f) {
    return;
  }
  simTime_ += physDt;

  // Physics constants
  const float dragFactor = (cfg.airResistance > 1e-5f)
                               ? std::exp(-std::clamp(cfg.airResistance, 0.0f, 20.0f) * physDt)
                               : 1.0f;
  const float fieldRadius = std::max(20.0f, cfg.forceFieldRadius);
  const float fieldStr = cfg.forceFieldStrength;
  const float turbScale = std::max(0.05f, cfg.turbulenceScale) * 0.004f;
  const float turbTime = simTime_ * cfg.turbulenceEvolution;
  const bool hasTurbulence = (std::abs(cfg.turbulenceAffectPos) > 1e-3f) ||
                             (std::abs(cfg.turbulenceAffectSize) > 1e-3f) ||
                             (std::abs(cfg.turbulenceAffectOpacity) > 1e-3f);
  const bool hasOrbitalSpin = (cfg.orbitalSpinAmp > 1e-4f) && (cfg.orbitalSpinFreq > 1e-4f);
  const float floorY = std::clamp(cfg.bounceFloorY, 0.0f, 1.2f) * static_cast<float>(std::max(1, viewH));
  const float bounceRest = std::clamp(cfg.bounceRestitution, 0.0f, 1.0f);
  const float bounceFric = std::clamp(cfg.bounceFriction, 0.0f, 1.0f);

  auxCount_ = 0;
  pendingAux_.clear();
  size_t i = 0;
  while (i < particles_.size()) {
    Particle& pt = particles_[i];
    pt.age += physDt;
    if (pt.age >= pt.maxAge) {
      if (!pt.isAux && cfg.auxMode == ParticleAuxMode::AtDeath) {
        const Particle copy = pt;
        if (i + 1 < particles_.size()) {
          particles_[i] = particles_.back();
        }
        particles_.pop_back();
        const int burst = std::clamp(cfg.auxBurstCount, 1, 32);
        for (int b = 0; b < burst; ++b) {
          spawnAuxFrom(cfg, copy, 45.0f, maxCap, pendingAux_);
        }
        continue;
      }
      if (i + 1 < particles_.size()) {
        particles_[i] = particles_.back();
      }
      particles_.pop_back();
      continue;
    }

    // Force fields around emitter center
    if (cfg.forceField != ParticleForceFieldType::None) {
      const float rx = pt.x - curCenter.x;
      const float ry = pt.y - curCenter.y;
      const float dist = std::sqrt(rx * rx + ry * ry);
      if (dist < fieldRadius) {
        const float falloff = 1.0f - (dist / fieldRadius);
        const float safeDist = std::max(12.0f, dist);
        const float nx = rx / safeDist;
        const float ny = ry / safeDist;
        if (cfg.forceField == ParticleForceFieldType::Vortex) {
          pt.vx += (-ny * fieldStr * falloff) * physDt;
          pt.vy += (nx * fieldStr * falloff) * physDt;
        } else if (cfg.forceField == ParticleForceFieldType::Attract) {
          pt.vx += (-nx * fieldStr * falloff) * physDt;
          pt.vy += (-ny * fieldStr * falloff) * physDt;
        } else if (cfg.forceField == ParticleForceFieldType::Repel) {
          pt.vx += (nx * fieldStr * falloff) * physDt;
          pt.vy += (ny * fieldStr * falloff) * physDt;
        } else if (cfg.forceField == ParticleForceFieldType::TurbulentVortex) {
          pt.vx += ((-ny * 0.85f - nx * 0.35f) * fieldStr * falloff) * physDt;
          pt.vy += ((nx * 0.85f - ny * 0.35f) * fieldStr * falloff) * physDt;
        }
      }
    }

    // Gravity + Wind
    pt.vx += (cfg.wind.x) * physDt;
    pt.vy += (cfg.gravity.y + cfg.wind.y) * physDt;
    pt.vx += (cfg.gravity.x) * physDt;
    pt.vz += (cfg.gravityZ + cfg.wind.z) * physDt;

    // Frame-rate-independent exponential air resistance
    pt.vx *= dragFactor;
    pt.vy *= dragFactor;
    pt.vz *= dragFactor;

    // 3D Curl Turbulence field
    iota::core::Vec3 curlVel{};
    if (hasTurbulence) {
      const float uTurb = (cfg.turbulenceFadeIn > 1e-4f)
                              ? std::clamp(pt.age / cfg.turbulenceFadeIn, 0.0f, 1.0f)
                              : 1.0f;
      const float fadeInEnv = uTurb * uTurb * (3.0f - 2.0f * uTurb);
      const auto curl = curlNoise3D(pt.x * turbScale + turbTime * 0.35f,
                                    pt.y * turbScale - turbTime * 0.25f,
                                    pt.z * turbScale + turbTime * 0.50f,
                                    cfg.turbulenceOctaves);
      curlVel = curl * (cfg.turbulenceAffectPos * fadeInEnv);
      pt.turbSizeMod = std::max(0.05f, 1.0f + curl.x * cfg.turbulenceAffectSize * fadeInEnv);
      pt.turbAlphaMod = std::clamp(1.0f + curl.y * cfg.turbulenceAffectOpacity * fadeInEnv, 0.0f, 1.0f);
    } else {
      pt.turbSizeMod = 1.0f;
      pt.turbAlphaMod = 1.0f;
    }

    // Trapcode Particular-style 3D orbital Spin drift
    iota::core::Vec3 spinVel{};
    if (hasOrbitalSpin && !pt.isAux) {
      const float uSpin = (cfg.orbitalSpinFadeIn > 1e-4f)
                              ? std::clamp(pt.age / cfg.orbitalSpinFadeIn, 0.0f, 1.0f)
                              : 1.0f;
      const float spinFade = uSpin * uSpin * (3.0f - 2.0f * uSpin);
      const float omega = kTwoPi * cfg.orbitalSpinFreq;
      const float theta = pt.age * omega + pt.orbitPhase;
      const float spinSpeedScale = cfg.orbitalSpinAmp * omega * spinFade;
      const float sTh = std::sin(theta);
      const float cTh = std::cos(theta);
      spinVel = {
          (-sTh * pt.orbitAxisU.x + cTh * pt.orbitAxisV.x) * spinSpeedScale,
          (-sTh * pt.orbitAxisU.y + cTh * pt.orbitAxisV.y) * spinSpeedScale,
          (-sTh * pt.orbitAxisU.z + cTh * pt.orbitAxisV.z) * spinSpeedScale,
      };
    }

    // Legacy sinusoidal wave perpendicular to horizontal velocity
    float waveDispX = 0.0f;
    float waveDispY = 0.0f;
    if (std::abs(pt.waveMag) > 1e-4f) {
      const float lateral = std::sin(pt.age * pt.waveFreq * kTwoPi + pt.wavePhase) * pt.waveMag * 20.0f;
      const float spdLen = std::max(1e-3f, std::sqrt(pt.vx * pt.vx + pt.vy * pt.vy));
      waveDispX = (-pt.vy / spdLen) * lateral;
      waveDispY = (pt.vx / spdLen) * lateral;
    }

    pt.x += (pt.vx * cfg.overallSpeedMultiplier + curlVel.x + spinVel.x + waveDispX) * physDt;
    pt.y += (pt.vy * cfg.overallSpeedMultiplier + curlVel.y + spinVel.y + waveDispY) * physDt;
    pt.z += (pt.vz * cfg.overallSpeedMultiplier + curlVel.z + spinVel.z) * physDt;
    pt.rotation += pt.spinSpeed * physDt;

    // Floor bounce collision
    if (cfg.bounceEnabled && pt.y >= floorY) {
      pt.y = floorY;
      if (pt.vy > 0.0f) {
        pt.vy = -pt.vy * bounceRest;
        pt.vx *= (1.0f - bounceFric);
        pt.vz *= (1.0f - bounceFric);
        if (!pt.isAux && cfg.auxMode == ParticleAuxMode::AtBounce) {
          const int burst = std::clamp(cfg.auxBurstCount, 1, 24);
          for (int b = 0; b < burst; ++b) {
            spawnAuxFrom(cfg, pt, 35.0f, maxCap, pendingAux_);
          }
        }
      }
    }

    // Continuous Aux trailing emitter
    if (!pt.isAux && cfg.auxMode == ParticleAuxMode::Continuously && cfg.auxParticlesPerSec > 0.0f) {
      pt.auxAccum += cfg.auxParticlesPerSec * physDt;
      int auxBudget = 6;
      while (pt.auxAccum >= 1.0f && auxBudget-- > 0) {
        pt.auxAccum -= 1.0f;
        spawnAuxFrom(cfg, pt, 12.0f, maxCap, pendingAux_);
      }
      if (pt.auxAccum > 6.0f) {
        pt.auxAccum = 6.0f;
      }
    }

    if (pt.isAux) {
      ++auxCount_;
    }
    ++i;
  }

  if (!pendingAux_.empty()) {
    const size_t avail = (static_cast<int>(particles_.size()) < maxCap)
                             ? static_cast<size_t>(maxCap - static_cast<int>(particles_.size()))
                             : 0u;
    const size_t toAppend = std::min(pendingAux_.size(), avail);
    if (toAppend > 0) {
      particles_.insert(particles_.end(),
                        pendingAux_.begin(),
                        pendingAux_.begin() + static_cast<std::ptrdiff_t>(toAppend));
      auxCount_ += static_cast<int>(toAppend);
    }
    pendingAux_.clear();
  }
}

void ParticleSystem::buildGeometry(const ParticleSimConfig& cfg,
                                   const iota::core::RectF& emitterRect,
                                   int viewW,
                                   int viewH,
                                   bool spriteIsFbo,
                                   bool sortBackToFront,
                                   std::vector<iota::rhi::VertexPosColUv>& outVerts,
                                   std::vector<uint32_t>& outIndices) const {
  outVerts.clear();
  outIndices.clear();
  if (particles_.empty() || viewW <= 0 || viewH <= 0) {
    return;
  }

  const float minDim = static_cast<float>(std::min(viewW, viewH));
  const float scalePx = cfg.particleScale * (minDim * 0.004f);
  const float fadeTimeFrac = std::clamp(cfg.fadeInAndOutTime, 0.01f, 0.49f);
  const float aspect = std::max(0.1f, cfg.sizeAspectRatio);

  const float fovRad = std::clamp(cfg.perspectiveFov, 15.0f, 170.0f) * (kPi / 180.0f);
  const float legacyFocalScale = 1.0f / std::max(0.15f, std::tan(fovRad * 0.5f));
  const float focalPx = (0.5f * minDim) * legacyFocalScale;
  const float camDist = cfg.camera3DEnabled ? std::max(40.0f, cfg.cameraDistance) : focalPx;
  const float viewCx = static_cast<float>(viewW) * 0.5f;
  const float viewCy = static_cast<float>(viewH) * 0.5f;
  const float yawRad = cfg.camera3DEnabled ? (cfg.cameraOrbitYawDeg * kPi / 180.0f) : 0.0f;
  const float pitchRad = cfg.camera3DEnabled ? (cfg.cameraOrbitPitchDeg * kPi / 180.0f) : 0.0f;
  const float cy = std::cos(yawRad);
  const float sy = std::sin(yawRad);
  const float cp = std::cos(pitchRad);
  const float sp = std::sin(pitchRad);
  const bool use3DProj = cfg.camera3DEnabled || cfg.perspectiveDepth > 0.0f;
  const float nearFade = std::max(1.0f, cfg.nearCameraFadeOutDistance);

  const bool isPureSprite = (cfg.shapeType == ParticleShapeType::Sprite);
  const float vTop = (isPureSprite && spriteIsFbo) ? 1.0f : 0.0f;
  const float vBot = (isPureSprite && spriteIsFbo) ? 0.0f : 1.0f;

  scratchDrawItems_.clear();
  scratchDrawItems_.reserve(particles_.size());
  auto& items = scratchDrawItems_;

  for (const auto& pt : particles_) {
    const float lifeT = std::clamp(pt.age / pt.maxAge, 0.0f, 1.0f);

    float sx = pt.x;
    float syScreen = pt.y;
    float svx = pt.vx;
    float svy = pt.vy;
    float camZ = 0.0f;
    float perspFactor = 1.0f;
    float nearFadeFactor = 1.0f;

    if (cfg.camera3DEnabled) {
      const iota::core::Vec3 rel{pt.x - viewCx, pt.y - viewCy, pt.z};
      const iota::core::Vec3 r = rotateYawPitch(rel, cy, sy, cp, sp);
      camZ = camDist + r.z;
      if (camZ < 8.0f) {
        continue;
      }
      perspFactor = std::clamp(focalPx / camZ, 0.05f, 18.0f);
      sx = viewCx + r.x * perspFactor;
      syScreen = viewCy + r.y * perspFactor;
      const iota::core::Vec3 rv = rotateYawPitch({pt.vx, pt.vy, pt.vz}, cy, sy, cp, sp);
      svx = rv.x * perspFactor;
      svy = rv.y * perspFactor;
      if (camZ < nearFade) {
        nearFadeFactor = std::clamp(camZ / nearFade, 0.0f, 1.0f);
      }
    } else if (cfg.perspectiveDepth > 0.0f) {
      // Legacy 2.5D depth scaling preserving anchored XY centers
      camZ = std::max(10.0f, pt.z + 120.0f);
      perspFactor = std::clamp((legacyFocalScale * 180.0f) / camZ, 0.1f, 3.5f);
      if (camZ < nearFade) {
        nearFadeFactor = std::clamp(camZ / nearFade, 0.0f, 1.0f);
      }
    }

    // Over-life Opacity curve
    const float opacityMul = evaluateParticleCurve(cfg.opacityOverLife, lifeT, fadeTimeFrac);

    // Over-life Size curve + Legacy Start/Fade/End size scaling
    float szCurve = 1.0f;
    const bool usesCustomEndpoints =
        (std::abs(cfg.startSize - 1.0f) > 1e-3f) ||
        (std::abs(cfg.fadeInSize - 1.0f) > 1e-3f) ||
        (std::abs(cfg.endSize - 0.2f) > 1e-3f);
    if (cfg.sizeOverLife == ParticleCurvePreset::Linear || usesCustomEndpoints) {
      if (cfg.sizeOverLife == ParticleCurvePreset::Linear) {
        const float effectiveEnd = cfg.endSize + cfg.endSizeRandom * (pt.endSizeRand - 0.5f) +
                                   (use3DProj ? (std::max(0.0f, pt.z) * 0.002f * cfg.endSizeZFactor) : 0.0f);
        szCurve = (lifeT < fadeTimeFrac)
                      ? (cfg.startSize + (cfg.fadeInSize - cfg.startSize) * (lifeT / fadeTimeFrac))
                      : (cfg.fadeInSize + (effectiveEnd - cfg.fadeInSize) *
                                              ((lifeT - fadeTimeFrac) / (1.0f - fadeTimeFrac)));
      } else {
        szCurve = cfg.fadeInSize * evaluateParticleCurve(cfg.sizeOverLife, lifeT, fadeTimeFrac);
      }
    } else {
      szCurve = cfg.fadeInSize * evaluateParticleCurve(cfg.sizeOverLife, lifeT, fadeTimeFrac);
    }

    const float sizeJitter = std::max(0.05f, 1.0f + std::clamp(cfg.sizeRandom, 0.0f, 1.0f) * (pt.sizeRand * 2.0f - 1.0f));
    szCurve = std::max(0.01f, szCurve * sizeJitter * pt.turbSizeMod);
    if (pt.isAux) {
      szCurve *= std::clamp(cfg.auxSizeScale, 0.05f, 4.0f);
    }

    // Depth-of-Field Circle-of-Confusion
    float coc = 0.0f;
    float dofAlphaAtten = 1.0f;
    if (cfg.dofEnabled && use3DProj) {
      const float cocRaw = (std::abs(camZ - cfg.dofFocusDist) / std::max(25.0f, cfg.dofFocusDist)) *
                           std::clamp(cfg.dofAperture, 0.0f, 3.0f);
      coc = std::clamp(cocRaw, 0.0f, std::clamp(cfg.dofMaxBlur, 0.0f, 1.0f));
      if (coc > 0.01f) {
        dofAlphaAtten = 1.0f / (1.0f + coc * coc * 2.2f);
      }
    }

    // Color over life (2-stop or 3-stop ColorFrom -> ColorMid -> ColorTo + hue shift + LayerMask/Aux tint)
    iota::core::ColorHslaNorm hsla;
    if (cfg.useMidColor) {
      if (lifeT < 0.5f) {
        hsla = iota::core::ColorHslaNorm::lerp(cfg.colorFrom, cfg.colorMid, lifeT * 2.0f);
      } else {
        hsla = iota::core::ColorHslaNorm::lerp(cfg.colorMid, cfg.colorTo, (lifeT - 0.5f) * 2.0f);
      }
    } else {
      hsla = iota::core::ColorHslaNorm::lerp(cfg.colorFrom, cfg.colorTo, lifeT);
    }
    if (std::abs(pt.hueShift) > 1e-4f) {
      hsla.h = std::fmod(hsla.h + pt.hueShift + 1.0f, 1.0f);
    }
    auto col = hsla.toRgba();

    if (pt.hasCustomColor) {
      const uint8_t cr = static_cast<uint8_t>(pt.customAbgr & 0xFFu);
      const uint8_t cg = static_cast<uint8_t>((pt.customAbgr >> 8) & 0xFFu);
      const uint8_t cb = static_cast<uint8_t>((pt.customAbgr >> 16) & 0xFFu);
      col.r = static_cast<uint8_t>((static_cast<uint32_t>(col.r) * cr) / 255u);
      col.g = static_cast<uint8_t>((static_cast<uint32_t>(col.g) * cg) / 255u);
      col.b = static_cast<uint8_t>((static_cast<uint32_t>(col.b) * cb) / 255u);
    }
    if (pt.isAux) {
      auto auxRgb = cfg.auxColor.toRgba();
      const float k = std::clamp(cfg.auxColorFromMain, 0.0f, 1.0f);
      col.r = static_cast<uint8_t>(std::lround(static_cast<float>(auxRgb.r) * (1.0f - k) + static_cast<float>(col.r) * k));
      col.g = static_cast<uint8_t>(std::lround(static_cast<float>(auxRgb.g) * (1.0f - k) + static_cast<float>(col.g) * k));
      col.b = static_cast<uint8_t>(std::lround(static_cast<float>(auxRgb.b) * (1.0f - k) + static_cast<float>(col.b) * k));
      col.a = static_cast<uint8_t>(std::clamp(static_cast<float>(auxRgb.a) * cfg.auxOpacity * opacityMul, 0.0f, 255.0f));
    }

    col.r = static_cast<uint8_t>((static_cast<uint32_t>(col.r) * cfg.baseTint.r) / 255u);
    col.g = static_cast<uint8_t>((static_cast<uint32_t>(col.g) * cfg.baseTint.g) / 255u);
    col.b = static_cast<uint8_t>((static_cast<uint32_t>(col.b) * cfg.baseTint.b) / 255u);
    const float opacityRandMul = 1.0f - std::clamp(cfg.opacityRandom, 0.0f, 1.0f) * pt.opacityRand;
    const float finalAlpha = static_cast<float>(col.a) * (cfg.baseTint.a / 255.0f) *
                             opacityMul * opacityRandMul * nearFadeFactor * pt.turbAlphaMod * dofAlphaAtten;
    col.a = static_cast<uint8_t>(std::clamp(finalAlpha, 0.0f, 255.0f));
    if (col.a == 0) {
      continue;
    }

    const float dofSizeExpand = 1.0f + coc * 2.5f;
    const float glowMult = (cfg.shapeType == ParticleShapeType::GlowSphere && cfg.glowOpacity > 1e-4f)
                               ? std::clamp(cfg.glowSize, 1.0f, 6.0f)
                               : 1.0f;
    float halfW = std::max(0.75f, scalePx * szCurve * perspFactor * dofSizeExpand) * glowMult;
    float halfH = halfW * aspect;
    float angle = pt.rotation;
    const float speed2D = std::sqrt(svx * svx + svy * svy);

    if (cfg.trailLength > 0.001f && speed2D > 1.0f) {
      angle = std::atan2(svy, svx);
      const float stretch = std::min(minDim * 0.45f, speed2D * cfg.trailLength * 0.06f);
      halfW += stretch * 0.5f;
    } else if (cfg.velocityAngle && speed2D > 1.0f) {
      angle = std::atan2(svy, svx);
    }

    items.push_back(DrawItem{camZ, sx, syScreen, svx, svy, halfW, halfH, angle, coc, col.toPackedAbgrU32()});
  }

  if (sortBackToFront && items.size() > 1) {
    std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
      return a.camZ > b.camZ;
    });
  }

  auto emitItemQuad = [&](float cxPos, float cyPos, const DrawItem& it) {
    float qx = cxPos;
    float qy = cyPos;
    // Offset elongated velocity trails backward so the head stays on the particle position
    const float extraHalf = (cfg.trailLength > 0.001f) ? (it.halfW - it.halfH / aspect) : 0.0f;
    const float ca = std::cos(it.angle);
    const float sa = std::sin(it.angle);
    if (extraHalf > 0.5f) {
      qx -= ca * extraHalf;
      qy -= sa * extraHalf;
    }

    const float rx = ca * it.halfW;
    const float ry = sa * it.halfW;
    const float ux = -sa * it.halfH;
    const float uy = ca * it.halfH;

    const uint32_t base = static_cast<uint32_t>(outVerts.size());
    outVerts.push_back({qx - rx - ux, qy - ry - uy, it.coc, it.abgr, 0.0f, vTop});
    outVerts.push_back({qx + rx - ux, qy + ry - uy, it.coc, it.abgr, 1.0f, vTop});
    outVerts.push_back({qx + rx + ux, qy + ry + uy, it.coc, it.abgr, 1.0f, vBot});
    outVerts.push_back({qx - rx + ux, qy - ry + uy, it.coc, it.abgr, 0.0f, vBot});
    outIndices.push_back(base + 0);
    outIndices.push_back(base + 1);
    outIndices.push_back(base + 2);
    outIndices.push_back(base + 0);
    outIndices.push_back(base + 2);
    outIndices.push_back(base + 3);
  };

  outVerts.reserve(items.size() * 4);
  outIndices.reserve(items.size() * 6);
  const float mirrorSumX = emitterRect.left + emitterRect.right;
  const float mirrorSumY = emitterRect.top + emitterRect.bottom;

  for (const auto& it : items) {
    emitItemQuad(it.sx, it.sy, it);
    if (cfg.mirrorX) {
      DrawItem mx = it;
      mx.angle = kPi - it.angle;
      emitItemQuad(mirrorSumX - it.sx, it.sy, mx);
    }
    if (cfg.mirrorY) {
      DrawItem my = it;
      my.angle = -it.angle;
      emitItemQuad(it.sx, mirrorSumY - it.sy, my);
    }
    if (cfg.mirrorX && cfg.mirrorY) {
      DrawItem mxy = it;
      mxy.angle = it.angle + kPi;
      emitItemQuad(mirrorSumX - it.sx, mirrorSumY - it.sy, mxy);
    }
  }
}

}  // namespace iota::engine
