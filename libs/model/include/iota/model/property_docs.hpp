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

#include "iota/model/scene.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace iota::model {

struct PropertyDoc {
  std::string_view rawKey;
  std::string_view displayName;
  std::string_view summary;
  std::string_view usage;
  std::string_view unit;
};

struct GroupDoc {
  std::string_view rawTag;
  std::string_view displayName;
  std::string_view summary;
};

struct ElementDoc {
  ElementType type = ElementType::Unknown;
  std::string_view displayName;
  std::string_view category;
  std::string_view overview;
  std::string_view tips;
};

struct MVarModeDoc {
  std::string_view mode;
  std::string_view displayName;
  std::string_view summary;
  std::string_view paramA;
  std::string_view paramB;
};

struct TextTokenDoc {
  std::string_view token;
  std::string_view summary;
};

/// Finds documentation for a property, checking element-specific overrides before shared base keys.
[[nodiscard]] const PropertyDoc* findPropertyDoc(ElementType ownerType, std::string_view rawKey) noexcept;

/// Returns registered human-readable display name or formats an unknown key cleanly into Title Case.
[[nodiscard]] std::string formatPropertyDisplayName(ElementType ownerType, std::string_view rawKey);

/// Finds documentation for an Inspector property section tag (e.g., "2_spectrumHz").
[[nodiscard]] const GroupDoc* findGroupDoc(std::string_view rawGroupTag) noexcept;

/// Returns registered section title or formats an unknown group tag into clean Title Case.
[[nodiscard]] std::string formatGroupDisplayName(std::string_view rawGroupTag);

/// Returns overview documentation for an element type (or Composition).
[[nodiscard]] const ElementDoc& getElementDoc(ElementType type) noexcept;

/// Ordered list of all documented Element & Composition pages for the Built-in Documentation panel.
[[nodiscard]] const std::vector<ElementDoc>& allElementDocs() noexcept;

/// Finds documentation for an MVar signal/driver mode (such as "Beat" or "SoundKeys1").
[[nodiscard]] const MVarModeDoc* findMVarModeDoc(std::string_view mode) noexcept;

/// All documented MVar signal modes matching `allMeasureWhatModes()`.
[[nodiscard]] const std::vector<MVarModeDoc>& allMVarModeDocs() noexcept;

/// All supported dynamic text placeholder tokens for Text/StatText layers.
[[nodiscard]] const std::vector<TextTokenDoc>& allTextTokenDocs() noexcept;

}  // namespace iota::model
