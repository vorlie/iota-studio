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
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iota::core {

struct ZipEntryData {
  std::string name;
  std::vector<uint8_t> bytes;
};

bool isZipSignature(std::span<const uint8_t> data) noexcept;

std::optional<std::vector<ZipEntryData>> readZipFromMemory(
    std::span<const uint8_t> data,
    std::string* outError = nullptr);

std::optional<std::vector<ZipEntryData>> readZipFromFile(
    const std::filesystem::path& path,
    std::string* outError = nullptr);

bool writeZipToMemory(
    const std::vector<ZipEntryData>& entries,
    std::vector<uint8_t>& outBytes,
    std::string* outError = nullptr);

bool writeZipToFile(
    const std::filesystem::path& path,
    const std::vector<ZipEntryData>& entries,
    std::string* outError = nullptr);

} // namespace iota::core
