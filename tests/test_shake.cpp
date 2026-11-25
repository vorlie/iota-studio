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

  SUBCASE("Twitchy style produces quiet holds and active bursts governed by stillness") {
    ShakeConfig cfg;
    cfg.style = ShakeStyle::Twitchy;
    cfg.amplitude = 1.0f;
    cfg.frequency = 8.0f;
    cfg.twitchFreq = 2.0f;
    cfg.stillness = 0.70f;

    int exactZeroCount = 0;
    int activeCount = 0;
    for (int i = 0; i < 200; ++i) {
      const double t = static_cast<double>(i) * 0.025;
      const auto p = evaluateCameraShake(cfg, t);
      if (std::abs(p.offsetX) < 1e-6f && std::abs(p.offsetY) < 1e-6f && std::abs(p.tiltRad) < 1e-6f) {
        ++exactZeroCount;
      } else {
        ++activeCount;
      }
    }
    CHECK(exactZeroCount > 100);
    CHECK(activeCount >= 10);

    cfg.stillness = 1.0f;
    for (int i = 0; i < 40; ++i) {
      const auto p = evaluateCameraShake(cfg, static_cast<double>(i) * 0.05);
      CHECK(p.offsetX == doctest::Approx(0.0f));
      CHECK(p.offsetY == doctest::Approx(0.0f));
      CHECK(p.tiltRad == doctest::Approx(0.0f));
    }
  }

  SUBCASE("Jumpy style holds plateaus when drift=0, glides with drift, and snaps center with centerBias") {
    ShakeConfig cfg;
    cfg.style = ShakeStyle::Jumpy;
    cfg.amplitude = 1.0f;
    cfg.frequency = 1.0f;
    cfg.drift = 0.0f;
    cfg.centerBias = 0.0f;
    cfg.xRandAmp = 0.25f;
    cfg.xRandFreq = 1.0f;
    cfg.yRandAmp = 0.20f;
    cfg.zRandAmp = 0.0f;
    cfg.tiltRandAmp = 0.0f;

    const auto c1a = evaluateCameraShake(cfg, 1.15);
    const auto c1b = evaluateCameraShake(cfg, 1.50);
    const auto c1c = evaluateCameraShake(cfg, 1.85);
    CHECK(c1a.offsetX == doctest::Approx(c1b.offsetX).epsilon(1e-6f));
    CHECK(c1a.offsetX == doctest::Approx(c1c.offsetX).epsilon(1e-6f));

    const auto c2 = evaluateCameraShake(cfg, 2.50);
    CHECK(std::abs(c2.offsetX - c1a.offsetX) > 1e-3f);

    cfg.drift = 0.80f;
    const auto d1a = evaluateCameraShake(cfg, 1.15);
    const auto d1b = evaluateCameraShake(cfg, 1.85);
    CHECK(std::abs(d1a.offsetX - d1b.offsetX) > 1e-3f);

    cfg.drift = 0.0f;
    auto measureDispAndSnaps = [&](float bias, int& outSnaps) {
      cfg.centerBias = bias;
      outSnaps = 0;
      double totalDisp = 0.0;
      for (int i = 0; i < 200; ++i) {
        const auto p = evaluateCameraShake(cfg, static_cast<double>(i) + 0.5);
        if (std::abs(p.offsetX) < 1e-6f && std::abs(p.offsetY) < 1e-6f) {
          ++outSnaps;
        }
        totalDisp += std::abs(p.offsetX) + std::abs(p.offsetY);
      }
      return totalDisp / 200.0;
    };

    int snaps0 = 0;
    int snaps4 = 0;
    const double mean0 = measureDispAndSnaps(0.0f, snaps0);
    const double mean4 = measureDispAndSnaps(4.0f, snaps4);
    CHECK(snaps4 > snaps0 + 80);
    CHECK(mean4 < mean0 * 0.5);
  }

  SUBCASE("Per-axis phases shift individual X and Tilt trajectories cleanly") {
    ShakeConfig cfg;
    cfg.amplitude = 1.0f;
    cfg.frequency = 4.0f;
    cfg.xRandAmp = 0.0f;
    cfg.yRandAmp = 0.0f;
    cfg.zRandAmp = 0.0f;
    cfg.tiltRandAmp = 0.0f;
    cfg.xWaveAmp = 0.30f;
    cfg.xWaveFreq = 1.0f;
    cfg.xPhase = 0.25f;
    CHECK(evaluateCameraShake(cfg, 0.0).offsetX == doctest::Approx(0.30f).epsilon(1e-5f));

    ShakeConfig tiltCfg;
    tiltCfg.amplitude = 0.5f;
    tiltCfg.frequency = 1.0f;
    tiltCfg.tiltRandAmp = 3.2f;
    tiltCfg.tiltWaveAmp = 2.2f;
    tiltCfg.tiltWaveFreq = 0.38f;
    tiltCfg.tiltPhase = 0.0f;
    const float tiltAtZero = evaluateCameraShake(tiltCfg, 0.4).tiltRad;
    tiltCfg.tiltPhase = 2.6f;
    const float tiltShifted = evaluateCameraShake(tiltCfg, 0.4).tiltRad;
    CHECK(std::abs(tiltShifted - tiltAtZero) > 1e-3f);
  }

  SUBCASE("Channels support independent RGB amplitudes, phases, and RGB randomness") {
    ShakeConfig cfg;
    cfg.amplitude = 1.0f;
    cfg.frequency = 4.0f;
    cfg.xRandAmp = 0.0f;
    cfg.yRandAmp = 0.0f;
    cfg.zRandAmp = 0.0f;
    cfg.tiltRandAmp = 0.0f;
    cfg.xWaveAmp = 0.20f;
    cfg.xWaveFreq = 1.0f;

    const auto unified = evaluateCameraShakeRgb(cfg, 0.25 / 4.0, 0.0f);
    CHECK_FALSE(unified.splitChannels);
    CHECK(unified.r.offsetX == doctest::Approx(unified.g.offsetX));
    CHECK(unified.b.offsetX == doctest::Approx(unified.g.offsetX));

    cfg.redAmp = 1.6f;
    cfg.greenAmp = 1.0f;
    cfg.blueAmp = 0.4f;
    const auto ampSplit = evaluateCameraShakeRgb(cfg, 0.25 / 4.0, 0.0f);
    CHECK(ampSplit.splitChannels);
    CHECK(ampSplit.r.offsetX == doctest::Approx(0.32f).epsilon(1e-5f));
    CHECK(ampSplit.g.offsetX == doctest::Approx(0.20f).epsilon(1e-5f));
    CHECK(ampSplit.b.offsetX == doctest::Approx(0.08f).epsilon(1e-5f));

    cfg.redAmp = 1.0f;
    cfg.blueAmp = 1.0f;
    cfg.redPhase = 0.10f;
    cfg.bluePhase = -0.10f;
    const auto phaseSplit = evaluateCameraShakeRgb(cfg, 0.0, 0.0f);
    CHECK(phaseSplit.splitChannels);
    CHECK(phaseSplit.r.offsetX > phaseSplit.g.offsetX + 0.05f);
    CHECK(phaseSplit.b.offsetX < phaseSplit.g.offsetX - 0.05f);

    ShakeConfig randCfg;
    randCfg.amplitude = 0.8f;
    randCfg.rgbRandomness = 0.8f;
    randCfg.rgbFrequency = 4.0f;
    const auto randSplit = evaluateCameraShakeRgb(randCfg, 0.37, 0.0f);
    CHECK(randSplit.splitChannels);
    CHECK(std::abs(randSplit.r.offsetX - randSplit.g.offsetX) > 1e-4f);
    CHECK(std::abs(randSplit.g.offsetX - randSplit.b.offsetX) > 1e-4f);
    CHECK(std::abs(randSplit.r.offsetX - randSplit.b.offsetX) > 1e-4f);
  }
}

TEST_CASE("ShakeEffect serializes cleanly through Project JSON round-trip") {
  iota::model::Project proj;
  proj.version = "10";
  auto comp0 = iota::model::createDefaultComposition(0);
  auto shake = iota::model::createDefaultElement(iota::model::ElementType::ShakeEffect);
  shake.props.setString("wrapX", "Tile", "sel No Tile Reflect Clamp", "1_appearance");
  shake.props.setString("style", "Twitchy", "sel Normal Twitchy Jumpy", "2_shakeMaster");
  shake.props.setFloat("stillness", 0.65f, 0.0f, 1.0f, "2_shakeMaster");
  shake.props.setFloat("drift", 0.45f, 0.0f, 1.0f, "2_shakeMaster");
  shake.props.setFloat("centerBias", 2.5f, 0.0f, 10.0f, "2_shakeMaster");
  shake.props.setInt("octaves", 6, 1, 10, "2_shakeMaster");
  shake.props.setFloat("tiltPhase", 2.6f, -20.0f, 20.0f, "3_axes");
  shake.props.setFloat("redAmp", 1.4f, 0.0f, 3.0f, "4_channels");
  shake.props.setFloat("redPhase", 0.12f, -10.0f, 10.0f, "4_channels");
  shake.props.setFloat("rgbRandomness", 0.55f, 0.0f, 2.0f, "4_channels");
  shake.props.setFloat("rgbPhase", 0.35f, 0.0f, 1.0f, "4_channels");
  shake.props.setFloat("moBlurLength", 1.4f, 0.0f, 4.0f, "5_motionAndCrop");
  shake.props.setFloat("cropLeft", 0.15f, 0.0f, 0.45f, "5_motionAndCrop");
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
  CHECK(outEl.props.getString("style") == "Twitchy");
  CHECK(outEl.props.getFloat("stillness") == doctest::Approx(0.65f));
  CHECK(outEl.props.getFloat("drift") == doctest::Approx(0.45f));
  CHECK(outEl.props.getFloat("centerBias") == doctest::Approx(2.5f));
  CHECK(outEl.props.getInt("octaves") == 6);
  CHECK(outEl.props.getFloat("tiltPhase") == doctest::Approx(2.6f));
  CHECK(outEl.props.getFloat("redAmp") == doctest::Approx(1.4f));
  CHECK(outEl.props.getFloat("redPhase") == doctest::Approx(0.12f));
  CHECK(outEl.props.getFloat("rgbRandomness") == doctest::Approx(0.55f));
  CHECK(outEl.props.getFloat("moBlurLength") == doctest::Approx(1.4f));
  CHECK(outEl.props.getFloat("rgbPhase") == doctest::Approx(0.35f));
  CHECK(outEl.props.getFloat("cropLeft") == doctest::Approx(0.15f));
}
