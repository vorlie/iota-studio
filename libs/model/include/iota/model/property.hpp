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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "iota/core/types.hpp"
#include "iota/model/measured_var.hpp"

namespace iota::model {

using PropertyValue = std::variant<std::monostate, bool, int64_t, double, std::string>;

struct PropertyEntry;
struct PropertyChild;

struct NamedProperty {
  std::string name;
  PropertyEntry* ptr = nullptr; // unused placeholder; see PropertyBag
};

struct PropertyEntry {
  using ChildItem = PropertyChild;

  PropertyValue value;
  std::string typeTag;
  int order = 0;
  std::string groupTag;
  std::optional<std::string> hint;
  std::vector<ChildItem> children;

  PropertyEntry();
  ~PropertyEntry();
  PropertyEntry(const PropertyEntry&);
  PropertyEntry(PropertyEntry&&) noexcept;
  PropertyEntry& operator=(const PropertyEntry&);
  PropertyEntry& operator=(PropertyEntry&&) noexcept;

  const PropertyEntry* findChild(std::string_view key) const noexcept;
  PropertyEntry* findChild(std::string_view key) noexcept;
  PropertyEntry& getOrAddChild(std::string_view key);
};

struct PropertyChild {
  std::string key;
  PropertyEntry entry;
};

inline PropertyEntry::PropertyEntry() = default;
inline PropertyEntry::~PropertyEntry() = default;
inline PropertyEntry::PropertyEntry(const PropertyEntry&) = default;
inline PropertyEntry::PropertyEntry(PropertyEntry&&) noexcept = default;
inline PropertyEntry& PropertyEntry::operator=(const PropertyEntry&) = default;
inline PropertyEntry& PropertyEntry::operator=(PropertyEntry&&) noexcept = default;

class PropertyBag {
public:
  struct Item {
    std::string key;
    PropertyEntry entry;
  };

  bool contains(std::string_view key) const noexcept;
  const PropertyEntry* find(std::string_view key) const noexcept;
  PropertyEntry* find(std::string_view key) noexcept;
  PropertyEntry& getOrAdd(std::string_view key);
  bool erase(std::string_view key);

  const std::vector<Item>& items() const noexcept { return items_; }
  std::vector<Item>& items() noexcept { return items_; }

  // Typed readers preserving v10 preset defaults and coercions
  bool getBool(std::string_view key, bool fallback = false) const noexcept;
  int getInt(std::string_view key, int fallback = 0) const noexcept;
  float getFloat(std::string_view key, float fallback = 0.0f) const noexcept;
  iota::core::Vec2 getVec2(std::string_view key, iota::core::Vec2 fallback = {}) const noexcept;
  iota::core::ColorRgba getColorArgb(std::string_view key,
                                     iota::core::ColorRgba fallback = {255, 255, 255, 255}) const noexcept;
  iota::core::ColorHslaNorm getColorHsla(std::string_view key,
                                         iota::core::ColorHslaNorm fallback = {}) const noexcept;
  std::string getString(std::string_view key, std::string_view fallback = "") const;

  // Unified MVar getter: reads both semicolon "mvarf"/"mvarf2" and legacy "_child measureWhat" forms.
  MVariableFloat getMVar(std::string_view key, const MVariableFloat& fallback = {}) const;

  // Typed writers updating "v" while preserving existing type metadata or initializing it cleanly
  void setBool(std::string_view key, bool val, std::string_view group = "0_general",
               std::string_view typeTag = "b");
  void setInt(std::string_view key, int val, int minVal = 0, int maxVal = 100,
              std::string_view group = "0_general", std::string_view typeTagPrefix = "i");
  void setFloat(std::string_view key, float val, float minVal = 0.0f, float maxVal = 1.0f,
                std::string_view group = "0_general");
  void setVec2(std::string_view key, iota::core::Vec2 val, float minVal = 0.0f, float maxVal = 1.0f,
               std::string_view group = "0_general");
  void setColorArgb(std::string_view key, iota::core::ColorRgba val,
                    std::string_view typeTag = "crgba", std::string_view group = "1_appearance");
  void setColorHsla(std::string_view key, iota::core::ColorHslaNorm val,
                    std::string_view group = "2_color");
  void setString(std::string_view key, std::string_view val, std::string_view typeTag = "txt",
                 std::string_view group = "0_general");
  void setMVar(std::string_view key, const MVariableFloat& val, std::string_view typeTag,
               std::string_view group = "0_general");

private:
  void ensureOrder(PropertyEntry& entry, std::string_view defaultType, std::string_view group);
  std::vector<Item> items_;
};

} // namespace iota::model
