#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace iota::core {

/// Validates RFC 3629 UTF-8 and replaces any malformed byte sequence with U+FFFD.
std::string sanitizeUtf8(std::string_view input);

/// Strips a leading UTF-8 BOM (0xEF 0xBB 0xBF) if present.
std::string_view stripUtf8Bom(std::string_view input) noexcept;

/// Sanitizes to UTF-8 and truncates cleanly on Unicode codepoint boundaries.
std::string truncateUtf8(std::string_view input,
                         size_t maxCodepoints,
                         std::string_view ellipsis = "..");

/// Converts a filesystem path to valid UTF-8 without throwing std::system_error.
std::string pathToUtf8(const std::filesystem::path& path);

/// Converts a UTF-8 (or legacy ACP fallback on Windows) string to std::filesystem::path safely.
std::filesystem::path pathFromUtf8(std::string_view utf8);

/// Safe UTF-8 accessors for path components.
std::string pathFilenameUtf8(const std::filesystem::path& path);
std::string pathStemUtf8(const std::filesystem::path& path);
std::string pathExtensionUtf8(const std::filesystem::path& path);

/// Extracts the filename component after '/' or '\\' and sanitizes to UTF-8.
std::string baseNameFromSlashPath(std::string_view raw);

/// Checks std::filesystem::exists without throwing exceptions.
bool pathExistsNoThrow(const std::filesystem::path& path) noexcept;

}  // namespace iota::core
