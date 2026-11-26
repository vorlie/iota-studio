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

#include "iota/discord/discord_presence.hpp"
#include "iota/preset_io/project_manager.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("DiscordConfig invariants match Discord Developer Portal setup") {
  using namespace iota::discord;

  CHECK(DiscordConfig::kApplicationId == 1574946746683891832ULL);
  CHECK(DiscordConfig::kLargeImageKey == "icon");
  CHECK(DiscordConfig::kLargeImageText == "Iota Studio");
}

TEST_CASE("Discord Rich Presence formatting covers all states and privacy bounds") {
  using namespace iota::discord;

  SUBCASE("Basic presence hides contextual details and shows required strings") {
    PresenceSnapshot snap;
    snap.compositionName = "BassNation";
    snap.exportEncodedFrames = 120;
    snap.exportTotalFrames = 240;

    snap.state = PresenceActivityState::Editing;
    auto pEdit = formatPresencePayload(snap, false);
    CHECK(pEdit.details == "Creating a composition");
    CHECK_FALSE(pEdit.state.has_value());
    CHECK(pEdit.largeImageKey == "icon");
    CHECK(pEdit.largeImageText == "Iota Studio");

    snap.state = PresenceActivityState::Previewing;
    auto pPrev = formatPresencePayload(snap, false);
    CHECK(pPrev.details == "Previewing a composition");
    CHECK_FALSE(pPrev.state.has_value());

    snap.state = PresenceActivityState::Exporting;
    auto pExp = formatPresencePayload(snap, false);
    CHECK(pExp.details == "Rendering a video");
    CHECK_FALSE(pExp.state.has_value());

    snap.state = PresenceActivityState::Idle;
    auto pIdle = formatPresencePayload(snap, false);
    CHECK(pIdle.details == "Using Iota Studio");
    CHECK_FALSE(pIdle.state.has_value());
  }

  SUBCASE("Expanded presence includes composition name and 5-percent quantized export progress") {
    PresenceSnapshot snap;
    snap.compositionName = "Main Spectrum";

    snap.state = PresenceActivityState::Editing;
    auto pEdit = formatPresencePayload(snap, true);
    CHECK(pEdit.details == "Creating a composition");
    REQUIRE(pEdit.state.has_value());
    CHECK(*pEdit.state == "Composition: Main Spectrum");

    snap.state = PresenceActivityState::Previewing;
    auto pPrev = formatPresencePayload(snap, true);
    CHECK(pPrev.details == "Previewing a composition");
    REQUIRE(pPrev.state.has_value());
    CHECK(*pPrev.state == "Composition: Main Spectrum");

    snap.state = PresenceActivityState::Idle;
    auto pIdle = formatPresencePayload(snap, true);
    CHECK(pIdle.details == "Using Iota Studio");
    REQUIRE(pIdle.state.has_value());
    CHECK(*pIdle.state == "Idle");

    snap.state = PresenceActivityState::Exporting;
    snap.exportTotalFrames = 1000;

    snap.exportEncodedFrames = 0;
    CHECK(*formatPresencePayload(snap, true).state == "Main Spectrum (0%)");

    snap.exportEncodedFrames = 49;
    CHECK(*formatPresencePayload(snap, true).state == "Main Spectrum (0%)");

    snap.exportEncodedFrames = 50;
    CHECK(*formatPresencePayload(snap, true).state == "Main Spectrum (5%)");

    snap.exportEncodedFrames = 499;
    CHECK(*formatPresencePayload(snap, true).state == "Main Spectrum (45%)");

    snap.exportEncodedFrames = 999;
    CHECK(*formatPresencePayload(snap, true).state == "Main Spectrum (95%)");

    snap.exportEncodedFrames = 1000;
    CHECK(*formatPresencePayload(snap, true).state == "Main Spectrum (100%)");
  }

  SUBCASE("Composition sanitization strips file paths, drive letters, and control characters") {
    CHECK(sanitizeCompositionName("C:\\Users\\secret\\Desktop\\track_project.viz") == "track_project.viz");
    CHECK(sanitizeCompositionName("/home/user/private/secret_mix.iota") == "secret_mix.iota");
    CHECK(sanitizeCompositionName("  \tLayer\n  One \r\n ") == "Layer One");
    CHECK(sanitizeCompositionName("..") == "");
    CHECK(sanitizeCompositionName(".") == "");

    PresenceSnapshot snap;
    snap.state = PresenceActivityState::Editing;
    snap.compositionName = "C:\\Users\\Alice\\PrivateFolder\\MyComp";
    const auto p = formatPresencePayload(snap, true);
    REQUIRE(p.state.has_value());
    CHECK(p.state->find("Alice") == std::string::npos);
    CHECK(p.state->find("PrivateFolder") == std::string::npos);
    CHECK(p.state->find("C:") == std::string::npos);
    CHECK(*p.state == "Composition: MyComp");
  }

  SUBCASE("UTF-8 clamping stays valid on multi-byte boundaries and within 128 bytes") {
    std::string multi;
    for (int i = 0; i < 40; ++i) {
      multi += "\xC5\xBB\xC3\xB3\xC5\x82\xC4\x87";  // "Żółć" (8 bytes each, 320 bytes total)
    }
    const std::string clamped = clampDiscordFieldUtf8(multi, 128);
    CHECK(clamped.size() <= 128);
    CHECK(clamped.size() % 2 == 0);
    // Must not cut inside a 2-byte UTF-8 character.
    CHECK(clamped.size() == 128);

    std::string threeByte;
    for (int i = 0; i < 50; ++i) {
      threeByte += "\xE2\x99\xAA";  // U+266A EIGHTH NOTE (3 bytes)
    }
    const std::string clamped3 = clampDiscordFieldUtf8(threeByte, 128);
    CHECK(clamped3.size() <= 128);
    CHECK(clamped3.size() == 126);  // 42 * 3 = 126 bytes (clean boundary)
  }
}

TEST_CASE("DiscordPresence manager defaults to disabled and respects independent expanded toggle") {
  using namespace iota::discord;

  DiscordPresence presence;
  CHECK_FALSE(presence.isEnabled());
  CHECK_FALSE(presence.isExpanded());
  CHECK(presence.connectionState() == DiscordConnectionState::Disabled);
  CHECK_FALSE(presence.lastPublishedPayload().has_value());

  PresenceSnapshot snap;
  snap.state = PresenceActivityState::Editing;
  snap.compositionName = "Beat";
  presence.updateState(snap);
  presence.tick(1.0);
  presence.tick(2.0);
  CHECK(presence.connectionState() == DiscordConnectionState::Disabled);
  CHECK_FALSE(presence.lastPublishedPayload().has_value());

  presence.setExpanded(true);
  CHECK(presence.isExpanded());
  CHECK_FALSE(presence.isEnabled());
  CHECK(presence.connectionState() == DiscordConnectionState::Disabled);
  CHECK(presence.currentDesiredPayload().state == std::optional<std::string>("Composition: Beat"));

  presence.shutdown();
  CHECK(presence.connectionState() == DiscordConnectionState::Disabled);
}

TEST_CASE("ProjectLibrary persists discordRichPresenceEnabled and discordExpandedPresence independently") {
  using namespace iota::preset_io;

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path tempPath =
      std::filesystem::temp_directory_path() /
      ("iota_discord_prefs_" + std::to_string(stamp) + ".json");
  std::error_code ec;
  std::filesystem::remove(tempPath, ec);

  {
    ProjectLibrary lib;
    CHECK_FALSE(lib.discordRichPresenceEnabled());
    CHECK_FALSE(lib.discordExpandedPresence());

    lib.setDiscordRichPresenceEnabled(true);
    lib.setDiscordExpandedPresence(false);
    REQUIRE(lib.save(tempPath));
  }
  {
    ProjectLibrary loaded;
    REQUIRE(loaded.load(tempPath));
    CHECK(loaded.discordRichPresenceEnabled());
    CHECK_FALSE(loaded.discordExpandedPresence());

    loaded.setDiscordRichPresenceEnabled(false);
    loaded.setDiscordExpandedPresence(true);
    REQUIRE(loaded.save(tempPath));
  }
  {
    ProjectLibrary loaded;
    REQUIRE(loaded.load(tempPath));
    CHECK_FALSE(loaded.discordRichPresenceEnabled());
    CHECK(loaded.discordExpandedPresence());
  }
  {
    // Backward compatibility: older JSON without Discord fields must default both to false.
    std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
    out << R"({"version":1,"suppressVizImportNotice":true,"recent":[]})";
    out.close();

    ProjectLibrary legacyLoaded;
    REQUIRE(legacyLoaded.load(tempPath));
    CHECK(legacyLoaded.suppressVizImportNotice());
    CHECK_FALSE(legacyLoaded.discordRichPresenceEnabled());
    CHECK_FALSE(legacyLoaded.discordExpandedPresence());
  }

  std::filesystem::remove(tempPath, ec);
}

#ifndef _WIN32
TEST_CASE("ProjectLibrary preserves case-distinct paths and resolves XDG fallback on Linux") {
  using namespace iota::preset_io;

  ProjectLibrary lib;
  lib.recordOpened({"Upper", "/tmp/iota_case/Track.viz", "", 1, 0});
  lib.recordOpened({"Lower", "/tmp/iota_case/track.viz", "", 1, 0});
  CHECK(lib.entries().size() == 2);
  CHECK(lib.removeEntry("/tmp/iota_case/Track.viz"));
  REQUIRE(lib.entries().size() == 1);
  CHECK(lib.entries()[0].path == "/tmp/iota_case/track.viz");

  const std::filesystem::path reg = ProjectLibrary::defaultRegistryPath();
  CHECK(reg.filename() == "projects.json");
  CHECK(reg.parent_path().filename() == "IotaStudio");
}
#endif
