#include "iota/dsp/mvar_evaluator.hpp"

#include "iota/dsp/filters.hpp"
#include "iota/dsp/windows.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace iota::dsp {

namespace {

float fractf(float x) noexcept {
  return x - std::floor(x);
}

float lerpf(float a, float b, float t) noexcept {
  return a + (b - a) * t;
}

/// Clean-room deterministic harmonic shake synthesizer replacing static APK shake LUT tables.
/// Produces band-limited smooth pseudo-random XY [0, 1] and rotation radians.
iota::core::Vec3 sampleHarmonicShake(float phase01, bool moreChaotic) noexcept {
  const float p = fractf(phase01) * kTwoPi;
  const float freqBoost = moreChaotic ? 2.0f : 1.0f;

  // Deterministic orthogonal multi-sine superposition with irrational-like integer harmonic ratios
  const float sx = 0.50f * std::sin(p * 3.0f * freqBoost + 0.41f) +
                   0.30f * std::sin(p * 7.0f * freqBoost + 1.87f) +
                   0.20f * std::cos(p * 11.0f * freqBoost + 2.93f);
  const float sy = 0.50f * std::cos(p * 4.0f * freqBoost + 0.92f) +
                   0.30f * std::sin(p * 9.0f * freqBoost + 2.15f) +
                   0.20f * std::sin(p * 13.0f * freqBoost + 4.11f);
  const float sr = 0.55f * std::sin(p * 5.0f * freqBoost + 1.23f) +
                   0.30f * std::cos(p * 8.0f * freqBoost + 3.07f) +
                   0.15f * std::sin(p * 14.0f * freqBoost + 0.65f);

  return iota::core::Vec3{
      std::clamp(0.5f + 0.5f * sx, 0.0f, 1.0f),
      std::clamp(0.5f + 0.5f * sy, 0.0f, 1.0f),
      sr * (moreChaotic ? 0.6f : 0.35f),
  };
}

/// Deterministic fast hash-based 2D random vector in [-1, 1] for BeatRandomShake.
iota::core::Vec2 samplePseudoRand2(uint32_t seed) noexcept {
  uint32_t x = seed * 747796405u + 2891336453u;
  x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
  x = (x >> 22u) ^ x;

  uint32_t y = (seed ^ 0x9e3779b9u) * 747796405u + 2891336453u;
  y = ((y >> ((y >> 28u) + 4u)) ^ y) * 277803737u;
  y = (y >> 22u) ^ y;

  const float fx = static_cast<float>(x & 0xFFFFu) / 32767.5f - 1.0f;
  const float fy = static_cast<float>(y & 0xFFFFu) / 32767.5f - 1.0f;
  return {fx, fy};
}

iota::core::Vec2 evalSlot(const iota::model::MeasuredVar& slot,
                          MVarSlotState& state,
                          const MVarEvalContext& ctx) noexcept {
  const std::string_view m = slot.measureWhat;
  const float a = slot.a;
  const float b = slot.b;
  const bool active = ctx.isPlaying || ctx.isExporting;
  const float time = static_cast<float>(ctx.totalTimeSec);

  if (m.empty() || m == "Nothing") {
    return {0.0f, 0.0f};
  }
  if (m == "Constant") {
    return {a, b};
  }
  if (m == "IsPlaying") {
    return active ? iota::core::Vec2{1.0f, 1.0f} : iota::core::Vec2{0.0f, 0.0f};
  }
  if (m == "Beat") {
    return {a * ctx.beat, b * ctx.beat};
  }
  if (m == "TrackPosition") {
    const float p = (ctx.trackDurationSec > 1e-4)
                        ? std::clamp(static_cast<float>(ctx.trackPositionSec / ctx.trackDurationSec), 0.0f, 1.0f)
                        : 0.0f;
    return {a * p, b * p};
  }
  if (m == "TotalTime") {
    return {fractf(a * time), fractf(b * time)};
  }
  if (m == "TotalTimeBackward") {
    return {1.0f - fractf(a * time), 1.0f - fractf(b * time)};
  }
  if (m == "TotalTimeWhenPlaying") {
    const float playTime = static_cast<float>(ctx.playAccumTimeSec);
    return {fractf(a * playTime), fractf(b * playTime)};
  }
  if (m == "TotalTimeAndBeat") {
    if (active) {
      const float v = state.prev.x + ctx.dt * a + ctx.beat * b * 0.1f * (ctx.dt * 60.0f);
      state.prev = {v, v};
    }
    return state.prev;
  }
  if (m == "PeakBarIndex") {
    const float followAlpha = frameRateIndependentAlpha(1.0f - std::clamp(b, 0.0f, 1.0f), ctx.dt);
    const float v = lerpf(ctx.peakBarIndex, state.prev.x, 1.0f - followAlpha) * a;
    state.prev = {v, v};
    return state.prev;
  }
  if (m == "BeatRandomShake") {
    const float mag = std::clamp(active ? (ctx.beat * 2.5f) : 0.0f, 0.0f, 1.0f);
    const float m3 = mag * mag * mag;
    const uint32_t timeSeed = static_cast<uint32_t>(time * 60.0f) + static_cast<uint32_t>(time * 120.0f);
    const auto rnd = samplePseudoRand2(timeSeed);
    return {rnd.x * m3 * a * 0.025f, rnd.y * m3 * a * 0.025f};
  }
  if (m == "ConstantShake" || m == "ConstantShakeMore") {
    const bool more = (m == "ConstantShakeMore");
    const auto s = sampleHarmonicShake(time * 0.5f * b, more);
    const iota::core::Vec2 target{(s.x - 0.5f) * a * 0.06f, (s.y - 0.5f) * a * 0.06f};
    const float alpha = frameRateIndependentAlpha(0.1f, ctx.dt);
    state.prev = {lerpf(state.prev.x, target.x, alpha), lerpf(state.prev.y, target.y, alpha)};
    return state.prev;
  }
  if (m == "BeatCamShakeLess" || m == "BeatCamShakeMore") {
    const bool more = (m == "BeatCamShakeMore");
    const float mag = std::clamp(active ? (ctx.beat * 2.5f) : 0.0f, 0.0f, 1.0f);
    const auto s = sampleHarmonicShake(time * 0.5f * mag * b, more);
    const float gain = mag * mag * a * 0.06f;
    const iota::core::Vec2 target{(s.x - 0.5f) * gain, (s.y - 0.5f) * gain};
    const float alpha = frameRateIndependentAlpha(0.8f, ctx.dt);
    state.prev = {lerpf(state.prev.x, target.x, alpha), lerpf(state.prev.y, target.y, alpha)};
    return state.prev;
  }
  if (m == "ConstantShakeRotLess" || m == "ConstantShakeRotMore") {
    const bool more = (m == "ConstantShakeRotMore");
    const auto s = sampleHarmonicShake(time * 0.5f * b, more);
    const float rot = (s.z / kTwoPi) * a * 0.5f;
    const float alpha = frameRateIndependentAlpha(0.1f, ctx.dt);
    state.prev = {lerpf(state.prev.x, rot, alpha), lerpf(state.prev.y, rot, alpha)};
    return state.prev;
  }
  if (m == "BeatCamShakeRotLess" || m == "BeatCamShakeRotMore") {
    const bool more = (m == "BeatCamShakeRotMore");
    const float mag = std::clamp(active ? (ctx.beat * 2.5f) : 0.0f, 0.0f, 1.0f);
    const auto s = sampleHarmonicShake(time * 0.5f * mag * b, more);
    const float rot = (s.z / kTwoPi) * a * 0.5f * (mag * mag);
    const float alpha = frameRateIndependentAlpha(0.8f, ctx.dt);
    state.prev = {lerpf(state.prev.x, rot, alpha), lerpf(state.prev.y, rot, alpha)};
    return state.prev;
  }
  if (m == "BeatTriggerAnim") {
    if (state.prev.y <= 0.0f && state.prev.x <= 0.0f) {
      state.prev.y = 2.0f;  // Initially armed
    }
    if (active && ctx.beat > a && state.prev.y >= 2.0f) {
      state.prev = {0.0f, 0.0f};
    }
    if (state.prev.x < 0.5f && state.prev.y < 2.0f) {
      state.prev.x = std::min(0.5f, state.prev.x + ctx.dt * b);
      return {state.prev.x, state.prev.x};
    }
    state.prev.y = std::min(2.0f, state.prev.y + ctx.dt * b);
    return {0.5f, state.prev.y};
  }

  return {0.0f, 0.0f};
}

}  // namespace

iota::core::Vec2 evaluateMVar(const iota::model::MVariableFloat& var,
                               std::vector<MVarSlotState>& slotStates,
                               const MVarEvalContext& ctx) {
  if (slotStates.size() != var.slots.size()) {
    slotStates.resize(var.slots.size());
  }
  iota::core::Vec2 total{};
  for (size_t i = 0; i < var.slots.size(); ++i) {
    const auto v = evalSlot(var.slots[i], slotStates[i], ctx);
    total.x += v.x;
    total.y += v.y;
  }
  return total;
}

void MVarEvaluator::reset() {
  states_.clear();
}

iota::core::Vec2 MVarEvaluator::evaluateVec2(uint64_t propertyKey,
                                             const iota::model::MVariableFloat& var,
                                             const MVarEvalContext& ctx) {
  return evaluateMVar(var, states_[propertyKey], ctx);
}

float MVarEvaluator::evaluateScalar(uint64_t propertyKey,
                                    const iota::model::MVariableFloat& var,
                                    const MVarEvalContext& ctx) {
  return evaluateVec2(propertyKey, var, ctx).x;
}

uint64_t MVarEvaluator::makeKey(int elementId, std::string_view propName) noexcept {
  // FNV-1a 64-bit hash combined with elementId
  uint64_t h = 14695981039346656037ull ^ static_cast<uint64_t>(static_cast<uint32_t>(elementId));
  for (unsigned char c : propName) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace iota::dsp
