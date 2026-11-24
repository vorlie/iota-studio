#include "iota/mcp/studio_session.hpp"

#include "iota/core/utf8_path.hpp"
#include "iota/mcp/png_encode.hpp"
#include "iota/mcp/working_copy.hpp"
#include "iota/preset_io/preset_io.hpp"
#include "iota/preset_io/project_manager.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace iota::mcp {

HeadlessStudioSession::HeadlessStudioSession()
    : project_(iota::model::createStarterProject()),
      undoStack_(100) {
  previewWidth_ = std::max(64, project_.metadata.previewWidth);
  previewHeight_ = std::max(64, project_.metadata.previewHeight);
  targetFps_ = std::clamp(project_.metadata.targetFps, 15, 120);
  undoStack_.clear(project_);
}

HeadlessStudioSession::~HeadlessStudioSession() {
  releaseGraphicsContext();
}

void HeadlessStudioSession::releaseGraphicsContext() {
  if (glReady_) {
    if (glWindow_) {
      glfwMakeContextCurrent(glWindow_);
    }
    renderer_.shutdown();
    device_.shutdown();
    glReady_ = false;
  }
  if (glWindow_) {
    glfwDestroyWindow(glWindow_);
    glWindow_ = nullptr;
    if (glfwTerminatesOnDestroy_) {
      glfwTerminate();
    }
  }
}

bool HeadlessStudioSession::ensureGlReady(std::string* outError) {
  if (glReady_ && glWindow_) {
    glfwMakeContextCurrent(glWindow_);
    return true;
  }

  if (!glfwInit()) {
    if (outError) *outError = "Failed to initialize GLFW for headless OpenGL 4.5 rendering.";
    return false;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

  glWindow_ = glfwCreateWindow(256, 256, "iota_mcp_headless", nullptr, nullptr);
  if (!glWindow_) {
    if (outError) *outError = "Failed to create hidden OpenGL 4.5 context (OpenGL 4.5 driver required).";
    glfwTerminate();
    return false;
  }
  glfwMakeContextCurrent(glWindow_);

  if (!device_.initialize(outError)) {
    glfwDestroyWindow(glWindow_);
    glWindow_ = nullptr;
    glfwTerminate();
    return false;
  }

  if (!renderer_.initialize(&device_, outError)) {
    device_.shutdown();
    glfwDestroyWindow(glWindow_);
    glWindow_ = nullptr;
    glfwTerminate();
    return false;
  }

  glReady_ = true;
  return true;
}

bool HeadlessStudioSession::loadProject(const std::filesystem::path& path,
                                        std::string* outError,
                                        std::filesystem::path* outWorkingCopyPath) {
  auto loaded = iota::preset_io::loadProjectFromFile(path, outError);
  if (!loaded) {
    return false;
  }
  project_ = std::move(*loaded);
  if (project_.metadata.name.empty() || project_.metadata.name == "Untitled Project") {
    project_.metadata.name = iota::core::pathStemUtf8(path);
  }
  currentPath_ = path;
  if (outWorkingCopyPath) {
    *outWorkingCopyPath = currentPath_;
  }
  previewWidth_ = std::max(64, project_.metadata.previewWidth);
  previewHeight_ = std::max(64, project_.metadata.previewHeight);
  targetFps_ = std::clamp(project_.metadata.targetFps, 15, 120);

  if (glReady_) {
    glfwMakeContextCurrent(glWindow_);
    renderer_.invalidateLocalAssets();
    renderer_.resetSimulation();
  }
  undoStack_.clear(project_);

  if (!project_.metadata.audioPath.empty()) {
    const auto audioPath = iota::core::pathFromUtf8(project_.metadata.audioPath);
    if (iota::core::pathExistsNoThrow(audioPath) && audio_.loadFile(audioPath)) {
      audio_.pause();
    }
  }
  return true;
}

bool HeadlessStudioSession::createNewProject(iota::model::Project project,
                                             const std::string& undoLabel) {
  project_ = std::move(project);
  currentPath_.clear();
  previewWidth_ = std::max(64, project_.metadata.previewWidth);
  previewHeight_ = std::max(64, project_.metadata.previewHeight);
  targetFps_ = std::clamp(project_.metadata.targetFps, 15, 120);
  if (glReady_) {
    glfwMakeContextCurrent(glWindow_);
    renderer_.invalidateLocalAssets();
    renderer_.resetSimulation();
  }
  undoStack_.clear(project_);
  (void)undoLabel;
  return true;
}

bool HeadlessStudioSession::saveProject(const std::filesystem::path& path,
                                        bool allowVizOverwrite,
                                        std::string* outError,
                                        std::filesystem::path* outSavedPath) {
  const std::filesystem::path target = path.empty() ? currentPath_ : path;
  if (target.empty()) {
    if (outError) {
      *outError = "No target path provided and current project has not been saved yet.";
    }
    return false;
  }

  if (isVizExtension(target) && !allowVizOverwrite && iota::core::pathExistsNoThrow(target)) {
    if (outError) {
      *outError =
          "Refusing to overwrite existing legacy .viz template '" + iota::core::pathToUtf8(target) +
          "' without allow_viz_overwrite=true (saving from Iota Studio updates .viz files and makes them "
          "incompatible with the legacy mobile player). Save to a .iota path instead.";
    }
    return false;
  }

  project_.metadata.previewWidth = previewWidth_;
  project_.metadata.previewHeight = previewHeight_;
  project_.metadata.targetFps = targetFps_;

  if (!iota::preset_io::saveProjectAutoFormat(project_, target, outError)) {
    return false;
  }
  currentPath_ = target;
  if (outSavedPath) {
    *outSavedPath = target;
  }
  return true;
}

void HeadlessStudioSession::commitEdit(const std::string& label,
                                       bool invalidateAssets,
                                       bool resetSim) {
  if (glReady_) {
    glfwMakeContextCurrent(glWindow_);
    if (invalidateAssets) {
      renderer_.invalidateLocalAssets();
    }
    if (resetSim) {
      renderer_.resetSimulation();
    }
  }
  undoStack_.pushSnapshot(label.empty() ? "MCP Edit" : label, project_);
}

bool HeadlessStudioSession::undo(std::string* outLabel) {
  if (!undoStack_.canUndo()) {
    return false;
  }
  const std::string desc = undoStack_.undoDescription();
  if (const auto* snap = undoStack_.undo()) {
    project_ = *snap;
    previewWidth_ = std::max(64, project_.metadata.previewWidth);
    previewHeight_ = std::max(64, project_.metadata.previewHeight);
    targetFps_ = std::clamp(project_.metadata.targetFps, 15, 120);
    if (glReady_) {
      glfwMakeContextCurrent(glWindow_);
      renderer_.invalidateLocalAssets();
    }
    if (outLabel) *outLabel = desc;
    return true;
  }
  return false;
}

bool HeadlessStudioSession::redo(std::string* outLabel) {
  if (!undoStack_.canRedo()) {
    return false;
  }
  const std::string desc = undoStack_.redoDescription();
  if (const auto* snap = undoStack_.redo()) {
    project_ = *snap;
    previewWidth_ = std::max(64, project_.metadata.previewWidth);
    previewHeight_ = std::max(64, project_.metadata.previewHeight);
    targetFps_ = std::clamp(project_.metadata.targetFps, 15, 120);
    if (glReady_) {
      glfwMakeContextCurrent(glWindow_);
      renderer_.invalidateLocalAssets();
    }
    if (outLabel) *outLabel = desc;
    return true;
  }
  return false;
}

bool HeadlessStudioSession::renderPreviewPng(int width,
                                             int height,
                                             double timeSec,
                                             int preRollFrames,
                                             bool fxaa,
                                             std::vector<uint8_t>& outPng,
                                             iota::engine::RenderFrameStats& outStats,
                                             std::string* outError) {
  if (!ensureGlReady(outError)) {
    return false;
  }

  const int w = std::clamp(width > 0 ? width : previewWidth_, 64, 4096);
  const int h = std::clamp(height > 0 ? height : previewHeight_, 64, 4096);
  const int roll = std::clamp(preRollFrames, 0, 120);
  const double tTarget = std::max(0.0, timeSec);
  const float dt = 1.0f / static_cast<float>(std::clamp(targetFps_, 15, 120));

  renderer_.resetSimulation();
  iota::rhi::RenderTargetHandle finalRt{};
  for (int i = -roll; i <= 0; ++i) {
    const double tFrame = std::max(0.0, tTarget + static_cast<double>(i) * dt);
    iota::engine::FrameTiming timing;
    timing.dt = dt;
    timing.totalTimeSec = std::max(0.0, static_cast<double>(i + roll) * dt);
    timing.playAccumTimeSec = tFrame;
    timing.isExporting = true;
    timing.fxaaEnabled = fxaa;
    timing.fpsStat = static_cast<float>(targetFps_);

    finalRt = renderer_.renderFrame(project_, audio_, timing, w, h, {}, &outStats);
    if (!finalRt) {
      if (outError) *outError = "SceneRenderer returned null render target.";
      return false;
    }
  }

  std::vector<uint8_t> rgba;
  int rw = 0;
  int rh = 0;
  if (!device_.readRenderTargetRgba(finalRt, rw, rh, rgba, true) || rw != w || rh != h) {
    if (outError) *outError = "Failed to read rendered preview framebuffer.";
    return false;
  }

  outPng = encodeRgbaToPng(rw, rh, rgba);
  if (outPng.empty()) {
    if (outError) *outError = "PNG compression failed.";
    return false;
  }
  return true;
}

bool HeadlessStudioSession::exportVideo(const iota::export_video::ExportSettings& settings,
                                        std::string* outError,
                                        std::string* outEncoderUsed,
                                        bool* outUsedFallback,
                                        std::string* outFallbackNotice) {
  if (!ensureGlReady(outError)) {
    return false;
  }

  iota::export_video::VideoExportSession session;
  if (!session.begin(settings, audio_, outError)) {
    return false;
  }
  if (outEncoderUsed) *outEncoderUsed = session.activeEncoderDisplayName();
  if (outUsedFallback) *outUsedFallback = session.usedCpuFallback();
  if (outFallbackNotice) *outFallbackNotice = session.fallbackNotice();

  renderer_.resetSimulation();
  while (!session.isDone()) {
    if (!session.stepFrame(device_, renderer_, project_, audio_, outError)) {
      session.cancel();
      return false;
    }
  }
  return session.finish(outError);
}

bool HeadlessStudioSession::controlPlayback(std::string_view action,
                                            std::optional<double> seekSec,
                                            std::string* outError) {
  if (seekSec.has_value()) {
    audio_.seekSeconds(std::max(0.0, *seekSec));
  }
  if (action == "play") {
    audio_.play();
  } else if (action == "pause") {
    audio_.pause();
  } else if (action == "stop") {
    audio_.stop();
  } else if (action == "toggle") {
    if (audio_.isPlaying()) audio_.pause();
    else audio_.play();
  } else if (action == "seek") {
    if (!seekSec.has_value()) {
      if (outError) *outError = "Action 'seek' requires seek_sec.";
      return false;
    }
  } else if (!action.empty()) {
    if (outError) *outError = "Unknown playback action: " + std::string(action);
    return false;
  }
  return true;
}

void HeadlessStudioSession::setPreviewMetrics(int width, int height, int fps) {
  if (width > 0) {
    previewWidth_ = std::clamp(width, 64, 4096);
    project_.metadata.previewWidth = previewWidth_;
  }
  if (height > 0) {
    previewHeight_ = std::clamp(height, 64, 4096);
    project_.metadata.previewHeight = previewHeight_;
  }
  if (fps > 0) {
    targetFps_ = std::clamp(fps, 15, 120);
    project_.metadata.targetFps = targetFps_;
  }
}

}  // namespace iota::mcp
