#include "doctest/doctest.h"

#include "iota/audio/audio_engine.hpp"
#include "iota/engine/scene_renderer.hpp"
#include "iota/export/video_exporter.hpp"
#include "iota/model/scene.hpp"
#include "iota/rhi/gl45_device.hpp"

#include <GLFW/glfw3.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {

struct ScopedTempFile {
  std::filesystem::path path;
  ~ScopedTempFile() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }
};

}  // namespace

TEST_CASE("Video export labels, encoder mapping, and fallback resolution") {
  using namespace iota::export_video;

  CHECK(std::string(videoCodecDisplayName(VideoCodec::H264)).find("H.264") != std::string::npos);
  CHECK(std::string(videoCodecDisplayName(VideoCodec::H265)).find("H.265") != std::string::npos);
  CHECK(std::string(videoEncoderBackendDisplayName(VideoEncoderBackend::Auto)).find("Auto") != std::string::npos);
  CHECK(std::string(videoEncoderBackendDisplayName(VideoEncoderBackend::NvidiaNvenc)).find("NVENC") != std::string::npos);
  CHECK(std::string(videoEncoderBackendDisplayName(VideoEncoderBackend::AmdAmf)).find("AMD AMF") != std::string::npos);
  CHECK(std::string(videoEncoderBackendDisplayName(VideoEncoderBackend::IntelQsv)).find("Quick Sync") != std::string::npos);
  CHECK(std::string(videoEncoderBackendDisplayName(VideoEncoderBackend::CpuSoftware)).find("CPU") != std::string::npos);
  CHECK(std::string(encoderPresetDisplayName(EncoderPreset::Fast)).find("Fast") != std::string::npos);
  CHECK(std::string(encoderPresetDisplayName(EncoderPreset::Balanced)).find("Balanced") != std::string::npos);
  CHECK(std::string(encoderPresetDisplayName(EncoderPreset::Quality)).find("Quality") != std::string::npos);

  CHECK(std::string(ffmpegEncoderName(VideoEncoderBackend::CpuSoftware, VideoCodec::H264)) == "libx264");
  CHECK(std::string(ffmpegEncoderName(VideoEncoderBackend::CpuSoftware, VideoCodec::H265)) == "libx265");
  CHECK(std::string(ffmpegEncoderName(VideoEncoderBackend::NvidiaNvenc, VideoCodec::H264)) == "h264_nvenc");
  CHECK(std::string(ffmpegEncoderName(VideoEncoderBackend::NvidiaNvenc, VideoCodec::H265)) == "hevc_nvenc");
  CHECK(std::string(ffmpegEncoderName(VideoEncoderBackend::AmdAmf, VideoCodec::H264)) == "h264_amf");
  CHECK(std::string(ffmpegEncoderName(VideoEncoderBackend::AmdAmf, VideoCodec::H265)) == "hevc_amf");
  CHECK(std::string(ffmpegEncoderName(VideoEncoderBackend::IntelQsv, VideoCodec::H264)) == "h264_qsv");
  CHECK(std::string(ffmpegEncoderName(VideoEncoderBackend::IntelQsv, VideoCodec::H265)) == "hevc_qsv");

  SUBCASE("Auto priority selects NVENC > AMF > QSV > CPU") {
    ExportSettings cfg;
    cfg.encoderBackend = VideoEncoderBackend::Auto;
    cfg.codec = VideoCodec::H264;

    HardwareEncoderCapabilities caps;
    caps.probed = true;
    caps.ffmpegAvailable = true;
    caps.cpuH264 = true;
    caps.qsvH264 = true;
    caps.amfH264 = true;
    caps.nvencH264 = true;

    auto p1 = resolveEncoderSelection(cfg, caps);
    CHECK(p1.ok);
    CHECK(p1.resolvedBackend == VideoEncoderBackend::NvidiaNvenc);
    CHECK(p1.ffmpegEncoder == "h264_nvenc");
    CHECK(p1.pixelFormat == "nv12");
    CHECK(!p1.usedCpuFallback);

    caps.nvencH264 = false;
    auto p2 = resolveEncoderSelection(cfg, caps);
    CHECK(p2.ok);
    CHECK(p2.resolvedBackend == VideoEncoderBackend::AmdAmf);
    CHECK(p2.ffmpegEncoder == "h264_amf");

    caps.amfH264 = false;
    auto p3 = resolveEncoderSelection(cfg, caps);
    CHECK(p3.ok);
    CHECK(p3.resolvedBackend == VideoEncoderBackend::IntelQsv);
    CHECK(p3.ffmpegEncoder == "h264_qsv");

    caps.qsvH264 = false;
    auto p4 = resolveEncoderSelection(cfg, caps);
    CHECK(p4.ok);
    CHECK(p4.resolvedBackend == VideoEncoderBackend::CpuSoftware);
    CHECK(p4.ffmpegEncoder == "libx264");
    CHECK(p4.pixelFormat == "yuv420p");
    CHECK(!p4.usedCpuFallback);
  }

  SUBCASE("Explicit unavailable GPU respects CPU fallback toggle and FFmpeg availability") {
    HardwareEncoderCapabilities caps;
    caps.probed = true;
    caps.ffmpegAvailable = true;
    caps.cpuH264 = true;
    caps.cpuH265 = true;

    ExportSettings cfg;
    cfg.encoderBackend = VideoEncoderBackend::NvidiaNvenc;
    cfg.codec = VideoCodec::H265;
    cfg.fallbackToCpuOnError = true;

    auto withFallback = resolveEncoderSelection(cfg, caps);
    CHECK(withFallback.ok);
    CHECK(withFallback.resolvedBackend == VideoEncoderBackend::CpuSoftware);
    CHECK(withFallback.ffmpegEncoder == "libx265");
    CHECK(withFallback.usedCpuFallback);
    CHECK(!withFallback.fallbackReason.empty());

    cfg.fallbackToCpuOnError = false;
    auto noFallback = resolveEncoderSelection(cfg, caps);
    CHECK(!noFallback.ok);
    CHECK(!noFallback.errorMessage.empty());

    HardwareEncoderCapabilities noFfmpeg;
    noFfmpeg.probed = true;
    noFfmpeg.ffmpegAvailable = false;
    auto missing = resolveEncoderSelection(cfg, noFfmpeg);
    CHECK(!missing.ok);
    CHECK(missing.errorMessage.find("ffmpeg.exe") != std::string::npos);

    CHECK(caps.summaryText().find("CPU") != std::string::npos);
    caps.amfH264 = true;
    caps.amfH265 = true;
    CHECK(caps.hasAnyHardware());
    CHECK(caps.summaryText().find("AMD AMF (H.264, H.265)") != std::string::npos);
  }

  SUBCASE("FFmpeg CLI generation emits backend pixel formats, presets, and HEVC hvc1 tag") {
    HardwareEncoderCapabilities caps;
    caps.probed = true;
    caps.ffmpegAvailable = true;
    caps.cpuH264 = caps.cpuH265 = true;
    caps.nvencH264 = caps.nvencH265 = true;
    caps.amfH264 = caps.amfH265 = true;
    caps.qsvH264 = caps.qsvH265 = true;

    ExportSettings cfg;
    cfg.outputPath = "out test.mp4";
    cfg.fps = 60;
    cfg.videoBitrateKbps = 15000;

    cfg.encoderBackend = VideoEncoderBackend::CpuSoftware;
    cfg.codec = VideoCodec::H264;
    cfg.encoderPreset = EncoderPreset::Fast;
    auto cpuPlan = resolveEncoderSelection(cfg, caps);
    auto cpuCmd = buildFfmpegExportCommand(cfg, cpuPlan, 1280, 720, 0.0, 5.0, {}, false);
    CHECK(cpuCmd.find(L"-c:v libx264") != std::wstring::npos);
    CHECK(cpuCmd.find(L"-preset veryfast") != std::wstring::npos);
    CHECK(cpuCmd.find(L"-pix_fmt yuv420p") != std::wstring::npos);
    CHECK(cpuCmd.find(L"-tag:v hvc1") == std::wstring::npos);

    cfg.encoderBackend = VideoEncoderBackend::NvidiaNvenc;
    cfg.codec = VideoCodec::H265;
    cfg.encoderPreset = EncoderPreset::Quality;
    auto nvPlan = resolveEncoderSelection(cfg, caps);
    auto nvCmd = buildFfmpegExportCommand(cfg, nvPlan, 1920, 1080, 1.5, 4.0, "song.mp3", true);
    CHECK(nvCmd.find(L"-c:v hevc_nvenc") != std::wstring::npos);
    CHECK(nvCmd.find(L"-preset p6") != std::wstring::npos);
    CHECK(nvCmd.find(L"-pix_fmt nv12") != std::wstring::npos);
    CHECK(nvCmd.find(L"-tag:v hvc1") != std::wstring::npos);
    CHECK(nvCmd.find(L"-c:a aac") != std::wstring::npos);

    cfg.encoderBackend = VideoEncoderBackend::AmdAmf;
    cfg.codec = VideoCodec::H264;
    cfg.encoderPreset = EncoderPreset::Balanced;
    auto amfPlan = resolveEncoderSelection(cfg, caps);
    auto amfCmd = buildFfmpegExportCommand(cfg, amfPlan, 1280, 720, 0.0, 5.0, {}, false);
    CHECK(amfCmd.find(L"-c:v h264_amf") != std::wstring::npos);
    CHECK(amfCmd.find(L"-quality balanced") != std::wstring::npos);
    CHECK(amfCmd.find(L"-pix_fmt nv12") != std::wstring::npos);

    cfg.encoderBackend = VideoEncoderBackend::IntelQsv;
    cfg.codec = VideoCodec::H265;
    cfg.encoderPreset = EncoderPreset::Fast;
    auto qsvPlan = resolveEncoderSelection(cfg, caps);
    auto qsvCmd = buildFfmpegExportCommand(cfg, qsvPlan, 1280, 720, 0.0, 5.0, {}, false);
    CHECK(qsvCmd.find(L"-c:v hevc_qsv") != std::wstring::npos);
    CHECK(qsvCmd.find(L"-preset veryfast") != std::wstring::npos);
    CHECK(qsvCmd.find(L"-pix_fmt nv12") != std::wstring::npos);
    CHECK(qsvCmd.find(L"-tag:v hvc1") != std::wstring::npos);
  }
}

TEST_CASE("HardwareEncoderProbe async cancellation and lifecycle are crash-safe") {
  using namespace iota::export_video;

  for (int iter = 0; iter < 5; ++iter) {
    HardwareEncoderProbe probe;
    probe.startAsync();
    std::this_thread::sleep_for(std::chrono::milliseconds(5 * iter));
    probe.cancel();
    CHECK(!probe.isProbing());
  }

  HardwareEncoderProbe probe;
  probe.startAsync();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (probe.isProbing() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(!probe.isProbing());
  const auto caps = probe.snapshot();
  CHECK(caps.probed);
}

TEST_CASE("Live OpenGL 4.5 VideoExportSession handles hardware export, CPU fallback, and cancel cleanly") {
  using namespace iota::export_video;

  const auto caps = probeHardwareEncodersSync();
  REQUIRE(caps.probed);
  if (!caps.ffmpegAvailable) {
    MESSAGE("Skipping live FFmpeg video export test because ffmpeg.exe is not installed.");
    return;
  }

  if (!glfwInit()) {
    FAIL("glfwInit failed");
    return;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

  GLFWwindow* win = glfwCreateWindow(256, 256, "iota_export_test", nullptr, nullptr);
  if (!win) {
    glfwTerminate();
    FAIL("Failed to create OpenGL 4.5 hidden test window");
    return;
  }
  glfwMakeContextCurrent(win);

  iota::rhi::Gl45RenderDevice device;
  std::string err;
  REQUIRE_MESSAGE(device.initialize(&err), err);

  iota::engine::SceneRenderer renderer;
  REQUIRE_MESSAGE(renderer.initialize(&device, &err), err);

  iota::audio::AudioEngine audio;
  audio.loadTrack(iota::audio::createSyntheticSineTrack(120.0f, 3.0, 44100, 0.5f, "Export Test Tone"));
  const auto proj = iota::model::createStarterProject();

  const auto tempDir = std::filesystem::temp_directory_path();

  SUBCASE("Auto encoder exports valid MP4 end-to-end") {
    ScopedTempFile outGuard{tempDir / "iota_test_auto_export.mp4"};
    ExportSettings cfg;
    cfg.outputPath = outGuard.path;
    cfg.width = 320;
    cfg.height = 240;
    cfg.fps = 30;
    cfg.videoBitrateKbps = 4000;
    cfg.codec = VideoCodec::H264;
    cfg.encoderBackend = VideoEncoderBackend::Auto;
    cfg.encoderPreset = EncoderPreset::Fast;
    cfg.startSec = 0.0;
    cfg.endSec = 8.0 / 30.0;
    cfg.preRollFrames = 2;

    std::string exportErr;
    REQUIRE_MESSAGE(exportVideoBlocking(cfg, device, renderer, proj, audio, {}, &exportErr), exportErr);
    REQUIRE(std::filesystem::exists(outGuard.path));
    CHECK(std::filesystem::file_size(outGuard.path) > 1024);
  }

  SUBCASE("Unavailable GPU backend triggers clean fallback or clean error without crashing") {
    VideoEncoderBackend missingBackend = VideoEncoderBackend::Auto;
    for (VideoEncoderBackend candidate : {VideoEncoderBackend::NvidiaNvenc,
                                        VideoEncoderBackend::AmdAmf,
                                        VideoEncoderBackend::IntelQsv}) {
      if (!caps.supports(candidate, VideoCodec::H264)) {
        missingBackend = candidate;
        break;
      }
    }

    if (missingBackend != VideoEncoderBackend::Auto && caps.cpuH264) {
      ScopedTempFile outGuard{tempDir / "iota_test_fallback_export.mp4"};
      ExportSettings cfg;
      cfg.outputPath = outGuard.path;
      cfg.width = 320;
      cfg.height = 240;
      cfg.fps = 30;
      cfg.videoBitrateKbps = 4000;
      cfg.codec = VideoCodec::H264;
      cfg.encoderBackend = missingBackend;
      cfg.encoderPreset = EncoderPreset::Fast;
      cfg.fallbackToCpuOnError = true;
      cfg.startSec = 0.0;
      cfg.endSec = 6.0 / 30.0;
      cfg.preRollFrames = 2;

      VideoExportSession session;
      std::string beginErr;
      REQUIRE_MESSAGE(session.begin(cfg, audio, &beginErr, &caps), beginErr);
      CHECK(session.usedCpuFallback());
      CHECK(session.activeBackend() == VideoEncoderBackend::CpuSoftware);
      CHECK(!session.fallbackNotice().empty());

      renderer.resetSimulation();
      while (!session.isDone()) {
        std::string stepErr;
        REQUIRE_MESSAGE(session.stepFrame(device, renderer, proj, audio, &stepErr), stepErr);
      }
      std::string finErr;
      REQUIRE_MESSAGE(session.finish(&finErr), finErr);
      REQUIRE(std::filesystem::exists(outGuard.path));
      CHECK(std::filesystem::file_size(outGuard.path) > 1024);

      cfg.fallbackToCpuOnError = false;
      VideoExportSession failSession;
      std::string failErr;
      CHECK(!failSession.begin(cfg, audio, &failErr, &caps));
      CHECK(!failErr.empty());
      CHECK(!failSession.isActive());
    }
  }

  SUBCASE("Mid-export cancel cleans up cleanly") {
    ScopedTempFile outGuard{tempDir / "iota_test_cancel_export.mp4"};
    ExportSettings cfg;
    cfg.outputPath = outGuard.path;
    cfg.width = 320;
    cfg.height = 240;
    cfg.fps = 30;
    cfg.encoderBackend = VideoEncoderBackend::Auto;
    cfg.encoderPreset = EncoderPreset::Fast;
    cfg.startSec = 0.0;
    cfg.endSec = 1.0;
    cfg.preRollFrames = 1;

    VideoExportSession session;
    std::string beginErr;
    REQUIRE_MESSAGE(session.begin(cfg, audio, &beginErr, &caps), beginErr);
    renderer.resetSimulation();
    for (int f = 0; f < 3 && !session.isDone(); ++f) {
      std::string stepErr;
      REQUIRE_MESSAGE(session.stepFrame(device, renderer, proj, audio, &stepErr), stepErr);
    }
    session.cancel();
    CHECK(!session.isActive());
  }

  renderer.shutdown();
  device.shutdown();
  glfwDestroyWindow(win);
  glfwTerminate();
}
