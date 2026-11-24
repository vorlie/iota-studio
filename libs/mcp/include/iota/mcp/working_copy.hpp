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
