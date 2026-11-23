#pragma once

#include "iota/core/types.hpp"
#include "iota/rhi/rhi.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace iota::engine {

enum class TextAlignment {
  Center = 0,
  Left = 1,
  Right = 2,
};

enum class FontWeight {
  Default = 0,
  Light = 1,
  Regular = 2,
  SemiBold = 3,
  Bold = 4,
  Black = 5,
};

struct TextStyle {
  FontWeight weight = FontWeight::Default;
  bool italic = false;
  TextAlignment alignment = TextAlignment::Center;
  bool wordWrap = false;
  float letterSpacingPx = 0.0f;
  float lineSpacing = 1.0f;
  float outlineWidthPx = 0.0f;
  iota::core::ColorRgba outlineColor{0, 0, 0, 255};
  iota::core::Vec2 shadowOffsetPx{0.0f, 0.0f};
  iota::core::ColorRgba shadowColor{0, 0, 0, 180};
  const std::map<std::string, std::vector<uint8_t>>* localAssets = nullptr;
};

struct FontAtlasStats {
  int cachedGlyphs = 0;
  int atlasResets = 0;
  int atlasWidth = 0;
  int atlasHeight = 0;
  bool usingDirectWrite = false;
};

[[nodiscard]] TextAlignment textAlignmentFromString(std::string_view s) noexcept;
[[nodiscard]] FontWeight fontWeightFromString(std::string_view s) noexcept;

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

/// Dynamic shelf-packed Unicode font atlas cache backed by Windows DirectWrite shaping/fallback
/// (plus an stb_truetype / embedded 5x7 fallback path when OS DirectWrite is unavailable).
class FontAtlasCache {
 public:
  FontAtlasCache();
  ~FontAtlasCache();

  FontAtlasCache(const FontAtlasCache&) = delete;
  FontAtlasCache& operator=(const FontAtlasCache&) = delete;

  void setDevice(iota::rhi::IRenderDevice* device);
  void shutdown();
  void invalidateCustomFonts();
  [[nodiscard]] FontAtlasStats stats() const noexcept;

  /// Generates textured vertex quads (in Y-down pixels) and returns texture handle to bind.
  iota::rhi::TextureHandle buildTextQuads(std::string_view typeFaceKey,
                                          std::string_view text,
                                          float fontSizePx,
                                          iota::core::RectF bounds,
                                          iota::core::ColorRgba color,
                                          const TextStyle& style,
                                          std::vector<iota::rhi::VertexPosColUv>& outVertices,
                                          std::vector<uint32_t>& outIndices);

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
