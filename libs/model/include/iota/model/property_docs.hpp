#pragma once

#include "iota/model/scene.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace iota::model {

struct PropertyDoc {
  std::string_view rawKey;
  std::string_view displayName;
  std::string_view summary;
  std::string_view usage;
  std::string_view unit;
};

struct GroupDoc {
  std::string_view rawTag;
  std::string_view displayName;
  std::string_view summary;
};

struct ElementDoc {
  ElementType type = ElementType::Unknown;
  std::string_view displayName;
  std::string_view category;
  std::string_view overview;
  std::string_view tips;
};

struct MVarModeDoc {
  std::string_view mode;
  std::string_view displayName;
  std::string_view summary;
  std::string_view paramA;
  std::string_view paramB;
};

struct TextTokenDoc {
  std::string_view token;
  std::string_view summary;
};

/// Finds documentation for a property, checking element-specific overrides before shared base keys.
[[nodiscard]] const PropertyDoc* findPropertyDoc(ElementType ownerType, std::string_view rawKey) noexcept;

/// Returns registered human-readable display name or formats an unknown key cleanly into Title Case.
[[nodiscard]] std::string formatPropertyDisplayName(ElementType ownerType, std::string_view rawKey);

/// Finds documentation for an Inspector property section tag (e.g., "2_spectrumHz").
[[nodiscard]] const GroupDoc* findGroupDoc(std::string_view rawGroupTag) noexcept;

/// Returns registered section title or formats an unknown group tag into clean Title Case.
[[nodiscard]] std::string formatGroupDisplayName(std::string_view rawGroupTag);

/// Returns overview documentation for an element type (or Composition).
[[nodiscard]] const ElementDoc& getElementDoc(ElementType type) noexcept;

/// Ordered list of all documented Element & Composition pages for the Built-in Documentation panel.
[[nodiscard]] const std::vector<ElementDoc>& allElementDocs() noexcept;

/// Finds documentation for an MVar signal/driver mode (such as "Beat" or "SoundKeys1").
[[nodiscard]] const MVarModeDoc* findMVarModeDoc(std::string_view mode) noexcept;

/// All documented MVar signal modes matching `allMeasureWhatModes()`.
[[nodiscard]] const std::vector<MVarModeDoc>& allMVarModeDocs() noexcept;

/// All supported dynamic text placeholder tokens for Text/StatText layers.
[[nodiscard]] const std::vector<TextTokenDoc>& allTextTokenDocs() noexcept;

}  // namespace iota::model
