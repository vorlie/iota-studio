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

#include "iota/model/hierarchy_ops.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <optional>
#include <utility>

#include "iota/core/utf8_path.hpp"

namespace iota::model {

namespace {

bool eqIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::optional<int> parseNonNegativeCompSuffix(std::string_view s) noexcept {
  constexpr std::string_view kPrefix = "composition:";
  if (s.size() <= kPrefix.size()) return std::nullopt;
  for (size_t i = 0; i < kPrefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) != kPrefix[i]) {
      return std::nullopt;
    }
  }
  std::string_view tail = s.substr(kPrefix.size());
  int val = 0;
  for (char c : tail) {
    if (c < '0' || c > '9') return std::nullopt;
    val = val * 10 + (c - '0');
    if (val > 1000000) return std::nullopt;
  }
  return val;
}

void remapEntryCompRefs(std::string_view key, PropertyEntry& entry, const std::vector<int>& oldToNew) {
  if (auto* s = std::get_if<std::string>(&entry.value)) {
    if (const auto compId = parseNonNegativeCompSuffix(*s);
        compId.has_value() && *compId > 0 && static_cast<size_t>(*compId) < oldToNew.size()) {
      const int mapped = oldToNew[static_cast<size_t>(*compId)];
      *s = "composition:" + std::to_string(mapped >= 0 ? mapped : 0);
    }
  }

  if (eqIgnoreCaseAscii(key, "sourceCompositionIndex")) {
    if (auto* iv = std::get_if<int64_t>(&entry.value)) {
      const int k = static_cast<int>(*iv);
      if (k > 0 && static_cast<size_t>(k) < oldToNew.size()) {
        const int mapped = oldToNew[static_cast<size_t>(k)];
        *iv = static_cast<int64_t>(mapped >= 0 ? mapped : 0);
      }
    } else if (auto* dv = std::get_if<double>(&entry.value)) {
      const int k = static_cast<int>(*dv);
      if (k > 0 && static_cast<size_t>(k) < oldToNew.size()) {
        const int mapped = oldToNew[static_cast<size_t>(k)];
        *dv = static_cast<double>(mapped >= 0 ? mapped : 0);
      }
    }
  }

  for (auto& ch : entry.children) {
    remapEntryCompRefs(ch.key, ch.entry, oldToNew);
  }
}

bool isValidRef(const Project& project, ElementRef r) noexcept {
  return r.compIdx >= 0 &&
         r.compIdx < static_cast<int>(project.compositions.size()) &&
         r.elemIdx >= 0 &&
         r.elemIdx < static_cast<int>(project.compositions[static_cast<size_t>(r.compIdx)].elements.size());
}

std::vector<ElementRef> normalizeValidRefs(const Project& project,
                                             const std::vector<ElementRef>& refs) {
  std::vector<ElementRef> out;
  out.reserve(refs.size());
  for (const auto& r : refs) {
    if (isValidRef(project, r)) {
      out.push_back(r);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool isVisualLayer(const Element& el) noexcept {
  return el.type != ElementType::AudioProvider && el.type != ElementType::SoundKeys;
}

bool compHasDisplayName(const Composition& comp, std::string_view name) noexcept {
  for (const auto& el : comp.elements) {
    if (el.displayName == name) return true;
  }
  return false;
}

bool projectHasCompName(const Project& project, std::string_view name) noexcept {
  for (const auto& c : project.compositions) {
    if (c.displayName == name) return true;
  }
  return false;
}

void stripSecondaryCompHdrProps(Composition& comp) {
  auto& items = comp.props.items();
  items.erase(std::remove_if(items.begin(), items.end(),
                             [](const PropertyBag::Item& item) {
                               return item.entry.groupTag == "3_hdrPost";
                             }),
              items.end());
}

void swapTracksInComp(Project& project, int compIdx, int idxA, int idxB) noexcept {
  for (auto& tr : project.desktopTracks) {
    if (tr.compositionIndex != compIdx) continue;
    if (tr.elementIndex == idxA) tr.elementIndex = idxB;
    else if (tr.elementIndex == idxB) tr.elementIndex = idxA;
  }
}

}  // namespace

int allocateFreshElementId(const Project& project) noexcept {
  static std::atomic<int> sNextId{1000};
  int maxExisting = 999;
  for (const auto& comp : project.compositions) {
    for (const auto& el : comp.elements) {
      maxExisting = std::max(maxExisting, el.runtimeId());
    }
  }
  int expected = sNextId.load(std::memory_order_relaxed);
  while (true) {
    const int candidate = std::max(expected, maxExisting + 1);
    if (sNextId.compare_exchange_weak(expected, candidate + 1, std::memory_order_relaxed)) {
      return candidate;
    }
  }
}

void assignFreshElementId(const Project& project, Element& el) {
  el.props.setInt("_id", allocateFreshElementId(project), 0, 0, "", "ih");
}

void assignFreshElementIds(const Project& project, Composition& comp) {
  for (auto& el : comp.elements) {
    assignFreshElementId(project, el);
  }
}

void remapCompositionReferences(Project& project, const std::vector<int>& oldToNew) {
  for (auto& comp : project.compositions) {
    for (auto& item : comp.props.items()) {
      remapEntryCompRefs(item.key, item.entry, oldToNew);
    }
    for (auto& el : comp.elements) {
      for (auto& item : el.props.items()) {
        remapEntryCompRefs(item.key, item.entry, oldToNew);
      }
    }
  }

  auto& tracks = project.desktopTracks;
  tracks.erase(
      std::remove_if(tracks.begin(), tracks.end(),
                     [&](KeyframeTrack& tr) {
                       if (tr.compositionIndex < 0 ||
                           static_cast<size_t>(tr.compositionIndex) >= oldToNew.size()) {
                         return true;
                       }
                       const int mapped = oldToNew[static_cast<size_t>(tr.compositionIndex)];
                       if (mapped < 0) return true;
                       tr.compositionIndex = mapped;
                       return false;
                     }),
      tracks.end());
}

std::vector<ElementRef> moveElements(Project& project,
                                     const std::vector<ElementRef>& sources,
                                     int dstCompIdx,
                                     int dstInsertIdx) {
  const auto valid = normalizeValidRefs(project, sources);
  if (valid.empty() || dstCompIdx < 0 ||
      dstCompIdx >= static_cast<int>(project.compositions.size())) {
    return {};
  }

  const int numComps = static_cast<int>(project.compositions.size());
  const int dstSize = static_cast<int>(project.compositions[static_cast<size_t>(dstCompIdx)].elements.size());
  const int clampedInsert =
      (dstInsertIdx < 0 || dstInsertIdx > dstSize) ? dstSize : dstInsertIdx;

  int beforeInDst = 0;
  for (const auto& r : valid) {
    if (r.compIdx == dstCompIdx && r.elemIdx < clampedInsert) {
      ++beforeInDst;
    }
  }
  const int adjustedInsert = clampedInsert - beforeInDst;
  const int movedCount = static_cast<int>(valid.size());

  // Build exact old(ci, ei) -> new(ci, ei) lookup for synchronizing desktopTracks
  std::vector<std::vector<int>> movedSlotMap(static_cast<size_t>(numComps));
  for (int ci = 0; ci < numComps; ++ci) {
    movedSlotMap[static_cast<size_t>(ci)].assign(
        project.compositions[static_cast<size_t>(ci)].elements.size(), -1);
  }
  for (int m = 0; m < movedCount; ++m) {
    const auto& r = valid[static_cast<size_t>(m)];
    movedSlotMap[static_cast<size_t>(r.compIdx)][static_cast<size_t>(r.elemIdx)] = m;
  }

  std::vector<std::vector<ElementRef>> destMap(static_cast<size_t>(numComps));
  for (int ci = 0; ci < numComps; ++ci) {
    const int nElems = static_cast<int>(project.compositions[static_cast<size_t>(ci)].elements.size());
    destMap[static_cast<size_t>(ci)].resize(static_cast<size_t>(nElems));
    int remainingCursor = 0;
    for (int ei = 0; ei < nElems; ++ei) {
      const int movedIdx = movedSlotMap[static_cast<size_t>(ci)][static_cast<size_t>(ei)];
      if (movedIdx >= 0) {
        destMap[static_cast<size_t>(ci)][static_cast<size_t>(ei)] = {dstCompIdx, adjustedInsert + movedIdx};
      } else {
        const int finalEi = (ci == dstCompIdx && remainingCursor >= adjustedInsert)
                                ? (remainingCursor + movedCount)
                                : remainingCursor;
        destMap[static_cast<size_t>(ci)][static_cast<size_t>(ei)] = {ci, finalEi};
        ++remainingCursor;
      }
    }
  }

  // Remap desktopTracks before modifying element arrays
  auto& tracks = project.desktopTracks;
  tracks.erase(
      std::remove_if(tracks.begin(), tracks.end(),
                     [&](KeyframeTrack& tr) {
                       if (tr.compositionIndex < 0 || tr.compositionIndex >= numComps) return true;
                       const auto& compMap = destMap[static_cast<size_t>(tr.compositionIndex)];
                       if (tr.elementIndex < 0 || tr.elementIndex >= static_cast<int>(compMap.size())) {
                         return true;
                       }
                       const ElementRef next = compMap[static_cast<size_t>(tr.elementIndex)];
                       tr.compositionIndex = next.compIdx;
                       tr.elementIndex = next.elemIdx;
                       return false;
                     }),
      tracks.end());

  // Extract moved elements in canonical (compIdx, elemIdx) order
  std::vector<Element> movedElems;
  movedElems.reserve(static_cast<size_t>(movedCount));
  for (const auto& r : valid) {
    movedElems.push_back(
        std::move(project.compositions[static_cast<size_t>(r.compIdx)].elements[static_cast<size_t>(r.elemIdx)]));
  }

  // Erase moved slots in reverse order so earlier indices remain valid
  for (auto it = valid.rbegin(); it != valid.rend(); ++it) {
    auto& srcElems = project.compositions[static_cast<size_t>(it->compIdx)].elements;
    srcElems.erase(srcElems.begin() + it->elemIdx);
  }

  // Insert extracted elements into destination composition
  auto& dstElems = project.compositions[static_cast<size_t>(dstCompIdx)].elements;
  dstElems.insert(dstElems.begin() + adjustedInsert,
                  std::make_move_iterator(movedElems.begin()),
                  std::make_move_iterator(movedElems.end()));

  std::vector<ElementRef> result;
  result.reserve(static_cast<size_t>(movedCount));
  for (int m = 0; m < movedCount; ++m) {
    result.push_back({dstCompIdx, adjustedInsert + m});
  }
  return result;
}

std::vector<ElementRef> nudgeElements(Project& project,
                                      const std::vector<ElementRef>& sources,
                                      int delta) {
  auto valid = normalizeValidRefs(project, sources);
  if (valid.empty() || delta == 0) {
    return valid;
  }

  std::map<int, std::vector<int>> byComp;
  for (const auto& r : valid) {
    byComp[r.compIdx].push_back(r.elemIdx);
  }

  const int steps = std::abs(delta);
  const int dir = (delta < 0) ? -1 : 1;

  for (auto& [ci, indices] : byComp) {
    auto& elems = project.compositions[static_cast<size_t>(ci)].elements;
    const int n = static_cast<int>(elems.size());
    for (int step = 0; step < steps; ++step) {
      if (dir < 0) {
        if (indices.front() <= 0) break;
        for (int& idx : indices) {
          std::swap(elems[static_cast<size_t>(idx)], elems[static_cast<size_t>(idx - 1)]);
          swapTracksInComp(project, ci, idx, idx - 1);
          --idx;
        }
      } else {
        if (indices.back() + 1 >= n) break;
        for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
          int& idx = *it;
          std::swap(elems[static_cast<size_t>(idx)], elems[static_cast<size_t>(idx + 1)]);
          swapTracksInComp(project, ci, idx, idx + 1);
          ++idx;
        }
      }
    }
  }

  std::vector<ElementRef> result;
  result.reserve(valid.size());
  for (const auto& [ci, indices] : byComp) {
    for (int idx : indices) {
      result.push_back({ci, idx});
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<ElementRef> moveElementsToBoundary(Project& project,
                                               const std::vector<ElementRef>& sources,
                                               bool toTop) {
  const auto valid = normalizeValidRefs(project, sources);
  if (valid.empty()) return {};

  std::map<int, std::vector<ElementRef>> byComp;
  for (const auto& r : valid) {
    byComp[r.compIdx].push_back(r);
  }

  std::vector<ElementRef> result;
  result.reserve(valid.size());
  for (const auto& [ci, refs] : byComp) {
    const int dstIdx =
        toTop ? 0 : static_cast<int>(project.compositions[static_cast<size_t>(ci)].elements.size());
    auto moved = moveElements(project, refs, ci, dstIdx);
    result.insert(result.end(), moved.begin(), moved.end());
  }
  std::sort(result.begin(), result.end());
  return result;
}

HierarchyClipboard copyElements(const Project& project, const std::vector<ElementRef>& sources) {
  HierarchyClipboard clip;
  const auto valid = normalizeValidRefs(project, sources);
  if (valid.empty()) {
    return clip;
  }

  clip.kind = HierarchyClipboard::Kind::Elements;
  clip.elements.reserve(valid.size());
  for (const auto& r : valid) {
    CopiedElement ce;
    ce.element = project.compositions[static_cast<size_t>(r.compIdx)].elements[static_cast<size_t>(r.elemIdx)];
    for (const auto& tr : project.desktopTracks) {
      if (tr.compositionIndex == r.compIdx && tr.elementIndex == r.elemIdx) {
        KeyframeTrack norm = tr;
        norm.compositionIndex = 0;
        norm.elementIndex = 0;
        ce.tracks.push_back(std::move(norm));
      }
    }
    clip.elements.push_back(std::move(ce));
  }
  return clip;
}

std::optional<ElementRef> deleteElements(Project& project, const std::vector<ElementRef>& targets) {
  const auto valid = normalizeValidRefs(project, targets);
  if (valid.empty()) {
    return std::nullopt;
  }

  const ElementRef firstTarget = valid.front();
  const int numComps = static_cast<int>(project.compositions.size());

  std::vector<std::vector<bool>> deleteMask(static_cast<size_t>(numComps));
  for (int ci = 0; ci < numComps; ++ci) {
    deleteMask[static_cast<size_t>(ci)].assign(
        project.compositions[static_cast<size_t>(ci)].elements.size(), false);
  }
  for (const auto& r : valid) {
    deleteMask[static_cast<size_t>(r.compIdx)][static_cast<size_t>(r.elemIdx)] = true;
  }

  std::vector<std::vector<int>> newIndexMap(static_cast<size_t>(numComps));
  for (int ci = 0; ci < numComps; ++ci) {
    const auto& mask = deleteMask[static_cast<size_t>(ci)];
    newIndexMap[static_cast<size_t>(ci)].assign(mask.size(), -1);
    int nextIdx = 0;
    for (size_t ei = 0; ei < mask.size(); ++ei) {
      if (!mask[ei]) {
        newIndexMap[static_cast<size_t>(ci)][ei] = nextIdx++;
      }
    }
  }

  auto& tracks = project.desktopTracks;
  tracks.erase(
      std::remove_if(tracks.begin(), tracks.end(),
                     [&](KeyframeTrack& tr) {
                       if (tr.compositionIndex < 0 || tr.compositionIndex >= numComps) return true;
                       const auto& mapVec = newIndexMap[static_cast<size_t>(tr.compositionIndex)];
                       if (tr.elementIndex < 0 || tr.elementIndex >= static_cast<int>(mapVec.size())) {
                         return true;
                       }
                       const int mapped = mapVec[static_cast<size_t>(tr.elementIndex)];
                       if (mapped < 0) return true;
                       tr.elementIndex = mapped;
                       return false;
                     }),
      tracks.end());

  for (auto it = valid.rbegin(); it != valid.rend(); ++it) {
    auto& elems = project.compositions[static_cast<size_t>(it->compIdx)].elements;
    elems.erase(elems.begin() + it->elemIdx);
  }

  const int remaining =
      static_cast<int>(project.compositions[static_cast<size_t>(firstTarget.compIdx)].elements.size());
  if (remaining <= 0) {
    return ElementRef{firstTarget.compIdx, -1};
  }
  return ElementRef{firstTarget.compIdx, std::min(firstTarget.elemIdx, remaining - 1)};
}

HierarchyClipboard cutElements(Project& project,
                               const std::vector<ElementRef>& targets,
                               std::optional<ElementRef>* outFallbackSel) {
  auto clip = copyElements(project, targets);
  const auto sel = deleteElements(project, targets);
  if (outFallbackSel) {
    *outFallbackSel = sel;
  }
  return clip;
}

std::vector<ElementRef> pasteClipboard(Project& project,
                                       const HierarchyClipboard& clip,
                                       int dstCompIdx,
                                       int dstInsertIdx,
                                       bool appendCopySuffix) {
  if (clip.kind == HierarchyClipboard::Kind::Composition) {
    Composition copyComp = clip.composition;
    const int newCompIdx = static_cast<int>(project.compositions.size());
    if (copyComp.displayName.empty()) {
      copyComp.displayName = "Composition " + std::to_string(newCompIdx);
    } else if (appendCopySuffix || projectHasCompName(project, copyComp.displayName)) {
      copyComp.displayName += " Copy";
    }
    if (newCompIdx > 0) {
      stripSecondaryCompHdrProps(copyComp);
    }
    assignFreshElementIds(project, copyComp);
    const int elemCount = static_cast<int>(copyComp.elements.size());
    project.compositions.push_back(std::move(copyComp));

    for (const auto& tr : clip.compTracks) {
      if (tr.elementIndex >= 0 && tr.elementIndex < elemCount) {
        KeyframeTrack cloned = tr;
        cloned.compositionIndex = newCompIdx;
        project.desktopTracks.push_back(std::move(cloned));
      }
    }
    return {{newCompIdx, -1}};
  }

  if (clip.kind != HierarchyClipboard::Kind::Elements || clip.elements.empty()) {
    return {};
  }
  if (dstCompIdx < 0 || dstCompIdx >= static_cast<int>(project.compositions.size())) {
    return {};
  }

  auto& dstComp = project.compositions[static_cast<size_t>(dstCompIdx)];
  const int dstSize = static_cast<int>(dstComp.elements.size());
  const int insertIdx = (dstInsertIdx < 0 || dstInsertIdx > dstSize) ? dstSize : dstInsertIdx;
  const int count = static_cast<int>(clip.elements.size());

  std::vector<Element> clones;
  clones.reserve(static_cast<size_t>(count));
  for (const auto& ce : clip.elements) {
    Element el = ce.element;
    assignFreshElementId(project, el);
    if (!el.displayName.empty() && (appendCopySuffix || compHasDisplayName(dstComp, el.displayName))) {
      el.displayName += " Copy";
    }
    clones.push_back(std::move(el));
  }

  for (auto& tr : project.desktopTracks) {
    if (tr.compositionIndex == dstCompIdx && tr.elementIndex >= insertIdx) {
      tr.elementIndex += count;
    }
  }

  dstComp.elements.insert(dstComp.elements.begin() + insertIdx,
                          std::make_move_iterator(clones.begin()),
                          std::make_move_iterator(clones.end()));

  std::vector<ElementRef> result;
  result.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const int targetElemIdx = insertIdx + i;
    for (const auto& tr : clip.elements[static_cast<size_t>(i)].tracks) {
      KeyframeTrack cloned = tr;
      cloned.compositionIndex = dstCompIdx;
      cloned.elementIndex = targetElemIdx;
      project.desktopTracks.push_back(std::move(cloned));
    }
    result.push_back({dstCompIdx, targetElemIdx});
  }
  return result;
}

std::vector<ElementRef> duplicateElements(Project& project, const std::vector<ElementRef>& sources) {
  const auto valid = normalizeValidRefs(project, sources);
  if (valid.empty()) return {};

  std::map<int, std::vector<ElementRef>> byComp;
  for (const auto& r : valid) {
    byComp[r.compIdx].push_back(r);
  }

  std::vector<ElementRef> result;
  result.reserve(valid.size());
  for (const auto& [ci, refs] : byComp) {
    const HierarchyClipboard clip = copyElements(project, refs);
    const int insertAt = refs.back().elemIdx + 1;
    auto pasted = pasteClipboard(project, clip, ci, insertAt, true);
    result.insert(result.end(), pasted.begin(), pasted.end());
  }
  std::sort(result.begin(), result.end());
  return result;
}

bool renameElement(Project& project, ElementRef ref, std::string_view newName) {
  if (!isValidRef(project, ref)) return false;
  auto& el = project.compositions[static_cast<size_t>(ref.compIdx)].elements[static_cast<size_t>(ref.elemIdx)];
  el.displayName = iota::core::sanitizeUtf8(newName);
  return true;
}

bool setElementsVisible(Project& project, const std::vector<ElementRef>& targets, bool visible) {
  const auto valid = normalizeValidRefs(project, targets);
  bool changed = false;
  for (const auto& r : valid) {
    auto& el = project.compositions[static_cast<size_t>(r.compIdx)].elements[static_cast<size_t>(r.elemIdx)];
    if (el.type == ElementType::AudioProvider) continue;
    el.props.setBool("visible", visible, "0_general",
                     el.type == ElementType::AppLogo ? "pb" : "b");
    changed = true;
  }
  return changed;
}

bool isElementSoloedInComposition(const Project& project, int compIdx, int elemIdx) noexcept {
  if (!isValidRef(project, {compIdx, elemIdx})) return false;
  const auto& elems = project.compositions[static_cast<size_t>(compIdx)].elements;
  const auto& target = elems[static_cast<size_t>(elemIdx)];
  if (!isVisualLayer(target) || !target.props.getBool("visible", true)) {
    return false;
  }
  for (size_t i = 0; i < elems.size(); ++i) {
    if (static_cast<int>(i) == elemIdx) continue;
    if (isVisualLayer(elems[i]) && !elems[i].props.getBool("visible", true)) {
      return true;
    }
  }
  return false;
}

bool soloElementsInComposition(Project& project, int compIdx, const std::vector<int>& elemIndices) {
  if (compIdx < 0 || compIdx >= static_cast<int>(project.compositions.size())) {
    return false;
  }
  auto& elems = project.compositions[static_cast<size_t>(compIdx)].elements;
  const int n = static_cast<int>(elems.size());

  std::vector<int> targets;
  targets.reserve(elemIndices.size());
  for (int idx : elemIndices) {
    if (idx >= 0 && idx < n && isVisualLayer(elems[static_cast<size_t>(idx)])) {
      targets.push_back(idx);
    }
  }
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  if (targets.empty()) {
    return false;
  }

  std::vector<bool> selectedMask(static_cast<size_t>(n), false);
  for (int idx : targets) {
    selectedMask[static_cast<size_t>(idx)] = true;
  }

  int otherVisualCount = 0;
  bool allTargetsVisible = true;
  bool allOthersHidden = true;
  for (int i = 0; i < n; ++i) {
    const auto& el = elems[static_cast<size_t>(i)];
    if (!isVisualLayer(el)) continue;
    const bool vis = el.props.getBool("visible", true);
    if (selectedMask[static_cast<size_t>(i)]) {
      if (!vis) allTargetsVisible = false;
    } else {
      ++otherVisualCount;
      if (vis) allOthersHidden = false;
    }
  }

  if (otherVisualCount > 0 && allTargetsVisible && allOthersHidden) {
    // Unsolo: restore all visual layers to visible
    for (auto& el : elems) {
      if (!isVisualLayer(el)) continue;
      el.props.setBool("visible", true, "0_general",
                       el.type == ElementType::AppLogo ? "pb" : "b");
    }
  } else {
    // Solo: show selected visual layers and hide all other visual layers
    for (int i = 0; i < n; ++i) {
      auto& el = elems[static_cast<size_t>(i)];
      if (!isVisualLayer(el)) continue;
      const bool vis = selectedMask[static_cast<size_t>(i)];
      el.props.setBool("visible", vis, "0_general",
                       el.type == ElementType::AppLogo ? "pb" : "b");
    }
  }
  return true;
}

int addComposition(Project& project, std::string_view name) {
  const int nextIdx = static_cast<int>(project.compositions.size());
  Composition comp = createDefaultComposition(nextIdx);
  if (!name.empty()) {
    comp.displayName = iota::core::sanitizeUtf8(name);
  }
  project.compositions.push_back(std::move(comp));
  return nextIdx;
}

bool renameComposition(Project& project, int compIdx, std::string_view newName) {
  if (compIdx < 0 || compIdx >= static_cast<int>(project.compositions.size())) {
    return false;
  }
  project.compositions[static_cast<size_t>(compIdx)].displayName = iota::core::sanitizeUtf8(newName);
  return true;
}

HierarchyClipboard copyComposition(const Project& project, int compIdx) {
  HierarchyClipboard clip;
  if (compIdx < 0 || compIdx >= static_cast<int>(project.compositions.size())) {
    return clip;
  }
  clip.kind = HierarchyClipboard::Kind::Composition;
  clip.composition = project.compositions[static_cast<size_t>(compIdx)];
  for (const auto& tr : project.desktopTracks) {
    if (tr.compositionIndex == compIdx) {
      KeyframeTrack norm = tr;
      norm.compositionIndex = 0;
      clip.compTracks.push_back(std::move(norm));
    }
  }
  return clip;
}

int duplicateComposition(Project& project, int compIdx) {
  const auto clip = copyComposition(project, compIdx);
  if (clip.kind != HierarchyClipboard::Kind::Composition) {
    return -1;
  }
  const auto refs = pasteClipboard(project, clip, 0, -1, true);
  return refs.empty() ? -1 : refs.front().compIdx;
}

bool moveComposition(Project& project, int fromCompIdx, int toCompIdx) {
  const int n = static_cast<int>(project.compositions.size());
  if (fromCompIdx < 1 || toCompIdx < 1 || fromCompIdx >= n || toCompIdx >= n || fromCompIdx == toCompIdx) {
    return false;
  }

  std::vector<int> order(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) order[static_cast<size_t>(i)] = i;
  const int moved = order[static_cast<size_t>(fromCompIdx)];
  order.erase(order.begin() + fromCompIdx);
  order.insert(order.begin() + toCompIdx, moved);

  std::vector<int> oldToNew(static_cast<size_t>(n), 0);
  std::vector<Composition> reordered;
  reordered.reserve(static_cast<size_t>(n));
  for (int newIdx = 0; newIdx < n; ++newIdx) {
    const int oldIdx = order[static_cast<size_t>(newIdx)];
    oldToNew[static_cast<size_t>(oldIdx)] = newIdx;
    reordered.push_back(std::move(project.compositions[static_cast<size_t>(oldIdx)]));
  }
  project.compositions = std::move(reordered);

  remapCompositionReferences(project, oldToNew);
  return true;
}

bool deleteComposition(Project& project, int compIdx) {
  const int n = static_cast<int>(project.compositions.size());
  if (compIdx < 1 || compIdx >= n) {
    return false;
  }

  std::vector<int> oldToNew(static_cast<size_t>(n), 0);
  for (int i = 0; i < n; ++i) {
    if (i < compIdx) oldToNew[static_cast<size_t>(i)] = i;
    else if (i == compIdx) oldToNew[static_cast<size_t>(i)] = -1;
    else oldToNew[static_cast<size_t>(i)] = i - 1;
  }

  project.compositions.erase(project.compositions.begin() + compIdx);
  remapCompositionReferences(project, oldToNew);
  return true;
}

PrecomposeResult precomposeElements(Project& project,
                                      int srcCompIdx,
                                      const std::vector<int>& elemIndices,
                                      std::string_view newCompName) {
  if (srcCompIdx < 0 || srcCompIdx >= static_cast<int>(project.compositions.size())) {
    return {};
  }
  const int srcCount = static_cast<int>(project.compositions[static_cast<size_t>(srcCompIdx)].elements.size());
  std::vector<int> validIdx;
  validIdx.reserve(elemIndices.size());
  for (int ei : elemIndices) {
    if (ei >= 0 && ei < srcCount) {
      validIdx.push_back(ei);
    }
  }
  std::sort(validIdx.begin(), validIdx.end());
  validIdx.erase(std::unique(validIdx.begin(), validIdx.end()), validIdx.end());
  if (validIdx.empty()) {
    return {};
  }

  const int earliestSlot = validIdx.front();
  const int predictedIdx = static_cast<int>(project.compositions.size());
  const std::string resolvedName = newCompName.empty()
                                       ? ("Pre-Comp " + std::to_string(predictedIdx))
                                       : iota::core::sanitizeUtf8(newCompName);
  const int newCompIdx = addComposition(project, resolvedName);

  std::vector<ElementRef> refs;
  refs.reserve(validIdx.size());
  for (int ei : validIdx) {
    refs.push_back({srcCompIdx, ei});
  }
  (void)moveElements(project, refs, newCompIdx, 0);

  Element host = createDefaultElement(ElementType::Image);
  assignFreshElementId(project, host);
  host.displayName = project.compositions[static_cast<size_t>(newCompIdx)].displayName;
  host.props.setString("customImage",
                       "composition:" + std::to_string(newCompIdx),
                       "img composition:0",
                       "1_image");
  host.props.setBool("keepAspectRatio", false, "1_image", "b");
  host.props.setBool("scaleIsUniform", false, "0_general", "b");
  host.props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");

  for (auto& tr : project.desktopTracks) {
    if (tr.compositionIndex == srcCompIdx && tr.elementIndex >= earliestSlot) {
      tr.elementIndex += 1;
    }
  }

  auto& srcElems = project.compositions[static_cast<size_t>(srcCompIdx)].elements;
  const int safeSlot = std::clamp(earliestSlot, 0, static_cast<int>(srcElems.size()));
  srcElems.insert(srcElems.begin() + safeSlot, std::move(host));

  return {newCompIdx, {srcCompIdx, safeSlot}};
}

}  // namespace iota::model
