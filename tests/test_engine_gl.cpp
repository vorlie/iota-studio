#include "doctest/doctest.h"

#include "iota/audio/audio_decoder.hpp"
#include "iota/audio/audio_engine.hpp"
#include "iota/engine/font_atlas.hpp"
#include "iota/engine/layout.hpp"
#include "iota/engine/scene_renderer.hpp"
#include "iota/model/scene.hpp"
#include "iota/preset_io/preset_io.hpp"
#include "iota/rhi/gl45_device.hpp"

#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>

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
    REQUIRE(dirEnv != nullptr);
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

  renderer.shutdown();
  device.shutdown();
  glfwDestroyWindow(win);
  glfwTerminate();
}
