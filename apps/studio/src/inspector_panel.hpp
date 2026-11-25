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

#include "iota/model/property.hpp"
#include "iota/model/scene.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace iota::studio {

struct InspectorActions {
  bool modified = false;
  bool undoSnapshotNeeded = false;
  std::string editLabel;
  std::string addKeyframeProperty;
  float addKeyframeValue = 0.0f;
  bool requestLocalAssetImport = false;
  bool requestLocalFontImport = false;
  bool requestLocalShaderImport = false;
  bool requestOpenShaderEditor = false;
  std::string assetTargetPropName;
  std::string editShaderTargetProp;
  const std::map<std::string, std::vector<uint8_t>>* availableLocalAssets = nullptr;
  bool showHiddenProperties = false;
  bool preciseNumericInputs = false;
  bool showRawPropertyKeys = false;
  std::string propertyFilter;
};

void renderPropertyBagInspector(iota::model::PropertyBag& bag,
                                iota::model::ElementType ownerType,
                                InspectorActions& actions,
                                std::function<void()> onBeforeDiscreteEdit = {});

}  // namespace iota::studio
