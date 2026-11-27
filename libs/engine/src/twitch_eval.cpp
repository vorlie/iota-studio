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

#include "iota/engine/twitch_eval.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

namespace iota::engine {

namespace {

constexpr float kPiF = 3.14159265358979323846f;

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

uint32_t mixSeed(uint32_t a, uint32_t b) noexcept {
  uint32_t x = a ^ (b * 0x9e3779b9u + 0x517cc1b7u);
  x ^= x >> 16;
  x *= 0x85ebca6bu;
  x ^= x >> 13;
  x *= 0xc2b2ae35u;
  x ^= x >> 16;
  return x;
}

double hashUnit01(int64_t cell, uint32_t seed) noexcept {
  uint64_t z = static_cast<uint64_t>(cell) + 0x9e3779b97f4a7c15ULL +
               static_cast<uint64_t>(seed) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

double hashCell(int64_t cell, uint32_t seed) noexcept {
  return hashUnit01(cell, seed) * 2.0 - 1.0;
}

double smoothCurve1D(double x, uint32_t seed) noexcept {
  const double fl = std::floor(x);
  const int64_t i = static_cast<int64_t>(fl);
  const double t = x - fl;
  const double p0 = hashCell(i - 1, seed);
  const double p1 = hashCell(i + 0, seed);
  const double p2 = hashCell(i + 1, seed);
  const double p3 = hashCell(i + 2, seed);
  const double t2 = t * t;
  const double t3 = t2 * t;
  const double v = 0.5 * ((2.0 * p1) +
                          (-p0 + p2) * t +
                          (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                          (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
  return std::clamp(v, -1.0, 1.0);
}

double smoothUnit1D(double x, uint32_t seed) noexcept {
  const double fl = std::floor(x);
  const int64_t i = static_cast<int64_t>(fl);
  const double t = x - fl;
  const double p0 = hashUnit01(i - 1, seed);
  const double p1 = hashUnit01(i + 0, seed);
  const double p2 = hashUnit01(i + 1, seed);
  const double p3 = hashUnit01(i + 2, seed);
  const double t2 = t * t;
  const double t3 = t2 * t;
  const double v = 0.5 * ((2.0 * p1) +
                          (-p0 + p2) * t +
                          (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                          (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
  return std::clamp(v, 0.0, 1.0);
}

double evalChannelSigned(TwitchMode mode,
                         double sampleT,
                         double threshold,
                         uint32_t seed) noexcept {
  const double th = std::clamp(threshold, 0.0, 0.95);
  switch (mode) {
    case TwitchMode::Pulse: {
      const double fl = std::floor(sampleT);
      const int64_t cell = static_cast<int64_t>(fl);
      const double frac = sampleT - fl;
      if (hashUnit01(cell, seed ^ 0x11CE0001u) < th) {
        return 0.0;
      }
      const double mag = std::exp(-5.5 * frac);
      return mag * hashCell(cell, seed);
    }
    case TwitchMode::Stutter: {
      const int64_t cell = static_cast<int64_t>(std::floor(sampleT));
      if (hashUnit01(cell, seed ^ 0x11CE0001u) < th) {
        return 0.0;
      }
      return hashCell(cell, seed);
    }
    case TwitchMode::Smooth: {
      const double n = 0.72 * smoothCurve1D(sampleT * 0.75, seed) +
                       0.28 * smoothCurve1D(sampleT * 1.50 + 13.7, seed ^ 0x55A1u);
      return std::clamp(n, -1.0, 1.0) * (1.0 - 0.5 * th);
    }
    case TwitchMode::Random:
    default: {
      const double gate = smoothUnit1D(sampleT * 0.65 + 7.13, seed ^ 0x4A7E11u);
      const double norm = std::clamp((gate - th) / std::max(0.05, 1.0 - th), 0.0, 1.0);
      const double env = std::pow(norm, 1.35);
      return env * smoothCurve1D(sampleT, seed);
    }
  }
}

double evalChannelUnipolar(TwitchMode mode,
                           double sampleT,
                           double threshold,
                           uint32_t seed) noexcept {
  const double th = std::clamp(threshold, 0.0, 0.95);
  switch (mode) {
    case TwitchMode::Pulse: {
      const double fl = std::floor(sampleT);
      const int64_t cell = static_cast<int64_t>(fl);
      const double frac = sampleT - fl;
      if (hashUnit01(cell, seed ^ 0x11CE0001u) < th) {
        return 0.0;
      }
      const double amp = 0.35 + 0.65 * hashUnit01(cell, seed ^ 0xBEEF01u);
      return amp * std::exp(-5.5 * frac);
    }
    case TwitchMode::Stutter: {
      const int64_t cell = static_cast<int64_t>(std::floor(sampleT));
      if (hashUnit01(cell, seed ^ 0x11CE0001u) < th) {
        return 0.0;
      }
      return 0.30 + 0.70 * hashUnit01(cell, seed);
    }
    case TwitchMode::Smooth: {
      const double n = 0.72 * smoothUnit1D(sampleT * 0.75, seed) +
                       0.28 * smoothUnit1D(sampleT * 1.50 + 13.7, seed ^ 0x55A1u);
      return std::clamp(n, 0.0, 1.0) * (1.0 - 0.5 * th);
    }
    case TwitchMode::Random:
    default: {
      const double gate = smoothUnit1D(sampleT * 0.65 + 7.13, seed ^ 0x4A7E11u);
      const double norm = std::clamp((gate - th) / std::max(0.05, 1.0 - th), 0.0, 1.0);
      const double env = std::pow(norm, 1.35);
      return env * (0.25 + 0.75 * smoothUnit1D(sampleT, seed));
    }
  }
}

TwitchPose evalPoseAtTime(const TwitchConfig& cfg,
                          double sampleT,
                          uint32_t baseSeed) noexcept {
  TwitchPose pose{};
  const float amt = std::max(0.0f, cfg.amount);
  const double th = static_cast<double>(cfg.threshold);

  if (cfg.enableSlide && cfg.slideAmount > 1e-6f) {
    const float maxOff = 0.32f * amt * cfg.slideAmount;
    const float sxRaw = static_cast<float>(
        evalChannelSigned(cfg.mode, sampleT, th, mixSeed(baseSeed, 0x101u)));
    const float syRaw = static_cast<float>(
        evalChannelSigned(cfg.mode, sampleT, th, mixSeed(baseSeed, 0x202u)));
    switch (cfg.slideDirection) {
      case TwitchSlideDirection::Horizontal:
        pose.slideX = sxRaw * maxOff;
        pose.slideY = 0.0f;
        break;
      case TwitchSlideDirection::Vertical:
        pose.slideX = 0.0f;
        pose.slideY = syRaw * maxOff;
        break;
      case TwitchSlideDirection::Diagonal: {
        const float signY = (hashUnit01(0, mixSeed(baseSeed, 0xD1A6u)) >= 0.5) ? 1.0f : -1.0f;
        const float mag = sxRaw * maxOff * 0.70710678f;
        pose.slideX = mag;
        pose.slideY = mag * signY;
        break;
      }
      case TwitchSlideDirection::Both:
      default:
        pose.slideX = sxRaw * maxOff;
        pose.slideY = syRaw * maxOff;
        break;
    }
  }

  if (cfg.enableScale) {
    if (cfg.scaleAmount > 1e-6f) {
      const uint32_t scSeed = mixSeed(baseSeed, 0x303u);
      const float pump = std::clamp(amt * cfg.scaleAmount * 0.55f, 0.0f, 3.0f);
      switch (cfg.scaleDirection) {
        case TwitchScaleDirection::ZoomIn: {
          const float u = static_cast<float>(evalChannelUnipolar(cfg.mode, sampleT, th, scSeed));
          pose.scale = 1.0f + pump * u;
          break;
        }
        case TwitchScaleDirection::ZoomOut: {
          const float u = static_cast<float>(evalChannelUnipolar(cfg.mode, sampleT, th, scSeed));
          pose.scale = std::max(0.2f, 1.0f / (1.0f + pump * u));
          break;
        }
        case TwitchScaleDirection::Both:
        default: {
          const float s = static_cast<float>(evalChannelSigned(cfg.mode, sampleT, th, scSeed));
          if (s >= 0.0f) {
            pose.scale = 1.0f + pump * s;
          } else {
            pose.scale = std::max(0.2f, 1.0f / (1.0f + pump * (-s)));
          }
          break;
        }
      }
    }
    if (std::abs(cfg.scaleRotation) > 1e-6f) {
      const float sTilt = static_cast<float>(
          evalChannelSigned(cfg.mode, sampleT, th, mixSeed(baseSeed, 0x404u)));
      const float deg = std::clamp(amt, 0.0f, 3.0f) * cfg.scaleRotation * sTilt;
      pose.tiltRad = deg * (kPiF / 180.0f);
    }
  }

  return pose;
}

}  // namespace

TwitchMode twitchModeFromString(std::string_view s) noexcept {
  if (asciiEqualsIgnoreCase(s, "Pulse")) return TwitchMode::Pulse;
  if (asciiEqualsIgnoreCase(s, "Stutter")) return TwitchMode::Stutter;
  if (asciiEqualsIgnoreCase(s, "Smooth")) return TwitchMode::Smooth;
  return TwitchMode::Random;
}

TwitchSlideDirection twitchSlideDirectionFromString(std::string_view s) noexcept {
  if (asciiEqualsIgnoreCase(s, "Horizontal")) return TwitchSlideDirection::Horizontal;
  if (asciiEqualsIgnoreCase(s, "Vertical")) return TwitchSlideDirection::Vertical;
  if (asciiEqualsIgnoreCase(s, "Diagonal")) return TwitchSlideDirection::Diagonal;
  return TwitchSlideDirection::Both;
}

TwitchScaleDirection twitchScaleDirectionFromString(std::string_view s) noexcept {
  if (asciiEqualsIgnoreCase(s, "ZoomOut")) return TwitchScaleDirection::ZoomOut;
  if (asciiEqualsIgnoreCase(s, "Both")) return TwitchScaleDirection::Both;
  return TwitchScaleDirection::ZoomIn;
}

TwitchLightBehavior twitchLightBehaviorFromString(std::string_view s) noexcept {
  if (asciiEqualsIgnoreCase(s, "Darken")) return TwitchLightBehavior::Darken;
  if (asciiEqualsIgnoreCase(s, "Both")) return TwitchLightBehavior::Both;
  return TwitchLightBehavior::Brighten;
}

TwitchColorMode twitchColorModeFromString(std::string_view s) noexcept {
  if (asciiEqualsIgnoreCase(s, "Tint")) return TwitchColorMode::Tint;
  if (asciiEqualsIgnoreCase(s, "Invert")) return TwitchColorMode::Invert;
  if (asciiEqualsIgnoreCase(s, "ChannelSwap")) return TwitchColorMode::ChannelSwap;
  return TwitchColorMode::HueShift;
}

TwitchBlurDirection twitchBlurDirectionFromString(std::string_view s) noexcept {
  if (asciiEqualsIgnoreCase(s, "RadialZoom")) return TwitchBlurDirection::RadialZoom;
  if (asciiEqualsIgnoreCase(s, "Isotropic")) return TwitchBlurDirection::Isotropic;
  return TwitchBlurDirection::Directional;
}

TwitchFrameState evaluateTwitch(const TwitchConfig& cfg,
                              double timeSec,
                              double halfShutterSec) noexcept {
  TwitchFrameState out{};
  const float amt = std::max(0.0f, cfg.amount);
  if (amt <= 1e-6f) {
    return out;
  }

  const float speed = std::max(0.05f, cfg.speed);
  const double th = static_cast<double>(std::clamp(cfg.threshold, 0.0f, 0.95f));
  const uint32_t baseSeed = mixSeed(
      static_cast<uint32_t>(std::llround(static_cast<double>(cfg.randomSeed) * 100.0)),
      0x7717C400u);

  const double phase = static_cast<double>(cfg.timePhase);
  const double sampleT1 = (timeSec + phase) * static_cast<double>(speed);
  const double dtSample = std::max(0.0, halfShutterSec) * static_cast<double>(speed);
  const double sampleT0 = sampleT1 - dtSample;
  const double sampleT2 = sampleT1 + dtSample;

  out.pose1 = evalPoseAtTime(cfg, sampleT1, baseSeed);
  out.pose0 = (dtSample > 1e-6) ? evalPoseAtTime(cfg, sampleT0, baseSeed) : out.pose1;
  out.pose2 = (dtSample > 1e-6) ? evalPoseAtTime(cfg, sampleT2, baseSeed) : out.pose1;

  // Slide chromatic aberration split
  if (cfg.enableSlide && cfg.slideRgbSplit > 1e-6f) {
    const float uSplit = static_cast<float>(
        evalChannelUnipolar(cfg.mode, sampleT1, th, mixSeed(baseSeed, 0x505u)));
    const float mag = amt * cfg.slideRgbSplit * (0.20f + 0.80f * uSplit) * 0.025f;
    float dirX = 1.0f;
    float dirY = 0.0f;
    const float slideLen = std::hypot(out.pose1.slideX, out.pose1.slideY);
    if (cfg.slideRgbAutoAngle && slideLen > 1e-5f) {
      dirX = out.pose1.slideX / slideLen;
      dirY = out.pose1.slideY / slideLen;
    } else {
      const float ang = cfg.slideRgbAngle * (kPiF / 180.0f);
      dirX = std::cos(ang);
      dirY = std::sin(ang);
    }
    out.rgbSplitX = dirX * mag;
    out.rgbSplitY = dirY * mag;
  }

  // Light strobe
  if (cfg.enableLight && cfg.lightAmount > 1e-6f) {
    const uint32_t lightSeed = mixSeed(baseSeed, 0x606u);
    switch (cfg.lightBehavior) {
      case TwitchLightBehavior::Brighten: {
        const float u = static_cast<float>(evalChannelUnipolar(cfg.mode, sampleT1, th, lightSeed));
        out.lightDelta = std::max(0.0f, amt * cfg.lightAmount * u);
        break;
      }
      case TwitchLightBehavior::Darken: {
        const float u = static_cast<float>(evalChannelUnipolar(cfg.mode, sampleT1, th, lightSeed));
        out.lightDelta = -std::clamp(amt * cfg.lightAmount * u, 0.0f, 0.95f);
        break;
      }
      case TwitchLightBehavior::Both:
      default: {
        const float s = static_cast<float>(evalChannelSigned(cfg.mode, sampleT1, th, lightSeed));
        if (s >= 0.0f) {
          out.lightDelta = amt * cfg.lightAmount * s;
        } else {
          out.lightDelta = -std::clamp(amt * cfg.lightAmount * (-s), 0.0f, 0.95f);
        }
        break;
      }
    }
  }

  // Color bursts
  if (cfg.enableColor && cfg.colorAmount > 1e-6f) {
    const uint32_t colSeed = mixSeed(baseSeed, 0x707u);
    const float uCol = static_cast<float>(evalChannelUnipolar(cfg.mode, sampleT1, th, colSeed));
    const float sCol = static_cast<float>(evalChannelSigned(cfg.mode, sampleT1, th, colSeed ^ 0x99u));
    out.colorMix = std::clamp(amt * cfg.colorAmount * uCol, 0.0f, 1.0f);
    out.colorHueShift = sCol * cfg.colorAmount * std::clamp(amt, 0.0f, 2.5f) * 0.5f;
    const int64_t cell = static_cast<int64_t>(std::floor(sampleT1));
    out.colorSwapDir = (hashUnit01(cell, mixSeed(baseSeed, 0x708u)) >= 0.5) ? 1 : 0;
  }

  // Operator blur bursts
  if (cfg.enableBlur && cfg.blurAmount > 1e-6f) {
    const float uBlur = static_cast<float>(
        evalChannelUnipolar(cfg.mode, sampleT1, th, mixSeed(baseSeed, 0x808u)));
    out.blurRadius = std::clamp(amt * cfg.blurAmount * uBlur * 0.045f, 0.0f, 0.18f);
    const float ang = cfg.blurAngle * (kPiF / 180.0f);
    out.blurDirX = std::cos(ang);
    out.blurDirY = std::sin(ang);
  }

  // Glitch slice tear bands
  if (cfg.enableSlice && cfg.sliceAmount > 1e-6f) {
    const uint32_t slSeed = mixSeed(baseSeed, 0x909u);
    const float uSlice = static_cast<float>(evalChannelUnipolar(cfg.mode, sampleT1, th, slSeed));
    out.sliceStrength = std::clamp(amt * cfg.sliceAmount * uSlice * 0.18f, 0.0f, 0.45f);
    out.sliceRgbSplit = std::clamp(amt * std::max(0.0f, cfg.sliceRgbSplit) * uSlice * 0.04f, 0.0f, 0.15f);
    const int64_t slCell = static_cast<int64_t>(std::floor(sampleT1));
    out.sliceSeed = static_cast<float>(hashUnit01(slCell, mixSeed(baseSeed, 0x90Au)) * 97.0 +
                                       static_cast<double>(baseSeed & 0xFFu) * 0.37);
  }

  return out;
}

}  // namespace iota::engine
