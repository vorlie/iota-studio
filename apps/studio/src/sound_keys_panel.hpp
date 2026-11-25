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

#include "iota/engine/scene_renderer.hpp"
#include "iota/model/scene.hpp"

#include "inspector_panel.hpp"

namespace iota::studio {

/// Interactive spectrum range selector canvas and live meters for a SoundKeys Element.
void renderSoundKeysCanvasAndMeters(iota::model::Element& soundKeysEl,
                                    const iota::engine::RenderFrameStats& stats,
                                    int& activeRangeIdx,
                                    InspectorActions& actions,
                                    float canvasHeight = 140.0f);

/// Docked Sound Keys studio panel for selecting frequency/amplitude bands across the project.
void renderSoundKeysPanel(iota::model::Project& project,
                          const iota::engine::RenderFrameStats& stats,
                          int& activeRangeIdx,
                          bool& openWindow,
                          int& selectedCompIdx,
                          int& selectedElemIdx,
                          InspectorActions& actions);

}  // namespace iota::studio
