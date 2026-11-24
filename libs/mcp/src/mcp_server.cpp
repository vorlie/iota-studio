#include "iota/mcp/mcp_server.hpp"

#include "tool_registry.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace iota::mcp {

namespace {

constexpr const char* kDefaultProtocolVersion = "2024-11-05";

nlohmann::json makeJsonRpcResult(const nlohmann::json& id, nlohmann::json result) {
  return {
      {"jsonrpc", "2.0"},
      {"id", id},
      {"result", std::move(result)},
  };
}

nlohmann::json makeJsonRpcError(const nlohmann::json& id, int code, const std::string& message) {
  return {
      {"jsonrpc", "2.0"},
      {"id", id},
      {"error", {{"code", code}, {"message", message}}},
  };
}

nlohmann::json makeToolErrorResult(const std::string& message) {
  return {
      {"content", nlohmann::json::array({{{"type", "text"}, {"text", message}}})},
      {"isError", true},
  };
}

nlohmann::json makeToolTextResult(const nlohmann::json& payload) {
  return {
      {"content",
       nlohmann::json::array({
           {{"type", "text"},
            {"text", payload.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)}},
       })},
      {"isError", false},
  };
}

std::string negotiateProtocolVersion(const nlohmann::json& params) {
  if (params.is_object() && params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
    const std::string reqVer = params["protocolVersion"].get<std::string>();
    if (reqVer == "2024-11-05" || reqVer == "2025-03-26" || reqVer == "2025-06-18") {
      return reqVer;
    }
  }
  return kDefaultProtocolVersion;
}

}  // namespace

nlohmann::json executeTargetBridgeRequest(IStudioBridgeTarget& target,
                                            const nlohmann::json& request,
                                            uint64_t liveRequestsHandled) {
  if (!request.is_object() || !request.contains("method") || !request["method"].is_string()) {
    return makeJsonRpcError(nullptr, -32600, "Invalid bridge request: missing method.");
  }

  const nlohmann::json id = request.contains("id") ? request["id"] : nlohmann::json(nullptr);
  const std::string method = request["method"].get<std::string>();
  const nlohmann::json params =
      request.contains("params") && request["params"].is_object()
          ? request["params"]
          : nlohmann::json::object();

  if (method == "ping") {
    return makeJsonRpcResult(id, nlohmann::json::object());
  }

  if (method == "tools/list") {
    return makeJsonRpcResult(id, {{"tools", detail::listMcpToolDefinitions()}});
  }

  if (method == "resources/list") {
    return makeJsonRpcResult(id, {{"resources", detail::listMcpResourceDefinitions()}});
  }

  if (method == "resources/read") {
    if (!params.contains("uri") || !params["uri"].is_string()) {
      return makeJsonRpcError(id, -32602, "resources/read requires a string 'uri' parameter.");
    }
    const std::string uri = params["uri"].get<std::string>();
    bool found = false;
    std::string err;
    nlohmann::json contents = detail::readMcpResource(target, uri, found, err);
    if (!found) {
      return makeJsonRpcError(id, -32602, err.empty() ? ("Unknown resource URI: " + uri) : err);
    }
    return makeJsonRpcResult(id, {{"contents", std::move(contents)}});
  }

  if (method == "tools/call") {
    if (!params.contains("name") || !params["name"].is_string()) {
      return makeJsonRpcError(id, -32602, "tools/call requires a string 'name' parameter.");
    }
    const std::string toolName = params["name"].get<std::string>();
    const nlohmann::json args =
        (params.contains("arguments") && params["arguments"].is_object())
            ? params["arguments"]
            : nlohmann::json::object();

    try {
      bool found = false;
      nlohmann::json result = detail::executeMcpTool(target, toolName, args, liveRequestsHandled, found);
      if (!found) {
        return makeJsonRpcError(id, -32601, "Unknown tool: " + toolName);
      }
      return makeJsonRpcResult(id, std::move(result));
    } catch (const std::exception& ex) {
      return makeJsonRpcResult(id, makeToolErrorResult(std::string("Tool execution exception: ") + ex.what()));
    } catch (...) {
      return makeJsonRpcResult(id, makeToolErrorResult("Unknown internal exception during tool execution."));
    }
  }

  return makeJsonRpcError(id, -32601, "Unsupported method: " + method);
}

McpServer::McpServer(IStudioBridgeTarget& localTarget, LiveBridgeClient* liveClient)
    : localTarget_(localTarget), liveClient_(liveClient) {}

std::optional<nlohmann::json> McpServer::handleJsonRpcMessage(const nlohmann::json& msg) {
  if (!msg.is_object()) {
    return makeJsonRpcError(nullptr, -32600, "JSON-RPC message must be an object.");
  }

  const bool hasId = msg.contains("id") && !msg["id"].is_null();
  const nlohmann::json id = msg.contains("id") ? msg["id"] : nlohmann::json(nullptr);

  if (!msg.contains("method") || !msg["method"].is_string()) {
    if (!hasId) {
      return std::nullopt;
    }
    return makeJsonRpcError(id, -32600, "Missing or non-string 'method'.");
  }

  const std::string method = msg["method"].get<std::string>();
  const nlohmann::json params =
      (msg.contains("params") && msg["params"].is_object())
          ? msg["params"]
          : nlohmann::json::object();

  // Notifications carry no id or expect no response
  if (method == "notifications/initialized" ||
      method == "notifications/cancelled" ||
      (!hasId && method.rfind("notifications/", 0) == 0)) {
    return std::nullopt;
  }

  if (method == "initialize") {
    nlohmann::json res = {
        {"protocolVersion", negotiateProtocolVersion(params)},
        {"capabilities",
         {
             {"tools", {{"listChanged", false}}},
             {"resources", {{"listChanged", false}}},
         }},
        {"serverInfo",
         {
             {"name", "iota-studio-mcp"},
             {"version", IOTA_STUDIO_VERSION},
         }},
        {"instructions",
         "Iota Studio MCP controls visualizers, layers, MVar modulation, properties, custom shaders, "
         "headless OpenGL preview PNGs, GPU video export, and optional live Studio sessions."},
    };
    return makeJsonRpcResult(id, std::move(res));
  }

  if (method == "ping") {
    return makeJsonRpcResult(id, nlohmann::json::object());
  }

  if (method == "tools/list") {
    return makeJsonRpcResult(id, {{"tools", detail::listMcpToolDefinitions()}});
  }

  if (method == "resources/list") {
    return makeJsonRpcResult(id, {{"resources", detail::listMcpResourceDefinitions()}});
  }

  if (method == "resources/read") {
    const std::string uri =
        (params.contains("uri") && params["uri"].is_string()) ? params["uri"].get<std::string>() : "";
    if (uri == "iota://project/current" && liveClient_ != nullptr && liveClient_->isConnected()) {
      nlohmann::json pipeResp;
      std::string err;
      if (!liveClient_->sendRequest({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}},
                                    pipeResp,
                                    &err)) {
        return makeJsonRpcError(id, -32001, err);
      }
      pipeResp["id"] = id;
      return pipeResp;
    }
    return executeTargetBridgeRequest(localTarget_,
                                      {{"id", id}, {"method", method}, {"params", params}});
  }

  if (method == "tools/call") {
    if (!params.contains("name") || !params["name"].is_string()) {
      return makeJsonRpcError(id, -32602, "tools/call requires a string 'name' parameter.");
    }
    const std::string toolName = params["name"].get<std::string>();
    const nlohmann::json args =
        (params.contains("arguments") && params["arguments"].is_object())
            ? params["arguments"]
            : nlohmann::json::object();

    if (toolName == "studio_connect") {
      if (liveClient_ == nullptr) {
        return makeJsonRpcResult(
            id, makeToolErrorResult("Live bridge client is not enabled in this server instance."));
      }
      const std::string pipe =
          (args.contains("pipe_name") && args["pipe_name"].is_string())
              ? args["pipe_name"].get<std::string>()
              : "";
      const uint32_t timeoutMs =
          (args.contains("timeout_ms") && args["timeout_ms"].is_number_unsigned())
              ? std::min<uint32_t>(args["timeout_ms"].get<uint32_t>(), 10000u)
              : 1500u;
      std::string err;
      if (!liveClient_->connect(pipe, timeoutMs, &err)) {
        return makeJsonRpcResult(id, makeToolErrorResult(err));
      }
      return makeJsonRpcResult(
          id,
          makeToolTextResult({
              {"connected", true},
              {"mode", "live_studio"},
              {"pipe_name", liveClient_->connectedPipeName()},
          }));
    }

    if (toolName == "studio_disconnect") {
      const bool wasConnected = (liveClient_ != nullptr && liveClient_->isConnected());
      if (liveClient_ != nullptr) {
        liveClient_->disconnect();
      }
      return makeJsonRpcResult(
          id,
          makeToolTextResult({
              {"connected", false},
              {"was_connected", wasConnected},
              {"mode", "headless"},
          }));
    }

    if (liveClient_ != nullptr && liveClient_->isConnected()) {
      nlohmann::json pipeResp;
      std::string err;
      if (!liveClient_->sendRequest(
              {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", params}},
              pipeResp,
              &err)) {
        return makeJsonRpcResult(
            id,
            makeToolErrorResult(
                err + " Call `studio_disconnect` to switch back to standalone headless mode or `studio_connect` after re-enabling Live Bridge in Studio."));
      }
      pipeResp["id"] = id;
      if (toolName == "studio_status" && pipeResp.contains("result") &&
          pipeResp["result"].contains("content") && pipeResp["result"]["content"].is_array() &&
          !pipeResp["result"]["content"].empty() &&
          pipeResp["result"]["content"][0].contains("text")) {
        auto parsedStatus = nlohmann::json::parse(
            pipeResp["result"]["content"][0]["text"].get<std::string>(), nullptr, false);
        if (parsedStatus.is_object()) {
          parsedStatus["client_attached"] = true;
          parsedStatus["pipe_name"] = liveClient_->connectedPipeName();
          pipeResp["result"]["content"][0]["text"] =
              parsedStatus.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        }
      }
      return pipeResp;
    }

    return executeTargetBridgeRequest(localTarget_,
                                      {{"id", id}, {"method", "tools/call"}, {"params", params}});
  }

  if (!hasId) {
    return std::nullopt;
  }
  return makeJsonRpcError(id, -32601, "Method not found: " + method);
}

int McpServer::runStdioLoop(std::istream& in, std::ostream& out) {
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    bool onlySpace = true;
    for (char c : line) {
      if (c != ' ' && c != '\t') {
        onlySpace = false;
        break;
      }
    }
    if (onlySpace) {
      continue;
    }

    auto parsed = nlohmann::json::parse(line, nullptr, false);
    if (parsed.is_discarded()) {
      const auto errResp = makeJsonRpcError(nullptr, -32700, "Parse error: invalid JSON.");
      out << errResp.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
      out.flush();
      continue;
    }

    try {
      const auto resp = handleJsonRpcMessage(parsed);
      if (resp.has_value()) {
        out << resp->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
        out.flush();
      }
    } catch (const std::exception& ex) {
      std::cerr << "[iota-mcp] Unhandled error processing message: " << ex.what() << "\n";
      const nlohmann::json errId = parsed.is_object() && parsed.contains("id") ? parsed["id"] : nlohmann::json(nullptr);
      const auto errResp = makeJsonRpcError(errId, -32603, std::string("Internal error: ") + ex.what());
      out << errResp.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
      out.flush();
    }
  }
  return 0;
}

}  // namespace iota::mcp
