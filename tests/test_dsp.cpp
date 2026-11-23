#include "doctest/doctest.h"

#include "iota/dsp/a_weighting.hpp"
#include "iota/dsp/filters.hpp"
#include "iota/dsp/mvar_evaluator.hpp"
#include "iota/dsp/sample_providers.hpp"
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
