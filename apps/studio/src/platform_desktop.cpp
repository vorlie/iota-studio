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

#include "platform_desktop.hpp"

#include "iota/core/utf8_path.hpp"

#include "imgui.h"
#include "stb_image.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#else
#include <GLFW/glfw3.h>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef IOTA_HAS_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif
#endif

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>

namespace iota::studio::platform {

namespace {

std::vector<std::string> splitSemicolonTokens(std::string_view text) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t sep = text.find(';', pos);
    const std::string_view part =
        (sep == std::string_view::npos) ? text.substr(pos) : text.substr(pos, sep - pos);
    if (!part.empty()) {
      out.emplace_back(part);
    }
    if (sep == std::string_view::npos) {
      break;
    }
    pos = sep + 1;
  }
  return out;
}

#ifdef _WIN32

std::wstring utf8ToWide(std::string_view s) {
  if (s.empty()) return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
  return out;
}

std::wstring buildWin32FilterString(std::span<const FileDialogFilter> filters) {
  std::wstring out;
  for (const auto& f : filters) {
    out += utf8ToWide(f.label ? f.label : "");
    out.push_back(L'\0');
    const auto exts = splitSemicolonTokens(f.extensions ? f.extensions : "*");
    if (exts.empty() || (exts.size() == 1 && exts[0] == "*")) {
      out += L"*.*";
    } else {
      for (size_t i = 0; i < exts.size(); ++i) {
        if (i > 0) out.push_back(L';');
        out += L"*." + utf8ToWide(exts[i]);
      }
    }
    out.push_back(L'\0');
  }
  if (out.empty()) {
    out = L"All Files (*.*)\0*.*\0";
  }
  out.push_back(L'\0');
  return out;
}

#else

bool runDialogCommand(const std::vector<std::string>& args,
                      int& outExitCode,
                      std::string& outStdout) {
  outExitCode = 1;
  outStdout.clear();
  if (args.empty()) {
    return false;
  }

  int pipefd[2]{-1, -1};
  if (pipe(pipefd) != 0) {
    return false;
  }
  fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
  fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    const int devNull = open("/dev/null", O_RDWR);
    if (devNull >= 0) {
      dup2(devNull, STDIN_FILENO);
      dup2(devNull, STDERR_FILENO);
      if (devNull > STDERR_FILENO) {
        close(devNull);
      }
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) {
      argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }

  close(pipefd[1]);
  char buf[4096];
  for (;;) {
    const ssize_t n = read(pipefd[0], buf, sizeof(buf));
    if (n > 0) {
      outStdout.append(buf, static_cast<size_t>(n));
    } else if (n == 0) {
      break;
    } else if (errno != EINTR) {
      break;
    }
  }
  close(pipefd[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  outExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  return outExitCode != 127;
}

std::string buildZenityPattern(const FileDialogFilter& f) {
  std::string pat = (f.label && *f.label) ? std::string(f.label) : "Files";
  pat += " |";
  const auto exts = splitSemicolonTokens(f.extensions ? f.extensions : "*");
  if (exts.empty() || (exts.size() == 1 && exts[0] == "*")) {
    pat += " *";
  } else {
    for (const auto& ext : exts) {
      pat += " *." + ext;
    }
  }
  return pat;
}

std::string buildKdialogFilter(std::span<const FileDialogFilter> filters) {
  std::string out;
  for (size_t i = 0; i < filters.size(); ++i) {
    if (i > 0) out += "\n";
    const auto exts = splitSemicolonTokens(filters[i].extensions ? filters[i].extensions : "*");
    if (exts.empty() || (exts.size() == 1 && exts[0] == "*")) {
      out += "*|All Files";
    } else {
      for (size_t j = 0; j < exts.size(); ++j) {
        if (j > 0) out += " ";
        out += "*." + exts[j];
      }
      out += "|";
      out += (filters[i].label && *filters[i].label) ? filters[i].label : "Supported Files";
    }
  }
  return out;
}

std::vector<std::string> splitLinesTrimmed(const std::string& raw) {
  std::vector<std::string> lines;
  std::istringstream iss(raw);
  std::string line;
  while (std::getline(iss, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

std::filesystem::path findBundledAssetPath(std::string_view relPath) {
  std::error_code ec;
  char exeBuf[PATH_MAX]{};
  const ssize_t n = readlink("/proc/self/exe", exeBuf, sizeof(exeBuf) - 1);
  if (n > 0) {
    exeBuf[n] = '\0';
    const std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
    const std::filesystem::path prefixAsset = (exeDir / "../share/iota-studio" / relPath).lexically_normal();
    if (iota::core::pathExistsNoThrow(prefixAsset)) {
      return prefixAsset;
    }
    std::filesystem::path dir = exeDir;
    for (int depth = 0; depth < 5 && !dir.empty(); ++depth) {
      const auto cand = dir / relPath;
      if (iota::core::pathExistsNoThrow(cand)) {
        return cand;
      }
      if (!dir.has_parent_path() || dir == dir.parent_path()) break;
      dir = dir.parent_path();
    }
  }
  const std::filesystem::path systemAsset = std::filesystem::path("/usr/share/iota-studio") / relPath;
  if (iota::core::pathExistsNoThrow(systemAsset)) {
    return systemAsset;
  }
  const auto cwdCand = std::filesystem::current_path(ec) / relPath;
  if (!ec && iota::core::pathExistsNoThrow(cwdCand)) {
    return cwdCand;
  }
  return {};
}

#ifdef IOTA_HAS_FONTCONFIG
std::string queryFontconfigFile(FcConfig* cfg, const char* patternStr) {
  if (!cfg || !patternStr) return {};
  FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(patternStr));
  if (!pat) return {};
  FcConfigSubstitute(cfg, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult res = FcResultMatch;
  FcPattern* match = FcFontMatch(cfg, pat, &res);
  FcPatternDestroy(pat);
  if (!match) return {};
  FcChar8* file = nullptr;
  std::string out;
  if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
    out = reinterpret_cast<const char*>(file);
  }
  FcPatternDestroy(match);
  return out;
}
#endif

#endif

}  // namespace

std::optional<std::filesystem::path> showOpenFileDialog(GLFWwindow* owner,
                                                        const char* title,
                                                        std::span<const FileDialogFilter> filters) {
#ifdef _WIN32
  std::vector<wchar_t> fileBuf(4096, L'\0');
  const std::wstring wTitle = utf8ToWide(title ? title : "Open File");
  const std::wstring wFilter = buildWin32FilterString(filters);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner ? glfwGetWin32Window(owner) : nullptr;
  ofn.lpstrFilter = wFilter.c_str();
  ofn.lpstrFile = fileBuf.data();
  ofn.nMaxFile = static_cast<DWORD>(fileBuf.size());
  ofn.lpstrTitle = wTitle.c_str();
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetOpenFileNameW(&ofn)) {
    return std::filesystem::path(fileBuf.data());
  }
  return std::nullopt;
#else
  (void)owner;
  std::vector<std::string> args = {
      "zenity",
      "--file-selection",
      std::string("--title=") + (title ? title : "Open File"),
  };
  for (const auto& f : filters) {
    args.push_back("--file-filter=" + buildZenityPattern(f));
  }
  int rc = 1;
  std::string out;
  if (runDialogCommand(args, rc, out)) {
    if (rc == 0) {
      const auto lines = splitLinesTrimmed(out);
      if (!lines.empty()) {
        return iota::core::pathFromUtf8(lines.front());
      }
    }
    return std::nullopt;
  }

  std::vector<std::string> kdArgs = {
      "kdialog",
      "--getopenfilename",
      ".",
      buildKdialogFilter(filters),
  };
  if (title && *title) {
    kdArgs.push_back("--title");
    kdArgs.push_back(title);
  }
  if (runDialogCommand(kdArgs, rc, out) && rc == 0) {
    const auto lines = splitLinesTrimmed(out);
    if (!lines.empty()) {
      return iota::core::pathFromUtf8(lines.front());
    }
  }
  return std::nullopt;
#endif
}

std::vector<std::filesystem::path> showOpenFilesDialog(GLFWwindow* owner,
                                                       const char* title,
                                                       std::span<const FileDialogFilter> filters) {
#ifdef _WIN32
  std::vector<wchar_t> buf(32768, L'\0');
  const std::wstring wTitle = utf8ToWide(title ? title : "Open Files");
  const std::wstring wFilter = buildWin32FilterString(filters);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner ? glfwGetWin32Window(owner) : nullptr;
  ofn.lpstrFilter = wFilter.c_str();
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = static_cast<DWORD>(buf.size());
  ofn.lpstrTitle = wTitle.c_str();
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
#else
  (void)owner;
  std::vector<std::string> args = {
      "zenity",
      "--file-selection",
      "--multiple",
      "--separator=\n",
      std::string("--title=") + (title ? title : "Open Files"),
  };
  for (const auto& f : filters) {
    args.push_back("--file-filter=" + buildZenityPattern(f));
  }
  int rc = 1;
  std::string out;
  std::vector<std::filesystem::path> result;
  if (runDialogCommand(args, rc, out)) {
    if (rc == 0) {
      for (const auto& line : splitLinesTrimmed(out)) {
        result.push_back(iota::core::pathFromUtf8(line));
      }
    }
    return result;
  }

  std::vector<std::string> kdArgs = {
      "kdialog",
      "--getopenfilename",
      ".",
      buildKdialogFilter(filters),
      "--multiple",
      "--separate-output",
  };
  if (title && *title) {
    kdArgs.push_back("--title");
    kdArgs.push_back(title);
  }
  if (runDialogCommand(kdArgs, rc, out) && rc == 0) {
    for (const auto& line : splitLinesTrimmed(out)) {
      result.push_back(iota::core::pathFromUtf8(line));
    }
  }
  return result;
#endif
}

std::optional<std::filesystem::path> showSaveFileDialog(GLFWwindow* owner,
                                                        const char* title,
                                                        std::span<const FileDialogFilter> filters,
                                                        const char* defaultExt,
                                                        const char* initialName) {
#ifdef _WIN32
  std::vector<wchar_t> fileBuf(4096, L'\0');
  if (initialName && *initialName) {
    const std::wstring wInit = utf8ToWide(initialName);
    wcsncpy_s(fileBuf.data(), fileBuf.size(), wInit.c_str(), _TRUNCATE);
  }
  const std::wstring wTitle = utf8ToWide(title ? title : "Save File");
  const std::wstring wFilter = buildWin32FilterString(filters);
  const std::wstring wDefExt = utf8ToWide(defaultExt ? defaultExt : "");
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner ? glfwGetWin32Window(owner) : nullptr;
  ofn.lpstrFilter = wFilter.c_str();
  ofn.lpstrFile = fileBuf.data();
  ofn.nMaxFile = static_cast<DWORD>(fileBuf.size());
  ofn.lpstrTitle = wTitle.c_str();
  ofn.lpstrDefExt = wDefExt.empty() ? nullptr : wDefExt.c_str();
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetSaveFileNameW(&ofn)) {
    return std::filesystem::path(fileBuf.data());
  }
  return std::nullopt;
#else
  (void)owner;
  std::vector<std::string> args = {
      "zenity",
      "--file-selection",
      "--save",
      "--confirm-overwrite",
      std::string("--title=") + (title ? title : "Save File"),
  };
  if (initialName && *initialName) {
    args.push_back(std::string("--filename=") + initialName);
  }
  for (const auto& f : filters) {
    args.push_back("--file-filter=" + buildZenityPattern(f));
  }
  int rc = 1;
  std::string out;
  auto ensureDefaultExt = [&](std::filesystem::path p) {
    if (defaultExt && *defaultExt && !p.has_extension()) {
      p += std::string(".") + defaultExt;
    }
    return p;
  };
  if (runDialogCommand(args, rc, out)) {
    if (rc == 0) {
      const auto lines = splitLinesTrimmed(out);
      if (!lines.empty()) {
        return ensureDefaultExt(iota::core::pathFromUtf8(lines.front()));
      }
    }
    return std::nullopt;
  }

  std::vector<std::string> kdArgs = {
      "kdialog",
      "--getsavefilename",
      (initialName && *initialName) ? initialName : ".",
      buildKdialogFilter(filters),
  };
  if (title && *title) {
    kdArgs.push_back("--title");
    kdArgs.push_back(title);
  }
  if (runDialogCommand(kdArgs, rc, out) && rc == 0) {
    const auto lines = splitLinesTrimmed(out);
    if (!lines.empty()) {
      return ensureDefaultExt(iota::core::pathFromUtf8(lines.front()));
    }
  }
  return std::nullopt;
#endif
}

bool openExternalUrl(GLFWwindow* owner, const std::string& url) {
#ifdef _WIN32
  const std::wstring wurl = utf8ToWide(url);
  if (wurl.empty()) {
    return false;
  }
  HWND hwnd = owner ? glfwGetWin32Window(owner) : nullptr;
  const INT_PTR rc = reinterpret_cast<INT_PTR>(
      ShellExecuteW(hwnd, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return rc > 32;
#else
  (void)owner;
  const pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    const pid_t grand = fork();
    if (grand < 0) {
      _exit(1);
    }
    if (grand > 0) {
      _exit(0);
    }
    setsid();
    const int devNull = open("/dev/null", O_RDWR);
    if (devNull >= 0) {
      dup2(devNull, STDIN_FILENO);
      dup2(devNull, STDOUT_FILENO);
      dup2(devNull, STDERR_FILENO);
      if (devNull > STDERR_FILENO) {
        close(devNull);
      }
    }
    execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

void applyWindowHintsBeforeCreate() {
  glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#if !defined(_WIN32) && defined(GLFW_WAYLAND_APP_ID)
  glfwWindowHintString(GLFW_WAYLAND_APP_ID, "iota-studio");
#endif
}

void applyWindowIcon(GLFWwindow* window, void*& outBigHandle, void*& outSmallHandle) {
  outBigHandle = nullptr;
  outSmallHandle = nullptr;
  if (!window) return;

#ifdef _WIN32
  if (HWND hwnd = glfwGetWin32Window(window)) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const int bigW = GetSystemMetrics(SM_CXICON);
    const int bigH = GetSystemMetrics(SM_CYICON);
    const int smallW = GetSystemMetrics(SM_CXSMICON);
    const int smallH = GetSystemMetrics(SM_CYSMICON);
    outBigHandle = LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, bigW, bigH, LR_DEFAULTCOLOR);
    outSmallHandle = LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, smallW, smallH, LR_DEFAULTCOLOR);
    if (outBigHandle) {
      SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(outBigHandle));
    }
    if (outSmallHandle) {
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(outSmallHandle));
    }
  }
#else
  // Wayland does not allow clients to set window icons dynamically; X11 does.
  if (glfwGetPlatform() != GLFW_PLATFORM_X11) {
    return;
  }
  const std::filesystem::path iconPath = findBundledAssetPath("assets/icon.png");
  if (iconPath.empty()) {
    return;
  }
  std::ifstream in(iconPath, std::ios::binary | std::ios::ate);
  if (!in.is_open()) return;
  const auto sz = in.tellg();
  if (sz <= 0) return;
  std::vector<unsigned char> pngBytes(static_cast<size_t>(sz));
  in.seekg(0, std::ios::beg);
  if (!in.read(reinterpret_cast<char*>(pngBytes.data()), sz)) return;

  int w = 0, h = 0, comp = 0;
  unsigned char* pixels = stbi_load_from_memory(
      pngBytes.data(), static_cast<int>(pngBytes.size()), &w, &h, &comp, 4);
  if (pixels != nullptr && w > 0 && h > 0) {
    GLFWimage img{};
    img.width = w;
    img.height = h;
    img.pixels = pixels;
    glfwSetWindowIcon(window, 1, &img);
  }
  if (pixels != nullptr) {
    stbi_image_free(pixels);
  }
#endif
}

void destroyWindowIcons(void*& bigHandle, void*& smallHandle) {
#ifdef _WIN32
  if (bigHandle) {
    DestroyIcon(static_cast<HICON>(bigHandle));
    bigHandle = nullptr;
  }
  if (smallHandle) {
    DestroyIcon(static_cast<HICON>(smallHandle));
    smallHandle = nullptr;
  }
#else
  bigHandle = nullptr;
  smallHandle = nullptr;
#endif
}

void loadPlatformImGuiFonts(ImGuiIO& io, ImFont*& outCodeFont) {
  outCodeFont = nullptr;

#ifdef _WIN32
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
  for (const char* monoName : {"consola.ttf", "CascadiaMono.ttf", "lucon.ttf"}) {
    const auto monoPath = winFonts / monoName;
    if (iota::core::pathExistsNoThrow(monoPath)) {
      outCodeFont = io.Fonts->AddFontFromFileTTF(iota::core::pathToUtf8(monoPath).c_str(), 15.0f);
      if (outCodeFont != nullptr) break;
    }
  }
#else
  std::vector<std::string> uiCandidates;
  std::vector<std::string> extraCandidates;
  std::vector<std::string> monoCandidates;

#ifdef IOTA_HAS_FONTCONFIG
  if (FcConfig* cfg = FcInitLoadConfigAndFonts()) {
    if (auto p = queryFontconfigFile(cfg, "sans-serif:weight=regular"); !p.empty()) {
      uiCandidates.push_back(std::move(p));
    }
    for (const char* pat : {"sans-serif:lang=ja", "sans-serif:lang=ko", "sans-serif:lang=zh-cn"}) {
      if (auto p = queryFontconfigFile(cfg, pat); !p.empty()) {
        extraCandidates.push_back(std::move(p));
      }
    }
    if (auto p = queryFontconfigFile(cfg, "monospace:weight=regular"); !p.empty()) {
      monoCandidates.push_back(std::move(p));
    }
    FcConfigDestroy(cfg);
  }
#endif

  for (const char* p : {
           "/usr/share/fonts/noto/NotoSans-Regular.ttf",
           "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
           "/usr/share/fonts/TTF/DejaVuSans.ttf",
           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
           "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
           "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
       }) {
    uiCandidates.emplace_back(p);
  }

  for (const char* p : {
           "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
           "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
           "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
           "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
           "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
       }) {
    monoCandidates.emplace_back(p);
  }

  std::string loadedPrimary;
  for (const auto& cand : uiCandidates) {
    if (iota::core::pathExistsNoThrow(iota::core::pathFromUtf8(cand)) &&
        io.Fonts->AddFontFromFileTTF(cand.c_str(), 16.0f) != nullptr) {
      loadedPrimary = cand;
      break;
    }
  }

  if (!loadedPrimary.empty()) {
    ImFontConfig mergeCfg;
    mergeCfg.MergeMode = true;
    std::vector<std::string> mergedPaths = {loadedPrimary};
    for (const auto& cand : extraCandidates) {
      if (std::find(mergedPaths.begin(), mergedPaths.end(), cand) != mergedPaths.end()) {
        continue;
      }
      if (iota::core::pathExistsNoThrow(iota::core::pathFromUtf8(cand))) {
        io.Fonts->AddFontFromFileTTF(cand.c_str(), 16.0f, &mergeCfg);
        mergedPaths.push_back(cand);
      }
    }
  }

  for (const auto& cand : monoCandidates) {
    if (iota::core::pathExistsNoThrow(iota::core::pathFromUtf8(cand))) {
      outCodeFont = io.Fonts->AddFontFromFileTTF(cand.c_str(), 15.0f);
      if (outCodeFont != nullptr) break;
    }
  }
#endif
}

std::string defaultLayoutIniPath() {
#ifdef _WIN32
  return "iota_studio_layout.ini";
#else
  std::filesystem::path baseDir;
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
    baseDir = iota::core::pathFromUtf8(xdg);
  } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    baseDir = iota::core::pathFromUtf8(home) / ".config";
  }
  if (!baseDir.empty()) {
    const std::filesystem::path appDir = baseDir / "IotaStudio";
    std::error_code ec;
    std::filesystem::create_directories(appDir, ec);
    return iota::core::pathToUtf8(appDir / "iota_studio_layout.ini");
  }
  return "iota_studio_layout.ini";
#endif
}

}  // namespace iota::studio::platform
