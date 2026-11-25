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

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace iota::core {

struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::string prerelease;
};

/// Parses a semantic version string such as "1.3.1", "v1.3.1", or "v1.4.0-rc.1".
[[nodiscard]] std::optional<SemVer> parseSemVer(std::string_view text);

/// Compares two SemVer values according to SemVer 2.0.0 precedence (-1, 0, +1).
[[nodiscard]] int compareSemVer(const SemVer& a, const SemVer& b);

/// Returns true iff `candidate` parses cleanly and is strictly newer than `current`.
[[nodiscard]] bool isNewerVersion(std::string_view current, std::string_view candidate);

/// Validates that a URL uses HTTPS and contains no unsafe control/shell characters.
[[nodiscard]] bool isSafeHttpsUrl(std::string_view url);

struct ReleaseInfo {
  std::string tagName;
  std::string version;
  std::string title;
  std::string htmlUrl;
  std::string publishedAt;
  std::string releaseNotes;
  std::string assetName;
  std::string assetDownloadUrl;
  uint64_t assetSizeBytes = 0;
  bool isPrerelease = false;
  bool isDraft = false;
  bool updateAvailable = false;
};

/// Parses a GitHub `/releases/latest` JSON payload and compares against `currentVersion`.
[[nodiscard]] std::optional<ReleaseInfo> parseGitHubReleaseJson(
    std::string_view jsonBody,
    std::string_view currentVersion,
    std::string_view fallbackReleasesUrl = "https://github.com/vorlie/iota-studio/releases",
    std::string* outError = nullptr,
    bool preferMcpAsset = false);

enum class UpdateCheckStatus {
  Idle,
  Checking,
  UpToDate,
  UpdateAvailable,
  Error,
};

struct UpdateCheckSnapshot {
  UpdateCheckStatus status = UpdateCheckStatus::Idle;
  std::string currentVersion;
  ReleaseInfo latestRelease;
  std::string errorMessage;
  bool isManualCheck = false;
};

/// Non-blocking asynchronous GitHub Releases update checker backed by WinHTTP.
class UpdateChecker {
 public:
  static constexpr const char* kDefaultRepoOwner = "vorlie";
  static constexpr const char* kDefaultRepoName = "iota-studio";
  static constexpr const char* kReleasesPageUrl = "https://github.com/vorlie/iota-studio/releases";

  UpdateChecker() = default;
  ~UpdateChecker();

  UpdateChecker(const UpdateChecker&) = delete;
  UpdateChecker& operator=(const UpdateChecker&) = delete;

  /// Starts an asynchronous HTTPS check against GitHub Releases (no-op if already checking).
  void startCheckAsync(std::string currentVersion,
                       std::string owner = kDefaultRepoOwner,
                       std::string repo = kDefaultRepoName,
                       bool isManual = false,
                       bool preferMcpAsset = false);

  /// Thread-safe snapshot of the current update check state for UI polling.
  [[nodiscard]] UpdateCheckSnapshot snapshot() const;

  /// Marks any active request as cancelled without blocking the UI thread.
  void cancel();

 private:
  struct SharedState;
  mutable std::mutex statePtrMutex_;
  std::shared_ptr<SharedState> state_;
};

}  // namespace iota::core
