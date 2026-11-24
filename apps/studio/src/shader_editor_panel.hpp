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
