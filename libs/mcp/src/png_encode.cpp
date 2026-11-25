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

#include "iota/mcp/png_encode.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace iota::mcp {

namespace {

void pngWriteCallback(void* context, void* data, int size) {
  if (context == nullptr || data == nullptr || size <= 0) {
    return;
  }
  auto* out = static_cast<std::vector<uint8_t>*>(context);
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

std::vector<uint8_t> encodeRgbaToPng(int width,
                                     int height,
                                     std::span<const uint8_t> rgba) {
  if (width <= 0 || height <= 0) {
    return {};
  }
  const size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  if (rgba.size() != expectedSize) {
    return {};
  }

  std::vector<uint8_t> out;
  out.reserve(expectedSize / 2u + 256u);
  const int ok = stbi_write_png_to_func(
      &pngWriteCallback, &out, width, height, 4, rgba.data(), width * 4);
  if (ok == 0) {
    out.clear();
  }
  return out;
}

std::string base64Encode(std::span<const uint8_t> bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve(((bytes.size() + 2u) / 3u) * 4u);

  size_t i = 0;
  while (i + 2u < bytes.size()) {
    const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16) |
                            (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                            static_cast<uint32_t>(bytes[i + 2]);
    out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
    out.push_back(kAlphabet[(triple >> 6) & 0x3Fu]);
    out.push_back(kAlphabet[triple & 0x3Fu]);
    i += 3u;
  }

  const size_t rem = bytes.size() - i;
  if (rem == 1u) {
    const uint32_t triple = static_cast<uint32_t>(bytes[i]) << 16;
    out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2u) {
    const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16) |
                            (static_cast<uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
    out.push_back(kAlphabet[(triple >> 6) & 0x3Fu]);
    out.push_back('=');
  }

  return out;
}

}  // namespace iota::mcp
