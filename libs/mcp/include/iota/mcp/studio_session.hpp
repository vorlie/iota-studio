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
#include "iota/export/video_exporter.hpp"
#include "iota/model/command_stack.hpp"
#include "iota/model/scene.hpp"
#include "iota/rhi/gl45_device.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct GLFWwindow;

namespace iota::mcp {

/// Abstract studio target shared by headless `HeadlessStudioSession` and live `EditorApp` bridge.
class IStudioBridgeTarget {
 public:
  virtual ~IStudioBridgeTarget() = default;

  [[nodiscard]] virtual bool isLiveStudio() const noexcept = 0;

  [[nodiscard]] virtual iota::model::Project& project() noexcept = 0;
  [[nodiscard]] virtual const iota::model::Project& project() const noexcept = 0;
  [[nodiscard]] virtual iota::audio::AudioEngine& audio() noexcept = 0;
  [[nodiscard]] virtual const iota::audio::AudioEngine& audio() const noexcept = 0;

  [[nodiscard]] virtual std::filesystem::path currentProjectPath() const = 0;

  virtual bool loadProject(const std::filesystem::path& path,
                           std::string* outError,
                           std::filesystem::path* outWorkingCopyPath = nullptr) = 0;

  virtual bool createNewProject(iota::model::Project project,
                                const std::string& undoLabel) = 0;

  virtual bool saveProject(const std::filesystem::path& path,
                           bool allowVizOverwrite,
                           std::string* outError,
                           std::filesystem::path* outSavedPath = nullptr) = 0;

  virtual void commitEdit(const std::string& label,
                          bool invalidateAssets = false,
                          bool resetSim = false) = 0;

  [[nodiscard]] virtual bool canUndo() const noexcept = 0;
  [[nodiscard]] virtual bool canRedo() const noexcept = 0;
  virtual bool undo(std::string* outLabel = nullptr) = 0;
  virtual bool redo(std::string* outLabel = nullptr) = 0;

  virtual bool renderPreviewPng(int width,
                                int height,
                                double timeSec,
                                int preRollFrames,
                                bool fxaa,
                                std::vector<uint8_t>& outPng,
                                iota::engine::RenderFrameStats& outStats,
                                std::string* outError) = 0;

  virtual bool exportVideo(const iota::export_video::ExportSettings& settings,
                           std::string* outError,
                           std::string* outEncoderUsed = nullptr,
                           bool* outUsedFallback = nullptr,
                           std::string* outFallbackNotice = nullptr) = 0;

  virtual bool controlPlayback(std::string_view action,
                               std::optional<double> seekSec,
                               std::string* outError) = 0;

  [[nodiscard]] virtual int previewWidth() const noexcept = 0;
  [[nodiscard]] virtual int previewHeight() const noexcept = 0;
  [[nodiscard]] virtual int targetFps() const noexcept = 0;
  virtual void setPreviewMetrics(int width, int height, int fps) = 0;
};

/// Standalone headless Studio session with lazy OpenGL 4.5 offscreen context initialization.
class HeadlessStudioSession final : public IStudioBridgeTarget {
 public:
  HeadlessStudioSession();
  ~HeadlessStudioSession() override;

  HeadlessStudioSession(const HeadlessStudioSession&) = delete;
  HeadlessStudioSession& operator=(const HeadlessStudioSession&) = delete;

  [[nodiscard]] bool isLiveStudio() const noexcept override { return false; }

  [[nodiscard]] iota::model::Project& project() noexcept override { return project_; }
  [[nodiscard]] const iota::model::Project& project() const noexcept override { return project_; }
  [[nodiscard]] iota::audio::AudioEngine& audio() noexcept override { return audio_; }
  [[nodiscard]] const iota::audio::AudioEngine& audio() const noexcept override { return audio_; }

  [[nodiscard]] std::filesystem::path currentProjectPath() const override { return currentPath_; }

  bool loadProject(const std::filesystem::path& path,
                   std::string* outError,
                   std::filesystem::path* outWorkingCopyPath = nullptr) override;

  bool createNewProject(iota::model::Project project,
                        const std::string& undoLabel) override;

  bool saveProject(const std::filesystem::path& path,
                   bool allowVizOverwrite,
                   std::string* outError,
                   std::filesystem::path* outSavedPath = nullptr) override;

  void commitEdit(const std::string& label,
                  bool invalidateAssets = false,
                  bool resetSim = false) override;

  [[nodiscard]] bool canUndo() const noexcept override { return undoStack_.canUndo(); }
  [[nodiscard]] bool canRedo() const noexcept override { return undoStack_.canRedo(); }
  bool undo(std::string* outLabel = nullptr) override;
  bool redo(std::string* outLabel = nullptr) override;

  bool renderPreviewPng(int width,
                        int height,
                        double timeSec,
                        int preRollFrames,
                        bool fxaa,
                        std::vector<uint8_t>& outPng,
                        iota::engine::RenderFrameStats& outStats,
                        std::string* outError) override;

  bool exportVideo(const iota::export_video::ExportSettings& settings,
                   std::string* outError,
                   std::string* outEncoderUsed = nullptr,
                   bool* outUsedFallback = nullptr,
                   std::string* outFallbackNotice = nullptr) override;

  bool controlPlayback(std::string_view action,
                       std::optional<double> seekSec,
                       std::string* outError) override;

  [[nodiscard]] int previewWidth() const noexcept override { return previewWidth_; }
  [[nodiscard]] int previewHeight() const noexcept override { return previewHeight_; }
  [[nodiscard]] int targetFps() const noexcept override { return targetFps_; }
  void setPreviewMetrics(int width, int height, int fps) override;

  /// Releases lazy OpenGL/GLFW context early (useful in multi-test runners).
  void releaseGraphicsContext();

 private:
  bool ensureGlReady(std::string* outError);

  iota::model::Project project_;
  iota::model::CommandStack undoStack_;
  iota::audio::AudioEngine audio_;
  std::filesystem::path currentPath_;
  int previewWidth_ = 1280;
  int previewHeight_ = 720;
  int targetFps_ = 60;

  GLFWwindow* glWindow_ = nullptr;
  bool glReady_ = false;
  bool glfwTerminatesOnDestroy_ = true;
  iota::rhi::Gl45RenderDevice device_;
  iota::engine::SceneRenderer renderer_;
};

}  // namespace iota::mcp
