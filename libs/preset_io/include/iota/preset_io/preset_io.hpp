#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

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

} // namespace iota::preset_io
