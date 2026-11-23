#include "inspector_panel.hpp"

#include "asset_browser_panel.hpp"
#include "iota/core/types.hpp"
#include "iota/model/measured_var.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace iota::studio {

namespace {

std::string friendlyGroupLabel(std::string_view tag) {
  if (tag.empty()) return "Misc";
  size_t start = 0;
  if (tag.size() >= 2 && tag[0] >= '0' && tag[0] <= '9' && tag[1] == '_') {
    start = 2;
  }
  std::string out(tag.substr(start));
  if (!out.empty()) {
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  }
  return out;
}

bool drawInputTextString(const char* label, std::string& str, bool multiline = false) {
  char buf[2048]{};
  const size_t copyLen = std::min(str.size(), sizeof(buf) - 1);
  std::memcpy(buf, str.data(), copyLen);
  bool changed = false;
  if (multiline) {
    changed = ImGui::InputTextMultiline(label, buf, sizeof(buf), ImVec2(-FLT_MIN, 100.0f));
  } else {
    changed = ImGui::InputText(label, buf, sizeof(buf));
  }
  if (changed) {
    str = buf;
  }
  return changed;
}

void drawTableLabelCell(const std::string& key,
                        const std::optional<std::string>& hint,
                        int depth) {
  ImGui::TableSetColumnIndex(0);
  if (depth > 0) {
    ImGui::Indent(static_cast<float>(depth) * 10.0f);
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(key.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    if (hint.has_value() && !hint->empty()) {
      ImGui::SetTooltip("%s\n%s", key.c_str(), hint->c_str());
    } else {
      ImGui::SetTooltip("%s", key.c_str());
    }
  }
  if (depth > 0) {
    ImGui::Unindent(static_cast<float>(depth) * 10.0f);
  }
}

bool drawTableTreeLabelCell(const std::string& key,
                            const std::optional<std::string>& hint,
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
  const bool open = ImGui::TreeNodeEx(key.c_str(), flags);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    if (hint.has_value() && !hint->empty()) {
      ImGui::SetTooltip("%s\n%s", key.c_str(), hint->c_str());
    } else {
      ImGui::SetTooltip("%s", key.c_str());
    }
  }
  if (depth > 0) {
    ImGui::Unindent(static_cast<float>(depth) * 10.0f);
  }
  return open;
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
    drawTableLabelCell("Slot " + std::to_string(i + 1), std::nullopt, depth);
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
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    if (canRemove) {
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
        removeIdx = i;
      }
    }

    if (slot.measureWhat != "Nothing") {
      if (isVec2) {
        ImGui::TableNextRow();
        drawTableLabelCell("Amount (X/Y)", std::nullopt, depth + 1);
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
        drawTableLabelCell("Scale A", std::nullopt, depth + 1);
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
        drawTableLabelCell("Param B", std::nullopt, depth + 1);
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

void renderPropertyEntry(const std::string& key,
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
      actions.editLabel = std::string(opName) + " " + key;
    }
  };

  switch (spec.tag) {
    case PropertyTypeTag::Bool:
    case PropertyTypeTag::ProBool: {
      ImGui::TableNextRow();
      drawTableLabelCell(key, entry.hint, depth);
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
        actions.editLabel = "Toggle " + key;
      }
      break;
    }

    case PropertyTypeTag::Int:
    case PropertyTypeTag::HiddenInt: {
      ImGui::TableNextRow();
      drawTableLabelCell(key, entry.hint, depth);
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
      drawTableLabelCell(key, entry.hint, depth);
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
      drawTableLabelCell(key, entry.hint, depth);
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
      const bool open = drawTableTreeLabelCell(key, entry.hint, depth, false);
      ImGui::TableSetColumnIndex(1);
      std::string* sv = std::get_if<std::string>(&entry.value);
      auto mvar = sv ? iota::model::parseMVarString(*sv) : iota::model::MVariableFloat{};
      ImGui::TextDisabled("%zu slot%s", mvar.slots.size(), mvar.slots.size() == 1 ? "" : "s");

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
      drawTableLabelCell(key, entry.hint, depth);
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
      drawTableLabelCell(key, entry.hint, depth);
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
      drawTableLabelCell(key, entry.hint, depth);
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
      if (spec.tag == PropertyTypeTag::ProText && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Tokens: {Artist}, {Title}, {ArtistAndTitle}, {TimeCurrent}, {TimeLength}, {Fps}");
      }
      break;
    }

    case PropertyTypeTag::Image:
    case PropertyTypeTag::ParticleImage: {
      ImGui::TableNextRow();
      drawTableLabelCell(key, entry.hint, depth);
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
      drawTableLabelCell(key, entry.hint, depth);
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
      if (choiceList->empty()) {
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
              actions.editLabel = "Select " + key;
            }
            if (selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
      }
      break;
    }

    case PropertyTypeTag::Child:
    case PropertyTypeTag::DynamicGroup: {
      ImGui::TableNextRow();
      const bool open = drawTableTreeLabelCell(key, entry.hint, depth, true);
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
              actions.modified = true;
              actions.undoSnapshotNeeded = true;
              actions.editLabel = "Switch " + key;
            }
          }
          ImGui::EndCombo();
        }
      }

      if (open) {
        if (auto* mw = entry.findChild("measureWhat")) {
          renderPropertyEntry("measureWhat", *mw, actions, depth + 1);
          const auto* mwStr = std::get_if<std::string>(&mw->value);
          if (mwStr && *mwStr != "Nothing") {
            if (!entry.findChild("A")) {
              auto& ea = entry.getOrAddChild("A");
              ea.value = 0.5;
              ea.typeTag = "f -2.0 2.0";
            }
            if (!entry.findChild("B")) {
              auto& eb = entry.getOrAddChild("B");
              eb.value = 0.5;
              eb.typeTag = "f -2.0 2.0";
            }
          }
        }
        for (auto& child : entry.children) {
          if (child.key == "measureWhat") continue;
          renderPropertyEntry(child.key, child.entry, actions, depth + 1);
        }
      }
      break;
    }

    default: {
      ImGui::TableNextRow();
      drawTableLabelCell(key, entry.hint, depth);
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
                                InspectorActions& actions,
                                std::function<void()> /*onBeforeDiscreteEdit*/) {
  struct ItemRef {
    std::string key;
    iota::model::PropertyEntry* entry = nullptr;
  };
  std::map<std::string, std::vector<ItemRef>> grouped;
  for (auto& bagItem : bag.items()) {
    const std::string tag = bagItem.entry.groupTag.empty() ? "misc" : bagItem.entry.groupTag;
    grouped[tag].push_back({bagItem.key, &bagItem.entry});
  }

  for (auto& [tag, vec] : grouped) {
    std::stable_sort(vec.begin(), vec.end(), [](const ItemRef& a, const ItemRef& b) {
      return a.entry->order < b.entry->order;
    });
    const std::string header = friendlyGroupLabel(tag);
    if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      const ImGuiTableFlags tblFlags = ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_SizingStretchProp |
                                       ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_NoSavedSettings;
      if (ImGui::BeginTable(("##prop_tbl_" + tag).c_str(), 2, tblFlags)) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        for (auto& item : vec) {
          renderPropertyEntry(item.key, *item.entry, actions, 0);
        }
        ImGui::EndTable();
      }
    }
  }
}

}  // namespace iota::studio
