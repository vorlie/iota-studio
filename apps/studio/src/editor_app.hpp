#pragma once

#include "asset_browser_panel.hpp"
#include "docs_panel.hpp"
#include "hierarchy_panel.hpp"
#include "project_manager_modal.hpp"
#include "update_modal.hpp"

#include "iota/audio/audio_engine.hpp"
#include "iota/core/update_checker.hpp"
#include "iota/engine/scene_renderer.hpp"
#include "iota/export/video_exporter.hpp"
#include "iota/model/command_stack.hpp"
#include "iota/model/scene.hpp"
#include "iota/preset_io/project_manager.hpp"
#include "iota/rhi/gl45_device.hpp"

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

namespace iota::studio {

class EditorApp {
 public:
  EditorApp();
  ~EditorApp();

  EditorApp(const EditorApp&) = delete;
  EditorApp& operator=(const EditorApp&) = delete;

  bool initialize(bool headlessSmokeTest);
  int run();
  int runSmokeTest();
  void shutdown();

 private:
  void applyStudioTheme();
  void setupDefaultDockLayout(unsigned int dockspaceId);
  void drawMenuBar();
  void drawViewportPanel();
  void drawHierarchyPanel();
  void drawAssetBrowserPanel();
  void drawInspectorPanel();
  void drawAudioTimelinePanel();
  void drawSoundKeysPanel();
  void drawProjectManagerModal();
  void drawExportModal();
  void drawUpdateModal();
  void drawDocsPanel();
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

  iota::rhi::Gl45RenderDevice device_;
  iota::engine::SceneRenderer renderer_;
  iota::audio::AudioEngine audio_;
  iota::export_video::VideoExportSession exportSession_;

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
  bool showSoundKeysPanel_ = true;
  bool showAssetBrowserPanel_ = true;
  int activeSoundKeysRange_ = 0;
  bool fxaaEnabled_ = true;
  int previewWidth_ = 1280;
  int previewHeight_ = 720;
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

  iota::export_video::ExportSettings exportCfg_;
  iota::export_video::HardwareEncoderProbe encoderProbe_;
  double exportStartWallSec_ = 0.0;
  std::string exportModalError_;
  std::string exportModalSuccess_;
  bool openExportModal_ = false;
#ifdef IOTA_ENABLE_MCP
  iota::mcp::LiveBridgeHost mcpBridgeHost_;
  std::filesystem::path mcpOriginalProjectPath_;
#endif
  iota::core::UpdateChecker updateChecker_;
  bool openUpdateModal_ = false;
  bool updateBannerNotified_ = false;
  void* appIconBig_ = nullptr;
  void* appIconSmall_ = nullptr;
  std::string statusBanner_ = "Ready";
};

}  // namespace iota::studio
