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

#include "hierarchy_panel.hpp"

#include "asset_browser_panel.hpp"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

namespace iota::studio {

namespace {

std::string toLowerCopy(std::string_view sv) {
  std::string out(sv);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool matchesFilter(std::string_view displayName, std::string_view typeName, const char* needleBuf) {
  if (needleBuf == nullptr || *needleBuf == '\0') {
    return true;
  }
  const std::string needle = toLowerCopy(needleBuf);
  return toLowerCopy(displayName).find(needle) != std::string::npos ||
         toLowerCopy(typeName).find(needle) != std::string::npos;
}

iota::model::Element createConfiguredElementForComp(const iota::model::Project& project,
                                                    iota::model::ElementType et) {
  auto newEl = iota::model::createDefaultElement(et);
  iota::model::assignFreshElementId(project, newEl);
  if (project.compositions.size() == 1) {
    if (et == iota::model::ElementType::BlurEffect) {
      newEl.props.setInt("sourceCompositionIndex", 0, 0, 5, "1_appearance");
      newEl.props.setString("blendMode", "Alpha",
                            "_child Alpha PreMulAlpha Screen Add AddAlpha Opaque", "0_general");
      newEl.props.setBool("showUnblurredContent", false, "1_appearance");
    } else if (et == iota::model::ElementType::MotionBlurEffect) {
      newEl.props.setString("TargetImage", "composition:0", "img composition:0", "1_appearance");
      newEl.props.setString("motionSource", "Temporal",
                            "_child EffectTransform Manual Temporal Hybrid", "2_motionBlur");
      newEl.props.setString("blendMode", "Opaque",
                            "_child Alpha PreMulAlpha Screen Add AddAlpha Opaque", "0_general");
    }
  }
  return newEl;
}

bool drawCategorizedAddElementMenu(iota::model::ElementType& outSelected) {
  struct CategoryGroup {
    const char* header;
    std::vector<iota::model::ElementType> types;
  };
  static const CategoryGroup kGroups[] = {
      {"Audio & Analysis",
       {iota::model::ElementType::AudioProvider, iota::model::ElementType::SoundKeys}},
      {"Visual Layers",
       {iota::model::ElementType::Image, iota::model::ElementType::Bars,
        iota::model::ElementType::Particles, iota::model::ElementType::Text}},
      {"Effects & Shaders",
       {iota::model::ElementType::BlurEffect, iota::model::ElementType::RgbSplitEffect,
        iota::model::ElementType::MotionBlurEffect, iota::model::ElementType::ShakeEffect,
        iota::model::ElementType::MirrorEffect, iota::model::ElementType::CustomShader}},
  };

  for (size_t gi = 0; gi < std::size(kGroups); ++gi) {
    if (gi > 0) ImGui::Separator();
    ImGui::TextDisabled("%s", kGroups[gi].header);
    for (iota::model::ElementType et : kGroups[gi].types) {
      const std::string label(iota::model::elementTypeToString(et));
      if (ImGui::MenuItem(label.c_str())) {
        outSelected = et;
        return true;
      }
    }
  }
  return false;
}

}  // namespace

void HierarchyPanelState::selectComposition(int ci) {
  primaryCompIdx = ci;
  primaryElemIdx = -1;
  selectedElements.clear();
  selectionAnchor = {ci, -1};
}

void HierarchyPanelState::selectSingleElement(int ci, int ei) {
  if (ei < 0) {
    selectComposition(ci);
    return;
  }
  primaryCompIdx = ci;
  primaryElemIdx = ei;
  selectedElements = {{ci, ei}};
  selectionAnchor = {ci, ei};
}

void HierarchyPanelState::setSelection(const std::vector<iota::model::ElementRef>& refs, int fallbackComp) {
  selectedElements.clear();
  for (const auto& r : refs) {
    if (r.compIdx >= 0 && r.elemIdx >= 0) {
      selectedElements.push_back(r);
    }
  }
  std::sort(selectedElements.begin(), selectedElements.end());
  selectedElements.erase(std::unique(selectedElements.begin(), selectedElements.end()),
                         selectedElements.end());
  if (selectedElements.empty()) {
    selectComposition(fallbackComp);
  } else {
    primaryCompIdx = selectedElements.front().compIdx;
    primaryElemIdx = selectedElements.front().elemIdx;
    selectionAnchor = selectedElements.front();
  }
}

bool HierarchyPanelState::isElementSelected(int ci, int ei) const noexcept {
  for (const auto& r : selectedElements) {
    if (r.compIdx == ci && r.elemIdx == ei) return true;
  }
  return false;
}

bool HierarchyPanelState::allSelectedInSameComp(int* outCompIdx) const noexcept {
  if (selectedElements.empty()) return false;
  const int comp = selectedElements.front().compIdx;
  for (const auto& r : selectedElements) {
    if (r.compIdx != comp) return false;
  }
  if (outCompIdx) *outCompIdx = comp;
  return true;
}

void HierarchyPanelState::clampToProject(const iota::model::Project& project) {
  const int nComps = static_cast<int>(project.compositions.size());
  if (nComps <= 0) {
    primaryCompIdx = -1;
    primaryElemIdx = -1;
    selectedElements.clear();
    selectionAnchor = {-1, -1};
    renaming = false;
    return;
  }

  selectedElements.erase(
      std::remove_if(selectedElements.begin(), selectedElements.end(),
                     [&](const iota::model::ElementRef& r) {
                       return r.compIdx < 0 || r.compIdx >= nComps || r.elemIdx < 0 ||
                              r.elemIdx >= static_cast<int>(
                                               project.compositions[static_cast<size_t>(r.compIdx)].elements.size());
                     }),
      selectedElements.end());

  primaryCompIdx = std::clamp(primaryCompIdx, 0, nComps - 1);
  const int primaryCompSize =
      static_cast<int>(project.compositions[static_cast<size_t>(primaryCompIdx)].elements.size());

  if (!selectedElements.empty()) {
    if (!isElementSelected(primaryCompIdx, primaryElemIdx)) {
      primaryCompIdx = selectedElements.front().compIdx;
      primaryElemIdx = selectedElements.front().elemIdx;
    }
  } else if (primaryElemIdx >= 0) {
    if (primaryCompSize <= 0) {
      primaryElemIdx = -1;
    } else {
      primaryElemIdx = std::clamp(primaryElemIdx, 0, primaryCompSize - 1);
      selectedElements.push_back({primaryCompIdx, primaryElemIdx});
    }
  }

  if (selectionAnchor.compIdx < 0 || selectionAnchor.compIdx >= nComps ||
      selectionAnchor.elemIdx >=
          static_cast<int>(project.compositions[static_cast<size_t>(selectionAnchor.compIdx)].elements.size())) {
    selectionAnchor = {primaryCompIdx, primaryElemIdx};
  }

  if (renaming) {
    if (renameTarget.compIdx < 0 || renameTarget.compIdx >= nComps) {
      renaming = false;
    } else if (!renamingComp &&
               (renameTarget.elemIdx < 0 ||
                renameTarget.elemIdx >=
                    static_cast<int>(
                        project.compositions[static_cast<size_t>(renameTarget.compIdx)].elements.size()))) {
      renaming = false;
    }
  }
}

void HierarchyPanelState::beginRenameCurrent(const iota::model::Project& project) {
  const int nComps = static_cast<int>(project.compositions.size());
  if (primaryCompIdx < 0 || primaryCompIdx >= nComps) return;
  const auto& comp = project.compositions[static_cast<size_t>(primaryCompIdx)];
  if (primaryElemIdx >= 0 && primaryElemIdx < static_cast<int>(comp.elements.size())) {
    renaming = true;
    renamingComp = false;
    renameTarget = {primaryCompIdx, primaryElemIdx};
    renameFocusPending = true;
    std::snprintf(renameBuf, sizeof(renameBuf), "%s",
                  comp.elements[static_cast<size_t>(primaryElemIdx)].displayName.c_str());
  } else {
    renaming = true;
    renamingComp = true;
    renameTarget = {primaryCompIdx, -1};
    renameFocusPending = true;
    std::snprintf(renameBuf, sizeof(renameBuf), "%s", comp.displayName.c_str());
  }
}

void renderHierarchyPanel(iota::model::Project& project,
                          HierarchyPanelState& state,
                          bool hasClipboard,
                          HierarchyActions& actions) {
  ImGui::Begin("Hierarchy");
  state.windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  state.clampToProject(project);

  // Top toolbar
  if (ImGui::Button("+ Comp")) {
    const int newIdx = iota::model::addComposition(project);
    state.selectComposition(newIdx);
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Add Composition";
    actions.statusMessage = "Added Composition " + std::to_string(newIdx);
  }
  ImGui::SameLine();
  const bool canAddElem =
      state.primaryCompIdx >= 0 && state.primaryCompIdx < static_cast<int>(project.compositions.size());
  ImGui::BeginDisabled(!canAddElem);
  if (ImGui::Button("+ Layer")) {
    ImGui::OpenPopup("AddElementPopup");
  }
  ImGui::EndDisabled();

  if (ImGui::BeginPopup("AddElementPopup")) {
    iota::model::ElementType chosen = iota::model::ElementType::Unknown;
    if (drawCategorizedAddElementMenu(chosen) && canAddElem) {
      auto& elems = project.compositions[static_cast<size_t>(state.primaryCompIdx)].elements;
      elems.push_back(createConfiguredElementForComp(project, chosen));
      const int insertedIdx = static_cast<int>(elems.size()) - 1;
      state.selectSingleElement(state.primaryCompIdx, insertedIdx);
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Add " + std::string(iota::model::elementTypeToString(chosen));
      actions.statusMessage = actions.editLabel;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::SetNextItemWidth(-26.0f);
  ImGui::InputTextWithHint("##HierSearch", "Filter layers...", state.searchFilter, sizeof(state.searchFilter));
  ImGui::SameLine();
  if (ImGui::SmallButton("X##ClearSearch")) {
    state.searchFilter[0] = '\0';
  }

  if (state.selectedElements.size() > 1) {
    int sameComp = -1;
    const bool singleCompSel = state.allSelectedInSameComp(&sameComp);
    ImGui::TextDisabled("%zu selected", state.selectedElements.size());
    ImGui::SameLine();
    if (singleCompSel) {
      if (ImGui::SmallButton("Pre-Comp")) actions.requestPrecompose = true;
      ImGui::SameLine();
    }
    if (ImGui::SmallButton("Copy")) actions.requestCopy = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Del")) actions.requestDelete = true;
  }

  ImGui::Separator();

  // Deferred mutations executed safely after iterating over compositions & elements
  struct PendingEdits {
    bool moveLayers = false;
    std::vector<iota::model::ElementRef> moveSrcs;
    int moveDstComp = -1;
    int moveDstIdx = -1;

    bool moveToNewComp = false;
    std::vector<iota::model::ElementRef> moveNewCompSrcs;

    int nudgeDelta = 0;
    int boundaryDir = 0;

    int moveCompFrom = -1;
    int moveCompTo = -1;
    int compToDelete = -1;
    int compToDup = -1;

    bool inlineRename = false;
    bool renameIsComp = false;
    iota::model::ElementRef renameRef{};
    std::string renameValue;

    bool soloToggle = false;
    int soloCompIdx = -1;
    std::vector<int> soloElems;

    bool batchVis = false;
    std::vector<iota::model::ElementRef> batchVisTargets;
    bool batchVisVal = true;

    int addToCompIdx = -1;
    iota::model::ElementType addElemType = iota::model::ElementType::Unknown;

    bool assetDrop = false;
    int assetDropComp = -1;
    int assetDropElem = -1;
    std::string assetDropUri;
  } pending;

  const int numComps = static_cast<int>(project.compositions.size());
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const ImGuiIO& io = ImGui::GetIO();

  for (int ci = 0; ci < numComps; ++ci) {
    auto& comp = project.compositions[static_cast<size_t>(ci)];
    ImGui::PushID(ci);

    const bool compSelected = (state.primaryCompIdx == ci && state.selectedElements.empty() && state.primaryElemIdx < 0);
    const bool renamingThisComp = (state.renaming && state.renamingComp && state.renameTarget.compIdx == ci);

    const ImGuiTreeNodeFlags nodeFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
        (compSelected ? ImGuiTreeNodeFlags_Selected : 0);

    bool open = false;
    if (renamingThisComp) {
      open = ImGui::TreeNodeEx("##CompTree", nodeFlags, "[%d]", ci);
      ImGui::SameLine();
      if (state.renameFocusPending) {
        ImGui::SetKeyboardFocusHere();
        state.renameFocusPending = false;
      }
      ImGui::SetNextItemWidth(-FLT_MIN);
      const bool enterPressed = ImGui::InputText(
          "##RenameComp", state.renameBuf, sizeof(state.renameBuf),
          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        state.renaming = false;
        state.renameFocusPending = false;
      } else if (enterPressed || ImGui::IsItemDeactivatedAfterEdit()) {
        pending.inlineRename = true;
        pending.renameIsComp = true;
        pending.renameRef = {ci, -1};
        pending.renameValue = state.renameBuf;
        state.renaming = false;
      }
    } else {
      open = ImGui::TreeNodeEx("##CompTree", nodeFlags, "[%d] %s", ci, comp.displayName.c_str());
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        state.selectComposition(ci);
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        state.selectComposition(ci);
        state.beginRenameCurrent(project);
      }
    }

    // Secondary compositions can be dragged to reorder
    if (ci >= 1 && !renamingThisComp && ImGui::BeginDragDropSource()) {
      int srcComp = ci;
      ImGui::SetDragDropPayload(kCompDragDropPayload, &srcComp, sizeof(srcComp));
      ImGui::Text("Reorder [%d] %s", ci, comp.displayName.c_str());
      ImGui::EndDragDropSource();
    }

    // Drop target on composition header (accepts layers, secondary comp reorder, or assets)
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kLayerDragDropPayload)) {
        (void)payload;
        if (!state.activeDragElements.empty()) {
          pending.moveLayers = true;
          pending.moveSrcs = state.activeDragElements;
          pending.moveDstComp = ci;
          pending.moveDstIdx = -1;
        }
      }
      if (ci >= 1) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kCompDragDropPayload)) {
          if (payload->DataSize == sizeof(int)) {
            const int srcComp = *static_cast<const int*>(payload->Data);
            if (srcComp >= 1 && srcComp != ci) {
              pending.moveCompFrom = srcComp;
              pending.moveCompTo = ci;
            }
          }
        }
      }
      if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragDropPayload)) {
        const char* dropped = static_cast<const char*>(payload->Data);
        if (dropped != nullptr && *dropped != '\0') {
          pending.assetDrop = true;
          pending.assetDropComp = ci;
          pending.assetDropElem = -1;
          pending.assetDropUri = dropped;
        }
      }
      ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem("CompCtx")) {
      if (ImGui::BeginMenu("Add Layer")) {
        iota::model::ElementType chosen = iota::model::ElementType::Unknown;
        if (drawCategorizedAddElementMenu(chosen)) {
          pending.addToCompIdx = ci;
          pending.addElemType = chosen;
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Paste Into Composition\tCtrl+V", nullptr, false, hasClipboard)) {
        state.selectComposition(ci);
        actions.requestPaste = true;
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Rename Composition\tF2")) {
        state.selectComposition(ci);
        state.beginRenameCurrent(project);
      }
      if (ImGui::MenuItem("Copy Composition\tCtrl+C")) {
        state.selectComposition(ci);
        actions.requestCopy = true;
      }
      if (ImGui::MenuItem("Duplicate Composition\tCtrl+D")) {
        pending.compToDup = ci;
      }
      if (ci >= 1) {
        ImGui::Separator();
        if (ImGui::MenuItem("Move Comp Up", nullptr, false, ci > 1)) {
          pending.moveCompFrom = ci;
          pending.moveCompTo = ci - 1;
        }
        if (ImGui::MenuItem("Move Comp Down", nullptr, false, ci + 1 < numComps)) {
          pending.moveCompFrom = ci;
          pending.moveCompTo = ci + 1;
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Delete Composition", nullptr, false, ci > 0)) {
        pending.compToDelete = ci;
      }
      ImGui::EndPopup();
    }

    if (open) {
      const int nElems = static_cast<int>(comp.elements.size());
      if (nElems == 0) {
        ImGui::TextDisabled("  (empty - drop layers or assets here)");
        if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kLayerDragDropPayload)) {
            (void)payload;
            if (!state.activeDragElements.empty()) {
              pending.moveLayers = true;
              pending.moveSrcs = state.activeDragElements;
              pending.moveDstComp = ci;
              pending.moveDstIdx = 0;
            }
          }
          if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragDropPayload)) {
            const char* dropped = static_cast<const char*>(payload->Data);
            if (dropped != nullptr && *dropped != '\0') {
              pending.assetDrop = true;
              pending.assetDropComp = ci;
              pending.assetDropElem = -1;
              pending.assetDropUri = dropped;
            }
          }
          ImGui::EndDragDropTarget();
        }
      }

      for (int ei = 0; ei < nElems; ++ei) {
        auto& el = comp.elements[static_cast<size_t>(ei)];
        if (!matchesFilter(el.displayName, el.typeName, state.searchFilter)) {
          continue;
        }

        ImGui::PushID(ei);
        const bool isSelected = state.isElementSelected(ci, ei);
        const bool isVisual =
            (el.type != iota::model::ElementType::AudioProvider && el.type != iota::model::ElementType::SoundKeys);
        bool vis = el.props.getBool("visible", true);

        if (el.type != iota::model::ElementType::AudioProvider) {
          if (ImGui::Checkbox("##vis", &vis)) {
            if (isSelected && state.selectedElements.size() > 1) {
              pending.batchVis = true;
              pending.batchVisTargets = state.selectedElements;
              pending.batchVisVal = vis;
            } else {
              el.props.setBool("visible", vis, "0_general",
                               el.type == iota::model::ElementType::AppLogo ? "pb" : "b");
              actions.undoSnapshotNeeded = true;
              actions.editLabel = vis ? "Show Layer" : "Hide Layer";
            }
          }
          ImGui::SameLine(0.0f, 3.0f);
        }

        if (isVisual) {
          const bool soloed = iota::model::isElementSoloedInComposition(project, ci, ei);
          if (soloed) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.82f, 0.56f, 0.14f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.65f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
          } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.20f, 0.24f, 0.70f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.36f, 0.44f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.74f, 0.80f, 0.85f));
          }
          if (ImGui::SmallButton("S##solo")) {
            pending.soloToggle = true;
            pending.soloCompIdx = ci;
            int selComp = -1;
            if (isSelected && state.allSelectedInSameComp(&selComp) && selComp == ci) {
              for (const auto& r : state.selectedElements) {
                pending.soloElems.push_back(r.elemIdx);
              }
            } else {
              pending.soloElems = {ei};
            }
          }
          ImGui::PopStyleColor(3);
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Solo / Unsolo visual layer");
          }
          ImGui::SameLine(0.0f, 5.0f);
        }

        const bool renamingThisElem =
            (state.renaming && !state.renamingComp && state.renameTarget == iota::model::ElementRef{ci, ei});

        if (renamingThisElem) {
          if (state.renameFocusPending) {
            ImGui::SetKeyboardFocusHere();
            state.renameFocusPending = false;
          }
          ImGui::SetNextItemWidth(-FLT_MIN);
          const bool enterPressed = ImGui::InputText(
              "##RenameElem", state.renameBuf, sizeof(state.renameBuf),
              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
          if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            state.renaming = false;
            state.renameFocusPending = false;
          } else if (enterPressed || ImGui::IsItemDeactivatedAfterEdit()) {
            pending.inlineRename = true;
            pending.renameIsComp = false;
            pending.renameRef = {ci, ei};
            pending.renameValue = state.renameBuf;
            state.renaming = false;
          }
        } else {
          const std::string elLabel = el.displayName.empty()
                                          ? el.typeName
                                          : (el.displayName + " (" + el.typeName + ")");
          const bool dimRow = (!vis && el.type != iota::model::ElementType::AudioProvider);
          if (dimRow) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.64f, 0.72f));
          }
          const bool rowClicked = ImGui::Selectable(elLabel.c_str(), isSelected);
          if (dimRow) {
            ImGui::PopStyleColor();
          }

          if (rowClicked) {
            if (io.KeyCtrl) {
              if (isSelected) {
                std::vector<iota::model::ElementRef> next = state.selectedElements;
                next.erase(std::remove(next.begin(), next.end(), iota::model::ElementRef{ci, ei}), next.end());
                state.setSelection(next, ci);
              } else {
                auto next = state.selectedElements;
                next.push_back({ci, ei});
                state.setSelection(next, ci);
                state.primaryCompIdx = ci;
                state.primaryElemIdx = ei;
                state.selectionAnchor = {ci, ei};
              }
            } else if (io.KeyShift &&
                       state.selectionAnchor.compIdx == ci &&
                       state.selectionAnchor.elemIdx >= 0 &&
                       state.selectionAnchor.elemIdx < nElems) {
              const int a = std::min(state.selectionAnchor.elemIdx, ei);
              const int b = std::max(state.selectionAnchor.elemIdx, ei);
              std::vector<iota::model::ElementRef> range;
              range.reserve(static_cast<size_t>(b - a + 1));
              for (int k = a; k <= b; ++k) {
                range.push_back({ci, k});
              }
              const auto savedAnchor = state.selectionAnchor;
              state.setSelection(range, ci);
              state.primaryCompIdx = ci;
              state.primaryElemIdx = ei;
              state.selectionAnchor = savedAnchor;
            } else {
              state.selectSingleElement(ci, ei);
            }
          }

          if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            state.selectSingleElement(ci, ei);
            state.beginRenameCurrent(project);
          }

          if (ImGui::BeginDragDropSource()) {
            if (isSelected && !state.selectedElements.empty()) {
              state.activeDragElements = state.selectedElements;
            } else {
              state.activeDragElements = {{ci, ei}};
            }
            const int dragToken = 1;
            ImGui::SetDragDropPayload(kLayerDragDropPayload, &dragToken, sizeof(dragToken));
            if (state.activeDragElements.size() > 1) {
              ImGui::Text("Move %zu layers", state.activeDragElements.size());
            } else {
              ImGui::Text("Move %s", elLabel.c_str());
            }
            ImGui::EndDragDropSource();
          }

          if (ImGui::BeginDragDropTarget()) {
            const ImVec2 rMin = ImGui::GetItemRectMin();
            const ImVec2 rMax = ImGui::GetItemRectMax();
            const bool insertAfter = (ImGui::GetMousePos().y >= 0.5f * (rMin.y + rMax.y));
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    kLayerDragDropPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
              (void)payload;
              if (!state.activeDragElements.empty()) {
                const float lineY = insertAfter ? rMax.y : rMin.y;
                drawList->AddLine(ImVec2(rMin.x, lineY), ImVec2(rMax.x, lineY),
                                  IM_COL32(82, 178, 255, 255), 2.5f);
                if (payload->IsDelivery()) {
                  pending.moveLayers = true;
                  pending.moveSrcs = state.activeDragElements;
                  pending.moveDstComp = ci;
                  pending.moveDstIdx = insertAfter ? (ei + 1) : ei;
                }
              }
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragDropPayload)) {
              const char* dropped = static_cast<const char*>(payload->Data);
              if (dropped != nullptr && *dropped != '\0') {
                pending.assetDrop = true;
                pending.assetDropComp = ci;
                pending.assetDropElem = ei;
                pending.assetDropUri = dropped;
              }
            }
            ImGui::EndDragDropTarget();
          }

          if (ImGui::BeginPopupContextItem("ElemCtx")) {
            if (!isSelected) {
              state.selectSingleElement(ci, ei);
            }

            if (ImGui::MenuItem("Cut\tCtrl+X")) actions.requestCut = true;
            if (ImGui::MenuItem("Copy\tCtrl+C")) actions.requestCopy = true;
            if (ImGui::MenuItem("Paste\tCtrl+V", nullptr, false, hasClipboard)) actions.requestPaste = true;
            if (ImGui::MenuItem("Duplicate\tCtrl+D")) actions.requestDuplicate = true;
            if (ImGui::MenuItem("Rename\tF2")) {
              state.selectSingleElement(ci, ei);
              state.beginRenameCurrent(project);
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Move Up\tCtrl+Up", nullptr, false, ei > 0 || state.selectedElements.size() > 1)) {
              pending.nudgeDelta = -1;
            }
            if (ImGui::MenuItem("Move Down\tCtrl+Down", nullptr, false, ei + 1 < nElems || state.selectedElements.size() > 1)) {
              pending.nudgeDelta = +1;
            }
            if (ImGui::MenuItem("Move to Top\tCtrl+Shift+Up")) {
              pending.boundaryDir = -1;
            }
            if (ImGui::MenuItem("Move to Bottom\tCtrl+Shift+Down")) {
              pending.boundaryDir = +1;
            }

            if (ImGui::BeginMenu("Move to Composition")) {
              int sameSelComp = -1;
              const bool oneCompSelected = state.allSelectedInSameComp(&sameSelComp);
              for (int dstCi = 0; dstCi < numComps; ++dstCi) {
                const auto& dstComp = project.compositions[static_cast<size_t>(dstCi)];
                const std::string itemLabel = "[" + std::to_string(dstCi) + "] " + dstComp.displayName;
                const bool sameTarget = (oneCompSelected && sameSelComp == dstCi);
                if (ImGui::MenuItem(itemLabel.c_str(), nullptr, false, !sameTarget)) {
                  pending.moveLayers = true;
                  pending.moveSrcs = state.selectedElements;
                  pending.moveDstComp = dstCi;
                  pending.moveDstIdx = -1;
                }
              }
              ImGui::Separator();
              if (ImGui::MenuItem("New Composition...")) {
                pending.moveToNewComp = true;
                pending.moveNewCompSrcs = state.selectedElements;
              }
              ImGui::EndMenu();
            }

            int selComp = -1;
            const bool canPrecomp = state.allSelectedInSameComp(&selComp) && !state.selectedElements.empty();
            if (ImGui::MenuItem("Pre-Compose Selected...", nullptr, false, canPrecomp)) {
              actions.requestPrecompose = true;
            }

            if (isVisual) {
              if (ImGui::MenuItem("Solo / Unsolo Visual Layer")) {
                pending.soloToggle = true;
                pending.soloCompIdx = ci;
                if (canPrecomp && selComp == ci) {
                  for (const auto& r : state.selectedElements) pending.soloElems.push_back(r.elemIdx);
                } else {
                  pending.soloElems = {ei};
                }
              }
            }

            if (state.selectedElements.size() > 1) {
              if (ImGui::MenuItem("Show Selected")) {
                pending.batchVis = true;
                pending.batchVisTargets = state.selectedElements;
                pending.batchVisVal = true;
              }
              if (ImGui::MenuItem("Hide Selected")) {
                pending.batchVis = true;
                pending.batchVisTargets = state.selectedElements;
                pending.batchVisVal = false;
              }
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Delete\tDel")) {
              actions.requestDelete = true;
            }
            ImGui::EndPopup();
          }
        }

        ImGui::PopID();
      }

      ImGui::TreePop();
    }
    ImGui::PopID();
  }

  // Apply deferred mutations cleanly in safe post-loop order
  if (pending.addToCompIdx >= 0 && pending.addToCompIdx < static_cast<int>(project.compositions.size())) {
    auto& elems = project.compositions[static_cast<size_t>(pending.addToCompIdx)].elements;
    elems.push_back(createConfiguredElementForComp(project, pending.addElemType));
    state.selectSingleElement(pending.addToCompIdx, static_cast<int>(elems.size()) - 1);
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Add " + std::string(iota::model::elementTypeToString(pending.addElemType));
    actions.statusMessage = actions.editLabel;
  } else if (pending.moveToNewComp && !pending.moveNewCompSrcs.empty()) {
    const int newCompIdx = iota::model::addComposition(project);
    const auto moved = iota::model::moveElements(project, pending.moveNewCompSrcs, newCompIdx, 0);
    state.setSelection(moved, newCompIdx);
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Move Layers to New Comp";
    actions.statusMessage = "Moved " + std::to_string(moved.size()) + " layer(s) to Composition " + std::to_string(newCompIdx);
  } else if (pending.moveLayers && !pending.moveSrcs.empty()) {
    const auto moved = iota::model::moveElements(project, pending.moveSrcs, pending.moveDstComp, pending.moveDstIdx);
    if (!moved.empty()) {
      state.setSelection(moved, pending.moveDstComp);
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Move Layers";
      actions.statusMessage = "Moved " + std::to_string(moved.size()) + " layer(s) to Composition " + std::to_string(pending.moveDstComp);
    }
  } else if (pending.nudgeDelta != 0 && !state.selectedElements.empty()) {
    const auto moved = iota::model::nudgeElements(project, state.selectedElements, pending.nudgeDelta);
    if (moved != state.selectedElements) {
      state.setSelection(moved, state.primaryCompIdx);
      actions.undoSnapshotNeeded = true;
      actions.editLabel = pending.nudgeDelta < 0 ? "Move Layer Up" : "Move Layer Down";
    }
  } else if (pending.boundaryDir != 0 && !state.selectedElements.empty()) {
    const auto moved = iota::model::moveElementsToBoundary(project, state.selectedElements, pending.boundaryDir < 0);
    if (!moved.empty() && moved != state.selectedElements) {
      state.setSelection(moved, state.primaryCompIdx);
      actions.undoSnapshotNeeded = true;
      actions.editLabel = pending.boundaryDir < 0 ? "Move Layer to Top" : "Move Layer to Bottom";
    }
  } else if (pending.moveCompFrom >= 1 && pending.moveCompTo >= 1) {
    if (iota::model::moveComposition(project, pending.moveCompFrom, pending.moveCompTo)) {
      state.selectComposition(pending.moveCompTo);
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Reorder Composition";
      actions.statusMessage = "Reordered Composition to index " + std::to_string(pending.moveCompTo);
    }
  } else if (pending.compToDup >= 0) {
    const int newCi = iota::model::duplicateComposition(project, pending.compToDup);
    if (newCi >= 0) {
      state.selectComposition(newCi);
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Duplicate Composition";
      actions.statusMessage = "Duplicated Composition [" + std::to_string(newCi) + "]";
    }
  } else if (pending.compToDelete > 0) {
    if (iota::model::deleteComposition(project, pending.compToDelete)) {
      const int nextComp = std::clamp(pending.compToDelete - 1, 0,
                                      std::max(0, static_cast<int>(project.compositions.size()) - 1));
      state.selectComposition(nextComp);
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Delete Composition";
      actions.statusMessage = "Deleted Composition";
    }
  } else if (pending.inlineRename) {
    bool ok = false;
    if (pending.renameIsComp) {
      ok = iota::model::renameComposition(project, pending.renameRef.compIdx, pending.renameValue);
      actions.editLabel = "Rename Composition";
    } else {
      ok = iota::model::renameElement(project, pending.renameRef, pending.renameValue);
      actions.editLabel = "Rename Layer";
    }
    if (ok) {
      actions.undoSnapshotNeeded = true;
    }
  } else if (pending.soloToggle && pending.soloCompIdx >= 0) {
    if (iota::model::soloElementsInComposition(project, pending.soloCompIdx, pending.soloElems)) {
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Toggle Solo";
    }
  } else if (pending.batchVis && !pending.batchVisTargets.empty()) {
    if (iota::model::setElementsVisible(project, pending.batchVisTargets, pending.batchVisVal)) {
      actions.undoSnapshotNeeded = true;
      actions.editLabel = pending.batchVisVal ? "Show Selected Layers" : "Hide Selected Layers";
    }
  } else if (pending.assetDrop && !pending.assetDropUri.empty() &&
             pending.assetDropComp >= 0 && pending.assetDropComp < static_cast<int>(project.compositions.size())) {
    auto& comp = project.compositions[static_cast<size_t>(pending.assetDropComp)];
    if (pending.assetDropElem >= 0 && pending.assetDropElem < static_cast<int>(comp.elements.size())) {
      if (iota::model::assignAssetToElement(comp.elements[static_cast<size_t>(pending.assetDropElem)],
                                            pending.assetDropUri, false)) {
        state.selectSingleElement(pending.assetDropComp, pending.assetDropElem);
        actions.undoSnapshotNeeded = true;
        actions.editLabel = "Assign Asset to Layer";
      }
    } else {
      auto newImg = iota::model::createDefaultElement(iota::model::ElementType::Image);
      iota::model::assignFreshElementId(project, newImg);
      std::string label = pending.assetDropUri;
      if (label.rfind("local:", 0) == 0) label = label.substr(6);
      else if (label.rfind("internalres:", 0) == 0) label = label.substr(12);
      newImg.displayName = label;
      iota::model::assignAssetToElement(newImg, pending.assetDropUri, false);
      comp.elements.push_back(std::move(newImg));
      state.selectSingleElement(pending.assetDropComp, static_cast<int>(comp.elements.size()) - 1);
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Drop Asset into Comp";
    }
  }

  state.clampToProject(project);
  ImGui::End();
}

}  // namespace iota::studio
