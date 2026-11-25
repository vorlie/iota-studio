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

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace iota::mcp {

/// Returns the default per-user local Windows Named Pipe name (`\\.\pipe\iota_studio_mcp_<user>`).
[[nodiscard]] std::string defaultLiveBridgePipeName();

/// Multi-threaded Named Pipe host embedded in `iota_studio` (main thread drains queued RPCs via `pollAndDispatch`).
class LiveBridgeHost {
 public:
  LiveBridgeHost();
  ~LiveBridgeHost();

  LiveBridgeHost(const LiveBridgeHost&) = delete;
  LiveBridgeHost& operator=(const LiveBridgeHost&) = delete;

  bool start(const std::string& pipeName = defaultLiveBridgePipeName());
  void stop();

  /// Executes up to `maxRequests` queued JSON-RPC requests on the caller's thread (Studio main thread).
  size_t pollAndDispatch(const std::function<nlohmann::json(const nlohmann::json&)>& handler,
                         size_t maxRequests = 8);

  [[nodiscard]] bool isListening() const noexcept;
  [[nodiscard]] bool hasConnectedClient() const noexcept;
  [[nodiscard]] uint64_t requestsHandled() const noexcept;
  [[nodiscard]] std::string pipeName() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Named Pipe client used by `iota_mcp` when attached to a running Iota Studio instance.
class LiveBridgeClient {
 public:
  LiveBridgeClient();
  ~LiveBridgeClient();

  LiveBridgeClient(const LiveBridgeClient&) = delete;
  LiveBridgeClient& operator=(const LiveBridgeClient&) = delete;

  bool connect(const std::string& pipeName = defaultLiveBridgePipeName(),
               uint32_t timeoutMs = 1500,
               std::string* outError = nullptr);
  void disconnect();

  [[nodiscard]] bool isConnected() const noexcept;
  [[nodiscard]] std::string connectedPipeName() const;

  bool sendRequest(const nlohmann::json& request,
                   nlohmann::json& outResponse,
                   std::string* outError = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace iota::mcp
