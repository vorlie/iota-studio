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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace iota::discord {

struct DiscordConfig {
  static constexpr uint64_t kApplicationId = 1574946746683891832ULL;
  static constexpr std::string_view kLargeImageKey = "icon";
  static constexpr std::string_view kLargeImageText = "Iota Studio";
};

enum class PresenceActivityState {
  Idle = 0,
  Editing,
  Previewing,
  Exporting,
};

enum class DiscordConnectionState {
  Disabled = 0,
  SdkUnavailable,
  Connecting,
  Connected,
  Unavailable,
};

struct PresenceSnapshot {
  PresenceActivityState state = PresenceActivityState::Idle;
  std::string compositionName;
  int exportEncodedFrames = 0;
  int exportTotalFrames = 0;
};

struct PresencePayload {
  std::string details;
  std::optional<std::string> state;
  std::string largeImageKey;
  std::string largeImageText;

  bool operator==(const PresencePayload&) const = default;
};

/// Truncates UTF-8 text cleanly on a codepoint boundary to at most maxBytes bytes.
[[nodiscard]] std::string clampDiscordFieldUtf8(std::string_view text, size_t maxBytes = 128);

/// Sanitizes a composition display name for Rich Presence:
/// - Strips control characters and collapses whitespace
/// - Removes any directory path components or drive prefixes to avoid leaking file paths
/// - Clamps length to safe bounds on a UTF-8 codepoint boundary
[[nodiscard]] std::string sanitizeCompositionName(std::string_view rawName);

/// Builds the Discord Rich Presence payload for the current application state.
[[nodiscard]] PresencePayload formatPresencePayload(const PresenceSnapshot& snapshot, bool expanded);

/// Optional, non-blocking Discord Rich Presence manager built on the Discord Social SDK.
class DiscordPresence {
 public:
  DiscordPresence();
  ~DiscordPresence();

  DiscordPresence(const DiscordPresence&) = delete;
  DiscordPresence& operator=(const DiscordPresence&) = delete;

  /// True when compiled with Discord Social SDK headers/import library.
  [[nodiscard]] static bool isSdkCompiled() noexcept;

  /// True when compiled with the SDK and the runtime shared library is available.
  [[nodiscard]] bool isSdkAvailable() const noexcept;

  /// Master enable/disable toggle. When disabled, no SDK client is initialized and any
  /// published presence is cleared cleanly.
  void setEnabled(bool enabled);

  /// Secondary toggle controlling contextual details (composition name & export progress).
  /// Independent of setEnabled() and triggers an immediate presence refresh when enabled.
  void setExpanded(bool expanded);

  [[nodiscard]] bool isEnabled() const noexcept;
  [[nodiscard]] bool isExpanded() const noexcept;

  /// Records the latest application activity snapshot. Non-blocking; deduplicates and
  /// throttles redundant updates automatically in tick().
  void updateState(const PresenceSnapshot& snapshot);

  /// Pumps SDK callbacks, manages connection health / bounded retries, and publishes
  /// pending Rich Presence updates when appropriate. Safe to call once per UI frame.
  void tick(double nowWallSec);

  /// Clears any active presence, pumps callbacks briefly, and releases the SDK client.
  void shutdown();

  [[nodiscard]] DiscordConnectionState connectionState() const noexcept;
  [[nodiscard]] std::string statusText() const;
  [[nodiscard]] PresencePayload currentDesiredPayload() const;
  [[nodiscard]] std::optional<PresencePayload> lastPublishedPayload() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace iota::discord
