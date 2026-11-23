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
