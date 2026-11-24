#pragma once

#include "iota/core/update_checker.hpp"

#include <string>

namespace iota::studio {

struct UpdateModalActions {
  bool requestRecheck = false;
  bool requestOpenUrl = false;
  std::string urlToOpen;
  bool copiedLink = false;
};

/// Draws the "Check for Updates" modal dialog with release notes and direct GitHub links.
void renderUpdateModal(bool& openRequested,
                       const iota::core::UpdateCheckSnapshot& snap,
                       UpdateModalActions& actions);

}  // namespace iota::studio
