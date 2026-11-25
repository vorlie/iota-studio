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

#include "shader_editor_panel.hpp"

#include "iota/engine/builtin_shader_library.hpp"

#include "ImGuiColorTextEdit.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace iota::studio {

namespace {

bool hasSuffixCI(std::string_view text, std::string_view suffix) noexcept {
  if (text.size() < suffix.size()) return false;
  const size_t offset = text.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[offset + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

bool isShaderAssetName(std::string_view name) noexcept {
  return hasSuffixCI(name, ".glsl") ||
         hasSuffixCI(name, ".frag") ||
         hasSuffixCI(name, ".vert") ||
         hasSuffixCI(name, ".fsh") ||
         hasSuffixCI(name, ".vsh") ||
         hasSuffixCI(name, ".fs") ||
         hasSuffixCI(name, ".vs");
}

std::string bytesToUtf8StrippingBom(const std::vector<uint8_t>& bytes) {
  std::string out(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (out.size() >= 3 &&
      static_cast<uint8_t>(out[0]) == 0xEF &&
      static_cast<uint8_t>(out[1]) == 0xBB &&
      static_cast<uint8_t>(out[2]) == 0xBF) {
    out.erase(0, 3);
  }
  return out;
}

std::string normalizeNewlinesLf(std::string_view src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size(); ++i) {
    if (src[i] == '\r') {
      if (i + 1 < src.size() && src[i + 1] == '\n') {
        ++i;
      }
      out.push_back('\n');
    } else {
      out.push_back(src[i]);
    }
  }
  return out;
}

const ImTextEdit::LanguageDefinition& makeIotaGlslLanguageDef() {
  static const ImTextEdit::LanguageDefinition def = [] {
    ImTextEdit::LanguageDefinition lang = ImTextEdit::GLSL();
    lang.mName = "GLSL";
    auto addIdent = [&](const char* name, const char* doc) {
      lang.mIdentifiers.emplace(name, ImTextEdit::Identifier{doc});
    };
    addIdent("u_texture", "uniform sampler2D u_texture\nBound TargetImage texture or composition FBO.");
    addIdent("u_time", "uniform float u_time\nCurrent timeline/scene time in seconds.");
    addIdent("u_resolution", "uniform vec2 u_resolution\nActive render target dimensions in pixels.");
    addIdent("u_beat", "uniform float u_beat\nNormalized smoothed audio beat intensity [0..1+].");
    addIdent("u_soundKeys", "uniform vec4 u_soundKeys\nActive Sound Keys bands (R1, R2, R3, beat).");
    addIdent("u_hdrGain", "uniform float u_hdrGain\nHDR emissive gain multiplier.");
    addIdent("v_uv", "in vec2 v_uv\nInterpolated quad texture coordinates [0..1].");
    addIdent("v_color", "in vec4 v_color\nLayer tint color & opacity.");
    addIdent("iota_FragColor", "out vec4 iota_FragColor\nFragment shader color output (auto-mapped from gl_FragColor).");
    return lang;
  }();
  return def;
}

ImTextEdit::TextEditor::ErrorMarkers parseGlslErrorMarkers(std::string_view err) {
  ImTextEdit::TextEditor::ErrorMarkers markers;
  if (err.empty()) {
    return markers;
  }

  size_t lineStart = 0;
  while (lineStart < err.size()) {
    size_t lineEnd = err.find('\n', lineStart);
    if (lineEnd == std::string_view::npos) lineEnd = err.size();
    std::string_view line = err.substr(lineStart, lineEnd - lineStart);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.remove_suffix(1);
    }

    int parsedLine = -1;
    // Match "0(42) : error ..." (NVIDIA) or "0:42: error ..." (AMD/Intel/Mesa)
    for (size_t i = 0; i + 2 < line.size(); ++i) {
      if ((line[i] == '(' || line[i] == ':') && std::isdigit(static_cast<unsigned char>(line[i + 1]))) {
        size_t j = i + 1;
        int val = 0;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
          val = val * 10 + (line[j] - '0');
          ++j;
        }
        const char expectedClose = (line[i] == '(') ? ')' : ':';
        if (j < line.size() && (line[j] == expectedClose || line[j] == '(') && val > 0 && val < 100000) {
          parsedLine = val;
          break;
        }
      }
    }

    if (parsedLine > 0) {
      markers.emplace(parsedLine, std::string(line));
    }
    lineStart = lineEnd + 1;
  }
  return markers;
}

constexpr const char* kStarterFragmentShader =
R"(#version 450 core

in vec2 v_uv;
in vec4 v_color;
out vec4 iota_FragColor;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_beat;
uniform vec4 u_soundKeys; // (R1, R2, R3, beat)

void main() {
  vec4 baseCol = texture(u_texture, v_uv);
  float wave = 0.08 * sin(v_uv.y * 24.0 - u_time * 3.0) * (0.25 + u_beat);
  baseCol.rgb += vec3(0.15, 0.35, 0.65) * max(wave, 0.0);
  baseCol *= v_color;
  baseCol.a = clamp(baseCol.a, 0.0, 1.0);
  iota_FragColor = baseCol;
}
)";

std::string makeUniqueShaderName(const iota::model::Project& project) {
  for (int i = 1; i < 1000; ++i) {
    const std::string cand = "shader_" + std::to_string(i) + ".glsl";
    if (project.localAssets.find(cand) == project.localAssets.end()) {
      return cand;
    }
  }
  return "shader_custom.glsl";
}

void writeStringToAsset(iota::model::Project& project, const std::string& name, const std::string& text) {
  project.localAssets[name] = std::vector<uint8_t>(text.begin(), text.end());
}

}  // namespace

struct ShaderEditorPanel::Impl {
  ImTextEdit::TextEditor editor;
  std::string activeAssetName;
  std::string lastSyncedAssetText;
  std::string revertSnapshotText;
  std::string lastParsedError;
  bool focusRequested = false;
  bool liveApply = true;
  bool dirtyFromRevert = false;
  bool pendingUndoOnBlur = false;
  bool focused = false;
  bool wasFocused = false;
  int paletteIdx = 0;  // 0=Dark, 1=Light, 2=RetroBlue
  bool showWhitespace = false;

  Impl() {
    editor.SetLanguageDefinition(makeIotaGlslLanguageDef());
    editor.SetPalette(ImTextEdit::GetDarkPalette());
    editor.SetShowWhitespaces(false);
    editor.SetShowLineNumbers(true);
    editor.SetTabSize(2);
    editor.SetInsertSpaces(true);
    editor.SetHiglightBrackets(true);
    editor.SetSearchEnabled(true);
    editor.SetSidebarVisible(true);
    editor.SetHandleKeyboardInputs(true);
    editor.SetHandleMouseInputs(true);
  }

  void loadTextIntoEditor(const std::string& rawText, bool resetRevert) {
    const std::string normalized = normalizeNewlinesLf(rawText);
    editor.SetText(normalized);
    editor.ResetTextChanged();
    lastSyncedAssetText = editor.GetText();
    if (resetRevert) {
      revertSnapshotText = lastSyncedAssetText;
      dirtyFromRevert = false;
    }
    pendingUndoOnBlur = false;
  }
};

ShaderEditorPanel::ShaderEditorPanel() : impl_(std::make_unique<Impl>()) {}
ShaderEditorPanel::~ShaderEditorPanel() = default;

void ShaderEditorPanel::openAsset(const iota::model::Project& project, std::string_view assetName) {
  if (assetName.empty()) return;
  const std::string nameStr(assetName);
  const auto it = project.localAssets.find(nameStr);
  if (it == project.localAssets.end()) {
    return;
  }
  impl_->activeAssetName = nameStr;
  impl_->loadTextIntoEditor(bytesToUtf8StrippingBom(it->second), true);
  impl_->focusRequested = true;
}

void ShaderEditorPanel::clearIfMissing(const iota::model::Project& project) {
  if (!impl_->activeAssetName.empty() &&
      project.localAssets.find(impl_->activeAssetName) == project.localAssets.end()) {
    impl_->activeAssetName.clear();
    impl_->lastSyncedAssetText.clear();
    impl_->revertSnapshotText.clear();
    impl_->editor.SetText("");
    impl_->editor.ResetTextChanged();
    impl_->dirtyFromRevert = false;
    impl_->pendingUndoOnBlur = false;
  }
}

bool ShaderEditorPanel::isFocused() const noexcept {
  return impl_ && impl_->focused;
}

const std::string& ShaderEditorPanel::activeAssetName() const noexcept {
  static const std::string kEmpty;
  return impl_ ? impl_->activeAssetName : kEmpty;
}

void ShaderEditorPanel::render(iota::model::Project& project,
                               bool& openWindow,
                               const std::string& lastRenderError,
                               ImFont* codeFont,
                               ShaderEditorActions& actions) {
  clearIfMissing(project);

  if (!openWindow) {
    impl_->focused = false;
    impl_->wasFocused = false;
    return;
  }

  // Collect all shader assets cleanly for selector dropdown
  std::vector<std::string> shaderNames;
  for (const auto& [name, bytes] : project.localAssets) {
    (void)bytes;
    if (isShaderAssetName(name)) {
      shaderNames.push_back(name);
    }
  }

  // Auto-open the first shader asset when opening the window with nothing loaded
  if (impl_->activeAssetName.empty() && !shaderNames.empty()) {
    const auto it = project.localAssets.find(shaderNames.front());
    if (it != project.localAssets.end()) {
      impl_->activeAssetName = shaderNames.front();
      impl_->loadTextIntoEditor(bytesToUtf8StrippingBom(it->second), true);
    }
  } else if (!impl_->activeAssetName.empty()) {
    // Detect external edits (e.g., Undo/Redo or Asset Browser Replace File...)
    const auto it = project.localAssets.find(impl_->activeAssetName);
    if (it != project.localAssets.end()) {
      const std::string currentAssetText = normalizeNewlinesLf(bytesToUtf8StrippingBom(it->second));
      if (currentAssetText != impl_->lastSyncedAssetText && !impl_->editor.IsTextChanged()) {
        impl_->loadTextIntoEditor(currentAssetText, true);
      }
    }
  }

  // Update inline GLSL error markers only when lastRenderError changes
  if (lastRenderError != impl_->lastParsedError) {
    impl_->lastParsedError = lastRenderError;
    impl_->editor.SetErrorMarkers(parseGlslErrorMarkers(lastRenderError));
  }

  if (impl_->focusRequested) {
    ImGui::SetNextWindowFocus();
    impl_->focusRequested = false;
  }

  if (!ImGui::Begin("Shader Editor", &openWindow)) {
    impl_->focused = false;
    impl_->wasFocused = false;
    ImGui::End();
    return;
  }

  const bool hasAsset = !impl_->activeAssetName.empty() &&
                        project.localAssets.find(impl_->activeAssetName) != project.localAssets.end();

  // --- Toolbar Row ---
  ImGui::SetNextItemWidth(190.0f);
  const std::string comboLabel = hasAsset
                                     ? (impl_->activeAssetName + (impl_->dirtyFromRevert ? " *" : ""))
                                     : "(No shader selected)";
  if (ImGui::BeginCombo("##shader_asset_select", comboLabel.c_str())) {
    if (shaderNames.empty()) {
      ImGui::TextDisabled("(No shader assets in project)");
    }
    for (const auto& name : shaderNames) {
      const bool sel = (name == impl_->activeAssetName);
      if (ImGui::Selectable(name.c_str(), sel)) {
        const auto it = project.localAssets.find(name);
        if (it != project.localAssets.end()) {
          impl_->activeAssetName = name;
          impl_->loadTextIntoEditor(bytesToUtf8StrippingBom(it->second), true);
        }
      }
    }
    ImGui::EndCombo();
  }

  ImGui::SameLine();
  if (ImGui::Button("+ New")) {
    ImGui::OpenPopup("##NewShaderTemplatePopup");
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Create a blank shader or add an editable built-in shader preset");
  }
  if (ImGui::BeginPopup("##NewShaderTemplatePopup")) {
    if (ImGui::MenuItem("Blank Starter Shader")) {
      const std::string newName = makeUniqueShaderName(project);
      writeStringToAsset(project, newName, kStarterFragmentShader);
      impl_->activeAssetName = newName;
      impl_->loadTextIntoEditor(kStarterFragmentShader, true);
      actions.modified = true;
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Create Shader " + newName;
      actions.statusMessage = "Created local:" + newName;
    }
    ImGui::Separator();
    ImGui::TextDisabled("Built-in Shader Presets");
    const auto& presets = iota::engine::listBuiltinShaderPresets();
    static constexpr iota::engine::BuiltinShaderCategory kCats[] = {
        iota::engine::BuiltinShaderCategory::AudioReactive,
        iota::engine::BuiltinShaderCategory::Procedural,
        iota::engine::BuiltinShaderCategory::CinematicLens,
        iota::engine::BuiltinShaderCategory::GeometricSciFi,
        iota::engine::BuiltinShaderCategory::RetroArtistic,
    };
    for (const auto cat : kCats) {
      const std::string catLabel(iota::engine::builtinShaderCategoryName(cat));
      if (ImGui::BeginMenu(catLabel.c_str())) {
        for (const auto& preset : presets) {
          if (preset.category != cat) continue;
          const char* kindTag =
              (preset.kind == iota::engine::BuiltinShaderKind::PostFx) ? "[Post-FX]" : "[BG]";
          const std::string itemLabel = std::string(preset.displayName) + "  " + kindTag;
          if (ImGui::MenuItem(itemLabel.c_str())) {
            const std::string newName = iota::engine::instantiateBuiltinShaderAsset(project, preset);
            impl_->activeAssetName = newName;
            const auto it = project.localAssets.find(newName);
            if (it != project.localAssets.end()) {
              impl_->loadTextIntoEditor(bytesToUtf8StrippingBom(it->second), true);
            }
            actions.modified = true;
            actions.undoSnapshotNeeded = true;
            actions.editLabel = "Add Built-in Shader " + newName;
            actions.statusMessage = "Added local:" + newName;
          }
          if (ImGui::IsItemHovered() && !preset.description.empty()) {
            ImGui::SetTooltip("%s", std::string(preset.description).c_str());
          }
        }
        ImGui::EndMenu();
      }
    }
    ImGui::EndPopup();
  }

  ImGui::SameLine();
  if (ImGui::Button("Import...")) {
    actions.requestImportShader = true;
  }

  ImGui::SameLine();
  ImGui::BeginDisabled(!hasAsset);
  const bool applyClicked = ImGui::Button("Apply");
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Apply edits and push an Undo snapshot (Ctrl+S)");
  }

  ImGui::SameLine();
  ImGui::Checkbox("Live", &impl_->liveApply);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Automatically recompile shader as you type");
  }

  ImGui::SameLine();
  ImGui::BeginDisabled(!hasAsset || !impl_->dirtyFromRevert);
  if (ImGui::Button("Revert")) {
    impl_->loadTextIntoEditor(impl_->revertSnapshotText, true);
    writeStringToAsset(project, impl_->activeAssetName, impl_->revertSnapshotText);
    actions.modified = true;
    actions.statusMessage = "Reverted changes to local:" + impl_->activeAssetName;
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  ImGui::BeginDisabled(!hasAsset);
  if (ImGui::Button("+ Layer")) {
    actions.requestAddCustomShaderLayer = true;
    actions.addLayerAssetName = impl_->activeAssetName;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Add a new CustomShader layer using this shader");
  }

  ImGui::SameLine();
  if (ImGui::Button("Options")) {
    ImGui::OpenPopup("##ShaderEditorOptionsPopup");
  }
  if (ImGui::BeginPopup("##ShaderEditorOptionsPopup")) {
    ImGui::TextDisabled("Syntax Theme");
    if (ImGui::MenuItem("Dark", nullptr, impl_->paletteIdx == 0)) {
      impl_->paletteIdx = 0;
      impl_->editor.SetPalette(ImTextEdit::GetDarkPalette());
    }
    if (ImGui::MenuItem("Light", nullptr, impl_->paletteIdx == 1)) {
      impl_->paletteIdx = 1;
      impl_->editor.SetPalette(ImTextEdit::GetLightPalette());
    }
    if (ImGui::MenuItem("Retro Blue", nullptr, impl_->paletteIdx == 2)) {
      impl_->paletteIdx = 2;
      impl_->editor.SetPalette(ImTextEdit::GetRetroBluePalette());
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Show Whitespace", nullptr, &impl_->showWhitespace)) {
      impl_->editor.SetShowWhitespaces(impl_->showWhitespace);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Open GLSL Palette Manager (Web)...")) {
      actions.requestOpenUrl = "https://vorlie.github.io/iota-studio/";
    }
    ImGui::EndPopup();
  }

  if (!lastRenderError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 115, 115, 255));
    ImGui::TextWrapped("%s", lastRenderError.c_str());
    ImGui::PopStyleColor();
  }

  if (!hasAsset) {
    ImGui::Separator();
    ImGui::TextDisabled("No shader asset loaded. Click '+ New' to create one or 'Import...' to open a .glsl file.");
    impl_->focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    impl_->wasFocused = impl_->focused;
    ImGui::End();
    return;
  }

  // --- Code Editor Area ---
  const float statusBarH = ImGui::GetFrameHeight() + 6.0f;
  if (codeFont != nullptr) {
    ImGui::PushFont(codeFont);
  }
  impl_->editor.Render("##glsl_code_editor", ImVec2(0.0f, -statusBarH), true);
  if (codeFont != nullptr) {
    ImGui::PopFont();
  }

  const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
  impl_->focused = impl_->editor.IsFocused() || windowFocused;

  const ImGuiIO& io = ImGui::GetIO();
  const bool ctrlSPressed = impl_->focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);

  if (impl_->editor.IsTextChanged()) {
    const std::string currentText = impl_->editor.GetText();
    impl_->dirtyFromRevert = (currentText != impl_->revertSnapshotText);
    impl_->editor.ResetTextChanged();
    if (impl_->liveApply) {
      writeStringToAsset(project, impl_->activeAssetName, currentText);
      impl_->lastSyncedAssetText = currentText;
      impl_->pendingUndoOnBlur = true;
      actions.modified = true;
    }
  }

  if (applyClicked || ctrlSPressed) {
    const std::string currentText = impl_->editor.GetText();
    writeStringToAsset(project, impl_->activeAssetName, currentText);
    impl_->lastSyncedAssetText = currentText;
    impl_->revertSnapshotText = currentText;
    impl_->dirtyFromRevert = false;
    impl_->pendingUndoOnBlur = false;
    actions.modified = true;
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Edit Shader " + impl_->activeAssetName;
    actions.statusMessage = "Applied changes to local:" + impl_->activeAssetName;
  } else if (impl_->wasFocused && !impl_->focused && impl_->pendingUndoOnBlur) {
    impl_->pendingUndoOnBlur = false;
    impl_->revertSnapshotText = impl_->lastSyncedAssetText;
    impl_->dirtyFromRevert = false;
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Edit Shader " + impl_->activeAssetName;
  }
  impl_->wasFocused = impl_->focused;

  // --- Status Bar ---
  const auto cpos = impl_->editor.GetCursorPosition();
  const int lines = impl_->editor.GetTotalLines();
  const bool pendingLive = impl_->dirtyFromRevert && !impl_->liveApply;
  ImGui::Text("Ln %d, Col %d | %d lines | GLSL | local:%s%s",
              cpos.mLine + 1,
              cpos.mColumn + 1,
              lines,
              impl_->activeAssetName.c_str(),
              pendingLive ? " (unsaved - Apply / Ctrl+S)" : "");
  ImGui::SameLine();
  if (lastRenderError.empty()) {
    ImGui::TextColored(ImVec4(0.40f, 0.88f, 0.52f, 1.0f), " | OK");
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), " | Compile Error");
  }

  ImGui::End();
}

}  // namespace iota::studio
