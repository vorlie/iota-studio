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

#include "iota/core/types.hpp"
#include "iota/model/measured_var.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace iota::dsp {

struct MVarEvalContext {
  float dt = 1.0f / 60.0f;
  double totalTimeSec = 0.0;
  double playAccumTimeSec = 0.0;
  double trackPositionSec = 0.0;
  double trackDurationSec = 0.0;
  float beat = 0.0f;
  float peakBarIndex = 0.0f;
  std::array<float, 3> soundKeys{0.0f, 0.0f, 0.0f};
  bool isPlaying = false;
  bool isExporting = false;
  uint32_t frameCounter = 0;
};

struct MVarSlotState {
  iota::core::Vec2 prev{};
};

/// Evaluates an MVariableFloat using explicit slot state storage.
iota::core::Vec2 evaluateMVar(const iota::model::MVariableFloat& var,
                               std::vector<MVarSlotState>& slotStates,
                               const MVarEvalContext& ctx);

/// Stateful evaluator storing persistent slot history keyed by property hash.
class MVarEvaluator {
 public:
  void reset();

  iota::core::Vec2 evaluateVec2(uint64_t propertyKey,
                                const iota::model::MVariableFloat& var,
                                const MVarEvalContext& ctx);

  float evaluateScalar(uint64_t propertyKey,
                       const iota::model::MVariableFloat& var,
                       const MVarEvalContext& ctx);

  /// Stable 64-bit key combining element runtime ID and property name.
  static uint64_t makeKey(int elementId, std::string_view propName) noexcept;

 private:
  std::unordered_map<uint64_t, std::vector<MVarSlotState>> states_;
};

}  // namespace iota::dsp
