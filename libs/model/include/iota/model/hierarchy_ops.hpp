#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iota/model/scene.hpp"

namespace iota::model {

struct ElementRef {
  int compIdx = -1;
  int elemIdx = -1;

  bool operator==(const ElementRef&) const noexcept = default;
  bool operator<(const ElementRef& other) const noexcept {
    return compIdx != other.compIdx ? (compIdx < other.compIdx) : (elemIdx < other.elemIdx);
  }
};

struct CopiedElement {
  Element element;
  std::vector<KeyframeTrack> tracks;  // normalized to compositionIndex = 0, elementIndex = 0
};

struct HierarchyClipboard {
  enum class Kind {
    Empty = 0,
    Elements,
    Composition,
  };

  Kind kind = Kind::Empty;
  std::vector<CopiedElement> elements;
  Composition composition;
  std::vector<KeyframeTrack> compTracks;  // normalized to compositionIndex = 0, preserves elementIndex
};

struct PrecomposeResult {
  int newCompIdx = -1;
  ElementRef hostLayerRef;
};

// Runtime ID & reference integrity helpers
[[nodiscard]] int allocateFreshElementId(const Project& project) noexcept;
void assignFreshElementId(const Project& project, Element& el);
void assignFreshElementIds(const Project& project, Composition& comp);
void remapCompositionReferences(Project& project, const std::vector<int>& oldToNew);

// Layer selection/move/clipboard/solo/rename operations (keeps desktopTracks synced)
[[nodiscard]] std::vector<ElementRef> moveElements(Project& project,
                                                   const std::vector<ElementRef>& sources,
                                                   int dstCompIdx,
                                                   int dstInsertIdx = -1);

[[nodiscard]] std::vector<ElementRef> nudgeElements(Project& project,
                                                    const std::vector<ElementRef>& sources,
                                                    int delta);

[[nodiscard]] std::vector<ElementRef> moveElementsToBoundary(Project& project,
                                                             const std::vector<ElementRef>& sources,
                                                             bool toTop);

[[nodiscard]] HierarchyClipboard copyElements(const Project& project,
                                              const std::vector<ElementRef>& sources);

std::optional<ElementRef> deleteElements(Project& project,
                                         const std::vector<ElementRef>& targets);

[[nodiscard]] HierarchyClipboard cutElements(Project& project,
                                             const std::vector<ElementRef>& targets,
                                             std::optional<ElementRef>* outFallbackSel = nullptr);

[[nodiscard]] std::vector<ElementRef> pasteClipboard(Project& project,
                                                     const HierarchyClipboard& clip,
                                                     int dstCompIdx,
                                                     int dstInsertIdx = -1,
                                                     bool appendCopySuffix = false);

[[nodiscard]] std::vector<ElementRef> duplicateElements(Project& project,
                                                        const std::vector<ElementRef>& sources);

bool renameElement(Project& project, ElementRef ref, std::string_view newName);
bool setElementsVisible(Project& project, const std::vector<ElementRef>& targets, bool visible);
[[nodiscard]] bool isElementSoloedInComposition(const Project& project, int compIdx, int elemIdx) noexcept;
bool soloElementsInComposition(Project& project, int compIdx, const std::vector<int>& elemIndices);

// Composition operations & AE-style Pre-Compose
int addComposition(Project& project, std::string_view name = "");
bool renameComposition(Project& project, int compIdx, std::string_view newName);
[[nodiscard]] HierarchyClipboard copyComposition(const Project& project, int compIdx);
int duplicateComposition(Project& project, int compIdx);
bool moveComposition(Project& project, int fromCompIdx, int toCompIdx);
bool deleteComposition(Project& project, int compIdx);

[[nodiscard]] PrecomposeResult precomposeElements(Project& project,
                                                  int srcCompIdx,
                                                  const std::vector<int>& elemIndices,
                                                  std::string_view newCompName = "");

}  // namespace iota::model
