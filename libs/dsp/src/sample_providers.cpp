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
    // Higher temporalSmooth and higher smooth both produce faster, less-damped response.
    const float responsiveness = std::clamp(cfg.temporalSmooth, 0.0f, 1.0f);
    const float temporalScale = (1.0f - responsiveness) * 2.0f;
    const float smoothScale = 1.0f / std::clamp(cfg.smooth, 0.05f, 20.0f);
    const float totalScale = temporalScale * smoothScale;
    const float atkAlpha = expSmoothingAlphaMs(dt, cfg.attackMs * totalScale);
    const float relAlpha = expSmoothingAlphaMs(dt, cfg.releaseMs * totalScale);
    for (int i = 0; i < barCount; ++i) {
      const float target = (targetBars != nullptr) ? std::max(0.0f, targetBars[i]) : 0.0f;
      float& sm = smoothedBars[static_cast<size_t>(i)];
      const float alpha = (target >= sm) ? atkAlpha : relAlpha;
      sm = std::max(0.0f, sm + (target - sm) * alpha);
    }
    return;
  }

  // Continuous exponential response matching `dy/dt = 20 * smooth * (target - y)` without clamping >= 3.0
  const float safeSmooth = std::max(0.0f, cfg.smooth);
  const float alpha = (dt > 0.0f && safeSmooth > 0.0f)
                          ? std::clamp(1.0f - std::exp(-20.0f * safeSmooth * dt), 0.0f, 1.0f)
                          : 0.0f;
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

}  // namespace

struct AudioAnalyzer::Impl {
  bool configInitialized = false;
  int fftSize = 0;
  std::unique_ptr<kiss_fftr_state, KissFftDeleter> fftCfg;
  std::vector<float> windowedPcm;
  std::vector<kiss_fft_cpx> fftOut;
  std::vector<float> fftMagnitudes;
  std::vector<float> preSmoothedFftMags;

  // Cached Spectrum2 Goertzel filterbank parameters + cached bar frequencies & Hann window
  std::vector<float> cachedBarFreqs;
  std::vector<float> cachedHann;
  int hannSize = 0;
  int lutSampleRate = 0;
  bool goertzelDirty = true;
  std::vector<double> goertzelTwoCos;
  std::vector<double> goertzelCos;
  std::vector<double> goertzelSin;
  std::vector<float> goertzelGainMul;
  std::vector<uint8_t> goertzelActive;

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

  void ensureHannWindow(int n) {
    if (n == hannSize && static_cast<int>(cachedHann.size()) == n) {
      return;
    }
    hannSize = n;
    cachedHann.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      cachedHann[static_cast<size_t>(i)] = hannWindow(i, n);
    }
  }

  void ensureGoertzelTables(const SpectrumConfig& cfg, int effectiveSampleRate) {
    if (!goertzelDirty && lutSampleRate == effectiveSampleRate && !goertzelTwoCos.empty()) {
      return;
    }
    if (cachedBarFreqs.empty()) {
      cachedBarFreqs = computeBarFrequencies(cfg);
    }
    lutSampleRate = effectiveSampleRate;
    goertzelDirty = false;

    const size_t nBars = cachedBarFreqs.size();
    goertzelTwoCos.resize(nBars);
    goertzelCos.resize(nBars);
    goertzelSin.resize(nBars);
    goertzelGainMul.resize(nBars);
    goertzelActive.resize(nBars);

    const double fs = static_cast<double>(std::max(effectiveSampleRate, 8000));
    const double twoPiOverFs = (2.0 * static_cast<double>(kPi)) / fs;

    for (size_t i = 0; i < nBars; ++i) {
      const float rawFreq = cachedBarFreqs[i];
      if (rawFreq < 1.0f) {
        goertzelTwoCos[i] = 2.0;
        goertzelCos[i] = 1.0;
        goertzelSin[i] = 0.0;
        goertzelGainMul[i] = 0.0f;
        goertzelActive[i] = 0;
        continue;
      }
      const float clampedFreq = std::clamp(rawFreq, 1.0f, static_cast<float>(fs * 0.49));
      const double omega = twoPiOverFs * static_cast<double>(clampedFreq);
      const double cosW = std::cos(omega);
      const double sinW = std::sin(omega);
      goertzelTwoCos[i] = 2.0 * cosW;
      goertzelCos[i] = cosW;
      goertzelSin[i] = sinW;
      goertzelGainMul[i] = spectrum2LowerFreqMultiplier(clampedFreq, cfg.lowerFreqMagnitude) * cfg.outputMultiplier;
      goertzelActive[i] = 1;
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
  const float defaultSmooth = (cfg.kind == SampleProviderKind::Spectrum2) ? 1.0f : 0.8f;
  cfg.smooth = std::clamp(p.getFloat("smooth", defaultSmooth), 0.01f, 20.0f);
  cfg.preSmooth = std::clamp(p.getFloat("preSmooth", 1.0f), 0.01f, 1.0f);

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
  if (impl_->configInitialized && cfg == cfg_) {
    return;
  }
  impl_->configInitialized = true;
  cfg_ = cfg;
  impl_->cachedBarFreqs = computeBarFrequencies(cfg_);
  impl_->goertzelDirty = true;
  if (cfg_.kind == SampleProviderKind::Spectrum) {
    impl_->sharpenKernel.configure(static_cast<int>(std::lround(cfg_.filterRadius)), cfg_.filterStrength);
  } else if (cfg_.kind == SampleProviderKind::Spectrum2) {
    impl_->gaussKernel.configure(cfg_.filterRadius, std::clamp(cfg_.filterStrength, 0.0f, 1.0f));
  }
  impl_->freqSmoother.configure(cfg_.freqSmoothMode, cfg_.freqSmoothRadius, cfg_.freqSmoothStrength);
}

const std::vector<float>& AudioAnalyzer::cachedBarFrequencies() const noexcept {
  if (impl_->cachedBarFreqs.empty()) {
    impl_->cachedBarFreqs = computeBarFrequencies(cfg_);
  }
  return impl_->cachedBarFreqs;
}

void AudioAnalyzer::reset() {
  impl_->agc.reset();
  impl_->beatState.reset();
  impl_->history.clear();
  impl_->preSmoothedFftMags.clear();
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
    if (!impl_->preSmoothedFftMags.empty()) {
      if (cfg_.preSmooth < 0.999f) {
        const float preAlpha = frameRateIndependentAlpha(std::clamp(cfg_.preSmooth, 0.01f, 1.0f), dt);
        for (float& prev : impl_->preSmoothedFftMags) {
          prev = lerpf(prev, 0.0f, preAlpha);
        }
      } else {
        std::fill(impl_->preSmoothedFftMags.begin(), impl_->preSmoothedFftMags.end(), 0.0f);
      }
    }
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
    const int offset = 0;
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
    impl_->ensureHannWindow(copyCount);
    const float* hann = impl_->cachedHann.data();
    for (int i = 0; i < copyCount; ++i) {
      impl_->windowedPcm[static_cast<size_t>(i)] =
          input.monoPcm[static_cast<size_t>(i)] * preScale * hann[i];
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

    if (impl_->preSmoothedFftMags.size() != static_cast<size_t>(binCount)) {
      impl_->preSmoothedFftMags.assign(static_cast<size_t>(binCount), 0.0f);
    }
    if (cfg_.preSmooth < 0.999f) {
      const float preAlpha = frameRateIndependentAlpha(std::clamp(cfg_.preSmooth, 0.01f, 1.0f), dt);
      for (int k = 0; k < binCount; ++k) {
        float& prev = impl_->preSmoothedFftMags[static_cast<size_t>(k)];
        prev = lerpf(prev, impl_->fftMagnitudes[static_cast<size_t>(k)], preAlpha);
        impl_->fftMagnitudes[static_cast<size_t>(k)] = prev;
      }
    } else {
      impl_->preSmoothedFftMags = impl_->fftMagnitudes;
    }

    const std::vector<float>& barFreqs = cachedBarFrequencies();
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

    impl_->sharpenKernel.apply(impl_->workBars, result_.rawBars);
  } else {
    // Spectrum2: Branchless 4-wide Goertzel filterbank across per-bar frequency centers
    impl_->ensureGoertzelTables(cfg_, effectiveRate);

    const int targetSamples = std::clamp((cfg_.audioDurationMs * effectiveRate) / 1000, 64, 32768);
    const int availDecimated = availSamples / decimation;
    const int nSamples = std::min(targetSamples, std::max(1, availDecimated));

    impl_->ensureHannWindow(nSamples);
    impl_->windowedPcm.resize(static_cast<size_t>(nSamples));
    const float* hann = impl_->cachedHann.data();
    const float* pcmPtr = input.monoPcm.data();
    if (decimation == 2) {
      for (int n = 0; n < nSamples; ++n) {
        const size_t idx0 = static_cast<size_t>(n * 2);
        const float sample = (idx0 + 1 < static_cast<size_t>(availSamples))
                                 ? (0.5f * (pcmPtr[idx0] + pcmPtr[idx0 + 1]))
                                 : pcmPtr[idx0];
        impl_->windowedPcm[static_cast<size_t>(n)] = sample * preScale * hann[n];
      }
    } else {
      for (int n = 0; n < nSamples; ++n) {
        impl_->windowedPcm[static_cast<size_t>(n)] = pcmPtr[n] * preScale * hann[n];
      }
    }

    impl_->workBars.assign(static_cast<size_t>(barCount), 0.0f);
    const double invNScale = 800.0 / static_cast<double>(std::max(1, nSamples));
    const float* xPtr = impl_->windowedPcm.data();
    const double* twoCosPtr = impl_->goertzelTwoCos.data();
    const double* cosPtr = impl_->goertzelCos.data();
    const double* sinPtr = impl_->goertzelSin.data();
    const float* gainPtr = impl_->goertzelGainMul.data();
    const uint8_t* activePtr = impl_->goertzelActive.data();
    float* outBars = impl_->workBars.data();

    int b = 0;
    for (; b + 4 <= barCount; b += 4) {
      const double tc0 = twoCosPtr[b + 0];
      const double tc1 = twoCosPtr[b + 1];
      const double tc2 = twoCosPtr[b + 2];
      const double tc3 = twoCosPtr[b + 3];
      double s10 = 0.0, s20 = 0.0;
      double s11 = 0.0, s21 = 0.0;
      double s12 = 0.0, s22 = 0.0;
      double s13 = 0.0, s23 = 0.0;

      for (int n = 0; n < nSamples; ++n) {
        const double x = static_cast<double>(xPtr[n]);
        const double t0 = x + tc0 * s10 - s20;
        const double t1 = x + tc1 * s11 - s21;
        const double t2 = x + tc2 * s12 - s22;
        const double t3 = x + tc3 * s13 - s23;
        s20 = s10; s10 = t0;
        s21 = s11; s11 = t1;
        s22 = s12; s12 = t2;
        s23 = s13; s13 = t3;
      }

      const double s1Arr[4] = {s10, s11, s12, s13};
      const double s2Arr[4] = {s20, s21, s22, s23};
      for (int j = 0; j < 4; ++j) {
        const int idx = b + j;
        if (!activePtr[idx]) {
          outBars[idx] = 0.0f;
          continue;
        }
        const double re = cosPtr[idx] * s1Arr[j] - s2Arr[j];
        const double im = sinPtr[idx] * s1Arr[j];
        const float mag = static_cast<float>(std::sqrt(re * re + im * im) * invNScale);
        outBars[idx] = mag * gainPtr[idx];
      }
    }

    for (; b < barCount; ++b) {
      if (!activePtr[b]) {
        outBars[b] = 0.0f;
        continue;
      }
      const double tc = twoCosPtr[b];
      double s1 = 0.0, s2 = 0.0;
      for (int n = 0; n < nSamples; ++n) {
        const double t = static_cast<double>(xPtr[n]) + tc * s1 - s2;
        s2 = s1;
        s1 = t;
      }
      const double re = cosPtr[b] * s1 - s2;
      const double im = sinPtr[b] * s1;
      const float mag = static_cast<float>(std::sqrt(re * re + im * im) * invNScale);
      outBars[b] = mag * gainPtr[b];
    }

    impl_->gaussKernel.apply(impl_->workBars, result_.rawBars);
  }

  // Optional separate spatial frequency smoothing stage before temporal attack/release
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
