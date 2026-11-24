#include "iota/mcp/live_bridge.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace iota::mcp {

namespace {

constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

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

}  // namespace

std::string defaultLiveBridgePipeName() {
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
}

struct LiveBridgeHost::Impl {
  struct PendingRequest {
    nlohmann::json request;
    std::mutex doneMutex;
    std::condition_variable doneCv;
    bool done = false;
    nlohmann::json response;
  };

  mutable std::mutex configMutex;
  std::string pipeName;

  HANDLE stopEvent = nullptr;
  std::atomic<bool> stopRequested{false};
  std::atomic<bool> listening{false};
  std::atomic<bool> clientConnected{false};
  std::atomic<uint64_t> handledCount{0};
  std::thread worker;

  std::mutex queueMutex;
  std::deque<std::shared_ptr<PendingRequest>> queue;

  HANDLE createListeningPipe(const std::wstring& wideName) const {
    return CreateNamedPipeW(
        wideName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        4,
        65536,
        65536,
        1000,
        nullptr);
  }

  void workerLoop(std::string targetPipeName) {
    const std::wstring wideName = utf8ToWide(targetPipeName);
    while (!stopRequested.load(std::memory_order_acquire)) {
      HANDLE pipe = createListeningPipe(wideName);
      if (pipe == INVALID_HANDLE_VALUE) {
        Sleep(100);
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

      clientConnected.store(true, std::memory_order_release);
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

      clientConnected.store(false, std::memory_order_release);
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
    }

    listening.store(false, std::memory_order_release);
  }
};

LiveBridgeHost::LiveBridgeHost() : impl_(std::make_unique<Impl>()) {}

LiveBridgeHost::~LiveBridgeHost() {
  stop();
}

bool LiveBridgeHost::start(const std::string& pipeName) {
  stop();

  const std::string targetName = pipeName.empty() ? defaultLiveBridgePipeName() : pipeName;
  HANDLE testPipe = impl_->createListeningPipe(utf8ToWide(targetName));
  if (testPipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  CloseHandle(testPipe);

  impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!impl_->stopEvent) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->configMutex);
    impl_->pipeName = targetName;
  }
  impl_->stopRequested.store(false, std::memory_order_release);
  impl_->clientConnected.store(false, std::memory_order_release);
  impl_->listening.store(true, std::memory_order_release);
  impl_->worker = std::thread([this, targetName]() { impl_->workerLoop(targetName); });
  return true;
}

void LiveBridgeHost::stop() {
  if (!impl_) {
    return;
  }
  impl_->stopRequested.store(true, std::memory_order_release);
  if (impl_->stopEvent != nullptr) {
    SetEvent(impl_->stopEvent);
  }
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
  if (impl_->stopEvent != nullptr) {
    CloseHandle(impl_->stopEvent);
    impl_->stopEvent = nullptr;
  }
  impl_->listening.store(false, std::memory_order_release);
  impl_->clientConnected.store(false, std::memory_order_release);
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
  return impl_->clientConnected.load(std::memory_order_acquire);
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
  HANDLE pipe = INVALID_HANDLE_VALUE;
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
}

void LiveBridgeClient::disconnect() {
  if (!impl_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->pipe);
    impl_->pipe = INVALID_HANDLE_VALUE;
  }
  impl_->pipeName.clear();
}

bool LiveBridgeClient::isConnected() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->pipe != INVALID_HANDLE_VALUE;
}

std::string LiveBridgeClient::connectedPipeName() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->pipeName;
}

bool LiveBridgeClient::sendRequest(const nlohmann::json& request,
                                   nlohmann::json& outResponse,
                                   std::string* outError) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
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
}

}  // namespace iota::mcp
