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

#include "iota/rhi/webgl2_device.hpp"

#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iota::rhi {

namespace {

constexpr GLuint kInvalidGlId = std::numeric_limits<GLuint>::max();

GLint toGlWrap(TextureWrap wrap) noexcept {
  switch (wrap) {
    case TextureWrap::Repeat: return GL_REPEAT;
    case TextureWrap::MirroredRepeat: return GL_MIRRORED_REPEAT;
    default: return GL_CLAMP_TO_EDGE;
  }
}

GLenum toGlTopology(PrimitiveTopology topo) noexcept {
  switch (topo) {
    case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
    case PrimitiveTopology::Lines: return GL_LINES;
    default: return GL_TRIANGLES;
  }
}

PixelFormat resolveSupportedFormat(PixelFormat requested, bool hasColorBufferFloat) noexcept {
  if (requested == PixelFormat::Rgba8 || !hasColorBufferFloat) {
    return PixelFormat::Rgba8;
  }
  // Use GL_RGBA16F (resolved via Rgba16F) for HDR targets in WebGL2 since EXT_color_buffer_float
  // guarantees RGBA16F completeness with GL_RGBA + GL_HALF_FLOAT uploads.
  return PixelFormat::Rgba16F;
}

GLint toGlInternalFormat(PixelFormat fmt) noexcept {
  switch (fmt) {
    case PixelFormat::Rgba16F:
    case PixelFormat::R11G11B10F:
      return GL_RGBA16F;
    case PixelFormat::Rgba8:
    default:
      return GL_RGBA8;
  }
}

GLenum toGlUploadPixelType(PixelFormat fmt, bool hasPixelData) noexcept {
  if (hasPixelData) {
    return GL_UNSIGNED_BYTE;
  }
  switch (fmt) {
    case PixelFormat::Rgba16F:
    case PixelFormat::R11G11B10F:
      return GL_HALF_FLOAT;
    case PixelFormat::Rgba8:
    default:
      return GL_UNSIGNED_BYTE;
  }
}

/// Converts GLSL 450/330 desktop shaders into valid GLSL ES 3.00 (`#version 300 es`).
std::string adaptGlslToEs300(std::string_view src) {
  if (src.size() >= 3 &&
      static_cast<unsigned char>(src[0]) == 0xEF &&
      static_cast<unsigned char>(src[1]) == 0xBB &&
      static_cast<unsigned char>(src[2]) == 0xBF) {
    src.remove_prefix(3);
  }

  std::string body(src);
  size_t pos = 0;
  bool replacedVersion = false;
  while (pos < body.size()) {
    const size_t lineStart = pos;
    size_t lineEnd = body.find('\n', pos);
    const size_t nextLine = (lineEnd == std::string::npos) ? body.size() : (lineEnd + 1);
    if (lineEnd == std::string::npos) lineEnd = body.size();

    size_t first = lineStart;
    while (first < lineEnd && (body[first] == ' ' || body[first] == '\t' || body[first] == '\r')) {
      ++first;
    }
    if (first < lineEnd) {
      if (body.compare(first, 8, "#version") == 0) {
        body.erase(lineStart, nextLine - lineStart);
        replacedVersion = true;
      }
      break;
    }
    pos = nextLine;
  }
  (void)replacedVersion;

  std::string out = "#version 300 es\n";
  out += "precision highp float;\n";
  out += "precision highp int;\n";
  out += "precision highp sampler2D;\n";
  out += body;
  return out;
}

struct TransparentStringHash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
  size_t operator()(const std::string& s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
};

struct TextureRecord {
  GLuint glId = 0;
  int width = 0;
  int height = 0;
  TextureFilter filter = TextureFilter::Linear;
  TextureWrap wrap = TextureWrap::ClampToEdge;
  PixelFormat format = PixelFormat::Rgba8;
};

struct RenderTargetRecord {
  GLuint fbo = 0;
  TextureHandle colorTex{};
  int width = 0;
  int height = 0;
  TextureFilter filter = TextureFilter::Linear;
  PixelFormat format = PixelFormat::Rgba8;
};

struct ShaderRecord {
  GLuint program = 0;
  std::unordered_map<std::string, GLint, TransparentStringHash, std::equal_to<>> uniforms;
};

template <typename T>
struct SlotEntry {
  bool alive = false;
  T value{};
};

}  // namespace

struct WebGl2RenderDevice::Impl {
  bool ready = false;
  bool hasColorBufferFloat = false;
  bool hasFloatLinear = false;

  std::vector<SlotEntry<TextureRecord>> textures;
  std::vector<SlotEntry<RenderTargetRecord>> renderTargets;
  std::vector<SlotEntry<ShaderRecord>> shaders;

  // Static fullscreen quad
  GLuint fsVao = 0;
  GLuint fsVbo = 0;
  GLuint fsIbo = 0;

  // Dynamic streaming geometry buffers (orphaned GL_STREAM_DRAW)
  GLuint dynVao = 0;
  GLuint dynVbo = 0;
  GLuint dynIbo = 0;

  std::vector<uint8_t> flipRowScratch;

  // Cached GL state
  GLuint activeProgram = kInvalidGlId;
  GLuint cachedVao = kInvalidGlId;
  bool cachedFboValid = false;
  GLuint cachedFbo = 0;
  int cachedVpW = -1;
  int cachedVpH = -1;
  bool cachedBlendValid = false;
  iota::core::BlendMode cachedBlendMode = iota::core::BlendMode::Alpha;
  bool cachedScissorValid = false;
  bool cachedScissorEnabled = false;
  int cachedScissorX = 0;
  int cachedScissorY = 0;
  int cachedScissorW = 0;
  int cachedScissorH = 0;
  int cachedActiveTexUnit = -1;
  std::array<GLuint, 16> cachedBoundTex{};

  Impl() { cachedBoundTex.fill(kInvalidGlId); }

  template <typename T>
  static T* findSlot(std::vector<SlotEntry<T>>& table, uint32_t id) noexcept {
    if (id == 0 || id > table.size()) {
      return nullptr;
    }
    auto& slot = table[static_cast<size_t>(id - 1)];
    return slot.alive ? &slot.value : nullptr;
  }

  template <typename T>
  static const T* findSlot(const std::vector<SlotEntry<T>>& table, uint32_t id) noexcept {
    if (id == 0 || id > table.size()) {
      return nullptr;
    }
    const auto& slot = table[static_cast<size_t>(id - 1)];
    return slot.alive ? &slot.value : nullptr;
  }

  template <typename T>
  static uint32_t allocSlot(std::vector<SlotEntry<T>>& table, T&& val) {
    table.push_back(SlotEntry<T>{true, std::move(val)});
    return static_cast<uint32_t>(table.size());
  }

  void invalidateStateCache() noexcept {
    activeProgram = kInvalidGlId;
    cachedVao = kInvalidGlId;
    cachedFboValid = false;
    cachedVpW = -1;
    cachedVpH = -1;
    cachedBlendValid = false;
    cachedScissorValid = false;
    cachedActiveTexUnit = -1;
    cachedBoundTex.fill(kInvalidGlId);
  }

  void bindVaoCached(GLuint vao) noexcept {
    if (cachedVao != vao) {
      glBindVertexArray(vao);
      cachedVao = vao;
    }
  }

  static void configureVertexAttributes() noexcept {
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexPosColUv),
                          reinterpret_cast<const void*>(offsetof(VertexPosColUv, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexPosColUv),
                          reinterpret_cast<const void*>(offsetof(VertexPosColUv, abgr)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(VertexPosColUv),
                          reinterpret_cast<const void*>(offsetof(VertexPosColUv, u)));
  }

  GLuint compileStage(GLenum stage, std::string_view rawSrc, std::string* outError) {
    const std::string src = adaptGlslToEs300(rawSrc);
    const GLuint sh = glCreateShader(stage);
    const char* ptr = src.data();
    const GLint len = static_cast<GLint>(src.size());
    glShaderSource(sh, 1, &ptr, &len);
    glCompileShader(sh);

    GLint status = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
      GLint logLen = 0;
      glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
      std::string log(static_cast<size_t>(std::max(1, logLen)), '\0');
      glGetShaderInfoLog(sh, logLen, nullptr, log.data());
      while (!log.empty() && log.back() == '\0') log.pop_back();
      if (outError) {
        *outError = (stage == GL_VERTEX_SHADER ? "Vertex shader compile failed: "
                                               : "Fragment shader compile failed: ") +
                    log;
      }
      glDeleteShader(sh);
      return 0;
    }
    return sh;
  }

  GLint findUniform(ShaderHandle sh, std::string_view name) {
    auto* rec = findSlot(shaders, sh.id);
    if (!rec || rec->program == 0) {
      return -1;
    }
    if (auto uIt = rec->uniforms.find(name); uIt != rec->uniforms.end()) {
      return uIt->second;
    }
    std::string key(name);
    const GLint loc = glGetUniformLocation(rec->program, key.c_str());
    rec->uniforms.emplace(std::move(key), loc);
    return loc;
  }
};

WebGl2RenderDevice::WebGl2RenderDevice() : impl_(std::make_unique<Impl>()) {}

WebGl2RenderDevice::~WebGl2RenderDevice() {
  shutdown();
}

bool WebGl2RenderDevice::initialize(std::string* outError) {
  if (impl_->ready) {
    return true;
  }

  const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_get_current_context();
  if (ctx <= 0) {
    if (outError) {
      *outError = "No active WebGL2 context";
    }
    return false;
  }

  impl_->hasColorBufferFloat =
      (emscripten_webgl_enable_extension(ctx, "EXT_color_buffer_float") == EM_TRUE) ||
      (emscripten_webgl_enable_extension(ctx, "EXT_color_buffer_half_float") == EM_TRUE);
  impl_->hasFloatLinear =
      (emscripten_webgl_enable_extension(ctx, "OES_texture_float_linear") == EM_TRUE);

  // 1. Static fullscreen clip-space quad
  static constexpr VertexPosColUv kFullscreenQuad[4] = {
      {-1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
      {+1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
      {+1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
      {-1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
  };
  static constexpr uint32_t kFullscreenIdx[6] = {0, 1, 2, 0, 2, 3};

  glGenVertexArrays(1, &impl_->fsVao);
  glGenBuffers(1, &impl_->fsVbo);
  glGenBuffers(1, &impl_->fsIbo);
  glBindVertexArray(impl_->fsVao);
  glBindBuffer(GL_ARRAY_BUFFER, impl_->fsVbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kFullscreenQuad), kFullscreenQuad, GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl_->fsIbo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kFullscreenIdx), kFullscreenIdx, GL_STATIC_DRAW);
  Impl::configureVertexAttributes();

  // 2. Dynamic streaming VAO & buffers
  glGenVertexArrays(1, &impl_->dynVao);
  glGenBuffers(1, &impl_->dynVbo);
  glGenBuffers(1, &impl_->dynIbo);
  glBindVertexArray(impl_->dynVao);
  glBindBuffer(GL_ARRAY_BUFFER, impl_->dynVbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl_->dynIbo);
  Impl::configureVertexAttributes();

  glBindVertexArray(0);
  impl_->invalidateStateCache();
  impl_->ready = true;
  return true;
}

void WebGl2RenderDevice::shutdown() {
  if (!impl_ || !impl_->ready) {
    return;
  }

  for (auto& slot : impl_->renderTargets) {
    if (slot.alive && slot.value.fbo != 0) {
      glDeleteFramebuffers(1, &slot.value.fbo);
    }
  }
  impl_->renderTargets.clear();

  for (auto& slot : impl_->textures) {
    if (slot.alive && slot.value.glId != 0) {
      glDeleteTextures(1, &slot.value.glId);
    }
  }
  impl_->textures.clear();

  for (auto& slot : impl_->shaders) {
    if (slot.alive && slot.value.program != 0) {
      glDeleteProgram(slot.value.program);
    }
  }
  impl_->shaders.clear();

  auto deleteVaoBuf = [&](GLuint& vao, GLuint& vbo, GLuint& ibo) {
    if (ibo != 0) { glDeleteBuffers(1, &ibo); ibo = 0; }
    if (vbo != 0) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (vao != 0) { glDeleteVertexArrays(1, &vao); vao = 0; }
  };
  deleteVaoBuf(impl_->fsVao, impl_->fsVbo, impl_->fsIbo);
  deleteVaoBuf(impl_->dynVao, impl_->dynVbo, impl_->dynIbo);

  impl_->invalidateStateCache();
  impl_->ready = false;
}

void WebGl2RenderDevice::beginFrame() {
  if (!impl_->ready) {
    return;
  }
  impl_->invalidateStateCache();
}

void WebGl2RenderDevice::endFrame() {
  if (!impl_->ready) {
    return;
  }
  impl_->bindVaoCached(0);
  impl_->invalidateStateCache();
}

TextureHandle WebGl2RenderDevice::createTexture2D(const TextureDesc& desc, std::string* outError) {
  if (!impl_->ready || desc.width <= 0 || desc.height <= 0) {
    if (outError) {
      *outError = "Invalid texture dimensions or uninitialized WebGL2 device";
    }
    return {};
  }

  const PixelFormat effectiveFormat =
      (desc.rgbaPixels != nullptr) ? PixelFormat::Rgba8
                                   : resolveSupportedFormat(desc.format, impl_->hasColorBufferFloat);

  GLuint glTex = 0;
  glGenTextures(1, &glTex);
  glBindTexture(GL_TEXTURE_2D, glTex);
  if (impl_->cachedActiveTexUnit >= 0 && impl_->cachedActiveTexUnit < 16) {
    impl_->cachedBoundTex[static_cast<size_t>(impl_->cachedActiveTexUnit)] = kInvalidGlId;
  }

  const GLint minFilter = desc.generateMipmaps
                              ? GL_LINEAR_MIPMAP_LINEAR
                              : (desc.filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR);
  const GLint magFilter = (desc.filter == TextureFilter::Nearest) ? GL_NEAREST : GL_LINEAR;
  const GLint wrap = toGlWrap(desc.wrap);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  const GLint internalFormat = toGlInternalFormat(effectiveFormat);
  const GLenum pixelType = toGlUploadPixelType(effectiveFormat, desc.rgbaPixels != nullptr);
  glTexImage2D(GL_TEXTURE_2D,
               0,
               internalFormat,
               desc.width,
               desc.height,
               0,
               GL_RGBA,
               pixelType,
               desc.rgbaPixels);

  if (desc.generateMipmaps && desc.rgbaPixels != nullptr) {
    glGenerateMipmap(GL_TEXTURE_2D);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  const uint32_t id = Impl::allocSlot(
      impl_->textures, TextureRecord{glTex, desc.width, desc.height, desc.filter, desc.wrap, effectiveFormat});
  return TextureHandle{id};
}

void WebGl2RenderDevice::updateTexture2D(TextureHandle tex, int width, int height, const uint8_t* rgbaPixels) {
  auto* rec = Impl::findSlot(impl_->textures, tex.id);
  if (!rec || width <= 0 || height <= 0 || !rgbaPixels) {
    return;
  }
  glBindTexture(GL_TEXTURE_2D, rec->glId);
  if (impl_->cachedActiveTexUnit >= 0 && impl_->cachedActiveTexUnit < 16) {
    impl_->cachedBoundTex[static_cast<size_t>(impl_->cachedActiveTexUnit)] = kInvalidGlId;
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (rec->width == width && rec->height == height && rec->format == PixelFormat::Rgba8) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
  } else {
    rec->format = PixelFormat::Rgba8;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
    rec->width = width;
    rec->height = height;
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

void WebGl2RenderDevice::updateTexture2DRegion(TextureHandle tex,
                                               int x,
                                               int y,
                                               int width,
                                               int height,
                                               const uint8_t* rgbaPixels) {
  auto* rec = Impl::findSlot(impl_->textures, tex.id);
  if (!rec || !rgbaPixels || width <= 0 || height <= 0 ||
      x < 0 || y < 0 || x + width > rec->width || y + height > rec->height) {
    return;
  }
  glBindTexture(GL_TEXTURE_2D, rec->glId);
  if (impl_->cachedActiveTexUnit >= 0 && impl_->cachedActiveTexUnit < 16) {
    impl_->cachedBoundTex[static_cast<size_t>(impl_->cachedActiveTexUnit)] = kInvalidGlId;
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void WebGl2RenderDevice::destroyTexture(TextureHandle tex) {
  if (tex.id == 0 || tex.id > impl_->textures.size()) {
    return;
  }
  auto& slot = impl_->textures[static_cast<size_t>(tex.id - 1)];
  if (!slot.alive) {
    return;
  }
  const GLuint oldGlId = slot.value.glId;
  if (oldGlId != 0) {
    for (GLuint& cachedId : impl_->cachedBoundTex) {
      if (cachedId == oldGlId) {
        cachedId = kInvalidGlId;
      }
    }
    glDeleteTextures(1, &slot.value.glId);
  }
  slot.alive = false;
  slot.value = TextureRecord{};
}

bool WebGl2RenderDevice::textureSize(TextureHandle tex, int& outW, int& outH) const {
  const auto* rec = Impl::findSlot(impl_->textures, tex.id);
  if (!rec) {
    return false;
  }
  outW = rec->width;
  outH = rec->height;
  return true;
}

uint64_t WebGl2RenderDevice::nativeTextureId(TextureHandle tex) const noexcept {
  const auto* rec = Impl::findSlot(impl_->textures, tex.id);
  return rec ? static_cast<uint64_t>(rec->glId) : 0ull;
}

RenderTargetHandle WebGl2RenderDevice::createRenderTarget(int width,
                                                          int height,
                                                          TextureFilter filter,
                                                          PixelFormat format,
                                                          std::string* outError) {
  if (!impl_->ready || width <= 0 || height <= 0) {
    if (outError) {
      *outError = "Invalid render target dimensions";
    }
    return {};
  }

  PixelFormat effectiveFormat = resolveSupportedFormat(format, impl_->hasColorBufferFloat);

  auto tryCreate = [&](PixelFormat fmtToTry) -> std::pair<TextureHandle, GLuint> {
    TextureDesc td;
    td.width = width;
    td.height = height;
    td.rgbaPixels = nullptr;
    td.filter = filter;
    td.wrap = TextureWrap::ClampToEdge;
    td.format = fmtToTry;
    const TextureHandle colorTex = createTexture2D(td, outError);
    if (!colorTex) {
      return {{}, 0};
    }

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    const auto* texRec = Impl::findSlot(impl_->textures, colorTex.id);
    const GLuint glTex = texRec ? texRec->glId : 0;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex, 0);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    impl_->cachedFboValid = false;
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      glDeleteFramebuffers(1, &fbo);
      destroyTexture(colorTex);
      return {{}, 0};
    }
    return {colorTex, fbo};
  };

  auto [colorTex, fbo] = tryCreate(effectiveFormat);
  if (!colorTex && effectiveFormat != PixelFormat::Rgba8) {
    effectiveFormat = PixelFormat::Rgba8;
    impl_->hasColorBufferFloat = false;
    std::tie(colorTex, fbo) = tryCreate(effectiveFormat);
  }
  if (!colorTex) {
    if (outError && outError->empty()) {
      *outError = "WebGL2 framebuffer incomplete";
    }
    return {};
  }

  const uint32_t id = Impl::allocSlot(
      impl_->renderTargets, RenderTargetRecord{fbo, colorTex, width, height, filter, effectiveFormat});
  return RenderTargetHandle{id};
}

void WebGl2RenderDevice::resizeRenderTarget(RenderTargetHandle rt, int width, int height) {
  auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!rec || width <= 0 || height <= 0) {
    return;
  }
  if (rec->width == width && rec->height == height) {
    return;
  }
  auto* texRec = Impl::findSlot(impl_->textures, rec->colorTex.id);
  if (!texRec) {
    return;
  }
  const GLint internalFormat = toGlInternalFormat(texRec->format);
  const GLenum pixelType = toGlUploadPixelType(texRec->format, false);
  glBindTexture(GL_TEXTURE_2D, texRec->glId);
  if (impl_->cachedActiveTexUnit >= 0 && impl_->cachedActiveTexUnit < 16) {
    impl_->cachedBoundTex[static_cast<size_t>(impl_->cachedActiveTexUnit)] = kInvalidGlId;
  }
  glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, pixelType, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  texRec->width = width;
  texRec->height = height;
  rec->width = width;
  rec->height = height;
}

void WebGl2RenderDevice::destroyRenderTarget(RenderTargetHandle rt) {
  if (rt.id == 0 || rt.id > impl_->renderTargets.size()) {
    return;
  }
  auto& slot = impl_->renderTargets[static_cast<size_t>(rt.id - 1)];
  if (!slot.alive) {
    return;
  }
  if (slot.value.fbo != 0) {
    if (impl_->cachedFboValid && impl_->cachedFbo == slot.value.fbo) {
      impl_->cachedFboValid = false;
    }
    glDeleteFramebuffers(1, &slot.value.fbo);
  }
  destroyTexture(slot.value.colorTex);
  slot.alive = false;
  slot.value = RenderTargetRecord{};
}

TextureHandle WebGl2RenderDevice::renderTargetColorTexture(RenderTargetHandle rt) const noexcept {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  return rec ? rec->colorTex : TextureHandle{};
}

PixelFormat WebGl2RenderDevice::renderTargetFormat(RenderTargetHandle rt) const noexcept {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  return rec ? rec->format : PixelFormat::Rgba8;
}

bool WebGl2RenderDevice::renderTargetSize(RenderTargetHandle rt, int& outW, int& outH) const {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!rec) {
    return false;
  }
  outW = rec->width;
  outH = rec->height;
  return true;
}

ShaderHandle WebGl2RenderDevice::createShader(std::string_view vertexGlsl,
                                              std::string_view fragmentGlsl,
                                              std::string* outError) {
  if (!impl_->ready) {
    if (outError) {
      *outError = "WebGL2 device not initialized";
    }
    return {};
  }
  const GLuint vs = impl_->compileStage(GL_VERTEX_SHADER, vertexGlsl, outError);
  if (vs == 0) {
    return {};
  }
  const GLuint fs = impl_->compileStage(GL_FRAGMENT_SHADER, fragmentGlsl, outError);
  if (fs == 0) {
    glDeleteShader(vs);
    return {};
  }

  const GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linked = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &linked);
  if (!linked) {
    GLint logLen = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
    std::string log(static_cast<size_t>(std::max(1, logLen)), '\0');
    glGetProgramInfoLog(prog, logLen, nullptr, log.data());
    while (!log.empty() && log.back() == '\0') log.pop_back();
    if (outError) {
      *outError = "Shader program link failed: " + log;
    }
    glDeleteProgram(prog);
    return {};
  }

  const uint32_t id = Impl::allocSlot(impl_->shaders, ShaderRecord{prog, {}});
  return ShaderHandle{id};
}

void WebGl2RenderDevice::destroyShader(ShaderHandle sh) {
  if (sh.id == 0 || sh.id > impl_->shaders.size()) {
    return;
  }
  auto& slot = impl_->shaders[static_cast<size_t>(sh.id - 1)];
  if (!slot.alive) {
    return;
  }
  if (slot.value.program != 0) {
    if (impl_->activeProgram == slot.value.program) {
      glUseProgram(0);
      impl_->activeProgram = 0;
    }
    glDeleteProgram(slot.value.program);
  }
  slot.alive = false;
  slot.value = ShaderRecord{};
}

void WebGl2RenderDevice::bindRenderTarget(RenderTargetHandle rt, int viewportWidth, int viewportHeight) {
  if (!rt) {
    if (!impl_->cachedFboValid || impl_->cachedFbo != 0) {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      impl_->cachedFbo = 0;
      impl_->cachedFboValid = true;
    }
    if (viewportWidth > 0 && viewportHeight > 0 &&
        (impl_->cachedVpW != viewportWidth || impl_->cachedVpH != viewportHeight)) {
      glViewport(0, 0, viewportWidth, viewportHeight);
      impl_->cachedVpW = viewportWidth;
      impl_->cachedVpH = viewportHeight;
    }
    return;
  }

  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!rec) {
    return;
  }
  if (!impl_->cachedFboValid || impl_->cachedFbo != rec->fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, rec->fbo);
    impl_->cachedFbo = rec->fbo;
    impl_->cachedFboValid = true;
  }
  const int w = viewportWidth > 0 ? viewportWidth : rec->width;
  const int h = viewportHeight > 0 ? viewportHeight : rec->height;
  if (impl_->cachedVpW != w || impl_->cachedVpH != h) {
    glViewport(0, 0, w, h);
    impl_->cachedVpW = w;
    impl_->cachedVpH = h;
  }
}

void WebGl2RenderDevice::clear(float r, float g, float b, float a) {
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT);
}

void WebGl2RenderDevice::setBlendMode(iota::core::BlendMode mode) {
  using iota::core::BlendMode;
  if (impl_->cachedBlendValid && impl_->cachedBlendMode == mode) {
    return;
  }
  const bool needEnable = !impl_->cachedBlendValid || (impl_->cachedBlendMode == BlendMode::Opaque);
  impl_->cachedBlendValid = true;
  impl_->cachedBlendMode = mode;

  if (mode == BlendMode::Opaque) {
    glDisable(GL_BLEND);
    return;
  }
  if (needEnable) {
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
  }
  switch (mode) {
    case BlendMode::Alpha:
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::Screen:
      glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::Add:
      glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE_MINUS_DST_ALPHA, GL_ONE);
      break;
    case BlendMode::PreMulAlpha:
      glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::AddAlpha:
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_DST_ALPHA, GL_ONE);
      break;
    case BlendMode::Inverse:
      glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    default:
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
  }
}

void WebGl2RenderDevice::setScissor(bool enabled, int x, int y, int w, int h) {
  const int cw = std::max(0, w);
  const int ch = std::max(0, h);
  if (impl_->cachedScissorValid && impl_->cachedScissorEnabled == enabled &&
      (!enabled || (impl_->cachedScissorX == x && impl_->cachedScissorY == y &&
                    impl_->cachedScissorW == cw && impl_->cachedScissorH == ch))) {
    return;
  }
  impl_->cachedScissorValid = true;
  impl_->cachedScissorEnabled = enabled;
  if (!enabled) {
    glDisable(GL_SCISSOR_TEST);
    return;
  }
  impl_->cachedScissorX = x;
  impl_->cachedScissorY = y;
  impl_->cachedScissorW = cw;
  impl_->cachedScissorH = ch;
  glEnable(GL_SCISSOR_TEST);
  glScissor(x, y, cw, ch);
}

void WebGl2RenderDevice::useShader(ShaderHandle sh) {
  const auto* rec = Impl::findSlot(impl_->shaders, sh.id);
  const GLuint prog = rec ? rec->program : 0;
  if (impl_->activeProgram != prog) {
    glUseProgram(prog);
    impl_->activeProgram = prog;
  }
}

void WebGl2RenderDevice::bindTexture(int slot, TextureHandle tex) {
  const int s = std::clamp(slot, 0, 15);
  const auto* rec = Impl::findSlot(impl_->textures, tex.id);
  const GLuint glId = rec ? rec->glId : 0;
  if (impl_->cachedBoundTex[static_cast<size_t>(s)] == glId) {
    return;
  }
  if (impl_->cachedActiveTexUnit != s) {
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(s));
    impl_->cachedActiveTexUnit = s;
  }
  glBindTexture(GL_TEXTURE_2D, glId);
  impl_->cachedBoundTex[static_cast<size_t>(s)] = glId;
}

void WebGl2RenderDevice::setUniformMat4(ShaderHandle sh, std::string_view name, const glm::mat4& mat) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(mat));
  }
}

void WebGl2RenderDevice::setUniformVec2(ShaderHandle sh, std::string_view name, float x, float y) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    glUniform2f(loc, x, y);
  }
}

void WebGl2RenderDevice::setUniformVec3(ShaderHandle sh, std::string_view name, float x, float y, float z) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    glUniform3f(loc, x, y, z);
  }
}

void WebGl2RenderDevice::setUniformVec4(ShaderHandle sh, std::string_view name, float x, float y, float z, float w) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    glUniform4f(loc, x, y, z, w);
  }
}

void WebGl2RenderDevice::setUniformFloat(ShaderHandle sh, std::string_view name, float v) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    glUniform1f(loc, v);
  }
}

void WebGl2RenderDevice::setUniformInt(ShaderHandle sh, std::string_view name, int v) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    glUniform1i(loc, v);
  }
}

void WebGl2RenderDevice::drawIndexed(PrimitiveTopology topology,
                                     std::span<const VertexPosColUv> vertices,
                                     std::span<const uint32_t> indices) {
  if (!impl_->ready || vertices.empty() || indices.empty()) {
    return;
  }
  const size_t vertBytes = vertices.size() * sizeof(VertexPosColUv);
  const size_t idxBytes = indices.size() * sizeof(uint32_t);
  impl_->bindVaoCached(impl_->dynVao);

  glBindBuffer(GL_ARRAY_BUFFER, impl_->dynVbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertBytes), nullptr, GL_STREAM_DRAW);
  glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vertBytes), vertices.data());

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl_->dynIbo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(idxBytes), nullptr, GL_STREAM_DRAW);
  glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(idxBytes), indices.data());

  glDrawElements(toGlTopology(topology), static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, nullptr);
}

void WebGl2RenderDevice::drawVertices(PrimitiveTopology topology,
                                      std::span<const VertexPosColUv> vertices) {
  if (!impl_->ready || vertices.empty()) {
    return;
  }
  const size_t vertBytes = vertices.size() * sizeof(VertexPosColUv);
  impl_->bindVaoCached(impl_->dynVao);

  glBindBuffer(GL_ARRAY_BUFFER, impl_->dynVbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertBytes), nullptr, GL_STREAM_DRAW);
  glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vertBytes), vertices.data());

  glDrawArrays(toGlTopology(topology), 0, static_cast<GLsizei>(vertices.size()));
}

void WebGl2RenderDevice::drawFullscreenQuad() {
  if (!impl_->ready || impl_->fsVao == 0) {
    return;
  }
  impl_->bindVaoCached(impl_->fsVao);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
}

bool WebGl2RenderDevice::readRenderTargetRgba(RenderTargetHandle rt,
                                              int& outWidth,
                                              int& outHeight,
                                              std::vector<uint8_t>& outRgba,
                                              bool flipVertical) {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!rec || rec->width <= 0 || rec->height <= 0) {
    return false;
  }
  outWidth = rec->width;
  outHeight = rec->height;
  const size_t rowBytes = static_cast<size_t>(outWidth) * 4u;
  outRgba.resize(rowBytes * static_cast<size_t>(outHeight));

  glBindFramebuffer(GL_FRAMEBUFFER, rec->fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  if (rec->format == PixelFormat::Rgba8) {
    glReadPixels(0, 0, outWidth, outHeight, GL_RGBA, GL_UNSIGNED_BYTE, outRgba.data());
  } else {
    // WebGL2 requires GL_FLOAT when reading floating-point FBOs.
    std::vector<float> tmp(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 4u);
    glReadPixels(0, 0, outWidth, outHeight, GL_RGBA, GL_FLOAT, tmp.data());
    for (size_t i = 0; i < tmp.size(); ++i) {
      const float v = std::clamp(tmp[i], 0.0f, 1.0f);
      outRgba[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  impl_->cachedFboValid = false;

  if (flipVertical && outHeight > 1) {
    impl_->flipRowScratch.resize(rowBytes);
    for (int y = 0; y < outHeight / 2; ++y) {
      uint8_t* top = outRgba.data() + static_cast<size_t>(y) * rowBytes;
      uint8_t* bot = outRgba.data() + static_cast<size_t>(outHeight - 1 - y) * rowBytes;
      std::memcpy(impl_->flipRowScratch.data(), top, rowBytes);
      std::memcpy(top, bot, rowBytes);
      std::memcpy(bot, impl_->flipRowScratch.data(), rowBytes);
    }
  }
  return true;
}

bool WebGl2RenderDevice::readRenderTargetRgbaFloat(RenderTargetHandle rt,
                                                   int& outWidth,
                                                   int& outHeight,
                                                   std::vector<float>& outRgbaFloat,
                                                   bool flipVertical) {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!rec || rec->width <= 0 || rec->height <= 0) {
    return false;
  }
  outWidth = rec->width;
  outHeight = rec->height;
  const size_t rowFloats = static_cast<size_t>(outWidth) * 4u;
  outRgbaFloat.resize(rowFloats * static_cast<size_t>(outHeight));

  glBindFramebuffer(GL_FRAMEBUFFER, rec->fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  if (rec->format == PixelFormat::Rgba8) {
    std::vector<uint8_t> tmp(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 4u);
    glReadPixels(0, 0, outWidth, outHeight, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    for (size_t i = 0; i < tmp.size(); ++i) {
      outRgbaFloat[i] = static_cast<float>(tmp[i]) * (1.0f / 255.0f);
    }
  } else {
    glReadPixels(0, 0, outWidth, outHeight, GL_RGBA, GL_FLOAT, outRgbaFloat.data());
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  impl_->cachedFboValid = false;

  if (flipVertical && outHeight > 1) {
    std::vector<float> tempRow(rowFloats);
    const size_t rowBytes = rowFloats * sizeof(float);
    for (int y = 0; y < outHeight / 2; ++y) {
      float* top = outRgbaFloat.data() + static_cast<size_t>(y) * rowFloats;
      float* bot = outRgbaFloat.data() + static_cast<size_t>(outHeight - 1 - y) * rowFloats;
      std::memcpy(tempRow.data(), top, rowBytes);
      std::memcpy(top, bot, rowBytes);
      std::memcpy(bot, tempRow.data(), rowBytes);
    }
  }
  return true;
}

}  // namespace iota::rhi
