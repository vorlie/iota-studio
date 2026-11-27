export const SITE = {
  name: 'Iota Studio',
  tagline: 'Cross-Platform Audio-Reactive Visualizer Studio',
  description:
    'Native C++20 desktop application and OpenGL 4.5 engine for designing, previewing, and exporting audio-reactive music visualizers on Windows and Linux.',
  repoOwner: 'vorlie',
  repoName: 'iota-studio',
  repoUrl: 'https://github.com/vorlie/iota-studio',
  releasesUrl: 'https://github.com/vorlie/iota-studio/releases',
  issuesUrl: 'https://github.com/vorlie/iota-studio/issues',
  apiLatestUrl: 'https://api.github.com/repos/vorlie/iota-studio/releases/latest',
  fallbackVersion: '1.14.0',
};

export interface NavItem {
  label: string;
  href: string;
  matchPrefix?: string;
}

export const NAV_ITEMS: NavItem[] = [
  { label: 'Home', href: '/' },
  { label: 'Features', href: '/features/' },
  { label: 'Web Studio', href: '/studio/' },
  { label: 'Download', href: '/download/' },
  { label: 'Docs', href: '/docs/', matchPrefix: '/docs/' },
  { label: 'Palette Manager', href: '/palette/' },
];

export interface DocsNavItem {
  title: string;
  href: string;
  summary: string;
}

export const DOCS_NAV: DocsNavItem[] = [
  {
    title: 'Getting Started',
    href: '/docs/',
    summary: 'Overview, project formats (.iota / .viz / .json), portable audio paths, and shortcuts.',
  },
  {
    title: 'Elements & MVars',
    href: '/docs/elements/',
    summary: 'All 14 layer types, SoundKeys bands, and all 26 MVar audio-reactive signal modes.',
  },
  {
    title: 'Custom GLSL Shaders',
    href: '/docs/shaders/',
    summary: 'GLSL 450 uniforms, canvas-below Post-FX, and OKLab / linear RGB color palettes.',
  },
  {
    title: 'MCP & Live Bridge',
    href: '/docs/mcp/',
    summary: 'Opt-in stdio MCP server (iota_mcp), Live Studio Bridge IPC, and safe working copies.',
  },
  {
    title: 'Linux & Wayland',
    href: '/docs/linux/',
    summary: 'Distro packages, CMake presets, Wayland vblank pacing, VA-API export, and Arch PKGBUILD.',
  },
  {
    title: 'Building & Architecture',
    href: '/docs/building/',
    summary: 'System requirements, tech stack, module layout, Windows/Linux/Wasm builds, tests, and installers.',
  },
];

export interface ElementInfo {
  name: string;
  category: 'Audio' | 'Visual' | 'Effect';
  summary: string;
}

export const ELEMENTS: ElementInfo[] = [
  { name: 'AudioProvider', category: 'Audio', summary: 'Waveform, kissfft Spectrum, or direct DFT Spectrum2 analyzer with AGC and IEC A-weighting.' },
  { name: 'SoundKeys', category: 'Audio', summary: 'Extracts 3 independent frequency/amplitude bands (R1, R2, R3) with interactive visual selection and falloff modes.' },
  { name: 'Bars', category: 'Visual', summary: 'Circle, HorizontalLine, SidedPolygon, or Letter shapes rendered as Bars, SharpBars, RoundBars, or spline-interpolated curves.' },
  { name: 'Particles', category: 'Visual', summary: 'Up to 50,000 3D GPU particles with 8 emitter shapes, curl turbulence, force fields, aux sub-particles, and DoF bokeh.' },
  { name: 'Image', category: 'Visual', summary: 'Bundled or procedural textures with optional per-layer Gaussian/Kawase blur and HDR emissive gain.' },
  { name: 'Text', category: 'Visual', summary: 'Unicode-shaped typography with dynamic glyph atlas, circularly dilated outlines, drop shadows, word wrap, and live tokens.' },
  { name: 'StatText', category: 'Visual', summary: 'Dynamic live telemetry and audio track readout overlay driven by text tokens.' },
  { name: 'AppLogo', category: 'Visual', summary: 'Stylized logo element with alpha/glow controls compatible with preset compositions.' },
  { name: 'CustomShader', category: 'Visual', summary: 'User GLSL 450 generator or canvas-below Post-FX shader with live editing and SoundKeys uniforms.' },
  { name: 'BlurEffect', category: 'Effect', summary: 'Dual Kawase, Gaussian, Radial, and Directional GPU blur supporting composition:0 canvas-below capture.' },
  { name: 'RgbSplitEffect', category: 'Effect', summary: 'Audio-reactive per-channel chromatic aberration split across radial or directional offsets.' },
  { name: 'MotionBlurEffect', category: 'Effect', summary: 'IGN-jittered 4–64 sample shutter blur across translation, radial zoom, spin, and temporal persistence trails.' },
  { name: 'ShakeEffect', category: 'Effect', summary: '4-axis S_Shake-grade camera shake (X/Y/Z/Tilt) with fractal Perlin/Catmull noise, per-channel RGB split, and crop.' },
  { name: 'TwitchEffect', category: 'Effect', summary: 'SoundKeys-driven multi-operator glitch (Slide + RGB Split, Scale + Tilt, Light Strobe, Color, Blur, Glitch Slice).' },
  { name: 'MirrorEffect', category: 'Effect', summary: 'Horizontal, vertical, or quadrant symmetry mirroring over compositions or canvas-below layers.' },
];

export const MVAR_MODES: string[] = [
  'Nothing',
  'Constant',
  'IsPlaying',
  'Beat',
  'BeatCamShakeMore',
  'BeatCamShakeLess',
  'TotalTime',
  'TotalTimeBackward',
  'TotalTimeWhenPlaying',
  'ConstantShakeMore',
  'ConstantShake',
  'BeatRandomShake',
  'TrackPosition',
  'TotalTimeAndBeat',
  'ConstantShakeRotMore',
  'ConstantShakeRotLess',
  'BeatCamShakeRotMore',
  'BeatCamShakeRotLess',
  'PeakBarIndex',
  'BeatTriggerAnim',
  'SoundKeys1',
  'SoundKeys2',
  'SoundKeys3',
  'SoundKeysShake1',
  'SoundKeysShake2',
  'SoundKeysShake3',
];

export interface BuiltinShaderInfo {
  id: string;
  title: string;
  fileName: string;
  category: 'Audio-Reactive' | 'Procedural & Fluid' | 'Cinematic & Lens' | 'Geometric & Sci-Fi' | 'Retro & Artistic';
  kind: 'Post-FX' | 'Generator';
  paletteSpace: 'linear' | 'oklab' | 'none';
  description: string;
}

export const BUILTIN_SHADERS: BuiltinShaderInfo[] = [
  { id: 'bass_shockwave', title: 'Bass Shockwave', fileName: 'BassShockwave.glsl', category: 'Audio-Reactive', kind: 'Post-FX', paletteSpace: 'linear', description: 'Expanding radial bass shockwave ring with subtle chromatic fringe driven by SoundKeys bass and beat.' },
  { id: 'beat_glitch', title: 'Beat Glitch & Slice', fileName: 'BeatGlitch.glsl', category: 'Audio-Reactive', kind: 'Post-FX', paletteSpace: 'linear', description: 'Beat-triggered horizontal slice displacement and RGB tear above threshold.' },
  { id: 'audio_warp', title: '3-Band Audio Warp', fileName: 'AudioWarp.glsl', category: 'Audio-Reactive', kind: 'Post-FX', paletteSpace: 'none', description: 'Bass radial zoom, mid-band UV swirl, and high-band shimmer driven by u_soundKeys.' },
  { id: 'audio_nebula', title: 'Audio-Reactive Nebula', fileName: 'AudioNebula.glsl', category: 'Audio-Reactive', kind: 'Generator', paletteSpace: 'linear', description: 'Domain-warped FBM nebula clouds reactive to bass, mids, highs, and beat.' },
  { id: 'fluid_gradient', title: 'Fluid Gradient (OKLab)', fileName: 'FluidGradient.glsl', category: 'Procedural & Fluid', kind: 'Generator', paletteSpace: 'oklab', description: 'Smooth OKLab domain-warped multi-stop gradient compatible with the Web Palette Manager.' },
  { id: 'liquid_flow', title: 'Liquid Flow Ridges', fileName: 'LiquidFlow.glsl', category: 'Procedural & Fluid', kind: 'Generator', paletteSpace: 'linear', description: 'Flowing iridescent silk plasma waves modulated by beat and bass.' },
  { id: 'voronoi_caustics', title: 'Voronoi Caustics', fileName: 'VoronoiCaustics.glsl', category: 'Procedural & Fluid', kind: 'Generator', paletteSpace: 'linear', description: 'Animated cellular caustics and energy web that blends cleanly via Screen.' },
  { id: 'hyperdrive_tunnel', title: 'Hyperdrive Tunnel', fileName: 'HyperdriveTunnel.glsl', category: 'Procedural & Fluid', kind: 'Generator', paletteSpace: 'linear', description: 'Polar warp star tunnel accelerating on audio beat and bass hits.' },
  { id: 'film_grain', title: 'Analog Film Grain', fileName: 'FilmGrain.glsl', category: 'Cinematic & Lens', kind: 'Post-FX', paletteSpace: 'none', description: 'Luminance-weighted animated film grain to add organic texture and eliminate banding.' },
  { id: 'optical_lens', title: 'Optical Lens & Radial CA', fileName: 'OpticalLens.glsl', category: 'Cinematic & Lens', kind: 'Post-FX', paletteSpace: 'none', description: 'Barrel curvature, beat pump, and edge-weighted radial chromatic aberration.' },
  { id: 'halation_streaks', title: 'Halation & Anamorphic Streaks', fileName: 'HalationStreaks.glsl', category: 'Cinematic & Lens', kind: 'Post-FX', paletteSpace: 'linear', description: 'Warm film halation glow combined with horizontal anamorphic highlight streaks.' },
  { id: 'kaleidoscope', title: 'Kaleidoscope Mirror', fileName: 'Kaleidoscope.glsl', category: 'Geometric & Sci-Fi', kind: 'Post-FX', paletteSpace: 'none', description: 'N-fold polar symmetry with slow rotation and beat-reactive zoom.' },
  { id: 'synthwave_grid', title: 'Synthwave Horizon Grid', fileName: 'SynthwaveGrid.glsl', category: 'Geometric & Sci-Fi', kind: 'Generator', paletteSpace: 'linear', description: 'Scrolling 3D perspective neon wireframe grid beneath a sliced retro sunset.' },
  { id: 'crt_vhs', title: 'CRT & VHS Simulation', fileName: 'CrtVhs.glsl', category: 'Retro & Artistic', kind: 'Post-FX', paletteSpace: 'none', description: 'Curved CRT phosphor scanlines, VHS chroma bleed, and beat-reactive tracking jitter.' },
  { id: 'halftone_dither', title: 'Halftone & Bayer Dither', fileName: 'HalftoneDither.glsl', category: 'Retro & Artistic', kind: 'Post-FX', paletteSpace: 'linear', description: 'Stylized halftone dots and 4x4 ordered Bayer dithering with customizable ink/paper palette.' },
];
