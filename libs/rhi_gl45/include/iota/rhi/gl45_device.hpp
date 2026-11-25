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

#include "iota/rhi/rhi.hpp"

#include <memory>
#include <string>

namespace iota::rhi {

/// OpenGL 4.5 Core implementation of IRenderDevice.
/// Must be constructed and destroyed while an OpenGL 3.3+/4.5 context is current on the calling thread.
class Gl45RenderDevice final : public IRenderDevice {
 public:
  Gl45RenderDevice();
  ~Gl45RenderDevice() override;

  Gl45RenderDevice(const Gl45RenderDevice&) = delete;
  Gl45RenderDevice& operator=(const Gl45RenderDevice&) = delete;

  bool initialize(std::string* outError = nullptr);
  void shutdown();

  [[nodiscard]] std::string_view backendName() const noexcept override { return "OpenGL 4.5 Core"; }

  void beginFrame() override;
  void endFrame() override;

  TextureHandle createTexture2D(const TextureDesc& desc, std::string* outError = nullptr) override;
  void updateTexture2D(TextureHandle tex, int width, int height, const uint8_t* rgbaPixels) override;
  void updateTexture2DRegion(TextureHandle tex,
                             int x,
                             int y,
                             int width,
                             int height,
                             const uint8_t* rgbaPixels) override;
  void destroyTexture(TextureHandle tex) override;
  bool textureSize(TextureHandle tex, int& outW, int& outH) const override;
  [[nodiscard]] uint64_t nativeTextureId(TextureHandle tex) const noexcept override;

  RenderTargetHandle createRenderTarget(int width,
                                        int height,
                                        TextureFilter filter = TextureFilter::Linear,
                                        PixelFormat format = PixelFormat::Rgba8,
                                        std::string* outError = nullptr) override;
  void resizeRenderTarget(RenderTargetHandle rt, int width, int height) override;
  void destroyRenderTarget(RenderTargetHandle rt) override;
  [[nodiscard]] TextureHandle renderTargetColorTexture(RenderTargetHandle rt) const noexcept override;
  [[nodiscard]] PixelFormat renderTargetFormat(RenderTargetHandle rt) const noexcept override;
  bool renderTargetSize(RenderTargetHandle rt, int& outW, int& outH) const override;

  ShaderHandle createShader(std::string_view vertexGlsl,
                            std::string_view fragmentGlsl,
                            std::string* outError = nullptr) override;
  void destroyShader(ShaderHandle sh) override;

  void bindRenderTarget(RenderTargetHandle rt, int viewportWidth = 0, int viewportHeight = 0) override;
  void clear(float r, float g, float b, float a) override;
  void setBlendMode(iota::core::BlendMode mode) override;
  void setScissor(bool enabled, int x = 0, int y = 0, int w = 0, int h = 0) override;

  void useShader(ShaderHandle sh) override;
  void bindTexture(int slot, TextureHandle tex) override;

  void setUniformMat4(ShaderHandle sh, std::string_view name, const glm::mat4& mat) override;
  void setUniformVec2(ShaderHandle sh, std::string_view name, float x, float y) override;
  void setUniformVec3(ShaderHandle sh, std::string_view name, float x, float y, float z) override;
  void setUniformVec4(ShaderHandle sh, std::string_view name, float x, float y, float z, float w) override;
  void setUniformFloat(ShaderHandle sh, std::string_view name, float v) override;
  void setUniformInt(ShaderHandle sh, std::string_view name, int v) override;

  void drawIndexed(PrimitiveTopology topology,
                   std::span<const VertexPosColUv> vertices,
                   std::span<const uint32_t> indices) override;
  void drawVertices(PrimitiveTopology topology,
                    std::span<const VertexPosColUv> vertices) override;
  void drawFullscreenQuad() override;

  bool readRenderTargetRgba(RenderTargetHandle rt,
                            int& outWidth,
                            int& outHeight,
                            std::vector<uint8_t>& outRgba,
                            bool flipVertical = true) override;
  bool readRenderTargetRgbaAsync(RenderTargetHandle rt,
                                 int& outWidth,
                                 int& outHeight,
                                 std::vector<uint8_t>& outRgba,
                                 bool flipVertical = true) override;
  bool readRenderTargetRgbaFloat(RenderTargetHandle rt,
                                 int& outWidth,
                                 int& outHeight,
                                 std::vector<float>& outRgbaFloat,
                                 bool flipVertical = true) override;
  [[nodiscard]] float lastGpuFrameTimeMs() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace iota::rhi
