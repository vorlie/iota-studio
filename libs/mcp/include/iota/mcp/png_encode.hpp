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
