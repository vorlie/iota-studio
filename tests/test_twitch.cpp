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

#include "iota/engine/twitch_eval.hpp"
#include "iota/model/scene.hpp"
#include "iota/preset_io/preset_io.hpp"

#include <cmath>

TEST_CASE("Twitch evaluator is deterministic, respects zero amount, toggles, and modes") {
  using namespace iota::engine;

  TwitchConfig cfg;
  cfg.amount = 1.0f;
  cfg.speed = 14.0f;
  cfg.threshold = 0.25f;
  cfg.randomSeed = 42.0f;
  cfg.enableSlide = true;
  cfg.enableScale = true;
  cfg.enableLight = true;
  cfg.enableColor = true;
  cfg.enableBlur = true;
  cfg.enableSlice = true;

  SUBCASE("Determinism across repeated evaluations and distinct seeds") {
    const auto a1 = evaluateTwitch(cfg, 1.375, 1.0 / 120.0);
    const auto a2 = evaluateTwitch(cfg, 1.375, 1.0 / 120.0);
    CHECK(a1.pose1.slideX == a2.pose1.slideX);
    CHECK(a1.pose1.slideY == a2.pose1.slideY);
    CHECK(a1.pose1.scale == a2.pose1.scale);
    CHECK(a1.pose1.tiltRad == a2.pose1.tiltRad);
    CHECK(a1.rgbSplitX == a2.rgbSplitX);
    CHECK(a1.lightDelta == a2.lightDelta);
    CHECK(a1.colorMix == a2.colorMix);
    CHECK(a1.blurRadius == a2.blurRadius);
    CHECK(a1.sliceStrength == a2.sliceStrength);

    TwitchConfig cfg2 = cfg;
    cfg2.randomSeed = 1337.0f;
    const auto b = evaluateTwitch(cfg2, 1.375, 1.0 / 120.0);
    const bool anyDiff = (a1.pose1.slideX != b.pose1.slideX) ||
                         (a1.lightDelta != b.lightDelta) ||
                         (a1.sliceStrength != b.sliceStrength);
    CHECK(anyDiff);
  }

  SUBCASE("Zero amount produces exact identity") {
    TwitchConfig zeroCfg = cfg;
    zeroCfg.amount = 0.0f;
    const auto st = evaluateTwitch(zeroCfg, 2.5, 1.0 / 120.0);
    CHECK(st.pose0.slideX == 0.0f);
    CHECK(st.pose1.slideX == 0.0f);
    CHECK(st.pose1.slideY == 0.0f);
    CHECK(st.pose1.scale == 1.0f);
    CHECK(st.pose1.tiltRad == 0.0f);
    CHECK(st.rgbSplitX == 0.0f);
    CHECK(st.rgbSplitY == 0.0f);
    CHECK(st.lightDelta == 0.0f);
    CHECK(st.colorMix == 0.0f);
    CHECK(st.blurRadius == 0.0f);
    CHECK(st.sliceStrength == 0.0f);
    CHECK(st.sliceRgbSplit == 0.0f);
  }

  SUBCASE("Operator toggles isolate cleanly") {
    TwitchConfig iso = cfg;
    iso.threshold = 0.0f;
    iso.enableSlide = false;
    iso.enableScale = false;
    iso.enableLight = false;
    iso.enableColor = false;
    iso.enableBlur = false;
    iso.enableSlice = false;
    const auto off = evaluateTwitch(iso, 0.85, 0.0);
    CHECK(off.pose1.slideX == 0.0f);
    CHECK(off.pose1.slideY == 0.0f);
    CHECK(off.pose1.scale == 1.0f);
    CHECK(off.pose1.tiltRad == 0.0f);
    CHECK(off.rgbSplitX == 0.0f);
    CHECK(off.lightDelta == 0.0f);
    CHECK(off.colorMix == 0.0f);
    CHECK(off.blurRadius == 0.0f);
    CHECK(off.sliceStrength == 0.0f);
  }

  SUBCASE("Direction and behavior invariants across all timing modes") {
    for (TwitchMode m : {TwitchMode::Random, TwitchMode::Pulse, TwitchMode::Stutter, TwitchMode::Smooth}) {
      TwitchConfig c = cfg;
      c.mode = m;
      c.threshold = 0.0f;

      c.slideDirection = TwitchSlideDirection::Horizontal;
      for (int i = 0; i < 10; ++i) {
        const auto s = evaluateTwitch(c, i * 0.13);
        CHECK(s.pose1.slideY == 0.0f);
      }

      c.slideDirection = TwitchSlideDirection::Vertical;
      for (int i = 0; i < 10; ++i) {
        const auto s = evaluateTwitch(c, i * 0.13);
        CHECK(s.pose1.slideX == 0.0f);
      }

      c.slideDirection = TwitchSlideDirection::Diagonal;
      for (int i = 0; i < 10; ++i) {
        const auto s = evaluateTwitch(c, i * 0.13);
        CHECK(std::abs(s.pose1.slideX) == doctest::Approx(std::abs(s.pose1.slideY)));
      }

      c.scaleDirection = TwitchScaleDirection::ZoomIn;
      for (int i = 0; i < 10; ++i) {
        CHECK(evaluateTwitch(c, i * 0.13).pose1.scale >= 1.0f);
      }

      c.scaleDirection = TwitchScaleDirection::ZoomOut;
      for (int i = 0; i < 10; ++i) {
        const float sc = evaluateTwitch(c, i * 0.13).pose1.scale;
        CHECK(sc <= 1.0f);
        CHECK(sc >= 0.2f);
      }

      c.lightBehavior = TwitchLightBehavior::Brighten;
      for (int i = 0; i < 10; ++i) {
        CHECK(evaluateTwitch(c, i * 0.13).lightDelta >= 0.0f);
      }

      c.lightBehavior = TwitchLightBehavior::Darken;
      for (int i = 0; i < 10; ++i) {
        CHECK(evaluateTwitch(c, i * 0.13).lightDelta <= 0.0f);
      }
    }
  }

  SUBCASE("Direct mode, directed slide axes, per-operator drives, and zero randomness") {
    TwitchConfig d;
    d.amount = 1.0f;
    d.mode = TwitchMode::Direct;
    d.slideAmount = 0.25f;
    d.slideRgbSplit = 0.0f;
    d.enableScale = false;
    d.enableLight = false;
    d.enableColor = false;
    d.enableBlur = false;
    d.enableSlice = false;

    d.slideDirection = TwitchSlideDirection::Up;
    const auto up1 = evaluateTwitch(d, 0.10);
    const auto up2 = evaluateTwitch(d, 3.75);
    CHECK(up1.pose1.slideX == 0.0f);
    CHECK(up1.pose1.slideY < -0.05f);
    CHECK(up1.pose1.slideY == doctest::Approx(up2.pose1.slideY));

    d.slideDirection = TwitchSlideDirection::Down;
    const auto down = evaluateTwitch(d, 0.50);
    CHECK(down.pose1.slideX == 0.0f);
    CHECK(down.pose1.slideY == doctest::Approx(-up1.pose1.slideY));

    d.slideDirection = TwitchSlideDirection::Left;
    const auto left = evaluateTwitch(d, 0.50);
    CHECK(left.pose1.slideY == 0.0f);
    CHECK(left.pose1.slideX < -0.05f);

    d.slideDirection = TwitchSlideDirection::Right;
    const auto right = evaluateTwitch(d, 0.50);
    CHECK(right.pose1.slideY == 0.0f);
    CHECK(right.pose1.slideX == doctest::Approx(-left.pose1.slideX));

    d.slideDirection = TwitchSlideDirection::CustomAngle;
    d.slideAngle = 90.0f;
    const auto ang90 = evaluateTwitch(d, 0.50);
    CHECK(ang90.pose1.slideX == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(ang90.pose1.slideY == doctest::Approx(up1.pose1.slideY));

    // Per-operator drive overrides master amount independently
    TwitchConfig splitCfg;
    splitCfg.amount = 0.0f;
    splitCfg.mode = TwitchMode::Direct;
    splitCfg.enableSlide = true;
    splitCfg.enableLight = true;
    splitCfg.slideDirection = TwitchSlideDirection::Up;
    splitCfg.slideDrive = 1.0f;
    splitCfg.lightDrive = 0.0f;
    const auto opRes = evaluateTwitch(splitCfg, 0.25);
    CHECK(opRes.pose1.slideY < 0.0f);
    CHECK(opRes.lightDelta == 0.0f);

    // Zero randomness in Random mode holds directed slide constant at fixed drive
    TwitchConfig noRand = d;
    noRand.mode = TwitchMode::Random;
    noRand.slideDirection = TwitchSlideDirection::Up;
    noRand.slideRandomness = 0.0f;
    CHECK(evaluateTwitch(noRand, 0.15).pose1.slideY == doctest::Approx(up1.pose1.slideY));
    CHECK(evaluateTwitch(noRand, 2.40).pose1.slideY == doctest::Approx(up1.pose1.slideY));
  }
}

TEST_CASE("TwitchEffect serializes cleanly through Project JSON round-trip") {
  using namespace iota::model;
  using namespace iota::preset_io;

  Project proj;
  proj.version = "10";
  auto comp0 = createDefaultComposition(0);
  auto tw = createDefaultElement(ElementType::TwitchEffect);
  CHECK(tw.props.getString("blendMode") == "Opaque");
  CHECK(tw.props.getString("TargetImage") == "composition:0");
  tw.props.setString("twitchMode", "Pulse", "sel Random Pulse Stutter Smooth", "2_twitchMaster");
  tw.props.setBool("enableSlice", true, "3_twitchEnable", "b");
  tw.props.setInt("sliceCount", 32, 2, 96, "6_twitchBlurSlice");
  comp0.elements.push_back(std::move(tw));
  proj.compositions.push_back(std::move(comp0));

  const std::string json = serializeProjectToJsonString(proj);
  std::string err;
  auto restored = parseProjectJsonString(json, &err);
  REQUIRE_MESSAGE(restored.has_value(), err);
  REQUIRE(restored->compositions.size() == 1);
  REQUIRE(restored->compositions[0].elements.size() == 1);

  const auto& outEl = restored->compositions[0].elements[0];
  CHECK(outEl.type == ElementType::TwitchEffect);
  CHECK(outEl.props.getString("twitchMode") == "Pulse");
  CHECK(outEl.props.getBool("enableSlice"));
  CHECK(outEl.props.getInt("sliceCount") == 32);
}
