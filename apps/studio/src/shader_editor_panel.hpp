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

#include <memory>
#include <string>
#include <string_view>

struct ImFont;

namespace iota::studio {

struct ShaderEditorActions {
  bool modified = false;
  bool undoSnapshotNeeded = false;
  std::string editLabel;
  bool requestImportShader = false;
  bool requestAddCustomShaderLayer = false;
  std::string addLayerAssetName;
  std::string requestOpenUrl;
  std::string statusMessage;
};

/// Dockable GLSL code editor backed by ImGuiColorTextEdit for editing bundled `local:` shaders live.
class ShaderEditorPanel {
 public:
  ShaderEditorPanel();
  ~ShaderEditorPanel();

  ShaderEditorPanel(const ShaderEditorPanel&) = delete;
  ShaderEditorPanel& operator=(const ShaderEditorPanel&) = delete;

  void openAsset(const iota::model::Project& project, std::string_view assetName);
  void clearIfMissing(const iota::model::Project& project);

  [[nodiscard]] bool isFocused() const noexcept;
  [[nodiscard]] const std::string& activeAssetName() const noexcept;

  void render(iota::model::Project& project,
              bool& openWindow,
              const std::string& lastRenderError,
              ImFont* codeFont,
              ShaderEditorActions& actions);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace iota::studio
