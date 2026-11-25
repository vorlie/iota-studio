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
