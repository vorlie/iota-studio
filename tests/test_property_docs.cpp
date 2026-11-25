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

#include "iota/model/measured_var.hpp"
#include "iota/model/property_docs.hpp"
#include "iota/model/scene.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace {

void checkEntryDocsRecursive(iota::model::ElementType ownerType,
                             const std::string& key,
                             const iota::model::PropertyEntry& entry) {
  CAPTURE(static_cast<int>(ownerType));
  CAPTURE(key);

  const auto* doc = iota::model::findPropertyDoc(ownerType, key);
  REQUIRE_MESSAGE(doc != nullptr, "Missing PropertyDoc for key: " << key);
  CHECK_FALSE(doc->displayName.empty());
  CHECK_FALSE(doc->summary.empty());

  if (!entry.groupTag.empty()) {
    CAPTURE(entry.groupTag);
    const auto* gdoc = iota::model::findGroupDoc(entry.groupTag);
    REQUIRE_MESSAGE(gdoc != nullptr, "Missing GroupDoc for groupTag: " << entry.groupTag);
    CHECK_FALSE(gdoc->displayName.empty());
    CHECK_FALSE(gdoc->summary.empty());
  }

  for (const auto& ch : entry.children) {
    checkEntryDocsRecursive(ownerType, ch.key, ch.entry);
  }
}

}  // namespace

TEST_CASE("PropertyDocs provides 100% coverage for all default elements, compositions, and MVar modes") {
  using namespace iota::model;

  SUBCASE("All element types and compositions have complete PropertyDoc and GroupDoc coverage") {
    std::vector<ElementType> types = addableElementTypes();
    types.push_back(ElementType::AppLogo);
    types.push_back(ElementType::StatText);

    for (ElementType t : types) {
      const auto& edoc = getElementDoc(t);
      CHECK(edoc.type == t);
      CHECK_FALSE(edoc.displayName.empty());
      CHECK_FALSE(edoc.category.empty());
      CHECK_FALSE(edoc.overview.empty());

      const Element el = createDefaultElement(t);
      for (const auto& item : el.props.items()) {
        checkEntryDocsRecursive(t, item.key, item.entry);
      }
    }

    const auto& compDoc = getElementDoc(ElementType::Composition);
    CHECK(compDoc.type == ElementType::Composition);
    CHECK_FALSE(compDoc.displayName.empty());
    CHECK_FALSE(compDoc.overview.empty());

    for (int idx : {0, 1}) {
      const Composition comp = createDefaultComposition(idx);
      for (const auto& item : comp.props.items()) {
        checkEntryDocsRecursive(ElementType::Composition, item.key, item.entry);
      }
    }
  }

  SUBCASE("All 26 MVar measureWhat modes have documented metadata") {
    const auto& modes = allMeasureWhatModes();
    REQUIRE(modes.size() == 26);
    for (std::string_view m : modes) {
      CAPTURE(m);
      const auto* doc = findMVarModeDoc(m);
      REQUIRE_MESSAGE(doc != nullptr, "Missing MVarModeDoc for mode: " << m);
      CHECK_FALSE(doc->displayName.empty());
      CHECK_FALSE(doc->summary.empty());
      CHECK_FALSE(doc->paramA.empty());
      CHECK_FALSE(doc->paramB.empty());
    }
  }

  SUBCASE("Fallback Title Case formatting and text token docs work cleanly") {
    CHECK(formatPropertyDisplayName(ElementType::Unknown, "customFooBar") == "Custom Foo Bar");
    CHECK(formatPropertyDisplayName(ElementType::Unknown, "my_legacy_prop") == "My Legacy Prop");
    CHECK(formatGroupDisplayName("3_customGroupName") == "Custom Group Name");

    const auto& tokens = allTextTokenDocs();
    CHECK(tokens.size() >= 10);
    bool hasArtistTitle = false;
    bool hasTimeCurr = false;
    bool hasFps = false;
    for (const auto& tk : tokens) {
      CHECK_FALSE(tk.token.empty());
      CHECK_FALSE(tk.summary.empty());
      if (tk.token == "{ArtistAndTitle}") hasArtistTitle = true;
      if (tk.token == "{TimeCurrent}") hasTimeCurr = true;
      if (tk.token == "{Fps}") hasFps = true;
    }
    CHECK(hasArtistTitle);
    CHECK(hasTimeCurr);
    CHECK(hasFps);
  }
}
