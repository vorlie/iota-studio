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
