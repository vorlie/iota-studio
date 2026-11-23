#include "iota/model/measured_var.hpp"

#include <charconv>
#include <cstdio>

#include "iota/core/types.hpp"

namespace iota::model {

namespace {

bool tryParseFloat(std::string_view sv, float& out) noexcept {
  sv = iota::core::trim(sv);
  if (sv.empty()) return false;
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
  return ec == std::errc() && ptr == sv.data() + sv.size();
}

} // namespace

ParsedTypeSpec parseTypeTag(std::string_view typeTag) {
  ParsedTypeSpec spec;
  auto tokens = iota::core::splitWhitespace(typeTag);
  if (tokens.empty()) return spec;

  const auto tok = tokens[0];
  if (tok == "b") spec.tag = PropertyTypeTag::Bool;
  else if (tok == "pb") spec.tag = PropertyTypeTag::ProBool;
  else if (tok == "i") spec.tag = PropertyTypeTag::Int;
  else if (tok == "ih") spec.tag = PropertyTypeTag::HiddenInt;
  else if (tok == "f") spec.tag = PropertyTypeTag::Float;
  else if (tok == "f2") spec.tag = PropertyTypeTag::Vec2f;
  else if (tok == "mvarf") spec.tag = PropertyTypeTag::MVarFloat;
  else if (tok == "mvarf2") spec.tag = PropertyTypeTag::MVarVec2f;
  else if (tok == "crgb") spec.tag = PropertyTypeTag::ColorRgb;
  else if (tok == "crgba") spec.tag = PropertyTypeTag::ColorRgba;
  else if (tok == "crgb_hl") spec.tag = PropertyTypeTag::ColorRgbHl;
  else if (tok == "chsl4f") spec.tag = PropertyTypeTag::ColorHsla4f;
  else if (tok == "chsla4f") spec.tag = PropertyTypeTag::ColorHslaAlpha4f;
  else if (tok == "txt") spec.tag = PropertyTypeTag::Text;
  else if (tok == "ptxt") spec.tag = PropertyTypeTag::ProText;
  else if (tok == "etxt") spec.tag = PropertyTypeTag::CodeText;
  else if (tok == "img") spec.tag = PropertyTypeTag::Image;
  else if (tok == "pimg") spec.tag = PropertyTypeTag::ParticleImage;
  else if (tok == "asset") spec.tag = PropertyTypeTag::Asset;
  else if (tok == "passet") spec.tag = PropertyTypeTag::ProAsset;
  else if (tok == "sel") spec.tag = PropertyTypeTag::Select;
  else if (tok == "_child") spec.tag = PropertyTypeTag::Child;
  else if (tok == "_dyn") spec.tag = PropertyTypeTag::DynamicGroup;

  switch (spec.tag) {
    case PropertyTypeTag::Int:
    case PropertyTypeTag::HiddenInt:
    case PropertyTypeTag::Float:
    case PropertyTypeTag::Vec2f:
    case PropertyTypeTag::MVarFloat:
    case PropertyTypeTag::MVarVec2f: {
      float lo = 0.0f, hi = 1.0f;
      if (tokens.size() >= 3 && tryParseFloat(tokens[1], lo) && tryParseFloat(tokens[2], hi)) {
        spec.minVal = lo;
        spec.maxVal = hi;
        spec.hasBounds = true;
      }
      break;
    }
    case PropertyTypeTag::Image:
    case PropertyTypeTag::ParticleImage:
    case PropertyTypeTag::Asset:
    case PropertyTypeTag::ProAsset:
    case PropertyTypeTag::Select:
    case PropertyTypeTag::Child: {
      for (size_t i = 1; i < tokens.size(); ++i) {
        spec.choices.emplace_back(tokens[i]);
      }
      break;
    }
    default:
      break;
  }
  return spec;
}

std::string_view typeTagToken(PropertyTypeTag tag) noexcept {
  switch (tag) {
    case PropertyTypeTag::Bool: return "b";
    case PropertyTypeTag::ProBool: return "pb";
    case PropertyTypeTag::Int: return "i";
    case PropertyTypeTag::HiddenInt: return "ih";
    case PropertyTypeTag::Float: return "f";
    case PropertyTypeTag::Vec2f: return "f2";
    case PropertyTypeTag::MVarFloat: return "mvarf";
    case PropertyTypeTag::MVarVec2f: return "mvarf2";
    case PropertyTypeTag::ColorRgb: return "crgb";
    case PropertyTypeTag::ColorRgba: return "crgba";
    case PropertyTypeTag::ColorRgbHl: return "crgb_hl";
    case PropertyTypeTag::ColorHsla4f: return "chsl4f";
    case PropertyTypeTag::ColorHslaAlpha4f: return "chsla4f";
    case PropertyTypeTag::Text: return "txt";
    case PropertyTypeTag::ProText: return "ptxt";
    case PropertyTypeTag::CodeText: return "etxt";
    case PropertyTypeTag::Image: return "img";
    case PropertyTypeTag::ParticleImage: return "pimg";
    case PropertyTypeTag::Asset: return "asset";
    case PropertyTypeTag::ProAsset: return "passet";
    case PropertyTypeTag::Select: return "sel";
    case PropertyTypeTag::Child: return "_child";
    case PropertyTypeTag::DynamicGroup: return "_dyn";
    default: return "";
  }
}

MVariableFloat parseMVarString(std::string_view text, const MVariableFloat& fallback) {
  MVariableFloat result;
  for (auto slotSv : iota::core::splitChar(text, ';')) {
    auto parts = iota::core::splitWhitespace(slotSv);
    if (parts.size() >= 3) {
      float a = 0.0f, b = 0.0f;
      if (tryParseFloat(parts[1], a) && tryParseFloat(parts[2], b)) {
        result.slots.push_back({std::string(parts[0]), a, b});
      }
    } else if (parts.size() == 1 && !parts[0].empty()) {
      result.slots.push_back({std::string(parts[0]), 0.5f, 0.5f});
    }
  }
  if (result.slots.empty()) return fallback;
  return result;
}

std::string formatMVarString(const MVariableFloat& mvar) {
  std::string out;
  char buf[96];
  for (size_t i = 0; i < mvar.slots.size(); ++i) {
    if (i > 0) out.push_back(';');
    const auto& s = mvar.slots[i];
    std::snprintf(buf, sizeof(buf), "%s %.6f %.6f",
                  s.measureWhat.empty() ? "Nothing" : s.measureWhat.c_str(), s.a, s.b);
    out += buf;
  }
  return out;
}

const std::vector<std::string_view>& allMeasureWhatModes() noexcept {
  static const std::vector<std::string_view> kModes = {
      "Nothing",
      "Constant",
      "IsPlaying",
      "Beat",
      "BeatCamShakeMore",
      "BeatCamShakeLess",
      "TotalTime",
      "TotalTimeBackward",
      "TotalTimeWhenPlaying",
      "ConstantShakeMore",
      "ConstantShake",
      "BeatRandomShake",
      "TrackPosition",
      "TotalTimeAndBeat",
      "ConstantShakeRotMore",
      "ConstantShakeRotLess",
      "BeatCamShakeRotMore",
      "BeatCamShakeRotLess",
      "PeakBarIndex",
      "BeatTriggerAnim",
      "SoundKeys1",
      "SoundKeys2",
      "SoundKeys3",
      "SoundKeysShake1",
      "SoundKeysShake2",
      "SoundKeysShake3",
  };
  return kModes;
}

} // namespace iota::model
