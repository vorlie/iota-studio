#include "sound_keys_panel.hpp"

#include "iota/dsp/sound_keys.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace iota::studio {

namespace {

struct RangePalette {
  const char* shortLabel;
  ImU32 solid;
  ImU32 fill;
  ImU32 activeFill;
  ImVec4 uiColor;
};

constexpr std::array<RangePalette, 3> kRangeColors = {{
    {"R1", IM_COL32(60, 210, 255, 255), IM_COL32(60, 210, 255, 28), IM_COL32(60, 210, 255, 68),
     ImVec4(0.24f, 0.82f, 1.00f, 1.0f)},
    {"R2", IM_COL32(80, 235, 145, 255), IM_COL32(80, 235, 145, 28), IM_COL32(80, 235, 145, 68),
     ImVec4(0.31f, 0.92f, 0.57f, 1.0f)},
    {"R3", IM_COL32(255, 185, 65, 255), IM_COL32(255, 185, 65, 28), IM_COL32(255, 185, 65, 68),
     ImVec4(1.00f, 0.73f, 0.25f, 1.0f)},
}};

struct CanvasDragState {
  ImGuiID activeId = 0;
  float startU = 0.0f;
  float startAmpFrac = 0.0f;
  float dragCeiling = 100.0f;
  int rangeIdx = 0;
};

std::string formatHzShort(float hz) {
  char buf[32]{};
  if (hz >= 1000.0f) {
    std::snprintf(buf, sizeof(buf), "%.1fk", hz / 1000.0f);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f", hz);
  }
  return buf;
}

std::string formatRangeFreqLabel(float f0, float f1, const std::vector<float>& freqsHz) {
  if (freqsHz.size() >= 2) {
    const int n = static_cast<int>(freqsHz.size());
    const int i0 = std::clamp(static_cast<int>(std::floor(f0 * static_cast<float>(n - 1) + 0.5f)), 0, n - 1);
    const int i1 = std::clamp(static_cast<int>(std::floor(f1 * static_cast<float>(n - 1) + 0.5f)), i0, n - 1);
    return formatHzShort(freqsHz[static_cast<size_t>(i0)]) + "-" +
           formatHzShort(freqsHz[static_cast<size_t>(i1)]) + "Hz";
  }
  char buf[32]{};
  std::snprintf(buf, sizeof(buf), "%.0f-%.0f%%", f0 * 100.0f, f1 * 100.0f);
  return buf;
}

std::string rangeKey(int idx, const char* suffix) {
  return "range" + std::to_string(idx + 1) + suffix;
}

std::string rangeGroup(int idx) {
  return std::to_string(idx + 1) + "_range" + std::to_string(idx + 1);
}

}  // namespace

void renderSoundKeysCanvasAndMeters(iota::model::Element& soundKeysEl,
                                    const iota::engine::RenderFrameStats& stats,
                                    int& activeRangeIdx,
                                    InspectorActions& actions,
                                    float canvasHeight) {
  activeRangeIdx = std::clamp(activeRangeIdx, 0, 2);
  auto& p = soundKeysEl.props;
  const bool autoScale = p.getBool("autoScaleCeiling", true);
  const float displayCeiling = std::clamp(p.getFloat("displayCeiling", 50.0f), 5.0f, 10000.0f);
  const auto cfg = iota::dsp::soundKeysConfigFromElement(soundKeysEl);

  float canvasCeiling = displayCeiling;
  if (autoScale) {
    canvasCeiling = std::max(canvasCeiling, stats.soundKeysCeiling);
    for (const auto& rc : cfg.ranges) {
      if (rc.enabled) {
        canvasCeiling = std::max(canvasCeiling, rc.maxAmplitude * 1.05f);
      }
    }
  }

  ImGui::PushID(&soundKeysEl);

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Drag Range:");
  for (int r = 0; r < 3; ++r) {
    ImGui::SameLine();
    const bool isActive = (activeRangeIdx == r);
    const auto& pal = kRangeColors[static_cast<size_t>(r)];
    ImVec4 btnCol = pal.uiColor;
    btnCol.w = isActive ? 0.90f : 0.28f;
    ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(pal.uiColor.x, pal.uiColor.y, pal.uiColor.z, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Text, isActive ? ImVec4(0.05f, 0.06f, 0.08f, 1.0f) : ImVec4(0.95f, 0.97f, 1.0f, 1.0f));
    if (ImGui::SmallButton(pal.shortLabel)) {
      activeRangeIdx = r;
    }
    ImGui::PopStyleColor(3);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Full")) {
    const int r = activeRangeIdx;
    const std::string grp = rangeGroup(r);
    const float ampHi = std::max(10.0f, canvasCeiling);
    p.setBool(rangeKey(r, "Enabled"), true, grp, "b");
    p.setFloat(rangeKey(r, "MinFreq"), 0.0f, 0.0f, 1.0f, grp);
    p.setFloat(rangeKey(r, "MaxFreq"), 1.0f, 0.0f, 1.0f, grp);
    p.setFloat(rangeKey(r, "MinAmp"), 0.0f, 0.0f, std::max(1000.0f, ampHi), grp);
    p.setFloat(rangeKey(r, "MaxAmp"), ampHi, 1.0f, std::max(2000.0f, ampHi), grp);
    actions.modified = true;
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Set Full Spectrum Range";
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Expand active range to cover full frequency & amplitude spectrum");
  }

  const float availW = std::max(80.0f, ImGui::GetContentRegionAvail().x);
  const float h = std::max(72.0f, canvasHeight);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + availW, p0.y + h);

  ImGui::InvisibleButton("##sk_canvas", ImVec2(availW, h));
  const ImGuiID canvasId = ImGui::GetItemID();
  static CanvasDragState sDrag;

  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const float hoverU = std::clamp((mouse.x - p0.x) / availW, 0.0f, 1.0f);
  const float hoverAmpFrac = std::clamp((p1.y - mouse.y) / h, 0.0f, 1.0f);

  if (ImGui::IsItemActivated()) {
    sDrag.activeId = canvasId;
    sDrag.startU = hoverU;
    sDrag.startAmpFrac = hoverAmpFrac;
    sDrag.dragCeiling = canvasCeiling;
    sDrag.rangeIdx = activeRangeIdx;
  }

  if (sDrag.activeId == canvasId) {
    canvasCeiling = sDrag.dragCeiling;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      float f0 = std::min(sDrag.startU, hoverU);
      float f1 = std::max(sDrag.startU, hoverU);
      if (f1 - f0 < 0.015f) {
        f1 = std::min(1.0f, f0 + 0.04f);
        f0 = std::max(0.0f, f1 - 0.04f);
      }
      float a0 = std::min(sDrag.startAmpFrac, hoverAmpFrac) * canvasCeiling;
      float a1 = std::max(sDrag.startAmpFrac, hoverAmpFrac) * canvasCeiling;
      if (a1 - a0 < 2.0f) {
        a1 = std::min(canvasCeiling, a0 + canvasCeiling * 0.25f);
      }
      const int r = sDrag.rangeIdx;
      const std::string grp = rangeGroup(r);
      p.setBool(rangeKey(r, "Enabled"), true, grp, "b");
      p.setFloat(rangeKey(r, "MinFreq"), f0, 0.0f, 1.0f, grp);
      p.setFloat(rangeKey(r, "MaxFreq"), f1, 0.0f, 1.0f, grp);
      p.setFloat(rangeKey(r, "MinAmp"), a0, 0.0f, std::max(1000.0f, canvasCeiling), grp);
      p.setFloat(rangeKey(r, "MaxAmp"), a1, 1.0f, std::max(2000.0f, canvasCeiling), grp);
      actions.modified = true;
    } else {
      sDrag.activeId = 0;
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Adjust Sound Keys Range " + std::to_string(sDrag.rangeIdx + 1);
    }
  }

  if (ImGui::IsItemHovered() && sDrag.activeId == 0) {
    std::string freqTxt = formatRangeFreqLabel(hoverU, hoverU, stats.soundKeysBarFreqsHz);
    ImGui::SetTooltip("Cursor: %s | Amp: %.1f\nClick & drag to set %s range",
                      freqTxt.c_str(),
                      hoverAmpFrac * canvasCeiling,
                      kRangeColors[static_cast<size_t>(activeRangeIdx)].shortLabel);
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(15, 19, 27, 255), 3.0f);
  for (int q = 1; q <= 3; ++q) {
    const float y = p0.y + (static_cast<float>(q) * 0.25f) * h;
    dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(255, 255, 255, 20));
  }

  const auto& bars = stats.soundKeysSpectrum;
  const int nBars = static_cast<int>(bars.size());
  if (nBars > 0) {
    const float barSpan = availW / static_cast<float>(nBars);
    const auto& actRange = cfg.ranges[static_cast<size_t>(activeRangeIdx)];
    for (int i = 0; i < nBars; ++i) {
      const float x0 = p0.x + static_cast<float>(i) * barSpan;
      const float x1 = std::max(x0 + 1.0f, p0.x + static_cast<float>(i + 1) * barSpan - 1.0f);
      const float normU = (nBars > 1) ? (static_cast<float>(i) / static_cast<float>(nBars - 1)) : 0.0f;
      const float magFrac = std::clamp(bars[static_cast<size_t>(i)] / canvasCeiling, 0.0f, 1.0f);
      const float barTop = p1.y - magFrac * h;
      const bool inActive =
          actRange.enabled && normU >= actRange.minFreq && normU <= actRange.maxFreq;
      const ImU32 barCol = inActive ? IM_COL32(155, 210, 255, 225) : IM_COL32(85, 115, 155, 165);
      if (barTop < p1.y) {
        dl->AddRectFilled(ImVec2(x0, barTop), ImVec2(x1, p1.y), barCol);
      }
    }
  }

  // Re-read config after any drag updates so box renders cleanly on the same frame
  const auto updatedCfg = iota::dsp::soundKeysConfigFromElement(soundKeysEl);
  for (int r = 0; r < 3; ++r) {
    const auto& rc = updatedCfg.ranges[static_cast<size_t>(r)];
    const auto& pal = kRangeColors[static_cast<size_t>(r)];
    const bool isActive = (r == activeRangeIdx);

    const float xL = p0.x + rc.minFreq * availW;
    const float xR = std::max(xL + 3.0f, p0.x + rc.maxFreq * availW);
    const float a0 = std::clamp(rc.minAmplitude / canvasCeiling, 0.0f, 1.0f);
    const float a1 = std::clamp(rc.maxAmplitude / canvasCeiling, 0.0f, 1.0f);
    const float yB = p1.y - a0 * h;
    const float yT = std::min(yB - 3.0f, p1.y - a1 * h);

    if (rc.enabled) {
      dl->AddRectFilled(ImVec2(xL, yT), ImVec2(xR, yB), isActive ? pal.activeFill : pal.fill, 2.0f);
    }
    const ImU32 borderCol = rc.enabled ? pal.solid : IM_COL32(140, 145, 155, 90);
    dl->AddRect(ImVec2(xL, yT), ImVec2(xR, yB), borderCol, 2.0f, 0, isActive ? 2.0f : 1.0f);
    if ((xR - xL) >= 18.0f && (yB - yT) >= 14.0f) {
      dl->AddText(ImVec2(xL + 3.0f, yT + 1.0f), borderCol, pal.shortLabel);
    }
  }
  {
    char statBuf[64]{};
    std::snprintf(statBuf, sizeof(statBuf), "Peak %.0f / Ceil %.0f", stats.soundKeysPeak, canvasCeiling);
    const ImVec2 txtSz = ImGui::CalcTextSize(statBuf);
    if (availW > txtSz.x + 12.0f) {
      dl->AddText(ImVec2(p1.x - txtSz.x - 6.0f, p0.y + 3.0f), IM_COL32(205, 215, 232, 200), statBuf);
    }
  }
  dl->AddRect(p0, p1, IM_COL32(70, 82, 102, 220), 3.0f);

  // Per-range meters and quick toggles
  for (int r = 0; r < 3; ++r) {
    const auto& rc = updatedCfg.ranges[static_cast<size_t>(r)];
    const auto& pal = kRangeColors[static_cast<size_t>(r)];
    const float outVal = stats.soundKeysOutputs[static_cast<size_t>(r)];
    const float span = rc.outputMax - rc.outputMin;
    const float meterFrac =
        (std::abs(span) > 1e-5f) ? std::clamp((outVal - rc.outputMin) / span, 0.0f, 1.0f) : 0.0f;

    ImGui::PushID(r);
    bool en = rc.enabled;
    if (ImGui::Checkbox("##en", &en)) {
      p.setBool(rangeKey(r, "Enabled"), en, rangeGroup(r), "b");
      actions.modified = true;
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Toggle Sound Keys Range";
    }
    ImGui::SameLine();

    ImVec4 lblCol = pal.uiColor;
    lblCol.w = (r == activeRangeIdx) ? 0.85f : 0.25f;
    ImGui::PushStyleColor(ImGuiCol_Button, lblCol);
    ImGui::PushStyleColor(ImGuiCol_Text, (r == activeRangeIdx) ? ImVec4(0.05f, 0.06f, 0.08f, 1.0f)
                                                               : ImVec4(0.95f, 0.97f, 1.0f, 1.0f));
    if (ImGui::SmallButton(pal.shortLabel)) {
      activeRangeIdx = r;
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();

    const std::string hzLabel = formatRangeFreqLabel(rc.minFreq, rc.maxFreq, stats.soundKeysBarFreqsHz);
    char valBuf[64]{};
    std::snprintf(valBuf, sizeof(valBuf), "%s | %.2f", hzLabel.c_str(), outVal);
    const float txtW = ImGui::CalcTextSize(valBuf).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const float meterW = std::max(24.0f, ImGui::GetContentRegionAvail().x - txtW);

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, pal.uiColor);
    ImGui::ProgressBar(meterFrac, ImVec2(meterW, 0.0f), "");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(valBuf);
    ImGui::PopID();
  }

  // Quick controls for currently selected range
  const int ar = activeRangeIdx;
  const auto& arc = updatedCfg.ranges[static_cast<size_t>(ar)];
  const std::string arGrp = rangeGroup(ar);
  const float colAvail = ImGui::GetContentRegionAvail().x;
  const float halfW = std::max(55.0f, (colAvail - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

  ImGui::SetNextItemWidth(halfW);
  const std::string curMode = std::string(iota::dsp::soundKeysRangeModeToString(arc.rangeMode));
  if (ImGui::BeginCombo("##sk_mode", curMode.c_str())) {
    for (const char* m : {"Peak", "Average", "OnOff"}) {
      if (ImGui::Selectable(m, curMode == m)) {
        p.setString(rangeKey(ar, "Mode"), m, "sel Average Peak OnOff", arGrp);
        actions.modified = true;
        actions.undoSnapshotNeeded = true;
        actions.editLabel = "Change Sound Keys Mode";
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(halfW);
  const std::string curFalloff = std::string(iota::dsp::soundKeysFalloffModeToString(arc.falloffMode));
  if (ImGui::BeginCombo("##sk_falloff", curFalloff.c_str())) {
    for (const char* fm : {"Exponential", "Linear", "None", "Integrate"}) {
      if (ImGui::Selectable(fm, curFalloff == fm)) {
        p.setString(rangeKey(ar, "Falloff"), fm, "sel None Linear Exponential Integrate", arGrp);
        actions.modified = true;
        actions.undoSnapshotNeeded = true;
        actions.editLabel = "Change Sound Keys Falloff";
      }
    }
    ImGui::EndCombo();
  }

  float falloffSec = arc.falloffTimeSec;
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::SliderFloat("##sk_falloff_sec", &falloffSec, 0.01f, 3.0f, "Falloff %.2fs")) {
    p.setFloat(rangeKey(ar, "FalloffSec"), falloffSec, 0.01f, 5.0f, arGrp);
    actions.modified = true;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Adjust Sound Keys Falloff";
  }

  ImGui::TextDisabled("Bind via MVar/measureWhat -> SoundKeys1..3 / SoundKeysShake1..3");
  ImGui::PopID();
}

void renderSoundKeysPanel(iota::model::Project& project,
                          const iota::engine::RenderFrameStats& stats,
                          int& activeRangeIdx,
                          bool& openWindow,
                          int& selectedCompIdx,
                          int& selectedElemIdx,
                          InspectorActions& actions) {
  if (!openWindow) {
    return;
  }
  if (!ImGui::Begin("Sound Keys", &openWindow)) {
    ImGui::End();
    return;
  }

  int skCompIdx = -1;
  int skElemIdx = -1;
  iota::model::Element* skEl = nullptr;
  for (int ci = 0; ci < static_cast<int>(project.compositions.size()); ++ci) {
    auto& comp = project.compositions[static_cast<size_t>(ci)];
    for (int ei = 0; ei < static_cast<int>(comp.elements.size()); ++ei) {
      if (comp.elements[static_cast<size_t>(ei)].type == iota::model::ElementType::SoundKeys) {
        skCompIdx = ci;
        skElemIdx = ei;
        skEl = &comp.elements[static_cast<size_t>(ei)];
        break;
      }
    }
    if (skEl) break;
  }

  if (!skEl) {
    ImGui::TextWrapped(
        "No Sound Keys layer exists in this project yet. Add one to isolate up to 3 spectrum frequency/amplitude ranges that any layer, Blur, RgbSplit, Particles, or CustomShader can react to.");
    if (ImGui::Button("+ Add Sound Keys Layer") && !project.compositions.empty()) {
      const int targetComp = std::clamp(selectedCompIdx, 0, static_cast<int>(project.compositions.size()) - 1);
      auto& elems = project.compositions[static_cast<size_t>(targetComp)].elements;
      size_t insertPos = 0;
      for (size_t i = 0; i < elems.size(); ++i) {
        if (elems[i].type == iota::model::ElementType::AudioProvider) {
          insertPos = i + 1;
          break;
        }
      }
      elems.insert(elems.begin() + static_cast<ptrdiff_t>(insertPos),
                   iota::model::createDefaultElement(iota::model::ElementType::SoundKeys));
      selectedCompIdx = targetComp;
      selectedElemIdx = static_cast<int>(insertPos);
      actions.modified = true;
      actions.undoSnapshotNeeded = true;
      actions.editLabel = "Add Sound Keys Layer";
    }
    ImGui::End();
    return;
  }

  ImGui::AlignTextToFramePadding();
  ImGui::Text("Layer: %s", skEl->displayName.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Inspect Layer")) {
    selectedCompIdx = skCompIdx;
    selectedElemIdx = skElemIdx;
  }
  ImGui::SameLine();
  bool useRaw = skEl->props.getBool("useRawSpectrum", false);
  if (ImGui::Checkbox("Raw Spectrum", &useRaw)) {
    skEl->props.setBool("useRawSpectrum", useRaw, "0_general", "b");
    actions.modified = true;
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Toggle Sound Keys Raw Spectrum";
  }
  ImGui::SameLine();
  bool autoCeil = skEl->props.getBool("autoScaleCeiling", true);
  if (ImGui::Checkbox("Auto Ceil", &autoCeil)) {
    skEl->props.setBool("autoScaleCeiling", autoCeil, "0_general", "b");
    actions.modified = true;
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Toggle Sound Keys Auto Ceiling";
  }
  ImGui::SameLine();
  float ceilVal = skEl->props.getFloat("displayCeiling", 50.0f);
  ImGui::SetNextItemWidth(std::min(140.0f, std::max(70.0f, ImGui::GetContentRegionAvail().x)));
  if (ImGui::SliderFloat("##ceil", &ceilVal, 10.0f, 2000.0f, autoCeil ? "Floor %.0f" : "Ceil %.0f")) {
    skEl->props.setFloat("displayCeiling", ceilVal, 10.0f, 2000.0f, "0_general");
    actions.modified = true;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    actions.undoSnapshotNeeded = true;
    actions.editLabel = "Adjust Sound Keys Ceiling";
  }

  renderSoundKeysCanvasAndMeters(*skEl, stats, activeRangeIdx, actions, 145.0f);
  ImGui::End();
}

}  // namespace iota::studio
