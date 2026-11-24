#pragma once

#include "iota/model/property.hpp"
#include "iota/model/scene.hpp"

#include <functional>
#include <string>

namespace iota::studio {

struct InspectorActions {
  bool modified = false;
  bool undoSnapshotNeeded = false;
  std::string editLabel;
  std::string addKeyframeProperty;
  float addKeyframeValue = 0.0f;
  bool requestLocalAssetImport = false;
  bool requestLocalFontImport = false;
  std::string assetTargetPropName;
  bool showHiddenProperties = false;
  bool preciseNumericInputs = false;
  bool showRawPropertyKeys = false;
  std::string propertyFilter;
};

void renderPropertyBagInspector(iota::model::PropertyBag& bag,
                                iota::model::ElementType ownerType,
                                InspectorActions& actions,
                                std::function<void()> onBeforeDiscreteEdit = {});

}  // namespace iota::studio
