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

namespace iota::studio {

enum class DocsTopic {
  QuickStart,
  ElementReference,
  AudioReactivity,
  TextTokensAndUris,
  CustomShaders,
  Shortcuts,
};

struct DocsPanelState {
  DocsTopic activeTopic = DocsTopic::QuickStart;
  iota::model::ElementType activeElementType = iota::model::ElementType::AudioProvider;
  char searchBuf[128]{};
  bool focusRequested = false;

  void openElement(iota::model::ElementType type) noexcept {
    activeTopic = DocsTopic::ElementReference;
    activeElementType = type;
    searchBuf[0] = '\0';
    focusRequested = true;
  }

  void openTopic(DocsTopic topic) noexcept {
    activeTopic = topic;
    focusRequested = true;
  }
};

struct DocsPanelActions {
  std::string copiedSnippet;
};

/// Draws the dockable Built-in Documentation window (toggled via F1, Help menu, or Inspector '?').
void renderDocsPanel(bool& showPanel, DocsPanelState& state, DocsPanelActions& actions);

}  // namespace iota::studio
