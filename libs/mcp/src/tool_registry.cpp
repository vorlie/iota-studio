#include "tool_registry.hpp"

#include "iota/audio/audio_decoder.hpp"
#include "iota/core/types.hpp"
#include "iota/core/utf8_path.hpp"
#include "iota/export/video_exporter.hpp"
#include "iota/mcp/png_encode.hpp"
#include "iota/mcp/working_copy.hpp"
#include "iota/model/hierarchy_ops.hpp"
#include "iota/model/measured_var.hpp"
#include "iota/model/property_docs.hpp"
#include "iota/preset_io/project_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace iota::mcp::detail {

namespace {

bool eqCI(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool containsCI(std::string_view haystack, std::string_view needle) noexcept {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;
  for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
    bool ok = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
          std::tolower(static_cast<unsigned char>(needle[j]))) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

nlohmann::json makeToolOk(const nlohmann::json& payload) {
  return {
      {"content",
       nlohmann::json::array({
           {{"type", "text"},
            {"text", payload.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)}},
       })},
      {"isError", false},
  };
}

nlohmann::json makeToolErr(const std::string& msg) {
  return {
      {"content", nlohmann::json::array({{{"type", "text"}, {"text", msg}}})},
      {"isError", true},
  };
}

std::string formatArgbHex(iota::core::ColorRgba c) {
  char buf[16]{};
  std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", c.a, c.r, c.g, c.b);
  return buf;
}

bool parseHexColorString(std::string_view text, iota::core::ColorRgba& out) noexcept {
  if (text.empty()) return false;
  if (text.front() == '#') text.remove_prefix(1);
  else if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
  if (text.size() != 6 && text.size() != 8) return false;

  uint32_t val = 0;
  for (char ch : text) {
    const unsigned char uc = static_cast<unsigned char>(ch);
    uint32_t nibble = 0;
    if (uc >= '0' && uc <= '9') nibble = static_cast<uint32_t>(uc - '0');
    else if (uc >= 'a' && uc <= 'f') nibble = static_cast<uint32_t>(10 + (uc - 'a'));
    else if (uc >= 'A' && uc <= 'F') nibble = static_cast<uint32_t>(10 + (uc - 'A'));
    else return false;
    val = (val << 4) | nibble;
  }
  if (text.size() == 6) {
    out.r = static_cast<uint8_t>((val >> 16) & 0xFFu);
    out.g = static_cast<uint8_t>((val >> 8) & 0xFFu);
    out.b = static_cast<uint8_t>(val & 0xFFu);
    out.a = 255;
  } else {
    out.a = static_cast<uint8_t>((val >> 24) & 0xFFu);
    out.r = static_cast<uint8_t>((val >> 16) & 0xFFu);
    out.g = static_cast<uint8_t>((val >> 8) & 0xFFu);
    out.b = static_cast<uint8_t>(val & 0xFFu);
  }
  return true;
}

bool parseColorRgbaJson(const nlohmann::json& v, iota::core::ColorRgba& out) noexcept {
  if (v.is_number_integer()) {
    out = iota::core::ColorRgba::fromArgbInt32(static_cast<int32_t>(v.get<int64_t>()));
    return true;
  }
  if (v.is_string()) {
    return parseHexColorString(v.get<std::string>(), out);
  }
  auto clampByte = [](double d) {
    return static_cast<uint8_t>(std::clamp(std::llround(d), 0LL, 255LL));
  };
  if (v.is_array() && (v.size() == 3 || v.size() == 4)) {
    for (const auto& item : v) {
      if (!item.is_number()) return false;
    }
    out.r = clampByte(v[0].get<double>());
    out.g = clampByte(v[1].get<double>());
    out.b = clampByte(v[2].get<double>());
    out.a = (v.size() == 4) ? clampByte(v[3].get<double>()) : 255;
    return true;
  }
  if (v.is_object() && v.contains("r") && v.contains("g") && v.contains("b")) {
    out.r = v["r"].is_number() ? clampByte(v["r"].get<double>()) : 255;
    out.g = v["g"].is_number() ? clampByte(v["g"].get<double>()) : 255;
    out.b = v["b"].is_number() ? clampByte(v["b"].get<double>()) : 255;
    out.a = (v.contains("a") && v["a"].is_number()) ? clampByte(v["a"].get<double>()) : 255;
    return true;
  }
  return false;
}

bool parseColorHslaJson(const nlohmann::json& v, iota::core::ColorHslaNorm& out) noexcept {
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (auto parsed = iota::core::parseHsla4f(s)) {
      out = *parsed;
      return true;
    }
    iota::core::ColorRgba rgba;
    if (parseHexColorString(s, rgba)) {
      out = iota::core::ColorHslaNorm::fromRgba(rgba);
      return true;
    }
    return false;
  }
  if (v.is_array() && (v.size() == 3 || v.size() == 4)) {
    for (const auto& item : v) {
      if (!item.is_number()) return false;
    }
    out.h = std::clamp(static_cast<float>(v[0].get<double>()), 0.0f, 1.0f);
    out.s = std::clamp(static_cast<float>(v[1].get<double>()), 0.0f, 1.0f);
    out.l = std::clamp(static_cast<float>(v[2].get<double>()), 0.0f, 1.0f);
    out.a = (v.size() == 4) ? std::clamp(static_cast<float>(v[3].get<double>()), 0.0f, 1.0f) : 1.0f;
    return true;
  }
  if (v.is_object() && v.contains("h") && v.contains("s") && v.contains("l")) {
    out.h = std::clamp(static_cast<float>(v.value("h", 0.0)), 0.0f, 1.0f);
    out.s = std::clamp(static_cast<float>(v.value("s", 0.0)), 0.0f, 1.0f);
    out.l = std::clamp(static_cast<float>(v.value("l", 1.0)), 0.0f, 1.0f);
    out.a = std::clamp(static_cast<float>(v.value("a", 1.0)), 0.0f, 1.0f);
    return true;
  }
  return false;
}

bool parseVec2Json(const nlohmann::json& v, iota::core::Vec2& out) noexcept {
  if (v.is_string()) {
    if (auto p = iota::core::parseF2(v.get<std::string>())) {
      out = *p;
      return true;
    }
    return false;
  }
  if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number()) {
    out = {static_cast<float>(v[0].get<double>()), static_cast<float>(v[1].get<double>())};
    return true;
  }
  if (v.is_object() && v.contains("x") && v.contains("y") && v["x"].is_number() && v["y"].is_number()) {
    out = {static_cast<float>(v["x"].get<double>()), static_cast<float>(v["y"].get<double>())};
    return true;
  }
  return false;
}

bool parseMVarJson(const nlohmann::json& v, std::string& outFormatted, std::string& outErr) {
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    const auto parsed = iota::model::parseMVarString(s);
    outFormatted = iota::model::formatMVarString(parsed);
    return true;
  }
  auto parseSlot = [](const nlohmann::json& obj, iota::model::MeasuredVar& slot) -> bool {
    if (!obj.is_object()) return false;
    std::string mode;
    if (obj.contains("measureWhat") && obj["measureWhat"].is_string()) {
      mode = obj["measureWhat"].get<std::string>();
    } else if (obj.contains("mode") && obj["mode"].is_string()) {
      mode = obj["mode"].get<std::string>();
    } else {
      return false;
    }
    slot.measureWhat = mode;
    slot.a = static_cast<float>(obj.value("a", 0.5));
    slot.b = static_cast<float>(obj.value("b", obj.value("y", 0.5)));
    return true;
  };

  iota::model::MVariableFloat mv;
  if (v.is_object()) {
    if (v.contains("slots") && v["slots"].is_array()) {
      for (const auto& s : v["slots"]) {
        iota::model::MeasuredVar slot;
        if (!parseSlot(s, slot)) {
          outErr = "Invalid MVar slot object in 'slots' array.";
          return false;
        }
        mv.slots.push_back(std::move(slot));
      }
    } else {
      iota::model::MeasuredVar slot;
      if (!parseSlot(v, slot)) {
        outErr = "MVar object requires 'mode' (or 'measureWhat') string field.";
        return false;
      }
      mv.slots.push_back(std::move(slot));
    }
  } else if (v.is_array()) {
    for (const auto& s : v) {
      iota::model::MeasuredVar slot;
      if (!parseSlot(s, slot)) {
        outErr = "Each item in MVar array must be {mode, a, b}.";
        return false;
      }
      mv.slots.push_back(std::move(slot));
    }
  } else {
    outErr = "MVar value must be a string, {mode,a,b} object, or array of slot objects.";
    return false;
  }

  outFormatted = iota::model::formatMVarString(mv);
  return true;
}

std::optional<iota::model::ElementType> resolveElementTypeCI(std::string_view name) noexcept {
  for (auto t : iota::model::addableElementTypes()) {
    if (eqCI(iota::model::elementTypeToString(t), name)) {
      return t;
    }
  }
  const auto direct = iota::model::elementTypeFromString(name);
  if (direct != iota::model::ElementType::Unknown &&
      direct != iota::model::ElementType::Root &&
      direct != iota::model::ElementType::Composition) {
    return direct;
  }
  return std::nullopt;
}

std::optional<iota::model::ElementType> resolveElementOrCompTypeCI(std::string_view name) noexcept {
  if (eqCI(name, "Composition")) {
    return iota::model::ElementType::Composition;
  }
  return resolveElementTypeCI(name);
}

iota::model::Composition* getCompositionOrErr(iota::model::Project& proj,
                                              int compIdx,
                                              std::string& outErr) {
  if (compIdx < 0 || compIdx >= static_cast<int>(proj.compositions.size())) {
    outErr = "composition_index " + std::to_string(compIdx) + " out of bounds (count=" +
             std::to_string(proj.compositions.size()) + ").";
    return nullptr;
  }
  return &proj.compositions[static_cast<size_t>(compIdx)];
}

iota::model::Element* getElementOrErr(iota::model::Project& proj,
                                      int compIdx,
                                      int elemIdx,
                                      std::string& outErr) {
  auto* comp = getCompositionOrErr(proj, compIdx, outErr);
  if (!comp) return nullptr;
  if (elemIdx < 0 || elemIdx >= static_cast<int>(comp->elements.size())) {
    outErr = "element_index " + std::to_string(elemIdx) + " out of bounds in composition " +
             std::to_string(compIdx) + " (count=" + std::to_string(comp->elements.size()) + ").";
    return nullptr;
  }
  return &comp->elements[static_cast<size_t>(elemIdx)];
}

nlohmann::json serializePropertyValue(const iota::model::PropertyEntry& entry,
                                      const iota::model::ParsedTypeSpec& spec) {
  using iota::model::PropertyTypeTag;

  if (std::holds_alternative<bool>(entry.value)) {
    return std::get<bool>(entry.value);
  }
  if (std::holds_alternative<int64_t>(entry.value)) {
    const int64_t iv = std::get<int64_t>(entry.value);
    if (spec.tag == PropertyTypeTag::Bool || spec.tag == PropertyTypeTag::ProBool) {
      return (iv != 0);
    }
    return iv;
  }
  if (std::holds_alternative<double>(entry.value)) {
    return std::get<double>(entry.value);
  }
  if (std::holds_alternative<std::string>(entry.value)) {
    return std::get<std::string>(entry.value);
  }
  return nullptr;
}

nlohmann::json serializePropertyEntryJson(iota::model::ElementType ownerType,
                                          const std::string& key,
                                          const iota::model::PropertyEntry& entry,
                                          bool includeDocs,
                                          bool includeHidden) {
  const auto spec = iota::model::parseTypeTag(entry.typeTag);
  if (spec.tag == iota::model::PropertyTypeTag::HiddenInt && !includeHidden) {
    return nullptr;
  }

  nlohmann::json obj = {
      {"key", key},
      {"display_name", iota::model::formatPropertyDisplayName(ownerType, key)},
      {"group_tag", entry.groupTag},
      {"group", iota::model::formatGroupDisplayName(entry.groupTag.empty() ? "misc" : entry.groupTag)},
      {"type_tag", entry.typeTag},
      {"value", serializePropertyValue(entry, spec)},
  };

  if (spec.hasBounds) {
    obj["range"] = {spec.minVal, spec.maxVal};
  }
  if (!spec.choices.empty()) {
    obj["choices"] = spec.choices;
  }

  if (spec.tag == iota::model::PropertyTypeTag::ColorRgb ||
      spec.tag == iota::model::PropertyTypeTag::ColorRgba ||
      spec.tag == iota::model::PropertyTypeTag::ColorRgbHl) {
    if (const auto* iv = std::get_if<int64_t>(&entry.value)) {
      obj["hex"] = formatArgbHex(iota::core::ColorRgba::fromArgbInt32(static_cast<int32_t>(*iv)));
    }
  } else if (spec.tag == iota::model::PropertyTypeTag::Vec2f) {
    if (const auto* s = std::get_if<std::string>(&entry.value)) {
      if (auto v2 = iota::core::parseF2(*s)) {
        obj["vec2"] = {v2->x, v2->y};
      }
    }
  } else if (spec.tag == iota::model::PropertyTypeTag::MVarFloat ||
             spec.tag == iota::model::PropertyTypeTag::MVarVec2f) {
    if (const auto* s = std::get_if<std::string>(&entry.value)) {
      const auto mv = iota::model::parseMVarString(*s);
      nlohmann::json slots = nlohmann::json::array();
      for (const auto& sl : mv.slots) {
        slots.push_back({{"mode", sl.measureWhat}, {"a", sl.a}, {"b", sl.b}});
      }
      obj["mvar_slots"] = std::move(slots);
    }
  }

  if (includeDocs) {
    if (const auto* doc = iota::model::findPropertyDoc(ownerType, key)) {
      obj["summary"] = doc->summary;
      if (!doc->usage.empty()) obj["usage"] = doc->usage;
      if (!doc->unit.empty()) obj["unit"] = doc->unit;
    }
  }

  if (!entry.children.empty()) {
    nlohmann::json kids = nlohmann::json::array();
    for (const auto& ch : entry.children) {
      nlohmann::json kj = serializePropertyEntryJson(ownerType, ch.key, ch.entry, includeDocs, includeHidden);
      if (!kj.is_null()) {
        kids.push_back(std::move(kj));
      }
    }
    obj["children"] = std::move(kids);
  }
  return obj;
}

bool applyPropertyValue(iota::model::ElementType /*ownerType*/,
                        const std::string& leafKey,
                        iota::model::PropertyEntry& entry,
                        const nlohmann::json& val,
                        std::string& outErr) {
  using iota::model::PropertyTypeTag;
  const auto spec = iota::model::parseTypeTag(entry.typeTag);

  switch (spec.tag) {
    case PropertyTypeTag::Bool:
    case PropertyTypeTag::ProBool: {
      bool bVal = false;
      if (val.is_boolean()) {
        bVal = val.get<bool>();
      } else if (val.is_number_integer()) {
        bVal = (val.get<int64_t>() != 0);
      } else {
        outErr = "Property '" + leafKey + "' expects a boolean.";
        return false;
      }
      if (std::holds_alternative<int64_t>(entry.value)) {
        entry.value = static_cast<int64_t>(bVal ? 1 : 0);
      } else {
        entry.value = bVal;
      }
      return true;
    }

    case PropertyTypeTag::Int:
    case PropertyTypeTag::HiddenInt: {
      if (!val.is_number()) {
        outErr = "Property '" + leafKey + "' expects an integer.";
        return false;
      }
      int64_t iv = val.is_number_integer() ? val.get<int64_t>() : std::llround(val.get<double>());
      if (spec.hasBounds) {
        iv = std::clamp<int64_t>(iv, std::llround(spec.minVal), std::llround(spec.maxVal));
      }
      entry.value = iv;
      return true;
    }

    case PropertyTypeTag::Float: {
      if (!val.is_number()) {
        outErr = "Property '" + leafKey + "' expects a number.";
        return false;
      }
      double dv = val.get<double>();
      if (!std::isfinite(dv)) {
        outErr = "Property '" + leafKey + "' must be finite.";
        return false;
      }
      if (spec.hasBounds) {
        dv = std::clamp<double>(dv, spec.minVal, spec.maxVal);
      }
      entry.value = dv;
      return true;
    }

    case PropertyTypeTag::Vec2f: {
      iota::core::Vec2 vec;
      if (!parseVec2Json(val, vec) || !std::isfinite(vec.x) || !std::isfinite(vec.y)) {
        outErr = "Property '" + leafKey + "' expects [x, y], {x, y}, or \"x y\" vec2.";
        return false;
      }
      if (spec.hasBounds) {
        vec.x = std::clamp(vec.x, spec.minVal, spec.maxVal);
        vec.y = std::clamp(vec.y, spec.minVal, spec.maxVal);
      }
      entry.value = iota::core::formatF2(vec);
      return true;
    }

    case PropertyTypeTag::MVarFloat:
    case PropertyTypeTag::MVarVec2f: {
      std::string formatted;
      if (!parseMVarJson(val, formatted, outErr)) {
        return false;
      }
      entry.value = std::move(formatted);
      return true;
    }

    case PropertyTypeTag::ColorRgb:
    case PropertyTypeTag::ColorRgba:
    case PropertyTypeTag::ColorRgbHl: {
      iota::core::ColorRgba col;
      if (!parseColorRgbaJson(val, col)) {
        outErr = "Property '" + leafKey + "' expects color (#RRGGBB, #AARRGGBB, ARGB int, or [r,g,b,a]).";
        return false;
      }
      if (spec.tag != PropertyTypeTag::ColorRgba) {
        col.a = 255;
      }
      entry.value = static_cast<int64_t>(col.toArgbInt32());
      return true;
    }

    case PropertyTypeTag::ColorHsla4f:
    case PropertyTypeTag::ColorHslaAlpha4f: {
      iota::core::ColorHslaNorm hsla;
      if (!parseColorHslaJson(val, hsla)) {
        outErr = "Property '" + leafKey + "' expects HSLA string, [h,s,l,a], {h,s,l,a}, or #hex color.";
        return false;
      }
      entry.value = iota::core::formatHsla4f(hsla);
      return true;
    }

    case PropertyTypeTag::Select: {
      if (!val.is_string()) {
        outErr = "Property '" + leafKey + "' expects a string option.";
        return false;
      }
      const std::string chosen = val.get<std::string>();
      if (leafKey == "measureWhat") {
        const auto& modes = iota::model::allMeasureWhatModes();
        for (std::string_view m : modes) {
          if (eqCI(m, chosen)) {
            entry.value = std::string(m);
            return true;
          }
        }
        outErr = "Invalid measureWhat mode '" + chosen + "'.";
        return false;
      }
      if (!spec.choices.empty()) {
        for (const auto& ch : spec.choices) {
          if (eqCI(ch, chosen)) {
            entry.value = ch;
            return true;
          }
        }
        std::string allowed;
        for (size_t i = 0; i < spec.choices.size(); ++i) {
          if (i > 0) allowed += ", ";
          allowed += spec.choices[i];
        }
        outErr = "Invalid choice '" + chosen + "' for '" + leafKey + "'. Allowed: " + allowed;
        return false;
      }
      entry.value = chosen;
      return true;
    }

    case PropertyTypeTag::Child:
    case PropertyTypeTag::DynamicGroup: {
      if (val.is_string()) {
        entry.value = val.get<std::string>();
        return true;
      }
      if (val.is_object()) {
        for (auto it = val.begin(); it != val.end(); ++it) {
          auto* child = entry.findChild(it.key());
          if (!child && (it.key() == "A" || it.key() == "B") && entry.findChild("measureWhat") != nullptr) {
            auto& created = entry.getOrAddChild(it.key());
            created.value = 0.5;
            created.typeTag = "f -2.0 2.0";
            child = &created;
          }
          if (!child) {
            outErr = "Unknown child key '" + it.key() + "' under '" + leafKey + "'.";
            return false;
          }
          if (!applyPropertyValue(iota::model::ElementType::Unknown, it.key(), *child, it.value(), outErr)) {
            return false;
          }
        }
        return true;
      }
      outErr = "Group property '" + leafKey + "' expects a variant string or child object.";
      return false;
    }

    case PropertyTypeTag::Text:
    case PropertyTypeTag::ProText:
    case PropertyTypeTag::CodeText:
    case PropertyTypeTag::Image:
    case PropertyTypeTag::ParticleImage:
    case PropertyTypeTag::Asset:
    case PropertyTypeTag::ProAsset:
    default: {
      if (val.is_string()) {
        entry.value = val.get<std::string>();
        return true;
      }
      if (std::holds_alternative<double>(entry.value) && val.is_number()) {
        entry.value = val.get<double>();
        return true;
      }
      if (std::holds_alternative<int64_t>(entry.value) && val.is_number()) {
        entry.value = val.is_number_integer() ? val.get<int64_t>() : std::llround(val.get<double>());
        return true;
      }
      outErr = "Property '" + leafKey + "' expects a string value.";
      return false;
    }
  }
}

bool resolveOrSeedProperty(iota::model::PropertyBag& bag,
                           iota::model::ElementType ownerType,
                           int compIdx,
                           std::string_view propPath,
                           iota::model::PropertyEntry*& outEntry,
                           std::string& outLeafKey,
                           std::string& outErr) {
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos <= propPath.size()) {
    const size_t dot = propPath.find('.', pos);
    if (dot == std::string_view::npos) {
      parts.emplace_back(propPath.substr(pos));
      break;
    }
    parts.emplace_back(propPath.substr(pos, dot - pos));
    pos = dot + 1;
  }
  if (parts.empty() || parts[0].empty()) {
    outErr = "Property key cannot be empty.";
    return false;
  }

  iota::model::PropertyEntry* cur = bag.find(parts[0]);
  if (!cur) {
    for (auto& item : bag.items()) {
      if (eqCI(item.key, parts[0])) {
        cur = &item.entry;
        parts[0] = item.key;
        break;
      }
    }
  }
  if (!cur) {
    iota::model::PropertyBag defaults =
        (ownerType == iota::model::ElementType::Composition)
            ? iota::model::createDefaultComposition(std::max(0, compIdx)).props
            : iota::model::createDefaultElement(ownerType).props;
    const std::string* canonicalKey = nullptr;
    const iota::model::PropertyEntry* defEntry = defaults.find(parts[0]);
    if (defEntry) {
      canonicalKey = &parts[0];
    } else {
      for (const auto& item : defaults.items()) {
        if (eqCI(item.key, parts[0])) {
          defEntry = &item.entry;
          canonicalKey = &item.key;
          break;
        }
      }
    }
    if (defEntry && canonicalKey) {
      bag.getOrAdd(*canonicalKey) = *defEntry;
      cur = bag.find(*canonicalKey);
    }
  }
  if (!cur) {
    outErr = "Unknown property '" + parts[0] + "' on element type '" +
             std::string(ownerType == iota::model::ElementType::Composition
                             ? "Composition"
                             : iota::model::elementTypeToString(ownerType)) +
             "'.";
    return false;
  }

  for (size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].empty()) {
      outErr = "Empty child segment in property path '" + std::string(propPath) + "'.";
      return false;
    }
    auto* next = cur->findChild(parts[i]);
    if (!next) {
      for (auto& ch : cur->children) {
        if (eqCI(ch.key, parts[i])) {
          next = &ch.entry;
          parts[i] = ch.key;
          break;
        }
      }
    }
    if (!next && (eqCI(parts[i], "A") || eqCI(parts[i], "B")) && cur->findChild("measureWhat") != nullptr) {
      const std::string abKey = eqCI(parts[i], "A") ? "A" : "B";
      auto& created = cur->getOrAddChild(abKey);
      created.value = 0.5;
      created.typeTag = "f -2.0 2.0";
      next = &created;
      parts[i] = abKey;
    }
    if (!next) {
      outErr = "Unknown child property '" + parts[i] + "' in path '" + std::string(propPath) + "'.";
      return false;
    }
    cur = next;
  }

  outEntry = cur;
  outLeafKey = parts.back();
  return true;
}

nlohmann::json summarizeProject(const IStudioBridgeTarget& target) {
  const auto& proj = target.project();
  nlohmann::json comps = nlohmann::json::array();
  for (size_t ci = 0; ci < proj.compositions.size(); ++ci) {
    const auto& comp = proj.compositions[ci];
    nlohmann::json elems = nlohmann::json::array();
    for (size_t ei = 0; ei < comp.elements.size(); ++ei) {
      const auto& el = comp.elements[ei];
      elems.push_back({
          {"element_index", static_cast<int>(ei)},
          {"runtime_id", el.runtimeId()},
          {"type", el.typeName},
          {"name", el.displayName},
          {"visible", el.props.getBool("visible", true)},
          {"soloed", iota::model::isElementSoloedInComposition(proj, static_cast<int>(ci), static_cast<int>(ei))},
      });
    }
    comps.push_back({
        {"composition_index", static_cast<int>(ci)},
        {"name", comp.displayName},
        {"element_count", static_cast<int>(comp.elements.size())},
        {"elements", std::move(elems)},
    });
  }

  nlohmann::json assets = nlohmann::json::array();
  for (const auto& [name, bytes] : proj.localAssets) {
    const std::string uri = "local:" + name;
    auto folderIt = proj.metadata.assetFolders.find(name);
    assets.push_back({
        {"name", name},
        {"uri", uri},
        {"bytes", bytes.size()},
        {"reference_count", iota::model::countAssetReferences(proj, uri)},
        {"folder", folderIt != proj.metadata.assetFolders.end() ? folderIt->second : ""},
    });
  }

  const auto curPath = target.currentProjectPath();
  return {
      {"mode", target.isLiveStudio() ? "live_studio" : "headless"},
      {"current_project_path", iota::core::pathToUtf8(curPath)},
      {"is_mcp_working_copy", isMcpWorkingCopyPath(curPath)},
      {"metadata",
       {
           {"name", proj.metadata.name},
           {"notes", proj.metadata.notes},
           {"audio_path", proj.metadata.audioPath},
           {"preview_width", target.previewWidth()},
           {"preview_height", target.previewHeight()},
           {"target_fps", target.targetFps()},
       }},
      {"can_undo", target.canUndo()},
      {"can_redo", target.canRedo()},
      {"compositions", std::move(comps)},
      {"local_assets", std::move(assets)},
      {"desktop_keyframe_tracks", static_cast<int>(proj.desktopTracks.size())},
  };
}

nlohmann::json buildElementSchemaJson(iota::model::ElementType type) {
  const auto& edoc = iota::model::getElementDoc(type);
  const iota::model::PropertyBag bag =
      (type == iota::model::ElementType::Composition)
          ? iota::model::createDefaultComposition(0).props
          : iota::model::createDefaultElement(type).props;

  nlohmann::json props = nlohmann::json::array();
  for (const auto& item : bag.items()) {
    nlohmann::json pj = serializePropertyEntryJson(type, item.key, item.entry, true, false);
    if (!pj.is_null()) {
      props.push_back(std::move(pj));
    }
  }

  return {
      {"type", type == iota::model::ElementType::Composition
                   ? "Composition"
                   : std::string(iota::model::elementTypeToString(type))},
      {"display_name", edoc.displayName},
      {"category", edoc.category},
      {"overview", edoc.overview},
      {"tips", edoc.tips},
      {"properties", std::move(props)},
  };
}

nlohmann::json buildAllDocsJson(std::string_view uri) {
  if (uri == "iota://docs/elements") {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& ed : iota::model::allElementDocs()) {
      arr.push_back({
          {"type", ed.type == iota::model::ElementType::Composition
                       ? "Composition"
                       : std::string(iota::model::elementTypeToString(ed.type))},
          {"display_name", ed.displayName},
          {"category", ed.category},
          {"overview", ed.overview},
          {"tips", ed.tips},
      });
    }
    return arr;
  }
  if (uri == "iota://docs/mvar-modes") {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : iota::model::allMVarModeDocs()) {
      arr.push_back({
          {"mode", m.mode},
          {"display_name", m.displayName},
          {"summary", m.summary},
          {"param_a", m.paramA},
          {"param_b", m.paramB},
      });
    }
    return arr;
  }
  if (uri == "iota://docs/text-tokens") {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& tk : iota::model::allTextTokenDocs()) {
      arr.push_back({{"token", tk.token}, {"summary", tk.summary}});
    }
    return arr;
  }
  return {};
}

nlohmann::json makeToolDef(const char* name,
                           const char* desc,
                           nlohmann::json props = nlohmann::json::object(),
                           std::vector<std::string> required = {}) {
  nlohmann::json schema = {
      {"type", "object"},
      {"properties", std::move(props)},
  };
  if (!required.empty()) {
    schema["required"] = std::move(required);
  }
  return {
      {"name", name},
      {"description", desc},
      {"inputSchema", std::move(schema)},
  };
}

}  // namespace

nlohmann::json listMcpToolDefinitions() {
  nlohmann::json tools = nlohmann::json::array();

  tools.push_back(makeToolDef(
      "project_new",
      "Create a new empty or template Iota Studio project (starter, minimal_circular, dual_comp_glow, horizontal_curve, blank).",
      {{"template", {{"type", "string"}, {"description", "Template name: starter, minimal_circular, dual_comp_glow, horizontal_curve, or blank"}}},
       {"name", {{"type", "string"}}},
       {"width", {{"type", "integer"}}},
       {"height", {{"type", "integer"}}},
       {"fps", {{"type", "integer"}}}}));

  tools.push_back(makeToolDef(
      "project_open",
      "Load an .iota, .viz, or .json visualizer file. When Studio Live Bridge is active, automatically creates and switches onto a sibling <name>.mcp.iota working copy.",
      {{"path", {{"type", "string"}}}},
      {"path"}));

  tools.push_back(makeToolDef(
      "project_save",
      "Save the current project (.iota or .json recommended). Existing .viz files are protected unless allow_viz_overwrite is true.",
      {{"path", {{"type", "string"}, {"description", "Target file path (optional when already associated with a .iota working copy)"}}},
       {"allow_viz_overwrite", {{"type", "boolean"}, {"description", "Must be true to overwrite an existing legacy .viz template"}}}}));

  tools.push_back(makeToolDef(
      "project_get_summary",
      "Return compositions, layers (indices, runtime IDs, visibility, solo), metadata, and embedded local assets."));

  tools.push_back(makeToolDef(
      "project_set_metadata",
      "Update project metadata such as name, notes, preview width/height, and target FPS.",
      {{"name", {{"type", "string"}}},
       {"notes", {{"type", "string"}}},
       {"preview_width", {{"type", "integer"}}},
       {"preview_height", {{"type", "integer"}}},
       {"target_fps", {{"type", "integer"}}}}));

  tools.push_back(makeToolDef(
      "project_import_asset",
      "Import a local image or font file into project.localAssets and return its local:<name> URI.",
      {{"file_path", {{"type", "string"}}},
       {"asset_name", {{"type", "string"}}},
       {"folder", {{"type", "string"}}}},
      {"file_path"}));

  tools.push_back(makeToolDef(
      "composition_add",
      "Add a new offscreen composition (Composition N) to the project.",
      {{"name", {{"type", "string"}}}}));

  tools.push_back(makeToolDef(
      "composition_rename",
      "Rename an existing composition.",
      {{"composition_index", {{"type", "integer"}}},
       {"name", {{"type", "string"}}}},
      {"composition_index", "name"}));

  tools.push_back(makeToolDef(
      "composition_duplicate",
      "Duplicate a composition cleanly with fresh element IDs.",
      {{"composition_index", {{"type", "integer"}}}},
      {"composition_index"}));

  tools.push_back(makeToolDef(
      "composition_delete",
      "Delete a secondary composition (Composition 1+) and remap remaining composition:N references.",
      {{"composition_index", {{"type", "integer"}}}},
      {"composition_index"}));

  tools.push_back(makeToolDef(
      "layer_add",
      "Add a layer/effect (AudioProvider, SoundKeys, Bars, Particles, Image, Text, StatText, BlurEffect, RgbSplitEffect, MotionBlurEffect, ShakeEffect, MirrorEffect, AppLogo, CustomShader).",
      {{"element_type", {{"type", "string"}}},
       {"composition_index", {{"type", "integer"}}},
       {"insert_index", {{"type", "integer"}}},
       {"name", {{"type", "string"}}}},
      {"element_type"}));

  tools.push_back(makeToolDef(
      "layer_remove",
      "Remove a layer from a composition.",
      {{"composition_index", {{"type", "integer"}}},
       {"element_index", {{"type", "integer"}}}},
      {"composition_index", "element_index"}));

  tools.push_back(makeToolDef(
      "layer_rename",
      "Rename a layer.",
      {{"composition_index", {{"type", "integer"}}},
       {"element_index", {{"type", "integer"}}},
       {"name", {{"type", "string"}}}},
      {"composition_index", "element_index", "name"}));

  tools.push_back(makeToolDef(
      "layer_duplicate",
      "Duplicate a layer with fresh runtime IDs and synced keyframe tracks.",
      {{"composition_index", {{"type", "integer"}}},
       {"element_index", {{"type", "integer"}}}},
      {"composition_index", "element_index"}));

  tools.push_back(makeToolDef(
      "layer_move",
      "Move a layer within a composition or across compositions.",
      {{"src_composition_index", {{"type", "integer"}}},
       {"src_element_index", {{"type", "integer"}}},
       {"dst_composition_index", {{"type", "integer"}}},
       {"dst_insert_index", {{"type", "integer"}}}},
      {"src_composition_index", "src_element_index", "dst_composition_index"}));

  tools.push_back(makeToolDef(
      "layer_set_visible",
      "Show or hide a layer.",
      {{"composition_index", {{"type", "integer"}}},
       {"element_index", {{"type", "integer"}}},
       {"visible", {{"type", "boolean"}}}},
      {"composition_index", "element_index", "visible"}));

  tools.push_back(makeToolDef(
      "layer_solo",
      "Toggle solo visibility on one or more layers inside a composition.",
      {{"composition_index", {{"type", "integer"}}},
       {"element_indices", {{"type", "array"}, {"items", {{"type", "integer"}}}}}},
      {"composition_index", "element_indices"}));

  tools.push_back(makeToolDef(
      "layer_precompose",
      "Move selected layers into a new secondary composition and replace them with a composite Image host layer.",
      {{"composition_index", {{"type", "integer"}}},
       {"element_indices", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
       {"name", {{"type", "string"}}}},
      {"composition_index", "element_indices"}));

  tools.push_back(makeToolDef(
      "layer_get_properties",
      "Read properties, human-readable names, valid ranges/choices, nested children, and documentation for a layer (or element_index=-1 for composition HDR/bloom properties).",
      {{"composition_index", {{"type", "integer"}}},
       {"element_index", {{"type", "integer"}}},
       {"include_docs", {{"type", "boolean"}}},
       {"include_hidden", {{"type", "boolean"}}}},
      {"composition_index"}));

  tools.push_back(makeToolDef(
      "layer_set_properties",
      "Batch-update properties on a layer (or element_index=-1 for composition properties). Supports scalar values, [x,y] vec2, #RRGGBB/#AARRGGBB hex colors, MVar expressions, CustomShader GLSL, and dotted paths like 'segment1.barWidth'.",
      {{"composition_index", {{"type", "integer"}}},
       {"element_index", {{"type", "integer"}}},
       {"properties", {{"type", "object"}}},
       {"undo_label", {{"type", "string"}}}},
      {"composition_index", "properties"}));

  tools.push_back(makeToolDef(
      "layer_set_keyframe",
      "Insert or update a desktop keyframe on a numeric float property.",
      {{"composition_index", {{"type", "integer"}}},
       {"element_index", {{"type", "integer"}}},
       {"property_path", {{"type", "string"}}},
       {"time_sec", {{"type", "number"}}},
       {"value", {{"type", "number"}}}},
      {"composition_index", "element_index", "property_path", "time_sec", "value"}));

  tools.push_back(makeToolDef(
      "layer_clear_keyframes",
      "Clear all desktop keyframes from a layer's property track.",
      {{"composition_index", {{"type", "integer"}}},
       {"element_index", {{"type", "integer"}}},
       {"property_path", {{"type", "string"}}}},
      {"composition_index", "element_index", "property_path"}));

  tools.push_back(makeToolDef(
      "docs_list_elements",
      "List all documented Iota Studio layer types, effects, and Composition settings."));

  tools.push_back(makeToolDef(
      "docs_get_element_schema",
      "Get full property schema, units, numeric ranges, choices, and usage notes for an element type (or 'Composition').",
      {{"element_type", {{"type", "string"}}}},
      {"element_type"}));

  tools.push_back(makeToolDef(
      "docs_search",
      "Search across element properties, internal keys, all 26 MVar audio-reactive signal modes, and text tokens.",
      {{"query", {{"type", "string"}}}},
      {"query"}));

  tools.push_back(makeToolDef(
      "audio_load",
      "Load an audio file (MP3/FLAC/WAV/OGG) or generate a synthetic sine test tone for visual testing.",
      {{"file_path", {{"type", "string"}}},
       {"synthetic_freq_hz", {{"type", "number"}}},
       {"synthetic_duration_sec", {{"type", "number"}}}}));

  tools.push_back(makeToolDef(
      "render_preview_frame",
      "Render a frame with OpenGL 4.5 SceneRenderer, returning render telemetry and an inline PNG image (optionally also writing to save_png_path).",
      {{"width", {{"type", "integer"}}},
       {"height", {{"type", "integer"}}},
       {"time_sec", {{"type", "number"}}},
       {"pre_roll_frames", {{"type", "integer"}}},
       {"fxaa", {{"type", "boolean"}}},
       {"return_image", {{"type", "boolean"}}},
       {"save_png_path", {{"type", "string"}}}}));

  tools.push_back(makeToolDef(
      "export_list_encoders",
      "Probe FFmpeg availability and GPU hardware video encoders (NVIDIA NVENC, AMD AMF, Intel Quick Sync) plus CPU software encoders."));

  tools.push_back(makeToolDef(
      "export_video",
      "Export MP4 video using hardware GPU (auto/nvenc/amf/qsv) or CPU (x264/x265) encoding with automatic CPU fallback.",
      {{"output_path", {{"type", "string"}}},
       {"width", {{"type", "integer"}}},
       {"height", {{"type", "integer"}}},
       {"fps", {{"type", "integer"}}},
       {"bitrate_kbps", {{"type", "integer"}}},
       {"codec", {{"type", "string", }, {"description", "h264 or h265"}}},
       {"encoder_backend", {{"type", "string"}, {"description", "auto, nvenc, amf, qsv, or cpu"}}},
       {"encoder_preset", {{"type", "string"}, {"description", "fast, balanced, or quality"}}},
       {"fallback_to_cpu", {{"type", "boolean"}}},
       {"start_sec", {{"type", "number"}}},
       {"end_sec", {{"type", "number"}}},
       {"pre_roll_frames", {{"type", "integer"}}},
       {"fxaa", {{"type", "boolean"}}}},
      {"output_path"}));

  tools.push_back(makeToolDef(
      "studio_status",
      "Return session mode (headless vs live_studio), current project path/working-copy status, audio playback state, and undo availability."));

  tools.push_back(makeToolDef(
      "studio_playback",
      "Control audio playback state (play, pause, stop, toggle, or seek).",
      {{"action", {{"type", "string"}}},
       {"seek_sec", {{"type", "number"}}}},
      {"action"}));

  tools.push_back(makeToolDef(
      "studio_undo",
      "Undo the last edit in the current session."));

  tools.push_back(makeToolDef(
      "studio_redo",
      "Redo a previously undone edit in the current session."));

  tools.push_back(makeToolDef(
      "studio_connect",
      "Connect iota_mcp to a running Iota Studio GUI window with MCP Live Bridge enabled.",
      {{"pipe_name", {{"type", "string"}}},
       {"timeout_ms", {{"type", "integer"}}}}));

  tools.push_back(makeToolDef(
      "studio_disconnect",
      "Disconnect from the live Studio GUI bridge and return to standalone headless mode."));

  return tools;
}

nlohmann::json listMcpResourceDefinitions() {
  return nlohmann::json::array({
      {{"uri", "iota://docs/elements"},
       {"name", "Element & Composition Documentation"},
       {"description", "Overview, category, and authoring tips for every built-in element and effect."},
       {"mimeType", "application/json"}},
      {{"uri", "iota://docs/mvar-modes"},
       {"name", "MVar Audio Reactivity Modes"},
       {"description", "All 26 MVar signal sources and parameter meanings."},
       {"mimeType", "application/json"}},
      {{"uri", "iota://docs/text-tokens"},
       {"name", "Dynamic Text Tokens"},
       {"description", "Supported {Token} placeholders for Text and StatText layers."},
       {"mimeType", "application/json"}},
      {{"uri", "iota://docs/custom-shader"},
       {"name", "CustomShader GLSL 450 Reference"},
       {"description", "Built-in shader uniforms, varyings, and starter GLSL snippet."},
       {"mimeType", "application/json"}},
      {{"uri", "iota://project/current"},
       {"name", "Current Project Summary"},
       {"description", "Live or headless project structure, layers, and assets."},
       {"mimeType", "application/json"}},
  });
}

nlohmann::json readMcpResource(IStudioBridgeTarget& target,
                                 std::string_view uri,
                                 bool& outFound,
                                 std::string& outError) {
  outFound = true;
  auto wrapJson = [&](const nlohmann::json& j) {
    return nlohmann::json::array({
        {{"uri", std::string(uri)},
         {"mimeType", "application/json"},
         {"text", j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)}},
    });
  };

  if (uri == "iota://docs/elements" || uri == "iota://docs/mvar-modes" || uri == "iota://docs/text-tokens") {
    return wrapJson(buildAllDocsJson(uri));
  }
  if (uri == "iota://docs/custom-shader") {
    const nlohmann::json j = {
        {"glsl_version", "#version 450 core"},
        {"output", "out vec4 iota_FragColor;"},
        {"inputs", nlohmann::json::array({"in vec2 v_uv;", "in vec4 v_color;"})},
        {"uniforms",
         nlohmann::json::array({
             {{"name", "u_texture"}, {"type", "sampler2D"}, {"description", "Input from Target Image (e.g. composition:0 or composition:1)"}},
             {{"name", "u_time"}, {"type", "float"}, {"description", "Timeline time in seconds"}},
             {{"name", "u_resolution"}, {"type", "vec2"}, {"description", "Canvas pixel size"}},
             {{"name", "u_beat"}, {"type", "float"}, {"description", "Normalized beat [0..1] from AudioProvider 1"}},
             {{"name", "u_soundKeys"}, {"type", "vec4"}, {"description", "(R1, R2, R3, beat)"}},
             {{"name", "u_projView"}, {"type", "mat4"}, {"description", "Orthographic projection-view matrix"}},
         })},
    };
    return wrapJson(j);
  }
  if (uri == "iota://project/current") {
    return wrapJson(summarizeProject(target));
  }

  outFound = false;
  outError = "Unknown resource URI: " + std::string(uri);
  return nlohmann::json::array();
}

nlohmann::json executeMcpTool(IStudioBridgeTarget& target,
                              std::string_view name,
                              const nlohmann::json& args,
                              uint64_t liveRequestsHandled,
                              bool& outFound) {
  outFound = true;
  auto& proj = target.project();

  if (name == "project_new") {
    const std::string tmpl = args.value("template", std::string("starter"));
    const std::string projName = args.value("name", std::string("Untitled Project"));
    const int w = std::clamp(args.value("width", 1280), 64, 4096);
    const int h = std::clamp(args.value("height", 720), 64, 4096);
    const int fps = std::clamp(args.value("fps", 60), 15, 120);

    iota::model::Project nextProj;
    if (eqCI(tmpl, "starter")) {
      nextProj = iota::model::createStarterProject();
      nextProj.metadata.name = projName;
      nextProj.metadata.previewWidth = w;
      nextProj.metadata.previewHeight = h;
      nextProj.metadata.targetFps = fps;
    } else if (eqCI(tmpl, "minimal_circular") || eqCI(tmpl, "MinimalCircular")) {
      nextProj = iota::preset_io::createProjectFromTemplate(
          iota::preset_io::ProjectTemplateKind::MinimalCircular, projName, w, h, fps);
    } else if (eqCI(tmpl, "dual_comp_glow") || eqCI(tmpl, "DualCompGlow")) {
      nextProj = iota::preset_io::createProjectFromTemplate(
          iota::preset_io::ProjectTemplateKind::DualCompGlow, projName, w, h, fps);
    } else if (eqCI(tmpl, "horizontal_curve") || eqCI(tmpl, "HorizontalCurve") || eqCI(tmpl, "HorizontalSpectrumBar")) {
      nextProj = iota::preset_io::createProjectFromTemplate(
          iota::preset_io::ProjectTemplateKind::HorizontalCurve, projName, w, h, fps);
    } else if (eqCI(tmpl, "blank")) {
      nextProj = iota::preset_io::createProjectFromTemplate(
          iota::preset_io::ProjectTemplateKind::Blank, projName, w, h, fps);
    } else {
      return makeToolErr("Unknown template '" + tmpl +
                         "'. Use starter, minimal_circular, dual_comp_glow, horizontal_curve, or blank.");
    }

    target.createNewProject(std::move(nextProj), "MCP New Project");
    return makeToolOk(summarizeProject(target));
  }

  if (name == "project_open") {
    if (!args.contains("path") || !args["path"].is_string()) {
      return makeToolErr("project_open requires a string 'path'.");
    }
    const auto p = iota::core::pathFromUtf8(args["path"].get<std::string>());
    std::string err;
    std::filesystem::path activePath;
    if (!target.loadProject(p, &err, &activePath)) {
      return makeToolErr(err.empty() ? ("Failed to open project: " + iota::core::pathToUtf8(p)) : err);
    }
    nlohmann::json res = summarizeProject(target);
    res["opened_source_path"] = iota::core::pathToUtf8(p);
    res["active_working_path"] = iota::core::pathToUtf8(activePath);
    return makeToolOk(res);
  }

  if (name == "project_save") {
    std::filesystem::path p;
    if (args.contains("path") && args["path"].is_string()) {
      p = iota::core::pathFromUtf8(args["path"].get<std::string>());
    }
    const bool allowVizOverwrite = args.value("allow_viz_overwrite", false);
    std::string err;
    std::filesystem::path savedPath;
    if (!target.saveProject(p, allowVizOverwrite, &err, &savedPath)) {
      return makeToolErr(err.empty() ? "Failed to save project." : err);
    }
    return makeToolOk({
        {"saved", true},
        {"path", iota::core::pathToUtf8(savedPath)},
        {"is_mcp_working_copy", isMcpWorkingCopyPath(savedPath)},
    });
  }

  if (name == "project_get_summary") {
    return makeToolOk(summarizeProject(target));
  }

  if (name == "project_set_metadata") {
    if (args.contains("name") && args["name"].is_string()) {
      proj.metadata.name = iota::core::sanitizeUtf8(args["name"].get<std::string>());
    }
    if (args.contains("notes") && args["notes"].is_string()) {
      proj.metadata.notes = iota::core::sanitizeUtf8(args["notes"].get<std::string>());
    }
    const int pw = args.value("preview_width", 0);
    const int ph = args.value("preview_height", 0);
    const int fps = args.value("target_fps", 0);
    if (pw > 0 || ph > 0 || fps > 0) {
      target.setPreviewMetrics(pw, ph, fps);
    }
    target.commitEdit("MCP Update Project Metadata");
    return makeToolOk(summarizeProject(target));
  }

  if (name == "project_import_asset") {
    if (!args.contains("file_path") || !args["file_path"].is_string()) {
      return makeToolErr("project_import_asset requires a string 'file_path'.");
    }
    const auto diskPath = iota::core::pathFromUtf8(args["file_path"].get<std::string>());
    if (!iota::core::pathExistsNoThrow(diskPath)) {
      return makeToolErr("Asset file not found: " + iota::core::pathToUtf8(diskPath));
    }
    std::ifstream ifs(diskPath, std::ios::binary | std::ios::ate);
    if (!ifs) {
      return makeToolErr("Failed to open asset file for reading.");
    }
    const std::streamoff sz = ifs.tellg();
    constexpr std::streamoff kMaxAssetBytes = 64LL * 1024LL * 1024LL;
    if (sz <= 0 || sz > kMaxAssetBytes) {
      return makeToolErr("Asset file is empty or exceeds 64 MB limit.");
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (!ifs.read(reinterpret_cast<char*>(bytes.data()), sz)) {
      return makeToolErr("Failed reading asset bytes.");
    }

    std::string assetName = args.value("asset_name", std::string{});
    if (assetName.empty()) {
      assetName = iota::core::pathFilenameUtf8(diskPath);
    } else {
      assetName = iota::core::baseNameFromSlashPath(assetName);
    }
    if (assetName.empty()) {
      assetName = "asset.bin";
    }

    proj.localAssets[assetName] = bytes;
    const std::string folder = args.value("folder", std::string{});
    if (!folder.empty()) {
      proj.metadata.assetFolders[assetName] = folder;
      if (std::find(proj.metadata.customFolders.begin(), proj.metadata.customFolders.end(), folder) ==
          proj.metadata.customFolders.end()) {
        proj.metadata.customFolders.push_back(folder);
      }
    }
    target.commitEdit("MCP Import Asset", true, false);
    return makeToolOk({
        {"asset_name", assetName},
        {"uri", "local:" + assetName},
        {"bytes", bytes.size()},
        {"folder", folder},
    });
  }

  if (name == "composition_add") {
    const std::string compName = args.value("name", std::string{});
    const int newIdx = iota::model::addComposition(proj, compName);
    target.commitEdit("MCP Add Composition");
    return makeToolOk({
        {"composition_index", newIdx},
        {"name", proj.compositions[static_cast<size_t>(newIdx)].displayName},
    });
  }

  if (name == "composition_rename") {
    const int compIdx = args.value("composition_index", -1);
    const std::string newName = args.value("name", std::string{});
    if (newName.empty() || !iota::model::renameComposition(proj, compIdx, newName)) {
      return makeToolErr("Failed to rename composition (check composition_index and non-empty name).");
    }
    target.commitEdit("MCP Rename Composition");
    return makeToolOk({{"composition_index", compIdx}, {"name", newName}});
  }

  if (name == "composition_duplicate") {
    const int compIdx = args.value("composition_index", -1);
    const int copyIdx = iota::model::duplicateComposition(proj, compIdx);
    if (copyIdx < 0) {
      return makeToolErr("Invalid composition_index for duplication.");
    }
    target.commitEdit("MCP Duplicate Composition");
    return makeToolOk({
        {"source_index", compIdx},
        {"new_composition_index", copyIdx},
        {"name", proj.compositions[static_cast<size_t>(copyIdx)].displayName},
    });
  }

  if (name == "composition_delete") {
    const int compIdx = args.value("composition_index", -1);
    if (!iota::model::deleteComposition(proj, compIdx)) {
      return makeToolErr("Cannot delete Composition 0 or invalid composition_index.");
    }
    target.commitEdit("MCP Delete Composition", false, true);
    return makeToolOk({{"deleted_index", compIdx}, {"composition_count", proj.compositions.size()}});
  }

  if (name == "layer_add") {
    const std::string typeStr = args.value("element_type", std::string{});
    const auto resolved = resolveElementTypeCI(typeStr);
    if (!resolved) {
      return makeToolErr("Unknown or non-addable element_type '" + typeStr + "'. Call docs_list_elements first.");
    }
    const int compIdx = args.value("composition_index", 0);
    std::string err;
    auto* comp = getCompositionOrErr(proj, compIdx, err);
    if (!comp) return makeToolErr(err);

    iota::model::Element el = iota::model::createDefaultElement(*resolved);
    if (args.contains("name") && args["name"].is_string() && !args["name"].get<std::string>().empty()) {
      el.displayName = iota::core::sanitizeUtf8(args["name"].get<std::string>());
    }
    iota::model::assignFreshElementId(proj, el);

    int insertIdx = args.value("insert_index", -1);
    if (insertIdx < 0 || insertIdx > static_cast<int>(comp->elements.size())) {
      insertIdx = static_cast<int>(comp->elements.size());
    }
    const int rid = el.runtimeId();
    const std::string elTypeName = el.typeName;
    const std::string elDispName = el.displayName;
    comp->elements.insert(comp->elements.begin() + insertIdx, std::move(el));

    target.commitEdit("MCP Add " + elTypeName);
    return makeToolOk({
        {"composition_index", compIdx},
        {"element_index", insertIdx},
        {"runtime_id", rid},
        {"type", elTypeName},
        {"name", elDispName},
    });
  }

  if (name == "layer_remove") {
    const int compIdx = args.value("composition_index", -1);
    const int elemIdx = args.value("element_index", -1);
    std::string err;
    if (!getElementOrErr(proj, compIdx, elemIdx, err)) {
      return makeToolErr(err);
    }
    iota::model::deleteElements(proj, {{compIdx, elemIdx}});
    target.commitEdit("MCP Remove Layer");
    return makeToolOk({{"removed", true}, {"composition_index", compIdx}, {"element_index", elemIdx}});
  }

  if (name == "layer_rename") {
    const int compIdx = args.value("composition_index", -1);
    const int elemIdx = args.value("element_index", -1);
    const std::string newName = args.value("name", std::string{});
    if (newName.empty() || !iota::model::renameElement(proj, {compIdx, elemIdx}, newName)) {
      return makeToolErr("Failed to rename layer (invalid indices or empty name).");
    }
    target.commitEdit("MCP Rename Layer");
    return makeToolOk({{"composition_index", compIdx}, {"element_index", elemIdx}, {"name", newName}});
  }

  if (name == "layer_duplicate") {
    const int compIdx = args.value("composition_index", -1);
    const int elemIdx = args.value("element_index", -1);
    std::string err;
    if (!getElementOrErr(proj, compIdx, elemIdx, err)) {
      return makeToolErr(err);
    }
    const auto dup = iota::model::duplicateElements(proj, {{compIdx, elemIdx}});
    if (dup.empty()) {
      return makeToolErr("Failed to duplicate layer.");
    }
    target.commitEdit("MCP Duplicate Layer");
    return makeToolOk({
        {"composition_index", dup.front().compIdx},
        {"element_index", dup.front().elemIdx},
    });
  }

  if (name == "layer_move") {
    const int srcComp = args.value("src_composition_index", -1);
    const int srcElem = args.value("src_element_index", -1);
    const int dstComp = args.value("dst_composition_index", -1);
    const int dstInsert = args.value("dst_insert_index", -1);
    std::string err;
    if (!getElementOrErr(proj, srcComp, srcElem, err) || !getCompositionOrErr(proj, dstComp, err)) {
      return makeToolErr(err);
    }
    const auto moved = iota::model::moveElements(proj, {{srcComp, srcElem}}, dstComp, dstInsert);
    if (moved.empty()) {
      return makeToolErr("Failed to move layer.");
    }
    target.commitEdit("MCP Move Layer");
    return makeToolOk({
        {"composition_index", moved.front().compIdx},
        {"element_index", moved.front().elemIdx},
    });
  }

  if (name == "layer_set_visible") {
    const int compIdx = args.value("composition_index", -1);
    const int elemIdx = args.value("element_index", -1);
    if (!args.contains("visible") || !args["visible"].is_boolean()) {
      return makeToolErr("layer_set_visible requires boolean 'visible'.");
    }
    const bool vis = args["visible"].get<bool>();
    if (!iota::model::setElementsVisible(proj, {{compIdx, elemIdx}}, vis)) {
      return makeToolErr("Invalid layer indices for layer_set_visible.");
    }
    target.commitEdit(vis ? "MCP Show Layer" : "MCP Hide Layer");
    return makeToolOk({{"composition_index", compIdx}, {"element_index", elemIdx}, {"visible", vis}});
  }

  if (name == "layer_solo") {
    const int compIdx = args.value("composition_index", -1);
    std::string err;
    if (!getCompositionOrErr(proj, compIdx, err)) {
      return makeToolErr(err);
    }
    std::vector<int> indices;
    if (args.contains("element_indices") && args["element_indices"].is_array()) {
      for (const auto& idx : args["element_indices"]) {
        if (idx.is_number_integer()) indices.push_back(idx.get<int>());
      }
    }
    if (indices.empty() || !iota::model::soloElementsInComposition(proj, compIdx, indices)) {
      return makeToolErr("Failed to solo specified element_indices.");
    }
    target.commitEdit("MCP Solo Layer");
    return makeToolOk({{"composition_index", compIdx}, {"soloed_indices", indices}});
  }

  if (name == "layer_precompose") {
    const int compIdx = args.value("composition_index", -1);
    std::vector<int> indices;
    if (args.contains("element_indices") && args["element_indices"].is_array()) {
      for (const auto& idx : args["element_indices"]) {
        if (idx.is_number_integer()) indices.push_back(idx.get<int>());
      }
    }
    const std::string newName = args.value("name", std::string{});
    const auto res = iota::model::precomposeElements(proj, compIdx, indices, newName);
    if (res.newCompIdx < 0) {
      return makeToolErr("Failed to pre-compose selected element_indices.");
    }
    target.commitEdit("MCP Pre-Compose Layers");
    return makeToolOk({
        {"new_composition_index", res.newCompIdx},
        {"host_composition_index", res.hostLayerRef.compIdx},
        {"host_element_index", res.hostLayerRef.elemIdx},
    });
  }

  if (name == "layer_get_properties") {
    const int compIdx = args.value("composition_index", 0);
    const int elemIdx = args.value("element_index", -1);
    const bool includeDocs = args.value("include_docs", true);
    const bool includeHidden = args.value("include_hidden", false);

    std::string err;
    auto* comp = getCompositionOrErr(proj, compIdx, err);
    if (!comp) return makeToolErr(err);

    iota::model::ElementType ownerType = iota::model::ElementType::Composition;
    std::string dispName = comp->displayName;
    const iota::model::PropertyBag* bag = &comp->props;
    if (elemIdx >= 0) {
      auto* el = getElementOrErr(proj, compIdx, elemIdx, err);
      if (!el) return makeToolErr(err);
      ownerType = el->type;
      dispName = el->displayName;
      bag = &el->props;
    }

    nlohmann::json propsArr = nlohmann::json::array();
    for (const auto& item : bag->items()) {
      nlohmann::json pj = serializePropertyEntryJson(ownerType, item.key, item.entry, includeDocs, includeHidden);
      if (!pj.is_null()) {
        propsArr.push_back(std::move(pj));
      }
    }
    return makeToolOk({
        {"composition_index", compIdx},
        {"element_index", elemIdx},
        {"type", ownerType == iota::model::ElementType::Composition ? "Composition"
                                                                   : std::string(iota::model::elementTypeToString(ownerType))},
        {"name", dispName},
        {"properties", std::move(propsArr)},
    });
  }

  if (name == "layer_set_properties") {
    const int compIdx = args.value("composition_index", 0);
    const int elemIdx = args.value("element_index", -1);
    if (!args.contains("properties") || !args["properties"].is_object()) {
      return makeToolErr("layer_set_properties requires a 'properties' JSON object.");
    }
    std::string err;
    auto* comp = getCompositionOrErr(proj, compIdx, err);
    if (!comp) return makeToolErr(err);

    iota::model::ElementType ownerType = iota::model::ElementType::Composition;
    iota::model::PropertyBag* bag = &comp->props;
    if (elemIdx >= 0) {
      auto* el = getElementOrErr(proj, compIdx, elemIdx, err);
      if (!el) return makeToolErr(err);
      ownerType = el->type;
      bag = &el->props;
    }

    // Apply onto a temporary clone first so an error leaves the project atomically untouched
    iota::model::PropertyBag stagedBag = *bag;
    std::vector<std::string> updatedKeys;
    bool mayInvalidateAssets = false;
    for (auto it = args["properties"].begin(); it != args["properties"].end(); ++it) {
      iota::model::PropertyEntry* entry = nullptr;
      std::string leafKey;
      if (!resolveOrSeedProperty(stagedBag, ownerType, compIdx, it.key(), entry, leafKey, err)) {
        return makeToolErr(err);
      }
      if (!applyPropertyValue(ownerType, leafKey, *entry, it.value(), err)) {
        return makeToolErr(err);
      }
      if (leafKey == "customImage" || leafKey == "TargetImage" || leafKey == "targetImage" ||
          leafKey == "MaskImage" || leafKey == "typeFace" || leafKey == "emitterLayer") {
        mayInvalidateAssets = true;
      }
      updatedKeys.push_back(it.key());
    }

    *bag = std::move(stagedBag);
    const std::string undoLabel = args.value("undo_label", std::string("MCP Edit Properties"));
    target.commitEdit(undoLabel, mayInvalidateAssets, false);
    return makeToolOk({
        {"updated", true},
        {"composition_index", compIdx},
        {"element_index", elemIdx},
        {"updated_keys", std::move(updatedKeys)},
    });
  }

  if (name == "layer_set_keyframe") {
    const int compIdx = args.value("composition_index", -1);
    const int elemIdx = args.value("element_index", -1);
    const std::string propPath = args.value("property_path", std::string{});
    const double timeSec = args.value("time_sec", -1.0);
    if (!args.contains("value") || !args["value"].is_number() || !std::isfinite(timeSec) || timeSec < 0.0 ||
        propPath.empty()) {
      return makeToolErr("Invalid keyframe parameters (require valid indices, property_path, time_sec >= 0, value).");
    }
    const float value = static_cast<float>(args["value"].get<double>());
    if (!std::isfinite(value)) {
      return makeToolErr("Keyframe value must be finite.");
    }
    std::string err;
    if (!getElementOrErr(proj, compIdx, elemIdx, err)) {
      return makeToolErr(err);
    }

    iota::model::KeyframeTrack* track = nullptr;
    for (auto& t : proj.desktopTracks) {
      if (t.compositionIndex == compIdx && t.elementIndex == elemIdx && t.propertyPath == propPath) {
        track = &t;
        break;
      }
    }
    if (!track) {
      proj.desktopTracks.push_back({compIdx, elemIdx, propPath, {}});
      track = &proj.desktopTracks.back();
    }
    bool replaced = false;
    for (auto& kf : track->keyframes) {
      if (std::abs(kf.timeSec - timeSec) < 1e-4) {
        kf.value = value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      track->keyframes.push_back({timeSec, value});
    }
    std::sort(track->keyframes.begin(), track->keyframes.end(),
              [](const iota::model::Keyframe& a, const iota::model::Keyframe& b) {
                return a.timeSec < b.timeSec;
              });
    target.commitEdit("MCP Set Keyframe");
    return makeToolOk({
        {"composition_index", compIdx},
        {"element_index", elemIdx},
        {"property_path", propPath},
        {"keyframe_count", track->keyframes.size()},
    });
  }

  if (name == "layer_clear_keyframes") {
    const int compIdx = args.value("composition_index", -1);
    const int elemIdx = args.value("element_index", -1);
    const std::string propPath = args.value("property_path", std::string{});
    const size_t before = proj.desktopTracks.size();
    proj.desktopTracks.erase(
        std::remove_if(proj.desktopTracks.begin(), proj.desktopTracks.end(),
                       [&](const iota::model::KeyframeTrack& t) {
                         return t.compositionIndex == compIdx &&
                                t.elementIndex == elemIdx &&
                                t.propertyPath == propPath;
                       }),
        proj.desktopTracks.end());
    if (proj.desktopTracks.size() != before) {
      target.commitEdit("MCP Clear Keyframes");
    }
    return makeToolOk({{"cleared", proj.desktopTracks.size() != before}});
  }

  if (name == "docs_list_elements") {
    return makeToolOk({{"elements", buildAllDocsJson("iota://docs/elements")}});
  }

  if (name == "docs_get_element_schema") {
    const std::string typeStr = args.value("element_type", std::string{});
    const auto resolved = resolveElementOrCompTypeCI(typeStr);
    if (!resolved) {
      return makeToolErr("Unknown element_type '" + typeStr + "'.");
    }
    return makeToolOk(buildElementSchemaJson(*resolved));
  }

  if (name == "docs_search") {
    const std::string query = args.value("query", std::string{});
    if (query.empty()) {
      return makeToolErr("docs_search requires non-empty 'query'.");
    }

    nlohmann::json elemHits = nlohmann::json::array();
    nlohmann::json propHits = nlohmann::json::array();
    for (const auto& ed : iota::model::allElementDocs()) {
      const std::string typeName =
          (ed.type == iota::model::ElementType::Composition)
              ? "Composition"
              : std::string(iota::model::elementTypeToString(ed.type));
      if (containsCI(ed.displayName, query) || containsCI(ed.category, query) ||
          containsCI(ed.overview, query) || containsCI(ed.tips, query)) {
        elemHits.push_back({{"type", typeName}, {"display_name", ed.displayName}, {"category", ed.category}});
      }
      const iota::model::PropertyBag bag =
          (ed.type == iota::model::ElementType::Composition)
              ? iota::model::createDefaultComposition(0).props
              : iota::model::createDefaultElement(ed.type).props;
      for (const auto& item : bag.items()) {
        const auto* pdoc = iota::model::findPropertyDoc(ed.type, item.key);
        const std::string dname = iota::model::formatPropertyDisplayName(ed.type, item.key);
        const bool match = containsCI(item.key, query) || containsCI(dname, query) ||
                           (pdoc && (containsCI(pdoc->summary, query) || containsCI(pdoc->usage, query)));
        if (match && propHits.size() < 80) {
          propHits.push_back({
              {"element_type", typeName},
              {"key", item.key},
              {"display_name", dname},
              {"summary", pdoc ? std::string(pdoc->summary) : ""},
              {"usage", pdoc ? std::string(pdoc->usage) : ""},
          });
        }
      }
    }

    nlohmann::json mvarHits = nlohmann::json::array();
    for (const auto& m : iota::model::allMVarModeDocs()) {
      if (containsCI(m.mode, query) || containsCI(m.displayName, query) || containsCI(m.summary, query)) {
        mvarHits.push_back({{"mode", m.mode}, {"display_name", m.displayName}, {"summary", m.summary}});
      }
    }

    nlohmann::json tokenHits = nlohmann::json::array();
    for (const auto& tk : iota::model::allTextTokenDocs()) {
      if (containsCI(tk.token, query) || containsCI(tk.summary, query)) {
        tokenHits.push_back({{"token", tk.token}, {"summary", tk.summary}});
      }
    }

    return makeToolOk({
        {"query", query},
        {"elements", std::move(elemHits)},
        {"properties", std::move(propHits)},
        {"mvar_modes", std::move(mvarHits)},
        {"text_tokens", std::move(tokenHits)},
    });
  }

  if (name == "audio_load") {
    if (args.contains("file_path") && args["file_path"].is_string() &&
        !args["file_path"].get<std::string>().empty()) {
      const auto p = iota::core::pathFromUtf8(args["file_path"].get<std::string>());
      std::string err;
      if (!target.audio().loadFile(p, &err)) {
        return makeToolErr(err.empty() ? ("Failed to load audio: " + iota::core::pathToUtf8(p)) : err);
      }
      target.audio().pause();
      proj.metadata.audioPath = iota::core::pathToUtf8(p);
      return makeToolOk({
          {"loaded", true},
          {"source", "file"},
          {"path", proj.metadata.audioPath},
          {"duration_sec", target.audio().durationSeconds()},
          {"sample_rate", target.audio().sampleRate()},
          {"title", target.audio().currentTitle()},
          {"artist", target.audio().currentArtist()},
      });
    }

    const float freq = std::clamp(args.value("synthetic_freq_hz", 110.0f), 20.0f, 12000.0f);
    const double dur = std::clamp(args.value("synthetic_duration_sec", 6.0), 0.5, 120.0);
    std::string err;
    if (!target.audio().loadTrack(
            iota::audio::createSyntheticSineTrack(freq, dur, 44100, 0.5f, "MCP Synthetic Tone"),
            &err)) {
      return makeToolErr(err.empty() ? "Failed loading synthetic audio tone." : err);
    }
    target.audio().pause();
    return makeToolOk({
        {"loaded", true},
        {"source", "synthetic_tone"},
        {"freq_hz", freq},
        {"duration_sec", target.audio().durationSeconds()},
    });
  }

  if (name == "render_preview_frame") {
    const int w = std::clamp(args.value("width", target.previewWidth()), 64, 4096);
    const int h = std::clamp(args.value("height", target.previewHeight()), 64, 4096);
    const double tSec = std::max(0.0, args.value("time_sec", 0.5));
    const int preRoll = std::clamp(args.value("pre_roll_frames", 15), 0, 120);
    const bool fxaa = args.value("fxaa", true);
    const bool returnImage = args.value("return_image", true);
    const std::string savePngPath = args.value("save_png_path", std::string{});

    std::vector<uint8_t> pngBytes;
    iota::engine::RenderFrameStats stats;
    std::string err;
    if (!target.renderPreviewPng(w, h, tSec, preRoll, fxaa, pngBytes, stats, &err)) {
      return makeToolErr(err.empty() ? "Failed to render preview frame." : err);
    }

    if (!savePngPath.empty()) {
      const auto outPath = iota::core::pathFromUtf8(savePngPath);
      if (outPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(outPath.parent_path(), ec);
      }
      std::ofstream ofs(outPath, std::ios::binary | std::ios::trunc);
      if (!ofs || !ofs.write(reinterpret_cast<const char*>(pngBytes.data()),
                             static_cast<std::streamsize>(pngBytes.size()))) {
        return makeToolErr("Rendered frame successfully, but failed writing PNG to: " + savePngPath);
      }
    }

    const nlohmann::json summary = {
        {"width", w},
        {"height", h},
        {"time_sec", tSec},
        {"png_bytes", pngBytes.size()},
        {"saved_png_path", savePngPath},
        {"draw_calls", stats.drawCalls},
        {"compositions_rendered", stats.compositionsRendered},
        {"beat", stats.beat},
        {"peak_bar_index", stats.peakBarIndex},
        {"sound_keys", stats.soundKeysOutputs},
        {"active_particles", stats.activeParticles},
        {"hdr_active", stats.hdrPipelineActive},
        {"bloom_active", stats.bloomActive},
        {"last_error", stats.lastError},
    };

    nlohmann::json content = nlohmann::json::array({
        {{"type", "text"},
         {"text", summary.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)}},
    });
    if (returnImage) {
      content.push_back({
          {"type", "image"},
          {"data", base64Encode(pngBytes)},
          {"mimeType", "image/png"},
      });
    }
    return {{"content", std::move(content)}, {"isError", false}};
  }

  if (name == "export_list_encoders") {
    const auto caps = iota::export_video::probeHardwareEncodersSync();
    return makeToolOk({
        {"ffmpeg_available", caps.ffmpegAvailable},
        {"has_any_hardware", caps.hasAnyHardware()},
        {"summary", caps.summaryText()},
        {"encoders",
         {
             {"cpu_h264", caps.cpuH264},
             {"cpu_h265", caps.cpuH265},
             {"nvenc_h264", caps.nvencH264},
             {"nvenc_h265", caps.nvencH265},
             {"amf_h264", caps.amfH264},
             {"amf_h265", caps.amfH265},
             {"qsv_h264", caps.qsvH264},
             {"qsv_h265", caps.qsvH265},
         }},
    });
  }

  if (name == "export_video") {
    if (!args.contains("output_path") || !args["output_path"].is_string() ||
        args["output_path"].get<std::string>().empty()) {
      return makeToolErr("export_video requires a non-empty 'output_path'.");
    }

    iota::export_video::ExportSettings cfg;
    cfg.outputPath = iota::core::pathFromUtf8(args["output_path"].get<std::string>());
    if (isVizExtension(cfg.outputPath)) {
      return makeToolErr("output_path for export_video must be a video container such as .mp4, not .viz.");
    }
    cfg.width = std::clamp(args.value("width", target.previewWidth()), 64, 7680);
    cfg.height = std::clamp(args.value("height", target.previewHeight()), 64, 7680);
    cfg.fps = std::clamp(args.value("fps", target.targetFps()), 1, 120);
    cfg.videoBitrateKbps = std::clamp(args.value("bitrate_kbps", 12000), 500, 100000);
    cfg.fallbackToCpuOnError = args.value("fallback_to_cpu", true);
    cfg.startSec = std::max(0.0, args.value("start_sec", 0.0));
    cfg.endSec = std::max(0.0, args.value("end_sec", 0.0));
    cfg.preRollFrames = std::clamp(args.value("pre_roll_frames", 30), 0, 240);
    cfg.fxaa = args.value("fxaa", true);

    const std::string codecStr = args.value("codec", std::string("h264"));
    if (eqCI(codecStr, "h265") || eqCI(codecStr, "hevc")) {
      cfg.codec = iota::export_video::VideoCodec::H265;
    } else if (eqCI(codecStr, "h264") || eqCI(codecStr, "avc")) {
      cfg.codec = iota::export_video::VideoCodec::H264;
    } else {
      return makeToolErr("Invalid codec '" + codecStr + "'. Use 'h264' or 'h265'.");
    }

    const std::string backendStr = args.value("encoder_backend", std::string("auto"));
    if (eqCI(backendStr, "auto")) {
      cfg.encoderBackend = iota::export_video::VideoEncoderBackend::Auto;
    } else if (eqCI(backendStr, "nvenc") || eqCI(backendStr, "nvidia")) {
      cfg.encoderBackend = iota::export_video::VideoEncoderBackend::NvidiaNvenc;
    } else if (eqCI(backendStr, "amf") || eqCI(backendStr, "amd")) {
      cfg.encoderBackend = iota::export_video::VideoEncoderBackend::AmdAmf;
    } else if (eqCI(backendStr, "qsv") || eqCI(backendStr, "intel")) {
      cfg.encoderBackend = iota::export_video::VideoEncoderBackend::IntelQsv;
    } else if (eqCI(backendStr, "cpu") || eqCI(backendStr, "software") || eqCI(backendStr, "x264") || eqCI(backendStr, "x265")) {
      cfg.encoderBackend = iota::export_video::VideoEncoderBackend::CpuSoftware;
    } else {
      return makeToolErr("Invalid encoder_backend '" + backendStr + "'. Use auto, nvenc, amf, qsv, or cpu.");
    }

    const std::string presetStr = args.value("encoder_preset", std::string("balanced"));
    if (eqCI(presetStr, "fast")) {
      cfg.encoderPreset = iota::export_video::EncoderPreset::Fast;
    } else if (eqCI(presetStr, "balanced")) {
      cfg.encoderPreset = iota::export_video::EncoderPreset::Balanced;
    } else if (eqCI(presetStr, "quality")) {
      cfg.encoderPreset = iota::export_video::EncoderPreset::Quality;
    } else {
      return makeToolErr("Invalid encoder_preset '" + presetStr + "'. Use fast, balanced, or quality.");
    }

    std::string err;
    std::string encUsed;
    bool usedFallback = false;
    std::string fallbackNotice;
    if (!target.exportVideo(cfg, &err, &encUsed, &usedFallback, &fallbackNotice)) {
      return makeToolErr(err.empty() ? "Video export failed." : err);
    }

    std::error_code ec;
    const uint64_t outBytes = std::filesystem::exists(cfg.outputPath, ec)
                                  ? std::filesystem::file_size(cfg.outputPath, ec)
                                  : 0;
    return makeToolOk({
        {"exported", true},
        {"output_path", iota::core::pathToUtf8(cfg.outputPath)},
        {"bytes", outBytes},
        {"encoder_used", encUsed},
        {"used_cpu_fallback", usedFallback},
        {"fallback_notice", fallbackNotice},
    });
  }

  if (name == "studio_status") {
    const auto curPath = target.currentProjectPath();
    return makeToolOk({
        {"mode", target.isLiveStudio() ? "live_studio" : "headless"},
        {"live_bridge_connected", target.isLiveStudio()},
        {"requests_handled", liveRequestsHandled},
        {"current_project_path", iota::core::pathToUtf8(curPath)},
        {"is_mcp_working_copy", isMcpWorkingCopyPath(curPath)},
        {"preview_width", target.previewWidth()},
        {"preview_height", target.previewHeight()},
        {"target_fps", target.targetFps()},
        {"can_undo", target.canUndo()},
        {"can_redo", target.canRedo()},
        {"audio_loaded", target.audio().hasTrack()},
        {"audio_playing", target.audio().isPlaying()},
        {"audio_time_sec", target.audio().currentTimeSeconds()},
        {"audio_duration_sec", target.audio().durationSeconds()},
        {"audio_title", target.audio().currentTitle()},
        {"audio_path", target.audio().currentFilePath()},
        {"composition_count", proj.compositions.size()},
        {"local_asset_count", proj.localAssets.size()},
    });
  }

  if (name == "studio_playback") {
    const std::string action = args.value("action", std::string{});
    std::optional<double> seekSec;
    if (args.contains("seek_sec") && args["seek_sec"].is_number()) {
      seekSec = args["seek_sec"].get<double>();
    }
    std::string err;
    if (!target.controlPlayback(action, seekSec, &err)) {
      return makeToolErr(err);
    }
    return makeToolOk({
        {"playing", target.audio().isPlaying()},
        {"time_sec", target.audio().currentTimeSeconds()},
        {"duration_sec", target.audio().durationSeconds()},
    });
  }

  if (name == "studio_undo") {
    std::string label;
    if (!target.undo(&label)) {
      return makeToolErr("Nothing to undo.");
    }
    return makeToolOk({{"undone", true}, {"label", label}, {"can_undo", target.canUndo()}, {"can_redo", target.canRedo()}});
  }

  if (name == "studio_redo") {
    std::string label;
    if (!target.redo(&label)) {
      return makeToolErr("Nothing to redo.");
    }
    return makeToolOk({{"redone", true}, {"label", label}, {"can_undo", target.canUndo()}, {"can_redo", target.canRedo()}});
  }

  outFound = false;
  return nullptr;
}

}  // namespace iota::mcp::detail
