#include "project_manager_modal.hpp"

#include "iota/core/utf8_path.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <system_error>

namespace iota::studio {

void renderProjectManagerModal(bool& openRequested,
                               iota::model::Project& project,
                               const std::filesystem::path& currentProjectPath,
                               bool projectDirty,
                               iota::preset_io::ProjectLibrary& library,
                               ProjectManagerModalState& state,
                               ProjectManagerActions& actions) {
  if (openRequested) {
    ImGui::OpenPopup("Project Manager");
    openRequested = false;
    std::snprintf(state.curName, sizeof(state.curName), "%s", project.metadata.name.c_str());
    std::snprintf(state.curNotes, sizeof(state.curNotes), "%s", project.metadata.notes.c_str());
    state.curWidth = project.metadata.previewWidth;
    state.curHeight = project.metadata.previewHeight;
    state.curFps = project.metadata.targetFps;
  }

  ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_FirstUseEver);
  bool modalOpen = true;
  if (!ImGui::BeginPopupModal("Project Manager", &modalOpen)) {
    return;
  }

  if (ImGui::BeginTabBar("##pm_tabs")) {
    if (ImGui::BeginTabItem("Recent & Open")) {
      if (ImGui::Button("Open Project / Preset File...")) {
        actions.openFileDialog = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Prune Missing")) {
        if (library.pruneMissing() > 0) {
          actions.registryModified = true;
        }
      }
      ImGui::Separator();

      const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
      if (ImGui::BeginTable("##recent_tbl", 4, tflags, ImVec2(0.0f, -38.0f))) {
        ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Contents", ImGuiTableColumnFlags_WidthFixed, 105.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        std::string removePath;
        const auto& entries = library.entries();
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
          const auto& e = entries[static_cast<size_t>(i)];
          const std::filesystem::path entryDiskPath = iota::core::pathFromUtf8(e.path);
          const bool exists = iota::core::pathExistsNoThrow(entryDiskPath);
          ImGui::PushID(i);
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          if (!exists) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s [Missing]", e.name.c_str());
          } else {
            ImGui::TextUnformatted(e.name.c_str());
          }
          ImGui::TableSetColumnIndex(1);
          ImGui::TextDisabled("%s", e.path.c_str());
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s\nLast opened: %s", e.path.c_str(), e.lastOpened.c_str());
          }
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%dc / %da", e.compositionCount, e.assetCount);
          ImGui::TableSetColumnIndex(3);
          ImGui::BeginDisabled(!exists);
          if (ImGui::SmallButton("Open")) {
            actions.openRecentFile = true;
            actions.openPath = entryDiskPath;
            ImGui::CloseCurrentPopup();
          }
          ImGui::EndDisabled();
          ImGui::SameLine();
          if (ImGui::SmallButton("Remove")) {
            removePath = e.path;
          }
          ImGui::PopID();
        }
        if (!removePath.empty()) {
          library.removeEntry(removePath);
          actions.registryModified = true;
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("New Project")) {
      ImGui::InputText("Project Name", state.newName, sizeof(state.newName));
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Preset Size:");
      ImGui::SameLine();
      if (ImGui::SmallButton("720p")) { state.newWidth = 1280; state.newHeight = 720; }
      ImGui::SameLine();
      if (ImGui::SmallButton("1080p")) { state.newWidth = 1920; state.newHeight = 1080; }
      ImGui::SameLine();
      if (ImGui::SmallButton("9:16")) { state.newWidth = 1080; state.newHeight = 1920; }
      ImGui::SameLine();
      if (ImGui::SmallButton("1:1")) { state.newWidth = 1080; state.newHeight = 1080; }

      ImGui::SetNextItemWidth(110.0f);
      ImGui::InputInt("Width##nw", &state.newWidth);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.0f);
      ImGui::InputInt("Height##nh", &state.newHeight);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(90.0f);
      ImGui::InputInt("FPS##nfps", &state.newFps);
      state.newWidth = std::clamp(state.newWidth, 64, 7680);
      state.newHeight = std::clamp(state.newHeight, 64, 4320);
      state.newFps = std::clamp(state.newFps, 15, 240);

      ImGui::InputTextMultiline("Notes##nnotes", state.newNotes, sizeof(state.newNotes), ImVec2(-FLT_MIN, 54.0f));
      ImGui::Separator();
      ImGui::TextUnformatted("Choose Starter Template:");

      const auto& templates = iota::preset_io::projectTemplates();
      ImGui::BeginChild("##tpl_cards", ImVec2(0.0f, -42.0f), true);
      for (int i = 0; i < static_cast<int>(templates.size()); ++i) {
        const auto& tpl = templates[static_cast<size_t>(i)];
        const bool sel = (state.selectedTemplate == i);
        ImGui::PushID(i);
        if (ImGui::Selectable(tpl.title, sel)) {
          state.selectedTemplate = i;
        }
        ImGui::Indent(14.0f);
        ImGui::TextWrapped("%s", tpl.description);
        ImGui::Unindent(14.0f);
        ImGui::Spacing();
        ImGui::PopID();
      }
      ImGui::EndChild();

      const auto chosenKind =
          templates[static_cast<size_t>(std::clamp(state.selectedTemplate, 0,
                                                   static_cast<int>(templates.size()) - 1))].kind;
      if (ImGui::Button("Create Project")) {
        actions.createTemplateProject = true;
        actions.templateKind = chosenKind;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Create & Save (.iota)...")) {
        actions.createAndSaveProject = true;
        actions.templateKind = chosenKind;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Current Project")) {
      const std::string curPathStr =
          currentProjectPath.empty() ? "(Unsaved)" : iota::core::pathToUtf8(currentProjectPath);
      ImGui::Text("File: %s%s", curPathStr.c_str(), projectDirty ? " *" : "");
      ImGui::Separator();

      ImGui::InputText("Name", state.curName, sizeof(state.curName));
      ImGui::InputTextMultiline("Notes", state.curNotes, sizeof(state.curNotes), ImVec2(-FLT_MIN, 64.0f));
      ImGui::SetNextItemWidth(110.0f);
      ImGui::InputInt("Preview Width", &state.curWidth);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.0f);
      ImGui::InputInt("Preview Height", &state.curHeight);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(90.0f);
      ImGui::InputInt("Target FPS", &state.curFps);
      state.curWidth = std::clamp(state.curWidth, 64, 7680);
      state.curHeight = std::clamp(state.curHeight, 64, 4320);
      state.curFps = std::clamp(state.curFps, 15, 240);

      if (ImGui::Button("Apply Settings")) {
        actions.applyCurrentSettings = true;
      }

      ImGui::Separator();
      ImGui::Text("Linked Audio: %s",
                  project.metadata.audioPath.empty() ? "(None)" : project.metadata.audioPath.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Change Audio...")) {
        actions.requestChangeAudio = true;
      }

      ImGui::Text("Compositions: %zu   |   Bundled Assets: %zu   |   Audio Entries: %zu   |   Keyframes: %zu",
                  project.compositions.size(),
                  project.localAssets.size(),
                  project.metadata.audioAssets.size(),
                  project.desktopTracks.size());

      if (ImGui::Button("Remove Unused Assets")) {
        actions.requestCleanUnusedAssets = true;
      }
      ImGui::Separator();
      if (ImGui::Button("Save")) actions.requestSave = true;
      ImGui::SameLine();
      if (ImGui::Button("Save As (.iota)...")) actions.requestSaveAsIota = true;
      ImGui::SameLine();
      if (ImGui::Button("Export (.viz)...")) actions.requestExportViz = true;
      ImGui::SameLine();
      if (ImGui::Button("Export (.json)...")) actions.requestExportJson = true;

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::Separator();
  if (ImGui::Button("Close")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

}  // namespace iota::studio
