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

#include "iota/core/types.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace iota::core {

BlendMode blendModeFromString(std::string_view name, BlendMode fallback) noexcept {
  if (name == "Alpha" || name == "0") return BlendMode::Alpha;
  if (name == "Screen" || name == "1") return BlendMode::Screen;
  if (name == "Add" || name == "2") return BlendMode::Add;
  if (name == "3" || name == "Opaque") return BlendMode::Opaque;
  if (name == "PreMulAlpha" || name == "4") return BlendMode::PreMulAlpha;
  if (name == "AddAlpha" || name == "5") return BlendMode::AddAlpha;
  if (name == "6" || name == "Inverse") return BlendMode::Inverse;
  return fallback;
}

std::string_view blendModeToString(BlendMode mode) noexcept {
  switch (mode) {
    case BlendMode::Alpha: return "Alpha";
    case BlendMode::Screen: return "Screen";
    case BlendMode::Add: return "Add";
    case BlendMode::Opaque: return "3";
    case BlendMode::PreMulAlpha: return "PreMulAlpha";
    case BlendMode::AddAlpha: return "AddAlpha";
    case BlendMode::Inverse: return "6";
  }
  return "Alpha";
}

void ColorRgba::toFloat4(float out[4]) const noexcept {
  constexpr float kInv255 = 1.0f / 255.0f;
  out[0] = static_cast<float>(r) * kInv255;
  out[1] = static_cast<float>(g) * kInv255;
  out[2] = static_cast<float>(b) * kInv255;
  out[3] = static_cast<float>(a) * kInv255;
}

ColorRgba ColorRgba::fromFloat4(const float rgba[4]) noexcept {
  auto toByte = [](float v) -> uint8_t {
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
  };
  return {toByte(rgba[0]), toByte(rgba[1]), toByte(rgba[2]), toByte(rgba[3])};
}

ColorRgba ColorRgba::lerp(ColorRgba c0, ColorRgba c1, float t) noexcept {
  const float u = std::clamp(t, 0.0f, 1.0f);
  auto mixByte = [u](uint8_t a, uint8_t b) -> uint8_t {
    return static_cast<uint8_t>(std::lround((1.0f - u) * a + u * b));
  };
  return {mixByte(c0.r, c1.r), mixByte(c0.g, c1.g), mixByte(c0.b, c1.b), mixByte(c0.a, c1.a)};
}

namespace {

float hue2rgb(float p, float q, float t) noexcept {
  if (t < 0.0f) t += 1.0f;
  if (t > 1.0f) t -= 1.0f;
  if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
  if (t < 0.5f) return q;
  if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
  return p;
}

std::optional<float> parseFloat(std::string_view sv) noexcept {
  sv = trim(sv);
  if (sv.empty()) return std::nullopt;
  float val = 0.0f;
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
  if (ec != std::errc() || ptr != sv.data() + sv.size()) return std::nullopt;
  return val;
}

} // namespace

ColorRgba ColorHslaNorm::toRgba() const noexcept {
  float hue = h - std::floor(h);
  if (hue < 0.0f) hue += 1.0f;
  const float sat = std::clamp(s, 0.0f, 1.0f);
  const float lum = std::clamp(l, 0.0f, 1.0f);
  const float alp = std::clamp(a, 0.0f, 1.0f);

  float r = lum, g = lum, b = lum;
  if (sat > 1e-6f) {
    const float q = lum < 0.5f ? lum * (1.0f + sat) : lum + sat - lum * sat;
    const float p = 2.0f * lum - q;
    r = hue2rgb(p, q, hue + 1.0f / 3.0f);
    g = hue2rgb(p, q, hue);
    b = hue2rgb(p, q, hue - 1.0f / 3.0f);
  }
  const float rgba[4] = {r, g, b, alp};
  return ColorRgba::fromFloat4(rgba);
}

ColorHslaNorm ColorHslaNorm::fromRgba(ColorRgba c) noexcept {
  float f[4];
  c.toFloat4(f);
  const float maxC = std::max({f[0], f[1], f[2]});
  const float minC = std::min({f[0], f[1], f[2]});
  const float lum = 0.5f * (maxC + minC);
  float hue = 0.0f;
  float sat = 0.0f;
  const float d = maxC - minC;
  if (d > 1e-6f) {
    sat = lum > 0.5f ? d / (2.0f - maxC - minC) : d / (maxC + minC);
    if (maxC == f[0]) {
      hue = (f[1] - f[2]) / d + (f[1] < f[2] ? 6.0f : 0.0f);
    } else if (maxC == f[1]) {
      hue = (f[2] - f[0]) / d + 2.0f;
    } else {
      hue = (f[0] - f[1]) / d + 4.0f;
    }
    hue /= 6.0f;
  }
  return {hue, sat, lum, f[3]};
}

ColorHslaNorm ColorHslaNorm::lerp(ColorHslaNorm c0, ColorHslaNorm c1, float t) noexcept {
  const float u = std::clamp(t, 0.0f, 1.0f);
  const float inv = 1.0f - u;
  return {
      inv * c0.h + u * c1.h,
      inv * c0.s + u * c1.s,
      inv * c0.l + u * c1.l,
      inv * c0.a + u * c1.a,
  };
}

std::string_view trim(std::string_view sv) noexcept {
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
    sv.remove_suffix(1);
  }
  return sv;
}

std::vector<std::string_view> splitWhitespace(std::string_view sv) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < sv.size()) {
    while (i < sv.size() && (sv[i] == ' ' || sv[i] == '\t' || sv[i] == '\r' || sv[i] == '\n')) ++i;
    if (i >= sv.size()) break;
    size_t start = i;
    while (i < sv.size() && sv[i] != ' ' && sv[i] != '\t' && sv[i] != '\r' && sv[i] != '\n') ++i;
    out.push_back(sv.substr(start, i - start));
  }
  return out;
}

std::vector<std::string_view> splitChar(std::string_view sv, char sep) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (start <= sv.size()) {
    const size_t pos = sv.find(sep, start);
    if (pos == std::string_view::npos) {
      out.push_back(sv.substr(start));
      break;
    }
    out.push_back(sv.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

std::optional<Vec2> parseF2(std::string_view sv) noexcept {
  auto parts = splitWhitespace(sv);
  if (parts.size() < 2) return std::nullopt;
  auto x = parseFloat(parts[0]);
  auto y = parseFloat(parts[1]);
  if (!x || !y) return std::nullopt;
  return Vec2{*x, *y};
}

std::string formatF2(Vec2 v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f %.6f", v.x, v.y);
  return std::string(buf);
}

std::optional<ColorHslaNorm> parseHsla4f(std::string_view sv) noexcept {
  auto parts = splitWhitespace(sv);
  if (parts.size() < 4) return std::nullopt;
  auto h = parseFloat(parts[0]);
  auto s = parseFloat(parts[1]);
  auto l = parseFloat(parts[2]);
  auto a = parseFloat(parts[3]);
  if (!h || !s || !l || !a) return std::nullopt;
  return ColorHslaNorm{*h, *s, *l, *a};
}

std::string formatHsla4f(ColorHslaNorm c) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%.6f %.6f %.6f %.6f", c.h, c.s, c.l, c.a);
  return std::string(buf);
}

} // namespace iota::core
