#include "docs_panel.hpp"

#include "iota/model/measured_var.hpp"
#include "iota/model/property_docs.hpp"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iota::studio {

namespace {

bool icontains(std::string_view haystack, std::string_view needle) noexcept {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;
  for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
          std::tolower(static_cast<unsigned char>(needle[j]))) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

struct DocPropRow {
  std::string rawKey;
  std::string displayName;
  std::string summary;
  std::string usage;
  std::string unit;
  std::string rangeText;
  std::string groupTag;
  int order = 0;
  bool isChild = false;
  std::string parentDisplayName;
};

std::string formatSpecRange(const iota::model::ParsedTypeSpec& spec) {
  if (!spec.hasBounds) return {};
  char buf[64]{};
  std::snprintf(buf, sizeof(buf), "%.4g .. %.4g", spec.minVal, spec.maxVal);
  return buf;
}

void collectEntryDocsRecursive(iota::model::ElementType ownerType,
                               const std::string& rawKey,
                               const iota::model::PropertyEntry& entry,
                               const std::string& groupTag,
                               int baseOrder,
                               bool isChild,
                               const std::string& parentDisplay,
                               std::vector<DocPropRow>& out) {
  const auto spec = iota::model::parseTypeTag(entry.typeTag);
  if (spec.tag == iota::model::PropertyTypeTag::HiddenInt) {
    return;
  }
  const auto* doc = iota::model::findPropertyDoc(ownerType, rawKey);
  DocPropRow row;
  row.rawKey = rawKey;
  row.displayName = iota::model::formatPropertyDisplayName(ownerType, rawKey);
  if (isChild && !parentDisplay.empty()) {
    row.displayName = parentDisplay + " -> " + row.displayName;
  }
  row.summary = doc ? std::string(doc->summary) : std::string("Element property.");
  row.usage = doc ? std::string(doc->usage) : std::string{};
  row.unit = doc ? std::string(doc->unit) : std::string{};
  row.rangeText = formatSpecRange(spec);
  row.groupTag = groupTag;
  row.order = baseOrder;
  row.isChild = isChild;
  row.parentDisplayName = parentDisplay;
  out.push_back(std::move(row));

  const std::string thisParent = iota::model::formatPropertyDisplayName(ownerType, rawKey);
  for (const auto& ch : entry.children) {
    collectEntryDocsRecursive(ownerType, ch.key, ch.entry, groupTag, baseOrder, true, thisParent, out);
  }
}

std::vector<DocPropRow> buildElementPropertyCatalog(iota::model::ElementType type) {
  std::vector<DocPropRow> rows;
  iota::model::PropertyBag bag;
  if (type == iota::model::ElementType::Composition) {
    bag = iota::model::createDefaultComposition(0).props;
  } else {
    bag = iota::model::createDefaultElement(type).props;
  }
  for (const auto& item : bag.items()) {
    const std::string g = item.entry.groupTag.empty() ? "misc" : item.entry.groupTag;
    collectEntryDocsRecursive(type, item.key, item.entry, g, item.entry.order, false, {}, rows);
  }
  return rows;
}

void drawQuickStartPage() {
  ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f), "Iota Studio Quick Start & Workflow");
  ImGui::Separator();
  ImGui::TextWrapped(
      "Iota Studio is a hardware-accelerated OpenGL 4.5 visual synthesizer for building multi-layer, "
      "audio-reactive music visualizers and exporting deterministic MP4 videos.");
  ImGui::Spacing();

  if (ImGui::CollapsingHeader("1. Compositions & Layer Order", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Bullet();
    ImGui::TextWrapped("Composition 0 (Final Composition) produces the rendered frame, optionally using 16-bit HDR (RGBA16F), 6-mip Physical Bloom, and Filmic Tonemapping.");
    ImGui::Bullet();
    ImGui::TextWrapped("Secondary compositions (Composition 1, 2, ...) render offscreen first and can be referenced from Image, Blur, Motion Blur, Mirror, Camera Shake, or Particle LayerMask via 'composition:1', 'composition:2', etc.");
    ImGui::Bullet();
    ImGui::TextWrapped("Layers execute top-to-bottom within a composition: layers lower in the Hierarchy draw later (on top). Effects using 'composition:0' read everything rendered earlier in the same composition.");
  }

  if (ImGui::CollapsingHeader("2. Audio Analysis & Reactive Modulation", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Bullet();
    ImGui::TextWrapped("Audio Provider computes frequency spectrum bars (Spectrum2 DFT or Spectrum FFT), waveforms, and normalized Beat values. Frame-rate-independent smoothing makes previews match 60 FPS and 144 FPS identically.");
    ImGui::Bullet();
    ImGui::TextWrapped("Response Speed / Smooth & Envelope Speed polarity: HIGHER values always respond faster and snappier; LOWER values smooth motion more slowly.");
    ImGui::Bullet();
    ImGui::TextWrapped("Sound Keys lets you paint up to 3 frequency/amplitude boxes (R1, R2, R3) and bind them into any MVar property (Scale, Position, Shake Amplitude, Bloom Strength, etc.) or GLSL uniform u_soundKeys.");
  }

  if (ImGui::CollapsingHeader("3. Inspector Tips & Preset Formats", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Bullet();
    ImGui::TextWrapped("Hover any option name in the Inspector to view its full explanation, practical tips, numeric range, and internal JSON key.");
    ImGui::Bullet();
    ImGui::TextWrapped("Ctrl+Click any slider to type an exact value, or toggle Precise Inputs at the top of the Inspector.");
    ImGui::Bullet();
    ImGui::TextWrapped("FFmpeg (ffmpeg.exe & ffprobe.exe) is used for MP3/FLAC/AAC/OGG audio decoding and H.264/H.265 MP4 video export (uncompressed PCM WAV works without FFmpeg).");
    ImGui::Bullet();
    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
                       "Backup Warning: Saving over an external mobile .viz preset in Iota Studio updates its schema and makes that overwritten file incompatible with the legacy mobile player.");
  }
}

void drawElementReferencePage(iota::model::ElementType type) {
  const auto& edoc = iota::model::getElementDoc(type);
  ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f),
                     "%.*s",
                     static_cast<int>(edoc.displayName.size()),
                     edoc.displayName.data());
  ImGui::SameLine();
  ImGui::TextDisabled("[%.*s]",
                      static_cast<int>(edoc.category.size()),
                      edoc.category.data());
  ImGui::Separator();

  ImGui::TextWrapped("%.*s", static_cast<int>(edoc.overview.size()), edoc.overview.data());
  if (!edoc.tips.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.55f, 0.92f, 0.65f, 1.0f),
                       "Tip: %.*s",
                       static_cast<int>(edoc.tips.size()),
                       edoc.tips.data());
  }
  ImGui::Spacing();

  auto rows = buildElementPropertyCatalog(type);
  std::map<std::string, std::vector<DocPropRow>> byGroup;
  for (auto& r : rows) {
    byGroup[r.groupTag].push_back(std::move(r));
  }

  for (auto& [tag, vec] : byGroup) {
    std::stable_sort(vec.begin(), vec.end(), [](const DocPropRow& a, const DocPropRow& b) {
      if (a.order != b.order) return a.order < b.order;
      return !a.isChild && b.isChild;
    });
    const std::string gname = iota::model::formatGroupDisplayName(tag);
    if (ImGui::CollapsingHeader((gname + "##docgrp_" + tag).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      if (const auto* gdoc = iota::model::findGroupDoc(tag); gdoc && !gdoc->summary.empty()) {
        ImGui::TextDisabled("%.*s", static_cast<int>(gdoc->summary.size()), gdoc->summary.data());
      }
      const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
      if (ImGui::BeginTable(("##doc_tbl_" + tag).c_str(), 3, tflags)) {
        ImGui::TableSetupColumn("Option", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("Key / Unit", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Description & Use", ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableHeadersRow();

        for (const auto& r : vec) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextWrapped("%s", r.displayName.c_str());

          ImGui::TableSetColumnIndex(1);
          ImGui::TextDisabled("%s", r.rawKey.c_str());
          if (!r.unit.empty() || !r.rangeText.empty()) {
            std::string meta;
            if (!r.unit.empty()) meta += "[" + r.unit + "] ";
            if (!r.rangeText.empty()) meta += r.rangeText;
            ImGui::TextDisabled("%s", meta.c_str());
          }

          ImGui::TableSetColumnIndex(2);
          ImGui::TextWrapped("%s", r.summary.c_str());
          if (!r.usage.empty()) {
            ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "%s", r.usage.c_str());
          }
        }
        ImGui::EndTable();
      }
    }
  }
}

void drawAudioReactivityPage() {
  ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f), "Audio Reactivity: MVar Drivers & Sound Keys");
  ImGui::Separator();
  ImGui::TextWrapped(
      "MVar (Measured Variable) properties let you stack one or more audio, time, or shake signals onto "
      "positions, scales, rotations, blur radius, bloom intensity, shake amplitude, or particle emission.");
  ImGui::Spacing();

  const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
  if (ImGui::BeginTable("##mvar_docs_tbl", 4, tflags)) {
    ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthStretch, 0.24f);
    ImGui::TableSetupColumn("Signal Behavior", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("Scale A / X", ImGuiTableColumnFlags_WidthStretch, 0.17f);
    ImGui::TableSetupColumn("Param B / Y", ImGuiTableColumnFlags_WidthStretch, 0.17f);
    ImGui::TableHeadersRow();

    for (const auto& m : iota::model::allMVarModeDocs()) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextWrapped("%.*s", static_cast<int>(m.displayName.size()), m.displayName.data());
      ImGui::TextDisabled("%.*s", static_cast<int>(m.mode.size()), m.mode.data());

      ImGui::TableSetColumnIndex(1);
      ImGui::TextWrapped("%.*s", static_cast<int>(m.summary.size()), m.summary.data());

      ImGui::TableSetColumnIndex(2);
      ImGui::TextWrapped("%.*s", static_cast<int>(m.paramA.size()), m.paramA.data());

      ImGui::TableSetColumnIndex(3);
      ImGui::TextWrapped("%.*s", static_cast<int>(m.paramB.size()), m.paramB.data());
    }
    ImGui::EndTable();
  }
}

void drawTokensAndUrisPage(DocsPanelActions& actions) {
  ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f), "Dynamic Text Tokens & Resource URIs");
  ImGui::Separator();

  ImGui::TextUnformatted("Text Tokens (click Copy to paste into Text / StatText layers):");
  const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("##tokens_tbl", 3, tflags)) {
    ImGui::TableSetupColumn("Token", ImGuiTableColumnFlags_WidthStretch, 0.28f);
    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 64.0f);
    ImGui::TableHeadersRow();

    int rowId = 0;
    for (const auto& tk : iota::model::allTextTokenDocs()) {
      ImGui::PushID(rowId++);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(tk.token.data(), tk.token.data() + tk.token.size());
      ImGui::TableSetColumnIndex(1);
      ImGui::TextWrapped("%.*s", static_cast<int>(tk.summary.size()), tk.summary.data());
      ImGui::TableSetColumnIndex(2);
      if (ImGui::SmallButton("Copy")) {
        std::string clip(tk.token);
        ImGui::SetClipboardText(clip.c_str());
        actions.copiedSnippet = clip;
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Resource URIs (for Target Image / Custom Image / Emitter Layer / Font):");
  if (ImGui::BeginTable("##uris_tbl", 2, tflags)) {
    ImGui::TableSetupColumn("URI Pattern", ImGuiTableColumnFlags_WidthStretch, 0.30f);
    ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch, 0.70f);
    ImGui::TableHeadersRow();

    struct UriRow { const char* uri; const char* desc; };
    static constexpr UriRow kUris[] = {
        {"composition:0", "Samples everything rendered beneath the current layer inside the same composition (adjustment layer capture)."},
        {"composition:1", "Samples the offscreen render target produced by Composition 1 (use composition:2, composition:3, etc. for other comps)."},
        {"local:<filename>", "References an image or font embedded inside the .iota / .viz project (import via File... or Asset Browser)."},
        {"internalres:white / black / transparent", "Built-in solid 1x1 procedural textures."},
        {"internalres:vignette80 / rainbow128", "Built-in radial vignette mask and smooth 128-step horizontal rainbow ramp."},
        {"internalres:particle_blur01 / particle_circle_blur4 / lens_flare", "Built-in soft particle glow discs and procedural lens flare textures."},
        {"internalres:placeholderart5", "Default procedurally generated album cover placeholder texture."},
    };
    for (const auto& u : kUris) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextWrapped("%s", u.uri);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextWrapped("%s", u.desc);
    }
    ImGui::EndTable();
  }
}

void drawCustomShadersPage(DocsPanelActions& actions) {
  ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f), "Custom Shaders (GLSL 450 Core)");
  ImGui::Separator();
  ImGui::TextWrapped(
      "CustomShader layers render a textured quad using Target Image as u_texture. If you omit a '#version' "
      "directive, Iota Studio automatically prepends '#version 450 core', out vec4 iota_FragColor, in vec2 v_uv, "
      "and translates legacy GLSL ES tokens (texture2D -> texture, gl_FragColor -> iota_FragColor).");
  ImGui::Spacing();

  const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("##shader_uni_tbl", 2, tflags)) {
    ImGui::TableSetupColumn("Shader Variable", ImGuiTableColumnFlags_WidthStretch, 0.34f);
    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.66f);
    ImGui::TableHeadersRow();
    struct UniformRow { const char* name; const char* desc; };
    static constexpr UniformRow kUniforms[] = {
        {"uniform sampler2D u_texture;", "Input texture sampled from Target Image (e.g. composition:1 or composition:0)."},
        {"uniform float u_time;", "Elapsed timeline time in seconds."},
        {"uniform vec2 u_resolution;", "Output canvas resolution in pixels (width, height)."},
        {"uniform float u_beat;", "Normalized global beat value (0.0 .. 1.0) from Audio Provider 1."},
        {"uniform vec4 u_soundKeys;", "(SoundKeys R1, R2, R3, global beat) ready for multi-band shader reactivity."},
        {"uniform mat4 u_projView;", "Orthographic transform matrix used by the vertex stage."},
        {"in vec2 v_uv; in vec4 v_color;", "Interpolated UV coordinates (0..1) and vertex color tint."},
    };
    for (const auto& u : kUniforms) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(u.name);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextWrapped("%s", u.desc);
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  static constexpr const char* kStarterFragment =
      "uniform sampler2D u_texture;\n"
      "uniform float u_time;\n"
      "uniform float u_beat;\n"
      "uniform vec4 u_soundKeys;\n\n"
      "void main() {\n"
      "  float pulse = 0.015 * u_soundKeys.x;\n"
      "  vec2 uv = (v_uv - 0.5) * (1.0 - pulse) + 0.5;\n"
      "  vec4 col = texture(u_texture, uv);\n"
      "  col.rgb *= 1.0 + 0.35 * u_beat;\n"
      "  iota_FragColor = col * v_color;\n"
      "}\n";

  if (ImGui::Button("Copy Starter Fragment Shader")) {
    ImGui::SetClipboardText(kStarterFragment);
    actions.copiedSnippet = "Starter GLSL Fragment Shader";
  }
  ImGui::Spacing();
  if (ImGui::BeginChild("##shader_sample", ImVec2(0.0f, 180.0f), ImGuiChildFlags_Borders)) {
    ImGui::TextUnformatted(kStarterFragment);
  }
  ImGui::EndChild();
}

void drawShortcutsPage() {
  ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f), "Studio Keyboard Shortcuts");
  ImGui::Separator();

  const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("##shortcuts_tbl", 2, tflags)) {
    ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 0.34f);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.66f);
    ImGui::TableHeadersRow();
    struct ShortcutRow { const char* keys; const char* action; };
    static constexpr ShortcutRow kShortcuts[] = {
        {"F1", "Toggle Built-in Documentation panel"},
        {"Space", "Play / Pause audio playback"},
        {"Ctrl + N", "Open Project Manager modal"},
        {"Ctrl + O", "Open Project / Preset (.iota / .viz / .json)"},
        {"Ctrl + S", "Save current project"},
        {"Ctrl + Shift + S", "Save Project As (.iota)"},
        {"Ctrl + Z", "Undo last edit"},
        {"Ctrl + Y / Ctrl + Shift + Z", "Redo undone edit"},
        {"Ctrl + C / Ctrl + X / Ctrl + V", "Copy / Cut / Paste selected layers or composition"},
        {"Ctrl + D", "Duplicate selected layers or composition"},
        {"Ctrl + A (Hierarchy focused)", "Select all layers in active composition"},
        {"Ctrl + Up / Down", "Move selected layers up or down one slot"},
        {"Ctrl + Shift + Up / Down", "Send selected layers to top or bottom of composition"},
        {"F2", "Rename selected layer or composition inline"},
        {"Delete", "Delete selected layers or secondary composition"},
        {"Ctrl + Click (Slider)", "Type an exact numeric value directly into any slider"},
    };
    for (const auto& s : kShortcuts) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(s.keys);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextWrapped("%s", s.action);
    }
    ImGui::EndTable();
  }
}

void drawSearchResultsPage(std::string_view query, DocsPanelState& state, DocsPanelActions& actions) {
  ImGui::TextColored(ImVec4(0.65f, 0.85f, 1.0f, 1.0f),
                     "Search Results for \"%.*s\"",
                     static_cast<int>(query.size()),
                     query.data());
  ImGui::Separator();

  int hitCount = 0;

  // Matching Elements & Compositions
  std::vector<iota::model::ElementDoc> elemHits;
  for (const auto& ed : iota::model::allElementDocs()) {
    if (icontains(ed.displayName, query) || icontains(ed.category, query) ||
        icontains(ed.overview, query) || icontains(ed.tips, query)) {
      elemHits.push_back(ed);
    }
  }
  if (!elemHits.empty()) {
    ImGui::TextDisabled("Elements & Compositions (%zu)", elemHits.size());
    for (const auto& ed : elemHits) {
      ImGui::PushID(static_cast<int>(ed.type) + 100);
      if (ImGui::SmallButton("Open")) {
        state.openElement(ed.type);
      }
      ImGui::SameLine();
      ImGui::Text("%.*s [%.*s]",
                  static_cast<int>(ed.displayName.size()),
                  ed.displayName.data(),
                  static_cast<int>(ed.category.size()),
                  ed.category.data());
      ImGui::PopID();
      ++hitCount;
    }
    ImGui::Spacing();
  }

  // Matching Options across all Elements & Composition
  struct SearchPropHit {
    iota::model::ElementType elemType;
    std::string elemName;
    DocPropRow row;
  };
  std::vector<SearchPropHit> propHits;
  for (const auto& ed : iota::model::allElementDocs()) {
    for (auto& r : buildElementPropertyCatalog(ed.type)) {
      if (icontains(r.displayName, query) || icontains(r.rawKey, query) ||
          icontains(r.summary, query) || icontains(r.usage, query)) {
        propHits.push_back({ed.type, std::string(ed.displayName), std::move(r)});
      }
    }
  }
  if (!propHits.empty()) {
    ImGui::TextDisabled("Matching Options (%zu)", propHits.size());
    const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##docs_search_props", 4, tflags)) {
      ImGui::TableSetupColumn("Element", ImGuiTableColumnFlags_WidthStretch, 0.18f);
      ImGui::TableSetupColumn("Option", ImGuiTableColumnFlags_WidthStretch, 0.24f);
      ImGui::TableSetupColumn("Internal Key", ImGuiTableColumnFlags_WidthStretch, 0.18f);
      ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 0.40f);
      ImGui::TableHeadersRow();

      int pid = 0;
      for (const auto& h : propHits) {
        if (pid >= 120) break;
        ImGui::PushID(pid++);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::SmallButton(h.elemName.c_str())) {
          state.openElement(h.elemType);
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TextWrapped("%s", h.row.displayName.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("%s", h.row.rawKey.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextWrapped("%s", h.row.summary.c_str());
        ImGui::PopID();
        ++hitCount;
      }
      ImGui::EndTable();
    }
    ImGui::Spacing();
  }

  // Matching MVar modes
  std::vector<iota::model::MVarModeDoc> mvarHits;
  for (const auto& m : iota::model::allMVarModeDocs()) {
    if (icontains(m.mode, query) || icontains(m.displayName, query) ||
        icontains(m.summary, query) || icontains(m.paramA, query) || icontains(m.paramB, query)) {
      mvarHits.push_back(m);
    }
  }
  if (!mvarHits.empty()) {
    ImGui::TextDisabled("MVar Signal Modes (%zu)", mvarHits.size());
    for (const auto& m : mvarHits) {
      ImGui::BulletText("%.*s (%.*s): %.*s",
                        static_cast<int>(m.displayName.size()),
                        m.displayName.data(),
                        static_cast<int>(m.mode.size()),
                        m.mode.data(),
                        static_cast<int>(m.summary.size()),
                        m.summary.data());
      ++hitCount;
    }
    ImGui::Spacing();
  }

  // Matching Text Tokens
  std::vector<iota::model::TextTokenDoc> tokenHits;
  for (const auto& tk : iota::model::allTextTokenDocs()) {
    if (icontains(tk.token, query) || icontains(tk.summary, query)) {
      tokenHits.push_back(tk);
    }
  }
  if (!tokenHits.empty()) {
    ImGui::TextDisabled("Text Tokens (%zu)", tokenHits.size());
    int tid = 0;
    for (const auto& tk : tokenHits) {
      ImGui::PushID(tid++);
      if (ImGui::SmallButton("Copy")) {
        std::string s(tk.token);
        ImGui::SetClipboardText(s.c_str());
        actions.copiedSnippet = s;
      }
      ImGui::SameLine();
      ImGui::Text("%.*s - %.*s",
                  static_cast<int>(tk.token.size()),
                  tk.token.data(),
                  static_cast<int>(tk.summary.size()),
                  tk.summary.data());
      ImGui::PopID();
      ++hitCount;
    }
  }

  if (hitCount == 0) {
    ImGui::TextDisabled("No matching documentation topics or options found.");
  }
}

}  // namespace

void renderDocsPanel(bool& showPanel, DocsPanelState& state, DocsPanelActions& actions) {
  if (!showPanel) {
    return;
  }

  if (state.focusRequested) {
    ImGui::SetNextWindowFocus();
    state.focusRequested = false;
  }
  ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Documentation", &showPanel)) {
    ImGui::End();
    return;
  }

  // Top Search Bar
  ImGui::SetNextItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x - 68.0f));
  ImGui::InputTextWithHint("##docs_search",
                           "Search options, JSON keys, MVar modes, or text tokens...",
                           state.searchBuf,
                           sizeof(state.searchBuf));
  ImGui::SameLine();
  if (ImGui::Button("Clear", ImVec2(60.0f, 0.0f))) {
    state.searchBuf[0] = '\0';
  }
  ImGui::Separator();

  const std::string_view searchQuery(state.searchBuf);

  // Left navigation sidebar
  if (ImGui::BeginChild("##docs_nav", ImVec2(210.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
    auto selectTopic = [&](const char* label, DocsTopic topic) {
      const bool sel = searchQuery.empty() && state.activeTopic == topic;
      if (ImGui::Selectable(label, sel)) {
        state.searchBuf[0] = '\0';
        state.activeTopic = topic;
      }
    };

    selectTopic("Quick Start & Workflow", DocsTopic::QuickStart);
    selectTopic("Audio Reactivity (MVar)", DocsTopic::AudioReactivity);
    selectTopic("Text Tokens & URIs", DocsTopic::TextTokensAndUris);
    selectTopic("Custom Shaders (GLSL)", DocsTopic::CustomShaders);
    selectTopic("Keyboard Shortcuts", DocsTopic::Shortcuts);
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Elements & Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
      for (const auto& ed : iota::model::allElementDocs()) {
        const bool sel = searchQuery.empty() &&
                         state.activeTopic == DocsTopic::ElementReference &&
                         state.activeElementType == ed.type;
        std::string itemLabel(ed.displayName);
        if (ImGui::Selectable(itemLabel.c_str(), sel)) {
          state.searchBuf[0] = '\0';
          state.activeTopic = DocsTopic::ElementReference;
          state.activeElementType = ed.type;
        }
      }
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  // Right scrollable content pane
  if (ImGui::BeginChild("##docs_content", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
    if (!searchQuery.empty()) {
      drawSearchResultsPage(searchQuery, state, actions);
    } else {
      switch (state.activeTopic) {
        case DocsTopic::QuickStart:
          drawQuickStartPage();
          break;
        case DocsTopic::ElementReference:
          drawElementReferencePage(state.activeElementType);
          break;
        case DocsTopic::AudioReactivity:
          drawAudioReactivityPage();
          break;
        case DocsTopic::TextTokensAndUris:
          drawTokensAndUrisPage(actions);
          break;
        case DocsTopic::CustomShaders:
          drawCustomShadersPage(actions);
          break;
        case DocsTopic::Shortcuts:
          drawShortcutsPage();
          break;
      }
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

}  // namespace iota::studio
