#include "iota/core/utf8_path.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <exception>
#include <limits>
#include <system_error>

namespace iota::core {

namespace {

constexpr std::string_view kReplacementChar = "\xEF\xBF\xBD";

bool decodeNextUtf8Codepoint(std::string_view input, size_t pos, size_t& outLen) noexcept {
  const uint8_t b0 = static_cast<uint8_t>(input[pos]);
  if (b0 < 0x80u) {
    outLen = 1;
    return true;
  }
  if (b0 >= 0xC2u && b0 <= 0xDFu) {
    if (pos + 1 >= input.size()) return false;
    const uint8_t b1 = static_cast<uint8_t>(input[pos + 1]);
    if ((b1 & 0xC0u) != 0x80u) return false;
    outLen = 2;
    return true;
  }
  if (b0 >= 0xE0u && b0 <= 0xEFu) {
    if (pos + 2 >= input.size()) return false;
    const uint8_t b1 = static_cast<uint8_t>(input[pos + 1]);
    const uint8_t b2 = static_cast<uint8_t>(input[pos + 2]);
    if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) return false;
    // Reject overlong 3-byte sequences and UTF-16 surrogate codepoints U+D800..U+DFFF
    if (b0 == 0xE0u && b1 < 0xA0u) return false;
    if (b0 == 0xEDu && b1 >= 0xA0u) return false;
    outLen = 3;
    return true;
  }
  if (b0 >= 0xF0u && b0 <= 0xF4u) {
    if (pos + 3 >= input.size()) return false;
    const uint8_t b1 = static_cast<uint8_t>(input[pos + 1]);
    const uint8_t b2 = static_cast<uint8_t>(input[pos + 2]);
    const uint8_t b3 = static_cast<uint8_t>(input[pos + 3]);
    if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) return false;
    // Reject overlong 4-byte sequences and codepoints > U+10FFFF
    if (b0 == 0xF0u && b1 < 0x90u) return false;
    if (b0 == 0xF4u && b1 > 0x8Fu) return false;
    outLen = 4;
    return true;
  }
  return false;
}

}  // namespace

std::string sanitizeUtf8(std::string_view input) {
  std::string out;
  out.reserve(input.size());

  size_t i = 0;
  while (i < input.size()) {
    size_t cpLen = 0;
    if (decodeNextUtf8Codepoint(input, i, cpLen)) {
      out.append(input.substr(i, cpLen));
      i += cpLen;
    } else {
      out.append(kReplacementChar);
      ++i;
    }
  }
  return out;
}

std::string_view stripUtf8Bom(std::string_view input) noexcept {
  if (input.size() >= 3 &&
      static_cast<uint8_t>(input[0]) == 0xEFu &&
      static_cast<uint8_t>(input[1]) == 0xBBu &&
      static_cast<uint8_t>(input[2]) == 0xBFu) {
    return input.substr(3);
  }
  return input;
}

std::string truncateUtf8(std::string_view input,
                         size_t maxCodepoints,
                         std::string_view ellipsis) {
  const std::string clean = sanitizeUtf8(input);
  size_t count = 0;
  size_t pos = 0;
  size_t cutBytePos = clean.size();

  while (pos < clean.size()) {
    size_t cpLen = 1;
    if (!decodeNextUtf8Codepoint(clean, pos, cpLen)) {
      cpLen = 1;
    }
    ++count;
    if (count == maxCodepoints) {
      cutBytePos = pos + cpLen;
    }
    pos += cpLen;
  }

  if (count <= maxCodepoints) {
    return clean;
  }
  if (maxCodepoints == 0) {
    return sanitizeUtf8(ellipsis);
  }
  return clean.substr(0, cutBytePos) + sanitizeUtf8(ellipsis);
}

std::string pathToUtf8(const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
#ifdef _WIN32
  const std::wstring& wide = path.native();
  if (wide.empty()) {
    return {};
  }
  if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  const int wideLen = static_cast<int>(wide.size());
  // Flags = 0 lets WideCharToMultiByte replace any lone UTF-16 surrogates with U+FFFD safely.
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string utf8(static_cast<size_t>(needed), '\0');
  const int written =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, utf8.data(), needed, nullptr, nullptr);
  if (written <= 0) {
    return {};
  }
  utf8.resize(static_cast<size_t>(written));
  return sanitizeUtf8(utf8);
#else
  try {
    const auto u8 = path.u8string();
    return sanitizeUtf8(std::string_view(reinterpret_cast<const char*>(u8.data()), u8.size()));
  } catch (const std::exception&) {
    return sanitizeUtf8(path.native());
  }
#endif
}

std::filesystem::path pathFromUtf8(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
#ifdef _WIN32
  if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  const int srcLen = static_cast<int>(utf8.size());
  auto convertWithCodePage = [](UINT cp, DWORD flags, const char* data, int len) -> std::wstring {
    const int needed = MultiByteToWideChar(cp, flags, data, len, nullptr, 0);
    if (needed <= 0) {
      return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(cp, flags, data, len, wide.data(), needed);
    if (written <= 0) {
      return {};
    }
    wide.resize(static_cast<size_t>(written));
    return wide;
  };

  std::wstring wide = convertWithCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), srcLen);
  if (wide.empty()) {
    wide = convertWithCodePage(CP_ACP, 0, utf8.data(), srcLen);
  }
  if (wide.empty()) {
    const std::string clean = sanitizeUtf8(utf8);
    if (!clean.empty() && clean.size() <= static_cast<size_t>(std::numeric_limits<int>::max())) {
      wide = convertWithCodePage(CP_UTF8, 0, clean.data(), static_cast<int>(clean.size()));
    }
  }
  return std::filesystem::path(std::move(wide));
#else
  return std::filesystem::path(std::string(utf8));
#endif
}

std::string pathFilenameUtf8(const std::filesystem::path& path) {
  return pathToUtf8(path.filename());
}

std::string pathStemUtf8(const std::filesystem::path& path) {
  return pathToUtf8(path.stem());
}

std::string pathExtensionUtf8(const std::filesystem::path& path) {
  return pathToUtf8(path.extension());
}

std::string baseNameFromSlashPath(std::string_view raw) {
  const size_t slashPos = raw.find_last_of("/\\");
  const std::string_view base = (slashPos == std::string_view::npos) ? raw : raw.substr(slashPos + 1);
  return sanitizeUtf8(base);
}

bool pathExistsNoThrow(const std::filesystem::path& path) noexcept {
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

}  // namespace iota::core
