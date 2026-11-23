#pragma once

#include "iota/model/scene.hpp"
#include "iota/preset_io/project_manager.hpp"

#include <filesystem>
#include <string>

namespace iota::studio {

struct ProjectManagerModalState {
  char newName[128] = "My Visualizer";
  int selectedTemplate = 0;
  int newWidth = 1280;
  int newHeight = 720;
  int newFps = 60;
  char newNotes[512]{};

  char curName[128]{};
  char curNotes[512]{};
  int curWidth = 1280;
  int curHeight = 720;
  int curFps = 60;
};

struct ProjectManagerActions {
  bool openRecentFile = false;
  std::filesystem::path openPath;
  bool openFileDialog = false;

  bool createTemplateProject = false;
  bool createAndSaveProject = false;
  iota::preset_io::ProjectTemplateKind templateKind = iota::preset_io::ProjectTemplateKind::MinimalCircular;

  bool applyCurrentSettings = false;
  bool requestChangeAudio = false;
  bool requestCleanUnusedAssets = false;
  bool requestSave = false;
  bool requestSaveAsIota = false;
  bool requestExportViz = false;
  bool requestExportJson = false;
  bool registryModified = false;
};

/// Draws the Project Manager modal dialog (Recent, Templates/New, Current Project).
void renderProjectManagerModal(bool& openRequested,
                               iota::model::Project& project,
                               const std::filesystem::path& currentProjectPath,
                               bool projectDirty,
                               iota::preset_io::ProjectLibrary& library,
                               ProjectManagerModalState& state,
                               ProjectManagerActions& actions);

}  // namespace iota::studio
