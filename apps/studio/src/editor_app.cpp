#include "editor_app.hpp"

#include "inspector_panel.hpp"
#include "sound_keys_panel.hpp"

#include "iota/core/utf8_path.hpp"
#include "iota/preset_io/preset_io.hpp"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

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
  std::vector<wchar_t> fileBuf(4096, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = fileBuf.data();
  ofn.nMaxFile = static_cast<DWORD>(fileBuf.size());
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetOpenFileNameW(&ofn)) {
    return std::filesystem::path(fileBuf.data());
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> showSaveFileDialog(HWND owner,
                                                        const wchar_t* title,
                                                        const wchar_t* filter,
                                                        const wchar_t* defExt,
                                                        const wchar_t* initialName) {
  std::vector<wchar_t> fileBuf(4096, L'\0');
  if (initialName) {
    wcsncpy_s(fileBuf.data(), fileBuf.size(), initialName, _TRUNCATE);
  }
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = fileBuf.data();
  ofn.nMaxFile = static_cast<DWORD>(fileBuf.size());
  ofn.lpstrTitle = title;
  ofn.lpstrDefExt = defExt;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetSaveFileNameW(&ofn)) {
    return std::filesystem::path(fileBuf.data());
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

std::vector<std::filesystem::path> showOpenFilesDialog(HWND owner,
                                                       const wchar_t* title,
                                                       const wchar_t* filter) {
  std::vector<wchar_t> buf(32768, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = static_cast<DWORD>(buf.size());
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT |
              OFN_EXPLORER | OFN_NOCHANGEDIR;
  std::vector<std::filesystem::path> out;
  if (!GetOpenFileNameW(&ofn)) {
    return out;
  }
  const wchar_t* p = buf.data();
  const std::wstring first(p);
  if (first.empty()) {
    return out;
  }
  p += first.size() + 1;
  if (*p == L'\0') {
    out.emplace_back(first);
    return out;
  }
  const std::filesystem::path dir(first);
  while (*p != L'\0') {
    const std::wstring fname(p);
    out.push_back(dir / fname);
    p += fname.size() + 1;
  }
  return out;
}

}  // namespace

EditorApp::EditorApp() = default;

EditorApp::~EditorApp() {
  shutdown();
}

bool EditorApp::initialize(bool headlessSmokeTest) {
  isSmokeTest_ = headlessSmokeTest;
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

  if (!headlessSmokeTest) {
    if (HWND hwnd = glfwGetWin32Window(window_)) {
      HINSTANCE hInst = GetModuleHandleW(nullptr);
      const int bigW = GetSystemMetrics(SM_CXICON);
      const int bigH = GetSystemMetrics(SM_CYICON);
      const int smallW = GetSystemMetrics(SM_CXSMICON);
      const int smallH = GetSystemMetrics(SM_CYSMICON);
      appIconBig_ = LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, bigW, bigH, LR_DEFAULTCOLOR);
      appIconSmall_ = LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, smallW, smallH, LR_DEFAULTCOLOR);
      if (appIconBig_) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIconBig_));
      }
      if (appIconSmall_) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIconSmall_));
      }
    }
  }

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

  {
    std::filesystem::path winFonts("C:/Windows/Fonts");
    wchar_t winDir[MAX_PATH]{};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) > 0) {
      winFonts = std::filesystem::path(winDir) / "Fonts";
    }
    const auto primaryFont = winFonts / "segoeui.ttf";
    if (iota::core::pathExistsNoThrow(primaryFont) &&
        io.Fonts->AddFontFromFileTTF(iota::core::pathToUtf8(primaryFont).c_str(), 16.0f) != nullptr) {
      ImFontConfig mergeCfg;
      mergeCfg.MergeMode = true;
      for (const char* cjkName : {"YuGothM.ttc", "msgothic.ttc", "msyh.ttc"}) {
        const auto cjkPath = winFonts / cjkName;
        if (iota::core::pathExistsNoThrow(cjkPath)) {
          io.Fonts->AddFontFromFileTTF(iota::core::pathToUtf8(cjkPath).c_str(), 16.0f, &mergeCfg);
          break;
        }
      }
      for (const char* extraName : {"malgun.ttf", "seguisym.ttf"}) {
        const auto extraPath = winFonts / extraName;
        if (iota::core::pathExistsNoThrow(extraPath)) {
          io.Fonts->AddFontFromFileTTF(iota::core::pathToUtf8(extraPath).c_str(), 16.0f, &mergeCfg);
        }
      }
    }
  }

  if (!ImGui_ImplGlfw_InitForOpenGL(window_, true) ||
      !ImGui_ImplOpenGL3_Init("#version 450")) {
    std::cerr << "Failed to initialize Dear ImGui GLFW/OpenGL3 backends\n";
    return false;
  }
  imguiInitialized_ = true;

  project_ = iota::model::createStarterProject();
  if (!project_.compositions.empty() && project_.compositions[0].elements.size() > 4) {
    setSingleSelection(0, 4);  // Circular Spectrum default selection
  } else {
    setSingleSelection(0, -1);
  }
  undoStack_.clear(project_);
  if (!isSmokeTest_) {
    projectLibrary_.load(iota::preset_io::ProjectLibrary::defaultRegistryPath());
    updateChecker_.startCheckAsync(IOTA_STUDIO_VERSION,
                                   iota::core::UpdateChecker::kDefaultRepoOwner,
                                   iota::core::UpdateChecker::kDefaultRepoName,
                                   false);
  }
  updateWindowTitle();

  actionLoadBuiltinTestTone(110.0f);
  lastWallTimeSec_ = glfwGetTime();
  return true;
}

void EditorApp::shutdown() {
#ifdef IOTA_ENABLE_MCP
  mcpBridgeHost_.stop();
#endif
  exportSession_.cancel();
  encoderProbe_.cancel();
  updateChecker_.cancel();
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
  if (appIconBig_) {
    DestroyIcon(static_cast<HICON>(appIconBig_));
    appIconBig_ = nullptr;
  }
  if (appIconSmall_) {
    DestroyIcon(static_cast<HICON>(appIconSmall_));
    appIconSmall_ = nullptr;
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
  if (iota::core::pathExistsNoThrow("iota_studio_layout.ini")) {
    return;
  }

  ImGui::DockBuilderRemoveNode(dockspaceId);
  ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

  ImGuiID dockMain = dockspaceId;
  ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.21f, nullptr, &dockMain);
  ImGuiID dockLeftBottom = ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Down, 0.48f, nullptr, &dockLeft);
  ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.28f, nullptr, &dockMain);
  ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.26f, nullptr, &dockMain);

  ImGui::DockBuilderDockWindow("Hierarchy", dockLeft);
  ImGui::DockBuilderDockWindow("Assets", dockLeftBottom);
  ImGui::DockBuilderDockWindow("Inspector", dockRight);
  ImGui::DockBuilderDockWindow("Documentation", dockRight);
  ImGui::DockBuilderDockWindow("Audio & Timeline", dockBottom);
  ImGui::DockBuilderDockWindow("Sound Keys", dockBottom);
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

    // Progress active offline video export in bounded time slices so GPU encoders run fast and UI stays responsive
    if (exportSession_.isActive()) {
      std::string err;
      const double sliceStart = glfwGetTime();
      int stepped = 0;
      while (exportSession_.isActive() && !exportSession_.isDone() && stepped < 12) {
        if (!exportSession_.stepFrame(device_, renderer_, project_, audio_, &err)) {
          exportSession_.cancel();
          statusBanner_ = "Export error: " + err;
          exportModalError_ = err;
          break;
        }
        ++stepped;
        if (glfwGetTime() - sliceStart >= 0.025) {
          break;
        }
      }
      if (exportSession_.isActive() && exportSession_.isDone()) {
        std::string finErr;
        if (exportSession_.finish(&finErr)) {
          const std::string doneMsg = "Export finished (" + exportSession_.activeEncoderDisplayName() +
                                      "): " + iota::core::pathToUtf8(exportCfg_.outputPath);
          statusBanner_ = doneMsg;
          exportModalSuccess_ = doneMsg;
          exportModalError_.clear();
        } else {
          statusBanner_ = "Export finish error: " + finErr;
          exportModalError_ = finErr;
        }
      }
    } else {
#ifdef IOTA_ENABLE_MCP
      pollMcpLiveBridge();
#endif
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
    drawAssetBrowserPanel();
    drawInspectorPanel();
    drawDocsPanel();
    drawAudioTimelinePanel();
    drawSoundKeysPanel();
    drawProjectManagerModal();
    drawExportModal();
    drawUpdateModal();

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
  drawAssetBrowserPanel();
  drawInspectorPanel();
  drawAudioTimelinePanel();
  drawSoundKeysPanel();
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

  if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S)) {
    saveProjectAsDialog("iota");
  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
    saveCurrentProject();
  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
    actionOpenPreset();
  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) {
    openProjectManagerModal_ = true;
  } else if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))) {
    actionRedo();
  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
    actionUndo();
  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
    actionCopySelection();
  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X)) {
    actionCutSelection();
  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
    actionPasteClipboard();
  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
    actionDuplicateSelection();
  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A) && hierarchyState_.windowFocused) {
    if (selectedCompIdx_ >= 0 && selectedCompIdx_ < static_cast<int>(project_.compositions.size())) {
      const int n = static_cast<int>(project_.compositions[static_cast<size_t>(selectedCompIdx_)].elements.size());
      std::vector<iota::model::ElementRef> allRefs;
      allRefs.reserve(static_cast<size_t>(n));
      for (int ei = 0; ei < n; ++ei) {
        allRefs.push_back({selectedCompIdx_, ei});
      }
      hierarchyState_.setSelection(allRefs, selectedCompIdx_);
      syncSelectionFromHierarchy();
    }
  } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
    actionNudgeSelection(-1, true);
  } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
    actionNudgeSelection(+1, true);
  } else if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
    actionNudgeSelection(-1, false);
  } else if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
    actionNudgeSelection(+1, false);
  } else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
    actionDeleteSelection();
  } else if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
    showDocsPanel_ = !showDocsPanel_;
    if (showDocsPanel_) {
      docsPanelState_.focusRequested = true;
    }
  } else if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
    hierarchyState_.beginRenameCurrent(project_);
  } else if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
    if (audio_.isPlaying()) audio_.pause();
    else audio_.play();
  }
}

void EditorApp::syncSelectionFromHierarchy() {
  hierarchyState_.clampToProject(project_);
  selectedCompIdx_ = hierarchyState_.primaryCompIdx;
  selectedElemIdx_ = hierarchyState_.primaryElemIdx;
}

void EditorApp::setSingleSelection(int compIdx, int elemIdx) {
  if (elemIdx >= 0) {
    hierarchyState_.selectSingleElement(compIdx, elemIdx);
  } else {
    hierarchyState_.selectComposition(compIdx);
  }
  syncSelectionFromHierarchy();
}

void EditorApp::clampSelectionToProject() {
  syncSelectionFromHierarchy();
}

bool EditorApp::hasHierarchyClipboard() const {
  if (imguiInitialized_) {
    if (const char* txt = ImGui::GetClipboardText(); txt != nullptr && *txt != '\0') {
      if (iota::preset_io::parseHierarchyClipboardFromJson(txt).has_value()) {
        return true;
      }
    }
  }
  return hierarchyClipboard_.kind != iota::model::HierarchyClipboard::Kind::Empty;
}

void EditorApp::actionUndo() {
  if (const auto* snap = undoStack_.undo()) {
    project_ = *snap;
    renderer_.invalidateLocalAssets();
    clampSelectionToProject();
    projectDirty_ = true;
    updateWindowTitle();
    statusBanner_ = "Undo";
  }
}

void EditorApp::actionRedo() {
  if (const auto* snap = undoStack_.redo()) {
    project_ = *snap;
    renderer_.invalidateLocalAssets();
    clampSelectionToProject();
    projectDirty_ = true;
    updateWindowTitle();
    statusBanner_ = "Redo";
  }
}

void EditorApp::actionCopySelection() {
  iota::model::HierarchyClipboard clip;
  std::string banner;
  if (!hierarchyState_.selectedElements.empty()) {
    clip = iota::model::copyElements(project_, hierarchyState_.selectedElements);
    if (clip.kind != iota::model::HierarchyClipboard::Kind::Empty) {
      banner = "Copied " + std::to_string(clip.elements.size()) + " layer(s)";
    }
  } else if (selectedCompIdx_ >= 0 && selectedCompIdx_ < static_cast<int>(project_.compositions.size())) {
    clip = iota::model::copyComposition(project_, selectedCompIdx_);
    if (clip.kind != iota::model::HierarchyClipboard::Kind::Empty) {
      banner = "Copied Composition " + std::to_string(selectedCompIdx_);
    }
  }
  if (clip.kind == iota::model::HierarchyClipboard::Kind::Empty) return;

  hierarchyClipboard_ = std::move(clip);
  if (imguiInitialized_) {
    const std::string jsonTxt = iota::preset_io::serializeHierarchyClipboardToJson(hierarchyClipboard_);
    if (!jsonTxt.empty()) ImGui::SetClipboardText(jsonTxt.c_str());
  }
  statusBanner_ = std::move(banner);
}

void EditorApp::actionCutSelection() {
  if (hierarchyState_.selectedElements.empty()) return;
  std::optional<iota::model::ElementRef> fallbackSel;
  auto clip = iota::model::cutElements(project_, hierarchyState_.selectedElements, &fallbackSel);
  if (clip.kind != iota::model::HierarchyClipboard::Kind::Empty) {
    const size_t nCut = clip.elements.size();
    hierarchyClipboard_ = std::move(clip);
    if (imguiInitialized_) {
      const std::string jsonTxt = iota::preset_io::serializeHierarchyClipboardToJson(hierarchyClipboard_);
      if (!jsonTxt.empty()) ImGui::SetClipboardText(jsonTxt.c_str());
    }
    if (fallbackSel.has_value()) {
      setSingleSelection(fallbackSel->compIdx, fallbackSel->elemIdx);
    } else {
      clampSelectionToProject();
    }
    pushUndoState(nCut == 1 ? "Cut Layer" : "Cut Layers");
    statusBanner_ = "Cut " + std::to_string(nCut) + " layer(s)";
  }
}

void EditorApp::actionPasteClipboard() {
  iota::model::HierarchyClipboard effective = hierarchyClipboard_;
  if (imguiInitialized_) {
    if (const char* txt = ImGui::GetClipboardText(); txt != nullptr && *txt != '\0') {
      if (auto parsed = iota::preset_io::parseHierarchyClipboardFromJson(txt)) {
        effective = std::move(*parsed);
      }
    }
  }
  if (effective.kind == iota::model::HierarchyClipboard::Kind::Empty || project_.compositions.empty()) {
    return;
  }

  const int dstComp = std::clamp(selectedCompIdx_, 0, static_cast<int>(project_.compositions.size()) - 1);
  int insertAt = -1;
  if (effective.kind == iota::model::HierarchyClipboard::Kind::Elements && selectedElemIdx_ >= 0) {
    insertAt = selectedElemIdx_ + 1;
  }
  const auto pasted = iota::model::pasteClipboard(project_, effective, dstComp, insertAt, false);
  if (!pasted.empty()) {
    if (effective.kind == iota::model::HierarchyClipboard::Kind::Composition) {
      setSingleSelection(pasted.front().compIdx, -1);
      pushUndoState("Paste Composition");
      statusBanner_ = "Pasted Composition " + std::to_string(pasted.front().compIdx);
    } else {
      hierarchyState_.setSelection(pasted, dstComp);
      syncSelectionFromHierarchy();
      pushUndoState(pasted.size() == 1 ? "Paste Layer" : "Paste Layers");
      statusBanner_ = "Pasted " + std::to_string(pasted.size()) + " layer(s)";
    }
  }
}

void EditorApp::actionDuplicateSelection() {
  if (!hierarchyState_.selectedElements.empty()) {
    const auto dups = iota::model::duplicateElements(project_, hierarchyState_.selectedElements);
    if (!dups.empty()) {
      hierarchyState_.setSelection(dups, selectedCompIdx_);
      syncSelectionFromHierarchy();
      pushUndoState(dups.size() == 1 ? "Duplicate Layer" : "Duplicate Layers");
      statusBanner_ = "Duplicated " + std::to_string(dups.size()) + " layer(s)";
    }
  } else if (selectedCompIdx_ >= 0 && selectedCompIdx_ < static_cast<int>(project_.compositions.size())) {
    const int newCompIdx = iota::model::duplicateComposition(project_, selectedCompIdx_);
    if (newCompIdx >= 0) {
      setSingleSelection(newCompIdx, -1);
      pushUndoState("Duplicate Composition");
      statusBanner_ = "Duplicated Composition [" + std::to_string(newCompIdx) + "]";
    }
  }
}

void EditorApp::actionDeleteSelection() {
  if (!hierarchyState_.selectedElements.empty()) {
    const size_t nDel = hierarchyState_.selectedElements.size();
    if (const auto fallback = iota::model::deleteElements(project_, hierarchyState_.selectedElements)) {
      setSingleSelection(fallback->compIdx, fallback->elemIdx);
      pushUndoState(nDel == 1 ? "Delete Layer" : "Delete Layers");
      statusBanner_ = "Deleted " + std::to_string(nDel) + " layer(s)";
    }
  } else if (selectedCompIdx_ > 0 && selectedCompIdx_ < static_cast<int>(project_.compositions.size())) {
    const int deletedIdx = selectedCompIdx_;
    if (iota::model::deleteComposition(project_, deletedIdx)) {
      setSingleSelection(std::max(0, deletedIdx - 1), -1);
      pushUndoState("Delete Composition");
      statusBanner_ = "Deleted Composition " + std::to_string(deletedIdx);
    }
  }
}

void EditorApp::actionPrecomposeSelection() {
  int srcComp = -1;
  if (!hierarchyState_.allSelectedInSameComp(&srcComp) || hierarchyState_.selectedElements.empty()) {
    return;
  }
  std::vector<int> indices;
  indices.reserve(hierarchyState_.selectedElements.size());
  for (const auto& r : hierarchyState_.selectedElements) {
    indices.push_back(r.elemIdx);
  }
  const auto res = iota::model::precomposeElements(project_, srcComp, indices);
  if (res.newCompIdx >= 0 && res.hostLayerRef.elemIdx >= 0) {
    setSingleSelection(res.hostLayerRef.compIdx, res.hostLayerRef.elemIdx);
    pushUndoState("Pre-Compose Layers");
    statusBanner_ = "Pre-composed into Composition " + std::to_string(res.newCompIdx);
  }
}

void EditorApp::actionNudgeSelection(int delta, bool toBoundary) {
  if (hierarchyState_.selectedElements.empty()) return;
  const auto moved = toBoundary
                         ? iota::model::moveElementsToBoundary(project_, hierarchyState_.selectedElements, delta < 0)
                         : iota::model::nudgeElements(project_, hierarchyState_.selectedElements, delta);
  if (!moved.empty() && moved != hierarchyState_.selectedElements) {
    hierarchyState_.setSelection(moved, selectedCompIdx_);
    syncSelectionFromHierarchy();
    pushUndoState(toBoundary ? (delta < 0 ? "Move Layer to Top" : "Move Layer to Bottom")
                             : (delta < 0 ? "Move Layer Up" : "Move Layer Down"));
  }
}

void EditorApp::pushUndoState(std::string label) {
  undoStack_.pushSnapshot(std::move(label), project_);
  projectDirty_ = true;
  updateWindowTitle();
}

void EditorApp::drawMenuBar() {
  if (!ImGui::BeginMainMenuBar()) return;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Project Manager...\tCtrl+N")) {
      openProjectManagerModal_ = true;
    }
    if (ImGui::MenuItem("New Starter Project")) {
      project_ = iota::model::createStarterProject();
      currentProjectPath_.clear();
      projectDirty_ = false;
      applyProjectMetadata(project_.metadata);
      setSingleSelection(0, 4);
      renderer_.invalidateLocalAssets();
      renderer_.resetSimulation();
      undoStack_.clear(project_);
      updateWindowTitle();
      statusBanner_ = "Created starter project";
    }
    if (ImGui::MenuItem("Open Project / Preset...\tCtrl+O")) {
      actionOpenPreset();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save Project\tCtrl+S")) {
      saveCurrentProject();
    }
    if (ImGui::MenuItem("Save Project As (.iota)...\tCtrl+Shift+S")) {
      saveProjectAsDialog("iota");
    }
    if (ImGui::MenuItem("Export Preset (.viz)...")) {
      saveProjectAsDialog("viz");
    }
    if (ImGui::MenuItem("Export Scene JSON (.json)...")) {
      saveProjectAsDialog("json");
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
      actionUndo();
    }
    if (ImGui::MenuItem(redoLabel.c_str(), nullptr, false, undoStack_.canRedo())) {
      actionRedo();
    }
    ImGui::Separator();
    const bool hasElemSel = !hierarchyState_.selectedElements.empty();
    const bool hasCompSel =
        (selectedCompIdx_ >= 0 && selectedCompIdx_ < static_cast<int>(project_.compositions.size()));
    const bool canDeleteSel = hasElemSel || selectedCompIdx_ > 0;
    const bool canPrecomp = hierarchyState_.allSelectedInSameComp();
    if (ImGui::MenuItem("Cut\tCtrl+X", nullptr, false, hasElemSel)) {
      actionCutSelection();
    }
    if (ImGui::MenuItem("Copy\tCtrl+C", nullptr, false, hasElemSel || hasCompSel)) {
      actionCopySelection();
    }
    if (ImGui::MenuItem("Paste\tCtrl+V", nullptr, false, hasHierarchyClipboard())) {
      actionPasteClipboard();
    }
    if (ImGui::MenuItem("Duplicate\tCtrl+D", nullptr, false, hasElemSel || hasCompSel)) {
      actionDuplicateSelection();
    }
    if (ImGui::MenuItem("Delete\tDel", nullptr, false, canDeleteSel)) {
      actionDeleteSelection();
    }
    if (ImGui::MenuItem("Pre-Compose Selected", nullptr, false, canPrecomp)) {
      actionPrecomposeSelection();
    }
    ImGui::Separator();
    ImGui::MenuItem("Show Advanced Hidden Properties", nullptr, &showHiddenProps_);
    ImGui::MenuItem("Show Raw Property Keys", nullptr, &showRawPropertyKeys_);
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
    if (!project_.compositions.empty()) {
      auto ensureComp0HdrProps = [&]() {
        const auto defComp = iota::model::createDefaultComposition(0);
        auto& comp0 = project_.compositions[0];
        for (const auto& defItem : defComp.props.items()) {
          if (defItem.entry.groupTag == "3_hdrPost" && !comp0.props.contains(defItem.key)) {
            comp0.props.items().push_back(defItem);
          }
        }
      };
      auto& comp0Props = project_.compositions[0].props;
      ImGui::Separator();
      bool hdrOn = comp0Props.getBool("hdrPipeline", true);
      if (ImGui::MenuItem("HDR 16-bit Float Pipeline", nullptr, &hdrOn)) {
        ensureComp0HdrProps();
        comp0Props.setBool("hdrPipeline", hdrOn, "3_hdrPost", "b");
        pushUndoState(hdrOn ? "Enable HDR Pipeline" : "Disable HDR Pipeline");
      }
      bool bloomOn = comp0Props.getBool("bloomEnabled", false);
      if (ImGui::MenuItem("HDR Bloom Glow", nullptr, &bloomOn)) {
        ensureComp0HdrProps();
        comp0Props.setBool("bloomEnabled", bloomOn, "3_hdrPost", "b");
        pushUndoState(bloomOn ? "Enable HDR Bloom" : "Disable HDR Bloom");
      }
      if (ImGui::BeginMenu("Tonemapping")) {
        const std::string curTm = comp0Props.getString("tonemapMode", "None");
        for (const char* tm : {"None", "ACES", "AgX", "PBRNeutral", "Reinhard"}) {
          const bool selected = (curTm == tm);
          if (ImGui::MenuItem(tm, nullptr, selected)) {
            ensureComp0HdrProps();
            comp0Props.setString("tonemapMode", tm, "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
            pushUndoState(std::string("Set Tonemap: ") + tm);
          }
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Edit HDR & Bloom Settings...")) {
        ensureComp0HdrProps();
        setSingleSelection(0, -1);
      }
    }
    ImGui::Separator();
    ImGui::MenuItem("Asset Browser", nullptr, &showAssetBrowserPanel_);
    ImGui::MenuItem("Sound Keys Panel", nullptr, &showSoundKeysPanel_);
    ImGui::MenuItem("Documentation Panel\tF1", nullptr, &showDocsPanel_);
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Help")) {
    if (ImGui::MenuItem("Documentation...\tF1")) {
      showDocsPanel_ = true;
      docsPanelState_.focusRequested = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Check for Updates...")) {
      updateChecker_.startCheckAsync(IOTA_STUDIO_VERSION,
                                     iota::core::UpdateChecker::kDefaultRepoOwner,
                                     iota::core::UpdateChecker::kDefaultRepoName,
                                     true);
      openUpdateModal_ = true;
    }
    if (ImGui::MenuItem("Open Releases Page")) {
      openExternalUrl(iota::core::UpdateChecker::kReleasesPageUrl);
    }
    ImGui::Separator();
    const std::string verLabel = std::string("Iota Studio v") + IOTA_STUDIO_VERSION;
    ImGui::MenuItem(verLabel.c_str(), nullptr, false, false);
    ImGui::EndMenu();
  }

#ifdef IOTA_ENABLE_MCP
  if (ImGui::BeginMenu("MCP")) {
    bool liveOn = mcpBridgeHost_.isListening();
    if (ImGui::MenuItem("Enable Live Studio Bridge", nullptr, &liveOn)) {
      if (liveOn) {
        enableMcpLiveBridge();
      } else {
        disableMcpLiveBridge("MCP Live Bridge disabled");
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
      ImGui::SetTooltip(
          "Allows local AI agents (via iota_mcp --attach or studio_connect) to inspect and edit this session.\n"
          "Automatically saves a <name>.mcp.iota working copy first so original files are never modified.");
    }
    ImGui::Separator();
    const char* bridgeState =
        !mcpBridgeHost_.isListening()
            ? "Status: Disabled"
            : (mcpBridgeHost_.hasConnectedClient() ? "Status: Listening (Agent Connected)"
                                                   : "Status: Listening (Waiting for Agent)");
    ImGui::MenuItem(bridgeState, nullptr, false, false);
    const std::string reqStr = "Requests Handled: " + std::to_string(mcpBridgeHost_.requestsHandled());
    ImGui::MenuItem(reqStr.c_str(), nullptr, false, false);
    if (!currentProjectPath_.empty()) {
      const std::string activeStr = "Active File: " + iota::core::pathFilenameUtf8(currentProjectPath_);
      ImGui::MenuItem(activeStr.c_str(), nullptr, false, false);
    }
    if (!mcpOriginalProjectPath_.empty()) {
      const std::string origStr = "Protected Original: " + iota::core::pathFilenameUtf8(mcpOriginalProjectPath_);
      ImGui::MenuItem(origStr.c_str(), nullptr, false, false);
    }
    const std::string curPipe = mcpBridgeHost_.isListening()
                                    ? mcpBridgeHost_.pipeName()
                                    : iota::mcp::defaultLiveBridgePipeName();
    if (ImGui::MenuItem("Copy Bridge Pipe Name")) {
      ImGui::SetClipboardText(curPipe.c_str());
      statusBanner_ = "Copied MCP pipe name";
    }
    ImGui::EndMenu();
  }

  if (mcpBridgeHost_.isListening()) {
    ImGui::Separator();
    const bool clientOn = mcpBridgeHost_.hasConnectedClient();
    ImGui::PushStyleColor(ImGuiCol_Button,
                          clientOn ? ImVec4(0.14f, 0.50f, 0.56f, 1.0f) : ImVec4(0.18f, 0.34f, 0.44f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          clientOn ? ImVec4(0.18f, 0.62f, 0.68f, 1.0f) : ImVec4(0.24f, 0.45f, 0.58f, 1.0f));
    const char* badge = clientOn ? "MCP Live (Connected)" : "MCP Live";
    if (ImGui::SmallButton(badge)) {
      disableMcpLiveBridge("MCP Live Bridge disabled");
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
      ImGui::SetTooltip("Pipe: %s\nClick to turn off MCP Live Studio Bridge.",
                        mcpBridgeHost_.pipeName().c_str());
    }
  }
#endif

  const auto updSnap = updateChecker_.snapshot();
  if (updSnap.status == iota::core::UpdateCheckStatus::UpdateAvailable) {
    if (!updateBannerNotified_) {
      updateBannerNotified_ = true;
      statusBanner_ = "Update available: " + updSnap.latestRelease.tagName;
    }
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.52f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.65f, 0.36f, 1.0f));
    const std::string badgeLabel = "Update Available (" + updSnap.latestRelease.tagName + ")";
    if (ImGui::SmallButton(badgeLabel.c_str())) {
      openUpdateModal_ = true;
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
      ImGui::SetTooltip("Click to view release notes and open the GitHub release link.");
    }
  }

  ImGui::Separator();
  const char* pipeLabel = frameStats_.hdrPipelineActive
                              ? (frameStats_.bloomActive ? "HDR16F+Bloom" : "HDR16F")
                              : (frameStats_.bloomActive ? "SDR8+Bloom" : "SDR8");
  ImGui::TextDisabled("%s | %dx%d | %s | %.1f FPS | Draws: %d | Beat: %.2f",
                      statusBanner_.c_str(),
                      previewWidth_,
                      previewHeight_,
                      pipeLabel,
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
  HierarchyActions actions;
  renderHierarchyPanel(project_, hierarchyState_, hasHierarchyClipboard(), actions);
  syncSelectionFromHierarchy();
  if (!actions.statusMessage.empty()) {
    statusBanner_ = actions.statusMessage;
  }
  if (actions.undoSnapshotNeeded) {
    pushUndoState(actions.editLabel.empty() ? "Modify Hierarchy" : actions.editLabel);
  }
  if (actions.requestCopy) actionCopySelection();
  if (actions.requestCut) actionCutSelection();
  if (actions.requestPaste) actionPasteClipboard();
  if (actions.requestDuplicate) actionDuplicateSelection();
  if (actions.requestDelete) actionDeleteSelection();
  if (actions.requestPrecompose) actionPrecomposeSelection();
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
  ImGui::SameLine();
  if (ImGui::SmallButton("? Docs")) {
    const iota::model::ElementType targetType =
        (selectedElemIdx_ >= 0 && selectedElemIdx_ < static_cast<int>(comp.elements.size()))
            ? comp.elements[static_cast<size_t>(selectedElemIdx_)].type
            : iota::model::ElementType::Composition;
    openDocsForElement(targetType);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Open Built-in Documentation for the selected layer or composition (F1)");
  }

  ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x - 28.0f));
  ImGui::InputTextWithHint("##insp_filter", "Filter options...", inspectorFilterBuf_, sizeof(inspectorFilterBuf_));
  ImGui::SameLine();
  if (ImGui::SmallButton("X##clr_insp")) {
    inspectorFilterBuf_[0] = '\0';
  }
  ImGui::Separator();

  InspectorActions actions;
  actions.showHiddenProperties = showHiddenProps_;
  actions.showRawPropertyKeys = showRawPropertyKeys_;
  actions.preciseNumericInputs = preciseNumericInputs_;
  actions.propertyFilter = inspectorFilterBuf_;

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
          !el.props.contains("useAttackRelease") || !el.props.contains("temporalSmooth") ||
          !el.props.contains("attackMs") || !el.props.contains("releaseMs");
      const bool missingFreqSmooth = !el.props.contains("freqSmoothMode");
      if (missingTemporal) {
        if (ImGui::SmallButton("Enable Attack/Release Smoothing")) {
          if (!el.props.contains("useAttackRelease")) el.props.setBool("useAttackRelease", true, "2_temporal", "b");
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
      if (!el.props.contains("curveInterpolation") || !el.props.contains("curveSubdivisions") ||
          !el.props.contains("hdrIntensity") || !el.props.contains("measureHdrIntensity")) {
        if (ImGui::SmallButton("Enable Curve & HDR Controls")) {
          if (!el.props.contains("curveInterpolation")) {
            el.props.setString("curveInterpolation", "None", "sel None Linear CatmullRom CubicBSpline", "1_bars");
          }
          if (!el.props.contains("curveSubdivisions")) {
            el.props.setInt("curveSubdivisions", 4, 1, 8, "1_bars");
          }
          if (!el.props.contains("hdrIntensity")) {
            el.props.setFloat("hdrIntensity", 1.0f, 0.0f, 16.0f, "1_bars");
          }
          if (!el.props.contains("measureHdrIntensity")) {
            el.props.setMVar("measureHdrIntensity",
                             iota::model::MVariableFloat{{{"Nothing", 1.0f, 0.0f}}},
                             "mvarf 0.0 8.0",
                             "1_bars");
          }
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Enable Bars Curve & HDR Controls";
        }
      }
    } else if (el.type == iota::model::ElementType::SoundKeys) {
      ImGui::Separator();
      renderSoundKeysCanvasAndMeters(el, frameStats_, activeSoundKeysRange_, actions, 115.0f);
    } else if (el.type == iota::model::ElementType::Particles) {
      ImGui::TextDisabled("Active: %d particles (%d aux)",
                          frameStats_.activeParticles,
                          frameStats_.activeAuxParticles);
      if (ImGui::SmallButton("Reset Sim")) {
        renderer_.resetSimulation();
      }
      auto upgradeParticularProps = [&]() {
        const auto defEl = iota::model::createDefaultElement(iota::model::ElementType::Particles);
        for (const auto& defItem : defEl.props.items()) {
          if (!el.props.contains(defItem.key)) {
            el.props.items().push_back(defItem);
          }
        }
        el.props.getOrAdd("CountLimit").typeTag = "i 1 50000";
        el.props.getOrAdd("trailLength").typeTag = "f 0.0 5.0";
        el.props.getOrAdd("SpawnArea").typeTag =
            "_child HorizontalLine Rectangle Point Box3D Sphere Ring Grid LayerMask";
        el.props.getOrAdd("ForceField").typeTag =
            "_child None Vortex Attract Repel TurbulentVortex";
      };
      if (!el.props.contains("turbulenceAffectPos")) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Enable Particular 3D Controls")) {
          upgradeParticularProps();
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Enable Particular 3D Controls";
        }
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Presets:");
      const float maxWrapX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
      auto applyPreset = [&](const char* label, const auto& applyFn) {
        const float btnW = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + btnW <= maxWrapX) {
          ImGui::SameLine();
        }
        if (ImGui::SmallButton(label)) {
          upgradeParticularProps();
          applyFn();
          renderer_.resetSimulation();
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = std::string("Apply Particle Preset: ") + label;
        }
      };
      applyPreset("3D Bokeh", [&]() {
        el.props.setString("particleShape", "BokehRing",
                           "sel Sprite Sphere GlowSphere Star BokehRing Streaklet", "appearance");
        el.props.getOrAdd("SpawnArea").value = std::string("Box3D");
        el.props.setInt("CountLimit", 800, 1, 50000, "1_overall");
        el.props.setFloat("particlesPerSec", 90.0f, 0.0f, 5000.0f, "1_overall");
        el.props.setMVar("measureEmitRate", iota::model::MVariableFloat::constant(1.0f), "mvarf 0.0 5.0", "1_overall");
        el.props.setFloat("emitterSizeZ", 350.0f, 0.0f, 1000.0f, "1_overall");
        el.props.setFloat("preRunSec", 1.5f, 0.0f, 10.0f, "1_overall");
        el.props.setFloat("Speed", 45.0f, -500.0f, 500.0f, "behaviour");
        el.props.setFloat("airResistance", 0.6f, 0.0f, 10.0f, "behaviour");
        el.props.setFloat("particleScale", 6.0f, 0.25f, 16.0f, "appearance");
        el.props.setFloat("colorRandomHue", 0.15f, 0.0f, 1.0f, "appearance");
        el.props.setFloat("turbulenceAffectPos", 40.0f, 0.0f, 500.0f, "4_turbulence");
        el.props.setString("auxEmitMode", "Off", "sel Off Continuously AtBounce AtDeath", "5_auxSystem");
        el.props.setBool("camera3DEnabled", true, "6_cameraDof");
        el.props.setFloat("cameraDistance", 420.0f, 80.0f, 2000.0f, "6_cameraDof");
        el.props.setBool("dofEnabled", true, "6_cameraDof");
        el.props.setFloat("dofFocusDist", 420.0f, 40.0f, 2000.0f, "6_cameraDof");
        el.props.setFloat("dofAperture", 0.75f, 0.0f, 2.0f, "6_cameraDof");
        el.props.setFloat("hdrIntensity", 2.4f, 0.0f, 16.0f, "appearance");
      });
      applyPreset("Audio Sparks", [&]() {
        el.props.setString("particleShape", "Streaklet",
                           "sel Sprite Sphere GlowSphere Star BokehRing Streaklet", "appearance");
        el.props.getOrAdd("SpawnArea").value = std::string("Ring");
        el.props.setInt("CountLimit", 1500, 1, 50000, "1_overall");
        el.props.setFloat("particlesPerSec", 180.0f, 0.0f, 5000.0f, "1_overall");
        el.props.setMVar("measureEmitRate",
                         iota::model::MVariableFloat{{{"Constant", 0.25f, 0.0f}, {"Beat", 3.0f, 0.0f}}},
                         "mvarf 0.0 5.0",
                         "1_overall");
        el.props.setFloat("trailLength", 1.6f, 0.0f, 5.0f, "appearance");
        el.props.setFloat("Speed", 240.0f, -500.0f, 500.0f, "behaviour");
        el.props.setFloat("speedRandom", 120.0f, 0.0f, 500.0f, "behaviour");
        el.props.setFloat("airResistance", 1.8f, 0.0f, 10.0f, "behaviour");
        el.props.setVec2("gravity", {0.0f, 120.0f}, -500.0f, 500.0f, "behaviour");
        el.props.setBool("bounceEnabled", true, "behaviour");
        el.props.setFloat("bounceFloorY", 0.88f, 0.0f, 1.2f, "behaviour");
        el.props.setFloat("turbulenceAffectPos", 75.0f, 0.0f, 500.0f, "4_turbulence");
        el.props.setFloat("particleScale", 3.2f, 0.25f, 16.0f, "appearance");
        el.props.setColorHsla("ColorFrom", {0.08f, 1.0f, 0.72f, 1.0f}, "appearance");
        el.props.setColorHsla("ColorTo", {0.98f, 0.95f, 0.55f, 0.0f}, "appearance");
        el.props.setBool("useMidColor", false, "appearance");
        el.props.setFloat("hdrIntensity", 2.8f, 0.0f, 16.0f, "appearance");
      });
      applyPreset("Curl Smoke", [&]() {
        el.props.setString("particleShape", "Sphere",
                           "sel Sprite Sphere GlowSphere Star BokehRing Streaklet", "appearance");
        el.props.setFloat("feather", 0.92f, 0.0f, 1.0f, "appearance");
        el.props.getOrAdd("SpawnArea").value = std::string("Sphere");
        el.props.setInt("CountLimit", 1200, 1, 50000, "1_overall");
        el.props.setFloat("particlesPerSec", 120.0f, 0.0f, 5000.0f, "1_overall");
        el.props.setMVar("measureEmitRate", iota::model::MVariableFloat::constant(1.0f), "mvarf 0.0 5.0", "1_overall");
        el.props.setFloat("emitterSizeZ", 180.0f, 0.0f, 1000.0f, "1_overall");
        el.props.setFloat("preRunSec", 1.5f, 0.0f, 10.0f, "1_overall");
        el.props.setFloat("lifetime", 3.5f, 0.05f, 20.0f, "behaviour");
        el.props.setFloat("Speed", 35.0f, -500.0f, 500.0f, "behaviour");
        el.props.setFloat("airResistance", 1.1f, 0.0f, 10.0f, "behaviour");
        el.props.setFloat("turbulenceAffectPos", 120.0f, 0.0f, 500.0f, "4_turbulence");
        el.props.setFloat("turbulenceAffectSize", 0.35f, 0.0f, 2.0f, "4_turbulence");
        el.props.setInt("turbulenceOctaves", 3, 1, 3, "4_turbulence");
        el.props.setFloat("particleScale", 7.5f, 0.25f, 16.0f, "appearance");
        el.props.setBool("useMidColor", true, "appearance");
        el.props.setColorHsla("ColorFrom", {0.55f, 0.85f, 0.65f, 0.35f}, "appearance");
        el.props.setColorHsla("ColorMid", {0.72f, 0.85f, 0.55f, 0.28f}, "appearance");
        el.props.setColorHsla("ColorTo", {0.88f, 0.75f, 0.45f, 0.0f}, "appearance");
      });
      applyPreset("Aux Trails", [&]() {
        el.props.setString("particleShape", "GlowSphere",
                           "sel Sprite Sphere GlowSphere Star BokehRing Streaklet", "appearance");
        el.props.setFloat("feather", 0.7f, 0.0f, 1.0f, "appearance");
        el.props.getOrAdd("SpawnArea").value = std::string("Point");
        el.props.setInt("CountLimit", 2500, 1, 50000, "1_overall");
        el.props.setFloat("particlesPerSec", 24.0f, 0.0f, 5000.0f, "1_overall");
        el.props.setMVar("measureEmitRate", iota::model::MVariableFloat::constant(1.0f), "mvarf 0.0 5.0", "1_overall");
        el.props.setFloat("Speed", 220.0f, -500.0f, 500.0f, "behaviour");
        el.props.setFloat("directionSpread", 140.0f, 0.0f, 180.0f, "behaviour");
        el.props.setVec2("gravity", {0.0f, 110.0f}, -500.0f, 500.0f, "behaviour");
        el.props.setFloat("turbulenceAffectPos", 55.0f, 0.0f, 500.0f, "4_turbulence");
        el.props.setFloat("trailLength", 0.0f, 0.0f, 5.0f, "appearance");
        el.props.setString("auxEmitMode", "Continuously", "sel Off Continuously AtBounce AtDeath", "5_auxSystem");
        el.props.setFloat("auxParticlesPerSec", 28.0f, 0.0f, 60.0f, "5_auxSystem");
        el.props.setFloat("auxLifetime", 0.75f, 0.05f, 5.0f, "5_auxSystem");
        el.props.setFloat("auxSizeScale", 0.55f, 0.1f, 3.0f, "5_auxSystem");
        el.props.setFloat("hdrIntensity", 2.2f, 0.0f, 16.0f, "appearance");
      });
      applyPreset("Layer Emit", [&]() {
        el.props.setString("particleShape", "GlowSphere",
                           "sel Sprite Sphere GlowSphere Star BokehRing Streaklet", "appearance");
        el.props.getOrAdd("SpawnArea").value = std::string("LayerMask");
        el.props.setString("emitterLayer", "", "img composition:0", "1_overall");
        el.props.setInt("CountLimit", 2000, 1, 50000, "1_overall");
        el.props.setFloat("particlesPerSec", 320.0f, 0.0f, 5000.0f, "1_overall");
        el.props.setMVar("measureEmitRate", iota::model::MVariableFloat::constant(1.0f), "mvarf 0.0 5.0", "1_overall");
        el.props.setFloat("Speed", 55.0f, -500.0f, 500.0f, "behaviour");
        el.props.setFloat("turbulenceAffectPos", 65.0f, 0.0f, 500.0f, "4_turbulence");
        el.props.setFloat("airResistance", 1.4f, 0.0f, 10.0f, "behaviour");
        el.props.setFloat("lifetime", 1.3f, 0.05f, 20.0f, "behaviour");
        el.props.setFloat("particleScale", 2.8f, 0.25f, 16.0f, "appearance");
        el.props.setFloat("hdrIntensity", 2.0f, 0.0f, 16.0f, "appearance");
      });
    } else if (el.type == iota::model::ElementType::BlurEffect) {
      if (!el.props.contains("measureBlurRadius") || !el.props.contains("blurType")) {
        if (ImGui::SmallButton("Enable Advanced Blur Controls")) {
          if (!el.props.contains("measureBlurRadius")) {
            el.props.setMVar("measureBlurRadius",
                             iota::model::MVariableFloat{{{"Nothing", 0.5f, 0.5f}}},
                             "mvarf 0.0 3.0",
                             "2_blur");
          }
          if (!el.props.contains("blurType")) {
            el.props.setString("blurType", "DualKawase",
                               "sel DualKawase Gaussian Radial Directional", "2_blur");
          }
          if (!el.props.contains("blurAngle")) {
            el.props.setFloat("blurAngle", 0.0f, 0.0f, 360.0f, "2_blur");
          }
          if (!el.props.contains("blurCenter")) {
            el.props.setVec2("blurCenter", {0.5f, 0.5f}, 0.0f, 1.0f, "2_blur");
          }
          el.props.getOrAdd("sourceCompositionIndex").typeTag = "i 0 5";
          el.props.getOrAdd("blurRadius").typeTag = "f 0.0 6.0";
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Enable Advanced Blur Controls";
        }
      }
    } else if (el.type == iota::model::ElementType::MotionBlurEffect) {
      if (!el.props.contains("shutterAngle")) {
        if (ImGui::SmallButton("Enable Advanced Motion Blur Controls")) {
          el.props.getOrAdd("motionSource").typeTag = "_child EffectTransform Manual Temporal Hybrid";
          el.props.setFloat("shutterAngle", 180.0f, 0.0f, 720.0f, "2_motionBlur");
          el.props.setFloat("shutterPhase", 0.0f, -0.5f, 0.5f, "2_motionBlur");
          el.props.setInt("blurSamples", 24, 8, 64, "2_motionBlur");
          el.props.setVec2("blurCenter", {0.5f, 0.5f}, 0.0f, 1.0f, "2_motionBlur");
          el.props.setFloat("manualAngle", 0.0f, 0.0f, 360.0f, "2_motionBlur");
          el.props.setFloat("manualDirectional", 0.0f, 0.0f, 0.5f, "2_motionBlur");
          el.props.setFloat("manualZoom", 0.08f, -0.5f, 0.5f, "2_motionBlur");
          el.props.setFloat("manualSpin", 0.0f, -45.0f, 45.0f, "2_motionBlur");
          el.props.setFloat("trailDecayMs", 120.0f, 10.0f, 1000.0f, "2_motionBlur");
          el.props.setString("trailBlend", "Mix", "sel Mix Lighten Screen", "2_motionBlur");
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Enable Advanced Motion Blur Controls";
        }
      }
    } else if (el.type == iota::model::ElementType::Text ||
               el.type == iota::model::ElementType::StatText ||
               el.type == iota::model::ElementType::AppLogo) {
      ImGui::TextDisabled("%s | Cached glyphs: %d",
                          frameStats_.fontUsingDirectWrite ? "DirectWrite Unicode" : "Fallback Atlas",
                          frameStats_.cachedGlyphs);
      auto ensureTextProps = [&]() {
        const auto defText = iota::model::createDefaultElement(iota::model::ElementType::Text);
        for (const auto& defItem : defText.props.items()) {
          if (!el.props.contains(defItem.key)) {
            el.props.items().push_back(defItem);
          }
        }
      };
      if (!el.props.contains("outlineWidth")) {
        if (ImGui::SmallButton("Enable Typography Controls")) {
          ensureTextProps();
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Enable Typography Controls";
        }
      }
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Presets:");
      const float maxWrapX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
      auto applyTextPreset = [&](const char* label, const auto& fn) {
        const float btnW = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + btnW <= maxWrapX) {
          ImGui::SameLine();
        }
        if (ImGui::SmallButton(label)) {
          ensureTextProps();
          fn();
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = std::string("Apply Text Preset: ") + label;
        }
      };
      applyTextPreset("Clean Title", [&]() {
        el.props.setString("fontWeight", "Regular", "sel Default Light Regular SemiBold Bold Black", "2_typography");
        el.props.setBool("fontItalic", false, "2_typography", "b");
        el.props.setFloat("letterSpacing", 1.5f, -10.0f, 40.0f, "2_typography");
        el.props.setFloat("outlineWidth", 0.0f, 0.0f, 12.0f, "2_typography");
        el.props.setVec2("shadowOffset", {0.0f, 3.0f}, -40.0f, 40.0f, "2_typography");
        el.props.setColorArgb("shadowColor", {0, 0, 0, 140}, "crgba", "2_typography");
        el.props.setFloat("hdrIntensity", 1.0f, 0.0f, 16.0f, "appearance");
      });
      applyTextPreset("Neon Glow", [&]() {
        el.props.setString("fontWeight", "Bold", "sel Default Light Regular SemiBold Bold Black", "2_typography");
        el.props.setFloat("letterSpacing", 2.0f, -10.0f, 40.0f, "2_typography");
        el.props.setFloat("outlineWidth", 1.5f, 0.0f, 12.0f, "2_typography");
        el.props.setColorArgb("outlineColor", {0, 220, 255, 220}, "crgba", "2_typography");
        el.props.setVec2("shadowOffset", {0.0f, 0.0f}, -40.0f, 40.0f, "2_typography");
        el.props.setColorArgb("color", {220, 250, 255, 255}, "crgba", "appearance");
        el.props.setFloat("hdrIntensity", 2.6f, 0.0f, 16.0f, "appearance");
      });
      applyTextPreset("Bold Outline", [&]() {
        el.props.setString("fontWeight", "Black", "sel Default Light Regular SemiBold Bold Black", "2_typography");
        el.props.setFloat("letterSpacing", 0.5f, -10.0f, 40.0f, "2_typography");
        el.props.setFloat("outlineWidth", 4.0f, 0.0f, 12.0f, "2_typography");
        el.props.setColorArgb("outlineColor", {10, 10, 15, 255}, "crgba", "2_typography");
        el.props.setVec2("shadowOffset", {3.0f, 4.0f}, -40.0f, 40.0f, "2_typography");
        el.props.setColorArgb("shadowColor", {0, 0, 0, 210}, "crgba", "2_typography");
        el.props.setFloat("hdrIntensity", 1.1f, 0.0f, 16.0f, "appearance");
      });
      applyTextPreset("Cinema Wide", [&]() {
        el.props.setString("fontWeight", "Light", "sel Default Light Regular SemiBold Bold Black", "2_typography");
        el.props.setFloat("letterSpacing", 8.0f, -10.0f, 40.0f, "2_typography");
        el.props.setFloat("lineSpacing", 1.25f, 0.6f, 2.5f, "2_typography");
        el.props.setFloat("outlineWidth", 0.0f, 0.0f, 12.0f, "2_typography");
        el.props.setVec2("shadowOffset", {0.0f, 2.0f}, -40.0f, 40.0f, "2_typography");
        el.props.setColorArgb("shadowColor", {0, 0, 0, 160}, "crgba", "2_typography");
        el.props.setFloat("hdrIntensity", 1.2f, 0.0f, 16.0f, "appearance");
      });
    } else if (el.type == iota::model::ElementType::ShakeEffect) {
      ImGui::TextDisabled("Sapphire S_Shake Parity (Target: composition:0 shakes full canvas below)");
      const auto defShake = iota::model::createDefaultElement(iota::model::ElementType::ShakeEffect);
      auto resetShakeToNeutral = [&]() {
        auto& items = el.props.items();
        auto isMotionOrChannelGroup = [](const std::string& g) {
          return g.rfind("2_", 0) == 0 || g.rfind("3_", 0) == 0 ||
                 g.rfind("4_", 0) == 0 || g.rfind("5_", 0) == 0;
        };
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&](const auto& it) {
                                     return isMotionOrChannelGroup(it.entry.groupTag);
                                   }),
                    items.end());
        for (const auto& defItem : defShake.props.items()) {
          if (isMotionOrChannelGroup(defItem.entry.groupTag)) {
            if (!el.props.contains(defItem.key)) {
              items.push_back(defItem);
            }
          } else if (!el.props.contains(defItem.key)) {
            items.push_back(defItem);
          }
        }
      };
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Presets:");
      const float maxWrapX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
      auto applyShakePreset = [&](const char* label, const auto& fn) {
        const float btnW = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + btnW <= maxWrapX) {
          ImGui::SameLine();
        }
        if (ImGui::SmallButton(label)) {
          resetShakeToNeutral();
          fn();
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = std::string("Apply Shake Preset: ") + label;
        }
      };
      applyShakePreset("Cinematic Ambient", [&]() {
        el.props.setString("style", "Normal", "sel Normal Twitchy Jumpy", "2_shakeMaster");
        el.props.setFloat("amplitude", 0.24f, 0.0f, 5.0f, "2_shakeMaster");
        el.props.setMVar("measureAmplitude", iota::model::MVariableFloat{{{"Nothing", 1.0f, 0.0f}}}, "mvarf 0.0 5.0", "2_shakeMaster");
        el.props.setFloat("frequency", 0.8f, 0.05f, 30.0f, "2_shakeMaster");
        el.props.setFloat("phase", 1.0f, -100.0f, 100.0f, "2_shakeMaster");
        el.props.setFloat("zDist", 1.0f, 0.15f, 3.0f, "2_shakeMaster");
        el.props.setInt("octaves", 3, 1, 10, "2_shakeMaster");
        el.props.setFloat("xRandAmp", 0.075f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("xRandFreq", 1.0f, 0.1f, 5.0f, "3_axes");
        el.props.setFloat("yRandAmp", 0.083f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("yRandFreq", 1.0f, 0.1f, 5.0f, "3_axes");
        el.props.setFloat("zRandAmp", 0.0f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("tiltRandAmp", 3.2f, 0.0f, 45.0f, "3_axes");
        el.props.setFloat("tiltRandFreq", 0.65f, 0.1f, 5.0f, "3_axes");
        el.props.setFloat("tiltWaveAmp", 2.2f, 0.0f, 45.0f, "3_axes");
        el.props.setFloat("tiltWaveFreq", 0.38f, 0.1f, 5.0f, "3_axes");
        el.props.setFloat("tiltPhase", 2.6f, -20.0f, 20.0f, "3_axes");
        el.props.setBool("motionBlur", false, "5_motionAndCrop", "b");
        el.props.setFloat("moBlurLength", 1.0f, 0.0f, 4.0f, "5_motionAndCrop");
      });
      applyShakePreset("Handheld Drift", [&]() {
        el.props.setFloat("amplitude", 0.25f, 0.0f, 5.0f, "2_shakeMaster");
        el.props.setMVar("measureAmplitude", iota::model::MVariableFloat{{{"Nothing", 1.0f, 0.0f}}}, "mvarf 0.0 5.0", "2_shakeMaster");
        el.props.setFloat("frequency", 1.8f, 0.05f, 30.0f, "2_shakeMaster");
        el.props.setInt("octaves", 2, 1, 10, "2_shakeMaster");
        el.props.setFloat("xRandAmp", 0.12f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("yRandAmp", 0.10f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("zRandAmp", 0.04f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("tiltRandAmp", 2.0f, 0.0f, 45.0f, "3_axes");
        el.props.setBool("motionBlur", true, "5_motionAndCrop", "b");
        el.props.setFloat("moBlurLength", 0.50f, 0.0f, 4.0f, "5_motionAndCrop");
      });
      applyShakePreset("Bass Impact", [&]() {
        el.props.setFloat("amplitude", 0.05f, 0.0f, 5.0f, "2_shakeMaster");
        el.props.setMVar("measureAmplitude", iota::model::MVariableFloat{{{"SoundKeys1", 1.6f, 0.0f}}}, "mvarf 0.0 5.0", "2_shakeMaster");
        el.props.setFloat("frequency", 9.0f, 0.05f, 30.0f, "2_shakeMaster");
        el.props.setInt("octaves", 4, 1, 10, "2_shakeMaster");
        el.props.setFloat("xRandAmp", 0.20f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("yRandAmp", 0.18f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("zRandAmp", 0.12f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("tiltRandAmp", 5.0f, 0.0f, 45.0f, "3_axes");
        el.props.setFloat("twitchAmp", 0.35f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("redAmp", 1.12f, 0.0f, 3.0f, "4_channels");
        el.props.setFloat("blueAmp", 0.88f, 0.0f, 3.0f, "4_channels");
        el.props.setFloat("redPhase", 0.015f, -10.0f, 10.0f, "4_channels");
        el.props.setFloat("bluePhase", -0.015f, -10.0f, 10.0f, "4_channels");
        el.props.setBool("motionBlur", true, "5_motionAndCrop", "b");
        el.props.setFloat("moBlurLength", 1.2f, 0.0f, 4.0f, "5_motionAndCrop");
      });
      applyShakePreset("Twitchy Burst", [&]() {
        el.props.setString("style", "Twitchy", "sel Normal Twitchy Jumpy", "2_shakeMaster");
        el.props.setFloat("stillness", 0.70f, 0.0f, 1.0f, "2_shakeMaster");
        el.props.setFloat("twitchFreq", 2.0f, 0.05f, 20.0f, "2_shakeMaster");
        el.props.setFloat("amplitude", 0.55f, 0.0f, 5.0f, "2_shakeMaster");
        el.props.setMVar("measureAmplitude", iota::model::MVariableFloat{{{"Nothing", 1.0f, 0.0f}}}, "mvarf 0.0 5.0", "2_shakeMaster");
        el.props.setFloat("frequency", 12.0f, 0.05f, 30.0f, "2_shakeMaster");
        el.props.setInt("octaves", 4, 1, 10, "2_shakeMaster");
        el.props.setFloat("xRandAmp", 0.22f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("yRandAmp", 0.18f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("zRandAmp", 0.10f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("tiltRandAmp", 5.5f, 0.0f, 45.0f, "3_axes");
        el.props.setFloat("twitchAmp", 0.45f, 0.0f, 1.0f, "3_axes");
        el.props.setBool("motionBlur", true, "5_motionAndCrop", "b");
        el.props.setFloat("moBlurLength", 1.1f, 0.0f, 4.0f, "5_motionAndCrop");
        el.props.setInt("blurSamples", 20, 4, 48, "5_motionAndCrop");
      });
      applyShakePreset("Jumpy Strobe", [&]() {
        el.props.setString("style", "Jumpy", "sel Normal Twitchy Jumpy", "2_shakeMaster");
        el.props.setFloat("drift", 0.25f, 0.0f, 1.0f, "2_shakeMaster");
        el.props.setFloat("centerBias", 1.5f, 0.0f, 10.0f, "2_shakeMaster");
        el.props.setFloat("amplitude", 0.50f, 0.0f, 5.0f, "2_shakeMaster");
        el.props.setMVar("measureAmplitude", iota::model::MVariableFloat{{{"Nothing", 1.0f, 0.0f}}}, "mvarf 0.0 5.0", "2_shakeMaster");
        el.props.setFloat("frequency", 5.0f, 0.05f, 30.0f, "2_shakeMaster");
        el.props.setInt("octaves", 2, 1, 10, "2_shakeMaster");
        el.props.setFloat("xRandAmp", 0.20f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("yRandAmp", 0.15f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("zRandAmp", 0.12f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("tiltRandAmp", 4.0f, 0.0f, 45.0f, "3_axes");
        el.props.setBool("motionBlur", true, "5_motionAndCrop", "b");
        el.props.setFloat("moBlurLength", 0.85f, 0.0f, 4.0f, "5_motionAndCrop");
      });
      applyShakePreset("Z-Pump Bounce", [&]() {
        el.props.setFloat("amplitude", 0.85f, 0.0f, 5.0f, "2_shakeMaster");
        el.props.setMVar("measureAmplitude", iota::model::MVariableFloat{{{"SoundKeys1", 1.0f, 0.0f}}}, "mvarf 0.0 5.0", "2_shakeMaster");
        el.props.setFloat("frequency", 4.0f, 0.05f, 30.0f, "2_shakeMaster");
        el.props.setInt("octaves", 2, 1, 10, "2_shakeMaster");
        el.props.setFloat("xRandAmp", 0.06f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("yRandAmp", 0.06f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("zRandAmp", 0.15f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("zWaveAmp", 0.28f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("zWaveFreq", 1.0f, 0.1f, 5.0f, "3_axes");
        el.props.setFloat("tiltRandAmp", 0.0f, 0.0f, 45.0f, "3_axes");
        el.props.setFloat("tiltWaveAmp", 2.5f, 0.0f, 45.0f, "3_axes");
        el.props.setFloat("tiltWaveFreq", 0.5f, 0.1f, 5.0f, "3_axes");
        el.props.setBool("motionBlur", true, "5_motionAndCrop", "b");
        el.props.setFloat("moBlurLength", 1.0f, 0.0f, 4.0f, "5_motionAndCrop");
      });
      applyShakePreset("RGB Glitch Twitch", [&]() {
        el.props.setString("style", "Twitchy", "sel Normal Twitchy Jumpy", "2_shakeMaster");
        el.props.setFloat("stillness", 0.55f, 0.0f, 1.0f, "2_shakeMaster");
        el.props.setFloat("twitchFreq", 3.0f, 0.05f, 20.0f, "2_shakeMaster");
        el.props.setFloat("amplitude", 0.55f, 0.0f, 5.0f, "2_shakeMaster");
        el.props.setMVar("measureAmplitude", iota::model::MVariableFloat{{{"Beat", 1.0f, 0.0f}}}, "mvarf 0.0 5.0", "2_shakeMaster");
        el.props.setFloat("frequency", 16.0f, 0.05f, 30.0f, "2_shakeMaster");
        el.props.setInt("octaves", 5, 1, 10, "2_shakeMaster");
        el.props.setFloat("xRandAmp", 0.22f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("yRandAmp", 0.16f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("zRandAmp", 0.10f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("tiltRandAmp", 4.5f, 0.0f, 45.0f, "3_axes");
        el.props.setFloat("twitchAmp", 0.85f, 0.0f, 1.0f, "3_axes");
        el.props.setFloat("redAmp", 1.35f, 0.0f, 3.0f, "4_channels");
        el.props.setFloat("greenAmp", 1.0f, 0.0f, 3.0f, "4_channels");
        el.props.setFloat("blueAmp", 0.70f, 0.0f, 3.0f, "4_channels");
        el.props.setFloat("redPhase", 0.05f, -10.0f, 10.0f, "4_channels");
        el.props.setFloat("bluePhase", -0.05f, -10.0f, 10.0f, "4_channels");
        el.props.setFloat("rgbRandomness", 0.55f, 0.0f, 2.0f, "4_channels");
        el.props.setFloat("rgbFrequency", 8.0f, 0.05f, 30.0f, "4_channels");
        el.props.setBool("motionBlur", true, "5_motionAndCrop", "b");
        el.props.setFloat("moBlurLength", 0.80f, 0.0f, 4.0f, "5_motionAndCrop");
      });
    }

    ImGui::Separator();

    renderPropertyBagInspector(el.props, el.type, actions);

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
          const std::string fname = iota::core::pathFilenameUtf8(*chosen);
          project_.localAssets[fname] = std::move(bytes);
          el.props.setString(actions.assetTargetPropName, "local:" + fname, "img", "1_image");
          renderer_.invalidateLocalAssets();
          pushUndoState("Import Local Asset");
          statusBanner_ = "Bundled asset local:" + fname;
        }
      }
    }

    if (actions.requestLocalFontImport && !actions.assetTargetPropName.empty() && window_) {
      HWND hwnd = glfwGetWin32Window(window_);
      if (auto chosen = showOpenFileDialog(hwnd,
                                             L"Import TrueType / OpenType Font",
                                             L"Fonts (*.ttf;*.otf;*.ttc)\0*.ttf;*.otf;*.ttc\0All Files (*.*)\0*.*\0")) {
        auto bytes = readAllBytes(*chosen);
        if (!bytes.empty()) {
          const std::string fname = iota::core::pathFilenameUtf8(*chosen);
          project_.localAssets[fname] = std::move(bytes);
          el.props.setString(actions.assetTargetPropName, "local:" + fname, "passet", "appearance");
          renderer_.invalidateLocalAssets();
          pushUndoState("Import Custom Font");
          statusBanner_ = "Bundled font local:" + fname;
        }
      }
    }
  } else {
    ImGui::TextWrapped("Composition %d: %s", selectedCompIdx_, comp.displayName.c_str());
    char compNameBuf[256]{};
    std::snprintf(compNameBuf, sizeof(compNameBuf), "%s", comp.displayName.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Composition Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##CompName", compNameBuf, sizeof(compNameBuf))) {
      comp.displayName = iota::core::sanitizeUtf8(compNameBuf);
      actions.modified = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Rename Composition";
    }
    if (selectedCompIdx_ == 0) {
      ImGui::TextDisabled("Pipeline: %s | Bloom: %s",
                          frameStats_.hdrPipelineActive ? "HDR16F Float" : "SDR8",
                          frameStats_.bloomActive ? "ON" : "OFF");
      auto ensureHdrProps = [&]() {
        const auto defComp = iota::model::createDefaultComposition(0);
        for (const auto& defItem : defComp.props.items()) {
          if (defItem.entry.groupTag == "3_hdrPost" && !comp.props.contains(defItem.key)) {
            comp.props.items().push_back(defItem);
          }
        }
      };
      if (!comp.props.contains("hdrPipeline")) {
        if (ImGui::SmallButton("Enable HDR & Bloom Controls")) {
          ensureHdrProps();
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = "Enable HDR & Bloom Controls";
        }
      }
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("HDR Presets:");
      const float maxWrapX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
      auto applyHdrPreset = [&](const char* label, const auto& fn) {
        const float btnW = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + btnW <= maxWrapX) {
          ImGui::SameLine();
        }
        if (ImGui::SmallButton(label)) {
          ensureHdrProps();
          fn();
          actions.modified = true;
          actions.undoSnapshotNeeded = true;
          actions.editLabel = std::string("Apply HDR Preset: ") + label;
        }
      };
      applyHdrPreset("Clean SDR", [&]() {
        comp.props.setBool("hdrPipeline", false, "3_hdrPost", "b");
        comp.props.setString("tonemapMode", "None", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
        comp.props.setBool("bloomEnabled", false, "3_hdrPost", "b");
        comp.props.setFloat("exposureEv", 0.0f, -4.0f, 4.0f, "3_hdrPost");
        comp.props.setFloat("contrast", 1.0f, 0.5f, 2.0f, "3_hdrPost");
        comp.props.setFloat("saturation", 1.0f, 0.0f, 2.0f, "3_hdrPost");
        comp.props.setFloat("gamma", 1.0f, 0.4f, 2.5f, "3_hdrPost");
        comp.props.setFloat("vignette", 0.0f, 0.0f, 1.0f, "3_hdrPost");
      });
      applyHdrPreset("ACES Cinema", [&]() {
        comp.props.setBool("hdrPipeline", true, "3_hdrPost", "b");
        comp.props.setString("tonemapMode", "ACES", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
        comp.props.setBool("bloomEnabled", true, "3_hdrPost", "b");
        comp.props.setFloat("bloomThreshold", 0.85f, 0.0f, 5.0f, "3_hdrPost");
        comp.props.setFloat("bloomSoftKnee", 0.55f, 0.0f, 1.0f, "3_hdrPost");
        comp.props.setFloat("bloomIntensity", 0.55f, 0.0f, 4.0f, "3_hdrPost");
        comp.props.setFloat("bloomRadius", 0.72f, 0.10f, 0.98f, "3_hdrPost");
        comp.props.setFloat("bloomAnamorphic", 0.0f, 0.0f, 1.0f, "3_hdrPost");
        comp.props.setFloat("contrast", 1.04f, 0.5f, 2.0f, "3_hdrPost");
        comp.props.setFloat("vignette", 0.18f, 0.0f, 1.0f, "3_hdrPost");
      });
      applyHdrPreset("AgX Neon", [&]() {
        comp.props.setBool("hdrPipeline", true, "3_hdrPost", "b");
        comp.props.setString("tonemapMode", "AgX", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
        comp.props.setBool("bloomEnabled", true, "3_hdrPost", "b");
        comp.props.setFloat("bloomThreshold", 0.65f, 0.0f, 5.0f, "3_hdrPost");
        comp.props.setFloat("bloomSoftKnee", 0.65f, 0.0f, 1.0f, "3_hdrPost");
        comp.props.setFloat("bloomIntensity", 0.80f, 0.0f, 4.0f, "3_hdrPost");
        comp.props.setFloat("bloomRadius", 0.78f, 0.10f, 0.98f, "3_hdrPost");
        comp.props.setFloat("bloomAnamorphic", 0.0f, 0.0f, 1.0f, "3_hdrPost");
        comp.props.setFloat("saturation", 1.12f, 0.0f, 2.0f, "3_hdrPost");
      });
      applyHdrPreset("Anamorphic Flare", [&]() {
        comp.props.setBool("hdrPipeline", true, "3_hdrPost", "b");
        comp.props.setString("tonemapMode", "ACES", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
        comp.props.setBool("bloomEnabled", true, "3_hdrPost", "b");
        comp.props.setFloat("bloomThreshold", 0.95f, 0.0f, 5.0f, "3_hdrPost");
        comp.props.setFloat("bloomIntensity", 0.70f, 0.0f, 4.0f, "3_hdrPost");
        comp.props.setFloat("bloomRadius", 0.85f, 0.10f, 0.98f, "3_hdrPost");
        comp.props.setFloat("bloomAnamorphic", 0.90f, 0.0f, 1.0f, "3_hdrPost");
        comp.props.setColorArgb("bloomTint", {160, 210, 255, 255}, "crgb", "3_hdrPost");
      });
      applyHdrPreset("Audio Pulse Bloom", [&]() {
        comp.props.setBool("hdrPipeline", true, "3_hdrPost", "b");
        comp.props.setString("tonemapMode", "PBRNeutral", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
        comp.props.setBool("bloomEnabled", true, "3_hdrPost", "b");
        comp.props.setFloat("bloomThreshold", 0.75f, 0.0f, 5.0f, "3_hdrPost");
        comp.props.setFloat("bloomIntensity", 0.35f, 0.0f, 4.0f, "3_hdrPost");
        comp.props.setMVar("measureBloomIntensity",
                           iota::model::MVariableFloat{{{"SoundKeys1", 1.25f, 0.0f}}},
                           "mvarf 0.0 3.0",
                           "3_hdrPost");
        comp.props.setFloat("bloomRadius", 0.76f, 0.10f, 0.98f, "3_hdrPost");
      });
    }
    ImGui::Separator();
    renderPropertyBagInspector(comp.props, iota::model::ElementType::Composition, actions);
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

void EditorApp::drawSoundKeysPanel() {
  InspectorActions skActions;
  renderSoundKeysPanel(project_,
                       frameStats_,
                       activeSoundKeysRange_,
                       showSoundKeysPanel_,
                       selectedCompIdx_,
                       selectedElemIdx_,
                       skActions);
  if (skActions.undoSnapshotNeeded) {
    pushUndoState(skActions.editLabel.empty() ? "Modify Sound Keys" : skActions.editLabel);
  }
}

void EditorApp::drawExportModal() {
  if (openExportModal_) {
    ImGui::OpenPopup("Export Video (MP4)");
    openExportModal_ = false;
    exportModalError_.clear();
    exportModalSuccess_.clear();
    if (!encoderProbe_.snapshot().probed && !encoderProbe_.isProbing()) {
      encoderProbe_.startAsync();
    }
  }

  if (openExportModal_ || ImGui::IsPopupOpen("Export Video (MP4)")) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(500.0f, 0.0f), ImVec2(660.0f, 820.0f));
  }
  if (ImGui::BeginPopupModal("Export Video (MP4)", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const bool exporting = exportSession_.isActive();
    const auto hwCaps = encoderProbe_.snapshot();
    const bool probingHw = encoderProbe_.isProbing();

    auto showWrappedTooltip = [](const char* text) {
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(360.0f);
        ImGui::TextWrapped("%s", text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
    };

    ImGui::BeginDisabled(exporting);

    ImGui::SeparatorText("Resolution & Frame Rate");
    if (ImGui::Button("720p60")) {
      exportCfg_.width = 1280; exportCfg_.height = 720; exportCfg_.fps = 60; exportCfg_.videoBitrateKbps = 12000;
    }
    ImGui::SameLine();
    if (ImGui::Button("1080p60")) {
      exportCfg_.width = 1920; exportCfg_.height = 1080; exportCfg_.fps = 60; exportCfg_.videoBitrateKbps = 18000;
    }
    ImGui::SameLine();
    if (ImGui::Button("1440p60")) {
      exportCfg_.width = 2560; exportCfg_.height = 1440; exportCfg_.fps = 60; exportCfg_.videoBitrateKbps = 28000;
    }
    ImGui::SameLine();
    if (ImGui::Button("Shorts 1080x1920")) {
      exportCfg_.width = 1080; exportCfg_.height = 1920; exportCfg_.fps = 60; exportCfg_.videoBitrateKbps = 16000;
    }

    ImGui::PushItemWidth(260.0f);
    if (ImGui::InputInt("Width", &exportCfg_.width)) {
      exportCfg_.width = std::clamp(exportCfg_.width, 64, 7680);
    }
    if (ImGui::InputInt("Height", &exportCfg_.height)) {
      exportCfg_.height = std::clamp(exportCfg_.height, 64, 7680);
    }
    if (ImGui::InputInt("Frame Rate (FPS)", &exportCfg_.fps)) {
      exportCfg_.fps = std::clamp(exportCfg_.fps, 1, 120);
    }

    ImGui::SeparatorText("Hardware & Video Encoder");

    static constexpr iota::export_video::VideoCodec kCodecs[] = {
        iota::export_video::VideoCodec::H264,
        iota::export_video::VideoCodec::H265,
    };
    if (ImGui::BeginCombo("Video Codec", iota::export_video::videoCodecDisplayName(exportCfg_.codec))) {
      for (auto codec : kCodecs) {
        const bool sel = (exportCfg_.codec == codec);
        if (ImGui::Selectable(iota::export_video::videoCodecDisplayName(codec), sel)) {
          exportCfg_.codec = codec;
        }
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    showWrappedTooltip(
        "H.264 / AVC offers maximum compatibility with Discord, YouTube, and mobile players.\n"
        "H.265 / HEVC produces smaller files at the same bitrate, especially fast with GPU hardware.");

    auto backendOptionLabel = [&](iota::export_video::VideoEncoderBackend b) -> std::string {
      std::string lbl = iota::export_video::videoEncoderBackendDisplayName(b);
      if (probingHw && !hwCaps.probed) {
        lbl += "  [Detecting...]";
      } else if (hwCaps.probed) {
        if (!hwCaps.ffmpegAvailable) {
          lbl += "  [FFmpeg Missing]";
        } else if (b == iota::export_video::VideoEncoderBackend::Auto) {
          lbl += hwCaps.hasAnyHardware() ? "  [GPU Ready]" : "  [CPU Mode]";
        } else if (hwCaps.supports(b, exportCfg_.codec)) {
          lbl += "  [Available]";
        } else {
          lbl += "  [Not Detected]";
        }
      }
      return lbl;
    };

    static constexpr iota::export_video::VideoEncoderBackend kBackends[] = {
        iota::export_video::VideoEncoderBackend::Auto,
        iota::export_video::VideoEncoderBackend::NvidiaNvenc,
        iota::export_video::VideoEncoderBackend::AmdAmf,
        iota::export_video::VideoEncoderBackend::IntelQsv,
        iota::export_video::VideoEncoderBackend::CpuSoftware,
    };
    const std::string curBackendLabel = backendOptionLabel(exportCfg_.encoderBackend);
    if (ImGui::BeginCombo("Hardware / Encoder", curBackendLabel.c_str())) {
      for (auto b : kBackends) {
        const bool sel = (exportCfg_.encoderBackend == b);
        const std::string optLabel = backendOptionLabel(b);
        if (ImGui::Selectable(optLabel.c_str(), sel)) {
          exportCfg_.encoderBackend = b;
        }
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    showWrappedTooltip(
        "Auto uses GPU hardware acceleration (NVIDIA NVENC, AMD AMF, or Intel Quick Sync) when detected, "
        "which significantly speeds up MP4 rendering and reduces CPU usage.\n"
        "Select CPU Software (x264 / x265) for maximum software encoding quality.");

    static constexpr iota::export_video::EncoderPreset kPresets[] = {
        iota::export_video::EncoderPreset::Fast,
        iota::export_video::EncoderPreset::Balanced,
        iota::export_video::EncoderPreset::Quality,
    };
    if (ImGui::BeginCombo("Quality / Speed Preset",
                          iota::export_video::encoderPresetDisplayName(exportCfg_.encoderPreset))) {
      for (auto preset : kPresets) {
        const bool sel = (exportCfg_.encoderPreset == preset);
        if (ImGui::Selectable(iota::export_video::encoderPresetDisplayName(preset), sel)) {
          exportCfg_.encoderPreset = preset;
        }
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    showWrappedTooltip(
        "Controls the trade-off between export speed and compression efficiency for both GPU and CPU encoders.");

    if (ImGui::InputInt("Video Bitrate (kbps)", &exportCfg_.videoBitrateKbps)) {
      exportCfg_.videoBitrateKbps = std::clamp(exportCfg_.videoBitrateKbps, 500, 100000);
    }

    ImGui::Checkbox("Auto-fallback to CPU if hardware encoder fails", &exportCfg_.fallbackToCpuOnError);
    showWrappedTooltip(
        "When enabled, if a selected GPU encoder is unavailable on the current GPU or driver, export "
        "automatically falls back to CPU Software (libx264 / libx265) instead of aborting.");
    ImGui::Checkbox("FXAA Anti-Aliasing", &exportCfg_.fxaa);

    if (probingHw) {
      ImGui::TextDisabled("Hardware Status: Detecting GPU hardware encoders...");
    } else if (!hwCaps.ffmpegAvailable) {
      ImGui::TextColored(ImVec4(1.0f, 0.40f, 0.40f, 1.0f),
                         "FFmpeg not found — install ffmpeg.exe to export MP4 videos.");
    } else if (hwCaps.hasAnyHardware()) {
      ImGui::TextColored(ImVec4(0.45f, 0.92f, 0.55f, 1.0f), "%s", hwCaps.summaryText().c_str());
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
                         "No GPU encoder detected — CPU Software encoding will be used.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(probingHw);
    if (ImGui::SmallButton("Re-detect")) {
      encoderProbe_.startAsync();
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Time Range & Output");
    if (ImGui::InputDouble("Start Time (sec)", &exportCfg_.startSec, 0.5, 5.0, "%.2f")) {
      exportCfg_.startSec = std::max(0.0, exportCfg_.startSec);
    }
    if (ImGui::InputDouble("End Time (sec, 0 = full)", &exportCfg_.endSec, 0.5, 5.0, "%.2f")) {
      exportCfg_.endSec = std::max(0.0, exportCfg_.endSec);
    }

    char outBuf[1024]{};
    const std::string outUtf8 = iota::core::pathToUtf8(exportCfg_.outputPath);
    std::snprintf(outBuf, sizeof(outBuf), "%s", outUtf8.c_str());
    if (ImGui::InputText("Output MP4", outBuf, sizeof(outBuf))) {
      exportCfg_.outputPath = iota::core::pathFromUtf8(outBuf);
    }
    ImGui::PopItemWidth();
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
      ImGui::Text("Active Encoder: %s (%s)",
                  exportSession_.activeEncoderDisplayName().c_str(),
                  exportSession_.activeFfmpegEncoder().c_str());
      if (exportSession_.usedCpuFallback()) {
        const std::string notice = exportSession_.fallbackNotice();
        ImGui::PushTextWrapPos(480.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "%s", notice.c_str());
        ImGui::PopTextWrapPos();
      }

      const int encoded = exportSession_.encodedFrames();
      const int total = exportSession_.totalFrames();
      const float prog = total > 0 ? static_cast<float>(encoded) / static_cast<float>(total) : 0.0f;
      ImGui::ProgressBar(prog, ImVec2(360.0f, 0.0f));
      ImGui::Text("Encoding frame %d / %d (pre-roll %d / %d)...",
                  encoded,
                  total,
                  exportSession_.currentPreRollIndex(),
                  exportCfg_.preRollFrames);

      const double elapsedSec = std::max(0.0, glfwGetTime() - exportStartWallSec_);
      if (encoded > 0 && elapsedSec > 0.05) {
        const double fpsSpeed = static_cast<double>(encoded) / elapsedSec;
        const double rtMult = fpsSpeed / static_cast<double>(std::max(1, exportCfg_.fps));
        const int etaTotalSec = static_cast<int>(std::llround(
            static_cast<double>(std::max(0, total - encoded)) / std::max(0.05, fpsSpeed)));
        ImGui::TextDisabled("Speed: %.1f fps (%.2fx realtime)  |  ETA %d:%02d",
                            fpsSpeed,
                            rtMult,
                            etaTotalSec / 60,
                            etaTotalSec % 60);
      } else {
        ImGui::TextDisabled("Warming up pre-roll & encoder...");
      }

      if (ImGui::Button("Cancel Export")) {
        exportSession_.cancel();
        statusBanner_ = "Export cancelled";
      }
    } else {
      if (!exportModalError_.empty()) {
        ImGui::PushTextWrapPos(480.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s", exportModalError_.c_str());
        ImGui::PopTextWrapPos();
      } else if (!exportModalSuccess_.empty()) {
        ImGui::PushTextWrapPos(480.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.92f, 0.55f, 1.0f), "%s", exportModalSuccess_.c_str());
        ImGui::PopTextWrapPos();
      }

      if (ImGui::Button("Start Export")) {
        exportCfg_.visOffsetMs = visOffsetMs_;
        exportStartWallSec_ = glfwGetTime();
        exportModalError_.clear();
        exportModalSuccess_.clear();
        std::string err;
        renderer_.resetSimulation();
        if (exportSession_.begin(exportCfg_, audio_, &err, hwCaps.probed ? &hwCaps : nullptr)) {
          std::string msg = "Exporting MP4 (" + exportSession_.activeEncoderDisplayName() + ")";
          if (exportSession_.usedCpuFallback() && !exportSession_.fallbackNotice().empty()) {
            msg += " — " + exportSession_.fallbackNotice();
          }
          statusBanner_ = std::move(msg);
        } else {
          statusBanner_ = "Export failed to start: " + err;
          exportModalError_ = std::move(err);
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

void EditorApp::drawAssetBrowserPanel() {
  AssetBrowserActions actions;
  renderAssetBrowserPanel(project_,
                          audio_,
                          device_,
                          renderer_,
                          assetBrowserState_,
                          selectedCompIdx_,
                          selectedElemIdx_,
                          showAssetBrowserPanel_,
                          actions);

  if (actions.selectCompIdx >= 0) {
    setSingleSelection(actions.selectCompIdx, actions.selectElemIdx);
  }
  if (actions.invalidateLocalAssets) {
    renderer_.invalidateLocalAssets();
  }
  if (actions.requestImportImages && window_) {
    const auto chosen = showOpenFilesDialog(
        glfwGetWin32Window(window_),
        L"Import Image Assets into Project",
        L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0");
    int imported = 0;
    for (const auto& path : chosen) {
      auto bytes = readAllBytes(path);
      if (!bytes.empty()) {
        const std::string fname = iota::core::pathFilenameUtf8(path);
        project_.localAssets[fname] = std::move(bytes);
        assetBrowserState_.selectedKind = BrowserItemKind::LocalImage;
        assetBrowserState_.selectedLabel = fname;
        assetBrowserState_.selectedUri = "local:" + fname;
        ++imported;
      }
    }
    if (imported > 0) {
      renderer_.invalidateLocalAssets();
      pushUndoState(imported == 1 ? "Import Image Asset" : "Import Image Assets");
      statusBanner_ = "Imported " + std::to_string(imported) + " asset(s)";
    }
  }
  if (actions.requestReplaceLocalAsset && !actions.replaceTargetName.empty() && window_) {
    if (auto chosen = showOpenFileDialog(
            glfwGetWin32Window(window_),
            L"Replace Local Asset File",
            L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0")) {
      auto bytes = readAllBytes(*chosen);
      if (!bytes.empty()) {
        project_.localAssets[actions.replaceTargetName] = std::move(bytes);
        renderer_.invalidateLocalAssets();
        pushUndoState("Replace Local Asset");
        statusBanner_ = "Replaced local:" + actions.replaceTargetName;
      }
    }
  }
  if (actions.requestImportAudio) {
    actionOpenAudio();
  }
  if (actions.requestLoadAudioPath && !actions.loadAudioPath.empty()) {
    const std::filesystem::path audioDiskPath = iota::core::pathFromUtf8(actions.loadAudioPath);
    std::string err;
    if (audio_.loadFile(audioDiskPath, &err)) {
      project_.metadata.audioPath = iota::core::pathToUtf8(audioDiskPath);
      playAccumTimeSec_ = 0.0;
      renderer_.resetSimulation();
      audio_.play();
      statusBanner_ = "Playing " + iota::core::pathFilenameUtf8(audioDiskPath);
    } else {
      statusBanner_ = "Audio load failed: " + err;
    }
  }
  if (actions.undoSnapshotNeeded) {
    pushUndoState(actions.editLabel.empty() ? "Modify Assets" : actions.editLabel);
  }
}

void EditorApp::drawProjectManagerModal() {
  ProjectManagerActions actions;
  renderProjectManagerModal(openProjectManagerModal_,
                            project_,
                            currentProjectPath_,
                            projectDirty_,
                            projectLibrary_,
                            projectManagerState_,
                            actions);

  if (actions.registryModified && !isSmokeTest_) {
    projectLibrary_.save(iota::preset_io::ProjectLibrary::defaultRegistryPath());
  }
  if (actions.openFileDialog) {
    actionOpenPreset();
  }
  if (actions.openRecentFile && !actions.openPath.empty()) {
    loadProjectFromPath(actions.openPath);
  }
  if (actions.createTemplateProject || actions.createAndSaveProject) {
    project_ = iota::preset_io::createProjectFromTemplate(actions.templateKind,
                                                          projectManagerState_.newName,
                                                          projectManagerState_.newWidth,
                                                          projectManagerState_.newHeight,
                                                          projectManagerState_.newFps);
    project_.metadata.notes = projectManagerState_.newNotes;
    currentProjectPath_.clear();
    projectDirty_ = false;
    applyProjectMetadata(project_.metadata);
    setSingleSelection(0, project_.compositions.empty() || project_.compositions[0].elements.empty() ? -1 : 0);
    renderer_.invalidateLocalAssets();
    renderer_.resetSimulation();
    undoStack_.clear(project_);
    updateWindowTitle();
    statusBanner_ = "Created project " + project_.metadata.name;
    if (actions.createAndSaveProject) {
      saveProjectAsDialog("iota");
    }
  }
  if (actions.applyCurrentSettings) {
    project_.metadata.name = projectManagerState_.curName;
    project_.metadata.notes = projectManagerState_.curNotes;
    project_.metadata.previewWidth = projectManagerState_.curWidth;
    project_.metadata.previewHeight = projectManagerState_.curHeight;
    project_.metadata.targetFps = projectManagerState_.curFps;
    applyProjectMetadata(project_.metadata);
    pushUndoState("Update Project Settings");
    statusBanner_ = "Updated project settings";
  }
  if (actions.requestChangeAudio) {
    actionOpenAudio();
  }
  if (actions.requestCleanUnusedAssets) {
    const int removed = iota::model::removeUnusedLocalAssets(project_);
    if (removed > 0) {
      renderer_.invalidateLocalAssets();
      pushUndoState("Remove Unused Assets");
      statusBanner_ = "Removed " + std::to_string(removed) + " unused asset(s)";
    } else {
      statusBanner_ = "No unused local assets found";
    }
  }
  if (actions.requestSave) saveCurrentProject();
  if (actions.requestSaveAsIota) saveProjectAsDialog("iota");
  if (actions.requestExportViz) saveProjectAsDialog("viz");
  if (actions.requestExportJson) saveProjectAsDialog("json");
}

void EditorApp::drawUpdateModal() {
  const auto snap = updateChecker_.snapshot();
  UpdateModalActions actions;
  renderUpdateModal(openUpdateModal_, snap, actions);

  if (actions.requestRecheck) {
    updateChecker_.startCheckAsync(IOTA_STUDIO_VERSION,
                                   iota::core::UpdateChecker::kDefaultRepoOwner,
                                   iota::core::UpdateChecker::kDefaultRepoName,
                                   true);
  }
  if (actions.requestOpenUrl && !actions.urlToOpen.empty()) {
    openExternalUrl(actions.urlToOpen);
  }
  if (actions.copiedLink) {
    statusBanner_ = "Copied release link to clipboard";
  }
}

void EditorApp::drawDocsPanel() {
  DocsPanelActions actions;
  renderDocsPanel(showDocsPanel_, docsPanelState_, actions);
  if (!actions.copiedSnippet.empty()) {
    statusBanner_ = "Copied to clipboard: " + actions.copiedSnippet;
  }
}

void EditorApp::openDocsForElement(iota::model::ElementType type) {
  showDocsPanel_ = true;
  docsPanelState_.openElement(type);
}

void EditorApp::openExternalUrl(const std::string& url) {
  if (!iota::core::isSafeHttpsUrl(url)) {
    statusBanner_ = "Blocked unsafe release URL";
    return;
  }
  const int needed = MultiByteToWideChar(
      CP_UTF8, 0, url.data(), static_cast<int>(url.size()), nullptr, 0);
  if (needed <= 0) {
    statusBanner_ = "Failed to encode URL";
    return;
  }
  std::wstring wurl(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, url.data(), static_cast<int>(url.size()), wurl.data(), needed);

  HWND owner = window_ ? glfwGetWin32Window(window_) : nullptr;
  const INT_PTR rc = reinterpret_cast<INT_PTR>(
      ShellExecuteW(owner, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (rc <= 32) {
    statusBanner_ = "Failed to open browser for release link";
  } else {
    statusBanner_ = "Opened release page in browser";
  }
}

void EditorApp::updateWindowTitle() {
  if (isSmokeTest_ || !window_) {
    return;
  }
  const std::string rawName = project_.metadata.name.empty() ? "Untitled Project" : project_.metadata.name;
  std::string title = "Iota Studio - " + iota::core::sanitizeUtf8(rawName);
#ifdef IOTA_ENABLE_MCP
  if (mcpBridgeHost_.isListening()) {
    title += " [MCP Live]";
  }
#endif
  if (projectDirty_) {
    title += " *";
  }
  glfwSetWindowTitle(window_, title.c_str());
}

void EditorApp::applyProjectMetadata(const iota::model::ProjectMetadata& meta) {
  previewWidth_ = std::max(64, meta.previewWidth);
  previewHeight_ = std::max(64, meta.previewHeight);
  exportCfg_.width = previewWidth_;
  exportCfg_.height = previewHeight_;
  exportCfg_.fps = std::clamp(meta.targetFps, 15, 240);
}

void EditorApp::recordProjectInLibrary(const std::filesystem::path& path) {
  if (isSmokeTest_ || path.empty()) {
    return;
  }
  iota::preset_io::RecentProjectEntry entry;
  entry.name = project_.metadata.name.empty()
                   ? iota::core::pathStemUtf8(path)
                   : iota::core::sanitizeUtf8(project_.metadata.name);
  entry.path = iota::core::pathToUtf8(path);
  entry.compositionCount = static_cast<int>(project_.compositions.size());
  entry.assetCount = static_cast<int>(project_.localAssets.size());
  projectLibrary_.recordOpened(std::move(entry));
  projectLibrary_.save(iota::preset_io::ProjectLibrary::defaultRegistryPath());
}

bool EditorApp::loadProjectFromPath(const std::filesystem::path& path) {
#ifdef IOTA_ENABLE_MCP
  if (mcpBridgeHost_.isListening()) {
    disableMcpLiveBridge("MCP Live Bridge disabled for newly opened project");
  }
#endif
  std::string err;
  auto loaded = iota::preset_io::loadProjectFromFile(path, &err);
  if (!loaded) {
    statusBanner_ = "Failed to load preset: " + err;
    return false;
  }
  project_ = std::move(*loaded);
  if (project_.metadata.name.empty() || project_.metadata.name == "Untitled Project") {
    project_.metadata.name = iota::core::pathStemUtf8(path);
  }
  currentProjectPath_ = path;
  projectDirty_ = false;
  applyProjectMetadata(project_.metadata);
  setSingleSelection(0, project_.compositions.empty() || project_.compositions[0].elements.empty() ? -1 : 0);
  renderer_.invalidateLocalAssets();
  renderer_.resetSimulation();
  undoStack_.clear(project_);

  if (!project_.metadata.audioPath.empty()) {
    const std::filesystem::path linkedAudio = iota::core::pathFromUtf8(project_.metadata.audioPath);
    if (iota::core::pathExistsNoThrow(linkedAudio) && audio_.loadFile(linkedAudio)) {
      playAccumTimeSec_ = 0.0;
      audio_.pause();
    }
  }

  recordProjectInLibrary(path);
  updateWindowTitle();
  statusBanner_ = "Loaded " + iota::core::pathFilenameUtf8(path);
  return true;
}

bool EditorApp::saveCurrentProject() {
  if (currentProjectPath_.empty()) {
    return saveProjectAsDialog("iota");
  }
  project_.metadata.previewWidth = previewWidth_;
  project_.metadata.previewHeight = previewHeight_;
  project_.metadata.targetFps = exportCfg_.fps;

  std::string err;
  if (!iota::preset_io::saveProjectAutoFormat(project_, currentProjectPath_, &err)) {
    statusBanner_ = "Save failed: " + err;
    return false;
  }
  projectDirty_ = false;
  recordProjectInLibrary(currentProjectPath_);
  updateWindowTitle();
  statusBanner_ = "Saved " + iota::core::pathFilenameUtf8(currentProjectPath_);
  return true;
}

bool EditorApp::saveProjectAsDialog(const char* preferredExt) {
  if (!window_) return false;
  const std::string extStr = preferredExt ? preferredExt : "iota";
  const wchar_t* title = L"Save Project (.iota)";
  const wchar_t* filter =
      L"Iota Project (*.iota)\0*.iota\0Visualizer Preset (*.viz)\0*.viz\0JSON Scene (*.json)\0*.json\0";
  const wchar_t* defExt = L"iota";
  const wchar_t* initName = L"project.iota";
  if (extStr == "viz") {
    title = L"Export Preset (.viz)";
    filter = L"Visualizer Preset (*.viz)\0*.viz\0";
    defExt = L"viz";
    initName = L"preset.viz";
  } else if (extStr == "json") {
    title = L"Export Scene JSON (.json)";
    filter = L"JSON Scene (*.json)\0*.json\0";
    defExt = L"json";
    initName = L"scene.json";
  }

  auto chosen = showSaveFileDialog(glfwGetWin32Window(window_), title, filter, defExt, initName);
  if (!chosen) return false;

  project_.metadata.previewWidth = previewWidth_;
  project_.metadata.previewHeight = previewHeight_;
  project_.metadata.targetFps = exportCfg_.fps;
  if (project_.metadata.name.empty() || project_.metadata.name == "Untitled Project") {
    project_.metadata.name = iota::core::pathStemUtf8(*chosen);
  }

  std::string err;
  if (!iota::preset_io::saveProjectAutoFormat(project_, *chosen, &err)) {
    statusBanner_ = "Save failed: " + err;
    return false;
  }
  currentProjectPath_ = *chosen;
  projectDirty_ = false;
  recordProjectInLibrary(*chosen);
  updateWindowTitle();
  statusBanner_ = "Saved " + iota::core::pathFilenameUtf8(*chosen);
  return true;
}

void EditorApp::actionOpenPreset() {
  if (!window_) return;
  auto chosen = showOpenFileDialog(
      glfwGetWin32Window(window_),
      L"Open Project / Visualizer Preset (.iota / .viz / .json)",
      L"Iota & Presets (*.iota;*.viz;*.json;*.txt)\0*.iota;*.viz;*.json;*.txt\0All Files (*.*)\0*.*\0");
  if (!chosen) return;
  loadProjectFromPath(*chosen);
}

void EditorApp::actionSavePreset(bool asViz) {
  saveProjectAsDialog(asViz ? "viz" : "json");
}

void EditorApp::actionOpenAudio() {
  if (!window_) return;
  auto chosen = showOpenFileDialog(glfwGetWin32Window(window_),
                                     L"Open Audio Track",
                                     L"Audio Files (*.mp3;*.wav;*.flac;*.ogg;*.m4a;*.aac;*.opus)\0*.mp3;*.wav;*.flac;*.ogg;*.m4a;*.aac;*.opus\0All Files (*.*)\0*.*\0");
  if (!chosen) return;

  std::string err;
  if (audio_.loadFile(*chosen, &err)) {
    const std::string pathStr = iota::core::pathToUtf8(*chosen);
    const std::string fname = iota::core::pathFilenameUtf8(*chosen);
    project_.metadata.audioPath = pathStr;
    project_.metadata.audioAssets[fname] = pathStr;
    playAccumTimeSec_ = 0.0;
    renderer_.resetSimulation();
    audio_.play();
    pushUndoState("Link Audio Track");
    statusBanner_ = "Playing " + fname;
  } else {
    statusBanner_ = "Audio load failed: " + err;
  }
}

void EditorApp::actionLoadBuiltinTestTone(float freqHz) {
  auto track = iota::audio::createSyntheticSineTrack(freqHz, 8.0, 44100, 0.35f, "110 Hz Synth Test Tone");
  audio_.loadTrack(std::move(track));
  playAccumTimeSec_ = 0.0;
}

#ifdef IOTA_ENABLE_MCP

class EditorLiveStudioTarget final : public iota::mcp::IStudioBridgeTarget {
 public:
  explicit EditorLiveStudioTarget(EditorApp& app) noexcept : app_(app) {}

  bool isLiveStudio() const noexcept override { return true; }
  iota::model::Project& project() noexcept override { return app_.project_; }
  const iota::model::Project& project() const noexcept override { return app_.project_; }
  iota::audio::AudioEngine& audio() noexcept override { return app_.audio_; }
  const iota::audio::AudioEngine& audio() const noexcept override { return app_.audio_; }
  std::filesystem::path currentProjectPath() const override { return app_.currentProjectPath_; }

  bool loadProject(const std::filesystem::path& path,
                   std::string* outError,
                   std::filesystem::path* outWorkingCopyPath) override {
    std::string err;
    auto loaded = iota::preset_io::loadProjectFromFile(path, &err);
    if (!loaded) {
      if (outError) *outError = err;
      return false;
    }

    if (loaded->metadata.name.empty() || loaded->metadata.name == "Untitled Project") {
      loaded->metadata.name = iota::core::pathStemUtf8(path);
    }

    std::filesystem::path nextPath = path;
    std::filesystem::path origPath;
    if (!iota::mcp::isMcpWorkingCopyPath(path)) {
      const auto copyPath = iota::mcp::computeMcpWorkingCopyPath(path);
      if (!iota::preset_io::saveProjectAutoFormat(*loaded, copyPath, &err)) {
        if (outError) *outError = "Failed to create MCP working copy: " + err;
        return false;
      }
      origPath = path;
      nextPath = copyPath;
    }

    app_.project_ = std::move(*loaded);
    app_.mcpOriginalProjectPath_ = std::move(origPath);
    app_.currentProjectPath_ = nextPath;
    app_.projectDirty_ = false;
    app_.applyProjectMetadata(app_.project_.metadata);
    app_.setSingleSelection(
        0, app_.project_.compositions.empty() || app_.project_.compositions[0].elements.empty() ? -1 : 0);
    app_.renderer_.invalidateLocalAssets();
    app_.renderer_.resetSimulation();
    app_.undoStack_.clear(app_.project_);

    if (!app_.project_.metadata.audioPath.empty()) {
      const std::filesystem::path linkedAudio = iota::core::pathFromUtf8(app_.project_.metadata.audioPath);
      if (iota::core::pathExistsNoThrow(linkedAudio) && app_.audio_.loadFile(linkedAudio)) {
        app_.playAccumTimeSec_ = 0.0;
        app_.audio_.pause();
      }
    }

    app_.recordProjectInLibrary(nextPath);
    app_.updateWindowTitle();
    app_.statusBanner_ = "MCP Opened Working Copy: " + iota::core::pathFilenameUtf8(nextPath);
    if (outWorkingCopyPath) {
      *outWorkingCopyPath = nextPath;
    }
    return true;
  }

  bool createNewProject(iota::model::Project project,
                        const std::string& undoLabel) override {
    app_.project_ = std::move(project);
    app_.currentProjectPath_.clear();
    app_.mcpOriginalProjectPath_.clear();
    app_.projectDirty_ = true;
    app_.applyProjectMetadata(app_.project_.metadata);
    app_.setSingleSelection(
        0, app_.project_.compositions.empty() || app_.project_.compositions[0].elements.empty() ? -1 : 0);
    app_.renderer_.invalidateLocalAssets();
    app_.renderer_.resetSimulation();
    app_.pushUndoState(undoLabel);
    app_.updateWindowTitle();
    app_.statusBanner_ = undoLabel;
    return true;
  }

  bool saveProject(const std::filesystem::path& path,
                   bool allowVizOverwrite,
                   std::string* outError,
                   std::filesystem::path* outSavedPath) override {
    const std::filesystem::path target = path.empty() ? app_.currentProjectPath_ : path;
    if (target.empty()) {
      if (outError) *outError = "No target path provided and current project has not been saved yet.";
      return false;
    }
    if (iota::mcp::isVizExtension(target) && !allowVizOverwrite && iota::core::pathExistsNoThrow(target)) {
      if (outError) {
        *outError =
            "Refusing to overwrite legacy .viz template '" + iota::core::pathToUtf8(target) +
            "' without allow_viz_overwrite=true. Save to a .iota path instead.";
      }
      return false;
    }

    app_.project_.metadata.previewWidth = app_.previewWidth_;
    app_.project_.metadata.previewHeight = app_.previewHeight_;
    app_.project_.metadata.targetFps = std::clamp(app_.exportCfg_.fps, 15, 120);

    std::string err;
    if (!iota::preset_io::saveProjectAutoFormat(app_.project_, target, &err)) {
      if (outError) *outError = err;
      return false;
    }
    app_.currentProjectPath_ = target;
    app_.projectDirty_ = false;
    app_.recordProjectInLibrary(target);
    app_.updateWindowTitle();
    app_.statusBanner_ = "MCP Saved " + iota::core::pathFilenameUtf8(target);
    if (outSavedPath) {
      *outSavedPath = target;
    }
    return true;
  }

  void commitEdit(const std::string& label,
                  bool invalidateAssets,
                  bool resetSim) override {
    app_.clampSelectionToProject();
    if (invalidateAssets) {
      app_.renderer_.invalidateLocalAssets();
    }
    if (resetSim) {
      app_.renderer_.resetSimulation();
    }
    app_.pushUndoState(label.empty() ? "MCP Edit" : label);
    app_.statusBanner_ = label.empty() ? "MCP Edit" : label;
  }

  bool canUndo() const noexcept override { return app_.undoStack_.canUndo(); }
  bool canRedo() const noexcept override { return app_.undoStack_.canRedo(); }

  bool undo(std::string* outLabel) override {
    if (!app_.undoStack_.canUndo()) {
      return false;
    }
    const std::string desc = app_.undoStack_.undoDescription();
    app_.actionUndo();
    if (outLabel) *outLabel = desc;
    return true;
  }

  bool redo(std::string* outLabel) override {
    if (!app_.undoStack_.canRedo()) {
      return false;
    }
    const std::string desc = app_.undoStack_.redoDescription();
    app_.actionRedo();
    if (outLabel) *outLabel = desc;
    return true;
  }

  bool renderPreviewPng(int width,
                        int height,
                        double timeSec,
                        int preRollFrames,
                        bool fxaa,
                        std::vector<uint8_t>& outPng,
                        iota::engine::RenderFrameStats& outStats,
                        std::string* outError) override {
    const int w = std::clamp(width > 0 ? width : app_.previewWidth_, 64, 4096);
    const int h = std::clamp(height > 0 ? height : app_.previewHeight_, 64, 4096);
    const int roll = std::clamp(preRollFrames, 0, 120);
    const double tTarget = std::max(0.0, timeSec);
    const float dt = 1.0f / static_cast<float>(std::clamp(app_.exportCfg_.fps, 15, 120));

    app_.renderer_.resetSimulation();
    iota::rhi::RenderTargetHandle finalRt{};
    for (int i = -roll; i <= 0; ++i) {
      const double tFrame = std::max(0.0, tTarget + static_cast<double>(i) * dt);
      iota::engine::FrameTiming timing;
      timing.dt = dt;
      timing.totalTimeSec = tFrame;
      timing.playAccumTimeSec = tFrame;
      timing.isExporting = true;
      timing.visOffsetMs = app_.visOffsetMs_;
      timing.fxaaEnabled = fxaa;
      timing.fpsStat = static_cast<float>(std::clamp(app_.exportCfg_.fps, 15, 120));

      finalRt = app_.renderer_.renderFrame(app_.project_, app_.audio_, timing, w, h, {}, &outStats);
      if (!finalRt) {
        if (outError) *outError = "SceneRenderer returned null render target.";
        return false;
      }
    }

    std::vector<uint8_t> rgba;
    int rw = 0;
    int rh = 0;
    if (!app_.device_.readRenderTargetRgba(finalRt, rw, rh, rgba, true) || rw != w || rh != h) {
      if (outError) *outError = "Failed to read rendered preview framebuffer.";
      return false;
    }

    outPng = iota::mcp::encodeRgbaToPng(rw, rh, rgba);
    if (outPng.empty()) {
      if (outError) *outError = "PNG compression failed.";
      return false;
    }
    return true;
  }

  bool exportVideo(const iota::export_video::ExportSettings& settings,
                   std::string* outError,
                   std::string* outEncoderUsed,
                   bool* outUsedFallback,
                   std::string* outFallbackNotice) override {
    iota::export_video::ExportSettings effective = settings;
    effective.visOffsetMs = app_.visOffsetMs_;
    iota::export_video::VideoExportSession session;
    if (!session.begin(effective, app_.audio_, outError)) {
      return false;
    }
    if (outEncoderUsed) *outEncoderUsed = session.activeEncoderDisplayName();
    if (outUsedFallback) *outUsedFallback = session.usedCpuFallback();
    if (outFallbackNotice) *outFallbackNotice = session.fallbackNotice();

    app_.renderer_.resetSimulation();
    while (!session.isDone()) {
      if (!session.stepFrame(app_.device_, app_.renderer_, app_.project_, app_.audio_, outError)) {
        session.cancel();
        return false;
      }
    }
    const bool fin = session.finish(outError);
    app_.renderer_.resetSimulation();
    return fin;
  }

  bool controlPlayback(std::string_view action,
                       std::optional<double> seekSec,
                       std::string* outError) override {
    if (seekSec.has_value()) {
      const double clamped = std::max(0.0, *seekSec);
      app_.audio_.seekSeconds(clamped);
      app_.playAccumTimeSec_ = clamped;
    }
    if (action == "play") {
      app_.audio_.play();
    } else if (action == "pause") {
      app_.audio_.pause();
    } else if (action == "stop") {
      app_.audio_.stop();
      app_.playAccumTimeSec_ = 0.0;
    } else if (action == "toggle") {
      if (app_.audio_.isPlaying()) app_.audio_.pause();
      else app_.audio_.play();
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

  int previewWidth() const noexcept override { return app_.previewWidth_; }
  int previewHeight() const noexcept override { return app_.previewHeight_; }
  int targetFps() const noexcept override { return std::clamp(app_.exportCfg_.fps, 15, 120); }

  void setPreviewMetrics(int width, int height, int fps) override {
    if (width > 0) {
      app_.previewWidth_ = std::clamp(width, 64, 4096);
      app_.project_.metadata.previewWidth = app_.previewWidth_;
    }
    if (height > 0) {
      app_.previewHeight_ = std::clamp(height, 64, 4096);
      app_.project_.metadata.previewHeight = app_.previewHeight_;
    }
    if (fps > 0) {
      app_.exportCfg_.fps = std::clamp(fps, 15, 120);
      app_.project_.metadata.targetFps = app_.exportCfg_.fps;
    }
  }

 private:
  EditorApp& app_;
};

bool EditorApp::enableMcpLiveBridge() {
  if (mcpBridgeHost_.isListening()) {
    return true;
  }

  if (!currentProjectPath_.empty() && !iota::mcp::isMcpWorkingCopyPath(currentProjectPath_)) {
    const std::filesystem::path copyPath = iota::mcp::computeMcpWorkingCopyPath(currentProjectPath_);
    project_.metadata.previewWidth = previewWidth_;
    project_.metadata.previewHeight = previewHeight_;
    project_.metadata.targetFps = std::clamp(exportCfg_.fps, 15, 120);

    std::string saveErr;
    if (!iota::preset_io::saveProjectAutoFormat(project_, copyPath, &saveErr)) {
      statusBanner_ = "Failed to create MCP working copy: " + saveErr;
      return false;
    }
    mcpOriginalProjectPath_ = currentProjectPath_;
    currentProjectPath_ = copyPath;
    projectDirty_ = false;
    recordProjectInLibrary(copyPath);
  }

  pushUndoState("Before MCP Live Bridge");

  if (!mcpBridgeHost_.start()) {
    statusBanner_ = "Failed to start MCP Live Bridge listener.";
    return false;
  }

  updateWindowTitle();
  if (!currentProjectPath_.empty()) {
    statusBanner_ = "MCP Live Bridge enabled (" + iota::core::pathFilenameUtf8(currentProjectPath_) + ")";
  } else {
    statusBanner_ = "MCP Live Bridge enabled";
  }
  return true;
}

void EditorApp::disableMcpLiveBridge(std::string_view reason) {
  mcpBridgeHost_.stop();
  mcpOriginalProjectPath_.clear();
  updateWindowTitle();
  if (!reason.empty()) {
    statusBanner_ = std::string(reason);
  }
}

void EditorApp::pollMcpLiveBridge() {
  if (!mcpBridgeHost_.isListening()) {
    return;
  }
  EditorLiveStudioTarget target(*this);
  const uint64_t handled = mcpBridgeHost_.requestsHandled();
  mcpBridgeHost_.pollAndDispatch(
      [&](const nlohmann::json& req) {
        return iota::mcp::executeTargetBridgeRequest(target, req, handled);
      },
      8);
}

#endif  // IOTA_ENABLE_MCP

}  // namespace iota::studio
