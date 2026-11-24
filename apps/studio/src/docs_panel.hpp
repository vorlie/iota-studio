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
