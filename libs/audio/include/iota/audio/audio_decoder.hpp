#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace iota::audio {

struct AudioTrackData {
  int sampleRate = 44100;
  int channels = 2;
  double durationSec = 0.0;
  std::string filePath;
  std::string title;
  std::string artist;
  std::string album;
  std::vector<float> interleavedPcm;  // Stereo interleaved float [-1.0, 1.0]
  std::vector<float> monoPcm;         // Mono downmix (L + R) * 0.5
};

/// Decodes an audio file (MP3, FLAC, OGG, Opus, AAC, WAV, video tracks) into memory
/// via ffmpeg.exe / ffprobe.exe with miniaudio ma_decoder fallback.
std::optional<AudioTrackData> decodeAudioFile(const std::filesystem::path& path,
                                              int targetSampleRate = 44100,
                                              std::string* outError = nullptr);

/// Utility to recompute monoPcm and durationSec after modifying interleavedPcm.
void rebuildMonoAndDuration(AudioTrackData& track);

/// Generates a synthetic stereo + mono test tone (useful for smoke/unit tests).
AudioTrackData createSyntheticSineTrack(float freqHz,
                                        double durationSec,
                                        int sampleRate = 44100,
                                        float amplitude = 0.5f,
                                        std::string title = "Synthetic Tone");

}  // namespace iota::audio
