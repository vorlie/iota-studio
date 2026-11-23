#include "iota/dsp/sample_providers.hpp"

#include "iota/dsp/a_weighting.hpp"
#include "iota/dsp/windows.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "kiss_fftr.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace iota::dsp {

namespace {

int nextPowerOfTwo(int v) noexcept {
  int p = 1;
  while (p < v) {
    p <<= 1;
  }
  return p;
}

float lerpf(float a, float b, float t) noexcept {
  return a + (b - a) * t;
}

void applyTemporalSmoothing(const float* targetBars,
                            int barCount,
                            std::vector<float>& smoothedBars,
                            const SpectrumConfig& cfg,
                            float dt) noexcept {
  if (cfg.useAttackRelease) {
    const float scale = std::max(0.0f, cfg.temporalSmooth) * 2.0f;
    const float atkAlpha = expSmoothingAlphaMs(dt, cfg.attackMs * scale);
    const float relAlpha = expSmoothingAlphaMs(dt, cfg.releaseMs * scale);
    for (int i = 0; i < barCount; ++i) {
      const float target = (targetBars != nullptr) ? std::max(0.0f, targetBars[i]) : 0.0f;
      float& sm = smoothedBars[static_cast<size_t>(i)];
      const float alpha = (target >= sm) ? atkAlpha : relAlpha;
      sm = std::max(0.0f, sm + (target - sm) * alpha);
    }
    return;
  }

  // Legacy `smooth` factor is tuned for 60 Hz steps (`20 * (1/60) * smooth`)
  const float alpha60 = std::clamp((20.0f / 60.0f) * cfg.smooth, 0.01f, 1.0f);
  const float alpha = frameRateIndependentAlpha(alpha60, dt);
  for (int i = 0; i < barCount; ++i) {
    const float target = (targetBars != nullptr) ? std::max(0.0f, targetBars[i]) : 0.0f;
    float& sm = smoothedBars[static_cast<size_t>(i)];
    sm = std::max(0.0f, sm + (target - sm) * alpha);
  }
}

struct KissFftDeleter {
  void operator()(kiss_fftr_state* ptr) const noexcept {
    if (ptr != nullptr) {
      kiss_fftr_free(ptr);
    }
  }
};

struct BarLut {
  float freqHz = 100.0f;
  int periodSamples = 2;
  float calibScale = 1.0f;
  std::vector<float> cosTable;
  std::vector<float> sinTable;
};

}  // namespace

struct AudioAnalyzer::Impl {
  int fftSize = 0;
  std::unique_ptr<kiss_fftr_state, KissFftDeleter> fftCfg;
  std::vector<float> windowedPcm;
  std::vector<kiss_fft_cpx> fftOut;
  std::vector<float> fftMagnitudes;

  // Cached Spectrum2 DFT LUTs
  int lutSampleRate = 0;
  int lutBarCount = 0;
  float lutLowerHz = 0.0f;
  float lutHigherHz = 0.0f;
  float lutHzLinear = -1.0f;
  float lutFreqShift = 0.0f;
  int lutMirror = -1;
  int lutRepeat = -1;
  float lutGap = -1.0f;
  std::vector<BarLut> dftLuts;

  AgcEnvelope agc;
  BeatDetectorState beatState;
  BarHistoryRing history{64};
  SharpenKernel sharpenKernel;
  GaussianImpulseKernel gaussKernel;
  FrequencySmoother freqSmoother;
  std::vector<float> workBars;
  std::vector<float> filteredBars;

  void ensureFft(int n) {
    if (n != fftSize || !fftCfg) {
      fftSize = n;
      fftCfg.reset(kiss_fftr_alloc(fftSize, 0, nullptr, nullptr));
      windowedPcm.resize(static_cast<size_t>(fftSize));
      fftOut.resize(static_cast<size_t>(fftSize / 2 + 1));
      fftMagnitudes.resize(static_cast<size_t>(fftSize / 2 + 1));
    }
  }

  void ensureDftLuts(const SpectrumConfig& cfg, int effectiveSampleRate, const std::vector<float>& freqs) {
    const bool unchanged =
        lutSampleRate == effectiveSampleRate &&
        lutBarCount == static_cast<int>(freqs.size()) &&
        lutLowerHz == cfg.lowerHz &&
        lutHigherHz == cfg.higherHz &&
        lutHzLinear == cfg.hzLinearFactor &&
        lutFreqShift == cfg.freqShift &&
        lutMirror == cfg.mirrorSamples &&
        lutRepeat == cfg.repeatSamples &&
        lutGap == cfg.starAndEndGap;
    if (unchanged && !dftLuts.empty()) {
      return;
    }

    lutSampleRate = effectiveSampleRate;
    lutBarCount = static_cast<int>(freqs.size());
    lutLowerHz = cfg.lowerHz;
    lutHigherHz = cfg.higherHz;
    lutHzLinear = cfg.hzLinearFactor;
    lutFreqShift = cfg.freqShift;
    lutMirror = cfg.mirrorSamples;
    lutRepeat = cfg.repeatSamples;
    lutGap = cfg.starAndEndGap;

    dftLuts.resize(freqs.size());
    const float fs = static_cast<float>(std::max(effectiveSampleRate, 8000));

    for (size_t i = 0; i < freqs.size(); ++i) {
      auto& lut = dftLuts[i];
      if (freqs[i] < 1.0f) {
        lut.freqHz = freqs[i];
        lut.periodSamples = 0;
        lut.cosTable.clear();
        lut.sinTable.clear();
        lut.calibScale = 1.0f;
        continue;
      }
      lut.freqHz = std::clamp(freqs[i], 1.0f, fs * 0.49f);
      const float samplesPerCycle = fs / lut.freqHz;
      const int cycles = std::max(1, static_cast<int>(std::lround((fs / 400.0f) / std::max(samplesPerCycle, 1.0f))));
      int period = static_cast<int>(std::lround(static_cast<float>(cycles) * samplesPerCycle));
      period = std::clamp(period, 2, std::max(2, effectiveSampleRate));
      lut.periodSamples = period;
      lut.cosTable.resize(static_cast<size_t>(period));
      lut.sinTable.resize(static_cast<size_t>(period));

      const double phaseStep = (2.0 * static_cast<double>(kPi) * static_cast<double>(lut.freqHz)) /
                               static_cast<double>(fs);
      for (int n = 0; n < period; ++n) {
        const double ang = phaseStep * static_cast<double>(n);
        lut.cosTable[static_cast<size_t>(n)] = static_cast<float>(std::cos(ang));
        lut.sinTable[static_cast<size_t>(n)] = static_cast<float>(std::sin(ang));
      }
      lut.calibScale = 1.0f;
    }
  }
};

SpectrumConfig spectrumConfigFromElement(const iota::model::Element& audioProvider) {
  SpectrumConfig cfg;
  const auto& p = audioProvider.props;

  const auto spStr = p.getString("sampleProvider", "Spectrum2");
  if (spStr == "Waveform" || spStr == "0") {
    cfg.kind = SampleProviderKind::Waveform;
  } else if (spStr == "Spectrum" || spStr == "1") {
    cfg.kind = SampleProviderKind::Spectrum;
  } else {
    cfg.kind = SampleProviderKind::Spectrum2;
  }

  cfg.sampleOutCount = std::clamp(static_cast<int>(p.getInt("sampleOutCount", 128)), 4, 2048);
  const int wavePow = std::clamp(static_cast<int>(p.getInt("sampleOutCountPower", 8)), 3, 9);
  if (cfg.kind == SampleProviderKind::Waveform) {
    cfg.sampleOutCount = std::clamp(1 << wavePow, 8, 512);
  }

  int inputPow = static_cast<int>(p.getInt("inputSize", -1));
  if (inputPow < 0) {
    inputPow = static_cast<int>(p.getInt("fftSize", 11));
  }
  cfg.inputSizePow = std::clamp(inputPow, 5, 14);
  cfg.audioDurationMs = std::clamp(static_cast<int>(p.getInt("audioDurationMs", 60)), 10, 500);
  cfg.highQualityAudioCapture = p.getBool("highQualityAudioCapture", true);

  cfg.lowerHz = std::clamp(p.getFloat("lowerHz", 20.0f), 0.05f, 10000.0f);
  cfg.higherHz = std::clamp(p.getFloat("higherHz", 18000.0f), cfg.lowerHz + 0.1f, 22050.0f);
  cfg.hzLinearFactor = std::clamp(p.getFloat("hzLinearFactor", 0.1f), 0.0f, 1.0f);
  cfg.freqShift = p.getFloat("freqShift", 0.0f);

  cfg.mirrorSamples = p.getBool("mirrorSamples", false) ? 1 : static_cast<int>(p.getInt("mirrorSamples", 0));
  cfg.repeatSamples = std::clamp(static_cast<int>(p.getInt("repeatSamples", 1)), 1, 16);
  cfg.starAndEndGap = p.getFloat("starAndEndGap", 0.0f);
  cfg.outputMultiplier = std::clamp(p.getFloat("outputMultiplier", 1.0f), 0.01f, 50.0f);

  cfg.filterRadius = std::clamp(p.getFloat("filterRadius", 2.0f), 0.0f, 32.0f);
  cfg.filterStrength = std::clamp(p.getFloat("filterStrength", 0.6f), 0.0f, 5.0f);
  cfg.aWeight = std::clamp(p.getFloat("aWeight", 0.0f), 0.0f, 1.0f);
  cfg.lowerFreqMagnitude = std::clamp(p.getFloat("lowerFreqMagnitude", 0.25f), 0.0f, 1.0f);
  cfg.smooth = std::clamp(p.getFloat("smooth", 0.8f), 0.01f, 20.0f);

  const bool hasTemporalProps =
      p.contains("temporalSmooth") || p.contains("attackMs") || p.contains("releaseMs");
  cfg.useAttackRelease = p.contains("useAttackRelease") ? p.getBool("useAttackRelease", hasTemporalProps)
                                                      : hasTemporalProps;
  cfg.temporalSmooth = std::clamp(p.getFloat("temporalSmooth", 0.5f), 0.0f, 1.0f);
  cfg.attackMs = std::clamp(p.getFloat("attackMs", 25.0f), 0.0f, 500.0f);
  cfg.releaseMs = std::clamp(p.getFloat("releaseMs", 150.0f), 0.0f, 2000.0f);

  cfg.freqSmoothMode = frequencySmoothModeFromString(p.getString("freqSmoothMode", "None"));
  cfg.freqSmoothRadius = std::clamp(p.getFloat("freqSmoothRadius", 2.0f), 0.0f, 16.0f);
  cfg.freqSmoothStrength = std::clamp(p.getFloat("freqSmoothStrength", 0.7f), 0.0f, 1.0f);

  cfg.beatRangeBarFirst = std::clamp(p.getFloat("beatRangeBarFirst", 0.0f), 0.0f, 1.0f);
  cfg.beatRangeBarLast = std::clamp(p.getFloat("beatRangeBarLast", 0.2f), 0.0f, 1.0f);
  cfg.beatRangeValueLower = std::max(0.0f, p.getFloat("beatRangeValueLower", 0.7f));
  cfg.beatRangeValueHigher = std::max(cfg.beatRangeValueLower + 0.01f,
                                      p.getFloat("beatRangeValueHigher", 35.0f));
  cfg.beatSmooth = std::clamp(p.getFloat("beatSmooth", 0.4f), 0.01f, 5.0f);
  return cfg;
}

std::vector<float> computeBarFrequencies(const SpectrumConfig& cfg) {
  const int barCount = std::clamp(cfg.sampleOutCount, 1, 2048);
  std::vector<float> freqs(static_cast<size_t>(barCount), 0.0f);

  // Treat starAndEndGap >= 1 as bar gap count; fractional in (0, 1) as ratio
  int gapBars = 0;
  if (cfg.starAndEndGap >= 1.0f) {
    gapBars = std::clamp(static_cast<int>(std::lround(cfg.starAndEndGap)), 0, barCount / 3);
  } else if (cfg.starAndEndGap > 0.0f) {
    gapBars = std::clamp(static_cast<int>(std::lround(cfg.starAndEndGap * static_cast<float>(barCount) * 0.5f)),
                         0, barCount / 3);
  }

  const int activeStart = gapBars;
  const int activeEnd = std::max(activeStart + 1, barCount - gapBars);
  const int activeCount = activeEnd - activeStart;
  const int repeat = std::max(1, cfg.repeatSamples);
  const int tileLen = std::max(1, activeCount / repeat);
  const float fLoBound = std::max(cfg.lowerHz, 0.1f);
  const float fHiBound = std::max(cfg.higherHz, fLoBound + 0.01f);
  const float ratio = fHiBound / fLoBound;
  const float linFactor = std::clamp(cfg.hzLinearFactor, 0.0f, 1.0f);
  const int shift = static_cast<int>(std::floor(static_cast<float>(activeCount) * cfg.freqShift));

  for (int i = 0; i < barCount; ++i) {
    const int rel = i - activeStart;
    if (rel < 0 || rel >= activeCount) {
      freqs[static_cast<size_t>(i)] = 0.0f;
      continue;
    }
    const int shifted = ((rel - shift) % tileLen + tileLen) % tileLen;
    float u = (tileLen > 1) ? (static_cast<float>(shifted) / static_cast<float>(tileLen - 1)) : 0.0f;

    if (cfg.mirrorSamples != 0 && tileLen > 2) {
      // Fold mirrored pattern: 0 -> 1 -> 0 across tile
      u = (u <= 0.5f) ? (u * 2.0f) : ((1.0f - u) * 2.0f);
    }

    const float fLog = fLoBound * std::pow(ratio, u);
    const float fLin = cfg.lowerHz + (cfg.higherHz - cfg.lowerHz) * u;
    freqs[static_cast<size_t>(i)] = lerpf(fLog, fLin, linFactor);
  }
  return freqs;
}

float AgcEnvelope::update(float maxAbsInt16Sample, float dtSec) noexcept {
  constexpr float kFloorTarget = 0.0f;
  const float floorAlpha = frameRateIndependentAlpha(0.9f, dtSec);
  floor = lerpf(floor, kFloorTarget, floorAlpha);
  if (maxAbsInt16Sample > peak) {
    const float atkAlpha = frameRateIndependentAlpha(0.9f, dtSec);
    peak = lerpf(peak, maxAbsInt16Sample, atkAlpha);
  } else {
    const float relAlpha = frameRateIndependentAlpha(0.01f, dtSec);
    peak = lerpf(peak, maxAbsInt16Sample, relAlpha);
  }
  gain = 300.0f / std::max(peak - floor, 1.0f);
  return gain;
}

void BeatDetectorState::reset() noexcept {
  sumH = 0.0f;
  accumJ = 0.0f;
  smoothedBeat = 0.0f;
  peakBarIndexSmoothed = 0.0f;
  peakValue = 0.0f;
}

void BeatDetectorState::update(std::span<const float> bars,
                               const SpectrumConfig& cfg,
                               float dtSec) noexcept {
  const int n = static_cast<int>(bars.size());
  if (n <= 0) {
    smoothedBeat = 0.0f;
    peakBarIndexSmoothed = 0.0f;
    return;
  }

  int first = std::clamp(static_cast<int>(std::lround(cfg.beatRangeBarFirst * static_cast<float>(n))), 0, n - 1);
  int last = std::clamp(static_cast<int>(std::lround(cfg.beatRangeBarLast * static_cast<float>(n))), first, n - 1);

  const float valLo = cfg.beatRangeValueLower;
  const float valHi = std::max(valLo + 0.01f, cfg.beatRangeValueHigher);
  const float span = valHi - valLo;
  const int count = std::max(1, last - first + 1);

  float localSum = 0.0f;
  float localMax = cfg.strictCompatBeatQuirk ? accumJ : 0.0f;

  for (int i = first; i <= last; ++i) {
    const float clamped = std::clamp(bars[static_cast<size_t>(i)], valLo, valHi) - valLo;
    localSum += clamped;
    if (cfg.strictCompatBeatQuirk) {
      localMax = std::max(localSum, clamped);
    } else {
      localMax = std::max(localMax, clamped);
    }
  }
  sumH = localSum;
  accumJ = localMax;

  const float meanNorm = std::clamp(localSum / (span * static_cast<float>(count)), 0.0f, 1.0f);
  const float peakNorm = std::clamp(localMax / span, 0.0f, 1.0f);
  const float rawBeat = std::clamp(0.5f * meanNorm + 0.5f * peakNorm, 0.0f, 1.0f);
  const float beatAlpha = frameRateIndependentAlpha(std::clamp(cfg.beatSmooth, 0.01f, 1.0f), dtSec);
  smoothedBeat = lerpf(smoothedBeat, rawBeat, beatAlpha);

  // Peak bar index weighted across smoothed output bars
  int bestIdx = 0;
  float bestVal = -1.0f;
  for (int i = 0; i < n; ++i) {
    if (bars[static_cast<size_t>(i)] > bestVal) {
      bestVal = bars[static_cast<size_t>(i)];
      bestIdx = i;
    }
  }
  peakValue = std::max(0.0f, bestVal);
  const float rawPeakNorm = static_cast<float>(bestIdx) / static_cast<float>(std::max(1, n));
  const float peakIdxAlpha = frameRateIndependentAlpha(0.5f, dtSec);
  peakBarIndexSmoothed = lerpf(peakBarIndexSmoothed, rawPeakNorm, peakIdxAlpha);
}

AudioAnalyzer::AudioAnalyzer() : impl_(std::make_unique<Impl>()) {}
AudioAnalyzer::~AudioAnalyzer() = default;
AudioAnalyzer::AudioAnalyzer(AudioAnalyzer&&) noexcept = default;
AudioAnalyzer& AudioAnalyzer::operator=(AudioAnalyzer&&) noexcept = default;

void AudioAnalyzer::setConfig(const SpectrumConfig& cfg) {
  cfg_ = cfg;
  if (cfg_.kind == SampleProviderKind::Spectrum) {
    impl_->sharpenKernel.configure(static_cast<int>(std::lround(cfg_.filterRadius)), cfg_.filterStrength);
  } else if (cfg_.kind == SampleProviderKind::Spectrum2) {
    impl_->gaussKernel.configure(cfg_.filterRadius, std::clamp(cfg_.filterStrength, 0.0f, 1.0f));
  }
  impl_->freqSmoother.configure(cfg_.freqSmoothMode, cfg_.freqSmoothRadius, cfg_.freqSmoothStrength);
}

void AudioAnalyzer::reset() {
  impl_->agc.reset();
  impl_->beatState.reset();
  impl_->history.clear();
  result_.rawBars.clear();
  result_.smoothedBars.clear();
  result_.beat = 0.0f;
  result_.peakBarIndex = 0.0f;
  result_.rmsAmplitude = 0.0f;
  result_.peakAmplitude = 0.0f;
}

const AudioAnalysisResult& AudioAnalyzer::process(const AudioFrameInput& input) {
  const int barCount = std::clamp(cfg_.sampleOutCount, 4, 2048);
  const int sampleRate = std::max(input.sampleRate, 8000);
  const float dt = std::clamp(input.dt, 1.0f / 1000.0f, 0.1f);

  result_.rawBars.assign(static_cast<size_t>(barCount), 0.0f);
  if (result_.smoothedBars.size() != static_cast<size_t>(barCount)) {
    result_.smoothedBars.assign(static_cast<size_t>(barCount), 0.0f);
  }

  // Compute input amplitude statistics on int16-equivalent scale [-32767, +32767]
  double sumSq = 0.0;
  float maxAbsShort = 0.0f;
  for (float s : input.monoPcm) {
    const float absS = std::abs(s);
    sumSq += static_cast<double>(s) * static_cast<double>(s);
    maxAbsShort = std::max(maxAbsShort, absS * 32767.0f);
  }
  result_.peakAmplitude = maxAbsShort / 32767.0f;
  result_.rmsAmplitude = input.monoPcm.empty()
                             ? 0.0f
                             : static_cast<float>(std::sqrt(sumSq / static_cast<double>(input.monoPcm.size())));

  const float agcGain = impl_->agc.update(maxAbsShort, dt);
  const float preScale = (1.0f / 256.0f) * agcGain * 32767.0f;

  if (input.monoPcm.empty() || maxAbsShort < 1e-4f) {
    // Decay smoothly toward zero on silence using frame-rate-independent temporal release
    applyTemporalSmoothing(nullptr, barCount, result_.smoothedBars, cfg_, dt);
    impl_->beatState.update(result_.smoothedBars, cfg_, dt);
    impl_->history.pushFrame(result_.smoothedBars, dt);
    result_.beat = impl_->beatState.smoothedBeat;
    result_.peakBarIndex = impl_->beatState.peakBarIndexSmoothed;
    return result_;
  }

  const int decimation = (!cfg_.highQualityAudioCapture && cfg_.kind != SampleProviderKind::Spectrum) ? 2 : 1;
  const int effectiveRate = sampleRate / decimation;
  const int availSamples = static_cast<int>(input.monoPcm.size());

  if (cfg_.kind == SampleProviderKind::Waveform) {
    const int nSamples = std::clamp(nextPowerOfTwo(1 << cfg_.inputSizePow), 32, 16384);
    const int step = decimation;
    const int usedPcmCount = std::min(nSamples, availSamples / step);

    impl_->workBars.assign(static_cast<size_t>(barCount), 0.0f);
    const int offset = std::max(0, availSamples - usedPcmCount * step);
    const float ratio = static_cast<float>(std::max(1, usedPcmCount)) / static_cast<float>(barCount);

    for (int b = 0; b < barCount; ++b) {
      if (ratio <= 1.0f) {
        const float pos = static_cast<float>(b) * ratio;
        const int idx = std::clamp(static_cast<int>(pos), 0, std::max(0, usedPcmCount - 1));
        float sample = 0.0f;
        if (step == 2 && offset + idx * 2 + 1 < availSamples) {
          sample = 0.5f * (input.monoPcm[static_cast<size_t>(offset + idx * 2)] +
                           input.monoPcm[static_cast<size_t>(offset + idx * 2 + 1)]);
        } else if (offset + idx * step < availSamples) {
          sample = input.monoPcm[static_cast<size_t>(offset + idx * step)];
        }
        const float val = std::abs(sample) * kaiserWindow(idx, std::max(1, usedPcmCount), 0.8f) * preScale * 10.0f;
        impl_->workBars[static_cast<size_t>(b)] = val * cfg_.outputMultiplier;
      } else {
        const int i0 = std::clamp(static_cast<int>(static_cast<float>(b) * ratio), 0, usedPcmCount);
        const int i1 = std::clamp(static_cast<int>(static_cast<float>(b + 1) * ratio), i0 + 1, usedPcmCount);
        float blockSum = 0.0f;
        for (int sIdx = i0; sIdx < i1; ++sIdx) {
          float sample = 0.0f;
          if (step == 2 && offset + sIdx * 2 + 1 < availSamples) {
            sample = 0.5f * (input.monoPcm[static_cast<size_t>(offset + sIdx * 2)] +
                             input.monoPcm[static_cast<size_t>(offset + sIdx * 2 + 1)]);
          } else if (offset + sIdx * step < availSamples) {
            sample = input.monoPcm[static_cast<size_t>(offset + sIdx * step)];
          }
          blockSum += std::abs(sample) * kaiserWindow(sIdx, usedPcmCount, 0.8f) * preScale * 10.0f;
        }
        impl_->workBars[static_cast<size_t>(b)] =
            (blockSum / static_cast<float>(std::max(1, i1 - i0))) * cfg_.outputMultiplier;
      }
    }
    result_.rawBars = impl_->workBars;
  } else if (cfg_.kind == SampleProviderKind::Spectrum) {
    const int fftN = std::clamp(nextPowerOfTwo(1 << cfg_.inputSizePow), 32, 8192);
    impl_->ensureFft(fftN);
    std::fill(impl_->windowedPcm.begin(), impl_->windowedPcm.end(), 0.0f);

    const int copyCount = std::min(fftN, availSamples);
    const int srcStart = availSamples - copyCount;
    const int dstStart = fftN - copyCount;
    for (int i = 0; i < copyCount; ++i) {
      impl_->windowedPcm[static_cast<size_t>(dstStart + i)] =
          input.monoPcm[static_cast<size_t>(srcStart + i)] * preScale * hannWindow(i, copyCount);
    }

    kiss_fftr(impl_->fftCfg.get(), impl_->windowedPcm.data(), impl_->fftOut.data());

    // Normalize magnitudes consistently with Spectrum2 * 800 scale
    const int binCount = fftN / 2 + 1;
    const float invNorm = 1.0f / (static_cast<float>(fftN) * 0.00125f);
    for (int k = 0; k < binCount; ++k) {
      const float re = impl_->fftOut[static_cast<size_t>(k)].r;
      const float im = impl_->fftOut[static_cast<size_t>(k)].i;
      impl_->fftMagnitudes[static_cast<size_t>(k)] = std::hypot(re, im) * invNorm;
    }

    const std::vector<float> barFreqs = computeBarFrequencies(cfg_);
    const float df = static_cast<float>(sampleRate) / static_cast<float>(fftN);

    impl_->workBars.assign(static_cast<size_t>(barCount), 0.0f);
    for (int i = 0; i < barCount; ++i) {
      const float fCenter = barFreqs[static_cast<size_t>(i)];
      const float fPrev = (i > 0) ? 0.5f * (barFreqs[static_cast<size_t>(i - 1)] + fCenter) : fCenter;
      const float fNext = (i + 1 < barCount) ? 0.5f * (fCenter + barFreqs[static_cast<size_t>(i + 1)]) : fCenter;
      const float fLo = std::min(fPrev, fNext);
      const float fHi = std::max(fPrev, fNext);

      const float bLo = std::clamp(fLo / df, 0.0f, static_cast<float>(binCount - 1));
      const float bHi = std::clamp(fHi / df, 0.0f, static_cast<float>(binCount - 1));

      float mag = 0.0f;
      const int kLo = static_cast<int>(std::floor(bLo));
      const int kHi = static_cast<int>(std::ceil(bHi));
      if (kHi - kLo <= 1) {
        const float bCenter = std::clamp(fCenter / df, 0.0f, static_cast<float>(binCount - 1));
        const int idx0 = static_cast<int>(std::floor(bCenter));
        const int idx1 = std::min(idx0 + 1, binCount - 1);
        const float frac = bCenter - static_cast<float>(idx0);
        mag = lerpf(impl_->fftMagnitudes[static_cast<size_t>(idx0)],
                    impl_->fftMagnitudes[static_cast<size_t>(idx1)], frac);
      } else {
        // Peak + average energy across bin interval preserves sharp spectral spikes
        float sum = 0.0f;
        float peak = 0.0f;
        int cnt = 0;
        for (int k = kLo; k <= kHi && k < binCount; ++k) {
          const float v = impl_->fftMagnitudes[static_cast<size_t>(k)];
          sum += v;
          peak = std::max(peak, v);
          ++cnt;
        }
        const float avg = cnt > 0 ? (sum / static_cast<float>(cnt)) : 0.0f;
        mag = 0.5f * avg + 0.5f * peak;
      }

      const float widthEmphasis = ((std::abs(fHi - fLo) / 50.0f) * 0.3f + 0.7f);
      const float weighted = mag * std::clamp(widthEmphasis, 0.7f, 2.5f) *
                             spectrumAWeightMultiplier(fCenter, cfg_.aWeight) * cfg_.outputMultiplier;
      impl_->workBars[static_cast<size_t>(i)] = weighted;
    }

    impl_->sharpenKernel.configure(static_cast<int>(std::lround(cfg_.filterRadius)), cfg_.filterStrength);
    impl_->sharpenKernel.apply(impl_->workBars, result_.rawBars);
  } else {
    // Spectrum2: Direct-DFT filterbank tuned across per-bar frequency centers
    const std::vector<float> barFreqs = computeBarFrequencies(cfg_);
    impl_->ensureDftLuts(cfg_, effectiveRate, barFreqs);

    const int targetSamples = std::clamp((cfg_.audioDurationMs * effectiveRate) / 1000, 64, 16384);
    const int availDecimated = availSamples / decimation;
    const int nSamples = std::min(targetSamples, std::max(1, availDecimated));
    const int startSample = std::max(0, availSamples - nSamples * decimation);

    impl_->windowedPcm.resize(static_cast<size_t>(nSamples));
    for (int n = 0; n < nSamples; ++n) {
      float sample = 0.0f;
      if (decimation == 2 && startSample + n * 2 + 1 < availSamples) {
        sample = 0.5f * (input.monoPcm[static_cast<size_t>(startSample + n * 2)] +
                         input.monoPcm[static_cast<size_t>(startSample + n * 2 + 1)]);
      } else {
        sample = input.monoPcm[static_cast<size_t>(startSample + n * decimation)];
      }
      impl_->windowedPcm[static_cast<size_t>(n)] = sample * preScale * hannWindow(n, nSamples);
    }

    impl_->workBars.assign(static_cast<size_t>(barCount), 0.0f);
    const float invN = 1.0f / static_cast<float>(std::max(1, nSamples));
    const float* xPtr = impl_->windowedPcm.data();

    for (int b = 0; b < barCount; ++b) {
      const auto& lut = impl_->dftLuts[static_cast<size_t>(b)];
      const int period = lut.periodSamples;
      if (period <= 0) {
        impl_->workBars[static_cast<size_t>(b)] = 0.0f;
        continue;
      }
      const float* cPtr = lut.cosTable.data();
      const float* sPtr = lut.sinTable.data();

      double sumCos = 0.0;
      double sumSin = 0.0;
      int phase = 0;
      for (int n = 0; n < nSamples; ++n) {
        const float x = xPtr[n];
        sumCos += static_cast<double>(x) * static_cast<double>(cPtr[phase]);
        sumSin += static_cast<double>(x) * static_cast<double>(sPtr[phase]);
        if (++phase == period) {
          phase = 0;
        }
      }

      const float mag = static_cast<float>(std::hypot(sumCos, sumSin)) * invN * 800.0f;
      impl_->workBars[static_cast<size_t>(b)] =
          mag * spectrum2LowerFreqMultiplier(lut.freqHz, cfg_.lowerFreqMagnitude) * cfg_.outputMultiplier;
    }

    impl_->gaussKernel.configure(cfg_.filterRadius, std::clamp(cfg_.filterStrength, 0.0f, 1.0f));
    impl_->gaussKernel.apply(impl_->workBars, result_.rawBars);
  }

  // Optional separate spatial frequency smoothing stage before temporal attack/release
  impl_->freqSmoother.configure(cfg_.freqSmoothMode, cfg_.freqSmoothRadius, cfg_.freqSmoothStrength);
  const float* temporalTarget = result_.rawBars.data();
  if (cfg_.freqSmoothMode != FrequencySmoothMode::None) {
    impl_->freqSmoother.apply(result_.rawBars, impl_->filteredBars);
    temporalTarget = impl_->filteredBars.data();
  }

  applyTemporalSmoothing(temporalTarget, barCount, result_.smoothedBars, cfg_, dt);

  impl_->beatState.update(result_.smoothedBars, cfg_, dt);
  impl_->history.pushFrame(result_.smoothedBars, dt);

  result_.beat = impl_->beatState.smoothedBeat;
  result_.peakBarIndex = impl_->beatState.peakBarIndexSmoothed;
  return result_;
}

void AudioAnalyzer::sampleBarsForLayer(int reactionDelay,
                                       int reactionAccumulatedDelay,
                                       int softness,
                                       std::vector<float>& outBars) const {
  impl_->history.sampleBars(reactionDelay, reactionAccumulatedDelay, softness, outBars);
}

}  // namespace iota::dsp
