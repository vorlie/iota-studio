#include "doctest/doctest.h"

#include "iota/audio/audio_decoder.hpp"
#include "iota/core/procedural_assets.hpp"
#include "iota/core/types.hpp"
#include "iota/core/utf8_path.hpp"
#include "iota/core/zip_archive.hpp"
#include "iota/model/command_stack.hpp"
#include "iota/model/measured_var.hpp"
#include "iota/model/scene.hpp"
#include "iota/preset_io/preset_io.hpp"
#include "iota/preset_io/project_manager.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_CASE("Color and vector codecs round-trip cleanly") {
  using namespace iota::core;

  const ColorRgba c{0x12, 0x34, 0x56, 0x80};
  const int32_t argb = c.toArgbInt32();
  CHECK(argb == static_cast<int32_t>(0x80123456));
  const auto decoded = ColorRgba::fromArgbInt32(argb);
  CHECK(decoded.r == c.r);
  CHECK(decoded.g == c.g);
  CHECK(decoded.b == c.b);
  CHECK(decoded.a == c.a);

  const ColorHslaNorm h{0.625f, 0.75f, 0.45f, 0.9f};
  const auto rgba = h.toRgba();
  const auto back = ColorHslaNorm::fromRgba(rgba);
  CHECK(back.h == doctest::Approx(h.h).epsilon(0.02));
  CHECK(back.s == doctest::Approx(h.s).epsilon(0.02));
  CHECK(back.l == doctest::Approx(h.l).epsilon(0.02));
  CHECK(back.a == doctest::Approx(h.a).epsilon(0.02));

  const auto v = parseF2("0.250000 -0.750000");
  REQUIRE(v.has_value());
  CHECK(v->x == doctest::Approx(0.25f));
  CHECK(v->y == doctest::Approx(-0.75f));
}

TEST_CASE("Procedural internal resources cover core built-in URIs") {
  for (const char* uri : {"internalres:white",
                          "internalres:black",
                          "internalres:transparent",
                          "internalres:particle_blur01",
                          "internalres:particle_circle_blur4",
                          "internalres:vignette80",
                          "internalres:rainbow128",
                          "internalres:lens_flare",
                          "internalres:placeholderart5",
                          "internalres:anim128_g_m10_15"}) {
    auto img = iota::core::generateInternalResource(uri);
    REQUIRE_MESSAGE(img.has_value(), uri);
    CHECK(img->width > 0);
    CHECK(img->height > 0);
    CHECK(img->pixels.size() == static_cast<size_t>(img->width * img->height * 4));
  }
}

TEST_CASE("MeasuredVar parsing handles multi-slot strings and legacy child fallback") {
  using namespace iota::model;

  const auto parsed = parseMVarString("BeatCamShakeMore 0.5 0.5;Constant -0.001 -0.002");
  REQUIRE(parsed.slots.size() == 2);
  CHECK(parsed.slots[0].measureWhat == "BeatCamShakeMore");
  CHECK(parsed.slots[0].a == doctest::Approx(0.5f));
  CHECK(parsed.slots[1].measureWhat == "Constant");
  CHECK(parsed.slots[1].b == doctest::Approx(-0.002f));

  const auto formatted = formatMVarString(parsed);
  const auto reparsed = parseMVarString(formatted);
  REQUIRE(reparsed.slots.size() == 2);
  CHECK(reparsed.slots[1].b == doctest::Approx(-0.002f));
}

TEST_CASE("CommandStack undo and redo preserve Project state") {
  using namespace iota::model;
  Project proj = createStarterProject();
  CommandStack stack;
  stack.clear(proj);

  const size_t origComps = proj.compositions.size();
  proj.compositions.push_back(createDefaultComposition(1));
  stack.pushSnapshot("Add Comp", proj);
  CHECK(proj.compositions.size() == origComps + 1);
  CHECK(stack.canUndo());

  const Project* undone = stack.undo();
  REQUIRE(undone != nullptr);
  CHECK(undone->compositions.size() == origComps);
  CHECK(stack.canRedo());

  const Project* redone = stack.redo();
  REQUIRE(redone != nullptr);
  CHECK(redone->compositions.size() == origComps + 1);
}

TEST_CASE("VIZ archive round-trip preserves scene and localAssets") {
  using namespace iota::model;
  Project proj = createStarterProject();
  proj.localAssets["custom_logo.png"] = {0x89, 0x50, 0x4E, 0x47, 1, 2, 3, 4};

  const auto tempPath = std::filesystem::temp_directory_path() / "iota_test_roundtrip.viz";
  std::string err;
  REQUIRE_MESSAGE(iota::preset_io::saveProjectAsViz(proj, tempPath, &err), err);

  auto reloaded = iota::preset_io::loadProjectFromFile(tempPath, &err);
  std::filesystem::remove(tempPath);

  REQUIRE_MESSAGE(reloaded.has_value(), err);
  CHECK(reloaded->compositions.size() == proj.compositions.size());
  CHECK(reloaded->compositions[0].elements.size() == proj.compositions[0].elements.size());
  REQUIRE(reloaded->localAssets.contains("custom_logo.png"));
  CHECK(reloaded->localAssets.at("custom_logo.png") == proj.localAssets.at("custom_logo.png"));
}

TEST_CASE("All 18 built-in APK presets parse and round-trip cleanly") {
  const char* dirEnv = std::getenv("IOTA_TEST_PRESETS_DIR");
  REQUIRE_MESSAGE(dirEnv != nullptr, "IOTA_TEST_PRESETS_DIR environment variable must be set");

  const std::filesystem::path dir(dirEnv);
  REQUIRE(std::filesystem::exists(dir));

  for (int i = 0; i < 18; ++i) {
    const auto path = dir / ("visualizer_" + std::to_string(i) + ".json");
    INFO("Preset: " << path.string());
    REQUIRE(std::filesystem::exists(path));

    std::string err;
    auto proj = iota::preset_io::loadProjectFromFile(path, &err);
    REQUIRE_MESSAGE(proj.has_value(), err);
    CHECK(proj->version == "10");
    CHECK(proj->templateId == i);
    REQUIRE(!proj->compositions.empty());
    CHECK(!proj->compositions[0].elements.empty());

    // Round-trip serialize -> re-parse
    const std::string jsonOut = iota::preset_io::serializeProjectToJsonString(*proj, false);
    auto reparsed = iota::preset_io::parseProjectJsonString(jsonOut, &err);
    REQUIRE_MESSAGE(reparsed.has_value(), err);
    CHECK(reparsed->compositions.size() == proj->compositions.size());
    for (size_t c = 0; c < proj->compositions.size(); ++c) {
      CHECK(reparsed->compositions[c].elements.size() == proj->compositions[c].elements.size());
    }
  }
}

TEST_CASE("Built-in asset enumeration, project asset helpers, and metadata round-trip") {
  using namespace iota::model;

  const auto& internals = iota::core::listInternalResources();
  CHECK(internals.size() == 15);
  CHECK(std::find(internals.begin(), internals.end(), "internalres:lens_flare") != internals.end());

  Project proj = createStarterProject();
  proj.metadata.name = "Neon Horizon";
  proj.metadata.notes = "Club intro visualizer";
  proj.metadata.audioPath = "C:/music/track.mp3";
  proj.metadata.previewWidth = 1920;
  proj.metadata.previewHeight = 1080;
  proj.metadata.targetFps = 144;
  proj.metadata.customFolders = {"Textures", "Masks"};
  proj.metadata.assetFolders["bg.png"] = "Textures";
  proj.metadata.audioAssets["track.mp3"] = "C:/music/track.mp3";
  proj.localAssets["bg.png"] = {1, 2, 3, 4};
  proj.localAssets["unused_overlay.png"] = {5, 6, 7};

  Element img = createDefaultElement(ElementType::Image);
  CHECK(assignAssetToElement(img, "local:bg.png", false));
  CHECK(assignAssetToElement(img, "internalres:vignette80", true));
  proj.compositions[0].elements.push_back(std::move(img));

  CHECK(countAssetReferences(proj, "local:bg.png") == 1);
  CHECK(countAssetReferences(proj, "local:unused_overlay.png") == 0);
  CHECK(countAssetReferences(proj, "internalres:vignette80") == 1);

  CHECK_FALSE(renameLocalAsset(proj, "missing.png", "foo.png"));
  CHECK_FALSE(renameLocalAsset(proj, "bg.png", "unused_overlay.png"));
  CHECK(renameLocalAsset(proj, "bg.png", "hero_bg.png"));
  CHECK(countAssetReferences(proj, "local:bg.png") == 0);
  CHECK(countAssetReferences(proj, "local:hero_bg.png") == 1);
  CHECK(proj.metadata.assetFolders.at("hero_bg.png") == "Textures");

  CHECK(removeUnusedLocalAssets(proj) == 1);
  CHECK(proj.localAssets.size() == 1);
  CHECK(proj.localAssets.contains("hero_bg.png"));

  const auto tempIota = std::filesystem::temp_directory_path() / "iota_meta_test.iota";
  std::string err;
  REQUIRE_MESSAGE(iota::preset_io::saveProjectAutoFormat(proj, tempIota, &err), err);
  auto reloaded = iota::preset_io::loadProjectFromFile(tempIota, &err);
  std::filesystem::remove(tempIota);
  REQUIRE_MESSAGE(reloaded.has_value(), err);
  CHECK(reloaded->metadata.name == "Neon Horizon");
  CHECK(reloaded->metadata.notes == "Club intro visualizer");
  CHECK(reloaded->metadata.previewWidth == 1920);
  CHECK(reloaded->metadata.previewHeight == 1080);
  CHECK(reloaded->metadata.targetFps == 144);
  CHECK(reloaded->metadata.customFolders == std::vector<std::string>{"Textures", "Masks"});
  CHECK(reloaded->metadata.assetFolders.at("hero_bg.png") == "Textures");
  CHECK(reloaded->metadata.audioAssets.at("track.mp3") == "C:/music/track.mp3");
}

TEST_CASE("ProjectLibrary registry persistence, dedup, prune, and starter templates") {
  using namespace iota::preset_io;

  const auto& tpls = projectTemplates();
  REQUIRE(tpls.size() == 4);
  for (const auto& info : tpls) {
    auto proj = createProjectFromTemplate(info.kind, info.title, 1080, 1920, 60);
    CHECK(proj.metadata.name == info.title);
    CHECK(proj.metadata.previewWidth == 1080);
    CHECK(proj.metadata.previewHeight == 1920);
    CHECK(!proj.compositions.empty());
  }

  const auto tmpDir = std::filesystem::temp_directory_path();
  const auto existingA = tmpDir / "iota_lib_a.iota";
  const auto missingB = tmpDir / "iota_lib_missing_b.iota";
  {
    std::ofstream out(existingA, std::ios::binary);
    out << "stub";
  }
  std::filesystem::remove(missingB);

  ProjectLibrary lib;
  lib.recordOpened({"Project B", missingB.string(), "2026-01-01 10:00:00", 1, 0});
  lib.recordOpened({"Project A", existingA.string(), "2026-01-02 10:00:00", 2, 3});
  lib.recordOpened({"Project A Renamed", existingA.string(), "", 2, 4});
  REQUIRE(lib.entries().size() == 2);
  CHECK(lib.entries()[0].name == "Project A Renamed");

  const auto regPath = tmpDir / "iota_test_registry.json";
  REQUIRE(lib.save(regPath));

  ProjectLibrary reloaded;
  REQUIRE(reloaded.load(regPath));
  CHECK(reloaded.entries().size() == 2);
  CHECK(reloaded.pruneMissing() == 1);
  CHECK(reloaded.entries().size() == 1);
  CHECK(reloaded.removeEntry(existingA.string()));
  CHECK(reloaded.entries().empty());

  std::filesystem::remove(existingA);
  std::filesystem::remove(regPath);
}

TEST_CASE("UTF-8 sanitization, Unicode paths, non-UTF-8 ZIP/JSON, and Unicode audio decode") {
  using namespace iota::core;
  using namespace iota::preset_io;

  struct TempFileCleanup {
    std::vector<std::filesystem::path> paths;
    ~TempFileCleanup() {
      std::error_code ec;
      for (const auto& p : paths) {
        std::filesystem::remove(p, ec);
      }
    }
  } cleanup;

  // 1. UTF-8 validation, BOM stripping, and safe codepoint truncation
  const std::string validUtf8 = "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87_\xE9\x9F\xB3\xE6\xA5\xBD_\xF0\x9F\x8E\xB5";
  CHECK(sanitizeUtf8(validUtf8) == validUtf8);

  const std::string invalidBytes = "A\xB3\xEA\xFF\xFE\xC0\xAF\xED\xA0\x80Z";
  const std::string sanitized = sanitizeUtf8(invalidBytes);
  CHECK(sanitizeUtf8(sanitized) == sanitized);
  CHECK(sanitized.front() == 'A');
  CHECK(sanitized.back() == 'Z');
  CHECK(sanitized.find("\xEF\xBF\xBD") != std::string::npos);

  CHECK(stripUtf8Bom("\xEF\xBB\xBF{\"k\":1}") == "{\"k\":1}");
  CHECK(stripUtf8Bom("{\"k\":1}") == "{\"k\":1}");

  const std::string trunc = truncateUtf8("A\xE9\x9F\xB3\xE6\xA5\xBD\xF0\x9F\x8E\xB5Z", 3, "..");
  CHECK(trunc == "A\xE9\x9F\xB3\xE6\xA5\xBD..");
  CHECK(sanitizeUtf8(trunc) == trunc);

  CHECK(baseNameFromSlashPath("dir/sub\\t\x88o_\xFF.png") == "t\xEF\xBF\xBDo_\xEF\xBF\xBD.png");

  // 2. Wide Unicode filesystem path conversion round-trips cleanly
  const std::filesystem::path tmpDir = std::filesystem::temp_directory_path();
  const std::filesystem::path unicodeIota =
      tmpDir / std::filesystem::path(L"iota_za\u017C\u00F3\u0142\u0107_\u97F3\u697D_\U0001F3B5.iota");
  const std::filesystem::path unicodeJson =
      tmpDir / std::filesystem::path(L"iota_za\u017C\u00F3\u0142\u0107_\u97F3\u697D_\U0001F3B5.json");
  const std::filesystem::path unicodeMissing =
      tmpDir / std::filesystem::path(L"iota_missing_\u97F3\u697D_\U0001F3B5.iota");
  const std::filesystem::path unicodeReg =
      tmpDir / std::filesystem::path(L"iota_reg_\u0142\u017C_\u97F3\u697D.json");
  const std::filesystem::path unicodeWav =
      tmpDir / std::filesystem::path(L"iota_za\u017C\u00F3\u0142\u0107_\u97F3\u697D_\U0001F3B5.wav");
  cleanup.paths = {unicodeIota, unicodeJson, unicodeReg, unicodeWav};

  const std::string u8Iota = pathToUtf8(unicodeIota);
  CHECK(sanitizeUtf8(u8Iota) == u8Iota);
  CHECK(pathFromUtf8(u8Iota) == unicodeIota);
  CHECK(pathExtensionUtf8(unicodeIota) == ".iota");
  CHECK(pathFilenameUtf8(unicodeIota).find(".iota") != std::string::npos);
  CHECK(pathStemUtf8(unicodeIota) == "iota_" + validUtf8);

  // 3. Saving and loading .iota & .json at Unicode paths, even with raw non-UTF-8 strings in memory
  auto proj = iota::model::createStarterProject();
  proj.metadata.name = "Project " + validUtf8;
  proj.metadata.notes = std::string("Notes \xB3\xEA\xFF ") + validUtf8;
  if (!proj.compositions.empty() && !proj.compositions[0].elements.empty()) {
    proj.compositions[0].elements[0].displayName = std::string("Layer \x80\xFF");
  }

  std::string err;
  const std::string dumped = serializeProjectToJsonString(proj, 2);
  CHECK_FALSE(dumped.empty());
  CHECK(sanitizeUtf8(dumped) == dumped);

  REQUIRE_MESSAGE(saveProjectAutoFormat(proj, unicodeIota, &err), err);
  REQUIRE_MESSAGE(saveProjectAutoFormat(proj, unicodeJson, &err), err);
  CHECK(pathExistsNoThrow(unicodeIota));
  CHECK(pathExistsNoThrow(unicodeJson));

  auto reloadedIota = loadProjectFromFile(unicodeIota, &err);
  REQUIRE_MESSAGE(reloadedIota.has_value(), err);
  CHECK(reloadedIota->metadata.name == "Project " + validUtf8);

  auto reloadedJson = loadProjectFromFile(unicodeJson, &err);
  REQUIRE_MESSAGE(reloadedJson.has_value(), err);
  CHECK(reloadedJson->metadata.name == "Project " + validUtf8);

  // 4. ZIP archive containing UTF-8 BOM + invalid raw bytes in scene.json & non-UTF-8 asset name
  const std::string bomAndInvalidJson =
      "\xEF\xBB\xBF"
      "{"
      "\"ver\":\"10\","
      "\"template\":0,"
      "\"compositions\":[{\"v\":\"Composition\",\"objType\":\"Composition\",\"_name\":\"Comp\",\"elements\":[]}],"
      "\"_desktopExt\":{\"projectMeta\":{\"name\":\"Raw\xFFName\"}}"
      "}";
  std::vector<ZipEntryData> zipEntries;
  zipEntries.push_back({"scene.json", std::vector<uint8_t>(bomAndInvalidJson.begin(), bomAndInvalidJson.end())});
  zipEntries.push_back({"assets/t\x88o_\xFF.png", std::vector<uint8_t>{0x89, 0x50, 0x4E, 0x47}});

  std::vector<uint8_t> zipBytes;
  REQUIRE_MESSAGE(writeZipToMemory(zipEntries, zipBytes, &err), err);
  auto zipLoaded = loadProjectFromMemory(zipBytes, &err);
  REQUIRE_MESSAGE(zipLoaded.has_value(), err);
  CHECK(zipLoaded->localAssets.contains("t\xEF\xBF\xBDo_\xEF\xBF\xBD.png"));
  const std::string reSerialized = serializeProjectToJsonString(*zipLoaded, -1);
  CHECK_FALSE(reSerialized.empty());

  // 5. ProjectLibrary with Unicode paths and pruneMissing
  ProjectLibrary lib;
  lib.recordOpened({"", pathToUtf8(unicodeMissing), "", 1, 0});
  lib.recordOpened({"", u8Iota, "", 1, 0});
  REQUIRE(lib.entries().size() == 2);
  CHECK(lib.entries()[0].name == "iota_" + validUtf8);
  REQUIRE(lib.save(unicodeReg));

  ProjectLibrary libReloaded;
  REQUIRE(libReloaded.load(unicodeReg));
  CHECK(libReloaded.entries().size() == 2);
  CHECK(libReloaded.pruneMissing() == 1);
  REQUIRE(libReloaded.entries().size() == 1);
  CHECK(libReloaded.removeEntry(u8Iota));
  CHECK(libReloaded.entries().empty());

  // 6. Audio decode from Unicode filename WAV (minimal 44-byte RIFF header + stereo PCM16 frames)
  {
    constexpr uint32_t kSampleRate = 44100;
    constexpr uint16_t kChannels = 2;
    constexpr uint16_t kBitsPerSample = 16;
    constexpr uint32_t kNumFrames = 1024;
    constexpr uint32_t kDataBytes = kNumFrames * kChannels * (kBitsPerSample / 8);
    constexpr uint32_t kRiffChunkSize = 36 + kDataBytes;
    constexpr uint32_t kByteRate = kSampleRate * kChannels * (kBitsPerSample / 8);
    constexpr uint16_t kBlockAlign = kChannels * (kBitsPerSample / 8);

    std::ofstream wavOut(unicodeWav, std::ios::binary | std::ios::trunc);
    REQUIRE(wavOut.is_open());
    auto writeU32 = [&](uint32_t v) { wavOut.write(reinterpret_cast<const char*>(&v), 4); };
    auto writeU16 = [&](uint16_t v) { wavOut.write(reinterpret_cast<const char*>(&v), 2); };
    wavOut.write("RIFF", 4);
    writeU32(kRiffChunkSize);
    wavOut.write("WAVE", 4);
    wavOut.write("fmt ", 4);
    writeU32(16);
    writeU16(1);
    writeU16(kChannels);
    writeU32(kSampleRate);
    writeU32(kByteRate);
    writeU16(kBlockAlign);
    writeU16(kBitsPerSample);
    wavOut.write("data", 4);
    writeU32(kDataBytes);
    for (uint32_t f = 0; f < kNumFrames; ++f) {
      const float phase = 2.0f * 3.14159265f * 440.0f * static_cast<float>(f) / static_cast<float>(kSampleRate);
      const int16_t s = static_cast<int16_t>(std::sin(phase) * 12000.0f);
      wavOut.write(reinterpret_cast<const char*>(&s), 2);
      wavOut.write(reinterpret_cast<const char*>(&s), 2);
    }
    REQUIRE(wavOut.good());
  }

  auto decodedTrack = iota::audio::decodeAudioFile(unicodeWav, 44100, &err);
  REQUIRE_MESSAGE(decodedTrack.has_value(), err);
  CHECK(decodedTrack->monoPcm.size() >= 512);
  CHECK(pathFromUtf8(decodedTrack->filePath) == unicodeWav);
  CHECK(sanitizeUtf8(decodedTrack->title) == decodedTrack->title);
}
