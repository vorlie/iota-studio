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

#include "iota/engine/font_atlas.hpp"

#include "iota/core/utf8_path.hpp"

#ifdef _WIN32
#include <windows.h>
#include <dwrite_3.h>
#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include <hb-ft.h>
#include <hb.h>
#endif

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iota::engine {

namespace {

bool asciiEqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

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

uint32_t packPremultipliedAbgr(iota::core::ColorRgba color) noexcept {
  iota::core::ColorRgba premul = color;
  premul.r = static_cast<uint8_t>((static_cast<uint32_t>(color.r) * color.a) / 255u);
  premul.g = static_cast<uint8_t>((static_cast<uint32_t>(color.g) * color.a) / 255u);
  premul.b = static_cast<uint8_t>((static_cast<uint32_t>(color.b) * color.a) / 255u);
  return premul.toPackedAbgrU32();
}

struct GlyphKey {
  uintptr_t fontFaceKey = 0;
  uint32_t glyphIndex = 0;
  uint16_t sizeBucketPx = 48;
  uint8_t outlinePx = 0;
  bool operator==(const GlyphKey&) const noexcept = default;
};

struct GlyphKeyHash {
  size_t operator()(const GlyphKey& k) const noexcept {
    size_t h = std::hash<uintptr_t>{}(k.fontFaceKey);
    h ^= std::hash<uint32_t>{}(k.glyphIndex) + 0x9e3779b9u + (h << 6) + (h >> 2);
    const uint32_t packed = (static_cast<uint32_t>(k.sizeBucketPx) << 8) | k.outlinePx;
    h ^= std::hash<uint32_t>{}(packed) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
  }
};

struct CachedGlyph {
  bool visible = false;
  float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
  int offsetX = 0;
  int offsetY = 0;
  int width = 0;
  int height = 0;
};

struct AtlasShelf {
  int y = 0;
  int height = 0;
  int cursorX = 0;
};

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
  if (!f.read(reinterpret_cast<char*>(bytes.data()), sz)) {
    return {};
  }
  return bytes;
}

uint16_t quantizeFontBucketPx(float fontSizePx) noexcept {
  constexpr std::array<uint16_t, 7> kBuckets = {16, 24, 32, 48, 64, 96, 128};
  const float clamped = std::clamp(fontSizePx, 12.0f, 128.0f);
  for (uint16_t b : kBuckets) {
    if (static_cast<float>(b) >= clamped - 0.5f) {
      return b;
    }
  }
  return kBuckets.back();
}

void dilateAlphaCircle(const std::vector<uint8_t>& alphaIn,
                       int gw,
                       int gh,
                       int rad,
                       std::vector<uint8_t>& outDilated) {
  const int outW = gw + 2 * rad;
  const int outH = gh + 2 * rad;
  outDilated.assign(static_cast<size_t>(outW) * static_cast<size_t>(outH), 0);

  struct KernelTap {
    int dx;
    int dy;
    float weight;
  };
  std::vector<KernelTap> taps;
  taps.reserve(static_cast<size_t>((2 * rad + 1) * (2 * rad + 1)));
  const float maxDist = static_cast<float>(rad) + 0.5f;
  for (int dy = -rad; dy <= rad; ++dy) {
    for (int dx = -rad; dx <= rad; ++dx) {
      const float d = std::hypot(static_cast<float>(dx), static_cast<float>(dy));
      const float w = std::clamp(maxDist - d, 0.0f, 1.0f);
      if (w > 0.001f) {
        taps.push_back({dx, dy, w});
      }
    }
  }

  for (int oy = 0; oy < outH; ++oy) {
    const int iy = oy - rad;
    for (int ox = 0; ox < outW; ++ox) {
      const int ix = ox - rad;
      float best = 0.0f;
      for (const auto& t : taps) {
        const int sx = ix + t.dx;
        const int sy = iy + t.dy;
        if (sx >= 0 && sx < gw && sy >= 0 && sy < gh) {
          const float cov = static_cast<float>(alphaIn[static_cast<size_t>(sy) * gw + sx]) * t.weight;
          if (cov > best) {
            best = cov;
          }
        }
      }
      outDilated[static_cast<size_t>(oy) * outW + ox] =
          static_cast<uint8_t>(std::clamp(std::lround(best), 0l, 255l));
    }
  }
}

#if defined(_WIN32) || defined(IOTA_HAS_FREETYPE_HARFBUZZ)

struct ShapedLayoutKeyView {
  std::string_view typeFaceKey;
  std::string_view text;
  uintptr_t localAssetsPtr = 0;
  uint64_t customFontGen = 0;
  uint16_t bucketPx = 0;
  FontWeight weight = FontWeight::Default;
  bool italic = false;
  TextAlignment alignment = TextAlignment::Center;
  bool wordWrap = false;
  float layoutW = 0.0f;
  float letterSpacingDip = 0.0f;
  float lineSpacingNorm = 1.0f;
};

struct ShapedLayoutKey {
  std::string typeFaceKey;
  std::string text;
  uintptr_t localAssetsPtr = 0;
  uint64_t customFontGen = 0;
  uint16_t bucketPx = 0;
  FontWeight weight = FontWeight::Default;
  bool italic = false;
  TextAlignment alignment = TextAlignment::Center;
  bool wordWrap = false;
  float layoutW = 0.0f;
  float letterSpacingDip = 0.0f;
  float lineSpacingNorm = 1.0f;

  ShapedLayoutKeyView view() const noexcept {
    return {
        typeFaceKey,
        text,
        localAssetsPtr,
        customFontGen,
        bucketPx,
        weight,
        italic,
        alignment,
        wordWrap,
        layoutW,
        letterSpacingDip,
        lineSpacingNorm,
    };
  }
};

struct ShapedLayoutKeyHash {
  using is_transparent = void;

  static void hashCombine(size_t& h, size_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  }

  size_t operator()(const ShapedLayoutKeyView& k) const noexcept {
    size_t h = std::hash<std::string_view>{}(k.typeFaceKey);
    hashCombine(h, std::hash<std::string_view>{}(k.text));
    hashCombine(h, std::hash<uintptr_t>{}(k.localAssetsPtr));
    hashCombine(h, std::hash<uint64_t>{}(k.customFontGen));
    hashCombine(h, std::hash<uint16_t>{}(k.bucketPx));
    hashCombine(h, static_cast<size_t>(k.weight));
    hashCombine(h, k.italic ? 1u : 0u);
    hashCombine(h, static_cast<size_t>(k.alignment));
    hashCombine(h, k.wordWrap ? 1u : 0u);
    hashCombine(h, std::hash<float>{}(k.layoutW));
    hashCombine(h, std::hash<float>{}(k.letterSpacingDip));
    hashCombine(h, std::hash<float>{}(k.lineSpacingNorm));
    return h;
  }

  size_t operator()(const ShapedLayoutKey& k) const noexcept {
    return (*this)(k.view());
  }
};

struct ShapedLayoutKeyEq {
  using is_transparent = void;

  static bool eq(const ShapedLayoutKeyView& a, const ShapedLayoutKeyView& b) noexcept {
    return a.bucketPx == b.bucketPx &&
           a.weight == b.weight &&
           a.italic == b.italic &&
           a.alignment == b.alignment &&
           a.wordWrap == b.wordWrap &&
           a.localAssetsPtr == b.localAssetsPtr &&
           a.customFontGen == b.customFontGen &&
           a.layoutW == b.layoutW &&
           a.letterSpacingDip == b.letterSpacingDip &&
           a.lineSpacingNorm == b.lineSpacingNorm &&
           a.typeFaceKey == b.typeFaceKey &&
           a.text == b.text;
  }

  bool operator()(const ShapedLayoutKey& a, const ShapedLayoutKey& b) const noexcept {
    return eq(a.view(), b.view());
  }
  bool operator()(const ShapedLayoutKeyView& a, const ShapedLayoutKey& b) const noexcept {
    return eq(a, b.view());
  }
  bool operator()(const ShapedLayoutKey& a, const ShapedLayoutKeyView& b) const noexcept {
    return eq(a.view(), b);
  }
};

#endif

#ifdef _WIN32

template <typename T>
class ComPtr {
 public:
  ComPtr() noexcept = default;
  ~ComPtr() { reset(); }

  ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) {
    if (ptr_) ptr_->AddRef();
  }
  ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

  ComPtr& operator=(const ComPtr& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.ptr_;
      if (ptr_) ptr_->AddRef();
    }
    return *this;
  }
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }

  void attach(T* p) noexcept {
    reset();
    ptr_ = p;
  }

  void reset() noexcept {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

  T* get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  T** put() noexcept {
    reset();
    return &ptr_;
  }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

 private:
  T* ptr_ = nullptr;
};

std::wstring utf8ToWide(std::string_view utf8) {
  const std::string clean = iota::core::sanitizeUtf8(utf8);
  if (clean.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(
      CP_UTF8, 0, clean.data(), static_cast<int>(clean.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, clean.data(), static_cast<int>(clean.size()), out.data(), needed);
  return out;
}

DWRITE_FONT_WEIGHT toDWriteWeight(FontWeight w, DWRITE_FONT_WEIGHT fallbackDefault) noexcept {
  switch (w) {
    case FontWeight::Light: return DWRITE_FONT_WEIGHT_LIGHT;
    case FontWeight::Regular: return DWRITE_FONT_WEIGHT_REGULAR;
    case FontWeight::SemiBold: return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    case FontWeight::Bold: return DWRITE_FONT_WEIGHT_BOLD;
    case FontWeight::Black: return DWRITE_FONT_WEIGHT_BLACK;
    case FontWeight::Default:
    default: return fallbackDefault;
  }
}

struct FamilyPresetSpec {
  std::wstring familyName;
  DWRITE_FONT_WEIGHT defaultWeight = DWRITE_FONT_WEIGHT_REGULAR;
};

FamilyPresetSpec resolveSystemFamilyPreset(std::string_view key) {
  if (key.empty() || key == "internal_2") return {L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD};
  if (key == "internal_0") return {L"Segoe Script", DWRITE_FONT_WEIGHT_NORMAL};
  if (key == "internal_1") return {L"Consolas", DWRITE_FONT_WEIGHT_NORMAL};
  if (key == "internal_3") return {L"Segoe UI", DWRITE_FONT_WEIGHT_LIGHT};
  if (key == "internal_4" || key == "internal_6") return {L"Impact", DWRITE_FONT_WEIGHT_NORMAL};
  if (key == "internal_5" || key == "internal_7") return {L"Bahnschrift", DWRITE_FONT_WEIGHT_SEMI_BOLD};
  if (key == "internal_8") return {L"Georgia", DWRITE_FONT_WEIGHT_NORMAL};
  return {utf8ToWide(key), DWRITE_FONT_WEIGHT_REGULAR};
}

struct ShapedGlyph {
  IDWriteFontFace* face = nullptr;
  uint16_t glyphIndex = 0;
  float x = 0.0f;
  float y = 0.0f;
};

struct ShapedLayoutEntry {
  std::vector<ShapedGlyph> glyphs;
  std::vector<ComPtr<IDWriteFontFace>> heldFaces;
  float layoutW = 0.0f;
  float metricsTop = 0.0f;
  float metricsHeight = 0.0f;
  uint64_t lastUseStamp = 0;
};

class ShapingCollector final : public IDWriteTextRenderer {
 public:
  explicit ShapingCollector(std::vector<ShapedGlyph>& outGlyphs,
                            std::unordered_map<uintptr_t, ComPtr<IDWriteFontFace>>& pinnedFaces) noexcept
      : out_(outGlyphs), pinned_(pinnedFaces) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IDWritePixelSnapping) ||
        riid == __uuidof(IDWriteTextRenderer)) {
      *ppv = static_cast<IDWriteTextRenderer*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* isDisabled) override {
    if (!isDisabled) return E_INVALIDARG;
    *isDisabled = TRUE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
    if (!transform) return E_INVALIDARG;
    transform->m11 = 1.0f;
    transform->m12 = 0.0f;
    transform->m21 = 0.0f;
    transform->m22 = 1.0f;
    transform->dx = 0.0f;
    transform->dy = 0.0f;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override {
    if (!pixelsPerDip) return E_INVALIDARG;
    *pixelsPerDip = 1.0f;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,
                                         FLOAT baselineOriginX,
                                         FLOAT baselineOriginY,
                                         DWRITE_MEASURING_MODE,
                                         DWRITE_GLYPH_RUN const* glyphRun,
                                         DWRITE_GLYPH_RUN_DESCRIPTION const*,
                                         IUnknown*) override {
    if (!glyphRun || !glyphRun->fontFace || glyphRun->glyphCount == 0 || !glyphRun->glyphIndices) {
      return S_OK;
    }

    const uintptr_t faceKey = reinterpret_cast<uintptr_t>(glyphRun->fontFace);
    if (pinned_.find(faceKey) == pinned_.end()) {
      glyphRun->fontFace->AddRef();
      ComPtr<IDWriteFontFace> holder;
      holder.attach(glyphRun->fontFace);
      pinned_.emplace(faceKey, std::move(holder));
    }

    const bool rtl = (glyphRun->bidiLevel & 1u) != 0u;
    float penX = baselineOriginX;
    for (UINT32 i = 0; i < glyphRun->glyphCount; ++i) {
      const float adv = glyphRun->glyphAdvances ? glyphRun->glyphAdvances[i] : 0.0f;
      const float advOff = glyphRun->glyphOffsets ? glyphRun->glyphOffsets[i].advanceOffset : 0.0f;
      const float ascOff = glyphRun->glyphOffsets ? glyphRun->glyphOffsets[i].ascenderOffset : 0.0f;

      float gx = penX;
      if (rtl) {
        penX -= adv;
        gx = penX - advOff;
      } else {
        gx = penX + advOff;
        penX += adv;
      }
      const float gy = baselineOriginY - ascOff;
      out_.push_back({glyphRun->fontFace, glyphRun->glyphIndices[i], gx, gy});
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, DWRITE_UNDERLINE const*, IUnknown*) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const*, IUnknown*) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override {
    return S_OK;
  }

 private:
  std::vector<ShapedGlyph>& out_;
  std::unordered_map<uintptr_t, ComPtr<IDWriteFontFace>>& pinned_;
};

struct CustomFontCollectionEntry {
  ComPtr<IDWriteFontCollection1> collection;
  std::wstring familyName;
};

#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)

struct FtFaceEntry {
  FT_Face ftFace = nullptr;
  hb_font_t* hbFont = nullptr;
  std::vector<uint8_t> memoryBytes;
  bool synthBold = false;
  bool synthItalic = false;
  uint16_t currentPixelSize = 0;

  ~FtFaceEntry() {
    if (hbFont != nullptr) {
      hb_font_destroy(hbFont);
      hbFont = nullptr;
    }
    if (ftFace != nullptr) {
      FT_Done_Face(ftFace);
      ftFace = nullptr;
    }
  }

  void ensurePixelSize(uint16_t px) noexcept {
    if (!ftFace || currentPixelSize == px) {
      return;
    }
    if (FT_Set_Pixel_Sizes(ftFace, 0, px) == 0) {
      currentPixelSize = px;
      if (hbFont != nullptr) {
        hb_ft_font_changed(hbFont);
      }
    }
  }
};

struct FtShapedGlyph {
  FtFaceEntry* face = nullptr;
  uint32_t glyphIndex = 0;
  float x = 0.0f;
  float y = 0.0f;
};

struct FtShapedLayoutEntry {
  std::vector<FtShapedGlyph> glyphs;
  std::vector<std::shared_ptr<FtFaceEntry>> heldFaces;
  float layoutW = 0.0f;
  float metricsTop = 0.0f;
  float metricsHeight = 0.0f;
  uint64_t lastUseStamp = 0;
};

struct LinuxFamilySpec {
  std::vector<std::string> families;
  FontWeight defaultWeight = FontWeight::Regular;
  bool defaultItalic = false;
};

LinuxFamilySpec resolveLinuxFamilySpec(std::string_view key) {
  if (key.empty() || key == "internal_2") {
    return {{"Segoe UI", "Noto Sans", "Liberation Sans", "DejaVu Sans"}, FontWeight::Bold, false};
  }
  if (key == "internal_0") {
    return {{"Segoe Script", "URW Chancery L", "Noto Sans", "DejaVu Serif"}, FontWeight::Regular, true};
  }
  if (key == "internal_1") {
    return {{"Consolas", "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono"}, FontWeight::Regular, false};
  }
  if (key == "internal_3") {
    return {{"Segoe UI", "Noto Sans", "Liberation Sans", "DejaVu Sans"}, FontWeight::Light, false};
  }
  if (key == "internal_4" || key == "internal_6") {
    return {{"Impact", "Anton", "Noto Sans", "Liberation Sans"}, FontWeight::Black, false};
  }
  if (key == "internal_5" || key == "internal_7") {
    return {{"Bahnschrift", "Noto Sans", "Liberation Sans", "DejaVu Sans"}, FontWeight::SemiBold, false};
  }
  if (key == "internal_8") {
    return {{"Georgia", "Liberation Serif", "DejaVu Serif", "Noto Serif"}, FontWeight::Regular, false};
  }
  if (asciiEqualsIgnoreCase(key, "Segoe_UI") || asciiEqualsIgnoreCase(key, "Segoe UI")) {
    return {{"Segoe UI", "Noto Sans", "Liberation Sans", "DejaVu Sans"}, FontWeight::Regular, false};
  }
  if (asciiEqualsIgnoreCase(key, "Segoe Script")) {
    return {{"Segoe Script", "URW Chancery L", "Noto Sans", "DejaVu Serif"}, FontWeight::Regular, true};
  }
  if (asciiEqualsIgnoreCase(key, "Bahnschrift")) {
    return {{"Bahnschrift", "Noto Sans", "Liberation Sans", "DejaVu Sans"}, FontWeight::SemiBold, false};
  }
  if (asciiEqualsIgnoreCase(key, "Consolas")) {
    return {{"Consolas", "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono"}, FontWeight::Regular, false};
  }
  if (asciiEqualsIgnoreCase(key, "Georgia")) {
    return {{"Georgia", "Liberation Serif", "DejaVu Serif", "Noto Serif"}, FontWeight::Regular, false};
  }
  if (asciiEqualsIgnoreCase(key, "Impact")) {
    return {{"Impact", "Anton", "Noto Sans", "Liberation Sans"}, FontWeight::Black, false};
  }
  if (asciiEqualsIgnoreCase(key, "Arial")) {
    return {{"Arial", "Liberation Sans", "Noto Sans", "DejaVu Sans"}, FontWeight::Regular, false};
  }
  if (asciiEqualsIgnoreCase(key, "Yu Gothic UI") ||
      asciiEqualsIgnoreCase(key, "Malgun Gothic") ||
      asciiEqualsIgnoreCase(key, "Microsoft YaHei UI")) {
    return {{std::string(key), "Noto Sans CJK JP", "Noto Sans CJK KR", "Noto Sans CJK SC", "Noto Sans", "DejaVu Sans"},
            FontWeight::Regular,
            false};
  }

  std::string clean(key);
  std::vector<std::string> fams = {clean};
  if (clean.find('_') != std::string::npos) {
    std::string spaced = clean;
    std::replace(spaced.begin(), spaced.end(), '_', ' ');
    fams.push_back(std::move(spaced));
  }
  fams.emplace_back("Noto Sans");
  fams.emplace_back("DejaVu Sans");
  return {std::move(fams), FontWeight::Regular, false};
}

int toFontconfigWeight(FontWeight w, FontWeight fallbackDefault) noexcept {
  const FontWeight eff = (w == FontWeight::Default) ? fallbackDefault : w;
  switch (eff) {
    case FontWeight::Light: return FC_WEIGHT_LIGHT;
    case FontWeight::Regular: return FC_WEIGHT_REGULAR;
    case FontWeight::SemiBold: return FC_WEIGHT_SEMIBOLD;
    case FontWeight::Bold: return FC_WEIGHT_BOLD;
    case FontWeight::Black: return FC_WEIGHT_BLACK;
    case FontWeight::Default:
    default: return FC_WEIGHT_REGULAR;
  }
}

bool isBoldWeight(FontWeight w, FontWeight fallbackDefault) noexcept {
  const FontWeight eff = (w == FontWeight::Default) ? fallbackDefault : w;
  return eff == FontWeight::SemiBold || eff == FontWeight::Bold || eff == FontWeight::Black;
}

bool isRtlCodepoint(uint32_t cp) noexcept {
  return (cp >= 0x0590u && cp <= 0x05FFu) ||
         (cp >= 0x0600u && cp <= 0x06FFu) ||
         (cp >= 0x0750u && cp <= 0x077Fu) ||
         (cp >= 0x08A0u && cp <= 0x08FFu) ||
         (cp >= 0xFB50u && cp <= 0xFDFFu) ||
         (cp >= 0xFE70u && cp <= 0xFEFFu);
}

std::vector<uint32_t> decodeUtf8Codepoints(std::string_view utf8) {
  const std::string clean = iota::core::sanitizeUtf8(utf8);
  std::vector<uint32_t> out;
  out.reserve(clean.size());
  size_t i = 0;
  while (i < clean.size()) {
    const uint8_t b0 = static_cast<uint8_t>(clean[i]);
    if (b0 < 0x80u) {
      out.push_back(b0);
      ++i;
    } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < clean.size()) {
      const uint32_t cp = ((static_cast<uint32_t>(b0) & 0x1Fu) << 6) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(clean[i + 1])) & 0x3Fu);
      out.push_back(cp);
      i += 2;
    } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < clean.size()) {
      const uint32_t cp = ((static_cast<uint32_t>(b0) & 0x0Fu) << 12) |
                          ((static_cast<uint32_t>(static_cast<uint8_t>(clean[i + 1])) & 0x3Fu) << 6) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(clean[i + 2])) & 0x3Fu);
      out.push_back(cp);
      i += 3;
    } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < clean.size()) {
      const uint32_t cp = ((static_cast<uint32_t>(b0) & 0x07u) << 18) |
                          ((static_cast<uint32_t>(static_cast<uint8_t>(clean[i + 1])) & 0x3Fu) << 12) |
                          ((static_cast<uint32_t>(static_cast<uint8_t>(clean[i + 2])) & 0x3Fu) << 6) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(clean[i + 3])) & 0x3Fu);
      out.push_back(cp);
      i += 4;
    } else {
      out.push_back(0xFFFDu);
      ++i;
    }
  }
  return out;
}

#endif

// Compact fallback 5x7 glyph bitmap atlas (ASCII 32..126) used only if OS font backend is unavailable.
std::vector<uint8_t> buildFallbackBitmapAtlas(int& outW, int& outH) {
  outW = 256;
  outH = 128;
  std::vector<uint8_t> rgba(static_cast<size_t>(outW * outH * 4), 0);
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

}  // namespace

TextAlignment textAlignmentFromString(std::string_view s) noexcept {
  if (asciiEqualsIgnoreCase(s, "Left")) return TextAlignment::Left;
  if (asciiEqualsIgnoreCase(s, "Right")) return TextAlignment::Right;
  return TextAlignment::Center;
}

FontWeight fontWeightFromString(std::string_view s) noexcept {
  if (asciiEqualsIgnoreCase(s, "Light")) return FontWeight::Light;
  if (asciiEqualsIgnoreCase(s, "Regular") || asciiEqualsIgnoreCase(s, "Normal")) return FontWeight::Regular;
  if (asciiEqualsIgnoreCase(s, "SemiBold")) return FontWeight::SemiBold;
  if (asciiEqualsIgnoreCase(s, "Bold")) return FontWeight::Bold;
  if (asciiEqualsIgnoreCase(s, "Black")) return FontWeight::Black;
  return FontWeight::Default;
}

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
  static constexpr int kAtlasSize = 2048;

  iota::rhi::IRenderDevice* device = nullptr;
  iota::rhi::TextureHandle atlasTex{};
  int atlasW = kAtlasSize;
  int atlasH = kAtlasSize;
  int nextShelfY = 1;
  int atlasResets = 0;
  std::vector<AtlasShelf> shelves;
  std::unordered_map<GlyphKey, CachedGlyph, GlyphKeyHash> glyphCache;

  // Scratch buffers reused across glyphs
  std::vector<uint8_t> rgbScratch;
  std::vector<uint8_t> alphaScratch;
  std::vector<uint8_t> dilatedScratch;
  std::vector<uint8_t> rgbaScratch;

#ifdef _WIN32
  static constexpr size_t kMaxShapedLayouts = 64;
  bool dwriteInitAttempted = false;
  bool dwriteReady = false;
  ComPtr<IDWriteFactory> dwFactory;
  ComPtr<IDWriteFactory2> dwFactory2;
  ComPtr<IDWriteFactory3> dwFactory3;
  ComPtr<IDWriteFactory5> dwFactory5;
  ComPtr<IDWriteFontFallback> systemFallback;
  ComPtr<IDWriteInMemoryFontFileLoader> inMemoryLoader;
  std::unordered_map<std::string, CustomFontCollectionEntry> customFonts;
  std::unordered_map<uintptr_t, ComPtr<IDWriteFontFace>> pinnedFontFaces;
  std::vector<ShapedGlyph> scratchShaped;
  uint64_t customFontGen = 0;
  uint64_t shapedLruCounter = 0;
  uint64_t shapedLayoutHits = 0;
  std::unordered_map<ShapedLayoutKey, ShapedLayoutEntry, ShapedLayoutKeyHash, ShapedLayoutKeyEq> shapedLayoutCache;
#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)
  static constexpr size_t kMaxShapedLayouts = 64;
  bool ftInitAttempted = false;
  bool ftReady = false;
  FT_Library ftLib = nullptr;
  FcConfig* fcConfig = nullptr;
  std::unordered_map<std::string, std::shared_ptr<FtFaceEntry>> systemFaces;
  std::unordered_map<std::string, std::shared_ptr<FtFaceEntry>> customFaces;
  std::unordered_map<uintptr_t, std::shared_ptr<FtFaceEntry>> pinnedFaces;
  std::vector<FtShapedGlyph> scratchShaped;
  uint64_t customFontGen = 0;
  uint64_t shapedLruCounter = 0;
  uint64_t shapedLayoutHits = 0;
  std::unordered_map<ShapedLayoutKey, FtShapedLayoutEntry, ShapedLayoutKeyHash, ShapedLayoutKeyEq> shapedLayoutCache;
#else
  struct StbFaceEntry {
    std::vector<uint8_t> ttfBytes;
    stbtt_fontinfo info{};
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    bool valid = false;
  };
  bool stbInitAttempted = false;
  std::shared_ptr<StbFaceEntry> defaultStbSans;
  std::shared_ptr<StbFaceEntry> defaultStbMono;
  std::unordered_map<std::string, std::shared_ptr<StbFaceEntry>> customStbFaces;
  std::unordered_map<uintptr_t, std::shared_ptr<StbFaceEntry>> pinnedStbFaces;
#endif

  iota::rhi::TextureHandle fallbackTex{};
  int fallbackW = 256;
  int fallbackH = 128;

#ifdef _WIN32
  void initDirectWrite() {
    if (dwriteInitAttempted) return;
    dwriteInitAttempted = true;

    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwFactory.put()));
    if (FAILED(hr) || !dwFactory) {
      return;
    }

    dwFactory->QueryInterface(__uuidof(IDWriteFactory2), reinterpret_cast<void**>(dwFactory2.put()));
    if (dwFactory2) {
      dwFactory2->GetSystemFontFallback(systemFallback.put());
    }
    dwFactory->QueryInterface(__uuidof(IDWriteFactory3), reinterpret_cast<void**>(dwFactory3.put()));
    dwFactory->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(dwFactory5.put()));
    if (dwFactory5) {
      if (SUCCEEDED(dwFactory5->CreateInMemoryFontFileLoader(inMemoryLoader.put())) && inMemoryLoader) {
        if (FAILED(dwFactory->RegisterFontFileLoader(inMemoryLoader.get()))) {
          inMemoryLoader.reset();
        }
      }
    }
    dwriteReady = true;
  }

  void shutdownDirectWrite() {
    shapedLayoutCache.clear();
    pinnedFontFaces.clear();
    customFonts.clear();
    if (dwFactory && inMemoryLoader) {
      dwFactory->UnregisterFontFileLoader(inMemoryLoader.get());
    }
    inMemoryLoader.reset();
    systemFallback.reset();
    dwFactory5.reset();
    dwFactory3.reset();
    dwFactory2.reset();
    dwFactory.reset();
    dwriteInitAttempted = false;
    dwriteReady = false;
  }
#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)
  void initFreeType() {
    if (ftInitAttempted) return;
    ftInitAttempted = true;
    if (FT_Init_FreeType(&ftLib) != 0) {
      ftLib = nullptr;
      return;
    }
    fcConfig = FcInitLoadConfigAndFonts();
    if (!fcConfig) {
      FT_Done_FreeType(ftLib);
      ftLib = nullptr;
      return;
    }
    ftReady = true;
  }

  void shutdownFreeType() {
    shapedLayoutCache.clear();
    pinnedFaces.clear();
    customFaces.clear();
    systemFaces.clear();
    if (fcConfig != nullptr) {
      FcConfigDestroy(fcConfig);
      fcConfig = nullptr;
    }
    if (ftLib != nullptr) {
      FT_Done_FreeType(ftLib);
      ftLib = nullptr;
    }
    ftInitAttempted = false;
    ftReady = false;
  }
#else
  static std::shared_ptr<StbFaceEntry> loadStbFaceFromBytes(std::vector<uint8_t> bytes) {
    if (bytes.empty()) return nullptr;
    auto entry = std::make_shared<StbFaceEntry>();
    entry->ttfBytes = std::move(bytes);
    const int offset = stbtt_GetFontOffsetForIndex(entry->ttfBytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&entry->info, entry->ttfBytes.data(), offset)) {
      return nullptr;
    }
    stbtt_GetFontVMetrics(&entry->info, &entry->ascent, &entry->descent, &entry->lineGap);
    entry->valid = true;
    return entry;
  }

  void initStbFonts() {
    if (stbInitAttempted) return;
    stbInitAttempted = true;
    for (const char* p : {"/assets/fonts/NotoSans-Regular.ttf", "assets/fonts/NotoSans-Regular.ttf"}) {
      if (auto bytes = readFileBytes(iota::core::pathFromUtf8(p)); !bytes.empty()) {
        defaultStbSans = loadStbFaceFromBytes(std::move(bytes));
        if (defaultStbSans) break;
      }
    }
    for (const char* p : {"/assets/fonts/NotoSansMono-Regular.ttf", "assets/fonts/NotoSansMono-Regular.ttf"}) {
      if (auto bytes = readFileBytes(iota::core::pathFromUtf8(p)); !bytes.empty()) {
        defaultStbMono = loadStbFaceFromBytes(std::move(bytes));
        if (defaultStbMono) break;
      }
    }
  }

  void shutdownStbFonts() {
    pinnedStbFaces.clear();
    customStbFaces.clear();
    defaultStbSans.reset();
    defaultStbMono.reset();
    stbInitAttempted = false;
  }

  std::shared_ptr<StbFaceEntry> resolveStbFace(std::string_view typeFaceKey, const TextStyle& style) {
    initStbFonts();
    if (!typeFaceKey.empty()) {
      const bool isLocalUri = (typeFaceKey.rfind("local:", 0) == 0);
      const std::filesystem::path diskPath =
          isLocalUri ? std::filesystem::path() : iota::core::pathFromUtf8(typeFaceKey);
      const bool isDiskFont = !isLocalUri && iota::core::pathExistsNoThrow(diskPath);
      if (isLocalUri || isDiskFont) {
        const std::string cacheKey(typeFaceKey);
        if (auto it = customStbFaces.find(cacheKey); it != customStbFaces.end()) {
          return it->second;
        }
        std::vector<uint8_t> bytes;
        if (isLocalUri && style.localAssets) {
          const std::string assetName(typeFaceKey.substr(6));
          if (auto mapIt = style.localAssets->find(assetName); mapIt != style.localAssets->end()) {
            bytes = mapIt->second;
          }
        } else if (isDiskFont) {
          bytes = readFileBytes(diskPath);
        }
        if (auto face = loadStbFaceFromBytes(std::move(bytes))) {
          customStbFaces.emplace(cacheKey, face);
          return face;
        }
      }
    }
    if (typeFaceKey == "internal_1" && defaultStbMono) {
      return defaultStbMono;
    }
    return defaultStbSans ? defaultStbSans : defaultStbMono;
  }

  bool ensureGlyphStb(const std::shared_ptr<StbFaceEntry>& faceHolder,
                      int glyphIdx,
                      uint16_t sizeBucketPx,
                      uint8_t outlinePx,
                      bool& outResetOccurred) {
    if (!faceHolder || !faceHolder->valid) return false;
    const uintptr_t faceKey = reinterpret_cast<uintptr_t>(faceHolder.get());
    pinnedStbFaces.try_emplace(faceKey, faceHolder);

    const GlyphKey key{faceKey, static_cast<uint32_t>(glyphIdx), sizeBucketPx, outlinePx};
    if (glyphCache.find(key) != glyphCache.end()) {
      return true;
    }

    const float scale = stbtt_ScaleForPixelHeight(&faceHolder->info, static_cast<float>(sizeBucketPx));
    int gw = 0, gh = 0, xoff = 0, yoff = 0;
    uint8_t* rawBmp = stbtt_GetGlyphBitmap(&faceHolder->info, scale, scale, glyphIdx, &gw, &gh, &xoff, &yoff);
    if (!rawBmp || gw <= 0 || gh <= 0) {
      if (rawBmp) stbtt_FreeBitmap(rawBmp, nullptr);
      glyphCache.emplace(key, CachedGlyph{false});
      return true;
    }

    alphaScratch.assign(rawBmp, rawBmp + static_cast<size_t>(gw) * static_cast<size_t>(gh));
    stbtt_FreeBitmap(rawBmp, nullptr);

    const uint8_t* finalAlpha = alphaScratch.data();
    int outW = gw;
    int outH = gh;
    int offX = xoff;
    int offY = yoff;

    if (outlinePx > 0) {
      const int rad = static_cast<int>(outlinePx);
      outW = gw + 2 * rad;
      outH = gh + 2 * rad;
      offX -= rad;
      offY -= rad;
      dilateAlphaCircle(alphaScratch, gw, gh, rad, dilatedScratch);
      finalAlpha = dilatedScratch.data();
    }

    int px = 0;
    int py = 0;
    if (!packRect(outW, outH, px, py)) {
      resetAtlas();
      outResetOccurred = true;
      pinnedStbFaces.try_emplace(faceKey, faceHolder);
      if (!packRect(outW, outH, px, py)) {
        glyphCache.emplace(key, CachedGlyph{false});
        return false;
      }
    }

    const size_t totalOut = static_cast<size_t>(outW) * static_cast<size_t>(outH);
    rgbaScratch.resize(totalOut * 4u);
    for (size_t i = 0; i < totalOut; ++i) {
      const uint8_t a = finalAlpha[i];
      rgbaScratch[i * 4 + 0] = a;
      rgbaScratch[i * 4 + 1] = a;
      rgbaScratch[i * 4 + 2] = a;
      rgbaScratch[i * 4 + 3] = a;
    }
    device->updateTexture2DRegion(atlasTex, px, py, outW, outH, rgbaScratch.data());

    CachedGlyph cg;
    cg.visible = true;
    cg.u0 = static_cast<float>(px) / static_cast<float>(atlasW);
    cg.v0 = static_cast<float>(py) / static_cast<float>(atlasH);
    cg.u1 = static_cast<float>(px + outW) / static_cast<float>(atlasW);
    cg.v1 = static_cast<float>(py + outH) / static_cast<float>(atlasH);
    cg.offsetX = offX;
    cg.offsetY = offY;
    cg.width = outW;
    cg.height = outH;
    glyphCache.emplace(key, cg);
    return true;
  }
#endif

  bool ensureAtlasTexture() {
    if (!device) return false;
    if (atlasTex) return true;

    std::vector<uint8_t> zeros(static_cast<size_t>(kAtlasSize) * kAtlasSize * 4u, 0);
    iota::rhi::TextureDesc td;
    td.width = kAtlasSize;
    td.height = kAtlasSize;
    td.rgbaPixels = zeros.data();
    td.filter = iota::rhi::TextureFilter::Linear;
    td.wrap = iota::rhi::TextureWrap::ClampToEdge;
    atlasTex = device->createTexture2D(td);
    atlasW = kAtlasSize;
    atlasH = kAtlasSize;
    nextShelfY = 1;
    shelves.clear();
    return static_cast<bool>(atlasTex);
  }

  void resetAtlas() {
    glyphCache.clear();
    shelves.clear();
    nextShelfY = 1;
#ifdef _WIN32
    pinnedFontFaces.clear();
#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)
    pinnedFaces.clear();
#else
    pinnedStbFaces.clear();
#endif
    ++atlasResets;
    if (device && atlasTex) {
      std::vector<uint8_t> zeros(static_cast<size_t>(atlasW) * atlasH * 4u, 0);
      device->updateTexture2D(atlasTex, atlasW, atlasH, zeros.data());
    }
  }

  bool packRect(int w, int h, int& outX, int& outY) {
    if (!ensureAtlasTexture()) return false;
    const int pw = w + 2;
    const int ph = h + 2;
    if (pw > atlasW - 2 || ph > atlasH - 2) {
      return false;
    }

    for (auto& shelf : shelves) {
      if (shelf.height >= ph && shelf.height <= ph + 24 && shelf.cursorX + pw <= atlasW - 1) {
        outX = shelf.cursorX + 1;
        outY = shelf.y + 1;
        shelf.cursorX += pw;
        return true;
      }
    }

    if (nextShelfY + ph <= atlasH - 1) {
      shelves.push_back(AtlasShelf{nextShelfY, ph, 1 + pw});
      outX = 2;
      outY = nextShelfY + 1;
      nextShelfY += ph;
      return true;
    }
    return false;
  }

#ifdef _WIN32
  void resolveCustomCollection(std::string_view typeFaceKey,
                               const TextStyle& style,
                               std::wstring& outFamily,
                               IDWriteFontCollection*& outCollection) {
    outCollection = nullptr;
    if (typeFaceKey.empty()) {
      return;
    }

    const bool isLocalUri = (typeFaceKey.rfind("local:", 0) == 0);
    const std::filesystem::path diskPath =
        isLocalUri ? std::filesystem::path() : iota::core::pathFromUtf8(typeFaceKey);
    const bool isDiskFont = !isLocalUri && iota::core::pathExistsNoThrow(diskPath);
    if (!isLocalUri && !isDiskFont) {
      return;
    }

    std::string cacheKey(typeFaceKey);
    auto it = customFonts.find(cacheKey);
    if (it != customFonts.end() && it->second.collection) {
      outFamily = it->second.familyName;
      outCollection = it->second.collection.get();
      return;
    }

    if (!dwFactory3 || !dwFactory5 || !inMemoryLoader) {
      return;
    }

    std::vector<uint8_t> bytes;
    if (isLocalUri && style.localAssets) {
      const std::string assetName(typeFaceKey.substr(6));
      if (auto mapIt = style.localAssets->find(assetName); mapIt != style.localAssets->end()) {
        bytes = mapIt->second;
      }
    } else if (isDiskFont) {
      bytes = readFileBytes(diskPath);
    }
    if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<UINT32>::max())) {
      return;
    }

    ComPtr<IDWriteFontFile> fontFile;
    if (FAILED(inMemoryLoader->CreateInMemoryFontFileReference(
            dwFactory.get(), bytes.data(), static_cast<UINT32>(bytes.size()), nullptr, fontFile.put())) ||
        !fontFile) {
      return;
    }

    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(dwFactory5->CreateFontSetBuilder(builder.put())) || !builder) {
      return;
    }
    if (FAILED(builder->AddFontFile(fontFile.get()))) {
      return;
    }
    ComPtr<IDWriteFontSet> fontSet;
    if (FAILED(builder->CreateFontSet(fontSet.put())) || !fontSet) {
      return;
    }
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(dwFactory3->CreateFontCollectionFromFontSet(fontSet.get(), collection.put())) || !collection) {
      return;
    }

    std::wstring familyName;
    if (collection->GetFontFamilyCount() > 0) {
      ComPtr<IDWriteFontFamily> fam;
      if (SUCCEEDED(collection->GetFontFamily(0, fam.put())) && fam) {
        ComPtr<IDWriteLocalizedStrings> names;
        if (SUCCEEDED(fam->GetFamilyNames(names.put())) && names && names->GetCount() > 0) {
          UINT32 nameLen = 0;
          if (SUCCEEDED(names->GetStringLength(0, &nameLen)) && nameLen > 0) {
            std::wstring wname(nameLen + 1, L'\0');
            if (SUCCEEDED(names->GetString(0, wname.data(), nameLen + 1))) {
              wname.resize(nameLen);
              familyName = std::move(wname);
            }
          }
        }
      }
    }
    if (familyName.empty()) {
      return;
    }

    CustomFontCollectionEntry entry{std::move(collection), familyName};
    outFamily = entry.familyName;
    outCollection = entry.collection.get();
    customFonts.emplace(std::move(cacheKey), std::move(entry));
  }

  bool ensureGlyph(IDWriteFontFace* face,
                   uint16_t glyphIndex,
                   uint16_t sizeBucketPx,
                   uint8_t outlinePx,
                   bool& outResetOccurred) {
    if (!face) return false;
    const uintptr_t faceKey = reinterpret_cast<uintptr_t>(face);
    if (pinnedFontFaces.find(faceKey) == pinnedFontFaces.end()) {
      face->AddRef();
      ComPtr<IDWriteFontFace> holder;
      holder.attach(face);
      pinnedFontFaces.emplace(faceKey, std::move(holder));
    }
    const GlyphKey key{faceKey, glyphIndex, sizeBucketPx, outlinePx};
    if (glyphCache.find(key) != glyphCache.end()) {
      return true;
    }

    DWRITE_GLYPH_RUN run{};
    run.fontFace = face;
    run.fontEmSize = static_cast<FLOAT>(sizeBucketPx);
    run.glyphCount = 1;
    run.glyphIndices = &glyphIndex;

    ComPtr<IDWriteGlyphRunAnalysis> analysis;
    const HRESULT hr = dwFactory->CreateGlyphRunAnalysis(
        &run,
        1.0f,
        nullptr,
        DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
        DWRITE_MEASURING_MODE_NATURAL,
        0.0f,
        0.0f,
        analysis.put());
    if (FAILED(hr) || !analysis) {
      glyphCache.emplace(key, CachedGlyph{false});
      return true;
    }

    RECT rc{};
    DWRITE_TEXTURE_TYPE texType = DWRITE_TEXTURE_CLEARTYPE_3x1;
    if (FAILED(analysis->GetAlphaTextureBounds(texType, &rc)) ||
        rc.right <= rc.left || rc.bottom <= rc.top) {
      texType = DWRITE_TEXTURE_ALIASED_1x1;
      if (FAILED(analysis->GetAlphaTextureBounds(texType, &rc)) ||
          rc.right <= rc.left || rc.bottom <= rc.top) {
        glyphCache.emplace(key, CachedGlyph{false});
        return true;
      }
    }

    const int gw = static_cast<int>(rc.right - rc.left);
    const int gh = static_cast<int>(rc.bottom - rc.top);
    const size_t pixelCount = static_cast<size_t>(gw) * static_cast<size_t>(gh);
    alphaScratch.resize(pixelCount);

    if (texType == DWRITE_TEXTURE_CLEARTYPE_3x1) {
      rgbScratch.resize(pixelCount * 3u);
      if (FAILED(analysis->CreateAlphaTexture(texType, &rc, rgbScratch.data(),
                                              static_cast<UINT32>(rgbScratch.size())))) {
        glyphCache.emplace(key, CachedGlyph{false});
        return true;
      }
      for (size_t i = 0; i < pixelCount; ++i) {
        const uint32_t avg = (static_cast<uint32_t>(rgbScratch[i * 3 + 0]) +
                              static_cast<uint32_t>(rgbScratch[i * 3 + 1]) +
                              static_cast<uint32_t>(rgbScratch[i * 3 + 2]) + 1u) /
                             3u;
        alphaScratch[i] = static_cast<uint8_t>(avg);
      }
    } else {
      if (FAILED(analysis->CreateAlphaTexture(texType, &rc, alphaScratch.data(),
                                              static_cast<UINT32>(alphaScratch.size())))) {
        glyphCache.emplace(key, CachedGlyph{false});
        return true;
      }
    }

    const uint8_t* finalAlpha = alphaScratch.data();
    int outW = gw;
    int outH = gh;
    int offX = static_cast<int>(rc.left);
    int offY = static_cast<int>(rc.top);

    if (outlinePx > 0) {
      const int rad = static_cast<int>(outlinePx);
      outW = gw + 2 * rad;
      outH = gh + 2 * rad;
      offX -= rad;
      offY -= rad;
      dilateAlphaCircle(alphaScratch, gw, gh, rad, dilatedScratch);
      finalAlpha = dilatedScratch.data();
    }

    int px = 0;
    int py = 0;
    if (!packRect(outW, outH, px, py)) {
      resetAtlas();
      outResetOccurred = true;
      face->AddRef();
      ComPtr<IDWriteFontFace> rePin;
      rePin.attach(face);
      pinnedFontFaces.emplace(faceKey, std::move(rePin));
      if (!packRect(outW, outH, px, py)) {
        glyphCache.emplace(key, CachedGlyph{false});
        return false;
      }
    }

    const size_t totalOut = static_cast<size_t>(outW) * static_cast<size_t>(outH);
    rgbaScratch.resize(totalOut * 4u);
    for (size_t i = 0; i < totalOut; ++i) {
      const uint8_t a = finalAlpha[i];
      rgbaScratch[i * 4 + 0] = a;
      rgbaScratch[i * 4 + 1] = a;
      rgbaScratch[i * 4 + 2] = a;
      rgbaScratch[i * 4 + 3] = a;
    }
    device->updateTexture2DRegion(atlasTex, px, py, outW, outH, rgbaScratch.data());

    CachedGlyph cg;
    cg.visible = true;
    cg.u0 = static_cast<float>(px) / static_cast<float>(atlasW);
    cg.v0 = static_cast<float>(py) / static_cast<float>(atlasH);
    cg.u1 = static_cast<float>(px + outW) / static_cast<float>(atlasW);
    cg.v1 = static_cast<float>(py + outH) / static_cast<float>(atlasH);
    cg.offsetX = offX;
    cg.offsetY = offY;
    cg.width = outW;
    cg.height = outH;
    glyphCache.emplace(key, cg);
    return true;
  }
#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)
  std::shared_ptr<FtFaceEntry> openFaceFromMemory(std::vector<uint8_t> bytes,
                                                  bool wantBold,
                                                  bool wantItalic) {
    if (!ftLib || bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<FT_Long>::max())) {
      return nullptr;
    }
    FT_Face ftFace = nullptr;
    if (FT_New_Memory_Face(ftLib, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &ftFace) != 0 || !ftFace) {
      return nullptr;
    }
    FT_Select_Charmap(ftFace, FT_ENCODING_UNICODE);
    auto entry = std::make_shared<FtFaceEntry>();
    entry->memoryBytes = std::move(bytes);
    entry->ftFace = ftFace;
    entry->hbFont = hb_ft_font_create_referenced(ftFace);
    entry->synthBold = wantBold && ((ftFace->style_flags & FT_STYLE_FLAG_BOLD) == 0);
    entry->synthItalic = wantItalic && ((ftFace->style_flags & FT_STYLE_FLAG_ITALIC) == 0);
    return entry;
  }

  std::shared_ptr<FtFaceEntry> openFaceFromPathIndex(const std::string& filePath,
                                                     int faceIndex,
                                                     bool wantBold,
                                                     bool wantItalic) {
    if (!ftLib || filePath.empty()) {
      return nullptr;
    }
    const std::string cacheKey = filePath + "#" + std::to_string(faceIndex) +
                                 (wantBold ? "#B" : "#N") + (wantItalic ? "#I" : "#R");
    if (auto it = systemFaces.find(cacheKey); it != systemFaces.end()) {
      return it->second;
    }

    FT_Face ftFace = nullptr;
    if (FT_New_Face(ftLib, filePath.c_str(), static_cast<FT_Long>(faceIndex), &ftFace) != 0 || !ftFace) {
      return nullptr;
    }
    FT_Select_Charmap(ftFace, FT_ENCODING_UNICODE);
    auto entry = std::make_shared<FtFaceEntry>();
    entry->ftFace = ftFace;
    entry->hbFont = hb_ft_font_create_referenced(ftFace);
    entry->synthBold = wantBold && ((ftFace->style_flags & FT_STYLE_FLAG_BOLD) == 0);
    entry->synthItalic = wantItalic && ((ftFace->style_flags & FT_STYLE_FLAG_ITALIC) == 0);
    systemFaces.emplace(cacheKey, entry);
    return entry;
  }

  std::shared_ptr<FtFaceEntry> resolveFontconfigFace(const std::vector<std::string>& families,
                                                     int fcWeight,
                                                     bool wantBold,
                                                     bool wantItalic,
                                                     uint32_t requiredCp) {
    if (!fcConfig) return nullptr;
    FcPattern* pat = FcPatternCreate();
    if (!pat) return nullptr;

    for (const auto& fam : families) {
      if (!fam.empty()) {
        FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(fam.c_str()));
      }
    }
    FcPatternAddInteger(pat, FC_WEIGHT, fcWeight);
    FcPatternAddInteger(pat, FC_SLANT, wantItalic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);

    if (requiredCp != 0) {
      FcCharSet* cs = FcCharSetCreate();
      if (cs) {
        FcCharSetAddChar(cs, static_cast<FcChar32>(requiredCp));
        FcPatternAddCharSet(pat, FC_CHARSET, cs);
        FcCharSetDestroy(cs);
      }
    }

    FcConfigSubstitute(fcConfig, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult fcRes = FcResultMatch;
    FcPattern* match = FcFontMatch(fcConfig, pat, &fcRes);
    FcPatternDestroy(pat);
    if (!match) return nullptr;

    FcChar8* file = nullptr;
    int index = 0;
    std::string filePath;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
      filePath = reinterpret_cast<const char*>(file);
    }
    FcPatternGetInteger(match, FC_INDEX, 0, &index);
    FcPatternDestroy(match);

    if (filePath.empty()) {
      return nullptr;
    }
    auto face = openFaceFromPathIndex(filePath, index, wantBold, wantItalic);
    if (face && requiredCp != 0 && FT_Get_Char_Index(face->ftFace, requiredCp) == 0 && !families.empty()) {
      return resolveFontconfigFace({}, fcWeight, wantBold, wantItalic, requiredCp);
    }
    return face;
  }

  std::shared_ptr<FtFaceEntry> resolveCustomFace(std::string_view typeFaceKey,
                                                 const TextStyle& style,
                                                 bool wantBold,
                                                 bool wantItalic) {
    if (typeFaceKey.empty()) {
      return nullptr;
    }
    const bool isLocalUri = (typeFaceKey.rfind("local:", 0) == 0);
    const std::filesystem::path diskPath =
        isLocalUri ? std::filesystem::path() : iota::core::pathFromUtf8(typeFaceKey);
    const bool isDiskFont = !isLocalUri && iota::core::pathExistsNoThrow(diskPath);
    if (!isLocalUri && !isDiskFont) {
      return nullptr;
    }

    const std::string cacheKey = std::string(typeFaceKey) + (wantBold ? "#B" : "#N") + (wantItalic ? "#I" : "#R");
    if (auto it = customFaces.find(cacheKey); it != customFaces.end()) {
      return it->second;
    }

    std::vector<uint8_t> bytes;
    if (isLocalUri && style.localAssets) {
      const std::string assetName(typeFaceKey.substr(6));
      if (auto mapIt = style.localAssets->find(assetName); mapIt != style.localAssets->end()) {
        bytes = mapIt->second;
      }
    } else if (isDiskFont) {
      bytes = readFileBytes(diskPath);
    }
    if (bytes.empty()) {
      return nullptr;
    }

    auto entry = openFaceFromMemory(std::move(bytes), wantBold, wantItalic);
    if (entry) {
      customFaces.emplace(cacheKey, entry);
    }
    return entry;
  }

  bool ensureGlyphFt(const std::shared_ptr<FtFaceEntry>& faceHolder,
                     uint32_t glyphIndex,
                     uint16_t sizeBucketPx,
                     uint8_t outlinePx,
                     bool& outResetOccurred) {
    if (!faceHolder || !faceHolder->ftFace) return false;
    FT_Face face = faceHolder->ftFace;
    const uintptr_t faceKey = reinterpret_cast<uintptr_t>(face);
    if (pinnedFaces.find(faceKey) == pinnedFaces.end()) {
      pinnedFaces.emplace(faceKey, faceHolder);
    }

    const GlyphKey key{faceKey, glyphIndex, sizeBucketPx, outlinePx};
    if (glyphCache.find(key) != glyphCache.end()) {
      return true;
    }

    faceHolder->ensurePixelSize(sizeBucketPx);
    if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT) != 0) {
      glyphCache.emplace(key, CachedGlyph{false});
      return true;
    }
    if (faceHolder->synthBold) {
      FT_GlyphSlot_Embolden(face->glyph);
    }
    if (faceHolder->synthItalic) {
      FT_GlyphSlot_Oblique(face->glyph);
    }
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
      glyphCache.emplace(key, CachedGlyph{false});
      return true;
    }

    const FT_Bitmap& bmp = face->glyph->bitmap;
    const int gw = static_cast<int>(bmp.width);
    const int gh = static_cast<int>(bmp.rows);
    if (gw <= 0 || gh <= 0 || bmp.buffer == nullptr) {
      glyphCache.emplace(key, CachedGlyph{false});
      return true;
    }

    const size_t pixelCount = static_cast<size_t>(gw) * static_cast<size_t>(gh);
    alphaScratch.resize(pixelCount);
    const int pitch = bmp.pitch;
    for (int row = 0; row < gh; ++row) {
      const uint8_t* srcRow = (pitch >= 0)
                                  ? (bmp.buffer + row * pitch)
                                  : (bmp.buffer + (gh - 1 - row) * (-pitch));
      if (bmp.pixel_mode == FT_PIXEL_MODE_MONO) {
        for (int col = 0; col < gw; ++col) {
          const uint8_t byteVal = srcRow[col >> 3];
          const bool bitSet = (byteVal & (0x80u >> (col & 7))) != 0;
          alphaScratch[static_cast<size_t>(row) * gw + col] = bitSet ? 255 : 0;
        }
      } else {
        std::copy_n(srcRow, gw, alphaScratch.data() + static_cast<size_t>(row) * gw);
      }
    }

    const uint8_t* finalAlpha = alphaScratch.data();
    int outW = gw;
    int outH = gh;
    int offX = face->glyph->bitmap_left;
    int offY = -face->glyph->bitmap_top;

    if (outlinePx > 0) {
      const int rad = static_cast<int>(outlinePx);
      outW = gw + 2 * rad;
      outH = gh + 2 * rad;
      offX -= rad;
      offY -= rad;
      dilateAlphaCircle(alphaScratch, gw, gh, rad, dilatedScratch);
      finalAlpha = dilatedScratch.data();
    }

    int px = 0;
    int py = 0;
    if (!packRect(outW, outH, px, py)) {
      resetAtlas();
      outResetOccurred = true;
      pinnedFaces.emplace(faceKey, faceHolder);
      if (!packRect(outW, outH, px, py)) {
        glyphCache.emplace(key, CachedGlyph{false});
        return false;
      }
    }

    const size_t totalOut = static_cast<size_t>(outW) * static_cast<size_t>(outH);
    rgbaScratch.resize(totalOut * 4u);
    for (size_t i = 0; i < totalOut; ++i) {
      const uint8_t a = finalAlpha[i];
      rgbaScratch[i * 4 + 0] = a;
      rgbaScratch[i * 4 + 1] = a;
      rgbaScratch[i * 4 + 2] = a;
      rgbaScratch[i * 4 + 3] = a;
    }
    device->updateTexture2DRegion(atlasTex, px, py, outW, outH, rgbaScratch.data());

    CachedGlyph cg;
    cg.visible = true;
    cg.u0 = static_cast<float>(px) / static_cast<float>(atlasW);
    cg.v0 = static_cast<float>(py) / static_cast<float>(atlasH);
    cg.u1 = static_cast<float>(px + outW) / static_cast<float>(atlasW);
    cg.v1 = static_cast<float>(py + outH) / static_cast<float>(atlasH);
    cg.offsetX = offX;
    cg.offsetY = offY;
    cg.width = outW;
    cg.height = outH;
    glyphCache.emplace(key, cg);
    return true;
  }
#endif

  iota::rhi::TextureHandle buildFallbackQuads(std::string_view text,
                                                float fontSizePx,
                                                iota::core::RectF bounds,
                                                uint32_t packedColor,
                                                std::vector<iota::rhi::VertexPosColUv>& outVertices,
                                                std::vector<uint32_t>& outIndices) {
    if (!fallbackTex) {
      auto fallbackRgba = buildFallbackBitmapAtlas(fallbackW, fallbackH);
      iota::rhi::TextureDesc td;
      td.width = fallbackW;
      td.height = fallbackH;
      td.rgbaPixels = fallbackRgba.data();
      td.filter = iota::rhi::TextureFilter::Linear;
      td.wrap = iota::rhi::TextureWrap::ClampToEdge;
      fallbackTex = device->createTexture2D(td);
    }
    if (!fallbackTex) return {};

    const float scale = std::max(8.0f, fontSizePx) / 14.0f;
    const float lineAdvance = std::max(8.0f, fontSizePx) * 1.15f;

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

    const float totalH = static_cast<float>(lines.size()) * lineAdvance;
    float baselineY = bounds.centerY() - totalH * 0.5f + fontSizePx * 0.85f;
    for (std::string_view line : lines) {
      float lineWidth = 0.0f;
      for (unsigned char ch : line) {
        if (ch >= 32 && ch < 127) lineWidth += 11.0f * scale;
      }
      float cursorX = bounds.centerX() - lineWidth * 0.5f;
      for (unsigned char ch : line) {
        if (ch < 32 || ch >= 127) continue;
        const int idx = ch - 32;
        const float u0 = static_cast<float>(idx % 16) / 16.0f;
        const float v0 = static_cast<float>(idx / 16) / 8.0f;
        const float u1 = u0 + (1.0f / 16.0f);
        const float v1 = v0 + (1.0f / 8.0f);
        const float gw = 11.0f * scale;
        const float gh = 14.0f * scale;

        const uint32_t base = static_cast<uint32_t>(outVertices.size());
        outVertices.push_back({cursorX, baselineY - gh, 0.0f, packedColor, u0, v0});
        outVertices.push_back({cursorX + gw, baselineY - gh, 0.0f, packedColor, u1, v0});
        outVertices.push_back({cursorX + gw, baselineY, 0.0f, packedColor, u1, v1});
        outVertices.push_back({cursorX, baselineY, 0.0f, packedColor, u0, v1});
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 3);
        cursorX += gw;
      }
      baselineY += lineAdvance;
    }
    return fallbackTex;
  }
};

FontAtlasCache::FontAtlasCache() : impl_(std::make_unique<Impl>()) {}
FontAtlasCache::~FontAtlasCache() { shutdown(); }

void FontAtlasCache::setDevice(iota::rhi::IRenderDevice* device) {
  if (impl_->device != device) {
    shutdown();
    impl_->device = device;
    if (impl_->device) {
#ifdef _WIN32
      impl_->initDirectWrite();
#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)
      impl_->initFreeType();
#else
      impl_->initStbFonts();
#endif
    }
  }
}

void FontAtlasCache::shutdown() {
  if (!impl_) return;
  if (impl_->device) {
    if (impl_->atlasTex) {
      impl_->device->destroyTexture(impl_->atlasTex);
      impl_->atlasTex = {};
    }
    if (impl_->fallbackTex) {
      impl_->device->destroyTexture(impl_->fallbackTex);
      impl_->fallbackTex = {};
    }
  }
  impl_->glyphCache.clear();
  impl_->shelves.clear();
  impl_->nextShelfY = 1;
#ifdef _WIN32
  impl_->shutdownDirectWrite();
#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)
  impl_->shutdownFreeType();
#else
  impl_->shutdownStbFonts();
#endif
  impl_->device = nullptr;
}

void FontAtlasCache::invalidateCustomFonts() {
  if (!impl_) return;
#ifdef _WIN32
  impl_->shapedLayoutCache.clear();
  ++impl_->customFontGen;
  impl_->customFonts.clear();
#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)
  impl_->shapedLayoutCache.clear();
  ++impl_->customFontGen;
  impl_->customFaces.clear();
#else
  impl_->customStbFaces.clear();
#endif
  impl_->resetAtlas();
}

FontAtlasStats FontAtlasCache::stats() const noexcept {
  FontAtlasStats st;
  if (!impl_) return st;
  st.cachedGlyphs = static_cast<int>(impl_->glyphCache.size());
  st.atlasResets = impl_->atlasResets;
  st.atlasWidth = impl_->atlasW;
  st.atlasHeight = impl_->atlasH;
#ifdef _WIN32
  st.usingDirectWrite = impl_->dwriteReady;
  st.usingNativeFontBackend = impl_->dwriteReady;
  st.cachedShapedLayouts = static_cast<int>(impl_->shapedLayoutCache.size());
  st.shapedLayoutHits = impl_->shapedLayoutHits;
#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)
  st.usingNativeFontBackend = impl_->ftReady;
  st.cachedShapedLayouts = static_cast<int>(impl_->shapedLayoutCache.size());
  st.shapedLayoutHits = impl_->shapedLayoutHits;
#endif
  return st;
}

iota::rhi::TextureHandle FontAtlasCache::buildTextQuads(std::string_view typeFaceKey,
                                                       std::string_view text,
                                                       float fontSizePx,
                                                       iota::core::RectF bounds,
                                                       iota::core::ColorRgba color,
                                                       std::vector<iota::rhi::VertexPosColUv>& outVertices,
                                                       std::vector<uint32_t>& outIndices) {
  static const TextStyle kDefaultStyle{};
  return buildTextQuads(typeFaceKey, text, fontSizePx, bounds, color, kDefaultStyle, outVertices, outIndices);
}

iota::rhi::TextureHandle FontAtlasCache::buildTextQuads(std::string_view typeFaceKey,
                                                       std::string_view text,
                                                       float fontSizePx,
                                                       iota::core::RectF bounds,
                                                       iota::core::ColorRgba color,
                                                       const TextStyle& style,
                                                       std::vector<iota::rhi::VertexPosColUv>& outVertices,
                                                       std::vector<uint32_t>& outIndices) {
  outVertices.clear();
  outIndices.clear();
  if (!impl_ || !impl_->device || text.empty()) {
    return {};
  }

  const uint32_t fillAbgr = packPremultipliedAbgr(color);

#ifdef _WIN32
  impl_->initDirectWrite();
  if (impl_->dwriteReady && impl_->dwFactory) {
    const float clampedSize = std::max(8.0f, fontSizePx);
    const uint16_t bucketPx = quantizeFontBucketPx(clampedSize);
    const float renderScale = clampedSize / static_cast<float>(bucketPx);
    const float safeScale = std::max(renderScale, 0.05f);

    const float layoutW = style.wordWrap
                              ? std::max(bounds.width() / safeScale, 16.0f)
                              : std::max(16384.0f, (bounds.width() / safeScale) * 2.0f);
    const float layoutWKey = std::round(layoutW * 4.0f) * 0.25f;
    const bool hasCharSpacing = std::abs(style.letterSpacingPx) > 1e-3f;
    const float letterSpacingDip = hasCharSpacing ? (style.letterSpacingPx / safeScale) : 0.0f;
    const bool hasLineSpacing = std::abs(style.lineSpacing - 1.0f) > 1e-3f;
    const float lineSpacingNorm = hasLineSpacing ? std::clamp(style.lineSpacing, 0.4f, 4.0f) : 1.0f;

    const ShapedLayoutKeyView lookupKey{
        typeFaceKey,
        text,
        reinterpret_cast<uintptr_t>(style.localAssets),
        impl_->customFontGen,
        bucketPx,
        style.weight,
        style.italic,
        style.alignment,
        style.wordWrap,
        layoutWKey,
        letterSpacingDip,
        lineSpacingNorm,
    };

    const std::vector<ShapedGlyph>* shapedGlyphs = nullptr;
    float usedLayoutW = layoutW;
    float metricsTop = 0.0f;
    float metricsHeight = 0.0f;

    if (const auto hit = impl_->shapedLayoutCache.find(lookupKey); hit != impl_->shapedLayoutCache.end()) {
      hit->second.lastUseStamp = ++impl_->shapedLruCounter;
      ++impl_->shapedLayoutHits;
      shapedGlyphs = &hit->second.glyphs;
      usedLayoutW = hit->second.layoutW;
      metricsTop = hit->second.metricsTop;
      metricsHeight = hit->second.metricsHeight;
    } else {
      const std::wstring wText = utf8ToWide(text);
      if (wText.empty()) {
        return {};
      }

      FamilyPresetSpec famSpec = resolveSystemFamilyPreset(typeFaceKey);
      IDWriteFontCollection* customCol = nullptr;
      std::wstring customFam;
      impl_->resolveCustomCollection(typeFaceKey, style, customFam, customCol);
      if (customCol && !customFam.empty()) {
        famSpec.familyName = std::move(customFam);
      }

      const DWRITE_FONT_WEIGHT dwWeight = toDWriteWeight(style.weight, famSpec.defaultWeight);
      const DWRITE_FONT_STYLE dwStyle = style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;

      ComPtr<IDWriteTextFormat> textFormat;
      HRESULT hr = impl_->dwFactory->CreateTextFormat(
          famSpec.familyName.c_str(),
          customCol,
          dwWeight,
          dwStyle,
          DWRITE_FONT_STRETCH_NORMAL,
          static_cast<FLOAT>(bucketPx),
          L"",
          textFormat.put());
      if ((FAILED(hr) || !textFormat) && customCol != nullptr) {
        hr = impl_->dwFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            dwWeight,
            dwStyle,
            DWRITE_FONT_STRETCH_NORMAL,
            static_cast<FLOAT>(bucketPx),
            L"",
            textFormat.put());
      }
      if (FAILED(hr) || !textFormat) {
        return impl_->buildFallbackQuads(text, fontSizePx, bounds, fillAbgr, outVertices, outIndices);
      }

      DWRITE_TEXT_ALIGNMENT dwAlign = DWRITE_TEXT_ALIGNMENT_CENTER;
      if (style.alignment == TextAlignment::Left) dwAlign = DWRITE_TEXT_ALIGNMENT_LEADING;
      else if (style.alignment == TextAlignment::Right) dwAlign = DWRITE_TEXT_ALIGNMENT_TRAILING;
      textFormat->SetTextAlignment(dwAlign);
      textFormat->SetWordWrapping(style.wordWrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);

      const float layoutH = 65536.0f;
      ComPtr<IDWriteTextLayout> textLayout;
      hr = impl_->dwFactory->CreateTextLayout(
          wText.c_str(),
          static_cast<UINT32>(wText.size()),
          textFormat.get(),
          layoutW,
          layoutH,
          textLayout.put());
      if (FAILED(hr) || !textLayout) {
        return impl_->buildFallbackQuads(text, fontSizePx, bounds, fillAbgr, outVertices, outIndices);
      }

      if (impl_->systemFallback) {
        ComPtr<IDWriteTextLayout2> layout2;
        if (SUCCEEDED(textLayout->QueryInterface(__uuidof(IDWriteTextLayout2),
                                                 reinterpret_cast<void**>(layout2.put()))) &&
            layout2) {
          layout2->SetFontFallback(impl_->systemFallback.get());
        }
      }

      if (hasCharSpacing) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(textLayout->QueryInterface(__uuidof(IDWriteTextLayout1),
                                                 reinterpret_cast<void**>(layout1.put()))) &&
            layout1) {
          const DWRITE_TEXT_RANGE fullRange{0, static_cast<UINT32>(wText.size())};
          layout1->SetCharacterSpacing(0.0f, letterSpacingDip, 0.0f, fullRange);
        }
      }

      if (hasLineSpacing) {
        const float spacingDip = static_cast<float>(bucketPx) * 1.2f * lineSpacingNorm;
        const float baselineDip = spacingDip * 0.78f;
        textLayout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, spacingDip, baselineDip);
      }

      DWRITE_TEXT_METRICS metrics{};
      textLayout->GetMetrics(&metrics);

      impl_->scratchShaped.clear();
      ShapingCollector collector(impl_->scratchShaped, impl_->pinnedFontFaces);
      if (FAILED(textLayout->Draw(nullptr, &collector, 0.0f, 0.0f))) {
        return impl_->buildFallbackQuads(text, fontSizePx, bounds, fillAbgr, outVertices, outIndices);
      }

      metricsTop = metrics.top;
      metricsHeight = metrics.height;

      if (impl_->shapedLayoutCache.size() >= Impl::kMaxShapedLayouts) {
        auto oldest = impl_->shapedLayoutCache.begin();
        for (auto it = std::next(oldest); it != impl_->shapedLayoutCache.end(); ++it) {
          if (it->second.lastUseStamp < oldest->second.lastUseStamp) {
            oldest = it;
          }
        }
        impl_->shapedLayoutCache.erase(oldest);
      }

      ShapedLayoutKey ownedKey;
      ownedKey.typeFaceKey.assign(typeFaceKey);
      ownedKey.text.assign(text);
      ownedKey.localAssetsPtr = lookupKey.localAssetsPtr;
      ownedKey.customFontGen = lookupKey.customFontGen;
      ownedKey.bucketPx = bucketPx;
      ownedKey.weight = style.weight;
      ownedKey.italic = style.italic;
      ownedKey.alignment = style.alignment;
      ownedKey.wordWrap = style.wordWrap;
      ownedKey.layoutW = layoutWKey;
      ownedKey.letterSpacingDip = letterSpacingDip;
      ownedKey.lineSpacingNorm = lineSpacingNorm;

      ShapedLayoutEntry newEntry;
      newEntry.layoutW = layoutW;
      newEntry.metricsTop = metricsTop;
      newEntry.metricsHeight = metricsHeight;
      newEntry.lastUseStamp = ++impl_->shapedLruCounter;
      newEntry.glyphs = impl_->scratchShaped;
      for (const auto& sg : newEntry.glyphs) {
        if (!sg.face) continue;
        bool seen = false;
        for (const auto& f : newEntry.heldFaces) {
          if (f.get() == sg.face) {
            seen = true;
            break;
          }
        }
        if (!seen) {
          sg.face->AddRef();
          ComPtr<IDWriteFontFace> holder;
          holder.attach(sg.face);
          newEntry.heldFaces.push_back(std::move(holder));
        }
      }
      auto [insertedIt, inserted] = impl_->shapedLayoutCache.emplace(std::move(ownedKey), std::move(newEntry));
      (void)inserted;
      shapedGlyphs = &insertedIt->second.glyphs;
    }

    float originScreenX = bounds.centerX() - (usedLayoutW * 0.5f) * renderScale;
    if (style.alignment == TextAlignment::Left) {
      originScreenX = bounds.left;
    } else if (style.alignment == TextAlignment::Right) {
      originScreenX = bounds.right - usedLayoutW * renderScale;
    }
    const float originScreenY = bounds.centerY() - (metricsTop + metricsHeight * 0.5f) * renderScale;

    const int outlineRad = (style.outlineWidthPx > 0.05f)
                               ? std::clamp(static_cast<int>(std::lround(style.outlineWidthPx / safeScale)), 1, 10)
                               : 0;
    const uint8_t outlineKey = static_cast<uint8_t>(outlineRad);

    for (int attempt = 0; attempt < 2; ++attempt) {
      bool anyReset = false;
      for (const auto& sg : *shapedGlyphs) {
        if (outlineKey > 0) {
          impl_->ensureGlyph(sg.face, sg.glyphIndex, bucketPx, outlineKey, anyReset);
          if (anyReset) break;
        }
        impl_->ensureGlyph(sg.face, sg.glyphIndex, bucketPx, 0, anyReset);
        if (anyReset) break;
      }
      if (!anyReset) {
        break;
      }
    }

    if (!impl_->atlasTex) {
      return {};
    }

    auto emitGlyphQuads = [&](uint8_t glyphOutlineKey,
                              uint32_t abgrCol,
                              float shiftX,
                              float shiftY) {
      for (const auto& sg : *shapedGlyphs) {
        const GlyphKey gk{reinterpret_cast<uintptr_t>(sg.face), sg.glyphIndex, bucketPx, glyphOutlineKey};
        const auto it = impl_->glyphCache.find(gk);
        if (it == impl_->glyphCache.end() || !it->second.visible) {
          continue;
        }
        const auto& cg = it->second;
        const float gx = originScreenX + (sg.x + static_cast<float>(cg.offsetX)) * renderScale + shiftX;
        const float gy = originScreenY + (sg.y + static_cast<float>(cg.offsetY)) * renderScale + shiftY;
        const float gw = static_cast<float>(cg.width) * renderScale;
        const float gh = static_cast<float>(cg.height) * renderScale;

        const uint32_t base = static_cast<uint32_t>(outVertices.size());
        outVertices.push_back({gx, gy, 0.0f, abgrCol, cg.u0, cg.v0});
        outVertices.push_back({gx + gw, gy, 0.0f, abgrCol, cg.u1, cg.v0});
        outVertices.push_back({gx + gw, gy + gh, 0.0f, abgrCol, cg.u1, cg.v1});
        outVertices.push_back({gx, gy + gh, 0.0f, abgrCol, cg.u0, cg.v1});
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 3);
      }
    };

    const bool hasShadow =
        style.shadowColor.a > 0 &&
        (std::abs(style.shadowOffsetPx.x) > 0.01f || std::abs(style.shadowOffsetPx.y) > 0.01f);
    if (hasShadow) {
      emitGlyphQuads(outlineKey,
                     packPremultipliedAbgr(style.shadowColor),
                     style.shadowOffsetPx.x,
                     style.shadowOffsetPx.y);
    }
    if (outlineKey > 0 && style.outlineColor.a > 0) {
      emitGlyphQuads(outlineKey, packPremultipliedAbgr(style.outlineColor), 0.0f, 0.0f);
    }
    emitGlyphQuads(0, fillAbgr, 0.0f, 0.0f);
    return impl_->atlasTex;
  }
#elif defined(IOTA_HAS_FREETYPE_HARFBUZZ)
  impl_->initFreeType();
  if (impl_->ftReady && impl_->ftLib && impl_->fcConfig) {
    const float clampedSize = std::max(8.0f, fontSizePx);
    const uint16_t bucketPx = quantizeFontBucketPx(clampedSize);
    const float renderScale = clampedSize / static_cast<float>(bucketPx);
    const float safeScale = std::max(renderScale, 0.05f);

    const float layoutW = style.wordWrap
                              ? std::max(bounds.width() / safeScale, 16.0f)
                              : std::max(16384.0f, (bounds.width() / safeScale) * 2.0f);
    const float layoutWKey = std::round(layoutW * 4.0f) * 0.25f;
    const bool hasCharSpacing = std::abs(style.letterSpacingPx) > 1e-3f;
    const float letterSpacingDip = hasCharSpacing ? (style.letterSpacingPx / safeScale) : 0.0f;
    const bool hasLineSpacing = std::abs(style.lineSpacing - 1.0f) > 1e-3f;
    const float lineSpacingNorm = hasLineSpacing ? std::clamp(style.lineSpacing, 0.4f, 4.0f) : 1.0f;

    const ShapedLayoutKeyView lookupKey{
        typeFaceKey,
        text,
        reinterpret_cast<uintptr_t>(style.localAssets),
        impl_->customFontGen,
        bucketPx,
        style.weight,
        style.italic,
        style.alignment,
        style.wordWrap,
        layoutWKey,
        letterSpacingDip,
        lineSpacingNorm,
    };

    const std::vector<FtShapedGlyph>* shapedGlyphs = nullptr;
    const std::vector<std::shared_ptr<FtFaceEntry>>* heldFaces = nullptr;
    float usedLayoutW = layoutW;
    float metricsTop = 0.0f;
    float metricsHeight = 0.0f;

    if (const auto hit = impl_->shapedLayoutCache.find(lookupKey); hit != impl_->shapedLayoutCache.end()) {
      hit->second.lastUseStamp = ++impl_->shapedLruCounter;
      ++impl_->shapedLayoutHits;
      shapedGlyphs = &hit->second.glyphs;
      heldFaces = &hit->second.heldFaces;
      usedLayoutW = hit->second.layoutW;
      metricsTop = hit->second.metricsTop;
      metricsHeight = hit->second.metricsHeight;
    } else {
      const std::vector<uint32_t> codepoints = decodeUtf8Codepoints(text);
      if (codepoints.empty()) {
        return {};
      }

      const LinuxFamilySpec famSpec = resolveLinuxFamilySpec(typeFaceKey);
      const int fcWeight = toFontconfigWeight(style.weight, famSpec.defaultWeight);
      const bool wantBold = isBoldWeight(style.weight, famSpec.defaultWeight);
      const bool wantItalic = style.italic || famSpec.defaultItalic;

      std::shared_ptr<FtFaceEntry> primaryFace =
          impl_->resolveCustomFace(typeFaceKey, style, wantBold, wantItalic);
      if (!primaryFace) {
        primaryFace = impl_->resolveFontconfigFace(famSpec.families, fcWeight, wantBold, wantItalic, 0);
      }
      if (!primaryFace) {
        return impl_->buildFallbackQuads(text, fontSizePx, bounds, fillAbgr, outVertices, outIndices);
      }

      std::vector<std::shared_ptr<FtFaceEntry>> usedFaceHolders = {primaryFace};
      auto addHeldFace = [&](const std::shared_ptr<FtFaceEntry>& f) {
        if (!f) return;
        for (const auto& existing : usedFaceHolders) {
          if (existing.get() == f.get()) return;
        }
        usedFaceHolders.push_back(f);
      };

      auto resolveFaceForCodepoint = [&](uint32_t cp) -> std::shared_ptr<FtFaceEntry> {
        if (cp < 32u || cp == 0x20u || cp == 0xA0u) {
          return primaryFace;
        }
        for (const auto& candidate : usedFaceHolders) {
          if (candidate && candidate->ftFace && FT_Get_Char_Index(candidate->ftFace, cp) != 0) {
            return candidate;
          }
        }
        auto fallback = impl_->resolveFontconfigFace(famSpec.families, fcWeight, wantBold, wantItalic, cp);
        if (fallback) {
          addHeldFace(fallback);
          return fallback;
        }
        return primaryFace;
      };

      struct RawGlyph {
        FtFaceEntry* face = nullptr;
        uint32_t glyphIndex = 0;
        float relX = 0.0f;
        float relY = 0.0f;
      };

      auto shapeLineRange = [&](size_t startIdx, size_t endIdx, std::vector<RawGlyph>& outLineGlyphs) -> float {
        outLineGlyphs.clear();
        if (startIdx >= endIdx) {
          return 0.0f;
        }

        bool lineHasRtl = false;
        for (size_t k = startIdx; k < endIdx; ++k) {
          if (isRtlCodepoint(codepoints[k])) {
            lineHasRtl = true;
            break;
          }
        }

        struct Run {
          std::shared_ptr<FtFaceEntry> face;
          size_t begin = 0;
          size_t end = 0;
          bool rtl = false;
        };

        std::vector<Run> runs;
        size_t pos = startIdx;
        while (pos < endIdx) {
          auto face = resolveFaceForCodepoint(codepoints[pos]);
          const bool cpRtl = isRtlCodepoint(codepoints[pos]);
          size_t runEnd = pos + 1;
          while (runEnd < endIdx) {
            auto nextFace = resolveFaceForCodepoint(codepoints[runEnd]);
            const bool nextRtl = isRtlCodepoint(codepoints[runEnd]);
            if (nextFace.get() != face.get()) {
              break;
            }
            if (codepoints[runEnd] >= 0x40u && nextRtl != cpRtl) {
              break;
            }
            ++runEnd;
          }
          runs.push_back({std::move(face), pos, runEnd, cpRtl || lineHasRtl});
          pos = runEnd;
        }

        if (lineHasRtl && runs.size() > 1) {
          std::reverse(runs.begin(), runs.end());
        }

        hb_buffer_t* hbBuf = hb_buffer_create();
        float penX = 0.0f;
        size_t emittedGlyphCount = 0;
        for (const auto& run : runs) {
          if (!run.face || !run.face->hbFont) continue;
          run.face->ensurePixelSize(bucketPx);

          hb_buffer_reset(hbBuf);
          hb_buffer_add_utf32(hbBuf,
                              codepoints.data(),
                              static_cast<int>(codepoints.size()),
                              static_cast<unsigned int>(run.begin),
                              static_cast<int>(run.end - run.begin));
          hb_buffer_set_direction(hbBuf, run.rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
          hb_buffer_guess_segment_properties(hbBuf);
          hb_shape(run.face->hbFont, hbBuf, nullptr, 0);

          unsigned int glyphCount = 0;
          const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(hbBuf, &glyphCount);
          const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(hbBuf, &glyphCount);

          for (unsigned int gi = 0; gi < glyphCount; ++gi) {
            if (emittedGlyphCount > 0 && hasCharSpacing) {
              penX += letterSpacingDip;
            }
            const float gx = penX + static_cast<float>(positions[gi].x_offset) / 64.0f;
            const float gy = -static_cast<float>(positions[gi].y_offset) / 64.0f;
            outLineGlyphs.push_back({run.face.get(), infos[gi].codepoint, gx, gy});
            penX += static_cast<float>(positions[gi].x_advance) / 64.0f;
            ++emittedGlyphCount;
          }
        }
        hb_buffer_destroy(hbBuf);
        return penX;
      };

      // Split into paragraphs on '\n', wrapping words when requested.
      struct LineSlice {
        size_t begin = 0;
        size_t end = 0;
      };
      std::vector<LineSlice> lines;
      {
        size_t pStart = 0;
        while (pStart <= codepoints.size()) {
          size_t nl = pStart;
          while (nl < codepoints.size() && codepoints[nl] != '\n') {
            ++nl;
          }

          if (!style.wordWrap || nl == pStart) {
            lines.push_back({pStart, nl});
          } else {
            std::vector<RawGlyph> measureBuf;
            size_t curStart = pStart;
            while (curStart < nl) {
              size_t bestEnd = curStart;
              size_t lastSpace = curStart;
              for (size_t probe = curStart + 1; probe <= nl; ++probe) {
                const bool isSep = (probe == nl || codepoints[probe - 1] == ' ');
                if (isSep || probe == curStart + 1) {
                  const float w = shapeLineRange(curStart, probe, measureBuf);
                  if (w <= layoutW || probe == curStart + 1) {
                    bestEnd = probe;
                    if (probe < nl && codepoints[probe - 1] == ' ') {
                      lastSpace = probe;
                    }
                  } else {
                    break;
                  }
                }
              }
              if (bestEnd < nl && lastSpace > curStart) {
                bestEnd = lastSpace;
              }
              while (bestEnd > curStart && codepoints[bestEnd - 1] == ' ') {
                --bestEnd;
              }
              lines.push_back({curStart, bestEnd});
              curStart = (bestEnd == curStart) ? (curStart + 1) : bestEnd;
              while (curStart < nl && codepoints[curStart] == ' ') {
                ++curStart;
              }
            }
          }

          if (nl >= codepoints.size()) {
            break;
          }
          pStart = nl + 1;
        }
      }

      primaryFace->ensurePixelSize(bucketPx);
      const float ascender = (primaryFace->ftFace && primaryFace->ftFace->size)
                                 ? static_cast<float>(primaryFace->ftFace->size->metrics.ascender) / 64.0f
                                 : static_cast<float>(bucketPx) * 0.8f;
      const float descender = (primaryFace->ftFace && primaryFace->ftFace->size)
                                  ? static_cast<float>(-primaryFace->ftFace->size->metrics.descender) / 64.0f
                                  : static_cast<float>(bucketPx) * 0.2f;
      const float naturalHeight = (primaryFace->ftFace && primaryFace->ftFace->size)
                                      ? static_cast<float>(primaryFace->ftFace->size->metrics.height) / 64.0f
                                      : static_cast<float>(bucketPx) * 1.15f;
      const float lineAdvance = hasLineSpacing
                                    ? (static_cast<float>(bucketPx) * 1.2f * lineSpacingNorm)
                                    : std::max(static_cast<float>(bucketPx) * 1.15f, naturalHeight);

      impl_->scratchShaped.clear();
      std::vector<RawGlyph> lineGlyphs;
      float baselineY = ascender;
      for (const auto& ln : lines) {
        const float lineWidth = shapeLineRange(ln.begin, ln.end, lineGlyphs);
        float lineOriginX = (layoutW - lineWidth) * 0.5f;
        if (style.alignment == TextAlignment::Left) {
          lineOriginX = 0.0f;
        } else if (style.alignment == TextAlignment::Right) {
          lineOriginX = layoutW - lineWidth;
        }
        for (const auto& rg : lineGlyphs) {
          impl_->scratchShaped.push_back({rg.face, rg.glyphIndex, lineOriginX + rg.relX, baselineY + rg.relY});
        }
        baselineY += lineAdvance;
      }

      metricsTop = 0.0f;
      metricsHeight = std::max(
          static_cast<float>(bucketPx),
          static_cast<float>(std::max<size_t>(1, lines.size()) - 1) * lineAdvance + ascender + std::max(0.0f, descender));

      if (impl_->shapedLayoutCache.size() >= Impl::kMaxShapedLayouts) {
        auto oldest = impl_->shapedLayoutCache.begin();
        for (auto it = std::next(oldest); it != impl_->shapedLayoutCache.end(); ++it) {
          if (it->second.lastUseStamp < oldest->second.lastUseStamp) {
            oldest = it;
          }
        }
        impl_->shapedLayoutCache.erase(oldest);
      }

      ShapedLayoutKey ownedKey;
      ownedKey.typeFaceKey.assign(typeFaceKey);
      ownedKey.text.assign(text);
      ownedKey.localAssetsPtr = lookupKey.localAssetsPtr;
      ownedKey.customFontGen = lookupKey.customFontGen;
      ownedKey.bucketPx = bucketPx;
      ownedKey.weight = style.weight;
      ownedKey.italic = style.italic;
      ownedKey.alignment = style.alignment;
      ownedKey.wordWrap = style.wordWrap;
      ownedKey.layoutW = layoutWKey;
      ownedKey.letterSpacingDip = letterSpacingDip;
      ownedKey.lineSpacingNorm = lineSpacingNorm;

      FtShapedLayoutEntry newEntry;
      newEntry.layoutW = layoutW;
      newEntry.metricsTop = metricsTop;
      newEntry.metricsHeight = metricsHeight;
      newEntry.lastUseStamp = ++impl_->shapedLruCounter;
      newEntry.glyphs = impl_->scratchShaped;
      newEntry.heldFaces = std::move(usedFaceHolders);

      auto [insertedIt, inserted] = impl_->shapedLayoutCache.emplace(std::move(ownedKey), std::move(newEntry));
      (void)inserted;
      shapedGlyphs = &insertedIt->second.glyphs;
      heldFaces = &insertedIt->second.heldFaces;
    }

    auto findFaceHolder = [&](FtFaceEntry* rawPtr) -> std::shared_ptr<FtFaceEntry> {
      if (!heldFaces || !rawPtr) return nullptr;
      for (const auto& h : *heldFaces) {
        if (h.get() == rawPtr) return h;
      }
      return nullptr;
    };

    float originScreenX = bounds.centerX() - (usedLayoutW * 0.5f) * renderScale;
    if (style.alignment == TextAlignment::Left) {
      originScreenX = bounds.left;
    } else if (style.alignment == TextAlignment::Right) {
      originScreenX = bounds.right - usedLayoutW * renderScale;
    }
    const float originScreenY = bounds.centerY() - (metricsTop + metricsHeight * 0.5f) * renderScale;

    const int outlineRad = (style.outlineWidthPx > 0.05f)
                               ? std::clamp(static_cast<int>(std::lround(style.outlineWidthPx / safeScale)), 1, 10)
                               : 0;
    const uint8_t outlineKey = static_cast<uint8_t>(outlineRad);

    for (int attempt = 0; attempt < 2; ++attempt) {
      bool anyReset = false;
      for (const auto& sg : *shapedGlyphs) {
        const auto holder = findFaceHolder(sg.face);
        if (!holder) continue;
        if (outlineKey > 0) {
          impl_->ensureGlyphFt(holder, sg.glyphIndex, bucketPx, outlineKey, anyReset);
          if (anyReset) break;
        }
        impl_->ensureGlyphFt(holder, sg.glyphIndex, bucketPx, 0, anyReset);
        if (anyReset) break;
      }
      if (!anyReset) {
        break;
      }
    }

    if (!impl_->atlasTex) {
      return {};
    }

    auto emitGlyphQuads = [&](uint8_t glyphOutlineKey,
                              uint32_t abgrCol,
                              float shiftX,
                              float shiftY) {
      for (const auto& sg : *shapedGlyphs) {
        if (!sg.face || !sg.face->ftFace) continue;
        const GlyphKey gk{reinterpret_cast<uintptr_t>(sg.face->ftFace), sg.glyphIndex, bucketPx, glyphOutlineKey};
        const auto it = impl_->glyphCache.find(gk);
        if (it == impl_->glyphCache.end() || !it->second.visible) {
          continue;
        }
        const auto& cg = it->second;
        const float gx = originScreenX + (sg.x + static_cast<float>(cg.offsetX)) * renderScale + shiftX;
        const float gy = originScreenY + (sg.y + static_cast<float>(cg.offsetY)) * renderScale + shiftY;
        const float gw = static_cast<float>(cg.width) * renderScale;
        const float gh = static_cast<float>(cg.height) * renderScale;

        const uint32_t base = static_cast<uint32_t>(outVertices.size());
        outVertices.push_back({gx, gy, 0.0f, abgrCol, cg.u0, cg.v0});
        outVertices.push_back({gx + gw, gy, 0.0f, abgrCol, cg.u1, cg.v0});
        outVertices.push_back({gx + gw, gy + gh, 0.0f, abgrCol, cg.u1, cg.v1});
        outVertices.push_back({gx, gy + gh, 0.0f, abgrCol, cg.u0, cg.v1});
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 3);
      }
    };

    const bool hasShadow =
        style.shadowColor.a > 0 &&
        (std::abs(style.shadowOffsetPx.x) > 0.01f || std::abs(style.shadowOffsetPx.y) > 0.01f);
    if (hasShadow) {
      emitGlyphQuads(outlineKey,
                     packPremultipliedAbgr(style.shadowColor),
                     style.shadowOffsetPx.x,
                     style.shadowOffsetPx.y);
    }
    if (outlineKey > 0 && style.outlineColor.a > 0) {
      emitGlyphQuads(outlineKey, packPremultipliedAbgr(style.outlineColor), 0.0f, 0.0f);
    }
    emitGlyphQuads(0, fillAbgr, 0.0f, 0.0f);
    return impl_->atlasTex;
  }
#else
  if (auto faceHolder = impl_->resolveStbFace(typeFaceKey, style)) {
    const float clampedSize = std::max(8.0f, fontSizePx);
    const uint16_t bucketPx = quantizeFontBucketPx(clampedSize);
    const float renderScale = clampedSize / static_cast<float>(bucketPx);
    const float safeScale = std::max(renderScale, 0.05f);
    const float pxScale = stbtt_ScaleForPixelHeight(&faceHolder->info, static_cast<float>(bucketPx));
    const float letterSpacingBucket = style.letterSpacingPx / safeScale;
    const float lineSpacingNorm = std::clamp(style.lineSpacing, 0.4f, 4.0f);

    std::vector<uint32_t> codepoints;
    codepoints.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
      const unsigned char c0 = static_cast<unsigned char>(text[i]);
      uint32_t cp = '?';
      size_t adv = 1;
      if (c0 < 0x80) {
        cp = c0;
      } else if ((c0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
        cp = ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
        adv = 2;
      } else if ((c0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
        cp = ((c0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
             (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
        adv = 3;
      } else if ((c0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
        cp = ((c0 & 0x07u) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12) |
             ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6) |
             (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
        adv = 4;
      }
      codepoints.push_back(cp);
      i += adv;
    }

    const float layoutW = style.wordWrap ? std::max(bounds.width() / safeScale, 16.0f) : 16384.0f;
    struct StbRawGlyph {
      int glyphIdx = 0;
      float x = 0.0f;
      float y = 0.0f;
    };
    auto measureLine = [&](size_t start, size_t end, std::vector<int>& outIndices, std::vector<float>& outX) -> float {
      outIndices.clear();
      outX.clear();
      float penX = 0.0f;
      int prevGlyph = 0;
      for (size_t idx = start; idx < end; ++idx) {
        const uint32_t cp = codepoints[idx];
        const int gi = stbtt_FindGlyphIndex(&faceHolder->info, static_cast<int>(cp));
        if (prevGlyph != 0 && gi != 0) {
          penX += static_cast<float>(stbtt_GetGlyphKernAdvance(&faceHolder->info, prevGlyph, gi)) * pxScale;
        }
        outIndices.push_back(gi);
        outX.push_back(penX);
        int advW = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&faceHolder->info, gi, &advW, &lsb);
        penX += static_cast<float>(advW) * pxScale + letterSpacingBucket;
        prevGlyph = gi;
      }
      return penX;
    };

    struct LineSpan { size_t begin = 0; size_t end = 0; };
    std::vector<LineSpan> lines;
    std::vector<int> tmpIdx;
    std::vector<float> tmpX;
    for (size_t pStart = 0; pStart <= codepoints.size();) {
      size_t nl = pStart;
      while (nl < codepoints.size() && codepoints[nl] != '\n') ++nl;
      if (!style.wordWrap || nl == pStart) {
        lines.push_back({pStart, nl});
      } else {
        size_t curStart = pStart;
        while (curStart < nl) {
          size_t bestEnd = curStart + 1;
          size_t lastSpace = curStart;
          for (size_t probe = curStart + 1; probe <= nl; ++probe) {
            const bool isSep = (probe == nl || codepoints[probe - 1] == ' ' || codepoints[probe - 1] == '\t');
            if (isSep || probe == curStart + 1) {
              const float w = measureLine(curStart, probe, tmpIdx, tmpX);
              if (w <= layoutW || probe == curStart + 1) {
                bestEnd = probe;
                if (probe < nl && codepoints[probe - 1] == ' ') lastSpace = probe;
              } else {
                break;
              }
            }
          }
          if (bestEnd < nl && lastSpace > curStart) bestEnd = lastSpace;
          while (bestEnd > curStart && codepoints[bestEnd - 1] == ' ') --bestEnd;
          lines.push_back({curStart, bestEnd});
          curStart = (bestEnd == curStart) ? (curStart + 1) : bestEnd;
          while (curStart < nl && codepoints[curStart] == ' ') ++curStart;
        }
      }
      if (nl >= codepoints.size()) break;
      pStart = nl + 1;
    }

    const float ascender = static_cast<float>(faceHolder->ascent) * pxScale;
    const float descender = static_cast<float>(-faceHolder->descent) * pxScale;
    const float lineAdvance = std::max(
        static_cast<float>(bucketPx) * 1.15f,
        (static_cast<float>(faceHolder->ascent - faceHolder->descent + faceHolder->lineGap) * pxScale)) *
        lineSpacingNorm;
    const float metricsHeight = std::max(
        static_cast<float>(bucketPx),
        static_cast<float>(std::max<size_t>(1, lines.size()) - 1) * lineAdvance + ascender + std::max(0.0f, descender));

    std::vector<StbRawGlyph> shaped;
    float maxLineWidth = 0.0f;
    std::vector<float> lineWidths;
    lineWidths.reserve(lines.size());
    float baselineY = ascender;
    for (const auto& ln : lines) {
      const float lw = measureLine(ln.begin, ln.end, tmpIdx, tmpX);
      maxLineWidth = std::max(maxLineWidth, lw);
      lineWidths.push_back(lw);
    }
    const float boxW = style.wordWrap ? layoutW : maxLineWidth;
    for (size_t li = 0; li < lines.size(); ++li) {
      const float lw = measureLine(lines[li].begin, lines[li].end, tmpIdx, tmpX);
      float originX = (boxW - lw) * 0.5f;
      if (style.alignment == TextAlignment::Left) originX = 0.0f;
      else if (style.alignment == TextAlignment::Right) originX = boxW - lw;
      for (size_t gi = 0; gi < tmpIdx.size(); ++gi) {
        shaped.push_back({tmpIdx[gi], originX + tmpX[gi], baselineY});
      }
      baselineY += lineAdvance;
    }

    float originScreenX = bounds.centerX() - (boxW * 0.5f) * renderScale;
    if (style.alignment == TextAlignment::Left) originScreenX = bounds.left;
    else if (style.alignment == TextAlignment::Right) originScreenX = bounds.right - boxW * renderScale;
    const float originScreenY = bounds.centerY() - (metricsHeight * 0.5f) * renderScale;

    const int outlineRad = (style.outlineWidthPx > 0.05f)
                               ? std::clamp(static_cast<int>(std::lround(style.outlineWidthPx / safeScale)), 1, 10)
                               : 0;
    const uint8_t outlineKey = static_cast<uint8_t>(outlineRad);

    for (int attempt = 0; attempt < 2; ++attempt) {
      bool anyReset = false;
      for (const auto& sg : shaped) {
        if (outlineKey > 0) {
          impl_->ensureGlyphStb(faceHolder, sg.glyphIdx, bucketPx, outlineKey, anyReset);
          if (anyReset) break;
        }
        impl_->ensureGlyphStb(faceHolder, sg.glyphIdx, bucketPx, 0, anyReset);
        if (anyReset) break;
      }
      if (!anyReset) break;
    }

    if (!impl_->atlasTex) return {};

    const uintptr_t faceKey = reinterpret_cast<uintptr_t>(faceHolder.get());
    auto emitGlyphQuads = [&](uint8_t glyphOutlineKey, uint32_t abgrCol, float shiftX, float shiftY) {
      for (const auto& sg : shaped) {
        const GlyphKey gk{faceKey, static_cast<uint32_t>(sg.glyphIdx), bucketPx, glyphOutlineKey};
        const auto it = impl_->glyphCache.find(gk);
        if (it == impl_->glyphCache.end() || !it->second.visible) continue;
        const auto& cg = it->second;
        const float gx = originScreenX + (sg.x + static_cast<float>(cg.offsetX)) * renderScale + shiftX;
        const float gy = originScreenY + (sg.y + static_cast<float>(cg.offsetY)) * renderScale + shiftY;
        const float gw = static_cast<float>(cg.width) * renderScale;
        const float gh = static_cast<float>(cg.height) * renderScale;

        const uint32_t base = static_cast<uint32_t>(outVertices.size());
        outVertices.push_back({gx, gy, 0.0f, abgrCol, cg.u0, cg.v0});
        outVertices.push_back({gx + gw, gy, 0.0f, abgrCol, cg.u1, cg.v0});
        outVertices.push_back({gx + gw, gy + gh, 0.0f, abgrCol, cg.u1, cg.v1});
        outVertices.push_back({gx, gy + gh, 0.0f, abgrCol, cg.u0, cg.v1});
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 3);
      }
    };

    const bool hasShadow =
        style.shadowColor.a > 0 &&
        (std::abs(style.shadowOffsetPx.x) > 0.01f || std::abs(style.shadowOffsetPx.y) > 0.01f);
    if (hasShadow) {
      emitGlyphQuads(outlineKey, packPremultipliedAbgr(style.shadowColor), style.shadowOffsetPx.x, style.shadowOffsetPx.y);
    }
    if (outlineKey > 0 && style.outlineColor.a > 0) {
      emitGlyphQuads(outlineKey, packPremultipliedAbgr(style.outlineColor), 0.0f, 0.0f);
    }
    emitGlyphQuads(0, fillAbgr, 0.0f, 0.0f);
    return impl_->atlasTex;
  }
#endif

  return impl_->buildFallbackQuads(text, fontSizePx, bounds, fillAbgr, outVertices, outIndices);
}

}  // namespace iota::engine
