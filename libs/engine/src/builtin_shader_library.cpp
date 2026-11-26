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

#include "iota/engine/builtin_shader_library.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace iota::engine {

namespace {

// --- Audio-Reactive Shaders ---

constexpr const char* kShaderBassShockwave =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1=Bass, R2=Mids, R3=Highs, Beat)

// --- Parameters & Palette (linear RGB) ---
const vec3 RING_TINT = vec3(0.2500, 0.7800, 1.0000);
const float SHOCK_STRENGTH = 0.045;
const float CHROMA_SPREAD  = 0.014;
const float RING_WIDTH     = 0.14;
const float RING_SPEED     = 1.15;

void main() {
  float aspect = u_resolution.x / max(u_resolution.y, 1.0);
  vec2 centered = (v_uv - 0.5) * vec2(aspect, 1.0);
  float dist = length(centered);
  vec2 dir = (dist > 1e-5) ? (centered / dist) : vec2(0.0);

  float pulse = clamp(u_soundKeys.x * 0.75 + u_beat * 0.55, 0.0, 1.4);
  float radius = fract(u_time * RING_SPEED * 0.45) * 0.85;
  float ringDist = (dist - radius) / max(RING_WIDTH, 0.02);
  float ringEnv = exp(-(ringDist * ringDist));
  float wave = sin((dist - radius) * 32.0) * ringEnv * pulse * SHOCK_STRENGTH;

  vec2 uvR = clamp(v_uv - dir * (wave + CHROMA_SPREAD * pulse * ringEnv), 0.001, 0.999);
  vec2 uvG = clamp(v_uv - dir * wave, 0.001, 0.999);
  vec2 uvB = clamp(v_uv - dir * (wave - CHROMA_SPREAD * pulse * ringEnv), 0.001, 0.999);

  vec4 col;
  col.r = texture(u_texture, uvR).r;
  col.g = texture(u_texture, uvG).g;
  col.b = texture(u_texture, uvB).b;
  col.a = texture(u_texture, uvG).a;
  col.rgb += RING_TINT * ringEnv * pulse * 0.18;

  col *= v_color;
  col.a = clamp(col.a, 0.0, 1.0);
  iota_FragColor = col;
}
)";

constexpr const char* kShaderBeatGlitch =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1=Bass, R2=Mids, R3=Highs, Beat)

// --- Parameters & Palette (linear RGB) ---
const vec3 GLITCH_ACCENT = vec3(1.0000, 0.1800, 0.5500);
const float BEAT_THRESHOLD = 0.25;
const float SLICE_COUNT    = 26.0;
const float MAX_SHIFT      = 0.055;
const float RGB_TEAR       = 0.018;

float hash12(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}

void main() {
  float gate = smoothstep(BEAT_THRESHOLD, BEAT_THRESHOLD + 0.45, max(u_beat, u_soundKeys.x));
  float tick = floor(u_time * 18.0);
  float band = floor(v_uv.y * SLICE_COUNT);
  float rnd = hash12(vec2(band, tick));
  float trigger = step(0.62 - 0.35 * gate, rnd) * gate;

  float shift = (hash12(vec2(band + 13.0, tick)) - 0.5) * 2.0 * MAX_SHIFT * trigger;
  float jitterY = (hash12(vec2(tick, 9.0)) - 0.5) * 0.008 * trigger;

  vec2 uvG = clamp(v_uv + vec2(shift, jitterY), 0.001, 0.999);
  vec2 uvR = clamp(uvG + vec2(RGB_TEAR * trigger, 0.0), 0.001, 0.999);
  vec2 uvB = clamp(uvG - vec2(RGB_TEAR * trigger, 0.0), 0.001, 0.999);

  vec4 col;
  col.r = texture(u_texture, uvR).r;
  col.g = texture(u_texture, uvG).g;
  col.b = texture(u_texture, uvB).b;
  col.a = texture(u_texture, uvG).a;

  float scanFlash = step(0.88, hash12(vec2(band * 2.0, tick + 4.0))) * trigger;
  col.rgb = mix(col.rgb, GLITCH_ACCENT, scanFlash * 0.28);

  col *= v_color;
  col.a = clamp(col.a, 0.0, 1.0);
  iota_FragColor = col;
}
)";

constexpr const char* kShaderAudioWarp =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1=Bass, R2=Mids, R3=Highs, Beat)

// --- Parameters ---
const float BASS_ZOOM_STRENGTH = 0.075;
const float MID_SWIRL_STRENGTH = 0.22;
const float HIGH_SHIMMER_AMP   = 0.0045;

void main() {
  float bass = clamp(u_soundKeys.x, 0.0, 1.5);
  float mids = clamp(u_soundKeys.y, 0.0, 1.5);
  float highs = clamp(u_soundKeys.z, 0.0, 1.5);

  vec2 p = v_uv - 0.5;
  float r = length(p);

  // Bass radial zoom pulse
  p *= (1.0 - BASS_ZOOM_STRENGTH * bass * exp(-r * 1.6));

  // Mid-band gentle angular twist
  float angle = sin(u_time * 1.4) * MID_SWIRL_STRENGTH * mids * (1.0 - smoothstep(0.0, 0.65, r));
  float cs = cos(angle);
  float sn = sin(angle);
  p = mat2(cs, -sn, sn, cs) * p;

  // High-band fine wave shimmer
  vec2 shimmer = vec2(
      sin(v_uv.y * 42.0 + u_time * 9.0),
      cos(v_uv.x * 42.0 - u_time * 8.0)) * (HIGH_SHIMMER_AMP * highs);

  vec2 uv = clamp(p + 0.5 + shimmer, 0.001, 0.999);
  vec4 col = texture(u_texture, uv) * v_color;
  col.a = clamp(col.a, 0.0, 1.0);
  iota_FragColor = col;
}
)";

constexpr const char* kShaderAudioNebula =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1=Bass, R2=Mids, R3=Highs, Beat)

// --- Parameters & Palette (linear RGB) ---
const vec3 BG_DEEP  = vec3(0.0120, 0.0150, 0.0450);
const vec3 NEB_LOW  = vec3(0.0800, 0.4200, 0.8500);
const vec3 NEB_MID  = vec3(0.5500, 0.1400, 0.7200);
const vec3 NEB_CORE = vec3(1.0000, 0.5200, 0.2500);
const float DRIFT_SPEED = 0.22;

float hash21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}

float valueNoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(
      mix(hash21(i + vec2(0.0, 0.0)), hash21(i + vec2(1.0, 0.0)), u.x),
      mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), u.x),
      u.y);
}

float fbm(vec2 p) {
  float v = 0.0;
  float amp = 0.5;
  mat2 rot = mat2(0.80, -0.60, 0.60, 0.80);
  for (int i = 0; i < 5; ++i) {
    v += amp * valueNoise(p);
    p = rot * p * 2.04 + vec2(1.7, 9.2);
    amp *= 0.5;
  }
  return v;
}

void main() {
  float aspect = u_resolution.x / max(u_resolution.y, 1.0);
  vec2 p = (v_uv - 0.5) * vec2(aspect, 1.0) * 2.4;

  float t = u_time * DRIFT_SPEED;
  float bass = u_soundKeys.x;
  float mids = u_soundKeys.y;
  float highs = u_soundKeys.z;

  vec2 q = vec2(fbm(p + t * 0.6), fbm(p + vec2(5.2, 1.3) - t * 0.4));
  vec2 r = vec2(
      fbm(p + 2.6 * q + vec2(1.7, 9.2) + t * 0.35 + bass * 0.25),
      fbm(p + 2.6 * q + vec2(8.3, 2.8) - t * 0.25));
  float f = fbm(p + 2.4 * r);

  vec3 col = mix(BG_DEEP, NEB_LOW, smoothstep(0.15, 0.65, f) * (0.75 + 0.5 * bass));
  col = mix(col, NEB_MID, smoothstep(0.35, 0.85, length(q)) * (0.65 + 0.5 * mids));
  float glow = pow(clamp(f, 0.0, 1.0), 2.4) * (0.55 + 0.8 * u_beat + 0.4 * highs);
  col += NEB_CORE * glow;

  col *= v_color.rgb;
  iota_FragColor = vec4(clamp(col, 0.0, 1.0), v_color.a);
}
)";

// --- Procedural & Fluid Shaders ---

constexpr const char* kShaderFluidGradient =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Palette (OKLab: paste into Web Palette Manager with space = OKLab) ---
const vec3 BG = vec3(0.1450, 0.0350, -0.0550);
const vec3 C0 = vec3(0.3650, 0.0850, -0.0450);
const vec3 C1 = vec3(0.4950, -0.0250, -0.1450);
const vec3 C2 = vec3(0.3450, 0.0350, -0.1350);
const vec3 C3 = vec3(0.6250, -0.0850, -0.0950);
const float SPEED = 0.055;
const float ROT_RATE = 0.05;

vec3 oklabToLinear(vec3 lab) {
  float l_ = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
  float m_ = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
  float s_ = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
  float l = l_ * l_ * l_;
  float m = m_ * m_ * m_;
  float s = s_ * s_ * s_;
  return vec3(
      4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
     -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
     -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

vec3 linearToSrgb(vec3 lin) {
  vec3 c = clamp(lin, 0.0, 1.0);
  vec3 lo = c * 12.92;
  vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, step(vec3(0.0031308), c));
}

vec2 blobOffset(float ph, float rad, float t) {
  return rad * vec2(sin(t * 0.7 + ph), cos(t * 0.55 + ph * 1.4));
}

void main() {
  float aspect = u_resolution.x / max(u_resolution.y, 1.0);
  vec2 p = (v_uv - 0.5) * vec2(aspect, 1.0);
  float t = u_time * SPEED * 12.0;

  float a = u_time * ROT_RATE;
  mat2 rot = mat2(cos(a), -sin(a), sin(a), cos(a));
  p = rot * p;

  float w0 = exp(-4.5 * dot(p - blobOffset(0.0, 0.35, t), p - blobOffset(0.0, 0.35, t)));
  float w1 = exp(-4.2 * dot(p - blobOffset(2.1, 0.42, t), p - blobOffset(2.1, 0.42, t)));
  float w2 = exp(-4.8 * dot(p - blobOffset(4.2, 0.38, t), p - blobOffset(4.2, 0.38, t)));
  float w3 = exp(-5.0 * dot(p - blobOffset(5.5, 0.32, t), p - blobOffset(5.5, 0.32, t)));

  float total = 1.0 + w0 + w1 + w2 + w3;
  vec3 lab = (BG + C0 * w0 + C1 * w1 + C2 * w2 + C3 * w3) / total;
  lab.x += (u_beat * 0.05 + u_soundKeys.x * 0.04);

  vec3 rgb = linearToSrgb(oklabToLinear(lab)) * v_color.rgb;
  iota_FragColor = vec4(rgb, v_color.a);
}
)";

constexpr const char* kShaderLiquidFlow =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Palette (linear RGB) ---
const vec3 DEEP_BLUE = vec3(0.0080, 0.0180, 0.0650);
const vec3 CYAN      = vec3(0.0800, 0.5800, 0.6800);
const vec3 PINK      = vec3(0.6500, 0.1900, 0.4200);
const vec3 GOLD      = vec3(0.8500, 0.5200, 0.2300);
const float FLOW_SPEED = 0.45;
const float WAVE_SCALE = 2.6;

void main() {
  float aspect = u_resolution.x / max(u_resolution.y, 1.0);
  vec2 p = (v_uv - 0.5) * vec2(aspect, 1.0) * WAVE_SCALE;
  float t = u_time * FLOW_SPEED + u_soundKeys.x * 0.25;

  for (int i = 1; i <= 4; ++i) {
    float fi = float(i);
    p.x += (0.55 / fi) * sin(fi * p.y + t * (1.0 + 0.2 * fi) + 0.4 * fi);
    p.y += (0.55 / fi) * cos(fi * p.x - t * (0.9 + 0.15 * fi) + 0.7 * fi);
  }

  float w1 = 0.5 + 0.5 * sin(p.x + p.y);
  float w2 = 0.5 + 0.5 * cos(p.x * 1.3 - p.y * 0.9);
  float crestBase = max(0.0, 0.5 + 0.5 * sin(p.x * 2.1 + t));
  float crest2 = crestBase * crestBase;
  float crest = crest2 * crest2;

  vec3 col = mix(DEEP_BLUE, CYAN, w1 * 0.75);
  col = mix(col, PINK, w2 * 0.55);
  col += GOLD * crest * (0.28 + 0.45 * u_beat);

  col *= v_color.rgb;
  iota_FragColor = vec4(clamp(col, 0.0, 1.0), v_color.a);
}
)";

constexpr const char* kShaderVoronoiCaustics =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Palette (linear RGB) ---
const vec3 CELL_BG   = vec3(0.0100, 0.0300, 0.0700);
const vec3 CAUSTIC_A = vec3(0.0800, 0.6500, 0.9500);
const vec3 CAUSTIC_B = vec3(0.5500, 0.9000, 1.0000);
const float CELL_DENSITY = 5.5;
const float ANIM_SPEED   = 0.75;

vec2 hash22(vec2 p) {
  p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
  return fract(sin(p) * 43758.5453);
}

void main() {
  float aspect = u_resolution.x / max(u_resolution.y, 1.0);
  vec2 st = (v_uv - 0.5) * vec2(aspect, 1.0) * CELL_DENSITY;
  vec2 iSt = floor(st);
  vec2 fSt = fract(st);

  float minDist = 1.0;
  float secondDist = 1.0;
  float t = u_time * ANIM_SPEED + u_soundKeys.y * 0.35;

  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      vec2 neighbor = vec2(float(x), float(y));
      vec2 pt = hash22(iSt + neighbor);
      pt = 0.5 + 0.45 * sin(t + 6.2831 * pt);
      float d = length(neighbor + pt - fSt);
      if (d < minDist) {
        secondDist = minDist;
        minDist = d;
      } else if (d < secondDist) {
        secondDist = d;
      }
    }
  }

  float edge = exp(-9.0 * (secondDist - minDist));
  float energy = pow(minDist, 2.2) * 0.55 + edge * (0.75 + 0.65 * u_soundKeys.x + 0.4 * u_beat);
  vec3 col = CELL_BG + mix(CAUSTIC_A, CAUSTIC_B, clamp(edge, 0.0, 1.0)) * energy;

  col *= v_color.rgb;
  iota_FragColor = vec4(clamp(col, 0.0, 1.0), v_color.a);
}
)";

constexpr const char* kShaderHyperdriveTunnel =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Palette (linear RGB) ---
const vec3 VOID_COL   = vec3(0.0150, 0.0150, 0.0450);
const vec3 STREAK_A   = vec3(0.1600, 0.7200, 1.0000);
const vec3 STREAK_B   = vec3(0.8800, 0.3200, 0.9600);
const float TUNNEL_SPEED = 1.8;
const float SPOKES       = 28.0;

float hash11(float p) {
  p = fract(p * 0.1031);
  p *= p + 33.33;
  p *= p + p;
  return fract(p);
}

void main() {
  float aspect = u_resolution.x / max(u_resolution.y, 1.0);
  vec2 p = (v_uv - 0.5) * vec2(aspect, 1.0);
  float r = length(p);
  float theta = (r > 1e-5) ? atan(p.y, p.x) : 0.0;

  float sector = floor((theta / 6.2831853 + 0.5) * SPOKES);
  float localA = abs(fract((theta / 6.2831853 + 0.5) * SPOKES) - 0.5);
  float depth = 0.45 / max(r, 0.03) - u_time * TUNNEL_SPEED - u_beat * 0.8;

  float streakSeed = hash11(sector + floor(depth * 0.5) * 17.0);
  float radialPulse = (1.0 - smoothstep(0.0, 0.32, localA)) * smoothstep(0.15, 0.85, fract(depth * 0.5 + streakSeed));
  float centerFog = smoothstep(0.02, 0.42, r);
  float energy = radialPulse * centerFog * (0.65 + 0.85 * u_soundKeys.x + 0.5 * u_beat);

  vec3 streakCol = mix(STREAK_A, STREAK_B, hash11(sector * 3.1));
  vec3 col = VOID_COL + streakCol * energy;

  col *= v_color.rgb;
  iota_FragColor = vec4(clamp(col, 0.0, 1.0), v_color.a);
}
)";

// --- Cinematic & Lens Shaders ---

constexpr const char* kShaderFilmGrain =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Parameters ---
const float GRAIN_AMOUNT = 0.065;
const float GRAIN_SIZE   = 1.25;
const float COLOR_GRAIN  = 0.25;

float hash12(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}

void main() {
  vec4 base = texture(u_texture, v_uv);
  vec2 px = floor(v_uv * u_resolution / max(GRAIN_SIZE, 0.5));
  float seed = fract(u_time * 37.0) * 120.0;

  float nMono = hash12(px + seed) - 0.5;
  vec3 nRgb = vec3(
      hash12(px + seed + 11.3) - 0.5,
      nMono,
      hash12(px + seed + 29.7) - 0.5);
  vec3 grain = mix(vec3(nMono), nRgb, COLOR_GRAIN);

  float luma = dot(base.rgb, vec3(0.2126, 0.7152, 0.0722));
  float weight = mix(1.0, 0.35, smoothstep(0.25, 0.95, luma));
  base.rgb = clamp(base.rgb + grain * (GRAIN_AMOUNT * weight), 0.0, 1.0);

  base *= v_color;
  base.a = clamp(base.a, 0.0, 1.0);
  iota_FragColor = base;
}
)";

constexpr const char* kShaderOpticalLens =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Parameters ---
const float BARREL_CURVATURE = 0.16;
const float CHROMA_ABERRATION = 0.022;
const float BEAT_PUMP         = 0.06;

vec2 distortUv(vec2 uv, float k) {
  vec2 d = uv - 0.5;
  float r2 = dot(d, d);
  return 0.5 + d * (1.0 + k * r2);
}

void main() {
  float k = BARREL_CURVATURE + u_beat * BEAT_PUMP;
  vec2 uvR = distortUv(v_uv, k + CHROMA_ABERRATION);
  vec2 uvG = distortUv(v_uv, k);
  vec2 uvB = distortUv(v_uv, k - CHROMA_ABERRATION);

  float inBounds = step(0.0, uvG.x) * step(uvG.x, 1.0) * step(0.0, uvG.y) * step(uvG.y, 1.0);
  vec4 col;
  col.r = texture(u_texture, clamp(uvR, 0.001, 0.999)).r;
  col.g = texture(u_texture, clamp(uvG, 0.001, 0.999)).g;
  col.b = texture(u_texture, clamp(uvB, 0.001, 0.999)).b;
  col.a = texture(u_texture, clamp(uvG, 0.001, 0.999)).a;
  col *= inBounds;

  col *= v_color;
  col.a = clamp(col.a, 0.0, 1.0);
  iota_FragColor = col;
}
)";

constexpr const char* kShaderHalationStreaks =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Parameters & Palette (linear RGB) ---
const vec3 HALATION_TINT = vec3(1.0000, 0.2400, 0.0800);
const vec3 STREAK_TINT   = vec3(0.2800, 0.7200, 1.0000);
const float THRESHOLD    = 0.68;
const float HALATION_AMT = 0.65;
const float STREAK_AMT   = 0.55;

vec3 sampleBright(vec2 uv) {
  vec3 c = texture(u_texture, clamp(uv, 0.001, 0.999)).rgb;
  float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
  return c * smoothstep(THRESHOLD, 1.0, luma);
}

void main() {
  vec4 base = texture(u_texture, v_uv);
  vec2 texel = 1.0 / max(u_resolution, vec2(1.0));

  vec3 hal = vec3(0.0);
  vec3 strk = vec3(0.0);
  for (int i = -4; i <= 4; ++i) {
    float fi = float(i);
    float w = exp(-0.18 * fi * fi);
    strk += sampleBright(v_uv + vec2(fi * texel.x * 11.0, 0.0)) * w;
    hal += sampleBright(v_uv + vec2(fi * texel.x * 4.0, fi * texel.y * 4.0)) * w;
    hal += sampleBright(v_uv + vec2(fi * texel.x * 4.0, -fi * texel.y * 4.0)) * w;
  }
  strk /= 4.5;
  hal /= 9.0;

  float boost = 1.0 + 0.45 * u_beat;
  base.rgb += hal * HALATION_TINT * (HALATION_AMT * boost);
  base.rgb += strk * STREAK_TINT * (STREAK_AMT * boost);

  base *= v_color;
  base.a = clamp(base.a, 0.0, 1.0);
  iota_FragColor = base;
}
)";

// --- Geometric & Sci-Fi Shaders ---

constexpr const char* kShaderKaleidoscope =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Parameters ---
const float SEGMENTS   = 8.0;
const float SPIN_SPEED = 0.14;
const float ZOOM_SCALE = 1.0;

void main() {
  float aspect = u_resolution.x / max(u_resolution.y, 1.0);
  vec2 p = (v_uv - 0.5) * vec2(aspect, 1.0);
  float zoom = max(0.2, ZOOM_SCALE + u_soundKeys.x * 0.12 + u_beat * 0.08);
  float r = length(p) / zoom;
  float a = ((length(p) > 1e-5) ? atan(p.y, p.x) : 0.0) + u_time * SPIN_SPEED;

  float sectorAngle = 6.2831853 / max(SEGMENTS, 2.0);
  a = mod(a, sectorAngle);
  if (a > sectorAngle * 0.5) {
    a = sectorAngle - a;
  }

  vec2 kUv = vec2(cos(a), sin(a)) * r;
  kUv.x /= aspect;
  kUv = clamp(kUv + 0.5, 0.001, 0.999);

  vec4 col = texture(u_texture, kUv) * v_color;
  col.a = clamp(col.a, 0.0, 1.0);
  iota_FragColor = col;
}
)";

constexpr const char* kShaderSynthwaveGrid =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Palette (linear RGB) ---
const vec3 SKY_TOP    = vec3(0.0300, 0.0150, 0.0950);
const vec3 SKY_HORIZ  = vec3(0.4200, 0.0800, 0.4500);
const vec3 SUN_TOP    = vec3(1.0000, 0.7800, 0.2200);
const vec3 SUN_BOT    = vec3(1.0000, 0.1600, 0.4800);
const vec3 GRID_LINE  = vec3(0.1800, 0.9200, 1.0000);
const float HORIZON_Y = 0.54;
const float SCROLL_SPEED = 2.4;

void main() {
  float aspect = u_resolution.x / max(u_resolution.y, 1.0);
  vec2 uv = v_uv;
  vec3 col = vec3(0.0);

  if (uv.y >= HORIZON_Y) {
    float skyT = (uv.y - HORIZON_Y) / max(1.0 - HORIZON_Y, 0.01);
    col = mix(SKY_HORIZ, SKY_TOP, pow(clamp(skyT, 0.0, 1.0), 0.7));

    vec2 sunP = vec2((uv.x - 0.5) * aspect, uv.y - (HORIZON_Y + 0.16));
    float sunDist = length(sunP);
    float sunMask = (1.0 - smoothstep(0.180, 0.185, sunDist));
    float slice = step(0.28, fract(uv.y * 42.0)) + step(0.12, uv.y - HORIZON_Y);
    vec3 sunCol = mix(SUN_BOT, SUN_TOP, clamp((uv.y - HORIZON_Y) * 3.2, 0.0, 1.0));
    col = mix(col, sunCol, sunMask * clamp(slice, 0.0, 1.0));
  } else {
    float dy = max(HORIZON_Y - uv.y, 0.004);
    float z = 0.22 / dy;
    float x = (uv.x - 0.5) * aspect * z * 2.2;
    float gz = z + u_time * SCROLL_SPEED + u_soundKeys.x * 0.4;

    vec2 cell = abs(fract(vec2(x, gz)) - 0.5);
    float line = 1.0 - smoothstep(0.02, 0.06 + 0.02 * dy, min(cell.x, cell.y));
    float horizonFade = exp(-dy * 5.0);
    col = mix(SKY_TOP, GRID_LINE * (0.9 + 0.65 * u_beat), line * (1.0 - 0.45 * horizonFade));
    col += SKY_HORIZ * horizonFade * 0.75;
  }

  col *= v_color.rgb;
  iota_FragColor = vec4(clamp(col, 0.0, 1.0), v_color.a);
}
)";

// --- Retro & Artistic Shaders ---

constexpr const char* kShaderCrtVhs =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Parameters ---
const float CRT_CURVE     = 0.12;
const float SCANLINE_AMT  = 0.28;
const float CHROMA_BLEED  = 0.0055;
const float TRACKING_JITTER = 0.003;

float hash11(float p) {
  return fract(sin(p * 127.1) * 43758.5453);
}

void main() {
  vec2 d = v_uv - 0.5;
  vec2 curved = 0.5 + d * (1.0 + CRT_CURVE * dot(d, d));

  float jitter = (hash11(floor(v_uv.y * 120.0) + floor(u_time * 24.0)) - 0.5) *
                 TRACKING_JITTER * (1.0 + 3.0 * u_beat);
  vec2 uv = curved + vec2(jitter, 0.0);

  float mask = step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);
  vec4 col;
  col.r = texture(u_texture, clamp(uv + vec2(CHROMA_BLEED, 0.0), 0.001, 0.999)).r;
  col.g = texture(u_texture, clamp(uv, 0.001, 0.999)).g;
  col.b = texture(u_texture, clamp(uv - vec2(CHROMA_BLEED, 0.0), 0.001, 0.999)).b;
  col.a = texture(u_texture, clamp(uv, 0.001, 0.999)).a;

  float scan = 1.0 - SCANLINE_AMT * (0.5 + 0.5 * sin(uv.y * max(u_resolution.y, 1.0) * 1.5));
  col.rgb *= scan * mask;
  col.a *= mask;

  col *= v_color;
  col.a = clamp(col.a, 0.0, 1.0);
  iota_FragColor = col;
}
)";

constexpr const char* kShaderHalftoneDither =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

// --- Palette (linear RGB) ---
const vec3 INK_DARK   = vec3(0.0600, 0.0500, 0.1100);
const vec3 INK_MID    = vec3(0.9200, 0.2800, 0.4500);
const vec3 PAPER_LIGHT = vec3(0.9800, 0.9400, 0.8600);
const float DOT_PITCH_PX = 8.0;

float bayer4x4(vec2 p) {
  int x = int(mod(p.x, 4.0));
  int y = int(mod(p.y, 4.0));
  int idx = y * 4 + x;
  const float m[16] = float[16](
      0.0,  8.0,  2.0, 10.0,
     12.0,  4.0, 14.0,  6.0,
      3.0, 11.0,  1.0,  9.0,
     15.0,  7.0, 13.0,  5.0);
  return (m[idx] + 0.5) / 16.0;
}

void main() {
  vec4 src = texture(u_texture, v_uv);
  float luma = dot(src.rgb, vec3(0.2126, 0.7152, 0.0722));

  vec2 px = v_uv * u_resolution;
  mat2 rot = mat2(0.8660, -0.5, 0.5, 0.8660);
  vec2 gridUv = fract((rot * px) / max(DOT_PITCH_PX, 2.0)) - 0.5;
  float dist = length(gridUv);

  float radius = sqrt(clamp(luma, 0.0, 1.0)) * 0.58;
  float dotMask = (1.0 - smoothstep(radius - 0.06, radius + 0.06, dist));
  float dither = step(bayer4x4(floor(px * 0.5)), luma);

  vec3 stylized = mix(INK_DARK, INK_MID, clamp(dotMask * 0.8 + dither * 0.35, 0.0, 1.0));
  stylized = mix(stylized, PAPER_LIGHT, smoothstep(0.62, 0.95, luma) * dotMask);

  vec4 outCol = vec4(stylized, src.a) * v_color;
  outCol.a = clamp(outCol.a, 0.0, 1.0);
  iota_FragColor = outCol;
}
)";

std::string appendSuffixBeforeExt(std::string_view fileName, int copyNumber) {
  const size_t dot = fileName.rfind('.');
  if (dot == std::string_view::npos) {
    return std::string(fileName) + "_" + std::to_string(copyNumber);
  }
  return std::string(fileName.substr(0, dot)) + "_" + std::to_string(copyNumber) +
         std::string(fileName.substr(dot));
}

}  // namespace

const std::vector<BuiltinShaderPreset>& listBuiltinShaderPresets() noexcept {
  static const std::vector<BuiltinShaderPreset> kPresets = {
      // Audio-Reactive
      {"bass_shockwave", "Bass Shockwave", "BassShockwave.glsl",
       BuiltinShaderCategory::AudioReactive, BuiltinShaderKind::PostFx,
       "Alpha", "composition:0",
       "Expanding radial bass shockwave ring with subtle chromatic fringe driven by SoundKeys bass and beat.",
       "linear", kShaderBassShockwave},
      {"beat_glitch", "Beat Glitch & Slice", "BeatGlitch.glsl",
       BuiltinShaderCategory::AudioReactive, BuiltinShaderKind::PostFx,
       "Alpha", "composition:0",
       "Beat-triggered horizontal slice displacement and RGB tear above threshold.",
       "linear", kShaderBeatGlitch},
      {"audio_warp", "3-Band Audio Warp", "AudioWarp.glsl",
       BuiltinShaderCategory::AudioReactive, BuiltinShaderKind::PostFx,
       "Alpha", "composition:0",
       "Bass radial zoom, mid-band UV swirl, and high-band shimmer driven by u_soundKeys.",
       "", kShaderAudioWarp},
      {"audio_nebula", "Audio-Reactive Nebula", "AudioNebula.glsl",
       BuiltinShaderCategory::AudioReactive, BuiltinShaderKind::Generator,
       "Alpha", "composition:0",
       "Domain-warped FBM nebula clouds reactive to bass, mids, highs, and beat.",
       "linear", kShaderAudioNebula},

      // Procedural & Fluid
      {"fluid_gradient", "Fluid Gradient (OKLab)", "FluidGradient.glsl",
       BuiltinShaderCategory::Procedural, BuiltinShaderKind::Generator,
       "Alpha", "composition:0",
       "Smooth OKLab domain-warped multi-stop gradient compatible with the Web Palette Manager.",
       "oklab", kShaderFluidGradient},
      {"liquid_flow", "Liquid Flow Ridges", "LiquidFlow.glsl",
       BuiltinShaderCategory::Procedural, BuiltinShaderKind::Generator,
       "Alpha", "composition:0",
       "Flowing iridescent silk plasma waves modulated by beat and bass.",
       "linear", kShaderLiquidFlow},
      {"voronoi_caustics", "Voronoi Caustics", "VoronoiCaustics.glsl",
       BuiltinShaderCategory::Procedural, BuiltinShaderKind::Generator,
       "Screen", "composition:0",
       "Animated cellular caustics and energy web that blends cleanly via Screen.",
       "linear", kShaderVoronoiCaustics},
      {"hyperdrive_tunnel", "Hyperdrive Tunnel", "HyperdriveTunnel.glsl",
       BuiltinShaderCategory::Procedural, BuiltinShaderKind::Generator,
       "Alpha", "composition:0",
       "Polar warp star tunnel accelerating on audio beat and bass hits.",
       "linear", kShaderHyperdriveTunnel},

      // Cinematic & Lens
      {"film_grain", "Analog Film Grain", "FilmGrain.glsl",
       BuiltinShaderCategory::CinematicLens, BuiltinShaderKind::PostFx,
       "Alpha", "composition:0",
       "Luminance-weighted animated film grain to add organic texture and eliminate banding.",
       "", kShaderFilmGrain},
      {"optical_lens", "Optical Lens & Radial CA", "OpticalLens.glsl",
       BuiltinShaderCategory::CinematicLens, BuiltinShaderKind::PostFx,
       "Alpha", "composition:0",
       "Barrel curvature, beat pump, and edge-weighted radial chromatic aberration.",
       "", kShaderOpticalLens},
      {"halation_streaks", "Halation & Anamorphic Streaks", "HalationStreaks.glsl",
       BuiltinShaderCategory::CinematicLens, BuiltinShaderKind::PostFx,
       "Alpha", "composition:0",
       "Warm film halation glow combined with horizontal anamorphic highlight streaks.",
       "linear", kShaderHalationStreaks},

      // Geometric & Sci-Fi
      {"kaleidoscope", "Kaleidoscope Mirror", "Kaleidoscope.glsl",
       BuiltinShaderCategory::GeometricSciFi, BuiltinShaderKind::PostFx,
       "Alpha", "composition:0",
       "N-fold polar symmetry with slow rotation and beat-reactive zoom.",
       "", kShaderKaleidoscope},
      {"synthwave_grid", "Synthwave Horizon Grid", "SynthwaveGrid.glsl",
       BuiltinShaderCategory::GeometricSciFi, BuiltinShaderKind::Generator,
       "Alpha", "composition:0",
       "Scrolling 3D perspective neon wireframe grid beneath a sliced retro sunset.",
       "linear", kShaderSynthwaveGrid},

      // Retro & Artistic
      {"crt_vhs", "CRT & VHS Simulation", "CrtVhs.glsl",
       BuiltinShaderCategory::RetroArtistic, BuiltinShaderKind::PostFx,
       "Alpha", "composition:0",
       "Curved CRT phosphor scanlines, VHS chroma bleed, and beat-reactive tracking jitter.",
       "", kShaderCrtVhs},
      {"halftone_dither", "Halftone & Bayer Dither", "HalftoneDither.glsl",
       BuiltinShaderCategory::RetroArtistic, BuiltinShaderKind::PostFx,
       "Alpha", "composition:0",
       "Stylized halftone dots and 4x4 ordered Bayer dithering with customizable ink/paper palette.",
       "linear", kShaderHalftoneDither},
  };
  return kPresets;
}

const BuiltinShaderPreset* findBuiltinShaderPreset(std::string_view id) noexcept {
  for (const auto& preset : listBuiltinShaderPresets()) {
    if (preset.id == id) {
      return &preset;
    }
  }
  return nullptr;
}

std::string_view builtinShaderCategoryName(BuiltinShaderCategory cat) noexcept {
  switch (cat) {
    case BuiltinShaderCategory::AudioReactive: return "Audio-Reactive";
    case BuiltinShaderCategory::Procedural: return "Procedural & Fluid";
    case BuiltinShaderCategory::CinematicLens: return "Cinematic & Lens";
    case BuiltinShaderCategory::GeometricSciFi: return "Geometric & Sci-Fi";
    case BuiltinShaderCategory::RetroArtistic: return "Retro & Artistic";
    default: return "Shaders";
  }
}

std::string instantiateBuiltinShaderAsset(iota::model::Project& project,
                                          const BuiltinShaderPreset& preset) {
  std::string chosen(preset.defaultFileName);
  if (project.localAssets.find(chosen) != project.localAssets.end()) {
    int n = 2;
    while (n < 1000) {
      const std::string cand = appendSuffixBeforeExt(preset.defaultFileName, n);
      if (project.localAssets.find(cand) == project.localAssets.end()) {
        chosen = cand;
        break;
      }
      ++n;
    }
  }
  project.localAssets[chosen] =
      std::vector<uint8_t>(preset.fragmentSource.begin(), preset.fragmentSource.end());
  return chosen;
}

}  // namespace iota::engine
