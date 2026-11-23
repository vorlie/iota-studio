#include "editor_app.hpp"

#include "inspector_panel.hpp"

#include "iota/preset_io/preset_io.hpp"

#include <windows.h>
#include <commdlg.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace iota::studio {

namespace {

std::optional<std::filesystem::path> showOpenFileDialog(HWND owner,
                                                        const wchar_t* title,
                                                        const wchar_t* filter) {
  wchar_t fileBuf[MAX_PATH]{};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = fileBuf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetOpenFileNameW(&ofn)) {
    return std::filesystem::path(fileBuf);
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> showSaveFileDialog(HWND owner,
                                                        const wchar_t* title,
                                                        const wchar_t* filter,
                                                        const wchar_t* defExt,
                                                        const wchar_t* initialName) {
  wchar_t fileBuf[MAX_PATH]{};
  if (initialName) {
    wcsncpy_s(fileBuf, initialName, MAX_PATH - 1);
  }
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = fileBuf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = title;
  ofn.lpstrDefExt = defExt;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetSaveFileNameW(&ofn)) {
    return std::filesystem::path(fileBuf);
  }
  return std::nullopt;
}

std::vector<uint8_t> readAllBytes(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f.is_open()) return {};
  const auto sz = f.tellg();
  if (sz <= 0) return {};
  std::vector<uint8_t> out(static_cast<size_t>(sz));
  f.seekg(0, std::ios::beg);
  f.read(reinterpret_cast<char*>(out.data()), sz);
  return out;
}

}  // namespace

EditorApp::EditorApp() = default;

EditorApp::~EditorApp() {
  shutdown();
}

bool EditorApp::initialize(bool headlessSmokeTest) {
  if (!glfwInit()) {
    std::cerr << "Failed to initialize GLFW\n";
    return false;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, headlessSmokeTest ? GLFW_FALSE : GLFW_TRUE);

  window_ = glfwCreateWindow(headlessSmokeTest ? 320 : 1600,
                             headlessSmokeTest ? 240 : 940,
                             "Iota Studio",
                             nullptr,
                             nullptr);
  if (!window_) {
    std::cerr << "Failed to create GLFW OpenGL 4.5 window\n";
    glfwTerminate();
    return false;
  }

  glfwMakeContextCurrent(window_);
  glfwSwapInterval(headlessSmokeTest ? 0 : 1);

  std::string err;
  if (!device_.initialize(&err)) {
    std::cerr << "GL45 device init failed: " << err << "\n";
    return false;
  }
  if (!renderer_.initialize(&device_, &err)) {
    std::cerr << "SceneRenderer init failed: " << err << "\n";
    return false;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.IniFilename = headlessSmokeTest ? nullptr : "iota_studio_layout.ini";

  applyStudioTheme();

  if (!ImGui_ImplGlfw_InitForOpenGL(window_, true) ||
      !ImGui_ImplOpenGL3_Init("#version 450")) {
    std::cerr << "Failed to initialize Dear ImGui GLFW/OpenGL3 backends\n";
    return false;
  }
  imguiInitialized_ = true;

  project_ = iota::model::createStarterProject();
  if (!project_.compositions.empty() && project_.compositions[0].elements.size() > 3) {
    selectedCompIdx_ = 0;
    selectedElemIdx_ = 3;  // Circular Spectrum default selection
  }
  undoStack_.clear(project_);

  actionLoadBuiltinTestTone(110.0f);
  lastWallTimeSec_ = glfwGetTime();
  return true;
}

void EditorApp::shutdown() {
  exportSession_.cancel();
  renderer_.shutdown();
  device_.shutdown();

  if (imguiInitialized_) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    imguiInitialized_ = false;
  }
  if (window_) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
  }
}

void EditorApp::applyStudioTheme() {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 4.0f;
  style.FrameRounding = 3.0f;
  style.GrabRounding = 3.0f;
  style.TabRounding = 3.0f;
  style.ScrollbarRounding = 4.0f;
  style.FramePadding = ImVec2(6.0f, 4.0f);
  style.ItemSpacing = ImVec2(7.0f, 5.0f);

  auto* c = style.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.085f, 0.095f, 0.115f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.16f, 0.24f, 0.34f, 0.85f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.36f, 0.50f, 0.90f);
  c[ImGuiCol_Button] = ImVec4(0.16f, 0.25f, 0.36f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.39f, 0.56f, 1.0f);
  c[ImGuiCol_CheckMark] = ImVec4(0.25f, 0.82f, 0.98f, 1.0f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.22f, 0.72f, 0.92f, 1.0f);
  c[ImGuiCol_TabActive] = ImVec4(0.18f, 0.30f, 0.44f, 1.0f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.11f, 0.14f, 0.19f, 1.0f);
}

void EditorApp::setupDefaultDockLayout(unsigned int dockspaceId) {
  dockLayoutBuilt_ = true;
  if (std::filesystem::exists("iota_studio_layout.ini")) {
    return;
  }

  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

  ImGuiID dockMain = dockspaceId;
  ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.20f, nullptr, &dockMain);
  ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.28f, nullptr, &dockMain);
  ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.26f, nullptr, &dockMain);

  ImGui::DockBuilderDockWindow("Hierarchy", dockLeft);
  ImGui::DockBuilderDockWindow("Inspector", dockRight);
  ImGui::DockBuilderDockWindow("Audio & Timeline", dockBottom);
  ImGui::DockBuilderDockWindow("Viewport", dockMain);
  ImGui::DockBuilderFinish(dockspaceId);
}

int EditorApp::run() {
  while (!glfwWindowShouldClose(window_)) {
    glfwPollEvents();
    handleShortcuts();

    const double now = glfwGetTime();
    const float rawDt = static_cast<float>(now - lastWallTimeSec_);
    lastWallTimeSec_ = now;
    const float dt = std::clamp(rawDt, 1.0f / 240.0f, 0.1f);
    totalTimeSec_ += dt;
    if (audio_.isPlaying()) {
      playAccumTimeSec_ += dt;
    }

    // Progress active offline video export incrementally so UI stays smooth
    if (exportSession_.isActive()) {
      std::string err;
      for (int batch = 0; batch < 2 && !exportSession_.isDone(); ++batch) {
        if (!exportSession_.stepFrame(device_, renderer_, project_, audio_, &err)) {
          exportSession_.cancel();
          statusBanner_ = "Export error: " + err;
          break;
        }
      }
      if (exportSession_.isDone()) {
        std::string finErr;
        if (exportSession_.finish(&finErr)) {
          statusBanner_ = "Export finished: " + exportCfg_.outputPath.string();
        } else {
          statusBanner_ = "Export finish error: " + finErr;
        }
      }
    } else {
      iota::engine::FrameTiming timing;
      timing.dt = dt;
      timing.totalTimeSec = totalTimeSec_;
      timing.playAccumTimeSec = playAccumTimeSec_;
      timing.isExporting = false;
      timing.visOffsetMs = visOffsetMs_;
      timing.fxaaEnabled = fxaaEnabled_;
      timing.fpsStat = ImGui::GetIO().Framerate;
      renderer_.renderFrame(project_, audio_, timing, previewWidth_, previewHeight_, {}, &frameStats_);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const ImGuiID dockId = ImGui::DockSpaceOverViewport(
        0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
    if (!dockLayoutBuilt_) {
      setupDefaultDockLayout(dockId);
    }

    drawMenuBar();
    drawViewportPanel();
    drawHierarchyPanel();
    drawInspectorPanel();
    drawAudioTimelinePanel();
    drawExportModal();

    ImGui::Render();

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    device_.bindRenderTarget({}, fbW, fbH);
    device_.clear(0.05f, 0.055f, 0.07f, 1.0f);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window_);
  }
  return 0;
}

int EditorApp::runSmokeTest() {
  for (int i = 0; i < 5; ++i) {
    iota::engine::FrameTiming timing;
    timing.dt = 1.0f / 60.0f;
    timing.totalTimeSec = static_cast<double>(i + 1) * (1.0 / 60.0);
    timing.playAccumTimeSec = timing.totalTimeSec;
    timing.isExporting = true;
    timing.fxaaEnabled = true;

    const auto rt = renderer_.renderFrame(project_, audio_, timing, 640, 360, {}, &frameStats_);
    if (!rt) {
      std::cerr << "[smoke-test] SceneRenderer returned null target on frame " << i << "\n";
      return 2;
    }
  }

  std::vector<uint8_t> pixels;
  int rw = 0, rh = 0;
  if (!device_.readRenderTargetRgba(renderer_.finalRenderTarget(), rw, rh, pixels, true) ||
      rw != 640 || rh != 360 || pixels.size() != static_cast<size_t>(640 * 360 * 4)) {
    std::cerr << "[smoke-test] Pixel readback mismatch\n";
    return 3;
  }

  // Verify non-empty rendered output
  uint64_t colorSum = 0;
  for (size_t i = 0; i < pixels.size(); i += 4) {
    colorSum += pixels[i] + pixels[i + 1] + pixels[i + 2];
  }
  if (colorSum == 0) {
    std::cerr << "[smoke-test] Rendered output was completely black\n";
    return 4;
  }

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
  setupDefaultDockLayout(dockId);
  drawMenuBar();
  drawViewportPanel();
  drawHierarchyPanel();
  drawInspectorPanel();
  drawAudioTimelinePanel();
  ImGui::Render();
  device_.bindRenderTarget({}, 320, 240);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  std::cout << "[smoke-test] OK draws=" << frameStats_.drawCalls
            << " comps=" << frameStats_.compositionsRendered
            << " beat=" << frameStats_.beat << "\n";
  return 0;
}

void EditorApp::handleShortcuts() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;

  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
    if (const auto* snap = undoStack_.undo()) {
      project_ = *snap;
      renderer_.invalidateLocalAssets();
      statusBanner_ = "Undo";
    }
  } else if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))) {
    if (const auto* snap = undoStack_.redo()) {
      project_ = *snap;
      renderer_.invalidateLocalAssets();
      statusBanner_ = "Redo";
    }
  } else if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
    if (audio_.isPlaying()) audio_.pause();
    else audio_.play();
  }
}

void EditorApp::pushUndoState(std::string label) {
  undoStack_.pushSnapshot(std::move(label), project_);
}

void EditorApp::drawMenuBar() {
  if (!ImGui::BeginMainMenuBar()) return;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("New Starter Project")) {
      project_ = iota::model::createStarterProject();
      selectedCompIdx_ = 0;
      selectedElemIdx_ = 0;
      renderer_.resetSimulation();
      undoStack_.clear(project_);
      statusBanner_ = "Created starter project";
    }
    if (ImGui::MenuItem("Open Preset (.viz / .json)...")) {
      actionOpenPreset();
    }
    if (ImGui::MenuItem("Save Preset as .viz...")) {
      actionSavePreset(true);
    }
    if (ImGui::MenuItem("Save Preset as .json...")) {
      actionSavePreset(false);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Open Audio Track...")) {
      actionOpenAudio();
    }
    if (ImGui::MenuItem("Load 110 Hz Synth Bass Test Tone")) {
      actionLoadBuiltinTestTone(110.0f);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Export Video (MP4)...")) {
      openExportModal_ = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Exit")) {
      glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit")) {
    const std::string undoLabel = undoStack_.canUndo()
                                      ? ("Undo " + undoStack_.undoDescription() + "\tCtrl+Z")
                                      : "Undo\tCtrl+Z";
    const std::string redoLabel = undoStack_.canRedo()
                                      ? ("Redo " + undoStack_.redoDescription() + "\tCtrl+Y")
                                      : "Redo\tCtrl+Y";
    if (ImGui::MenuItem(undoLabel.c_str(), nullptr, false, undoStack_.canUndo())) {
      if (const auto* snap = undoStack_.undo()) {
        project_ = *snap;
        renderer_.invalidateLocalAssets();
      }
    }
    if (ImGui::MenuItem(redoLabel.c_str(), nullptr, false, undoStack_.canRedo())) {
      if (const auto* snap = undoStack_.redo()) {
        project_ = *snap;
        renderer_.invalidateLocalAssets();
      }
    }
    ImGui::Separator();
    ImGui::MenuItem("Show Advanced Hidden Properties", nullptr, &showHiddenProps_);
    ImGui::MenuItem("Precise Numeric Inputs", nullptr, &preciseNumericInputs_);
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Viewport")) {
    if (ImGui::MenuItem("HD 720p (1280x720)", nullptr, previewWidth_ == 1280 && previewHeight_ == 720)) {
      previewWidth_ = 1280; previewHeight_ = 720;
    }
    if (ImGui::MenuItem("Full HD 1080p (1920x1080)", nullptr, previewWidth_ == 1920 && previewHeight_ == 1080)) {
      previewWidth_ = 1920; previewHeight_ = 1080;
    }
    if (ImGui::MenuItem("Vertical 9:16 (1080x1920)", nullptr, previewWidth_ == 1080 && previewHeight_ == 1920)) {
      previewWidth_ = 1080; previewHeight_ = 1920;
    }
    if (ImGui::MenuItem("Square 1:1 (1080x1080)", nullptr, previewWidth_ == 1080 && previewHeight_ == 1080)) {
      previewWidth_ = 1080; previewHeight_ = 1080;
    }
    ImGui::Separator();
    ImGui::MenuItem("FXAA Anti-Aliasing", nullptr, &fxaaEnabled_);
    ImGui::EndMenu();
  }

  ImGui::Separator();
  ImGui::TextDisabled("%s | %dx%d | %.1f FPS | Draws: %d | Beat: %.2f",
                      statusBanner_.c_str(),
                      previewWidth_,
                      previewHeight_,
                      ImGui::GetIO().Framerate,
                      frameStats_.drawCalls,
                      frameStats_.beat);

  ImGui::EndMainMenuBar();
}

void EditorApp::drawViewportPanel() {
  ImGui::Begin("Viewport");
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const auto tex = renderer_.finalOutputTexture();
  if (tex && avail.x > 8.0f && avail.y > 8.0f) {
    const float srcAspect = static_cast<float>(previewWidth_) / static_cast<float>(previewHeight_);
    float drawW = avail.x;
    float drawH = drawW / srcAspect;
    if (drawH > avail.y) {
      drawH = avail.y;
      drawW = drawH * srcAspect;
    }
    const float padX = std::max(0.0f, (avail.x - drawW) * 0.5f);
    const float padY = std::max(0.0f, (avail.y - drawH) * 0.5f);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + padX, ImGui::GetCursorPosY() + padY));

    const auto glId = static_cast<ImTextureID>(device_.nativeTextureId(tex));
    // OpenGL FBO color attachment has V=1 at screen top
    ImGui::Image(glId, ImVec2(drawW, drawH), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
  } else {
    ImGui::TextDisabled("No active render target.");
  }
  ImGui::End();
}

void EditorApp::drawHierarchyPanel() {
  ImGui::Begin("Hierarchy");

  if (ImGui::Button("+ Composition")) {
    const int nextIdx = static_cast<int>(project_.compositions.size());
    project_.compositions.push_back(iota::model::createDefaultComposition(nextIdx));
    selectedCompIdx_ = nextIdx;
    selectedElemIdx_ = -1;
    pushUndoState("Add Composition");
  }
  ImGui::SameLine();
  if (ImGui::Button("+ Element") &&
      selectedCompIdx_ >= 0 && selectedCompIdx_ < static_cast<int>(project_.compositions.size())) {
    ImGui::OpenPopup("AddElementPopup");
  }

  if (ImGui::BeginPopup("AddElementPopup")) {
    for (iota::model::ElementType et : iota::model::addableElementTypes()) {
      const std::string_view label = iota::model::elementTypeToString(et);
      std::string s(label);
      if (ImGui::MenuItem(s.c_str())) {
        auto& elems = project_.compositions[static_cast<size_t>(selectedCompIdx_)].elements;
        elems.push_back(iota::model::createDefaultElement(et));
        selectedElemIdx_ = static_cast<int>(elems.size()) - 1;
        pushUndoState("Add " + s);
      }
    }
    ImGui::EndPopup();
  }

  ImGui::Separator();

  int compToDelete = -1;
  for (int ci = 0; ci < static_cast<int>(project_.compositions.size()); ++ci) {
    auto& comp = project_.compositions[static_cast<size_t>(ci)];
    ImGui::PushID(ci);

    const bool compSelected = (selectedCompIdx_ == ci && selectedElemIdx_ < 0);
    std::string compTitle = "[" + std::to_string(ci) + "] " + comp.displayName;
    const ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen |
        (compSelected ? ImGuiTreeNodeFlags_Selected : 0);

    const bool open = ImGui::TreeNodeEx(compTitle.c_str(), flags);
    if (ImGui::IsItemClicked()) {
      selectedCompIdx_ = ci;
      selectedElemIdx_ = -1;
    }
    if (ci > 0 && ImGui::BeginPopupContextItem()) {
      if (ImGui::MenuItem("Delete Composition")) compToDelete = ci;
      ImGui::EndPopup();
    }

    if (open) {
      int moveFrom = -1;
      int moveTo = -1;
      int deleteElem = -1;
      int dupElem = -1;

      for (int ei = 0; ei < static_cast<int>(comp.elements.size()); ++ei) {
        auto& el = comp.elements[static_cast<size_t>(ei)];
        ImGui::PushID(ei);

        bool vis = el.props.getBool("visible", true);
        if (el.type != iota::model::ElementType::AudioProvider) {
          if (ImGui::Checkbox("##vis", &vis)) {
            el.props.setBool("visible", vis, "0_general",
                             el.type == iota::model::ElementType::AppLogo ? "pb" : "b");
            pushUndoState("Toggle Visible");
          }
          ImGui::SameLine();
        }

        const bool selected = (selectedCompIdx_ == ci && selectedElemIdx_ == ei);
        const std::string elLabel = el.displayName.empty()
                                        ? el.typeName
                                        : (el.displayName + " (" + el.typeName + ")");
        if (ImGui::Selectable(elLabel.c_str(), selected)) {
          selectedCompIdx_ = ci;
          selectedElemIdx_ = ei;
        }
        if (ImGui::BeginPopupContextItem()) {
          if (ImGui::MenuItem("Move Up", nullptr, false, ei > 0)) {
            moveFrom = ei; moveTo = ei - 1;
          }
          if (ImGui::MenuItem("Move Down", nullptr, false, ei + 1 < static_cast<int>(comp.elements.size()))) {
            moveFrom = ei; moveTo = ei + 1;
          }
          if (ImGui::MenuItem("Duplicate")) dupElem = ei;
          ImGui::Separator();
          if (ImGui::MenuItem("Delete")) deleteElem = ei;
          ImGui::EndPopup();
        }
        ImGui::PopID();
      }

      if (moveFrom >= 0 && moveTo >= 0) {
        std::swap(comp.elements[static_cast<size_t>(moveFrom)],
                  comp.elements[static_cast<size_t>(moveTo)]);
        selectedElemIdx_ = moveTo;
        pushUndoState("Reorder Element");
      } else if (dupElem >= 0) {
        auto copy = comp.elements[static_cast<size_t>(dupElem)];
        copy.displayName += " Copy";
        comp.elements.insert(comp.elements.begin() + dupElem + 1, std::move(copy));
        selectedElemIdx_ = dupElem + 1;
        pushUndoState("Duplicate Element");
      } else if (deleteElem >= 0) {
        comp.elements.erase(comp.elements.begin() + deleteElem);
        if (selectedElemIdx_ == deleteElem) selectedElemIdx_ = -1;
        else if (selectedElemIdx_ > deleteElem) selectedElemIdx_ -= 1;
        pushUndoState("Delete Element");
      }

      ImGui::TreePop();
    }
    ImGui::PopID();
  }

  if (compToDelete > 0) {
    project_.compositions.erase(project_.compositions.begin() + compToDelete);
    selectedCompIdx_ = 0;
    selectedElemIdx_ = -1;
    pushUndoState("Delete Composition");
  }

  ImGui::End();
}

void EditorApp::drawInspectorPanel() {
  ImGui::Begin("Inspector");

  if (selectedCompIdx_ < 0 || selectedCompIdx_ >= static_cast<int>(project_.compositions.size())) {
    ImGui::TextDisabled("Select a composition or element in the Hierarchy.");
    ImGui::End();
    return;
  }

  auto& comp = project_.compositions[static_cast<size_t>(selectedCompIdx_)];
  ImGui::Checkbox("Precise Inputs", &preciseNumericInputs_);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Switch sliders to exact numeric text inputs (tip: Ctrl+Click any slider to type a value too)");
  }
  ImGui::Separator();

  InspectorActions actions;
  actions.showHiddenProperties = showHiddenProps_;
  actions.preciseNumericInputs = preciseNumericInputs_;

  if (selectedElemIdx_ >= 0 && selectedElemIdx_ < static_cast<int>(comp.elements.size())) {
    auto& el = comp.elements[static_cast<size_t>(selectedElemIdx_)];
    ImGui::TextWrapped("%s  [%s]", el.displayName.c_str(), el.typeName.c_str());
    char nameBuf[256]{};
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", el.displayName.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Layer Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##LayerName", nameBuf, sizeof(nameBuf))) {
      el.displayName = nameBuf;
      actions.modified = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Rename Layer";
    }

    if (el.type == iota::model::ElementType::AudioProvider) {
      const bool missingTemporal =
          !el.props.contains("temporalSmooth") || !el.props.contains("attackMs") || !el.props.contains("releaseMs");
      const bool missingFreqSmooth = !el.props.contains("freqSmoothMode");
      if (missingTemporal) {
        if (ImGui::SmallButton("Enable Attack/Release Smoothing")) {
          if (!el.props.contains("temporalSmooth")) el.props.setFloat("temporalSmooth", 0.5f, 0.0f, 1.0f, "2_temporal");
          if (!el.props.contains("attackMs")) el.props.setFloat("attackMs", 25.0f, 0.0f, 250.0f, "2_temporal");
          if (!el.props.contains("releaseMs")) el.props.setFloat("releaseMs", 150.0f, 0.0f, 1000.0f, "2_temporal");
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Enable Attack/Release Smoothing";
        }
      }
      if (missingFreqSmooth) {
        if (ImGui::SmallButton("Enable Frequency Smoothing")) {
          el.props.setString("freqSmoothMode", "None",
                             "sel None MovingAverage Gaussian SavitzkyGolay",
                             "3_frequencySmooth");
          if (!el.props.contains("freqSmoothRadius")) el.props.setFloat("freqSmoothRadius", 2.0f, 0.0f, 8.0f, "3_frequencySmooth");
          if (!el.props.contains("freqSmoothStrength")) el.props.setFloat("freqSmoothStrength", 0.7f, 0.0f, 1.0f, "3_frequencySmooth");
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Enable Frequency Smoothing";
        }
      }
    } else if (el.type == iota::model::ElementType::Bars) {
      if (!el.props.contains("curveInterpolation") || !el.props.contains("curveSubdivisions")) {
        if (ImGui::SmallButton("Enable Curve Interpolation")) {
          if (!el.props.contains("curveInterpolation")) {
            el.props.setString("curveInterpolation", "None", "sel None Linear CatmullRom CubicBSpline", "1_bars");
          }
          if (!el.props.contains("curveSubdivisions")) {
            el.props.setInt("curveSubdivisions", 4, 1, 8, "1_bars");
          }
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Enable Curve Interpolation";
        }
      }
    }

    ImGui::Separator();

    renderPropertyBagInspector(el.props, actions);

    if (!actions.addKeyframeProperty.empty()) {
      const double tSec = audio_.currentTimeSeconds();
      bool found = false;
      for (auto& track : project_.desktopTracks) {
        if (track.compositionIndex == selectedCompIdx_ &&
            track.elementIndex == selectedElemIdx_ &&
            track.propertyPath == actions.addKeyframeProperty) {
          track.keyframes.push_back({tSec, actions.addKeyframeValue});
          std::sort(track.keyframes.begin(), track.keyframes.end(),
                    [](const auto& a, const auto& b) { return a.timeSec < b.timeSec; });
          found = true;
          break;
        }
      }
      if (!found) {
        iota::model::KeyframeTrack tr;
        tr.compositionIndex = selectedCompIdx_;
        tr.elementIndex = selectedElemIdx_;
        tr.propertyPath = actions.addKeyframeProperty;
        tr.keyframes.push_back({tSec, actions.addKeyframeValue});
        project_.desktopTracks.push_back(std::move(tr));
      }
      pushUndoState("Add Keyframe");
      statusBanner_ = "Keyframed " + actions.addKeyframeProperty;
    }

    if (actions.requestLocalAssetImport && !actions.assetTargetPropName.empty() && window_) {
      HWND hwnd = glfwGetWin32Window(window_);
      if (auto chosen = showOpenFileDialog(hwnd,
                                             L"Import Image Asset into Preset",
                                             L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All Files (*.*)\0*.*\0")) {
        auto bytes = readAllBytes(*chosen);
        if (!bytes.empty()) {
          const std::string fname = chosen->filename().string();
          project_.localAssets[fname] = std::move(bytes);
          el.props.setString(actions.assetTargetPropName, "local:" + fname, "img", "1_image");
          renderer_.invalidateLocalAssets();
          pushUndoState("Import Local Asset");
          statusBanner_ = "Bundled asset local:" + fname;
        }
      }
    }
  } else {
    ImGui::TextWrapped("Composition %d: %s", selectedCompIdx_, comp.displayName.c_str());
    ImGui::Separator();
    renderPropertyBagInspector(comp.props, actions);
  }

  if (actions.undoSnapshotNeeded) {
    pushUndoState(actions.editLabel.empty() ? "Modify Property" : actions.editLabel);
  }

  ImGui::End();
}

void EditorApp::drawAudioTimelinePanel() {
  ImGui::Begin("Audio & Timeline");

  if (ImGui::Button("Open Audio...")) {
    actionOpenAudio();
  }
  ImGui::SameLine();
  if (audio_.isPlaying()) {
    if (ImGui::Button("Pause")) audio_.pause();
  } else {
    if (ImGui::Button("Play")) audio_.play();
  }
  ImGui::SameLine();
  if (ImGui::Button("Stop")) {
    audio_.stop();
    playAccumTimeSec_ = 0.0;
    renderer_.resetSimulation();
  }
  ImGui::SameLine();
  if (ImGui::Button("Export MP4...")) {
    openExportModal_ = true;
  }

  float pos = static_cast<float>(audio_.currentTimeSeconds());
  const float dur = static_cast<float>(std::max(0.1, audio_.durationSeconds()));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(260.0f);
  if (ImGui::SliderFloat("##seek", &pos, 0.0f, dur, "%.2f s")) {
    audio_.seekSeconds(pos);
  }
  ImGui::SameLine();
  ImGui::Text(" / %.2f s  |  %s", dur, audio_.currentTitle().c_str());

  float vol = audio_.volume();
  ImGui::SetNextItemWidth(140.0f);
  if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.2f, "%.2f")) {
    audio_.setVolume(vol);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140.0f);
  ImGui::SliderFloat("Vis Offset (ms)", &visOffsetMs_, -250.0f, 250.0f, "%.0f ms");
  ImGui::SameLine();
  ImGui::ProgressBar(std::clamp(frameStats_.beat, 0.0f, 1.0f), ImVec2(140.0f, 0.0f), "Beat");

  if (!frameStats_.lastBars.empty()) {
    const float peakVal = *std::max_element(frameStats_.lastBars.begin(), frameStats_.lastBars.end());
    const float histMax = std::max(50.0f, peakVal * 1.15f);
    ImGui::PlotHistogram("##spectrum",
                         frameStats_.lastBars.data(),
                         static_cast<int>(frameStats_.lastBars.size()),
                         0,
                         "Real-time Audio Analyzer",
                         0.0f,
                         histMax,
                         ImVec2(-FLT_MIN, 75.0f));
  }

  if (!project_.desktopTracks.empty()) {
    ImGui::Separator();
    ImGui::Text("Keyframe Tracks (%zu):", project_.desktopTracks.size());
    int delTrack = -1;
    for (int i = 0; i < static_cast<int>(project_.desktopTracks.size()); ++i) {
      const auto& tr = project_.desktopTracks[static_cast<size_t>(i)];
      ImGui::Text("  Comp[%d].Elem[%d].%s (%zu keys)",
                  tr.compositionIndex, tr.elementIndex, tr.propertyPath.c_str(), tr.keyframes.size());
      ImGui::SameLine();
      ImGui::PushID(i);
      if (ImGui::SmallButton("Remove")) delTrack = i;
      ImGui::PopID();
    }
    if (delTrack >= 0) {
      project_.desktopTracks.erase(project_.desktopTracks.begin() + delTrack);
      pushUndoState("Remove Keyframe Track");
    }
  }

  ImGui::End();
}

void EditorApp::drawExportModal() {
  if (openExportModal_) {
    ImGui::OpenPopup("Export Video (MP4)");
    openExportModal_ = false;
  }

  if (ImGui::BeginPopupModal("Export Video (MP4)", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const bool exporting = exportSession_.isActive();
    ImGui::BeginDisabled(exporting);

    if (ImGui::Button("720p60")) {
      exportCfg_.width = 1280; exportCfg_.height = 720; exportCfg_.fps = 60; exportCfg_.videoBitrateKbps = 12000;
    }
    ImGui::SameLine();
    if (ImGui::Button("1080p60")) {
      exportCfg_.width = 1920; exportCfg_.height = 1080; exportCfg_.fps = 60; exportCfg_.videoBitrateKbps = 18000;
    }
    ImGui::SameLine();
    if (ImGui::Button("Shorts 1080x1920")) {
      exportCfg_.width = 1080; exportCfg_.height = 1920; exportCfg_.fps = 60; exportCfg_.videoBitrateKbps = 16000;
    }

    ImGui::InputInt("Width", &exportCfg_.width);
    ImGui::InputInt("Height", &exportCfg_.height);
    ImGui::InputInt("FPS", &exportCfg_.fps);
    ImGui::InputInt("Bitrate (kbps)", &exportCfg_.videoBitrateKbps);
    ImGui::Checkbox("HEVC / H.265 (libx265)", &exportCfg_.useHevc);
    ImGui::Checkbox("FXAA Anti-Aliasing", &exportCfg_.fxaa);
    ImGui::InputDouble("Start (sec)", &exportCfg_.startSec, 0.5, 5.0, "%.2f");
    ImGui::InputDouble("End (sec, 0=full)", &exportCfg_.endSec, 0.5, 5.0, "%.2f");

    char outBuf[512]{};
    std::snprintf(outBuf, sizeof(outBuf), "%s", exportCfg_.outputPath.string().c_str());
    if (ImGui::InputText("Output MP4", outBuf, sizeof(outBuf))) {
      exportCfg_.outputPath = std::filesystem::path(outBuf);
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...") && window_) {
      if (auto chosen = showSaveFileDialog(glfwGetWin32Window(window_),
                                             L"Save Exported Video",
                                             L"MP4 Video (*.mp4)\0*.mp4\0",
                                             L"mp4",
                                             L"iota_export.mp4")) {
        exportCfg_.outputPath = *chosen;
      }
    }

    ImGui::EndDisabled();

    ImGui::Separator();
    if (exporting) {
      const float prog = exportSession_.totalFrames() > 0
                             ? static_cast<float>(exportSession_.encodedFrames()) /
                                   static_cast<float>(exportSession_.totalFrames())
                             : 0.0f;
      ImGui::ProgressBar(prog, ImVec2(320.0f, 0.0f));
      ImGui::Text("Encoding frame %d / %d (pre-roll %d / %d)...",
                  exportSession_.encodedFrames(),
                  exportSession_.totalFrames(),
                  exportSession_.currentPreRollIndex(),
                  exportCfg_.preRollFrames);
      if (ImGui::Button("Cancel Export")) {
        exportSession_.cancel();
        statusBanner_ = "Export cancelled";
      }
    } else {
      if (ImGui::Button("Start Export")) {
        exportCfg_.visOffsetMs = visOffsetMs_;
        std::string err;
        renderer_.resetSimulation();
        if (exportSession_.begin(exportCfg_, audio_, &err)) {
          statusBanner_ = "Exporting MP4...";
        } else {
          statusBanner_ = "Export failed to start: " + err;
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
      }
    }

    ImGui::EndPopup();
  }
}

void EditorApp::actionOpenPreset() {
  if (!window_) return;
  auto chosen = showOpenFileDialog(glfwGetWin32Window(window_),
                                     L"Open Visualizer Preset (.viz / .json)",
                                     L"Visualizer Presets (*.viz;*.json;*.txt)\0*.viz;*.json;*.txt\0All Files (*.*)\0*.*\0");
  if (!chosen) return;

  std::string err;
  auto loaded = iota::preset_io::loadProjectFromFile(*chosen, &err);
  if (!loaded) {
    statusBanner_ = "Failed to load preset: " + err;
    return;
  }
  project_ = std::move(*loaded);
  selectedCompIdx_ = 0;
  selectedElemIdx_ = project_.compositions.empty() || project_.compositions[0].elements.empty() ? -1 : 0;
  renderer_.invalidateLocalAssets();
  renderer_.resetSimulation();
  pushUndoState("Open Preset");
  statusBanner_ = "Loaded " + chosen->filename().string();
}

void EditorApp::actionSavePreset(bool asViz) {
  if (!window_) return;
  auto chosen = showSaveFileDialog(glfwGetWin32Window(window_),
                                     asViz ? L"Export Preset (.viz)" : L"Save Scene JSON (.json)",
                                     asViz ? L"Visualizer Preset (*.viz)\0*.viz\0" : L"JSON Preset (*.json)\0*.json\0",
                                     asViz ? L"viz" : L"json",
                                     asViz ? L"preset.viz" : L"scene.json");
  if (!chosen) return;

  std::string err;
  const bool ok = asViz ? iota::preset_io::saveProjectAsViz(project_, *chosen, &err)
                        : iota::preset_io::saveProjectAsJson(project_, *chosen, &err);
  statusBanner_ = ok ? ("Saved " + chosen->filename().string()) : ("Save failed: " + err);
}

void EditorApp::actionOpenAudio() {
  if (!window_) return;
  auto chosen = showOpenFileDialog(glfwGetWin32Window(window_),
                                     L"Open Audio Track",
                                     L"Audio Files (*.mp3;*.wav;*.flac;*.ogg;*.m4a;*.aac;*.opus)\0*.mp3;*.wav;*.flac;*.ogg;*.m4a;*.aac;*.opus\0All Files (*.*)\0*.*\0");
  if (!chosen) return;

  std::string err;
  if (audio_.loadFile(*chosen, &err)) {
    playAccumTimeSec_ = 0.0;
    renderer_.resetSimulation();
    audio_.play();
    statusBanner_ = "Playing " + chosen->filename().string();
  } else {
    statusBanner_ = "Audio load failed: " + err;
  }
}

void EditorApp::actionLoadBuiltinTestTone(float freqHz) {
  auto track = iota::audio::createSyntheticSineTrack(freqHz, 8.0, 44100, 0.35f, "110 Hz Synth Test Tone");
  audio_.loadTrack(std::move(track));
  playAccumTimeSec_ = 0.0;
}

}  // namespace iota::studio
