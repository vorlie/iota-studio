#pragma once

#include "iota/audio/audio_decoder.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace iota::audio {

/// Thread-safe WASAPI/miniaudio playback device and PCM sample window extractor.
class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  bool loadFile(const std::filesystem::path& path, std::string* outError = nullptr);
  bool loadTrack(AudioTrackData track, std::string* outError = nullptr);
  void unload();

  void play();
  void pause();
  void stop();
  void seekSeconds(double seconds);
  void setVolume(float volume);

  [[nodiscard]] bool hasTrack() const noexcept;
  [[nodiscard]] bool isPlaying() const noexcept;
  [[nodiscard]] bool isAudioDeviceReady() const noexcept;
  [[nodiscard]] float volume() const noexcept;
  [[nodiscard]] double currentTimeSeconds() const noexcept;
  [[nodiscard]] double durationSeconds() const noexcept;
  [[nodiscard]] int sampleRate() const noexcept;

  /// Returns metadata snapshot of current track (empty if none loaded).
  [[nodiscard]] std::string currentTitle() const;
  [[nodiscard]] std::string currentArtist() const;
  [[nodiscard]] std::string currentAlbum() const;
  [[nodiscard]] std::string currentFilePath() const;

  /// Extracts `numSamples` mono samples starting at `(timeSec + visOffsetMs * 0.001)`
  /// for deterministic live or offline DSP analysis. Zero-pads out of track bounds.
  void extractMonoWindow(double timeSec,
                         int numSamples,
                         float visOffsetMs,
                         std::vector<float>& outMono) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace iota::audio
