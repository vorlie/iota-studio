#pragma once

#include "iota/model/scene.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace iota::engine {

enum class BuiltinShaderCategory {
  AudioReactive = 0,
  Procedural,
  CinematicLens,
  GeometricSciFi,
  RetroArtistic,
};

enum class BuiltinShaderKind {
  Generator = 0,
  PostFx,
};

struct BuiltinShaderPreset {
  std::string_view id;
  std::string_view displayName;
  std::string_view defaultFileName;
  BuiltinShaderCategory category;
  BuiltinShaderKind kind;
  std::string_view defaultBlendMode;
  std::string_view defaultTarget;
  std::string_view description;
  std::string_view paletteSpaceHint;
  std::string_view fragmentSource;
};

[[nodiscard]] const std::vector<BuiltinShaderPreset>& listBuiltinShaderPresets() noexcept;
[[nodiscard]] const BuiltinShaderPreset* findBuiltinShaderPreset(std::string_view id) noexcept;
[[nodiscard]] std::string_view builtinShaderCategoryName(BuiltinShaderCategory cat) noexcept;

/// Copies a built-in shader template into project.localAssets with a unique filename and
/// returns the created asset name (for referencing as "local:<name>").
[[nodiscard]] std::string instantiateBuiltinShaderAsset(iota::model::Project& project,
                                                        const BuiltinShaderPreset& preset);

}  // namespace iota::engine
