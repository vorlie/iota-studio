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

#include "iota/model/property.hpp"

#include <cstdio>

namespace iota::model {

const PropertyEntry* PropertyEntry::findChild(std::string_view key) const noexcept {
  for (const auto& c : children) {
    if (c.key == key) return &c.entry;
  }
  return nullptr;
}

PropertyEntry* PropertyEntry::findChild(std::string_view key) noexcept {
  for (auto& c : children) {
    if (c.key == key) return &c.entry;
  }
  return nullptr;
}

PropertyEntry& PropertyEntry::getOrAddChild(std::string_view key) {
  if (auto* existing = findChild(key)) return *existing;
  int nextOrder = static_cast<int>(children.size());
  children.push_back({std::string(key), PropertyEntry{}});
  children.back().entry.order = nextOrder;
  return children.back().entry;
}

bool PropertyBag::contains(std::string_view key) const noexcept {
  return find(key) != nullptr;
}

const PropertyEntry* PropertyBag::find(std::string_view key) const noexcept {
  for (const auto& item : items_) {
    if (item.key == key) return &item.entry;
  }
  return nullptr;
}

PropertyEntry* PropertyBag::find(std::string_view key) noexcept {
  for (auto& item : items_) {
    if (item.key == key) return &item.entry;
  }
  return nullptr;
}

PropertyEntry& PropertyBag::getOrAdd(std::string_view key) {
  if (auto* existing = find(key)) return *existing;
  int nextOrder = static_cast<int>(items_.size());
  items_.push_back({std::string(key), PropertyEntry{}});
  items_.back().entry.order = nextOrder;
  return items_.back().entry;
}

bool PropertyBag::erase(std::string_view key) {
  for (auto it = items_.begin(); it != items_.end(); ++it) {
    if (it->key == key) {
      items_.erase(it);
      return true;
    }
  }
  return false;
}

bool PropertyBag::getBool(std::string_view key, bool fallback) const noexcept {
  const auto* e = find(key);
  if (!e) return fallback;
  if (const auto* b = std::get_if<bool>(&e->value)) return *b;
  if (const auto* i = std::get_if<int64_t>(&e->value)) return *i != 0;
  if (const auto* d = std::get_if<double>(&e->value)) return *d != 0.0;
  return fallback;
}

int PropertyBag::getInt(std::string_view key, int fallback) const noexcept {
  const auto* e = find(key);
  if (!e) return fallback;
  if (const auto* i = std::get_if<int64_t>(&e->value)) return static_cast<int>(*i);
  if (const auto* d = std::get_if<double>(&e->value)) return static_cast<int>(*d);
  if (const auto* b = std::get_if<bool>(&e->value)) return *b ? 1 : 0;
  return fallback;
}

float PropertyBag::getFloat(std::string_view key, float fallback) const noexcept {
  const auto* e = find(key);
  if (!e) return fallback;
  if (const auto* d = std::get_if<double>(&e->value)) return static_cast<float>(*d);
  if (const auto* i = std::get_if<int64_t>(&e->value)) return static_cast<float>(*i);
  if (const auto* b = std::get_if<bool>(&e->value)) return *b ? 1.0f : 0.0f;
  return fallback;
}

iota::core::Vec2 PropertyBag::getVec2(std::string_view key, iota::core::Vec2 fallback) const noexcept {
  const auto* e = find(key);
  if (!e) return fallback;
  if (const auto* s = std::get_if<std::string>(&e->value)) {
    if (auto parsed = iota::core::parseF2(*s)) return *parsed;
  }
  return fallback;
}

iota::core::ColorRgba PropertyBag::getColorArgb(std::string_view key,
                                                  iota::core::ColorRgba fallback) const noexcept {
  const auto* e = find(key);
  if (!e) return fallback;
  if (const auto* i = std::get_if<int64_t>(&e->value)) {
    return iota::core::ColorRgba::fromArgbInt32(static_cast<int32_t>(*i));
  }
  if (const auto* d = std::get_if<double>(&e->value)) {
    return iota::core::ColorRgba::fromArgbInt32(static_cast<int32_t>(*d));
  }
  if (const auto* s = std::get_if<std::string>(&e->value)) {
    if (auto hsla = iota::core::parseHsla4f(*s)) return hsla->toRgba();
  }
  return fallback;
}

iota::core::ColorHslaNorm PropertyBag::getColorHsla(std::string_view key,
                                                      iota::core::ColorHslaNorm fallback) const noexcept {
  const auto* e = find(key);
  if (!e) return fallback;
  if (const auto* s = std::get_if<std::string>(&e->value)) {
    if (auto hsla = iota::core::parseHsla4f(*s)) return *hsla;
  }
  if (const auto* i = std::get_if<int64_t>(&e->value)) {
    return iota::core::ColorHslaNorm::fromRgba(
        iota::core::ColorRgba::fromArgbInt32(static_cast<int32_t>(*i)));
  }
  return fallback;
}

std::string PropertyBag::getString(std::string_view key, std::string_view fallback) const {
  const auto* e = find(key);
  if (!e) return std::string(fallback);
  if (const auto* s = std::get_if<std::string>(&e->value)) return *s;
  return std::string(fallback);
}

MVariableFloat PropertyBag::getMVar(std::string_view key, const MVariableFloat& fallback) const {
  const auto* e = find(key);
  if (!e) return fallback;

  // Legacy _child single-slot form (MeasurePos, measureScale, measureRot, etc.)
  if (const auto* mw = e->findChild("measureWhat")) {
    std::string what = "Nothing";
    if (const auto* s = std::get_if<std::string>(&mw->value)) {
      if (!s->empty()) what = *s;
    }
    auto readScalar = [](const PropertyEntry* p, float def) -> float {
      if (!p) return def;
      if (const auto* d = std::get_if<double>(&p->value)) return static_cast<float>(*d);
      if (const auto* i = std::get_if<int64_t>(&p->value)) return static_cast<float>(*i);
      return def;
    };
    const float a = readScalar(e->findChild("A"), 0.5f);
    const float b = readScalar(e->findChild("B"), 0.5f);
    return MVariableFloat{{MeasuredVar{what, a, b}}};
  }

  // Modern multi-slot semicolon string form ("mvarf" / "mvarf2")
  if (const auto* s = std::get_if<std::string>(&e->value)) {
    if (!s->empty()) return parseMVarString(*s, fallback);
  }
  return fallback;
}

void PropertyBag::ensureOrder(PropertyEntry& entry, std::string_view defaultType, std::string_view group) {
  if (entry.typeTag.empty()) entry.typeTag = std::string(defaultType);
  if (entry.groupTag.empty()) entry.groupTag = std::string(group);
}

void PropertyBag::setBool(std::string_view key, bool val, std::string_view group,
                          std::string_view typeTag) {
  auto& e = getOrAdd(key);
  ensureOrder(e, typeTag, group);
  e.value = static_cast<int64_t>(val ? 1 : 0);
}

void PropertyBag::setInt(std::string_view key, int val, int minVal, int maxVal,
                         std::string_view group, std::string_view typeTagPrefix) {
  char buf[64];
  if (typeTagPrefix == "ih" && minVal == 0 && maxVal == 0) {
    std::snprintf(buf, sizeof(buf), "ih");
  } else {
    std::snprintf(buf, sizeof(buf), "%.*s %d %d",
                  static_cast<int>(typeTagPrefix.size()), typeTagPrefix.data(), minVal, maxVal);
  }
  auto& e = getOrAdd(key);
  ensureOrder(e, buf, group);
  e.value = static_cast<int64_t>(val);
}

void PropertyBag::setFloat(std::string_view key, float val, float minVal, float maxVal,
                           std::string_view group) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "f %.2f %.2f", minVal, maxVal);
  auto& e = getOrAdd(key);
  ensureOrder(e, buf, group);
  e.value = static_cast<double>(val);
}

void PropertyBag::setVec2(std::string_view key, iota::core::Vec2 val, float minVal, float maxVal,
                          std::string_view group) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "f2 %.2f %.2f", minVal, maxVal);
  auto& e = getOrAdd(key);
  ensureOrder(e, buf, group);
  e.value = iota::core::formatF2(val);
}

void PropertyBag::setColorArgb(std::string_view key, iota::core::ColorRgba val,
                               std::string_view typeTag, std::string_view group) {
  auto& e = getOrAdd(key);
  ensureOrder(e, typeTag, group);
  e.value = static_cast<int64_t>(val.toArgbInt32());
}

void PropertyBag::setColorHsla(std::string_view key, iota::core::ColorHslaNorm val,
                               std::string_view group) {
  auto& e = getOrAdd(key);
  ensureOrder(e, "chsla4f", group);
  e.value = iota::core::formatHsla4f(val);
}

void PropertyBag::setString(std::string_view key, std::string_view val, std::string_view typeTag,
                            std::string_view group) {
  auto& e = getOrAdd(key);
  ensureOrder(e, typeTag, group);
  e.value = std::string(val);
}

void PropertyBag::setMVar(std::string_view key, const MVariableFloat& val, std::string_view typeTag,
                          std::string_view group) {
  auto& e = getOrAdd(key);
  ensureOrder(e, typeTag, group);
  if (!e.children.empty() && e.findChild("measureWhat")) {
    const auto slot = val.slots.empty() ? MeasuredVar{} : val.slots[0];
    auto& mw = e.getOrAddChild("measureWhat");
    mw.value = slot.measureWhat;
    if (mw.typeTag.empty()) mw.typeTag = "_child Nothing Beat Constant TotalTime TotalTimeBackward TotalTimeWhenPlaying TotalTimeAndBeat TrackPosition BeatRandomShake BeatCamShakeMore BeatCamShakeLess ConstantShakeMore ConstantShake BeatCamShakeRotMore BeatCamShakeRotLess ConstantShakeRotMore ConstantShakeRotLess PeakBarIndex BeatTriggerAnim";
    if (mw.groupTag.empty()) mw.groupTag = "misc";
    if (slot.measureWhat != "Nothing") {
      auto& ea = e.getOrAddChild("A");
      ea.value = static_cast<double>(slot.a);
      if (ea.typeTag.empty()) ea.typeTag = "f 0.0 1.0";
      if (ea.groupTag.empty()) ea.groupTag = "misc";
      auto& eb = e.getOrAddChild("B");
      eb.value = static_cast<double>(slot.b);
      if (eb.typeTag.empty()) eb.typeTag = "f 0.0 1.0";
      if (eb.groupTag.empty()) eb.groupTag = "misc";
    }
    e.value = std::string("");
  } else {
    e.value = formatMVarString(val);
  }
}

} // namespace iota::model
