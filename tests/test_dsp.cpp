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

#include "iota/audio/audio_engine.hpp"
#include "iota/dsp/a_weighting.hpp"
#include "iota/dsp/filters.hpp"
#include "iota/dsp/mvar_evaluator.hpp"
#include "iota/dsp/sample_providers.hpp"
#include "iota/dsp/sound_keys.hpp"
#include "iota/dsp/windows.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {

std::vector<float> makeSine(float freqHz, int sampleRate, int count, float amp = 0.5f) {
  std::vector<float> pcm(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sampleRate);
    pcm[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * iota::dsp::kPi * freqHz * t));
  }
  return pcm;
}

}  // namespace

TEST_CASE("DSP window functions and IEC A-weighting match spec") {
  using namespace iota::dsp;

  CHECK(hammingWindow(0, 64) == doctest::Approx(0.08f).epsilon(0.01));
  CHECK(hannWindow(0, 64) == doctest::Approx(0.0f).epsilon(0.001));
  CHECK(hannWindow(32, 65) == doctest::Approx(1.0f).epsilon(0.001));
  CHECK(kaiserWindow(0, 64, 0.8f) > 0.0f);
  CHECK(kaiserWindow(32, 64, 0.8f) == doctest::Approx(1.0f).epsilon(0.01));

  CHECK(aWeightingNormalizedGain(1000.0) == doctest::Approx(1.0f).epsilon(0.01));
  CHECK(aWeightingNormalizedGain(50.0) < 0.25f);
}

TEST_CASE("Spectrum (kiss_fftr) and Spectrum2 detect resonant sine tone peak cleanly") {
  using namespace iota::dsp;

  const auto tone = makeSine(440.0f, 44100, 4096, 0.5f);

  for (SampleProviderKind kind : {SampleProviderKind::Spectrum, SampleProviderKind::Spectrum2}) {
    SpectrumConfig cfg;
    cfg.kind = kind;
    cfg.sampleOutCount = 64;
    cfg.lowerHz = 40.0f;
    cfg.higherHz = 12000.0f;
    cfg.hzLinearFactor = 0.0f;
    cfg.filterRadius = 1.0f;
    cfg.filterStrength = 0.2f;
    cfg.smooth = 1.0f;

    AudioAnalyzer analyzer;
    analyzer.setConfig(cfg);

    AudioAnalysisResult res;
    for (int f = 0; f < 10; ++f) {
      AudioFrameInput fi{tone, 44100, 1.0f / 60.0f, f / 60.0, f / 60.0, 5.0, true, false};
      res = analyzer.process(fi);
    }

    REQUIRE(res.smoothedBars.size() == 64);
    const auto maxIt = std::max_element(res.smoothedBars.begin(), res.smoothedBars.end());
    const int maxIdx = static_cast<int>(std::distance(res.smoothedBars.begin(), maxIt));

    // Near 440 Hz on a 40..12000 Hz log grid (~bin 27/64) and far exceeds 10 kHz high bins
    CHECK(*maxIt > 1.0f);
    CHECK(maxIdx > 15);
    CHECK(maxIdx < 45);
    CHECK(*maxIt > res.smoothedBars.back() * 5.0f);
  }
}

TEST_CASE("BeatDetector responds to bass energy and decays on silence") {
  using namespace iota::dsp;

  SpectrumConfig cfg;
  cfg.kind = SampleProviderKind::Spectrum2;
  cfg.sampleOutCount = 64;
  cfg.beatRangeBarFirst = 0.0f;
  cfg.beatRangeBarLast = 0.25f;
  cfg.beatRangeValueLower = 0.5f;
  cfg.beatRangeValueHigher = 25.0f;
  cfg.beatSmooth = 0.6f;

  AudioAnalyzer analyzer;
  analyzer.setConfig(cfg);

  const auto bassTone = makeSine(80.0f, 44100, 4096, 0.8f);
  AudioAnalysisResult loudRes;
  for (int i = 0; i < 12; ++i) {
    loudRes = analyzer.process({bassTone, 44100, 1.0f / 60.0f, i / 60.0, i / 60.0, 5.0, true, false});
  }
  CHECK(loudRes.beat > 0.15f);

  const std::vector<float> silence(4096, 0.0f);
  AudioAnalysisResult silentRes;
  for (int i = 0; i < 20; ++i) {
    silentRes = analyzer.process({silence, 44100, 1.0f / 60.0f, (12 + i) / 60.0, 0.0, 5.0, true, false});
  }
  CHECK(silentRes.beat < loudRes.beat * 0.25f);
}

TEST_CASE("BarHistoryRing supports reactionDelay and spatial softness smoothing") {
  using namespace iota::dsp;

  BarHistoryRing ring(10);
  const std::array<float, 4> frameA{0.0f, 10.0f, 0.0f, 0.0f};
  const std::array<float, 4> frameB{0.0f, 0.0f, 0.0f, 0.0f};
  ring.pushFrame(frameA);
  ring.pushFrame(frameB);

  std::vector<float> out;
  ring.sampleBars(1, 0, 0, out);
  REQUIRE(out.size() == 4);
  CHECK(out[1] == doctest::Approx(10.0f));

  ring.sampleBars(1, 0, 1, out);
  CHECK(out[1] < 10.0f);
  CHECK(out[0] > 0.0f);
}

TEST_CASE("MVarEvaluator exercises all 20 modes safely") {
  using namespace iota::dsp;
  using namespace iota::model;

  MVarEvaluator eval;
  MVarEvalContext ctx;
  ctx.dt = 1.0f / 60.0f;
  ctx.totalTimeSec = 1.25;
  ctx.playAccumTimeSec = 0.75;
  ctx.trackPositionSec = 2.0;
  ctx.trackDurationSec = 4.0;
  ctx.beat = 0.8f;
  ctx.peakBarIndex = 0.4f;
  ctx.isPlaying = true;

  for (std::string_view mode : allMeasureWhatModes()) {
    MVariableFloat var;
    var.slots.push_back({std::string(mode), 0.5f, 0.5f});
    const auto v = eval.evaluateVec2(MVarEvaluator::makeKey(42, mode), var, ctx);
    CHECK(std::isfinite(v.x));
    CHECK(std::isfinite(v.y));
  }

  CHECK(eval.evaluateVec2(1, MVariableFloat{{{"Constant", 0.3f, 0.7f}}}, ctx).y == doctest::Approx(0.7f));
  CHECK(eval.evaluateVec2(2, MVariableFloat{{{"TrackPosition", 2.0f, 4.0f}}}, ctx).x == doctest::Approx(1.0f));
}

TEST_CASE("Spectrum2 wraps freqShift modulo tileLen for Trap Nation bass templates") {
  using namespace iota::dsp;

  SpectrumConfig cfg;
  cfg.kind = SampleProviderKind::Spectrum2;
  cfg.sampleOutCount = 250;
  cfg.audioDurationMs = 180;
  cfg.highQualityAudioCapture = false;
  cfg.lowerHz = 0.1f;
  cfg.higherHz = 460.0f;
  cfg.hzLinearFactor = 1.0f;
  cfg.freqShift = 0.75f;
  cfg.smooth = 5.0f;
  cfg.filterRadius = 1.0f;
  cfg.filterStrength = 0.1f;
  cfg.lowerFreqMagnitude = 0.0f;
  cfg.outputMultiplier = 4.5f;

  AudioAnalyzer analyzer;
  analyzer.setConfig(cfg);
  const auto bassTone = makeSine(110.0f, 44100, 8192, 0.7f);
  AudioAnalysisResult res;
  for (int i = 0; i < 6; ++i) {
    res = analyzer.process({bassTone, 44100, 1.0f / 60.0f, i / 60.0, i / 60.0, 5.0, true, false});
  }

  REQUIRE(res.smoothedBars.size() == 250);
  const auto maxIt = std::max_element(res.smoothedBars.begin(), res.smoothedBars.end());
  const int maxIdx = static_cast<int>(std::distance(res.smoothedBars.begin(), maxIt));
  // With shift = floor(250 * 0.75) = 187, 110 Hz (~60 bins above 0.1 Hz) wraps to ~bar 247 or ~0..249
  CHECK(*maxIt > 50.0f);
  CHECK(maxIdx >= 235);
}

TEST_CASE("Frame-rate independent temporal smoothing matches 60 FPS vs 144 FPS") {
  using namespace iota::dsp;

  // Mathematical equivalence of continuous & discrete normalized alphas
  const float a60 = expSmoothingAlphaMs(1.0f / 60.0f, 80.0f);
  const float a144 = expSmoothingAlphaMs(1.0f / 144.0f, 80.0f);
  CHECK(std::pow(1.0f - a144, 144.0f) == doctest::Approx(std::pow(1.0f - a60, 60.0f)).epsilon(1e-4));

  const float r60 = frameRateIndependentAlpha(0.35f, 1.0f / 60.0f);
  const float r144 = frameRateIndependentAlpha(0.35f, 1.0f / 144.0f);
  CHECK(r60 == doctest::Approx(0.35f).epsilon(1e-5));
  CHECK(std::pow(1.0f - r144, 144.0f) == doctest::Approx(std::pow(1.0f - r60, 60.0f)).epsilon(1e-4));

  const auto tone = makeSine(220.0f, 44100, 4096, 0.5f);

  auto runAnalyzer = [&](int fps, bool useAttackRelease) -> AudioAnalysisResult {
    SpectrumConfig cfg;
    cfg.kind = SampleProviderKind::Spectrum2;
    cfg.sampleOutCount = 64;
    cfg.smooth = 0.45f;
    cfg.useAttackRelease = useAttackRelease;
    cfg.temporalSmooth = 0.5f;
    cfg.attackMs = 40.0f;
    cfg.releaseMs = 200.0f;
    cfg.beatSmooth = 0.35f;

    AudioAnalyzer analyzer;
    analyzer.setConfig(cfg);
    const float dt = 1.0f / static_cast<float>(fps);
    AudioAnalysisResult res;

    // 0.25s active tone followed by 0.25s silence (divisible cleanly by both 60 and 144 Hz)
    const int activeFrames = fps / 4;
    const int silentFrames = fps / 4;
    for (int f = 0; f < activeFrames; ++f) {
      const double t = static_cast<double>(f) * dt;
      res = analyzer.process({tone, 44100, dt, t, t, 5.0, true, false});
    }
    for (int f = 0; f < silentFrames; ++f) {
      const double t = 0.25 + static_cast<double>(f) * dt;
      res = analyzer.process({{}, 44100, dt, t, t, 5.0, true, false});
    }
    return res;
  };

  for (bool useAR : {true, false}) {
    const auto r60Res = runAnalyzer(60, useAR);
    const auto r144Res = runAnalyzer(144, useAR);
    REQUIRE(r60Res.smoothedBars.size() == 64);
    REQUIRE(r144Res.smoothedBars.size() == 64);
    const float peak60 = *std::max_element(r60Res.smoothedBars.begin(), r60Res.smoothedBars.end());
    const float peak144 = *std::max_element(r144Res.smoothedBars.begin(), r144Res.smoothedBars.end());
    CHECK(peak60 > 1.0f);
    CHECK(peak144 == doctest::Approx(peak60).epsilon(0.06));
    CHECK(r144Res.beat == doctest::Approx(r60Res.beat).epsilon(0.06));
  }
}

TEST_CASE("Asymmetric attack/release responds quickly on rise and decays slowly") {
  using namespace iota::dsp;

  SpectrumConfig cfg;
  cfg.kind = SampleProviderKind::Spectrum;
  cfg.sampleOutCount = 64;
  cfg.useAttackRelease = true;
  cfg.temporalSmooth = 0.5f;
  cfg.attackMs = 12.0f;
  cfg.releaseMs = 350.0f;

  AudioAnalyzer analyzer;
  analyzer.setConfig(cfg);
  const auto tone = makeSine(440.0f, 44100, 4096, 0.5f);

  // Reach fast peak in ~50 ms (3 frames at 60 FPS)
  AudioAnalysisResult attackRes;
  for (int f = 0; f < 3; ++f) {
    attackRes = analyzer.process({tone, 44100, 1.0f / 60.0f, f / 60.0, f / 60.0, 5.0, true, false});
  }
  const float attackPeak = *std::max_element(attackRes.smoothedBars.begin(), attackRes.smoothedBars.end());
  REQUIRE(attackPeak > 10.0f);

  // Slow release over ~50 ms should retain > 75% of peak (exp(-50/350) ~ 0.867)
  AudioAnalysisResult releaseRes;
  for (int f = 0; f < 3; ++f) {
    releaseRes = analyzer.process({{}, 44100, 1.0f / 60.0f, (f + 3) / 60.0, (f + 3) / 60.0, 5.0, true, false});
  }
  const float afterRelease = *std::max_element(releaseRes.smoothedBars.begin(), releaseRes.smoothedBars.end());
  CHECK(afterRelease > attackPeak * 0.75f);
  CHECK(afterRelease < attackPeak);
}

TEST_CASE("FrequencySmoother, time-indexed BarHistoryRing, and MVarEvaluator parity") {
  using namespace iota::dsp;

  std::vector<float> impulse = {0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f, 0.0f};
  std::vector<float> out;
  FrequencySmoother smoother;

  smoother.configure(FrequencySmoothMode::None, 2.0f, 1.0f);
  smoother.apply(impulse, out);
  CHECK(out[3] == doctest::Approx(100.0f));

  smoother.configure(FrequencySmoothMode::MovingAverage, 2.0f, 1.0f);
  smoother.apply(impulse, out);
  const float maPeak = out[3];
  CHECK(maPeak == doctest::Approx(20.0f).epsilon(0.01));

  smoother.configure(FrequencySmoothMode::Gaussian, 1.2f, 1.0f);
  smoother.apply(impulse, out);
  CHECK(out[3] > maPeak);
  CHECK(out[2] > 0.0f);

  smoother.configure(FrequencySmoothMode::SavitzkyGolay, 2.0f, 1.0f);
  smoother.apply(impulse, out);
  CHECK(out[3] > maPeak * 2.0f);
  CHECK(out[0] >= 0.0f);

  auto runHistoryAtFps = [](int fps) {
    BarHistoryRing ring(64);
    const float dt = 1.0f / static_cast<float>(fps);
    const int totalFrames = fps / 4;  // 0.25s exact at both 60 and 144 Hz
    for (int f = 1; f <= totalFrames; ++f) {
      const float val = (static_cast<float>(f) * dt) * 100.0f;
      std::array<float, 4> bars = {val, val * 0.5f, val * 0.25f, val};
      ring.pushFrame(bars, dt);
    }
    std::vector<float> sampled;
    ring.sampleBars(6, 2, 0, sampled);
    return sampled;
  };
  const auto h60 = runHistoryAtFps(60);
  const auto h144 = runHistoryAtFps(144);
  REQUIRE(h60.size() == 4);
  REQUIRE(h144.size() == 4);
  CHECK(h144[0] == doctest::Approx(h60[0]).epsilon(0.03));

  auto runMVarAtFps = [](int fps, std::string_view mode) {
    MVarEvaluator ev;
    iota::model::MVariableFloat var;
    var.slots.push_back({std::string(mode), 1.0f, 0.8f});
    const float dt = 1.0f / static_cast<float>(fps);
    iota::core::Vec2 outVec{};
    const int steps = fps / 2;  // 0.5s exact
    for (int f = 0; f < steps; ++f) {
      MVarEvalContext ctx;
      ctx.dt = dt;
      ctx.totalTimeSec = static_cast<double>(f + 1) * dt;
      ctx.playAccumTimeSec = ctx.totalTimeSec;
      ctx.beat = 0.75f;
      ctx.isPlaying = true;
      ctx.frameCounter = static_cast<uint32_t>(f);
      outVec = ev.evaluateVec2(1, var, ctx);
    }
    return outVec;
  };

  for (std::string_view m : {"TotalTimeAndBeat", "ConstantShake", "BeatCamShakeLess"}) {
    const auto v60 = runMVarAtFps(60, m);
    const auto v144 = runMVarAtFps(144, m);
    CHECK(v144.x == doctest::Approx(v60.x).epsilon(0.03).scale(1.0));
    CHECK(v144.y == doctest::Approx(v60.y).epsilon(0.03).scale(1.0));
  }
}

TEST_CASE("CurveInterpolationMode subdivides open and closed bar arrays accurately") {
  using namespace iota::dsp;

  const std::vector<float> ctrl = {0.0f, 10.0f, 0.0f, 10.0f};
  std::vector<float> out;

  interpolateBarCurve(ctrl, CurveInterpolationMode::Linear, 4, true, out);
  REQUIRE(out.size() == 16);
  CHECK(out[0] == doctest::Approx(0.0f));
  CHECK(out[2] == doctest::Approx(5.0f));
  CHECK(out[4] == doctest::Approx(10.0f));

  interpolateBarCurve(ctrl, CurveInterpolationMode::CatmullRom, 4, false, out);
  REQUIRE(out.size() == 13);
  CHECK(out.front() == doctest::Approx(0.0f));
  CHECK(out[4] == doctest::Approx(10.0f));
  CHECK(out.back() == doctest::Approx(10.0f));

  interpolateBarCurve(ctrl, CurveInterpolationMode::CubicBSpline, 4, true, out);
  REQUIRE(out.size() == 16);
  const auto [minIt, maxIt] = std::minmax_element(out.begin(), out.end());
  CHECK(*minIt >= 0.0f);
  CHECK(*maxIt <= 10.0f);

  const std::vector<float> flat = {5.0f, 5.0f, 5.0f, 5.0f};
  interpolateBarCurve(flat, CurveInterpolationMode::CubicBSpline, 4, true, out);
  CHECK(out[7] == doctest::Approx(5.0f).epsilon(1e-4));
}

TEST_CASE("SoundKeys isolates frequency bands and evaluates Peak/Average/OnOff modes") {
  using namespace iota::dsp;

  auto analyzeTone = [](float freqHz) {
    SpectrumConfig scfg;
    scfg.kind = SampleProviderKind::Spectrum;
    scfg.sampleOutCount = 64;
    scfg.lowerHz = 40.0f;
    scfg.higherHz = 12000.0f;
    scfg.hzLinearFactor = 0.0f;
    scfg.smooth = 1.0f;

    AudioAnalyzer analyzer;
    analyzer.setConfig(scfg);
    const auto tone = makeSine(freqHz, 44100, 4096, 0.5f);
    AudioAnalysisResult ares;
    for (int f = 0; f < 6; ++f) {
      ares = analyzer.process({tone, 44100, 1.0f / 60.0f, f / 60.0, f / 60.0, 5.0, true, false});
    }

    SoundKeysConfig skCfg;
    for (auto& r : skCfg.ranges) {
      r.falloffMode = SoundKeysFalloffMode::None;
      r.minAmplitude = 2.0f;
      r.maxAmplitude = 40.0f;
    }
    SoundKeysProcessor proc;
    return proc.process(ares.smoothedBars, skCfg, 1.0f / 60.0f);
  };

  const auto bassRes = analyzeTone(120.0f);
  CHECK(bassRes.outputs[0] > 0.3f);
  CHECK(bassRes.outputs[2] < 0.05f);

  const auto trebleRes = analyzeTone(4000.0f);
  CHECK(trebleRes.outputs[2] > 0.3f);
  CHECK(trebleRes.outputs[0] < 0.05f);

  // Synthetic range mode and output scaling verification
  SoundKeysConfig modeCfg;
  modeCfg.ranges[0].minFreq = 0.0f;
  modeCfg.ranges[0].maxFreq = 1.0f;
  modeCfg.ranges[0].minAmplitude = 10.0f;
  modeCfg.ranges[0].maxAmplitude = 50.0f;
  modeCfg.ranges[0].falloffMode = SoundKeysFalloffMode::None;
  modeCfg.ranges[0].outputMin = 2.0f;
  modeCfg.ranges[0].outputMax = 10.0f;
  modeCfg.ranges[1].enabled = false;
  modeCfg.ranges[2].enabled = false;

  const std::array<float, 4> bars = {0.0f, 20.0f, 40.0f, 0.0f};
  SoundKeysProcessor proc;

  modeCfg.ranges[0].rangeMode = SoundKeysRangeMode::Peak;
  CHECK(proc.process(bars, modeCfg, 1.0f / 60.0f).outputs[0] == doctest::Approx(8.0f).epsilon(1e-4));

  modeCfg.ranges[0].rangeMode = SoundKeysRangeMode::Average;
  CHECK(proc.process(bars, modeCfg, 1.0f / 60.0f).outputs[0] == doctest::Approx(3.0f).epsilon(1e-4));

  modeCfg.ranges[0].rangeMode = SoundKeysRangeMode::OnOff;
  CHECK(proc.process(bars, modeCfg, 1.0f / 60.0f).outputs[0] == doctest::Approx(10.0f).epsilon(1e-4));

  const std::array<float, 4> quietBars = {0.0f, 5.0f, 8.0f, 0.0f};
  CHECK(proc.process(quietBars, modeCfg, 1.0f / 60.0f).outputs[0] == doctest::Approx(2.0f).epsilon(1e-4));
}

TEST_CASE("SoundKeys falloff modes and MVar bindings match across 60 FPS vs 144 FPS") {
  using namespace iota::dsp;

  const std::array<float, 8> hotBars = {50.0f, 50.0f, 50.0f, 50.0f, 50.0f, 50.0f, 50.0f, 50.0f};
  const std::array<float, 8> zeroBars = {};

  for (SoundKeysFalloffMode fm : {SoundKeysFalloffMode::None,
                                  SoundKeysFalloffMode::Linear,
                                  SoundKeysFalloffMode::Exponential,
                                  SoundKeysFalloffMode::Integrate}) {
    auto runAtFps = [&](int fps) {
      SoundKeysConfig cfg;
      cfg.ranges[0].minFreq = 0.0f;
      cfg.ranges[0].maxFreq = 1.0f;
      cfg.ranges[0].minAmplitude = 0.0f;
      cfg.ranges[0].maxAmplitude = 50.0f;
      cfg.ranges[0].falloffMode = fm;
      cfg.ranges[0].attackMs = 20.0f;
      cfg.ranges[0].falloffTimeSec = 0.5f;
      cfg.ranges[0].integrateSpeed = 2.0f;
      cfg.ranges[1].enabled = false;
      cfg.ranges[2].enabled = false;

      SoundKeysProcessor proc;
      const float dt = 1.0f / static_cast<float>(fps);
      const int quarterSecFrames = fps / 4;  // Exact integer at both 60 and 144 Hz
      SoundKeysResult res;
      for (int f = 0; f < quarterSecFrames; ++f) {
        res = proc.process(hotBars, cfg, dt);
      }
      if (fm != SoundKeysFalloffMode::Integrate) {
        for (int f = 0; f < quarterSecFrames; ++f) {
          res = proc.process(zeroBars, cfg, dt);
        }
      }
      return res.outputs[0];
    };

    const float v60 = runAtFps(60);
    const float v144 = runAtFps(144);
    CHECK(v144 == doctest::Approx(v60).epsilon(0.01));
    if (fm == SoundKeysFalloffMode::Linear || fm == SoundKeysFalloffMode::Integrate) {
      CHECK(v60 == doctest::Approx(0.5f).epsilon(0.02));
    }
  }

  auto evalSkMVarAtFps = [](int fps, std::string_view mode) {
    MVarEvaluator ev;
    iota::model::MVariableFloat var;
    var.slots.push_back({std::string(mode), 1.5f, 0.75f});
    const float dt = 1.0f / static_cast<float>(fps);
    const int steps = fps / 2;
    iota::core::Vec2 outVec{};
    for (int f = 0; f < steps; ++f) {
      MVarEvalContext ctx;
      ctx.dt = dt;
      ctx.totalTimeSec = static_cast<double>(f + 1) * dt;
      ctx.playAccumTimeSec = ctx.totalTimeSec;
      ctx.soundKeys = {0.8f, 0.4f, 0.2f};
      ctx.isPlaying = true;
      ctx.frameCounter = static_cast<uint32_t>(f);
      outVec = ev.evaluateVec2(7, var, ctx);
    }
    return outVec;
  };

  const auto sk1 = evalSkMVarAtFps(60, "SoundKeys1");
  CHECK(sk1.x == doctest::Approx(1.2f));
  CHECK(sk1.y == doctest::Approx(0.6f));

  const auto sk2 = evalSkMVarAtFps(60, "SoundKeys2");
  CHECK(sk2.x == doctest::Approx(0.6f));

  const auto sk3 = evalSkMVarAtFps(60, "SoundKeys3");
  CHECK(sk3.x == doctest::Approx(0.3f));

  for (std::string_view shakeMode : {"SoundKeysShake1", "SoundKeysShake2", "SoundKeysShake3"}) {
    const auto s60 = evalSkMVarAtFps(60, shakeMode);
    const auto s144 = evalSkMVarAtFps(144, shakeMode);
    CHECK(s144.x == doctest::Approx(s60.x).epsilon(0.03).scale(1.0));
    CHECK(s144.y == doctest::Approx(s60.y).epsilon(0.03).scale(1.0));
  }
}

TEST_CASE("SoundKeys auto-ceiling prevents spectrum cap-out on high-magnitude signals") {
  using namespace iota::dsp;

  SoundKeysConfig cfg;
  cfg.autoScaleCeiling = true;
  cfg.displayCeiling = 100.0f;
  cfg.ranges[0].minFreq = 0.0f;
  cfg.ranges[0].maxFreq = 1.0f;
  cfg.ranges[0].minAmplitude = 0.0f;
  cfg.ranges[0].maxAmplitude = 1500.0f;
  cfg.ranges[0].falloffMode = SoundKeysFalloffMode::None;
  cfg.ranges[1].enabled = false;
  cfg.ranges[2].enabled = false;

  SoundKeysProcessor proc;
  const std::array<float, 8> hot = {200.0f, 1200.0f, 400.0f, 100.0f, 50.0f, 20.0f, 10.0f, 5.0f};
  const auto rHot = proc.process(hot, cfg, 1.0f / 60.0f);
  CHECK(rHot.spectrumPeak == doctest::Approx(1200.0f));
  CHECK(rHot.effectiveCeiling == doctest::Approx(1500.0f));
  CHECK(rHot.outputs[0] == doctest::Approx(0.8f).epsilon(1e-4));

  const std::array<float, 8> mid = {100.0f, 600.0f, 200.0f, 50.0f, 25.0f, 10.0f, 5.0f, 2.0f};
  const auto rMid = proc.process(mid, cfg, 1.0f / 60.0f);
  CHECK(rMid.outputs[0] == doctest::Approx(0.4f).epsilon(1e-4));

  auto runDecayAtFps = [](int fps) {
    SoundKeysConfig c;
    c.autoScaleCeiling = true;
    c.displayCeiling = 100.0f;
    c.ranges[0].enabled = false;
    c.ranges[1].enabled = false;
    c.ranges[2].enabled = false;

    SoundKeysProcessor p;
    const std::array<float, 4> spike = {1000.0f, 100.0f, 50.0f, 10.0f};
    const std::array<float, 4> silent = {};
    const float dt = 1.0f / static_cast<float>(fps);
    p.process(spike, c, dt);
    SoundKeysResult r;
    for (int f = 0; f < fps; ++f) {
      r = p.process(silent, c, dt);
    }
    return r.effectiveCeiling;
  };
  CHECK(runDecayAtFps(144) == doctest::Approx(runDecayAtFps(60)).epsilon(0.01));
}

TEST_CASE("AudioProvider smoothing polarity: higher smooth/temporalSmooth/preSmooth/beatSmooth responds faster") {
  using namespace iota::dsp;

  const auto tone = makeSine(220.0f, 44100, 4096, 0.7f);
  constexpr float kDt = 1.0f / 60.0f;

  auto measureAttackAndReleaseRatio = [&](SpectrumConfig cfg, float& outAttackPeak, float& outReleaseRatio) {
    AudioAnalyzer az;
    az.setConfig(cfg);
    AudioAnalysisResult res;
    for (int f = 0; f < 2; ++f) {
      res = az.process({tone, 44100, kDt, f * kDt, f * kDt, 10.0, true, false});
    }
    outAttackPeak = *std::max_element(res.smoothedBars.begin(), res.smoothedBars.end());

    // Settle before measuring release decay ratio
    for (int f = 2; f < 120; ++f) {
      res = az.process({tone, 44100, kDt, f * kDt, f * kDt, 10.0, true, false});
    }
    const float steadyPeak = *std::max_element(res.smoothedBars.begin(), res.smoothedBars.end());
    for (int f = 0; f < 12; ++f) {
      res = az.process({{}, 44100, kDt, (120 + f) * kDt, (120 + f) * kDt, 10.0, true, false});
    }
    const float afterRelease = *std::max_element(res.smoothedBars.begin(), res.smoothedBars.end());
    outReleaseRatio = (steadyPeak > 1e-4f) ? (afterRelease / steadyPeak) : 0.0f;
  };

  SUBCASE("Legacy mode (useAttackRelease=false): higher smooth rises faster and falls faster") {
    SpectrumConfig slow;
    slow.kind = SampleProviderKind::Spectrum2;
    slow.sampleOutCount = 64;
    slow.useAttackRelease = false;
    slow.smooth = 0.15f;

    SpectrumConfig fast = slow;
    fast.smooth = 0.90f;

    float slowAtk = 0.0f, slowRelRatio = 0.0f;
    float fastAtk = 0.0f, fastRelRatio = 0.0f;
    measureAttackAndReleaseRatio(slow, slowAtk, slowRelRatio);
    measureAttackAndReleaseRatio(fast, fastAtk, fastRelRatio);
    CHECK(fastAtk > slowAtk * 1.5f);
    CHECK(fastRelRatio < slowRelRatio * 0.7f);
  }

  SUBCASE("Attack/Release mode (useAttackRelease=true): higher temporalSmooth and higher smooth both respond faster") {
    SpectrumConfig slowTs;
    slowTs.kind = SampleProviderKind::Spectrum2;
    slowTs.sampleOutCount = 64;
    slowTs.useAttackRelease = true;
    slowTs.smooth = 1.0f;
    slowTs.temporalSmooth = 0.10f;
    slowTs.attackMs = 45.0f;
    slowTs.releaseMs = 200.0f;

    SpectrumConfig fastTs = slowTs;
    fastTs.temporalSmooth = 0.90f;

    float slowAtk = 0.0f, slowRelRatio = 0.0f;
    float fastAtk = 0.0f, fastRelRatio = 0.0f;
    measureAttackAndReleaseRatio(slowTs, slowAtk, slowRelRatio);
    measureAttackAndReleaseRatio(fastTs, fastAtk, fastRelRatio);
    CHECK(fastAtk > slowAtk * 1.3f);
    CHECK(fastRelRatio < slowRelRatio * 0.6f);

    SpectrumConfig slowSm = slowTs;
    slowSm.temporalSmooth = 0.50f;
    slowSm.smooth = 0.20f;

    SpectrumConfig fastSm = slowSm;
    fastSm.smooth = 0.95f;

    float slowSmAtk = 0.0f, slowSmRelRatio = 0.0f;
    float fastSmAtk = 0.0f, fastSmRelRatio = 0.0f;
    measureAttackAndReleaseRatio(slowSm, slowSmAtk, slowSmRelRatio);
    measureAttackAndReleaseRatio(fastSm, fastSmAtk, fastSmRelRatio);
    CHECK(fastSmAtk > slowSmAtk * 1.3f);
    CHECK(fastSmRelRatio < slowSmRelRatio * 0.6f);
  }

  SUBCASE("Spectrum preSmooth: higher preSmooth rises faster") {
    SpectrumConfig slowPre;
    slowPre.kind = SampleProviderKind::Spectrum;
    slowPre.sampleOutCount = 64;
    slowPre.useAttackRelease = false;
    slowPre.smooth = 1.0f;
    slowPre.preSmooth = 0.15f;

    SpectrumConfig fastPre = slowPre;
    fastPre.preSmooth = 0.95f;

    float slowAtk = 0.0f, slowRelRatio = 0.0f;
    float fastAtk = 0.0f, fastRelRatio = 0.0f;
    measureAttackAndReleaseRatio(slowPre, slowAtk, slowRelRatio);
    measureAttackAndReleaseRatio(fastPre, fastAtk, fastRelRatio);
    CHECK(fastAtk > slowAtk * 1.5f);
  }

  SUBCASE("beatSmooth: higher beatSmooth responds faster on rise and decay") {
    auto evalBeatRiseAndDecay = [&](float beatSm, float& outRise, float& outDecayRatio) {
      SpectrumConfig cfg;
      cfg.kind = SampleProviderKind::Spectrum2;
      cfg.sampleOutCount = 64;
      cfg.useAttackRelease = false;
      cfg.smooth = 3.0f;  // Immediate bar response so test isolates beatSmooth envelope
      cfg.outputMultiplier = 2.0f;
      cfg.beatRangeBarFirst = 0.0f;
      cfg.beatRangeBarLast = 1.0f;
      cfg.beatRangeValueLower = 0.0f;
      cfg.beatRangeValueHigher = 20.0f;
      cfg.beatSmooth = beatSm;

      AudioAnalyzer az;
      az.setConfig(cfg);
      AudioAnalysisResult res;
      for (int f = 0; f < 2; ++f) {
        res = az.process({tone, 44100, kDt, f * kDt, f * kDt, 10.0, true, false});
      }
      outRise = res.beat;
      for (int f = 2; f < 90; ++f) {
        res = az.process({tone, 44100, kDt, f * kDt, f * kDt, 10.0, true, false});
      }
      const float steadyBeat = res.beat;
      for (int f = 0; f < 4; ++f) {
        res = az.process({{}, 44100, kDt, (90 + f) * kDt, (90 + f) * kDt, 10.0, true, false});
      }
      outDecayRatio = (steadyBeat > 1e-4f) ? (res.beat / steadyBeat) : 0.0f;
    };

    float slowBeatRise = 0.0f, slowBeatDecay = 0.0f;
    float fastBeatRise = 0.0f, fastBeatDecay = 0.0f;
    evalBeatRiseAndDecay(0.15f, slowBeatRise, slowBeatDecay);
    evalBeatRiseAndDecay(0.90f, fastBeatRise, fastBeatDecay);
    CHECK(fastBeatRise > slowBeatRise * 1.3f);
    CHECK(fastBeatDecay < slowBeatDecay * 0.5f);
  }
}

TEST_CASE("Forward audio window alignment and high smooth preserve stacked Spectrum2 lead order") {
  using namespace iota::audio;
  using namespace iota::dsp;

  SUBCASE("AudioEngine::extractMonoWindow captures forward from queryTime + visOffsetMs") {
    constexpr int kSr = 44100;
    AudioTrackData track;
    track.sampleRate = kSr;
    track.channels = 2;
    track.interleavedPcm.assign(static_cast<size_t>(kSr) * 2u, 0.0f);
    // Zero for t < 0.30s, non-zero 80 Hz tone for t >= 0.30s
    const int onsetSample = static_cast<int>(0.30 * kSr);
    for (int i = onsetSample; i < kSr; ++i) {
      const float s = 0.6f * static_cast<float>(std::sin(2.0 * kPi * 80.0 * static_cast<double>(i) / kSr));
      track.interleavedPcm[static_cast<size_t>(i) * 2u] = s;
      track.interleavedPcm[static_cast<size_t>(i) * 2u + 1u] = s;
    }
    rebuildMonoAndDuration(track);

    AudioEngine engine;
    REQUIRE(engine.loadTrack(std::move(track)));

    std::vector<float> win195;
    std::vector<float> win90;
    engine.extractMonoWindow(0.20, (195 * kSr) / 1000, 0.0f, win195);
    engine.extractMonoWindow(0.20, (90 * kSr) / 1000, 0.0f, win90);

    float maxAbs90 = 0.0f;
    for (float s : win90) {
      maxAbs90 = std::max(maxAbs90, std::abs(s));
    }
    CHECK(maxAbs90 < 1e-6f);

    float maxAbs195Head = 0.0f;
    for (int i = 0; i < 4000; ++i) {
      maxAbs195Head = std::max(maxAbs195Head, std::abs(win195[static_cast<size_t>(i)]));
    }
    float maxAbs195Tail = 0.0f;
    for (size_t i = 5000; i < win195.size(); ++i) {
      maxAbs195Tail = std::max(maxAbs195Tail, std::abs(win195[i]));
    }
    CHECK(maxAbs195Head < 1e-6f);
    CHECK(maxAbs195Tail > 0.25f);

    // Positive visOffsetMs advances window into active region; negative pads head safely
    std::vector<float> winShifted;
    engine.extractMonoWindow(0.20, (45 * kSr) / 1000, 150.0f, winShifted);
    float maxAbsShifted = 0.0f;
    for (float s : winShifted) {
      maxAbsShifted = std::max(maxAbsShifted, std::abs(s));
    }
    CHECK(maxAbsShifted > 0.25f);

    std::vector<float> winPreStart;
    engine.extractMonoWindow(0.10, 4410, -150.0f, winPreStart);
    REQUIRE(winPreStart.size() == 4410);
    for (int i = 0; i < 2000; ++i) {
      CHECK(winPreStart[static_cast<size_t>(i)] == doctest::Approx(0.0f));
    }
  }

  SUBCASE("Stacked Spectrum2: longer duration + higher smooth leads rising and finishes first falling") {
    constexpr int kSr = 44100;
    AudioTrackData track;
    track.sampleRate = kSr;
    track.channels = 2;
    track.interleavedPcm.assign(static_cast<size_t>(kSr) * 2u, 0.0f);
    // 80 Hz bass burst spanning [0.35s, 0.60s) with 15 ms attack and 50 ms release
    for (int i = 0; i < kSr; ++i) {
      const double t = static_cast<double>(i) / kSr;
      if (t >= 0.35 && t < 0.60) {
        double env = 1.0;
        if (t < 0.365) {
          env = (t - 0.35) / 0.015;
        } else if (t > 0.55) {
          env = (0.60 - t) / 0.050;
        }
        const float s = static_cast<float>(0.8 * env * std::sin(2.0 * kPi * 80.0 * t));
        track.interleavedPcm[static_cast<size_t>(i) * 2u] = s;
        track.interleavedPcm[static_cast<size_t>(i) * 2u + 1u] = s;
      }
    }
    rebuildMonoAndDuration(track);

    AudioEngine engine;
    REQUIRE(engine.loadTrack(std::move(track)));

    auto makeStackedCfg = [](int durationMs, float smooth) {
      SpectrumConfig cfg;
      cfg.kind = SampleProviderKind::Spectrum2;
      cfg.sampleOutCount = 250;
      cfg.audioDurationMs = durationMs;
      cfg.highQualityAudioCapture = true;
      cfg.lowerHz = 0.1f;
      cfg.higherHz = 460.0f;
      cfg.hzLinearFactor = 0.45f;
      cfg.freqShift = 0.9f;
      cfg.filterRadius = 1.0f;
      cfg.filterStrength = 0.0f;
      cfg.lowerFreqMagnitude = 0.0f;
      cfg.outputMultiplier = 4.5f;
      cfg.useAttackRelease = false;
      cfg.smooth = smooth;
      return cfg;
    };

    AudioAnalyzer prov1;  // Bar 1 (front / white)
    AudioAnalyzer prov5;  // Bar 5 (middle / magenta)
    AudioAnalyzer prov9;  // Bar 9 (back / green)
    prov1.setConfig(makeStackedCfg(195, 5.0f));
    prov5.setConfig(makeStackedCfg(151, 4.0f));
    prov9.setConfig(makeStackedCfg(107, 3.0f));

    auto peakBar = [](const AudioAnalyzer& az) {
      const auto& bars = az.lastResult().smoothedBars;
      return bars.empty() ? 0.0f : *std::max_element(bars.begin(), bars.end());
    };

    constexpr float kDt = 1.0f / 60.0f;
    std::vector<float> pcm;
    auto stepAll = [&](int frame) {
      const double t = static_cast<double>(frame) * kDt;
      for (AudioAnalyzer* az : {&prov1, &prov5, &prov9}) {
        const int req = (az->config().audioDurationMs * kSr) / 1000;
        engine.extractMonoWindow(t, req, 0.0f, pcm);
        az->process({pcm, kSr, kDt, t, t, 1.0, true, false});
      }
    };

    // Frame 15 -> t = 0.25s: rising into [0.35, 0.60) burst
    for (int f = 0; f <= 15; ++f) {
      stepAll(f);
    }
    const float rise1 = peakBar(prov1);
    const float rise5 = peakBar(prov5);
    const float rise9 = peakBar(prov9);
    CHECK(rise1 > rise5);
    CHECK(rise5 > rise9);

    // Frame 30 -> t = 0.50s: falling past peak toward silence
    for (int f = 16; f <= 30; ++f) {
      stepAll(f);
    }
    const float fall1 = peakBar(prov1);
    const float fall5 = peakBar(prov5);
    const float fall9 = peakBar(prov9);
    CHECK(fall9 > fall5);
    CHECK(fall5 > fall1);
  }

  SUBCASE("Legacy high smooth > 3.0 remains strictly monotonic and frame-rate independent") {
    const auto tone = makeSine(220.0f, 44100, 4096, 0.6f);
    constexpr float kDt60 = 1.0f / 60.0f;

    auto runStep = [&](float smooth, float dt, int steps) {
      SpectrumConfig cfg;
      cfg.kind = SampleProviderKind::Spectrum2;
      cfg.sampleOutCount = 64;
      cfg.useAttackRelease = false;
      cfg.smooth = smooth;

      AudioAnalyzer az;
      az.setConfig(cfg);
      AudioAnalysisResult res;
      for (int f = 0; f < steps; ++f) {
        res = az.process({tone, 44100, dt, f * dt, f * dt, 5.0, true, false});
      }
      return res;
    };

    const auto r3 = runStep(3.0f, kDt60, 1);
    const auto r4 = runStep(4.0f, kDt60, 1);
    const auto r5 = runStep(5.0f, kDt60, 1);
    const float peak3 = *std::max_element(r3.smoothedBars.begin(), r3.smoothedBars.end());
    const float peak4 = *std::max_element(r4.smoothedBars.begin(), r4.smoothedBars.end());
    const float peak5 = *std::max_element(r5.smoothedBars.begin(), r5.smoothedBars.end());
    const float raw5 = *std::max_element(r5.rawBars.begin(), r5.rawBars.end());
    CHECK(peak5 > peak4);
    CHECK(peak4 > peak3);
    CHECK(peak5 <= raw5 + 1e-4f);  // Never overshoots raw steady target

    // 5 frames at 60 FPS == 12 frames at 144 FPS (exact 1/12 s)
    const auto r60 = runStep(4.0f, 1.0f / 60.0f, 5);
    const auto r144 = runStep(4.0f, 1.0f / 144.0f, 12);
    const float p60 = *std::max_element(r60.smoothedBars.begin(), r60.smoothedBars.end());
    const float p144 = *std::max_element(r144.smoothedBars.begin(), r144.smoothedBars.end());
    CHECK(p144 == doctest::Approx(p60).epsilon(0.06));
  }
}

TEST_CASE("Goertzel Spectrum2 filterbank localizes frequencies and invalidates cached config") {
  using namespace iota::dsp;

  SpectrumConfig cfg;
  cfg.kind = SampleProviderKind::Spectrum2;
  cfg.sampleOutCount = 128;
  cfg.useAttackRelease = false;
  cfg.smooth = 0.0f;

  AudioAnalyzer az;
  az.setConfig(cfg);

  const auto& freqs = az.cachedBarFrequencies();
  REQUIRE(freqs.size() == 128);
  for (size_t i = 0; i < freqs.size(); ++i) {
    CHECK(std::isfinite(freqs[i]));
    CHECK(freqs[i] > 0.0f);
    if (i > 0) {
      CHECK(freqs[i] > freqs[i - 1]);
    }
  }

  for (size_t targetIdx : {size_t{16}, size_t{64}, size_t{108}}) {
    const float targetHz = freqs[targetIdx];
    const auto tone = makeSine(targetHz, 44100, 4096, 0.8f);
    // Test deterministic repeat with matching config across an independent analyzer and redundant setConfig()
    AudioAnalyzer azRef;
    azRef.setConfig(cfg);
    azRef.setConfig(cfg);
    const auto rRef = azRef.process({tone, 44100, 1.0f / 60.0f, 0.1, 0.1, 5.0, true, false});

    AudioAnalyzer azCmp;
    azCmp.setConfig(cfg);
    const auto rCmp = azCmp.process({tone, 44100, 1.0f / 60.0f, 0.1, 0.1, 5.0, true, false});

    REQUIRE(rRef.rawBars.size() == 128);
    REQUIRE(rCmp.rawBars.size() == 128);

    size_t bestIdx = 0;
    float bestVal = -1.0f;
    for (size_t b = 0; b < rRef.rawBars.size(); ++b) {
      CHECK(std::isfinite(rRef.rawBars[b]));
      CHECK(rRef.rawBars[b] == doctest::Approx(rCmp.rawBars[b]).epsilon(1e-5));
      if (rRef.rawBars[b] > bestVal) {
        bestVal = rRef.rawBars[b];
        bestIdx = b;
      }
    }
    const int diff = std::abs(static_cast<int>(bestIdx) - static_cast<int>(targetIdx));
    CHECK(diff <= 1);
    CHECK(bestVal > 0.01f);
  }

  cfg.sampleOutCount = 256;
  az.setConfig(cfg);
  CHECK(az.cachedBarFrequencies().size() == 256);
}
