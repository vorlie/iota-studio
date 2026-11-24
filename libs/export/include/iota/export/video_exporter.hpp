#pragma once

#include "iota/audio/audio_engine.hpp"
#include "iota/engine/scene_renderer.hpp"
#include "iota/model/scene.hpp"
#include "iota/rhi/rhi.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace iota::export_video {

enum class VideoCodec {
  H264 = 0,
  H265 = 1,
};

enum class VideoEncoderBackend {
  Auto = 0,
  CpuSoftware = 1,
  NvidiaNvenc = 2,
  AmdAmf = 3,
  IntelQsv = 4,
};

enum class EncoderPreset {
  Fast = 0,
  Balanced = 1,
  Quality = 2,
};

[[nodiscard]] const char* videoCodecDisplayName(VideoCodec codec) noexcept;
[[nodiscard]] const char* videoEncoderBackendDisplayName(VideoEncoderBackend backend) noexcept;
[[nodiscard]] const char* encoderPresetDisplayName(EncoderPreset preset) noexcept;
[[nodiscard]] const char* ffmpegEncoderName(VideoEncoderBackend backend, VideoCodec codec) noexcept;
[[nodiscard]] std::string resolvedEncoderDisplayName(VideoEncoderBackend backend, VideoCodec codec);

struct ExportSettings {
  std::filesystem::path outputPath = "output.mp4";
  int width = 1280;
  int height = 720;
  int fps = 60;
  int videoBitrateKbps = 12000;
  VideoCodec codec = VideoCodec::H264;
  VideoEncoderBackend encoderBackend = VideoEncoderBackend::Auto;
  EncoderPreset encoderPreset = EncoderPreset::Balanced;
  bool fallbackToCpuOnError = true;
  double startSec = 0.0;
  double endSec = 0.0;  // <= startSec uses full audio track duration (or 10s if no track)
  int preRollFrames = 60;
  bool align16 = true;
  bool fxaa = true;
  float visOffsetMs = 0.0f;
};

struct HardwareEncoderCapabilities {
  bool probed = false;
  bool ffmpegAvailable = false;
  bool cpuH264 = false;
  bool cpuH265 = false;
  bool nvencH264 = false;
  bool nvencH265 = false;
  bool amfH264 = false;
  bool amfH265 = false;
  bool qsvH264 = false;
  bool qsvH265 = false;

  [[nodiscard]] bool supports(VideoEncoderBackend backend, VideoCodec codec) const noexcept;
  [[nodiscard]] bool hasAnyHardware() const noexcept;
  [[nodiscard]] std::string summaryText() const;
};

struct ResolvedEncoderPlan {
  bool ok = false;
  VideoEncoderBackend resolvedBackend = VideoEncoderBackend::CpuSoftware;
  VideoCodec codec = VideoCodec::H264;
  std::string ffmpegEncoder;
  std::string pixelFormat;
  std::string displayName;
  bool usedCpuFallback = false;
  std::string fallbackReason;
  std::string errorMessage;
};

/// Selects the concrete FFmpeg encoder & pixel format from user preferences and probed GPU capabilities.
[[nodiscard]] ResolvedEncoderPlan resolveEncoderSelection(const ExportSettings& settings,
                                                          const HardwareEncoderCapabilities& caps);

/// Builds the full FFmpeg CLI line for deterministic raw RGBA stdin video export.
[[nodiscard]] std::wstring buildFfmpegExportCommand(const ExportSettings& settings,
                                                    const ResolvedEncoderPlan& plan,
                                                    int outWidth,
                                                    int outHeight,
                                                    double startSec,
                                                    double exportDur,
                                                    const std::filesystem::path& audioDiskPath,
                                                    bool hasDiskAudio);

/// Runs a fast bounded 1-frame FFmpeg null encode to test whether a hardware/software encoder initializes.
[[nodiscard]] bool probeSingleFfmpegEncoder(VideoEncoderBackend backend,
                                            VideoCodec codec,
                                            std::string* outError = nullptr);

/// Synchronously probes FFmpeg availability and all CPU/GPU encoders (NVENC, AMF, QSV).
[[nodiscard]] HardwareEncoderCapabilities probeHardwareEncodersSync();

/// Asynchronous background hardware encoder detector safe for UI modal polling and early shutdown.
class HardwareEncoderProbe {
 public:
  HardwareEncoderProbe();
  ~HardwareEncoderProbe();

  HardwareEncoderProbe(const HardwareEncoderProbe&) = delete;
  HardwareEncoderProbe& operator=(const HardwareEncoderProbe&) = delete;

  void startAsync();
  void cancel();

  [[nodiscard]] bool isProbing() const noexcept;
  [[nodiscard]] HardwareEncoderCapabilities snapshot() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Frame-by-frame deterministic offline video exporter piping fixed-timestep RGBA frames
/// and optional audio into ffmpeg.exe (H.264 / H.265 via CPU or GPU hardware + AAC MP4).
/// Methods must be called on the thread owning the active IRenderDevice OpenGL context.
class VideoExportSession {
 public:
  VideoExportSession();
  ~VideoExportSession();

  VideoExportSession(const VideoExportSession&) = delete;
  VideoExportSession& operator=(const VideoExportSession&) = delete;

  bool begin(const ExportSettings& settings,
             const iota::audio::AudioEngine& audio,
             std::string* outError = nullptr,
             const HardwareEncoderCapabilities* cachedCaps = nullptr);

  bool stepFrame(iota::rhi::IRenderDevice& device,
                 iota::engine::SceneRenderer& renderer,
                 const iota::model::Project& project,
                 const iota::audio::AudioEngine& audio,
                 std::string* outError = nullptr);

  bool finish(std::string* outError = nullptr);
  void cancel();

  [[nodiscard]] bool isActive() const noexcept;
  [[nodiscard]] bool isDone() const noexcept;
  [[nodiscard]] int encodedFrames() const noexcept;
  [[nodiscard]] int totalFrames() const noexcept;
  [[nodiscard]] int currentPreRollIndex() const noexcept;
  [[nodiscard]] int alignedWidth() const noexcept;
  [[nodiscard]] int alignedHeight() const noexcept;

  [[nodiscard]] VideoEncoderBackend activeBackend() const noexcept;
  [[nodiscard]] std::string activeEncoderDisplayName() const;
  [[nodiscard]] std::string activeFfmpegEncoder() const;
  [[nodiscard]] bool usedCpuFallback() const noexcept;
  [[nodiscard]] std::string fallbackNotice() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Synchronous blocking helper useful for CLI tools and automated tests.
bool exportVideoBlocking(const ExportSettings& settings,
                         iota::rhi::IRenderDevice& device,
                         iota::engine::SceneRenderer& renderer,
                         const iota::model::Project& project,
                         const iota::audio::AudioEngine& audio,
                         std::function<bool(int encoded, int total)> onProgress = {},
                         std::string* outError = nullptr);

}  // namespace iota::export_video
