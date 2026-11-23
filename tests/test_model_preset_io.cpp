#include "doctest/doctest.h"

#include "iota/core/procedural_assets.hpp"
#include "iota/core/types.hpp"
#include "iota/model/command_stack.hpp"
#include "iota/model/measured_var.hpp"
#include "iota/model/scene.hpp"
#include "iota/preset_io/preset_io.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

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
