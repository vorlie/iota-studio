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

struct ExportSettings {
  std::filesystem::path outputPath = "output.mp4";
  int width = 1280;
  int height = 720;
  int fps = 60;
  int videoBitrateKbps = 12000;
  bool useHevc = false;
  double startSec = 0.0;
  double endSec = 0.0;  // <= startSec uses full audio track duration (or 10s if no track)
  int preRollFrames = 60;
  bool align16 = true;
  bool fxaa = true;
  float visOffsetMs = 0.0f;
};

/// Frame-by-frame deterministic offline video exporter piping fixed-timestep RGBA frames
/// and optional audio into ffmpeg.exe (H.264 / H.265 + AAC MP4).
/// Methods must be called on the thread owning the active IRenderDevice OpenGL context.
class VideoExportSession {
 public:
  VideoExportSession();
  ~VideoExportSession();

  VideoExportSession(const VideoExportSession&) = delete;
  VideoExportSession& operator=(const VideoExportSession&) = delete;

  bool begin(const ExportSettings& settings,
             const iota::audio::AudioEngine& audio,
             std::string* outError = nullptr);

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
