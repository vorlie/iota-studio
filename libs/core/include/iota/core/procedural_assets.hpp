#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace iota::core {

struct RgbaImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels; // RGBA8 row-major
};

// Clean-room procedural textures for built-in internalres: identifiers.
std::optional<RgbaImage> generateInternalResource(std::string_view uri);

/// Returns all built-in `internalres:*` texture identifiers supported by generateInternalResource.
const std::vector<std::string_view>& listInternalResources() noexcept;

} // namespace iota::core
