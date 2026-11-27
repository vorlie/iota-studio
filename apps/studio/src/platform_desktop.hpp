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

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct GLFWwindow;
struct ImFont;
struct ImGuiIO;

namespace iota::studio::platform {

struct FileDialogFilter {
  const char* label = "";
  const char* extensions = "*";  // Semicolon-separated without dots, e.g. "png;jpg" or "*"
};

struct DecodedAudioPayload {
  std::string filename;
  int sampleRate = 44100;
  std::vector<float> interleavedPcm;
};

std::optional<std::filesystem::path> showOpenFileDialog(GLFWwindow* owner,
                                                        const char* title,
                                                        std::span<const FileDialogFilter> filters);

std::vector<std::filesystem::path> showOpenFilesDialog(GLFWwindow* owner,
                                                       const char* title,
                                                       std::span<const FileDialogFilter> filters);

std::optional<std::filesystem::path> showSaveFileDialog(GLFWwindow* owner,
                                                        const char* title,
                                                        std::span<const FileDialogFilter> filters,
                                                        const char* defaultExt,
                                                        const char* initialName);

void requestOpenFile(GLFWwindow* owner,
                     const char* title,
                     std::span<const FileDialogFilter> filters,
                     std::function<void(std::filesystem::path)> onPicked);

void requestOpenFiles(GLFWwindow* owner,
                      const char* title,
                      std::span<const FileDialogFilter> filters,
                      std::function<void(std::vector<std::filesystem::path>)> onPicked);

void requestOpenAudioFile(GLFWwindow* owner,
                          const char* title,
                          std::span<const FileDialogFilter> filters,
                          std::function<void(std::filesystem::path)> onPathPicked,
                          std::function<void(DecodedAudioPayload)> onDecodedPcm,
                          std::function<void(std::string)> onError);

void triggerWebFileDownload(const std::filesystem::path& path, const std::string& downloadName);
void syncPersistentStorage();
void pollPlatformEvents();
void setWebDropHandlers(std::function<void(std::filesystem::path)> onFileDropped,
                        std::function<void(DecodedAudioPayload)> onAudioDropped,
                        std::function<void(std::string)> onDropError);
void queryWebCanvasSize(int& outWidth, int& outHeight);
void resumeWebAudioContext();

bool openExternalUrl(GLFWwindow* owner, const std::string& url);

void applyWindowHintsBeforeCreate();
void applyWindowIcon(GLFWwindow* window, void*& outBigHandle, void*& outSmallHandle);
void destroyWindowIcons(void*& bigHandle, void*& smallHandle);

void loadPlatformImGuiFonts(ImGuiIO& io, ImFont*& outCodeFont);

std::string defaultLayoutIniPath();

/// Wayland wl_surface.frame vblank pacer with bounded wait so compositor pings stay responsive.
class WaylandFramePacer {
 public:
  WaylandFramePacer();
  ~WaylandFramePacer();

  WaylandFramePacer(const WaylandFramePacer&) = delete;
  WaylandFramePacer& operator=(const WaylandFramePacer&) = delete;

  bool attach(GLFWwindow* window);
  void detach();

  [[nodiscard]] bool isActive() const noexcept;
  [[nodiscard]] bool isWaitingForCompositor() const noexcept;

  void cancelPendingFrame();
  void armBeforeSwap();
  bool waitForFrameOrTimeout(int maxWaitMs = 50);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace iota::studio::platform
