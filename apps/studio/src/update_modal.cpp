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

#include "update_modal.hpp"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace iota::studio {

namespace {

std::string formatPublishedDate(const std::string& isoTimestamp) {
  if (isoTimestamp.size() >= 10) {
    return isoTimestamp.substr(0, 10);
  }
  return isoTimestamp;
}

std::string formatAssetSize(uint64_t bytes) {
  if (bytes == 0) {
    return {};
  }
  char buf[32]{};
  const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
  if (mb >= 1.0) {
    std::snprintf(buf, sizeof(buf), "%.2f MB", mb);
  } else {
    const double kb = static_cast<double>(bytes) / 1024.0;
    std::snprintf(buf, sizeof(buf), "%.0f KB", kb);
  }
  return buf;
}

}  // namespace

void renderUpdateModal(bool& openRequested,
                       const iota::core::UpdateCheckSnapshot& snap,
                       UpdateModalActions& actions) {
  if (openRequested) {
    ImGui::OpenPopup("Check for Updates");
    openRequested = false;
  }

  ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
  bool modalOpen = true;
  if (!ImGui::BeginPopupModal("Check for Updates", &modalOpen)) {
    return;
  }

  const std::string curVer = snap.currentVersion.empty() ? std::string(IOTA_STUDIO_VERSION) : snap.currentVersion;
  ImGui::Text("Installed version: v%s", curVer.c_str());
  ImGui::Separator();

  using Status = iota::core::UpdateCheckStatus;
  if (snap.status == Status::Checking) {
    const int dots = static_cast<int>(ImGui::GetTime() * 2.5) % 4;
    std::string dotStr(static_cast<size_t>(dots), '.');
    ImGui::Text("Checking GitHub for updates%s", dotStr.c_str());
    ImGui::TextDisabled("Querying %s/%s releases on api.github.com",
                        iota::core::UpdateChecker::kDefaultRepoOwner,
                        iota::core::UpdateChecker::kDefaultRepoName);
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    if (ImGui::Button("Open Releases Page")) {
      actions.requestOpenUrl = true;
      actions.urlToOpen = iota::core::UpdateChecker::kReleasesPageUrl;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
    }
  } else if (snap.status == Status::UpdateAvailable) {
    const auto& rel = snap.latestRelease;
    ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f),
                       "Update available: %s",
                       rel.tagName.c_str());
    const std::string dateStr = formatPublishedDate(rel.publishedAt);
    if (!rel.title.empty() && !dateStr.empty()) {
      ImGui::TextDisabled("%s  |  Released %s", rel.title.c_str(), dateStr.c_str());
    } else if (!rel.title.empty()) {
      ImGui::TextDisabled("%s", rel.title.c_str());
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Release Notes:");
    if (ImGui::BeginChild("##update_notes_scroll", ImVec2(0.0f, -68.0f), ImGuiChildFlags_Borders)) {
      if (rel.releaseNotes.empty()) {
        ImGui::TextDisabled("(No release notes provided.)");
      } else {
        ImGui::TextWrapped("%s", rel.releaseNotes.c_str());
      }
    }
    ImGui::EndChild();

    ImGui::TextDisabled("%s", rel.htmlUrl.c_str());

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.52f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.65f, 0.36f, 1.0f));
    if (ImGui::Button("Open Release Page")) {
      actions.requestOpenUrl = true;
      actions.urlToOpen = rel.htmlUrl;
    }
    ImGui::PopStyleColor(2);

    if (!rel.assetDownloadUrl.empty()) {
      ImGui::SameLine();
      const std::string szStr = formatAssetSize(rel.assetSizeBytes);
      const std::string dlLabel = szStr.empty()
                                      ? ("Download " + (rel.assetName.empty() ? std::string("Asset") : rel.assetName))
                                      : ("Download " + (rel.assetName.empty() ? std::string("Asset") : rel.assetName) +
                                         " (" + szStr + ")");
      if (ImGui::Button(dlLabel.c_str())) {
        actions.requestOpenUrl = true;
        actions.urlToOpen = rel.assetDownloadUrl;
      }
    }

    ImGui::SameLine();
    if (ImGui::Button("Copy Release Link")) {
      ImGui::SetClipboardText(rel.htmlUrl.c_str());
      actions.copiedLink = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
    }
  } else if (snap.status == Status::UpToDate) {
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.0f, 1.0f),
                       "Iota Studio v%s is up to date.",
                       curVer.c_str());
    if (!snap.latestRelease.tagName.empty()) {
      ImGui::TextDisabled("Latest GitHub release: %s", snap.latestRelease.tagName.c_str());
    }
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    if (ImGui::Button("Open Releases Page")) {
      actions.requestOpenUrl = true;
      actions.urlToOpen = snap.latestRelease.htmlUrl.empty()
                              ? std::string(iota::core::UpdateChecker::kReleasesPageUrl)
                              : snap.latestRelease.htmlUrl;
    }
    ImGui::SameLine();
    if (ImGui::Button("Check Again")) {
      actions.requestRecheck = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
    }
  } else if (snap.status == Status::Error) {
    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "Could not check for updates.");
    ImGui::TextWrapped("%s", snap.errorMessage.empty() ? "Unknown error." : snap.errorMessage.c_str());
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    if (ImGui::Button("Open Releases Page")) {
      actions.requestOpenUrl = true;
      actions.urlToOpen = iota::core::UpdateChecker::kReleasesPageUrl;
    }
    ImGui::SameLine();
    if (ImGui::Button("Retry")) {
      actions.requestRecheck = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
    }
  } else {
    ImGui::Text("Check GitHub for the latest Iota Studio release.");
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    if (ImGui::Button("Check Now")) {
      actions.requestRecheck = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Releases Page")) {
      actions.requestOpenUrl = true;
      actions.urlToOpen = iota::core::UpdateChecker::kReleasesPageUrl;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
    }
  }

  ImGui::EndPopup();
}

}  // namespace iota::studio
