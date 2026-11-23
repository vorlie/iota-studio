#define MA_IMPLEMENTATION
#include "iota/audio/audio_engine.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace iota::audio {

struct AudioEngine::Impl {
  mutable std::mutex trackMutex;
  AudioTrackData track;
  bool hasTrackData = false;

  ma_device device{};
  bool deviceInitialized = false;
  int deviceSampleRate = 0;

  std::atomic<uint64_t> cursorFrames{0};
  std::atomic<uint64_t> totalFrames{0};
  std::atomic<bool> playing{false};
  std::atomic<float> volume{0.9f};

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
      return;
    }

    const uint64_t avail = trackFrames - pos;
    const ma_uint32 toCopy = static_cast<ma_uint32>(std::min<uint64_t>(frameCount, avail));
    const float vol = std::clamp(self->volume.load(std::memory_order_relaxed), 0.0f, 1.5f);

    const float* src = self->track.interleavedPcm.data() + static_cast<size_t>(pos) * 2;
    for (ma_uint32 i = 0; i < toCopy * 2; ++i) {
      out[i] = src[i] * vol;
    }

    const uint64_t nextPos = pos + toCopy;
    self->cursorFrames.store(nextPos, std::memory_order_release);
    if (nextPos >= trackFrames) {
      self->playing.store(false, std::memory_order_release);
    }
  }

  bool ensureDevice(int sampleRate) {
    if (deviceInitialized && deviceSampleRate == sampleRate) {
      return true;
    }
    if (deviceInitialized) {
      ma_device_uninit(&device);
      deviceInitialized = false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = static_cast<ma_uint32>(std::max(8000, sampleRate));
    cfg.dataCallback = &Impl::dataCallback;
    cfg.pUserData = this;

    if (ma_device_init(nullptr, &cfg, &device) != MA_SUCCESS) {
      deviceInitialized = false;
      return false;
    }
    if (ma_device_start(&device) != MA_SUCCESS) {
      ma_device_uninit(&device);
      deviceInitialized = false;
      return false;
    }
    deviceInitialized = true;
    deviceSampleRate = sampleRate;
    return true;
  }

  ~Impl() {
    if (deviceInitialized) {
      ma_device_uninit(&device);
      deviceInitialized = false;
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
  {
    std::lock_guard<std::mutex> lock(impl_->trackMutex);
    impl_->track = std::move(track);
    impl_->hasTrackData = true;
    impl_->totalFrames.store(impl_->track.monoPcm.size(), std::memory_order_release);
    impl_->cursorFrames.store(0, std::memory_order_release);
  }
  impl_->ensureDevice(impl_->track.sampleRate);
  return true;
}

void AudioEngine::unload() {
  impl_->playing.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  impl_->track = AudioTrackData{};
  impl_->hasTrackData = false;
  impl_->totalFrames.store(0, std::memory_order_release);
  impl_->cursorFrames.store(0, std::memory_order_release);
}

void AudioEngine::play() {
  if (!hasTrack()) {
    return;
  }
  if (impl_->cursorFrames.load(std::memory_order_acquire) >=
      impl_->totalFrames.load(std::memory_order_acquire)) {
    impl_->cursorFrames.store(0, std::memory_order_release);
  }
  if (impl_->deviceInitialized) {
    ma_device_start(&impl_->device);
  }
  impl_->playing.store(true, std::memory_order_release);
}

void AudioEngine::pause() {
  impl_->playing.store(false, std::memory_order_release);
}

void AudioEngine::stop() {
  impl_->playing.store(false, std::memory_order_release);
  impl_->cursorFrames.store(0, std::memory_order_release);
}

void AudioEngine::seekSeconds(double seconds) {
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  if (!impl_->hasTrackData) {
    return;
  }
  const double clamped = std::clamp(seconds, 0.0, impl_->track.durationSec);
  const uint64_t frame = static_cast<uint64_t>(clamped * static_cast<double>(impl_->track.sampleRate));
  impl_->cursorFrames.store(std::min(frame, impl_->totalFrames.load(std::memory_order_relaxed)),
                            std::memory_order_release);
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
  return impl_->deviceInitialized;
}

float AudioEngine::volume() const noexcept {
  return impl_->volume.load(std::memory_order_acquire);
}

double AudioEngine::currentTimeSeconds() const noexcept {
  const uint64_t pos = impl_->cursorFrames.load(std::memory_order_acquire);
  std::lock_guard<std::mutex> lock(impl_->trackMutex);
  if (!impl_->hasTrackData || impl_->track.sampleRate <= 0) {
    return 0.0;
  }
  return static_cast<double>(pos) / static_cast<double>(impl_->track.sampleRate);
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
