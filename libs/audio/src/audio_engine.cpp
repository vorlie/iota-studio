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

#define MA_IMPLEMENTATION
#include "iota/audio/audio_engine.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace iota::audio {

namespace {

int64_t steadyNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

struct AudioEngine::Impl {
  mutable std::mutex trackMutex;
  AudioTrackData track;
  bool hasTrackData = false;

  std::mutex deviceMutex;
  ma_device device{};
  std::atomic<bool> deviceInitialized{false};
  bool deviceStarted = false;
  int deviceSampleRate = 0;

  std::atomic<uint64_t> cursorFrames{0};
  std::atomic<uint64_t> totalFrames{0};
  std::atomic<bool> playing{false};
  std::atomic<float> volume{0.9f};

  uint64_t lastBlockStartFrames = 0;
  uint32_t lastBlockFrames = 0;
  int64_t lastCallbackSteadyNs = 0;
  mutable double lastReportedPlaySec = 0.0;

  [[nodiscard]] double computeInterpolatedTimeLocked() const noexcept {
    const double sr = static_cast<double>(track.sampleRate);
    if (!playing.load(std::memory_order_acquire) || lastCallbackSteadyNs <= 0) {
      const double curSec = static_cast<double>(cursorFrames.load(std::memory_order_acquire)) / sr;
      return std::clamp(curSec, 0.0, track.durationSec);
    }
    const double elapsedSec =
        std::max(0.0, static_cast<double>(steadyNowNs() - lastCallbackSteadyNs) * 1e-9);
    const double blockSpanSec =
        (lastBlockFrames > 0) ? (static_cast<double>(lastBlockFrames) / sr) : 0.015;
    const double maxInterpSec = std::clamp(blockSpanSec * 1.25, 0.005, 0.06);
    double t = (static_cast<double>(lastBlockStartFrames) / sr) + std::min(elapsedSec, maxInterpSec);
    t = std::clamp(t, 0.0, track.durationSec);
    if (t < lastReportedPlaySec) {
      t = lastReportedPlaySec;
    } else {
      lastReportedPlaySec = t;
    }
    return t;
  }

  static void dataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount) {
    auto* self = static_cast<Impl*>(device->pUserData);
    float* out = static_cast<float*>(output);
    const size_t totalOutFloats = static_cast<size_t>(frameCount) * 2;
    std::memset(out, 0, totalOutFloats * sizeof(float));

    if (!self || !self->playing.load(std::memory_order_acquire)) {
      return;
    }

    std::unique_lock<std::mutex> lock(self->trackMutex, std::try_to_lock);
    if (!lock.owns_lock() || !self->hasTrackData || self->track.interleavedPcm.empty()) {
      return;
    }

    const uint64_t trackFrames = self->totalFrames.load(std::memory_order_relaxed);
    uint64_t pos = self->cursorFrames.load(std::memory_order_relaxed);
    if (pos >= trackFrames) {
      self->playing.store(false, std::memory_order_release);
      self->lastCallbackSteadyNs = 0;
      return;
    }

    const uint64_t avail = trackFrames - pos;
    const ma_uint32 toCopy = static_cast<ma_uint32>(std::min<uint64_t>(frameCount, avail));
    const float vol = std::clamp(self->volume.load(std::memory_order_relaxed), 0.0f, 1.5f);

    const float* src = self->track.interleavedPcm.data() + static_cast<size_t>(pos) * 2;
    for (ma_uint32 i = 0; i < toCopy * 2; ++i) {
      out[i] = src[i] * vol;
    }

    self->lastBlockStartFrames = pos;
    self->lastBlockFrames = toCopy;
    self->lastCallbackSteadyNs = steadyNowNs();

    const uint64_t nextPos = pos + toCopy;
    self->cursorFrames.store(nextPos, std::memory_order_release);
    if (nextPos >= trackFrames) {
      self->playing.store(false, std::memory_order_release);
      self->lastCallbackSteadyNs = 0;
    }
  }

  bool ensureDevice(int sampleRate) {
    std::lock_guard<std::mutex> devLock(deviceMutex);
    const bool initialized = deviceInitialized.load(std::memory_order_acquire);
    if (initialized && deviceSampleRate == sampleRate) {
      if (!deviceStarted) {
        deviceStarted = (ma_device_start(&device) == MA_SUCCESS);
      }
      return true;
    }
    if (initialized) {
      ma_device_uninit(&device);
      deviceInitialized.store(false, std::memory_order_release);
      deviceStarted = false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = static_cast<ma_uint32>(std::max(8000, sampleRate));
    cfg.periodSizeInMilliseconds = 10;
    cfg.dataCallback = &Impl::dataCallback;
    cfg.pUserData = this;

    if (ma_device_init(nullptr, &cfg, &device) != MA_SUCCESS) {
      deviceInitialized.store(false, std::memory_order_release);
      deviceStarted = false;
      return false;
    }
    if (ma_device_start(&device) != MA_SUCCESS) {
      ma_device_uninit(&device);
      deviceInitialized.store(false, std::memory_order_release);
      deviceStarted = false;
      return false;
    }
    deviceInitialized.store(true, std::memory_order_release);
    deviceStarted = true;
    deviceSampleRate = sampleRate;
    return true;
  }

  ~Impl() {
    std::lock_guard<std::mutex> devLock(deviceMutex);
    if (deviceInitialized.load(std::memory_order_acquire)) {
      ma_device_uninit(&device);
      deviceInitialized.store(false, std::memory_order_release);
      deviceStarted = false;
    }
  }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() = default;

bool AudioEngine::loadFile(const std::filesystem::path& path, std::string* outError) {
  auto decoded = decodeAudioFile(path, 44100, outError);
  if (!decoded) {
    return false;
  }
  return loadTrack(std::move(*decoded), outError);
}

bool AudioEngine::loadTrack(AudioTrackData track, std::string* /*outError*/) {
  if (track.monoPcm.empty() && !track.interleavedPcm.empty()) {
    rebuildMonoAndDuration(track);
  }
  impl_->playing.store(false, std::memory_order_release);
  int sampleRate = 44100;
  {
    std::lock_guard<std::mutex> lock(impl_->trackMutex);
    impl_->track = std::move(track);
    impl_->hasTrackData = true;
    sampleRate = impl_->track.sampleRate;
    impl_->totalFrames.store(impl_->track.monoPcm.size(), std::memory_order_release);
    impl_->cursorFrames.store(0, std::memory_order_release);
    impl_->lastBlockStartFrames = 0;
    impl_->lastBlockFrames = 0;
    impl_->lastCallbackSteadyNs = 0;
    impl_->lastReportedPlaySec = 0.0;
  }
  impl_->ensureDevice(sampleRate);
  return true;
}

void AudioEngine::unload() {
  impl_->playing.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  impl_->track = AudioTrackData{};
  impl_->hasTrackData = false;
  impl_->totalFrames.store(0, std::memory_order_release);
  impl_->cursorFrames.store(0, std::memory_order_release);
  impl_->lastBlockStartFrames = 0;
  impl_->lastBlockFrames = 0;
  impl_->lastCallbackSteadyNs = 0;
  impl_->lastReportedPlaySec = 0.0;
}

void AudioEngine::play() {
  if (!hasTrack()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->trackMutex);
    if (impl_->cursorFrames.load(std::memory_order_acquire) >=
        impl_->totalFrames.load(std::memory_order_acquire)) {
      impl_->cursorFrames.store(0, std::memory_order_release);
      impl_->lastBlockStartFrames = 0;
      impl_->lastReportedPlaySec = 0.0;
    } else if (impl_->track.sampleRate > 0) {
      const uint64_t pos = impl_->cursorFrames.load(std::memory_order_acquire);
      impl_->lastBlockStartFrames = pos;
      impl_->lastReportedPlaySec = static_cast<double>(pos) / static_cast<double>(impl_->track.sampleRate);
    }
    impl_->lastBlockFrames = 0;
    impl_->lastCallbackSteadyNs = 0;
  }
  {
    std::lock_guard<std::mutex> devLock(impl_->deviceMutex);
    if (impl_->deviceInitialized.load(std::memory_order_acquire) && !impl_->deviceStarted) {
      impl_->deviceStarted = (ma_device_start(&impl_->device) == MA_SUCCESS);
    }
  }
  impl_->playing.store(true, std::memory_order_release);
}

void AudioEngine::pause() {
  if (!impl_->playing.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  if (impl_->hasTrackData && impl_->track.sampleRate > 0 && impl_->lastCallbackSteadyNs > 0) {
    const double t = impl_->computeInterpolatedTimeLocked();
    const uint64_t total = impl_->totalFrames.load(std::memory_order_relaxed);
    const uint64_t pausedFrame =
        std::min(total, static_cast<uint64_t>(std::llround(t * static_cast<double>(impl_->track.sampleRate))));
    impl_->cursorFrames.store(pausedFrame, std::memory_order_release);
    impl_->lastBlockStartFrames = pausedFrame;
    impl_->lastReportedPlaySec = static_cast<double>(pausedFrame) / static_cast<double>(impl_->track.sampleRate);
  }
  impl_->playing.store(false, std::memory_order_release);
  impl_->lastCallbackSteadyNs = 0;
  impl_->lastBlockFrames = 0;
}

void AudioEngine::stop() {
  impl_->playing.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  impl_->cursorFrames.store(0, std::memory_order_release);
  impl_->lastBlockStartFrames = 0;
  impl_->lastBlockFrames = 0;
  impl_->lastCallbackSteadyNs = 0;
  impl_->lastReportedPlaySec = 0.0;
}

void AudioEngine::seekSeconds(double seconds) {
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  if (!impl_->hasTrackData) {
    return;
  }
  const double clamped = std::clamp(seconds, 0.0, impl_->track.durationSec);
  const uint64_t frame = static_cast<uint64_t>(clamped * static_cast<double>(impl_->track.sampleRate));
  const uint64_t bounded = std::min(frame, impl_->totalFrames.load(std::memory_order_relaxed));
  impl_->cursorFrames.store(bounded, std::memory_order_release);
  impl_->lastBlockStartFrames = bounded;
  impl_->lastBlockFrames = 0;
  impl_->lastCallbackSteadyNs = 0;
  impl_->lastReportedPlaySec =
      (impl_->track.sampleRate > 0)
          ? (static_cast<double>(bounded) / static_cast<double>(impl_->track.sampleRate))
          : 0.0;
}

void AudioEngine::setVolume(float volume) {
  impl_->volume.store(std::clamp(volume, 0.0f, 1.5f), std::memory_order_release);
}

bool AudioEngine::hasTrack() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  return impl_->hasTrackData;
}

bool AudioEngine::isPlaying() const noexcept {
  return impl_->playing.load(std::memory_order_acquire);
}

bool AudioEngine::isAudioDeviceReady() const noexcept {
  return impl_->deviceInitialized.load(std::memory_order_acquire);
}

float AudioEngine::volume() const noexcept {
  return impl_->volume.load(std::memory_order_acquire);
}

double AudioEngine::currentTimeSeconds() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  if (!impl_->hasTrackData || impl_->track.sampleRate <= 0) {
    return 0.0;
  }
  return impl_->computeInterpolatedTimeLocked();
}

double AudioEngine::durationSeconds() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  return impl_->hasTrackData ? impl_->track.durationSec : 0.0;
}

int AudioEngine::sampleRate() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  return impl_->hasTrackData ? impl_->track.sampleRate : 44100;
}

std::string AudioEngine::currentTitle() const {
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  return impl_->hasTrackData ? impl_->track.title : std::string{};
}

std::string AudioEngine::currentArtist() const {
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  return impl_->hasTrackData ? impl_->track.artist : std::string{};
}

std::string AudioEngine::currentAlbum() const {
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  return impl_->hasTrackData ? impl_->track.album : std::string{};
}

std::string AudioEngine::currentFilePath() const {
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  return impl_->hasTrackData ? impl_->track.filePath : std::string{};
}

void AudioEngine::extractMonoWindow(double timeSec,
                                    int numSamples,
                                    float visOffsetMs,
                                    std::vector<float>& outMono) const {
  const int count = std::clamp(numSamples, 1, 65536);
  outMono.assign(static_cast<size_t>(count), 0.0f);

  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  if (!impl_->hasTrackData || impl_->track.monoPcm.empty()) {
    return;
  }

  const double queryTime = timeSec + static_cast<double>(visOffsetMs) * 0.001;
  const int64_t startIdx = static_cast<int64_t>(std::llround(queryTime * static_cast<double>(impl_->track.sampleRate)));
  const int64_t endIdx = startIdx + static_cast<int64_t>(count);
  const int64_t total = static_cast<int64_t>(impl_->track.monoPcm.size());

  const int64_t validStart = std::max<int64_t>(0, startIdx);
  const int64_t validEnd = std::min<int64_t>(total, endIdx);
  if (validEnd <= validStart) {
    return;
  }

  const int64_t dstOffset = validStart - startIdx;
  const int64_t copyLen = validEnd - validStart;
  std::copy_n(impl_->track.monoPcm.data() + validStart,
              copyLen,
              outMono.data() + dstOffset);
}

}  // namespace iota::audio
