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

#include <filesystem>

namespace iota::mcp {

/// Case-insensitive check whether `path` has a legacy `.viz` extension.
[[nodiscard]] bool isVizExtension(const std::filesystem::path& path);

/// Returns true if `path` is already an MCP working-copy `.iota` file (`*.mcp.iota` or `*.mcp_<n>.iota`).
[[nodiscard]] bool isMcpWorkingCopyPath(const std::filesystem::path& path);

/// Computes a safe sibling `<stem>.mcp.iota` (or `<stem>.mcp_2.iota`, ...) path next to `originalPath`.
/// Never stacks `.mcp.mcp.iota` and never emits `.viz`.
[[nodiscard]] std::filesystem::path computeMcpWorkingCopyPath(const std::filesystem::path& originalPath);

}  // namespace iota::mcp
