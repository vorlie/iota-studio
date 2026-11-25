#pragma once

#include "iota/model/scene.hpp"

#include <filesystem>
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

const std::vector<ProjectTemplateInfo>& projectTemplates() noexcept;

iota::model::Project createProjectFromTemplate(ProjectTemplateKind kind,
                                               std::string name,
                                               int width = 1280,
                                               int height = 720,
                                               int fps = 60);

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
