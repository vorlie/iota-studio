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

#include "iota/model/command_stack.hpp"

#include <cstddef>

namespace iota::model {

void CommandStack::clear(const Project& initialSnapshot) {
  history_.clear();
  history_.push_back({"Initial", initialSnapshot});
  cursor_ = 0;
}

void CommandStack::pushSnapshot(std::string description, const Project& afterSnapshot) {
  if (history_.empty()) {
    clear(afterSnapshot);
    return;
  }
  if (cursor_ + 1 < history_.size()) {
    history_.erase(history_.begin() + static_cast<ptrdiff_t>(cursor_ + 1), history_.end());
  }
  history_.push_back({std::move(description), afterSnapshot});
  if (history_.size() > maxEntries_) {
    history_.erase(history_.begin());
  }
  cursor_ = history_.size() - 1;
}

std::string CommandStack::undoDescription() const {
  return canUndo() ? history_[cursor_].description : "";
}

std::string CommandStack::redoDescription() const {
  return canRedo() ? history_[cursor_ + 1].description : "";
}

const Project* CommandStack::undo() noexcept {
  if (!canUndo()) return nullptr;
  --cursor_;
  return &history_[cursor_].snapshot;
}

const Project* CommandStack::redo() noexcept {
  if (!canRedo()) return nullptr;
  ++cursor_;
  return &history_[cursor_].snapshot;
}

} // namespace iota::model
