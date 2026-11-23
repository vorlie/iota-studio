#pragma once

#include "iota/core/types.hpp"
#include "iota/rhi/rhi.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace iota::engine {

struct GlyphQuad {
  float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
  float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

struct TextTokenContext {
  double timeCurrentSec = 0.0;
  double durationSec = 0.0;
  std::string artist;
  std::string title;
  std::string album;
  float fps = 60.0f;
  float frameTimeMs = 16.6f;
  int flushCount = 0;
  int rtSwitchCount = 0;
};

/// Expands all 16 dynamic text tokens (`{TimeCurrent}`, `{ArtistAndTitle}`, `{Fps}`, etc.).
std::string expandTextTokens(std::string_view pattern, const TextTokenContext& ctx);

/// Clean-room TrueType font atlas cache backed by Windows system fonts & stb_truetype
/// (plus an embedded 5x7 fallback bitmap font if OS fonts are missing).
class FontAtlasCache {
 public:
  FontAtlasCache();
  ~FontAtlasCache();

  FontAtlasCache(const FontAtlasCache&) = delete;
  FontAtlasCache& operator=(const FontAtlasCache&) = delete;

  void setDevice(iota::rhi::IRenderDevice* device);
  void shutdown();

  /// Generates textured vertex quads (in Y-down pixels) and returns texture handle to bind.
  iota::rhi::TextureHandle buildTextQuads(std::string_view typeFaceKey,
                                          std::string_view text,
                                          float fontSizePx,
                                          iota::core::RectF bounds,
                                          iota::core::ColorRgba color,
                                          std::vector<iota::rhi::VertexPosColUv>& outVertices,
                                          std::vector<uint32_t>& outIndices);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace iota::engine
