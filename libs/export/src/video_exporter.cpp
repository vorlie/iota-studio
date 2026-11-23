#include "iota/export/video_exporter.hpp"

#include "iota/core/utf8_path.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace iota::export_video {

namespace {

std::wstring quoteWindowsArg(const std::wstring& arg) {
  if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos && !arg.empty()) {
    return arg;
  }
  std::wstring out = L"\"";
  for (auto it = arg.begin();; ++it) {
    int numBackslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++numBackslashes;
      ++it;
    }
    if (it == arg.end()) {
      out.append(static_cast<size_t>(numBackslashes * 2), L'\\');
      break;
    }
    if (*it == L'"') {
      out.append(static_cast<size_t>(numBackslashes * 2 + 1), L'\\');
      out.push_back(L'"');
    } else {
      out.append(static_cast<size_t>(numBackslashes), L'\\');
      out.push_back(*it);
    }
  }
  out.push_back(L'"');
  return out;
}

int roundUp16(int v) noexcept {
  return ((v + 15) / 16) * 16;
}

}  // namespace

struct VideoExportSession::Impl {
  ExportSettings cfg;
  int outWidth = 1280;
  int outHeight = 720;
  double startSec = 0.0;
  double endSec = 10.0;
  int totalOutFrames = 0;
  int frameCursor = 0;  // Runs from -preRollFrames to totalOutFrames - 1

  bool active = false;
  HANDLE stdinWritePipe = nullptr;
  HANDLE processHandle = nullptr;
  HANDLE threadHandle = nullptr;
  std::vector<uint8_t> rgbaReadback;

  void cleanupProcess(bool terminate) {
    if (stdinWritePipe != nullptr) {
      CloseHandle(stdinWritePipe);
      stdinWritePipe = nullptr;
    }
    if (processHandle != nullptr) {
      if (terminate) {
        TerminateProcess(processHandle, 1);
      }
      WaitForSingleObject(processHandle, terminate ? 2000 : INFINITE);
      CloseHandle(processHandle);
      processHandle = nullptr;
    }
    if (threadHandle != nullptr) {
      CloseHandle(threadHandle);
      threadHandle = nullptr;
    }
    active = false;
  }

  ~Impl() {
    cleanupProcess(true);
  }
};

VideoExportSession::VideoExportSession() : impl_(std::make_unique<Impl>()) {}
VideoExportSession::~VideoExportSession() = default;

bool VideoExportSession::begin(const ExportSettings& settings,
                               const iota::audio::AudioEngine& audio,
                               std::string* outError) {
  cancel();

  impl_->cfg = settings;
  impl_->outWidth = std::clamp(settings.align16 ? roundUp16(settings.width) : settings.width, 16, 8192);
  impl_->outHeight = std::clamp(settings.align16 ? roundUp16(settings.height) : settings.height, 16, 8192);
  impl_->cfg.fps = std::clamp(settings.fps, 1, 120);
  impl_->cfg.preRollFrames = std::clamp(settings.preRollFrames, 0, 240);

  const double trackDur = audio.hasTrack() ? audio.durationSeconds() : 10.0;
  impl_->startSec = std::max(0.0, settings.startSec);
  impl_->endSec = (settings.endSec > impl_->startSec) ? settings.endSec : std::max(impl_->startSec + 0.1, trackDur);
  const double exportDur = std::max(0.05, impl_->endSec - impl_->startSec);
  impl_->totalOutFrames = std::max(1, static_cast<int>(std::llround(exportDur * static_cast<double>(impl_->cfg.fps))));
  impl_->frameCursor = -impl_->cfg.preRollFrames;

  if (settings.outputPath.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(settings.outputPath.parent_path(), ec);
  }

  // Prepare FFmpeg command reading raw RGBA frames from stdin pipe
  std::wstring cmd =
      L"ffmpeg.exe -y -v error -f rawvideo -pix_fmt rgba -s " +
      std::to_wstring(impl_->outWidth) + L"x" + std::to_wstring(impl_->outHeight) +
      L" -r " + std::to_wstring(impl_->cfg.fps) + L" -i pipe:0";

  const std::string audioPathStr = audio.currentFilePath();
  const std::filesystem::path audioDiskPath = iota::core::pathFromUtf8(audioPathStr);
  const bool hasDiskAudio = audio.hasTrack() && !audioPathStr.empty() &&
                            iota::core::pathExistsNoThrow(audioDiskPath);
  if (hasDiskAudio) {
    wchar_t ssBuf[32];
    wchar_t durBuf[32];
    std::swprintf(ssBuf, 32, L"%.3f", impl_->startSec);
    std::swprintf(durBuf, 32, L"%.3f", exportDur);
    cmd += L" -ss " + std::wstring(ssBuf) + L" -t " + std::wstring(durBuf) +
           L" -i " + quoteWindowsArg(audioDiskPath.wstring());
  }

  cmd += L" -c:v ";
  cmd += settings.useHevc ? L"libx265" : L"libx264";
  cmd += L" -preset medium -pix_fmt yuv420p -b:v " +
         std::to_wstring(std::clamp(settings.videoBitrateKbps, 500, 100000)) + L"k";

  if (hasDiskAudio) {
    cmd += L" -c:a aac -b:a 192k -shortest";
  } else {
    cmd += L" -an";
  }
  cmd += L" -movflags +faststart " + quoteWindowsArg(settings.outputPath.wstring());

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
    if (outError) *outError = "Failed to create FFmpeg stdin pipe";
    return false;
  }
  SetHandleInformation(writePipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = readPipe;
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION pi{};
  std::wstring mutableCmd = cmd;
  const BOOL ok = CreateProcessW(nullptr,
                                 mutableCmd.data(),
                                 nullptr,
                                 nullptr,
                                 TRUE,
                                 CREATE_NO_WINDOW,
                                 nullptr,
                                 nullptr,
                                 &si,
                                 &pi);
  CloseHandle(readPipe);

  if (!ok) {
    CloseHandle(writePipe);
    if (outError) *outError = "Failed to launch ffmpeg.exe for video export";
    return false;
  }

  impl_->stdinWritePipe = writePipe;
  impl_->processHandle = pi.hProcess;
  impl_->threadHandle = pi.hThread;
  impl_->active = true;
  return true;
}

bool VideoExportSession::stepFrame(iota::rhi::IRenderDevice& device,
                                   iota::engine::SceneRenderer& renderer,
                                   const iota::model::Project& project,
                                   const iota::audio::AudioEngine& audio,
                                   std::string* outError) {
  if (!impl_->active) {
    if (outError) *outError = "Export session is not active";
    return false;
  }
  if (impl_->frameCursor >= impl_->totalOutFrames) {
    return true;
  }

  const float fixedDt = 1.0f / static_cast<float>(impl_->cfg.fps);
  const double frameTimeSec = std::max(0.0, impl_->startSec + static_cast<double>(impl_->frameCursor) * fixedDt);

  iota::engine::FrameTiming timing;
  timing.dt = fixedDt;
  timing.totalTimeSec = std::max(0.0, static_cast<double>(impl_->frameCursor + impl_->cfg.preRollFrames) * fixedDt);
  timing.playAccumTimeSec = frameTimeSec;
  timing.isExporting = true;
  timing.visOffsetMs = impl_->cfg.visOffsetMs;
  timing.fxaaEnabled = impl_->cfg.fxaa;
  timing.fpsStat = static_cast<float>(impl_->cfg.fps);

  const auto rt = renderer.renderFrame(project,
                                       audio,
                                       timing,
                                       impl_->outWidth,
                                       impl_->outHeight);
  if (!rt) {
    if (outError) *outError = "SceneRenderer failed during export frame";
    return false;
  }

  if (impl_->frameCursor >= 0) {
    int rw = 0, rh = 0;
    if (!device.readRenderTargetRgba(rt, rw, rh, impl_->rgbaReadback, true) ||
        rw != impl_->outWidth || rh != impl_->outHeight) {
      if (outError) *outError = "GPU readback failed during video export";
      return false;
    }

    const size_t totalBytes = impl_->rgbaReadback.size();
    size_t writtenTotal = 0;
    while (writtenTotal < totalBytes) {
      const DWORD chunk = static_cast<DWORD>(std::min<size_t>(totalBytes - writtenTotal, 1024u * 1024u));
      DWORD written = 0;
      if (!WriteFile(impl_->stdinWritePipe,
                     impl_->rgbaReadback.data() + writtenTotal,
                     chunk,
                     &written,
                     nullptr) ||
          written == 0) {
        if (outError) *outError = "FFmpeg stdin pipe closed prematurely";
        return false;
      }
      writtenTotal += written;
    }
  }

  impl_->frameCursor += 1;
  return true;
}

bool VideoExportSession::finish(std::string* outError) {
  if (!impl_->active) {
    return false;
  }
  if (impl_->stdinWritePipe != nullptr) {
    CloseHandle(impl_->stdinWritePipe);
    impl_->stdinWritePipe = nullptr;
  }
  DWORD exitCode = 1;
  if (impl_->processHandle != nullptr) {
    WaitForSingleObject(impl_->processHandle, INFINITE);
    GetExitCodeProcess(impl_->processHandle, &exitCode);
  }
  impl_->cleanupProcess(false);
  if (exitCode != 0) {
    if (outError) *outError = "ffmpeg.exe returned non-zero exit code during export";
    return false;
  }
  return true;
}

void VideoExportSession::cancel() {
  if (impl_ && impl_->active) {
    impl_->cleanupProcess(true);
  }
}

bool VideoExportSession::isActive() const noexcept { return impl_->active; }
bool VideoExportSession::isDone() const noexcept { return impl_->frameCursor >= impl_->totalOutFrames; }
int VideoExportSession::encodedFrames() const noexcept { return std::max(0, impl_->frameCursor); }
int VideoExportSession::totalFrames() const noexcept { return impl_->totalOutFrames; }
int VideoExportSession::currentPreRollIndex() const noexcept {
  return impl_->frameCursor < 0 ? (impl_->frameCursor + impl_->cfg.preRollFrames) : impl_->cfg.preRollFrames;
}
int VideoExportSession::alignedWidth() const noexcept { return impl_->outWidth; }
int VideoExportSession::alignedHeight() const noexcept { return impl_->outHeight; }

bool exportVideoBlocking(const ExportSettings& settings,
                         iota::rhi::IRenderDevice& device,
                         iota::engine::SceneRenderer& renderer,
                         const iota::model::Project& project,
                         const iota::audio::AudioEngine& audio,
                         std::function<bool(int encoded, int total)> onProgress,
                         std::string* outError) {
  VideoExportSession session;
  if (!session.begin(settings, audio, outError)) {
    return false;
  }
  renderer.resetSimulation();
  while (!session.isDone()) {
    if (!session.stepFrame(device, renderer, project, audio, outError)) {
      session.cancel();
      return false;
    }
    if (onProgress && session.encodedFrames() > 0) {
      if (!onProgress(session.encodedFrames(), session.totalFrames())) {
        session.cancel();
        if (outError) *outError = "Export cancelled";
        return false;
      }
    }
  }
  return session.finish(outError);
}

}  // namespace iota::export_video
