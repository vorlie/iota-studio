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

#include <string>
#include <string_view>
#include <vector>

namespace iota::model {

// Schema property "t" type tokens (boolean, numeric, vector, MVar, color, text, asset, group).
enum class PropertyTypeTag {
  Unknown = 0,
  Bool,            // b
  ProBool,         // pb
  Int,             // i <min> <max>
  HiddenInt,       // ih [<min> <max>]
  Float,           // f <min> <max>
  Vec2f,           // f2 <min> <max>
  MVarFloat,       // mvarf <min> <max>
  MVarVec2f,       // mvarf2 <min> <max>
  ColorRgb,        // crgb
  ColorRgba,       // crgba
  ColorRgbHl,      // crgb_hl
  ColorHsla4f,     // chsl4f
  ColorHslaAlpha4f,// chsla4f
  Text,            // txt
  ProText,         // ptxt
  CodeText,        // etxt
  Image,           // img <choices...>
  ParticleImage,   // pimg <choices...>
  Asset,           // asset <choices...>
  ProAsset,        // passet <choices...>
  Select,          // sel <choices...>
  Child,           // _child [<choices...>]
  DynamicGroup     // _dyn
};

struct ParsedTypeSpec {
  PropertyTypeTag tag = PropertyTypeTag::Unknown;
  float minVal = 0.0f;
  float maxVal = 1.0f;
  bool hasBounds = false;
  std::vector<std::string> choices;
};

ParsedTypeSpec parseTypeTag(std::string_view typeTag);
std::string_view typeTagToken(PropertyTypeTag tag) noexcept;

struct MeasuredVar {
  std::string measureWhat = "Nothing";
  float a = 0.5f;
  float b = 0.5f;
};

struct MVariableFloat {
  std::vector<MeasuredVar> slots;

  static MVariableFloat constant(float v) { return MVariableFloat{{MeasuredVar{"Constant", v, v}}}; }
  static MVariableFloat constant2(float a, float b) { return MVariableFloat{{MeasuredVar{"Constant", a, b}}}; }
};

MVariableFloat parseMVarString(std::string_view text, const MVariableFloat& fallback = {});
std::string formatMVarString(const MVariableFloat& mvar);

const std::vector<std::string_view>& allMeasureWhatModes() noexcept;

} // namespace iota::model
