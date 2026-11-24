#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iota::mcp {

/// Encodes tightly packed RGBA8 pixel rows into an in-memory PNG byte buffer.
[[nodiscard]] std::vector<uint8_t> encodeRgbaToPng(int width,
                                                   int height,
                                                   std::span<const uint8_t> rgba);

/// Encodes arbitrary binary bytes into standard RFC 4648 base64 text.
[[nodiscard]] std::string base64Encode(std::span<const uint8_t> bytes);

}  // namespace iota::mcp
