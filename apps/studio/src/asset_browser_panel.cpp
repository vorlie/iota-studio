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

#include "asset_browser_panel.hpp"

#include "iota/core/procedural_assets.hpp"
#include "iota/core/utf8_path.hpp"
#include "iota/engine/builtin_shader_library.hpp"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace iota::studio {

namespace {

std::string toLowerCopy(std::string_view sv) {
  std::string out(sv);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool matchesSearch(std::string_view haystack, const char* needleBuf) {
  if (needleBuf == nullptr || *needleBuf == '\0') {
    return true;
  }
  return toLowerCopy(haystack).find(toLowerCopy(needleBuf)) != std::string::npos;
}

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

std::string formatBytesShort(size_t bytes) {
  char buf[32]{};
  if (bytes >= 1024u * 1024u) {
    std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024u) {
    std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void drawFittedThumbnailBox(iota::rhi::IRenderDevice& device,
                            iota::engine::SceneRenderer& renderer,
                            const iota::model::Project& project,
                            std::string_view uri,
                            float boxSize,
                            bool validVisual,
                            const char* fallbackTag = "AUD",
                            ImU32 fallbackCol = IM_COL32(95, 205, 250, 255)) {
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + boxSize, p0.y + boxSize);
  ImGui::Dummy(ImVec2(boxSize, boxSize));

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(18, 22, 30, 255), 3.0f);

  if (validVisual && !uri.empty()) {
    const auto thumb = renderer.resolveAssetPreview(project, uri);
    if (thumb.texture) {
      const float iw = static_cast<float>(std::max(1, thumb.width));
      const float ih = static_cast<float>(std::max(1, thumb.height));
      const float inner = std::max(4.0f, boxSize - 4.0f);
      const float scale = std::min(inner / iw, inner / ih);
      const float dw = iw * scale;
      const float dh = ih * scale;
      const float ox = p0.x + (boxSize - dw) * 0.5f;
      const float oy = p0.y + (boxSize - dh) * 0.5f;
      const ImVec2 uv0(0.0f, thumb.flipV ? 1.0f : 0.0f);
      const ImVec2 uv1(1.0f, thumb.flipV ? 0.0f : 1.0f);
      const auto texId = static_cast<ImTextureID>(device.nativeTextureId(thumb.texture));
      dl->AddImage(texId, ImVec2(ox, oy), ImVec2(ox + dw, oy + dh), uv0, uv1);
    }
  } else if (!validVisual && fallbackTag != nullptr) {
    const ImVec2 ts = ImGui::CalcTextSize(fallbackTag);
    dl->AddText(ImVec2(p0.x + (boxSize - ts.x) * 0.5f, p0.y + (boxSize - ts.y) * 0.5f),
                fallbackCol, fallbackTag);
  }
  dl->AddRect(p0, p1, IM_COL32(66, 78, 98, 220), 3.0f);
}

void pushAssetDragSource(std::string_view uri,
                         std::string_view label,
                         iota::rhi::IRenderDevice& device,
                         iota::engine::SceneRenderer& renderer,
                         const iota::model::Project& project) {
  if (uri.empty()) return;
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
    std::string payload(uri);
    ImGui::SetDragDropPayload(kAssetDragDropPayload, payload.c_str(), payload.size() + 1);
    const bool isShader = isShaderAssetName(label);
    drawFittedThumbnailBox(device, renderer, project, uri, 32.0f, !isShader,
                           isShader ? "GLSL" : "AUD",
                           isShader ? IM_COL32(250, 195, 90, 255) : IM_COL32(95, 205, 250, 255));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(std::string(label).c_str());
    ImGui::EndDragDropSource();
  }
}

void addImageLayerWithAsset(iota::model::Project& project,
                            int selectedCompIdx,
                            std::string_view uri,
                            std::string_view label,
                            AssetBrowserActions& actions) {
  if (project.compositions.empty()) return;
  const int compIdx = std::clamp(selectedCompIdx, 0, static_cast<int>(project.compositions.size()) - 1);
  auto el = iota::model::createDefaultElement(iota::model::ElementType::Image);
  el.displayName = label.empty() ? "Image Layer" : std::string(label);
  iota::model::assignAssetToElement(el, uri, false);
  auto& elems = project.compositions[static_cast<size_t>(compIdx)].elements;
  elems.push_back(std::move(el));
  actions.selectCompIdx = compIdx;
  actions.selectElemIdx = static_cast<int>(elems.size()) - 1;
  actions.modified = true;
  actions.undoSnapshotNeeded = true;
  actions.editLabel = "Add Image from Asset";
}

void addCustomShaderLayerWithAsset(iota::model::Project& project,
                                   int selectedCompIdx,
                                   std::string_view uri,
                                   std::string_view label,
                                   AssetBrowserActions& actions) {
  if (project.compositions.empty()) return;
  const int compIdx = std::clamp(selectedCompIdx, 0, static_cast<int>(project.compositions.size()) - 1);
  auto el = iota::model::createDefaultElement(iota::model::ElementType::CustomShader);
  el.displayName = label.empty() ? "Custom Shader" : std::string(label);
  iota::model::assignAssetToElement(el, uri, false);
  auto& elems = project.compositions[static_cast<size_t>(compIdx)].elements;
  elems.push_back(std::move(el));
  actions.selectCompIdx = compIdx;
  actions.selectElemIdx = static_cast<int>(elems.size()) - 1;
  actions.modified = true;
  actions.undoSnapshotNeeded = true;
  actions.editLabel = "Add CustomShader from Asset";
}

void copyBuiltinShaderAndEdit(iota::model::Project& project,
                              AssetBrowserState& state,
                              const iota::engine::BuiltinShaderPreset& preset,
                              AssetBrowserActions& actions) {
  const std::string fileName = iota::engine::instantiateBuiltinShaderAsset(project, preset);
  state.selectedKind = BrowserItemKind::LocalImage;
  state.selectedLabel = fileName;
  state.selectedUri = "local:" + fileName;
  actions.invalidateLocalAssets = true;
  actions.requestEditShader = true;
  actions.editShaderAssetName = fileName;
  actions.modified = true;
  actions.undoSnapshotNeeded = true;
  actions.editLabel = "Add Built-in Shader " + fileName;
}

void addCustomShaderLayerFromBuiltin(iota::model::Project& project,
                                     int selectedCompIdx,
                                     const iota::engine::BuiltinShaderPreset& preset,
                                     AssetBrowserActions& actions) {
  if (project.compositions.empty()) return;
  const std::string fileName = iota::engine::instantiateBuiltinShaderAsset(project, preset);
  const int compIdx = std::clamp(selectedCompIdx, 0, static_cast<int>(project.compositions.size()) - 1);
  auto el = iota::model::createDefaultElement(iota::model::ElementType::CustomShader);
  el.displayName = std::string(preset.displayName);
  iota::model::assignAssetToElement(el, "local:" + fileName, false);
  if (!preset.defaultTarget.empty()) {
    el.props.getOrAdd("TargetImage").value = std::string(preset.defaultTarget);
  }
  if (!preset.defaultBlendMode.empty()) {
    el.props.getOrAdd("blendMode").value = std::string(preset.defaultBlendMode);
  }
  auto& elems = project.compositions[static_cast<size_t>(compIdx)].elements;
  elems.push_back(std::move(el));
  actions.selectCompIdx = compIdx;
  actions.selectElemIdx = static_cast<int>(elems.size()) - 1;
  actions.invalidateLocalAssets = true;
  actions.requestEditShader = true;
  actions.editShaderAssetName = fileName;
  actions.modified = true;
  actions.undoSnapshotNeeded = true;
  actions.editLabel = "Add " + std::string(preset.displayName) + " Shader Layer";
}

void applyAssetToSelectedElement(iota::model::Project& project,
                                 int selectedCompIdx,
                                 int selectedElemIdx,
                                 std::string_view uri,
                                 bool asMask,
                                 AssetBrowserActions& actions) {
  if (selectedCompIdx < 0 || selectedCompIdx >= static_cast<int>(project.compositions.size())) return;
  auto& elems = project.compositions[static_cast<size_t>(selectedCompIdx)].elements;
  if (selectedElemIdx < 0 || selectedElemIdx >= static_cast<int>(elems.size())) return;
  auto& el = elems[static_cast<size_t>(selectedElemIdx)];
  if (iota::model::assignAssetToElement(el, uri, asMask)) {
    actions.modified = true;
    actions.undoSnapshotNeeded = true;
    actions.editLabel = asMask ? "Assign Mask Asset" : "Assign Asset";
  }
}

void deleteLocalAssetEntry(iota::model::Project& project,
                           AssetBrowserState& state,
                           std::string_view name,
                           AssetBrowserActions& actions) {
  const std::string key(name);
  if (project.localAssets.erase(key) > 0) {
    project.metadata.assetFolders.erase(key);
    if (state.selectedKind == BrowserItemKind::LocalImage && state.selectedLabel == key) {
      state.selectedKind = BrowserItemKind::None;
      state.selectedUri.clear();
      state.selectedLabel.clear();
    }
    actions.invalidateLocalAssets = true;
    actions.modified = true;
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Delete Local Asset";
  }
}

}  // namespace

void renderAssetBrowserPanel(iota::model::Project& project,
                             const iota::audio::AudioEngine& audio,
                             iota::rhi::IRenderDevice& device,
                             iota::engine::SceneRenderer& renderer,
                             AssetBrowserState& state,
                             int selectedCompIdx,
                             int selectedElemIdx,
                             bool& openWindow,
                             AssetBrowserActions& actions) {
  if (!openWindow) {
    return;
  }
  if (!ImGui::Begin("Assets", &openWindow)) {
    ImGui::End();
    return;
  }

  iota::model::Element* selectedEl = nullptr;
  if (selectedCompIdx >= 0 && selectedCompIdx < static_cast<int>(project.compositions.size())) {
    auto& elems = project.compositions[static_cast<size_t>(selectedCompIdx)].elements;
    if (selectedElemIdx >= 0 && selectedElemIdx < static_cast<int>(elems.size())) {
      selectedEl = &elems[static_cast<size_t>(selectedElemIdx)];
    }
  }
  const bool canAssignVisual = selectedEl != nullptr &&
      (selectedEl->type == iota::model::ElementType::Image ||
       selectedEl->type == iota::model::ElementType::Particles ||
       selectedEl->type == iota::model::ElementType::RgbSplitEffect ||
       selectedEl->type == iota::model::ElementType::MotionBlurEffect ||
       selectedEl->type == iota::model::ElementType::MirrorEffect ||
       selectedEl->type == iota::model::ElementType::ShakeEffect ||
       selectedEl->type == iota::model::ElementType::TwitchEffect ||
       selectedEl->type == iota::model::ElementType::CustomShader);
  const bool canAssignMask = selectedEl != nullptr &&
      (selectedEl->type == iota::model::ElementType::Image ||
       selectedEl->type == iota::model::ElementType::BlurEffect);

  const bool canAssignShader =
      selectedEl != nullptr && selectedEl->type == iota::model::ElementType::CustomShader;

  // --- After Effects-style Top Preview Header ---
  if (state.selectedKind != BrowserItemKind::None) {
    const auto* selectedBuiltinShader =
        (state.selectedKind == BrowserItemKind::BuiltinShader)
            ? iota::engine::findBuiltinShaderPreset(state.selectedUri)
            : nullptr;
    const bool selectedIsShader =
        (state.selectedKind == BrowserItemKind::LocalImage && isShaderAssetName(state.selectedLabel));
    const bool isVisual = (state.selectedKind == BrowserItemKind::LocalImage && !selectedIsShader) ||
                          state.selectedKind == BrowserItemKind::InternalTexture ||
                          state.selectedKind == BrowserItemKind::CompositionRef;
    const bool validPreviewUri =
        (state.selectedKind != BrowserItemKind::CompositionRef || state.selectedUri != "composition:0");

    const char* badgeTag = "AUD";
    ImU32 badgeCol = IM_COL32(95, 205, 250, 255);
    if (selectedIsShader) {
      badgeTag = "GLSL";
      badgeCol = IM_COL32(250, 195, 90, 255);
    } else if (selectedBuiltinShader != nullptr) {
      badgeTag = "GLSL";
      badgeCol = (selectedBuiltinShader->kind == iota::engine::BuiltinShaderKind::PostFx)
                     ? IM_COL32(250, 195, 90, 255)
                     : IM_COL32(95, 225, 195, 255);
    }

    drawFittedThumbnailBox(device, renderer, project,
                           selectedBuiltinShader ? std::string_view{} : std::string_view(state.selectedUri),
                           56.0f, isVisual && validPreviewUri, badgeTag, badgeCol);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(state.selectedLabel.c_str());

    if (selectedBuiltinShader != nullptr) {
      const char* kindStr =
          (selectedBuiltinShader->kind == iota::engine::BuiltinShaderKind::PostFx) ? "Post-FX" : "Generator";
      const std::string catName(iota::engine::builtinShaderCategoryName(selectedBuiltinShader->category));
      ImGui::TextDisabled("%s  %s  (%s)", catName.c_str(), kindStr,
                          std::string(selectedBuiltinShader->defaultFileName).c_str());
    } else if (selectedIsShader) {
      const auto assetIt = project.localAssets.find(state.selectedLabel);
      const size_t bytes = (assetIt != project.localAssets.end()) ? assetIt->second.size() : 0;
      const int refs = iota::model::countAssetReferences(project, state.selectedUri);
      ImGui::TextDisabled("GLSL Shader  %s  Used:%d", formatBytesShort(bytes).c_str(), refs);
    } else if (state.selectedKind == BrowserItemKind::LocalImage) {
      const auto thumb = renderer.resolveAssetPreview(project, state.selectedUri);
      const auto assetIt = project.localAssets.find(state.selectedLabel);
      const size_t bytes = (assetIt != project.localAssets.end()) ? assetIt->second.size() : 0;
      const int refs = iota::model::countAssetReferences(project, state.selectedUri);
      ImGui::TextDisabled("%dx%d  %s  Used:%d", thumb.width, thumb.height, formatBytesShort(bytes).c_str(), refs);
    } else if (state.selectedKind == BrowserItemKind::InternalTexture) {
      const auto thumb = renderer.resolveAssetPreview(project, state.selectedUri);
      const int refs = iota::model::countAssetReferences(project, state.selectedUri);
      ImGui::TextDisabled("Built-in  %dx%d  Used:%d", thumb.width, thumb.height, refs);
    } else if (state.selectedKind == BrowserItemKind::CompositionRef) {
      const int refs = iota::model::countAssetReferences(project, state.selectedUri);
      ImGui::TextDisabled("Composition  Used:%d", refs);
    } else if (state.selectedKind == BrowserItemKind::AudioTrack) {
      const std::string audioName = iota::core::pathFilenameUtf8(iota::core::pathFromUtf8(state.selectedUri));
      ImGui::TextDisabled("%s", audioName.c_str());
    }

    if (selectedBuiltinShader != nullptr) {
      if (ImGui::SmallButton("Add & Edit")) {
        copyBuiltinShaderAndEdit(project, state, *selectedBuiltinShader, actions);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("+ Layer")) {
        addCustomShaderLayerFromBuiltin(project, selectedCompIdx, *selectedBuiltinShader, actions);
      }
    } else if (selectedIsShader) {
      if (ImGui::SmallButton("Edit")) {
        actions.requestEditShader = true;
        actions.editShaderAssetName = state.selectedLabel;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("+ Shader")) {
        addCustomShaderLayerWithAsset(project, selectedCompIdx, state.selectedUri, state.selectedLabel, actions);
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!canAssignShader);
      if (ImGui::SmallButton("Apply")) {
        applyAssetToSelectedElement(project, selectedCompIdx, selectedElemIdx, state.selectedUri, false, actions);
      }
      ImGui::EndDisabled();
    } else if (isVisual && validPreviewUri) {
      if (ImGui::SmallButton("+ Layer")) {
        addImageLayerWithAsset(project, selectedCompIdx, state.selectedUri, state.selectedLabel, actions);
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!canAssignVisual);
      if (ImGui::SmallButton("Apply")) {
        applyAssetToSelectedElement(project, selectedCompIdx, selectedElemIdx, state.selectedUri, false, actions);
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(!canAssignMask);
      if (ImGui::SmallButton("Mask")) {
        applyAssetToSelectedElement(project, selectedCompIdx, selectedElemIdx, state.selectedUri, true, actions);
      }
      ImGui::EndDisabled();
    } else if (state.selectedKind == BrowserItemKind::AudioTrack) {
      if (ImGui::SmallButton("Load & Play")) {
        actions.requestLoadAudioPath = true;
        actions.loadAudioPath = state.selectedUri;
      }
    } else if (state.selectedKind == BrowserItemKind::CompositionRef) {
      if (ImGui::SmallButton("Open Comp") && state.selectedUri.rfind("composition:", 0) == 0) {
        actions.selectCompIdx = std::atoi(state.selectedUri.c_str() + 12);
        actions.selectElemIdx = -1;
      }
    }
    ImGui::EndGroup();
  } else {
    ImGui::TextDisabled("Select or drag an asset below to preview and apply to layers.");
  }

  ImGui::Separator();

  // --- Toolbar Row 1: Actions & View Mode ---
  if (ImGui::SmallButton("+ Import")) {
    ImGui::OpenPopup("ImportAssetPopup");
  }
  if (ImGui::BeginPopup("ImportAssetPopup")) {
    if (ImGui::MenuItem("Import Images...")) {
      actions.requestImportImages = true;
    }
    if (ImGui::MenuItem("Import Shaders (.glsl)...")) {
      actions.requestImportShaders = true;
    }
    if (ImGui::MenuItem("Import Audio...")) {
      actions.requestImportAudio = true;
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("+ Bin")) {
    state.newFolderBuf[0] = '\0';
    state.openNewFolderPopup = true;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Clean")) {
    if (iota::model::removeUnusedLocalAssets(project) > 0) {
      if (state.selectedKind == BrowserItemKind::LocalImage &&
          project.localAssets.find(state.selectedLabel) == project.localAssets.end()) {
        state.selectedKind = BrowserItemKind::None;
        state.selectedUri.clear();
        state.selectedLabel.clear();
      }
      actions.invalidateLocalAssets = true;
      actions.modified = true;
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Clean Unused Assets";
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Remove unused local assets from project");
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(state.gridView ? "List" : "Grid")) {
    state.gridView = !state.gridView;
  }

  // --- Toolbar Row 2: Category Filter + Search ---
  static constexpr const char* kCatLabels[] = {"All", "Project", "Audio", "Comps", "Built-in"};
  const float catW = std::min(88.0f, std::max(60.0f, ImGui::GetContentRegionAvail().x * 0.38f));
  ImGui::SetNextItemWidth(catW);
  if (ImGui::BeginCombo("##cat", kCatLabels[std::clamp(state.categoryFilter, 0, 4)])) {
    for (int i = 0; i < 5; ++i) {
      if (ImGui::Selectable(kCatLabels[i], state.categoryFilter == i)) {
        state.categoryFilter = i;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputTextWithHint("##asset_search", "Search assets...", state.searchFilter, sizeof(state.searchFilter));

  // Popups for creating folder & renaming asset
  if (state.openNewFolderPopup) {
    ImGui::OpenPopup("NewFolderPopup");
    state.openNewFolderPopup = false;
  }
  if (ImGui::BeginPopup("NewFolderPopup")) {
    ImGui::TextUnformatted("New Asset Bin");
    ImGui::SetNextItemWidth(180.0f);
    const bool enter = ImGui::InputText("##folder_name", state.newFolderBuf, sizeof(state.newFolderBuf),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Create") || enter) {
      std::string fname(state.newFolderBuf);
      if (!fname.empty() &&
          std::find(project.metadata.customFolders.begin(),
                    project.metadata.customFolders.end(), fname) == project.metadata.customFolders.end()) {
        project.metadata.customFolders.push_back(fname);
        actions.modified = true;
        actions.undoSnapshotNeeded = true;
        actions.editLabel = "Create Asset Bin";
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (state.openRenamePopup) {
    ImGui::OpenPopup("RenameAssetPopup");
    state.openRenamePopup = false;
  }
  if (ImGui::BeginPopup("RenameAssetPopup")) {
    ImGui::Text("Rename '%s'", state.renameTarget.c_str());
    ImGui::SetNextItemWidth(220.0f);
    const bool enter = ImGui::InputText("##rename_val", state.renameBuf, sizeof(state.renameBuf),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Rename") || enter) {
      const std::string newName(state.renameBuf);
      if (iota::model::renameLocalAsset(project, state.renameTarget, newName)) {
        if (state.selectedKind == BrowserItemKind::LocalImage && state.selectedLabel == state.renameTarget) {
          state.selectedLabel = newName;
          state.selectedUri = "local:" + newName;
        }
        actions.invalidateLocalAssets = true;
        actions.modified = true;
        actions.undoSnapshotNeeded = true;
        actions.editLabel = "Rename Local Asset";
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // Collect virtual folders cleanly
  std::set<std::string> allFolders;
  for (const auto& f : project.metadata.customFolders) {
    if (!f.empty()) allFolders.insert(f);
  }
  for (const auto& [assetName, folder] : project.metadata.assetFolders) {
    if (!folder.empty() && project.localAssets.find(assetName) != project.localAssets.end()) {
      allFolders.insert(folder);
    }
  }

  const bool showComps = (state.categoryFilter == 0 || state.categoryFilter == 3);
  const bool showLocal = (state.categoryFilter == 0 || state.categoryFilter == 1);
  const bool showAudio = (state.categoryFilter == 0 || state.categoryFilter == 2);
  const bool showBuiltin = (state.categoryFilter == 0 || state.categoryFilter == 4);

  auto drawLocalAssetContextMenu = [&](const std::string& assetName, const std::string& uri) {
    if (!ImGui::BeginPopupContextItem()) return;
    const bool isShader = isShaderAssetName(assetName);
    if (isShader) {
      if (ImGui::MenuItem("Edit in Shader Editor")) {
        actions.requestEditShader = true;
        actions.editShaderAssetName = assetName;
      }
      if (ImGui::MenuItem("Add as CustomShader Layer")) {
        addCustomShaderLayerWithAsset(project, selectedCompIdx, uri, assetName, actions);
      }
      if (ImGui::MenuItem("Assign to Selected CustomShader", nullptr, false, canAssignShader)) {
        applyAssetToSelectedElement(project, selectedCompIdx, selectedElemIdx, uri, false, actions);
      }
    } else {
      if (ImGui::MenuItem("Add as Image Layer")) {
        addImageLayerWithAsset(project, selectedCompIdx, uri, assetName, actions);
      }
      if (ImGui::MenuItem("Assign to Selected Layer", nullptr, false, canAssignVisual)) {
        applyAssetToSelectedElement(project, selectedCompIdx, selectedElemIdx, uri, false, actions);
      }
      if (ImGui::MenuItem("Assign as Mask", nullptr, false, canAssignMask)) {
        applyAssetToSelectedElement(project, selectedCompIdx, selectedElemIdx, uri, true, actions);
      }
    }
    if (ImGui::BeginMenu("Move to Bin")) {
      if (ImGui::MenuItem("(Root)")) {
        project.metadata.assetFolders.erase(assetName);
        actions.modified = true;
        actions.undoSnapshotNeeded = true;
        actions.editLabel = "Move Asset to Root";
      }
      for (const auto& folder : allFolders) {
        if (ImGui::MenuItem(folder.c_str())) {
          project.metadata.assetFolders[assetName] = folder;
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Move Asset to Bin";
        }
      }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Rename...")) {
      state.renameTarget = assetName;
      std::snprintf(state.renameBuf, sizeof(state.renameBuf), "%s", assetName.c_str());
      state.openRenamePopup = true;
    }
    if (ImGui::MenuItem("Replace File...")) {
      actions.requestReplaceLocalAsset = true;
      actions.replaceTargetName = assetName;
    }
    if (ImGui::MenuItem("Delete Asset")) {
      deleteLocalAssetEntry(project, state, assetName, actions);
    }
    ImGui::EndPopup();
  };

  ImGui::BeginChild("##asset_browser_body", ImVec2(0.0f, 0.0f), false);

  if (state.gridView) {
    struct GridItem {
      BrowserItemKind kind;
      std::string label;
      std::string uri;
      bool canDrag;
      bool validThumb;
    };
    std::vector<GridItem> gridItems;

    if (showComps) {
      for (int ci = 0; ci < static_cast<int>(project.compositions.size()); ++ci) {
        const auto& c = project.compositions[static_cast<size_t>(ci)];
        if (!matchesSearch(c.displayName, state.searchFilter)) continue;
        gridItems.push_back({BrowserItemKind::CompositionRef,
                             c.displayName,
                             "composition:" + std::to_string(ci),
                             ci > 0,
                             ci > 0});
      }
    }
    if (showLocal) {
      for (const auto& [name, bytes] : project.localAssets) {
        (void)bytes;
        if (!matchesSearch(name, state.searchFilter)) continue;
        gridItems.push_back({BrowserItemKind::LocalImage, name, "local:" + name, true, !isShaderAssetName(name)});
      }
    }
    if (showAudio) {
      const std::string curPath = audio.currentFilePath();
      if (!curPath.empty()) {
        const std::string title = audio.currentTitle().empty()
                                      ? iota::core::pathFilenameUtf8(iota::core::pathFromUtf8(curPath))
                                      : audio.currentTitle();
        if (matchesSearch(title, state.searchFilter)) {
          gridItems.push_back({BrowserItemKind::AudioTrack, title, curPath, false, false});
        }
      }
      for (const auto& [lbl, fpath] : project.metadata.audioAssets) {
        if (fpath == curPath || !matchesSearch(lbl, state.searchFilter)) continue;
        gridItems.push_back({BrowserItemKind::AudioTrack, lbl, fpath, false, false});
      }
    }
    if (showBuiltin) {
      for (const auto& preset : iota::engine::listBuiltinShaderPresets()) {
        const std::string catName(iota::engine::builtinShaderCategoryName(preset.category));
        if (!matchesSearch(preset.displayName, state.searchFilter) &&
            !matchesSearch(preset.defaultFileName, state.searchFilter) &&
            !matchesSearch(catName, state.searchFilter) &&
            !matchesSearch(preset.description, state.searchFilter)) {
          continue;
        }
        gridItems.push_back({BrowserItemKind::BuiltinShader,
                             std::string(preset.displayName),
                             std::string(preset.id),
                             false,
                             false});
      }
      for (std::string_view uri : iota::core::listInternalResources()) {
        std::string name(uri.substr(12));
        if (!matchesSearch(name, state.searchFilter)) continue;
        gridItems.push_back({BrowserItemKind::InternalTexture, name, std::string(uri), true, true});
      }
    }

    const float cellW = 76.0f;
    const float spacingX = ImGui::GetStyle().ItemSpacing.x;
    const float panelW = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((panelW + spacingX) / (cellW + spacingX)));
    for (int idx = 0; idx < static_cast<int>(gridItems.size()); ++idx) {
      const auto& gi = gridItems[static_cast<size_t>(idx)];
      if (idx % cols != 0) ImGui::SameLine();
      ImGui::PushID(idx);
      ImGui::BeginGroup();
      const bool selected = (state.selectedKind == gi.kind && state.selectedUri == gi.uri);
      const ImVec2 tileOrigin = ImGui::GetCursorScreenPos();
      const auto* giBuiltinShader =
          (gi.kind == BrowserItemKind::BuiltinShader)
              ? iota::engine::findBuiltinShaderPreset(gi.uri)
              : nullptr;
      const bool giIsShader = (gi.kind == BrowserItemKind::LocalImage && isShaderAssetName(gi.label));
      const bool showGlslBadge = giIsShader || giBuiltinShader != nullptr;
      ImU32 giBadgeCol = IM_COL32(95, 205, 250, 255);
      if (giIsShader) {
        giBadgeCol = IM_COL32(250, 195, 90, 255);
      } else if (giBuiltinShader != nullptr) {
        giBadgeCol = (giBuiltinShader->kind == iota::engine::BuiltinShaderKind::PostFx)
                         ? IM_COL32(250, 195, 90, 255)
                         : IM_COL32(95, 225, 195, 255);
      }
      drawFittedThumbnailBox(device, renderer, project, gi.uri, cellW, gi.validThumb,
                             showGlslBadge ? "GLSL" : "AUD", giBadgeCol);
      if (ImGui::IsItemHovered() && giBuiltinShader != nullptr && !giBuiltinShader->description.empty()) {
        ImGui::SetTooltip("%s", std::string(giBuiltinShader->description).c_str());
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.selectedKind = gi.kind;
        state.selectedUri = gi.uri;
        state.selectedLabel = gi.label;
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (gi.kind == BrowserItemKind::AudioTrack) {
          actions.requestLoadAudioPath = true;
          actions.loadAudioPath = gi.uri;
        } else if (gi.kind == BrowserItemKind::CompositionRef) {
          actions.selectCompIdx = std::atoi(gi.uri.c_str() + 12);
          actions.selectElemIdx = -1;
        } else if (giBuiltinShader != nullptr) {
          copyBuiltinShaderAndEdit(project, state, *giBuiltinShader, actions);
        } else if (giIsShader) {
          actions.requestEditShader = true;
          actions.editShaderAssetName = gi.label;
        } else if (canAssignVisual) {
          applyAssetToSelectedElement(project, selectedCompIdx, selectedElemIdx, gi.uri, false, actions);
        } else {
          addImageLayerWithAsset(project, selectedCompIdx, gi.uri, gi.label, actions);
        }
      }
      if (gi.canDrag) {
        pushAssetDragSource(gi.uri, gi.label, device, renderer, project);
      }
      if (gi.kind == BrowserItemKind::LocalImage) {
        drawLocalAssetContextMenu(gi.label, gi.uri);
      } else if (giBuiltinShader != nullptr && ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Copy to Project & Edit")) {
          copyBuiltinShaderAndEdit(project, state, *giBuiltinShader, actions);
        }
        if (ImGui::MenuItem("Add as Layer & Edit")) {
          addCustomShaderLayerFromBuiltin(project, selectedCompIdx, *giBuiltinShader, actions);
        }
        ImGui::EndPopup();
      }
      if (selected) {
        ImGui::GetWindowDrawList()->AddRect(tileOrigin,
                                            ImVec2(tileOrigin.x + cellW, tileOrigin.y + cellW),
                                            IM_COL32(65, 205, 255, 255), 3.0f, 0, 2.0f);
      }
      const std::string shortLbl = iota::core::truncateUtf8(gi.label, 10, "..");
      ImGui::TextUnformatted(shortLbl.c_str());
      ImGui::EndGroup();
      ImGui::PopID();
    }
  } else {
    // --- Tree / List View ---
    if (showComps && ImGui::TreeNodeEx("Compositions", ImGuiTreeNodeFlags_DefaultOpen)) {
      for (int ci = 0; ci < static_cast<int>(project.compositions.size()); ++ci) {
        const auto& c = project.compositions[static_cast<size_t>(ci)];
        if (!matchesSearch(c.displayName, state.searchFilter)) continue;
        const std::string uri = "composition:" + std::to_string(ci);
        ImGui::PushID(ci);
        if (ci > 0) {
          drawFittedThumbnailBox(device, renderer, project, uri, 18.0f, true);
          ImGui::SameLine();
        }
        const bool selected = (state.selectedKind == BrowserItemKind::CompositionRef && state.selectedUri == uri);
        const std::string rowLabel = "[" + std::to_string(ci) + "] " + c.displayName +
                                     " (" + std::to_string(c.elements.size()) + ")";
        if (ImGui::Selectable(rowLabel.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
          state.selectedKind = BrowserItemKind::CompositionRef;
          state.selectedUri = uri;
          state.selectedLabel = c.displayName;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            actions.selectCompIdx = ci;
            actions.selectElemIdx = -1;
          }
        }
        if (ci > 0) {
          pushAssetDragSource(uri, c.displayName, device, renderer, project);
        }
        ImGui::PopID();
      }
      ImGui::TreePop();
    }

    if (showLocal && ImGui::TreeNodeEx("Project Assets", ImGuiTreeNodeFlags_DefaultOpen)) {
      auto renderLocalRow = [&](const std::string& name) {
        const std::string uri = "local:" + name;
        const bool isShader = isShaderAssetName(name);
        const int refs = iota::model::countAssetReferences(project, uri);
        ImGui::PushID(name.c_str());
        drawFittedThumbnailBox(device, renderer, project, uri, 18.0f, !isShader,
                               isShader ? "GLSL" : "AUD",
                               isShader ? IM_COL32(250, 195, 90, 255) : IM_COL32(95, 205, 250, 255));
        ImGui::SameLine();
        const bool selected = (state.selectedKind == BrowserItemKind::LocalImage && state.selectedUri == uri);
        const std::string lbl = name + (refs > 0 ? (" [" + std::to_string(refs) + "]") : "");
        if (ImGui::Selectable(lbl.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
          state.selectedKind = BrowserItemKind::LocalImage;
          state.selectedUri = uri;
          state.selectedLabel = name;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (isShader) {
              actions.requestEditShader = true;
              actions.editShaderAssetName = name;
            } else if (canAssignVisual) {
              applyAssetToSelectedElement(project, selectedCompIdx, selectedElemIdx, uri, false, actions);
            } else {
              addImageLayerWithAsset(project, selectedCompIdx, uri, name, actions);
            }
          }
        }
        pushAssetDragSource(uri, name, device, renderer, project);
        drawLocalAssetContextMenu(name, uri);
        ImGui::PopID();
      };

      for (const auto& folder : allFolders) {
        ImGui::PushID(("bin_" + folder).c_str());
        if (ImGui::TreeNodeEx(folder.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
          for (const auto& [name, bytes] : project.localAssets) {
            (void)bytes;
            auto fit = project.metadata.assetFolders.find(name);
            if (fit == project.metadata.assetFolders.end() || fit->second != folder) continue;
            if (!matchesSearch(name, state.searchFilter)) continue;
            renderLocalRow(name);
          }
          ImGui::TreePop();
        }
        ImGui::PopID();
      }

      bool anyRoot = false;
      for (const auto& [name, bytes] : project.localAssets) {
        (void)bytes;
        auto fit = project.metadata.assetFolders.find(name);
        if (fit != project.metadata.assetFolders.end() && !fit->second.empty()) continue;
        if (!matchesSearch(name, state.searchFilter)) continue;
        anyRoot = true;
        renderLocalRow(name);
      }
      if (!anyRoot && allFolders.empty()) {
        ImGui::TextDisabled("(Empty — click + Import to add images or shaders)");
      }
      ImGui::TreePop();
    }

    if (showAudio && ImGui::TreeNodeEx("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
      const std::string activePath = audio.currentFilePath();
      if (!activePath.empty()) {
        const std::string title = audio.currentTitle().empty()
                                      ? iota::core::pathFilenameUtf8(iota::core::pathFromUtf8(activePath))
                                      : audio.currentTitle();
        if (matchesSearch(title, state.searchFilter)) {
          const bool selected = (state.selectedKind == BrowserItemKind::AudioTrack && state.selectedUri == activePath);
          const std::string lbl = "[Active] " + title;
          if (ImGui::Selectable(lbl.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            state.selectedKind = BrowserItemKind::AudioTrack;
            state.selectedUri = activePath;
            state.selectedLabel = title;
          }
        }
      }
      std::string removeAudioKey;
      for (const auto& [label, fpath] : project.metadata.audioAssets) {
        if (fpath == activePath || !matchesSearch(label, state.searchFilter)) continue;
        ImGui::PushID(label.c_str());
        const bool selected = (state.selectedKind == BrowserItemKind::AudioTrack && state.selectedUri == fpath);
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
          state.selectedKind = BrowserItemKind::AudioTrack;
          state.selectedUri = fpath;
          state.selectedLabel = label;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            actions.requestLoadAudioPath = true;
            actions.loadAudioPath = fpath;
          }
        }
        if (ImGui::BeginPopupContextItem()) {
          if (ImGui::MenuItem("Load & Play")) {
            actions.requestLoadAudioPath = true;
            actions.loadAudioPath = fpath;
          }
          if (ImGui::MenuItem("Remove from Project")) {
            removeAudioKey = label;
          }
          ImGui::EndPopup();
        }
        ImGui::PopID();
      }
      if (!removeAudioKey.empty()) {
        project.metadata.audioAssets.erase(removeAudioKey);
        actions.modified = true;
        actions.undoSnapshotNeeded = true;
        actions.editLabel = "Remove Audio Reference";
      }
      ImGui::TreePop();
    }

    if (showBuiltin && ImGui::TreeNodeEx("Built-in Shaders", ImGuiTreeNodeFlags_DefaultOpen)) {
      for (const auto& preset : iota::engine::listBuiltinShaderPresets()) {
        const std::string catName(iota::engine::builtinShaderCategoryName(preset.category));
        if (!matchesSearch(preset.displayName, state.searchFilter) &&
            !matchesSearch(preset.defaultFileName, state.searchFilter) &&
            !matchesSearch(catName, state.searchFilter) &&
            !matchesSearch(preset.description, state.searchFilter)) {
          continue;
        }
        ImGui::PushID(preset.id.data(), preset.id.data() + preset.id.size());
        const bool isPostFx = (preset.kind == iota::engine::BuiltinShaderKind::PostFx);
        drawFittedThumbnailBox(device, renderer, project, {}, 18.0f, false, "GLSL",
                               isPostFx ? IM_COL32(250, 195, 90, 255) : IM_COL32(95, 225, 195, 255));
        ImGui::SameLine();
        const bool selected = (state.selectedKind == BrowserItemKind::BuiltinShader &&
                               state.selectedUri == preset.id);
        const std::string rowLbl =
            std::string(preset.displayName) + (isPostFx ? "  [Post-FX]" : "  [BG]");
        if (ImGui::Selectable(rowLbl.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
          state.selectedKind = BrowserItemKind::BuiltinShader;
          state.selectedUri = std::string(preset.id);
          state.selectedLabel = std::string(preset.displayName);
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            copyBuiltinShaderAndEdit(project, state, preset, actions);
          }
        }
        if (ImGui::IsItemHovered() && !preset.description.empty()) {
          ImGui::SetTooltip("%s\n(%s - double-click to copy to project & edit)",
                            std::string(preset.description).c_str(), catName.c_str());
        }
        if (ImGui::BeginPopupContextItem()) {
          if (ImGui::MenuItem("Copy to Project & Edit")) {
            copyBuiltinShaderAndEdit(project, state, preset, actions);
          }
          if (ImGui::MenuItem("Add as Layer & Edit")) {
            addCustomShaderLayerFromBuiltin(project, selectedCompIdx, preset, actions);
          }
          ImGui::EndPopup();
        }
        ImGui::PopID();
      }
      ImGui::TreePop();
    }

    if (showBuiltin && ImGui::TreeNodeEx("Built-in Textures", 0)) {
      for (std::string_view uri : iota::core::listInternalResources()) {
        const std::string name(uri.substr(12));
        if (!matchesSearch(name, state.searchFilter)) continue;
        const std::string uriStr(uri);
        ImGui::PushID(name.c_str());
        drawFittedThumbnailBox(device, renderer, project, uriStr, 18.0f, true);
        ImGui::SameLine();
        const bool selected = (state.selectedKind == BrowserItemKind::InternalTexture && state.selectedUri == uriStr);
        if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
          state.selectedKind = BrowserItemKind::InternalTexture;
          state.selectedUri = uriStr;
          state.selectedLabel = name;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (canAssignVisual) {
              applyAssetToSelectedElement(project, selectedCompIdx, selectedElemIdx, uriStr, false, actions);
            } else {
              addImageLayerWithAsset(project, selectedCompIdx, uriStr, name, actions);
            }
          }
        }
        pushAssetDragSource(uriStr, name, device, renderer, project);
        ImGui::PopID();
      }
      ImGui::TreePop();
    }
  }

  ImGui::EndChild();
  ImGui::End();
}

}  // namespace iota::studio
