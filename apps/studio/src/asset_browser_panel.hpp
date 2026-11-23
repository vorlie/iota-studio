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
