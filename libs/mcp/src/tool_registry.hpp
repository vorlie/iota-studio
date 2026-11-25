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
