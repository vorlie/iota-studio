# Iota Studio

**Iota Studio** is a native Windows desktop application and modular C++20 engine for designing, previewing, and exporting audio-reactive music visualizers.

## Purpose

Iota Studio provides a responsive, standalone desktop environment for creating layered audio-reactive graphics and rendering them directly to video—without relying on mobile runtimes, outdated store wrappers, or cloud pipelines. Its architecture separates the pure project/scene model from the Dear ImGui editor UI and graphics backend, delivering frame-rate-independent audio analysis so visualizers behave consistently at 60 FPS, 144 FPS, and during fixed-timestep offline video export.

## Key Features

- **Multi-Composition Scene Graph & Layer Types**
  - Compose multi-pass visual scenes with cross-composition references and layer blend modes.
  - Built-in element types: `AudioProvider`, `Bars`, `Particles`, `Image`, `Text`, `StatText`, `BlurEffect`, `RgbSplitEffect`, `MotionBlurEffect`, `MirrorEffect`, `AppLogo`, and `CustomShader`.
- **Frame-Rate-Independent Audio Analysis (DSP)**
  - Three analysis providers: `Waveform`, `Spectrum` (`kissfft`), and `Spectrum2` (direct DFT filterbank).
  - Automatic Gain Control (`AgcEnvelope`), IEC A-weighting calibration, beat detection, and a 64-frame time-indexed bar history ring (`BarHistoryRing`) for layer reaction delay and accumulation.
  - Continuous exponential **Temporal Smoothing** with configurable `temporalSmooth`, `attackMs`, and `releaseMs` (plus frame-rate-independent fallback for legacy `smooth` presets).
  - Separate **Frequency Smoothing** stage across spectrum bins (`None`, `MovingAverage`, `Gaussian`, `SavitzkyGolay`) before temporal envelopes.
- **Audio-Reactive Properties & Keyframes**
  - Driven via multi-slot `MVar` evaluators (`Beat`, `TotalTime`, `TotalTimeAndBeat`, `PeakBarIndex`, `TrackPosition`, harmonic/beat camera shake generators, `BeatTriggerAnim`) plus optional desktop property keyframe tracks.
- **Visual Geometry & Curve Interpolation**
  - Configurable `Bars` shapes (`Circle`, `HorizontalLine`, `SidedPolygon`, `Letter`) and segment renderers (`Bars`, `SharpBars`, `RoundBars`, `Line`).
  - Render-time **Curve Interpolation** (`None`, `Linear`, `CatmullRom`, `CubicBSpline` with configurable subdivisions) decoupled from DSP spectrum calculations.
- **Docking Studio Editor UI**
  - Multi-panel Dear ImGui workspace with live **Viewport** (720p, 1080p, 9:16, 1:1 presets, optional FXAA), **Hierarchy**, **Inspector**, and **Audio & Timeline** transport.
  - Inspector supports both slider/drag editing (`Ctrl+Click` for inline typing) and a **Precise Inputs** toggle (`Edit -> Precise Numeric Inputs` or the Inspector checkbox) for exact keyboard entry, plus full snapshot Undo/Redo (`CommandStack`).
- **Presets, Bundled Assets & Video Export**
  - Load and save `.json` scenes and compressed `.viz` archive presets with embedded `local:<asset>` image resources and procedural `internalres:*` textures.
  - Audio decoding via `ffmpeg`/`ffprobe` (with `miniaudio` fallback) and deterministic offline MP4 encoding (H.264 / H.265 + AAC) piped to `ffmpeg`.

## Architecture & Directory Layout

Core logic lives in UI-agnostic C++20 libraries under `libs/`, consumed by the desktop application (`apps/studio/`) and test suite (`tests/`):

```text
.
├── apps/
│   └── studio/       # Dear ImGui docking editor executable (iota_studio)
├── libs/
│   ├── core/         # Math types, color codecs (ARGB/HSLA), procedural internal textures
│   ├── model/        # Decoupled scene graph, PropertyBag, MVar expressions, Undo/Redo stack
│   ├── preset_io/    # .json and zipped .viz preset serialization & local asset bundling
│   ├── dsp/          # Waveform/FFT/DFT analysis, attack/release, frequency smoothing, curve splines, MVar evaluator
│   ├── audio/        # Audio decoding (FFmpeg/miniaudio) and miniaudio real-time playback/PCM windows
│   ├── rhi/          # Backend-agnostic Rendering Hardware Interface (abstract render device/resources)
│   ├── rhi_gl45/     # OpenGL 4.5 implementation of iota_rhi
│   ├── engine/       # GPU scene composer, particle simulator, font atlas cache, post-process effects
│   └── export/       # Deterministic fixed-timestep offscreen renderer -> FFmpeg MP4 video exporter
└── tests/            # Automated doctest suite (unit + headless OpenGL 4.5 rendering tests)
```

## Tech Stack

| Area | Technology |
|---|---|
| Language & Build | C++20, CMake 3.24+, Ninja (`CMakePresets.json`), MSVC x64 |
| UI | Dear ImGui (`v1.93.0-docking`) + GLFW 3.4 |
| Graphics | OpenGL 4.5 via `iota_rhi` + GLM 1.0.1 |
| Audio Playback & Analysis | `miniaudio`, `kissfft` (`float`), custom DFT filterbank |
| Media & Packaging | `ffmpeg` / `ffprobe` (external CLI in `PATH`), `miniz`, `nlohmann/json`, `stb` |
| Testing | `doctest` (`v2.4.12`) |

Third-party C/C++ libraries are fetched automatically at configure time via CMake `FetchContent`.

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

Core DSP, model, round-trip `.viz`, and OpenGL tests live in `iota_tests.exe`. Two preset-integration test cases expect `IOTA_TEST_PRESETS_DIR` to point to a folder containing `visualizer_0.json`..`visualizer_17.json`:

```powershell
# With external preset fixtures available:
$env:IOTA_TEST_PRESETS_DIR = "C:\path\to\preset_jsons"
.\build\release\tests\iota_tests.exe

# Without external preset fixtures (excludes preset-fixture test cases):
.\build\release\tests\iota_tests.exe "-tce=*APK presets*,*built-in presets*"
```
