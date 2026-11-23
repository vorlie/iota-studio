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

out vec4 fragColor;

void main() {
  fragColor = v_color * texture(u_texture, v_uv);
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

  fragColor = vec4(c.rgb * maskFactor, c.a * maskFactor);
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

out vec4 fragColor;

void main() {
  vec4 c0 = texture(u_texture, clamp(v_uv + dirAmount, 0.0, 1.0));
  vec4 c1 = texture(u_texture, v_uv);
  vec4 c2 = texture(u_texture, clamp(v_uv - dirAmount, 0.0, 1.0));

  vec3 rgb = c0.rgb * splitColor0 + c1.rgb * splitColor1 + c2.rgb * splitColor2;
  float alpha = (c0.a + c1.a + c2.a) / 3.0;
  fragColor = vec4(rgb, alpha) * v_color;
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

out vec4 fragColor;

float ign(vec2 p) {
  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
  float motionMag = length(u_posAmount) + length(u_scaleAmount) + abs(u_rotAmount);
  if (motionMag < 1e-5) {
    fragColor = texture(u_texture, v_uv) * v_color;
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

  fragColor = (acc / float(count)) * v_color;
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
    fragColor = vec4(1.0 - (1.0 - clamp(curr.rgb, 0.0, 1.0)) * (1.0 - clamp(prev.rgb, 0.0, 1.0)),
                     max(curr.a, prev.a));
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
    fragColor = vec4(rgb, a);
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
  fragColor = vec4(rgb, a);
}
)glsl";

}  // namespace iota::engine::shaders
