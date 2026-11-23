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
  v_uv = a_pos.xy * 0.5 + vec2(0.5);
  gl_Position = vec4(a_pos.xy, 0.0, 1.0);
}
)glsl";

inline constexpr const char* kBlurStackFrag = R"glsl(#version 450 core
in vec2 v_uv;

uniform sampler2D u_texture;
uniform float blurWH;
uniform vec2 resolution;

out vec4 fragColor;

void main() {
  vec2 stepUv = resolution * blurWH;
  vec4 acc = vec4(0.0);
  acc += texture(u_texture, clamp(v_uv + vec2(-stepUv.x, -stepUv.y), 0.0, 1.0)) * 0.0625;
  acc += texture(u_texture, clamp(v_uv + vec2(0.0,       -stepUv.y), 0.0, 1.0)) * 0.1250;
  acc += texture(u_texture, clamp(v_uv + vec2(+stepUv.x, -stepUv.y), 0.0, 1.0)) * 0.0625;

  acc += texture(u_texture, clamp(v_uv + vec2(-stepUv.x, 0.0),       0.0, 1.0)) * 0.1250;
  acc += texture(u_texture, v_uv)                                               * 0.2500;
  acc += texture(u_texture, clamp(v_uv + vec2(+stepUv.x, 0.0),       0.0, 1.0)) * 0.1250;

  acc += texture(u_texture, clamp(v_uv + vec2(-stepUv.x, +stepUv.y), 0.0, 1.0)) * 0.0625;
  acc += texture(u_texture, clamp(v_uv + vec2(0.0,       +stepUv.y), 0.0, 1.0)) * 0.1250;
  acc += texture(u_texture, clamp(v_uv + vec2(+stepUv.x, +stepUv.y), 0.0, 1.0)) * 0.0625;
  fragColor = acc;
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

inline constexpr const char* kMotionBlurFrag = R"glsl(#version 450 core
in vec4 v_color;
in vec2 v_uv;

uniform sampler2D u_texture;
uniform vec2 posAmount;
uniform vec2 scaleAmount;

out vec4 fragColor;

void main() {
  vec2 d = posAmount + abs(scaleAmount) * ((v_uv - vec2(0.5)) * 2.0);
  const float w[9] = float[9](
    0.063327, 0.093095, 0.122589, 0.144599, 0.152781,
    0.144599, 0.122589, 0.093095, 0.063327
  );
  vec4 sum = vec4(0.0);
  for (int i = -4; i <= 4; ++i) {
    vec2 uv = clamp(v_uv + float(i) * d, 0.0, 1.0);
    sum += texture(u_texture, uv) * w[i + 4];
  }
  fragColor = sum * v_color;
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

}  // namespace iota::engine::shaders
