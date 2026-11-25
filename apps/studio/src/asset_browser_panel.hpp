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

#include "iota/audio/audio_engine.hpp"
#include "iota/engine/scene_renderer.hpp"
#include "iota/model/scene.hpp"
#include "iota/rhi/rhi.hpp"

#include <string>

namespace iota::studio {

inline constexpr const char* kAssetDragDropPayload = "IOTA_ASSET_URI";

enum class BrowserItemKind {
  None = 0,
  LocalImage,
  InternalTexture,
  BuiltinShader,
  CompositionRef,
  AudioTrack,
};

struct AssetBrowserState {
  BrowserItemKind selectedKind = BrowserItemKind::None;
  std::string selectedUri;
  std::string selectedLabel;
  char searchFilter[128]{};
  bool gridView = false;
  int categoryFilter = 0;  // 0=All, 1=Project, 2=Audio, 3=Comps, 4=Built-in

  bool openRenamePopup = false;
  std::string renameTarget;
  char renameBuf[256]{};

  bool openNewFolderPopup = false;
  char newFolderBuf[128]{};
};

struct AssetBrowserActions {
  bool modified = false;
  bool undoSnapshotNeeded = false;
  std::string editLabel;
  bool invalidateLocalAssets = false;

  bool requestImportImages = false;
  bool requestImportAudio = false;
  bool requestImportShaders = false;
  bool requestEditShader = false;
  std::string editShaderAssetName;
  bool requestReplaceLocalAsset = false;
  std::string replaceTargetName;
  bool requestLoadAudioPath = false;
  std::string loadAudioPath;

  int selectCompIdx = -1;
  int selectElemIdx = -1;
};

/// Renders the After Effects-style docked Assets browser window.
void renderAssetBrowserPanel(iota::model::Project& project,
                             const iota::audio::AudioEngine& audio,
                             iota::rhi::IRenderDevice& device,
                             iota::engine::SceneRenderer& renderer,
                             AssetBrowserState& state,
                             int selectedCompIdx,
                             int selectedElemIdx,
                             bool& openWindow,
                             AssetBrowserActions& actions);

}  // namespace iota::studio
