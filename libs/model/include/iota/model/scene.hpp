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

} // namespace iota::model
