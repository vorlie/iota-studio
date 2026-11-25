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

#include <string>
#include <vector>

#include "iota/model/scene.hpp"

namespace iota::model {

class CommandStack {
public:
  explicit CommandStack(size_t maxEntries = 100) : maxEntries_(maxEntries) {}

  void clear(const Project& initialSnapshot);
  void pushSnapshot(std::string description, const Project& afterSnapshot);

  bool canUndo() const noexcept { return cursor_ > 0; }
  bool canRedo() const noexcept { return cursor_ + 1 < history_.size(); }

  std::string undoDescription() const;
  std::string redoDescription() const;

  const Project* undo() noexcept;
  const Project* redo() noexcept;

private:
  struct Entry {
    std::string description;
    Project snapshot;
  };
  size_t maxEntries_;
  std::vector<Entry> history_;
  size_t cursor_ = 0;
};

} // namespace iota::model
