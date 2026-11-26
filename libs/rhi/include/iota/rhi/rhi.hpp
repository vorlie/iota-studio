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

#pragma once

#include "iota/core/types.hpp"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iota::rhi {

struct TextureHandle {
  uint32_t id = 0;
  explicit operator bool() const noexcept { return id != 0; }
  friend bool operator==(TextureHandle, TextureHandle) = default;
};

struct RenderTargetHandle {
  uint32_t id = 0;
  explicit operator bool() const noexcept { return id != 0; }
  friend bool operator==(RenderTargetHandle, RenderTargetHandle) = default;
};

struct ShaderHandle {
  uint32_t id = 0;
  explicit operator bool() const noexcept { return id != 0; }
  friend bool operator==(ShaderHandle, ShaderHandle) = default;
};

struct VertexPosColUv {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  uint32_t abgr = 0xFFFFFFFFu;
  float u = 0.0f;
  float v = 0.0f;
};

enum class TextureFilter {
  Linear = 0,
  Nearest = 1,
};

enum class PixelFormat {
  Rgba8 = 0,
  Rgba16F = 1,
  R11G11B10F = 2,
};

enum class TextureWrap {
  ClampToEdge = 0,
  Repeat = 1,
  MirroredRepeat = 2,
};

enum class PrimitiveTopology {
  Triangles = 0,
  TriangleStrip = 1,
  Lines = 2,
};

struct TextureDesc {
  int width = 0;
  int height = 0;
  const uint8_t* rgbaPixels = nullptr;
  TextureFilter filter = TextureFilter::Linear;
  TextureWrap wrap = TextureWrap::ClampToEdge;
  bool generateMipmaps = false;
  PixelFormat format = PixelFormat::Rgba8;
};

/// Abstract Rendering Hardware Interface (OpenGL 4.5 implementation now, Vulkan-ready).
class IRenderDevice {
 public:
  virtual ~IRenderDevice() = default;

  [[nodiscard]] virtual std::string_view backendName() const noexcept = 0;

  virtual void beginFrame() {}
  virtual void endFrame() {}

  virtual TextureHandle createTexture2D(const TextureDesc& desc, std::string* outError = nullptr) = 0;
  virtual void updateTexture2D(TextureHandle tex, int width, int height, const uint8_t* rgbaPixels) = 0;
  virtual void updateTexture2DRegion(TextureHandle tex,
                                     int x,
                                     int y,
                                     int width,
                                     int height,
                                     const uint8_t* rgbaPixels) = 0;
  virtual void destroyTexture(TextureHandle tex) = 0;
  virtual bool textureSize(TextureHandle tex, int& outW, int& outH) const = 0;
  [[nodiscard]] virtual uint64_t nativeTextureId(TextureHandle tex) const noexcept = 0;

  virtual RenderTargetHandle createRenderTarget(int width,
                                                int height,
                                                TextureFilter filter = TextureFilter::Linear,
                                                PixelFormat format = PixelFormat::Rgba8,
                                                std::string* outError = nullptr) = 0;
  virtual void resizeRenderTarget(RenderTargetHandle rt, int width, int height) = 0;
  virtual void destroyRenderTarget(RenderTargetHandle rt) = 0;
  [[nodiscard]] virtual TextureHandle renderTargetColorTexture(RenderTargetHandle rt) const noexcept = 0;
  [[nodiscard]] virtual PixelFormat renderTargetFormat(RenderTargetHandle rt) const noexcept = 0;
  virtual bool renderTargetSize(RenderTargetHandle rt, int& outW, int& outH) const = 0;

  virtual ShaderHandle createShader(std::string_view vertexGlsl,
                                    std::string_view fragmentGlsl,
                                    std::string* outError = nullptr) = 0;
  virtual void destroyShader(ShaderHandle sh) = 0;

  virtual void bindRenderTarget(RenderTargetHandle rt, int viewportWidth = 0, int viewportHeight = 0) = 0;
  virtual void clear(float r, float g, float b, float a) = 0;
  virtual void setBlendMode(iota::core::BlendMode mode) = 0;
  virtual void setScissor(bool enabled, int x = 0, int y = 0, int w = 0, int h = 0) = 0;

  virtual void useShader(ShaderHandle sh) = 0;
  virtual void bindTexture(int slot, TextureHandle tex) = 0;

  virtual void setUniformMat4(ShaderHandle sh, std::string_view name, const glm::mat4& mat) = 0;
  virtual void setUniformVec2(ShaderHandle sh, std::string_view name, float x, float y) = 0;
  virtual void setUniformVec3(ShaderHandle sh, std::string_view name, float x, float y, float z) = 0;
  virtual void setUniformVec4(ShaderHandle sh, std::string_view name, float x, float y, float z, float w) = 0;
  virtual void setUniformFloat(ShaderHandle sh, std::string_view name, float v) = 0;
  virtual void setUniformInt(ShaderHandle sh, std::string_view name, int v) = 0;

  virtual void drawIndexed(PrimitiveTopology topology,
                           std::span<const VertexPosColUv> vertices,
                           std::span<const uint32_t> indices) = 0;
  virtual void drawVertices(PrimitiveTopology topology,
                            std::span<const VertexPosColUv> vertices) = 0;
  virtual void drawFullscreenQuad() {
    static constexpr VertexPosColUv kClipQuad[4] = {
        {-1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
        {+1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {+1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
        {-1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
    };
    static constexpr uint32_t kClipIdx[6] = {0, 1, 2, 0, 2, 3};
    drawIndexed(PrimitiveTopology::Triangles, kClipQuad, kClipIdx);
  }

  virtual bool readRenderTargetRgba(RenderTargetHandle rt,
                                    int& outWidth,
                                    int& outHeight,
                                    std::vector<uint8_t>& outRgba,
                                    bool flipVertical = true) = 0;
  virtual bool readRenderTargetRgbaAsync(RenderTargetHandle rt,
                                         int& outWidth,
                                         int& outHeight,
                                         std::vector<uint8_t>& outRgba,
                                         bool flipVertical = true) {
    return readRenderTargetRgba(rt, outWidth, outHeight, outRgba, flipVertical);
  }
  virtual bool readRenderTargetRgbaFloat(RenderTargetHandle rt,
                                         int& outWidth,
                                         int& outHeight,
                                         std::vector<float>& outRgbaFloat,
                                         bool flipVertical = true) = 0;
  [[nodiscard]] virtual float lastGpuFrameTimeMs() const noexcept { return 0.0f; }
};

}  // namespace iota::rhi
