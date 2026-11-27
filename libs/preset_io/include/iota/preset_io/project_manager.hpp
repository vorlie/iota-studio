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

#include "iota/model/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iota::preset_io {

enum class ProjectTemplateKind {
  MinimalCircular = 0,
  DualCompGlow,
  HorizontalCurve,
  Blank,
};

struct ProjectTemplateInfo {
  ProjectTemplateKind kind;
  const char* title;
  const char* description;
};

struct BundledTemplateInfo {
  std::string id;
  std::string title;
  std::string description;
  std::string file;
  std::filesystem::path archivePath;
  int defaultWidth = 1920;
  int defaultHeight = 1080;
  int defaultFps = 60;
};

const std::vector<ProjectTemplateInfo>& projectTemplates() noexcept;

iota::model::Project createProjectFromTemplate(ProjectTemplateKind kind,
                                               std::string name,
                                               int width = 1280,
                                               int height = 720,
                                               int fps = 60);

std::vector<BundledTemplateInfo> loadBundledTemplateManifest(
    const std::filesystem::path& templatesDir,
    bool requireArchiveExists = true);

std::optional<iota::model::Project> loadProjectFromBundledTemplate(
    const BundledTemplateInfo& tpl,
    std::string nameOverride = {},
    int widthOverride = 0,
    int heightOverride = 0,
    int fpsOverride = 0,
    std::string* outError = nullptr);

std::optional<iota::model::Project> loadProjectFromBundledTemplateMemory(
    std::span<const uint8_t> bytes,
    const BundledTemplateInfo& tpl,
    std::string nameOverride = {},
    int widthOverride = 0,
    int heightOverride = 0,
    int fpsOverride = 0,
    std::string* outError = nullptr);

struct RecentProjectEntry {
  std::string name;
  std::string path;
  std::string lastOpened;
  int compositionCount = 0;
  int assetCount = 0;
};

/// Persistent recent-project registry stored outside the workspace by default.
class ProjectLibrary {
 public:
  static std::filesystem::path defaultRegistryPath();

  bool load(const std::filesystem::path& path);
  bool save(const std::filesystem::path& path) const;

  void recordOpened(RecentProjectEntry entry);
  bool removeEntry(std::string_view path);
  int pruneMissing();
  void clear() noexcept { entries_.clear(); }

  [[nodiscard]] const std::vector<RecentProjectEntry>& entries() const noexcept { return entries_; }
  [[nodiscard]] bool suppressVizImportNotice() const noexcept { return suppressVizImportNotice_; }
  void setSuppressVizImportNotice(bool suppress) noexcept { suppressVizImportNotice_ = suppress; }
  [[nodiscard]] bool discordRichPresenceEnabled() const noexcept { return discordRichPresenceEnabled_; }
  void setDiscordRichPresenceEnabled(bool enabled) noexcept { discordRichPresenceEnabled_ = enabled; }
  [[nodiscard]] bool discordExpandedPresence() const noexcept { return discordExpandedPresence_; }
  void setDiscordExpandedPresence(bool expanded) noexcept { discordExpandedPresence_ = expanded; }

 private:
  std::vector<RecentProjectEntry> entries_;
  bool suppressVizImportNotice_ = false;
  bool discordRichPresenceEnabled_ = false;
  bool discordExpandedPresence_ = false;
};

/// Saves a Project in .json (if extension is .json/.txt) or zipped archive (.iota/.viz/.zip).
bool saveProjectAutoFormat(const iota::model::Project& project,
                           const std::filesystem::path& path,
                           std::string* outError = nullptr);

}  // namespace iota::preset_io
