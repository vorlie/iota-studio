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

#include "doctest/doctest.h"

#include "iota/core/utf8_path.hpp"
#include "iota/mcp/live_bridge.hpp"
#include "iota/mcp/mcp_server.hpp"
#include "iota/mcp/png_encode.hpp"
#include "iota/mcp/studio_session.hpp"
#include "iota/mcp/working_copy.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

unsigned long currentPid() noexcept {
#ifdef _WIN32
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

std::string makeTestBridgeEndpoint(const char* prefix) {
#ifdef _WIN32
  return std::string("\\\\.\\pipe\\") + prefix + "_" + std::to_string(currentPid());
#else
  return iota::core::pathToUtf8(
      std::filesystem::temp_directory_path() /
      (std::string(prefix) + "_" + std::to_string(currentPid()) + ".sock"));
#endif
}

struct ScopedTempDir {
  std::filesystem::path dir;
  explicit ScopedTempDir(const char* name) {
    dir = std::filesystem::temp_directory_path() /
          (std::string(name) + "_" + std::to_string(currentPid()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
  }
  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

nlohmann::json parseToolTextJson(const nlohmann::json& resp) {
  REQUIRE(resp.contains("result"));
  const auto& res = resp["result"];
  REQUIRE(res.contains("content"));
  REQUIRE(res["content"].is_array());
  REQUIRE(!res["content"].empty());
  const std::string text = res["content"][0].at("text").get<std::string>();
  return nlohmann::json::parse(text);
}

nlohmann::json callMcpTool(iota::mcp::McpServer& srv,
                           int id,
                           const std::string& name,
                           const nlohmann::json& args = nlohmann::json::object()) {
  auto opt = srv.handleJsonRpcMessage({
      {"jsonrpc", "2.0"},
      {"id", id},
      {"method", "tools/call"},
      {"params", {{"name", name}, {"arguments", args}}},
  });
  REQUIRE(opt.has_value());
  return *opt;
}

}  // namespace

TEST_CASE("MCP working-copy path generation protects original projects without stacking suffixes") {
  using namespace iota::mcp;

  ScopedTempDir tmp("iota_mcp_workcopy");
  const auto origIota = tmp.dir / "MyTrack.iota";
  const auto origViz = tmp.dir / "LegacyTemplate.viz";

  CHECK(isVizExtension(origViz));
  CHECK(!isVizExtension(origIota));

  const auto copy1 = computeMcpWorkingCopyPath(origIota);
  CHECK(copy1.filename() == "MyTrack.mcp.iota");
  CHECK(isMcpWorkingCopyPath(copy1));
  CHECK(!isMcpWorkingCopyPath(origIota));

  // Should never stack .mcp.mcp.iota
  const auto noStack = computeMcpWorkingCopyPath(copy1);
  CHECK(noStack.filename() == "MyTrack.mcp.iota");

  // When MyTrack.mcp.iota exists on disk, increment cleanly to .mcp_2.iota and .mcp_3.iota
  { std::ofstream(copy1) << "x"; }
  const auto copy2 = computeMcpWorkingCopyPath(origIota);
  CHECK(copy2.filename() == "MyTrack.mcp_2.iota");
  CHECK(isMcpWorkingCopyPath(copy2));

  { std::ofstream(copy2) << "y"; }
  const auto copy3 = computeMcpWorkingCopyPath(origIota);
  CHECK(copy3.filename() == "MyTrack.mcp_3.iota");
  CHECK(computeMcpWorkingCopyPath(copy2).filename() == "MyTrack.mcp_3.iota");

  // Legacy .viz always turns into a safe .mcp.iota sibling
  CHECK(computeMcpWorkingCopyPath(origViz).filename() == "LegacyTemplate.mcp.iota");
}

TEST_CASE("PNG and base64 encoders produce valid PNG signature") {
  const std::vector<uint8_t> rgba(4u * 4u * 4u, 255u);
  const auto png = iota::mcp::encodeRgbaToPng(4, 4, rgba);
  REQUIRE(png.size() > 16u);
  CHECK(png[0] == 0x89u);
  CHECK(png[1] == 0x50u);  // 'P'
  CHECK(png[2] == 0x4Eu);  // 'N'
  CHECK(png[3] == 0x47u);  // 'G'
  CHECK(png[4] == 0x0Du);
  CHECK(png[5] == 0x0Au);
  CHECK(png[6] == 0x1Au);
  CHECK(png[7] == 0x0Au);

  const std::string b64 = iota::mcp::base64Encode(png);
  CHECK(b64.rfind("iVBOR", 0) == 0);
}

TEST_CASE("McpServer handles protocol, project/layer edits, viz protection, docs, and OpenGL PNG preview") {
  iota::mcp::HeadlessStudioSession session;
  iota::mcp::McpServer server(session);

  // Protocol handshake & notifications
  auto initResp = server.handleJsonRpcMessage({
      {"jsonrpc", "2.0"},
      {"id", 1},
      {"method", "initialize"},
      {"params", {{"protocolVersion", "2024-11-05"}}},
  });
  REQUIRE(initResp.has_value());
  CHECK((*initResp)["result"]["serverInfo"]["name"] == "iota-studio-mcp");

  auto notifResp = server.handleJsonRpcMessage({
      {"jsonrpc", "2.0"},
      {"method", "notifications/initialized"},
  });
  CHECK(!notifResp.has_value());

  auto pingResp = server.handleJsonRpcMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}});
  REQUIRE(pingResp.has_value());
  CHECK(pingResp->contains("result"));

  auto listToolsResp = server.handleJsonRpcMessage({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}});
  REQUIRE(listToolsResp.has_value());
  CHECK((*listToolsResp)["result"]["tools"].size() >= 30u);

  auto listResResp = server.handleJsonRpcMessage({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "resources/list"}});
  REQUIRE(listResResp.has_value());
  CHECK((*listResResp)["result"]["resources"].size() == 5u);

  auto readDocsResp = server.handleJsonRpcMessage({
      {"jsonrpc", "2.0"},
      {"id", 5},
      {"method", "resources/read"},
      {"params", {{"uri", "iota://docs/mvar-modes"}}},
  });
  REQUIRE(readDocsResp.has_value());
  CHECK(!(*readDocsResp)["result"]["contents"].empty());

  auto unknownMethod = server.handleJsonRpcMessage({{"jsonrpc", "2.0"}, {"id", 6}, {"method", "nope/method"}});
  REQUIRE(unknownMethod.has_value());
  CHECK((*unknownMethod)["error"]["code"] == -32601);

  // New project, add layers, edit scalar + nested properties, undo/redo
  auto newRes = callMcpTool(server, 10, "project_new", {{"template", "minimal_circular"}, {"name", "MCP Test"}});
  CHECK(!newRes["result"]["isError"].get<bool>());

  auto addPart = callMcpTool(server, 11, "layer_add", {{"element_type", "Particles"}, {"name", "MCP Sparks"}});
  const auto partInfo = parseToolTextJson(addPart);
  const int partIdx = partInfo.at("element_index").get<int>();

  auto setProps = callMcpTool(
      server,
      12,
      "layer_set_properties",
      {{"composition_index", 0},
       {"element_index", partIdx},
       {"properties",
        {{"position", {0.5, 0.4}},
         {"scaleIsUniform", true},
         {"colorFrom", "#FFFF4020"},
         {"hdrIntensity", 1.8},
         {"measureEmitRate", "Beat 1.5 1.5"}}}});
  if (setProps["result"]["isError"].get<bool>()) {
    FAIL(setProps["result"]["content"][0]["text"].get<std::string>());
  }

  // Also verify dot-path child edit on Bars segment1
  auto addBars = callMcpTool(server, 13, "layer_add", {{"element_type", "Bars"}, {"name", "MCP Bars"}});
  const int barsIdx = parseToolTextJson(addBars).at("element_index").get<int>();
  auto setBarChild = callMcpTool(
      server,
      14,
      "layer_set_properties",
      {{"composition_index", 0},
       {"element_index", barsIdx},
       {"properties", {{"segment1.barWidth", 0.72}, {"blendMode", "Add"}}}});
  CHECK(!setBarChild["result"]["isError"].get<bool>());

  auto getBars = callMcpTool(server, 15, "layer_get_properties", {{"composition_index", 0}, {"element_index", barsIdx}});
  CHECK(!getBars["result"]["isError"].get<bool>());

  // Undo & Redo
  auto undoRes = callMcpTool(server, 16, "studio_undo");
  CHECK(!undoRes["result"]["isError"].get<bool>());
  auto redoRes = callMcpTool(server, 17, "studio_redo");
  CHECK(!redoRes["result"]["isError"].get<bool>());

  // Docs schema + search
  auto schemaRes = callMcpTool(server, 18, "docs_get_element_schema", {{"element_type", "CustomShader"}});
  CHECK(!schemaRes["result"]["isError"].get<bool>());
  auto searchRes = callMcpTool(server, 19, "docs_search", {{"query", "SoundKeys"}});
  CHECK(!searchRes["result"]["isError"].get<bool>());
  CHECK(!parseToolTextJson(searchRes)["mvar_modes"].empty());

  // Legacy .viz overwrite protection
  ScopedTempDir tmp("iota_mcp_viz_guard");
  const auto existingViz = tmp.dir / "template.viz";
  { std::ofstream ofs(existingViz, std::ios::binary); ofs << "ORIGINAL_VIZ_MARKER"; }
  auto blockedSave = callMcpTool(
      server,
      20,
      "project_save",
      {{"path", iota::core::pathToUtf8(existingViz)}, {"allow_viz_overwrite", false}});
  CHECK(blockedSave["result"]["isError"].get<bool>());
  {
    std::ifstream ifs(existingViz, std::ios::binary);
    std::string content;
    std::getline(ifs, content);
    CHECK(content == "ORIGINAL_VIZ_MARKER");
  }

  const auto safeIota = tmp.dir / "saved.iota";
  auto okSave = callMcpTool(server, 21, "project_save", {{"path", iota::core::pathToUtf8(safeIota)}});
  CHECK(!okSave["result"]["isError"].get<bool>());
  CHECK(std::filesystem::exists(safeIota));

  // Synthetic audio + OpenGL 4.5 preview PNG frame
  auto audioRes = callMcpTool(
      server, 22, "audio_load", {{"synthetic_freq_hz", 110.0}, {"synthetic_duration_sec", 3.0}});
  CHECK(!audioRes["result"]["isError"].get<bool>());

  const auto pngOut = tmp.dir / "preview.png";
  auto previewRes = callMcpTool(
      server,
      23,
      "render_preview_frame",
      {{"width", 320},
       {"height", 180},
       {"time_sec", 0.5},
       {"pre_roll_frames", 4},
       {"return_image", true},
       {"save_png_path", iota::core::pathToUtf8(pngOut)}});
  REQUIRE(!previewRes["result"]["isError"].get<bool>());
  REQUIRE(previewRes["result"]["content"].size() == 2u);
  CHECK(previewRes["result"]["content"][1]["type"] == "image");
  CHECK(previewRes["result"]["content"][1]["mimeType"] == "image/png");
  CHECK(previewRes["result"]["content"][1]["data"].get<std::string>().rfind("iVBOR", 0) == 0);
  CHECK(std::filesystem::exists(pngOut));
  CHECK(parseToolTextJson(previewRes)["draw_calls"].get<int>() > 0);

  session.releaseGraphicsContext();
}

TEST_CASE("MCP LiveBridgeHost and LiveBridgeClient complete round-trip and shut down without hanging") {
  using namespace iota::mcp;

  const std::string pipeName = makeTestBridgeEndpoint("iota_studio_mcp_test");

  HeadlessStudioSession hostSession;
  LiveBridgeHost host;
  REQUIRE(host.start(pipeName));
  CHECK(host.isListening());

  HeadlessStudioSession clientLocalSession;
  LiveBridgeClient client;
  McpServer clientServer(clientLocalSession, &client);

  std::atomic<bool> stopPolling{false};
  std::thread mainSimThread([&]() {
    while (!stopPolling.load(std::memory_order_acquire)) {
      host.pollAndDispatch([&](const nlohmann::json& req) {
        return executeTargetBridgeRequest(hostSession, req, host.requestsHandled());
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  auto connResp = callMcpTool(clientServer, 1, "studio_connect", {{"pipe_name", pipeName}, {"timeout_ms", 2000}});
  REQUIRE(!connResp["result"]["isError"].get<bool>());
  CHECK(client.isConnected());

  auto statusResp = callMcpTool(clientServer, 2, "studio_status");
  REQUIRE(!statusResp["result"]["isError"].get<bool>());
  const auto statusJson = parseToolTextJson(statusResp);
  CHECK(statusJson["client_attached"].get<bool>());
  CHECK(statusJson["pipe_name"] == pipeName);

  auto addShader = callMcpTool(clientServer, 3, "layer_add", {{"element_type", "CustomShader"}, {"name", "Live Shader"}});
  REQUIRE(!addShader["result"]["isError"].get<bool>());
  CHECK(hostSession.project().compositions[0].elements.back().displayName == "Live Shader");

  auto discResp = callMcpTool(clientServer, 4, "studio_disconnect");
  CHECK(!discResp["result"]["isError"].get<bool>());
  CHECK(!client.isConnected());

  stopPolling.store(true, std::memory_order_release);
  mainSimThread.join();
  host.stop();
  CHECK(!host.isListening());
}

#ifdef IOTA_MCP_EXE_PATH
TEST_CASE("Standalone iota_mcp handles stdio JSON-RPC requests cleanly") {
  const std::filesystem::path exePath(IOTA_MCP_EXE_PATH);
  if (!iota::core::pathExistsNoThrow(exePath)) {
    MESSAGE("Skipping subprocess stdio test; iota_mcp binary not built yet.");
    return;
  }

  const std::string requests =
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\"}}\n"
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"docs_list_elements\",\"arguments\":{}}}\n";

  std::string stdoutAll;
  int exitCode = 1;

#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE inRead = nullptr, inWrite = nullptr;
  HANDLE outRead = nullptr, outWrite = nullptr;
  REQUIRE(CreatePipe(&inRead, &inWrite, &sa, 0));
  REQUIRE(CreatePipe(&outRead, &outWrite, &sa, 0));
  SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = inRead;
  si.hStdOutput = outWrite;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exePath.wstring() + L"\"";
  const BOOL ok = CreateProcessW(
      nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(inRead);
  CloseHandle(outWrite);
  REQUIRE(ok);

  DWORD written = 0;
  REQUIRE(WriteFile(inWrite, requests.data(), static_cast<DWORD>(requests.size()), &written, nullptr));
  CloseHandle(inWrite);  // Signal EOF so iota_mcp exits cleanly after responding

  char buf[4096];
  DWORD bytesRead = 0;
  while (ReadFile(outRead, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
    stdoutAll.append(buf, bytesRead);
  }
  CloseHandle(outRead);

  REQUIRE(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0);
  DWORD winExit = 1;
  GetExitCodeProcess(pi.hProcess, &winExit);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  exitCode = static_cast<int>(winExit);
#else
  int inPipe[2]{-1, -1};
  int outPipe[2]{-1, -1};
  REQUIRE(pipe(inPipe) == 0);
  REQUIRE(pipe(outPipe) == 0);

  const pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    close(inPipe[1]);
    close(outPipe[0]);
    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    close(inPipe[0]);
    close(outPipe[1]);
    std::string exeStr = exePath.string();
    char* argv[] = {exeStr.data(), nullptr};
    execv(exeStr.c_str(), argv);
    _exit(127);
  }
  close(inPipe[0]);
  close(outPipe[1]);

  size_t off = 0;
  while (off < requests.size()) {
    const ssize_t n = write(inPipe[1], requests.data() + off, requests.size() - off);
    if (n > 0) {
      off += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  close(inPipe[1]);

  char buf[4096];
  for (;;) {
    const ssize_t n = read(outPipe[0], buf, sizeof(buf));
    if (n > 0) {
      stdoutAll.append(buf, static_cast<size_t>(n));
    } else if (n == 0) {
      break;
    } else if (errno != EINTR) {
      break;
    }
  }
  close(outPipe[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
  CHECK(exitCode == 0);

  std::vector<nlohmann::json> lines;
  std::istringstream iss(stdoutAll);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    lines.push_back(nlohmann::json::parse(line));
  }
  REQUIRE(lines.size() == 3u);
  CHECK(lines[0]["id"] == 1);
  CHECK(lines[0]["result"]["serverInfo"]["name"] == "iota-studio-mcp");
  CHECK(lines[1]["id"] == 2);
  CHECK(lines[1]["result"]["tools"].size() >= 30u);
  CHECK(lines[2]["id"] == 3);
  CHECK(!lines[2]["result"]["isError"].get<bool>());
}
#endif

TEST_CASE("MCP layer_set_properties supports ShakeEffect Opaque blend and parent-before-child ordering") {
  using namespace iota::mcp;

  HeadlessStudioSession session;
  McpServer server(session);

  auto addShake = callMcpTool(server, 1, "layer_add", {{"element_type", "ShakeEffect"}, {"name", "Fix4 Shake"}});
  REQUIRE(!addShake["result"]["isError"].get<bool>());
  const int shakeIdx = parseToolTextJson(addShake).at("element_index").get<int>();

  auto setScreen = callMcpTool(
      server,
      2,
      "layer_set_properties",
      {{"composition_index", 0}, {"element_index", shakeIdx}, {"properties", {{"blendMode", "Screen"}}}});
  CHECK(!setScreen["result"]["isError"].get<bool>());

  auto setOpaque = callMcpTool(
      server,
      3,
      "layer_set_properties",
      {{"composition_index", 0}, {"element_index", shakeIdx}, {"properties", {{"blendMode", "Opaque"}}}});
  CHECK(!setOpaque["result"]["isError"].get<bool>());

  auto setInvalid = callMcpTool(
      server,
      4,
      "layer_set_properties",
      {{"composition_index", 0}, {"element_index", shakeIdx}, {"properties", {{"blendMode", "NotABlend"}}}});
  CHECK(setInvalid["result"]["isError"].get<bool>());

  auto addBars = callMcpTool(server, 5, "layer_add", {{"element_type", "Bars"}, {"name", "Fix6 Bars"}});
  REQUIRE(!addBars["result"]["isError"].get<bool>());
  const int barsIdx = parseToolTextJson(addBars).at("element_index").get<int>();

  auto atomicSet = callMcpTool(
      server,
      6,
      "layer_set_properties",
      {{"composition_index", 0},
       {"element_index", barsIdx},
       {"properties",
        {{"Segment2.barWidth", 0.42},
         {"Segment2.colorFrom", "#ffff0000"},
         {"Segment2", "Bars"},
         {"ShapePath.sides", 8},
         {"ShapePath", "SidedPolygon"},
         {"MeasurePos.A", 0.9},
         {"MeasurePos.measureWhat", "Beat"}}}});
  REQUIRE(!atomicSet["result"]["isError"].get<bool>());

  const auto& barsProps = session.project().compositions[0].elements[static_cast<size_t>(barsIdx)].props;
  const auto* seg2 = barsProps.find("Segment2");
  REQUIRE(seg2 != nullptr);
  CHECK(std::get<std::string>(seg2->value) == "Bars");
  REQUIRE(seg2->findChild("barWidth") != nullptr);
  CHECK(std::get<double>(seg2->findChild("barWidth")->value) == doctest::Approx(0.42));

  const auto* sp = barsProps.find("ShapePath");
  REQUIRE(sp != nullptr);
  CHECK(std::get<std::string>(sp->value) == "SidedPolygon");
  REQUIRE(sp->findChild("sides") != nullptr);
  CHECK(std::get<int64_t>(sp->findChild("sides")->value) == 8);

  const auto* mp = barsProps.find("MeasurePos");
  REQUIRE(mp != nullptr);
  REQUIRE(mp->findChild("measureWhat") != nullptr);
  CHECK(std::get<std::string>(mp->findChild("measureWhat")->value) == "Beat");
  REQUIRE(mp->findChild("A") != nullptr);
  CHECK(std::get<double>(mp->findChild("A")->value) == doctest::Approx(0.9));
}

TEST_CASE("MCP render_preview_frame advances totalTimeSec cleanly during headless scrubbing") {
  using namespace iota::mcp;

  HeadlessStudioSession session;
  McpServer server(session);

  REQUIRE(!callMcpTool(server, 0, "project_new", {{"template", "blank"}})["result"]["isError"].get<bool>());
  REQUIRE(!callMcpTool(server, 1, "audio_load", {{"synthetic_freq_hz", 110.0}, {"synthetic_duration_sec", 4.0}})
               ["result"]["isError"]
                   .get<bool>());

  auto addCs = callMcpTool(server, 2, "layer_add", {{"element_type", "CustomShader"}, {"name", "Time Scrub"}});
  REQUIRE(!addCs["result"]["isError"].get<bool>());
  const int csIdx = parseToolTextJson(addCs).at("element_index").get<int>();

  auto setCs = callMcpTool(
      server,
      3,
      "layer_set_properties",
      {{"composition_index", 0},
       {"element_index", csIdx},
       {"properties",
        {{"TargetImage", "internalres:white"},
         {"scale", {1.0, 1.0}},
         {"shaderFragment", "uniform float u_time;\nvoid main() { gl_FragColor = vec4(0.15 + 0.75 * fract(u_time * 0.5), 0.25, 0.5, 1.0); }"}}}});
  REQUIRE(!setCs["result"]["isError"].get<bool>());

  auto prev0 = callMcpTool(
      server,
      4,
      "render_preview_frame",
      {{"width", 128}, {"height", 128}, {"time_sec", 0.0}, {"pre_roll_frames", 0}, {"return_image", true}});
  REQUIRE(!prev0["result"]["isError"].get<bool>());
  CHECK(parseToolTextJson(prev0)["last_error"] == "");

  auto prev1 = callMcpTool(
      server,
      5,
      "render_preview_frame",
      {{"width", 128}, {"height", 128}, {"time_sec", 1.0}, {"pre_roll_frames", 0}, {"return_image", true}});
  REQUIRE(!prev1["result"]["isError"].get<bool>());
  CHECK(parseToolTextJson(prev1)["last_error"] == "");

  CHECK(prev0["result"]["content"][1]["data"] != prev1["result"]["content"][1]["data"]);
  session.releaseGraphicsContext();
}

TEST_CASE("MCP LiveBridgeHost serves concurrent LiveBridgeClient connections cleanly") {
  using namespace iota::mcp;

  const std::string pipeName = makeTestBridgeEndpoint("iota_studio_mcp_multi_test");

  HeadlessStudioSession hostSession;
  LiveBridgeHost host;
  REQUIRE(host.start(pipeName));
  CHECK(host.isListening());

  std::atomic<bool> stopPolling{false};
  std::thread mainSimThread([&]() {
    while (!stopPolling.load(std::memory_order_acquire)) {
      host.pollAndDispatch([&](const nlohmann::json& req) {
        return executeTargetBridgeRequest(hostSession, req, host.requestsHandled());
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  HeadlessStudioSession localA;
  LiveBridgeClient clientA;
  McpServer serverA(localA, &clientA);

  HeadlessStudioSession localB;
  LiveBridgeClient clientB;
  McpServer serverB(localB, &clientB);

  REQUIRE(!callMcpTool(serverA, 1, "studio_connect", {{"pipe_name", pipeName}, {"timeout_ms", 2000}})
               ["result"]["isError"]
                   .get<bool>());
  REQUIRE(!callMcpTool(serverB, 1, "studio_connect", {{"pipe_name", pipeName}, {"timeout_ms", 2000}})
               ["result"]["isError"]
                   .get<bool>());

  CHECK(clientA.isConnected());
  CHECK(clientB.isConnected());
  for (int i = 0; i < 40 && !host.hasConnectedClient(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(host.hasConnectedClient());

  std::atomic<bool> okA{false};
  std::atomic<bool> okB{false};
  std::thread threadA([&]() {
    auto r1 = callMcpTool(serverA, 2, "studio_status");
    auto r2 = callMcpTool(serverA, 3, "layer_add", {{"element_type", "Text"}, {"name", "ClientA Text"}});
    okA.store(!r1["result"]["isError"].get<bool>() && !r2["result"]["isError"].get<bool>());
  });
  std::thread threadB([&]() {
    auto r1 = callMcpTool(serverB, 2, "studio_status");
    auto r2 = callMcpTool(serverB, 3, "layer_add", {{"element_type", "Text"}, {"name", "ClientB Text"}});
    okB.store(!r1["result"]["isError"].get<bool>() && !r2["result"]["isError"].get<bool>());
  });
  threadA.join();
  threadB.join();

  CHECK(okA.load());
  CHECK(okB.load());
  CHECK(host.requestsHandled() >= 4u);

  REQUIRE(!callMcpTool(serverA, 4, "studio_disconnect")["result"]["isError"].get<bool>());
  CHECK(clientB.isConnected());
  auto statusB = callMcpTool(serverB, 4, "studio_status");
  CHECK(!statusB["result"]["isError"].get<bool>());
  CHECK(host.hasConnectedClient());

  REQUIRE(!callMcpTool(serverB, 5, "studio_disconnect")["result"]["isError"].get<bool>());

  stopPolling.store(true, std::memory_order_release);
  mainSimThread.join();
  host.stop();
  CHECK(!host.isListening());
  CHECK(!host.hasConnectedClient());
}
