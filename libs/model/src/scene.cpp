#include "iota/model/scene.hpp"

#include <atomic>

namespace iota::model {

namespace {

int nextElementId() {
  static std::atomic<int> sId{1000};
  return sId.fetch_add(1, std::memory_order_relaxed);
}

constexpr const char* kBlendChoices = "_child Alpha PreMulAlpha Screen Add AddAlpha";
constexpr const char* kAnchorChoices = "sel unset start center end";
constexpr const char* kPosMeasureChoices =
    "_child Nothing BeatCamShakeMore BeatCamShakeLess BeatRandomShake ConstantShakeMore ConstantShake TrackPosition";
constexpr const char* kAllMeasureChoices =
    "_child Nothing Beat TotalTime TotalTimeBackward TotalTimeWhenPlaying TotalTimeAndBeat TrackPosition BeatRandomShake BeatCamShakeRotMore BeatCamShakeRotLess ConstantShakeRotMore ConstantShakeRotLess BeatTriggerAnim";

void setLegacyChildMVar(PropertyBag& props, std::string_view key, std::string_view what,
                        float a, float b, const char* whatChoices, std::string_view group) {
  auto& e = props.getOrAdd(key);
  e.typeTag = "_child";
  e.groupTag = std::string(group);
  e.value = std::string("");

  auto& mw = e.getOrAddChild("measureWhat");
  mw.value = std::string(what);
  mw.typeTag = whatChoices;
  mw.groupTag = "misc";

  if (what != "Nothing") {
    auto& ea = e.getOrAddChild("A");
    ea.value = static_cast<double>(a);
    ea.typeTag = "f 0.0 1.0";
    ea.groupTag = "misc";

    auto& eb = e.getOrAddChild("B");
    eb.value = static_cast<double>(b);
    eb.typeTag = "f 0.0 1.0";
    eb.groupTag = "misc";
  }
}

void addDefaultSegment(PropertyEntry& seg, std::string_view typeName,
                       iota::core::ColorRgba c0, iota::core::ColorRgba c1,
                       float heightMul, float width, bool mirror) {
  seg.typeTag = "_child None Bars Line SharpBars RoundBars";
  seg.groupTag = "1_bars";
  seg.value = std::string(typeName);
  if (typeName == "None") return;

  auto& cf = seg.getOrAddChild("colorFrom");
  cf.typeTag = "crgba";
  cf.groupTag = "misc";
  cf.value = static_cast<int64_t>(c0.toArgbInt32());

  auto& ct = seg.getOrAddChild("colorTo");
  ct.typeTag = "crgba";
  ct.groupTag = "misc";
  ct.value = static_cast<int64_t>(c1.toArgbInt32());

  auto& fh = seg.getOrAddChild("fixedHeight");
  fh.typeTag = "f -50.0 50.0";
  fh.groupTag = "misc";
  fh.value = 0.0;

  auto& hm = seg.getOrAddChild("barHeightMultiplier");
  hm.typeTag = "f -2.0 2.0";
  hm.groupTag = "misc";
  hm.value = static_cast<double>(heightMul);

  if (typeName == "Bars" || typeName == "SharpBars" || typeName == "RoundBars") {
    auto& bw = seg.getOrAddChild("barWidth");
    bw.typeTag = "f 0.0 2.0";
    bw.groupTag = "misc";
    bw.value = static_cast<double>(width);
  }
  if (typeName == "Bars" || typeName == "RoundBars") {
    auto& bwa = seg.getOrAddChild("barWidthAffectedByShape");
    bwa.typeTag = "b";
    bwa.groupTag = "b";
    bwa.value = static_cast<int64_t>(1);
  }
  auto& mr = seg.getOrAddChild("mirror");
  mr.typeTag = "b";
  mr.groupTag = "b";
  mr.value = static_cast<int64_t>(mirror ? 1 : 0);
}

} // namespace

ElementType elementTypeFromString(std::string_view name) noexcept {
  if (name == "Root") return ElementType::Root;
  if (name == "Composition") return ElementType::Composition;
  if (name == "AudioProvider") return ElementType::AudioProvider;
  if (name == "Bars") return ElementType::Bars;
  if (name == "Particles") return ElementType::Particles;
  if (name == "Image") return ElementType::Image;
  if (name == "Text") return ElementType::Text;
  if (name == "BlurEffect") return ElementType::BlurEffect;
  if (name == "RgbSplitEffect") return ElementType::RgbSplitEffect;
  if (name == "MotionBlurEffect") return ElementType::MotionBlurEffect;
  if (name == "MirrorEffect") return ElementType::MirrorEffect;
  if (name == "AppLogo") return ElementType::AppLogo;
  if (name == "StatText") return ElementType::StatText;
  if (name == "CustomShader") return ElementType::CustomShader;
  return ElementType::Unknown;
}

std::string_view elementTypeToString(ElementType type) noexcept {
  switch (type) {
    case ElementType::Root: return "Root";
    case ElementType::Composition: return "Composition";
    case ElementType::AudioProvider: return "AudioProvider";
    case ElementType::Bars: return "Bars";
    case ElementType::Particles: return "Particles";
    case ElementType::Image: return "Image";
    case ElementType::Text: return "Text";
    case ElementType::BlurEffect: return "BlurEffect";
    case ElementType::RgbSplitEffect: return "RgbSplitEffect";
    case ElementType::MotionBlurEffect: return "MotionBlurEffect";
    case ElementType::MirrorEffect: return "MirrorEffect";
    case ElementType::AppLogo: return "AppLogo";
    case ElementType::StatText: return "StatText";
    case ElementType::CustomShader: return "CustomShader";
    default: return "None";
  }
}

const std::vector<ElementType>& addableElementTypes() noexcept {
  static const std::vector<ElementType> kTypes = {
      ElementType::AudioProvider,
      ElementType::Image,
      ElementType::Bars,
      ElementType::Particles,
      ElementType::Text,
      ElementType::BlurEffect,
      ElementType::RgbSplitEffect,
      ElementType::MotionBlurEffect,
      ElementType::MirrorEffect,
      ElementType::CustomShader,
  };
  return kTypes;
}

void addBaseElementProperties(PropertyBag& props, int id, iota::core::BlendMode defaultBlend) {
  props.setInt("_id", id, 0, 0, "", "ih");
  props.setBool("visible", true, "0_general", "b");
  props.setString("blendMode", iota::core::blendModeToString(defaultBlend), kBlendChoices, "0_general");
  props.setVec2("position", {0.5f, 0.5f}, -1.0f, 1.0f, "0_general");
  props.setString("anchorX", "unset", kAnchorChoices, "0_general");
  props.setString("anchorY", "unset", kAnchorChoices, "0_general");
  setLegacyChildMVar(props, "MeasurePos", "Nothing", 0.5f, 0.5f, kPosMeasureChoices, "0_general");
  props.setVec2("alignmentPosition", {0.5f, 0.5f}, 0.0f, 1.0f, "0_general");
  props.setBool("scaleIsUniform", false, "0_general", "b");
  props.setVec2("scale", {1.0f, 1.0f}, 0.0f, 2.0f, "0_general");
  setLegacyChildMVar(props, "measureScale", "Nothing", 0.5f, 0.5f, kAllMeasureChoices, "0_general");
  props.setFloat("rotation", 0.0f, 0.0f, 300.0f, "0_general");
  setLegacyChildMVar(props, "measureRot", "Nothing", 0.5f, 0.5f, kAllMeasureChoices, "0_general");
}

Element createDefaultElement(ElementType type) {
  Element el;
  el.type = type;
  el.typeName = std::string(elementTypeToString(type));
  el.version = "10";
  el.displayName = el.typeName;
  const int id = nextElementId();

  switch (type) {
    case ElementType::AudioProvider: {
      el.displayName = "Audio Provider";
      el.props.setString("sampleProvider", "Spectrum2", "_child Waveform Spectrum Spectrum2", "0_general");
      el.props.setInt("audioDurationMs", 60, 20, 300, "1_AudioCapture");
      el.props.setInt("inputSize", 11, 5, 13, "Performance");
      el.props.setBool("highQualityAudioCapture", false, "1_AudioCapture");
      el.props.setInt("sampleOutCountPower", 8, 3, 9, "Waveform");
      el.props.setInt("sampleOutCount", 64, 0, 512, "2_spectrum");
      el.props.setFloat("lowerHz", 20.0f, 0.0f, 300.0f, "2_spectrumHz");
      el.props.setFloat("higherHz", 18000.0f, 300.0f, 18000.0f, "2_spectrumHz");
      el.props.setFloat("hzLinearFactor", 0.10f, 0.0f, 1.0f, "2_spectrumHz");
      el.props.setFloat("freqShift", 0.0f, 0.0f, 1.0f, "2_spectrumHz");
      el.props.setBool("mirrorSamples", false, "2_spectrum");
      el.props.setInt("repeatSamples", 1, 1, 6, "2_spectrum");
      el.props.setInt("starAndEndGap", 0, 0, 30, "2_spectrum");
      el.props.setFloat("smooth", 0.85f, 0.1f, 1.0f, "2_spectrum");
      el.props.setFloat("preSmooth", 1.0f, 0.1f, 1.0f, "2_spectrum");
      el.props.setInt("filterRadius", 4, 0, 20, "2_spectrum");
      el.props.setFloat("filterStrength", 1.0f, 0.1f, 2.0f, "2_spectrum");
      el.props.setFloat("beatSmooth", 0.4f, 0.1f, 1.0f, "beat");
      el.props.setFloat("beatRangeBarFirst", 0.0f, 0.0f, 1.0f, "beat");
      el.props.setFloat("beatRangeBarLast", 0.2f, 0.0f, 1.0f, "beat");
      el.props.setFloat("beatRangeValueLower", 0.7f, 0.0f, 50.0f, "beat");
      el.props.setFloat("beatRangeValueHigher", 35.0f, 0.0f, 100.0f, "beat");
      el.props.setFloat("lowerFreqMagnitude", 0.25f, 0.0f, 1.0f, "2_spectrum");
      el.props.setFloat("aWeight", 0.2f, 0.0f, 1.0f, "2_spectrum");
      el.props.setFloat("outputMultiplier", 1.2f, 0.1f, 3.0f, "2_spectrum");
      el.props.setFloat("temporalSmooth", 0.5f, 0.0f, 1.0f, "2_temporal");
      el.props.setFloat("attackMs", 25.0f, 0.0f, 250.0f, "2_temporal");
      el.props.setFloat("releaseMs", 150.0f, 0.0f, 1000.0f, "2_temporal");
      el.props.setString("freqSmoothMode", "None",
                         "sel None MovingAverage Gaussian SavitzkyGolay",
                         "3_frequencySmooth");
      el.props.setFloat("freqSmoothRadius", 2.0f, 0.0f, 8.0f, "3_frequencySmooth");
      el.props.setFloat("freqSmoothStrength", 0.7f, 0.0f, 1.0f, "3_frequencySmooth");
      break;
    }

    case ElementType::Image: {
      addBaseElementProperties(el.props, id, iota::core::BlendMode::Alpha);
      el.props.setString("customImage", "internalres:placeholderart5",
                         "img internalres:white internalres:black internalres:particle_circle_blur4 internalres:vignette80 internalres:rainbow128 internalres:particle_blur01_more internalres:lens_flare internalres:lens_flare_2 composition:0",
                         "1_image");
      setLegacyChildMVar(el.props, "measureAnimationSpeed", "TotalTimeAndBeat", 0.5f, 0.5f, kAllMeasureChoices, "1_image");
      el.props.setInt("generatedAlbumArtHint", 0, 0, 7, "generatedAlbumArt", "ih");
      el.props.setBool("generatedAlbumArtHintShiftHue", false, "generatedAlbumArt");
      el.props.setBool("generatedAlbumArtHintNoText", false, "generatedAlbumArt");
      el.props.setBool("generatedAlbumArtHintForceGen", false, "generatedAlbumArt");
      el.props.setColorArgb("generatedAlbumArtColor", {255, 255, 255, 255}, "crgba", "generatedAlbumArt");
      el.props.setBool("keepAspectRatioAndCropToFit", false, "1_image");
      el.props.setBool("colorKeyEnabled", false, "2_ColorKey");
      el.props.setBool("autoDetectColorKey", true, "2_ColorKey");
      el.props.setColorArgb("colorKey", {0, 255, 0, 255}, "crgb_hl", "2_ColorKey");
      el.props.setFloat("transparencyStrength", 1.0f, 0.0f, 4.0f, "2_ColorKey");
      el.props.setFloat("opacityStrength", 1.0f, 0.0f, 4.0f, "2_ColorKey");
      el.props.setBool("keepAspectRatio", true, "1_image");
      el.props.setBool("blurredBorder", false, "1_image");
      el.props.setColorHsla("Color", {0.0f, 0.0f, 1.0f, 1.0f}, "2_color");
      el.props.setColorHsla("ColorTo", {0.0f, 0.0f, 1.0f, 1.0f}, "2_color");
      setLegacyChildMVar(el.props, "MeasureColorBlend", "Nothing", 0.5f, 0.5f, kAllMeasureChoices, "2_color");
      el.props.setMVar("saturation", MVariableFloat::constant2(1.0f, 1.0f), "mvarf 0.0 1.0", "2_color");
      el.props.setBool("blurEnabled", false, "blur");
      el.props.setInt("blurDivider", 5, 0, 10, "blur");
      el.props.setInt("blurRadius", 7, 1, 25, "blur");
      el.props.setString("Shape", "None", "_child None Circle SidedPolygon", "1_image");
      el.props.setString("MaskImage", "",
                         "img internalres:transparent internalres:white internalres:black internalres:particle_circle_blur4 internalres:particle_blur01 internalres:particle_blur_inv internalres:vignette80 composition:0",
                         "1_image");
      el.props.setString("maskMode", "Transparency",
                         "sel Transparency TransparencyAndBlacks TransparencyAndWhites InvertedTransparency",
                         "1_image");
      break;
    }

    case ElementType::Bars: {
      addBaseElementProperties(el.props, id, iota::core::BlendMode::Alpha);
      el.props.setVec2("scale", {0.6f, 0.6f}, 0.0f, 2.0f, "0_general");
      el.props.setBool("flipInput", false, "1_bars");
      el.props.setFloat("heightScale", 2.0f, -10.0f, 10.0f, "1_bars");
      el.props.setFloat("minHeightScale", 0.01f, -0.03f, 0.03f, "1_bars");
      el.props.setFloat("maxHeightScale", 1.0f, 0.1f, 1.0f, "1_bars");
      auto& sp = el.props.getOrAdd("ShapePath");
      sp.typeTag = "_child HorizontalLine Circle SidedPolygon Letter";
      sp.groupTag = "1_bars";
      sp.value = std::string("Circle");
      auto& rad = sp.getOrAddChild("radius");
      rad.typeTag = "f 0.5 3.0";
      rad.groupTag = "misc";
      rad.value = 0.7f;
      auto& gap = sp.getOrAddChild("gap");
      gap.typeTag = "f 0.0 0.9";
      gap.groupTag = "misc";
      gap.value = 0.0;

      el.props.setMVar("colorBlendOffset", MVariableFloat{{{"TotalTime", 0.35f, 0.0f}}}, "mvarf 0.0 1.0", "1_bars");
      el.props.setString("curveInterpolation", "None", "sel None Linear CatmullRom CubicBSpline", "1_bars");
      el.props.setInt("curveSubdivisions", 4, 1, 8, "1_bars");
      el.props.setInt("softness", 2, 0, 20, "2_Behavior");
      el.props.setInt("reactionDelay", 0, 0, 9, "2_Behavior");
      el.props.setInt("reactionAccumulatedDelay", 0, 0, 9, "2_Behavior");
      el.props.setInt("audioProviderIndex", 1, 1, 4, "2_Behavior");

      addDefaultSegment(el.props.getOrAdd("Segment1"), "Bars",
                        {0, 210, 255, 230}, {255, 80, 200, 230}, 1.0f, 0.65f, true);
      addDefaultSegment(el.props.getOrAdd("Segment2"), "None",
                        {255, 255, 255, 255}, {255, 255, 255, 255}, 1.0f, 0.5f, false);
      break;
    }

    case ElementType::Particles: {
      addBaseElementProperties(el.props, id, iota::core::BlendMode::Add);
      el.props.erase("rotation");
      el.props.erase("measureRot");
      el.props.setString("customImage", "internalres:particle_blur01",
                         "pimg internalres:particle_blur01 internalres:particle_circle_blur4 internalres:particle_circle_w_a_64 internalres:particle_sharp composition:0",
                         "appearance");
      el.props.setColorArgb("color", {255, 255, 255, 255}, "crgba", "1_overall");
      el.props.setInt("CountLimit", 400, 1, 5000, "1_overall");
      el.props.setFloat("particleScale", 2.5f, 0.25f, 8.0f, "appearance");
      el.props.setFloat("spawnTime", 0.015f, 0.005f, 0.05f, "behaviour");
      el.props.setBool("mirrorX", false, "1_overall");
      el.props.setBool("mirrorY", false, "1_overall");
      el.props.setFloat("perspectiveDepth", 0.0f, 0.0f, 1000.0f, "1_overall");
      el.props.setFloat("perspectiveFov", 110.0f, 30.0f, 180.0f, "1_overall");
      auto& sa = el.props.getOrAdd("SpawnArea");
      sa.typeTag = "_child HorizontalLine Rectangle";
      sa.groupTag = "1_overall";
      sa.value = std::string("Rectangle");
      sa.getOrAddChild("vectorRelativeToCenter").value = static_cast<int64_t>(1);
      sa.getOrAddChild("vectorRelativeToCenter").typeTag = "b";
      sa.getOrAddChild("useRandomVectorInstead").value = static_cast<int64_t>(0);
      sa.getOrAddChild("useRandomVectorInstead").typeTag = "b";
      sa.getOrAddChild("vectorAngle").value = 270.0;
      sa.getOrAddChild("vectorAngle").typeTag = "f 0.0 360.0";
      sa.getOrAddChild("vectorAngleZ").value = 0.0;
      sa.getOrAddChild("vectorAngleZ").typeTag = "f -1.0 1.0";

      setLegacyChildMVar(el.props, "MeasureOverallSpeed", "Beat", 1.5f, 1.5f, kAllMeasureChoices, "behaviour");
      el.props.setFloat("Speed", 90.0f, -300.0f, 300.0f, "behaviour");
      el.props.setFloat("speedRandom", 40.0f, -300.0f, 300.0f, "behaviour");
      el.props.setFloat("trailLength", 0.0f, 0.0f, 1.0f, "appearance");
      el.props.setColorHsla("ColorFrom", {0.55f, 0.8f, 0.65f, 0.85f}, "appearance");
      el.props.setColorHsla("ColorTo", {0.85f, 0.9f, 0.55f, 0.0f}, "appearance");
      el.props.setFloat("lifetime", 2.2f, 0.1f, 10.0f, "behaviour");
      el.props.setFloat("fadeInAndOutTime", 0.25f, 0.0f, 0.5f, "appearance");
      el.props.setVec2("gravity", {0.0f, -20.0f}, -300.0f, 300.0f, "behaviour");
      el.props.setBool("velocityAngle", true, "appearance");
      el.props.setFloat("sizeAspectRatio", 1.0f, 0.1f, 10.0f, "appearance");
      el.props.setFloat("startSize", 1.0f, 0.0f, 20.0f, "appearance");
      el.props.setFloat("fadeInSize", 1.0f, 0.0f, 20.0f, "appearance");
      el.props.setFloat("endSize", 0.2f, 0.0f, 20.0f, "appearance");
      el.props.setFloat("endSizeRandom", 0.0f, 0.0f, 20.0f, "appearance");
      el.props.setFloat("endSizeZFactor", 0.0f, 0.0f, 40.0f, "appearance");
      el.props.setFloat("sideSineWaveFreq", 2.0f, 0.0f, 10.0f, "waveBehaviour");
      el.props.setFloat("sideSineWaveFreqRandom", 1.0f, -10.0f, 10.0f, "waveBehaviour");
      el.props.setFloat("sideSineWaveMag", 1.5f, 0.0f, 10.0f, "waveBehaviour");
      el.props.setFloat("sideSineWaveMagRandom", 1.0f, -10.0f, 10.0f, "waveBehaviour");
      el.props.setString("ForceField", "None", "_child None Vortex", "1_overall");
      el.props.setFloat("nearCameraFadeOutDistance", 40.0f, 0.0f, 500.0f, "appearance");
      break;
    }

    case ElementType::Text: {
      addBaseElementProperties(el.props, id, iota::core::BlendMode::PreMulAlpha);
      el.props.setBool("scaleIsUniform", true, "0_general");
      el.props.setVec2("position", {0.5f, 0.88f}, -1.0f, 1.0f, "0_general");
      el.props.setString("text", "{ArtistAndTitle}", "ptxt", "appearance");
      el.props.setColorArgb("color", {240, 244, 255, 255}, "crgba", "appearance");
      el.props.setInt("fontSize", 32, 8, 100, "appearance");
      el.props.setString("typeFace", "internal_2",
                         "passet internal_0:2131230884 internal_1:2131230885 internal_2:2131230886 internal_3:2131230887",
                         "appearance");
      break;
    }

    case ElementType::BlurEffect: {
      addBaseElementProperties(el.props, id, iota::core::BlendMode::Add);
      el.props.setString("blendModeContent", "Add", kBlendChoices, "1_appearance");
      el.props.setColorArgb("color", {0, 0, 0, 255}, "crgba", "1_appearance");
      el.props.setInt("sourceCompositionIndex", 1, 1, 5, "1_appearance");
      el.props.setString("MaskImage", "internalres:white", "img internalres:white internalres:vignette80 composition:0", "1_appearance");
      el.props.setFloat("blurRadius", 1.5f, 0.0f, 3.0f, "2_blur");
      el.props.setFloat("blurMultiplier", 4.0f, 1.0f, 6.0f, "2_blur");
      el.props.setBool("showUnblurredContent", true, "1_appearance");
      el.props.setBool("showUnblurredContentUnder", false, "1_appearance");
      el.props.setVec2("1layerScale", {1.0f, 1.0f}, 0.0f, 10.0f, "2_blur");
      el.props.setVec2("2layerScale", {1.05f, 1.05f}, 0.0f, 10.0f, "2_blur");
      el.props.setVec2("3layerScale", {0.0f, 0.0f}, 0.0f, 10.0f, "2_blur");
      el.props.setColorArgb("1layerColor", {255, 255, 255, 255}, "crgba", "2_blur");
      el.props.setColorArgb("2layerColor", {100, 180, 255, 180}, "crgba", "2_blur");
      el.props.setColorArgb("3layerColor", {255, 255, 255, 255}, "crgba", "2_blur");
      break;
    }

    case ElementType::RgbSplitEffect: {
      addBaseElementProperties(el.props, id, iota::core::BlendMode::PreMulAlpha);
      el.props.setString("blendModeContent", "Add", kBlendChoices, "1_appearance");
      el.props.setColorArgb("color", {255, 255, 255, 255}, "crgb", "1_appearance");
      el.props.setString("targetImage", "composition:1", "img composition:0", "1_appearance");
      el.props.setBool("showUnblurredContent", true, "1_appearance");
      el.props.setBool("showUnblurredContentUnder", false, "1_appearance");
      el.props.setMVar("splitMultiplier", MVariableFloat::constant(1.5f), "mvarf 0.0 6.0", "2_splitEffect");
      el.props.setMVar("splitAmount", MVariableFloat{{{"BeatRandomShake", 0.5f, 0.5f}}}, "mvarf2 -0.5 0.5", "2_splitEffect");
      el.props.setColorArgb("splitColor0", {255, 0, 0, 255}, "crgb", "2_splitEffect");
      el.props.setColorArgb("splitColor1", {0, 255, 0, 255}, "crgb", "2_splitEffect");
      el.props.setColorArgb("splitColor2", {0, 0, 255, 255}, "crgb", "2_splitEffect");
      break;
    }

    case ElementType::MotionBlurEffect: {
      addBaseElementProperties(el.props, id, iota::core::BlendMode::PreMulAlpha);
      el.props.setString("blendModeContent", "Add", kBlendChoices, "1_appearance");
      el.props.setColorArgb("color", {255, 255, 255, 255}, "crgb", "1_appearance");
      el.props.setString("TargetImage", "composition:1", "img composition:0", "1_appearance");
      el.props.setBool("showUnblurredContent", false, "1_appearance");
      el.props.setBool("showUnblurredContentUnder", false, "1_appearance");
      el.props.setMVar("blurAmountMultiplier", MVariableFloat::constant2(0.5f, 1.0f), "mvarf 0.0 2.0", "2_motionBlur");
      el.props.setString("motionSource", "EffectTransform", "_child EffectTransform Manual", "2_motionBlur");
      el.props.setBool("relativeMotionMode", true, "2_motionBlur");
      break;
    }

    case ElementType::MirrorEffect: {
      addBaseElementProperties(el.props, id, iota::core::BlendMode::PreMulAlpha);
      el.props.setString("TargetImage", "composition:1", "img composition:0", "1_appearance");
      el.props.setString("mirrorMode", "Horizontal", "sel Horizontal Vertical HorizontalAndVertical", "1_appearance");
      el.props.setBool("flipMirror", false, "1_appearance");
      el.props.setColorArgb("color", {255, 255, 255, 255}, "crgba", "1_appearance");
      break;
    }

    case ElementType::AppLogo: {
      el.props.setString("typeFace", "internal_3", "passet internal_0:2131230884 internal_1:2131230885 internal_2:2131230886 internal_3:2131230887", "appearance");
      el.props.setBool("visible", false, "0_general", "pb");
      el.props.setColorArgb("color", {255, 255, 255, 160}, "crgba", "0_general");
      break;
    }

    case ElementType::CustomShader: {
      addBaseElementProperties(el.props, id, iota::core::BlendMode::PreMulAlpha);
      el.props.setString("TargetImage", "composition:1", "img composition:0", "1_appearance");
      el.props.setString("shaderVertex", "", "etxt", "shader");
      el.props.setString("shaderFragment", "", "etxt", "shader");
      break;
    }

    default: {
      addBaseElementProperties(el.props, id, iota::core::BlendMode::Alpha);
      break;
    }
  }
  return el;
}

Composition createDefaultComposition(int index) {
  Composition comp;
  comp.version = "10";
  comp.displayName = index == 0 ? "Final Composition" : "Composition " + std::to_string(index);
  comp.props.setString("introBlendMode", "Alpha", "sel Alpha AddAlpha", "2_introOutro");
  comp.props.setFloat("introFadeDuration", 0.0f, 0.0f, 10.0f, "2_introOutro");
  comp.props.setFloat("outroFadeDuration", 0.0f, 0.0f, 10.0f, "2_introOutro");
  comp.props.setColorArgb("introFadeColor", {0, 0, 0, 223}, "crgba", "2_introOutro");
  comp.props.setBool("allowRecursion", false, "0_general");
  return comp;
}

Project createStarterProject() {
  Project proj;
  proj.version = "10";
  proj.templateId = 0;

  // Composition 0: Final output (AudioProvider + Background Image + Blur Glow + Circular Bars + Text)
  Composition finalComp = createDefaultComposition(0);
  finalComp.elements.push_back(createDefaultElement(ElementType::AudioProvider));

  Element bg = createDefaultElement(ElementType::Image);
  bg.displayName = "Background";
  bg.props.setColorHsla("Color", {0.62f, 0.55f, 0.14f, 1.0f}, "2_color");
  bg.props.setColorHsla("ColorTo", {0.82f, 0.65f, 0.28f, 1.0f}, "2_color");
  bg.props.getOrAdd("MeasureColorBlend").getOrAddChild("measureWhat").value = std::string("Beat");
  finalComp.elements.push_back(std::move(bg));

  Element particles = createDefaultElement(ElementType::Particles);
  particles.displayName = "Reactive Particles";
  finalComp.elements.push_back(std::move(particles));

  Element bars = createDefaultElement(ElementType::Bars);
  bars.displayName = "Circular Spectrum";
  finalComp.elements.push_back(std::move(bars));

  Element titleText = createDefaultElement(ElementType::Text);
  titleText.displayName = "Track Label";
  finalComp.elements.push_back(std::move(titleText));

  proj.compositions.push_back(std::move(finalComp));
  return proj;
}

} // namespace iota::model
