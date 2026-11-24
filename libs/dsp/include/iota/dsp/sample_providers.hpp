#pragma once

#include "iota/dsp/filters.hpp"
#include "iota/model/scene.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace iota::dsp {

enum class SampleProviderKind {
  Waveform = 0,
  Spectrum = 1,
  Spectrum2 = 2,
};

struct SpectrumConfig {
  SampleProviderKind kind = SampleProviderKind::Spectrum2;
  int sampleOutCount = 128;
  int inputSizePow = 11;       // 2^inputSizePow samples for FFT / Waveform
  int audioDurationMs = 60;    // Window duration for Spectrum2 (20..300 ms)
  bool highQualityAudioCapture = true;

  float lowerHz = 20.0f;
  float higherHz = 18000.0f;
  float hzLinearFactor = 0.1f;
  float freqShift = 0.0f;
  int mirrorSamples = 0;
  int repeatSamples = 1;
  float starAndEndGap = 0.0f;  // Preserves preset property spelling
  float outputMultiplier = 1.0f;

  float filterRadius = 2.0f;
  float filterStrength = 0.6f;
  float aWeight = 0.0f;
  float lowerFreqMagnitude = 0.25f;
  float smooth = 1.0f;
  float preSmooth = 1.0f;

  // Frame-rate-independent asymmetric temporal smoothing (opt-in via temporalSmooth/attackMs/releaseMs)
  bool useAttackRelease = false;
  float temporalSmooth = 0.5f;
  float attackMs = 25.0f;
  float releaseMs = 150.0f;

  // Separate DSP frequency-bin smoothing stage
  FrequencySmoothMode freqSmoothMode = FrequencySmoothMode::None;
  float freqSmoothRadius = 2.0f;
  float freqSmoothStrength = 0.7f;

  float beatRangeBarFirst = 0.0f;
  float beatRangeBarLast = 0.2f;
  float beatRangeValueLower = 0.7f;
  float beatRangeValueHigher = 35.0f;
  float beatSmooth = 0.4f;
  bool strictCompatBeatQuirk = false;

  bool operator==(const SpectrumConfig&) const = default;
};

/// Builds a SpectrumConfig from an AudioProvider Element's property bag.
SpectrumConfig spectrumConfigFromElement(const iota::model::Element& audioProvider);

/// Envelope follower for integer-16-range AGC (`300 / max(peak - floor, 1)`).
struct AgcEnvelope {
  float peak = 0.0f;
  float floor = 0.0f;
  float gain = 1.0f;

  void reset() noexcept {
    peak = 0.0f;
    floor = 0.0f;
    gain = 1.0f;
  }

  float update(float maxAbsInt16Sample, float dtSec = 1.0f / 60.0f) noexcept;
};

struct BeatDetectorState {
  float sumH = 0.0f;
  float accumJ = 0.0f;
  float smoothedBeat = 0.0f;
  float peakBarIndexSmoothed = 0.0f;
  float peakValue = 0.0f;

  void reset() noexcept;
  void update(std::span<const float> bars,
              const SpectrumConfig& cfg,
              float dtSec = 1.0f / 60.0f) noexcept;
};

struct AudioFrameInput {
  /// Normalized mono PCM [-1.0, 1.0] ending at the current playback cursor.
  std::span<const float> monoPcm;
  int sampleRate = 44100;
  float dt = 1.0f / 60.0f;
  double songTimeSec = 0.0;
  double trackPositionSec = 0.0;
  double trackDurationSec = 0.0;
  bool isPlaying = false;
  bool isExporting = false;
};

struct AudioAnalysisResult {
  std::vector<float> rawBars;
  std::vector<float> smoothedBars;
  float beat = 0.0f;
  float peakBarIndex = 0.0f;
  float rmsAmplitude = 0.0f;
  float peakAmplitude = 0.0f;
};

/// Stateful DSP engine implementing Waveform, Spectrum (kiss_fftr), Spectrum2 (DFT filterbank),
/// AGC, A-weighting, beat detection, and time-indexed bar history buffering.
class AudioAnalyzer {
 public:
  AudioAnalyzer();
  ~AudioAnalyzer();

  AudioAnalyzer(const AudioAnalyzer&) = delete;
  AudioAnalyzer& operator=(const AudioAnalyzer&) = delete;
  AudioAnalyzer(AudioAnalyzer&&) noexcept;
  AudioAnalyzer& operator=(AudioAnalyzer&&) noexcept;

  void setConfig(const SpectrumConfig& cfg);
  [[nodiscard]] const SpectrumConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] const std::vector<float>& cachedBarFrequencies() const noexcept;

  void reset();

  const AudioAnalysisResult& process(const AudioFrameInput& input);

  void sampleBarsForLayer(int reactionDelay,
                          int reactionAccumulatedDelay,
                          int softness,
                          std::vector<float>& outBars) const;

  [[nodiscard]] const AudioAnalysisResult& lastResult() const noexcept { return result_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  SpectrumConfig cfg_{};
  AudioAnalysisResult result_{};
};

/// Utility returning mapped bar center frequencies in Hz for diagnostic/UI inspection.
std::vector<float> computeBarFrequencies(const SpectrumConfig& cfg);

}  // namespace iota::dsp
