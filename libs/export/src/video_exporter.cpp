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

#include "iota/export/video_exporter.hpp"

#include "iota/core/utf8_path.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace iota::export_video {

namespace {

#ifdef _WIN32
constexpr const char* kFfmpegExeName = "ffmpeg.exe";
#else
constexpr const char* kFfmpegExeName = "ffmpeg";

void ensureSigpipeIgnored() {
  static std::once_flag flag;
  std::call_once(flag, []() { std::signal(SIGPIPE, SIG_IGN); });
}
#endif

std::string formatFixed3(double value) {
  char buf[64]{};
  const auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, 3);
  if (res.ec == std::errc{}) {
    return std::string(buf, res.ptr);
  }
  return "0.000";
}

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

std::wstring utf8ToWstringSimple(std::string_view text) {
  return iota::core::pathFromUtf8(text).wstring();
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
    case VideoEncoderBackend::LinuxVaApi:
      return "Linux VA-API";
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
    case VideoEncoderBackend::LinuxVaApi:
    case VideoEncoderBackend::AmdAmf:
    case VideoEncoderBackend::IntelQsv:
      return "nv12";
    case VideoEncoderBackend::CpuSoftware:
    case VideoEncoderBackend::Auto:
    default:
      return "yuv420p";
  }
}

std::string detectVaapiDevicePath() {
#ifdef _WIN32
  return {};
#else
  for (int idx = 128; idx <= 135; ++idx) {
    std::string candidate = "/dev/dri/renderD" + std::to_string(idx);
    if (::access(candidate.c_str(), R_OK | W_OK) == 0) {
      return candidate;
    }
  }
  return {};
#endif
}

std::pair<const char*, const char*> presetCliArgPair(VideoEncoderBackend backend, EncoderPreset preset) noexcept {
  switch (backend) {
    case VideoEncoderBackend::LinuxVaApi:
      return {"", ""};
    case VideoEncoderBackend::NvidiaNvenc:
      switch (preset) {
        case EncoderPreset::Fast:
          return {"-preset", "p2"};
        case EncoderPreset::Quality:
          return {"-preset", "p6"};
        case EncoderPreset::Balanced:
        default:
          return {"-preset", "p4"};
      }
    case VideoEncoderBackend::AmdAmf:
      switch (preset) {
        case EncoderPreset::Fast:
          return {"-quality", "speed"};
        case EncoderPreset::Quality:
          return {"-quality", "quality"};
        case EncoderPreset::Balanced:
        default:
          return {"-quality", "balanced"};
      }
    case VideoEncoderBackend::IntelQsv:
    case VideoEncoderBackend::CpuSoftware:
    case VideoEncoderBackend::Auto:
    default:
      switch (preset) {
        case EncoderPreset::Fast:
          return {"-preset", "veryfast"};
        case EncoderPreset::Quality:
          return {"-preset", "slow"};
        case EncoderPreset::Balanced:
        default:
          return {"-preset", "medium"};
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
    case VideoEncoderBackend::LinuxVaApi:
      if (h265) caps.vaapiH265 = supported;
      else caps.vaapiH264 = supported;
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

#ifdef _WIN32
using NativeProcHandle = HANDLE;
constexpr NativeProcHandle kInvalidNativeProc = nullptr;

void terminateNativeProc(NativeProcHandle proc) noexcept {
  if (proc != nullptr) {
    TerminateProcess(proc, 1);
  }
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

std::wstring joinWindowsArgs(const std::vector<std::string>& args) {
  std::wstring cmd;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) cmd.push_back(L' ');
    cmd += quoteWindowsArg(utf8ToWstringSimple(args[i]));
  }
  return cmd;
}

bool runBoundedCaptureCommand(const std::vector<std::string>& args,
                              uint32_t timeoutMs,
                              const std::atomic<bool>* cancelFlag,
                              std::mutex* procMutex,
                              NativeProcHandle* activeProcSlot,
                              uint32_t& outExitCode,
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
  std::wstring mutableCmd = joinWindowsArgs(args);
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
      outOutput = std::string(kFfmpegExeName) + " was not found in PATH or application folder.";
    } else {
      outOutput = std::string("Failed to launch ") + kFfmpegExeName +
                  " (Win32 error " + std::to_string(launchErr) + ").";
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
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    outExitCode = code;
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(readPipe);

  outOutput = trimWhitespace(outOutput);
  return finishedCleanly;
}
#else
using NativeProcHandle = pid_t;
constexpr NativeProcHandle kInvalidNativeProc = -1;

void terminateNativeProc(NativeProcHandle proc) noexcept {
  if (proc > 0) {
    kill(proc, SIGKILL);
  }
}

void setFdFlags(int fd, int extraFlags) noexcept {
  const int cur = fcntl(fd, F_GETFL, 0);
  if (cur >= 0) {
    fcntl(fd, F_SETFL, cur | extraFlags);
  }
}

void drainPipeNonBlocking(int fd, std::string& log, size_t maxKeepBytes) {
  if (fd < 0) {
    return;
  }
  char buf[1024];
  for (;;) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      if (log.size() < maxKeepBytes) {
        const size_t space = maxKeepBytes - log.size();
        log.append(buf, std::min(space, static_cast<size_t>(n)));
      }
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
}

bool runBoundedCaptureCommand(const std::vector<std::string>& args,
                              uint32_t timeoutMs,
                              const std::atomic<bool>* cancelFlag,
                              std::mutex* procMutex,
                              NativeProcHandle* activeProcSlot,
                              uint32_t& outExitCode,
                              std::string& outOutput) {
  ensureSigpipeIgnored();
  outExitCode = 1;
  outOutput.clear();

  if (args.empty()) {
    return false;
  }
  if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
    outOutput = "Probe cancelled.";
    return false;
  }

  int pipefd[2]{-1, -1};
  if (pipe(pipefd) != 0) {
    outOutput = "Failed to create probe pipe.";
    return false;
  }
  fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
  fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
  setFdFlags(pipefd[0], O_NONBLOCK);

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    outOutput = std::string("Failed to launch ") + kFfmpegExeName + ".";
    return false;
  }

  if (pid == 0) {
    close(pipefd[0]);
    const int devNull = open("/dev/null", O_RDONLY);
    if (devNull >= 0) {
      dup2(devNull, STDIN_FILENO);
      if (devNull > STDERR_FILENO) close(devNull);
    }
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& s : args) {
      argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }

  close(pipefd[1]);

  if (procMutex && activeProcSlot) {
    std::lock_guard<std::mutex> lock(*procMutex);
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
      kill(pid, SIGKILL);
    } else {
      *activeProcSlot = pid;
    }
  }

  const auto startTime = std::chrono::steady_clock::now();
  bool finishedCleanly = false;
  int status = 0;
  for (;;) {
    drainPipeNonBlocking(pipefd[0], outOutput, 2048);

    const pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      finishedCleanly = true;
      break;
    }
    if (w < 0 && errno != EINTR) {
      break;
    }
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      outOutput = "Probe cancelled.";
      break;
    }
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
    if (elapsedMs >= static_cast<long long>(timeoutMs)) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      if (outOutput.empty()) {
        outOutput = "FFmpeg probe timed out.";
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  drainPipeNonBlocking(pipefd[0], outOutput, 2048);
  close(pipefd[0]);

  if (procMutex && activeProcSlot) {
    std::lock_guard<std::mutex> lock(*procMutex);
    *activeProcSlot = kInvalidNativeProc;
  }

  if (finishedCleanly) {
    outExitCode = WIFEXITED(status) ? static_cast<uint32_t>(WEXITSTATUS(status)) : 1u;
    if (outExitCode == 127 && outOutput.empty()) {
      outOutput = std::string(kFfmpegExeName) + " was not found in PATH.";
      return false;
    }
  }

  outOutput = trimWhitespace(outOutput);
  return finishedCleanly;
}
#endif

bool checkFfmpegExecutableAvailable(const std::atomic<bool>* cancelFlag,
                                    std::mutex* procMutex,
                                    NativeProcHandle* activeProcSlot) {
  uint32_t exitCode = 1;
  std::string out;
  const bool finished = runBoundedCaptureCommand(
      {kFfmpegExeName, "-hide_banner", "-version"},
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
                           NativeProcHandle* activeProcSlot,
                           std::string* outError) {
  if (backend == VideoEncoderBackend::Auto) {
    backend = VideoEncoderBackend::CpuSoftware;
  }
  const std::string enc = ffmpegEncoderName(backend, codec);
  const std::string pix = pixelFormatForBackend(backend);
  const auto presetPair = presetCliArgPair(backend, EncoderPreset::Fast);

  std::vector<std::string> args;
  if (backend == VideoEncoderBackend::LinuxVaApi) {
#ifdef _WIN32
    if (outError) {
      *outError = "Linux VA-API is only available on Linux.";
    }
    return false;
#else
    const std::string devPath = detectVaapiDevicePath();
    if (devPath.empty()) {
      if (outError) {
        *outError = "No accessible /dev/dri/renderD* VA-API device found.";
      }
      return false;
    }
    args = {
        kFfmpegExeName,
        "-hide_banner",
        "-v",
        "error",
        "-vaapi_device",
        devPath,
        "-f",
        "lavfi",
        "-i",
        "color=c=black:s=256x256:r=30:d=0.05",
        "-frames:v",
        "1",
        "-vf",
        "format=nv12,hwupload",
        "-c:v",
        enc,
        "-f",
        "null",
        "-",
    };
#endif
  } else {
    args = {
        kFfmpegExeName,
        "-hide_banner",
        "-v",
        "error",
        "-f",
        "lavfi",
        "-i",
        "color=c=black:s=256x256:r=30:d=0.05",
        "-frames:v",
        "1",
        "-c:v",
        enc,
    };
    if (presetPair.first[0] != '\0') {
      args.push_back(presetPair.first);
      args.push_back(presetPair.second);
    }
    args.push_back("-pix_fmt");
    args.push_back(pix);
    args.push_back("-f");
    args.push_back("null");
    args.push_back("-");
  }

  uint32_t exitCode = 1;
  std::string out;
  const bool finished = runBoundedCaptureCommand(
      args,
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
#ifndef _WIN32
        VideoEncoderBackend::LinuxVaApi,
#endif
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
    case VideoEncoderBackend::LinuxVaApi:
      return "GPU: Linux VA-API";
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
    case VideoEncoderBackend::LinuxVaApi:
      return hevc ? "hevc_vaapi" : "h264_vaapi";
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
    case VideoEncoderBackend::LinuxVaApi:
      return std::string("Linux VA-API (") + codecShortLabel(codec) + ")";
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
             supports(VideoEncoderBackend::LinuxVaApi, codec) ||
             supports(VideoEncoderBackend::AmdAmf, codec) ||
             supports(VideoEncoderBackend::IntelQsv, codec) ||
             supports(VideoEncoderBackend::CpuSoftware, codec);
    case VideoEncoderBackend::CpuSoftware:
      return h265 ? cpuH265 : cpuH264;
    case VideoEncoderBackend::NvidiaNvenc:
      return h265 ? nvencH265 : nvencH264;
    case VideoEncoderBackend::LinuxVaApi:
      return h265 ? vaapiH265 : vaapiH264;
    case VideoEncoderBackend::AmdAmf:
      return h265 ? amfH265 : amfH264;
    case VideoEncoderBackend::IntelQsv:
      return h265 ? qsvH265 : qsvH264;
    default:
      return false;
  }
}

bool HardwareEncoderCapabilities::hasAnyHardware() const noexcept {
  return nvencH264 || nvencH265 || vaapiH264 || vaapiH265 || amfH264 || amfH265 || qsvH264 || qsvH265;
}

std::string HardwareEncoderCapabilities::summaryText() const {
  if (!probed) {
    return "Hardware encoders not yet probed.";
  }
  if (!ffmpegAvailable) {
    return std::string("FFmpeg executable (") + kFfmpegExeName + ") not found.";
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
  if (auto s = formatPair("Linux VA-API", vaapiH264, vaapiH265); !s.empty()) gpus.push_back(std::move(s));
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
    plan.errorMessage = std::string(kFfmpegExeName) +
                        " was not found in PATH or the application folder. Please install FFmpeg to export MP4 videos.";
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
#ifndef _WIN32
        VideoEncoderBackend::LinuxVaApi,
#endif
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

std::vector<std::string> buildFfmpegExportArgs(const ExportSettings& settings,
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
  const auto presetPair = presetCliArgPair(effectiveBackend, settings.encoderPreset);

  std::vector<std::string> args = {
      kFfmpegExeName,
      "-y",
      "-hide_banner",
      "-v",
      "error",
  };

  if (effectiveBackend == VideoEncoderBackend::LinuxVaApi) {
    std::string devPath = detectVaapiDevicePath();
    if (devPath.empty()) {
      devPath = "/dev/dri/renderD128";
    }
    args.push_back("-vaapi_device");
    args.push_back(std::move(devPath));
  }

  args.insert(args.end(), {
      "-f",
      "rawvideo",
      "-pix_fmt",
      "rgba",
      "-s",
      std::to_string(outWidth) + "x" + std::to_string(outHeight),
      "-r",
      std::to_string(safeFps),
      "-i",
      "pipe:0",
  });

  if (hasDiskAudio) {
    args.push_back("-ss");
    args.push_back(formatFixed3(std::max(0.0, startSec)));
    args.push_back("-t");
    args.push_back(formatFixed3(std::max(0.05, exportDur)));
    args.push_back("-i");
    args.push_back(iota::core::pathToUtf8(audioDiskPath));
  }

  args.push_back("-c:v");
  args.push_back(enc);
  if (presetPair.first[0] != '\0') {
    args.push_back(presetPair.first);
    args.push_back(presetPair.second);
  }
  if (effectiveBackend == VideoEncoderBackend::LinuxVaApi) {
    args.push_back("-vf");
    args.push_back("format=nv12,hwupload");
  } else {
    args.push_back("-pix_fmt");
    args.push_back(pix);
  }
  args.push_back("-b:v");
  args.push_back(std::to_string(safeBitrateKbps) + "k");

  if (hasDiskAudio) {
    args.push_back("-c:a");
    args.push_back("aac");
    args.push_back("-b:a");
    args.push_back("192k");
    args.push_back("-shortest");
  } else {
    args.push_back("-an");
  }

  if (plan.codec == VideoCodec::H265) {
    args.push_back("-tag:v");
    args.push_back("hvc1");
  }

  args.push_back("-movflags");
  args.push_back("+faststart");
  args.push_back(iota::core::pathToUtf8(settings.outputPath));
  return args;
}

std::wstring buildFfmpegExportCommand(const ExportSettings& settings,
                                      const ResolvedEncoderPlan& plan,
                                      int outWidth,
                                      int outHeight,
                                      double startSec,
                                      double exportDur,
                                      const std::filesystem::path& audioDiskPath,
                                      bool hasDiskAudio) {
  const auto args = buildFfmpegExportArgs(
      settings, plan, outWidth, outHeight, startSec, exportDur, audioDiskPath, hasDiskAudio);
  std::wstring cmd;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) cmd.push_back(L' ');
    cmd += quoteWindowsArg(utf8ToWstringSimple(args[i]));
  }
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
#ifndef _WIN32
  caps.vaapiH264 = probeEncoderWithFlags(VideoEncoderBackend::LinuxVaApi, VideoCodec::H264, nullptr, nullptr, nullptr, nullptr);
  caps.vaapiH265 = probeEncoderWithFlags(VideoEncoderBackend::LinuxVaApi, VideoCodec::H265, nullptr, nullptr, nullptr, nullptr);
#endif
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
  NativeProcHandle activeProc = kInvalidNativeProc;

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
#ifndef _WIN32
        {VideoEncoderBackend::LinuxVaApi, VideoCodec::H264},
        {VideoEncoderBackend::LinuxVaApi, VideoCodec::H265},
#endif
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
    if (impl_->activeProc != kInvalidNativeProc) {
      terminateNativeProc(impl_->activeProc);
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
#ifdef _WIN32
  HANDLE stdinWritePipe = nullptr;
  HANDLE stderrReadPipe = nullptr;
  HANDLE processHandle = nullptr;
  HANDLE threadHandle = nullptr;
#else
  int stdinWriteFd = -1;
  int stderrReadFd = -1;
  pid_t childPid = -1;
#endif
  std::string stderrLog;
  std::vector<uint8_t> rgbaReadback;

  void drainStderr() {
#ifdef _WIN32
    drainPipeNonBlocking(stderrReadPipe, stderrLog, 4096);
#else
    drainPipeNonBlocking(stderrReadFd, stderrLog, 4096);
#endif
  }

  void cleanupProcess(bool terminate) {
#ifdef _WIN32
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
#else
    if (stdinWriteFd >= 0) {
      close(stdinWriteFd);
      stdinWriteFd = -1;
    }
    if (childPid > 0) {
      if (terminate) {
        kill(childPid, SIGTERM);
      }
      const auto waitStart = std::chrono::steady_clock::now();
      bool sentKill = false;
      for (;;) {
        drainStderr();
        int status = 0;
        const pid_t w = waitpid(childPid, &status, WNOHANG);
        if (w == childPid || (w < 0 && errno != EINTR)) {
          break;
        }
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart)
                .count();
        if (terminate && !sentKill && elapsedMs >= 500) {
          kill(childPid, SIGKILL);
          sentKill = true;
        }
        if (terminate && elapsedMs >= 2000) {
          kill(childPid, SIGKILL);
          while (waitpid(childPid, &status, 0) < 0 && errno == EINTR) {
          }
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
      }
      drainStderr();
      childPid = -1;
    }
    if (stderrReadFd >= 0) {
      close(stderrReadFd);
      stderrReadFd = -1;
    }
#endif
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

  const std::vector<std::string> args = buildFfmpegExportArgs(impl_->cfg,
                                                            plan,
                                                            impl_->outWidth,
                                                            impl_->outHeight,
                                                            impl_->startSec,
                                                            exportDur,
                                                            audioDiskPath,
                                                            hasDiskAudio);

#ifdef _WIN32
  const std::wstring cmd = joinWindowsArgs(args);

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
        *outError = std::string("Failed to launch ") + kFfmpegExeName + " (executable not found).";
      } else {
        *outError = std::string("Failed to launch ") + kFfmpegExeName +
                    " for video export (error " + std::to_string(errCode) + ").";
      }
    }
    return false;
  }

  impl_->stdinWritePipe = stdinWritePipe;
  impl_->stderrReadPipe = stderrReadPipe;
  impl_->processHandle = pi.hProcess;
  impl_->threadHandle = pi.hThread;
#else
  ensureSigpipeIgnored();

  int stdinPipe[2]{-1, -1};
  int stderrPipe[2]{-1, -1};
  if (pipe(stdinPipe) != 0) {
    if (outError) *outError = "Failed to create FFmpeg stdin pipe.";
    return false;
  }
  if (pipe(stderrPipe) != 0) {
    close(stdinPipe[0]);
    close(stdinPipe[1]);
    if (outError) *outError = "Failed to create FFmpeg stderr pipe.";
    return false;
  }
  fcntl(stdinPipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(stdinPipe[1], F_SETFD, FD_CLOEXEC);
  fcntl(stderrPipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(stderrPipe[1], F_SETFD, FD_CLOEXEC);
  setFdFlags(stderrPipe[0], O_NONBLOCK);

  const pid_t pid = fork();
  if (pid < 0) {
    close(stdinPipe[0]);
    close(stdinPipe[1]);
    close(stderrPipe[0]);
    close(stderrPipe[1]);
    if (outError) *outError = std::string("Failed to fork process for ") + kFfmpegExeName + ".";
    return false;
  }

  if (pid == 0) {
    close(stdinPipe[1]);
    close(stderrPipe[0]);
    dup2(stdinPipe[0], STDIN_FILENO);
    close(stdinPipe[0]);
    const int devNull = open("/dev/null", O_WRONLY);
    if (devNull >= 0) {
      dup2(devNull, STDOUT_FILENO);
      if (devNull > STDERR_FILENO) close(devNull);
    }
    dup2(stderrPipe[1], STDERR_FILENO);
    close(stderrPipe[1]);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& s : args) {
      argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }

  close(stdinPipe[0]);
  close(stderrPipe[1]);
  impl_->stdinWriteFd = stdinPipe[1];
  impl_->stderrReadFd = stderrPipe[0];
  impl_->childPid = pid;
#endif
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
#ifdef _WIN32
      const DWORD chunk = static_cast<DWORD>(std::min<size_t>(totalBytes - writtenTotal, 1024u * 1024u));
      DWORD written = 0;
      const bool writeOk =
          WriteFile(impl_->stdinWritePipe, impl_->rgbaReadback.data() + writtenTotal, chunk, &written, nullptr) &&
          written > 0;
      if (!writeOk) {
        if (impl_->processHandle != nullptr) {
          WaitForSingleObject(impl_->processHandle, 1000);
        }
#else
      const size_t chunk = std::min<size_t>(totalBytes - writtenTotal, 1024u * 1024u);
      ssize_t written = -1;
      for (;;) {
        written = write(impl_->stdinWriteFd, impl_->rgbaReadback.data() + writtenTotal, chunk);
        if (written < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      if (written <= 0) {
        if (impl_->childPid > 0) {
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
          int status = 0;
          while (std::chrono::steady_clock::now() < deadline) {
            const pid_t w = waitpid(impl_->childPid, &status, WNOHANG);
            if (w == impl_->childPid || (w < 0 && errno != EINTR)) {
              impl_->childPid = -1;
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        }
#endif
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
      writtenTotal += static_cast<size_t>(written);
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
#ifdef _WIN32
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
#else
  if (impl_->stdinWriteFd >= 0) {
    close(impl_->stdinWriteFd);
    impl_->stdinWriteFd = -1;
  }
  uint32_t exitCode = 1;
  if (impl_->childPid > 0) {
    int status = 0;
    for (;;) {
      impl_->drainStderr();
      const pid_t w = waitpid(impl_->childPid, &status, WNOHANG);
      if (w == impl_->childPid) {
        exitCode = WIFEXITED(status) ? static_cast<uint32_t>(WEXITSTATUS(status)) : 1u;
        impl_->childPid = -1;
        break;
      }
      if (w < 0 && errno != EINTR) {
        impl_->childPid = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    impl_->drainStderr();
  }
#endif
  const std::string detail = trimWhitespace(impl_->stderrLog);
  impl_->cleanupProcess(false);
  if (exitCode != 0) {
    if (outError) {
      *outError = detail.empty()
                      ? (std::string(kFfmpegExeName) + " returned exit code " + std::to_string(exitCode) +
                         " during export.")
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
