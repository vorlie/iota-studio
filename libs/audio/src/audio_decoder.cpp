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

#include "iota/audio/audio_decoder.hpp"

#include "iota/core/utf8_path.hpp"

#include "miniaudio.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <vector>

namespace iota::audio {

namespace {

#ifdef _WIN32
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

struct ProcessResult {
  bool launched = false;
  DWORD exitCode = 1;
  std::vector<uint8_t> stdoutBytes;
};

ProcessResult runChildCaptureStdout(const std::wstring& commandLine) {
  ProcessResult res;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
    return res;
  }
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writePipe;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  si.hStdInput = nullptr;

  PROCESS_INFORMATION pi{};
  std::wstring mutableCmd = commandLine;
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
  CloseHandle(writePipe);

  if (!ok) {
    CloseHandle(readPipe);
    return res;
  }
  res.launched = true;

  std::vector<uint8_t> chunk(65536);
  DWORD bytesRead = 0;
  while (ReadFile(readPipe, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, nullptr) &&
         bytesRead > 0) {
    res.stdoutBytes.insert(res.stdoutBytes.end(), chunk.begin(), chunk.begin() + bytesRead);
  }
  CloseHandle(readPipe);

  WaitForSingleObject(pi.hProcess, INFINITE);
  GetExitCodeProcess(pi.hProcess, &res.exitCode);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return res;
}
#else
struct ProcessResult {
  bool launched = false;
  int exitCode = 1;
  std::vector<uint8_t> stdoutBytes;
};

ProcessResult runChildCaptureStdout(const std::vector<std::string>& args) {
  ProcessResult res;
  if (args.empty()) {
    return res;
  }

  int pipefd[2]{-1, -1};
  if (pipe(pipefd) != 0) {
    return res;
  }
  fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
  fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return res;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    const int devNull = open("/dev/null", O_RDWR);
    if (devNull >= 0) {
      dup2(devNull, STDIN_FILENO);
      dup2(devNull, STDERR_FILENO);
      if (devNull > STDERR_FILENO) {
        close(devNull);
      }
    }
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
  std::vector<uint8_t> chunk(65536);
  for (;;) {
    const ssize_t n = read(pipefd[0], chunk.data(), chunk.size());
    if (n > 0) {
      res.stdoutBytes.insert(res.stdoutBytes.end(), chunk.begin(), chunk.begin() + n);
    } else if (n == 0) {
      break;
    } else if (errno != EINTR) {
      break;
    }
  }
  close(pipefd[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (WIFEXITED(status)) {
    res.exitCode = WEXITSTATUS(status);
  } else {
    res.exitCode = 1;
  }
  res.launched = !(res.exitCode == 127 && res.stdoutBytes.empty());
  return res;
}
#endif

std::string asciiLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

void probeTagsWithFfprobe(const std::filesystem::path& path, AudioTrackData& track) {
#ifdef _WIN32
  const std::wstring cmd =
      L"ffprobe.exe -v error -show_entries format=duration:format_tags=title,artist,album "
      L"-of default=noprint_wrappers=1 " +
      quoteWindowsArg(path.wstring());
  const auto proc = runChildCaptureStdout(cmd);
#else
  const auto proc = runChildCaptureStdout({
      "ffprobe",
      "-v",
      "error",
      "-show_entries",
      "format=duration:format_tags=title,artist,album",
      "-of",
      "default=noprint_wrappers=1",
      iota::core::pathToUtf8(path),
  });
#endif
  if (!proc.launched || proc.exitCode != 0 || proc.stdoutBytes.empty()) {
    return;
  }

  std::string text(reinterpret_cast<const char*>(proc.stdoutBytes.data()), proc.stdoutBytes.size());
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = asciiLower(std::string_view(line).substr(0, eq));
    const std::string val = line.substr(eq + 1);
    if (key == "tag:title") {
      track.title = iota::core::sanitizeUtf8(val);
    } else if (key == "tag:artist") {
      track.artist = iota::core::sanitizeUtf8(val);
    } else if (key == "tag:album") {
      track.album = iota::core::sanitizeUtf8(val);
    }
  }
}

std::optional<AudioTrackData> decodeWithFfmpeg(const std::filesystem::path& path,
                                               int targetSampleRate,
                                               std::string* outError) {
#ifdef _WIN32
  const std::wstring srStr = std::to_wstring(std::max(8000, targetSampleRate));
  const std::wstring cmd =
      L"ffmpeg.exe -v error -i " + quoteWindowsArg(path.wstring()) +
      L" -vn -f f32le -acodec pcm_f32le -ac 2 -ar " + srStr + L" pipe:1";
  const auto proc = runChildCaptureStdout(cmd);
  constexpr const char* kFfmpegLabel = "ffmpeg.exe";
#else
  const std::string srStr = std::to_string(std::max(8000, targetSampleRate));
  const auto proc = runChildCaptureStdout({
      "ffmpeg",
      "-v",
      "error",
      "-i",
      iota::core::pathToUtf8(path),
      "-vn",
      "-f",
      "f32le",
      "-acodec",
      "pcm_f32le",
      "-ac",
      "2",
      "-ar",
      srStr,
      "pipe:1",
  });
  constexpr const char* kFfmpegLabel = "ffmpeg";
#endif
  if (!proc.launched) {
    if (outError) {
      *outError = std::string(kFfmpegLabel) + " could not be launched";
    }
    return std::nullopt;
  }
  if (proc.exitCode != 0 || proc.stdoutBytes.size() < sizeof(float) * 2) {
    if (outError) {
      *outError = std::string(kFfmpegLabel) + " failed to decode audio stream";
    }
    return std::nullopt;
  }

  const size_t floatCount = proc.stdoutBytes.size() / sizeof(float);
  const size_t stereoAligned = floatCount & ~size_t{1};
  if (stereoAligned == 0) {
    if (outError) {
      *outError = std::string(kFfmpegLabel) + " produced empty PCM output";
    }
    return std::nullopt;
  }

  AudioTrackData track;
  track.sampleRate = std::max(8000, targetSampleRate);
  track.channels = 2;
  track.filePath = iota::core::pathToUtf8(path);
  track.interleavedPcm.resize(stereoAligned);
  std::memcpy(track.interleavedPcm.data(), proc.stdoutBytes.data(), stereoAligned * sizeof(float));

  probeTagsWithFfprobe(path, track);
  if (track.title.empty()) {
    track.title = iota::core::pathStemUtf8(path);
  }
  rebuildMonoAndDuration(track);
  return track;
}

std::optional<AudioTrackData> decodeWithMiniaudio(const std::filesystem::path& path,
                                                  int targetSampleRate,
                                                  std::string* outError) {
  ma_decoder_config decCfg = ma_decoder_config_init(ma_format_f32, 2, static_cast<ma_uint32>(targetSampleRate));
  ma_decoder decoder{};
#ifdef _WIN32
  const ma_result initRes = ma_decoder_init_file_w(path.c_str(), &decCfg, &decoder);
#else
  const ma_result initRes = ma_decoder_init_file(path.c_str(), &decCfg, &decoder);
#endif
  if (initRes != MA_SUCCESS) {
    if (outError) {
      *outError = "miniaudio failed to open audio file";
    }
    return std::nullopt;
  }

  AudioTrackData track;
  track.sampleRate = static_cast<int>(decoder.outputSampleRate);
  track.channels = 2;
  track.filePath = iota::core::pathToUtf8(path);
  track.title = iota::core::pathStemUtf8(path);

  constexpr ma_uint64 kChunkFrames = 8192;
  std::vector<float> chunk(static_cast<size_t>(kChunkFrames) * 2);
  while (true) {
    ma_uint64 framesRead = 0;
    const ma_result r = ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunkFrames, &framesRead);
    if (framesRead > 0) {
      track.interleavedPcm.insert(track.interleavedPcm.end(),
                                  chunk.begin(),
                                  chunk.begin() + static_cast<ptrdiff_t>(framesRead * 2));
    }
    if (r != MA_SUCCESS || framesRead == 0) {
      break;
    }
  }
  ma_decoder_uninit(&decoder);

  if (track.interleavedPcm.empty()) {
    if (outError) {
      *outError = "Decoded PCM stream was empty";
    }
    return std::nullopt;
  }

  rebuildMonoAndDuration(track);
  return track;
}

}  // namespace

void rebuildMonoAndDuration(AudioTrackData& track) {
  const int ch = std::max(1, track.channels);
  const size_t totalFrames = track.interleavedPcm.size() / static_cast<size_t>(ch);
  track.monoPcm.resize(totalFrames);

  if (ch == 1) {
    std::copy_n(track.interleavedPcm.begin(), totalFrames, track.monoPcm.begin());
  } else {
    const float invCh = 1.0f / static_cast<float>(ch);
    for (size_t i = 0; i < totalFrames; ++i) {
      float sum = 0.0f;
      for (int c = 0; c < ch; ++c) {
        sum += track.interleavedPcm[i * static_cast<size_t>(ch) + static_cast<size_t>(c)];
      }
      track.monoPcm[i] = sum * invCh;
    }
  }
  track.durationSec = (track.sampleRate > 0)
                          ? (static_cast<double>(totalFrames) / static_cast<double>(track.sampleRate))
                          : 0.0;
}

std::optional<AudioTrackData> decodeAudioFile(const std::filesystem::path& path,
                                              int targetSampleRate,
                                              std::string* outError) {
  if (!iota::core::pathExistsNoThrow(path)) {
    if (outError) {
      *outError = "Audio file does not exist: " + iota::core::pathToUtf8(path);
    }
    return std::nullopt;
  }

  std::string ffmpegErr;
  if (auto track = decodeWithFfmpeg(path, targetSampleRate, &ffmpegErr)) {
    return track;
  }

  std::string maErr;
  if (auto track = decodeWithMiniaudio(path, targetSampleRate, &maErr)) {
    return track;
  }

  if (outError) {
    *outError = ffmpegErr + "; fallback: " + maErr;
  }
  return std::nullopt;
}

AudioTrackData createSyntheticSineTrack(float freqHz,
                                        double durationSec,
                                        int sampleRate,
                                        float amplitude,
                                        std::string title) {
  AudioTrackData track;
  track.sampleRate = std::max(8000, sampleRate);
  track.channels = 2;
  track.title = std::move(title);
  track.artist = "Test Signal";
  const size_t frames = static_cast<size_t>(std::max(1.0, durationSec * static_cast<double>(track.sampleRate)));
  track.interleavedPcm.resize(frames * 2);

  constexpr double kTwoPi = 6.28318530717958647692;
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(track.sampleRate);
    const float s = amplitude * static_cast<float>(std::sin(kTwoPi * static_cast<double>(freqHz) * t));
    track.interleavedPcm[i * 2] = s;
    track.interleavedPcm[i * 2 + 1] = s;
  }
  rebuildMonoAndDuration(track);
  return track;
}

}  // namespace iota::audio
