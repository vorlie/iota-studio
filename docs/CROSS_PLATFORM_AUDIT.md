# Iota Studio — Linux & Windows Cross-Platform Compatibility Audit

**Audit Date:** 2026-11-26  
**Audit Scope:** Full read-only codebase inspection (~42,289 lines of C/C++ across `apps/`, `libs/`, `tests/`, `CMakeLists.txt`, `CMakePresets.json`, `installer/`, `third_party/discord_social_sdk/`, and sample `.viz` preset `test_template/SPECIAL FOR 300SUBS.viz`).  
**Target Platforms:** Windows (x64 / ARM64) & Linux (x86_64 / aarch64, X11 & Wayland).  
**Constraint Adherence:** Audit-only; no source code, configuration files, build scripts, dependencies, tests, or assets were modified.

---

## 1. Executive Summary

Iota Studio (C++20, CMake 3.24+) is built on a **clean, modular, largely portable core architecture**:
- **Already portable or cleanly structured:** `libs/model`, `libs/dsp`, `libs/rhi`, `libs/core` (mostly), `libs/preset_io` (mostly), `libs/discord` (mostly), `libs/mcp` (except `live_bridge.cpp`), `apps/mcp/src/main.cpp`, most of `libs/engine` (except `font_atlas.cpp` and GLSL UB in `builtin_shader_library.cpp`), and almost all ImGui UI panels in `apps/studio/src/` (except `editor_app.cpp` and `inspector_panel.cpp` font dropdown).
- **Partial Linux groundwork already exists:** `CMakePresets.json` defines `linux-gcc-ninja-*` presets; `libs/core/src/utf8_path.cpp` isolates Win32 UTF-16 conversions; `libs/preset_io/src/project_manager.cpp` uses `localtime_r` and checks `XDG_DATA_HOME`; `CMakeLists.txt` and `apps/studio/CMakeLists.txt` detect `libdiscord_partner_sdk.so` and set `$ORIGIN` RPATH; and `apps/mcp/src/main.cpp` guards `_setmode`.

**However, Iota Studio currently CANNOT compile, link, or run on Linux.**

### Key Takeaways
1. **P0 Build & Link Blockers (6 subsystems):** Hardcoded `opengl32` / `comdlg32` / `shell32` in CMake; missing `-ldl`/`-lpthread`/`-lm` linkage for `miniaudio` and threaded targets; unconditional `<windows.h>` and Win32 APIs in `gl_loader.{hpp,cpp}`, `audio_decoder.cpp` (plus `ma_decoder_init_file_w` type error), `video_exporter.cpp`, `editor_app.cpp`, `live_bridge.cpp`, and `tests/test_mcp.cpp`.
2. **P1 Critical Linux Runtime & Parity Failures:**
   - **Typography:** `libs/engine/src/font_atlas.cpp` implements text rendering exclusively via DirectWrite (`#ifdef _WIN32`). On Linux it falls back to `buildFallbackBitmapAtlas()`, which draws **unreadable striped placeholder boxes** for ASCII and drops all Unicode text.
   - **GLSL Strictness:** 5 built-in shaders in `libs/engine/src/builtin_shader_library.cpp` contain GLSL 4.50 spec undefined behavior (`pow(negative, 2.0)` producing `NaN`, and 4 inverted `smoothstep(hi, lo, x)` calls) that NVIDIA Windows tolerates but Mesa/AMD/Intel Linux drivers can corrupt.
   - **Update Checker:** `libs/core/src/update_checker.cpp` stubs out non-Windows HTTP checks with an immediate error and prefers `.exe`/`.zip` release assets regardless of OS.
   - **XDG & Case-Sensitivity:** `ProjectLibrary::defaultRegistryPath()` lacks the `$HOME/.local/share` fallback when `XDG_DATA_HOME` is unset, and `normalizePathLower()` unconditionally folds path case on case-sensitive Linux filesystems.
   - **Discord SDK Linux Load-Time Coupling:** `libdiscord_partner_sdk.so` is x86-64 only (breaks Linux `aarch64` builds) and has no `/DELAYLOAD` equivalent on Linux—making `libpulse.so.0`, `libasound.so.2`, `libX11.so.6`, `libatomic.so.1`, and `GLIBC_2.29` mandatory before `main()`.
   - **Tests:** `test_engine_gl.cpp`, `test_model_preset_io.cpp`, `test_video_export.cpp`, and `test_mcp.cpp` contain DirectWrite assertions, `L"..."` wide-string paths that throw under Linux `"C"` locale, `ffmpeg.exe` assertions, and Win32 process APIs.
3. **Cross-Platform `.viz` / `.iota` Project Portability:** Bundled scenes (`scene.json`), `local:` images, `local:` shaders, and `local:` fonts serialize portably (confirmed on `test_template/SPECIAL FOR 300SUBS.viz`), **but external audio paths (`metadata.audioPath`, `metadata.audioAssets`) are stored as raw OS-specific absolute paths**, breaking projects moved between Windows and Linux.

### Subsystem Readiness Summary

| Audit Domain | Windows Status | Linux Status | Primary Blockers / Gaps |
|---|---|---|---|
| **1. Build System & Dependencies** | Supported (MSVC + Ninja) | **Fails to link** (`build/linux-release` configures only) | Hardcoded `opengl32`, `comdlg32`, `shell32`; missing `Threads`/`dl`/`m`; Discord SDK arch check |
| **2. Platform APIs & System Integration** | Implemented (Win32) | **Fails to compile** | Unconditional `<windows.h>`, `CreateProcessW`, `LoadImageW`, `ShellExecuteW`, Named Pipes |
| **3. Filesystem & Path Portability** | Implemented (`utf8_path.cpp`) | **Partial / Broken edge cases** | Missing `$HOME/.local/share` fallback; unconditional `normalizePathLower`; CWD `iota_studio_layout.ini`; dir `readZipFromFile` OOM |
| **4. Rendering & Graphics** | Implemented (WGL + DirectWrite) | **Fails to compile & broken text/UB** | `gl_loader` is WGL-only; DirectWrite-only fonts; 5 GLSL 4.50 UB shaders; `stb` `-Wpedantic` warnings |
| **5. Audio System** | Implemented (Playback + `ffmpeg.exe`) | **Fails to compile** | `audio_decoder.cpp` Win32 + `ma_decoder_init_file_w`; missing `-ldl -lpthread -lm`; no capture/loopback/device list |
| **6. UI & Desktop Integration** | Implemented (Win32 + ImGui) | **Fails to compile** | Win32 file dialogs (`OPENFILENAMEW`), `ShellExecuteW`, `C:/Windows/Fonts`, Win32 icon; Wayland quirks; no HiDPI |
| **7. Project Serialization & Assets** | Implemented (`.viz`/`.iota`/`.json`) | **Core portable; audio/fonts break** | Absolute `audioPath` breaks across OSes; Windows font names in schema; `LC_NUMERIC` `snprintf` vs `from_chars` hazard |
| **8. MCP & Networking** | Implemented (Stdio + Named Pipes) | **Bridge fails to compile** | `live_bridge.cpp` is Win32 Named Pipes only; headless GL needs display/`xvfb-run`; `SIGPIPE` risk; no VA-API tool |
| **9. Packaging & Distribution** | Implemented (Inno Setup 6) | **Not implemented** | No CMake `install()`, `.desktop`, AppStream, hicolor icon, AppImage/Flatpak/native pkg, or CI build/test |
| **10. Testing & Verification** | Primary dev suite (12 test files) | **Partial compile/run failures** | `test_mcp.cpp` Win32-only; `test_engine_gl`/`test_model_preset_io` `L"..."` & DirectWrite checks; `ffmpeg.exe` test |

---

## 2. Current Windows Compatibility

Windows (x64, MSVC + Ninja) is the primary implemented platform. **Note:** Because this audit was executed in a Linux environment, Windows runtime behavior is **code-reviewed but unverified** in this session.

### Implemented Windows Capabilities (Observed in Code)
- **Build & Packaging:** `CMakeLists.txt`, `CMakePresets.json` (`windows-msvc-ninja-{debug,release}{,-mcp}`), `build.ps1`, `apps/studio/resources/iota_studio.rc.in` (icon + version resource), and Inno Setup 6 installer (`installer/iota_studio.iss`, `installer/build_installer.ps1`).
- **Graphics & Text:** OpenGL 4.5 Core loader via `wglGetProcAddress` + `GetModuleHandleA("opengl32.dll")` (`libs/rhi_gl45/src/gl_loader.cpp`); DirectWrite shaping, system fallback, and in-memory custom font collection loading (`libs/engine/src/font_atlas.cpp`).
- **Audio & Export:** `miniaudio` playback (`libs/audio/src/audio_engine.cpp`), `ffmpeg.exe`/`ffprobe.exe` decoding via `CreateProcessW` with `ma_decoder_init_file_w` fallback (`libs/audio/src/audio_decoder.cpp`), and `ffmpeg.exe` video export supporting NVENC, AMF, QSV, and CPU (`libs/export/src/video_exporter.cpp`).
- **Desktop & System Integration:** `OPENFILENAMEW` open/multi-open/save file dialogs, `LoadImageW` + `WM_SETICON`, `GetWindowsDirectoryW` + `C:/Windows/Fonts` UI fonts, `ShellExecuteW` URL opening (`apps/studio/src/editor_app.cpp`); WinHTTP GitHub release update check (`libs/core/src/update_checker.cpp`); Discord Social SDK with `/DELAYLOAD:discord_partner_sdk.dll` (`libs/discord/`); Win32 Named Pipe MCP Live Bridge (`\\.\pipe\iota_studio_mcp_<user>` in `libs/mcp/src/live_bridge.cpp`).

### Windows Gaps & Caveats Discovered During Audit
1. **No HiDPI / DPI Awareness (`apps/studio/resources/iota_studio.rc.in`, `apps/studio/src/editor_app.cpp`):** No DPI-awareness manifest is embedded and `glfwGetWindowContentScale` / `ImGuiStyle::ScaleAllSizes` are never called, causing blurry scaling or fixed pixel sizing on high-DPI Windows displays.
2. **Console Subsystem Binary (`apps/studio/CMakeLists.txt:16`):** `add_executable(iota_studio ...)` omits `WIN32`, so launching `iota_studio.exe` from Explorer opens a background console window alongside the GUI.
3. **MSVC-gated Win32 Macros (`CMakeLists.txt:189-192`):** `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, and `_CRT_SECURE_NO_WARNINGS` are added under `if(MSVC)` rather than `if(WIN32)`, which breaks MinGW or Clang-GNU builds on Windows.
4. **Multi-Config Discord SDK Selection (`CMakeLists.txt:219-224`):** Uses `CMAKE_BUILD_TYPE` (empty in Visual Studio / Ninja Multi-Config generators), causing Debug multi-config builds to link Release Discord SDK binaries.
5. **Windows ARM64 Discord SDK Ignored (`CMakeLists.txt:226-242`):** `third_party/discord_social_sdk/{lib,bin}/{debug,release}/arm64/` exists on disk, but CMake always selects the x64 root `discord_partner_sdk.{lib,dll}`.

---

## 3. Current Linux Compatibility

### Observed Facts in the Linux Environment
- **Host Environment:** `Linux 7.3.6-zen1-1-zen x86_64`, `g++ (GCC) 16.2.1`, `cmake 4.5.1`, `ninja 1.13.2`, with `/usr/bin/ffmpeg`, `/usr/bin/zenity`, `/usr/bin/kdialog`, and `/usr/bin/xdg-open` present.
- **CMake Configure Status (Verified):** `build/linux-release/CMakeCache.txt` confirms CMake configuration **succeeds** on Linux using the `linux-gcc-ninja-release` preset (`CMakePresets.json:56-64`), resolving all 10 `FetchContent` dependencies and configuring GLFW 3.4 with both `GLFW_BUILD_WAYLAND=ON` and `GLFW_BUILD_X11=ON`. Only `libminiz.a` and `libkissfft_real.a` were built in that tree.
- **Dry-Run Compilation Checks (Verified):**
  - `libs/core` (`utf8_path.cpp`, `zip_archive.cpp`, `types.cpp`, `procedural_assets.cpp`) and `libs/discord/src/discord_presence.cpp` (both stub mode and `-DIOTA_HAS_DISCORD_SDK=1`) compile cleanly with 0 warnings on GCC 16.2 (`-std=c++20 -Wall -Wextra -Wpedantic`).
  - Clang (`-std=c++20 -Wall -Wextra -Wpedantic`) compiles `libs/core/src/update_checker.cpp` with a `-Wunused-lambda-capture` warning on `preferMcpAsset` at line 588.
- **Third-Party Binary Inspection (Verified):** `third_party/discord_social_sdk/lib/release/libdiscord_partner_sdk.so` is a 14.6 MB **x86-64 ELF** shared library (`SONAME libdiscord_partner_sdk.so`, no `RPATH`/`RUNPATH`) requiring `GLIBC_2.29` and `DT_NEEDED` entries including `libpulse.so.0`, `libasound.so.2`, `libX11.so.6`, `libatomic.so.1`, `libpthread.so.0`, `libdl.so.2`, `librt.so.1`, `libm.so.6`.

### Linux Compatibility Summary
- **Cannot Build Today:** Compilation fails immediately on 6 files (`gl_loader.hpp`, `audio_decoder.cpp`, `video_exporter.cpp`, `editor_app.cpp`, `live_bridge.cpp`, `test_mcp.cpp`), and linking fails on `opengl32`, `comdlg32`, and `shell32`.
- **Portable Modules Ready or Nearly Ready:** `libs/model`, `libs/dsp`, `libs/rhi`, `libs/core` (except `update_checker.cpp` networking), `libs/preset_io` (aside from XDG fallback & path case-folding), `libs/audio/src/audio_engine.cpp` (once `miniaudio` Linux libs are linked), `libs/engine/src/{layout,particle_system,shake_eval,scene_renderer}.cpp`, `libs/discord`, `libs/mcp/src/{mcp_server,tool_registry,working_copy,png_encode,studio_session}.cpp`, `apps/mcp/src/main.cpp`, and Studio ImGui panels (`hierarchy`, `sound_keys`, `shader_editor`, `asset_browser`, `docs`, `project_manager_modal`, `update_modal`).
- **Broken or Missing at Linux Runtime (Even After Build Fixes):** Text rendering outputs striped boxes; `UpdateChecker` returns an error; built-in shaders have GLSL UB on Mesa/AMD/Intel; no file dialogs, URL opener, UI fonts, or window icon; no VA-API export; `libdiscord_partner_sdk.so` prevents startup if audio/X11 system libs are missing.

---

## 4. Confirmed Compatibility Issues

### C-01 — Hardcoded Windows `opengl32` in CMake
- **Severity:** Critical *(P0)*
- **Affected subsystem:** Build System & Rendering (`imgui_docking`, `iota_rhi_gl45`)
- **Relevant file paths and line numbers:** `CMakeLists.txt:157`; `libs/rhi_gl45/CMakeLists.txt:26-31`
- **Explanation:** Both targets unconditionally link `opengl32` (`target_link_libraries(imgui_docking PUBLIC glfw opengl32)` and `target_link_libraries(iota_rhi_gl45 PUBLIC iota_rhi PRIVATE opengl32)`). `find_package(OpenGL REQUIRED)` is never called. On Linux `libopengl32` does not exist, causing an immediate linker failure.
- **Status:** Confirmed
- **Recommended solution:** Call `find_package(OpenGL REQUIRED)` in root `CMakeLists.txt` and link `OpenGL::GL` instead of `opengl32` on both targets.
- **Requires platform-specific code:** No (CMake's `OpenGL::GL` imported target abstracts `opengl32` on Windows and `libGL`/`libOpenGL` on Linux).

### C-02 — Unconditional Win32 Libraries (`comdlg32`, `shell32`) in `iota_studio`
- **Severity:** Critical *(P0)*
- **Affected subsystem:** Build System / Studio UI
- **Relevant file paths and line numbers:** `apps/studio/CMakeLists.txt:63-64`
- **Explanation:** `target_link_libraries(iota_studio PRIVATE ... comdlg32 shell32)` is outside any `if(WIN32)` check. Linux linkers fail with `cannot find -lcomdlg32` and `cannot find -lshell32`.
- **Status:** Confirmed
- **Recommended solution:** Move `comdlg32` and `shell32` into an `if(WIN32)` block (and add any Linux desktop/font libraries under `if(UNIX AND NOT APPLE)`).
- **Requires platform-specific code:** Yes (CMake `if(WIN32)` / `if(UNIX AND NOT APPLE)`).

### C-03 — Win32-Only OpenGL Symbol Loader (`gl_loader.hpp` / `gl_loader.cpp`)
- **Severity:** Critical *(P0)*
- **Affected subsystem:** Rendering & Graphics (`libs/rhi_gl45`)
- **Relevant file paths and line numbers:** `libs/rhi_gl45/src/gl_loader.hpp:18, 151-210`; `libs/rhi_gl45/src/gl_loader.cpp:24-40, 51, 58`
- **Explanation:**
  1. `gl_loader.hpp:18` unconditionally includes `<windows.h>`.
  2. `gl_loader.hpp:151-210` declares all 51 GL function pointers with `APIENTRY`, which is defined by `<windows.h>` on Windows but is not guaranteed to be defined by `<GL/gl.h>` on all Linux Mesa/libglvnd configurations.
  3. `gl_loader.cpp:24-40` implements `resolveGlSymbol()` solely via `wglGetProcAddress()`, `HMODULE`, `GetModuleHandleA("opengl32.dll")`, and `GetProcAddress()`.
  4. `gl_loader.cpp:25, 34, 51, 58` casts between `void*` and function pointers, violating ISO C++ under `-Wpedantic` (`CMakeLists.txt:193`).
- **Status:** Confirmed
- **Recommended solution:** Guard `<windows.h>` with `#ifdef _WIN32`; add `#ifndef APIENTRY` fallback (`#ifdef GLAPIENTRY #define APIENTRY GLAPIENTRY #else #define APIENTRY #endif`); replace `resolveGlSymbol` with `glfwGetProcAddress` (or a callback injected from the context creator) returning `void(*)()`, which works across WGL (Windows), GLX (Linux X11), and EGL (Linux Wayland) and eliminates `void*` function-pointer casts. Note: `glXGetProcAddress` alone would break Wayland.
- **Requires platform-specific code:** Yes (minimal `#ifdef _WIN32` / `APIENTRY` guards; `glfwGetProcAddress` itself is cross-platform).

### C-04 — Win32-Only Audio Decoder Subprocess & `ma_decoder_init_file_w` Compile Error
- **Severity:** Critical *(P0)*
- **Affected subsystem:** Audio System (`libs/audio`)
- **Relevant file paths and line numbers:** `libs/audio/src/audio_decoder.cpp:22, 34-122, 133-136, 169-172, 217`
- **Explanation:**
  1. Unconditional `#include <windows.h>` (line 22) and Win32-only `quoteWindowsArg` / `runChildCaptureStdout` using `CreateProcessW`, `CreatePipe`, `ReadFile`, `WaitForSingleObject`, `GetExitCodeProcess` (lines 34-122).
  2. `probeTagsWithFfprobe` (lines 133-136) and `decodeWithFfmpeg` (lines 169-172) hardcode `ffprobe.exe` and `ffmpeg.exe` with `path.wstring()`.
  3. Line 217 calls `ma_decoder_init_file_w(path.c_str(), &decCfg, &decoder)`. On Linux `std::filesystem::path::c_str()` returns `const char*`, not `const wchar_t*`, causing a hard compile error.
- **Status:** Confirmed
- **Recommended solution:** Gate `ma_decoder_init_file_w` under `#ifdef _WIN32` and use `ma_decoder_init_file(path.c_str(), &decCfg, &decoder)` on POSIX (native UTF-8). Isolate the Win32 `CreateProcessW` runner under `#ifdef _WIN32` and implement a POSIX `pipe()` + `fork()` + `execvp()` (or `posix_spawn`) runner invoking `"ffmpeg"` / `"ffprobe"` with an `argv` array (no shell quoting) and redirecting stderr to `/dev/null`.
- **Requires platform-specific code:** Yes.

### C-05 — Missing Linux Linker Libraries (`dl`, `pthread`, `m`) for `miniaudio` and Threaded Libraries
- **Severity:** Critical *(P0)*
- **Affected subsystem:** Build System / Audio / Core / Export / DSP
- **Relevant file paths and line numbers:** `libs/audio/CMakeLists.txt:26-31`; `CMakeLists.txt:171-185`; `libs/core/CMakeLists.txt:26-28`; `libs/export/CMakeLists.txt`
- **Explanation:** `libs/audio/src/audio_engine.cpp:16` compiles `MA_IMPLEMENTATION` (`miniaudio` 0.11.26), which on Linux dynamically loads PulseAudio/ALSA/JACK via `dlopen`/`dlsym`, creates threads via `pthread_*`, and uses `<math.h>`. `kissfft_real` (`kiss_fft.c`) calls `cos`/`sin`, and `iota_core` / `iota_export` use `std::thread`. None of these link `${CMAKE_DL_LIBS}`, `Threads::Threads`, or `m` on Linux. While glibc >= 2.34 merges `libpthread`/`libdl` into `libc`, older distros (Ubuntu 20.04, Debian 11) and strict linkers fail with unresolved symbol errors.
- **Status:** Confirmed
- **Recommended solution:** Call `find_package(Threads REQUIRED)` and under `if(UNIX AND NOT APPLE)` link `Threads::Threads`, `${CMAKE_DL_LIBS}`, and `m` to `iota_audio` / `miniaudio_hdr`, `m` to `kissfft_real`, and `Threads::Threads` to `iota_core` and `iota_export`.
- **Requires platform-specific code:** Yes (CMake `if(UNIX AND NOT APPLE)`).

### C-06 — Completely Win32-Only Video Exporter (`video_exporter.hpp` / `video_exporter.cpp`)
- **Severity:** Critical *(P0)*
- **Affected subsystem:** Video Export (`libs/export`)
- **Relevant file paths and line numbers:** `libs/export/include/iota/export/video_exporter.hpp:107`; `libs/export/src/video_exporter.cpp:20, 37-62, 205-353, 361, 385, 645-693, 725, 796, 825-869, 940-1015, 1067-1127`
- **Explanation:**
  1. Unconditional `#include <windows.h>` (line 20) and pervasive Win32 handles/APIs (`HANDLE`, `DWORD`, `CreatePipe`, `PeekNamedPipe`, `ReadFile`, `WriteFile`, `CreateFileW(L"NUL", ...)`, `CreateProcessW`, `TerminateProcess`, `WaitForSingleObject`, `GetExitCodeProcess`, `GetTickCount64`).
  2. Hardcodes `ffmpeg.exe` in command strings and error messages (`lines 294, 296, 361, 385, 552, 594, 663, 1002, 1004, 1122`).
  3. Public API `buildFfmpegExportCommand(...)` (`video_exporter.hpp:107`) returns a Win32-quoted `std::wstring` (`quoteWindowsArg`, `audioDiskPath.wstring()`, `settings.outputPath.wstring()`), incompatible with POSIX `execvp`/`posix_spawn` `char* const argv[]`.
- **Status:** Confirmed
- **Recommended solution:** Add a cross-platform `std::vector<std::string>` `argv` builder (`buildFfmpegExportArgs`); keep Win32 `CreateProcessW` under `#ifdef _WIN32`; implement a POSIX backend using `pipe()` (`O_NONBLOCK` on stderr read end), `fork()`/`execvp("ffmpeg", ...)` (or `posix_spawn`), `/dev/null` for stdout, `poll()` + `waitpid(..., WNOHANG)` + `kill(pid, SIGKILL)` + `std::chrono::steady_clock`, and `SIGPIPE` suppression (`MSG_NOSIGNAL` / `SIG_IGN`). Also resolve `ffmpeg` in the executable directory on Linux if bundled alongside the binary, before falling back to `PATH`.
- **Requires platform-specific code:** Yes.

### C-07 — Unconditional Win32 APIs in Studio UI (`editor_app.cpp`)
- **Severity:** Critical *(P0)*
- **Affected subsystem:** UI & Desktop Integration (`apps/studio`)
- **Relevant file paths and line numbers:** `apps/studio/src/editor_app.cpp:24-30, 56-143, 179-195, 216-248, 304-311, 1933-1971, 2434-2438, 2544-2593, 2786-2789, 2840-2862, 2975-3034`; `apps/studio/src/editor_app.hpp:203-204`
- **Explanation:**
  1. Unconditional `#include <windows.h>`, `<commdlg.h>`, `<shellapi.h>`, `#define GLFW_EXPOSE_NATIVE_WIN32`, and `<GLFW/glfw3native.h>` (lines 24-30).
  2. Native open, multi-open, and save file dialogs (lines 56-143, plus 11 call sites) use `OPENFILENAMEW`, `GetOpenFileNameW`, `GetSaveFileNameW`, `glfwGetWin32Window`, and MSVC-only `wcsncpy_s` (line 81).
  3. Window icon loading/destruction (lines 179-195, 304-311) uses `LoadImageW`, `SendMessageW(WM_SETICON)`, and `DestroyIcon`.
  4. ImGui UI and code font loading (lines 216-248) uses `GetWindowsDirectoryW` and hardcodes `C:/Windows/Fonts` + Windows font filenames (`segoeui.ttf`, `YuGothM.ttc`, `consola.ttf`, etc.).
  5. `openExternalUrl` (lines 2840-2862) uses `MultiByteToWideChar` and `ShellExecuteW`.
- **Status:** Confirmed
- **Recommended solution:** Extract a small `platform_desktop` abstraction with Windows and Linux implementations:
  - File dialogs: Native File Dialog Extended (`nfd` via `xdg-desktop-portal` / GTK) or portal D-Bus / `zenity`/`kdialog` fallback on Linux.
  - External URLs: `fork` + `execvp("xdg-open", ...)` on Linux (avoiding shell injection from `system()`).
  - ImGui fonts: Fontconfig (`FcPattern`/`FcFontMatch`) or `/usr/share/fonts` fallback search (e.g., Noto Sans / DejaVu Sans / Liberation Sans + Noto Sans CJK + monospace font for `codeFont_`).
  - Window icon: Decode `assets/icon.png` with `stb_image` and call `glfwSetWindowIcon` **only on X11** (`glfwGetPlatform() == GLFW_PLATFORM_X11`, as Wayland does not support client-set icons), plus `GLFW_WAYLAND_APP_ID` + `.desktop` icon installation.
- **Requires platform-specific code:** Yes.

### C-08 — Win32 Named Pipes Only in MCP Live Bridge (`live_bridge.cpp`)
- **Severity:** Critical *(P0 when `IOTA_BUILD_MCP=ON`)*
- **Affected subsystem:** MCP & Networking (`libs/mcp`)
- **Relevant file paths and line numbers:** `libs/mcp/src/live_bridge.cpp:18, 38-187, 246-266, 300-312, 355-356, 439-461, 628-658`
- **Explanation:** Unconditionally includes `<windows.h>` and implements `LiveBridgeHost` / `LiveBridgeClient` exclusively using Win32 Named Pipes (`CreateNamedPipeW` with `PIPE_REJECT_REMOTE_CLIENTS` / `FILE_FLAG_FIRST_PIPE_INSTANCE`, `OVERLAPPED`, `CreateEventW`, `GetUserNameW`, `WaitNamedPipeW`, `\\.\pipe\iota_studio_mcp_<user>`). Note: No TCP sockets are used anywhere in `libs/mcp/`.
- **Status:** Confirmed
- **Recommended solution:** Preserve the 4-byte little-endian length-prefixed JSON framing protocol; isolate Named Pipes under `#ifdef _WIN32` and add a POSIX `AF_UNIX` `SOCK_STREAM` transport (`$XDG_RUNTIME_DIR/iota_studio_mcp.sock`, fallback `/tmp/iota_studio_mcp_<uid>.sock`, checking `sun_path` 108-byte limit, `0600` permissions, stale-socket probe/unlink on `EADDRINUSE`+`ECONNREFUSED`, `poll()` + `eventfd()`/`pipe()` wakeup, `SOCK_CLOEXEC`, and `MSG_NOSIGNAL`).
- **Requires platform-specific code:** Yes.

### C-09 — Win32-Only MCP Automated Test (`tests/test_mcp.cpp`)
- **Severity:** Critical *(P0 when `IOTA_BUILD_MCP=ON`)*
- **Affected subsystem:** Testing & Verification (`tests/`)
- **Relevant file paths and line numbers:** `tests/test_mcp.cpp:25, 42, 280, 326-399, 525`
- **Explanation:** Unconditionally includes `<windows.h>`, calls `GetCurrentProcessId()`, hardcodes `\\.\pipe\iota_studio_mcp_test_...` pipe names, and spawns `iota_mcp` via Win32 `CreatePipe` + `CreateProcessW` + `WaitForSingleObject`.
- **Status:** Confirmed
- **Recommended solution:** Guard Win32 process/PID code with `#ifdef _WIN32`; use `getpid()`, temporary Unix domain socket paths, and POSIX `pipe()`/`fork()`/`execv()`/`waitpid()` on `#else`.
- **Requires platform-specific code:** Yes.

### C-10 — DirectWrite-Only Typography Engine Renders Unreadable Placeholder Boxes on Linux
- **Severity:** Critical *(P1)*
- **Affected subsystem:** Rendering & Graphics / Text Engine (`libs/engine`)
- **Relevant file paths and line numbers:** `libs/engine/src/font_atlas.cpp:25-32, 82-108, 475-502, 583-651, 713-750, 979-1046, 1121-1411`; `libs/engine/include/iota/engine/font_atlas.hpp:92-93`
- **Explanation:**
  1. Despite `font_atlas.hpp:92-93` documenting an `stb_truetype` fallback and `font_atlas.cpp:28-29` defining `STB_TRUETYPE_IMPLEMENTATION`, **`stb_truetype` is never called anywhere**.
  2. All font discovery, `local:` embedded project font loading, disk `.ttf`/`.otf` loading, Unicode/CJK/RTL shaping, alignment, word-wrapping, letter/line spacing, outlines, and drop shadows are gated under `#ifdef _WIN32` DirectWrite.
  3. On Linux, `buildTextQuads()` unconditionally executes `buildFallbackQuads()` (`lines 979-1046`) using `buildFallbackBitmapAtlas()` (`lines 476-502`), which does not even render real glyphs—it draws striped placeholder rectangles (`border || ((px + py + c) % 3) == 0`) for ASCII 32..126 and skips all non-ASCII characters (`line 1021`).
  4. Additionally, `readFileBytes` (`lines 82-97`), `quantizeFontBucketPx` (`lines 99-108`), and parameters `typeFaceKey`/`style` (`lines 1121-1128`) are unused on Linux, triggering `-Wunused-function` and `-Wunused-parameter` under `-Wall -Wextra`.
- **Status:** Confirmed
- **Recommended solution:** Implement a Linux font backend using **Fontconfig + FreeType + HarfBuzz** (for full parity: system font discovery, family fallback, `local:` memory font loading, Unicode/RTL/CJK shaping, outlines, and shadows), or wire `stb_truetype` + Fontconfig as an interim fallback if minimal dependencies are preferred initially (noting `stb_truetype` lacks RTL/complex-script shaping and system font fallback).
- **Requires platform-specific code:** Yes (Fontconfig on Linux vs DirectWrite on Windows; FreeType+HarfBuzz can be Linux-only or shared).

### C-11 — GLSL 4.50 Spec Undefined Behavior in 5 Built-In Shaders
- **Severity:** High *(P1)*
- **Affected subsystem:** Rendering & Graphics / Shaders (`libs/engine`)
- **Relevant file paths and line numbers:** `libs/engine/src/builtin_shader_library.cpp:56, 161, 464, 696, 809`
- **Explanation:**
  1. **Line 56 (`kShaderBassShockwave`):** `exp(-pow((dist - radius) / max(RING_WIDTH, 0.02), 2.0))` passes a negative base whenever `dist < radius`. Per GLSL 4.50 §8.2, `pow(x, y)` is undefined for `x < 0` (`exp2(y * log2(x))` -> `NaN`). NVIDIA Windows optimizes `pow(x, 2.0)` to `x * x`, hiding the bug; Mesa (Iris/RadeonSI/llvmpipe) and AMD drivers can output `NaN` across the shockwave interior.
  2. **Lines 161 (`kShaderAudioWarp`: `smoothstep(0.65, 0.0, r)`), 464 (`kShaderHyperdriveTunnel`: `smoothstep(0.32, 0.0, localA)`), 696 (`kShaderSynthwaveGrid`: `smoothstep(0.185, 0.180, sunDist)`), 809 (`kShaderHalftoneDither`: `smoothstep(radius + 0.06, radius - 0.06, dist)`):** Per GLSL 4.50 §8.3, `smoothstep(edge0, edge1, x)` is undefined when `edge0 >= edge1`. Strict Mesa/AMD/Intel drivers on Linux can invert, clamp, or produce garbage for inverted edges.
- **Status:** Confirmed
- **Recommended solution:** Replace `pow(d, 2.0)` with `d * d`, and replace every `smoothstep(hi, lo, x)` with `1.0 - smoothstep(lo, hi, x)`.
- **Requires platform-specific code:** No.

### C-12 — Missing `$HOME/.local/share` XDG Fallback in `ProjectLibrary::defaultRegistryPath()`
- **Severity:** High *(P1)*
- **Affected subsystem:** Filesystem & Path Portability (`libs/preset_io`)
- **Relevant file paths and line numbers:** `libs/preset_io/src/project_manager.cpp:192-203`
- **Explanation:** On non-Windows platforms, lines 198-200 check `std::getenv("XDG_DATA_HOME")`, and if unset (which is the default on nearly all Linux desktops), line 202 falls back directly to the CWD-relative path `iota_projects.json`. Launching Iota Studio from a desktop launcher (CWD = `$HOME` or `/`) or read-only directory either scatters files or fails to save recent projects/settings (including Discord/import preferences).
- **Status:** Confirmed
- **Recommended solution:** Per the XDG Base Directory Specification, when `XDG_DATA_HOME` is unset or empty, fall back to `$HOME/.local/share/IotaStudio/projects.json` before falling back to a relative path.
- **Requires platform-specific code:** Yes (inside existing `#else` branch).

### C-13 — Unconditional Path Lowercasing Breaks Case-Sensitive Linux Filesystems
- **Severity:** High *(P1)*
- **Affected subsystem:** Filesystem & Path Portability (`libs/preset_io`)
- **Relevant file paths and line numbers:** `libs/preset_io/src/project_manager.cpp:36-42, 317-322, 330-336`
- **Explanation:** `normalizePathLower()` lowercases every path character unconditionally on all OSes. On Linux (`ext4`/`btrfs`/`xfs`), `/home/user/Track.viz` and `/home/user/track.viz` are distinct files, yet `recordOpened()` and `removeEntry()` treat them as identical, overwriting or deleting the wrong recent-project entry.
- **Status:** Confirmed
- **Recommended solution:** Only lowercase ASCII path characters under `#ifdef _WIN32`; on Linux normalize via `lexically_normal()` preserving original case.
- **Requires platform-specific code:** Yes.

### C-14 — Missing Linux HTTPS Client & Windows-Only `.exe` Asset Scoring in `UpdateChecker`
- **Severity:** High *(P1)*
- **Affected subsystem:** Platform APIs & Networking (`libs/core`)
- **Relevant file paths and line numbers:** `libs/core/src/update_checker.cpp:476-514, 588, 598-644`; `libs/core/CMakeLists.txt:26-28`
- **Explanation:**
  1. On non-Windows platforms, `startCheckAsync()` immediately fails with `"Automatic update checks are only supported on Windows."` (`line 643`).
  2. Lambda capture `preferMcpAsset` (`line 588`) is unused in the `#else` branch, triggering `-Wunused-lambda-capture` on Clang.
  3. `parseGitHubReleaseJson()` (`lines 476-514`) scores `.exe` assets (score 4–5) and `.zip` assets (score 2–3) above everything else. If a release includes both Windows and Linux artifacts, Linux users are directed to download the `.exe`.
- **Status:** Confirmed
- **Recommended solution:** Implement an HTTPS client on Linux (e.g., `libcurl` via `find_package(CURL)`), fix the unused capture, and make asset extension scoring platform-aware (prefer `.AppImage`, `.tar.gz`/`.tar.xz`, `.deb`/`.rpm` on Linux; update `tests/test_update_checker.cpp` accordingly).
- **Requires platform-specific code:** Yes.

### C-15 — Discord Social SDK CMake Detection Lacks CPU Architecture Filtering
- **Severity:** High *(P1)*
- **Affected subsystem:** Build System & Third-Party SDK (`CMakeLists.txt`, `third_party/discord_social_sdk`)
- **Relevant file paths and line numbers:** `CMakeLists.txt:226-271`
- **Explanation:** `third_party/discord_social_sdk/lib/{debug,release}/libdiscord_partner_sdk.so` is **x86-64 ELF only**, while `CMakeLists.txt:257-271` links it on any `UNIX` host regardless of `CMAKE_SYSTEM_PROCESSOR`. On Linux `aarch64`, linking fails against the x86-64 `.so`. Similarly, on Windows ARM64, `CMakeLists.txt:227-242` ignores the `arm64/` subdirectory that exists inside `third_party/discord_social_sdk/{lib,bin}/{debug,release}/`.
- **Status:** Confirmed
- **Recommended solution:** Check `CMAKE_SYSTEM_PROCESSOR` (`x86_64`/`AMD64` vs `arm64`/`aarch64`); select the `arm64/` subfolder on Windows ARM64 and gracefully fall back to Discord stub mode on Linux architectures lacking a `.so`.
- **Requires platform-specific code:** Yes (CMake platform/arch conditions).

### C-16 — Automated Tests Fail on Linux Due to DirectWrite Assertions, `L"..."` Wide Paths, and `ffmpeg.exe` Checks
- **Severity:** High *(P1)*
- **Affected subsystem:** Testing & Verification (`tests/`)
- **Relevant file paths and line numbers:** `tests/test_engine_gl.cpp:342, 1048, 1102, 1109, 1123, 1129, 1177`; `tests/test_model_preset_io.cpp:309-317`; `tests/test_video_export.cpp:138, 165-200`; `tests/CMakeLists.txt:55-66`
- **Explanation:**
  1. `test_engine_gl.cpp:1102` asserts `CHECK_MESSAGE(st.fontUsingDirectWrite, ...)` unconditionally (failing on Linux), and lines `1048, 1109, 1123, 1129, 1177` request Windows-only fonts (`Segoe UI`, `Impact`, `C:/Windows/Fonts/consola.ttf`).
  2. `test_engine_gl.cpp:342` and `test_model_preset_io.cpp:309-317` construct `std::filesystem::path(L"iota_za\u017C...")` with non-ASCII wide literals. On Linux under the default `"C"` locale, `std::filesystem::path(const wchar_t*)` throws `std::filesystem::filesystem_error` during codecvt narrowing.
  3. `test_video_export.cpp:138` asserts `"ffmpeg.exe"` in error output and lines `165-200` test `buildFfmpegExportCommand(...)` returning `std::wstring`.
  4. `tests/CMakeLists.txt:55-62` copies `libdiscord_partner_sdk.so` next to `iota_tests` without setting `BUILD_RPATH "$ORIGIN"`, and line `66` (`add_test`) does not pass `IOTA_TEST_PRESETS_DIR`.
- **Status:** Confirmed
- **Recommended solution:** Construct Unicode test paths via `iota::core::pathFromUtf8(...)`; gate DirectWrite-specific checks or assert the active platform typography backend with cross-platform/bundled test fonts; update `test_video_export` for `argv` vector and platform binary name; set `BUILD_RPATH "$ORIGIN"` on `iota_tests` for Linux.
- **Requires platform-specific code:** Partial (test assertions for platform-specific typography/executable names).

### C-17 — External Audio Paths (`audioPath`, `audioAssets`) Break `.viz`/`.iota` Portability Across OSes
- **Severity:** High *(P2)*
- **Affected subsystem:** Project Serialization & Asset Management (`libs/model`, `libs/preset_io`, `apps/studio`)
- **Relevant file paths and line numbers:** `libs/model/include/iota/model/scene.hpp:86, 92`; `libs/preset_io/src/preset_io.cpp:280, 298-304, 341, 347`; `apps/studio/src/editor_app.cpp:2610, 2929-2935, 3039-3042`
- **Explanation:** Unlike images, fonts, and custom shaders (which can be embedded via `local:` inside `.viz`/`.iota` ZIP archives), `metadata.audioPath` and `metadata.audioAssets` store raw OS-specific absolute paths. A Windows project containing `C:\Users\Alice\Music\track.mp3` fails on Linux (`pathFromUtf8` treats it as a literal relative filename in CWD, failing `pathExistsNoThrow`), and `/home/alice/track.mp3` fails on Windows.
- **Status:** Confirmed
- **Recommended solution:** Store project-relative POSIX `/` paths alongside (or instead of) raw absolute paths when possible, check the project directory as a fallback when loading missing audio, and optionally support audio bundling or a missing-audio relink prompt.
- **Requires platform-specific code:** No (portable `std::filesystem::relative` + generic `/` path serialization).

### C-18 — Windows-Only System Font Family Names in Scene Schema & Inspector UI
- **Severity:** Medium *(P2)*
- **Affected subsystem:** Serialization / UI / Typography (`libs/model`, `apps/studio`, `libs/engine`)
- **Relevant file paths and line numbers:** `libs/model/src/scene.cpp:497-499`; `apps/studio/src/inspector_panel.cpp:782-787`; `libs/engine/src/font_atlas.cpp:237-246, 1210`
- **Explanation:** Default Text element metadata (`scene.cpp:498`) and the Inspector font dropdown (`inspector_panel.cpp:782-787`) hardcode Windows proprietary font names (`Segoe UI`, `Bahnschrift`, `Impact`, `Consolas`, `Georgia`, `Segoe Script`, `Arial`, `Yu Gothic UI`, `Malgun Gothic`, `Microsoft YaHei UI`). Projects using these system presets will miss fonts on standard Linux installs.
- **Status:** Confirmed
- **Recommended solution:** Map Windows family presets (`Segoe UI`, `Consolas`, `Georgia`, `Impact`, `Bahnschrift`, etc.) to Linux Fontconfig/metric-compatible equivalents (e.g., Noto Sans / Liberation Sans / DejaVu Sans Mono / Liberation Serif), and populate or supplement the Inspector picker dynamically on Linux.
- **Requires platform-specific code:** Yes.

### C-19 — Compiler Warnings & Non-`SYSTEM` Third-Party Headers Under GCC/Clang `-Wall -Wextra -Wpedantic`
- **Severity:** Medium *(P2)*
- **Affected subsystem:** Build System & Code Hygiene
- **Relevant file paths and line numbers:** `CMakeLists.txt:171-185, 193, 238, 252, 266`; `libs/engine/src/scene_renderer.cpp:27-34`; `libs/engine/src/font_atlas.cpp:25-32, 82-108, 1121-1128`; `libs/dsp/src/sample_providers.cpp:21-27`; `libs/core/src/update_checker.cpp:588`
- **Explanation:** `miniaudio_hdr`, `stb_hdr`, `kissfft_real`, and `discord_social_sdk` expose headers without `SYSTEM`, while warning suppression pragmas (`scene_renderer.cpp`, `font_atlas.cpp`, `sample_providers.cpp`) are guarded only with `#ifdef _MSC_VER`. Combined with `-Wall -Wextra -Wpedantic` (`CMakeLists.txt:193`), GCC/Clang emits third-party warnings from `miniaudio.h`, `stb_image.h`, `stb_truetype.h`, and `kiss_fftr.h`, plus unused-function/parameter/capture warnings in `font_atlas.cpp` and `update_checker.cpp`.
- **Status:** Confirmed
- **Recommended solution:** Add `SYSTEM` to third-party `target_include_directories`, guard `#ifdef _WIN32`-only helpers in `font_atlas.cpp`, and fix the unused lambda capture in `update_checker.cpp:588`.
- **Requires platform-specific code:** Minimal (`#ifdef _WIN32` guards; `SYSTEM` in CMake is cross-platform).

---

## 5. Potential Compatibility Risks

### R-01 — `libdiscord_partner_sdk.so` Hard Load-Time Dependency (`DT_NEEDED`) on Linux
- **Severity:** High *(P1)*
- **Affected subsystem:** Platform Integration / Packaging (`libs/discord`, `CMakeLists.txt`)
- **Relevant file paths and line numbers:** `CMakeLists.txt:257-270`; `libs/discord/CMakeLists.txt:31-34`; `libs/discord/src/discord_presence.cpp:282-297, 568-581`
- **Explanation:** On Windows, `libs/discord/CMakeLists.txt:31-34` uses `/DELAYLOAD:discord_partner_sdk.dll` and `discord_presence.cpp` probes with `LoadLibraryW`, allowing graceful fallback to `SdkUnavailable` if the DLL is absent. On Linux there is no `/DELAYLOAD` equivalent: `libdiscord_partner_sdk.so` is a hard `DT_NEEDED` dependency whose own `DT_NEEDED` list (`libpulse.so.0`, `libasound.so.2`, `libX11.so.6`, `libatomic.so.1`, `GLIBC_2.29`) must all be present or the dynamic linker refuses to start `iota_studio` / `iota_tests` (affecting headless CI, Wayland-only systems without X11/PulseAudio, or glibc < 2.29 distros).
- **Status:** Confirmed (binary `DT_NEEDED` verified) / Suspected runtime impact on minimal Linux hosts
- **Recommended solution:** Load `libdiscord_partner_sdk.so` via `dlopen`/`dlsym` on Linux (matching Windows `LoadLibraryW` behavior) or disable `IOTA_ENABLE_DISCORD_RPC` by default on Linux unless explicitly requested/bundled.
- **Requires platform-specific code:** Yes.

### R-02 — Locale-Dependent `snprintf`/`swprintf` Float Formatting vs `std::from_chars` (`LC_NUMERIC` Hazard)
- **Severity:** High *(P2)*
- **Affected subsystem:** Serialization / Core / Video Export
- **Relevant file paths and line numbers:** `libs/model/src/measured_var.cpp:30, 150`; `libs/model/src/property.cpp:210, 219`; `libs/core/src/types.cpp:88, 197, 214`; `libs/export/src/video_exporter.cpp:670-671`
- **Explanation:** Parsers use locale-independent `std::from_chars` (requiring `.` decimal separator), whereas formatters (`formatMVarString`, `setFloat`, `setVec2`, `formatF2`, `formatHsla4f`, and FFmpeg `-ss`/`-t` args) use `std::snprintf` / `std::swprintf`. On Linux, loading GTK (e.g., via native file dialogs) or calling `setlocale(LC_ALL, "")` under European locales (`de_DE.UTF-8`, `fr_FR.UTF-8`, `pl_PL.UTF-8`) switches `LC_NUMERIC` to `,`, producing `"0,500000"` which `std::from_chars` and FFmpeg fail to parse.
- **Status:** Suspected (confirmed code mismatch; triggers whenever `LC_NUMERIC` changes from `"C"`)
- **Recommended solution:** Replace `std::snprintf`/`std::swprintf` float formatting with `std::to_chars` (or `std::format`/`nlohmann::json`), which is always `"C"` locale-independent.
- **Requires platform-specific code:** No.

### R-03 — Headless OpenGL 4.5 Context Failure on Display-less Linux (`iota_mcp` & Tests)
- **Severity:** High *(P2)*
- **Affected subsystem:** MCP Server / Testing / Rendering (`libs/mcp`, `apps/studio`, `tests/`)
- **Relevant file paths and line numbers:** `libs/mcp/src/studio_session.cpp:57-104`; `apps/studio/src/editor_app.cpp:163`; `tests/test_engine_gl.cpp:64-80, 1448-1464`; `tests/test_video_export.cpp:235-250`
- **Explanation:** All headless GL contexts are created via `glfwInit()` + hidden `glfwCreateWindow(..., GLFW_VISIBLE=GLFW_FALSE)`. On Linux servers, SSH sessions, Docker containers, or headless CI without `DISPLAY` or `WAYLAND_DISPLAY`, `glfwInit()` fails—breaking `iota_mcp`'s `render_preview_frame`/`export_video` and all GL tests unless wrapped in `xvfb-run`. Additionally, `studio_session.cpp:57-59` unconditionally calls `glfwTerminate()` on teardown, risking interference if multiple GLFW sessions coexisted.
- **Status:** Suspected (standard GLFW behavior on Linux)
- **Recommended solution:** Document `xvfb-run` for CI/headless use, and optionally add an `EGL_PLATFORM_SURFACELESS_MESA` fallback in `HeadlessStudioSession` when no X11/Wayland display is present.
- **Requires platform-specific code:** Yes (for EGL surfaceless fallback).

### R-04 — `SIGPIPE` Crash Risk When Porting FFmpeg Pipes and MCP Unix Domain Sockets
- **Severity:** High *(P2)*
- **Affected subsystem:** Video Export / MCP Networking (`libs/export`, `libs/mcp`, `apps/mcp`)
- **Relevant file paths and line numbers:** `libs/export/src/video_exporter.cpp:1069`; `libs/mcp/src/live_bridge.cpp`; `apps/mcp/src/main.cpp`
- **Explanation:** On Linux, writing to a pipe or socket whose peer (`ffmpeg` or `iota_mcp` / Studio) has closed raises `SIGPIPE`, which terminates the entire process by default.
- **Status:** Suspected (applies as soon as POSIX pipes/UDS are implemented)
- **Recommended solution:** Ignore `SIGPIPE` at startup (`std::signal(SIGPIPE, SIG_IGN)`) or pass `MSG_NOSIGNAL` on `send()` and handle `EPIPE` errors cleanly.
- **Requires platform-specific code:** Yes (`#ifndef _WIN32`).

### R-05 — `readZipFromFile` Opening a Directory on Linux Can Trigger `std::bad_alloc` / OOM
- **Severity:** Medium *(P2)*
- **Affected subsystem:** Core / ZIP Archive (`libs/core`)
- **Relevant file paths and line numbers:** `libs/core/src/zip_archive.cpp:69-88`
- **Explanation:** `readZipFromFile` opens `std::ifstream(path, std::ios::binary | std::ios::ate)` and immediately allocates `std::vector<uint8_t>(static_cast<size_t>(sz))` from `in.tellg()`. On Linux, `open()` succeeds on directories and `lseek(SEEK_END)` can return `LONG_MAX` (`0x7fffffffffffffff`), causing a multi-exabyte allocation attempt / crash.
- **Status:** Suspected
- **Recommended solution:** Verify `std::filesystem::is_regular_file(path, ec)` (and reasonable max size) before reading.
- **Requires platform-specific code:** No.

### R-06 — Wayland Windowing Quirks (`GLFW_ICONIFIED`, Adaptive VSync `-1`)
- **Severity:** Medium *(P2)*
- **Affected subsystem:** UI & Desktop Integration (`apps/studio`)
- **Relevant file paths and line numbers:** `apps/studio/src/editor_app.cpp:210-211, 368, 1004-1007`
- **Explanation:**
  - *Good news:* `ImGuiConfigFlags_ViewportsEnable` (`line 211`) and `glfwGetWindowPos`/`glfwSetWindowPos` are **not** used, avoiding the most severe Wayland breakages.
  - *Risk 1 (`line 368`):* `glfwGetWindowAttrib(window_, GLFW_ICONIFIED)` is unsupported on Wayland (compositors do not report minimized state), so minimized idle throttling and Discord "Idle" transitions will not trigger when minimized under Wayland.
  - *Risk 2 (`lines 1004-1007`):* `glfwSwapInterval(-1)` (adaptive VSync) is frequently unsupported under Wayland/EGL or Mesa GLX and may emit GLFW errors or silently clamp to `1`.
- **Status:** Suspected
- **Recommended solution:** Check `glfwExtensionSupported` (`WGL_EXT_swap_control_tear` / `GLX_EXT_swap_control_tear` / `EGL_EXT_swap_control_tear` or platform guard) before calling `glfwSwapInterval(-1)`, and consider `GLFW_FOCUSED` as an additional idle-throttle signal on Wayland.
- **Requires platform-specific code:** Partial (platform/extension check).

### R-07 — CWD-Relative `iota_studio_layout.ini` Fails on Read-Only Installs & Pollutes Working Directory
- **Severity:** Medium *(P2)*
- **Affected subsystem:** UI & Desktop Integration / Filesystem (`apps/studio`)
- **Relevant file paths and line numbers:** `apps/studio/src/editor_app.cpp:212, 339`
- **Explanation:** `io.IniFilename = "iota_studio_layout.ini"` and `pathExistsNoThrow("iota_studio_layout.ini")` read/write in the current working directory. When installed to `/usr/bin` or launched via a `.desktop` file on Linux (or `Program Files` on Windows), CWD may be read-only or `$HOME`.
- **Status:** Suspected
- **Recommended solution:** Store layout `.ini` alongside `projects.json` in `$XDG_CONFIG_HOME/IotaStudio/layout.ini` (fallback `$HOME/.config/IotaStudio/`) on Linux and `%LOCALAPPDATA%\IotaStudio\layout.ini` on Windows.
- **Requires platform-specific code:** Yes.

### R-08 — Additional GLSL Edge-Case UBs & Fragile Custom Shader Adaptation on Strict Mesa Drivers
- **Severity:** Medium *(P2)*
- **Affected subsystem:** Rendering & Graphics / Shaders (`libs/engine`)
- **Relevant file paths and line numbers:** `libs/engine/src/builtin_shader_library.cpp:356, 457, 645`; `libs/engine/src/scene_renderer.cpp:512-554, 2628-2640`
- **Explanation:**
  1. `builtin_shader_library.cpp:457` (`kShaderHyperdriveTunnel`) and `645` (`kShaderKaleidoscope`) call `atan(p.y, p.x)` without guarding `p == vec2(0.0)`, which is undefined per GLSL 4.50 §8.1.
  2. `builtin_shader_library.cpp:356` (`kShaderLiquidFlow`) evaluates `pow(0.5 + 0.5 * sin(...), 4.0)`; floating-point rounding below `0.0` at `sin == -1.0` triggers `pow(neg, 4.0)` UB.
  3. `adaptCustomGlslTo450()` (`scene_renderer.cpp:512`) skips adaptation if `"#version"` appears anywhere (including inside comments) and passes legacy `#version 100`/`120`/`300 es` unmodified to an OpenGL 4.5 Core context where Mesa rejects them. Furthermore, lines `521-522, 549-554` omit `v_uv`/`v_color` if user shaders declare `vTexCoord`/`vColor`, causing a stage-interface mismatch with the fixed vertex shader; and inline `raw` shaders (`lines 2628-2640`) do not strip a UTF-8 BOM before `#version`.
- **Status:** Suspected
- **Recommended solution:** Guard `atan` at `(0,0)` and clamp `max(0.0, ...)` in `pow`; strip UTF-8 BOM on inline custom shaders; parse `#version` at the first non-whitespace/non-comment line and ensure vertex/fragment interface variables match.
- **Requires platform-specific code:** No.

### R-09 — OpenGL 4.5 Core Requirement vs Minimal Post-3.3 Usage & RHI State/Fence Edge Cases
- **Severity:** Medium *(P2)*
- **Affected subsystem:** Rendering & Graphics (`libs/rhi_gl45`)
- **Relevant file paths and line numbers:** `libs/rhi_gl45/include/iota/rhi/gl45_device.hpp:26`; `libs/rhi_gl45/src/gl45_device.cpp:154-159, 214-222, 248-253, 360-365, 476-483, 602`; context creation in `editor_app.cpp:160-162`, `studio_session.cpp:74-76`
- **Explanation:**
  1. All contexts request OpenGL 4.5 Core and shaders use `#version 450 core`, yet the only post-3.3 GL API used is GL 4.4 `glBufferStorage` (no DSA `glCreate*`/`glNamed*` calls). Older Linux Intel GPUs (pre-Broadwell Mesa) and some VM virtual GPUs capped at GL 3.3–4.3 fail context creation.
  2. `invalidateStateCache()` (`lines 214-222`) does not reset `cachedVpW`/`cachedVpH`, risking stale viewport caching after ImGui or window resize.
  3. `shutdown()` (`lines 476-483`) unconditionally calls `glUnmapBuffer` even if `MapBufferRange` failed.
  4. `waitRegionFence()` (`lines 248-253`) deletes the fence even on `GL_TIMEOUT_EXPIRED` (1s), risking buffer corruption on slow software renderers (e.g., Mesa `llvmpipe`).
  5. `updateTexture2D()` (`line 602`) hardcodes `GL_RGBA8` on resize instead of `toGlInternalFormat(rec->format)`.
- **Status:** Suspected
- **Recommended solution:** Fix state cache reset, `mappedVerts`/`mappedIndices` null checks, fence timeout handling, and `toGlInternalFormat` resize; optionally evaluate a GL 3.3 + `GL_ARB_buffer_storage` fallback if older Linux GPUs must be supported.
- **Requires platform-specific code:** No.

### R-10 — Missing Standard Header Includes & `std::from_chars(float)` Toolchain Floor
- **Severity:** Medium *(P2)*
- **Affected subsystem:** Build System / Model / DSP / Audio / Preset I/O
- **Relevant file paths and line numbers:** `libs/model/src/hierarchy_ops.cpp:315` (`std::abs` without `<cstdlib>`/`<cmath>`); `libs/model/src/command_stack.cpp:32`, `libs/audio/src/audio_decoder.cpp:239`, `libs/preset_io/src/preset_io.cpp:505` (`ptrdiff_t`/`std::ptrdiff_t` without `<cstddef>`); `libs/preset_io/include/iota/preset_io/preset_io.hpp:39,58`, `libs/dsp/include/iota/dsp/mvar_evaluator.hpp:65` (`std::string_view` without `<string_view>`); `libs/model/src/measured_var.cpp:30`, `libs/core/src/types.cpp:88` (`std::from_chars` for `float`)
- **Explanation:** Several files rely on transitive standard headers that vary between MSVC STL, libstdc++, and libc++. Additionally, floating-point `std::from_chars` requires GCC 11+ (libstdc++ 11+) or Clang with libstdc++ 11+ / libc++ 20+ on Linux.
- **Status:** Suspected
- **Recommended solution:** Add explicit `#include`s and document GCC 11+ / Clang 14+ (libstdc++ 11+) as the minimum Linux toolchain.
- **Requires platform-specific code:** No.

### R-11 — `AudioEngine` Data Races & Limited `miniaudio` Fallback Codecs Without FFmpeg
- **Severity:** Medium *(P2)*
- **Affected subsystem:** Audio System (`libs/audio`)
- **Relevant file paths and line numbers:** `libs/audio/src/audio_engine.cpp:33, 139-140, 161-163, 200-202`; `libs/audio/src/audio_decoder.cpp:212-256`
- **Explanation:** `loadTrack()` reads `impl_->track.sampleRate` after unlocking `trackMutex` (`line 140`), `deviceInitialized` is a non-atomic `bool` read/written across callers (`lines 33, 161, 201`), and `play()` calls `ma_device_start()` unconditionally even when `ensureDevice()` already started it. Furthermore, on Linux if `ffmpeg` is not installed, `decodeWithMiniaudio` only supports WAV/MP3/FLAC (no OGG/Opus/AAC/M4A) and extracts no ID3/metadata tags.
- **Status:** Suspected
- **Recommended solution:** Copy `sampleRate` under `trackMutex`, make `deviceInitialized` atomic or mutex-protected, and treat `ffmpeg`/`ffprobe` as a recommended Linux runtime dependency.
- **Requires platform-specific code:** No.

### R-12 — `.viz`/`.iota` Path & Archive Edge Cases (Backslashes, Case Sensitivity, Atomic Writes)
- **Severity:** Medium *(P2)*
- **Affected subsystem:** Serialization & Asset Management (`libs/core`, `libs/model`, `libs/preset_io`, `libs/engine`, `libs/mcp`)
- **Relevant file paths and line numbers:** `libs/core/src/utf8_path.cpp:202`; `libs/model/src/scene.cpp:850-884`; `libs/preset_io/src/preset_io.cpp:367, 389-391, 404, 427`; `libs/core/src/zip_archive.cpp:138`; `libs/engine/src/scene_renderer.cpp:784-793`; `libs/mcp/src/working_copy.cpp`
- **Explanation:**
  1. `renameLocalAsset()` (`scene.cpp:850-884`) does not reject `/` or `\`, allowing backslashes into ZIP entry names.
  2. `preset_io.cpp:367` matches `"scene.json"` case-sensitively, and lines `389-391` flatten ZIP directories with `baseNameFromSlashPath` (colliding `a/bg.png` and `b/bg.png`).
  3. Direct disk image/font paths (`scene_renderer.cpp:792`, `font_atlas.cpp:725`) break on Linux when saved with Windows `\` separators or Windows-only casing.
  4. `saveProjectAsJson`, `writeZipToFile`, `ProjectLibrary::save`, and `working_copy.cpp` truncate files directly (`std::ios::trunc`) without atomic temp-file-and-rename, and `working_copy.cpp` fails if the source `.iota`/`.viz` lives in a read-only Linux directory.
- **Status:** Suspected
- **Recommended solution:** Sanitize `/` and `\` in `renameLocalAsset`, use case-insensitive `"scene.json"` fallback, normalize `/` in relative paths, and write via temp file + `std::filesystem::rename`.
- **Requires platform-specific code:** No.

### R-13 — CMake & Preset Minor Discrepancies (Multi-Config Discord SDK, MinGW Defines, Presets, Windows Subsystem)
- **Severity:** Low *(P3)*
- **Affected subsystem:** Build System (`CMakeLists.txt`, `CMakePresets.json`, `apps/studio/CMakeLists.txt`)
- **Relevant file paths and line numbers:** `CMakeLists.txt:16, 189-192, 219-224`; `CMakePresets.json:3-7, 8-101`; `apps/studio/CMakeLists.txt:16`
- **Explanation:** `CMakePresets.json` declares `3.23.0` while `CMakeLists.txt` requires `3.24` and omits `condition` host filters and Linux Clang presets; `CMakeLists.txt:219` inspects `CMAKE_BUILD_TYPE` (breaking Multi-Config generators for Discord SDK); `CMakeLists.txt:191` gates Windows macros on `MSVC` instead of `WIN32`; `apps/studio/CMakeLists.txt:16` builds as Console subsystem on Windows.
- **Status:** Confirmed (in files) / Suspected impact
- **Recommended solution:** Use `IMPORTED_LOCATION_DEBUG`/`RELEASE` properties, `if(WIN32)` for Win32 defines, align `CMakePresets.json` to `3.24` with host `condition` blocks, and evaluate `WIN32` subsystem + `/ENTRY:mainCRTStartup` for Release Windows GUI.
- **Requires platform-specific code:** Yes (CMake conditionals).

### R-14 — Windows-Centric UI Strings, Docs & Mixed CRLF/LF Line Endings
- **Severity:** Low *(P3)*
- **Affected subsystem:** UI / Documentation / Repo Hygiene
- **Relevant file paths and line numbers:** `apps/studio/src/editor_app.cpp:934, 1124, 1144, 1669, 2403`; `apps/studio/src/docs_panel.cpp:154`; `docs/MCP.md`; `libs/model/src/property_docs.cpp:675-682, 1005-1078`; headers/sources across `libs/` and `apps/`
- **Explanation:** UI messages mention `"DLL"`, `"ffmpeg.exe"`, `"Pipe"`, and `"DirectWrite Unicode"`; `docs/MCP.md` only documents Windows paths and named pipes; and most source files have a 15-line LF license header followed by CRLF line endings.
- **Status:** Confirmed
- **Recommended solution:** Make UI strings and `docs/MCP.md` platform-neutral (or `#ifdef _WIN32` where appropriate), and normalize line endings via `.gitattributes` if desired.
- **Requires platform-specific code:** Partial (for platform-specific help strings).

---

## 6. Platform-Specific Implementations Inventory

| File & Lines | Mechanism Used | Isolation Status | Assessment |
|---|---|---|---|
| `libs/core/src/utf8_path.cpp:18-20, 131-204` | `WideCharToMultiByte` / `MultiByteToWideChar` vs `path.u8string()` | **Properly Isolated** (`#ifdef _WIN32`) | Compiles & works cleanly on both OSes. |
| `libs/preset_io/src/project_manager.cpp:47-51` | `localtime_s` vs `localtime_r` | **Properly Isolated** (`#ifdef _WIN32`) | Clean. |
| `libs/preset_io/src/project_manager.cpp:193-202` | `_wgetenv(L"LOCALAPPDATA")` vs `std::getenv("XDG_DATA_HOME")` | **Isolated, Incomplete** | Missing `$HOME/.local/share` Linux fallback (C-12). |
| `libs/core/CMakeLists.txt:26-28` | `winhttp` | **Properly Isolated** (`if(WIN32)`) | Needs Linux HTTPS library (`CURL`) in `else()`. |
| `libs/engine/CMakeLists.txt:41-43` | `dwrite` | **Properly Isolated** (`if(WIN32)`) | Needs Linux font libs in `else()`. |
| `libs/mcp/CMakeLists.txt:46-48` | `advapi32` | **Properly Isolated** (`if(WIN32)`) | Clean. |
| `libs/discord/CMakeLists.txt:31-34` | `/DELAYLOAD:discord_partner_sdk.dll` | **Properly Isolated** (`if(MSVC)`) | Linux lacks `/DELAYLOAD` equivalent (R-01). |
| `libs/discord/src/discord_presence.cpp:25-33, 113-122, 287-295, 573-577` | `GetModuleHandleW`, `SearchPathW`, `LoadLibraryW` | **Properly Isolated** (`#ifdef _WIN32`) | Linux `#else` assumes `.so` loaded unconditionally. |
| `apps/studio/CMakeLists.txt:38-46, 79-84` | `.rc` resource script (`WIN32`); `$ORIGIN` RPATH (`UNIX`) | **Properly Isolated** | Clean (note `INSTALL_RPATH` needs `install()` rules). |
| `apps/mcp/src/main.cpp:26-29, 51-54` | `_setmode(_O_BINARY)` for stdio | **Properly Isolated** (`#ifdef _WIN32`) | Clean; POSIX stdio is already binary. |
| `libs/dsp/src/sample_providers.cpp:21-27`, `libs/mcp/src/png_encode.cpp:19-25` | `#pragma warning(push/pop)` | **Properly Isolated** (`#ifdef _MSC_VER`) | Third-party headers need `SYSTEM` for GCC/Clang. |
| `tests/test_engine_gl.cpp:1709-1724` | DirectWrite layout cache assertions | **Properly Isolated** (`#ifdef _WIN32`) | Line 1102 is missing this guard (C-16). |
| `CMakeLists.txt:157`, `libs/rhi_gl45/CMakeLists.txt:30` | `opengl32` | **Unisolated (Win32-only)** | Blocks Linux linking (C-01). |
| `apps/studio/CMakeLists.txt:63-64` | `comdlg32`, `shell32` | **Unisolated (Win32-only)** | Blocks Linux linking (C-02). |
| `libs/rhi_gl45/src/gl_loader.{hpp,cpp}` | `<windows.h>`, `wglGetProcAddress`, `opengl32.dll`, `APIENTRY` | **Unisolated (Win32-only)** | Blocks Linux build (C-03). |
| `libs/audio/src/audio_decoder.cpp` | `<windows.h>`, `CreateProcessW`, `ffmpeg.exe`, `ma_decoder_init_file_w` | **Unisolated (Win32-only)** | Blocks Linux build (C-04). |
| `libs/export/src/video_exporter.{hpp,cpp}` | `<windows.h>`, `CreateProcessW`, `CreatePipe`, `ffmpeg.exe`, `wstring` cmd | **Unisolated (Win32-only)** | Blocks Linux build (C-06). |
| `apps/studio/src/editor_app.cpp` | `<windows.h>`, `OPENFILENAMEW`, `ShellExecuteW`, `LoadImageW`, `C:/Windows/Fonts` | **Unisolated (Win32-only)** | Blocks Linux build (C-07). |
| `libs/mcp/src/live_bridge.cpp` | `<windows.h>`, `CreateNamedPipeW`, `OVERLAPPED`, `GetUserNameW` | **Unisolated (Win32-only)** | Blocks Linux MCP build (C-08). |
| `tests/test_mcp.cpp` | `<windows.h>`, `GetCurrentProcessId`, `CreateProcessW`, Named Pipes | **Unisolated (Win32-only)** | Blocks Linux MCP test build (C-09). |
| `libs/core/src/update_checker.cpp:598-644` | `WinHttp*` vs `#else` error stub | **Isolated Stub** | Unimplemented on Linux (C-14). |
| `libs/engine/src/font_atlas.cpp` | DirectWrite vs striped placeholder fallback | **Isolated Degraded Fallback** | Unusable text output on Linux (C-10). |

---

## 7. Missing Implementations

| ID | Missing Implementation | Severity | Affected Subsystem | Files & Lines | Status | Recommended Solution | Platform-Specific Code? |
|---|---|---|---|---|---|---|---|
| M-01 | Linux OpenGL proc loader (`glfwGetProcAddress`) | Critical | Rendering (`libs/rhi_gl45`) | `gl_loader.hpp:18`, `gl_loader.cpp:24-40` | Confirmed | Use `glfwGetProcAddress` / injected `GlProc` callback | Minimal (header guard) |
| M-02 | Linux font discovery, shaping & rasterization | Critical | Typography (`libs/engine`) | `font_atlas.cpp:1137-1411` | Confirmed | Fontconfig + FreeType + HarfBuzz (or `stb_truetype` interim) | Yes |
| M-03 | POSIX `ffmpeg`/`ffprobe` subprocess decoder | Critical | Audio (`libs/audio`) | `audio_decoder.cpp:67-210` | Confirmed | `pipe()` + `fork()`/`execvp()` + `ma_decoder_init_file` | Yes |
| M-04 | POSIX `ffmpeg` pipe streaming & process control | Critical | Video Export (`libs/export`) | `video_exporter.cpp:205-353, 940-1015` | Confirmed | `argv` builder + POSIX `pipe`/`fork`/`execvp`/`poll`/`waitpid` | Yes |
| M-05 | Linux native file dialogs, `xdg-open`, ImGui fonts, X11/Wayland icon | Critical | Studio UI (`apps/studio`) | `editor_app.cpp:56-248, 2840-2862` | Confirmed | `platform_desktop` module (NFD/portal, `xdg-open`, Fontconfig, `assets/icon.png`) | Yes |
| M-06 | Linux MCP Live Bridge IPC (`AF_UNIX` Unix Domain Socket) | Critical | MCP (`libs/mcp`) | `live_bridge.cpp:18-705` | Confirmed | `AF_UNIX` `SOCK_STREAM` in `$XDG_RUNTIME_DIR` with `0600` perms | Yes |
| M-07 | Linux HTTPS update checker & Linux asset selection | High | Core (`libs/core`) | `update_checker.cpp:476-514, 639-644` | Confirmed | `libcurl` HTTPS fetch + `.AppImage`/`.tar.gz`/`.deb` scoring | Yes |
| M-08 | Linux hardware video encoding (VA-API `h264_vaapi`/`hevc_vaapi`) | Medium | Export / Studio / MCP | `video_exporter.{hpp,cpp}`, `editor_app.cpp:2346`, `tool_registry.cpp:1110` | Confirmed | Add `VaApi` backend (`-vaapi_device /dev/dri/renderD128`) | Yes |
| M-09 | Audio device enumeration, mic capture, & system loopback (both OSes; Linux needs `.monitor` capture) | Medium | Audio (`libs/audio`) | `audio_engine.cpp:88` | Confirmed | Add `ma_context_get_devices`; use PulseAudio/PipeWire `.monitor` on Linux since `ma_device_type_loopback` is WASAPI-only | Yes |
| M-10 | HiDPI / DPI scaling & Windows DPI manifest | Medium | Studio UI (`apps/studio`) | `editor_app.cpp`, `iota_studio.rc.in` | Confirmed | Query `glfwGetWindowContentScale`, scale ImGui fonts/style, add Windows manifest | Yes |
| M-11 | Portable relative/bundled audio & atomic project saves / autosave | Medium | Preset I/O (`libs/preset_io`) | `preset_io.cpp`, `project_manager.cpp`, `zip_archive.cpp` | Confirmed | Relative `/` audio paths + temp-file-rename saves | No |
| M-12 | `GL_DEBUG_OUTPUT` diagnostics & headless Linux EGL surfaceless fallback | Medium | RHI / MCP (`libs/rhi_gl45`, `libs/mcp`) | `gl45_device.cpp`, `studio_session.cpp:63-104` | Confirmed | Add GL debug callback; optional `EGL_PLATFORM_SURFACELESS_MESA` | Yes (EGL) |
| M-13 | Linux CMake `install()` rules, `.desktop`/AppStream/icon install, packaging, & CI workflows | Low | Packaging / CI | `CMakeLists.txt`, `installer/`, `.github/workflows/` | Confirmed | Add `install()`, `.desktop`, hicolor icon, AppImage/Flatpak, GitHub Actions CI | Yes |
| M-14 | Optional desktop polish (both OSes): OS file drop, fullscreen toggle, reveal in file manager, dark titlebar, notifications | Low | Studio UI (`apps/studio`) | `editor_app.cpp` | Confirmed | `glfwSetDropCallback`, `glfwSetWindowMonitor`, `xdg-open`/Explorer, `DwmSetWindowAttribute` | Partial |

---

## 8. Build & Dependency Requirements

### Toolchain & CMake Requirements
- **CMake:** `>= 3.24` (`CMakeLists.txt:16`; note `CMakePresets.json:4-6` currently states `3.23.0`).
- **Compiler (C++20):**
  - **Windows:** MSVC 2022+ (`cl` with `/permissive- /EHsc /utf-8`).
  - **Linux:** **GCC 11+** (tested with GCC 16.2.1 in host env) or **Clang 14+** with **libstdc++ 11+** (or libc++ 20+), required for floating-point `std::from_chars`.
- **Build Generator:** Ninja (`CMakePresets.json`).

### Third-Party `FetchContent` Dependencies (10/10 Cross-Platform Available)

| Library | Version / Tag | Usage | Windows & Linux Availability | Audit Notes |
|---|---|---|---|---|
| `nlohmann_json` | `v3.11.3` | Scene JSON, MCP JSON-RPC, GitHub API | Header-only; **both OSes** | Clean. |
| `glfw` | `3.4` | Windowing, input, OpenGL context | Static build; **both OSes** | Configures X11 + Wayland on Linux (`build/linux-release/CMakeCache.txt` verified). |
| `imgui` | `v1.93.0-docking` | Studio UI (`glfw` + `opengl3` backends) | Static wrapper; **both OSes** | `IMGUI_USE_WCHAR32`; drop `opengl32` link (C-01). |
| `ImGuiColorTextEdit` | `50cf923...` | Shader Editor code widget | Static wrapper; **both OSes** | Pure ImGui widget. |
| `glm` | `1.0.1` | Vectors, matrices, math | Header-only; **both OSes** | `GLM_ENABLE_CXX_20=ON`. |
| `miniaudio` | `0.11.26` | Audio playback & WAV/MP3/FLAC fallback | Header-only; **both OSes** | Needs `-ldl -lpthread -lm` on Linux (C-05); loopback is WASAPI-only. |
| `kissfft` | `v131` | Real FFT (`float`) for spectrum/DSP | Static C lib; **both OSes** | Add `-lm` explicitly on UNIX. |
| `miniz` | `3.0.2` | `.viz`/`.iota` ZIP archive read/write | Static C lib; **both OSes** | Already built in `build/linux-release/_deps/miniz-build/libminiz.a`. |
| `stb` | `6b5799b...` | `stb_image`, `stb_image_write`, `stb_truetype` | Header-only; **both OSes** | Mark `stb_hdr` include dir `SYSTEM` to silence GCC/Clang warnings. |
| `doctest` | `v2.4.12` | Automated test framework | Header-only; **both OSes** | Clean. |

### Prebuilt Binary Dependency: Discord Social SDK (`third_party/discord_social_sdk`)
- **Windows:** `discord_partner_sdk.lib` / `.dll` (both root x64 and `arm64/` exist). Delayed load via `/DELAYLOAD`.
- **Linux:** `libdiscord_partner_sdk.so` (14.6 MB release, **x86-64 only**, `GLIBC_2.29`, depends on `libpulse.so.0`, `libasound.so.2`, `libX11.so.6`, `libatomic.so.1`, `libpthread.so.0`, `libdl.so.2`, `librt.so.1`, `libm.so.6`). Missing on `aarch64`.

### System Packages Required on Linux
- **Build-Time:** OpenGL dev (`libgl-dev` / `libglvnd-dev`), X11 dev (`libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`, `libxi-dev`), Wayland dev (`libwayland-dev`, `wayland-protocols`, `wayland-scanner`, `libxkbcommon-dev`), plus chosen future libs: `libfontconfig-dev`, `libfreetype-dev`, `libharfbuzz-dev`, `libcurl4-openssl-dev`, `libdbus-1-dev` (or GTK3 for NFD).
- **Runtime:** OpenGL 4.5 driver (Mesa / NVIDIA / AMD), PipeWire (`pipewire-pulse`) or PulseAudio/ALSA, `ffmpeg` + `ffprobe` (strongly recommended for full audio codecs & required for MP4 export), `xdg-utils` (`xdg-open`) + `xdg-desktop-portal` (or `zenity`/`kdialog`), plus `libpulse0` / `libasound2` / `libx11-6` / `libatomic1` if Discord SDK is dynamically linked.

### Packaging & Distribution Evaluation
- **Windows:** Uses Inno Setup 6 (`installer/iota_studio.iss`, `installer/build_installer.ps1`), bundling `iota_studio.exe`, optional `iota_mcp.exe`, `discord_partner_sdk.dll` (+ `discord_krisp.dll` / `.kef` if applicable), VC++ Redistributable assumptions, and optional `ffmpeg.exe` on `PATH` or app folder.
- **Linux:** Requires adding CMake `install()` rules, an XDG `.desktop` entry (`StartupWMClass` / Wayland `app_id`), AppStream `.metainfo.xml`, and hicolor icon installation from `assets/icon.png`, plus XDG config/data compliance (`~/.config/IotaStudio`, `~/.local/share/IotaStudio`, `$XDG_RUNTIME_DIR`).
  - **AppImage (Best near-term portable format):** Matches existing `INSTALL_RPATH "$ORIGIN"` layout cleanly; build on an older glibc baseline (e.g., Ubuntu 22.04) so binaries don't require cutting-edge glibc; `ffmpeg` can be bundled or picked from host `PATH`.
  - **Flatpak:** Strong sandboxed UX with automatic `xdg-desktop-portal` file dialogs and predictable Mesa/FreeType/HarfBuzz/Fontconfig runtime (`org.freedesktop.Platform`); requires `org.freedesktop.Platform.ffmpeg-full` (or bundled `ffmpeg`), careful Discord IPC socket exposure (`xdg-run/discord-ipc-*`), and MCP Unix socket permissions inside/outside the sandbox.
  - **Native Packages (`.deb` / `.rpm` / Arch PKGBUILD):** Clean distro integration (`ffmpeg`, `fontconfig`, `freetype`, `harfbuzz`, `libcurl` as package dependencies); if bundling `libdiscord_partner_sdk.so` in `/usr/lib/iota-studio/`, update `INSTALL_RPATH` from `$ORIGIN` to `$ORIGIN/../lib/iota-studio`.

---

## 9. Testing Coverage and Verification Gaps

### Automated Test Suite Matrix (`tests/`)

| Test File | Scope | Requirements | Linux Status | Blocking Lines |
|---|---|---|---|---|
| `test_main.cpp` | Doctest entry point | None | Runnable | — |
| `test_hierarchy.cpp` | Layer/comp hierarchy, clipboard, precompose | CPU only | Runnable (code-reviewed) | — |
| `test_particles.cpp` | Curl noise, emitters, trails, DoF, Particular parity | CPU only | Runnable (code-reviewed) | — |
| `test_shake.cpp` | Shake/RGB-split math & serialization | CPU only | Runnable (code-reviewed) | — |
| `test_property_docs.cpp` | 100% property/group/token doc coverage | CPU only | Runnable (code-reviewed) | — |
| `test_dsp.cpp` | FFT, windows, Spectrum, BeatDetector, MVar, SoundKeys | CPU only | Runnable (code-reviewed) | — |
| `test_discord_presence.cpp` | Payload formatting, path sanitization, `ProjectLibrary` | Temp dir | Runnable (code-reviewed) | — |
| `test_update_checker.cpp` | SemVer, safe URL check, offline GitHub JSON | CPU only | Runnable *(Windows `.exe` asset checks)* | `115-131, 176-214` |
| `test_model_preset_io.cpp` | `.viz`/`.iota` ZIP, 18 presets, WAV decode, Unicode paths | `IOTA_TEST_PRESETS_DIR` | **Fails on Linux** | `309-317` (`L"..."` path throws under `"C"` locale) |
| `test_engine_gl.cpp` | Layout + full GL 4.5 scene/shader/particle/text tests | **GL 4.5 + Display** + `IOTA_TEST_PRESETS_DIR` | **Fails on Linux** | `342` (`L"..."` path), `1102` (DirectWrite check), `1048/1109/1123/1129/1177` (Win fonts) |
| `test_video_export.cpp` | Encoder probing, CLI builder, live GL + FFmpeg export | **GL 4.5 + Display + FFmpeg** | **Fails on Linux** | `138` (`"ffmpeg.exe"`), `165-200` (`std::wstring` CLI) |
| `test_mcp.cpp` *(when `IOTA_BUILD_MCP=ON`)* | Working copy, PNG/base64, MCP server + GL, LiveBridge, `iota_mcp` subprocess | **GL 4.5 + Display** + `iota_mcp` | **Won't compile on Linux** | `25, 42, 280, 326-399, 525` (`<windows.h>`, `CreateProcessW`, Named Pipes) |

### Cross-Platform `.viz` / `.iota` Project Portability Matrix
*(Cross-checked against `libs/model`, `libs/preset_io`, and `test_template/SPECIAL FOR 300SUBS.viz`, which contains `scene.json` + 2 flat image assets using only `local:`, `internalres:`, and `composition:` URIs.)*

| Project Content | Windows -> Linux | Linux -> Windows | Notes |
|---|---|---|---|
| Scene graph, keyframes, properties (`scene.json`) | **Yes** *(watch `LC_NUMERIC`)* | **Yes** | Pure UTF-8 JSON; beware R-02 `snprintf` comma-decimal risk on Linux. |
| Bundled images (`local:*.png/jpg`) | **Yes** | **Yes** | Flat ZIP entries; `baseNameFromSlashPath` handles `/` and `\`. |
| Bundled custom shaders (`local:*.glsl` & inline) | **Yes** | **Yes** | Subject to GLSL 4.50 strictness & BOM checks (R-08). |
| Bundled fonts (`local:*.ttf/otf`) | **I/O Yes; Render Broken** | **Yes** | Preserved in ZIP by `preset_io`, but Linux renderer ignores custom fonts until C-10 is fixed. |
| System font presets (`Segoe_UI`, `Impact`, etc.) | **Degraded** | **Degraded** | Requires Linux font substitution mapping (C-18). |
| External audio (`metadata.audioPath`, `audioAssets`) | **Broken** | **Broken** | Raw OS absolute paths break across OSes (C-17). |
| Direct disk image/font paths (non-`local:`) | **Broken** | **Broken** | Raw OS absolute paths, backslashes, and case sensitivity break across OSes. |

### Verification Status Matrix (Strict Provenance)

| Category | Items |
|---|---|
| **Verified on Windows** | *None runtime-executed in this Linux audit session.* (All Windows findings are from source/CMake/installer inspection.) |
| **Verified on Linux** | CMake configure succeeded (`build/linux-release/CMakeCache.txt`, GLFW X11+Wayland); `miniz` & `kissfft_real` static libs built; `libs/core` & `libs/discord` dry-run compiled on GCC 16.2 (`update_checker.cpp:588` unused capture verified on Clang); `libdiscord_partner_sdk.so` ELF arch (`x86-64`), `GLIBC_2.29`, and `DT_NEEDED` inspected; `SPECIAL FOR 300SUBS.viz` archive entries inspected; confirmed compile/link blockers in CMake and 6 Win32-coupled `.cpp`/`.hpp` files. |
| **Code-reviewed but unverified** | Runtime execution of `libs/dsp`, `libs/model`, `libs/preset_io`, `libs/rhi`, `libs/audio` playback via `miniaudio`, `libs/engine` GL 4.5 rendering/shaders on Mesa/AMD/Intel/NVIDIA, `apps/studio` ImGui panels on X11/Wayland, `iota_mcp` stdio loop, and all Windows runtime behavior. |
| **Not implemented or incompatible** | `gl_loader` Linux symbol resolution, `font_atlas` Linux font backend, `audio_decoder` POSIX runner & `ma_decoder_init_file`, `video_exporter` POSIX runner & VA-API, `editor_app` Linux dialogs/fonts/URLs/icons, `live_bridge` Unix Domain Sockets, `update_checker` Linux HTTPS client, audio loopback/capture/device enumeration, HiDPI scaling, Linux packaging & CI. |

### Subsystems Lacking Automated Test Coverage
1. **Studio UI (`apps/studio/`):** Zero automated tests (`iota_studio --smoke-test` in `main.cpp:25,36` is not registered in `tests/CMakeLists.txt`).
2. **Audio Playback & Compressed Decoding (`libs/audio/`):** No tests for `AudioEngine` playback/seek/volume or FFmpeg-decoded MP3/FLAC/OGG/AAC.
3. **Live Networking & Discord Runtime (`libs/core`, `libs/discord`):** Only offline JSON/payload formatting tested; `UpdateChecker::startCheckAsync()` and enabled Discord SDK callbacks are untested.
4. **MCP Tool Completeness (`libs/mcp/`):** `export_video`, `export_list_encoders`, `project_import_asset`, `project_open`, composition/layer/keyframe CRUD, `studio_playback`, and malformed JSON-RPC/EOF recovery lack direct tests.
5. **Linux OS Integration:** No tests for XDG path fallback, Unix Domain Socket bridge + stale socket cleanup, or Linux font rendering.

### Required Manual Linux Verification Matrix (Post-Porting)
- **Display Servers:** X11, Wayland (GNOME Mutter, KDE KWin, wlroots/Sway/Hyprland).
- **GPU Drivers:** Mesa RadeonSI (AMD), Mesa Iris (Intel), NVIDIA proprietary, Mesa `llvmpipe` (software GL 4.5).
- **Audio Backends:** PipeWire (`pipewire-pulse`), PulseAudio, bare ALSA.
- **Desktop Edge Cases:** Fractional HiDPI (125% / 150% / 200%), non-English locale (`LC_NUMERIC=de_DE.UTF-8`), Unicode/CJK/emoji file paths, headless CI (`xvfb-run`).

---

## 10. Recommended Fixes, Prioritized by Severity

### P0 — Blocks Building or Launching on Linux
1. **Fix CMake platform linkage & Discord arch checks** (`CMakeLists.txt:157, 171-185, 226-271`, `libs/rhi_gl45/CMakeLists.txt:30`, `apps/studio/CMakeLists.txt:63-64`, `libs/audio/CMakeLists.txt`, `tests/CMakeLists.txt:55-62`): Switch `opengl32` -> `OpenGL::GL`, gate `comdlg32`/`shell32` under `if(WIN32)`, link `Threads::Threads` + `${CMAKE_DL_LIBS}` + `m` on Linux, guard Discord SDK by `CMAKE_SYSTEM_PROCESSOR`, and add `BUILD_RPATH "$ORIGIN"` to `iota_tests`.
2. **Port OpenGL proc loader** (`libs/rhi_gl45/src/gl_loader.{hpp,cpp}`): Guard `<windows.h>`, define `APIENTRY` fallback, and use `glfwGetProcAddress` returning `void(*)()` to support WGL/GLX/EGL cleanly with `-Wpedantic`.
3. **Port audio decoder** (`libs/audio/src/audio_decoder.cpp`): Use `ma_decoder_init_file` on Linux (`#ifndef _WIN32`) and implement a POSIX `pipe()`/`fork()`/`execvp()` runner for `ffmpeg` and `ffprobe`.
4. **Port video exporter** (`libs/export/include/iota/export/video_exporter.hpp`, `libs/export/src/video_exporter.cpp`): Replace `std::wstring` CLI API with a `std::vector<std::string>` `argv` builder and add a POSIX `ffmpeg` pipe/process backend with `SIGPIPE` protection.
5. **Isolate Studio desktop Win32 APIs** (`apps/studio/src/editor_app.cpp`): Extract a `platform_desktop` module providing Linux file dialogs (NFD/portal/zenity), `xdg-open`, Fontconfig/`/usr/share/fonts` ImGui fonts, and X11/Wayland icon + app-id setup.
6. **Port MCP Live Bridge & MCP test** (`libs/mcp/src/live_bridge.cpp`, `tests/test_mcp.cpp`): Add POSIX `AF_UNIX` Unix Domain Sockets (`$XDG_RUNTIME_DIR/iota_studio_mcp.sock`, `0600` perms, stale-socket recovery, `poll()`+`eventfd()`) and POSIX test subprocess runner.

### P1 — Critical Functionality Unavailable or Broken on Linux
1. **Implement Linux typography engine** (`libs/engine/src/font_atlas.cpp`): Add Fontconfig + FreeType + HarfBuzz (or `stb_truetype` interim) + Windows family alias mapping so Linux renders real Unicode text instead of striped placeholder boxes.
2. **Fix GLSL 4.50 undefined behavior** (`libs/engine/src/builtin_shader_library.cpp:56, 161, 464, 696, 809` + lines `356, 457, 645`): Eliminate `pow(neg, 2.0)` and inverted `smoothstep(hi, lo, x)`.
3. **Fix XDG paths & case-sensitive path matching** (`libs/preset_io/src/project_manager.cpp:36-42, 192-203`, `apps/studio/src/editor_app.cpp:212, 339`): Add `$HOME/.local/share` fallback, move `iota_studio_layout.ini` to XDG config/LocalAppData, and gate `normalizePathLower()` to `#ifdef _WIN32`.
4. **Implement Linux `UpdateChecker` HTTPS & asset scoring** (`libs/core/src/update_checker.cpp`): Add `libcurl` HTTPS fetch, fix Clang unused capture (`line 588`), and select Linux release artifacts (`.AppImage`/`.tar.gz`/`.deb`).
5. **Decouple `libdiscord_partner_sdk.so` load-time dependency** (`libs/discord/`, `CMakeLists.txt`): Load via `dlopen`/`dlsym` on Linux so missing `.so` or PulseAudio/X11 libs degrade gracefully to `SdkUnavailable`.
6. **Fix Linux automated test blockers** (`tests/test_engine_gl.cpp`, `tests/test_model_preset_io.cpp`, `tests/test_video_export.cpp`, `tests/CMakeLists.txt`): Replace `L"..."` wide paths with `pathFromUtf8(...)`, adapt DirectWrite/Windows-font/FFmpeg assertions, and wire `IOTA_TEST_PRESETS_DIR`.

### P2 — Feature Parity and Portability Issues
1. **Cross-platform `.viz`/`.iota` external audio portability** (`libs/model`, `libs/preset_io`, `apps/studio`): Store relative `/` audio paths + project-dir fallback search (and optional bundling/relink), plus slash validation in `renameLocalAsset()` and atomic temp-file+rename saves.
2. **Eliminate `LC_NUMERIC` float formatting hazard** (`libs/core/src/types.cpp`, `libs/model/src/{measured_var,property}.cpp`, `libs/export/src/video_exporter.cpp`): Switch `snprintf`/`swprintf` float serialization to `std::to_chars`.
3. **Add Linux VA-API hardware encoding** (`libs/export/`, `apps/studio/src/editor_app.cpp`, `libs/mcp/src/tool_registry.cpp`): Support `h264_vaapi` and `hevc_vaapi`.
4. **Harden core, audio, RHI, & shader edge cases**: `is_regular_file` check in `readZipFromFile`; `AudioEngine` mutex/atomic cleanup; `Gl45RenderDevice` viewport cache reset, `MapBufferRange` unmap guard, fence timeout, and `toGlInternalFormat` resize; `adaptCustomGlslTo450` BOM/version/varying fixes; explicit std headers and `SYSTEM` third-party includes.
5. **Wayland & headless Linux resilience**: Guard `glfwSwapInterval(-1)`, handle Wayland iconify/focus idle behavior, and support `EGL_PLATFORM_SURFACELESS_MESA` (or document `xvfb-run`) for `HeadlessStudioSession`.
6. **Plan audio capture/loopback abstraction** (`libs/audio/`): If live capture/loopback is added later, use `ma_context_get_devices` and PulseAudio/PipeWire `.monitor` capture on Linux (`ma_device_type_loopback` is WASAPI-only).

### P3 — Polish, Packaging, and Platform-Specific Improvements
1. **HiDPI scaling** (`apps/studio/src/editor_app.cpp`, `apps/studio/resources/iota_studio.rc.in`): Implement `glfwGetWindowContentScale` scaling and Windows DPI awareness manifest.
2. **Linux packaging & desktop integration**: Add CMake `install()`, `.desktop` file, AppStream metainfo, `assets/icon.png` hicolor icon install, and AppImage/Flatpak build recipes.
3. **CI & smoke test automation**: Add GitHub Actions build/test matrix (Windows MSVC + Linux GCC/Clang under `xvfb-run` + Mesa `llvmpipe`) and register `iota_studio --smoke-test` in CTest.
4. **Cleanup & docs**: Platform-neutral Studio UI copy, Linux sections in `README.md` & `docs/MCP.md`, `CMakePresets.json` `3.24` + host `condition` filters, Windows `WIN32` subsystem decision, multi-config Discord SDK properties, and LF line-ending consistency.
