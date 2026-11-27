<p align="center">
  <img src="assets/icon.png" alt="Iota Studio icon" width="84" />
</p>

<h1 align="center">Iota Studio</h1>

<p align="center">
  Cross-platform C++20 desktop studio, OpenGL 4.5 / WebGL2 engine, and Dear ImGui workspace for designing, previewing, and exporting audio-reactive music visualizers on <strong>Windows</strong>, <strong>Linux</strong>, and in the <strong>browser via WebAssembly</strong>.
</p>

<p align="center">
  <a href="https://vorlie.github.io/iota-studio/">Website</a> ·
  <a href="https://vorlie.github.io/iota-studio/studio/"><strong>Launch Web Studio</strong></a> ·
  <a href="https://github.com/vorlie/iota-studio/releases/latest">Downloads</a> ·
  <a href="https://vorlie.github.io/iota-studio/docs/">Documentation</a> ·
  <a href="https://vorlie.github.io/iota-studio/palette/">Palette Manager</a>
</p>

> [!WARNING]
> **Template Compatibility Disclaimer:** Any Avee Player preset (`.viz`) saved or overwritten in Iota Studio will no longer load in mobile Avee Player. Always keep backups of original `.viz` files or save edits as `.iota` projects.

## Highlights

- **HDR16F Rendering, Physical Bloom & Tonemapping** — 16-bit half-float compositing (`RGBA16F`), 6-mip Jimenez/Karis bloom pyramid with anamorphic stretch, and `ACES` / `AgX` / `PBRNeutral` / `Reinhard` tonemapping.
- **Frame-Rate-Independent Audio DSP & SoundKeys** — `Waveform`, `kissfft` Spectrum, and direct DFT (`Spectrum2`) with AGC, IEC A-weighting, beat detection, 64-frame history rings, curve splines, and 3 interactive SoundKeys frequency bands (`R1`–`R3`).
- **3D GPU Particle Engine & Camera Shake** — Up to 50,000 particles with 8 emitter shapes (including `LayerMask`), 3D curl turbulence, force fields, Aux sub-particles, Depth-of-Field bokeh, and 4-axis `S_Shake`-grade camera shake with RGB splitting.
- **Custom GLSL 450 Shaders** — Live Shader Editor with `u_time`, `u_beat`, `u_soundKeys`, canvas-below Post-FX (`composition:0`), 15 built-in presets, and the companion [Web GLSL Palette Manager](https://vorlie.github.io/iota-studio/palette/).
- **Docking Workspace & Project Tools** — Multi-selection Hierarchy with Pre-Compose and cross-composition drag-and-drop, Asset Browser, Project Templates, snapshot Undo/Redo, searchable `F1` Docs, and `.iota` / `.viz` / `.json` portability.
- **Hardware GPU Video Export (Desktop)** — Deterministic fixed-timestep MP4 rendering (H.264/H.265 + AAC) via FFmpeg with auto-detected `NVIDIA NVENC`, `Linux VA-API`, `AMD AMF`, `Intel Quick Sync`, or CPU `x264`/`x265`.
- **Browser Web Studio & Opt-In MCP Bridge** — Run Studio live at [vorlie.github.io/iota-studio/studio](https://vorlie.github.io/iota-studio/studio/) with `AudioContext` decoding and IndexedDB persistence, or enable `IOTA_BUILD_MCP=ON` for AI co-creation via `iota_mcp` (see [docs/MCP.md](docs/MCP.md)).

## Try or Download

- **Browser (No install):** Launch [Iota Studio Web Edition](https://vorlie.github.io/iota-studio/studio/) in any WebGL2 browser (drag & drop audio, `.iota`/`.viz` projects, images, fonts, or `.glsl` files; offline MP4 export requires desktop).
- **Windows x64 & Linux x86_64:** Grab the latest Inno Setup `.exe` installer, portable `.AppImage`, or `.tar.gz` from [GitHub Releases](https://github.com/vorlie/iota-studio/releases/latest) or the [Download page](https://vorlie.github.io/iota-studio/download/).
- **Arch Linux:** Build cleanly using [packaging/arch/PKGBUILD](packaging/arch/PKGBUILD):
  ```bash
  git clone https://github.com/vorlie/iota-studio.git
  cd iota-studio/packaging/arch && makepkg -si
  ```

## Quick Start (Build from Source)

See the [Building & Architecture Guide](https://vorlie.github.io/iota-studio/docs/building/) for prerequisites, system requirements, unit tests, and packaging scripts.

**Windows x64 (PowerShell + VS 2022):**
```powershell
.\build.ps1            # Release build (use -Test to run CTest & smoke check, -Run to launch)
```

**Linux (GCC/Clang + Ninja):**
```bash
cmake --preset linux-gcc-ninja-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release --output-on-failure
```

**WebAssembly (Emscripten + Astro):**
```bash
source ~/emsdk/emsdk_env.sh
cmake --preset wasm-emscripten-release
cmake --build --preset wasm-emscripten-release --target iota_studio
```

## Repository Layout

```text
├── apps/        # iota_studio desktop/Wasm editor & opt-in iota_mcp stdio server
├── libs/        # core, model, preset_io, dsp, audio, rhi, rhi_gl45, rhi_webgl2, engine, export, discord, mcp
├── assets/      # Application icons and bundled fonts for WebAssembly preloading
├── installer/   # Windows Inno Setup 6 scripts & build_installer.ps1
├── packaging/   # Linux release tarball/AppImage bundler & Arch PKGBUILD
├── tests/       # Automated doctest suite
└── web/         # Astro 5 + Tailwind CSS website, Docs, GLSL Palette Manager & Web Studio host
```

## Documentation & License

- [Online Documentation Hub](https://vorlie.github.io/iota-studio/docs/) — Elements & 26 MVar modes, Custom GLSL Shaders, Linux/Wayland notes, Building & Architecture, and [MCP Reference](docs/MCP.md)
- Licensed under **GPL-3.0-or-later** — see [LICENSE](LICENSE).
