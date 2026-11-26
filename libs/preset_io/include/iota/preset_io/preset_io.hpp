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
#include <string_view>

#include "iota/model/hierarchy_ops.hpp"
#include "iota/model/scene.hpp"

namespace iota::preset_io {

// Loads .viz / .zip PKZIP archive or plain .json / .txt visualizer preset (ver "10").
std::optional<iota::model::Project> loadProjectFromMemory(
    std::span<const uint8_t> bytes,
    std::string* outError = nullptr);

std::optional<iota::model::Project> loadProjectFromFile(
    const std::filesystem::path& path,
    std::string* outError = nullptr);

std::optional<iota::model::Project> parseProjectJsonString(
    std::string_view jsonText,
    std::string* outError = nullptr);

std::string serializeProjectToJsonString(
    const iota::model::Project& project,
    int indent = 2);

bool saveProjectAsJson(
    const iota::model::Project& project,
    const std::filesystem::path& path,
    std::string* outError = nullptr);

bool saveProjectAsViz(
    const iota::model::Project& project,
    const std::filesystem::path& path,
    std::string* outError = nullptr);

std::string serializeHierarchyClipboardToJson(const iota::model::HierarchyClipboard& clip);
std::optional<iota::model::HierarchyClipboard> parseHierarchyClipboardFromJson(
    std::string_view jsonText);

/// Converts an audio track path inside the project directory into a portable relative POSIX path.
std::string makePortableAudioPath(
    std::string_view audioPath,
    const std::filesystem::path& projectPath);

/// Resolves an external audio path saved with a project (direct, relative to project dir, or sibling filename).
std::optional<std::filesystem::path> resolvePortableAudioPath(
    std::string_view storedAudioPath,
    const std::filesystem::path& projectPath);

} // namespace iota::preset_io
