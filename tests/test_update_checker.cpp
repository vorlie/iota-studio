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

#include "doctest/doctest.h"

#include "iota/core/update_checker.hpp"

#include <string>

TEST_CASE("UpdateChecker SemVer parsing, comparison, and HTTPS URL safety") {
  using namespace iota::core;

  SUBCASE("Built-in IOTA_STUDIO_VERSION parses cleanly") {
    const auto v = parseSemVer(IOTA_STUDIO_VERSION);
    REQUIRE(v.has_value());
    CHECK(v->major >= 1);
  }

  SUBCASE("parseSemVer accepts standard and v-prefixed versions") {
    const auto v1 = parseSemVer("1.3.1");
    REQUIRE(v1.has_value());
    CHECK(v1->major == 1);
    CHECK(v1->minor == 3);
    CHECK(v1->patch == 1);
    CHECK(v1->prerelease.empty());

    const auto v2 = parseSemVer("  v2.10.4-rc.1+build42 \n");
    REQUIRE(v2.has_value());
    CHECK(v2->major == 2);
    CHECK(v2->minor == 10);
    CHECK(v2->patch == 4);
    CHECK(v2->prerelease == "rc.1");

    const auto v3 = parseSemVer("V1.4");
    REQUIRE(v3.has_value());
    CHECK(v3->major == 1);
    CHECK(v3->minor == 4);
    CHECK(v3->patch == 0);

    CHECK_FALSE(parseSemVer("").has_value());
    CHECK_FALSE(parseSemVer("v").has_value());
    CHECK_FALSE(parseSemVer("1").has_value());
    CHECK_FALSE(parseSemVer("1.2.3.4").has_value());
    CHECK_FALSE(parseSemVer("1.2.").has_value());
    CHECK_FALSE(parseSemVer("1.2.x").has_value());
    CHECK_FALSE(parseSemVer("1.2.3-").has_value());
  }

  SUBCASE("SemVer precedence and isNewerVersion") {
    CHECK(isNewerVersion("1.3.1", "1.3.2"));
    CHECK(isNewerVersion("1.3.1", "v1.4.0"));
    CHECK(isNewerVersion("1.3.1", "2.0.0"));
    CHECK(isNewerVersion("1.3.1-rc.1", "1.3.1"));
    CHECK(isNewerVersion("1.3.1-alpha.2", "1.3.1-alpha.10"));
    CHECK(isNewerVersion("1.3.1-1", "1.3.1-beta"));

    CHECK(isNewerVersion("1.3.1", "1.3.2-rc.1"));
    CHECK_FALSE(isNewerVersion("1.3.1", "1.3.1"));
    CHECK_FALSE(isNewerVersion("1.3.1", "v1.3.0"));
    CHECK_FALSE(isNewerVersion("1.3.1", "1.3.1-rc.1"));
    CHECK_FALSE(isNewerVersion("1.3.1", "invalid"));
  }

  SUBCASE("isSafeHttpsUrl validates HTTPS URLs strictly") {
    CHECK(isSafeHttpsUrl("https://github.com/vorlie/iota-studio/releases/tag/v1.3.2"));
    CHECK(isSafeHttpsUrl("https://github.com/vorlie/iota-studio/releases/download/v1.3.2/iota_studio.exe"));

    CHECK_FALSE(isSafeHttpsUrl("http://github.com/vorlie/iota-studio/releases"));
    CHECK_FALSE(isSafeHttpsUrl("file:///C:/Windows/System32/cmd.exe"));
    CHECK_FALSE(isSafeHttpsUrl("javascript:alert(1)"));
    CHECK_FALSE(isSafeHttpsUrl("https://"));
    CHECK_FALSE(isSafeHttpsUrl("https://example.com/bad path"));
    CHECK_FALSE(isSafeHttpsUrl("https://example.com/bad\"quote"));
    CHECK_FALSE(isSafeHttpsUrl("https://example.com/\ncrlf"));
  }
}

TEST_CASE("parseGitHubReleaseJson extracts release metadata, assets, and update status") {
  using namespace iota::core;

  const char* kNewerPayload = R"({
    "tag_name": "v1.4.0",
    "name": "Iota Studio v1.4.0",
    "html_url": "https://github.com/vorlie/iota-studio/releases/tag/v1.4.0",
    "published_at": "2026-12-01T10:15:30Z",
    "prerelease": false,
    "draft": false,
    "body": "## What's New\n- Improved update workflow",
    "assets": [
      {
        "name": "source-archive.zip",
        "size": 102400,
        "browser_download_url": "https://github.com/vorlie/iota-studio/releases/download/v1.4.0/source-archive.zip"
      },
      {
        "name": "iota_studio.exe",
        "size": 2512000,
        "browser_download_url": "https://github.com/vorlie/iota-studio/releases/download/v1.4.0/iota_studio.exe"
      }
    ]
  })";

  SUBCASE("Detects newer release and prefers .exe asset") {
    std::string err;
    const auto rel = parseGitHubReleaseJson(
        kNewerPayload, "1.3.1", UpdateChecker::kReleasesPageUrl, &err);
    REQUIRE_MESSAGE(rel.has_value(), err);
    CHECK(rel->updateAvailable);
    CHECK(rel->tagName == "v1.4.0");
    CHECK(rel->version == "1.4.0");
    CHECK(rel->title == "Iota Studio v1.4.0");
    CHECK(rel->htmlUrl == "https://github.com/vorlie/iota-studio/releases/tag/v1.4.0");
    CHECK(rel->publishedAt == "2026-12-01T10:15:30Z");
    CHECK(rel->releaseNotes.find("Improved update workflow") != std::string::npos);
    CHECK(rel->assetName == "iota_studio.exe");
    CHECK(rel->assetSizeBytes == 2512000u);
    CHECK(rel->assetDownloadUrl ==
          "https://github.com/vorlie/iota-studio/releases/download/v1.4.0/iota_studio.exe");
  }

  SUBCASE("Up-to-date and older versions do not trigger updateAvailable") {
    const auto equalRel = parseGitHubReleaseJson(kNewerPayload, "1.4.0");
    REQUIRE(equalRel.has_value());
    CHECK_FALSE(equalRel->updateAvailable);

    const auto newerLocal = parseGitHubReleaseJson(kNewerPayload, "1.5.0");
    REQUIRE(newerLocal.has_value());
    CHECK_FALSE(newerLocal->updateAvailable);
  }

  SUBCASE("Draft, prerelease, and unsafe URLs are handled safely") {
    const char* kDraftOrUnsafe = R"json({
      "tag_name": "v2.0.0",
      "name": "Preview 2.0",
      "html_url": "javascript:evil()",
      "prerelease": true,
      "draft": false,
      "body": "Preview",
      "assets": [
        {
          "name": "bad.exe",
          "size": 123,
          "browser_download_url": "http://insecure.example/bad.exe"
        }
      ]
    })json";
    const auto rel = parseGitHubReleaseJson(kDraftOrUnsafe, "1.3.1");
    REQUIRE(rel.has_value());
    CHECK_FALSE(rel->updateAvailable);  // prerelease excluded
    CHECK(rel->htmlUrl == UpdateChecker::kReleasesPageUrl);
    CHECK(rel->assetDownloadUrl.empty());
  }

  SUBCASE("Malformed JSON or missing tag_name returns nullopt with error message") {
    std::string err;
    CHECK_FALSE(parseGitHubReleaseJson("{broken json", "1.3.1", UpdateChecker::kReleasesPageUrl, &err).has_value());
    CHECK_FALSE(err.empty());

    err.clear();
    CHECK_FALSE(parseGitHubReleaseJson(R"({"name":"no tag"})", "1.3.1", UpdateChecker::kReleasesPageUrl, &err).has_value());
    CHECK_FALSE(err.empty());
  }

  SUBCASE("Selects standard or MCP installer asset based on preferMcpAsset") {
    const char* kMultiAssetPayload = R"({
      "tag_name": "v1.7.0",
      "name": "Iota Studio v1.7.0",
      "html_url": "https://github.com/vorlie/iota-studio/releases/tag/v1.7.0",
      "published_at": "2026-11-24T18:00:00Z",
      "prerelease": false,
      "draft": false,
      "body": "Release 1.7.0",
      "assets": [
        {
          "name": "IotaStudio-MCP-Setup-1.7.0.exe",
          "size": 3600000,
          "browser_download_url": "https://github.com/vorlie/iota-studio/releases/download/v1.7.0/IotaStudio-MCP-Setup-1.7.0.exe"
        },
        {
          "name": "IotaStudio-Setup-1.7.0.exe",
          "size": 3500000,
          "browser_download_url": "https://github.com/vorlie/iota-studio/releases/download/v1.7.0/IotaStudio-Setup-1.7.0.exe"
        }
      ]
    })";

    const auto stdRel = parseGitHubReleaseJson(
        kMultiAssetPayload, "1.6.0", UpdateChecker::kReleasesPageUrl, nullptr, false);
    REQUIRE(stdRel.has_value());
    CHECK(stdRel->assetName == "IotaStudio-Setup-1.7.0.exe");

    const auto mcpRel = parseGitHubReleaseJson(
        kMultiAssetPayload, "1.6.0", UpdateChecker::kReleasesPageUrl, nullptr, true);
    REQUIRE(mcpRel.has_value());
    CHECK(mcpRel->assetName == "IotaStudio-MCP-Setup-1.7.0.exe");

    // Fallback when MCP build sees a release containing only the standard asset
    const auto fallbackRel = parseGitHubReleaseJson(
        kNewerPayload, "1.3.1", UpdateChecker::kReleasesPageUrl, nullptr, true);
    REQUIRE(fallbackRel.has_value());
    CHECK(fallbackRel->assetName == "iota_studio.exe");
  }
}
