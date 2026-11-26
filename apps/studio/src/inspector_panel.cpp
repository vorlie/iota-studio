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

#include "inspector_panel.hpp"

#include "asset_browser_panel.hpp"
#include "iota/core/types.hpp"
#include "iota/model/measured_var.hpp"
#include "iota/model/property_docs.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace iota::studio {

namespace {

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;
  for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
          std::tolower(static_cast<unsigned char>(needle[j]))) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

int inputTextStringCallback(ImGuiInputTextCallbackData* data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* str = static_cast<std::string*>(data->UserData);
    str->resize(static_cast<size_t>(data->BufTextLen));
    data->Buf = str->data();
  }
  return 0;
}

bool drawInputTextString(const char* label, std::string& str, bool multiline = false) {
  const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
  if (multiline) {
    return ImGui::InputTextMultiline(label,
                                     str.data(),
                                     str.capacity() + 1,
                                     ImVec2(-FLT_MIN, 110.0f),
                                     flags,
                                     inputTextStringCallback,
                                     &str);
  }
  return ImGui::InputText(label,
                          str.data(),
                          str.capacity() + 1,
                          flags,
                          inputTextStringCallback,
                          &str);
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

bool isShaderFileName(std::string_view name) noexcept {
  return hasSuffixCI(name, ".glsl") ||
         hasSuffixCI(name, ".frag") ||
         hasSuffixCI(name, ".vert") ||
         hasSuffixCI(name, ".fsh") ||
         hasSuffixCI(name, ".vsh") ||
         hasSuffixCI(name, ".fs") ||
         hasSuffixCI(name, ".vs");
}

void drawPropertyTooltip(iota::model::ElementType ownerType,
                         const std::string& rawKey,
                         const std::optional<std::string>& presetHint,
                         const iota::model::ParsedTypeSpec* spec) {
  const auto* doc = iota::model::findPropertyDoc(ownerType, rawKey);
  const std::string displayName = iota::model::formatPropertyDisplayName(ownerType, rawKey);

  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(360.0f);

  if (doc != nullptr && !doc->unit.empty()) {
    ImGui::Text("%s  (%.*s)",
                displayName.c_str(),
                static_cast<int>(doc->unit.size()),
                doc->unit.data());
  } else {
    ImGui::TextUnformatted(displayName.c_str());
  }

  if (doc != nullptr && !doc->summary.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%.*s", static_cast<int>(doc->summary.size()), doc->summary.data());
  }
  if (doc != nullptr && !doc->usage.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f),
                       "%.*s",
                       static_cast<int>(doc->usage.size()),
                       doc->usage.data());
  }
  if (presetHint.has_value() && !presetHint->empty()) {
    ImGui::Spacing();
    ImGui::TextDisabled("Preset hint: %s", presetHint->c_str());
  }

  ImGui::Separator();
  if (spec != nullptr && spec->hasBounds) {
    ImGui::TextDisabled("Key: %s   |   Range: %.4g .. %.4g", rawKey.c_str(), spec->minVal, spec->maxVal);
  } else {
    ImGui::TextDisabled("Key: %s", rawKey.c_str());
  }

  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

void drawSimpleDocTooltip(std::string_view title,
                          std::string_view summary,
                          std::string_view extraLabel = {},
                          std::string_view extraText = {}) {
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(340.0f);
  ImGui::Text("%.*s", static_cast<int>(title.size()), title.data());
  if (!summary.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%.*s", static_cast<int>(summary.size()), summary.data());
  }
  if (!extraText.empty()) {
    ImGui::Spacing();
    if (!extraLabel.empty()) {
      ImGui::TextDisabled("%.*s: %.*s",
                          static_cast<int>(extraLabel.size()),
                          extraLabel.data(),
                          static_cast<int>(extraText.size()),
                          extraText.data());
    } else {
      ImGui::TextDisabled("%.*s", static_cast<int>(extraText.size()), extraText.data());
    }
  }
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

void drawTableLabelCell(iota::model::ElementType ownerType,
                        const std::string& rawKey,
                        const std::optional<std::string>& presetHint,
                        const iota::model::ParsedTypeSpec* spec,
                        const InspectorActions& actions,
                        int depth) {
  ImGui::TableSetColumnIndex(0);
  if (depth > 0) {
    ImGui::Indent(static_cast<float>(depth) * 10.0f);
  }
  ImGui::AlignTextToFramePadding();
  const std::string label = actions.showRawPropertyKeys
                                ? rawKey
                                : iota::model::formatPropertyDisplayName(ownerType, rawKey);
  ImGui::TextUnformatted(label.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    drawPropertyTooltip(ownerType, rawKey, presetHint, spec);
  }
  if (depth > 0) {
    ImGui::Unindent(static_cast<float>(depth) * 10.0f);
  }
}

void drawSubLabelCell(const char* label,
                      std::string_view tooltipTitle,
                      std::string_view tooltipBody,
                      int depth) {
  ImGui::TableSetColumnIndex(0);
  if (depth > 0) {
    ImGui::Indent(static_cast<float>(depth) * 10.0f);
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    drawSimpleDocTooltip(tooltipTitle, tooltipBody);
  }
  if (depth > 0) {
    ImGui::Unindent(static_cast<float>(depth) * 10.0f);
  }
}

bool drawTableTreeLabelCell(iota::model::ElementType ownerType,
                            const std::string& rawKey,
                            const std::optional<std::string>& presetHint,
                            const iota::model::ParsedTypeSpec* spec,
                            const InspectorActions& actions,
                            int depth,
                            bool defaultOpen) {
  ImGui::TableSetColumnIndex(0);
  if (depth > 0) {
    ImGui::Indent(static_cast<float>(depth) * 10.0f);
  }
  ImGui::AlignTextToFramePadding();
  const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
  const std::string label = actions.showRawPropertyKeys
                                ? rawKey
                                : iota::model::formatPropertyDisplayName(ownerType, rawKey);
  const bool open = ImGui::TreeNodeEx("##tree", flags, "%s", label.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    drawPropertyTooltip(ownerType, rawKey, presetHint, spec);
  }
  if (depth > 0) {
    ImGui::Unindent(static_cast<float>(depth) * 10.0f);
  }
  return open;
}

bool entryMatchesFilter(iota::model::ElementType ownerType,
                        const std::string& key,
                        const iota::model::PropertyEntry& entry,
                        std::string_view filter) {
  if (filter.empty()) return true;
  if (containsIgnoreCase(key, filter)) return true;
  const std::string disp = iota::model::formatPropertyDisplayName(ownerType, key);
  if (containsIgnoreCase(disp, filter)) return true;
  if (const auto* doc = iota::model::findPropertyDoc(ownerType, key)) {
    if (containsIgnoreCase(doc->summary, filter) || containsIgnoreCase(doc->usage, filter)) {
      return true;
    }
  }
  for (const auto& ch : entry.children) {
    if (entryMatchesFilter(ownerType, ch.key, ch.entry, filter)) {
      return true;
    }
  }
  return false;
}

void drawMVarModeTooltip(std::string_view mode) {
  if (const auto* doc = iota::model::findMVarModeDoc(mode)) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(340.0f);
    ImGui::Text("%.*s", static_cast<int>(doc->displayName.size()), doc->displayName.data());
    ImGui::Separator();
    ImGui::TextWrapped("%.*s", static_cast<int>(doc->summary.size()), doc->summary.data());
    ImGui::Spacing();
    ImGui::TextDisabled("Scale A: %.*s", static_cast<int>(doc->paramA.size()), doc->paramA.data());
    ImGui::TextDisabled("Param B: %.*s", static_cast<int>(doc->paramB.size()), doc->paramB.data());
    ImGui::TextDisabled("Mode ID: %.*s", static_cast<int>(doc->mode.size()), doc->mode.data());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void renderMVarSlotsEditor(iota::model::MVariableFloat& mvar,
                           bool isVec2,
                           float minB,
                           float maxB,
                           int depth,
                           InspectorActions& actions) {
  const auto& modes = iota::model::allMeasureWhatModes();
  const auto& style = ImGui::GetStyle();
  const float removeBtnW = ImGui::CalcTextSize("X").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
  int removeIdx = -1;

  for (int i = 0; i < static_cast<int>(mvar.slots.size()); ++i) {
    auto& slot = mvar.slots[static_cast<size_t>(i)];
    ImGui::PushID(i);

    ImGui::TableNextRow();
    const std::string slotLabel = "Signal Slot " + std::to_string(i + 1);
    drawSubLabelCell(slotLabel.c_str(),
                     slotLabel,
                     "Selects the audio/time driver added into this property's modulation sum.",
                     depth);
    ImGui::TableSetColumnIndex(1);
    const bool canRemove = (mvar.slots.size() > 1);
    const float comboW = canRemove
                             ? std::max(40.0f, ImGui::GetContentRegionAvail().x - removeBtnW)
                             : -FLT_MIN;
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::BeginCombo("##mode", slot.measureWhat.c_str())) {
      for (std::string_view m : modes) {
        const bool selected = (slot.measureWhat == m);
        std::string itemStr(m);
        if (ImGui::Selectable(itemStr.c_str(), selected)) {
          slot.measureWhat = itemStr;
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Change Measure Mode";
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
          drawMVarModeTooltip(m);
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
      drawMVarModeTooltip(slot.measureWhat);
    }
    if (canRemove) {
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
        removeIdx = i;
      }
    }

    if (slot.measureWhat != "Nothing") {
      const auto* modeDoc = iota::model::findMVarModeDoc(slot.measureWhat);
      if (isVec2) {
        ImGui::TableNextRow();
        drawSubLabelCell("Amount (X, Y)",
                         "Amount (X, Y)",
                         modeDoc ? modeDoc->summary
                                 : "Horizontal (X) and vertical (Y) modulation weights for this signal.",
                         depth + 1);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        float ab[2] = {slot.a, slot.b};
        const bool edited = actions.preciseNumericInputs
                                ? ImGui::InputFloat2("##xy", ab, "%.6g")
                                : ImGui::DragFloat2("##xy", ab, 0.01f, minB, maxB);
        if (edited) {
          slot.a = ab[0];
          slot.b = ab[1];
          actions.modified = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Adjust MVar";
        }
      } else {
        ImGui::TableNextRow();
        drawSubLabelCell("Scale A",
                         "Scale A",
                         modeDoc ? modeDoc->paramA
                                 : "Primary amplitude scale factor for the selected signal.",
                         depth + 1);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool editedA = actions.preciseNumericInputs
                                 ? ImGui::InputFloat("##a", &slot.a, 0.0f, 0.0f, "%.6g")
                                 : ImGui::DragFloat("##a", &slot.a, 0.01f, minB, maxB);
        if (editedA) {
          actions.modified = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Adjust MVar";
        }

        ImGui::TableNextRow();
        drawSubLabelCell("Param B",
                         "Param B",
                         modeDoc ? modeDoc->paramB
                                 : "Secondary frequency/speed parameter depending on Signal Source.",
                         depth + 1);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool editedB = actions.preciseNumericInputs
                                 ? ImGui::InputFloat("##b", &slot.b, 0.0f, 0.0f, "%.6g")
                                 : ImGui::DragFloat("##b", &slot.b, 0.01f, -10.0f, 10.0f);
        if (editedB) {
          actions.modified = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Adjust MVar";
        }
      }
    }
    ImGui::PopID();
  }

  if (removeIdx >= 0) {
    mvar.slots.erase(mvar.slots.begin() + removeIdx);
    actions.modified = true;
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Remove MVar Slot";
  }

  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TableSetColumnIndex(1);
  if (ImGui::SmallButton("+ Add Slot")) {
    mvar.slots.push_back({"Constant", 0.5f, 0.5f});
    actions.modified = true;
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Add MVar Slot";
  }
}

void renderPropertyEntry(iota::model::ElementType ownerType,
                         const std::string& key,
                         iota::model::PropertyEntry& entry,
                         InspectorActions& actions,
                         int depth = 0) {
  const auto spec = iota::model::parseTypeTag(entry.typeTag);
  using iota::model::PropertyTypeTag;

  if (spec.tag == PropertyTypeTag::HiddenInt && !actions.showHiddenProperties) {
    return;
  }

  ImGui::PushID(key.c_str());

  auto markDeactivated = [&](const char* opName) {
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      actions.undoSnapshotNeeded = true;
      actions.editLabel = std::string(opName) + " " +
                          iota::model::formatPropertyDisplayName(ownerType, key);
    }
  };

  switch (spec.tag) {
    case PropertyTypeTag::Bool:
    case PropertyTypeTag::ProBool: {
      ImGui::TableNextRow();
      drawTableLabelCell(ownerType, key, entry.hint, &spec, actions, depth);
      ImGui::TableSetColumnIndex(1);
      bool v = false;
      if (const auto* b = std::get_if<bool>(&entry.value)) v = *b;
      else if (const auto* iv = std::get_if<int64_t>(&entry.value)) v = (*iv != 0);
      if (ImGui::Checkbox("##v", &v)) {
        if (std::holds_alternative<bool>(entry.value)) {
          entry.value = v;
        } else {
          entry.value = static_cast<int64_t>(v ? 1 : 0);
        }
        actions.modified = true;
        actions.undoSnapshotNeeded = true;
        actions.editLabel = "Toggle " + iota::model::formatPropertyDisplayName(ownerType, key);
      }
      break;
    }

    case PropertyTypeTag::Int:
    case PropertyTypeTag::HiddenInt: {
      ImGui::TableNextRow();
      drawTableLabelCell(ownerType, key, entry.hint, &spec, actions, depth);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      int iv = 0;
      if (const auto* p = std::get_if<int64_t>(&entry.value)) iv = static_cast<int>(*p);
      else if (const auto* d = std::get_if<double>(&entry.value)) iv = static_cast<int>(*d);
      const int minI = spec.hasBounds ? static_cast<int>(spec.minVal) : -10000;
      const int maxI = spec.hasBounds ? static_cast<int>(spec.maxVal) : 10000;
      bool edited = false;
      if (actions.preciseNumericInputs) {
        const int step = (ImGui::GetContentRegionAvail().x >= 130.0f) ? 1 : 0;
        edited = ImGui::InputInt("##v", &iv, step, step * 10);
      } else {
        edited = spec.hasBounds
                     ? ImGui::SliderInt("##v", &iv, minI, maxI)
                     : ImGui::DragInt("##v", &iv);
      }
      if (edited) {
        entry.value = static_cast<int64_t>(iv);
        actions.modified = true;
      }
      markDeactivated("Edit");
      break;
    }

    case PropertyTypeTag::Float: {
      ImGui::TableNextRow();
      drawTableLabelCell(ownerType, key, entry.hint, &spec, actions, depth);
      ImGui::TableSetColumnIndex(1);
      float fv = 0.0f;
      if (const auto* d = std::get_if<double>(&entry.value)) fv = static_cast<float>(*d);
      else if (const auto* p = std::get_if<int64_t>(&entry.value)) fv = static_cast<float>(*p);
      const float lo = spec.hasBounds ? spec.minVal : -1000.0f;
      const float hi = spec.hasBounds ? spec.maxVal : 1000.0f;

      const auto& style = ImGui::GetStyle();
      const float keyBtnW = ImGui::CalcTextSize("Key").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
      ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - keyBtnW));
      const bool edited = actions.preciseNumericInputs
                              ? ImGui::InputFloat("##v", &fv, 0.0f, 0.0f, "%.6g")
                              : (spec.hasBounds ? ImGui::SliderFloat("##v", &fv, lo, hi, "%.3f")
                                                : ImGui::DragFloat("##v", &fv, 0.01f));
      if (edited) {
        entry.value = static_cast<double>(fv);
        actions.modified = true;
      }
      markDeactivated("Edit");
      ImGui::SameLine();
      if (ImGui::SmallButton("Key")) {
        actions.addKeyframeProperty = key;
        actions.addKeyframeValue = fv;
      }
      break;
    }

    case PropertyTypeTag::Vec2f: {
      ImGui::TableNextRow();
      drawTableLabelCell(ownerType, key, entry.hint, &spec, actions, depth);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      std::string* sv = std::get_if<std::string>(&entry.value);
      iota::core::Vec2 vec = sv ? iota::core::parseF2(*sv).value_or(iota::core::Vec2{}) : iota::core::Vec2{};
      float arr[2] = {vec.x, vec.y};
      const float lo = spec.hasBounds ? spec.minVal : -10.0f;
      const float hi = spec.hasBounds ? spec.maxVal : 10.0f;
      const bool edited = actions.preciseNumericInputs
                              ? ImGui::InputFloat2("##v", arr, "%.6g")
                              : ImGui::DragFloat2("##v", arr, 0.005f, lo, hi, "%.3f");
      if (edited) {
        entry.value = iota::core::formatF2({arr[0], arr[1]});
        actions.modified = true;
      }
      markDeactivated("Edit");
      break;
    }

    case PropertyTypeTag::MVarFloat:
    case PropertyTypeTag::MVarVec2f: {
      ImGui::TableNextRow();
      const bool open = drawTableTreeLabelCell(ownerType, key, entry.hint, &spec, actions, depth, false);
      ImGui::TableSetColumnIndex(1);
      std::string* sv = std::get_if<std::string>(&entry.value);
      auto mvar = sv ? iota::model::parseMVarString(*sv) : iota::model::MVariableFloat{};
      if (!mvar.slots.empty()) {
        ImGui::TextDisabled("%s (%zu slot%s)",
                            mvar.slots[0].measureWhat.c_str(),
                            mvar.slots.size(),
                            mvar.slots.size() == 1 ? "" : "s");
      } else {
        ImGui::TextDisabled("0 slots");
      }

      if (open) {
        const float lo = spec.hasBounds ? spec.minVal : -5.0f;
        const float hi = spec.hasBounds ? spec.maxVal : 5.0f;
        const bool prevMod = actions.modified;
        actions.modified = false;
        renderMVarSlotsEditor(mvar, spec.tag == PropertyTypeTag::MVarVec2f, lo, hi, depth + 1, actions);
        if (actions.modified) {
          entry.value = iota::model::formatMVarString(mvar);
        }
        actions.modified = actions.modified || prevMod;
      }
      break;
    }

    case PropertyTypeTag::ColorRgb:
    case PropertyTypeTag::ColorRgba:
    case PropertyTypeTag::ColorRgbHl: {
      ImGui::TableNextRow();
      drawTableLabelCell(ownerType, key, entry.hint, &spec, actions, depth);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      int64_t raw = -1;
      if (const auto* iv = std::get_if<int64_t>(&entry.value)) raw = *iv;
      auto col = iota::core::ColorRgba::fromArgbInt32(static_cast<int32_t>(raw));
      float f4[4]{};
      col.toFloat4(f4);
      const bool edited = (spec.tag == PropertyTypeTag::ColorRgba)
                              ? ImGui::ColorEdit4("##v", f4, ImGuiColorEditFlags_AlphaPreviewHalf)
                              : ImGui::ColorEdit3("##v", f4);
      if (edited) {
        if (spec.tag != PropertyTypeTag::ColorRgba) f4[3] = 1.0f;
        entry.value = static_cast<int64_t>(iota::core::ColorRgba::fromFloat4(f4).toArgbInt32());
        actions.modified = true;
      }
      markDeactivated("Color");
      break;
    }

    case PropertyTypeTag::ColorHsla4f:
    case PropertyTypeTag::ColorHslaAlpha4f: {
      ImGui::TableNextRow();
      drawTableLabelCell(ownerType, key, entry.hint, &spec, actions, depth);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      std::string* sv = std::get_if<std::string>(&entry.value);
      auto hsla = sv ? iota::core::parseHsla4f(*sv).value_or(iota::core::ColorHslaNorm{})
                     : iota::core::ColorHslaNorm{};
      float f4[4]{};
      hsla.toRgba().toFloat4(f4);
      if (ImGui::ColorEdit4("##v", f4, ImGuiColorEditFlags_AlphaPreviewHalf)) {
        const auto newHsla = iota::core::ColorHslaNorm::fromRgba(iota::core::ColorRgba::fromFloat4(f4));
        entry.value = iota::core::formatHsla4f(newHsla);
        actions.modified = true;
      }
      markDeactivated("Color HSLA");
      break;
    }

    case PropertyTypeTag::Text:
    case PropertyTypeTag::ProText:
    case PropertyTypeTag::CodeText: {
      ImGui::TableNextRow();
      drawTableLabelCell(ownerType, key, entry.hint, &spec, actions, depth);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (!std::holds_alternative<std::string>(entry.value)) {
        entry.value = std::string{};
      }
      auto& s = std::get<std::string>(entry.value);
      if (drawInputTextString("##v", s, spec.tag == PropertyTypeTag::CodeText)) {
        actions.modified = true;
      }
      markDeactivated("Edit Text");
      if (ownerType == iota::model::ElementType::CustomShader &&
          (key == "shaderFragment" || key == "shaderVertex")) {
        if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragDropPayload)) {
            const char* dropped = static_cast<const char*>(payload->Data);
            if (dropped != nullptr && *dropped != '\0') {
              s = dropped;
              actions.modified = true;
              actions.undoSnapshotNeeded = true;
              actions.editLabel = "Drop Shader onto " + key;
            }
          }
          ImGui::EndDragDropTarget();
        }
        if (ImGui::SmallButton("Pick Shader")) {
          ImGui::OpenPopup("ShaderPickPopup");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("File...")) {
          actions.requestLocalShaderImport = true;
          actions.assetTargetPropName = key;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit")) {
          actions.requestOpenShaderEditor = true;
          actions.editShaderTargetProp = key;
        }
        if (ImGui::BeginPopup("ShaderPickPopup")) {
          if (ImGui::Selectable("(Clear / Inline)", s.empty())) {
            s.clear();
            actions.modified = true;
            actions.undoSnapshotNeeded = true;
            actions.editLabel = "Clear Shader " + key;
          }
          if (actions.availableLocalAssets != nullptr) {
            bool anyShader = false;
            for (const auto& [assetName, bytes] : *actions.availableLocalAssets) {
              (void)bytes;
              if (!isShaderFileName(assetName)) continue;
              anyShader = true;
              const std::string uri = "local:" + assetName;
              if (ImGui::Selectable(uri.c_str(), s == uri)) {
                s = uri;
                actions.modified = true;
                actions.undoSnapshotNeeded = true;
                actions.editLabel = "Select Shader " + key;
              }
            }
            if (!anyShader) {
              ImGui::TextDisabled("(No shader assets imported yet)");
            }
          }
          ImGui::EndPopup();
        }
      }
      if (spec.tag == PropertyTypeTag::ProText && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Tokens: {Artist}, {Title}, {ArtistAndTitle}, {TimeCurrent}, {TimeLength}, {Fps} (press F1 for full list)");
      }
      break;
    }

    case PropertyTypeTag::Image:
    case PropertyTypeTag::ParticleImage: {
      ImGui::TableNextRow();
      drawTableLabelCell(ownerType, key, entry.hint, &spec, actions, depth);
      ImGui::TableSetColumnIndex(1);
      if (!std::holds_alternative<std::string>(entry.value)) {
        entry.value = std::string{};
      }
      auto& s = std::get<std::string>(entry.value);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (drawInputTextString("##v", s)) {
        actions.modified = true;
      }
      markDeactivated("Change URI");
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragDropPayload)) {
          const char* dropped = static_cast<const char*>(payload->Data);
          if (dropped != nullptr && *dropped != '\0') {
            s = dropped;
            actions.modified = true;
            actions.undoSnapshotNeeded = true;
            actions.editLabel = "Drop Asset onto " + key;
          }
        }
        ImGui::EndDragDropTarget();
      }
      if (ImGui::SmallButton("Pick")) {
        ImGui::OpenPopup("AssetPickPopup");
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("File...")) {
        actions.requestLocalAssetImport = true;
        actions.assetTargetPropName = key;
      }
      if (ImGui::BeginPopup("AssetPickPopup")) {
        for (const auto& ch : spec.choices) {
          if (ImGui::Selectable(ch.c_str(), s == ch)) {
            s = ch;
            actions.modified = true;
            actions.undoSnapshotNeeded = true;
            actions.editLabel = "Select Asset " + key;
          }
        }
        ImGui::EndPopup();
      }
      break;
    }

    case PropertyTypeTag::Select:
    case PropertyTypeTag::Asset:
    case PropertyTypeTag::ProAsset: {
      ImGui::TableNextRow();
      drawTableLabelCell(ownerType, key, entry.hint, &spec, actions, depth);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (!std::holds_alternative<std::string>(entry.value)) {
        entry.value = std::string{};
      }
      auto& s = std::get<std::string>(entry.value);
      std::vector<std::string> dynChoices;
      const auto* choiceList = &spec.choices;
      if (key == "measureWhat" && !spec.choices.empty()) {
        dynChoices = spec.choices;
        for (std::string_view m : iota::model::allMeasureWhatModes()) {
          if (std::find(dynChoices.begin(), dynChoices.end(), m) == dynChoices.end()) {
            dynChoices.emplace_back(m);
          }
        }
        choiceList = &dynChoices;
      }
      if (key == "typeFace") {
        if (drawInputTextString("##v", s)) {
          actions.modified = true;
        }
        markDeactivated("Edit Font");
        if (ImGui::SmallButton("Pick")) {
          ImGui::OpenPopup("FontPickPopup");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Font...")) {
          actions.requestLocalFontImport = true;
          actions.assetTargetPropName = key;
        }
        if (ImGui::BeginPopup("FontPickPopup")) {
          static constexpr const char* kFontChoices[] = {
              "Segoe UI", "Bahnschrift", "Impact", "Consolas", "Georgia",
              "Segoe Script", "Arial", "Yu Gothic UI", "Malgun Gothic", "Microsoft YaHei UI",
              "internal_0", "internal_1", "internal_2", "internal_3",
              "internal_4", "internal_5", "internal_6", "internal_7", "internal_8",
          };
          for (const char* fc : kFontChoices) {
            const bool selected = (s == fc);
            if (ImGui::Selectable(fc, selected)) {
              s = fc;
              actions.modified = true;
              actions.undoSnapshotNeeded = true;
              actions.editLabel = "Select Font";
            }
            if (selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndPopup();
        }
      } else if (choiceList->empty()) {
        if (drawInputTextString("##v", s)) {
          actions.modified = true;
        }
        markDeactivated("Edit");
      } else {
        if (ImGui::BeginCombo("##v", s.c_str())) {
          for (const auto& rawChoice : *choiceList) {
            std::string label = rawChoice;
            const size_t colon = rawChoice.find(':');
            if ((spec.tag == PropertyTypeTag::Asset || spec.tag == PropertyTypeTag::ProAsset) &&
                colon != std::string::npos) {
              label = rawChoice.substr(0, colon);
            }
            const bool selected = (s == label);
            if (ImGui::Selectable(label.c_str(), selected)) {
              s = label;
              actions.modified = true;
              actions.undoSnapshotNeeded = true;
              actions.editLabel = "Select " + iota::model::formatPropertyDisplayName(ownerType, key);
            }
            if (key == "measureWhat" && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
              drawMVarModeTooltip(label);
            }
            if (selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        if (key == "measureWhat" && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
          drawMVarModeTooltip(s);
        }
      }
      break;
    }

    case PropertyTypeTag::Child:
    case PropertyTypeTag::DynamicGroup: {
      ImGui::TableNextRow();
      const bool open = drawTableTreeLabelCell(ownerType, key, entry.hint, &spec, actions, depth, true);
      ImGui::TableSetColumnIndex(1);
      std::vector<std::string> dynChoices;
      const auto* choiceList = &spec.choices;
      if (key == "measureWhat" && !spec.choices.empty()) {
        dynChoices = spec.choices;
        for (std::string_view m : iota::model::allMeasureWhatModes()) {
          if (std::find(dynChoices.begin(), dynChoices.end(), m) == dynChoices.end()) {
            dynChoices.emplace_back(m);
          }
        }
        choiceList = &dynChoices;
      }
      if (!choiceList->empty()) {
        if (!std::holds_alternative<std::string>(entry.value)) {
          entry.value = std::string{};
        }
        auto& cur = std::get<std::string>(entry.value);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##variant", cur.c_str())) {
          for (const auto& ch : *choiceList) {
            if (ImGui::Selectable(ch.c_str(), cur == ch)) {
              cur = ch;
              iota::model::seedVariantChildren(entry, key, ch);
              actions.modified = true;
              actions.undoSnapshotNeeded = true;
              actions.editLabel = "Switch " + iota::model::formatPropertyDisplayName(ownerType, key);
            }
            if (key == "measureWhat" && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
              drawMVarModeTooltip(ch);
            }
          }
          ImGui::EndCombo();
        }
      }

      if (open) {
        if (auto* mw = entry.findChild("measureWhat")) {
          renderPropertyEntry(ownerType, "measureWhat", *mw, actions, depth + 1);
          const auto* mwStr = std::get_if<std::string>(&mw->value);
          if (mwStr && *mwStr != "Nothing") {
            iota::model::seedVariantChildren(entry, key, *mwStr);
          }
        }
        for (auto& child : entry.children) {
          if (child.key == "measureWhat") continue;
          renderPropertyEntry(ownerType, child.key, child.entry, actions, depth + 1);
        }
      }
      break;
    }

    default: {
      ImGui::TableNextRow();
      drawTableLabelCell(ownerType, key, entry.hint, &spec, actions, depth);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (auto* s = std::get_if<std::string>(&entry.value)) {
        if (drawInputTextString("##v", *s)) actions.modified = true;
        markDeactivated("Edit");
      } else if (auto* d = std::get_if<double>(&entry.value)) {
        float fv = static_cast<float>(*d);
        const bool edited = actions.preciseNumericInputs
                                ? ImGui::InputFloat("##v", &fv, 0.0f, 0.0f, "%.6g")
                                : ImGui::DragFloat("##v", &fv, 0.01f);
        if (edited) {
          *d = fv;
          actions.modified = true;
        }
        markDeactivated("Edit");
      } else if (auto* iv = std::get_if<int64_t>(&entry.value)) {
        int i32 = static_cast<int>(*iv);
        bool edited = false;
        if (actions.preciseNumericInputs) {
          const int step = (ImGui::GetContentRegionAvail().x >= 130.0f) ? 1 : 0;
          edited = ImGui::InputInt("##v", &i32, step, step * 10);
        } else {
          edited = ImGui::DragInt("##v", &i32);
        }
        if (edited) {
          *iv = i32;
          actions.modified = true;
        }
        markDeactivated("Edit");
      }
      break;
    }
  }

  ImGui::PopID();
}

}  // namespace

void renderPropertyBagInspector(iota::model::PropertyBag& bag,
                                iota::model::ElementType ownerType,
                                InspectorActions& actions,
                                std::function<void()> /*onBeforeDiscreteEdit*/) {
  struct ItemRef {
    std::string key;
    iota::model::PropertyEntry* entry = nullptr;
  };
  std::map<std::string, std::vector<ItemRef>> grouped;
  for (auto& bagItem : bag.items()) {
    const auto spec = iota::model::parseTypeTag(bagItem.entry.typeTag);
    if (spec.tag == iota::model::PropertyTypeTag::HiddenInt && !actions.showHiddenProperties) {
      continue;
    }
    if (!entryMatchesFilter(ownerType, bagItem.key, bagItem.entry, actions.propertyFilter)) {
      continue;
    }
    const std::string tag = bagItem.entry.groupTag.empty() ? "misc" : bagItem.entry.groupTag;
    grouped[tag].push_back({bagItem.key, &bagItem.entry});
  }

  if (grouped.empty()) {
    if (!actions.propertyFilter.empty()) {
      ImGui::TextDisabled("No properties match \"%s\".", actions.propertyFilter.c_str());
    } else {
      ImGui::TextDisabled("No editable properties.");
    }
    return;
  }

  for (auto& [tag, vec] : grouped) {
    std::stable_sort(vec.begin(), vec.end(), [](const ItemRef& a, const ItemRef& b) {
      return a.entry->order < b.entry->order;
    });
    const std::string header = iota::model::formatGroupDisplayName(tag);
    const std::string headerId = header + "##grp_" + tag;
    const bool open = ImGui::CollapsingHeader(headerId.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
      if (const auto* gdoc = iota::model::findGroupDoc(tag)) {
        drawSimpleDocTooltip(gdoc->displayName, gdoc->summary, "Section tag", tag);
      } else {
        drawSimpleDocTooltip(header, "Property group", "Section tag", tag);
      }
    }
    if (open) {
      const ImGuiTableFlags tblFlags = ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_SizingStretchProp |
                                       ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_NoSavedSettings;
      if (ImGui::BeginTable(("##prop_tbl_" + tag).c_str(), 2, tblFlags)) {
        ImGui::TableSetupColumn("Option", ImGuiTableColumnFlags_WidthStretch, 0.44f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.56f);
        for (auto& item : vec) {
          renderPropertyEntry(ownerType, item.key, *item.entry, actions, 0);
        }
        ImGui::EndTable();
      }
    }
  }
}

}  // namespace iota::studio
