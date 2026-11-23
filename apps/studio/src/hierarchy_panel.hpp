#pragma once

#include "iota/model/hierarchy_ops.hpp"
#include "iota/model/scene.hpp"

#include <string>
#include <vector>

namespace iota::studio {

inline constexpr const char* kLayerDragDropPayload = "IOTA_HIERARCHY_LAYERS";
inline constexpr const char* kCompDragDropPayload = "IOTA_HIERARCHY_COMP";

struct HierarchyPanelState {
  int primaryCompIdx = 0;
  int primaryElemIdx = -1;
  std::vector<iota::model::ElementRef> selectedElements;
  iota::model::ElementRef selectionAnchor;
  char searchFilter[128]{};

  // Inline rename state
  bool renaming = false;
  bool renamingComp = false;
  iota::model::ElementRef renameTarget{};
  bool renameFocusPending = false;
  char renameBuf[256]{};

  // Staged multi-selection drag payload
  std::vector<iota::model::ElementRef> activeDragElements;
  bool windowFocused = false;

  void selectComposition(int ci);
  void selectSingleElement(int ci, int ei);
  void setSelection(const std::vector<iota::model::ElementRef>& refs, int fallbackComp = 0);
  [[nodiscard]] bool isElementSelected(int ci, int ei) const noexcept;
  [[nodiscard]] bool allSelectedInSameComp(int* outCompIdx = nullptr) const noexcept;
  void clampToProject(const iota::model::Project& project);
  void beginRenameCurrent(const iota::model::Project& project);
};

struct HierarchyActions {
  bool undoSnapshotNeeded = false;
  std::string editLabel;
  std::string statusMessage;

  bool requestCopy = false;
  bool requestCut = false;
  bool requestPaste = false;
  bool requestDuplicate = false;
  bool requestDelete = false;
  bool requestPrecompose = false;
};

void renderHierarchyPanel(iota::model::Project& project,
                          HierarchyPanelState& state,
                          bool hasClipboard,
                          HierarchyActions& actions);

}  // namespace iota::studio
