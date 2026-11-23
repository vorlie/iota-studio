#pragma once

#include "iota/audio/audio_engine.hpp"
#include "iota/dsp/mvar_evaluator.hpp"
#include "iota/dsp/sample_providers.hpp"
#include "iota/engine/font_atlas.hpp"
#include "iota/model/scene.hpp"
#include "iota/rhi/rhi.hpp"

#include <memory>
#include <string>
#include <vector>

namespace iota::engine {

struct FrameTiming {
  float dt = 1.0f / 60.0f;
  double totalTimeSec = 0.0;
  double playAccumTimeSec = 0.0;
  bool isExporting = false;
  float visOffsetMs = 0.0f;
  bool fxaaEnabled = true;
  float fpsStat = 60.0f;
};

struct RenderFrameStats {
  int drawCalls = 0;
  int compositionsRendered = 0;
  float beat = 0.0f;
  float peakBarIndex = 0.0f;
  std::vector<float> lastBars;
  std::string lastError;
};

/// Coordinates Composition DAG scheduling, DSP evaluation, particle simulation,
/// multi-pass BlurEffect pyramids, RgbSplit/MotionBlur/Mirror effects, and FXAA.
class SceneRenderer {
 public:
  SceneRenderer();
  ~SceneRenderer();

  SceneRenderer(const SceneRenderer&) = delete;
  SceneRenderer& operator=(const SceneRenderer&) = delete;

  bool initialize(iota::rhi::IRenderDevice* device, std::string* outError = nullptr);
  void shutdown();

  /// Resets stateful simulation buffers (particles, motion-blur history, DSP envelopes, MVars).
  void resetSimulation();

  /// Renders a complete frame at `(width, height)`. When `outputTarget` is null,
  /// renders into an internal final offscreen render target readable via `finalOutputTexture()`
  /// or `finalRenderTarget()`.
  iota::rhi::RenderTargetHandle renderFrame(const iota::model::Project& project,
                                            const iota::audio::AudioEngine& audio,
                                            const FrameTiming& timing,
                                            int width,
                                            int height,
                                            iota::rhi::RenderTargetHandle outputTarget = {},
                                            RenderFrameStats* outStats = nullptr);

  [[nodiscard]] iota::rhi::RenderTargetHandle finalRenderTarget() const noexcept;
  [[nodiscard]] iota::rhi::TextureHandle finalOutputTexture() const noexcept;

  /// Invalidates cached `local:` image textures (call after modifying Project::localAssets).
  void invalidateLocalAssets();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace iota::engine
