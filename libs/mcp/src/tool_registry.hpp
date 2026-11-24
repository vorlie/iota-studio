#pragma once

#include "iota/mcp/studio_session.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string_view>

namespace iota::mcp::detail {

[[nodiscard]] nlohmann::json listMcpToolDefinitions();
[[nodiscard]] nlohmann::json listMcpResourceDefinitions();

[[nodiscard]] nlohmann::json readMcpResource(IStudioBridgeTarget& target,
                                             std::string_view uri,
                                             bool& outFound,
                                             std::string& outError);

[[nodiscard]] nlohmann::json executeMcpTool(IStudioBridgeTarget& target,
                                            std::string_view name,
                                            const nlohmann::json& args,
                                            uint64_t liveRequestsHandled,
                                            bool& outFound);

}  // namespace iota::mcp::detail
