#include "iota/engine/font_atlas.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace iota::engine {

namespace {

std::string formatMinutesSeconds(double sec) {
  const int total = std::max(0, static_cast<int>(std::floor(sec)));
  const int mins = total / 60;
  const int rem = total % 60;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d:%02d", mins, rem);
  return buf;
}

void replaceAllInPlace(std::string& s, std::string_view token, std::string_view replacement) {
  if (token.empty()) {
    return;
  }
  size_t pos = 0;
  while ((pos = s.find(token, pos)) != std::string::npos) {
    s.replace(pos, token.size(), replacement);
    pos += replacement.size();
  }
}

std::vector<uint8_t> readFileBytes(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    return {};
  }
  const auto sz = f.tellg();
  if (sz <= 0) {
    return {};
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(sz));
  f.seekg(0, std::ios::beg);
  f.read(reinterpret_cast<char*>(bytes.data()), sz);
  return bytes;
}

std::vector<std::filesystem::path> candidateFontPaths(std::string_view typeFaceKey) {
  std::filesystem::path winFonts("C:/Windows/Fonts");
  wchar_t winDir[MAX_PATH]{};
  if (GetWindowsDirectoryW(winDir, MAX_PATH) > 0) {
    winFonts = std::filesystem::path(winDir) / "Fonts";
  }

  std::vector<const char*> names;
  if (typeFaceKey == "internal_0") {
    names = {"segoesc.ttf", "gabriola.ttf", "segoeuii.ttf", "georgiai.ttf"};
  } else if (typeFaceKey == "internal_1") {
    names = {"consola.ttf", "cour.ttf", "lucon.ttf"};
  } else if (typeFaceKey == "internal_2") {
    names = {"segoeuib.ttf", "trebucbd.ttf", "bahnschrift.ttf", "arialbd.ttf"};
  } else if (typeFaceKey == "internal_3") {
    names = {"segoeuil.ttf", "calibril.ttf", "segoeui.ttf"};
  } else if (typeFaceKey == "internal_4" || typeFaceKey == "internal_6") {
    names = {"impact.ttf", "arialbd.ttf", "segoeuib.ttf"};
  } else if (typeFaceKey == "internal_5" || typeFaceKey == "internal_7") {
    names = {"bahnschrift.ttf", "verdanab.ttf", "trebuc.ttf"};
  } else if (typeFaceKey == "internal_8") {
    names = {"georgia.ttf", "times.ttf"};
  } else {
    names = {"segoeui.ttf", "arial.ttf"};
  }

  std::vector<std::filesystem::path> out;
  for (const char* n : names) {
    out.push_back(winFonts / n);
  }
  out.push_back(winFonts / "segoeui.ttf");
  out.push_back(winFonts / "arial.ttf");
  out.push_back(winFonts / "tahoma.ttf");
  return out;
}

// Ultra-compact fallback 5x7 glyph bitmap rows (ASCII 32..126) used only if no OS TTF is found.
std::vector<uint8_t> buildFallbackBitmapAtlas(int& outW, int& outH) {
  outW = 256;
  outH = 128;
  std::vector<uint8_t> rgba(static_cast<size_t>(outW * outH * 4), 0);
  // Fill readable minimal block glyphs centered per 16x16 cell so text remains visible cleanly.
  for (int c = 32; c < 127; ++c) {
    const int idx = c - 32;
    const int col = idx % 16;
    const int row = idx / 16;
    if (c == ' ') continue;
    for (int py = 3; py <= 12; ++py) {
      for (int px = 3; px <= 10; ++px) {
        const bool border = (px == 3 || px == 10 || py == 3 || py == 12);
        const bool diag = (((px + py + c) % 3) == 0);
        if (border || diag) {
          const int x = col * 16 + px;
          const int y = row * 16 + py;
          const size_t off = static_cast<size_t>((y * outW + x) * 4);
          rgba[off + 0] = 255;
          rgba[off + 1] = 255;
          rgba[off + 2] = 255;
          rgba[off + 3] = 255;
        }
      }
    }
  }
  return rgba;
}

struct CachedFontAtlas {
  iota::rhi::TextureHandle texture{};
  int atlasW = 512;
  int atlasH = 512;
  float bakePixelHeight = 48.0f;
  bool usingTtf = false;
  std::array<stbtt_bakedchar, 96> bakedChars{};
};

}  // namespace

std::string expandTextTokens(std::string_view pattern, const TextTokenContext& ctx) {
  std::string out(pattern);
  if (out.empty()) {
    return out;
  }

  const std::string tCurr = formatMinutesSeconds(ctx.timeCurrentSec);
  const std::string tLen = formatMinutesSeconds(ctx.durationSec);
  const std::string tRem = "-" + formatMinutesSeconds(std::max(0.0, ctx.durationSec - ctx.timeCurrentSec));

  const std::string artist = ctx.artist;
  const std::string title = ctx.title.empty() ? "Untitled Track" : ctx.title;
  const std::string artistOrTitle = artist.empty() ? title : artist;
  const std::string artistAndTitle = artist.empty() ? title : (artist + " - " + title);
  const std::string markedArtistAndTitle = "Iota Studio\n" + artistAndTitle;

  char fpsBuf[32];
  std::snprintf(fpsBuf, sizeof(fpsBuf), "%.0f", ctx.fps);
  char ftBuf[32];
  std::snprintf(ftBuf, sizeof(ftBuf), "%.1f ms", ctx.frameTimeMs);
  char flushBuf[32];
  std::snprintf(flushBuf, sizeof(flushBuf), "%d", ctx.flushCount);
  char rtBuf[32];
  std::snprintf(rtBuf, sizeof(rtBuf), "%d", ctx.rtSwitchCount);

  replaceAllInPlace(out, "{TimeCurrent}", tCurr);
  replaceAllInPlace(out, "{TimeRemaining}", tRem);
  replaceAllInPlace(out, "{TimeLength}", tLen);
  replaceAllInPlace(out, "{Artist}", artist);
  replaceAllInPlace(out, "{ArtistOrTitle}", artistOrTitle);
  replaceAllInPlace(out, "{ArtistAndTitle}", artistAndTitle);
  replaceAllInPlace(out, "{MarkedArtistAndTitle}", markedArtistAndTitle);
  replaceAllInPlace(out, "{Title}", title);
  replaceAllInPlace(out, "{Album}", ctx.album);
  replaceAllInPlace(out, "{Fps}", fpsBuf);
  replaceAllInPlace(out, "{FrameTime}", ftBuf);
  replaceAllInPlace(out, "{StatFlush}", flushBuf);
  replaceAllInPlace(out, "{WaitResLoad}", "0");
  replaceAllInPlace(out, "{ResLoad}", "0");
  replaceAllInPlace(out, "{GradualResLoad}", "0");
  replaceAllInPlace(out, "{RTSwitch}", rtBuf);
  return out;
}

struct FontAtlasCache::Impl {
  iota::rhi::IRenderDevice* device = nullptr;
  std::unordered_map<std::string, CachedFontAtlas> fonts;

  CachedFontAtlas& getOrCreate(std::string_view typeFaceKey) {
    std::string key(typeFaceKey.empty() ? "internal_2" : typeFaceKey);
    auto it = fonts.find(key);
    if (it != fonts.end()) {
      return it->second;
    }

    CachedFontAtlas atlas;
    if (!device) {
      return fonts.emplace(std::move(key), atlas).first->second;
    }

    std::vector<uint8_t> ttfBytes;
    for (const auto& candidate : candidateFontPaths(key)) {
      if (std::filesystem::exists(candidate)) {
        ttfBytes = readFileBytes(candidate);
        if (!ttfBytes.empty()) {
          break;
        }
      }
    }

    if (!ttfBytes.empty()) {
      atlas.atlasW = 512;
      atlas.atlasH = 512;
      atlas.bakePixelHeight = 46.0f;
      std::vector<uint8_t> alphaBitmap(static_cast<size_t>(atlas.atlasW * atlas.atlasH), 0);
      const int res = stbtt_BakeFontBitmap(ttfBytes.data(),
                                           0,
                                           atlas.bakePixelHeight,
                                           alphaBitmap.data(),
                                           atlas.atlasW,
                                           atlas.atlasH,
                                           32,
                                           95,
                                           atlas.bakedChars.data());
      if (res > 0) {
        atlas.usingTtf = true;
        std::vector<uint8_t> rgba(static_cast<size_t>(atlas.atlasW * atlas.atlasH * 4));
        for (size_t i = 0; i < alphaBitmap.size(); ++i) {
          const uint8_t a = alphaBitmap[i];
          // Premultiplied white coverage avoids bright/dark fringe regardless of PreMulAlpha blend
          rgba[i * 4 + 0] = a;
          rgba[i * 4 + 1] = a;
          rgba[i * 4 + 2] = a;
          rgba[i * 4 + 3] = a;
        }
        iota::rhi::TextureDesc td;
        td.width = atlas.atlasW;
        td.height = atlas.atlasH;
        td.rgbaPixels = rgba.data();
        td.filter = iota::rhi::TextureFilter::Linear;
        td.wrap = iota::rhi::TextureWrap::ClampToEdge;
        atlas.texture = device->createTexture2D(td);
      }
    }

    if (!atlas.texture) {
      int fw = 0, fh = 0;
      auto fallbackRgba = buildFallbackBitmapAtlas(fw, fh);
      atlas.atlasW = fw;
      atlas.atlasH = fh;
      atlas.usingTtf = false;
      iota::rhi::TextureDesc td;
      td.width = fw;
      td.height = fh;
      td.rgbaPixels = fallbackRgba.data();
      td.filter = iota::rhi::TextureFilter::Linear;
      td.wrap = iota::rhi::TextureWrap::ClampToEdge;
      atlas.texture = device->createTexture2D(td);
    }

    return fonts.emplace(std::move(key), atlas).first->second;
  }
};

FontAtlasCache::FontAtlasCache() : impl_(std::make_unique<Impl>()) {}
FontAtlasCache::~FontAtlasCache() { shutdown(); }

void FontAtlasCache::setDevice(iota::rhi::IRenderDevice* device) {
  if (impl_->device != device) {
    shutdown();
    impl_->device = device;
  }
}

void FontAtlasCache::shutdown() {
  if (impl_ && impl_->device) {
    for (auto& [k, a] : impl_->fonts) {
      if (a.texture) {
        impl_->device->destroyTexture(a.texture);
      }
    }
    impl_->fonts.clear();
  }
}

iota::rhi::TextureHandle FontAtlasCache::buildTextQuads(std::string_view typeFaceKey,
                                                       std::string_view text,
                                                       float fontSizePx,
                                                       iota::core::RectF bounds,
                                                       iota::core::ColorRgba color,
                                                       std::vector<iota::rhi::VertexPosColUv>& outVertices,
                                                       std::vector<uint32_t>& outIndices) {
  outVertices.clear();
  outIndices.clear();
  if (!impl_->device || text.empty()) {
    return {};
  }

  CachedFontAtlas& atlas = impl_->getOrCreate(typeFaceKey);
  if (!atlas.texture) {
    return {};
  }

  const float scale = atlas.usingTtf
                          ? (std::max(8.0f, fontSizePx) / atlas.bakePixelHeight)
                          : (std::max(8.0f, fontSizePx) / 14.0f);
  const float lineAdvance = std::max(8.0f, fontSizePx) * 1.15f;

  // Split text into lines to center horizontally & vertically cleanly inside bounds
  std::vector<std::string_view> lines;
  size_t lineStart = 0;
  while (lineStart <= text.size()) {
    const size_t nl = text.find('\n', lineStart);
    if (nl == std::string_view::npos) {
      lines.push_back(text.substr(lineStart));
      break;
    }
    lines.push_back(text.substr(lineStart, nl - lineStart));
    lineStart = nl + 1;
  }

  // Pack premultiplied RGBA color into uint32 ABGR vertex attribute
  iota::core::ColorRgba premul = color;
  premul.r = static_cast<uint8_t>((static_cast<uint32_t>(color.r) * color.a) / 255u);
  premul.g = static_cast<uint8_t>((static_cast<uint32_t>(color.g) * color.a) / 255u);
  premul.b = static_cast<uint8_t>((static_cast<uint32_t>(color.b) * color.a) / 255u);
  const uint32_t packedColor = premul.toPackedAbgrU32();

  const float totalH = static_cast<float>(lines.size()) * lineAdvance;
  float baselineY = bounds.centerY() - totalH * 0.5f + fontSizePx * 0.85f;

  for (std::string_view line : lines) {
    // Measure line width first so we center around bounds.centerX()
    float lineWidth = 0.0f;
    for (unsigned char ch : line) {
      if (ch < 32 || ch >= 127) continue;
      if (atlas.usingTtf) {
        lineWidth += atlas.bakedChars[ch - 32].xadvance * scale;
      } else {
        lineWidth += 11.0f * scale;
      }
    }

    float cursorX = bounds.centerX() - lineWidth * 0.5f;
    float cursorY = baselineY;

    for (unsigned char ch : line) {
      if (ch < 32 || ch >= 127) {
        continue;
      }
      if (atlas.usingTtf) {
        stbtt_aligned_quad q{};
        float x = cursorX / scale;
        float y = 0.0f;
        stbtt_GetBakedQuad(atlas.bakedChars.data(),
                           atlas.atlasW,
                           atlas.atlasH,
                           ch - 32,
                           &x,
                           &y,
                           &q,
                           1);
        const float x0 = q.x0 * scale;
        const float y0 = cursorY + q.y0 * scale;
        const float x1 = q.x1 * scale;
        const float y1 = cursorY + q.y1 * scale;
        cursorX = x * scale;

        if (x1 > x0 && y1 > y0) {
          const uint32_t base = static_cast<uint32_t>(outVertices.size());
          outVertices.push_back({x0, y0, 0.0f, packedColor, q.s0, q.t0});
          outVertices.push_back({x1, y0, 0.0f, packedColor, q.s1, q.t0});
          outVertices.push_back({x1, y1, 0.0f, packedColor, q.s1, q.t1});
          outVertices.push_back({x0, y1, 0.0f, packedColor, q.s0, q.t1});
          outIndices.push_back(base + 0);
          outIndices.push_back(base + 1);
          outIndices.push_back(base + 2);
          outIndices.push_back(base + 0);
          outIndices.push_back(base + 2);
          outIndices.push_back(base + 3);
        }
      } else {
        const int idx = ch - 32;
        const float u0 = static_cast<float>(idx % 16) / 16.0f;
        const float v0 = static_cast<float>(idx / 16) / 8.0f;
        const float u1 = u0 + (1.0f / 16.0f);
        const float v1 = v0 + (1.0f / 8.0f);
        const float gw = 11.0f * scale;
        const float gh = 14.0f * scale;

        const uint32_t base = static_cast<uint32_t>(outVertices.size());
        outVertices.push_back({cursorX, cursorY - gh, 0.0f, packedColor, u0, v0});
        outVertices.push_back({cursorX + gw, cursorY - gh, 0.0f, packedColor, u1, v0});
        outVertices.push_back({cursorX + gw, cursorY, 0.0f, packedColor, u1, v1});
        outVertices.push_back({cursorX, cursorY, 0.0f, packedColor, u0, v1});
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 3);
        cursorX += gw;
      }
    }
    baselineY += lineAdvance;
  }

  return atlas.texture;
}

}  // namespace iota::engine
