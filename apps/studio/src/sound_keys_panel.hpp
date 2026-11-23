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
