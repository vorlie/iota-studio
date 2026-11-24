#include "iota/rhi/gl45_device.hpp"

#include "gl_loader.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iota::rhi {

namespace {

constexpr size_t kRingRegions = 3;
constexpr size_t kVertsPerRegion = 200000;
constexpr size_t kIndicesPerRegion = 600000;
constexpr GLuint kInvalidGlId = std::numeric_limits<GLuint>::max();

GLint toGlWrap(TextureWrap wrap) noexcept {
  switch (wrap) {
    case TextureWrap::Repeat: return GL_REPEAT;
    case TextureWrap::MirroredRepeat: return GL_MIRRORED_REPEAT;
    default: return GL_CLAMP_TO_EDGE;
  }
}

GLenum toGlTopology(PrimitiveTopology topo) noexcept {
  switch (topo) {
    case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
    case PrimitiveTopology::Lines: return GL_LINES;
    default: return GL_TRIANGLES;
  }
}

GLint toGlInternalFormat(PixelFormat fmt) noexcept {
  switch (fmt) {
    case PixelFormat::Rgba16F: return GL_RGBA16F;
    case PixelFormat::R11G11B10F: return GL_R11F_G11F_B10F;
    case PixelFormat::Rgba8:
    default: return GL_RGBA8;
  }
}

GLenum toGlUploadPixelType(PixelFormat fmt, bool hasPixelData) noexcept {
  if (hasPixelData) {
    return GL_UNSIGNED_BYTE;
  }
  switch (fmt) {
    case PixelFormat::Rgba16F:
    case PixelFormat::R11G11B10F:
      return GL_HALF_FLOAT;
    case PixelFormat::Rgba8:
    default:
      return GL_UNSIGNED_BYTE;
  }
}

struct TransparentStringHash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
  size_t operator()(const std::string& s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
};

struct TextureRecord {
  GLuint glId = 0;
  int width = 0;
  int height = 0;
  TextureFilter filter = TextureFilter::Linear;
  TextureWrap wrap = TextureWrap::ClampToEdge;
  PixelFormat format = PixelFormat::Rgba8;
};

struct RenderTargetRecord {
  GLuint fbo = 0;
  TextureHandle colorTex{};
  int width = 0;
  int height = 0;
  TextureFilter filter = TextureFilter::Linear;
  PixelFormat format = PixelFormat::Rgba8;
};

struct ShaderRecord {
  GLuint program = 0;
  std::unordered_map<std::string, GLint, TransparentStringHash, std::equal_to<>> uniforms;
};

template <typename T>
struct SlotEntry {
  bool alive = false;
  T value{};
};

}  // namespace

struct Gl45RenderDevice::Impl {
  gl::GlProcs gl{};
  bool ready = false;

  std::vector<SlotEntry<TextureRecord>> textures;
  std::vector<SlotEntry<RenderTargetRecord>> renderTargets;
  std::vector<SlotEntry<ShaderRecord>> shaders;

  // Triple-buffered persistent-mapped streaming geometry ring
  GLuint streamVao = 0;
  GLuint streamVbo = 0;
  GLuint streamIbo = 0;
  VertexPosColUv* mappedVerts = nullptr;
  uint32_t* mappedIndices = nullptr;
  bool persistentRingReady = false;
  size_t ringRegion = 0;
  size_t regionVertUsed = 0;
  size_t regionIdxUsed = 0;
  bool ringWrittenInRegion = false;
  std::array<gl::GLsync, kRingRegions> regionFences{};

  // Static fullscreen quad (zero upload)
  GLuint fsVao = 0;
  GLuint fsVbo = 0;
  GLuint fsIbo = 0;

  // Overflow / fallback dynamic buffers (orphaned GL_STREAM_DRAW)
  GLuint overflowVao = 0;
  GLuint overflowVbo = 0;
  GLuint overflowIbo = 0;

  // Async double-buffered PBO pixel readback state
  std::array<GLuint, 2> asyncPbos{};
  size_t asyncPboCapBytes = 0;
  int asyncWriteIdx = 0;
  std::array<bool, 2> asyncPrimed{};
  std::array<int, 2> asyncW{};
  std::array<int, 2> asyncH{};
  std::vector<uint8_t> flipRowScratch;

  // Optional GL_TIME_ELAPSED GPU timer queries
  bool hasTimerQueries = false;
  std::array<GLuint, 2> timerQueries{};
  std::array<bool, 2> timerPrimed{};
  int timerWriteIdx = 0;
  bool timerActive = false;
  float lastGpuMs = 0.0f;

  // Cached GL state (reset on beginFrame / raw mutations)
  GLuint activeProgram = kInvalidGlId;
  GLuint cachedVao = kInvalidGlId;
  bool cachedFboValid = false;
  GLuint cachedFbo = 0;
  int cachedVpW = -1;
  int cachedVpH = -1;
  bool cachedBlendValid = false;
  iota::core::BlendMode cachedBlendMode = iota::core::BlendMode::Alpha;
  bool cachedScissorValid = false;
  bool cachedScissorEnabled = false;
  int cachedScissorX = 0;
  int cachedScissorY = 0;
  int cachedScissorW = 0;
  int cachedScissorH = 0;
  int cachedActiveTexUnit = -1;
  std::array<GLuint, 16> cachedBoundTex{};

  Impl() { cachedBoundTex.fill(kInvalidGlId); }

  template <typename T>
  static T* findSlot(std::vector<SlotEntry<T>>& table, uint32_t id) noexcept {
    if (id == 0 || id > table.size()) {
      return nullptr;
    }
    auto& slot = table[static_cast<size_t>(id - 1)];
    return slot.alive ? &slot.value : nullptr;
  }

  template <typename T>
  static const T* findSlot(const std::vector<SlotEntry<T>>& table, uint32_t id) noexcept {
    if (id == 0 || id > table.size()) {
      return nullptr;
    }
    const auto& slot = table[static_cast<size_t>(id - 1)];
    return slot.alive ? &slot.value : nullptr;
  }

  template <typename T>
  static uint32_t allocSlot(std::vector<SlotEntry<T>>& table, T&& val) {
    table.push_back(SlotEntry<T>{true, std::move(val)});
    return static_cast<uint32_t>(table.size());
  }

  void invalidateStateCache() noexcept {
    activeProgram = kInvalidGlId;
    cachedVao = kInvalidGlId;
    cachedFboValid = false;
    cachedBlendValid = false;
    cachedScissorValid = false;
    cachedActiveTexUnit = -1;
    cachedBoundTex.fill(kInvalidGlId);
  }

  void bindVaoCached(GLuint vao) noexcept {
    if (cachedVao != vao) {
      gl.BindVertexArray(vao);
      cachedVao = vao;
    }
  }

  void configureVertexAttributes() const noexcept {
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexPosColUv),
                           reinterpret_cast<const void*>(offsetof(VertexPosColUv, x)));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexPosColUv),
                           reinterpret_cast<const void*>(offsetof(VertexPosColUv, abgr)));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(VertexPosColUv),
                           reinterpret_cast<const void*>(offsetof(VertexPosColUv, u)));
  }

  void waitRegionFence(size_t region) noexcept {
    gl::GLsync fence = regionFences[region];
    if (fence == nullptr) {
      return;
    }
    GLenum waitRet = gl.ClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    if (waitRet != GL_ALREADY_SIGNALED && waitRet != GL_CONDITION_SATISFIED) {
      gl.ClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    }
    gl.DeleteSync(fence);
    regionFences[region] = nullptr;
  }

  void signalAndAdvanceRegion() noexcept {
    waitRegionFence(ringRegion);
    regionFences[ringRegion] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    ringRegion = (ringRegion + 1) % kRingRegions;
    regionVertUsed = 0;
    regionIdxUsed = 0;
    ringWrittenInRegion = false;
  }

  GLuint compileStage(GLenum stage, std::string_view src, std::string* outError) {
    const GLuint sh = gl.CreateShader(stage);
    const char* ptr = src.data();
    const GLint len = static_cast<GLint>(src.size());
    gl.ShaderSource(sh, 1, &ptr, &len);
    gl.CompileShader(sh);

    GLint status = 0;
    gl.GetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
      GLint logLen = 0;
      gl.GetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
      std::string log(static_cast<size_t>(std::max(1, logLen)), '\0');
      gl.GetShaderInfoLog(sh, logLen, nullptr, log.data());
      while (!log.empty() && log.back() == '\0') log.pop_back();
      if (outError) {
        *outError = (stage == GL_VERTEX_SHADER ? "Vertex shader compile failed: "
                                               : "Fragment shader compile failed: ") +
                    log;
      }
      gl.DeleteShader(sh);
      return 0;
    }
    return sh;
  }

  GLint findUniform(ShaderHandle sh, std::string_view name) {
    auto* rec = findSlot(shaders, sh.id);
    if (!rec || rec->program == 0) {
      return -1;
    }
    if (auto uIt = rec->uniforms.find(name); uIt != rec->uniforms.end()) {
      return uIt->second;
    }
    std::string key(name);
    const GLint loc = gl.GetUniformLocation(rec->program, key.c_str());
    rec->uniforms.emplace(std::move(key), loc);
    return loc;
  }

  void drawIndexedOverflow(PrimitiveTopology topology,
                           std::span<const VertexPosColUv> vertices,
                           std::span<const uint32_t> indices) noexcept {
    const size_t vertBytes = vertices.size() * sizeof(VertexPosColUv);
    const size_t idxBytes = indices.size() * sizeof(uint32_t);
    bindVaoCached(overflowVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, overflowVbo);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(vertBytes), nullptr, GL_STREAM_DRAW);
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, static_cast<gl::GLsizeiptr>(vertBytes), vertices.data());

    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, overflowIbo);
    gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(idxBytes), nullptr, GL_STREAM_DRAW);
    gl.BufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<gl::GLsizeiptr>(idxBytes), indices.data());

    glDrawElements(toGlTopology(topology), static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, nullptr);
  }

  void drawVerticesOverflow(PrimitiveTopology topology,
                            std::span<const VertexPosColUv> vertices) noexcept {
    const size_t vertBytes = vertices.size() * sizeof(VertexPosColUv);
    bindVaoCached(overflowVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, overflowVbo);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(vertBytes), nullptr, GL_STREAM_DRAW);
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, static_cast<gl::GLsizeiptr>(vertBytes), vertices.data());

    glDrawArrays(toGlTopology(topology), 0, static_cast<GLsizei>(vertices.size()));
  }
};

Gl45RenderDevice::Gl45RenderDevice() : impl_(std::make_unique<Impl>()) {}

Gl45RenderDevice::~Gl45RenderDevice() {
  shutdown();
}

bool Gl45RenderDevice::initialize(std::string* outError) {
  if (impl_->ready) {
    return true;
  }
  if (!gl::loadGlProcs(impl_->gl, outError)) {
    return false;
  }

  // 1. Triple-buffered persistent-mapped streaming ring buffers
  impl_->gl.GenVertexArrays(1, &impl_->streamVao);
  impl_->gl.GenBuffers(1, &impl_->streamVbo);
  impl_->gl.GenBuffers(1, &impl_->streamIbo);

  const size_t totalVboBytes = kRingRegions * kVertsPerRegion * sizeof(VertexPosColUv);
  const size_t totalIboBytes = kRingRegions * kIndicesPerRegion * sizeof(uint32_t);
  constexpr GLbitfield kPersistFlags =
      GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

  impl_->gl.BindVertexArray(impl_->streamVao);
  impl_->gl.BindBuffer(GL_ARRAY_BUFFER, impl_->streamVbo);
  impl_->gl.BufferStorage(GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(totalVboBytes), nullptr, kPersistFlags);
  impl_->mappedVerts = static_cast<VertexPosColUv*>(
      impl_->gl.MapBufferRange(GL_ARRAY_BUFFER, 0, static_cast<gl::GLsizeiptr>(totalVboBytes), kPersistFlags));

  impl_->gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl_->streamIbo);
  impl_->gl.BufferStorage(GL_ELEMENT_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(totalIboBytes), nullptr, kPersistFlags);
  impl_->mappedIndices = static_cast<uint32_t*>(
      impl_->gl.MapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<gl::GLsizeiptr>(totalIboBytes), kPersistFlags));

  impl_->configureVertexAttributes();
  impl_->persistentRingReady = (impl_->mappedVerts != nullptr && impl_->mappedIndices != nullptr);

  // 2. Static fullscreen clip-space quad (zero per-pass uploads)
  static constexpr VertexPosColUv kFullscreenQuad[4] = {
      {-1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
      {+1.0f, -1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
      {+1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
      {-1.0f, +1.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
  };
  static constexpr uint32_t kFullscreenIdx[6] = {0, 1, 2, 0, 2, 3};

  impl_->gl.GenVertexArrays(1, &impl_->fsVao);
  impl_->gl.GenBuffers(1, &impl_->fsVbo);
  impl_->gl.GenBuffers(1, &impl_->fsIbo);
  impl_->gl.BindVertexArray(impl_->fsVao);
  impl_->gl.BindBuffer(GL_ARRAY_BUFFER, impl_->fsVbo);
  impl_->gl.BufferData(GL_ARRAY_BUFFER, sizeof(kFullscreenQuad), kFullscreenQuad, GL_STATIC_DRAW);
  impl_->gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl_->fsIbo);
  impl_->gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kFullscreenIdx), kFullscreenIdx, GL_STATIC_DRAW);
  impl_->configureVertexAttributes();

  // 3. Overflow / fallback VAO & buffers
  impl_->gl.GenVertexArrays(1, &impl_->overflowVao);
  impl_->gl.GenBuffers(1, &impl_->overflowVbo);
  impl_->gl.GenBuffers(1, &impl_->overflowIbo);
  impl_->gl.BindVertexArray(impl_->overflowVao);
  impl_->gl.BindBuffer(GL_ARRAY_BUFFER, impl_->overflowVbo);
  impl_->gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl_->overflowIbo);
  impl_->configureVertexAttributes();

  impl_->gl.BindVertexArray(0);

  // 4. Optional double-buffered GL_TIME_ELAPSED GPU timer queries
  impl_->hasTimerQueries =
      (impl_->gl.GenQueries != nullptr && impl_->gl.DeleteQueries != nullptr &&
       impl_->gl.BeginQuery != nullptr && impl_->gl.EndQuery != nullptr &&
       impl_->gl.GetQueryObjectiv != nullptr && impl_->gl.GetQueryObjectui64v != nullptr);
  if (impl_->hasTimerQueries) {
    impl_->gl.GenQueries(2, impl_->timerQueries.data());
  }

  impl_->invalidateStateCache();
  impl_->ready = true;
  return true;
}

void Gl45RenderDevice::shutdown() {
  if (!impl_ || !impl_->ready) {
    return;
  }

  for (auto& fence : impl_->regionFences) {
    if (fence != nullptr) {
      impl_->gl.DeleteSync(fence);
      fence = nullptr;
    }
  }
  impl_->mappedVerts = nullptr;
  impl_->mappedIndices = nullptr;
  impl_->persistentRingReady = false;

  if (impl_->hasTimerQueries) {
    if (impl_->timerActive) {
      impl_->gl.EndQuery(GL_TIME_ELAPSED);
      impl_->timerActive = false;
    }
    impl_->gl.DeleteQueries(2, impl_->timerQueries.data());
    impl_->timerQueries.fill(0);
    impl_->timerPrimed.fill(false);
    impl_->hasTimerQueries = false;
  }

  if (impl_->asyncPbos[0] != 0 || impl_->asyncPbos[1] != 0) {
    impl_->gl.DeleteBuffers(2, impl_->asyncPbos.data());
    impl_->asyncPbos.fill(0);
    impl_->asyncPboCapBytes = 0;
    impl_->asyncPrimed.fill(false);
  }

  for (auto& slot : impl_->renderTargets) {
    if (slot.alive && slot.value.fbo != 0) {
      impl_->gl.DeleteFramebuffers(1, &slot.value.fbo);
    }
  }
  impl_->renderTargets.clear();

  for (auto& slot : impl_->textures) {
    if (slot.alive && slot.value.glId != 0) {
      glDeleteTextures(1, &slot.value.glId);
    }
  }
  impl_->textures.clear();

  for (auto& slot : impl_->shaders) {
    if (slot.alive && slot.value.program != 0) {
      impl_->gl.DeleteProgram(slot.value.program);
    }
  }
  impl_->shaders.clear();

  auto deleteVaoBuf = [&](GLuint& vao, GLuint& vbo, GLuint& ibo) {
    if (ibo != 0) { impl_->gl.DeleteBuffers(1, &ibo); ibo = 0; }
    if (vbo != 0) { impl_->gl.DeleteBuffers(1, &vbo); vbo = 0; }
    if (vao != 0) { impl_->gl.DeleteVertexArrays(1, &vao); vao = 0; }
  };

  if (impl_->streamVbo != 0) {
    impl_->gl.BindBuffer(GL_ARRAY_BUFFER, impl_->streamVbo);
    impl_->gl.UnmapBuffer(GL_ARRAY_BUFFER);
  }
  if (impl_->streamIbo != 0) {
    impl_->gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl_->streamIbo);
    impl_->gl.UnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
  }
  deleteVaoBuf(impl_->streamVao, impl_->streamVbo, impl_->streamIbo);
  deleteVaoBuf(impl_->fsVao, impl_->fsVbo, impl_->fsIbo);
  deleteVaoBuf(impl_->overflowVao, impl_->overflowVbo, impl_->overflowIbo);

  impl_->invalidateStateCache();
  impl_->ready = false;
}

void Gl45RenderDevice::beginFrame() {
  if (!impl_->ready) {
    return;
  }
  impl_->invalidateStateCache();

  if (impl_->persistentRingReady) {
    impl_->waitRegionFence(impl_->ringRegion);
  }

  if (impl_->hasTimerQueries) {
    const int readIdx = 1 - impl_->timerWriteIdx;
    if (impl_->timerPrimed[static_cast<size_t>(readIdx)]) {
      GLint available = 0;
      impl_->gl.GetQueryObjectiv(
          impl_->timerQueries[static_cast<size_t>(readIdx)], GL_QUERY_RESULT_AVAILABLE, &available);
      if (available) {
        gl::GLuint64 elapsedNs = 0;
        impl_->gl.GetQueryObjectui64v(
            impl_->timerQueries[static_cast<size_t>(readIdx)], GL_QUERY_RESULT, &elapsedNs);
        impl_->lastGpuMs = static_cast<float>(elapsedNs) * 1e-6f;
        impl_->timerPrimed[static_cast<size_t>(readIdx)] = false;
      }
    }
    if (!impl_->timerActive && impl_->timerQueries[static_cast<size_t>(impl_->timerWriteIdx)] != 0) {
      impl_->gl.BeginQuery(GL_TIME_ELAPSED, impl_->timerQueries[static_cast<size_t>(impl_->timerWriteIdx)]);
      impl_->timerActive = true;
    }
  }
}

void Gl45RenderDevice::endFrame() {
  if (!impl_->ready) {
    return;
  }
  if (impl_->persistentRingReady && impl_->ringWrittenInRegion) {
    impl_->signalAndAdvanceRegion();
  }
  if (impl_->hasTimerQueries && impl_->timerActive) {
    impl_->gl.EndQuery(GL_TIME_ELAPSED);
    impl_->timerActive = false;
    impl_->timerPrimed[static_cast<size_t>(impl_->timerWriteIdx)] = true;
    impl_->timerWriteIdx = 1 - impl_->timerWriteIdx;
  }
  impl_->bindVaoCached(0);
  impl_->invalidateStateCache();
}

TextureHandle Gl45RenderDevice::createTexture2D(const TextureDesc& desc, std::string* outError) {
  if (!impl_->ready || desc.width <= 0 || desc.height <= 0) {
    if (outError) {
      *outError = "Invalid texture dimensions or uninitialized GL45 device";
    }
    return {};
  }

  GLuint glTex = 0;
  glGenTextures(1, &glTex);
  glBindTexture(GL_TEXTURE_2D, glTex);
  if (impl_->cachedActiveTexUnit >= 0 && impl_->cachedActiveTexUnit < 16) {
    impl_->cachedBoundTex[static_cast<size_t>(impl_->cachedActiveTexUnit)] = kInvalidGlId;
  }

  const GLint minFilter = desc.generateMipmaps
                              ? GL_LINEAR_MIPMAP_LINEAR
                              : (desc.filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR);
  const GLint magFilter = (desc.filter == TextureFilter::Nearest) ? GL_NEAREST : GL_LINEAR;
  const GLint wrap = toGlWrap(desc.wrap);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  const GLint internalFormat = toGlInternalFormat(desc.format);
  const GLenum pixelType = toGlUploadPixelType(desc.format, desc.rgbaPixels != nullptr);
  glTexImage2D(GL_TEXTURE_2D,
               0,
               internalFormat,
               desc.width,
               desc.height,
               0,
               GL_RGBA,
               pixelType,
               desc.rgbaPixels);

  if (desc.generateMipmaps && desc.rgbaPixels != nullptr) {
    impl_->gl.GenerateMipmap(GL_TEXTURE_2D);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  const uint32_t id = Impl::allocSlot(
      impl_->textures, TextureRecord{glTex, desc.width, desc.height, desc.filter, desc.wrap, desc.format});
  return TextureHandle{id};
}

void Gl45RenderDevice::updateTexture2D(TextureHandle tex, int width, int height, const uint8_t* rgbaPixels) {
  auto* rec = Impl::findSlot(impl_->textures, tex.id);
  if (!rec || width <= 0 || height <= 0 || !rgbaPixels) {
    return;
  }
  glBindTexture(GL_TEXTURE_2D, rec->glId);
  if (impl_->cachedActiveTexUnit >= 0 && impl_->cachedActiveTexUnit < 16) {
    impl_->cachedBoundTex[static_cast<size_t>(impl_->cachedActiveTexUnit)] = kInvalidGlId;
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (rec->width == width && rec->height == height) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
  } else {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
    rec->width = width;
    rec->height = height;
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

void Gl45RenderDevice::updateTexture2DRegion(TextureHandle tex,
                                             int x,
                                             int y,
                                             int width,
                                             int height,
                                             const uint8_t* rgbaPixels) {
  auto* rec = Impl::findSlot(impl_->textures, tex.id);
  if (!rec || !rgbaPixels || width <= 0 || height <= 0 ||
      x < 0 || y < 0 || x + width > rec->width || y + height > rec->height) {
    return;
  }
  glBindTexture(GL_TEXTURE_2D, rec->glId);
  if (impl_->cachedActiveTexUnit >= 0 && impl_->cachedActiveTexUnit < 16) {
    impl_->cachedBoundTex[static_cast<size_t>(impl_->cachedActiveTexUnit)] = kInvalidGlId;
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void Gl45RenderDevice::destroyTexture(TextureHandle tex) {
  if (tex.id == 0 || tex.id > impl_->textures.size()) {
    return;
  }
  auto& slot = impl_->textures[static_cast<size_t>(tex.id - 1)];
  if (!slot.alive) {
    return;
  }
  const GLuint oldGlId = slot.value.glId;
  if (oldGlId != 0) {
    for (GLuint& cachedId : impl_->cachedBoundTex) {
      if (cachedId == oldGlId) {
        cachedId = kInvalidGlId;
      }
    }
    glDeleteTextures(1, &slot.value.glId);
  }
  slot.alive = false;
  slot.value = TextureRecord{};
}

bool Gl45RenderDevice::textureSize(TextureHandle tex, int& outW, int& outH) const {
  const auto* rec = Impl::findSlot(impl_->textures, tex.id);
  if (!rec) {
    return false;
  }
  outW = rec->width;
  outH = rec->height;
  return true;
}

uint64_t Gl45RenderDevice::nativeTextureId(TextureHandle tex) const noexcept {
  const auto* rec = Impl::findSlot(impl_->textures, tex.id);
  return rec ? static_cast<uint64_t>(rec->glId) : 0ull;
}

RenderTargetHandle Gl45RenderDevice::createRenderTarget(int width,
                                                        int height,
                                                        TextureFilter filter,
                                                        PixelFormat format,
                                                        std::string* outError) {
  if (!impl_->ready || width <= 0 || height <= 0) {
    if (outError) {
      *outError = "Invalid render target dimensions";
    }
    return {};
  }

  TextureDesc td;
  td.width = width;
  td.height = height;
  td.rgbaPixels = nullptr;
  td.filter = filter;
  td.wrap = TextureWrap::ClampToEdge;
  td.format = format;
  const TextureHandle colorTex = createTexture2D(td, outError);
  if (!colorTex) {
    return {};
  }

  GLuint fbo = 0;
  impl_->gl.GenFramebuffers(1, &fbo);
  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
  const auto* texRec = Impl::findSlot(impl_->textures, colorTex.id);
  const GLuint glTex = texRec ? texRec->glId : 0;
  impl_->gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex, 0);

  const GLenum status = impl_->gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  impl_->cachedFboValid = false;
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    impl_->gl.DeleteFramebuffers(1, &fbo);
    destroyTexture(colorTex);
    if (outError) {
      *outError = "Framebuffer incomplete";
    }
    return {};
  }

  const uint32_t id = Impl::allocSlot(
      impl_->renderTargets, RenderTargetRecord{fbo, colorTex, width, height, filter, format});
  return RenderTargetHandle{id};
}

void Gl45RenderDevice::resizeRenderTarget(RenderTargetHandle rt, int width, int height) {
  auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!rec || width <= 0 || height <= 0) {
    return;
  }
  if (rec->width == width && rec->height == height) {
    return;
  }
  auto* texRec = Impl::findSlot(impl_->textures, rec->colorTex.id);
  if (!texRec) {
    return;
  }
  const GLint internalFormat = toGlInternalFormat(texRec->format);
  const GLenum pixelType = toGlUploadPixelType(texRec->format, false);
  glBindTexture(GL_TEXTURE_2D, texRec->glId);
  if (impl_->cachedActiveTexUnit >= 0 && impl_->cachedActiveTexUnit < 16) {
    impl_->cachedBoundTex[static_cast<size_t>(impl_->cachedActiveTexUnit)] = kInvalidGlId;
  }
  glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, pixelType, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  texRec->width = width;
  texRec->height = height;
  rec->width = width;
  rec->height = height;
}

void Gl45RenderDevice::destroyRenderTarget(RenderTargetHandle rt) {
  if (rt.id == 0 || rt.id > impl_->renderTargets.size()) {
    return;
  }
  auto& slot = impl_->renderTargets[static_cast<size_t>(rt.id - 1)];
  if (!slot.alive) {
    return;
  }
  if (slot.value.fbo != 0) {
    if (impl_->cachedFboValid && impl_->cachedFbo == slot.value.fbo) {
      impl_->cachedFboValid = false;
    }
    impl_->gl.DeleteFramebuffers(1, &slot.value.fbo);
  }
  destroyTexture(slot.value.colorTex);
  slot.alive = false;
  slot.value = RenderTargetRecord{};
}

TextureHandle Gl45RenderDevice::renderTargetColorTexture(RenderTargetHandle rt) const noexcept {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  return rec ? rec->colorTex : TextureHandle{};
}

PixelFormat Gl45RenderDevice::renderTargetFormat(RenderTargetHandle rt) const noexcept {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  return rec ? rec->format : PixelFormat::Rgba8;
}

bool Gl45RenderDevice::renderTargetSize(RenderTargetHandle rt, int& outW, int& outH) const {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!rec) {
    return false;
  }
  outW = rec->width;
  outH = rec->height;
  return true;
}

ShaderHandle Gl45RenderDevice::createShader(std::string_view vertexGlsl,
                                            std::string_view fragmentGlsl,
                                            std::string* outError) {
  if (!impl_->ready) {
    if (outError) {
      *outError = "GL45 device not initialized";
    }
    return {};
  }
  const GLuint vs = impl_->compileStage(GL_VERTEX_SHADER, vertexGlsl, outError);
  if (vs == 0) {
    return {};
  }
  const GLuint fs = impl_->compileStage(GL_FRAGMENT_SHADER, fragmentGlsl, outError);
  if (fs == 0) {
    impl_->gl.DeleteShader(vs);
    return {};
  }

  const GLuint prog = impl_->gl.CreateProgram();
  impl_->gl.AttachShader(prog, vs);
  impl_->gl.AttachShader(prog, fs);
  impl_->gl.LinkProgram(prog);
  impl_->gl.DeleteShader(vs);
  impl_->gl.DeleteShader(fs);

  GLint linked = 0;
  impl_->gl.GetProgramiv(prog, GL_LINK_STATUS, &linked);
  if (!linked) {
    GLint logLen = 0;
    impl_->gl.GetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
    std::string log(static_cast<size_t>(std::max(1, logLen)), '\0');
    impl_->gl.GetProgramInfoLog(prog, logLen, nullptr, log.data());
    while (!log.empty() && log.back() == '\0') log.pop_back();
    if (outError) {
      *outError = "Shader program link failed: " + log;
    }
    impl_->gl.DeleteProgram(prog);
    return {};
  }

  const uint32_t id = Impl::allocSlot(impl_->shaders, ShaderRecord{prog, {}});
  return ShaderHandle{id};
}

void Gl45RenderDevice::destroyShader(ShaderHandle sh) {
  if (sh.id == 0 || sh.id > impl_->shaders.size()) {
    return;
  }
  auto& slot = impl_->shaders[static_cast<size_t>(sh.id - 1)];
  if (!slot.alive) {
    return;
  }
  if (slot.value.program != 0) {
    if (impl_->activeProgram == slot.value.program) {
      impl_->gl.UseProgram(0);
      impl_->activeProgram = 0;
    }
    impl_->gl.DeleteProgram(slot.value.program);
  }
  slot.alive = false;
  slot.value = ShaderRecord{};
}

void Gl45RenderDevice::bindRenderTarget(RenderTargetHandle rt, int viewportWidth, int viewportHeight) {
  if (!rt) {
    if (!impl_->cachedFboValid || impl_->cachedFbo != 0) {
      impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
      impl_->cachedFbo = 0;
      impl_->cachedFboValid = true;
    }
    if (viewportWidth > 0 && viewportHeight > 0 &&
        (impl_->cachedVpW != viewportWidth || impl_->cachedVpH != viewportHeight)) {
      glViewport(0, 0, viewportWidth, viewportHeight);
      impl_->cachedVpW = viewportWidth;
      impl_->cachedVpH = viewportHeight;
    }
    return;
  }

  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!rec) {
    return;
  }
  if (!impl_->cachedFboValid || impl_->cachedFbo != rec->fbo) {
    impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, rec->fbo);
    impl_->cachedFbo = rec->fbo;
    impl_->cachedFboValid = true;
  }
  const int w = viewportWidth > 0 ? viewportWidth : rec->width;
  const int h = viewportHeight > 0 ? viewportHeight : rec->height;
  if (impl_->cachedVpW != w || impl_->cachedVpH != h) {
    glViewport(0, 0, w, h);
    impl_->cachedVpW = w;
    impl_->cachedVpH = h;
  }
}

void Gl45RenderDevice::clear(float r, float g, float b, float a) {
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT);
}

void Gl45RenderDevice::setBlendMode(iota::core::BlendMode mode) {
  using iota::core::BlendMode;
  if (impl_->cachedBlendValid && impl_->cachedBlendMode == mode) {
    return;
  }
  const bool needEnable = !impl_->cachedBlendValid || (impl_->cachedBlendMode == BlendMode::Opaque);
  impl_->cachedBlendValid = true;
  impl_->cachedBlendMode = mode;

  if (mode == BlendMode::Opaque) {
    glDisable(GL_BLEND);
    return;
  }
  if (needEnable) {
    glEnable(GL_BLEND);
    impl_->gl.BlendEquation(GL_FUNC_ADD);
  }
  switch (mode) {
    case BlendMode::Alpha:
      impl_->gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::Screen:
      impl_->gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::Add:
      impl_->gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE_MINUS_DST_ALPHA, GL_ONE);
      break;
    case BlendMode::PreMulAlpha:
      impl_->gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::AddAlpha:
      impl_->gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_DST_ALPHA, GL_ONE);
      break;
    case BlendMode::Inverse:
      impl_->gl.BlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    default:
      impl_->gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
  }
}

void Gl45RenderDevice::setScissor(bool enabled, int x, int y, int w, int h) {
  const int cw = std::max(0, w);
  const int ch = std::max(0, h);
  if (impl_->cachedScissorValid && impl_->cachedScissorEnabled == enabled &&
      (!enabled || (impl_->cachedScissorX == x && impl_->cachedScissorY == y &&
                    impl_->cachedScissorW == cw && impl_->cachedScissorH == ch))) {
    return;
  }
  impl_->cachedScissorValid = true;
  impl_->cachedScissorEnabled = enabled;
  if (!enabled) {
    glDisable(GL_SCISSOR_TEST);
    return;
  }
  impl_->cachedScissorX = x;
  impl_->cachedScissorY = y;
  impl_->cachedScissorW = cw;
  impl_->cachedScissorH = ch;
  glEnable(GL_SCISSOR_TEST);
  glScissor(x, y, cw, ch);
}

void Gl45RenderDevice::useShader(ShaderHandle sh) {
  const auto* rec = Impl::findSlot(impl_->shaders, sh.id);
  const GLuint prog = rec ? rec->program : 0;
  if (impl_->activeProgram != prog) {
    impl_->gl.UseProgram(prog);
    impl_->activeProgram = prog;
  }
}

void Gl45RenderDevice::bindTexture(int slot, TextureHandle tex) {
  const int s = std::clamp(slot, 0, 15);
  const auto* rec = Impl::findSlot(impl_->textures, tex.id);
  const GLuint glId = rec ? rec->glId : 0;
  if (impl_->cachedBoundTex[static_cast<size_t>(s)] == glId) {
    return;
  }
  if (impl_->cachedActiveTexUnit != s) {
    impl_->gl.ActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(s));
    impl_->cachedActiveTexUnit = s;
  }
  glBindTexture(GL_TEXTURE_2D, glId);
  impl_->cachedBoundTex[static_cast<size_t>(s)] = glId;
}

void Gl45RenderDevice::setUniformMat4(ShaderHandle sh, std::string_view name, const glm::mat4& mat) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    impl_->gl.UniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(mat));
  }
}

void Gl45RenderDevice::setUniformVec2(ShaderHandle sh, std::string_view name, float x, float y) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    impl_->gl.Uniform2f(loc, x, y);
  }
}

void Gl45RenderDevice::setUniformVec3(ShaderHandle sh, std::string_view name, float x, float y, float z) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    impl_->gl.Uniform3f(loc, x, y, z);
  }
}

void Gl45RenderDevice::setUniformVec4(ShaderHandle sh, std::string_view name, float x, float y, float z, float w) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    impl_->gl.Uniform4f(loc, x, y, z, w);
  }
}

void Gl45RenderDevice::setUniformFloat(ShaderHandle sh, std::string_view name, float v) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    impl_->gl.Uniform1f(loc, v);
  }
}

void Gl45RenderDevice::setUniformInt(ShaderHandle sh, std::string_view name, int v) {
  useShader(sh);
  const GLint loc = impl_->findUniform(sh, name);
  if (loc >= 0) {
    impl_->gl.Uniform1i(loc, v);
  }
}

void Gl45RenderDevice::drawIndexed(PrimitiveTopology topology,
                                   std::span<const VertexPosColUv> vertices,
                                   std::span<const uint32_t> indices) {
  if (!impl_->ready || vertices.empty() || indices.empty()) {
    return;
  }

  if (!impl_->persistentRingReady ||
      vertices.size() > kVertsPerRegion ||
      indices.size() > kIndicesPerRegion) {
    impl_->drawIndexedOverflow(topology, vertices, indices);
    return;
  }

  if (impl_->regionVertUsed + vertices.size() > kVertsPerRegion ||
      impl_->regionIdxUsed + indices.size() > kIndicesPerRegion) {
    impl_->signalAndAdvanceRegion();
  }
  impl_->waitRegionFence(impl_->ringRegion);

  const size_t baseVertex = impl_->ringRegion * kVertsPerRegion + impl_->regionVertUsed;
  const size_t baseIndex = impl_->ringRegion * kIndicesPerRegion + impl_->regionIdxUsed;
  std::memcpy(impl_->mappedVerts + baseVertex,
              vertices.data(),
              vertices.size() * sizeof(VertexPosColUv));
  std::memcpy(impl_->mappedIndices + baseIndex,
              indices.data(),
              indices.size() * sizeof(uint32_t));
  impl_->regionVertUsed += vertices.size();
  impl_->regionIdxUsed += indices.size();
  impl_->ringWrittenInRegion = true;

  impl_->bindVaoCached(impl_->streamVao);
  impl_->gl.DrawElementsBaseVertex(
      toGlTopology(topology),
      static_cast<GLsizei>(indices.size()),
      GL_UNSIGNED_INT,
      reinterpret_cast<const void*>(baseIndex * sizeof(uint32_t)),
      static_cast<GLint>(baseVertex));
}

void Gl45RenderDevice::drawVertices(PrimitiveTopology topology,
                                    std::span<const VertexPosColUv> vertices) {
  if (!impl_->ready || vertices.empty()) {
    return;
  }

  if (!impl_->persistentRingReady || vertices.size() > kVertsPerRegion) {
    impl_->drawVerticesOverflow(topology, vertices);
    return;
  }

  if (impl_->regionVertUsed + vertices.size() > kVertsPerRegion) {
    impl_->signalAndAdvanceRegion();
  }
  impl_->waitRegionFence(impl_->ringRegion);

  const size_t baseVertex = impl_->ringRegion * kVertsPerRegion + impl_->regionVertUsed;
  std::memcpy(impl_->mappedVerts + baseVertex,
              vertices.data(),
              vertices.size() * sizeof(VertexPosColUv));
  impl_->regionVertUsed += vertices.size();
  impl_->ringWrittenInRegion = true;

  impl_->bindVaoCached(impl_->streamVao);
  glDrawArrays(toGlTopology(topology), static_cast<GLint>(baseVertex), static_cast<GLsizei>(vertices.size()));
}

void Gl45RenderDevice::drawFullscreenQuad() {
  if (!impl_->ready || impl_->fsVao == 0) {
    return;
  }
  impl_->bindVaoCached(impl_->fsVao);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
}

bool Gl45RenderDevice::readRenderTargetRgba(RenderTargetHandle rt,
                                            int& outWidth,
                                            int& outHeight,
                                            std::vector<uint8_t>& outRgba,
                                            bool flipVertical) {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!rec || rec->width <= 0 || rec->height <= 0) {
    return false;
  }
  outWidth = rec->width;
  outHeight = rec->height;
  const size_t rowBytes = static_cast<size_t>(outWidth) * 4;
  outRgba.resize(rowBytes * static_cast<size_t>(outHeight));

  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, rec->fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, outWidth, outHeight, GL_RGBA, GL_UNSIGNED_BYTE, outRgba.data());
  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  impl_->cachedFboValid = false;

  if (flipVertical && outHeight > 1) {
    impl_->flipRowScratch.resize(rowBytes);
    for (int y = 0; y < outHeight / 2; ++y) {
      uint8_t* top = outRgba.data() + static_cast<size_t>(y) * rowBytes;
      uint8_t* bot = outRgba.data() + static_cast<size_t>(outHeight - 1 - y) * rowBytes;
      std::memcpy(impl_->flipRowScratch.data(), top, rowBytes);
      std::memcpy(top, bot, rowBytes);
      std::memcpy(bot, impl_->flipRowScratch.data(), rowBytes);
    }
  }
  return true;
}

bool Gl45RenderDevice::readRenderTargetRgbaAsync(RenderTargetHandle rt,
                                                 int& outWidth,
                                                 int& outHeight,
                                                 std::vector<uint8_t>& outRgba,
                                                 bool flipVertical) {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!impl_->ready || !rec || rec->width <= 0 || rec->height <= 0) {
    return false;
  }

  const int w = rec->width;
  const int h = rec->height;
  const size_t rowBytes = static_cast<size_t>(w) * 4u;
  const size_t totalBytes = rowBytes * static_cast<size_t>(h);

  if (impl_->asyncPbos[0] == 0) {
    impl_->gl.GenBuffers(2, impl_->asyncPbos.data());
  }
  if (totalBytes > impl_->asyncPboCapBytes) {
    impl_->asyncPboCapBytes = totalBytes;
    for (int i = 0; i < 2; ++i) {
      impl_->gl.BindBuffer(GL_PIXEL_PACK_BUFFER, impl_->asyncPbos[static_cast<size_t>(i)]);
      impl_->gl.BufferData(
          GL_PIXEL_PACK_BUFFER, static_cast<gl::GLsizeiptr>(impl_->asyncPboCapBytes), nullptr, GL_STREAM_READ);
      impl_->asyncPrimed[static_cast<size_t>(i)] = false;
    }
    impl_->gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }

  const int wrIdx = impl_->asyncWriteIdx;
  const int rdIdx = 1 - wrIdx;

  // Kick asynchronous DMA readback into wrIdx PBO
  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, rec->fbo);
  impl_->gl.BindBuffer(GL_PIXEL_PACK_BUFFER, impl_->asyncPbos[static_cast<size_t>(wrIdx)]);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  impl_->gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  impl_->cachedFboValid = false;
  impl_->asyncW[static_cast<size_t>(wrIdx)] = w;
  impl_->asyncH[static_cast<size_t>(wrIdx)] = h;
  impl_->asyncPrimed[static_cast<size_t>(wrIdx)] = true;

  bool hasPrev = false;
  if (impl_->asyncPrimed[static_cast<size_t>(rdIdx)] &&
      impl_->asyncW[static_cast<size_t>(rdIdx)] == w &&
      impl_->asyncH[static_cast<size_t>(rdIdx)] == h) {
    impl_->gl.BindBuffer(GL_PIXEL_PACK_BUFFER, impl_->asyncPbos[static_cast<size_t>(rdIdx)]);
    const void* mapped = impl_->gl.MapBufferRange(
        GL_PIXEL_PACK_BUFFER, 0, static_cast<gl::GLsizeiptr>(totalBytes), GL_MAP_READ_BIT);
    if (mapped != nullptr) {
      const auto* src = static_cast<const uint8_t*>(mapped);
      outWidth = w;
      outHeight = h;
      outRgba.resize(totalBytes);
      if (flipVertical && h > 1) {
        for (int y = 0; y < h; ++y) {
          const uint8_t* srcRow = src + static_cast<size_t>(h - 1 - y) * rowBytes;
          uint8_t* dstRow = outRgba.data() + static_cast<size_t>(y) * rowBytes;
          std::memcpy(dstRow, srcRow, rowBytes);
        }
      } else {
        std::memcpy(outRgba.data(), src, totalBytes);
      }
      impl_->gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
      hasPrev = true;
    }
    impl_->gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }

  impl_->asyncWriteIdx = rdIdx;
  return hasPrev;
}

bool Gl45RenderDevice::readRenderTargetRgbaFloat(RenderTargetHandle rt,
                                                 int& outWidth,
                                                 int& outHeight,
                                                 std::vector<float>& outRgbaFloat,
                                                 bool flipVertical) {
  const auto* rec = Impl::findSlot(impl_->renderTargets, rt.id);
  if (!rec || rec->width <= 0 || rec->height <= 0) {
    return false;
  }
  outWidth = rec->width;
  outHeight = rec->height;
  const size_t rowFloats = static_cast<size_t>(outWidth) * 4;
  outRgbaFloat.resize(rowFloats * static_cast<size_t>(outHeight));

  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, rec->fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, outWidth, outHeight, GL_RGBA, GL_FLOAT, outRgbaFloat.data());
  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  impl_->cachedFboValid = false;

  if (flipVertical && outHeight > 1) {
    std::vector<float> tempRow(rowFloats);
    const size_t rowBytes = rowFloats * sizeof(float);
    for (int y = 0; y < outHeight / 2; ++y) {
      float* top = outRgbaFloat.data() + static_cast<size_t>(y) * rowFloats;
      float* bot = outRgbaFloat.data() + static_cast<size_t>(outHeight - 1 - y) * rowFloats;
      std::memcpy(tempRow.data(), top, rowBytes);
      std::memcpy(top, bot, rowBytes);
      std::memcpy(bot, tempRow.data(), rowBytes);
    }
  }
  return true;
}

float Gl45RenderDevice::lastGpuFrameTimeMs() const noexcept {
  return impl_->lastGpuMs;
}

}  // namespace iota::rhi
