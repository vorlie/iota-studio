#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace iota::core {

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;

  constexpr Vec2 operator+(const Vec2& rhs) const noexcept { return {x + rhs.x, y + rhs.y}; }
  constexpr Vec2 operator-(const Vec2& rhs) const noexcept { return {x - rhs.x, y - rhs.y}; }
  constexpr Vec2 operator*(float s) const noexcept { return {x * s, y * s}; }
};

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  constexpr Vec3 operator+(const Vec3& rhs) const noexcept { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
  constexpr Vec3 operator-(const Vec3& rhs) const noexcept { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
  constexpr Vec3 operator*(float s) const noexcept { return {x * s, y * s, z * s}; }
};

struct RectF {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;

  constexpr float width() const noexcept { return right - left; }
  constexpr float height() const noexcept { return bottom - top; }
  constexpr float centerX() const noexcept { return 0.5f * (left + right); }
  constexpr float centerY() const noexcept { return 0.5f * (top + bottom); }
};

// Compositing blend modes mapping cleanly to OpenGL and Vulkan blend states.
enum class BlendMode : int {
  Alpha = 0,       // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
  Screen = 1,      // GL_ONE, GL_ONE_MINUS_SRC_COLOR
  Add = 2,         // GL_ONE, GL_ONE
  Opaque = 3,      // GL_ONE, GL_ZERO
  PreMulAlpha = 4, // GL_ONE, GL_ONE_MINUS_SRC_ALPHA
  AddAlpha = 5,    // GL_SRC_ALPHA, GL_ONE
  Inverse = 6      // GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR
};

BlendMode blendModeFromString(std::string_view name, BlendMode fallback = BlendMode::Alpha) noexcept;
std::string_view blendModeToString(BlendMode mode) noexcept;

struct ColorRgba {
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
  uint8_t a = 255;

  static constexpr ColorRgba fromArgbInt32(int32_t argb) noexcept {
    const auto u = static_cast<uint32_t>(argb);
    return {
        static_cast<uint8_t>((u >> 16) & 0xFFu),
        static_cast<uint8_t>((u >> 8) & 0xFFu),
        static_cast<uint8_t>(u & 0xFFu),
        static_cast<uint8_t>((u >> 24) & 0xFFu),
    };
  }

  constexpr int32_t toArgbInt32() const noexcept {
    const uint32_t u = (static_cast<uint32_t>(a) << 24) |
                       (static_cast<uint32_t>(r) << 16) |
                       (static_cast<uint32_t>(g) << 8) |
                       static_cast<uint32_t>(b);
    return static_cast<int32_t>(u);
  }

  // Packed RGBA8 little-endian memory order (byte0=R, byte1=G, byte2=B, byte3=A)
  constexpr uint32_t toPackedAbgrU32() const noexcept {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(r);
  }

  void toFloat4(float out[4]) const noexcept;
  static ColorRgba fromFloat4(const float rgba[4]) noexcept;
  static ColorRgba lerp(ColorRgba c0, ColorRgba c1, float t) noexcept;
};

// Normalized 'chsla4f' wire representation: h = H / 360.0f, s, l, a in [0, 1].
struct ColorHslaNorm {
  float h = 0.0f;
  float s = 0.0f;
  float l = 1.0f;
  float a = 1.0f;

  ColorRgba toRgba() const noexcept;
  static ColorHslaNorm fromRgba(ColorRgba c) noexcept;
  static ColorHslaNorm lerp(ColorHslaNorm c0, ColorHslaNorm c1, float t) noexcept;
};

std::string_view trim(std::string_view sv) noexcept;
std::vector<std::string_view> splitWhitespace(std::string_view sv);
std::vector<std::string_view> splitChar(std::string_view sv, char sep);

std::optional<Vec2> parseF2(std::string_view sv) noexcept;
std::string formatF2(Vec2 v);

std::optional<ColorHslaNorm> parseHsla4f(std::string_view sv) noexcept;
std::string formatHsla4f(ColorHslaNorm c);

} // namespace iota::core
