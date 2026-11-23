#include "iota/rhi/gl45_device.hpp"

#include "gl_loader.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <unordered_map>

namespace iota::rhi {

namespace {

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
  std::unordered_map<std::string, GLint> uniforms;
};

}  // namespace

struct Gl45RenderDevice::Impl {
  gl::GlProcs gl{};
  bool ready = false;

  uint32_t nextHandleId = 1;
  std::unordered_map<uint32_t, TextureRecord> textures;
  std::unordered_map<uint32_t, RenderTargetRecord> renderTargets;
  std::unordered_map<uint32_t, ShaderRecord> shaders;

  GLuint vao = 0;
  GLuint vbo = 0;
  GLuint ibo = 0;
  size_t vboCapacityBytes = 0;
  size_t iboCapacityBytes = 0;

  GLuint activeProgram = 0;

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
    auto it = shaders.find(sh.id);
    if (it == shaders.end() || it->second.program == 0) {
      return -1;
    }
    auto& rec = it->second;
    std::string key(name);
    auto uIt = rec.uniforms.find(key);
    if (uIt != rec.uniforms.end()) {
      return uIt->second;
    }
    const GLint loc = gl.GetUniformLocation(rec.program, key.c_str());
    rec.uniforms.emplace(std::move(key), loc);
    return loc;
  }

  void uploadVertices(std::span<const VertexPosColUv> vertices) {
    const size_t bytes = vertices.size() * sizeof(VertexPosColUv);
    gl.BindVertexArray(vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    if (bytes > vboCapacityBytes) {
      vboCapacityBytes = std::max<size_t>(bytes, 65536);
      gl.BufferData(GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(vboCapacityBytes), nullptr, GL_DYNAMIC_DRAW);
    }
    if (bytes > 0) {
      gl.BufferSubData(GL_ARRAY_BUFFER, 0, static_cast<gl::GLsizeiptr>(bytes), vertices.data());
    }
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

  impl_->gl.GenVertexArrays(1, &impl_->vao);
  impl_->gl.GenBuffers(1, &impl_->vbo);
  impl_->gl.GenBuffers(1, &impl_->ibo);

  impl_->gl.BindVertexArray(impl_->vao);
  impl_->gl.BindBuffer(GL_ARRAY_BUFFER, impl_->vbo);
  impl_->gl.BufferData(GL_ARRAY_BUFFER, 65536, nullptr, GL_DYNAMIC_DRAW);
  impl_->vboCapacityBytes = 65536;

  impl_->gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl_->ibo);
  impl_->gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, 32768, nullptr, GL_DYNAMIC_DRAW);
  impl_->iboCapacityBytes = 32768;

  // Attribute 0: position (vec3)
  impl_->gl.EnableVertexAttribArray(0);
  impl_->gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VertexPosColUv),
                                reinterpret_cast<const void*>(offsetof(VertexPosColUv, x)));
  // Attribute 1: color (normalized uint8 vec4)
  impl_->gl.EnableVertexAttribArray(1);
  impl_->gl.VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VertexPosColUv),
                                reinterpret_cast<const void*>(offsetof(VertexPosColUv, abgr)));
  // Attribute 2: uv (vec2)
  impl_->gl.EnableVertexAttribArray(2);
  impl_->gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(VertexPosColUv),
                                reinterpret_cast<const void*>(offsetof(VertexPosColUv, u)));

  impl_->gl.BindVertexArray(0);
  impl_->ready = true;
  return true;
}

void Gl45RenderDevice::shutdown() {
  if (!impl_ || !impl_->ready) {
    return;
  }
  for (auto& [id, rt] : impl_->renderTargets) {
    if (rt.fbo != 0) {
      impl_->gl.DeleteFramebuffers(1, &rt.fbo);
    }
  }
  impl_->renderTargets.clear();

  for (auto& [id, tex] : impl_->textures) {
    if (tex.glId != 0) {
      glDeleteTextures(1, &tex.glId);
    }
  }
  impl_->textures.clear();

  for (auto& [id, sh] : impl_->shaders) {
    if (sh.program != 0) {
      impl_->gl.DeleteProgram(sh.program);
    }
  }
  impl_->shaders.clear();

  if (impl_->ibo != 0) {
    impl_->gl.DeleteBuffers(1, &impl_->ibo);
    impl_->ibo = 0;
  }
  if (impl_->vbo != 0) {
    impl_->gl.DeleteBuffers(1, &impl_->vbo);
    impl_->vbo = 0;
  }
  if (impl_->vao != 0) {
    impl_->gl.DeleteVertexArrays(1, &impl_->vao);
    impl_->vao = 0;
  }
  impl_->ready = false;
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

  const GLint internalFormat = (desc.format == PixelFormat::Rgba16F) ? GL_RGBA16F : GL_RGBA8;
  const GLenum pixelType = (desc.format == PixelFormat::Rgba16F && desc.rgbaPixels == nullptr)
                               ? GL_HALF_FLOAT
                               : GL_UNSIGNED_BYTE;
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

  const uint32_t id = impl_->nextHandleId++;
  impl_->textures.emplace(
      id, TextureRecord{glTex, desc.width, desc.height, desc.filter, desc.wrap, desc.format});
  return TextureHandle{id};
}

void Gl45RenderDevice::updateTexture2D(TextureHandle tex, int width, int height, const uint8_t* rgbaPixels) {
  auto it = impl_->textures.find(tex.id);
  if (it == impl_->textures.end() || width <= 0 || height <= 0 || !rgbaPixels) {
    return;
  }
  glBindTexture(GL_TEXTURE_2D, it->second.glId);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (it->second.width == width && it->second.height == height) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
  } else {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
    it->second.width = width;
    it->second.height = height;
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

void Gl45RenderDevice::updateTexture2DRegion(TextureHandle tex,
                                             int x,
                                             int y,
                                             int width,
                                             int height,
                                             const uint8_t* rgbaPixels) {
  auto it = impl_->textures.find(tex.id);
  if (it == impl_->textures.end() || !rgbaPixels || width <= 0 || height <= 0 ||
      x < 0 || y < 0 || x + width > it->second.width || y + height > it->second.height) {
    return;
  }
  glBindTexture(GL_TEXTURE_2D, it->second.glId);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void Gl45RenderDevice::destroyTexture(TextureHandle tex) {
  auto it = impl_->textures.find(tex.id);
  if (it == impl_->textures.end()) {
    return;
  }
  if (it->second.glId != 0) {
    glDeleteTextures(1, &it->second.glId);
  }
  impl_->textures.erase(it);
}

bool Gl45RenderDevice::textureSize(TextureHandle tex, int& outW, int& outH) const {
  auto it = impl_->textures.find(tex.id);
  if (it == impl_->textures.end()) {
    return false;
  }
  outW = it->second.width;
  outH = it->second.height;
  return true;
}

uint64_t Gl45RenderDevice::nativeTextureId(TextureHandle tex) const noexcept {
  auto it = impl_->textures.find(tex.id);
  if (it == impl_->textures.end()) {
    return 0;
  }
  return static_cast<uint64_t>(it->second.glId);
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
  const GLuint glTex = impl_->textures.at(colorTex.id).glId;
  impl_->gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex, 0);

  const GLenum status = impl_->gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    impl_->gl.DeleteFramebuffers(1, &fbo);
    destroyTexture(colorTex);
    if (outError) {
      *outError = "Framebuffer incomplete";
    }
    return {};
  }

  const uint32_t id = impl_->nextHandleId++;
  impl_->renderTargets.emplace(
      id, RenderTargetRecord{fbo, colorTex, width, height, filter, format});
  return RenderTargetHandle{id};
}

void Gl45RenderDevice::resizeRenderTarget(RenderTargetHandle rt, int width, int height) {
  auto it = impl_->renderTargets.find(rt.id);
  if (it == impl_->renderTargets.end() || width <= 0 || height <= 0) {
    return;
  }
  if (it->second.width == width && it->second.height == height) {
    return;
  }
  auto texIt = impl_->textures.find(it->second.colorTex.id);
  if (texIt == impl_->textures.end()) {
    return;
  }
  const GLint internalFormat = (texIt->second.format == PixelFormat::Rgba16F) ? GL_RGBA16F : GL_RGBA8;
  const GLenum pixelType = (texIt->second.format == PixelFormat::Rgba16F) ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
  glBindTexture(GL_TEXTURE_2D, texIt->second.glId);
  glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, pixelType, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  texIt->second.width = width;
  texIt->second.height = height;
  it->second.width = width;
  it->second.height = height;
}

void Gl45RenderDevice::destroyRenderTarget(RenderTargetHandle rt) {
  auto it = impl_->renderTargets.find(rt.id);
  if (it == impl_->renderTargets.end()) {
    return;
  }
  if (it->second.fbo != 0) {
    impl_->gl.DeleteFramebuffers(1, &it->second.fbo);
  }
  destroyTexture(it->second.colorTex);
  impl_->renderTargets.erase(it);
}

TextureHandle Gl45RenderDevice::renderTargetColorTexture(RenderTargetHandle rt) const noexcept {
  auto it = impl_->renderTargets.find(rt.id);
  if (it == impl_->renderTargets.end()) {
    return {};
  }
  return it->second.colorTex;
}

PixelFormat Gl45RenderDevice::renderTargetFormat(RenderTargetHandle rt) const noexcept {
  auto it = impl_->renderTargets.find(rt.id);
  if (it == impl_->renderTargets.end()) {
    return PixelFormat::Rgba8;
  }
  return it->second.format;
}

bool Gl45RenderDevice::renderTargetSize(RenderTargetHandle rt, int& outW, int& outH) const {
  auto it = impl_->renderTargets.find(rt.id);
  if (it == impl_->renderTargets.end()) {
    return false;
  }
  outW = it->second.width;
  outH = it->second.height;
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
    if (outError) {
      *outError = "Shader program link failed: " + log;
    }
    impl_->gl.DeleteProgram(prog);
    return {};
  }

  const uint32_t id = impl_->nextHandleId++;
  impl_->shaders.emplace(id, ShaderRecord{prog, {}});
  return ShaderHandle{id};
}

void Gl45RenderDevice::destroyShader(ShaderHandle sh) {
  auto it = impl_->shaders.find(sh.id);
  if (it == impl_->shaders.end()) {
    return;
  }
  if (it->second.program != 0) {
    if (impl_->activeProgram == it->second.program) {
      impl_->gl.UseProgram(0);
      impl_->activeProgram = 0;
    }
    impl_->gl.DeleteProgram(it->second.program);
  }
  impl_->shaders.erase(it);
}

void Gl45RenderDevice::bindRenderTarget(RenderTargetHandle rt, int viewportWidth, int viewportHeight) {
  if (!rt) {
    impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (viewportWidth > 0 && viewportHeight > 0) {
      glViewport(0, 0, viewportWidth, viewportHeight);
    }
    return;
  }
  auto it = impl_->renderTargets.find(rt.id);
  if (it == impl_->renderTargets.end()) {
    return;
  }
  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
  const int w = viewportWidth > 0 ? viewportWidth : it->second.width;
  const int h = viewportHeight > 0 ? viewportHeight : it->second.height;
  glViewport(0, 0, w, h);
}

void Gl45RenderDevice::clear(float r, float g, float b, float a) {
  glClearColor(r, g, b, a);
  glClear(GL_COLOR_BUFFER_BIT);
}

void Gl45RenderDevice::setBlendMode(iota::core::BlendMode mode) {
  using iota::core::BlendMode;
  if (mode == BlendMode::Opaque) {
    glDisable(GL_BLEND);
    return;
  }
  glEnable(GL_BLEND);
  impl_->gl.BlendEquation(GL_FUNC_ADD);
  switch (mode) {
    case BlendMode::Alpha:
      impl_->gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::Screen:
      impl_->gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::Add:
      impl_->gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
      break;
    case BlendMode::PreMulAlpha:
      impl_->gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::AddAlpha:
      impl_->gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_SRC_ALPHA, GL_ONE);
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
  if (!enabled) {
    glDisable(GL_SCISSOR_TEST);
    return;
  }
  glEnable(GL_SCISSOR_TEST);
  glScissor(x, y, std::max(0, w), std::max(0, h));
}

void Gl45RenderDevice::useShader(ShaderHandle sh) {
  auto it = impl_->shaders.find(sh.id);
  const GLuint prog = (it != impl_->shaders.end()) ? it->second.program : 0;
  if (impl_->activeProgram != prog) {
    impl_->gl.UseProgram(prog);
    impl_->activeProgram = prog;
  }
}

void Gl45RenderDevice::bindTexture(int slot, TextureHandle tex) {
  impl_->gl.ActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(std::clamp(slot, 0, 15)));
  auto it = impl_->textures.find(tex.id);
  glBindTexture(GL_TEXTURE_2D, (it != impl_->textures.end()) ? it->second.glId : 0);
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
  impl_->uploadVertices(vertices);
  const size_t idxBytes = indices.size() * sizeof(uint32_t);
  impl_->gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl_->ibo);
  if (idxBytes > impl_->iboCapacityBytes) {
    impl_->iboCapacityBytes = std::max<size_t>(idxBytes, 32768);
    impl_->gl.BufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<gl::GLsizeiptr>(impl_->iboCapacityBytes),
                         nullptr,
                         GL_DYNAMIC_DRAW);
  }
  impl_->gl.BufferSubData(GL_ELEMENT_ARRAY_BUFFER,
                          0,
                          static_cast<gl::GLsizeiptr>(idxBytes),
                          indices.data());

  glDrawElements(toGlTopology(topology), static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, nullptr);
  impl_->gl.BindVertexArray(0);
}

void Gl45RenderDevice::drawVertices(PrimitiveTopology topology,
                                    std::span<const VertexPosColUv> vertices) {
  if (!impl_->ready || vertices.empty()) {
    return;
  }
  impl_->uploadVertices(vertices);
  glDrawArrays(toGlTopology(topology), 0, static_cast<GLsizei>(vertices.size()));
  impl_->gl.BindVertexArray(0);
}

bool Gl45RenderDevice::readRenderTargetRgba(RenderTargetHandle rt,
                                            int& outWidth,
                                            int& outHeight,
                                            std::vector<uint8_t>& outRgba,
                                            bool flipVertical) {
  auto it = impl_->renderTargets.find(rt.id);
  if (it == impl_->renderTargets.end() || it->second.width <= 0 || it->second.height <= 0) {
    return false;
  }
  outWidth = it->second.width;
  outHeight = it->second.height;
  const size_t rowBytes = static_cast<size_t>(outWidth) * 4;
  outRgba.resize(rowBytes * static_cast<size_t>(outHeight));

  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, outWidth, outHeight, GL_RGBA, GL_UNSIGNED_BYTE, outRgba.data());
  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

  if (flipVertical && outHeight > 1) {
    std::vector<uint8_t> tempRow(rowBytes);
    for (int y = 0; y < outHeight / 2; ++y) {
      uint8_t* top = outRgba.data() + static_cast<size_t>(y) * rowBytes;
      uint8_t* bot = outRgba.data() + static_cast<size_t>(outHeight - 1 - y) * rowBytes;
      std::memcpy(tempRow.data(), top, rowBytes);
      std::memcpy(top, bot, rowBytes);
      std::memcpy(bot, tempRow.data(), rowBytes);
    }
  }
  return true;
}

bool Gl45RenderDevice::readRenderTargetRgbaFloat(RenderTargetHandle rt,
                                                 int& outWidth,
                                                 int& outHeight,
                                                 std::vector<float>& outRgbaFloat,
                                                 bool flipVertical) {
  auto it = impl_->renderTargets.find(rt.id);
  if (it == impl_->renderTargets.end() || it->second.width <= 0 || it->second.height <= 0) {
    return false;
  }
  outWidth = it->second.width;
  outHeight = it->second.height;
  const size_t rowFloats = static_cast<size_t>(outWidth) * 4;
  outRgbaFloat.resize(rowFloats * static_cast<size_t>(outHeight));

  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, outWidth, outHeight, GL_RGBA, GL_FLOAT, outRgbaFloat.data());
  impl_->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

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

}  // namespace iota::rhi
