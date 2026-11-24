# Iota Studio

**Iota Studio** is a native Windows desktop application and modular C++20 engine for designing, previewing, and exporting audio-reactive music visualizers.

> [!WARNING]
> **Template Compatibility Disclaimer:** Any Avee Player template (`.viz`) saved or overwritten in Iota Studio will no longer be valid or loadable in Avee Player. Always keep a backup of your original Avee Player `.viz` files or save edits to a new `.iota` / `.viz` file instead of overwriting them.

## Purpose

Iota Studio provides a responsive, standalone desktop environment for creating layered audio-reactive graphics and rendering them directly to video—without relying on mobile runtimes, outdated store wrappers, or cloud pipelines. Its architecture separates the pure project/scene model from the Dear ImGui editor UI and graphics backend, delivering frame-rate-independent audio analysis so visualizers behave consistently at 60 FPS, 144 FPS, and during fixed-timestep offline video export.

## Key Features

- **Multi-Composition Scene Graph & Layer Types**
  - Compose multi-pass visual scenes with cross-composition references and layer blend modes.
  - Built-in element types: `AudioProvider`, `SoundKeys`, `Bars`, `Particles`, `Image`, `Text`, `StatText`, `BlurEffect`, `RgbSplitEffect`, `MotionBlurEffect`, `ShakeEffect`, `MirrorEffect`, `AppLogo`, and `CustomShader`.
- **Frame-Rate-Independent Audio Analysis (DSP)**
  - Three analysis providers: `Waveform`, `Spectrum` (`kissfft`), and `Spectrum2` (direct DFT filterbank).
  - Automatic Gain Control (`AgcEnvelope`), IEC A-weighting calibration, beat detection, and a 64-frame time-indexed bar history ring (`BarHistoryRing`) for layer reaction delay and accumulation.
  - Continuous exponential **Temporal Smoothing** with configurable `temporalSmooth`, `attackMs`, and `releaseMs` (plus frame-rate-independent fallback for legacy `smooth` presets).
  - Separate **Frequency Smoothing** stage across spectrum bins (`None`, `MovingAverage`, `Gaussian`, `SavitzkyGolay`) before temporal envelopes.
- **Sound Keys (Spectrum Range Extraction)**
  - Dedicated `SoundKeys` layer and docked **Sound Keys** panel with 3 independent frequency/amplitude bands (`R1`, `R2`, `R3`), interactive click-and-drag spectrum box selection, a `Full` range expansion shortcut, and auto-headroom ceiling tracking (`autoScaleCeiling`) so spectrum peaks stay unclipped.
  - Supports `Average`, `Peak`, and `OnOff` range evaluation modes alongside frame-rate-independent `None`, `Linear`, `Exponential`, and `Integrate` falloff modes, `attackMs`, `falloffTimeSec`, `integrateSpeed`, and `outputMin`/`outputMax` scaling.
  - Feeds `SoundKeys1..3` and `SoundKeysShake1..3` bindings into any `MVar` property (such as transforms, `BlurEffect::measureBlurRadius`, `RgbSplitEffect`, and particles) as well as the `vec4 u_soundKeys` (`(R1, R2, R3, beat)`) uniform in `CustomShader`.
- **Audio-Reactive Properties & Keyframes**
  - Driven via multi-slot `MVar` evaluators (`Beat`, `TotalTime`, `TotalTimeAndBeat`, `PeakBarIndex`, `TrackPosition`, `SoundKeys1..3`, `SoundKeysShake1..3`, harmonic/beat camera shake generators, `BeatTriggerAnim`) plus optional desktop property keyframe tracks.
- **Visual Geometry & Curve Interpolation**
  - Configurable `Bars` shapes (`Circle`, `HorizontalLine`, `SidedPolygon`, `Letter`) and segment renderers (`Bars`, `SharpBars`, `RoundBars`, `Line`).
  - Render-time **Curve Interpolation** (`None`, `Linear`, `CatmullRom`, `CubicBSpline` with configurable subdivisions) decoupled from DSP spectrum calculations.
- **GPU Blur & Motion Blur Effects**
  - Resolution-independent `BlurEffect` supporting **Dual Kawase** (continuous downsample + tent upsample pyramid), separable **Gaussian**, **Radial** zoom, and **Directional** blur modes, audio-reactive `measureBlurRadius`, single-composition canvas-below blurring (`sourceCompositionIndex = 0`), and per-layer `Image` blur (`blurEnabled`, `blurDivider`, `blurRadius`).
  - Frame-rate-independent `MotionBlurEffect` with an IGN-jittered 4–64 sample shutter blur (`shutterAngle`, `shutterPhase`, `blurSamples`, `blurCenter`) across translation, signed radial zoom, and aspect-corrected spin, supporting `EffectTransform`, `Manual` (`manualAngle`/`manualDirectional`/`manualZoom`/`manualSpin`), `Temporal` (exponential persistence buffer with `Mix`/`Lighten`/`Screen` trail blending), and `Hybrid` modes alongside single-composition canvas-below capture.
- **Composition-Wide Camera Shake (`ShakeEffect`)**
  - Full BorisFX Sapphire `S_Shake` parity across 4 axes (`X`, `Y`, `Z Dist`, and aspect-corrected `Tilt` roll) defaulting to `TargetImage = composition:0` to shake everything rendered below on the canvas.
  - Supports `Normal`, `Twitchy` (`stillness`, `twitchFreq`), and `Jumpy` (`drift`, `centerBias`) shake styles, C1 Catmull-Rom fractal random noise (`octaves = 1..10`), periodic sine waves, per-axis phases (`xPhase`, `yPhase`, `zPhase`, `tiltPhase`), `twitchAmp` impulses, deterministic `seed` / `phase` / `still` freeze, and audio-reactive `measureAmplitude` (`Beat` / `SoundKeys1..3`).
  - Features independent per-channel **Channels** splitting (`redAmp`/`greenAmp`/`blueAmp`, `redPhase`/`greenPhase`/`bluePhase`, `rgbRandomness`, `rgbFrequency`, plus `rgbPhase`), Lagrange-curved shutter motion blur (`moBlurLength`, `blurSamples`), **Crop Input** (`cropLeft`/`cropRight`/`cropTop`/`cropBottom`), `wrapX`/`wrapY` border modes (`Reflect`, `Tile`, `Clamp`, `No`), HDR emissive gain (`hdrIntensity`), and 7 1-click presets (`Cinematic Ambient`, `Handheld Drift`, `Bass Impact`, `Twitchy Burst`, `Jumpy Strobe`, `Z-Pump Bounce`, `RGB Glitch Twitch`).
- **3D Particular-Grade Particle Engine (`ParticleSystem`)**
  - Up to 50,000 active particles with `preRunSec` warm-up, velocity inheritance (`velocityFromMotion`), 3D cone `directionSpread`, and `MVar`-driven emission (`measureEmitRate` / `MeasureOverallSpeed`).
  - Eight emitter geometries: `Rectangle`, `HorizontalLine`, `Point`, `Box3D`, `Sphere`, `Ring`, `Grid`, and `LayerMask` (GPU-sampled color/alpha emission from any composition or canvas-below layers).
  - Frame-rate-independent physics & air: exponential `airResistance`, gravity & wind XYZ, `Vortex`/`Attract`/`Repel`/`TurbulentVortex` force fields, 3-octave 3D divergence-free curl turbulence (`turbulenceAffectPos`/`Size`/`Opacity`), spin/velocity orientation, and floor bounce collision.
  - Procedural GPU particle shaders (`Sprite`, `Sphere`, `GlowSphere`, `Star`, `BokehRing`, `Streaklet`) with `feather`, velocity `trailLength`, over-life size/opacity curves, 3-stop `ColorFrom`/`ColorMid`/`ColorTo` gradients, **Aux System** sub-particles (`Continuously`, `AtBounce`, `AtDeath`), true **3D Orbit Camera** with depth sorting, and **Depth-of-Field (DoF) Bokeh**, plus 1-click Inspector presets (`3D Bokeh`, `Audio Sparks`, `Curl Smoke`, `Aux Trails`, `Layer Emit`).
- **HDR (`RGBA16F`) Rendering, Physical Bloom & Filmic Tonemapping**
  - Switchable 16-bit half-float (`RGBA16F`) composition and intermediate effect buffers with live `SDR8` compatibility fallback and GPU float readback (`readRenderTargetRgbaFloat`).
  - Per-layer HDR emissive gain (`hdrIntensity` + audio-reactive `measureHdrIntensity` `MVar`) across `Image`, `Bars`, `Particles`, `Text`, `BlurEffect`, `RgbSplitEffect`, and `MotionBlurEffect`.
  - 6-mip Jimenez 2014 energy-conserving **Physical Bloom** pyramid with Karis anti-flicker prefiltering, soft-knee threshold (`bloomThreshold`, `bloomSoftKnee`), `bloomRadius`, optional anamorphic stretch (`bloomAnamorphic`), `bloomTint`, and audio-reactive `measureBloomIntensity`.
  - Filmic **Tonemapping** (`ACES` RRT+ODT, `AgX`, `PBRNeutral`, `Reinhard`, or `None`) with `exposureEv`, `contrast`, `saturation`, `gamma`, `vignette`, 8-bit IGN dithering, and 1-click Composition 0 HDR presets (`Clean SDR`, `ACES Cinema`, `AgX Neon`, `Anamorphic Flare`, `Audio Pulse Bloom`).
- **Unicode Typography & Dynamic GPU Font Atlas**
  - DirectWrite-shaped `FontAtlasCache` with automatic multi-script OS font fallback (Latin-Ext, Cyrillic, Greek, Japanese/Chinese CJK, Korean Hangul, Arabic/Hebrew RTL BiDi shaping, and symbol sets) plus safe `U+FFFD` sanitization for malformed UTF-8 sequences.
  - Dynamic 2048×2048 shelf-packed GPU glyph atlas with quantized resolution buckets, sub-region texture uploads (`updateTexture2DRegion`), custom `.ttf`/`.otf`/`.ttc` and bundled `local:` font loading, single-draw-call circularly dilated outlines & drop shadows, weight/italic/alignment/tracking/line-spacing/word-wrap controls, and 1-click typography presets (`Clean Title`, `Neon Glow`, `Bold Outline`, `Cinema Wide`).
- **Docking Studio Editor UI**
  - Multi-panel Dear ImGui workspace (`IMGUI_USE_WCHAR32` with merged Unicode system UI fonts) featuring **Viewport** (720p, 1080p, 9:16, 1:1 presets, optional FXAA, HDR16F/Bloom/Tonemapping toggles), **Hierarchy**, **Assets**, **Inspector**, **Audio & Timeline**, and **Sound Keys**.
  - **Hierarchy Management**: Multi-selection (`Ctrl+Click`, `Shift+Click`, `Ctrl+A`), drag-and-drop layer reordering with insertion guides both within and across compositions, context-menu cross-composition moves, **Copy / Cut / Paste / Duplicate** (`Ctrl+C/X/V/D`) with fresh ID allocation, synchronized keyframe tracks, and OS JSON clipboard support, keyboard layer nudging (`Ctrl+Up/Down`, `Ctrl+Shift+Up/Down`), **Pre-Compose Selected**, per-layer Solo (`S`) toggles, inline renaming (`F2` or double-click), live layer filtering, and secondary composition duplication/reordering/deletion with automatic reference remapping.
  - Inspector supports both slider/drag editing (`Ctrl+Click` for inline typing) and a **Precise Inputs** toggle (`Edit -> Precise Numeric Inputs` or the Inspector checkbox) for exact keyboard entry, plus full snapshot Undo/Redo (`CommandStack`).
- **Asset Browser & Project Manager**
  - **Assets** panel (`Viewport -> Asset Browser`) with live thumbnail preview header, layer reference usage counts, Grid/List views, category filters (`All`, `Project`, `Audio`, `Comps`, `Built-in`), live search, custom virtual bins, multi-file image import, rename/replace/unused-asset cleanup, and drag-and-drop onto Hierarchy layers or Inspector image/mask fields.
  - **Project Manager** (`File -> Project Manager...` / `Ctrl+N`) with starter templates (`Minimal Circular`, `Dual Composition Glow`, `Horizontal Spectrum Bar`, `Blank Project`), persistent recent-project history (`%LOCALAPPDATA%/IotaStudio/projects.json`), missing-file pruning, and per-project resolution/FPS/audio/notes metadata.
- **Projects, Presets, Bundled Assets & Video Export**
  - Load and save native `.iota` project archives alongside `.viz` presets and `.json` scenes, with embedded `local:<asset>` image resources, `ProjectMetadata`, and procedural `internalres:*` textures.
  - Cross-platform wide Unicode path handling (`utf8_path`) and UTF-8 sanitization for international filenames, legacy audio metadata tags, UTF-8 BOMs, ZIP archive entry names, and JSON serialization.
  - Audio decoding via `ffmpeg`/`ffprobe` (with `miniaudio` fallback) and deterministic offline MP4 encoding (H.264 / H.265 + AAC) piped to `ffmpeg`.

## Architecture & Directory Layout

Core logic lives in UI-agnostic C++20 libraries under `libs/`, consumed by the desktop application (`apps/studio/`) and test suite (`tests/`):

```text
.
├── apps/
│   └── studio/       # Dear ImGui docking editor (Viewport, Hierarchy, Assets, Inspector, Sound Keys, Project Manager)
├── libs/
│   ├── core/         # Math types, color codecs (ARGB/HSLA), procedural textures, ZIP & UTF-8/Unicode path utilities
│   ├── model/        # Decoupled scene graph, PropertyBag, ProjectMetadata, MVar expressions, Undo/Redo stack
│   ├── preset_io/    # .iota / .viz / .json serialization, starter templates, and ProjectLibrary recent registry
│   ├── dsp/          # Waveform/FFT/DFT analysis, SoundKeys bands, attack/release, frequency smoothing, curve splines
│   ├── audio/        # Audio decoding (FFmpeg/miniaudio) and miniaudio real-time playback/PCM windows
│   ├── rhi/          # Backend-agnostic Rendering Hardware Interface (abstract render device/resources)
│   ├── rhi_gl45/     # OpenGL 4.5 implementation of iota_rhi (Rgba8 & Rgba16F render targets, float readback, sub-rect updates)
│   ├── engine/       # GPU scene composer, HDR16F Bloom/Tonemapping, ParticleSystem, ShakeEffect, DirectWrite font atlas, blur/motion blur
│   └── export/       # Deterministic fixed-timestep offscreen renderer -> FFmpeg MP4 video exporter
└── tests/            # Automated doctest suite (unit, SoundKeys, ParticleSystem, ShakeEffect, Unicode IO, and OpenGL 4.5 tests)
```

## Tech Stack

| Area | Technology |
|---|---|
| Language & Build | C++20, CMake 3.24+, Ninja (`CMakePresets.json`), MSVC x64 |
| UI & Typography | Dear ImGui (`v1.93.0-docking`, `IMGUI_USE_WCHAR32`), GLFW 3.4, Windows DirectWrite |
| Graphics | OpenGL 4.5 via `iota_rhi` + GLM 1.0.1 |
| Audio Playback & Analysis | `miniaudio`, `kissfft` (`float`), custom DFT filterbank |
| Media & Packaging | `ffmpeg` / `ffprobe` (external CLI in `PATH`), `miniz`, `nlohmann/json`, `stb` |
| Testing | `doctest` (`v2.4.12`) |

Third-party C/C++ libraries are fetched automatically at configure time via CMake `FetchContent`.

## System Requirements

| Component | Minimum (720p/1080p SDR, standard templates) | Recommended (1080p–4K, HDR16F + Bloom, heavy templates & fast export) |
|---|---|---|
| **OS** | Windows 10 / 11 (64-bit) | Windows 10 / 11 (64-bit) |
| **CPU** | 64-bit dual/quad-core CPU (e.g., Intel Core i3 6th-gen+ / AMD Ryzen 3) | 6+ core modern CPU (e.g., Intel Core i5 10th-gen+ / AMD Ryzen 5 3600+) for stacked multi-provider `Spectrum2` DFTs, high particle counts, and FFmpeg encoding |
| **GPU** | **OpenGL 4.5**-capable GPU with 1 GB VRAM (e.g., Intel HD 520 / UHD 620+, NVIDIA GTX 650 / 750 Ti+, AMD R7 260 / Vega iGPU+) | Dedicated **OpenGL 4.5+** GPU with 4 GB+ VRAM (e.g., NVIDIA GTX 1060 / 1650+, AMD RX 570 / 5500 XT+) for multi-composition `RGBA16F` HDR, 6-mip Bloom, and motion/shake effects |
| **RAM** | 4 GB | 8 GB+ (16 GB for large 4K projects with high-resolution assets) |
| **Storage** | ~50 MB for the application + space for audio/video exports | SSD with free space for project assets and exported MP4s |
| **Runtime** | Microsoft Visual C++ 2015–2022 Redistributable (x64); optional `ffmpeg.exe` & `ffprobe.exe` on `PATH` | Same (`ffmpeg.exe` & `ffprobe.exe` on `PATH` for broad audio decoding and MP4 export) |

> [!NOTE]
> Older integrated GPUs capped at OpenGL 4.0–4.3 on Windows (such as Intel HD 4000 / 4th-gen Haswell) are not supported because Iota Studio requires an OpenGL 4.5 Core Profile context.

## Building & Running (Windows x64)

### Prerequisites

- **Visual Studio 2022** (or Build Tools) with C++ x64 toolchain (`vcvars64.bat`)
- **CMake 3.24+** and **Ninja**
- OpenGL 4.5-capable GPU driver
- *(Optional)* `ffmpeg.exe` and `ffprobe.exe` available on `PATH` for broad audio format decoding and MP4 video export

### Configure & Build

From a Developer Command Prompt for VS 2022 (or after calling `vcvars64.bat`):

```cmd
cmake --preset windows-msvc-ninja-release
cmake --build --preset release --parallel
```

*(Use `windows-msvc-ninja-debug` / `debug` for a Debug build.)*

### Run Iota Studio

```powershell
.\build\release\apps\studio\iota_studio.exe
```

Run the headless OpenGL smoke check (creates a hidden window, renders starter frames, and exits):

```powershell
.\build\release\apps\studio\iota_studio.exe --smoke-test
```

### Run Tests

Core DSP, SoundKeys, model, Unicode/non-UTF-8 IO, `.iota`/`.viz` round-trip, and OpenGL tests live in `iota_tests.exe`. Two preset-integration test cases expect `IOTA_TEST_PRESETS_DIR` to point to a folder containing `visualizer_0.json`..`visualizer_17.json`:

```powershell
# With external preset fixtures available:
$env:IOTA_TEST_PRESETS_DIR = "C:\path\to\preset_jsons"
.\build\release\tests\iota_tests.exe

# Without external preset fixtures (excludes preset-fixture test cases):
.\build\release\tests\iota_tests.exe "-tce=*APK presets*,*built-in presets*"
```
