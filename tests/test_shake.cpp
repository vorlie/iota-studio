#include "doctest/doctest.h"

#include "iota/engine/shake_eval.hpp"
#include "iota/model/scene.hpp"
#include "iota/preset_io/preset_io.hpp"

#include <cmath>

TEST_CASE("Procedural Sapphire-style ShakeConfig evaluation is deterministic, smooth, and bounded") {
  using namespace iota::engine;

  SUBCASE("Zero amplitude yields exact identity pose") {
    ShakeConfig cfg;
    cfg.amplitude = 0.0f;
    cfg.zDist = 1.35f;
    for (double t : {0.0, 0.42, 3.14}) {
      const auto p = evaluateCameraShake(cfg, t);
      CHECK(p.offsetX == doctest::Approx(0.0f));
      CHECK(p.offsetY == doctest::Approx(0.0f));
      CHECK(p.zDist == doctest::Approx(1.35f));
      CHECK(p.tiltRad == doctest::Approx(0.0f));
    }
  }

  SUBCASE("Still mode ignores time and follows phase only") {
    ShakeConfig cfg;
    cfg.amplitude = 0.8f;
    cfg.still = true;
    cfg.phase = 0.45f;
    const auto a = evaluateCameraShake(cfg, 0.0);
    const auto b = evaluateCameraShake(cfg, 5.25);
    CHECK(a.offsetX == doctest::Approx(b.offsetX));
    CHECK(a.offsetY == doctest::Approx(b.offsetY));
    CHECK(a.zDist == doctest::Approx(b.zDist));
    CHECK(a.tiltRad == doctest::Approx(b.tiltRad));

    cfg.phase = 1.75f;
    const auto c = evaluateCameraShake(cfg, 5.25);
    CHECK(std::abs(c.offsetX - a.offsetX) > 1e-4f);
  }

  SUBCASE("Seed produces reproducible yet distinct motion patterns") {
    ShakeConfig cfg;
    cfg.amplitude = 0.9f;
    cfg.seed = 17.0f;
    const auto s1a = evaluateCameraShake(cfg, 1.25);
    const auto s1b = evaluateCameraShake(cfg, 1.25);
    CHECK(s1a.offsetX == doctest::Approx(s1b.offsetX));
    CHECK(s1a.offsetY == doctest::Approx(s1b.offsetY));

    cfg.seed = 99.0f;
    const auto s2 = evaluateCameraShake(cfg, 1.25);
    const float diff = std::abs(s1a.offsetX - s2.offsetX) + std::abs(s1a.offsetY - s2.offsetY);
    CHECK(diff > 1e-3f);
  }

  SUBCASE("Octaves parameter increases fractal detail while remaining strictly bounded") {
    auto measureHighFreqVariation = [&](int octaves, float& outMaxAbsX) {
      ShakeConfig cfg;
      cfg.amplitude = 1.0f;
      cfg.frequency = 4.0f;
      cfg.octaves = octaves;
      cfg.xRandAmp = 0.25f;
      cfg.yRandAmp = 0.0f;
      cfg.zRandAmp = 0.0f;
      cfg.tiltRandAmp = 0.0f;
      double totalDiff = 0.0;
      float prevX = evaluateCameraShake(cfg, 0.0).offsetX;
      outMaxAbsX = std::abs(prevX);
      for (int i = 1; i <= 200; ++i) {
        const float curX = evaluateCameraShake(cfg, static_cast<double>(i) * 0.005).offsetX;
        totalDiff += std::abs(curX - prevX);
        outMaxAbsX = std::max(outMaxAbsX, std::abs(curX));
        prevX = curX;
      }
      return totalDiff;
    };

    float max1 = 0.0f;
    float max8 = 0.0f;
    const double var1 = measureHighFreqVariation(1, max1);
    const double var8 = measureHighFreqVariation(8, max8);
    CHECK(max1 <= 0.2501f);
    CHECK(max8 <= 0.2501f);
    CHECK(var8 > var1 * 1.15);
  }

  SUBCASE("Pure wave mode reproduces exact sinusoidal period and amplitude") {
    ShakeConfig cfg;
    cfg.amplitude = 1.0f;
    cfg.frequency = 4.0f;
    cfg.xRandAmp = 0.0f;
    cfg.yRandAmp = 0.0f;
    cfg.zRandAmp = 0.0f;
    cfg.tiltRandAmp = 0.0f;
    cfg.xWaveAmp = 0.30f;
    cfg.xWaveFreq = 1.0f;

    CHECK(evaluateCameraShake(cfg, 0.0).offsetX == doctest::Approx(0.0f).epsilon(1e-5f));
    CHECK(evaluateCameraShake(cfg, 0.25 / 4.0).offsetX == doctest::Approx(0.30f).epsilon(1e-5f));
    CHECK(evaluateCameraShake(cfg, 0.75 / 4.0).offsetX == doctest::Approx(-0.30f).epsilon(1e-5f));
  }
}

TEST_CASE("ShakeEffect serializes cleanly through Project JSON round-trip") {
  iota::model::Project proj;
  proj.version = "10";
  auto comp0 = iota::model::createDefaultComposition(0);
  auto shake = iota::model::createDefaultElement(iota::model::ElementType::ShakeEffect);
  shake.props.setString("wrapX", "Tile", "sel No Tile Reflect Clamp", "1_appearance");
  shake.props.setInt("octaves", 6, 1, 10, "2_shakeMaster");
  shake.props.setFloat("moBlurLength", 1.4f, 0.0f, 4.0f, "4_motionAndRgb");
  shake.props.setFloat("rgbPhase", 0.35f, 0.0f, 1.0f, "4_motionAndRgb");
  comp0.elements.push_back(std::move(shake));
  proj.compositions.push_back(std::move(comp0));

  const std::string jsonStr = iota::preset_io::serializeProjectToJsonString(proj, 2);
  REQUIRE_FALSE(jsonStr.empty());
  std::string err;
  auto parsed = iota::preset_io::parseProjectJsonString(jsonStr, &err);
  REQUIRE_MESSAGE(parsed.has_value(), err);
  REQUIRE(parsed->compositions.size() == 1);
  REQUIRE(parsed->compositions[0].elements.size() == 1);

  const auto& outEl = parsed->compositions[0].elements[0];
  CHECK(outEl.type == iota::model::ElementType::ShakeEffect);
  CHECK(outEl.props.getString("TargetImage") == "composition:0");
  CHECK(outEl.props.getString("wrapX") == "Tile");
  CHECK(outEl.props.getInt("octaves") == 6);
  CHECK(outEl.props.getFloat("moBlurLength") == doctest::Approx(1.4f));
  CHECK(outEl.props.getFloat("rgbPhase") == doctest::Approx(0.35f));
}
