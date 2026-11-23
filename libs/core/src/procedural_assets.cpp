#include "iota/core/procedural_assets.hpp"

#include <algorithm>
#include <cmath>

namespace iota::core {

namespace {

constexpr float kPi = 3.14159265358979323846f;

RgbaImage makeSolid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  RgbaImage img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
  for (size_t i = 0; i < img.pixels.size(); i += 4) {
    img.pixels[i + 0] = r;
    img.pixels[i + 1] = g;
    img.pixels[i + 2] = b;
    img.pixels[i + 3] = a;
  }
  return img;
}

void setPixel(RgbaImage& img, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  const size_t idx = (static_cast<size_t>(y) * img.width + x) * 4;
  img.pixels[idx + 0] = r;
  img.pixels[idx + 1] = g;
  img.pixels[idx + 2] = b;
  img.pixels[idx + 3] = a;
}

RgbaImage makeRadialGlow(int size, float power, bool inverted, bool whiteRgb) {
  RgbaImage img{size, size, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4)};
  const float center = 0.5f * (size - 1);
  const float invR = 1.0f / (0.5f * size);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float dx = (x - center) * invR;
      const float dy = (y - center) * invR;
      const float dist = std::clamp(std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
      float w = std::pow(1.0f - dist, power);
      if (inverted) w = 1.0f - w;
      const auto val = static_cast<uint8_t>(std::lround(std::clamp(w, 0.0f, 1.0f) * 255.0f));
      if (whiteRgb) {
        setPixel(img, x, y, 255, 255, 255, val);
      } else {
        setPixel(img, x, y, val, val, val, val);
      }
    }
  }
  return img;
}

RgbaImage makeCircle(int size, float edgeSoftness) {
  RgbaImage img{size, size, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4)};
  const float center = 0.5f * (size - 1);
  const float invR = 1.0f / (0.48f * size);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float dx = (x - center) * invR;
      const float dy = (y - center) * invR;
      const float dist = std::sqrt(dx * dx + dy * dy);
      const float t = std::clamp((1.0f - dist) / std::max(edgeSoftness, 1e-4f), 0.0f, 1.0f);
      const float alpha = t * t * (3.0f - 2.0f * t);
      const auto a = static_cast<uint8_t>(std::lround(alpha * 255.0f));
      setPixel(img, x, y, 255, 255, 255, a);
    }
  }
  return img;
}

RgbaImage makeVignette(int size) {
  RgbaImage img{size, size, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4)};
  const float center = 0.5f * (size - 1);
  const float invR = 1.0f / (0.5f * size * 1.15f);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float dx = (x - center) * invR;
      const float dy = (y - center) * invR;
      const float r2 = std::min(1.0f, dx * dx + dy * dy);
      const float keep = 1.0f - 0.8f * std::pow(r2, 1.5f);
      const auto v = static_cast<uint8_t>(std::lround(std::clamp(keep, 0.0f, 1.0f) * 255.0f));
      setPixel(img, x, y, v, v, v, v);
    }
  }
  return img;
}

RgbaImage makeRainbowLut(int w, int h) {
  RgbaImage img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
  for (int x = 0; x < w; ++x) {
    const float t = static_cast<float>(x) / static_cast<float>(std::max(1, w - 1));
    const float r = 0.5f + 0.5f * std::cos(2.0f * kPi * (t + 0.0f));
    const float g = 0.5f + 0.5f * std::cos(2.0f * kPi * (t - 1.0f / 3.0f));
    const float b = 0.5f + 0.5f * std::cos(2.0f * kPi * (t - 2.0f / 3.0f));
    const auto rb = static_cast<uint8_t>(std::lround(r * 255.0f));
    const auto gb = static_cast<uint8_t>(std::lround(g * 255.0f));
    const auto bb = static_cast<uint8_t>(std::lround(b * 255.0f));
    for (int y = 0; y < h; ++y) {
      setPixel(img, x, y, rb, gb, bb, 255);
    }
  }
  return img;
}

RgbaImage makeLensFlare(int size, bool warmTint) {
  RgbaImage img{size, size, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4)};
  const float center = 0.5f * (size - 1);
  const float invR = 1.0f / (0.5f * size);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float dx = (x - center) * invR;
      const float dy = (y - center) * invR;
      const float r = std::sqrt(dx * dx + dy * dy);
      const float core = std::exp(-r * 5.0f);
      const float streak = std::exp(-std::abs(dy) * 28.0f) * std::exp(-std::abs(dx) * 3.0f);
      const float ring = std::exp(-std::pow((r - 0.58f) * 14.0f, 2.0f)) * 0.45f;
      const float intensity = std::clamp(core + 0.85f * streak + ring, 0.0f, 1.0f);
      const float rf = intensity * (warmTint ? 1.0f : 0.85f);
      const float gf = intensity * (warmTint ? 0.90f : 0.95f);
      const float bf = intensity * (warmTint ? 0.72f : 1.0f);
      setPixel(img, x, y,
               static_cast<uint8_t>(std::lround(rf * 255.0f)),
               static_cast<uint8_t>(std::lround(gf * 255.0f)),
               static_cast<uint8_t>(std::lround(bf * 255.0f)),
               static_cast<uint8_t>(std::lround(intensity * 255.0f)));
    }
  }
  return img;
}

RgbaImage makeSnowflake(int size) {
  RgbaImage img{size, size, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4)};
  const float center = 0.5f * (size - 1);
  const float invR = 1.0f / (0.5f * size);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float dx = (x - center) * invR;
      const float dy = (y - center) * invR;
      const float r = std::sqrt(dx * dx + dy * dy);
      const float ang = std::atan2(dy, dx);
      const float rays = std::pow(std::abs(std::cos(3.0f * ang)), 8.0f);
      const float falloff = std::clamp(1.0f - r, 0.0f, 1.0f);
      const float w = std::clamp(falloff * (0.35f + 0.85f * rays), 0.0f, 1.0f);
      const auto v = static_cast<uint8_t>(std::lround(w * 255.0f));
      setPixel(img, x, y, 255, 255, 255, v);
    }
  }
  return img;
}

RgbaImage makePlaceholderArt(int size) {
  RgbaImage img{size, size, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4)};
  const float center = 0.5f * (size - 1);
  const float invR = 1.0f / (0.5f * size);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float u = static_cast<float>(x) / size;
      const float v = static_cast<float>(y) / size;
      const float dx = (x - center) * invR;
      const float dy = (y - center) * invR;
      const float r = std::sqrt(dx * dx + dy * dy);
      const float disc = (r < 0.72f && r > 0.14f) ? 0.15f : 0.0f;
      const float grooves = (r < 0.70f && r > 0.18f) ? 0.04f * std::sin(r * 90.0f) : 0.0f;
      const float baseR = 0.09f + 0.14f * u + disc + grooves;
      const float baseG = 0.11f + 0.16f * v + disc + grooves;
      const float baseB = 0.18f + 0.24f * (1.0f - u) + disc + grooves;
      setPixel(img, x, y,
               static_cast<uint8_t>(std::lround(std::clamp(baseR, 0.0f, 1.0f) * 255.0f)),
               static_cast<uint8_t>(std::lround(std::clamp(baseG, 0.0f, 1.0f) * 255.0f)),
               static_cast<uint8_t>(std::lround(std::clamp(baseB, 0.0f, 1.0f) * 255.0f)),
               255);
    }
  }
  return img;
}

} // namespace

std::optional<RgbaImage> generateInternalResource(std::string_view uri) {
  constexpr std::string_view kPrefix = "internalres:";
  if (uri.starts_with(kPrefix)) {
    uri.remove_prefix(kPrefix.size());
  }

  if (uri == "white") return makeSolid(4, 4, 255, 255, 255, 255);
  if (uri == "black") return makeSolid(4, 4, 0, 0, 0, 255);
  if (uri == "transparent") return makeSolid(4, 4, 0, 0, 0, 0);
  if (uri == "particle_blur01") return makeRadialGlow(64, 2.2f, false, true);
  if (uri == "particle_blur01_more") return makeRadialGlow(64, 3.4f, false, true);
  if (uri == "particle_blur_inv") return makeRadialGlow(64, 2.0f, true, true);
  if (uri == "particle_circle_blur4") return makeCircle(64, 0.22f);
  if (uri == "particle_circle_w_a_64") return makeCircle(64, 0.08f);
  if (uri == "particle_sharp") return makeCircle(64, 0.02f);
  if (uri == "vignette80") return makeVignette(128);
  if (uri == "rainbow128") return makeRainbowLut(128, 8);
  if (uri == "lens_flare") return makeLensFlare(128, true);
  if (uri == "lens_flare_2") return makeLensFlare(128, false);
  if (uri == "snowflake") return makeSnowflake(64);
  if (uri == "placeholderart5") return makePlaceholderArt(256);
  if (uri == "anim128_g_m10_15" || uri == "year2017" || uri == "year2018") {
    return makeCircle(64, 0.15f);
  }
  return std::nullopt;
}

} // namespace iota::core
