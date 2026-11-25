
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

#include "doctest/doctest.h"

#include "iota/model/hierarchy_ops.hpp"
#include "iota/model/scene.hpp"
#include "iota/preset_io/preset_io.hpp"

#include <set>
#include <string>
#include <vector>

namespace {

iota::model::Element makeNamedElem(iota::model::ElementType t,
                                   const std::string& name,
                                   int id) {
  auto el = iota::model::createDefaultElement(t);
  el.displayName = name;
  el.props.setInt("_id", id, 0, 0, "", "ih");
  return el;
}

iota::model::Project makeHierarchyTestProject() {
  iota::model::Project p;
  p.version = "10";
  auto comp0 = iota::model::createDefaultComposition(0);
  comp0.displayName = "Final";
  comp0.elements.push_back(makeNamedElem(iota::model::ElementType::AudioProvider, "Audio", 10));
  comp0.elements.push_back(makeNamedElem(iota::model::ElementType::Image, "LayerA", 11));
  comp0.elements.push_back(makeNamedElem(iota::model::ElementType::Bars, "LayerB", 12));
  comp0.elements.push_back(makeNamedElem(iota::model::ElementType::Text, "LayerC", 13));

  auto comp1 = iota::model::createDefaultComposition(1);
  comp1.displayName = "BG Comp";
  comp1.elements.push_back(makeNamedElem(iota::model::ElementType::Image, "BgImg", 21));

  p.compositions.push_back(std::move(comp0));
  p.compositions.push_back(std::move(comp1));

  p.desktopTracks.push_back({0, 1, "opacity", {{0.0, 1.0f}, {1.0, 0.5f}}});
  p.desktopTracks.push_back({0, 2, "hdrIntensity", {{0.0, 1.5f}}});
  p.desktopTracks.push_back({0, 3, "fontSize", {{0.0, 48.0f}}});
  return p;
}

}  // namespace

TEST_CASE("Hierarchy moveElements supports intra-comp, cross-comp, non-contiguous multi-select, and tracks") {
  using namespace iota::model;
  auto proj = makeHierarchyTestProject();

  // Intra-comp non-contiguous move: LayerA(1) & LayerC(3) to end of Comp 0
  auto moved = moveElements(proj, {{0, 1}, {0, 3}}, 0, 4);
  REQUIRE(moved.size() == 2);
  CHECK(moved[0] == ElementRef{0, 2});
  CHECK(moved[1] == ElementRef{0, 3});
  CHECK(proj.compositions[0].elements[0].displayName == "Audio");
  CHECK(proj.compositions[0].elements[1].displayName == "LayerB");
  CHECK(proj.compositions[0].elements[2].displayName == "LayerA");
  CHECK(proj.compositions[0].elements[3].displayName == "LayerC");

  // Verify tracks followed their layers accurately
  REQUIRE(proj.desktopTracks.size() == 3);
  CHECK(proj.desktopTracks[0].elementIndex == 2);  // LayerA -> index 2
  CHECK(proj.desktopTracks[1].elementIndex == 1);  // LayerB -> index 1
  CHECK(proj.desktopTracks[2].elementIndex == 3);  // LayerC -> index 3

  // Cross-comp move: move Audio(0) & LayerA(2) into Comp 1 before BgImg(0)
  auto cross = moveElements(proj, {{0, 0}, {0, 2}}, 1, 0);
  REQUIRE(cross.size() == 2);
  CHECK(cross[0] == ElementRef{1, 0});
  CHECK(cross[1] == ElementRef{1, 1});
  REQUIRE(proj.compositions[0].elements.size() == 2);
  CHECK(proj.compositions[0].elements[0].displayName == "LayerB");
  CHECK(proj.compositions[0].elements[1].displayName == "LayerC");
  REQUIRE(proj.compositions[1].elements.size() == 3);
  CHECK(proj.compositions[1].elements[0].displayName == "Audio");
  CHECK(proj.compositions[1].elements[1].displayName == "LayerA");
  CHECK(proj.compositions[1].elements[2].displayName == "BgImg");

  // LayerA's track is now (1, 1); LayerB -> (0, 0); LayerC -> (0, 1)
  CHECK(proj.desktopTracks[0].compositionIndex == 1);
  CHECK(proj.desktopTracks[0].elementIndex == 1);
  CHECK(proj.desktopTracks[1].compositionIndex == 0);
  CHECK(proj.desktopTracks[1].elementIndex == 0);
  CHECK(proj.desktopTracks[2].compositionIndex == 0);
  CHECK(proj.desktopTracks[2].elementIndex == 1);
}

TEST_CASE("Hierarchy nudgeElements and moveElementsToBoundary preserve layer order and tracks") {
  using namespace iota::model;
  auto proj = makeHierarchyTestProject();

  // Boundary no-op when already at top
  auto noUp = nudgeElements(proj, {{0, 0}}, -1);
  CHECK(noUp == std::vector<ElementRef>{{0, 0}});
  CHECK(proj.compositions[0].elements[0].displayName == "Audio");

  // Nudge LayerB(2) & LayerC(3) up by 1
  auto up = nudgeElements(proj, {{0, 2}, {0, 3}}, -1);
  CHECK(up == std::vector<ElementRef>{{0, 1}, {0, 2}});
  CHECK(proj.compositions[0].elements[1].displayName == "LayerB");
  CHECK(proj.compositions[0].elements[2].displayName == "LayerC");
  CHECK(proj.compositions[0].elements[3].displayName == "LayerA");
  CHECK(proj.desktopTracks[0].elementIndex == 3);  // LayerA
  CHECK(proj.desktopTracks[1].elementIndex == 1);  // LayerB
  CHECK(proj.desktopTracks[2].elementIndex == 2);  // LayerC

  // Move LayerA(3) to top
  auto topMoved = moveElementsToBoundary(proj, {{0, 3}}, true);
  CHECK(topMoved == std::vector<ElementRef>{{0, 0}});
  CHECK(proj.compositions[0].elements[0].displayName == "LayerA");
  CHECK(proj.desktopTracks[0].elementIndex == 0);
}

TEST_CASE("Hierarchy copy, cut, paste, duplicate assign unique IDs and round-trip JSON clipboard") {
  using namespace iota::model;
  auto proj = makeHierarchyTestProject();

  const auto clipElems = copyElements(proj, {{0, 1}, {0, 3}});
  REQUIRE(clipElems.kind == HierarchyClipboard::Kind::Elements);
  REQUIRE(clipElems.elements.size() == 2);

  // Serialize -> parse element clipboard JSON
  const std::string jsonElems = iota::preset_io::serializeHierarchyClipboardToJson(clipElems);
  REQUIRE_FALSE(jsonElems.empty());
  const auto parsedElems = iota::preset_io::parseHierarchyClipboardFromJson(jsonElems);
  REQUIRE(parsedElems.has_value());
  CHECK(parsedElems->kind == HierarchyClipboard::Kind::Elements);
  REQUIRE(parsedElems->elements.size() == 2);
  CHECK(parsedElems->elements[0].tracks.size() == 1);

  // Malformed or plain clipboard strings fail safely
  CHECK_FALSE(iota::preset_io::parseHierarchyClipboardFromJson("plain text").has_value());
  CHECK_FALSE(iota::preset_io::parseHierarchyClipboardFromJson("{\"foo\": 1}").has_value());

  // Paste parsed clipboard into Comp 1
  const auto pastedRefs = pasteClipboard(proj, *parsedElems, 1, -1, false);
  REQUIRE(pastedRefs.size() == 2);
  CHECK(pastedRefs[0] == ElementRef{1, 1});
  CHECK(pastedRefs[1] == ElementRef{1, 2});
  CHECK(proj.compositions[1].elements[1].displayName == "LayerA");
  CHECK(proj.compositions[1].elements[2].displayName == "LayerC");

  // All _ids across the project must be strictly unique!
  std::set<int> ids;
  for (const auto& comp : proj.compositions) {
    for (const auto& el : comp.elements) {
      CHECK(ids.insert(el.runtimeId()).second);
    }
  }

  // Duplicate LayerA(1) in Comp 0
  const auto dups = duplicateElements(proj, {{0, 1}});
  REQUIRE(dups.size() == 1);
  CHECK(dups[0] == ElementRef{0, 2});
  CHECK(proj.compositions[0].elements[2].displayName == "LayerA Copy");
  CHECK(proj.compositions[0].elements[2].runtimeId() != proj.compositions[0].elements[1].runtimeId());

  // Cut LayerB(3 now) from Comp 0 and verify tracks & fallback selection
  std::optional<ElementRef> fallback;
  const auto cutClip = cutElements(proj, {{0, 3}}, &fallback);
  CHECK(cutClip.kind == HierarchyClipboard::Kind::Elements);
  REQUIRE(fallback.has_value());
  CHECK(*fallback == ElementRef{0, 3});  // LayerC shifted into slot 3

  // Composition clipboard round-trip
  const auto clipComp = copyComposition(proj, 1);
  const std::string jsonComp = iota::preset_io::serializeHierarchyClipboardToJson(clipComp);
  const auto parsedComp = iota::preset_io::parseHierarchyClipboardFromJson(jsonComp);
  REQUIRE(parsedComp.has_value());
  CHECK(parsedComp->kind == HierarchyClipboard::Kind::Composition);
  const auto pastedComp = pasteClipboard(proj, *parsedComp, 0, -1, false);
  REQUIRE(pastedComp.size() == 1);
  CHECK(pastedComp[0].compIdx == 2);
  CHECK(proj.compositions[2].displayName == "BG Comp Copy");
}

TEST_CASE("Hierarchy composition reorder and deletion remap references and tracks safely") {
  using namespace iota::model;
  Project proj;
  proj.version = "10";
  proj.compositions.push_back(createDefaultComposition(0));
  proj.compositions.push_back(createDefaultComposition(1));
  proj.compositions.push_back(createDefaultComposition(2));
  proj.compositions[1].displayName = "One";
  proj.compositions[2].displayName = "Two";

  // References in Comp 0 pointing to Comp 1 and Comp 2
  auto imgRef2 = createDefaultElement(ElementType::Image);
  imgRef2.props.setString("customImage", "composition:2", "img composition:0", "1_image");
  imgRef2.props.setString("MaskImage", "composition:1", "img composition:0", "1_image");
  auto blurRef2 = createDefaultElement(ElementType::BlurEffect);
  blurRef2.props.setInt("sourceCompositionIndex", 2, 0, 5, "1_appearance");
  auto shakeRef2 = createDefaultElement(ElementType::ShakeEffect);
  shakeRef2.props.setString("TargetImage", "composition:2", "img composition:0", "1_appearance");
  auto rgbRef1 = createDefaultElement(ElementType::RgbSplitEffect);
  rgbRef1.props.setString("targetImage", "composition:1", "img composition:0", "1_appearance");
  auto partRef2 = createDefaultElement(ElementType::Particles);
  partRef2.props.setString("emitterLayer", "composition:2", "img composition:0", "1_overall");

  proj.compositions[0].elements.push_back(std::move(imgRef2));
  proj.compositions[0].elements.push_back(std::move(blurRef2));
  proj.compositions[0].elements.push_back(std::move(shakeRef2));
  proj.compositions[0].elements.push_back(std::move(rgbRef1));
  proj.compositions[0].elements.push_back(std::move(partRef2));
  proj.desktopTracks.push_back({2, 0, "opacity", {{0.0, 1.0f}}});

  // Comp 0 cannot be moved or deleted
  CHECK_FALSE(moveComposition(proj, 0, 1));
  CHECK_FALSE(moveComposition(proj, 1, 0));
  CHECK_FALSE(deleteComposition(proj, 0));

  // Move Comp 2 ("Two") -> index 1
  REQUIRE(moveComposition(proj, 2, 1));
  CHECK(proj.compositions[1].displayName == "Two");
  CHECK(proj.compositions[2].displayName == "One");
  CHECK(proj.compositions[0].elements[0].props.getString("customImage") == "composition:1");
  CHECK(proj.compositions[0].elements[0].props.getString("MaskImage") == "composition:2");
  CHECK(proj.compositions[0].elements[1].props.getInt("sourceCompositionIndex") == 1);
  CHECK(proj.compositions[0].elements[2].props.getString("TargetImage") == "composition:1");
  CHECK(proj.compositions[0].elements[3].props.getString("targetImage") == "composition:2");
  CHECK(proj.compositions[0].elements[4].props.getString("emitterLayer") == "composition:1");
  REQUIRE(proj.desktopTracks.size() == 1);
  CHECK(proj.desktopTracks[0].compositionIndex == 1);

  // Delete Comp 1 ("Two"): references to 1 reset to comp 0, references to 2 shift to 1, comp 1 tracks drop
  REQUIRE(deleteComposition(proj, 1));
  REQUIRE(proj.compositions.size() == 2);
  CHECK(proj.compositions[1].displayName == "One");
  CHECK(proj.compositions[0].elements[0].props.getString("customImage") == "composition:0");
  CHECK(proj.compositions[0].elements[0].props.getString("MaskImage") == "composition:1");
  CHECK(proj.compositions[0].elements[1].props.getInt("sourceCompositionIndex") == 0);
  CHECK(proj.desktopTracks.empty());
}

TEST_CASE("Hierarchy precomposeElements and soloElementsInComposition behave like studio compositors") {
  using namespace iota::model;
  auto proj = makeHierarchyTestProject();

  // Solo LayerB(2): hides LayerA(1) & LayerC(3), keeps AudioProvider(0) visible, and toggles back!
  REQUIRE(soloElementsInComposition(proj, 0, {2}));
  CHECK(proj.compositions[0].elements[0].props.getBool("visible", true));
  CHECK_FALSE(proj.compositions[0].elements[1].props.getBool("visible", true));
  CHECK(proj.compositions[0].elements[2].props.getBool("visible", true));
  CHECK_FALSE(proj.compositions[0].elements[3].props.getBool("visible", true));
  CHECK(isElementSoloedInComposition(proj, 0, 2));

  // Second solo call on LayerB restores all visual layers
  REQUIRE(soloElementsInComposition(proj, 0, {2}));
  CHECK(proj.compositions[0].elements[1].props.getBool("visible", true));
  CHECK(proj.compositions[0].elements[2].props.getBool("visible", true));
  CHECK(proj.compositions[0].elements[3].props.getBool("visible", true));
  CHECK_FALSE(isElementSoloedInComposition(proj, 0, 2));

  // Pre-Compose LayerA(1) and LayerB(2) into a new Comp 2
  const auto res = precomposeElements(proj, 0, {1, 2}, "Spectrum Group");
  CHECK(res.newCompIdx == 2);
  CHECK(res.hostLayerRef == ElementRef{0, 1});
  REQUIRE(proj.compositions.size() == 3);
  CHECK(proj.compositions[2].displayName == "Spectrum Group");
  REQUIRE(proj.compositions[2].elements.size() == 2);
  CHECK(proj.compositions[2].elements[0].displayName == "LayerA");
  CHECK(proj.compositions[2].elements[1].displayName == "LayerB");

  // Comp 0 now has: Audio(0), Spectrum Group host(1), LayerC(2)
  REQUIRE(proj.compositions[0].elements.size() == 3);
  const auto& host = proj.compositions[0].elements[1];
  CHECK(host.type == ElementType::Image);
  CHECK(host.displayName == "Spectrum Group");
  CHECK(host.props.getString("customImage") == "composition:2");
  CHECK_FALSE(host.props.getBool("keepAspectRatio", true));
  CHECK(proj.compositions[0].elements[2].displayName == "LayerC");

  // Verify tracks: LayerA -> (2, 0), LayerB -> (2, 1), LayerC -> (0, 2)
  CHECK(proj.desktopTracks[0].compositionIndex == 2);
  CHECK(proj.desktopTracks[0].elementIndex == 0);
  CHECK(proj.desktopTracks[1].compositionIndex == 2);
  CHECK(proj.desktopTracks[1].elementIndex == 1);
  CHECK(proj.desktopTracks[2].compositionIndex == 0);
  CHECK(proj.desktopTracks[2].elementIndex == 2);
}
