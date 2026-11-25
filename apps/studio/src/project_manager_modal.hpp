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
