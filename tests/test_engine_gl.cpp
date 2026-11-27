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

#include "iota/audio/audio_decoder.hpp"
#include "iota/audio/audio_engine.hpp"
#include "iota/core/utf8_path.hpp"
#include "iota/engine/builtin_shader_library.hpp"
#include "iota/engine/font_atlas.hpp"
#include "iota/engine/layout.hpp"
#include "iota/engine/scene_renderer.hpp"
#include "iota/model/scene.hpp"
#include "iota/preset_io/preset_io.hpp"
#include "iota/preset_io/project_manager.hpp"
#include "iota/rhi/gl45_device.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

TEST_CASE("Layout coordinate math and 16 text tokens work accurately") {
  using namespace iota::engine;

  LayoutSpec spec;
  spec.position = {0.5f, 0.5f};
  spec.alignmentPosition = {0.5f, 0.5f};
  spec.scale = {0.5f, 0.5f};
  spec.scaleIsUniform = false;
  const auto rect = computeElementRect(spec, 1280, 720);
  CHECK(rect.left == doctest::Approx(320.0f));
  CHECK(rect.top == doctest::Approx(180.0f));
  CHECK(rect.width() == doctest::Approx(640.0f));
  CHECK(rect.height() == doctest::Approx(360.0f));

  TextTokenContext tctx;
  tctx.timeCurrentSec = 65.0;
  tctx.durationSec = 185.0;
  tctx.artist = "Artist";
  tctx.title = "Song";
  tctx.album = "Album";
  tctx.fps = 60.0f;
  const std::string out = expandTextTokens("{TimeCurrent}/{TimeLength} {ArtistAndTitle} ({Fps})", tctx);
  CHECK(out == "1:05/3:05 Artist - Song (60)");
}

TEST_CASE("Headless OpenGL 4.5 SceneRenderer renders starter and built-in presets") {
  if (!glfwInit()) {
    FAIL("glfwInit failed");
    return;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

  GLFWwindow* win = glfwCreateWindow(256, 256, "iota_gl_test", nullptr, nullptr);
  if (!win) {
    glfwTerminate();
    FAIL("Failed to create OpenGL 4.5 hidden test window");
    return;
  }
  glfwMakeContextCurrent(win);

  iota::rhi::Gl45RenderDevice device;
  std::string err;
  REQUIRE_MESSAGE(device.initialize(&err), err);

  iota::engine::SceneRenderer renderer;
  REQUIRE_MESSAGE(renderer.initialize(&device, &err), err);

  iota::audio::AudioEngine audio;
  audio.loadTrack(iota::audio::createSyntheticSineTrack(120.0f, 3.0, 44100, 0.5f, "Test Bass"));

  SUBCASE("Starter project produces non-empty output") {
    const auto proj = iota::model::createStarterProject();
    iota::engine::RenderFrameStats stats;
    for (int f = 0; f < 4; ++f) {
      iota::engine::FrameTiming timing{1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f};
      const auto rt = renderer.renderFrame(proj, audio, timing, 320, 180, {}, &stats);
      REQUIRE(rt);
    }
    CHECK(stats.drawCalls > 0);

    std::vector<uint8_t> rgba;
    int rw = 0, rh = 0;
    REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, rgba, true));
    CHECK(rw == 320);
    CHECK(rh == 180);
    CHECK(rgba.size() == 320u * 180u * 4u);

    uint64_t lumaSum = 0;
    for (size_t i = 0; i < rgba.size(); i += 4) {
      lumaSum += rgba[i] + rgba[i + 1] + rgba[i + 2];
    }
    CHECK(lumaSum > 0);
  }

  SUBCASE("Built-in complex presets 0, 4 (RgbSplit), and 17 (Mirror+4 Comps) render without errors") {
    const char* dirEnv = std::getenv("IOTA_TEST_PRESETS_DIR");
    if (dirEnv == nullptr) {
      MESSAGE("Skipping built-in APK presets GL subcase because IOTA_TEST_PRESETS_DIR is not set.");
    } else {
    const std::filesystem::path dir(dirEnv);

    for (int id : {0, 4, 17}) {
      const auto p = dir / ("visualizer_" + std::to_string(id) + ".json");
      auto proj = iota::preset_io::loadProjectFromFile(p, &err);
      REQUIRE_MESSAGE(proj.has_value(), err);

      renderer.resetSimulation();
      iota::engine::RenderFrameStats stats;
      iota::engine::FrameTiming timing{1.0f / 60.0f, 0.5, 0.5, true, 0.0f, true, 60.0f};
      const auto rt = renderer.renderFrame(*proj, audio, timing, 320, 180, {}, &stats);
      REQUIRE(rt);
      CHECK(stats.lastError.empty());
      CHECK(stats.compositionsRendered == static_cast<int>(proj->compositions.size()));
    }
    }
  }

  const char* tmplEnv = std::getenv("IOTA_TEST_TEMPLATE_PATH");
  const std::filesystem::path userTemplate =
      tmplEnv ? std::filesystem::path(tmplEnv)
              : std::filesystem::path("test_template/SPECIAL FOR 300SUBS.viz");
  if (std::filesystem::exists(userTemplate)) {
    SUBCASE("User .viz template renders audio-reactive circular Line bars") {
      auto proj = iota::preset_io::loadProjectFromFile(userTemplate, &err);
      REQUIRE_MESSAGE(proj.has_value(), err);

      iota::audio::AudioEngine silentAudio;
      iota::audio::AudioTrackData silentTrack;
      silentTrack.sampleRate = 44100;
      silentTrack.channels = 2;
      silentTrack.interleavedPcm.assign(44100u * 2u, 0.0f);
      iota::audio::rebuildMonoAndDuration(silentTrack);
      silentAudio.loadTrack(std::move(silentTrack));

      renderer.resetSimulation();
      std::vector<uint8_t> silentRgba;
      int rw = 0, rh = 0;
      for (int f = 0; f < 3; ++f) {
        iota::engine::FrameTiming timing{1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f};
        REQUIRE(renderer.renderFrame(*proj, silentAudio, timing, 640, 360));
      }
      REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, silentRgba, true));

      iota::audio::AudioEngine bassAudio;
      bassAudio.loadTrack(iota::audio::createSyntheticSineTrack(110.0f, 3.0, 44100, 0.75f, "110 Hz Bass"));
      renderer.resetSimulation();
      std::vector<uint8_t> bassRgba;
      iota::engine::RenderFrameStats stats;
      for (int f = 0; f < 10; ++f) {
        iota::engine::FrameTiming timing{1.0f / 60.0f, (f + 10) / 60.0, (f + 10) / 60.0, true, 0.0f, true, 60.0f};
        REQUIRE(renderer.renderFrame(*proj, bassAudio, timing, 640, 360, {}, &stats));
      }
      REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, bassRgba, true));
      REQUIRE(silentRgba.size() == bassRgba.size());

      uint64_t ringDiffSum = 0;
      for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
          const float dx = static_cast<float>(x) - 320.0f;
          const float dy = static_cast<float>(y) - 180.0f;
          const float dist = std::hypot(dx, dy);
          // Outside the r=90px center logo circle where mirrored Bars waves extend
          if (dist >= 95.0f && dist <= 175.0f) {
            const size_t idx = static_cast<size_t>((y * rw + x) * 4);
            ringDiffSum += static_cast<uint64_t>(std::abs(static_cast<int>(bassRgba[idx + 0]) -
                                                           static_cast<int>(silentRgba[idx + 0])));
            ringDiffSum += static_cast<uint64_t>(std::abs(static_cast<int>(bassRgba[idx + 1]) -
                                                           static_cast<int>(silentRgba[idx + 1])));
            ringDiffSum += static_cast<uint64_t>(std::abs(static_cast<int>(bassRgba[idx + 2]) -
                                                           static_cast<int>(silentRgba[idx + 2])));
          }
        }
      }
      CHECK(stats.compositionsRendered == static_cast<int>(proj->compositions.size()));
      CHECK(ringDiffSum > 50000);
    }
  }

  SUBCASE("Bars CatmullRom curve interpolation renders cleanly") {
    auto proj = iota::model::createStarterProject();
    REQUIRE(!proj.compositions.empty());
    bool configuredAudio = false;
    bool configuredBars = false;
    for (auto& el : proj.compositions[0].elements) {
      if (el.type == iota::model::ElementType::AudioProvider) {
        CHECK(el.props.contains("temporalSmooth"));
        CHECK(el.props.contains("freqSmoothMode"));
        el.props.setString("freqSmoothMode", "Gaussian",
                           "sel None MovingAverage Gaussian SavitzkyGolay",
                           "3_frequencySmooth");
        configuredAudio = true;
      } else if (el.type == iota::model::ElementType::Bars) {
        CHECK(el.props.contains("curveInterpolation"));
        el.props.setString("curveInterpolation", "CatmullRom",
                           "sel None Linear CatmullRom CubicBSpline", "1_bars");
        el.props.setInt("curveSubdivisions", 4, 1, 8, "1_bars");
        el.props.getOrAdd("Segment1").value = std::string("Line");
        configuredBars = true;
      }
    }
    CHECK(configuredAudio);
    CHECK(configuredBars);

    renderer.resetSimulation();
    iota::engine::RenderFrameStats stats;
    for (int f = 0; f < 6; ++f) {
      iota::engine::FrameTiming timing{1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f};
      REQUIRE(renderer.renderFrame(proj, audio, timing, 320, 240, {}, &stats));
    }
    CHECK(stats.drawCalls > 0);

    std::vector<uint8_t> rgba;
    int rw = 0;
    int rh = 0;
    REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, rgba, true));
    uint64_t lumaSum = 0;
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      lumaSum += rgba[i] + rgba[i + 1] + rgba[i + 2];
    }
    CHECK(lumaSum > 50000);
  }

  SUBCASE("SoundKeys drives BlurEffect and RgbSplitEffect from selected spectrum range") {
    iota::model::Project proj;
    proj.version = "10";

    auto comp0 = iota::model::createDefaultComposition(0);
    auto blurEl = iota::model::createDefaultElement(iota::model::ElementType::BlurEffect);
    blurEl.props.setInt("sourceCompositionIndex", 1, 1, 5, "1_appearance");
    blurEl.props.setMVar("measureBlurRadius",
                         iota::model::MVariableFloat{{{"SoundKeys1", 2.0f, 0.0f}}},
                         "mvarf 0.0 3.0",
                         "2_blur");
    comp0.elements.push_back(std::move(blurEl));

    auto splitEl = iota::model::createDefaultElement(iota::model::ElementType::RgbSplitEffect);
    splitEl.props.setString("targetImage", "composition:1", "img composition:0", "1_appearance");
    splitEl.props.setMVar("splitMultiplier",
                          iota::model::MVariableFloat{{{"SoundKeys1", 3.0f, 0.0f}}},
                          "mvarf 0.0 6.0",
                          "2_splitEffect");
    splitEl.props.setMVar("splitAmount",
                          iota::model::MVariableFloat{{{"SoundKeysShake1", 0.5f, 0.5f}}},
                          "mvarf2 -0.5 0.5",
                          "2_splitEffect");
    comp0.elements.push_back(std::move(splitEl));

    auto comp1 = iota::model::createDefaultComposition(1);
    comp1.elements.push_back(iota::model::createDefaultElement(iota::model::ElementType::AudioProvider));
    comp1.elements.push_back(iota::model::createDefaultElement(iota::model::ElementType::SoundKeys));
    comp1.elements.push_back(iota::model::createDefaultElement(iota::model::ElementType::Bars));

    proj.compositions.push_back(std::move(comp0));
    proj.compositions.push_back(std::move(comp1));

    renderer.resetSimulation();
    iota::engine::RenderFrameStats stats;
    for (int f = 0; f < 15; ++f) {
      iota::engine::FrameTiming timing{1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f};
      REQUIRE(renderer.renderFrame(proj, audio, timing, 320, 240, {}, &stats));
    }
    CHECK(stats.compositionsRendered == 2);
    CHECK(stats.drawCalls > 0);
    CHECK(stats.soundKeysOutputs[0] > 0.3f);
    CHECK(stats.soundKeysOutputs[0] < 0.95f);
    CHECK(stats.soundKeysOutputs[2] < 0.05f);
    CHECK(stats.soundKeysCeiling >= stats.soundKeysPeak * 1.2f);
    CHECK(stats.soundKeysSpectrum.size() >= 32);
    CHECK(stats.soundKeysBarFreqsHz.size() == stats.soundKeysSpectrum.size());

    std::vector<uint8_t> rgba;
    int rw = 0, rh = 0;
    REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, rgba, true));
    uint64_t lumaSum = 0;
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      lumaSum += rgba[i] + rgba[i + 1] + rgba[i + 2];
    }
    CHECK(lumaSum > 50000);
  }

  SUBCASE("SceneRenderer resolves AssetThumbnail previews and renders DualCompGlow template") {
    auto proj = iota::preset_io::createProjectFromTemplate(
        iota::preset_io::ProjectTemplateKind::DualCompGlow, "Glow Test", 320, 240, 60);

    // 2x2 uncompressed 24-bit BMP image payload (70 bytes)
    const std::vector<uint8_t> tinyBmp = {
        'B', 'M', 70, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
        40, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        255, 0, 0, 0, 255, 0, 0, 0,
        0, 0, 255, 255, 255, 255, 0, 0,
    };
    proj.localAssets["tiny.bmp"] = tinyBmp;

    renderer.resetSimulation();
    iota::engine::RenderFrameStats stats;
    for (int f = 0; f < 6; ++f) {
      iota::engine::FrameTiming timing{1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f};
      REQUIRE(renderer.renderFrame(proj, audio, timing, 320, 240, {}, &stats));
    }
    CHECK(stats.compositionsRendered == 2);
    CHECK(stats.drawCalls > 0);

    const auto builtInThumb = renderer.resolveAssetPreview(proj, "internalres:lens_flare");
    CHECK(builtInThumb.texture);
    CHECK(builtInThumb.width == 128);
    CHECK(builtInThumb.height == 128);
    CHECK_FALSE(builtInThumb.flipV);

    const auto localThumb = renderer.resolveAssetPreview(proj, "local:tiny.bmp");
    CHECK(localThumb.texture);
    CHECK(localThumb.width == 2);
    CHECK(localThumb.height == 2);
    CHECK_FALSE(localThumb.flipV);

    const auto compThumb = renderer.resolveAssetPreview(proj, "composition:1");
    CHECK(compThumb.texture);
    CHECK(compThumb.width == 320);
    CHECK(compThumb.height == 240);
    CHECK(compThumb.flipV);

    // Unicode filesystem image path + malformed invalid-byte fallback must never throw
    const std::filesystem::path unicodeBmp =
        std::filesystem::temp_directory_path() /
        iota::core::pathFromUtf8("iota_za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87_\xE9\x9F\xB3\xE6\xA5\xBD_\xF0\x9F\x8E\xB5.bmp");
    {
      std::ofstream bmpOut(unicodeBmp, std::ios::binary | std::ios::trunc);
      REQUIRE(bmpOut.is_open());
      bmpOut.write(reinterpret_cast<const char*>(tinyBmp.data()), static_cast<std::streamsize>(tinyBmp.size()));
      REQUIRE(bmpOut.good());
    }
    const auto unicodeDiskThumb = renderer.resolveAssetPreview(proj, iota::core::pathToUtf8(unicodeBmp));
    std::error_code ec;
    std::filesystem::remove(unicodeBmp, ec);
    CHECK(unicodeDiskThumb.texture);
    CHECK(unicodeDiskThumb.width == 2);
    CHECK(unicodeDiskThumb.height == 2);

    const auto badPathThumb = renderer.resolveAssetPreview(proj, "C:/nonexistent:*?\xFF/bad.png");
    CHECK(badPathThumb.texture);
  }

  SUBCASE("Proper BlurEffect (DualKawase/Gaussian/Radial/Directional/Canvas/Image) and MotionBlurEffect (FPS-independent/Manual/Temporal)") {
    constexpr int kW = 320;
    constexpr int kH = 240;

    auto readRgba = [&]() {
      std::vector<uint8_t> buf;
      int rw = 0, rh = 0;
      REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, buf, true));
      REQUIRE(rw == kW);
      REQUIRE(rh == kH);
      return buf;
    };
    auto lumaAt = [](const std::vector<uint8_t>& buf, int x, int y) -> float {
      const size_t off = static_cast<size_t>((y * kW + x) * 4);
      return (static_cast<float>(buf[off]) + buf[off + 1] + buf[off + 2]) / 3.0f;
    };

    auto makeBoxProject = [&](iota::model::Element effectEl) {
      iota::model::Project p;
      p.version = "10";
      auto comp0 = iota::model::createDefaultComposition(0);
      comp0.elements.push_back(std::move(effectEl));

      auto comp1 = iota::model::createDefaultComposition(1);
      auto sq = iota::model::createDefaultElement(iota::model::ElementType::Image);
      sq.props.setString("customImage", "internalres:white", "img", "1_image");
      sq.props.setBool("scaleIsUniform", true, "0_general");
      sq.props.setVec2("scale", {0.25f, 0.25f}, 0.0f, 2.0f, "0_general");  // 60x60 centered at (160,120)
      sq.props.setVec2("position", {0.5f, 0.5f}, -1.0f, 1.0f, "0_general");
      comp1.elements.push_back(std::move(sq));

      p.compositions.push_back(std::move(comp0));
      p.compositions.push_back(std::move(comp1));
      return p;
    };

    // 1. BlurEffect: sharp at radius 0, progressive spread across all 4 blur types, and legacy MaskImage fix
    auto makeBlurEl = [&](std::string_view type, float radius, float angle = 0.0f) {
      auto blur = iota::model::createDefaultElement(iota::model::ElementType::BlurEffect);
      blur.props.setInt("sourceCompositionIndex", 1, 0, 5, "1_appearance");
      blur.props.setString("blendMode", "Opaque", "_child Alpha Opaque", "0_general");
      blur.props.setBool("showUnblurredContent", false, "1_appearance");
      blur.props.setString("MaskImage", "internalres:transparent", "img", "1_appearance");
      blur.props.setColorArgb("color", {0, 0, 0, 0}, "crgba", "1_appearance");
      blur.props.setVec2("1layerScale", {1.0f, 1.0f}, 0.0f, 10.0f, "2_blur");
      blur.props.setVec2("2layerScale", {0.0f, 0.0f}, 0.0f, 10.0f, "2_blur");
      blur.props.setVec2("3layerScale", {0.0f, 0.0f}, 0.0f, 10.0f, "2_blur");
      blur.props.setString("blurType", type, "sel DualKawase Gaussian Radial Directional", "2_blur");
      blur.props.setFloat("blurRadius", radius, 0.0f, 6.0f, "2_blur");
      blur.props.setFloat("blurMultiplier", 4.0f, 1.0f, 6.0f, "2_blur");
      blur.props.setFloat("blurAngle", angle, 0.0f, 360.0f, "2_blur");
      return blur;
    };

    iota::engine::FrameTiming staticTiming{1.0f / 60.0f, 0.1, 0.1, true, 0.0f, false, 60.0f};
    renderer.resetSimulation();

    {
      auto pZero = makeBoxProject(makeBlurEl("DualKawase", 0.0f));
      REQUIRE(renderer.renderFrame(pZero, audio, staticTiming, kW, kH));
      const auto px = readRgba();
      CHECK(lumaAt(px, 160, 120) > 240.0f);
      CHECK(lumaAt(px, 198, 120) < 2.0f);
    }

    float smallSpreadFar = 0.0f;
    {
      auto pSmall = makeBoxProject(makeBlurEl("DualKawase", 0.6f));
      REQUIRE(renderer.renderFrame(pSmall, audio, staticTiming, kW, kH));
      const auto px = readRgba();
      CHECK(lumaAt(px, 192, 120) > 10.0f);
      smallSpreadFar = lumaAt(px, 202, 120);
    }

    {
      auto pWide = makeBoxProject(makeBlurEl("DualKawase", 5.0f));
      REQUIRE(renderer.renderFrame(pWide, audio, staticTiming, kW, kH));
      const auto px = readRgba();
      CHECK(lumaAt(px, 202, 120) > smallSpreadFar + 15.0f);
    }

    for (const std::string mode : {"Gaussian", "Radial", "Directional"}) {
      auto pMode = makeBoxProject(makeBlurEl(mode, 3.0f, 0.0f));
      REQUIRE(renderer.renderFrame(pMode, audio, staticTiming, kW, kH));
      const auto px = readRgba();
      CHECK_MESSAGE(lumaAt(px, 194, 120) > 15.0f, "Mode failed to blur outward: " << mode);
      if (mode == "Directional") {
        CHECK(lumaAt(px, 160, 158) < 5.0f);  // 0-deg horizontal directional blur stays sharp vertically
      }
    }

    // 2. Single-comp Canvas-Below BlurEffect + Image layer blurEnabled
    {
      iota::model::Project singleComp;
      singleComp.version = "10";
      auto comp0 = iota::model::createDefaultComposition(0);
      auto sq = iota::model::createDefaultElement(iota::model::ElementType::Image);
      sq.props.setString("customImage", "internalres:white", "img", "1_image");
      sq.props.setBool("scaleIsUniform", true, "0_general");
      sq.props.setVec2("scale", {0.25f, 0.25f}, 0.0f, 2.0f, "0_general");
      comp0.elements.push_back(std::move(sq));
      auto blurCanvas = makeBlurEl("DualKawase", 3.0f);
      blurCanvas.props.setInt("sourceCompositionIndex", 0, 0, 5, "1_appearance");
      comp0.elements.push_back(std::move(blurCanvas));
      singleComp.compositions.push_back(std::move(comp0));

      REQUIRE(renderer.renderFrame(singleComp, audio, staticTiming, kW, kH));
      const auto px = readRgba();
      CHECK(lumaAt(px, 160, 120) > 120.0f);
      CHECK(lumaAt(px, 196, 120) > 15.0f);
    }

    {
      // 64x64 uncompressed 24-bit BMP with centered 16x16 white square on black background
      constexpr int kBmpSize = 64;
      std::vector<uint8_t> spotBmp(54 + kBmpSize * kBmpSize * 3, 0);
      spotBmp[0] = 'B';
      spotBmp[1] = 'M';
      const uint32_t fileSz = static_cast<uint32_t>(spotBmp.size());
      spotBmp[2] = static_cast<uint8_t>(fileSz & 0xFF);
      spotBmp[3] = static_cast<uint8_t>((fileSz >> 8) & 0xFF);
      spotBmp[4] = static_cast<uint8_t>((fileSz >> 16) & 0xFF);
      spotBmp[5] = static_cast<uint8_t>((fileSz >> 24) & 0xFF);
      spotBmp[10] = 54;
      spotBmp[14] = 40;
      spotBmp[18] = kBmpSize;
      spotBmp[22] = kBmpSize;
      spotBmp[26] = 1;
      spotBmp[28] = 24;
      for (int y = 24; y < 40; ++y) {
        for (int x = 24; x < 40; ++x) {
          const size_t p = 54 + static_cast<size_t>((y * kBmpSize + x) * 3);
          spotBmp[p + 0] = 255;
          spotBmp[p + 1] = 255;
          spotBmp[p + 2] = 255;
        }
      }

      iota::model::Project imgBlurProj;
      imgBlurProj.version = "10";
      imgBlurProj.localAssets["spot.bmp"] = std::move(spotBmp);
      auto comp0 = iota::model::createDefaultComposition(0);
      auto img = iota::model::createDefaultElement(iota::model::ElementType::Image);
      img.props.setString("customImage", "local:spot.bmp", "img", "1_image");
      img.props.setBool("scaleIsUniform", true, "0_general");
      img.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");
      img.props.setBool("blurEnabled", false, "blur");
      img.props.setInt("blurDivider", 6, 0, 10, "blur");
      img.props.setInt("blurRadius", 18, 1, 25, "blur");
      comp0.elements.push_back(std::move(img));
      imgBlurProj.compositions.push_back(std::move(comp0));

      REQUIRE(renderer.renderFrame(imgBlurProj, audio, staticTiming, kW, kH));
      const auto sharpPx = readRgba();
      CHECK(lumaAt(sharpPx, 122, 120) < 2.0f);   // Outside white square in dark surround
      CHECK(lumaAt(sharpPx, 134, 120) > 240.0f); // Inside white square near edge

      imgBlurProj.compositions[0].elements[0].props.setBool("blurEnabled", true, "blur");
      REQUIRE(renderer.renderFrame(imgBlurProj, audio, staticTiming, kW, kH));
      const auto blurPx = readRgba();
      CHECK(lumaAt(blurPx, 122, 120) > 15.0f);   // Blurred spot bleeds outward
      CHECK(lumaAt(blurPx, 134, 120) < lumaAt(sharpPx, 134, 120) - 25.0f);
    }

    // 3. MotionBlurEffect: frame-rate-independent EffectTransform (60 FPS vs 144 FPS)
    auto makeMotionProject = [&](float multVal) {
      auto mb = iota::model::createDefaultElement(iota::model::ElementType::MotionBlurEffect);
      mb.props.setString("TargetImage", "composition:1", "img composition:0", "1_appearance");
      mb.props.setString("blendMode", "Opaque", "_child Alpha Opaque", "0_general");
      mb.props.setString("motionSource", "EffectTransform", "_child EffectTransform Manual Temporal Hybrid", "2_motionBlur");
      mb.props.setMVar("blurAmountMultiplier",
                       iota::model::MVariableFloat::constant(multVal),
                       "mvarf 0.0 3.0",
                       "2_motionBlur");
      mb.props.setFloat("shutterAngle", 360.0f, 0.0f, 720.0f, "2_motionBlur");
      mb.props.setInt("blurSamples", 32, 8, 64, "2_motionBlur");
      auto p = makeBoxProject(std::move(mb));
      p.desktopTracks.push_back({0, 0, "position.x", {{0.0, 0.40f}, {1.0, 2.40f}}});  // 2.0 width/sec
      return p;
    };

    auto runFpsMotion = [&](int frames, float multVal) {
      auto p = makeMotionProject(multVal);
      renderer.resetSimulation();
      const double totalDur = 5.0 / 60.0;  // 5/60 s == 12/144 s exactly
      const float dt = static_cast<float>(totalDur / static_cast<double>(frames));
      for (int f = 0; f <= frames; ++f) {
        const double t = static_cast<double>(f) * dt;
        iota::engine::FrameTiming tm{dt, t, t, true, 0.0f, false, 1.0f / dt};
        REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH));
      }
      return readRgba();
    };

    auto measureActiveWidth = [&](const std::vector<uint8_t>& buf) {
      int lit = 0;
      for (int x = 0; x < kW; ++x) {
        if (lumaAt(buf, x, 120) > 15.0f) ++lit;
      }
      return lit;
    };

    const auto mbStaticPx = runFpsMotion(5, 0.0f);
    const auto mb60Px = runFpsMotion(5, 1.0f);
    const auto mb144Px = runFpsMotion(12, 1.0f);
    const int staticW = measureActiveWidth(mbStaticPx);
    const int blur60W = measureActiveWidth(mb60Px);
    const int blur144W = measureActiveWidth(mb144Px);
    CHECK(blur60W >= staticW + 6);
    CHECK(std::abs(blur60W - blur144W) <= 2);

    // 4. MotionBlurEffect: Manual directional mode produces blur on static input
    {
      auto mb = iota::model::createDefaultElement(iota::model::ElementType::MotionBlurEffect);
      mb.props.setString("TargetImage", "composition:1", "img composition:0", "1_appearance");
      mb.props.setString("blendMode", "Opaque", "_child Alpha Opaque", "0_general");
      mb.props.setString("motionSource", "Manual", "_child EffectTransform Manual Temporal Hybrid", "2_motionBlur");
      mb.props.setFloat("manualAngle", 0.0f, 0.0f, 360.0f, "2_motionBlur");
      mb.props.setFloat("manualDirectional", 0.16f, 0.0f, 0.5f, "2_motionBlur");
      mb.props.setFloat("manualZoom", 0.0f, -0.5f, 0.5f, "2_motionBlur");
      mb.props.setInt("blurSamples", 24, 8, 64, "2_motionBlur");
      auto pManual = makeBoxProject(std::move(mb));
      renderer.resetSimulation();
      REQUIRE(renderer.renderFrame(pManual, audio, staticTiming, kW, kH));
      const auto px = readRgba();
      CHECK(lumaAt(px, 198, 120) > 25.0f);
    }

    // 5. MotionBlurEffect: Temporal trail accumulation is frame-rate-independent (60 FPS vs 144 FPS)
    auto runTemporalFade = [&](int decayFrames) {
      iota::model::Project tp;
      tp.version = "10";
      auto comp0 = iota::model::createDefaultComposition(0);
      auto sq = iota::model::createDefaultElement(iota::model::ElementType::Image);
      sq.props.setString("customImage", "internalres:white", "img", "1_image");
      sq.props.setBool("scaleIsUniform", true, "0_general");
      sq.props.setVec2("scale", {0.25f, 0.25f}, 0.0f, 2.0f, "0_general");
      comp0.elements.push_back(std::move(sq));

      auto mb = iota::model::createDefaultElement(iota::model::ElementType::MotionBlurEffect);
      mb.props.setString("TargetImage", "composition:0", "img composition:0", "1_appearance");
      mb.props.setString("blendMode", "Opaque", "_child Alpha Opaque", "0_general");
      mb.props.setString("motionSource", "Temporal", "_child EffectTransform Manual Temporal Hybrid", "2_motionBlur");
      mb.props.setFloat("trailDecayMs", 150.0f, 10.0f, 1000.0f, "2_motionBlur");
      mb.props.setString("trailBlend", "Mix", "sel Mix Lighten Screen", "2_motionBlur");
      mb.props.setMVar("blurAmountMultiplier", iota::model::MVariableFloat::constant(1.0f), "mvarf 0.0 3.0", "2_motionBlur");
      comp0.elements.push_back(std::move(mb));
      tp.compositions.push_back(std::move(comp0));

      renderer.resetSimulation();
      const float dt = static_cast<float>((5.0 / 60.0) / static_cast<double>(decayFrames));
      REQUIRE(renderer.renderFrame(tp, audio, {dt, 0.0, 0.0, true, 0.0f, false, 1.0f / dt}, kW, kH));
      tp.compositions[0].elements[0].props.setBool("visible", false, "0_general");
      for (int f = 1; f <= decayFrames; ++f) {
        const double t = static_cast<double>(f) * dt;
        REQUIRE(renderer.renderFrame(tp, audio, {dt, t, t, true, 0.0f, false, 1.0f / dt}, kW, kH));
      }
      return lumaAt(readRgba(), 160, 120);
    };

    const float temp60 = runTemporalFade(5);
    const float temp144 = runTemporalFade(12);
    CHECK(temp60 > 90.0f);
    CHECK(temp60 < 200.0f);
    CHECK(std::abs(temp60 - temp144) < 12.0f);
  }

  SUBCASE("Particular 3D Particles: procedural shapes, DoF bokeh, curl turbulence, aux trails, and LayerMask emitter") {
    constexpr int kW = 320;
    constexpr int kH = 240;
    auto measureTotalLuma = [&](std::vector<uint8_t>& outBuf) {
      int rw = 0, rh = 0;
      REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, outBuf, true));
      REQUIRE(rw == kW);
      REQUIRE(rh == kH);
      uint64_t sum = 0;
      for (size_t i = 0; i + 3 < outBuf.size(); i += 4) {
        sum += outBuf[i] + outBuf[i + 1] + outBuf[i + 2];
      }
      return sum;
    };

    // 1. All procedural particle shapes + Sprite render cleanly with preRunSec & curl turbulence
    for (const std::string shape : {"Sprite", "Sphere", "GlowSphere", "Star", "BokehRing", "Streaklet"}) {
      iota::model::Project p;
      p.version = "10";
      auto comp0 = iota::model::createDefaultComposition(0);
      auto pt = iota::model::createDefaultElement(iota::model::ElementType::Particles);
      pt.props.setString("particleShape", shape,
                         "sel Sprite Sphere GlowSphere Star BokehRing Streaklet", "appearance");
      pt.props.getOrAdd("SpawnArea").value = std::string("Sphere");
      pt.props.setFloat("particlesPerSec", 140.0f, 0.0f, 5000.0f, "1_overall");
      pt.props.setFloat("preRunSec", 0.5f, 0.0f, 10.0f, "1_overall");
      pt.props.setFloat("particleScale", 5.5f, 0.25f, 16.0f, "appearance");
      pt.props.setFloat("turbulenceAffectPos", 50.0f, 0.0f, 500.0f, "4_turbulence");
      pt.props.setFloat("trailLength", shape == "Streaklet" ? 1.5f : 0.0f, 0.0f, 5.0f, "appearance");
      pt.props.setColorHsla("ColorFrom", {0.55f, 0.9f, 0.70f, 0.95f}, "appearance");
      pt.props.setColorHsla("ColorTo", {0.85f, 0.9f, 0.60f, 0.40f}, "appearance");
      comp0.elements.push_back(std::move(pt));
      p.compositions.push_back(std::move(comp0));

      renderer.resetSimulation();
      iota::engine::RenderFrameStats st;
      REQUIRE(renderer.renderFrame(p, audio, {1.0f / 60.0f, 0.1, 0.1, true, 0.0f, false, 60.0f}, kW, kH, {}, &st));
      std::vector<uint8_t> rgba;
      const uint64_t luma = measureTotalLuma(rgba);
      CHECK_MESSAGE(st.activeParticles > 20, "Shape failed to emit: " << shape);
      CHECK_MESSAGE(luma > 5000, "Shape produced dark frame: " << shape);
    }

    // 2. LayerMask emitter samples colors directly from Comp 1 (cyan rectangle)
    {
      iota::model::Project lm;
      lm.version = "10";
      auto comp0 = iota::model::createDefaultComposition(0);
      auto pt = iota::model::createDefaultElement(iota::model::ElementType::Particles);
      pt.props.getOrAdd("SpawnArea").value = std::string("LayerMask");
      pt.props.setString("emitterLayer", "composition:1", "img composition:0", "1_overall");
      pt.props.setString("particleShape", "Sphere", "sel Sprite Sphere GlowSphere Star BokehRing Streaklet", "appearance");
      pt.props.setFloat("particlesPerSec", 400.0f, 0.0f, 5000.0f, "1_overall");
      pt.props.setFloat("preRunSec", 0.5f, 0.0f, 10.0f, "1_overall");
      pt.props.setFloat("Speed", 5.0f, -500.0f, 500.0f, "behaviour");
      pt.props.setFloat("speedRandom", 2.0f, 0.0f, 500.0f, "behaviour");
      pt.props.setFloat("particleScale", 5.0f, 0.25f, 16.0f, "appearance");
      pt.props.setColorHsla("ColorFrom", {0.0f, 0.0f, 1.0f, 1.0f}, "appearance");
      pt.props.setColorHsla("ColorTo", {0.0f, 0.0f, 1.0f, 1.0f}, "appearance");
      pt.props.setString("opacityOverLife", "Constant", "sel FadeInOut Linear EaseOut EaseIn Pulse Constant", "appearance");
      comp0.elements.push_back(std::move(pt));

      auto comp1 = iota::model::createDefaultComposition(1);
      auto cyan = iota::model::createDefaultElement(iota::model::ElementType::Image);
      cyan.props.setString("customImage", "internalres:white", "img", "1_image");
      cyan.props.setVec2("position", {0.75f, 0.5f}, -1.0f, 1.0f, "0_general");
      cyan.props.setVec2("scale", {0.4f, 0.8f}, 0.0f, 2.0f, "0_general");
      cyan.props.setColorHsla("Color", {0.50f, 1.0f, 0.50f, 1.0f}, "2_color");  // Pure cyan RGB(0,255,255)
      cyan.props.setColorHsla("ColorTo", {0.50f, 1.0f, 0.50f, 1.0f}, "2_color");
      comp1.elements.push_back(std::move(cyan));

      lm.compositions.push_back(std::move(comp0));
      lm.compositions.push_back(std::move(comp1));

      renderer.resetSimulation();
      iota::engine::RenderFrameStats st;
      REQUIRE(renderer.renderFrame(lm, audio, {1.0f / 60.0f, 0.1, 0.1, true, 0.0f, false, 60.0f}, kW, kH, {}, &st));
      std::vector<uint8_t> rgba;
      (void)measureTotalLuma(rgba);
      uint64_t rSum = 0, gSum = 0, bSum = 0, leftGSum = 0;
      for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW / 2; ++x) {
          const size_t i = static_cast<size_t>((y * kW + x) * 4);
          leftGSum += rgba[i + 1];
        }
        for (int x = kW / 2; x < kW; ++x) {
          const size_t i = static_cast<size_t>((y * kW + x) * 4);
          rSum += rgba[i + 0];
          gSum += rgba[i + 1];
          bSum += rgba[i + 2];
        }
      }
      CHECK(st.activeParticles > 50);
      CHECK(gSum > rSum + 15000);
      CHECK(bSum > rSum + 15000);
      CHECK(gSum > leftGSum + 15000);
    }

    // 3. Full 3D DoF Bokeh + Aux Trails + SoundKeys reactive emission
    {
      iota::model::Project fullP;
      fullP.version = "10";
      auto comp0 = iota::model::createDefaultComposition(0);
      comp0.elements.push_back(iota::model::createDefaultElement(iota::model::ElementType::AudioProvider));
      comp0.elements.push_back(iota::model::createDefaultElement(iota::model::ElementType::SoundKeys));

      auto pt = iota::model::createDefaultElement(iota::model::ElementType::Particles);
      pt.props.setString("particleShape", "BokehRing",
                         "sel Sprite Sphere GlowSphere Star BokehRing Streaklet", "appearance");
      pt.props.getOrAdd("SpawnArea").value = std::string("Box3D");
      pt.props.setInt("CountLimit", 1500, 1, 50000, "1_overall");
      pt.props.setFloat("particlesPerSec", 90.0f, 0.0f, 5000.0f, "1_overall");
      pt.props.setMVar("measureEmitRate",
                       iota::model::MVariableFloat{{{"Constant", 0.4f, 0.0f}, {"SoundKeys1", 2.5f, 0.0f}}},
                       "mvarf 0.0 5.0",
                       "1_overall");
      pt.props.setFloat("preRunSec", 0.5f, 0.0f, 10.0f, "1_overall");
      pt.props.setFloat("emitterSizeZ", 350.0f, 0.0f, 1000.0f, "1_overall");
      pt.props.setFloat("turbulenceAffectPos", 70.0f, 0.0f, 500.0f, "4_turbulence");
      pt.props.setString("auxEmitMode", "Continuously", "sel Off Continuously AtBounce AtDeath", "5_auxSystem");
      pt.props.setFloat("auxParticlesPerSec", 20.0f, 0.0f, 60.0f, "5_auxSystem");
      pt.props.setBool("camera3DEnabled", true, "6_cameraDof");
      pt.props.setFloat("cameraDistance", 420.0f, 80.0f, 2000.0f, "6_cameraDof");
      pt.props.setBool("dofEnabled", true, "6_cameraDof");
      pt.props.setFloat("dofFocusDist", 420.0f, 40.0f, 2000.0f, "6_cameraDof");
      pt.props.setFloat("dofAperture", 0.8f, 0.0f, 2.0f, "6_cameraDof");
      pt.props.setFloat("particleScale", 5.0f, 0.25f, 16.0f, "appearance");
      comp0.elements.push_back(std::move(pt));
      fullP.compositions.push_back(std::move(comp0));

      renderer.resetSimulation();
      iota::engine::RenderFrameStats st;
      for (int f = 0; f < 15; ++f) {
        REQUIRE(renderer.renderFrame(fullP, audio, {1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f},
                                     kW, kH, {}, &st));
      }
      std::vector<uint8_t> rgba;
      const uint64_t luma = measureTotalLuma(rgba);
      CHECK(st.activeParticles > 30);
      CHECK(st.activeAuxParticles > 10);
      CHECK(luma > 20000);
    }

    // 4. BassNation Trapcode Particular configuration (GlowSphere 300% halo, Box3D, 50mm FOV, MirrorX, PhysicsTimeFactor)
    {
      iota::model::Project bn;
      bn.version = "10";
      auto comp0 = iota::model::createDefaultComposition(0);
      comp0.elements.push_back(iota::model::createDefaultElement(iota::model::ElementType::AudioProvider));
      comp0.elements.push_back(iota::model::createDefaultElement(iota::model::ElementType::SoundKeys));

      auto pt = iota::model::createDefaultElement(iota::model::ElementType::Particles);
      pt.props.setString("particleShape", "GlowSphere",
                         "sel Sprite Sphere GlowSphere Star BokehRing Streaklet", "appearance");
      pt.props.setFloat("feather", 0.5f, 0.0f, 1.0f, "appearance");
      pt.props.setFloat("glowSize", 3.0f, 1.0f, 6.0f, "appearance");
      pt.props.setFloat("glowOpacity", 0.25f, 0.0f, 1.0f, "appearance");
      pt.props.setFloat("glowFeather", 1.0f, 0.0f, 1.0f, "appearance");
      pt.props.setString("blendMode", "Alpha", "sel Add Alpha PreMulAlpha Screen", "0_general");
      pt.props.setBool("scaleIsUniform", false, "0_general");
      pt.props.setVec2("scale", {0.353f, 0.089f}, 0.0f, 4.0f, "0_general");
      pt.props.getOrAdd("SpawnArea").value = std::string("Box3D");
      pt.props.setInt("CountLimit", 1500, 1, 50000, "1_overall");
      pt.props.setFloat("particlesPerSec", 60.0f, 0.0f, 5000.0f, "1_overall");
      pt.props.setMVar("measureEmitRate",
                       iota::model::MVariableFloat{{{"Constant", 1.0f, 0.0f}, {"SoundKeys1", 1.417f, 0.0f}}},
                       "mvarf 0.0 5.0",
                       "1_overall");
      pt.props.setMVar("measurePhysicsTime",
                       iota::model::MVariableFloat{{{"Constant", 1.0f, 0.0f}, {"SoundKeys1", 20.0f, 0.0f}}},
                       "mvarf 0.0 25.0",
                       "behaviour");
      pt.props.setFloat("emitterSizeZ", 2088.0f, 0.0f, 4000.0f, "1_overall");
      pt.props.setFloat("preRunSec", 1.5f, 0.0f, 10.0f, "1_overall");
      pt.props.setBool("mirrorX", true, "1_overall");
      pt.props.setFloat("Speed", 90.0f, -500.0f, 500.0f, "behaviour");
      pt.props.setFloat("speedRandom", 36.0f, 0.0f, 500.0f, "behaviour");
      pt.props.setFloat("directionSpread", 20.0f, 0.0f, 180.0f, "behaviour");
      pt.props.setFloat("airResistance", 0.5f, 0.0f, 10.0f, "behaviour");
      pt.props.setFloat("lifetime", 11.0f, 0.05f, 20.0f, "behaviour");
      pt.props.setFloat("lifetimeRandom", 0.75f, 0.0f, 0.95f, "behaviour");
      pt.props.setFloat("particleScale", 0.8f, 0.25f, 16.0f, "appearance");
      pt.props.setFloat("sizeRandom", 1.0f, 0.0f, 1.0f, "appearance");
      pt.props.setFloat("opacityRandom", 0.5f, 0.0f, 1.0f, "appearance");
      pt.props.setString("sizeOverLife", "HoldFadeOut",
                         "sel Linear FadeInOut EaseOut EaseIn Pulse Constant HoldFadeOut", "appearance");
      pt.props.setString("opacityOverLife", "Constant",
                         "sel FadeInOut Linear EaseOut EaseIn Pulse Constant HoldFadeOut", "appearance");
      pt.props.setFloat("fadeInAndOutTime", 0.375f, 0.0f, 0.5f, "appearance");
      pt.props.setColorHsla("ColorFrom", {0.0f, 0.0f, 1.0f, 1.0f}, "appearance");
      pt.props.setColorHsla("ColorTo", {0.0f, 0.0f, 1.0f, 1.0f}, "appearance");
      pt.props.setFloat("orbitalSpinAmp", 20.0f, 0.0f, 200.0f, "behaviour");
      pt.props.setFloat("orbitalSpinFreq", 0.5f, 0.0f, 10.0f, "behaviour");
      pt.props.setFloat("turbulenceAffectPos", 0.0f, 0.0f, 500.0f, "4_turbulence");
      pt.props.setFloat("turbulenceAffectSize", 0.45f, 0.0f, 2.0f, "4_turbulence");
      pt.props.setBool("camera3DEnabled", true, "6_cameraDof");
      pt.props.setFloat("perspectiveFov", 22.9f, 15.0f, 170.0f, "1_overall");
      pt.props.setFloat("cameraDistance", 2666.7f, 80.0f, 5000.0f, "6_cameraDof");
      pt.props.setFloat("nearCameraFadeOutDistance", 10.0f, 0.0f, 500.0f, "appearance");
      comp0.elements.push_back(std::move(pt));
      bn.compositions.push_back(std::move(comp0));

      renderer.resetSimulation();
      iota::engine::RenderFrameStats st;
      for (int f = 0; f < 10; ++f) {
        REQUIRE(renderer.renderFrame(bn, audio, {1.0f / 30.0f, f / 30.0, f / 30.0, true, 0.0f, true, 30.0f},
                                     kW, kH, {}, &st));
      }
      std::vector<uint8_t> rgba;
      const uint64_t luma = measureTotalLuma(rgba);
      CHECK(st.activeParticles > 50);
      CHECK(luma > 10000);

      // Verify bilateral mirrorX symmetry between left and right halves
      uint64_t leftLuma = 0;
      uint64_t rightLuma = 0;
      for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW / 2; ++x) {
          const size_t i = static_cast<size_t>((y * kW + x) * 4);
          leftLuma += rgba[i] + rgba[i + 1] + rgba[i + 2];
        }
        for (int x = kW / 2; x < kW; ++x) {
          const size_t i = static_cast<size_t>((y * kW + x) * 4);
          rightLuma += rgba[i] + rgba[i + 1] + rgba[i + 2];
        }
      }
      CHECK(leftLuma > 3000);
      CHECK(rightLuma > 3000);
      const double ratio = static_cast<double>(std::min(leftLuma, rightLuma)) /
                           static_cast<double>(std::max<uint64_t>(1, std::max(leftLuma, rightLuma)));
      CHECK(ratio > 0.70);
    }
  }

  SUBCASE("HDR RGBA16F pipeline, emissive gain, physical Bloom pyramid, and Filmic Tonemapping") {
    constexpr int kW = 320;
    constexpr int kH = 240;
    const iota::engine::FrameTiming tm{1.0f / 60.0f, 0.1, 0.1, true, 0.0f, false, 60.0f};

    auto makeHdrBoxProject = [&](float hdrGain, iota::core::ColorHslaNorm col) {
      iota::model::Project p;
      p.version = "10";
      auto comp0 = iota::model::createDefaultComposition(0);
      auto sq = iota::model::createDefaultElement(iota::model::ElementType::Image);
      sq.props.setString("customImage", "internalres:white", "img", "1_image");
      sq.props.setBool("scaleIsUniform", true, "0_general");
      sq.props.setVec2("scale", {0.25f, 0.25f}, 0.0f, 2.0f, "0_general");  // 60x60 centered at (160,120)
      sq.props.setVec2("position", {0.5f, 0.5f}, -1.0f, 1.0f, "0_general");
      sq.props.setColorHsla("Color", col, "2_color");
      sq.props.setColorHsla("ColorTo", col, "2_color");
      sq.props.setFloat("hdrIntensity", hdrGain, 0.0f, 16.0f, "2_color");
      comp0.elements.push_back(std::move(sq));
      p.compositions.push_back(std::move(comp0));
      return p;
    };

    auto readFinal = [&](int expectedW, int expectedH) {
      std::vector<uint8_t> rgba;
      int rw = 0, rh = 0;
      REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, rgba, true));
      REQUIRE(rw == expectedW);
      REQUIRE(rh == expectedH);
      return rgba;
    };

    auto luma8At = [](const std::vector<uint8_t>& buf, int w, int x, int y) {
      const size_t idx = static_cast<size_t>((y * w + x) * 4);
      return (static_cast<float>(buf[idx + 0]) + buf[idx + 1] + buf[idx + 2]) / 3.0f;
    };

    // 1. True RGBA16F Float Precision, Live SDR/HDR Toggle, and Resize Preservation
    {
      auto p = makeHdrBoxProject(4.0f, {0.0f, 0.0f, 1.0f, 1.0f});
      renderer.resetSimulation();
      iota::engine::RenderFrameStats st;
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH, {}, &st));
      CHECK(st.hdrPipelineActive);
      CHECK(device.renderTargetFormat(renderer.hdrSceneRenderTarget()) == iota::rhi::PixelFormat::Rgba16F);
      CHECK(device.renderTargetFormat(renderer.finalRenderTarget()) == iota::rhi::PixelFormat::Rgba8);

      std::vector<float> hdrFloats;
      int fw = 0, fh = 0;
      REQUIRE(device.readRenderTargetRgbaFloat(renderer.hdrSceneRenderTarget(), fw, fh, hdrFloats, true));
      REQUIRE(fw == kW);
      REQUIRE(fh == kH);
      const size_t cIdx = static_cast<size_t>((120 * kW + 160) * 4);
      CHECK(hdrFloats[cIdx + 0] > 3.5f);
      CHECK(hdrFloats[cIdx + 1] > 3.5f);
      CHECK(hdrFloats[cIdx + 2] > 3.5f);

      // Live toggle to SDR8
      p.compositions[0].props.setBool("hdrPipeline", false, "3_hdrPost", "b");
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH, {}, &st));
      CHECK_FALSE(st.hdrPipelineActive);
      CHECK(device.renderTargetFormat(renderer.hdrSceneRenderTarget()) == iota::rhi::PixelFormat::Rgba8);
      REQUIRE(device.readRenderTargetRgbaFloat(renderer.hdrSceneRenderTarget(), fw, fh, hdrFloats, true));
      CHECK(hdrFloats[cIdx + 0] <= 1.01f);
      CHECK(hdrFloats[cIdx + 1] <= 1.01f);
      CHECK(hdrFloats[cIdx + 2] <= 1.01f);

      // Flip back to HDR16F and resize down then back up
      p.compositions[0].props.setBool("hdrPipeline", true, "3_hdrPost", "b");
      REQUIRE(renderer.renderFrame(p, audio, tm, 256, 192, {}, &st));
      CHECK(device.renderTargetFormat(renderer.hdrSceneRenderTarget()) == iota::rhi::PixelFormat::Rgba16F);
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH, {}, &st));
      CHECK(device.renderTargetFormat(renderer.hdrSceneRenderTarget()) == iota::rhi::PixelFormat::Rgba16F);
      REQUIRE(device.readRenderTargetRgbaFloat(renderer.hdrSceneRenderTarget(), fw, fh, hdrFloats, true));
      CHECK(hdrFloats[cIdx + 0] > 3.5f);
    }

    // 2. Physical Bloom Threshold, Halo Spread & Anamorphic Stretch
    {
      auto p = makeHdrBoxProject(3.0f, {0.0f, 0.0f, 1.0f, 1.0f});
      auto& cp = p.compositions[0].props;
      cp.setString("tonemapMode", "ACES", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
      cp.setBool("bloomEnabled", false, "3_hdrPost", "b");

      renderer.resetSimulation();
      iota::engine::RenderFrameStats st;
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH, {}, &st));
      CHECK_FALSE(st.bloomActive);
      const auto noBloomPx = readFinal(kW, kH);
      CHECK(luma8At(noBloomPx, kW, 160 + 55, 120) < 20.0f);

      cp.setBool("bloomEnabled", true, "3_hdrPost", "b");
      cp.setFloat("bloomThreshold", 1.0f, 0.0f, 5.0f, "3_hdrPost");
      cp.setFloat("bloomIntensity", 1.2f, 0.0f, 4.0f, "3_hdrPost");
      cp.setFloat("bloomRadius", 0.78f, 0.10f, 0.98f, "3_hdrPost");
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH, {}, &st));
      CHECK(st.bloomActive);
      const auto bloomPx = readFinal(kW, kH);
      CHECK(luma8At(bloomPx, kW, 160 + 55, 120) > 25.0f);

      // Anamorphic horizontal stretch
      cp.setFloat("bloomAnamorphic", 0.95f, 0.0f, 1.0f, "3_hdrPost");
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH, {}, &st));
      const auto anamPx = readFinal(kW, kH);
      CHECK(luma8At(anamPx, kW, 160 + 75, 120) > luma8At(anamPx, kW, 160, 120 + 75) + 4.0f);

      // Dim element below threshold emits negligible halo
      p.compositions[0].elements[0].props.setFloat("hdrIntensity", 0.25f, 0.0f, 16.0f, "2_color");
      cp.setFloat("bloomAnamorphic", 0.0f, 0.0f, 1.0f, "3_hdrPost");
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH, {}, &st));
      const auto dimPx = readFinal(kW, kH);
      CHECK(luma8At(dimPx, kW, 160 + 55, 120) < 12.0f);
    }

    // 3. Filmic Tonemappers, Highlight Desaturation, Exposure EV & Audio-Reactive Bloom
    {
      auto p = makeHdrBoxProject(4.0f, {0.0f, 1.0f, 0.5f, 1.0f});  // Pure red HDR 4.0
      auto& cp = p.compositions[0].props;
      cp.setBool("bloomEnabled", false, "3_hdrPost", "b");
      const size_t cIdx = static_cast<size_t>((120 * kW + 160) * 4);

      renderer.resetSimulation();
      cp.setString("tonemapMode", "None", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH));
      const auto nonePx = readFinal(kW, kH);
      CHECK(nonePx[cIdx + 0] > 240);
      CHECK(nonePx[cIdx + 1] < 5);
      CHECK(nonePx[cIdx + 2] < 5);

      for (const std::string tmMode : {"ACES", "AgX", "PBRNeutral"}) {
        cp.setString("tonemapMode", tmMode, "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
        REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH));
        const auto px = readFinal(kW, kH);
        CHECK_MESSAGE(px[cIdx + 0] > 200, "Mode: " << tmMode);
        CHECK_MESSAGE(px[cIdx + 1] > 20, "Highlight desaturation missing in " << tmMode);
      }

      cp.setString("tonemapMode", "Reinhard", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH));
      const auto reinPx = readFinal(kW, kH);
      CHECK(reinPx[cIdx + 0] > 180);

      // Exposure EV test on mid-grey ACES scene
      auto pExp = makeHdrBoxProject(1.0f, {0.0f, 0.0f, 0.5f, 1.0f});
      auto& cpExp = pExp.compositions[0].props;
      cpExp.setString("tonemapMode", "ACES", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
      cpExp.setFloat("exposureEv", -1.5f, -4.0f, 4.0f, "3_hdrPost");
      REQUIRE(renderer.renderFrame(pExp, audio, tm, kW, kH));
      const float lowEvLuma = luma8At(readFinal(kW, kH), kW, 160, 120);

      cpExp.setFloat("exposureEv", +1.5f, -4.0f, 4.0f, "3_hdrPost");
      REQUIRE(renderer.renderFrame(pExp, audio, tm, kW, kH));
      const float highEvLuma = luma8At(readFinal(kW, kH), kW, 160, 120);
      CHECK(highEvLuma > lowEvLuma + 45.0f);

      // Audio-reactive Bloom + Particles + FXAA combined stress check
      auto starter = iota::model::createStarterProject();
      auto& scp = starter.compositions[0].props;
      scp.setString("tonemapMode", "ACES", "sel None ACES AgX PBRNeutral Reinhard", "3_hdrPost");
      scp.setBool("bloomEnabled", true, "3_hdrPost", "b");
      scp.setFloat("bloomIntensity", 0.25f, 0.0f, 4.0f, "3_hdrPost");
      scp.setMVar("measureBloomIntensity",
                  iota::model::MVariableFloat{{{"SoundKeys1", 1.2f, 0.0f}}},
                  "mvarf 0.0 3.0",
                  "3_hdrPost");

      renderer.resetSimulation();
      iota::engine::RenderFrameStats st;
      for (int f = 0; f < 10; ++f) {
        const iota::engine::FrameTiming fxaaTm{1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f};
        REQUIRE(renderer.renderFrame(starter, audio, fxaaTm, kW, kH, {}, &st));
      }
      CHECK(st.hdrPipelineActive);
      CHECK(st.bloomActive);
      const auto finalFxaa = readFinal(kW, kH);
      uint64_t totalLuma = 0;
      for (size_t i = 0; i + 3 < finalFxaa.size(); i += 4) {
        totalLuma += finalFxaa[i] + finalFxaa[i + 1] + finalFxaa[i + 2];
      }
      CHECK(totalLuma > 30000);
    }
  }

  SUBCASE("Unicode FontAtlasCache: DirectWrite multi-script shaping, RTL Arabic, outlines, shadows, alignment, tracking, and embedded fonts") {
    constexpr int kW = 480;
    constexpr int kH = 360;
    const iota::engine::FrameTiming tm{1.0f / 60.0f, 0.1, 0.1, true, 0.0f, false, 60.0f};

    auto makeTextProject = [&](std::string_view textStr, std::string_view face = "Segoe UI") {
      iota::model::Project p;
      p.version = "10";
      auto comp0 = iota::model::createDefaultComposition(0);
      auto txt = iota::model::createDefaultElement(iota::model::ElementType::Text);
      txt.props.setString("text", textStr, "ptxt", "appearance");
      txt.props.setString("typeFace", face, "passet", "appearance");
      txt.props.setInt("fontSize", 72, 8, 120, "appearance");
      txt.props.setVec2("position", {0.5f, 0.5f}, -1.0f, 1.0f, "0_general");
      txt.props.setBool("scaleIsUniform", false, "0_general");
      txt.props.setVec2("scale", {0.9f, 0.45f}, 0.0f, 2.0f, "0_general");
      txt.props.setColorArgb("color", {255, 255, 255, 255}, "crgba", "appearance");
      comp0.elements.push_back(std::move(txt));
      p.compositions.push_back(std::move(comp0));
      return p;
    };

    auto renderAndRead = [&](const iota::model::Project& p, iota::engine::RenderFrameStats* outSt = nullptr) {
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH, {}, outSt));
      std::vector<uint8_t> rgba;
      int rw = 0, rh = 0;
      REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, rgba, true));
      REQUIRE(rw == kW);
      REQUIRE(rh == kH);
      return rgba;
    };

    auto measureAboveBg = [&](const std::vector<uint8_t>& rgba, int x0, int x1) {
      uint64_t sum = 0;
      for (int y = 0; y < kH; ++y) {
        for (int x = x0; x < x1; ++x) {
          const size_t i = static_cast<size_t>((y * kW + x) * 4);
          sum += static_cast<uint64_t>(std::max(0, static_cast<int>(rgba[i + 0]) - 15));
          sum += static_cast<uint64_t>(std::max(0, static_cast<int>(rgba[i + 1]) - 15));
          sum += static_cast<uint64_t>(std::max(0, static_cast<int>(rgba[i + 2]) - 15));
        }
      }
      return sum;
    };

    // 1. Multi-script non-ASCII rendering (Polish, Cyrillic, Greek, Japanese, Korean, Arabic RTL, Symbols + invalid UTF-8)
    const std::array<std::pair<const char*, std::string>, 7> scripts = {{
        {"Polish", "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85 ja\xC5\xBA\xC5\x84"},
        {"Cyrillic", "\xD0\x9C\xD1\x83\xD0\xB7\xD1\x8B\xD0\xBA\xD0\xB0 \xD0\xB7\xD0\xB2\xD1\x83\xD0\xBA"},
        {"Greek", "\xCE\x9C\xCE\xBF\xCF\x85\xCF\x83\xCE\xB9\xCE\xBA\xCE\xAE \xCE\x89\xCF\x87\xCE\xBF\xCF\x82"},
        {"Japanese", "\xE9\x9F\xB3\xE6\xA5\xBD\xE3\x82\xB5\xE3\x82\xA6\xE3\x83\xB3\xE3\x83\x89"},
        {"Korean", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4 \xEC\x9D\x8C\xEC\x95\x85"},
        {"ArabicRTL", "\xD9\x85\xD9\x88\xD8\xB3\xD9\x8A\xD9\x82\xD9\x89 \xD8\xAC\xD9\x85\xD9\x8A\xD9\x84\xD8\xA9"},
        {"SymbolsInvalidUtf8", std::string("\xE2\x99\xAA \xE2\x99\xAB \xE2\x98\x85 \xFF\xFE", 13)},
    }};
    for (const auto& [label, str] : scripts) {
      auto p = makeTextProject(str);
      iota::engine::RenderFrameStats st;
      const auto rgba = renderAndRead(p, &st);
      CHECK_MESSAGE(st.fontUsingNativeBackend, "Expected native font backend active for: " << label);
      CHECK_MESSAGE(st.cachedGlyphs > 0, "No glyphs cached for: " << label);
      CHECK_MESSAGE(measureAboveBg(rgba, 0, kW) > 5000, "Empty output for script: " << label);
    }

    // 2. Circular Outline & Drop Shadow in single draw call
    {
      auto pNoFx = makeTextProject("HDR", "Impact");
      const auto noFxPx = renderAndRead(pNoFx);
      auto measureCyanExcess = [&](const std::vector<uint8_t>& buf) {
        uint64_t sum = 0;
        for (size_t i = 0; i + 3 < buf.size(); i += 4) {
          const int r = buf[i + 0];
          const int g = buf[i + 1];
          const int b = buf[i + 2];
          sum += static_cast<uint64_t>(std::max(0, g - r - 12) + std::max(0, b - r - 12));
        }
        return sum;
      };
      CHECK(measureCyanExcess(noFxPx) < 500);

      auto pOutline = makeTextProject("HDR", "Impact");
      pOutline.compositions[0].elements[0].props.setFloat("outlineWidth", 6.0f, 0.0f, 12.0f, "2_typography");
      pOutline.compositions[0].elements[0].props.setColorArgb("outlineColor", {0, 255, 255, 255}, "crgba", "2_typography");
      const auto outlinePx = renderAndRead(pOutline);
      CHECK(measureCyanExcess(outlinePx) > 10000);

      auto pShadow = makeTextProject("HDR", "Impact");
      pShadow.compositions[0].elements[0].props.setVec2("shadowOffset", {12.0f, 12.0f}, -40.0f, 40.0f, "2_typography");
      pShadow.compositions[0].elements[0].props.setColorArgb("shadowColor", {255, 0, 0, 255}, "crgba", "2_typography");
      const auto shadowPx = renderAndRead(pShadow);
      uint64_t redExcess = 0;
      for (size_t i = 0; i + 3 < shadowPx.size(); i += 4) {
        redExcess += static_cast<uint64_t>(std::max(0, static_cast<int>(shadowPx[i + 0]) - static_cast<int>(shadowPx[i + 1])));
      }
      CHECK(redExcess > 5000);
    }

    // 3. Left/Right Alignment & Letter Spacing Tracking
    {
      auto pLeft = makeTextProject("TEST");
      pLeft.compositions[0].elements[0].props.setString("textAlign", "Left", "sel Center Left Right", "2_typography");
      const auto leftPx = renderAndRead(pLeft);
      CHECK(measureAboveBg(leftPx, 0, kW / 2) > measureAboveBg(leftPx, kW / 2, kW) + 5000);

      auto pRight = makeTextProject("TEST");
      pRight.compositions[0].elements[0].props.setString("textAlign", "Right", "sel Center Left Right", "2_typography");
      const auto rightPx = renderAndRead(pRight);
      CHECK(measureAboveBg(rightPx, kW / 2, kW) > measureAboveBg(rightPx, 0, kW / 2) + 5000);

      auto measureLitWidth = [&](const std::vector<uint8_t>& buf) {
        int minX = kW, maxX = -1;
        for (int y = 0; y < kH; ++y) {
          for (int x = 0; x < kW; ++x) {
            const size_t i = static_cast<size_t>((y * kW + x) * 4);
            if (buf[i] > 100 || buf[i + 1] > 100 || buf[i + 2] > 100) {
              minX = std::min(minX, x);
              maxX = std::max(maxX, x);
            }
          }
        }
        return (maxX >= minX) ? (maxX - minX) : 0;
      };

      auto pTight = makeTextProject("ABCD");
      pTight.compositions[0].elements[0].props.setFloat("letterSpacing", 0.0f, -10.0f, 40.0f, "2_typography");
      const int tightSpan = measureLitWidth(renderAndRead(pTight));

      auto pWide = makeTextProject("ABCD");
      pWide.compositions[0].elements[0].props.setFloat("letterSpacing", 28.0f, -10.0f, 40.0f, "2_typography");
      const int wideSpan = measureLitWidth(renderAndRead(pWide));
      CHECK(wideSpan > tightSpan + 20);
    }

    // 4. Embedded local: Font Loading + Automatic CJK System Font Fallback
    std::filesystem::path monoFontPath;
#ifdef _WIN32
    if (std::filesystem::exists("C:/Windows/Fonts/consola.ttf")) {
      monoFontPath = "C:/Windows/Fonts/consola.ttf";
    }
#else
    for (const char* cand : {
             "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
             "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
             "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
             "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
         }) {
      if (std::filesystem::exists(cand)) {
        monoFontPath = cand;
        break;
      }
    }
#endif
    if (!monoFontPath.empty()) {
      const std::filesystem::path& consolaPath = monoFontPath;
      std::ifstream tf(consolaPath, std::ios::binary | std::ios::ate);
      REQUIRE(tf.is_open());
      const auto tsz = tf.tellg();
      REQUIRE(tsz > 0);
      std::vector<uint8_t> ttfBytes(static_cast<size_t>(tsz));
      tf.seekg(0, std::ios::beg);
      REQUIRE(tf.read(reinterpret_cast<char*>(ttfBytes.data()), tsz));

      auto pLocal = makeTextProject("MONO  \xE9\x9F\xB3\xE6\xA5\xBD", "local:embedded_mono.ttf");
      pLocal.localAssets["embedded_mono.ttf"] = std::move(ttfBytes);
      renderer.invalidateLocalAssets();
      const auto localPx = renderAndRead(pLocal);
      CHECK(measureAboveBg(localPx, 0, kW / 2) > 1500);
      CHECK(measureAboveBg(localPx, kW / 2, kW) > 1500);
    }
  }

  SUBCASE("Sapphire-style ShakeEffect: canvas-below wrap modes (No/Reflect/Tile/Clamp), curved shutter blur, RGB phase, and audio-reactive HDR") {
    constexpr int kW = 320;
    constexpr int kH = 240;

    auto readRgba = [&]() {
      std::vector<uint8_t> rgba;
      int rw = 0, rh = 0;
      REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, rgba, true));
      REQUIRE(rw == kW);
      REQUIRE(rh == kH);
      return rgba;
    };

    auto lumaAt = [](const std::vector<uint8_t>& buf, int x, int y) {
      const size_t i = static_cast<size_t>((y * kW + x) * 4);
      return (static_cast<float>(buf[i + 0]) + buf[i + 1] + buf[i + 2]) / 3.0f;
    };

    auto makeCanvasShakeProj = [&](bool fullScreenWhite) {
      iota::model::Project p;
      p.version = "10";
      auto comp0 = iota::model::createDefaultComposition(0);
      auto bg = iota::model::createDefaultElement(iota::model::ElementType::Image);
      bg.props.setString("customImage", "internalres:white", "img", "1_image");
      bg.props.setBool("keepAspectRatio", !fullScreenWhite, "1_image");
      bg.props.setBool("scaleIsUniform", !fullScreenWhite, "0_general");
      bg.props.setVec2("scale",
                       fullScreenWhite ? iota::core::Vec2{1.0f, 1.0f} : iota::core::Vec2{0.25f, 0.25f},
                       0.0f,
                       2.0f,
                       "0_general");
      bg.props.setVec2("position", {0.5f, 0.5f}, -1.0f, 1.0f, "0_general");
      comp0.elements.push_back(std::move(bg));

      auto shake = iota::model::createDefaultElement(iota::model::ElementType::ShakeEffect);
      shake.props.setFloat("xRandAmp", 0.0f, 0.0f, 1.0f, "3_axes");
      shake.props.setFloat("yRandAmp", 0.0f, 0.0f, 1.0f, "3_axes");
      shake.props.setFloat("zRandAmp", 0.0f, 0.0f, 1.0f, "3_axes");
      shake.props.setFloat("tiltRandAmp", 0.0f, 0.0f, 45.0f, "3_axes");
      comp0.elements.push_back(std::move(shake));
      p.compositions.push_back(std::move(comp0));
      return p;
    };

    // 1. Edge wrap modes (No vs Reflect/Tile/Clamp) under Z Dist = 2.0 zoom-out
    {
      auto p = makeCanvasShakeProj(true);
      auto& sp = p.compositions[0].elements[1].props;
      sp.setFloat("amplitude", 0.0f, 0.0f, 5.0f, "2_shakeMaster");
      sp.setFloat("zDist", 2.0f, 0.15f, 3.0f, "2_shakeMaster");
      const iota::engine::FrameTiming tm{1.0f / 60.0f, 0.0, 0.0, true, 0.0f, false, 60.0f};

      sp.setString("wrapX", "No", "sel No Tile Reflect Clamp", "1_appearance");
      sp.setString("wrapY", "No", "sel No Tile Reflect Clamp", "1_appearance");
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH));
      const auto noPx = readRgba();
      CHECK(lumaAt(noPx, 160, 120) > 220.0f);
      CHECK(lumaAt(noPx, 10, 10) < 20.0f);

      for (const std::string wrap : {"Reflect", "Tile", "Clamp"}) {
        sp.setString("wrapX", wrap, "sel No Tile Reflect Clamp", "1_appearance");
        sp.setString("wrapY", wrap, "sel No Tile Reflect Clamp", "1_appearance");
        REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH));
        const auto px = readRgba();
        CHECK_MESSAGE(lumaAt(px, 10, 10) > 180.0f, "Wrap mode failed at border: " << wrap);
      }
    }

    // 2. Horizontal Wave shift, curved shutter Motion Blur smear, and Chromatic RGB Phase
    {
      auto p = makeCanvasShakeProj(false);
      auto& sp = p.compositions[0].elements[1].props;
      sp.setFloat("amplitude", 1.0f, 0.0f, 5.0f, "2_shakeMaster");
      sp.setFloat("frequency", 6.0f, 0.05f, 30.0f, "2_shakeMaster");
      sp.setFloat("xWaveAmp", 0.25f, 0.0f, 1.0f, "3_axes");
      sp.setFloat("xWaveFreq", 1.0f, 0.1f, 5.0f, "3_axes");
      sp.setString("wrapX", "No", "sel No Tile Reflect Clamp", "1_appearance");
      sp.setString("wrapY", "No", "sel No Tile Reflect Clamp", "1_appearance");
      sp.setBool("motionBlur", false, "4_motionAndRgb", "b");

      // Peak displacement at t = 0.25 / 6.0 moves square right by +80px -> center (160,120) clears, x=240 lights
      const double tPeak = 0.25 / 6.0;
      REQUIRE(renderer.renderFrame(p, audio, {1.0f / 60.0f, tPeak, tPeak, true, 0.0f, false, 60.0f}, kW, kH));
      const auto shiftPx = readRgba();
      CHECK(lumaAt(shiftPx, 160, 120) < 20.0f);
      CHECK(lumaAt(shiftPx, 240, 120) > 200.0f);

      auto measureLitWidth = [&](const std::vector<uint8_t>& buf) {
        int lit = 0;
        for (int x = 0; x < kW; ++x) {
          if (lumaAt(buf, x, 120) > 20.0f) ++lit;
        }
        return lit;
      };

      const iota::engine::FrameTiming velTm{1.0f / 60.0f, 0.0, 0.0, true, 0.0f, false, 60.0f};
      REQUIRE(renderer.renderFrame(p, audio, velTm, kW, kH));
      const auto sharpPx = readRgba();
      const int staticSpan = measureLitWidth(sharpPx);

      auto measureChromaticRB = [&](const std::vector<uint8_t>& buf) {
        uint64_t sum = 0;
        for (size_t i = 0; i + 3 < buf.size(); i += 4) {
          const int diff = std::abs(static_cast<int>(buf[i + 0]) - static_cast<int>(buf[i + 2]));
          sum += static_cast<uint64_t>(std::max(0, diff - 12));
        }
        return sum;
      };
      CHECK(measureChromaticRB(sharpPx) < 500);

      // Motion Blur active at peak velocity t=0.0 widens horizontal streak cleanly
      sp.setBool("motionBlur", true, "4_motionAndRgb", "b");
      sp.setFloat("moBlurLength", 2.5f, 0.0f, 4.0f, "4_motionAndRgb");
      sp.setInt("blurSamples", 24, 4, 48, "4_motionAndRgb");
      REQUIRE(renderer.renderFrame(p, audio, velTm, kW, kH));
      const int blurSpan = measureLitWidth(readRgba());
      CHECK(blurSpan > staticSpan + 12);

      // RGB Phase splits R/G/B along the shake velocity trajectory
      sp.setBool("motionBlur", false, "4_motionAndRgb", "b");
      sp.setFloat("rgbPhase", 0.85f, 0.0f, 1.0f, "4_motionAndRgb");
      REQUIRE(renderer.renderFrame(p, audio, velTm, kW, kH));
      const auto rgbPx = readRgba();
      CHECK(measureChromaticRB(rgbPx) > 5000);

      // True per-channel Red/Blue Amplitude splits R & B at stationary wave peak
      sp.setFloat("rgbPhase", 0.0f, 0.0f, 1.0f, "4_channels");
      sp.setFloat("redAmp", 1.8f, 0.0f, 3.0f, "4_channels");
      sp.setFloat("blueAmp", 0.2f, 0.0f, 3.0f, "4_channels");
      REQUIRE(renderer.renderFrame(p, audio, {1.0f / 60.0f, tPeak, tPeak, true, 0.0f, false, 60.0f}, kW, kH));
      CHECK(measureChromaticRB(readRgba()) > 5000);

      // Independent RGB Randomness splits channels cleanly even with equal channel amplitudes
      sp.setFloat("redAmp", 1.0f, 0.0f, 3.0f, "4_channels");
      sp.setFloat("blueAmp", 1.0f, 0.0f, 3.0f, "4_channels");
      sp.setFloat("rgbRandomness", 0.7f, 0.0f, 2.0f, "4_channels");
      sp.setFloat("rgbFrequency", 4.0f, 0.05f, 30.0f, "4_channels");
      REQUIRE(renderer.renderFrame(p, audio, {1.0f / 60.0f, 0.37, 0.37, true, 0.0f, false, 60.0f}, kW, kH));
      CHECK(measureChromaticRB(readRgba()) > 3000);
    }

    // 3. Crop Input borders with Wrap No vs Wrap Reflect
    {
      auto p = makeCanvasShakeProj(true);
      auto& sp = p.compositions[0].elements[1].props;
      sp.setFloat("amplitude", 0.0f, 0.0f, 5.0f, "2_shakeMaster");
      sp.setFloat("zDist", 1.0f, 0.15f, 3.0f, "2_shakeMaster");
      sp.setBool("motionBlur", false, "5_motionAndCrop", "b");
      sp.setFloat("cropLeft", 0.20f, 0.0f, 0.45f, "5_motionAndCrop");
      sp.setFloat("cropRight", 0.20f, 0.0f, 0.45f, "5_motionAndCrop");
      const iota::engine::FrameTiming tm{1.0f / 60.0f, 0.0, 0.0, true, 0.0f, false, 60.0f};

      sp.setString("wrapX", "No", "sel No Tile Reflect Clamp", "1_appearance");
      sp.setString("wrapY", "No", "sel No Tile Reflect Clamp", "1_appearance");
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH));
      const auto cropNoPx = readRgba();
      CHECK(lumaAt(cropNoPx, 10, 120) < 20.0f);
      CHECK(lumaAt(cropNoPx, 160, 120) > 220.0f);

      sp.setString("wrapX", "Reflect", "sel No Tile Reflect Clamp", "1_appearance");
      sp.setString("wrapY", "Reflect", "sel No Tile Reflect Clamp", "1_appearance");
      REQUIRE(renderer.renderFrame(p, audio, tm, kW, kH));
      const auto cropReflectPx = readRgba();
      CHECK(lumaAt(cropReflectPx, 10, 120) > 180.0f);
    }

    // 4. Audio-reactive Composition 0 ShakeEffect + HDR16F + Bloom + FXAA
    {
      auto starter = iota::model::createStarterProject();
      auto shake = iota::model::createDefaultElement(iota::model::ElementType::ShakeEffect);
      shake.props.setFloat("amplitude", 0.05f, 0.0f, 5.0f, "2_shakeMaster");
      shake.props.setMVar("measureAmplitude",
                          iota::model::MVariableFloat{{{"SoundKeys1", 1.5f, 0.0f}}},
                          "mvarf 0.0 5.0",
                          "2_shakeMaster");
      shake.props.setFloat("rgbPhase", 0.25f, 0.0f, 1.0f, "4_motionAndRgb");
      starter.compositions[0].elements.push_back(std::move(shake));

      renderer.resetSimulation();
      iota::engine::RenderFrameStats st;
      for (int f = 0; f < 8; ++f) {
        REQUIRE(renderer.renderFrame(starter, audio, {1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f},
                                     kW, kH, {}, &st));
      }
      CHECK(st.hdrPipelineActive);
      CHECK(st.bloomActive);
      const auto outPx = readRgba();
      uint64_t totalLuma = 0;
      for (size_t i = 0; i + 3 < outPx.size(); i += 4) {
        totalLuma += outPx[i] + outPx[i + 1] + outPx[i + 2];
      }
      CHECK(totalLuma > 25000);
    }
  }

  SUBCASE("TwitchEffect: SoundKeys-driven canvas-below multi-operator glitch and zero-amount identity") {
    auto proj = iota::model::createStarterProject();
    auto tw = iota::model::createDefaultElement(iota::model::ElementType::TwitchEffect);
    tw.props.setBool("enableColor", true, "3_twitchEnable", "b");
    tw.props.setBool("enableSlice", true, "3_twitchEnable", "b");
    tw.props.setFloat("amount", 0.0f, 0.0f, 5.0f, "2_twitchMaster");
    tw.props.setMVar("measureAmount", iota::model::MVariableFloat{{{"Nothing", 0.0f, 0.0f}}}, "mvarf 0.0 5.0", "2_twitchMaster");

    auto projNoTwitch = proj;
    proj.compositions[0].elements.push_back(tw);

    auto readRgba = [&](int w, int h) {
      std::vector<uint8_t> px;
      int rw = 0, rh = 0;
      REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, px, true));
      REQUIRE(rw == w);
      REQUIRE(rh == h);
      return px;
    };

    renderer.resetSimulation();
    for (int f = 0; f < 4; ++f) {
      REQUIRE(renderer.renderFrame(projNoTwitch, audio, {1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f}, 320, 180));
    }
    const auto basePx = readRgba(320, 180);

    renderer.resetSimulation();
    for (int f = 0; f < 4; ++f) {
      REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f}, 320, 180));
    }
    const auto idPx = readRgba(320, 180);
    CHECK(basePx == idPx);

    // Enable SoundKeys-driven multi-operator Twitch
    auto& activeTw = proj.compositions[0].elements.back();
    activeTw.props.setFloat("amount", 0.65f, 0.0f, 5.0f, "2_twitchMaster");
    activeTw.props.setMVar("measureAmount", iota::model::MVariableFloat{{{"SoundKeys1", 1.5f, 0.0f}}}, "mvarf 0.0 5.0", "2_twitchMaster");
    activeTw.props.setFloat("threshold", 0.0f, 0.0f, 0.95f, "2_twitchMaster");

    renderer.resetSimulation();
    iota::engine::RenderFrameStats st;
    for (int f = 0; f < 6; ++f) {
      REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, true, 60.0f}, 320, 180, {}, &st));
    }
    CHECK(st.drawCalls > 0);
    const auto fxPx = readRgba(320, 180);
    uint64_t luma = 0;
    size_t diffBytes = 0;
    for (size_t i = 0; i < fxPx.size(); ++i) {
      if ((i & 3u) != 3u) luma += fxPx[i];
      if (fxPx[i] != basePx[i]) ++diffBytes;
    }
    CHECK(luma > 25000);
    CHECK(diffBytes > 500);
  }

  renderer.shutdown();
  device.shutdown();
  glfwDestroyWindow(win);
  glfwTerminate();
}

TEST_CASE("ShakeEffect blend choices and variant child seeding work cleanly") {
  using namespace iota::model;

  auto shake = createDefaultElement(ElementType::ShakeEffect);
  CHECK(shake.props.getString("blendMode") == "Opaque");
  const auto* blendEntry = shake.props.find("blendMode");
  REQUIRE(blendEntry != nullptr);
  const auto blendSpec = parseTypeTag(blendEntry->typeTag);
  CHECK(blendSpec.tag == PropertyTypeTag::Child);
  CHECK(std::find(blendSpec.choices.begin(), blendSpec.choices.end(), "Opaque") != blendSpec.choices.end());

  auto bars = createDefaultElement(ElementType::Bars);
  auto* seg2 = bars.props.find("Segment2");
  REQUIRE(seg2 != nullptr);
  seg2->value = std::string("Bars");
  seedVariantChildren(*seg2, "Segment2", "Bars");
  REQUIRE(seg2->findChild("colorFrom") != nullptr);
  REQUIRE(seg2->findChild("colorTo") != nullptr);
  REQUIRE(seg2->findChild("barWidth") != nullptr);
  REQUIRE(seg2->findChild("barWidthAffectedByShape") != nullptr);
  REQUIRE(seg2->findChild("mirror") != nullptr);
  seg2->findChild("barWidth")->value = 0.85;
  seedVariantChildren(*seg2, "Segment2", "Bars");
  CHECK(std::get<double>(seg2->findChild("barWidth")->value) == doctest::Approx(0.85));

  auto* sp = bars.props.find("ShapePath");
  REQUIRE(sp != nullptr);
  sp->value = std::string("SidedPolygon");
  seedVariantChildren(*sp, "ShapePath", "SidedPolygon");
  REQUIRE(sp->findChild("sides") != nullptr);
  CHECK(std::get<int64_t>(sp->findChild("sides")->value) == 6);
  REQUIRE(sp->findChild("radius") != nullptr);

  auto img = createDefaultElement(ElementType::Image);
  auto* imgShape = img.props.find("Shape");
  REQUIRE(imgShape != nullptr);
  imgShape->value = std::string("SidedPolygon");
  seedVariantChildren(*imgShape, "Shape", "SidedPolygon");
  REQUIRE(imgShape->findChild("sides") != nullptr);
  REQUIRE(imgShape->findChild("radius") != nullptr);
  REQUIRE(imgShape->findChild("gap") != nullptr);

  auto* mp = bars.props.find("MeasurePos");
  REQUIRE(mp != nullptr);
  CHECK(mp->findChild("A") == nullptr);
  seedVariantChildren(*mp, "MeasurePos", "Beat");
  REQUIRE(mp->findChild("A") != nullptr);
  REQUIRE(mp->findChild("B") != nullptr);
}

TEST_CASE("GL45 HDR additive alpha clamp, CustomShader error persistence, and GLSL 450 decl adaptation") {
  if (!glfwInit()) {
    FAIL("glfwInit failed");
    return;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

  GLFWwindow* win = glfwCreateWindow(256, 256, "iota_gl_fix_test", nullptr, nullptr);
  if (!win) {
    glfwTerminate();
    FAIL("Failed to create OpenGL 4.5 hidden test window");
    return;
  }
  glfwMakeContextCurrent(win);

  iota::rhi::Gl45RenderDevice device;
  std::string err;
  REQUIRE_MESSAGE(device.initialize(&err), err);

  iota::engine::SceneRenderer renderer;
  REQUIRE_MESSAGE(renderer.initialize(&device, &err), err);

  iota::audio::AudioEngine audio;
  audio.loadTrack(iota::audio::createSyntheticSineTrack(110.0f, 3.0, 44100, 0.5f, "Test Tone"));

  constexpr int kW = 128;
  constexpr int kH = 128;
  auto readRgba = [&]() {
    std::vector<uint8_t> rgba;
    int rw = 0, rh = 0;
    REQUIRE(device.readRenderTargetRgba(renderer.finalRenderTarget(), rw, rh, rgba, true));
    REQUIRE(rw == kW);
    REQUIRE(rh == kH);
    return rgba;
  };

  SUBCASE("HDR RGBA16F additive blend clamps alpha without negative inversion over red background") {
    iota::model::Project proj;
    proj.compositions.push_back(iota::model::createDefaultComposition(0));
    proj.compositions.push_back(iota::model::createDefaultComposition(1));
    proj.compositions[0].props.setBool("hdrPipeline", true, "3_hdrPost", "b");
    proj.compositions[0].props.setBool("bloomEnabled", false, "3_hdrPost", "b");

    auto bg = iota::model::createDefaultElement(iota::model::ElementType::Image);
    bg.props.setString("customImage", "internalres:white", "img", "1_image");
    bg.props.setColorHsla("Color", {0.0f, 1.0f, 0.5f, 1.0f}, "2_color");
    bg.props.setString("blendMode", "Alpha", "_child Alpha", "0_general");
    bg.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");

    auto compLayer = iota::model::createDefaultElement(iota::model::ElementType::Image);
    compLayer.props.setString("customImage", "composition:1", "img", "1_image");
    compLayer.props.setColorHsla("Color", {0.0f, 0.0f, 1.0f, 1.0f}, "2_color");
    compLayer.props.setString("blendMode", "PreMulAlpha", "_child PreMulAlpha", "0_general");
    compLayer.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");

    proj.compositions[0].elements.push_back(std::move(bg));
    proj.compositions[0].elements.push_back(std::move(compLayer));

    for (int i = 0; i < 2; ++i) {
      auto addGray = iota::model::createDefaultElement(iota::model::ElementType::Image);
      addGray.props.setString("customImage", "internalres:white", "img", "1_image");
      addGray.props.setColorHsla("Color", {0.0f, 0.0f, 0.12f, 0.85f}, "2_color");
      addGray.props.setString("blendMode", "Add", "_child Add", "0_general");
      addGray.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");
      proj.compositions[1].elements.push_back(std::move(addGray));
    }

    renderer.resetSimulation();
    iota::engine::RenderFrameStats st;
    REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, 0.0, 0.0, true, 0.0f, false, 60.0f},
                                 kW, kH, {}, &st));
    const auto px = readRgba();
    const size_t c = (static_cast<size_t>(kH / 2) * kW + static_cast<size_t>(kW / 2)) * 4u;
    CHECK(static_cast<int>(px[c + 0]) >= 64);
    CHECK(static_cast<int>(px[c + 1]) >= 55);
    CHECK(static_cast<int>(px[c + 0]) > static_cast<int>(px[c + 1]) + 2);

    // Verify HDR .rgb intensity > 1.0 is preserved for Bloom without alpha overflow
    auto hdrWhite = iota::model::createDefaultElement(iota::model::ElementType::Image);
    hdrWhite.props.setString("customImage", "internalres:white", "img", "1_image");
    hdrWhite.props.setColorHsla("Color", {0.0f, 0.0f, 1.0f, 1.0f}, "2_color");
    hdrWhite.props.setFloat("hdrIntensity", 4.0f, 0.0f, 16.0f, "2_color");
    hdrWhite.props.setString("blendMode", "Add", "_child Add", "0_general");
    hdrWhite.props.setVec2("scale", {0.5f, 0.5f}, 0.0f, 2.0f, "0_general");
    proj.compositions[1].elements.push_back(std::move(hdrWhite));
    proj.compositions[0].props.setBool("bloomEnabled", true, "3_hdrPost", "b");

    REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, 0.1, 0.1, true, 0.0f, false, 60.0f},
                                 kW, kH, {}, &st));
    CHECK(st.hdrPipelineActive);
    CHECK(st.bloomActive);
    const auto hdrPx = readRgba();
    CHECK(hdrPx[c + 0] >= 250);
    CHECK(hdrPx[c + 1] >= 250);
    CHECK(hdrPx[c + 2] >= 250);
  }

  SUBCASE("CustomShader persists compile error across cached frames without trailing NUL") {
    iota::model::Project proj;
    proj.compositions.push_back(iota::model::createDefaultComposition(0));
    proj.compositions.push_back(iota::model::createDefaultComposition(1));

    auto cs = iota::model::createDefaultElement(iota::model::ElementType::CustomShader);
    cs.props.setString("TargetImage", "internalres:white", "img", "1_appearance");
    cs.props.setString("shaderFragment", "void main() { iota_undefined_sym = 1.0; }", "etxt", "shader");
    proj.compositions[0].elements.push_back(std::move(cs));

    renderer.resetSimulation();
    iota::engine::RenderFrameStats st0;
    REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, 0.0, 0.0, true, 0.0f, false, 60.0f},
                                 kW, kH, {}, &st0));
    REQUIRE(!st0.lastError.empty());
    CHECK(st0.lastError.back() != '\0');
    CHECK(st0.lastError.find("Fragment shader") != std::string::npos);

    iota::engine::RenderFrameStats st1;
    REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, 0.02, 0.02, true, 0.0f, false, 60.0f},
                                 kW, kH, {}, &st1));
    CHECK(st1.lastError == st0.lastError);
  }

  SUBCASE("CustomShader GLSL 450 adapter ignores comments when injecting v_uv and v_color") {
    iota::model::Project proj;
    proj.compositions.push_back(iota::model::createDefaultComposition(0));
    proj.compositions.push_back(iota::model::createDefaultComposition(1));

    auto cs = iota::model::createDefaultElement(iota::model::ElementType::CustomShader);
    cs.props.setString("TargetImage", "internalres:white", "img", "1_appearance");
    cs.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");
    cs.props.setString("shaderFragment",
                       "// Comment mentioning in vec2 v_uv; and in vec4 v_color;\n"
                       "void main() {\n"
                       "  gl_FragColor = vec4(v_uv.x, 0.25, v_uv.y, 1.0) * v_color;\n"
                       "}\n",
                       "etxt", "shader");
    proj.compositions[0].elements.push_back(std::move(cs));

    renderer.resetSimulation();
    iota::engine::RenderFrameStats st;
    for (int f = 0; f < 2; ++f) {
      REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, false, 60.0f},
                                   kW, kH, {}, &st));
      CHECK(st.lastError.empty());
    }
    const auto px = readRgba();
    const size_t leftIdx = (static_cast<size_t>(kH / 2) * kW + 6u) * 4u;
    const size_t rightIdx = (static_cast<size_t>(kH / 2) * kW + static_cast<size_t>(kW - 7)) * 4u;
    CHECK(static_cast<int>(px[leftIdx + 0]) < 45);
    CHECK(static_cast<int>(px[rightIdx + 0]) > 200);
  }

  SUBCASE("Streaming ring buffer, R11G11B10F bloom, FXAA toggle, and opaque ShakeEffect FBO swap stay coherent") {
    iota::model::Project proj;
    proj.compositions.push_back(iota::model::createDefaultComposition(0));
    proj.compositions.push_back(iota::model::createDefaultComposition(1));
    proj.compositions[0].props.setBool("hdrPipeline", true, "3_hdrPost", "b");
    proj.compositions[0].props.setBool("bloomEnabled", true, "3_hdrPost", "b");

    auto bg = iota::model::createDefaultElement(iota::model::ElementType::Image);
    bg.props.setString("customImage", "internalres:white", "img", "1_image");
    bg.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");
    bg.props.setColorHsla("Color", {0.50f, 1.0f, 0.50f, 1.0f}, "2_color");  // Cyan
    proj.compositions[0].elements.push_back(std::move(bg));

    auto shake = iota::model::createDefaultElement(iota::model::ElementType::ShakeEffect);
    shake.props.setString("TargetImage", "composition:0", "img", "1_appearance");
    shake.props.setString("blendMode", "Opaque", "_child Add Alpha Darken PreMul Screen Replace", "0_general");
    shake.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");
    shake.props.setFloat("amplitude", 0.03f, 0.0f, 5.0f, "2_shakeMaster");
    shake.props.setFloat("rgbPhase", 0.05f, 0.0f, 1.0f, "4_motionAndRgb");
    proj.compositions[0].elements.push_back(std::move(shake));

    renderer.resetSimulation();
    iota::engine::RenderFrameStats st;
    for (int f = 0; f < 10; ++f) {
      const bool useFxaa = (f % 2) == 0;
      REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, f / 60.0, f / 60.0, true, 0.0f, useFxaa, 60.0f},
                                   kW, kH, {}, &st));
      CHECK(st.lastError.empty());
      CHECK(st.hdrPipelineActive);
      CHECK(st.bloomActive);
      CHECK(st.cpuDspMs >= 0.0f);
      CHECK(st.cpuRenderMs >= 0.0f);
    }
    const auto px = readRgba();
    const size_t c = (static_cast<size_t>(kH / 2) * kW + static_cast<size_t>(kW / 2)) * 4u;
    CHECK(static_cast<int>(px[c + 1]) > 140);
    CHECK(static_cast<int>(px[c + 2]) > 140);
    CHECK(static_cast<int>(px[c + 0]) + 20 < static_cast<int>(px[c + 1]));
  }

  SUBCASE("Live async PBO LayerMask particle path renders cleanly across warmup frames") {
    iota::model::Project lm;
    lm.version = "10";
    auto comp0 = iota::model::createDefaultComposition(0);
    auto pt = iota::model::createDefaultElement(iota::model::ElementType::Particles);
    pt.props.getOrAdd("SpawnArea").value = std::string("LayerMask");
    pt.props.setString("emitterLayer", "composition:1", "img composition:0", "1_overall");
    pt.props.setString("particleShape", "Sphere", "sel Sprite Sphere GlowSphere Star BokehRing Streaklet", "appearance");
    pt.props.setFloat("particlesPerSec", 500.0f, 0.0f, 5000.0f, "1_overall");
    pt.props.setFloat("preRunSec", 0.5f, 0.0f, 10.0f, "1_overall");
    pt.props.setFloat("particleScale", 5.0f, 0.25f, 16.0f, "appearance");
    pt.props.setColorHsla("ColorFrom", {0.5f, 1.0f, 0.5f, 1.0f}, "appearance");
    pt.props.setColorHsla("ColorTo", {0.5f, 1.0f, 0.5f, 1.0f}, "appearance");
    comp0.elements.push_back(std::move(pt));

    auto comp1 = iota::model::createDefaultComposition(1);
    auto mask = iota::model::createDefaultElement(iota::model::ElementType::Image);
    mask.props.setString("customImage", "internalres:white", "img", "1_image");
    mask.props.setVec2("position", {0.75f, 0.5f}, -1.0f, 1.0f, "0_general");
    mask.props.setVec2("scale", {0.4f, 0.8f}, 0.0f, 2.0f, "0_general");
    comp1.elements.push_back(std::move(mask));

    lm.compositions.push_back(std::move(comp0));
    lm.compositions.push_back(std::move(comp1));

    renderer.resetSimulation();
    iota::engine::RenderFrameStats st;
    for (int f = 0; f < 6; ++f) {
      REQUIRE(renderer.renderFrame(lm, audio, {1.0f / 60.0f, f / 60.0, f / 60.0, false, 0.0f, true, 60.0f},
                                   kW, kH, {}, &st));
      CHECK(st.lastError.empty());
    }
    CHECK(st.activeParticles > 5);
    const auto px = readRgba();
    uint64_t rightBlueSum = 0;
    for (int y = 0; y < kH; ++y) {
      for (int x = kW / 2; x < kW; ++x) {
        rightBlueSum += px[static_cast<size_t>((y * kW + x) * 4) + 2];
      }
    }
    CHECK(rightBlueSum > 5000);
  }

  SUBCASE("FontAtlasCache shaped layout cache hits, bounds eviction, and invalidates cleanly") {
    iota::engine::FontAtlasCache atlas;
    atlas.setDevice(&device);

    iota::engine::TextStyle style;
    style.outlineWidthPx = 1.5f;
    style.shadowOffsetPx = {2.0f, 2.0f};
    const iota::core::RectF bounds{0.0f, 0.0f, 240.0f, 64.0f};
    const iota::core::ColorRgba white{255, 255, 255, 255};

    std::vector<iota::rhi::VertexPosColUv> v0, v1;
    std::vector<uint32_t> i0, i1;
    const auto t0 = atlas.buildTextQuads("internal_2", "Iota Perf Cache", 24.0f, bounds, white, style, v0, i0);
    const auto t1 = atlas.buildTextQuads("internal_2", "Iota Perf Cache", 24.0f, bounds, white, style, v1, i1);
    REQUIRE(t0);
    REQUIRE(t1);
    REQUIRE(v0.size() == v1.size());
    REQUIRE(i0 == i1);
    CHECK(!v0.empty());
    for (size_t vi = 0; vi < v0.size(); ++vi) {
      CHECK(v0[vi].x == doctest::Approx(v1[vi].x));
      CHECK(v0[vi].y == doctest::Approx(v1[vi].y));
    }

    const auto stHit = atlas.stats();
    if (stHit.usingNativeFontBackend) {
      CHECK(stHit.cachedShapedLayouts == 1);
      CHECK(stHit.shapedLayoutHits >= 1);
      for (int n = 0; n < 70; ++n) {
        std::vector<iota::rhi::VertexPosColUv> vn;
        std::vector<uint32_t> in;
        const std::string msg = "EvictMsg_" + std::to_string(n);
        atlas.buildTextQuads("internal_2", msg, 24.0f, bounds, white, style, vn, in);
      }
      CHECK(atlas.stats().cachedShapedLayouts <= 64);
      atlas.invalidateCustomFonts();
      CHECK(atlas.stats().cachedShapedLayouts == 0);
    }
    atlas.shutdown();
  }

  SUBCASE("CustomShader resolves local shader assets, strips BOM, hot-reloads, and routes URIs") {
    iota::model::Project proj;
    proj.compositions.push_back(iota::model::createDefaultComposition(0));
    proj.compositions.push_back(iota::model::createDefaultComposition(1));

    auto cs = iota::model::createDefaultElement(iota::model::ElementType::CustomShader);
    CHECK(iota::model::assignAssetToElement(cs, "local:bg.png", false));
    CHECK(cs.props.getString("TargetImage") == "local:bg.png");
    CHECK(iota::model::assignAssetToElement(cs, "local:pass.vert", false));
    CHECK(cs.props.getString("shaderVertex") == "local:pass.vert");
    CHECK(iota::model::assignAssetToElement(cs, "local:rain.glsl", false));
    CHECK(cs.props.getString("shaderFragment") == "local:rain.glsl");

    cs.props.setString("TargetImage", "internalres:white", "img", "1_appearance");
    cs.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");
    cs.props.setString("shaderVertex", "", "etxt", "shader");

    const std::string redShader =
        "\xEF\xBB\xBF"
        "void main() {\n"
        "  gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
        "}\n";
    proj.localAssets["rain.glsl"] = std::vector<uint8_t>(redShader.begin(), redShader.end());
    proj.compositions[0].elements.push_back(std::move(cs));

    renderer.resetSimulation();
    iota::engine::RenderFrameStats st;
    REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, 0.0, 0.0, true, 0.0f, false, 60.0f},
                                 kW, kH, {}, &st));
    CHECK(st.lastError.empty());
    const size_t c = (static_cast<size_t>(kH / 2) * kW + static_cast<size_t>(kW / 2)) * 4u;
    const auto redPx = readRgba();
    CHECK(static_cast<int>(redPx[c + 0]) > 220);
    CHECK(static_cast<int>(redPx[c + 1]) < 30);

    // Updating the local shader asset automatically hot-reloads the compiled program
    const std::string greenShader =
        "void main() {\n"
        "  gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n"
        "}\n";
    proj.localAssets["rain.glsl"] = std::vector<uint8_t>(greenShader.begin(), greenShader.end());
    REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, 0.02, 0.02, true, 0.0f, false, 60.0f},
                                 kW, kH, {}, &st));
    CHECK(st.lastError.empty());
    const auto greenPx = readRgba();
    CHECK(static_cast<int>(greenPx[c + 1]) > 220);
    CHECK(static_cast<int>(greenPx[c + 0]) < 30);

    // Missing local shader reports a diagnostic error
    proj.localAssets.erase("rain.glsl");
    REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, 0.04, 0.04, true, 0.0f, false, 60.0f},
                                 kW, kH, {}, &st));
    CHECK(st.lastError.find("local:rain.glsl") != std::string::npos);
  }

  SUBCASE("BuiltinShaderLibrary presets compile cleanly on OpenGL 4.5 and support canvas-below capture") {
    const auto& presets = iota::engine::listBuiltinShaderPresets();
    CHECK(presets.size() == 15);

    iota::model::Project dedupProj;
    const auto* grain = iota::engine::findBuiltinShaderPreset("film_grain");
    REQUIRE(grain != nullptr);
    CHECK(iota::engine::instantiateBuiltinShaderAsset(dedupProj, *grain) == "FilmGrain.glsl");
    CHECK(iota::engine::instantiateBuiltinShaderAsset(dedupProj, *grain) == "FilmGrain_2.glsl");
    CHECK(dedupProj.localAssets.size() == 2);

    for (const auto& preset : presets) {
      INFO("Testing built-in shader preset: " << preset.id);
      iota::model::Project proj;
      proj.compositions.push_back(iota::model::createDefaultComposition(0));

      // Render a bright white layer underneath so Post-FX shaders targeting composition:0 have input.
      auto bg = iota::model::createDefaultElement(iota::model::ElementType::Image);
      bg.props.setString("customImage", "internalres:white", "img", "1_image");
      bg.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");
      proj.compositions[0].elements.push_back(std::move(bg));

      const std::string assetName = iota::engine::instantiateBuiltinShaderAsset(proj, preset);
      auto cs = iota::model::createDefaultElement(iota::model::ElementType::CustomShader);
      cs.displayName = std::string(preset.displayName);
      REQUIRE(iota::model::assignAssetToElement(cs, "local:" + assetName, false));
      cs.props.getOrAdd("TargetImage").value = std::string(preset.defaultTarget);
      cs.props.getOrAdd("blendMode").value = std::string(preset.defaultBlendMode);
      cs.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");
      proj.compositions[0].elements.push_back(std::move(cs));

      renderer.resetSimulation();
      iota::engine::RenderFrameStats st;
      REQUIRE(renderer.renderFrame(proj, audio, {1.0f / 60.0f, 0.25, 0.25, true, 0.0f, false, 60.0f},
                                   kW, kH, {}, &st));
      CHECK_MESSAGE(st.lastError.empty(), "GLSL compile/render error in " << preset.id << ": " << st.lastError);
      CHECK(st.drawCalls >= 2);
      if (preset.id == "film_grain") {
        const auto px = readRgba();
        const size_t c = (static_cast<size_t>(kH / 2) * kW + static_cast<size_t>(kW / 2)) * 4u;
        CHECK(static_cast<int>(px[c + 0]) > 200);
      }
    }
  }

  renderer.shutdown();
  device.shutdown();
  glfwDestroyWindow(win);
  glfwTerminate();
}
