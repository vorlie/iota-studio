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

#include "iota/preset_io/preset_io.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>

#include "iota/core/utf8_path.hpp"
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

bool asciiEndsWithCaseInsensitive(std::string_view s, std::string_view suffix) noexcept {
  if (s.size() < suffix.size()) return false;
  const size_t offset = s.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[offset + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

Json serializeKeyframeTrack(const iota::model::KeyframeTrack& tr) {
  Json keys = Json::array();
  for (const auto& k : tr.keyframes) {
    keys.push_back({{"t", k.timeSec}, {"v", k.value}});
  }
  return {
      {"comp", tr.compositionIndex},
      {"elem", tr.elementIndex},
      {"prop", tr.propertyPath},
      {"keys", std::move(keys)},
  };
}

iota::model::KeyframeTrack parseKeyframeTrack(const Json& tj) {
  iota::model::KeyframeTrack tr;
  tr.compositionIndex = tj.value("comp", 0);
  tr.elementIndex = tj.value("elem", 0);
  tr.propertyPath = tj.value("prop", "");
  if (auto kfsIt = tj.find("keys"); kfsIt != tj.end() && kfsIt->is_array()) {
    for (const auto& kj : *kfsIt) {
      if (!kj.is_object()) continue;
      tr.keyframes.push_back({kj.value("t", 0.0), kj.value("v", 0.0f)});
    }
  }
  return tr;
}

bool hasNonDefaultMetadata(const iota::model::ProjectMetadata& m) noexcept {
  return m.name != "Untitled Project" || !m.notes.empty() || !m.audioPath.empty() ||
         m.previewWidth != 1280 || m.previewHeight != 720 || m.targetFps != 60 ||
         !m.assetFolders.empty() || !m.customFolders.empty() || !m.audioAssets.empty();
}

} // namespace

std::optional<iota::model::Project> parseProjectJsonString(
    std::string_view jsonText,
    std::string* outError) {
  const std::string_view trimmed = iota::core::stripUtf8Bom(jsonText);
  Json root;
  try {
    root = Json::parse(trimmed.begin(), trimmed.end());
  } catch (const std::exception& firstEx) {
    const std::string sanitized = iota::core::sanitizeUtf8(trimmed);
    if (sanitized != trimmed) {
      try {
        root = Json::parse(sanitized.begin(), sanitized.end());
      } catch (const std::exception& ex) {
        if (outError) *outError = std::string("JSON parse error: ") + ex.what();
        return std::nullopt;
      }
    } else {
      if (outError) *outError = std::string("JSON parse error: ") + firstEx.what();
      return std::nullopt;
    }
  }

  if (!root.is_object()) {
    if (outError) *outError = "Preset root JSON is not an object";
    return std::nullopt;
  }

  iota::model::Project proj;
  try {
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
          if (!tj.is_object()) continue;
          proj.desktopTracks.push_back(parseKeyframeTrack(tj));
        }
      }
      if (auto metaIt = it->find("projectMeta"); metaIt != it->end() && metaIt->is_object()) {
        auto& pm = proj.metadata;
        pm.name = metaIt->value("name", pm.name);
        pm.notes = metaIt->value("notes", pm.notes);
        pm.audioPath = metaIt->value("audioPath", pm.audioPath);
        pm.previewWidth = std::max(16, metaIt->value("width", pm.previewWidth));
        pm.previewHeight = std::max(16, metaIt->value("height", pm.previewHeight));
        pm.targetFps = std::clamp(metaIt->value("fps", pm.targetFps), 1, 240);
        if (auto afIt = metaIt->find("assetFolders"); afIt != metaIt->end() && afIt->is_object()) {
          for (auto fIt = afIt->begin(); fIt != afIt->end(); ++fIt) {
            if (fIt.value().is_string()) {
              pm.assetFolders[fIt.key()] = fIt.value().get<std::string>();
            }
          }
        }
        if (auto cfIt = metaIt->find("customFolders"); cfIt != metaIt->end() && cfIt->is_array()) {
          for (const auto& folderVal : *cfIt) {
            if (folderVal.is_string() && !folderVal.get_ref<const std::string&>().empty()) {
              pm.customFolders.push_back(folderVal.get<std::string>());
            }
          }
        }
        if (auto aaIt = metaIt->find("audioAssets"); aaIt != metaIt->end() && aaIt->is_object()) {
          for (auto aIt = aaIt->begin(); aIt != aaIt->end(); ++aIt) {
            if (aIt.value().is_string()) {
              pm.audioAssets[aIt.key()] = aIt.value().get<std::string>();
            }
          }
        }
      }
    }
  } catch (const std::exception& ex) {
    if (outError) *outError = std::string("Preset JSON structure error: ") + ex.what();
    return std::nullopt;
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

  const bool writeMeta = hasNonDefaultMetadata(project.metadata);
  if (!project.desktopTracks.empty() || writeMeta) {
    Json ext = Json::object();
    if (!project.desktopTracks.empty()) {
      Json tracks = Json::array();
      for (const auto& tr : project.desktopTracks) {
        tracks.push_back(serializeKeyframeTrack(tr));
      }
      ext["keyframeTracks"] = std::move(tracks);
    }
    if (writeMeta) {
      const auto& pm = project.metadata;
      Json meta = Json::object();
      meta["name"] = pm.name;
      if (!pm.notes.empty()) meta["notes"] = pm.notes;
      if (!pm.audioPath.empty()) meta["audioPath"] = pm.audioPath;
      meta["width"] = pm.previewWidth;
      meta["height"] = pm.previewHeight;
      meta["fps"] = pm.targetFps;
      if (!pm.assetFolders.empty()) meta["assetFolders"] = pm.assetFolders;
      if (!pm.customFolders.empty()) meta["customFolders"] = pm.customFolders;
      if (!pm.audioAssets.empty()) meta["audioAssets"] = pm.audioAssets;
      ext["projectMeta"] = std::move(meta);
    }
    root["_desktopExt"] = std::move(ext);
  }

  return root.dump(indent >= 0 ? indent : -1, ' ', false, Json::error_handler_t::replace);
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
      const std::string fname = iota::core::baseNameFromSlashPath(e.name);
      if (fname == "scene.json") {
        sceneEntry = &e;
        break;
      }
      if (asciiEndsWithCaseInsensitive(fname, ".json")) {
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
      std::string baseName = iota::core::baseNameFromSlashPath(e.name);
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
    if (outError) *outError = "Cannot open preset file: " + iota::core::pathToUtf8(path);
    return std::nullopt;
  }
  const auto sz = in.tellg();
  if (sz < 0) {
    if (outError) *outError = "Failed determining preset file size: " + iota::core::pathToUtf8(path);
    return std::nullopt;
  }
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
    if (outError) *outError = "Cannot open output JSON path: " + iota::core::pathToUtf8(path);
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

std::string serializeHierarchyClipboardToJson(const iota::model::HierarchyClipboard& clip) {
  if (clip.kind == iota::model::HierarchyClipboard::Kind::Empty) {
    return {};
  }
  Json root = Json::object();
  root["iotaClipboard"] = 1;

  if (clip.kind == iota::model::HierarchyClipboard::Kind::Elements) {
    root["kind"] = "elements";
    Json arr = Json::array();
    for (const auto& ce : clip.elements) {
      Json item = Json::object();
      item["element"] = serializeElement(ce.element);
      Json tracks = Json::array();
      for (const auto& tr : ce.tracks) {
        tracks.push_back(serializeKeyframeTrack(tr));
      }
      item["tracks"] = std::move(tracks);
      arr.push_back(std::move(item));
    }
    root["elements"] = std::move(arr);
  } else if (clip.kind == iota::model::HierarchyClipboard::Kind::Composition) {
    root["kind"] = "composition";
    root["composition"] = serializeComposition(clip.composition);
    Json tracks = Json::array();
    for (const auto& tr : clip.compTracks) {
      tracks.push_back(serializeKeyframeTrack(tr));
    }
    root["tracks"] = std::move(tracks);
  }

  return root.dump(2, ' ', false, Json::error_handler_t::replace);
}

std::optional<iota::model::HierarchyClipboard> parseHierarchyClipboardFromJson(
    std::string_view jsonText) {
  const std::string_view trimmed = iota::core::stripUtf8Bom(jsonText);
  size_t first = 0;
  while (first < trimmed.size() &&
         std::isspace(static_cast<unsigned char>(trimmed[first])) != 0) {
    ++first;
  }
  if (first >= trimmed.size() || trimmed[first] != '{') {
    return std::nullopt;
  }

  Json root;
  try {
    root = Json::parse(trimmed.begin() + static_cast<std::ptrdiff_t>(first), trimmed.end());
  } catch (...) {
    return std::nullopt;
  }

  if (!root.is_object()) return std::nullopt;
  auto verIt = root.find("iotaClipboard");
  if (verIt == root.end() || !verIt->is_number_integer() || verIt->get<int>() != 1) {
    return std::nullopt;
  }
  auto kindIt = root.find("kind");
  if (kindIt == root.end() || !kindIt->is_string()) {
    return std::nullopt;
  }

  const std::string kind = kindIt->get<std::string>();
  iota::model::HierarchyClipboard out;
  try {
    if (kind == "elements") {
      auto elemsIt = root.find("elements");
      if (elemsIt == root.end() || !elemsIt->is_array() || elemsIt->empty()) {
        return std::nullopt;
      }
      out.kind = iota::model::HierarchyClipboard::Kind::Elements;
      for (const auto& item : *elemsIt) {
        if (!item.is_object()) continue;
        auto elIt = item.find("element");
        if (elIt == item.end() || !elIt->is_object()) continue;
        iota::model::CopiedElement ce;
        ce.element = parseElement(*elIt);
        if (auto trIt = item.find("tracks"); trIt != item.end() && trIt->is_array()) {
          for (const auto& tj : *trIt) {
            if (tj.is_object()) ce.tracks.push_back(parseKeyframeTrack(tj));
          }
        }
        out.elements.push_back(std::move(ce));
      }
      if (out.elements.empty()) return std::nullopt;
      return out;
    }

    if (kind == "composition") {
      auto compIt = root.find("composition");
      if (compIt == root.end() || !compIt->is_object()) {
        return std::nullopt;
      }
      out.kind = iota::model::HierarchyClipboard::Kind::Composition;
      out.composition = parseComposition(*compIt);
      if (auto trIt = root.find("tracks"); trIt != root.end() && trIt->is_array()) {
        for (const auto& tj : *trIt) {
          if (tj.is_object()) out.compTracks.push_back(parseKeyframeTrack(tj));
        }
      }
      return out;
    }
  } catch (...) {
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace iota::preset_io
