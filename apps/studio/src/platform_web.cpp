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

#include "platform_desktop.hpp"

#include "iota/core/utf8_path.hpp"

#include "imgui.h"

#include <GLFW/glfw3.h>
#include <emscripten.h>
#include <emscripten/em_js.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iota::studio::platform {

namespace {

struct PendingDialog {
  bool multi = false;
  std::function<void(std::filesystem::path)> singleCb;
  std::function<void(std::vector<std::filesystem::path>)> multiCb;
  std::function<void(DecodedAudioPayload)> audioPcmCb;
  std::function<void(std::string)> errCb;
  std::vector<std::filesystem::path> batchPaths;
};

struct PendingTemplateFetch {
  std::function<void(std::vector<uint8_t>)> onLoaded;
  std::function<void(std::string)> onError;
};

int gNextRequestId = 1;
std::unordered_map<int, PendingDialog> gPendingDialogs;
std::unordered_map<int, PendingTemplateFetch> gPendingTemplateFetches;
std::vector<std::function<void()>> gQueuedEvents;

std::function<void(std::filesystem::path)> gDropFileCb;
std::function<void(DecodedAudioPayload)> gDropAudioCb;
std::function<void(std::string)> gDropErrCb;

std::string buildHtmlAcceptAttr(std::span<const FileDialogFilter> filters) {
  std::string out;
  for (const auto& f : filters) {
    std::string_view exts = f.extensions ? f.extensions : "*";
    size_t pos = 0;
    while (pos <= exts.size()) {
      const size_t sep = exts.find(';', pos);
      const std::string_view tok =
          (sep == std::string_view::npos) ? exts.substr(pos) : exts.substr(pos, sep - pos);
      if (tok == "*") {
        return "";
      }
      if (!tok.empty()) {
        if (!out.empty()) out += ',';
        out += '.';
        out += tok;
      }
      if (sep == std::string_view::npos) break;
      pos = sep + 1;
    }
  }
  return out;
}

EM_JS(void, jsOpenFilePicker, (int requestId, const char* acceptStr, int multiple, int decodeAudio), {
  var accept = UTF8ToString(acceptStr);
  var input = document.createElement('input');
  input.type = 'file';
  input.style.display = 'none';
  if (accept.length > 0) input.accept = accept;
  if (multiple) input.multiple = true;
  document.body.appendChild(input);

  function sanitizeName(rawName) {
    var base = String(rawName || 'file').replace(/[\\\/]/g, '_').replace(/\.\.+/g, '_');
    return base.length > 0 ? base : 'file';
  }

  function writeVirtualUpload(name, bytes) {
    try { FS.mkdirTree('/uploads'); } catch (e) {}
    var safe = sanitizeName(name);
    var vpath = '/uploads/' + safe;
    FS.writeFile(vpath, new Uint8Array(bytes));
    return vpath;
  }

  function cleanup() {
    if (input.parentNode) input.parentNode.removeChild(input);
  }

  input.addEventListener('change', async function () {
    try {
      var files = Array.from(input.files || []);
      if (files.length === 0) {
        return;
      }
      if (decodeAudio) {
        var file = files[0];
        var buf = await file.arrayBuffer();
        var AudioCtx = window.AudioContext || window.webkitAudioContext;
        if (AudioCtx) {
          var decodeCtx = null;
          try {
            decodeCtx = new AudioCtx();
            var audioBuf = await decodeCtx.decodeAudioData(buf.slice(0));
            var frames = audioBuf.length | 0;
            var sr = Math.max(8000, Math.round(audioBuf.sampleRate || 44100));
            var ch0 = audioBuf.getChannelData(0);
            var ch1 = audioBuf.numberOfChannels > 1 ? audioBuf.getChannelData(1) : ch0;
            var ptr = _malloc(frames * 2 * 4);
            if (ptr) {
              var baseIdx = ptr >> 2;
              for (var i = 0; i < frames; ++i) {
                HEAPF32[baseIdx + i * 2] = ch0[i];
                HEAPF32[baseIdx + i * 2 + 1] = ch1[i];
              }
              Module.ccall('iota_web_on_audio_decoded', null,
                           ['number', 'string', 'number', 'number', 'number'],
                           [requestId, file.name, sr, ptr, frames]);
              _free(ptr);
              return;
            }
          } catch (decodeErr) {
            // Fall through to virtual-FS upload + miniaudio decode attempt
          } finally {
            if (decodeCtx && typeof decodeCtx.close === 'function') {
              try { await decodeCtx.close(); } catch (e) {}
            }
          }
        }
        var vpath = writeVirtualUpload(file.name, buf);
        Module.ccall('iota_web_on_file_uploaded', null, ['number', 'string'], [requestId, vpath]);
        return;
      }

      for (var i = 0; i < files.length; ++i) {
        var f = files[i];
        var data = await f.arrayBuffer();
        var path = writeVirtualUpload(f.name, data);
        Module.ccall('iota_web_on_file_uploaded', null, ['number', 'string'], [requestId, path]);
      }
      if (multiple) {
        Module.ccall('iota_web_on_upload_batch_done', null, ['number'], [requestId]);
      }
    } catch (err) {
      Module.ccall('iota_web_on_audio_error', null, ['number', 'string'],
                   [requestId, String(err && err.message ? err.message : err)]);
    } finally {
      cleanup();
    }
  });

  input.click();
});

EM_JS(void, jsDownloadBytes, (const char* filenameUtf8, const uint8_t* ptr, int len), {
  var name = UTF8ToString(filenameUtf8) || 'download.bin';
  var view = HEAPU8.subarray(ptr, ptr + len);
  var copy = new Uint8Array(view.length);
  copy.set(view);
  var blob = new Blob([copy], { type: 'application/octet-stream' });
  var url = URL.createObjectURL(blob);
  var a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.style.display = 'none';
  document.body.appendChild(a);
  a.click();
  setTimeout(function () {
    URL.revokeObjectURL(url);
    if (a.parentNode) a.parentNode.removeChild(a);
  }, 1000);
});

EM_JS(void, jsSyncPersistentStorage, (), {
  try {
    FS.syncfs(false, function (err) {
      if (err) console.warn('[IotaStudio] IndexedDB sync warning:', err);
    });
  } catch (e) {}
});

EM_JS(void, jsOpenExternalUrl, (const char* urlUtf8), {
  var url = UTF8ToString(urlUtf8);
  window.open(url, '_blank', 'noopener,noreferrer');
});

EM_JS(void, jsQueryCanvasSize, (int* outW, int* outH), {
  var w = Math.max(640, Math.floor(window.innerWidth || 1280));
  var h = Math.max(480, Math.floor(window.innerHeight || 720));
  setValue(outW, w, 'i32');
  setValue(outH, h, 'i32');
});

EM_JS(void, jsResumeAudioContexts, (), {
  if (typeof window !== 'undefined' && Array.isArray(window.__iotaAudioContexts)) {
    for (var i = 0; i < window.__iotaAudioContexts.length; ++i) {
      var ctx = window.__iotaAudioContexts[i];
      if (ctx && typeof ctx.resume === 'function' && ctx.state === 'suspended') {
        try { ctx.resume(); } catch (e) {}
      }
    }
  }
});

EM_JS(void, jsFetchBundledTemplate, (int requestId, const char* filenameUtf8), {
  var fname = UTF8ToString(filenameUtf8);
  var rel = 'templates/' + fname;
  var url = (typeof Module !== 'undefined' && typeof Module.locateFile === 'function')
      ? Module.locateFile(rel)
      : rel;
  fetch(url).then(function (resp) {
    if (!resp.ok) {
      throw new Error('HTTP ' + resp.status + ' fetching ' + rel);
    }
    return resp.arrayBuffer();
  }).then(function (buf) {
    var u8 = new Uint8Array(buf);
    var ptr = _malloc(u8.length);
    if (!ptr) {
      throw new Error('Out of Wasm memory loading template');
    }
    HEAPU8.set(u8, ptr);
    Module.ccall('iota_web_on_template_fetched', null, ['number', 'number', 'number'],
                 [requestId, ptr, u8.length]);
    _free(ptr);
  }).catch(function (err) {
    Module.ccall('iota_web_on_template_error', null, ['number', 'string'],
                 [requestId, String(err && err.message ? err.message : err)]);
  });
});

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void iota_web_on_file_uploaded(int requestId, const char* pathUtf8) {
  if (!pathUtf8) return;
  const std::filesystem::path picked = iota::core::pathFromUtf8(pathUtf8);
  auto it = gPendingDialogs.find(requestId);
  if (it == gPendingDialogs.end()) return;
  if (it->second.multi) {
    it->second.batchPaths.push_back(picked);
  } else {
    auto cb = std::move(it->second.singleCb);
    gPendingDialogs.erase(it);
    if (cb) {
      gQueuedEvents.push_back([cb = std::move(cb), picked]() { cb(picked); });
    }
  }
}

EMSCRIPTEN_KEEPALIVE void iota_web_on_upload_batch_done(int requestId) {
  auto it = gPendingDialogs.find(requestId);
  if (it == gPendingDialogs.end()) return;
  auto cb = std::move(it->second.multiCb);
  auto paths = std::move(it->second.batchPaths);
  gPendingDialogs.erase(it);
  if (cb && !paths.empty()) {
    gQueuedEvents.push_back([cb = std::move(cb), paths = std::move(paths)]() mutable {
      cb(std::move(paths));
    });
  }
}

EMSCRIPTEN_KEEPALIVE void iota_web_on_audio_decoded(int requestId,
                                                    const char* filenameUtf8,
                                                    int sampleRate,
                                                    const float* interleavedPcm,
                                                    int frameCount) {
  auto it = gPendingDialogs.find(requestId);
  if (it == gPendingDialogs.end()) return;
  auto cb = std::move(it->second.audioPcmCb);
  gPendingDialogs.erase(it);
  if (!cb || !interleavedPcm || frameCount <= 0) return;

  DecodedAudioPayload payload;
  payload.filename = filenameUtf8 ? filenameUtf8 : "audio";
  payload.sampleRate = std::max(8000, sampleRate);
  payload.interleavedPcm.assign(interleavedPcm, interleavedPcm + static_cast<size_t>(frameCount) * 2u);
  gQueuedEvents.push_back([cb = std::move(cb), payload = std::move(payload)]() mutable {
    cb(std::move(payload));
  });
}

EMSCRIPTEN_KEEPALIVE void iota_web_on_audio_error(int requestId, const char* errorMsg) {
  auto it = gPendingDialogs.find(requestId);
  if (it == gPendingDialogs.end()) return;
  auto errCb = std::move(it->second.errCb);
  gPendingDialogs.erase(it);
  if (errCb) {
    std::string msg = errorMsg ? errorMsg : "Browser file error";
    gQueuedEvents.push_back([errCb = std::move(errCb), msg = std::move(msg)]() mutable {
      errCb(std::move(msg));
    });
  }
}

EMSCRIPTEN_KEEPALIVE void iota_web_on_file_dropped(const char* pathUtf8) {
  if (!pathUtf8 || !gDropFileCb) return;
  std::filesystem::path dropped = iota::core::pathFromUtf8(pathUtf8);
  gQueuedEvents.push_back([dropped = std::move(dropped)]() {
    if (gDropFileCb) gDropFileCb(dropped);
  });
}

EMSCRIPTEN_KEEPALIVE void iota_web_on_dropped_audio_decoded(const char* filenameUtf8,
                                                            int sampleRate,
                                                            const float* interleavedPcm,
                                                            int frameCount) {
  if (!gDropAudioCb || !interleavedPcm || frameCount <= 0) return;
  DecodedAudioPayload payload;
  payload.filename = filenameUtf8 ? filenameUtf8 : "audio";
  payload.sampleRate = std::max(8000, sampleRate);
  payload.interleavedPcm.assign(interleavedPcm, interleavedPcm + static_cast<size_t>(frameCount) * 2u);
  gQueuedEvents.push_back([payload = std::move(payload)]() mutable {
    if (gDropAudioCb) gDropAudioCb(std::move(payload));
  });
}

EMSCRIPTEN_KEEPALIVE void iota_web_on_drop_error(const char* errorMsg) {
  if (!gDropErrCb) return;
  std::string msg = errorMsg ? errorMsg : "Failed to import dropped file";
  gQueuedEvents.push_back([msg = std::move(msg)]() {
    if (gDropErrCb) gDropErrCb(msg);
  });
}

EMSCRIPTEN_KEEPALIVE void iota_web_on_template_fetched(int requestId, const uint8_t* data, int len) {
  auto it = gPendingTemplateFetches.find(requestId);
  if (it == gPendingTemplateFetches.end()) return;
  auto fetch = std::move(it->second);
  gPendingTemplateFetches.erase(it);
  if (!data || len <= 0) {
    if (fetch.onError) {
      gQueuedEvents.push_back([cb = std::move(fetch.onError)]() { cb("Empty template payload"); });
    }
    return;
  }
  std::vector<uint8_t> bytes(data, data + len);
  if (fetch.onLoaded) {
    gQueuedEvents.push_back([cb = std::move(fetch.onLoaded), bytes = std::move(bytes)]() mutable {
      cb(std::move(bytes));
    });
  }
}

EMSCRIPTEN_KEEPALIVE void iota_web_on_template_error(int requestId, const char* errorMsg) {
  auto it = gPendingTemplateFetches.find(requestId);
  if (it == gPendingTemplateFetches.end()) return;
  auto fetch = std::move(it->second);
  gPendingTemplateFetches.erase(it);
  if (fetch.onError) {
    std::string msg = errorMsg ? errorMsg : "Failed to fetch bundled template";
    gQueuedEvents.push_back([cb = std::move(fetch.onError), msg = std::move(msg)]() mutable {
      cb(std::move(msg));
    });
  }
}

}  // extern "C"

std::optional<std::filesystem::path> showOpenFileDialog(GLFWwindow* /*owner*/,
                                                        const char* /*title*/,
                                                        std::span<const FileDialogFilter> /*filters*/) {
  return std::nullopt;
}

std::vector<std::filesystem::path> showOpenFilesDialog(GLFWwindow* /*owner*/,
                                                       const char* /*title*/,
                                                       std::span<const FileDialogFilter> /*filters*/) {
  return {};
}

std::optional<std::filesystem::path> showSaveFileDialog(GLFWwindow* /*owner*/,
                                                        const char* /*title*/,
                                                        std::span<const FileDialogFilter> /*filters*/,
                                                        const char* defaultExt,
                                                        const char* initialName) {
  std::string name = (initialName && *initialName) ? initialName : "project";
  if (defaultExt && *defaultExt) {
    const std::string dotExt = std::string(".") + defaultExt;
    if (name.size() < dotExt.size() ||
        name.compare(name.size() - dotExt.size(), dotExt.size(), dotExt) != 0) {
      name += dotExt;
    }
  }
  const std::filesystem::path dir("/persist/projects");
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir / name;
}

void requestOpenFile(GLFWwindow* /*owner*/,
                     const char* /*title*/,
                     std::span<const FileDialogFilter> filters,
                     std::function<void(std::filesystem::path)> onPicked) {
  if (!onPicked) return;
  const int reqId = gNextRequestId++;
  PendingDialog pd;
  pd.multi = false;
  pd.singleCb = std::move(onPicked);
  gPendingDialogs.emplace(reqId, std::move(pd));
  const std::string accept = buildHtmlAcceptAttr(filters);
  jsOpenFilePicker(reqId, accept.c_str(), 0, 0);
}

void requestOpenFiles(GLFWwindow* /*owner*/,
                      const char* /*title*/,
                      std::span<const FileDialogFilter> filters,
                      std::function<void(std::vector<std::filesystem::path>)> onPicked) {
  if (!onPicked) return;
  const int reqId = gNextRequestId++;
  PendingDialog pd;
  pd.multi = true;
  pd.multiCb = std::move(onPicked);
  gPendingDialogs.emplace(reqId, std::move(pd));
  const std::string accept = buildHtmlAcceptAttr(filters);
  jsOpenFilePicker(reqId, accept.c_str(), 1, 0);
}

void requestOpenAudioFile(GLFWwindow* /*owner*/,
                          const char* /*title*/,
                          std::span<const FileDialogFilter> filters,
                          std::function<void(std::filesystem::path)> onPathPicked,
                          std::function<void(DecodedAudioPayload)> onDecodedPcm,
                          std::function<void(std::string)> onError) {
  const int reqId = gNextRequestId++;
  PendingDialog pd;
  pd.multi = false;
  pd.singleCb = std::move(onPathPicked);
  pd.audioPcmCb = std::move(onDecodedPcm);
  pd.errCb = std::move(onError);
  gPendingDialogs.emplace(reqId, std::move(pd));
  const std::string accept = buildHtmlAcceptAttr(filters);
  jsOpenFilePicker(reqId, accept.c_str(), 0, 1);
}

void triggerWebFileDownload(const std::filesystem::path& path, const std::string& downloadName) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.is_open()) return;
  const auto sz = in.tellg();
  if (sz <= 0) return;
  std::vector<uint8_t> bytes(static_cast<size_t>(sz));
  in.seekg(0, std::ios::beg);
  if (!in.read(reinterpret_cast<char*>(bytes.data()), sz)) return;
  const std::string name = downloadName.empty() ? iota::core::pathFilenameUtf8(path) : downloadName;
  jsDownloadBytes(name.c_str(), bytes.data(), static_cast<int>(bytes.size()));
}

void syncPersistentStorage() {
  jsSyncPersistentStorage();
}

void pollPlatformEvents() {
  if (gQueuedEvents.empty()) return;
  std::vector<std::function<void()>> batch;
  batch.swap(gQueuedEvents);
  for (auto& fn : batch) {
    if (fn) fn();
  }
}

void setWebDropHandlers(std::function<void(std::filesystem::path)> onFileDropped,
                        std::function<void(DecodedAudioPayload)> onAudioDropped,
                        std::function<void(std::string)> onDropError) {
  gDropFileCb = std::move(onFileDropped);
  gDropAudioCb = std::move(onAudioDropped);
  gDropErrCb = std::move(onDropError);
}

void queryWebCanvasSize(int& outWidth, int& outHeight) {
  jsQueryCanvasSize(&outWidth, &outHeight);
}

void resumeWebAudioContext() {
  jsResumeAudioContexts();
}

bool openExternalUrl(GLFWwindow* /*owner*/, const std::string& url) {
  jsOpenExternalUrl(url.c_str());
  return true;
}

std::filesystem::path findBundledAssetPath(std::string_view relPath) {
  while (!relPath.empty() && relPath.front() == '/') {
    relPath.remove_prefix(1);
  }
  const std::filesystem::path cand = std::filesystem::path("/") / iota::core::pathFromUtf8(relPath);
  if (iota::core::pathExistsNoThrow(cand)) {
    return cand;
  }
  return {};
}

void requestFetchBundledTemplate(const std::string& templateFilename,
                                 std::function<void(std::vector<uint8_t>)> onLoaded,
                                 std::function<void(std::string)> onError) {
  const int reqId = gNextRequestId++;
  PendingTemplateFetch pending;
  pending.onLoaded = std::move(onLoaded);
  pending.onError = std::move(onError);
  gPendingTemplateFetches.emplace(reqId, std::move(pending));
  jsFetchBundledTemplate(reqId, templateFilename.c_str());
}

void applyWindowHintsBeforeCreate() {
  // No desktop-specific hints required under Emscripten.
}

void applyWindowIcon(GLFWwindow* /*window*/, void*& outBigHandle, void*& outSmallHandle) {
  outBigHandle = nullptr;
  outSmallHandle = nullptr;
}

void destroyWindowIcons(void*& bigHandle, void*& smallHandle) {
  bigHandle = nullptr;
  smallHandle = nullptr;
}

void loadPlatformImGuiFonts(ImGuiIO& io, ImFont*& outCodeFont) {
  outCodeFont = nullptr;
  const char* uiFont = "/assets/fonts/NotoSans-Regular.ttf";
  if (iota::core::pathExistsNoThrow(iota::core::pathFromUtf8(uiFont))) {
    io.Fonts->AddFontFromFileTTF(uiFont, 16.0f);
  }
  const char* monoFont = "/assets/fonts/NotoSansMono-Regular.ttf";
  if (iota::core::pathExistsNoThrow(iota::core::pathFromUtf8(monoFont))) {
    outCodeFont = io.Fonts->AddFontFromFileTTF(monoFont, 15.0f);
  }
}

std::string defaultLayoutIniPath() {
  const std::filesystem::path dir("/persist");
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return "/persist/iota_studio_layout.ini";
}

struct WaylandFramePacer::Impl {};

WaylandFramePacer::WaylandFramePacer() = default;
WaylandFramePacer::~WaylandFramePacer() = default;
bool WaylandFramePacer::attach(GLFWwindow* /*window*/) { return false; }
void WaylandFramePacer::detach() {}
bool WaylandFramePacer::isActive() const noexcept { return false; }
bool WaylandFramePacer::isWaitingForCompositor() const noexcept { return false; }
void WaylandFramePacer::cancelPendingFrame() {}
void WaylandFramePacer::armBeforeSwap() {}
bool WaylandFramePacer::waitForFrameOrTimeout(int /*maxWaitMs*/) { return true; }

}  // namespace iota::studio::platform
