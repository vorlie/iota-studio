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

#include "asset_browser_panel.hpp"
#include "docs_panel.hpp"
#include "hierarchy_panel.hpp"
#include "platform_desktop.hpp"
#include "project_manager_modal.hpp"
#include "shader_editor_panel.hpp"
#include "update_modal.hpp"

#include "iota/audio/audio_engine.hpp"
#include "iota/core/update_checker.hpp"
#include "iota/discord/discord_presence.hpp"
#include "iota/engine/scene_renderer.hpp"
#ifdef IOTA_ENABLE_VIDEO_EXPORT
#include "iota/export/video_exporter.hpp"
#endif
#include "iota/model/command_stack.hpp"
#include "iota/model/scene.hpp"
#include "iota/preset_io/project_manager.hpp"
#include "iota/rhi/rhi.hpp"

#ifdef IOTA_ENABLE_MCP
#include "iota/mcp/live_bridge.hpp"
#include "iota/mcp/mcp_server.hpp"
#include "iota/mcp/png_encode.hpp"
#include "iota/mcp/working_copy.hpp"
#endif

#include <filesystem>
#include <memory>
#include <string>

struct GLFWwindow;
struct ImFont;

namespace iota::studio {

class EditorApp {
 public:
  EditorApp();
  ~EditorApp();

  EditorApp(const EditorApp&) = delete;
  EditorApp& operator=(const EditorApp&) = delete;

  bool initialize(bool headlessSmokeTest);
  int run();
  void tickFrame();
  int runSmokeTest();
  void shutdown();
#ifdef __EMSCRIPTEN__
  static void emscriptenMainLoop(void* arg);
#endif

 private:
  void applyStudioTheme();
  void applyUiDpiScale(float scale);
  void applySwapIntervalForCurrentMode() noexcept;
  [[nodiscard]] int currentMonitorRefreshRateHz() const noexcept;
  void setupDefaultDockLayout(unsigned int dockspaceId);
  void drawMenuBar();
  void drawViewportPanel();
  void drawHierarchyPanel();
  void drawAssetBrowserPanel();
  void drawInspectorPanel();
  void drawAudioTimelinePanel();
  void drawSoundKeysPanel();
  void drawProjectManagerModal();
#ifdef IOTA_ENABLE_VIDEO_EXPORT
  void drawExportModal();
#endif
  void drawUpdateModal();
  void drawVizImportNoticeModal();
  void drawDocsPanel();
  void drawShaderEditorPanel();
  void openShaderInEditor(std::string_view assetName);
  void openDocsForElement(iota::model::ElementType type);
  void openExternalUrl(const std::string& url);
  void handleShortcuts();

  void pushUndoState(std::string label);
  void updateWindowTitle();
  void syncSelectionFromHierarchy();
  void setSingleSelection(int compIdx, int elemIdx);
  void clampSelectionToProject();
  [[nodiscard]] bool hasHierarchyClipboard() const;
  void actionUndo();
  void actionRedo();
  void actionCopySelection();
  void actionCutSelection();
  void actionPasteClipboard();
  void actionDuplicateSelection();
  void actionDeleteSelection();
  void actionPrecomposeSelection();
  void actionNudgeSelection(int delta, bool toBoundary);
  void applyProjectMetadata(const iota::model::ProjectMetadata& meta);
  bool loadProjectFromPath(const std::filesystem::path& path);
  bool saveCurrentProject();
  bool saveProjectAsDialog(const char* preferredExt);
  void recordProjectInLibrary(const std::filesystem::path& path);

  void actionOpenPreset();
  void actionSavePreset(bool asViz);
  void actionOpenAudio();
  void actionLoadBuiltinTestTone(float freqHz);
  bool loadDecodedAudioTrack(platform::DecodedAudioPayload payload);
#ifdef __EMSCRIPTEN__
  void handleDroppedOrPickedFile(const std::filesystem::path& path);
#endif
  [[nodiscard]] int currentTargetFps() const noexcept;
  void updateDiscordPresence(double nowWallSec, bool isMinimized);
  void persistDiscordSettings();
  [[nodiscard]] std::string activeCompositionDisplayName() const;

#ifdef IOTA_ENABLE_MCP
  friend class EditorLiveStudioTarget;
  bool enableMcpLiveBridge();
  void disableMcpLiveBridge(std::string_view reason = {});
  void pollMcpLiveBridge();
#endif

  GLFWwindow* window_ = nullptr;
  bool imguiInitialized_ = false;
  bool dockLayoutBuilt_ = false;
  bool isSmokeTest_ = false;
  bool isWaylandPlatform_ = false;
  platform::WaylandFramePacer waylandPacer_;

  std::unique_ptr<iota::rhi::IRenderDevice> device_;
  iota::engine::SceneRenderer renderer_;
  iota::audio::AudioEngine audio_;
#ifdef IOTA_ENABLE_VIDEO_EXPORT
  iota::export_video::VideoExportSession exportSession_;
#endif

  iota::model::Project project_;
  iota::model::CommandStack undoStack_;
  std::filesystem::path currentProjectPath_;
  bool projectDirty_ = false;

  int selectedCompIdx_ = 0;
  int selectedElemIdx_ = -1;
  bool showHiddenProps_ = false;
  bool showRawPropertyKeys_ = false;
  bool preciseNumericInputs_ = false;
  char inspectorFilterBuf_[128]{};
  bool showDocsPanel_ = false;
  DocsPanelState docsPanelState_;
  bool showShaderEditorPanel_ = false;
  ShaderEditorPanel shaderEditor_;
  ImFont* codeFont_ = nullptr;
  bool showSoundKeysPanel_ = true;
  bool showAssetBrowserPanel_ = true;
  int activeSoundKeysRange_ = 0;
  bool fxaaEnabled_ = true;
  enum class PreviewScaleMode {
    Full100 = 0,
    Scale75 = 1,
    Scale50 = 2,
    AutoFitViewport = 3,
  };
  enum class VSyncMode {
    On = 0,
    Adaptive = 1,
    Off = 2,
  };
  PreviewScaleMode previewScaleMode_ = PreviewScaleMode::Full100;
  VSyncMode vsyncMode_ = VSyncMode::On;
  bool showPerfOverlay_ = false;
  int previewWidth_ = 1280;
  int previewHeight_ = 720;
  int lastLiveRenderW_ = 1280;
  int lastLiveRenderH_ = 720;
  float lastViewportAvailW_ = 0.0f;
  float lastViewportAvailH_ = 0.0f;
  float visOffsetMs_ = 0.0f;

  double totalTimeSec_ = 0.0;
  double playAccumTimeSec_ = 0.0;
  double lastWallTimeSec_ = 0.0;
  iota::engine::RenderFrameStats frameStats_;

  AssetBrowserState assetBrowserState_;
  HierarchyPanelState hierarchyState_;
  iota::model::HierarchyClipboard hierarchyClipboard_;
  ProjectManagerModalState projectManagerState_;
  iota::preset_io::ProjectLibrary projectLibrary_;
  bool openProjectManagerModal_ = false;

#ifdef IOTA_ENABLE_VIDEO_EXPORT
  iota::export_video::ExportSettings exportCfg_;
  iota::export_video::HardwareEncoderProbe encoderProbe_;
  double exportStartWallSec_ = 0.0;
  std::string exportModalError_;
  std::string exportModalSuccess_;
  bool openExportModal_ = false;
#else
  int targetFpsSetting_ = 60;
#endif
#ifdef IOTA_ENABLE_MCP
  iota::mcp::LiveBridgeHost mcpBridgeHost_;
  std::filesystem::path mcpOriginalProjectPath_;
#endif
  iota::core::UpdateChecker updateChecker_;
  iota::discord::DiscordPresence discordPresence_;
  double lastUserActivityWallSec_ = 0.0;
  bool openUpdateModal_ = false;
  bool updateBannerNotified_ = false;
  bool openVizImportNoticeModal_ = false;
  bool vizNoticeDontShowAgain_ = false;
  std::string lastImportedVizName_;
  void* appIconBig_ = nullptr;
  void* appIconSmall_ = nullptr;
  float uiDpiScale_ = 1.0f;
  std::string layoutIniPath_;
  std::string statusBanner_ = "Ready";
};

}  // namespace iota::studio
