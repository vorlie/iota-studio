#include "iota/discord/discord_presence.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef IOTA_HAS_DISCORD_SDK
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4189 4244 4267 4458 4996)
#endif
#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

namespace iota::discord {

namespace {

constexpr double kCallbackPumpIntervalSec = 0.10;
constexpr double kExportUpdateMinIntervalSec = 4.0;
constexpr double kHealthProbeIntervalSec = 8.0;
constexpr double kClearPumpTimeoutSec = 0.25;
constexpr size_t kMaxCompositionNameBytes = 96;
constexpr std::array<double, 4> kReconnectBackoffSec = {5.0, 10.0, 15.0, 30.0};

uint64_t currentEpochMs() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool isAsciiAlpha(unsigned char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool isUtf8ContinuationByte(unsigned char c) noexcept {
  return (c & 0xC0u) == 0x80u;
}

std::string trimAndCollapseSpaces(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  bool prevSpace = true;
  for (unsigned char ch : input) {
    if (ch < 0x20u || ch == 0x7Fu) {
      if (!prevSpace && !out.empty()) {
        out.push_back(' ');
        prevSpace = true;
      }
      continue;
    }
    if (ch == ' ') {
      if (!prevSpace) {
        out.push_back(' ');
        prevSpace = true;
      }
      continue;
    }
    out.push_back(static_cast<char>(ch));
    prevSpace = false;
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

int quantizedExportPercent(int encodedFrames, int totalFrames) noexcept {
  if (totalFrames <= 0) {
    return 0;
  }
  if (encodedFrames <= 0) {
    return 0;
  }
  if (encodedFrames >= totalFrames) {
    return 100;
  }
  const int64_t pct = (static_cast<int64_t>(encodedFrames) * 100) / static_cast<int64_t>(totalFrames);
  return std::clamp(static_cast<int>((pct / 5) * 5), 0, 95);
}

#ifdef _WIN32
constexpr const wchar_t* kDiscordSdkDllName = L"discord_partner_sdk.dll";

bool isWindowsSdkDllOnDiskOrLoaded() noexcept {
  if (GetModuleHandleW(kDiscordSdkDllName) != nullptr) {
    return true;
  }
  return SearchPathW(nullptr, kDiscordSdkDllName, nullptr, 0, nullptr, nullptr) > 0;
}
#endif

}  // namespace

std::string clampDiscordFieldUtf8(std::string_view text, size_t maxBytes) {
  if (maxBytes == 0 || text.empty()) {
    return {};
  }
  if (text.size() <= maxBytes) {
    std::string out(text);
    while (!out.empty() && out.back() == ' ') {
      out.pop_back();
    }
    return out;
  }
  size_t len = maxBytes;
  while (len > 0 && isUtf8ContinuationByte(static_cast<unsigned char>(text[len]))) {
    --len;
  }
  std::string out(text.substr(0, len));
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string sanitizeCompositionName(std::string_view rawName) {
  std::string_view sv = rawName;

  // Never allow directory traversal, UNC paths, or Windows drive prefixes in presence strings.
  while (!sv.empty() && (sv.back() == '/' || sv.back() == '\\')) {
    sv.remove_suffix(1);
  }
  const size_t lastSlash = sv.find_last_of("/\\");
  if (lastSlash != std::string_view::npos) {
    sv.remove_prefix(lastSlash + 1);
  }
  if (sv.size() >= 2 && isAsciiAlpha(static_cast<unsigned char>(sv[0])) && sv[1] == ':') {
    sv.remove_prefix(2);
  }

  std::string cleaned = trimAndCollapseSpaces(sv);
  if (cleaned.empty() || cleaned == "." || cleaned == "..") {
    return {};
  }

  return clampDiscordFieldUtf8(cleaned, kMaxCompositionNameBytes);
}

PresencePayload formatPresencePayload(const PresenceSnapshot& snapshot, bool expanded) {
  PresencePayload payload;
  payload.largeImageKey = std::string(DiscordConfig::kLargeImageKey);
  payload.largeImageText = std::string(DiscordConfig::kLargeImageText);

  const std::string compName = expanded ? sanitizeCompositionName(snapshot.compositionName) : std::string{};

  switch (snapshot.state) {
    case PresenceActivityState::Editing:
      payload.details = "Creating a composition";
      if (expanded) {
        payload.state = compName.empty() ? "Editing layers" : ("Composition: " + compName);
      }
      break;

    case PresenceActivityState::Previewing:
      payload.details = "Previewing a composition";
      if (expanded) {
        payload.state = compName.empty() ? "Live audio preview" : ("Composition: " + compName);
      }
      break;

    case PresenceActivityState::Exporting:
      payload.details = "Rendering a video";
      if (expanded) {
        const bool hasTotal = snapshot.exportTotalFrames > 0;
        const int pct = quantizedExportPercent(snapshot.exportEncodedFrames, snapshot.exportTotalFrames);
        if (!compName.empty() && hasTotal) {
          payload.state = compName + " (" + std::to_string(pct) + "%)";
        } else if (!compName.empty()) {
          payload.state = "Composition: " + compName;
        } else if (hasTotal) {
          payload.state = "Progress: " + std::to_string(pct) + "%";
        } else {
          payload.state = "Encoding video";
        }
      }
      break;

    case PresenceActivityState::Idle:
    default:
      payload.details = "Using Iota Studio";
      if (expanded) {
        payload.state = "Idle";
      }
      break;
  }

  payload.details = clampDiscordFieldUtf8(payload.details, 128);
  if (payload.details.size() < 2) {
    payload.details = "Using Iota Studio";
  }
  if (payload.state.has_value()) {
    *payload.state = clampDiscordFieldUtf8(*payload.state, 128);
    if (payload.state->size() < 2) {
      payload.state.reset();
    }
  }

  return payload;
}

struct DiscordPresence::Impl {
#ifdef IOTA_HAS_DISCORD_SDK
  struct CallbackState {
    std::mutex mutex;
    bool alive = true;
    uint64_t generation = 0;

    bool hasUpdateResult = false;
    bool updateSuccess = false;
    std::string updateError;
    uint64_t updateResultGen = 0;

    bool hasProbeResult = false;
    bool probeConnected = false;
    uint64_t probeResultGen = 0;
  };

  std::shared_ptr<CallbackState> cbState = std::make_shared<CallbackState>();
  std::unique_ptr<discordpp::Client> client;
  bool runtimeChecked = false;
  bool runtimeLoadOk = false;
#endif

  bool enabled = false;
  bool expanded = false;
  bool dirty = false;
  bool clearing = false;
  bool inFlightUpdate = false;
  bool inFlightProbe = false;
  bool needsClientRecreate = false;

  DiscordConnectionState connectionState = DiscordConnectionState::Disabled;
  PresenceSnapshot snapshot;
  PresenceActivityState lastMajorState = PresenceActivityState::Idle;
  std::string lastSanitizedComp;
  std::optional<PresencePayload> lastPublishedPayload;
  std::optional<PresencePayload> inFlightPayload;

  int consecutiveFailures = 0;
  double lastTickWallSec = -1000.0;
  double lastCallbackPumpWallSec = -1000.0;
  double lastPublishWallSec = -1000.0;
  double nextRetryWallSec = -1000.0;
  double nextProbeWallSec = -1000.0;
  double clearDeadlineWallSec = -1000.0;
  uint64_t stateStartEpochMs = 0;
  std::string lastErrorSummary;

#ifdef IOTA_HAS_DISCORD_SDK
  bool ensureRuntimeLoaded() noexcept {
    if (runtimeChecked) {
      return runtimeLoadOk;
    }
    runtimeChecked = true;
#ifdef _WIN32
    HMODULE mod = GetModuleHandleW(kDiscordSdkDllName);
    if (mod == nullptr) {
      mod = LoadLibraryW(kDiscordSdkDllName);
    }
    runtimeLoadOk = (mod != nullptr);
#else
    runtimeLoadOk = true;
#endif
    return runtimeLoadOk;
  }

  void destroyClient() noexcept {
    {
      std::lock_guard<std::mutex> lock(cbState->mutex);
      ++cbState->generation;
      cbState->hasUpdateResult = false;
      cbState->hasProbeResult = false;
    }
    inFlightUpdate = false;
    inFlightProbe = false;
    inFlightPayload.reset();
    if (client) {
      try {
        client->Drop();
      } catch (...) {
      }
      client.reset();
    }
    clearing = false;
    needsClientRecreate = false;
  }

  bool ensureClientCreated() noexcept {
    if (client) {
      return true;
    }
    if (!ensureRuntimeLoaded()) {
      connectionState = DiscordConnectionState::SdkUnavailable;
      lastErrorSummary = "Discord runtime library not found";
      return false;
    }
    try {
      auto newClient = std::make_unique<discordpp::Client>();
      uint64_t gen = 0;
      {
        std::lock_guard<std::mutex> lock(cbState->mutex);
        gen = ++cbState->generation;
        cbState->hasUpdateResult = false;
        cbState->hasProbeResult = false;
      }
      auto weakState = std::weak_ptr<CallbackState>(cbState);
      newClient->AddLogCallback(
          [weakState, gen](std::string message, discordpp::LoggingSeverity severity) {
            if (severity == discordpp::LoggingSeverity::Error) {
              if (auto locked = weakState.lock()) {
                std::lock_guard<std::mutex> guard(locked->mutex);
                if (locked->alive && locked->generation == gen && locked->updateError.empty()) {
                  locked->updateError = clampDiscordFieldUtf8(message, 96);
                }
              }
            }
          },
          discordpp::LoggingSeverity::Warning);
      newClient->SetApplicationId(DiscordConfig::kApplicationId);
      client = std::move(newClient);
      needsClientRecreate = false;
      connectionState = DiscordConnectionState::Connecting;
      if (stateStartEpochMs == 0) {
        stateStartEpochMs = currentEpochMs();
      }
      return true;
    } catch (...) {
      connectionState = DiscordConnectionState::Unavailable;
      lastErrorSummary = "Failed to create Discord client";
      return false;
    }
  }

  void drainAsyncCallbacks(double nowWallSec) noexcept {
    bool gotUpdate = false;
    bool updateOk = false;
    std::string updateErr;

    bool gotProbe = false;
    bool probeOk = false;

    {
      std::lock_guard<std::mutex> lock(cbState->mutex);
      const uint64_t activeGen = cbState->generation;
      if (cbState->hasUpdateResult) {
        cbState->hasUpdateResult = false;
        if (cbState->updateResultGen == activeGen) {
          gotUpdate = true;
          updateOk = cbState->updateSuccess;
          updateErr = std::move(cbState->updateError);
          cbState->updateError.clear();
        }
      }
      if (cbState->hasProbeResult) {
        cbState->hasProbeResult = false;
        if (cbState->probeResultGen == activeGen) {
          gotProbe = true;
          probeOk = cbState->probeConnected;
        }
      }
    }

    if (gotUpdate) {
      inFlightUpdate = false;
      if (updateOk) {
        connectionState = DiscordConnectionState::Connected;
        consecutiveFailures = 0;
        lastErrorSummary.clear();
        if (inFlightPayload.has_value()) {
          lastPublishedPayload = inFlightPayload;
        }
        nextProbeWallSec = nowWallSec + kHealthProbeIntervalSec;
      } else {
        connectionState = DiscordConnectionState::Unavailable;
        lastPublishedPayload.reset();
        lastErrorSummary = updateErr.empty() ? "Discord client unavailable" : std::move(updateErr);
        const size_t idx = static_cast<size_t>(
            std::min(consecutiveFailures, static_cast<int>(kReconnectBackoffSec.size() - 1)));
        const double backoff = kReconnectBackoffSec[idx];
        ++consecutiveFailures;
        nextRetryWallSec = nowWallSec + backoff;
        if (consecutiveFailures >= 2) {
          needsClientRecreate = true;
        }
      }
      inFlightPayload.reset();
    }

    if (gotProbe) {
      inFlightProbe = false;
      if (!probeOk) {
        if (connectionState == DiscordConnectionState::Connected) {
          connectionState = DiscordConnectionState::Unavailable;
          lastPublishedPayload.reset();
          dirty = true;
          needsClientRecreate = true;
          nextRetryWallSec = nowWallSec + kReconnectBackoffSec[0];
        }
      } else {
        nextProbeWallSec = nowWallSec + kHealthProbeIntervalSec;
      }
    }
  }

  void sendPresenceUpdate(const PresencePayload& payload, double nowWallSec) noexcept {
    if (!client) {
      return;
    }
    try {
      discordpp::Activity activity;
      activity.SetType(discordpp::ActivityTypes::Playing);
      activity.SetDetails(payload.details);
      if (payload.state.has_value()) {
        activity.SetState(*payload.state);
      }

      discordpp::ActivityAssets assets;
      assets.SetLargeImage(payload.largeImageKey);
      assets.SetLargeText(payload.largeImageText);
      activity.SetAssets(std::move(assets));

      if (stateStartEpochMs > 0) {
        discordpp::ActivityTimestamps ts;
        ts.SetStart(stateStartEpochMs);
        activity.SetTimestamps(std::move(ts));
      }

      uint64_t gen = 0;
      {
        std::lock_guard<std::mutex> lock(cbState->mutex);
        gen = cbState->generation;
      }

      inFlightUpdate = true;
      inFlightPayload = payload;
      lastPublishWallSec = nowWallSec;
      dirty = false;

      auto weakState = std::weak_ptr<CallbackState>(cbState);
      client->UpdateRichPresence(
          std::move(activity), [weakState, gen](discordpp::ClientResult result) {
            if (auto locked = weakState.lock()) {
              std::lock_guard<std::mutex> guard(locked->mutex);
              if (!locked->alive || locked->generation != gen) {
                return;
              }
              locked->hasUpdateResult = true;
              locked->updateResultGen = gen;
              locked->updateSuccess = result.Successful();
              if (!result.Successful()) {
                locked->updateError = clampDiscordFieldUtf8(result.ToString(), 96);
              }
            }
          });
    } catch (...) {
      inFlightUpdate = false;
      inFlightPayload.reset();
      connectionState = DiscordConnectionState::Unavailable;
      nextRetryWallSec = nowWallSec + kReconnectBackoffSec[0];
    }
  }

  void startConnectionProbe(double nowWallSec) noexcept {
    if (!client || inFlightProbe || inFlightUpdate) {
      return;
    }
    try {
      uint64_t gen = 0;
      {
        std::lock_guard<std::mutex> lock(cbState->mutex);
        gen = cbState->generation;
      }
      inFlightProbe = true;
      nextProbeWallSec = nowWallSec + kHealthProbeIntervalSec;
      auto weakState = std::weak_ptr<CallbackState>(cbState);
      client->GetDiscordClientConnectedUser(
          DiscordConfig::kApplicationId,
          [weakState, gen](discordpp::ClientResult result,
                           std::optional<discordpp::UserHandle> user) {
            if (auto locked = weakState.lock()) {
              std::lock_guard<std::mutex> guard(locked->mutex);
              if (!locked->alive || locked->generation != gen) {
                return;
              }
              locked->hasProbeResult = true;
              locked->probeResultGen = gen;
              locked->probeConnected = result.Successful() && user.has_value();
            }
          });
    } catch (...) {
      inFlightProbe = false;
    }
  }

  void beginDisableClear() noexcept {
    if (!client) {
      connectionState = DiscordConnectionState::Disabled;
      return;
    }
    {
      std::lock_guard<std::mutex> lock(cbState->mutex);
      ++cbState->generation;
      cbState->hasUpdateResult = false;
      cbState->hasProbeResult = false;
    }
    inFlightUpdate = false;
    inFlightProbe = false;
    inFlightPayload.reset();
    lastPublishedPayload.reset();
    try {
      client->ClearRichPresence();
      discordpp::RunCallbacks();
    } catch (...) {
    }
    clearing = true;
    clearDeadlineWallSec = lastTickWallSec + kClearPumpTimeoutSec;
    connectionState = DiscordConnectionState::Disabled;
  }
#endif
};

DiscordPresence::DiscordPresence() : impl_(std::make_unique<Impl>()) {}

DiscordPresence::~DiscordPresence() {
  shutdown();
}

bool DiscordPresence::isSdkCompiled() noexcept {
#ifdef IOTA_HAS_DISCORD_SDK
  return true;
#else
  return false;
#endif
}

bool DiscordPresence::isSdkAvailable() const noexcept {
#ifdef IOTA_HAS_DISCORD_SDK
  if (impl_->runtimeChecked) {
    return impl_->runtimeLoadOk;
  }
#ifdef _WIN32
  return isWindowsSdkDllOnDiskOrLoaded();
#else
  return true;
#endif
#else
  return false;
#endif
}

void DiscordPresence::setEnabled(bool enabled) {
  if (impl_->enabled == enabled) {
    return;
  }
  impl_->enabled = enabled;
  impl_->lastErrorSummary.clear();

  if (!enabled) {
    impl_->dirty = false;
    impl_->consecutiveFailures = 0;
#ifdef IOTA_HAS_DISCORD_SDK
    impl_->beginDisableClear();
#else
    impl_->connectionState = DiscordConnectionState::Disabled;
    impl_->lastPublishedPayload.reset();
#endif
    return;
  }

  if (!isSdkAvailable()) {
    impl_->connectionState = DiscordConnectionState::SdkUnavailable;
    impl_->lastErrorSummary = "Discord Social SDK runtime not available";
    return;
  }

  impl_->clearing = false;
  impl_->dirty = true;
  impl_->consecutiveFailures = 0;
  impl_->nextRetryWallSec = -1000.0;
  impl_->stateStartEpochMs = currentEpochMs();
  impl_->connectionState = DiscordConnectionState::Connecting;
}

void DiscordPresence::setExpanded(bool expanded) {
  if (impl_->expanded == expanded) {
    return;
  }
  impl_->expanded = expanded;
  if (impl_->enabled) {
    impl_->dirty = true;
  }
}

bool DiscordPresence::isEnabled() const noexcept {
  return impl_->enabled;
}

bool DiscordPresence::isExpanded() const noexcept {
  return impl_->expanded;
}

void DiscordPresence::updateState(const PresenceSnapshot& snapshot) {
  const bool majorChanged = (snapshot.state != impl_->lastMajorState);
  const std::string sanitizedComp = sanitizeCompositionName(snapshot.compositionName);
  const bool compChanged = (sanitizedComp != impl_->lastSanitizedComp);

  impl_->snapshot = snapshot;
  if (majorChanged) {
    impl_->lastMajorState = snapshot.state;
    impl_->stateStartEpochMs = currentEpochMs();
    impl_->dirty = true;
  }
  if (compChanged) {
    impl_->lastSanitizedComp = sanitizedComp;
    if (impl_->expanded) {
      impl_->dirty = true;
    }
  }
}

void DiscordPresence::tick(double nowWallSec) {
  impl_->lastTickWallSec = nowWallSec;

#ifndef IOTA_HAS_DISCORD_SDK
  (void)nowWallSec;
  return;
#else
  if (!impl_->enabled) {
    if (impl_->clearing && impl_->client) {
      if (nowWallSec - impl_->lastCallbackPumpWallSec >= kCallbackPumpIntervalSec) {
        impl_->lastCallbackPumpWallSec = nowWallSec;
        try {
          discordpp::RunCallbacks();
        } catch (...) {
        }
      }
      if (nowWallSec >= impl_->clearDeadlineWallSec) {
        impl_->destroyClient();
      }
    }
    return;
  }

  if (impl_->connectionState == DiscordConnectionState::SdkUnavailable) {
    return;
  }

  if (impl_->needsClientRecreate && !impl_->inFlightUpdate && nowWallSec >= impl_->nextRetryWallSec) {
    impl_->destroyClient();
  }

  if (!impl_->client) {
    if (nowWallSec < impl_->nextRetryWallSec) {
      return;
    }
    if (!impl_->ensureClientCreated()) {
      return;
    }
    impl_->dirty = true;
  }

  if (nowWallSec - impl_->lastCallbackPumpWallSec >= kCallbackPumpIntervalSec) {
    impl_->lastCallbackPumpWallSec = nowWallSec;
    try {
      discordpp::RunCallbacks();
    } catch (...) {
    }
  }

  impl_->drainAsyncCallbacks(nowWallSec);

  const PresencePayload desired = formatPresencePayload(impl_->snapshot, impl_->expanded);
  const bool payloadChanged =
      !impl_->lastPublishedPayload.has_value() || (*impl_->lastPublishedPayload != desired);

  const bool isUnavailable = (impl_->connectionState == DiscordConnectionState::Unavailable);
  const bool shouldRetryUnavailable = isUnavailable && (nowWallSec >= impl_->nextRetryWallSec);

  if (!impl_->inFlightUpdate && (!isUnavailable || shouldRetryUnavailable) &&
      (payloadChanged || shouldRetryUnavailable || impl_->dirty)) {
    const bool allowImmediate =
        impl_->dirty || shouldRetryUnavailable || !impl_->lastPublishedPayload.has_value() ||
        (nowWallSec - impl_->lastPublishWallSec >= kExportUpdateMinIntervalSec);
    if (allowImmediate) {
      impl_->sendPresenceUpdate(desired, nowWallSec);
    }
  }

  if (impl_->connectionState == DiscordConnectionState::Connected && !impl_->inFlightUpdate &&
      !impl_->inFlightProbe && nowWallSec >= impl_->nextProbeWallSec) {
    impl_->startConnectionProbe(nowWallSec);
  }
#endif
}

void DiscordPresence::shutdown() {
  impl_->enabled = false;
  impl_->dirty = false;
  impl_->lastPublishedPayload.reset();
#ifdef IOTA_HAS_DISCORD_SDK
  {
    std::lock_guard<std::mutex> lock(impl_->cbState->mutex);
    impl_->cbState->alive = false;
    ++impl_->cbState->generation;
  }
  if (impl_->client) {
    try {
      impl_->client->ClearRichPresence();
      for (int i = 0; i < 5; ++i) {
        discordpp::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    } catch (...) {
    }
    impl_->destroyClient();
  }
#endif
  impl_->connectionState = DiscordConnectionState::Disabled;
}

DiscordConnectionState DiscordPresence::connectionState() const noexcept {
  return impl_->connectionState;
}

std::string DiscordPresence::statusText() const {
  switch (impl_->connectionState) {
    case DiscordConnectionState::Disabled:
      return "Disabled";
    case DiscordConnectionState::SdkUnavailable:
      return "Discord SDK runtime unavailable";
    case DiscordConnectionState::Connecting:
      return "Connecting to Discord...";
    case DiscordConnectionState::Connected:
      return "Connected to Discord";
    case DiscordConnectionState::Unavailable:
      return "Discord not detected (retrying...)";
    default:
      return "Unknown";
  }
}

PresencePayload DiscordPresence::currentDesiredPayload() const {
  return formatPresencePayload(impl_->snapshot, impl_->expanded);
}

std::optional<PresencePayload> DiscordPresence::lastPublishedPayload() const {
  return impl_->lastPublishedPayload;
}

}  // namespace iota::discord
