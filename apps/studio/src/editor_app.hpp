#pragma once

#include "iota/audio/audio_engine.hpp"
#include "iota/engine/scene_renderer.hpp"
#include "iota/export/video_exporter.hpp"
#include "iota/model/command_stack.hpp"
#include "iota/model/scene.hpp"
#include "iota/rhi/gl45_device.hpp"

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
  void drawInspectorPanel();
  void drawAudioTimelinePanel();
  void drawExportModal();
  void handleShortcuts();

  void pushUndoState(std::string label);

  void actionOpenPreset();
  void actionSavePreset(bool asViz);
  void actionOpenAudio();
  void actionLoadBuiltinTestTone(float freqHz);

  GLFWwindow* window_ = nullptr;
  bool imguiInitialized_ = false;
  bool dockLayoutBuilt_ = false;

  iota::rhi::Gl45RenderDevice device_;
  iota::engine::SceneRenderer renderer_;
  iota::audio::AudioEngine audio_;
  iota::export_video::VideoExportSession exportSession_;

  iota::model::Project project_;
  iota::model::CommandStack undoStack_;

  int selectedCompIdx_ = 0;
  int selectedElemIdx_ = -1;
  bool showHiddenProps_ = false;
  bool preciseNumericInputs_ = false;
  bool fxaaEnabled_ = true;
  int previewWidth_ = 1280;
  int previewHeight_ = 720;
  float visOffsetMs_ = 0.0f;

  double totalTimeSec_ = 0.0;
  double playAccumTimeSec_ = 0.0;
  double lastWallTimeSec_ = 0.0;
  iota::engine::RenderFrameStats frameStats_;

  iota::export_video::ExportSettings exportCfg_;
  bool openExportModal_ = false;
  std::string statusBanner_ = "Ready";
};

}  // namespace iota::studio
