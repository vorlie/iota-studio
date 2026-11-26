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

#include "iota/mcp/live_bridge.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace iota::mcp {

namespace {

constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

#ifdef _WIN32

std::wstring utf8ToWide(std::string_view s) {
  if (s.empty()) return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
  return out;
}

bool overlappedReadExact(HANDLE pipe,
                         void* dst,
                         DWORD len,
                         HANDLE stopEvent,
                         DWORD timeoutMs,
                         std::string* outErr) {
  auto* ptr = static_cast<uint8_t*>(dst);
  DWORD offset = 0;
  while (offset < len) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
      if (outErr) *outErr = "Failed to create overlapped read event.";
      return false;
    }

    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(pipe, ptr + offset, len - offset, &bytesRead, &ov);
    if (!ok) {
      const DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        if (stopEvent != nullptr) {
          HANDLE waits[2] = {ov.hEvent, stopEvent};
          const DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
          if (w != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ov);
            DWORD dummy = 0;
            GetOverlappedResult(pipe, &ov, &dummy, TRUE);
            CloseHandle(ov.hEvent);
            if (outErr) {
              *outErr = (w == WAIT_OBJECT_0 + 1) ? "Bridge stopped." : "Timed out reading bridge frame.";
            }
            return false;
          }
        } else {
          const DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
          if (w != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ov);
            DWORD dummy = 0;
            GetOverlappedResult(pipe, &ov, &dummy, TRUE);
            CloseHandle(ov.hEvent);
            if (outErr) *outErr = "Timed out reading bridge frame.";
            return false;
          }
        }
        if (!GetOverlappedResult(pipe, &ov, &bytesRead, FALSE)) {
          const DWORD ovErr = GetLastError();
          CloseHandle(ov.hEvent);
          if (outErr) {
            *outErr = (ovErr == ERROR_BROKEN_PIPE || ovErr == ERROR_HANDLE_EOF)
                          ? "Named pipe disconnected."
                          : ("Named pipe read failed (error " + std::to_string(ovErr) + ").");
          }
          return false;
        }
      } else {
        CloseHandle(ov.hEvent);
        if (outErr) {
          *outErr = (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF)
                        ? "Named pipe disconnected."
                        : ("Named pipe read failed (error " + std::to_string(err) + ").");
        }
        return false;
      }
    }

    CloseHandle(ov.hEvent);
    if (bytesRead == 0) {
      if (outErr) *outErr = "Named pipe closed (EOF).";
      return false;
    }
    offset += bytesRead;
  }
  return true;
}

bool overlappedWriteExact(HANDLE pipe,
                          const void* src,
                          DWORD len,
                          HANDLE stopEvent,
                          DWORD timeoutMs,
                          std::string* outErr) {
  const auto* ptr = static_cast<const uint8_t*>(src);
  DWORD offset = 0;
  while (offset < len) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
      if (outErr) *outErr = "Failed to create overlapped write event.";
      return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, ptr + offset, len - offset, &written, &ov);
    if (!ok) {
      const DWORD err = GetLastError();
      if (err == ERROR_IO_PENDING) {
        if (stopEvent != nullptr) {
          HANDLE waits[2] = {ov.hEvent, stopEvent};
          const DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
          if (w != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ov);
            DWORD dummy = 0;
            GetOverlappedResult(pipe, &ov, &dummy, TRUE);
            CloseHandle(ov.hEvent);
            if (outErr) *outErr = "Named pipe write cancelled or timed out.";
            return false;
          }
        } else {
          const DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
          if (w != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ov);
            DWORD dummy = 0;
            GetOverlappedResult(pipe, &ov, &dummy, TRUE);
            CloseHandle(ov.hEvent);
            if (outErr) *outErr = "Timed out writing bridge frame.";
            return false;
          }
        }
        if (!GetOverlappedResult(pipe, &ov, &written, FALSE)) {
          const DWORD ovErr = GetLastError();
          CloseHandle(ov.hEvent);
          if (outErr) *outErr = "Named pipe write failed (error " + std::to_string(ovErr) + ").";
          return false;
        }
      } else {
        CloseHandle(ov.hEvent);
        if (outErr) *outErr = "Named pipe write failed (error " + std::to_string(err) + ").";
        return false;
      }
    }

    CloseHandle(ov.hEvent);
    if (written == 0) {
      if (outErr) *outErr = "Named pipe zero-byte write.";
      return false;
    }
    offset += written;
  }
  return true;
}

bool readJsonFrame(HANDLE pipe,
                   nlohmann::json& outJson,
                   HANDLE stopEvent,
                   DWORD timeoutMs,
                   std::string* outErr) {
  uint8_t hdr[4]{};
  if (!overlappedReadExact(pipe, hdr, 4, stopEvent, timeoutMs, outErr)) {
    return false;
  }
  const uint32_t msgLen = static_cast<uint32_t>(hdr[0]) |
                          (static_cast<uint32_t>(hdr[1]) << 8) |
                          (static_cast<uint32_t>(hdr[2]) << 16) |
                          (static_cast<uint32_t>(hdr[3]) << 24);
  if (msgLen == 0 || msgLen > kMaxFrameBytes) {
    if (outErr) *outErr = "Bridge frame size out of bounds.";
    return false;
  }

  std::string payload(msgLen, '\0');
  if (!overlappedReadExact(pipe, payload.data(), msgLen, stopEvent, timeoutMs, outErr)) {
    return false;
  }

  auto parsed = nlohmann::json::parse(payload, nullptr, false);
  if (parsed.is_discarded()) {
    if (outErr) *outErr = "Invalid JSON in bridge frame.";
    return false;
  }
  outJson = std::move(parsed);
  return true;
}

bool writeJsonFrame(HANDLE pipe,
                    const nlohmann::json& msg,
                    HANDLE stopEvent,
                    DWORD timeoutMs,
                    std::string* outErr) {
  const std::string payload = msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  if (payload.size() > kMaxFrameBytes) {
    if (outErr) *outErr = "Serialized bridge response exceeds maximum frame size.";
    return false;
  }

  const uint32_t len = static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> packet(4u + payload.size());
  packet[0] = static_cast<uint8_t>(len & 0xFFu);
  packet[1] = static_cast<uint8_t>((len >> 8) & 0xFFu);
  packet[2] = static_cast<uint8_t>((len >> 16) & 0xFFu);
  packet[3] = static_cast<uint8_t>((len >> 24) & 0xFFu);
  std::memcpy(packet.data() + 4, payload.data(), payload.size());

  return overlappedWriteExact(
      pipe, packet.data(), static_cast<DWORD>(packet.size()), stopEvent, timeoutMs, outErr);
}

#else

void ensureSigpipeIgnored() {
  static std::once_flag flag;
  std::call_once(flag, []() { std::signal(SIGPIPE, SIG_IGN); });
}

void setFdNonBlockingAndCloexec(int fd) noexcept {
  const int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) {
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}

// Returns 1 if fd is ready, 0 on timeout, -2 if stopReadFd signaled, -1 on error.
int waitSocketOrStop(int fd, int stopReadFd, short events, int timeoutMs) {
  const bool infinite = (timeoutMs < 0);
  const auto deadline =
      infinite ? std::chrono::steady_clock::time_point::max()
               : (std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs));

  for (;;) {
    pollfd fds[2]{};
    fds[0].fd = fd;
    fds[0].events = events;
    nfds_t count = 1;
    if (stopReadFd >= 0) {
      fds[1].fd = stopReadFd;
      fds[1].events = POLLIN;
      count = 2;
    }

    int remMs = -1;
    if (!infinite) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return 0;
      }
      remMs = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    }

    const int rc = poll(fds, count, remMs);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (rc == 0) {
      return 0;
    }
    if (count == 2 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      return -2;
    }
    if ((fds[0].revents & (events | POLLHUP | POLLERR | POLLNVAL)) != 0) {
      return 1;
    }
  }
}

bool socketReadExact(int fd,
                     void* dst,
                     size_t len,
                     int stopReadFd,
                     int timeoutMs,
                     std::string* outErr) {
  auto* ptr = static_cast<uint8_t*>(dst);
  size_t offset = 0;
  const bool infinite = (timeoutMs < 0);
  const auto deadline =
      infinite ? std::chrono::steady_clock::time_point::max()
               : (std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs));

  while (offset < len) {
    const ssize_t n = recv(fd, ptr + offset, len - offset, 0);
    if (n > 0) {
      offset += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      if (outErr) *outErr = "Bridge socket closed (EOF).";
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      int remMs = -1;
      if (!infinite) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          if (outErr) *outErr = "Timed out reading bridge frame.";
          return false;
        }
        remMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
      }
      const int w = waitSocketOrStop(fd, stopReadFd, POLLIN, remMs);
      if (w == -2) {
        if (outErr) *outErr = "Bridge stopped.";
        return false;
      }
      if (w == 0) {
        if (outErr) *outErr = "Timed out reading bridge frame.";
        return false;
      }
      if (w < 0) {
        if (outErr) *outErr = "Socket poll failed while reading bridge frame.";
        return false;
      }
      continue;
    }
    if (outErr) {
      *outErr = (errno == ECONNRESET || errno == EPIPE)
                    ? "Bridge socket disconnected."
                    : ("Bridge socket read failed (" + std::string(std::strerror(errno)) + ").");
    }
    return false;
  }
  return true;
}

bool socketWriteExact(int fd,
                      const void* src,
                      size_t len,
                      int stopReadFd,
                      int timeoutMs,
                      std::string* outErr) {
  const auto* ptr = static_cast<const uint8_t*>(src);
  size_t offset = 0;
  const bool infinite = (timeoutMs < 0);
  const auto deadline =
      infinite ? std::chrono::steady_clock::time_point::max()
               : (std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs));

  while (offset < len) {
    const ssize_t n = send(fd, ptr + offset, len - offset, MSG_NOSIGNAL);
    if (n > 0) {
      offset += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      if (outErr) *outErr = "Bridge socket zero-byte write.";
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      int remMs = -1;
      if (!infinite) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          if (outErr) *outErr = "Timed out writing bridge frame.";
          return false;
        }
        remMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
      }
      const int w = waitSocketOrStop(fd, stopReadFd, POLLOUT, remMs);
      if (w == -2) {
        if (outErr) *outErr = "Bridge socket write cancelled.";
        return false;
      }
      if (w == 0) {
        if (outErr) *outErr = "Timed out writing bridge frame.";
        return false;
      }
      if (w < 0) {
        if (outErr) *outErr = "Socket poll failed while writing bridge frame.";
        return false;
      }
      continue;
    }
    if (outErr) {
      *outErr = "Bridge socket write failed (" + std::string(std::strerror(errno)) + ").";
    }
    return false;
  }
  return true;
}

bool readJsonFrame(int fd,
                   nlohmann::json& outJson,
                   int stopReadFd,
                   int timeoutMs,
                   std::string* outErr) {
  uint8_t hdr[4]{};
  if (!socketReadExact(fd, hdr, 4, stopReadFd, timeoutMs, outErr)) {
    return false;
  }
  const uint32_t msgLen = static_cast<uint32_t>(hdr[0]) |
                          (static_cast<uint32_t>(hdr[1]) << 8) |
                          (static_cast<uint32_t>(hdr[2]) << 16) |
                          (static_cast<uint32_t>(hdr[3]) << 24);
  if (msgLen == 0 || msgLen > kMaxFrameBytes) {
    if (outErr) *outErr = "Bridge frame size out of bounds.";
    return false;
  }

  std::string payload(msgLen, '\0');
  if (!socketReadExact(fd, payload.data(), msgLen, stopReadFd, timeoutMs, outErr)) {
    return false;
  }

  auto parsed = nlohmann::json::parse(payload, nullptr, false);
  if (parsed.is_discarded()) {
    if (outErr) *outErr = "Invalid JSON in bridge frame.";
    return false;
  }
  outJson = std::move(parsed);
  return true;
}

bool writeJsonFrame(int fd,
                    const nlohmann::json& msg,
                    int stopReadFd,
                    int timeoutMs,
                    std::string* outErr) {
  const std::string payload = msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  if (payload.size() > kMaxFrameBytes) {
    if (outErr) *outErr = "Serialized bridge response exceeds maximum frame size.";
    return false;
  }

  const uint32_t len = static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> packet(4u + payload.size());
  packet[0] = static_cast<uint8_t>(len & 0xFFu);
  packet[1] = static_cast<uint8_t>((len >> 8) & 0xFFu);
  packet[2] = static_cast<uint8_t>((len >> 16) & 0xFFu);
  packet[3] = static_cast<uint8_t>((len >> 24) & 0xFFu);
  std::memcpy(packet.data() + 4, payload.data(), payload.size());

  return socketWriteExact(fd, packet.data(), packet.size(), stopReadFd, timeoutMs, outErr);
}

bool fillUnixAddr(const std::string& path, sockaddr_un& outAddr) {
  if (path.empty() || path.size() >= sizeof(outAddr.sun_path)) {
    return false;
  }
  std::memset(&outAddr, 0, sizeof(outAddr));
  outAddr.sun_family = AF_UNIX;
  std::memcpy(outAddr.sun_path, path.data(), path.size());
  outAddr.sun_path[path.size()] = '\0';
  return true;
}

#endif

}  // namespace

std::string defaultLiveBridgePipeName() {
#ifdef _WIN32
  wchar_t userBuf[128]{};
  DWORD len = static_cast<DWORD>(std::size(userBuf));
  std::string sanitized = "local";
  if (GetUserNameW(userBuf, &len) && len > 1) {
    sanitized.clear();
    for (DWORD i = 0; i < len - 1; ++i) {
      const wchar_t wc = userBuf[i];
      if ((wc >= L'a' && wc <= L'z') || (wc >= L'A' && wc <= L'Z') ||
          (wc >= L'0' && wc <= L'9') || wc == L'_' || wc == L'-') {
        sanitized.push_back(static_cast<char>(wc));
      } else {
        sanitized.push_back('_');
      }
    }
    if (sanitized.empty()) {
      sanitized = "local";
    }
  }
  return "\\\\.\\pipe\\iota_studio_mcp_" + sanitized;
#else
  if (const char* xdgRuntime = std::getenv("XDG_RUNTIME_DIR"); xdgRuntime != nullptr && *xdgRuntime != '\0') {
    std::string candidate = std::string(xdgRuntime) + "/iota_studio_mcp.sock";
    if (candidate.size() < sizeof(sockaddr_un::sun_path)) {
      return candidate;
    }
  }
  return "/tmp/iota_studio_mcp_" + std::to_string(static_cast<unsigned long>(getuid())) + ".sock";
#endif
}

struct LiveBridgeHost::Impl {
  struct PendingRequest {
    nlohmann::json request;
    std::mutex doneMutex;
    std::condition_variable doneCv;
    bool done = false;
    nlohmann::json response;
  };

  struct ClientSession {
    std::thread thread;
    std::atomic<bool> finished{false};
  };

  mutable std::mutex configMutex;
  std::string pipeName;

#ifdef _WIN32
  static constexpr DWORD kMaxPipeInstances = 8;
  HANDLE stopEvent = nullptr;
#else
  int listenFd = -1;
  int stopPipeReadFd = -1;
  int stopPipeWriteFd = -1;
#endif
  std::atomic<bool> stopRequested{false};
  std::atomic<bool> listening{false};
  std::atomic<int> activeClients{0};
  std::atomic<uint64_t> handledCount{0};
  std::thread worker;

  std::mutex clientsMutex;
  std::vector<std::unique_ptr<ClientSession>> clientSessions;

  std::mutex queueMutex;
  std::deque<std::shared_ptr<PendingRequest>> queue;

  void reapFinishedClients() {
    std::vector<std::unique_ptr<ClientSession>> done;
    {
      std::lock_guard<std::mutex> lock(clientsMutex);
      auto it = clientSessions.begin();
      while (it != clientSessions.end()) {
        if ((*it)->finished.load(std::memory_order_acquire)) {
          done.push_back(std::move(*it));
          it = clientSessions.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& session : done) {
      if (session && session->thread.joinable()) {
        session->thread.join();
      }
    }
  }

  void joinAllClients() {
    std::vector<std::unique_ptr<ClientSession>> all;
    {
      std::lock_guard<std::mutex> lock(clientsMutex);
      all = std::move(clientSessions);
      clientSessions.clear();
    }
    for (auto& session : all) {
      if (session && session->thread.joinable()) {
        session->thread.join();
      }
    }
  }

#ifdef _WIN32
  HANDLE createListeningPipe(const std::wstring& wideName, bool firstInstance) const {
    const DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                           (firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    return CreateNamedPipeW(
        wideName.c_str(),
        openMode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        kMaxPipeInstances,
        65536,
        65536,
        1000,
        nullptr);
  }

  void clientSessionLoop(HANDLE pipe, ClientSession* session) {
    struct Guard {
      Impl* self;
      HANDLE p;
      ClientSession* s;
      ~Guard() {
        DisconnectNamedPipe(p);
        CloseHandle(p);
        self->activeClients.fetch_sub(1, std::memory_order_release);
        if (s) s->finished.store(true, std::memory_order_release);
      }
    } guard{this, pipe, session};

    while (!stopRequested.load(std::memory_order_acquire)) {
      nlohmann::json req;
      std::string ioErr;
      if (!readJsonFrame(pipe, req, stopEvent, INFINITE, &ioErr)) {
        break;
      }

      auto item = std::make_shared<PendingRequest>();
      item->request = std::move(req);
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push_back(item);
      }

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
      {
        std::unique_lock<std::mutex> lock(item->doneMutex);
        while (!item->done && !stopRequested.load(std::memory_order_acquire)) {
          if (std::chrono::steady_clock::now() >= deadline) {
            break;
          }
          item->doneCv.wait_for(lock, std::chrono::milliseconds(50));
        }
      }

      if (stopRequested.load(std::memory_order_acquire)) {
        break;
      }

      nlohmann::json resp;
      {
        std::lock_guard<std::mutex> lock(item->doneMutex);
        if (item->done) {
          resp = item->response;
        } else {
          resp = {
              {"jsonrpc", "2.0"},
              {"id", nullptr},
              {"error", {{"code", -32000}, {"message", "Timed out waiting for Studio main thread."}}},
          };
        }
      }

      if (!writeJsonFrame(pipe, resp, stopEvent, 10000, &ioErr)) {
        break;
      }
      handledCount.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void acceptLoop(std::string targetPipeName, HANDLE firstPipe) {
    const std::wstring wideName = utf8ToWide(targetPipeName);
    HANDLE pendingPipe = firstPipe;

    while (!stopRequested.load(std::memory_order_acquire)) {
      reapFinishedClients();

      HANDLE pipe = pendingPipe;
      pendingPipe = INVALID_HANDLE_VALUE;
      if (pipe == INVALID_HANDLE_VALUE) {
        pipe = createListeningPipe(wideName, false);
      }
      if (pipe == INVALID_HANDLE_VALUE) {
        if (WaitForSingleObject(stopEvent, 50) == WAIT_OBJECT_0) {
          break;
        }
        continue;
      }

      OVERLAPPED ovConnect{};
      ovConnect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!ovConnect.hEvent) {
        CloseHandle(pipe);
        break;
      }

      bool connected = false;
      if (ConnectNamedPipe(pipe, &ovConnect)) {
        connected = true;
      } else {
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
          connected = true;
        } else if (err == ERROR_IO_PENDING) {
          HANDLE waits[2] = {ovConnect.hEvent, stopEvent};
          const DWORD waitRes = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
          if (waitRes == WAIT_OBJECT_0) {
            DWORD dummy = 0;
            connected = (GetOverlappedResult(pipe, &ovConnect, &dummy, FALSE) != FALSE);
          } else {
            CancelIoEx(pipe, &ovConnect);
            DWORD dummy = 0;
            GetOverlappedResult(pipe, &ovConnect, &dummy, TRUE);
          }
        }
      }
      CloseHandle(ovConnect.hEvent);

      if (!connected || stopRequested.load(std::memory_order_acquire)) {
        CloseHandle(pipe);
        continue;
      }

      auto session = std::make_unique<ClientSession>();
      ClientSession* sessionPtr = session.get();
      activeClients.fetch_add(1, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(clientsMutex);
        sessionPtr->thread = std::thread([this, pipe, sessionPtr]() {
          clientSessionLoop(pipe, sessionPtr);
        });
        clientSessions.push_back(std::move(session));
      }
    }

    if (pendingPipe != INVALID_HANDLE_VALUE) {
      CloseHandle(pendingPipe);
    }
    listening.store(false, std::memory_order_release);
  }
#else
  void clientSessionLoop(int clientFd, ClientSession* session) {
    struct Guard {
      Impl* self;
      int fd;
      ClientSession* s;
      ~Guard() {
        if (fd >= 0) {
          close(fd);
        }
        self->activeClients.fetch_sub(1, std::memory_order_release);
        if (s) s->finished.store(true, std::memory_order_release);
      }
    } guard{this, clientFd, session};

    while (!stopRequested.load(std::memory_order_acquire)) {
      nlohmann::json req;
      std::string ioErr;
      if (!readJsonFrame(clientFd, req, stopPipeReadFd, -1, &ioErr)) {
        break;
      }

      auto item = std::make_shared<PendingRequest>();
      item->request = std::move(req);
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push_back(item);
      }

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
      {
        std::unique_lock<std::mutex> lock(item->doneMutex);
        while (!item->done && !stopRequested.load(std::memory_order_acquire)) {
          if (std::chrono::steady_clock::now() >= deadline) {
            break;
          }
          item->doneCv.wait_for(lock, std::chrono::milliseconds(50));
        }
      }

      if (stopRequested.load(std::memory_order_acquire)) {
        break;
      }

      nlohmann::json resp;
      {
        std::lock_guard<std::mutex> lock(item->doneMutex);
        if (item->done) {
          resp = item->response;
        } else {
          resp = {
              {"jsonrpc", "2.0"},
              {"id", nullptr},
              {"error", {{"code", -32000}, {"message", "Timed out waiting for Studio main thread."}}},
          };
        }
      }

      if (!writeJsonFrame(clientFd, resp, stopPipeReadFd, 10000, &ioErr)) {
        break;
      }
      handledCount.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void acceptLoop() {
    while (!stopRequested.load(std::memory_order_acquire)) {
      reapFinishedClients();

      const int w = waitSocketOrStop(listenFd, stopPipeReadFd, POLLIN, -1);
      if (w == -2 || stopRequested.load(std::memory_order_acquire)) {
        break;
      }
      if (w <= 0) {
        continue;
      }

      for (;;) {
        const int clientFd = accept(listenFd, nullptr, nullptr);
        if (clientFd < 0) {
          if (errno == EINTR) {
            continue;
          }
          break;
        }
        setFdNonBlockingAndCloexec(clientFd);

        auto session = std::make_unique<ClientSession>();
        ClientSession* sessionPtr = session.get();
        activeClients.fetch_add(1, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lock(clientsMutex);
          sessionPtr->thread = std::thread([this, clientFd, sessionPtr]() {
            clientSessionLoop(clientFd, sessionPtr);
          });
          clientSessions.push_back(std::move(session));
        }
      }
    }
    listening.store(false, std::memory_order_release);
  }
#endif
};

LiveBridgeHost::LiveBridgeHost() : impl_(std::make_unique<Impl>()) {}

LiveBridgeHost::~LiveBridgeHost() {
  stop();
}

bool LiveBridgeHost::start(const std::string& pipeName) {
  stop();

  const std::string targetName = pipeName.empty() ? defaultLiveBridgePipeName() : pipeName;
#ifdef _WIN32
  HANDLE firstPipe = impl_->createListeningPipe(utf8ToWide(targetName), true);
  if (firstPipe == INVALID_HANDLE_VALUE) {
    return false;
  }

  impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!impl_->stopEvent) {
    CloseHandle(firstPipe);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->configMutex);
    impl_->pipeName = targetName;
  }
  impl_->stopRequested.store(false, std::memory_order_release);
  impl_->activeClients.store(0, std::memory_order_release);
  impl_->listening.store(true, std::memory_order_release);
  impl_->worker = std::thread([this, targetName, firstPipe]() { impl_->acceptLoop(targetName, firstPipe); });
  return true;
#else
  ensureSigpipeIgnored();

  sockaddr_un addr{};
  if (!fillUnixAddr(targetName, addr)) {
    return false;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  setFdNonBlockingAndCloexec(fd);

  auto tryBind = [&]() -> int {
    return ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0 ? 0 : errno;
  };

  int bindErr = tryBind();
  if (bindErr == EADDRINUSE) {
    // Probe whether an existing LiveBridgeHost is actively listening on this socket.
    const int probeFd = socket(AF_UNIX, SOCK_STREAM, 0);
    bool activeHost = false;
    if (probeFd >= 0) {
      setFdNonBlockingAndCloexec(probeFd);
      if (::connect(probeFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0 ||
          errno == EINPROGRESS || errno == EAGAIN) {
        activeHost = true;
      }
      close(probeFd);
    }
    if (activeHost) {
      close(fd);
      return false;
    }
    unlink(targetName.c_str());
    bindErr = tryBind();
  }
  if (bindErr != 0) {
    close(fd);
    return false;
  }

  chmod(targetName.c_str(), 0600);
  if (listen(fd, 16) != 0) {
    close(fd);
    unlink(targetName.c_str());
    return false;
  }

  int stopFds[2]{-1, -1};
  if (pipe(stopFds) != 0) {
    close(fd);
    unlink(targetName.c_str());
    return false;
  }
  setFdNonBlockingAndCloexec(stopFds[0]);
  setFdNonBlockingAndCloexec(stopFds[1]);

  {
    std::lock_guard<std::mutex> lock(impl_->configMutex);
    impl_->pipeName = targetName;
  }
  impl_->listenFd = fd;
  impl_->stopPipeReadFd = stopFds[0];
  impl_->stopPipeWriteFd = stopFds[1];
  impl_->stopRequested.store(false, std::memory_order_release);
  impl_->activeClients.store(0, std::memory_order_release);
  impl_->listening.store(true, std::memory_order_release);
  impl_->worker = std::thread([this]() { impl_->acceptLoop(); });
  return true;
#endif
}

void LiveBridgeHost::stop() {
  if (!impl_) {
    return;
  }
  impl_->stopRequested.store(true, std::memory_order_release);
#ifdef _WIN32
  if (impl_->stopEvent != nullptr) {
    SetEvent(impl_->stopEvent);
  }
#else
  if (impl_->stopPipeWriteFd >= 0) {
    const uint8_t b = 1;
    const ssize_t ignored = write(impl_->stopPipeWriteFd, &b, 1);
    (void)ignored;
  }
#endif
  {
    std::lock_guard<std::mutex> lock(impl_->queueMutex);
    while (!impl_->queue.empty()) {
      auto item = impl_->queue.front();
      impl_->queue.pop_front();
      std::lock_guard<std::mutex> dlock(item->doneMutex);
      item->done = true;
      item->response = {
          {"jsonrpc", "2.0"},
          {"id", nullptr},
          {"error", {{"code", -32000}, {"message", "MCP Live Bridge shutting down."}}},
      };
      item->doneCv.notify_all();
    }
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->joinAllClients();
#ifdef _WIN32
  if (impl_->stopEvent != nullptr) {
    CloseHandle(impl_->stopEvent);
    impl_->stopEvent = nullptr;
  }
#else
  if (impl_->listenFd >= 0) {
    close(impl_->listenFd);
    impl_->listenFd = -1;
  }
  if (impl_->stopPipeReadFd >= 0) {
    close(impl_->stopPipeReadFd);
    impl_->stopPipeReadFd = -1;
  }
  if (impl_->stopPipeWriteFd >= 0) {
    close(impl_->stopPipeWriteFd);
    impl_->stopPipeWriteFd = -1;
  }
  std::string boundPath;
  {
    std::lock_guard<std::mutex> lock(impl_->configMutex);
    boundPath = std::move(impl_->pipeName);
  }
  if (!boundPath.empty()) {
    unlink(boundPath.c_str());
  }
#endif
  impl_->listening.store(false, std::memory_order_release);
  impl_->activeClients.store(0, std::memory_order_release);
}

size_t LiveBridgeHost::pollAndDispatch(
    const std::function<nlohmann::json(const nlohmann::json&)>& handler,
    size_t maxRequests) {
  size_t processed = 0;
  while (processed < maxRequests) {
    std::shared_ptr<Impl::PendingRequest> item;
    {
      std::lock_guard<std::mutex> lock(impl_->queueMutex);
      if (impl_->queue.empty()) {
        break;
      }
      item = std::move(impl_->queue.front());
      impl_->queue.pop_front();
    }

    nlohmann::json resp;
    try {
      resp = handler(item->request);
    } catch (const std::exception& ex) {
      resp = {
          {"ok", false},
          {"error", ex.what()},
      };
    } catch (...) {
      resp = {
          {"ok", false},
          {"error", "Unknown exception inside live bridge request handler."},
      };
    }

    {
      std::lock_guard<std::mutex> dlock(item->doneMutex);
      item->response = std::move(resp);
      item->done = true;
    }
    item->doneCv.notify_all();
    ++processed;
  }
  return processed;
}

bool LiveBridgeHost::isListening() const noexcept {
  return impl_->listening.load(std::memory_order_acquire);
}

bool LiveBridgeHost::hasConnectedClient() const noexcept {
  return impl_->activeClients.load(std::memory_order_acquire) > 0;
}

uint64_t LiveBridgeHost::requestsHandled() const noexcept {
  return impl_->handledCount.load(std::memory_order_relaxed);
}

std::string LiveBridgeHost::pipeName() const {
  std::lock_guard<std::mutex> lock(impl_->configMutex);
  return impl_->pipeName;
}

struct LiveBridgeClient::Impl {
  mutable std::mutex mutex;
#ifdef _WIN32
  HANDLE pipe = INVALID_HANDLE_VALUE;
#else
  int sockFd = -1;
#endif
  std::string pipeName;
};

LiveBridgeClient::LiveBridgeClient() : impl_(std::make_unique<Impl>()) {}

LiveBridgeClient::~LiveBridgeClient() {
  disconnect();
}

bool LiveBridgeClient::connect(const std::string& pipeName,
                               uint32_t timeoutMs,
                               std::string* outError) {
  disconnect();

  const std::string targetName = pipeName.empty() ? defaultLiveBridgePipeName() : pipeName;
#ifdef _WIN32
  const std::wstring wideName = utf8ToWide(targetName);
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;

  HANDLE h = INVALID_HANDLE_VALUE;
  for (;;) {
    h = CreateFileW(wideName.c_str(),
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      break;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_BUSY) {
      const ULONGLONG now = GetTickCount64();
      if (now >= deadline) {
        if (outError) *outError = "Timed out waiting for Iota Studio MCP Live Bridge pipe.";
        return false;
      }
      const DWORD rem = static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 250));
      WaitNamedPipeW(wideName.c_str(), rem);
      continue;
    }
    if (outError) {
      *outError = "Could not connect to running Iota Studio MCP Live Bridge (" + targetName +
                  "). Enable MCP -> Enable Live Studio Bridge inside Iota Studio first.";
    }
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->pipe = h;
  impl_->pipeName = targetName;
  return true;
#else
  ensureSigpipeIgnored();
  sockaddr_un addr{};
  if (!fillUnixAddr(targetName, addr)) {
    if (outError) *outError = "Invalid Unix socket path for Iota Studio MCP Live Bridge.";
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  int fd = -1;
  for (;;) {
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      if (outError) *outError = "Failed to create Unix socket for Live Bridge.";
      return false;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
      setFdNonBlockingAndCloexec(fd);
      break;
    }
    const int err = errno;
    close(fd);
    fd = -1;

    if ((err == EAGAIN || err == EWOULDBLOCK || err == ECONNREFUSED || err == ENOENT) &&
        std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    if (outError) {
      *outError = "Could not connect to running Iota Studio MCP Live Bridge (" + targetName +
                  "). Enable MCP -> Enable Live Studio Bridge inside Iota Studio first.";
    }
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->sockFd = fd;
  impl_->pipeName = targetName;
  return true;
#endif
}

void LiveBridgeClient::disconnect() {
  if (!impl_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
#ifdef _WIN32
  if (impl_->pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->pipe);
    impl_->pipe = INVALID_HANDLE_VALUE;
  }
#else
  if (impl_->sockFd >= 0) {
    close(impl_->sockFd);
    impl_->sockFd = -1;
  }
#endif
  impl_->pipeName.clear();
}

bool LiveBridgeClient::isConnected() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
#ifdef _WIN32
  return impl_->pipe != INVALID_HANDLE_VALUE;
#else
  return impl_->sockFd >= 0;
#endif
}

std::string LiveBridgeClient::connectedPipeName() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->pipeName;
}

bool LiveBridgeClient::sendRequest(const nlohmann::json& request,
                                   nlohmann::json& outResponse,
                                   std::string* outError) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
#ifdef _WIN32
  if (impl_->pipe == INVALID_HANDLE_VALUE) {
    if (outError) *outError = "Not connected to Iota Studio Live Bridge.";
    return false;
  }

  if (!writeJsonFrame(impl_->pipe, request, nullptr, 10000, outError) ||
      !readJsonFrame(impl_->pipe, outResponse, nullptr, 300000, outError)) {
    CloseHandle(impl_->pipe);
    impl_->pipe = INVALID_HANDLE_VALUE;
    impl_->pipeName.clear();
    return false;
  }
  return true;
#else
  if (impl_->sockFd < 0) {
    if (outError) *outError = "Not connected to Iota Studio Live Bridge.";
    return false;
  }

  if (!writeJsonFrame(impl_->sockFd, request, -1, 10000, outError) ||
      !readJsonFrame(impl_->sockFd, outResponse, -1, 300000, outError)) {
    close(impl_->sockFd);
    impl_->sockFd = -1;
    impl_->pipeName.clear();
    return false;
  }
  return true;
#endif
}

}  // namespace iota::mcp
