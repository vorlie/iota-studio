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
