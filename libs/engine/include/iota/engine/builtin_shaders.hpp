#pragma once

namespace iota::engine::shaders {

inline constexpr const char* kBufferVert = R"glsl(#version 450 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;

uniform mat4 u_projView;

out vec4 v_color;
out vec2 v_uv;

void main() {
  v_color = a_color;
  v_uv = a_uv;
  gl_Position = u_projView * vec4(a_pos, 1.0);
}
)glsl";

inline constexpr const char* kBufferFrag = R"glsl(#version 450 core
in vec4 v_color;
in vec2 v_uv;

uniform sampler2D u_texture;
uniform float u_hdrGain;

out vec4 fragColor;

void main() {
  vec4 c = v_color * texture(u_texture, v_uv);
  fragColor = vec4(c.rgb * u_hdrGain, c.a);
}
)glsl";

inline constexpr const char* kBufferFxLightFrag = R"glsl(#version 450 core
in vec4 v_color;
in vec2 v_uv;

uniform sampler2D u_texture;
uniform sampler2D u_texture2;

uniform float maskadd;
uniform float maskmul;
uniform float mask_l_add;
uniform float mask_l_mul;
uniform float tex2_y_add;
uniform float tex2_y_mul;
uniform vec4 Color2;
uniform float saturation;
uniform float u_hdrGain;

out vec4 fragColor;

void main() {
  vec4 c = texture(u_texture, v_uv);
  float luma = dot(c.rgb, vec3(0.2125, 0.7154, 0.0721));
  c.rgb = mix(vec3(luma), c.rgb, saturation);
  c *= v_color;
  c.rgb = Color2.rgb * c.a + c.rgb;

  vec2 maskUv = vec2(v_uv.x, tex2_y_add + v_uv.y * tex2_y_mul);
  vec4 m = texture(u_texture2, maskUv);
  float mAlpha = clamp(maskadd + m.a * maskmul, 0.0, 1.0);
  float mLuma = dot(m.rgb, vec3(0.2125, 0.7154, 0.0721));
  float mLight = clamp(mask_l_add + mLuma * mask_l_mul, 0.0, 1.0);
  float maskFactor = mAlpha * mLight;

  fragColor = vec4(c.rgb * maskFactor * u_hdrGain, c.a * maskFactor);
}
)glsl";

inline constexpr const char* kBlurFullscreenVert = R"glsl(#version 450 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;

out vec2 v_uv;

void main() {
  v_uv = a_uv;
  gl_Position = vec4(a_pos.xy, 0.0, 1.0);
}
)glsl";

// Dual Kawase downsample pass (5 taps, energy-conserving).
inline constexpr const char* kKawaseDownFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_texture;
uniform vec2 u_halfPixel;
uniform float u_offset;

out vec4 fragColor;

void main() {
  vec2 d = u_halfPixel * u_offset;
  vec4 sum = texture(u_texture, v_uv) * 4.0;
  sum += texture(u_texture, clamp(v_uv - d, 0.0, 1.0));
  sum += texture(u_texture, clamp(v_uv + d, 0.0, 1.0));
  sum += texture(u_texture, clamp(v_uv + vec2(d.x, -d.y), 0.0, 1.0));
  sum += texture(u_texture, clamp(v_uv - vec2(d.x, -d.y), 0.0, 1.0));
  fragColor = sum * 0.125;
}
)glsl";

// Dual Kawase upsample pass (8-tap tent filter + continuous fine-level mix).
inline constexpr const char* kKawaseUpFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_texture;
uniform sampler2D u_fineTexture;
uniform vec2 u_halfPixel;
uniform float u_offset;
uniform float u_blend;

out vec4 fragColor;

void main() {
  vec2 d = u_halfPixel * u_offset;
  vec4 sum = vec4(0.0);
  sum += texture(u_texture, clamp(v_uv + vec2(-d.x * 2.0, 0.0), 0.0, 1.0));
  sum += texture(u_texture, clamp(v_uv + vec2(-d.x, d.y), 0.0, 1.0)) * 2.0;
  sum += texture(u_texture, clamp(v_uv + vec2(0.0, d.y * 2.0), 0.0, 1.0));
  sum += texture(u_texture, clamp(v_uv + vec2(d.x, d.y), 0.0, 1.0)) * 2.0;
  sum += texture(u_texture, clamp(v_uv + vec2(d.x * 2.0, 0.0), 0.0, 1.0));
  sum += texture(u_texture, clamp(v_uv + vec2(d.x, -d.y), 0.0, 1.0)) * 2.0;
  sum += texture(u_texture, clamp(v_uv + vec2(0.0, -d.y * 2.0), 0.0, 1.0));
  sum += texture(u_texture, clamp(v_uv + vec2(-d.x, -d.y), 0.0, 1.0)) * 2.0;
  vec4 blurred = sum / 12.0;
  if (u_blend < 0.999) {
    vec4 fine = texture(u_fineTexture, v_uv);
    fragColor = mix(fine, blurred, clamp(u_blend, 0.0, 1.0));
  } else {
    fragColor = blurred;
  }
}
)glsl";

// Separable Gaussian / Directional blur pass.
inline constexpr const char* kGaussianSepFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_texture;
uniform vec2 u_stepUv;
uniform float u_radiusPx;
uniform int u_halfTaps;

out vec4 fragColor;

void main() {
  float r = max(u_radiusPx, 0.0);
  if (r < 0.05) {
    fragColor = texture(u_texture, v_uv);
    return;
  }
  int taps = clamp(u_halfTaps, 1, 32);
  float sigma = max(r * 0.45, 0.25);
  float invTwoSigma2 = 0.5 / (sigma * sigma);
  float stepScale = max(1.0, r / float(taps));

  vec4 acc = texture(u_texture, v_uv);
  float wSum = 1.0;
  for (int i = 1; i <= taps; ++i) {
    float dist = float(i) * stepScale;
    float w = exp(-(dist * dist) * invTwoSigma2);
    vec2 off = u_stepUv * dist;
    acc += texture(u_texture, clamp(v_uv + off, 0.0, 1.0)) * w;
    acc += texture(u_texture, clamp(v_uv - off, 0.0, 1.0)) * w;
    wSum += 2.0 * w;
  }
  fragColor = acc / wSum;
}
)glsl";

// Radial zoom blur with spatial interleaved gradient noise jitter.
inline constexpr const char* kRadialBlurFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_texture;
uniform vec2 u_center;
uniform float u_strength;

out vec4 fragColor;

float ign(vec2 p) {
  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
  float s = clamp(u_strength, 0.0, 0.85);
  if (s < 0.001) {
    fragColor = texture(u_texture, v_uv);
    return;
  }
  const int kSamples = 24;
  vec2 dir = v_uv - u_center;
  float jitter = ign(gl_FragCoord.xy) - 0.5;
  vec4 acc = vec4(0.0);
  float wSum = 0.0;
  for (int i = 0; i < kSamples; ++i) {
    float t = ((float(i) + 0.5 + jitter) / float(kSamples)) - 0.5;
    float w = exp(-2.2 * t * t);
    float scale = 1.0 - t * s;
    vec2 uv = clamp(u_center + dir * scale, 0.0, 1.0);
    acc += texture(u_texture, uv) * w;
    wSum += w;
  }
  fragColor = acc / max(wSum, 1e-5);
}
)glsl";

inline constexpr const char* kRgbSplitFrag = R"glsl(#version 450 core
in vec4 v_color;
in vec2 v_uv;

uniform sampler2D u_texture;
uniform vec2 dirAmount;
uniform vec3 splitColor0;
uniform vec3 splitColor1;
uniform vec3 splitColor2;
uniform float u_hdrGain;

out vec4 fragColor;

void main() {
  vec4 c0 = texture(u_texture, clamp(v_uv + dirAmount, 0.0, 1.0));
  vec4 c1 = texture(u_texture, v_uv);
  vec4 c2 = texture(u_texture, clamp(v_uv - dirAmount, 0.0, 1.0));

  vec3 rgb = c0.rgb * splitColor0 + c1.rgb * splitColor1 + c2.rgb * splitColor2;
  float alpha = (c0.a + c1.a + c2.a) / 3.0;
  vec4 outCol = vec4(rgb, alpha) * v_color;
  fragColor = vec4(outCol.rgb * u_hdrGain, outCol.a);
}
)glsl";

// Multi-sample shutter motion blur supporting translation, signed radial zoom, and rotation.
inline constexpr const char* kMotionBlurFrag = R"glsl(#version 450 core
in vec4 v_color;
in vec2 v_uv;

uniform sampler2D u_texture;
uniform vec2 u_posAmount;
uniform vec2 u_scaleAmount;
uniform float u_rotAmount;
uniform float u_shutterPhase;
uniform vec2 u_center;
uniform float u_aspect;
uniform int u_samples;
uniform float u_hdrGain;

out vec4 fragColor;

float ign(vec2 p) {
  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
  float motionMag = length(u_posAmount) + length(u_scaleAmount) + abs(u_rotAmount);
  if (motionMag < 1e-5) {
    vec4 outCol = texture(u_texture, v_uv) * v_color;
    fragColor = vec4(outCol.rgb * u_hdrGain, outCol.a);
    return;
  }

  int count = clamp(u_samples, 4, 64);
  float jitter = ign(gl_FragCoord.xy) - 0.5;
  float aspect = max(u_aspect, 0.01);
  vec2 baseOffset = v_uv - u_center;
  vec4 acc = vec4(0.0);

  for (int i = 0; i < count; ++i) {
    float t = ((float(i) + 0.5 + jitter) / float(count)) - 0.5 + u_shutterPhase;
    vec2 p = baseOffset;

    if (abs(u_rotAmount) > 1e-5) {
      float a = t * u_rotAmount;
      float cs = cos(a);
      float sn = sin(a);
      vec2 q = vec2(p.x * aspect, p.y);
      vec2 rq = vec2(q.x * cs - q.y * sn, q.x * sn + q.y * cs);
      p = vec2(rq.x / aspect, rq.y);
    }

    vec2 zoom = max(vec2(0.25), vec2(1.0) + t * u_scaleAmount);
    p = p / zoom;

    vec2 sampleUv = clamp(p + u_center - t * u_posAmount, 0.0, 1.0);
    acc += texture(u_texture, sampleUv);
  }

  vec4 outCol = (acc / float(count)) * v_color;
  fragColor = vec4(outCol.rgb * u_hdrGain, outCol.a);
}
)glsl";

// Frame-rate-independent temporal accumulation shader for moving scene elements.
inline constexpr const char* kTemporalBlurFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_currentTex;
uniform sampler2D u_historyTex;
uniform float u_keepFactor;
uniform float u_currFlipY;
uniform int u_trailBlend;

out vec4 fragColor;

void main() {
  vec2 currUv = vec2(v_uv.x, mix(v_uv.y, 1.0 - v_uv.y, u_currFlipY));
  vec4 curr = texture(u_currentTex, currUv);
  vec4 prev = texture(u_historyTex, v_uv) * clamp(u_keepFactor, 0.0, 0.99);

  if (u_trailBlend == 1) {
    fragColor = max(curr, prev);
  } else if (u_trailBlend == 2) {
    vec3 currSdr = clamp(curr.rgb, 0.0, 1.0);
    vec3 prevSdr = clamp(prev.rgb, 0.0, 1.0);
    vec3 screenSdr = 1.0 - (1.0 - currSdr) * (1.0 - prevSdr);
    vec3 hdrExcess = max(curr.rgb - vec3(1.0), vec3(0.0)) + max(prev.rgb - vec3(1.0), vec3(0.0));
    fragColor = vec4(screenSdr + hdrExcess, max(curr.a, prev.a));
  } else {
    vec4 rawPrev = texture(u_historyTex, v_uv);
    fragColor = mix(curr, rawPrev, clamp(u_keepFactor, 0.0, 0.99));
  }
}
)glsl";

inline constexpr const char* kFxaaFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_texture;
uniform float resolutionW;
uniform float resolutionH;

out vec4 fragColor;

float fxaaLuma(vec3 rgb) {
  return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void main() {
  vec2 invRes = vec2(1.0 / max(resolutionW, 1.0), 1.0 / max(resolutionH, 1.0));
  vec3 rgbNW = texture(u_texture, v_uv + vec2(-1.0, -1.0) * invRes).rgb;
  vec3 rgbNE = texture(u_texture, v_uv + vec2( 1.0, -1.0) * invRes).rgb;
  vec3 rgbSW = texture(u_texture, v_uv + vec2(-1.0,  1.0) * invRes).rgb;
  vec3 rgbSE = texture(u_texture, v_uv + vec2( 1.0,  1.0) * invRes).rgb;
  vec4 texM  = texture(u_texture, v_uv);

  float lumaNW = fxaaLuma(rgbNW);
  float lumaNE = fxaaLuma(rgbNE);
  float lumaSW = fxaaLuma(rgbSW);
  float lumaSE = fxaaLuma(rgbSE);
  float lumaM  = fxaaLuma(texM.rgb);

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

  vec2 dir;
  dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
  dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

  float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * (1.0 / 8.0)), 1.0 / 128.0);
  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
  dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * invRes;

  vec3 rgbA = 0.5 * (
      texture(u_texture, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
      texture(u_texture, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
  vec3 rgbB = rgbA * 0.5 + 0.25 * (
      texture(u_texture, v_uv + dir * -0.5).rgb +
      texture(u_texture, v_uv + dir *  0.5).rgb);

  float lumaB = fxaaLuma(rgbB);
  vec3 finalRgb = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  fragColor = vec4(finalRgb, texM.a);
}
)glsl";

// Particular-style 3D Particle vertex shader (stores DoF Circle-of-Confusion in a_pos.z).
inline constexpr const char* kParticleVert = R"glsl(#version 450 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;

uniform mat4 u_projView;

out vec4 v_color;
out vec2 v_uv;
out float v_softness;

void main() {
  v_color = a_color;
  v_uv = a_uv;
  v_softness = clamp(a_pos.z, 0.0, 1.0);
  gl_Position = u_projView * vec4(a_pos.xy, 0.0, 1.0);
}
)glsl";

// Procedural + textured sprite particle fragment shader with continuous DoF bokeh feathering.
inline constexpr const char* kParticleFrag = R"glsl(#version 450 core
in vec4 v_color;
in vec2 v_uv;
in float v_softness;

uniform sampler2D u_texture;
uniform int u_particleShape;  // 0=Sprite, 1=Sphere, 2=GlowSphere, 3=Star, 4=BokehRing, 5=Streaklet
uniform float u_feather;
uniform float u_premulRgb;
uniform float u_hdrGain;

out vec4 fragColor;

void main() {
  vec2 p = v_uv * 2.0 - 1.0;
  float r = length(p);
  float f = clamp(u_feather + v_softness * (1.0 - u_feather), 0.02, 1.0);

  if (u_particleShape == 0) {
    vec4 tex = texture(u_texture, v_uv);
    float softMask = (v_softness > 0.005)
                         ? (1.0 - smoothstep(max(0.0, 1.0 - f), 1.0, r))
                         : 1.0;
    float a = clamp(v_color.a * tex.a * softMask, 0.0, 1.0);
    vec3 rgb = v_color.rgb * tex.rgb;
    if (u_premulRgb > 0.5) {
      rgb *= a;
    }
    fragColor = vec4(rgb * u_hdrGain, a);
    return;
  }

  float shapeA = 0.0;
  if (u_particleShape == 1) {
    // Soft shaded sphere
    shapeA = 1.0 - smoothstep(max(0.0, 1.0 - f), 1.0, r);
  } else if (u_particleShape == 2) {
    // Glowing core + exponential halo
    float core = 1.0 - smoothstep(0.0, mix(0.45, 0.85, f), r);
    float halo = exp(-r * r * mix(8.0, 2.2, f));
    float mask = 1.0 - smoothstep(0.82, 1.0, r);
    shapeA = clamp((core * 0.55 + halo * 0.85) * mask, 0.0, 1.0);
  } else if (u_particleShape == 3) {
    // 4-point star glint + central core
    float k = mix(14.0, 4.5, f);
    float rays = exp(-abs(p.x) * k) * exp(-abs(p.y) * 2.8) +
                 exp(-abs(p.y) * k) * exp(-abs(p.x) * 2.8);
    float core = exp(-r * r * mix(10.0, 3.2, f));
    float mask = 1.0 - smoothstep(0.82, 1.0, r);
    shapeA = clamp((core * 0.85 + rays * 0.9) * mask, 0.0, 1.0);
  } else if (u_particleShape == 4) {
    // Optical lens BokehRing with bright rim
    float disc = 1.0 - smoothstep(max(0.0, 1.0 - f * 0.45), 1.0, r);
    float rim = smoothstep(0.48, 0.84, r) * (1.0 - smoothstep(0.88, 1.0, r));
    shapeA = clamp(disc * 0.42 + rim * 0.85, 0.0, 1.0);
  } else {
    // Streaklet / elongated spark
    float core = exp(-(p.y * p.y * 9.0 + p.x * p.x * 2.2));
    float mask = (1.0 - smoothstep(0.85, 1.0, abs(p.x))) * (1.0 - smoothstep(0.80, 1.0, abs(p.y)));
    shapeA = clamp(core * mask, 0.0, 1.0);
  }

  float a = clamp(v_color.a * shapeA, 0.0, 1.0);
  vec3 rgb = (u_premulRgb > 0.5) ? (v_color.rgb * a) : v_color.rgb;
  fragColor = vec4(rgb * u_hdrGain, a);
}
)glsl";

// Physical Bloom prefilter with Karis anti-flicker luminance weight and soft-knee threshold.
inline constexpr const char* kBloomPrefilterFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_texture;
uniform vec2 u_texelSize;
uniform float u_exposure;
uniform float u_threshold;
uniform float u_softKnee;
uniform float u_anamorphic;

out vec4 fragColor;

vec3 sanitizeHdr(vec3 c) {
  vec3 safe = vec3(
      isnan(c.x) ? 0.0 : c.x,
      isnan(c.y) ? 0.0 : c.y,
      isnan(c.z) ? 0.0 : c.z);
  return clamp(safe, vec3(0.0), vec3(64.0));
}

vec3 extendedSrgbToLinear(vec3 c) {
  return vec3(
      c.r > 0.0 ? pow(c.r, 2.2) : 0.0,
      c.g > 0.0 ? pow(c.g, 2.2) : 0.0,
      c.b > 0.0 ? pow(c.b, 2.2) : 0.0);
}

vec3 applySoftThreshold(vec3 col, float thr, float kneeFrac) {
  float br = max(col.r, max(col.g, col.b));
  float knee = max(thr * clamp(kneeFrac, 0.0, 1.0), 1e-5);
  float soft = clamp(br - thr + knee, 0.0, 2.0 * knee);
  soft = (soft * soft) / (4.0 * knee);
  float contrib = max(soft, br - thr) / max(br, 1e-5);
  return col * max(contrib, 0.0);
}

void main() {
  vec2 off = u_texelSize * vec2(1.0 + clamp(u_anamorphic, 0.0, 1.0) * 1.5, 1.0);
  vec3 s0 = applySoftThreshold(extendedSrgbToLinear(sanitizeHdr(texture(u_texture, clamp(v_uv + vec2(-off.x, -off.y), 0.0, 1.0)).rgb)) * u_exposure, u_threshold, u_softKnee);
  vec3 s1 = applySoftThreshold(extendedSrgbToLinear(sanitizeHdr(texture(u_texture, clamp(v_uv + vec2( off.x, -off.y), 0.0, 1.0)).rgb)) * u_exposure, u_threshold, u_softKnee);
  vec3 s2 = applySoftThreshold(extendedSrgbToLinear(sanitizeHdr(texture(u_texture, clamp(v_uv + vec2(-off.x,  off.y), 0.0, 1.0)).rgb)) * u_exposure, u_threshold, u_softKnee);
  vec3 s3 = applySoftThreshold(extendedSrgbToLinear(sanitizeHdr(texture(u_texture, clamp(v_uv + vec2( off.x,  off.y), 0.0, 1.0)).rgb)) * u_exposure, u_threshold, u_softKnee);

  float w0 = 1.0 / (1.0 + dot(s0, vec3(0.2126, 0.7152, 0.0722)));
  float w1 = 1.0 / (1.0 + dot(s1, vec3(0.2126, 0.7152, 0.0722)));
  float w2 = 1.0 / (1.0 + dot(s2, vec3(0.2126, 0.7152, 0.0722)));
  float w3 = 1.0 / (1.0 + dot(s3, vec3(0.2126, 0.7152, 0.0722)));

  vec3 result = (s0 * w0 + s1 * w1 + s2 * w2 + s3 * w3) / max(w0 + w1 + w2 + w3, 1e-5);
  fragColor = vec4(max(result, vec3(0.0)), 1.0);
}
)glsl";

// Jimenez (SIGGRAPH 2014) 13-tap energy-conserving bloom downsample pass.
inline constexpr const char* kBloomDownFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_texture;
uniform vec2 u_texelSize;
uniform float u_anamorphic;

out vec4 fragColor;

void main() {
  float aStretch = clamp(u_anamorphic, 0.0, 1.0);
  vec2 s = u_texelSize * vec2(1.0 + aStretch * 2.5, max(0.25, 1.0 - aStretch * 0.35));

  vec3 a = max(texture(u_texture, clamp(v_uv + vec2(-2.0 * s.x,  2.0 * s.y), 0.0, 1.0)).rgb, vec3(0.0));
  vec3 b = max(texture(u_texture, clamp(v_uv + vec2( 0.0,        2.0 * s.y), 0.0, 1.0)).rgb, vec3(0.0));
  vec3 c = max(texture(u_texture, clamp(v_uv + vec2( 2.0 * s.x,  2.0 * s.y), 0.0, 1.0)).rgb, vec3(0.0));

  vec3 d = max(texture(u_texture, clamp(v_uv + vec2(-2.0 * s.x,  0.0),       0.0, 1.0)).rgb, vec3(0.0));
  vec3 e = max(texture(u_texture, v_uv).rgb, vec3(0.0));
  vec3 f = max(texture(u_texture, clamp(v_uv + vec2( 2.0 * s.x,  0.0),       0.0, 1.0)).rgb, vec3(0.0));

  vec3 g = max(texture(u_texture, clamp(v_uv + vec2(-2.0 * s.x, -2.0 * s.y), 0.0, 1.0)).rgb, vec3(0.0));
  vec3 h = max(texture(u_texture, clamp(v_uv + vec2( 0.0,       -2.0 * s.y), 0.0, 1.0)).rgb, vec3(0.0));
  vec3 i = max(texture(u_texture, clamp(v_uv + vec2( 2.0 * s.x, -2.0 * s.y), 0.0, 1.0)).rgb, vec3(0.0));

  vec3 j = max(texture(u_texture, clamp(v_uv + vec2(-s.x,  s.y), 0.0, 1.0)).rgb, vec3(0.0));
  vec3 k = max(texture(u_texture, clamp(v_uv + vec2( s.x,  s.y), 0.0, 1.0)).rgb, vec3(0.0));
  vec3 l = max(texture(u_texture, clamp(v_uv + vec2(-s.x, -s.y), 0.0, 1.0)).rgb, vec3(0.0));
  vec3 m = max(texture(u_texture, clamp(v_uv + vec2( s.x, -s.y), 0.0, 1.0)).rgb, vec3(0.0));

  vec3 sum = e * 0.125;
  sum += (a + c + g + i) * 0.03125;
  sum += (b + d + f + h) * 0.0625;
  sum += (j + k + l + m) * 0.125;
  fragColor = vec4(max(sum, vec3(0.0)), 1.0);
}
)glsl";

// 9-tap tent filter bloom upsample pass with scatter blending.
inline constexpr const char* kBloomUpFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_coarseTex;
uniform sampler2D u_fineTex;
uniform vec2 u_texelSize;
uniform float u_scatter;
uniform float u_anamorphic;

out vec4 fragColor;

void main() {
  float aStretch = clamp(u_anamorphic, 0.0, 1.0);
  vec2 s = u_texelSize * vec2(1.0 + aStretch * 2.5, max(0.25, 1.0 - aStretch * 0.35));

  vec3 up = vec3(0.0);
  up += texture(u_coarseTex, clamp(v_uv + vec2(-s.x,  s.y), 0.0, 1.0)).rgb * 1.0;
  up += texture(u_coarseTex, clamp(v_uv + vec2( 0.0,  s.y), 0.0, 1.0)).rgb * 2.0;
  up += texture(u_coarseTex, clamp(v_uv + vec2( s.x,  s.y), 0.0, 1.0)).rgb * 1.0;

  up += texture(u_coarseTex, clamp(v_uv + vec2(-s.x,  0.0), 0.0, 1.0)).rgb * 2.0;
  up += texture(u_coarseTex, v_uv).rgb * 4.0;
  up += texture(u_coarseTex, clamp(v_uv + vec2( s.x,  0.0), 0.0, 1.0)).rgb * 2.0;

  up += texture(u_coarseTex, clamp(v_uv + vec2(-s.x, -s.y), 0.0, 1.0)).rgb * 1.0;
  up += texture(u_coarseTex, clamp(v_uv + vec2( 0.0, -s.y), 0.0, 1.0)).rgb * 2.0;
  up += texture(u_coarseTex, clamp(v_uv + vec2( s.x, -s.y), 0.0, 1.0)).rgb * 1.0;
  up *= (1.0 / 16.0);

  vec3 fine = max(texture(u_fineTex, v_uv).rgb, vec3(0.0));
  vec3 blended = mix(fine, max(up, vec3(0.0)), clamp(u_scatter, 0.05, 0.98));
  fragColor = vec4(max(blended, vec3(0.0)), 1.0);
}
)glsl";

// HDR composite + Filmic Tonemapping (ACES / AgX / PBR Neutral / Reinhard) + Color Grading + Dithering.
inline constexpr const char* kTonemapFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_sceneTex;
uniform sampler2D u_bloomTex;
uniform int u_tonemapMode;  // 0=None, 1=ACES, 2=AgX, 3=PBRNeutral, 4=Reinhard
uniform float u_exposure;
uniform float u_bloomAmount;
uniform vec3 u_bloomTint;
uniform float u_contrast;
uniform float u_saturation;
uniform float u_gamma;
uniform float u_vignette;
uniform float u_dither;

out vec4 fragColor;

vec3 sanitizeHdr(vec3 c) {
  vec3 safe = vec3(
      isnan(c.x) ? 0.0 : c.x,
      isnan(c.y) ? 0.0 : c.y,
      isnan(c.z) ? 0.0 : c.z);
  return clamp(safe, vec3(0.0), vec3(64.0));
}

vec3 srgbToLinear(vec3 c) {
  return vec3(
      c.r > 0.0 ? pow(c.r, 2.2) : 0.0,
      c.g > 0.0 ? pow(c.g, 2.2) : 0.0,
      c.b > 0.0 ? pow(c.b, 2.2) : 0.0);
}

vec3 linearToSrgb(vec3 c) {
  c = clamp(c, 0.0, 1.0);
  return vec3(
      c.r > 0.0 ? pow(c.r, 1.0 / 2.2) : 0.0,
      c.g > 0.0 ? pow(c.g, 1.0 / 2.2) : 0.0,
      c.b > 0.0 ? pow(c.b, 1.0 / 2.2) : 0.0);
}

vec3 tonemapAces(vec3 color) {
  const mat3 kAcesInput = mat3(
      vec3(0.59719, 0.07600, 0.02840),
      vec3(0.35458, 0.90834, 0.13383),
      vec3(0.04823, 0.01566, 0.83777));
  const mat3 kAcesOutput = mat3(
      vec3( 1.60475, -0.10208, -0.00327),
      vec3(-0.53108,  1.10813, -0.07276),
      vec3(-0.07367, -0.00605,  1.07602));
  vec3 v = kAcesInput * color;
  vec3 a = v * (v + 0.0245786) - 0.000090537;
  vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
  return clamp(kAcesOutput * (a / b), 0.0, 1.0);
}

vec3 agxDefaultContrastApprox(vec3 x) {
  vec3 x2 = x * x;
  vec3 x4 = x2 * x2;
  return + 15.5 * x4 * x2
         - 40.14 * x4 * x
         + 31.96 * x4
         - 6.868 * x2 * x
         + 0.4298 * x2
         + 0.1191 * x
         - 0.00232;
}

vec3 tonemapAgx(vec3 val) {
  const mat3 kAgxInset = mat3(
      vec3(0.84247906, 0.04232824, 0.04237565),
      vec3(0.07843360, 0.87846864, 0.07843360),
      vec3(0.07922375, 0.07916613, 0.87914297));
  const mat3 kAgxOutset = mat3(
      vec3( 1.19687900, -0.05289685, -0.05297163),
      vec3(-0.09802088,  1.15190313, -0.09804345),
      vec3(-0.09902974, -0.09896118,  1.15107367));
  const float minEv = -12.47393;
  const float maxEv = 4.026069;

  val = kAgxInset * max(val, vec3(1e-10));
  val = clamp(log2(val), minEv, maxEv);
  val = (val - minEv) / (maxEv - minEv);
  val = agxDefaultContrastApprox(val);
  val = clamp(kAgxOutset * val, 0.0, 1.0);
  return vec3(
      val.r > 0.0 ? pow(val.r, 2.2) : 0.0,
      val.g > 0.0 ? pow(val.g, 2.2) : 0.0,
      val.b > 0.0 ? pow(val.b, 2.2) : 0.0);
}

vec3 tonemapPbrNeutral(vec3 color) {
  const float startCompression = 0.8 - 0.04;
  const float desaturation = 0.15;
  float x = min(color.r, min(color.g, color.b));
  float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
  color -= offset;
  float peak = max(color.r, max(color.g, color.b));
  if (peak < startCompression) return max(color, vec3(0.0));
  const float d = 1.0 - startCompression;
  float newPeak = 1.0 - d * d / (peak + d - startCompression);
  color *= newPeak / max(peak, 1e-6);
  float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
  return clamp(mix(color, vec3(newPeak), g), 0.0, 1.0);
}

vec3 tonemapReinhard(vec3 color) {
  float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
  if (luma <= 1e-6) return vec3(0.0);
  float mappedLuma = luma / (1.0 + luma);
  return clamp(color * (mappedLuma / luma), 0.0, 1.0);
}

void main() {
  vec4 sceneSample = texture(u_sceneTex, v_uv);
  vec3 lin = srgbToLinear(sanitizeHdr(sceneSample.rgb)) * max(u_exposure, 0.0);

  if (u_bloomAmount > 1e-4) {
    vec3 bloom = sanitizeHdr(texture(u_bloomTex, v_uv).rgb);
    lin += bloom * u_bloomAmount * u_bloomTint;
  }

  vec3 mapped;
  if (u_tonemapMode == 1) {
    mapped = tonemapAces(lin);
  } else if (u_tonemapMode == 2) {
    mapped = tonemapAgx(lin);
  } else if (u_tonemapMode == 3) {
    mapped = tonemapPbrNeutral(lin);
  } else if (u_tonemapMode == 4) {
    mapped = tonemapReinhard(lin);
  } else {
    mapped = clamp(lin, 0.0, 1.0);
  }

  vec3 rgb = linearToSrgb(mapped);
  rgb = clamp((rgb - 0.5) * u_contrast + 0.5, 0.0, 1.0);
  float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
  rgb = clamp(mix(vec3(luma), rgb, u_saturation), 0.0, 1.0);

  if (abs(u_gamma - 1.0) > 1e-4) {
    float invGamma = 1.0 / max(u_gamma, 0.1);
    rgb = vec3(
        rgb.r > 0.0 ? pow(rgb.r, invGamma) : 0.0,
        rgb.g > 0.0 ? pow(rgb.g, invGamma) : 0.0,
        rgb.b > 0.0 ? pow(rgb.b, invGamma) : 0.0);
  }

  if (u_vignette > 1e-4) {
    vec2 d = v_uv - vec2(0.5);
    float vig = 1.0 - clamp(dot(d, d) * 2.2 * u_vignette, 0.0, 0.92);
    rgb *= vig;
  }

  if (u_dither > 0.5) {
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    rgb = clamp(rgb + (n - 0.5) / 255.0, 0.0, 1.0);
  }

  fragColor = vec4(rgb, clamp(sceneSample.a, 0.0, 1.0));
}
)glsl";

}  // namespace iota::engine::shaders
