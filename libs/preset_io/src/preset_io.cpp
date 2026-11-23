#include "iota/preset_io/preset_io.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

#include "iota/core/zip_archive.hpp"

namespace iota::preset_io {

namespace {

using Json = nlohmann::json;

iota::model::PropertyValue jsonToPropertyValue(const Json& j) {
  if (j.is_boolean()) return j.get<bool>();
  if (j.is_number_integer()) return j.get<int64_t>();
  if (j.is_number_unsigned()) return static_cast<int64_t>(j.get<uint64_t>());
  if (j.is_number_float()) return j.get<double>();
  if (j.is_string()) return j.get<std::string>();
  return std::monostate{};
}

Json propertyValueToJson(const iota::model::PropertyValue& pv) {
  return std::visit(
      [](const auto& arg) -> Json {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) return nullptr;
        else return Json(arg);
      },
      pv);
}

iota::model::PropertyEntry parsePropertyEntry(const Json& j) {
  iota::model::PropertyEntry entry;
  if (!j.is_object()) {
    entry.value = jsonToPropertyValue(j);
    return entry;
  }

  if (auto it = j.find("v"); it != j.end()) entry.value = jsonToPropertyValue(*it);
  if (auto it = j.find("t"); it != j.end() && it->is_string()) entry.typeTag = it->get<std::string>();
  if (auto it = j.find("o"); it != j.end() && it->is_number()) entry.order = it->get<int>();
  if (auto it = j.find("tag"); it != j.end() && it->is_string()) entry.groupTag = it->get<std::string>();
  if (auto it = j.find("hint"); it != j.end() && it->is_string()) entry.hint = it->get<std::string>();

  for (auto it = j.begin(); it != j.end(); ++it) {
    const auto& k = it.key();
    if (k == "v" || k == "t" || k == "o" || k == "tag" || k == "hint") continue;
    if (it.value().is_object()) {
      entry.children.push_back({k, parsePropertyEntry(it.value())});
    } else {
      iota::model::PropertyEntry leaf;
      leaf.value = jsonToPropertyValue(it.value());
      entry.children.push_back({k, std::move(leaf)});
    }
  }
  return entry;
}

Json serializePropertyEntry(const iota::model::PropertyEntry& entry) {
  Json j = Json::object();
  if (!std::holds_alternative<std::monostate>(entry.value)) {
    j["v"] = propertyValueToJson(entry.value);
  } else {
    j["v"] = "";
  }
  if (!entry.typeTag.empty()) j["t"] = entry.typeTag;
  j["o"] = entry.order;
  if (!entry.groupTag.empty() || entry.typeTag == "ih") {
    j["tag"] = entry.groupTag;
  }
  if (entry.hint.has_value()) j["hint"] = *entry.hint;

  for (const auto& ch : entry.children) {
    j[ch.key] = serializePropertyEntry(ch.entry);
  }
  return j;
}

iota::model::Element parseElement(const Json& j) {
  iota::model::Element el;
  if (auto it = j.find("objType"); it != j.end() && it->is_string()) {
    el.typeName = it->get<std::string>();
  } else if (auto itV = j.find("v"); itV != j.end() && itV->is_string()) {
    el.typeName = itV->get<std::string>();
  }
  el.type = iota::model::elementTypeFromString(el.typeName);
  if (auto it = j.find("ver"); it != j.end() && it->is_string()) {
    el.version = it->get<std::string>();
  }
  if (auto it = j.find("_name"); it != j.end() && it->is_string()) {
    el.displayName = it->get<std::string>();
  } else {
    el.displayName = el.typeName;
  }

  for (auto it = j.begin(); it != j.end(); ++it) {
    const auto& k = it.key();
    if (k == "v" || k == "objType" || k == "ver" || k == "_name") continue;
    el.props.items().push_back({k, parsePropertyEntry(it.value())});
  }
  return el;
}

Json serializeElement(const iota::model::Element& el) {
  Json j = Json::object();
  const std::string tName = el.typeName.empty()
                                ? std::string(iota::model::elementTypeToString(el.type))
                                : el.typeName;
  j["v"] = tName;
  j["objType"] = tName;
  j["ver"] = el.version.empty() ? "10" : el.version;
  if (!el.displayName.empty()) {
    j["_name"] = el.displayName;
  }
  for (const auto& item : el.props.items()) {
    j[item.key] = serializePropertyEntry(item.entry);
  }
  return j;
}

iota::model::Composition parseComposition(const Json& j) {
  iota::model::Composition comp;
  if (auto it = j.find("ver"); it != j.end() && it->is_string()) {
    comp.version = it->get<std::string>();
  }
  if (auto it = j.find("_name"); it != j.end() && it->is_string()) {
    comp.displayName = it->get<std::string>();
  }
  if (auto it = j.find("elements"); it != j.end() && it->is_array()) {
    for (const auto& elJson : *it) {
      if (elJson.is_object()) comp.elements.push_back(parseElement(elJson));
    }
  }
  for (auto it = j.begin(); it != j.end(); ++it) {
    const auto& k = it.key();
    if (k == "v" || k == "objType" || k == "ver" || k == "_name" || k == "elements") continue;
    comp.props.items().push_back({k, parsePropertyEntry(it.value())});
  }
  return comp;
}

Json serializeComposition(const iota::model::Composition& comp) {
  Json j = Json::object();
  j["v"] = "Composition";
  j["objType"] = "Composition";
  j["ver"] = comp.version.empty() ? "10" : comp.version;
  j["_name"] = comp.displayName;

  Json elems = Json::array();
  for (const auto& el : comp.elements) {
    elems.push_back(serializeElement(el));
  }
  j["elements"] = std::move(elems);

  for (const auto& item : comp.props.items()) {
    j[item.key] = serializePropertyEntry(item.entry);
  }
  return j;
}

} // namespace

std::optional<iota::model::Project> parseProjectJsonString(
    std::string_view jsonText,
    std::string* outError) {
  Json root;
  try {
    root = Json::parse(jsonText.begin(), jsonText.end());
  } catch (const std::exception& ex) {
    if (outError) *outError = std::string("JSON parse error: ") + ex.what();
    return std::nullopt;
  }

  if (!root.is_object()) {
    if (outError) *outError = "Preset root JSON is not an object";
    return std::nullopt;
  }

  iota::model::Project proj;
  if (auto it = root.find("ver"); it != root.end() && it->is_string()) {
    proj.version = it->get<std::string>();
  }
  if (auto it = root.find("template"); it != root.end() && it->is_number_integer()) {
    proj.templateId = it->get<int>();
  }

  if (auto it = root.find("compositions"); it != root.end() && it->is_array()) {
    for (const auto& cj : *it) {
      if (cj.is_object()) proj.compositions.push_back(parseComposition(cj));
    }
  }

  if (auto it = root.find("_desktopExt"); it != root.end() && it->is_object()) {
    if (auto tracksIt = it->find("keyframeTracks"); tracksIt != it->end() && tracksIt->is_array()) {
      for (const auto& tj : *tracksIt) {
        iota::model::KeyframeTrack tr;
        tr.compositionIndex = tj.value("comp", 0);
        tr.elementIndex = tj.value("elem", 0);
        tr.propertyPath = tj.value("prop", "");
        if (auto kfsIt = tj.find("keys"); kfsIt != tj.end() && kfsIt->is_array()) {
          for (const auto& kj : *kfsIt) {
            tr.keyframes.push_back({kj.value("t", 0.0), kj.value("v", 0.0f)});
          }
        }
        proj.desktopTracks.push_back(std::move(tr));
      }
    }
  }

  return proj;
}

std::string serializeProjectToJsonString(const iota::model::Project& project, int indent) {
  Json root = Json::object();
  Json comps = Json::array();
  for (const auto& comp : project.compositions) {
    comps.push_back(serializeComposition(comp));
  }
  root["compositions"] = std::move(comps);
  root["ver"] = project.version.empty() ? "10" : project.version;
  root["template"] = project.templateId;
  root["objType"] = "Root";

  if (!project.desktopTracks.empty()) {
    Json tracks = Json::array();
    for (const auto& tr : project.desktopTracks) {
      Json keys = Json::array();
      for (const auto& k : tr.keyframes) {
        keys.push_back({{"t", k.timeSec}, {"v", k.value}});
      }
      tracks.push_back({
          {"comp", tr.compositionIndex},
          {"elem", tr.elementIndex},
          {"prop", tr.propertyPath},
          {"keys", std::move(keys)},
      });
    }
    root["_desktopExt"] = Json{{"keyframeTracks", std::move(tracks)}};
  }

  return indent >= 0 ? root.dump(indent) : root.dump();
}

std::optional<iota::model::Project> loadProjectFromMemory(
    std::span<const uint8_t> bytes,
    std::string* outError) {
  if (iota::core::isZipSignature(bytes)) {
    auto entriesOpt = iota::core::readZipFromMemory(bytes, outError);
    if (!entriesOpt) return std::nullopt;

    const iota::core::ZipEntryData* sceneEntry = nullptr;
    const iota::core::ZipEntryData* fallbackJson = nullptr;
    for (const auto& e : *entriesOpt) {
      std::string fname = std::filesystem::path(e.name).filename().string();
      if (fname == "scene.json") {
        sceneEntry = &e;
        break;
      }
      if (e.name.size() >= 5 && e.name.substr(e.name.size() - 5) == ".json") {
        fallbackJson = &e;
      }
    }
    if (!sceneEntry) sceneEntry = fallbackJson;
    if (!sceneEntry) {
      if (outError) *outError = ".viz archive does not contain scene.json or any .json file";
      return std::nullopt;
    }

    std::string_view jsonText(
        reinterpret_cast<const char*>(sceneEntry->bytes.data()),
        sceneEntry->bytes.size());
    auto projOpt = parseProjectJsonString(jsonText, outError);
    if (!projOpt) return std::nullopt;

    for (auto& e : *entriesOpt) {
      if (&e == sceneEntry) continue;
      std::string baseName = std::filesystem::path(e.name).filename().string();
      if (!baseName.empty()) {
        projOpt->localAssets.emplace(std::move(baseName), e.bytes);
      }
    }
    return projOpt;
  }

  std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return parseProjectJsonString(text, outError);
}

std::optional<iota::model::Project> loadProjectFromFile(
    const std::filesystem::path& path,
    std::string* outError) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    if (outError) *outError = "Cannot open preset file: " + path.string();
    return std::nullopt;
  }
  const auto sz = in.tellg();
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  if (sz > 0 && !in.read(reinterpret_cast<char*>(buf.data()), sz)) {
    if (outError) *outError = "Failed reading preset file bytes";
    return std::nullopt;
  }
  return loadProjectFromMemory(buf, outError);
}

bool saveProjectAsJson(
    const iota::model::Project& project,
    const std::filesystem::path& path,
    std::string* outError) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (outError) *outError = "Cannot open output JSON path: " + path.string();
    return false;
  }
  const std::string txt = serializeProjectToJsonString(project, 2);
  out.write(txt.data(), static_cast<std::streamsize>(txt.size()));
  if (!out.good()) {
    if (outError) *outError = "Failed writing JSON preset file";
    return false;
  }
  return true;
}

bool saveProjectAsViz(
    const iota::model::Project& project,
    const std::filesystem::path& path,
    std::string* outError) {
  std::vector<iota::core::ZipEntryData> entries;
  const std::string txt = serializeProjectToJsonString(project, 2);
  entries.push_back({
      "scene.json",
      std::vector<uint8_t>(txt.begin(), txt.end()),
  });
  for (const auto& [name, bytes] : project.localAssets) {
    entries.push_back({name, bytes});
  }
  return iota::core::writeZipToFile(path, entries, outError);
}

} // namespace iota::preset_io
