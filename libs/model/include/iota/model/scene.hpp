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
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "iota/model/property.hpp"

namespace iota::model {

// Complete 14 element types from MF.java
enum class ElementType {
  Unknown = 0,
  Root,
  Composition,
  AudioProvider,
  Bars,
  Particles,
  Image,
  Text,
  BlurEffect,
  RgbSplitEffect,
  MotionBlurEffect,
  MirrorEffect,
  AppLogo,
  StatText,
  CustomShader,
  SoundKeys,
  ShakeEffect
};

ElementType elementTypeFromString(std::string_view name) noexcept;
std::string_view elementTypeToString(ElementType type) noexcept;
const std::vector<ElementType>& addableElementTypes() noexcept;

struct Element {
  ElementType type = ElementType::Unknown;
  std::string typeName;
  std::string version = "10";
  std::string displayName;
  PropertyBag props;

  int runtimeId() const noexcept { return props.getInt("_id", -1); }
};

struct Composition {
  std::string version = "10";
  std::string displayName = "Composition";
  PropertyBag props;
  std::vector<Element> elements;
};

// Optional desktop-only keyframe timeline extension (non-destructive alongside MeasuredVar)
struct Keyframe {
  double timeSec = 0.0;
  float value = 0.0f;
};

struct KeyframeTrack {
  int compositionIndex = 0;
  int elementIndex = 0;
  std::string propertyPath;
  std::vector<Keyframe> keyframes;
};

struct ProjectMetadata {
  std::string name = "Untitled Project";
  std::string notes;
  std::string audioPath;
  int previewWidth = 1280;
  int previewHeight = 720;
  int targetFps = 60;
  std::map<std::string, std::string> assetFolders;  // localAsset filename -> virtual folder
  std::vector<std::string> customFolders;           // user-created empty/active bins
  std::map<std::string, std::string> audioAssets;   // audio asset label -> file path
};

struct Project {
  std::string version = "10";
  int templateId = 0;
  ProjectMetadata metadata;
  std::vector<Composition> compositions;

  // Bundled local assets inside .viz / .iota archives (keyed by filename for "local:<filename>")
  std::map<std::string, std::vector<uint8_t>> localAssets;

  // Optional desktop timeline extension
  std::vector<KeyframeTrack> desktopTracks;
};

Element createDefaultElement(ElementType type);
Composition createDefaultComposition(int index);
Project createStarterProject();

// UI-independent project asset inspection & management helpers
int countAssetReferences(const Project& project, std::string_view assetUri) noexcept;
bool renameLocalAsset(Project& project, std::string_view oldName, std::string_view newName);
int removeUnusedLocalAssets(Project& project);
bool assignAssetToElement(Element& el, std::string_view uri, bool asMask = false);

// Helpers to ensure Base transform props exist cleanly on newly created Elements
void addBaseElementProperties(PropertyBag& props, int id, iota::core::BlendMode defaultBlend);

// Ensures default child properties exist for the active `_child` variant without overwriting existing children.
void seedVariantChildren(PropertyEntry& entry, std::string_view propKey, std::string_view variant);

} // namespace iota::model
