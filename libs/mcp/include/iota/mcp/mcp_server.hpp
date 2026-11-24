#pragma once

#include "iota/mcp/live_bridge.hpp"
#include "iota/mcp/studio_session.hpp"

#include <nlohmann/json.hpp>

#include <iosfwd>
#include <optional>

namespace iota::mcp {

/// Dispatches direct MCP requests (`tools/list`, `tools/call`, `resources/list`, `resources/read`, `ping`)
/// against a concrete `IStudioBridgeTarget` (used on the main thread by LiveBridgeHost and internally by McpServer).
[[nodiscard]] nlohmann::json executeTargetBridgeRequest(IStudioBridgeTarget& target,
                                                        const nlohmann::json& request,
                                                        uint64_t liveRequestsHandled = 0);

/// Spec-compliant JSON-RPC 2.0 MCP server operating over stdio or direct unit calls,
/// with optional forwarding to a running Iota Studio Live Bridge.
class McpServer {
 public:
  explicit McpServer(IStudioBridgeTarget& localTarget,
                     LiveBridgeClient* liveClient = nullptr);

  /// Handles a single parsed JSON-RPC 2.0 request/notification.
  /// Returns `std::nullopt` for JSON-RPC notifications (no response required).
  [[nodiscard]] std::optional<nlohmann::json> handleJsonRpcMessage(const nlohmann::json& msg);

  /// Runs the newline-delimited JSON-RPC stdio loop until EOF. Writes diagnostics strictly to stderr.
  int runStdioLoop(std::istream& in, std::ostream& out);

 private:
  IStudioBridgeTarget& localTarget_;
  LiveBridgeClient* liveClient_ = nullptr;
};

}  // namespace iota::mcp
