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

#include "iota/preset_io/project_manager.hpp"

#include "iota/core/utf8_path.hpp"
#include "iota/preset_io/preset_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <system_error>

namespace iota::preset_io {

namespace {

using Json = nlohmann::json;

std::string normalizePathLower(std::string_view p) {
  std::string s = iota::core::pathToUtf8(iota::core::pathFromUtf8(p).lexically_normal());
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string currentLocalIsoTimestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tmBuf{};
#ifdef _WIN32
  localtime_s(&tmBuf, &now);
#else
  localtime_r(&now, &tmBuf);
#endif
  char buf[32]{};
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf) > 0) {
    return buf;
  }
  return "";
}

std::string toLowerAscii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

}  // namespace

const std::vector<ProjectTemplateInfo>& projectTemplates() noexcept {
  static const std::vector<ProjectTemplateInfo> kTemplates = {
      {ProjectTemplateKind::MinimalCircular, "Minimal Circular",
       "Balanced starter composition with circular spectrum, reactive particles, background, and track label."},
      {ProjectTemplateKind::DualCompGlow, "Dual Composition Glow",
       "Sub-composition source feeding SoundKeys-driven Blur glow and RgbSplit chromatic aberration on Composition 0."},
      {ProjectTemplateKind::HorizontalCurve, "Horizontal Spectrum Bar",
       "Widescreen horizontal Catmull-Rom spectrum ribbon paired with SoundKeys range presets."},
      {ProjectTemplateKind::Blank, "Blank Project",
       "Minimal single composition containing AudioProvider and SoundKeys ready to build from scratch."},
  };
  return kTemplates;
}

iota::model::Project createProjectFromTemplate(ProjectTemplateKind kind,
                                               std::string name,
                                               int width,
                                               int height,
                                               int fps) {
  using namespace iota::model;
  Project proj;
  proj.version = "10";
  proj.templateId = static_cast<int>(kind);

  switch (kind) {
    case ProjectTemplateKind::DualCompGlow: {
      Composition comp0 = createDefaultComposition(0);
      comp0.props.setString("tonemapMode", "ACES", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
      comp0.props.setBool("bloomEnabled", true, "3_hdrPost", "b");
      comp0.props.setFloat("bloomThreshold", 0.85f, 0.0f, 5.0f, "3_hdrPost");
      comp0.props.setFloat("bloomIntensity", 0.50f, 0.0f, 4.0f, "3_hdrPost");
      Element bg = createDefaultElement(ElementType::Image);
      bg.displayName = "Background";
      bg.props.setColorHsla("Color", {0.58f, 0.45f, 0.10f, 1.0f}, "2_color");
      bg.props.setColorHsla("ColorTo", {0.72f, 0.55f, 0.18f, 1.0f}, "2_color");
      comp0.elements.push_back(std::move(bg));

      Element blur = createDefaultElement(ElementType::BlurEffect);
      blur.displayName = "Glow Blur";
      blur.props.setInt("sourceCompositionIndex", 1, 1, 5, "1_appearance");
      blur.props.setFloat("blurRadius", 0.9f, 0.0f, 3.0f, "2_blur");
      blur.props.setMVar("measureBlurRadius", MVariableFloat{{{"SoundKeys1", 1.8f, 0.0f}}}, "mvarf 0.0 3.0", "2_blur");
      comp0.elements.push_back(std::move(blur));

      Element split = createDefaultElement(ElementType::RgbSplitEffect);
      split.displayName = "Bass RGB Split";
      split.props.setString("targetImage", "composition:1", "img composition:0", "1_appearance");
      split.props.setMVar("splitMultiplier", MVariableFloat{{{"SoundKeys1", 2.2f, 0.0f}}}, "mvarf 0.0 6.0", "2_splitEffect");
      split.props.setMVar("splitAmount", MVariableFloat{{{"SoundKeysShake1", 0.35f, 0.35f}}}, "mvarf2 -0.5 0.5", "2_splitEffect");
      comp0.elements.push_back(std::move(split));

      Element label = createDefaultElement(ElementType::Text);
      label.displayName = "Track Label";
      comp0.elements.push_back(std::move(label));

      Composition comp1 = createDefaultComposition(1);
      comp1.displayName = "Visual Source";
      comp1.elements.push_back(createDefaultElement(ElementType::AudioProvider));
      comp1.elements.push_back(createDefaultElement(ElementType::SoundKeys));
      Element bars = createDefaultElement(ElementType::Bars);
      bars.displayName = "Source Spectrum";
      bars.props.setFloat("hdrIntensity", 1.8f, 0.0f, 16.0f, "1_bars");
      comp1.elements.push_back(std::move(bars));
      Element particles = createDefaultElement(ElementType::Particles);
      particles.displayName = "Source Particles";
      particles.props.setFloat("hdrIntensity", 1.8f, 0.0f, 16.0f, "appearance");
      comp1.elements.push_back(std::move(particles));

      proj.compositions.push_back(std::move(comp0));
      proj.compositions.push_back(std::move(comp1));
      break;
    }

    case ProjectTemplateKind::HorizontalCurve: {
      Composition comp0 = createDefaultComposition(0);
      comp0.elements.push_back(createDefaultElement(ElementType::AudioProvider));
      comp0.elements.push_back(createDefaultElement(ElementType::SoundKeys));

      Element bg = createDefaultElement(ElementType::Image);
      bg.displayName = "Background";
      bg.props.setColorHsla("Color", {0.54f, 0.42f, 0.12f, 1.0f}, "2_color");
      bg.props.setColorHsla("ColorTo", {0.66f, 0.52f, 0.22f, 1.0f}, "2_color");
      comp0.elements.push_back(std::move(bg));

      Element bars = createDefaultElement(ElementType::Bars);
      bars.displayName = "Horizon Wave";
      bars.props.setBool("scaleIsUniform", false, "0_general", "b");
      bars.props.setVec2("position", {0.5f, 0.58f}, -1.0f, 1.0f, "0_general");
      bars.props.setVec2("scale", {0.85f, 0.45f}, 0.0f, 2.0f, "0_general");
      bars.props.getOrAdd("ShapePath").value = std::string("HorizontalLine");
      bars.props.setString("curveInterpolation", "CatmullRom", "sel None Linear CatmullRom CubicBSpline", "1_bars");
      bars.props.setInt("curveSubdivisions", 4, 1, 8, "1_bars");
      bars.props.getOrAdd("Segment1").value = std::string("Line");
      comp0.elements.push_back(std::move(bars));

      Element label = createDefaultElement(ElementType::Text);
      label.displayName = "Track Label";
      comp0.elements.push_back(std::move(label));

      proj.compositions.push_back(std::move(comp0));
      break;
    }

    case ProjectTemplateKind::Blank: {
      Composition comp0 = createDefaultComposition(0);
      comp0.elements.push_back(createDefaultElement(ElementType::AudioProvider));
      comp0.elements.push_back(createDefaultElement(ElementType::SoundKeys));
      proj.compositions.push_back(std::move(comp0));
      break;
    }

    case ProjectTemplateKind::MinimalCircular:
    default:
      proj = createStarterProject();
      break;
  }

  proj.metadata.name = name.empty() ? "Untitled Project" : std::move(name);
  proj.metadata.previewWidth = std::max(16, width);
  proj.metadata.previewHeight = std::max(16, height);
  proj.metadata.targetFps = std::clamp(fps, 1, 240);
  return proj;
}

std::filesystem::path ProjectLibrary::defaultRegistryPath() {
#ifdef _WIN32
  if (const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA"); localAppData != nullptr && *localAppData != L'\0') {
    return std::filesystem::path(localAppData) / "IotaStudio" / "projects.json";
  }
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg) / "IotaStudio" / "projects.json";
  }
#endif
  return std::filesystem::path("iota_projects.json");
}

bool ProjectLibrary::load(const std::filesystem::path& path) {
  entries_.clear();
  suppressVizImportNotice_ = false;
  discordRichPresenceEnabled_ = false;
  discordExpandedPresence_ = false;
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    return false;
  }
  const auto sz = in.tellg();
  if (sz < 0) {
    return false;
  }
  std::string raw(static_cast<size_t>(sz), '\0');
  in.seekg(0, std::ios::beg);
  if (sz > 0 && !in.read(raw.data(), sz)) {
    return false;
  }
  const std::string_view trimmed = iota::core::stripUtf8Bom(raw);
  Json root;
  try {
    root = Json::parse(trimmed.begin(), trimmed.end());
  } catch (const std::exception&) {
    const std::string clean = iota::core::sanitizeUtf8(trimmed);
    if (clean == trimmed) {
      return false;
    }
    try {
      root = Json::parse(clean.begin(), clean.end());
    } catch (const std::exception&) {
      return false;
    }
  }
  if (!root.is_object()) {
    return false;
  }
  if (auto supIt = root.find("suppressVizImportNotice"); supIt != root.end() && supIt->is_boolean()) {
    suppressVizImportNotice_ = supIt->get<bool>();
  }
  if (auto drpIt = root.find("discordRichPresenceEnabled"); drpIt != root.end() && drpIt->is_boolean()) {
    discordRichPresenceEnabled_ = drpIt->get<bool>();
  }
  if (auto expIt = root.find("discordExpandedPresence"); expIt != root.end() && expIt->is_boolean()) {
    discordExpandedPresence_ = expIt->get<bool>();
  }
  auto recIt = root.find("recent");
  if (recIt == root.end() || !recIt->is_array()) {
    return true;
  }
  try {
    for (const auto& item : *recIt) {
      if (!item.is_object()) continue;
      RecentProjectEntry e;
      e.name = iota::core::sanitizeUtf8(item.value("name", ""));
      e.path = iota::core::sanitizeUtf8(item.value("path", ""));
      e.lastOpened = iota::core::sanitizeUtf8(item.value("lastOpened", ""));
      e.compositionCount = item.value("compositionCount", 0);
      e.assetCount = item.value("assetCount", 0);
      if (!e.path.empty()) {
        entries_.push_back(std::move(e));
      }
    }
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

bool ProjectLibrary::save(const std::filesystem::path& path) const {
  if (!path.parent_path().empty()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  Json arr = Json::array();
  for (const auto& e : entries_) {
    arr.push_back({
        {"name", iota::core::sanitizeUtf8(e.name)},
        {"path", iota::core::sanitizeUtf8(e.path)},
        {"lastOpened", iota::core::sanitizeUtf8(e.lastOpened)},
        {"compositionCount", e.compositionCount},
        {"assetCount", e.assetCount},
    });
  }
  Json root = {
      {"version", 1},
      {"suppressVizImportNotice", suppressVizImportNotice_},
      {"discordRichPresenceEnabled", discordRichPresenceEnabled_},
      {"discordExpandedPresence", discordExpandedPresence_},
      {"recent", std::move(arr)},
  };
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  const std::string dumped = root.dump(2, ' ', false, Json::error_handler_t::replace);
  out.write(dumped.data(), static_cast<std::streamsize>(dumped.size()));
  return out.good();
}

void ProjectLibrary::recordOpened(RecentProjectEntry entry) {
  entry.path = iota::core::sanitizeUtf8(entry.path);
  if (entry.path.empty()) {
    return;
  }
  entry.name = iota::core::sanitizeUtf8(entry.name);
  if (entry.name.empty()) {
    entry.name = iota::core::pathStemUtf8(iota::core::pathFromUtf8(entry.path));
  }
  entry.lastOpened = iota::core::sanitizeUtf8(entry.lastOpened);
  if (entry.lastOpened.empty()) {
    entry.lastOpened = currentLocalIsoTimestamp();
  }
  const std::string norm = normalizePathLower(entry.path);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const RecentProjectEntry& existing) {
                                  return normalizePathLower(existing.path) == norm;
                                }),
                 entries_.end());
  entries_.insert(entries_.begin(), std::move(entry));
  if (entries_.size() > 20) {
    entries_.resize(20);
  }
}

bool ProjectLibrary::removeEntry(std::string_view path) {
  const std::string norm = normalizePathLower(path);
  const size_t before = entries_.size();
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const RecentProjectEntry& e) {
                                  return normalizePathLower(e.path) == norm;
                                }),
                 entries_.end());
  return entries_.size() < before;
}

int ProjectLibrary::pruneMissing() {
  int removed = 0;
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const RecentProjectEntry& e) {
                                  const bool missing =
                                      !iota::core::pathExistsNoThrow(iota::core::pathFromUtf8(e.path));
                                  if (missing) ++removed;
                                  return missing;
                                }),
                 entries_.end());
  return removed;
}

bool saveProjectAutoFormat(const iota::model::Project& project,
                           const std::filesystem::path& path,
                           std::string* outError) {
  const std::string ext = toLowerAscii(iota::core::pathExtensionUtf8(path));
  if (ext == ".json" || ext == ".txt") {
    return saveProjectAsJson(project, path, outError);
  }
  return saveProjectAsViz(project, path, outError);
}

}  // namespace iota::preset_io
