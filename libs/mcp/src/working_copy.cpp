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

#include "iota/mcp/working_copy.hpp"

#include "iota/core/utf8_path.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace iota::mcp {

namespace {

bool eqIgnoreCase(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool endsWithIgnoreCase(std::string_view str, std::string_view suffix) noexcept {
  if (str.size() < suffix.size()) return false;
  return eqIgnoreCase(str.substr(str.size() - suffix.size()), suffix);
}

bool hasMcpStemSuffix(std::string_view stem) noexcept {
  if (endsWithIgnoreCase(stem, ".mcp")) {
    return true;
  }
  const size_t dotPos = stem.rfind(".mcp_");
  if (dotPos != std::string_view::npos) {
    const std::string_view digits = stem.substr(dotPos + 5);
    if (!digits.empty()) {
      for (char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

std::string stripMcpStemSuffix(std::string_view stem) {
  while (!stem.empty()) {
    if (endsWithIgnoreCase(stem, ".mcp")) {
      stem.remove_suffix(4);
      continue;
    }
    const size_t dotPos = stem.rfind(".mcp_");
    if (dotPos != std::string_view::npos) {
      const std::string_view digits = stem.substr(dotPos + 5);
      bool allDigits = !digits.empty();
      for (char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
          allDigits = false;
          break;
        }
      }
      if (allDigits) {
        stem = stem.substr(0, dotPos);
        continue;
      }
    }
    break;
  }
  return std::string(stem);
}

}  // namespace

bool isVizExtension(const std::filesystem::path& path) {
  return eqIgnoreCase(iota::core::pathExtensionUtf8(path), ".viz");
}

bool isMcpWorkingCopyPath(const std::filesystem::path& path) {
  if (!eqIgnoreCase(iota::core::pathExtensionUtf8(path), ".iota")) {
    return false;
  }
  return hasMcpStemSuffix(iota::core::pathStemUtf8(path));
}

std::filesystem::path computeMcpWorkingCopyPath(const std::filesystem::path& originalPath) {
  if (originalPath.empty()) {
    return {};
  }
  std::string base = stripMcpStemSuffix(iota::core::pathStemUtf8(originalPath));
  if (base.empty()) {
    base = "project";
  }

  const std::filesystem::path parent = originalPath.parent_path();
  std::filesystem::path candidate = parent / iota::core::pathFromUtf8(base + ".mcp.iota");
  if (!iota::core::pathExistsNoThrow(candidate)) {
    return candidate;
  }

  for (int idx = 2; idx <= 9999; ++idx) {
    std::filesystem::path next =
        parent / iota::core::pathFromUtf8(base + ".mcp_" + std::to_string(idx) + ".iota");
    if (!iota::core::pathExistsNoThrow(next)) {
      return next;
    }
  }
  return candidate;
}

}  // namespace iota::mcp
