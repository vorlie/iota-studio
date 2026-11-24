#include "iota/export/video_exporter.hpp"

#include "iota/core/utf8_path.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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

std::wstring widenAscii(std::string_view text) {
  std::wstring out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  return out;
}

int roundUp16(int v) noexcept {
  return ((v + 15) / 16) * 16;
}

int roundUp2(int v) noexcept {
  return (v + 1) & ~1;
}

std::string trimWhitespace(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return std::string(s);
}

const char* codecShortLabel(VideoCodec codec) noexcept {
  return (codec == VideoCodec::H265) ? "H.265 / HEVC" : "H.264 / AVC";
}

const char* backendShortLabel(VideoEncoderBackend backend) noexcept {
  switch (backend) {
    case VideoEncoderBackend::NvidiaNvenc:
      return "NVIDIA NVENC";
    case VideoEncoderBackend::AmdAmf:
      return "AMD AMF";
    case VideoEncoderBackend::IntelQsv:
      return "Intel Quick Sync";
    case VideoEncoderBackend::CpuSoftware:
      return "CPU Software";
    case VideoEncoderBackend::Auto:
    default:
      return "Auto";
  }
}

const char* pixelFormatForBackend(VideoEncoderBackend backend) noexcept {
  switch (backend) {
    case VideoEncoderBackend::NvidiaNvenc:
    case VideoEncoderBackend::AmdAmf:
    case VideoEncoderBackend::IntelQsv:
      return "nv12";
    case VideoEncoderBackend::CpuSoftware:
    case VideoEncoderBackend::Auto:
    default:
      return "yuv420p";
  }
}

std::wstring presetCliArgs(VideoEncoderBackend backend, EncoderPreset preset) {
  switch (backend) {
    case VideoEncoderBackend::NvidiaNvenc:
      switch (preset) {
        case EncoderPreset::Fast:
          return L"-preset p2";
        case EncoderPreset::Quality:
          return L"-preset p6";
        case EncoderPreset::Balanced:
        default:
          return L"-preset p4";
      }
    case VideoEncoderBackend::AmdAmf:
      switch (preset) {
        case EncoderPreset::Fast:
          return L"-quality speed";
        case EncoderPreset::Quality:
          return L"-quality quality";
        case EncoderPreset::Balanced:
        default:
          return L"-quality balanced";
      }
    case VideoEncoderBackend::IntelQsv:
    case VideoEncoderBackend::CpuSoftware:
    case VideoEncoderBackend::Auto:
    default:
      switch (preset) {
        case EncoderPreset::Fast:
          return L"-preset veryfast";
        case EncoderPreset::Quality:
          return L"-preset slow";
        case EncoderPreset::Balanced:
        default:
          return L"-preset medium";
      }
  }
}

void setCapFlag(HardwareEncoderCapabilities& caps,
                VideoEncoderBackend backend,
                VideoCodec codec,
                bool supported) noexcept {
  const bool h265 = (codec == VideoCodec::H265);
  switch (backend) {
    case VideoEncoderBackend::CpuSoftware:
      if (h265) caps.cpuH265 = supported;
      else caps.cpuH264 = supported;
      break;
    case VideoEncoderBackend::NvidiaNvenc:
      if (h265) caps.nvencH265 = supported;
      else caps.nvencH264 = supported;
      break;
    case VideoEncoderBackend::AmdAmf:
      if (h265) caps.amfH265 = supported;
      else caps.amfH264 = supported;
      break;
    case VideoEncoderBackend::IntelQsv:
      if (h265) caps.qsvH265 = supported;
      else caps.qsvH264 = supported;
      break;
    case VideoEncoderBackend::Auto:
      break;
  }
}

ResolvedEncoderPlan makeSuccessPlan(VideoEncoderBackend resolvedBackend,
                                    VideoCodec codec,
                                    bool usedCpuFallback = false,
                                    std::string fallbackReason = {}) {
  ResolvedEncoderPlan plan;
  plan.ok = true;
  plan.resolvedBackend = resolvedBackend;
  plan.codec = codec;
  plan.ffmpegEncoder = ffmpegEncoderName(resolvedBackend, codec);
  plan.pixelFormat = pixelFormatForBackend(resolvedBackend);
  plan.displayName = resolvedEncoderDisplayName(resolvedBackend, codec);
  plan.usedCpuFallback = usedCpuFallback;
  plan.fallbackReason = std::move(fallbackReason);
  return plan;
}

void drainPipeNonBlocking(HANDLE pipe, std::string& log, size_t maxKeepBytes) {
  if (pipe == nullptr) {
    return;
  }
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
      break;
    }
    char buf[1024];
    const DWORD toRead = std::min<DWORD>(avail, sizeof(buf));
    DWORD bytesRead = 0;
    if (!ReadFile(pipe, buf, toRead, &bytesRead, nullptr) || bytesRead == 0) {
      break;
    }
    if (log.size() < maxKeepBytes) {
      const size_t space = maxKeepBytes - log.size();
      log.append(buf, std::min(space, static_cast<size_t>(bytesRead)));
    }
  }
}

bool runBoundedCaptureCommand(const std::wstring& cmdLine,
                              DWORD timeoutMs,
                              const std::atomic<bool>* cancelFlag,
                              std::mutex* procMutex,
                              HANDLE* activeProcSlot,
                              DWORD& outExitCode,
                              std::string& outOutput) {
  outExitCode = 1;
  outOutput.clear();

  if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
    outOutput = "Probe cancelled.";
    return false;
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE nulIn = CreateFileW(L"NUL",
                             GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
    if (nulIn != INVALID_HANDLE_VALUE) {
      CloseHandle(nulIn);
    }
    outOutput = "Failed to create probe pipe.";
    return false;
  }
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nulIn;
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;

  PROCESS_INFORMATION pi{};
  std::wstring mutableCmd = cmdLine;
  const BOOL created = CreateProcessW(nullptr,
                                      mutableCmd.data(),
                                      nullptr,
                                      nullptr,
                                      TRUE,
                                      CREATE_NO_WINDOW,
                                      nullptr,
                                      nullptr,
                                      &si,
                                      &pi);

  CloseHandle(writePipe);
  if (nulIn != INVALID_HANDLE_VALUE) {
    CloseHandle(nulIn);
  }

  if (!created) {
    const DWORD launchErr = GetLastError();
    CloseHandle(readPipe);
    if (launchErr == ERROR_FILE_NOT_FOUND || launchErr == ERROR_PATH_NOT_FOUND) {
      outOutput = "ffmpeg.exe was not found in PATH or application folder.";
    } else {
      outOutput = "Failed to launch ffmpeg.exe (Win32 error " + std::to_string(launchErr) + ").";
    }
    return false;
  }

  if (procMutex && activeProcSlot) {
    std::lock_guard<std::mutex> lock(*procMutex);
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
      TerminateProcess(pi.hProcess, 1);
    } else {
      *activeProcSlot = pi.hProcess;
    }
  }

  const ULONGLONG startMs = GetTickCount64();
  bool finishedCleanly = false;
  for (;;) {
    drainPipeNonBlocking(readPipe, outOutput, 2048);

    const DWORD waitRes = WaitForSingleObject(pi.hProcess, 50);
    if (waitRes == WAIT_OBJECT_0) {
      finishedCleanly = true;
      break;
    }
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 500);
      outOutput = "Probe cancelled.";
      break;
    }
    if (GetTickCount64() - startMs >= timeoutMs) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 500);
      if (outOutput.empty()) {
        outOutput = "FFmpeg probe timed out.";
      }
      break;
    }
  }

  drainPipeNonBlocking(readPipe, outOutput, 2048);

  if (procMutex && activeProcSlot) {
    std::lock_guard<std::mutex> lock(*procMutex);
    *activeProcSlot = nullptr;
  }

  if (finishedCleanly) {
    GetExitCodeProcess(pi.hProcess, &outExitCode);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(readPipe);

  outOutput = trimWhitespace(outOutput);
  return finishedCleanly;
}

bool checkFfmpegExecutableAvailable(const std::atomic<bool>* cancelFlag,
                                    std::mutex* procMutex,
                                    HANDLE* activeProcSlot) {
  DWORD exitCode = 1;
  std::string out;
  const bool finished = runBoundedCaptureCommand(
      L"ffmpeg.exe -hide_banner -version",
      2500,
      cancelFlag,
      procMutex,
      activeProcSlot,
      exitCode,
      out);
  return finished && exitCode == 0;
}

bool probeEncoderWithFlags(VideoEncoderBackend backend,
                           VideoCodec codec,
                           const std::atomic<bool>* cancelFlag,
                           std::mutex* procMutex,
                           HANDLE* activeProcSlot,
                           std::string* outError) {
  if (backend == VideoEncoderBackend::Auto) {
    backend = VideoEncoderBackend::CpuSoftware;
  }
  const std::string enc = ffmpegEncoderName(backend, codec);
  const std::string pix = pixelFormatForBackend(backend);
  const std::wstring preset = presetCliArgs(backend, EncoderPreset::Fast);

  const std::wstring cmd =
      L"ffmpeg.exe -hide_banner -v error -f lavfi -i color=c=black:s=256x256:r=30:d=0.05 "
      L"-frames:v 1 -c:v " +
      widenAscii(enc) + L" " + preset + L" -pix_fmt " + widenAscii(pix) + L" -f null -";

  DWORD exitCode = 1;
  std::string out;
  const bool finished = runBoundedCaptureCommand(
      cmd,
      4000,
      cancelFlag,
      procMutex,
      activeProcSlot,
      exitCode,
      out);
  if (outError) {
    *outError = out;
  }
  return finished && exitCode == 0;
}

HardwareEncoderCapabilities probeMinimalCapabilitiesForSelection(const ExportSettings& settings) {
  HardwareEncoderCapabilities caps;
  caps.probed = true;
  caps.ffmpegAvailable = checkFfmpegExecutableAvailable(nullptr, nullptr, nullptr);
  if (!caps.ffmpegAvailable) {
    return caps;
  }

  if (settings.encoderBackend == VideoEncoderBackend::Auto) {
    static constexpr VideoEncoderBackend kGpuOrder[] = {
        VideoEncoderBackend::NvidiaNvenc,
        VideoEncoderBackend::AmdAmf,
        VideoEncoderBackend::IntelQsv,
    };
    bool foundGpu = false;
    for (VideoEncoderBackend gpu : kGpuOrder) {
      if (probeEncoderWithFlags(gpu, settings.codec, nullptr, nullptr, nullptr, nullptr)) {
        setCapFlag(caps, gpu, settings.codec, true);
        foundGpu = true;
        break;
      }
    }
    if (!foundGpu) {
      const bool cpuOk = probeEncoderWithFlags(
          VideoEncoderBackend::CpuSoftware, settings.codec, nullptr, nullptr, nullptr, nullptr);
      setCapFlag(caps, VideoEncoderBackend::CpuSoftware, settings.codec, cpuOk);
    }
  } else if (settings.encoderBackend == VideoEncoderBackend::CpuSoftware) {
    const bool cpuOk = probeEncoderWithFlags(
        VideoEncoderBackend::CpuSoftware, settings.codec, nullptr, nullptr, nullptr, nullptr);
    setCapFlag(caps, VideoEncoderBackend::CpuSoftware, settings.codec, cpuOk);
  } else {
    const bool gpuOk = probeEncoderWithFlags(
        settings.encoderBackend, settings.codec, nullptr, nullptr, nullptr, nullptr);
    setCapFlag(caps, settings.encoderBackend, settings.codec, gpuOk);
    if (!gpuOk && settings.fallbackToCpuOnError) {
      const bool cpuOk = probeEncoderWithFlags(
          VideoEncoderBackend::CpuSoftware, settings.codec, nullptr, nullptr, nullptr, nullptr);
      setCapFlag(caps, VideoEncoderBackend::CpuSoftware, settings.codec, cpuOk);
    }
  }

  return caps;
}

}  // namespace

const char* videoCodecDisplayName(VideoCodec codec) noexcept {
  switch (codec) {
    case VideoCodec::H265:
      return "H.265 / HEVC (Smaller Files)";
    case VideoCodec::H264:
    default:
      return "H.264 / AVC (Best Compatibility)";
  }
}

const char* videoEncoderBackendDisplayName(VideoEncoderBackend backend) noexcept {
  switch (backend) {
    case VideoEncoderBackend::CpuSoftware:
      return "CPU Software (x264 / x265)";
    case VideoEncoderBackend::NvidiaNvenc:
      return "GPU: NVIDIA NVENC";
    case VideoEncoderBackend::AmdAmf:
      return "GPU: AMD AMF";
    case VideoEncoderBackend::IntelQsv:
      return "GPU: Intel Quick Sync (QSV)";
    case VideoEncoderBackend::Auto:
    default:
      return "Auto (Use GPU Hardware if Available)";
  }
}

const char* encoderPresetDisplayName(EncoderPreset preset) noexcept {
  switch (preset) {
    case EncoderPreset::Fast:
      return "Fast (Fastest Render)";
    case EncoderPreset::Quality:
      return "Quality (Higher Detail, Slower)";
    case EncoderPreset::Balanced:
    default:
      return "Balanced (Recommended)";
  }
}

const char* ffmpegEncoderName(VideoEncoderBackend backend, VideoCodec codec) noexcept {
  const bool hevc = (codec == VideoCodec::H265);
  switch (backend) {
    case VideoEncoderBackend::NvidiaNvenc:
      return hevc ? "hevc_nvenc" : "h264_nvenc";
    case VideoEncoderBackend::AmdAmf:
      return hevc ? "hevc_amf" : "h264_amf";
    case VideoEncoderBackend::IntelQsv:
      return hevc ? "hevc_qsv" : "h264_qsv";
    case VideoEncoderBackend::CpuSoftware:
    case VideoEncoderBackend::Auto:
    default:
      return hevc ? "libx265" : "libx264";
  }
}

std::string resolvedEncoderDisplayName(VideoEncoderBackend backend, VideoCodec codec) {
  switch (backend) {
    case VideoEncoderBackend::NvidiaNvenc:
      return std::string("NVIDIA NVENC (") + codecShortLabel(codec) + ")";
    case VideoEncoderBackend::AmdAmf:
      return std::string("AMD AMF (") + codecShortLabel(codec) + ")";
    case VideoEncoderBackend::IntelQsv:
      return std::string("Intel Quick Sync (") + codecShortLabel(codec) + ")";
    case VideoEncoderBackend::CpuSoftware:
      return std::string("CPU Software (") + ffmpegEncoderName(VideoEncoderBackend::CpuSoftware, codec) + ")";
    case VideoEncoderBackend::Auto:
    default:
      return std::string("Auto (") + codecShortLabel(codec) + ")";
  }
}

bool HardwareEncoderCapabilities::supports(VideoEncoderBackend backend, VideoCodec codec) const noexcept {
  const bool h265 = (codec == VideoCodec::H265);
  switch (backend) {
    case VideoEncoderBackend::Auto:
      return supports(VideoEncoderBackend::NvidiaNvenc, codec) ||
             supports(VideoEncoderBackend::AmdAmf, codec) ||
             supports(VideoEncoderBackend::IntelQsv, codec) ||
             supports(VideoEncoderBackend::CpuSoftware, codec);
    case VideoEncoderBackend::CpuSoftware:
      return h265 ? cpuH265 : cpuH264;
    case VideoEncoderBackend::NvidiaNvenc:
      return h265 ? nvencH265 : nvencH264;
    case VideoEncoderBackend::AmdAmf:
      return h265 ? amfH265 : amfH264;
    case VideoEncoderBackend::IntelQsv:
      return h265 ? qsvH265 : qsvH264;
    default:
      return false;
  }
}

bool HardwareEncoderCapabilities::hasAnyHardware() const noexcept {
  return nvencH264 || nvencH265 || amfH264 || amfH265 || qsvH264 || qsvH265;
}

std::string HardwareEncoderCapabilities::summaryText() const {
  if (!probed) {
    return "Hardware encoders not yet probed.";
  }
  if (!ffmpegAvailable) {
    return "FFmpeg executable (ffmpeg.exe) not found.";
  }

  auto formatPair = [](const char* name, bool h264, bool h265) -> std::string {
    if (!h264 && !h265) return {};
    std::string out = name;
    out += " (";
    if (h264 && h265) {
      out += "H.264, H.265";
    } else if (h264) {
      out += "H.264";
    } else {
      out += "H.265";
    }
    out += ")";
    return out;
  };

  std::vector<std::string> gpus;
  if (auto s = formatPair("NVIDIA NVENC", nvencH264, nvencH265); !s.empty()) gpus.push_back(std::move(s));
  if (auto s = formatPair("AMD AMF", amfH264, amfH265); !s.empty()) gpus.push_back(std::move(s));
  if (auto s = formatPair("Intel QSV", qsvH264, qsvH265); !s.empty()) gpus.push_back(std::move(s));

  if (gpus.empty()) {
    return "No GPU encoder detected — CPU Software available";
  }

  std::string msg = "Detected GPU: ";
  for (size_t i = 0; i < gpus.size(); ++i) {
    if (i > 0) msg += ", ";
    msg += gpus[i];
  }
  return msg;
}

ResolvedEncoderPlan resolveEncoderSelection(const ExportSettings& settings,
                                          const HardwareEncoderCapabilities& caps) {
  ResolvedEncoderPlan plan;
  plan.codec = settings.codec;

  if (!caps.ffmpegAvailable) {
    plan.errorMessage =
        "ffmpeg.exe was not found in PATH or the application folder. Please install FFmpeg to export MP4 videos.";
    return plan;
  }

  if (settings.encoderBackend == VideoEncoderBackend::CpuSoftware) {
    if (caps.supports(VideoEncoderBackend::CpuSoftware, settings.codec)) {
      return makeSuccessPlan(VideoEncoderBackend::CpuSoftware, settings.codec);
    }
    plan.errorMessage = std::string(resolvedEncoderDisplayName(VideoEncoderBackend::CpuSoftware, settings.codec)) +
                        " is unavailable in this FFmpeg build.";
    return plan;
  }

  if (settings.encoderBackend == VideoEncoderBackend::Auto) {
    static constexpr VideoEncoderBackend kGpuOrder[] = {
        VideoEncoderBackend::NvidiaNvenc,
        VideoEncoderBackend::AmdAmf,
        VideoEncoderBackend::IntelQsv,
    };
    for (VideoEncoderBackend gpu : kGpuOrder) {
      if (caps.supports(gpu, settings.codec)) {
        return makeSuccessPlan(gpu, settings.codec);
      }
    }
    if (caps.supports(VideoEncoderBackend::CpuSoftware, settings.codec)) {
      return makeSuccessPlan(VideoEncoderBackend::CpuSoftware, settings.codec, false);
    }
    plan.errorMessage = std::string("No working ") + codecShortLabel(settings.codec) +
                        " encoder is available in FFmpeg.";
    return plan;
  }

  // Explicit hardware backend selected
  if (caps.supports(settings.encoderBackend, settings.codec)) {
    return makeSuccessPlan(settings.encoderBackend, settings.codec);
  }

  if (settings.fallbackToCpuOnError && caps.supports(VideoEncoderBackend::CpuSoftware, settings.codec)) {
    std::string reason = std::string(backendShortLabel(settings.encoderBackend)) + " (" +
                         codecShortLabel(settings.codec) +
                         ") is unavailable on this GPU/driver — fell back to CPU Software (" +
                         ffmpegEncoderName(VideoEncoderBackend::CpuSoftware, settings.codec) + ").";
    return makeSuccessPlan(VideoEncoderBackend::CpuSoftware, settings.codec, true, std::move(reason));
  }

  plan.errorMessage = std::string(backendShortLabel(settings.encoderBackend)) + " (" +
                      codecShortLabel(settings.codec) +
                      ") is unavailable on this system. Enable CPU fallback or choose another encoder.";
  return plan;
}

std::wstring buildFfmpegExportCommand(const ExportSettings& settings,
                                      const ResolvedEncoderPlan& plan,
                                      int outWidth,
                                      int outHeight,
                                      double startSec,
                                      double exportDur,
                                      const std::filesystem::path& audioDiskPath,
                                      bool hasDiskAudio) {
  const int safeFps = std::clamp(settings.fps, 1, 120);
  const int safeBitrateKbps = std::clamp(settings.videoBitrateKbps, 500, 100000);
  const VideoEncoderBackend effectiveBackend =
      (plan.resolvedBackend == VideoEncoderBackend::Auto) ? VideoEncoderBackend::CpuSoftware : plan.resolvedBackend;
  const std::string enc =
      plan.ffmpegEncoder.empty() ? ffmpegEncoderName(effectiveBackend, plan.codec) : plan.ffmpegEncoder;
  const std::string pix =
      plan.pixelFormat.empty() ? pixelFormatForBackend(effectiveBackend) : plan.pixelFormat;

  std::wstring cmd =
      L"ffmpeg.exe -y -hide_banner -v error -f rawvideo -pix_fmt rgba -s " +
      std::to_wstring(outWidth) + L"x" + std::to_wstring(outHeight) +
      L" -r " + std::to_wstring(safeFps) + L" -i pipe:0";

  if (hasDiskAudio) {
    wchar_t ssBuf[32];
    wchar_t durBuf[32];
    std::swprintf(ssBuf, 32, L"%.3f", std::max(0.0, startSec));
    std::swprintf(durBuf, 32, L"%.3f", std::max(0.05, exportDur));
    cmd += L" -ss " + std::wstring(ssBuf) + L" -t " + std::wstring(durBuf) +
           L" -i " + quoteWindowsArg(audioDiskPath.wstring());
  }

  cmd += L" -c:v " + widenAscii(enc);
  cmd += L" " + presetCliArgs(effectiveBackend, settings.encoderPreset);
  cmd += L" -pix_fmt " + widenAscii(pix);
  cmd += L" -b:v " + std::to_wstring(safeBitrateKbps) + L"k";

  if (hasDiskAudio) {
    cmd += L" -c:a aac -b:a 192k -shortest";
  } else {
    cmd += L" -an";
  }

  if (plan.codec == VideoCodec::H265) {
    cmd += L" -tag:v hvc1";
  }

  cmd += L" -movflags +faststart " + quoteWindowsArg(settings.outputPath.wstring());
  return cmd;
}

bool probeSingleFfmpegEncoder(VideoEncoderBackend backend,
                              VideoCodec codec,
                              std::string* outError) {
  return probeEncoderWithFlags(backend, codec, nullptr, nullptr, nullptr, outError);
}

HardwareEncoderCapabilities probeHardwareEncodersSync() {
  HardwareEncoderCapabilities caps;
  caps.probed = true;
  caps.ffmpegAvailable = checkFfmpegExecutableAvailable(nullptr, nullptr, nullptr);
  if (!caps.ffmpegAvailable) {
    return caps;
  }

  caps.nvencH264 = probeEncoderWithFlags(VideoEncoderBackend::NvidiaNvenc, VideoCodec::H264, nullptr, nullptr, nullptr, nullptr);
  caps.nvencH265 = probeEncoderWithFlags(VideoEncoderBackend::NvidiaNvenc, VideoCodec::H265, nullptr, nullptr, nullptr, nullptr);
  caps.amfH264 = probeEncoderWithFlags(VideoEncoderBackend::AmdAmf, VideoCodec::H264, nullptr, nullptr, nullptr, nullptr);
  caps.amfH265 = probeEncoderWithFlags(VideoEncoderBackend::AmdAmf, VideoCodec::H265, nullptr, nullptr, nullptr, nullptr);
  caps.qsvH264 = probeEncoderWithFlags(VideoEncoderBackend::IntelQsv, VideoCodec::H264, nullptr, nullptr, nullptr, nullptr);
  caps.qsvH265 = probeEncoderWithFlags(VideoEncoderBackend::IntelQsv, VideoCodec::H265, nullptr, nullptr, nullptr, nullptr);
  caps.cpuH264 = probeEncoderWithFlags(VideoEncoderBackend::CpuSoftware, VideoCodec::H264, nullptr, nullptr, nullptr, nullptr);
  caps.cpuH265 = probeEncoderWithFlags(VideoEncoderBackend::CpuSoftware, VideoCodec::H265, nullptr, nullptr, nullptr, nullptr);
  return caps;
}

struct HardwareEncoderProbe::Impl {
  mutable std::mutex mutex;
  HardwareEncoderCapabilities caps;

  std::mutex procMutex;
  HANDLE activeProc = nullptr;

  std::atomic<bool> probing{false};
  std::atomic<bool> cancelRequested{false};
  std::thread worker;

  void runWorker() {
    HardwareEncoderCapabilities result;
    result.probed = true;
    result.ffmpegAvailable = checkFfmpegExecutableAvailable(&cancelRequested, &procMutex, &activeProc);

    struct ProbeStep {
      VideoEncoderBackend backend;
      VideoCodec codec;
    };
    static constexpr ProbeStep kSteps[] = {
        {VideoEncoderBackend::NvidiaNvenc, VideoCodec::H264},
        {VideoEncoderBackend::NvidiaNvenc, VideoCodec::H265},
        {VideoEncoderBackend::AmdAmf, VideoCodec::H264},
        {VideoEncoderBackend::AmdAmf, VideoCodec::H265},
        {VideoEncoderBackend::IntelQsv, VideoCodec::H264},
        {VideoEncoderBackend::IntelQsv, VideoCodec::H265},
        {VideoEncoderBackend::CpuSoftware, VideoCodec::H264},
        {VideoEncoderBackend::CpuSoftware, VideoCodec::H265},
    };

    if (result.ffmpegAvailable) {
      for (const auto& step : kSteps) {
        if (cancelRequested.load(std::memory_order_acquire)) {
          break;
        }
        const bool ok = probeEncoderWithFlags(
            step.backend, step.codec, &cancelRequested, &procMutex, &activeProc, nullptr);
        setCapFlag(result, step.backend, step.codec, ok);
      }
    }

    if (!cancelRequested.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(mutex);
      caps = std::move(result);
    }
    probing.store(false, std::memory_order_release);
  }
};

HardwareEncoderProbe::HardwareEncoderProbe() : impl_(std::make_unique<Impl>()) {}

HardwareEncoderProbe::~HardwareEncoderProbe() {
  cancel();
}

void HardwareEncoderProbe::startAsync() {
  if (impl_->probing.load(std::memory_order_acquire)) {
    return;
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->cancelRequested.store(false, std::memory_order_release);
  impl_->probing.store(true, std::memory_order_release);
  impl_->worker = std::thread([this]() { impl_->runWorker(); });
}

void HardwareEncoderProbe::cancel() {
  if (!impl_) {
    return;
  }
  impl_->cancelRequested.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(impl_->procMutex);
    if (impl_->activeProc != nullptr) {
      TerminateProcess(impl_->activeProc, 1);
    }
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->probing.store(false, std::memory_order_release);
}

bool HardwareEncoderProbe::isProbing() const noexcept {
  return impl_->probing.load(std::memory_order_acquire);
}

HardwareEncoderCapabilities HardwareEncoderProbe::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->caps;
}

struct VideoExportSession::Impl {
  ExportSettings cfg;
  ResolvedEncoderPlan activePlan;
  int outWidth = 1280;
  int outHeight = 720;
  double startSec = 0.0;
  double endSec = 10.0;
  int totalOutFrames = 0;
  int frameCursor = 0;  // Runs from -preRollFrames to totalOutFrames - 1

  bool active = false;
  HANDLE stdinWritePipe = nullptr;
  HANDLE stderrReadPipe = nullptr;
  HANDLE processHandle = nullptr;
  HANDLE threadHandle = nullptr;
  std::string stderrLog;
  std::vector<uint8_t> rgbaReadback;

  void drainStderr() {
    drainPipeNonBlocking(stderrReadPipe, stderrLog, 4096);
  }

  void cleanupProcess(bool terminate) {
    if (stdinWritePipe != nullptr) {
      CloseHandle(stdinWritePipe);
      stdinWritePipe = nullptr;
    }
    if (processHandle != nullptr) {
      if (terminate) {
        TerminateProcess(processHandle, 1);
      }
      const ULONGLONG waitStart = GetTickCount64();
      for (;;) {
        drainStderr();
        const DWORD res = WaitForSingleObject(processHandle, 50);
        if (res == WAIT_OBJECT_0) {
          break;
        }
        if (terminate && GetTickCount64() - waitStart >= 2000) {
          break;
        }
      }
      drainStderr();
      CloseHandle(processHandle);
      processHandle = nullptr;
    }
    if (stderrReadPipe != nullptr) {
      CloseHandle(stderrReadPipe);
      stderrReadPipe = nullptr;
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
                               std::string* outError,
                               const HardwareEncoderCapabilities* cachedCaps) {
  cancel();

  if (settings.outputPath.empty()) {
    if (outError) *outError = "Output MP4 path cannot be empty.";
    return false;
  }

  HardwareEncoderCapabilities resolvedCaps;
  if (cachedCaps != nullptr && cachedCaps->probed) {
    resolvedCaps = *cachedCaps;
  } else {
    resolvedCaps = probeMinimalCapabilitiesForSelection(settings);
  }

  const ResolvedEncoderPlan plan = resolveEncoderSelection(settings, resolvedCaps);
  if (!plan.ok) {
    if (outError) *outError = plan.errorMessage;
    return false;
  }

  impl_->cfg = settings;
  impl_->activePlan = plan;
  impl_->stderrLog.clear();

  const int rawW = settings.align16 ? roundUp16(settings.width) : roundUp2(settings.width);
  const int rawH = settings.align16 ? roundUp16(settings.height) : roundUp2(settings.height);
  impl_->outWidth = std::clamp(rawW, 16, 8192);
  impl_->outHeight = std::clamp(rawH, 16, 8192);
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

  const std::string audioPathStr = audio.currentFilePath();
  const std::filesystem::path audioDiskPath = iota::core::pathFromUtf8(audioPathStr);
  const bool hasDiskAudio = audio.hasTrack() && !audioPathStr.empty() &&
                            iota::core::pathExistsNoThrow(audioDiskPath);

  const std::wstring cmd = buildFfmpegExportCommand(impl_->cfg,
                                                    plan,
                                                    impl_->outWidth,
                                                    impl_->outHeight,
                                                    impl_->startSec,
                                                    exportDur,
                                                    audioDiskPath,
                                                    hasDiskAudio);

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE stdinReadPipe = nullptr;
  HANDLE stdinWritePipe = nullptr;
  if (!CreatePipe(&stdinReadPipe, &stdinWritePipe, &sa, 0)) {
    if (outError) *outError = "Failed to create FFmpeg stdin pipe.";
    return false;
  }
  SetHandleInformation(stdinWritePipe, HANDLE_FLAG_INHERIT, 0);

  HANDLE stderrReadPipe = nullptr;
  HANDLE stderrWritePipe = nullptr;
  if (!CreatePipe(&stderrReadPipe, &stderrWritePipe, &sa, 0)) {
    CloseHandle(stdinReadPipe);
    CloseHandle(stdinWritePipe);
    if (outError) *outError = "Failed to create FFmpeg stderr pipe.";
    return false;
  }
  SetHandleInformation(stderrReadPipe, HANDLE_FLAG_INHERIT, 0);

  HANDLE nulOut = CreateFileW(L"NUL",
                              GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = stdinReadPipe;
  si.hStdOutput = nulOut;
  si.hStdError = stderrWritePipe;

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

  CloseHandle(stdinReadPipe);
  CloseHandle(stderrWritePipe);
  if (nulOut != INVALID_HANDLE_VALUE) {
    CloseHandle(nulOut);
  }

  if (!ok) {
    const DWORD errCode = GetLastError();
    CloseHandle(stdinWritePipe);
    CloseHandle(stderrReadPipe);
    if (outError) {
      if (errCode == ERROR_FILE_NOT_FOUND || errCode == ERROR_PATH_NOT_FOUND) {
        *outError = "Failed to launch ffmpeg.exe (executable not found).";
      } else {
        *outError = "Failed to launch ffmpeg.exe for video export (error " + std::to_string(errCode) + ").";
      }
    }
    return false;
  }

  impl_->stdinWritePipe = stdinWritePipe;
  impl_->stderrReadPipe = stderrReadPipe;
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
    if (outError) *outError = "Export session is not active.";
    return false;
  }
  if (impl_->frameCursor >= impl_->totalOutFrames) {
    return true;
  }

  impl_->drainStderr();

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
    if (outError) *outError = "SceneRenderer failed during export frame.";
    return false;
  }

  if (impl_->frameCursor >= 0) {
    int rw = 0, rh = 0;
    if (!device.readRenderTargetRgba(rt, rw, rh, impl_->rgbaReadback, true) ||
        rw != impl_->outWidth || rh != impl_->outHeight) {
      if (outError) *outError = "GPU readback failed during video export.";
      return false;
    }

    const size_t totalBytes = impl_->rgbaReadback.size();
    size_t writtenTotal = 0;
    while (writtenTotal < totalBytes) {
      impl_->drainStderr();
      const DWORD chunk = static_cast<DWORD>(std::min<size_t>(totalBytes - writtenTotal, 1024u * 1024u));
      DWORD written = 0;
      if (!WriteFile(impl_->stdinWritePipe,
                     impl_->rgbaReadback.data() + writtenTotal,
                     chunk,
                     &written,
                     nullptr) ||
          written == 0) {
        if (impl_->processHandle != nullptr) {
          WaitForSingleObject(impl_->processHandle, 1000);
        }
        impl_->drainStderr();
        const std::string detail = trimWhitespace(impl_->stderrLog);
        impl_->cleanupProcess(true);
        if (outError) {
          *outError = detail.empty()
                          ? ("FFmpeg encoder (" + impl_->activePlan.displayName + ") exited prematurely.")
                          : ("FFmpeg encoder (" + impl_->activePlan.displayName + ") failed: " + detail);
        }
        return false;
      }
      writtenTotal += written;
    }
  }

  impl_->drainStderr();
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
    for (;;) {
      impl_->drainStderr();
      const DWORD res = WaitForSingleObject(impl_->processHandle, 50);
      if (res == WAIT_OBJECT_0) {
        break;
      }
    }
    impl_->drainStderr();
    GetExitCodeProcess(impl_->processHandle, &exitCode);
  }
  const std::string detail = trimWhitespace(impl_->stderrLog);
  impl_->cleanupProcess(false);
  if (exitCode != 0) {
    if (outError) {
      *outError = detail.empty()
                      ? ("ffmpeg.exe returned exit code " + std::to_string(exitCode) + " during export.")
                      : ("FFmpeg encoder failed: " + detail);
    }
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

VideoEncoderBackend VideoExportSession::activeBackend() const noexcept {
  return impl_->activePlan.resolvedBackend;
}

std::string VideoExportSession::activeEncoderDisplayName() const {
  return impl_->activePlan.displayName;
}

std::string VideoExportSession::activeFfmpegEncoder() const {
  return impl_->activePlan.ffmpegEncoder;
}

bool VideoExportSession::usedCpuFallback() const noexcept {
  return impl_->activePlan.usedCpuFallback;
}

std::string VideoExportSession::fallbackNotice() const {
  return impl_->activePlan.fallbackReason;
}

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
