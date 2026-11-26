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

bool openExternalUrl(GLFWwindow* owner, const std::string& url);

void applyWindowHintsBeforeCreate();
void applyWindowIcon(GLFWwindow* window, void*& outBigHandle, void*& outSmallHandle);
void destroyWindowIcons(void*& bigHandle, void*& smallHandle);

void loadPlatformImGuiFonts(ImGuiIO& io, ImFont*& outCodeFont);

std::string defaultLayoutIniPath();

}  // namespace iota::studio::platform
