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
