#pragma once

#include <windows.h>
#include <GL/gl.h>

#include <cstddef>
#include <cstdint>
#include <string>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif

namespace iota::rhi::gl {

using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;

struct GlProcs {
  GLuint(APIENTRY* CreateShader)(GLenum type) = nullptr;
  void(APIENTRY* ShaderSource)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) = nullptr;
  void(APIENTRY* CompileShader)(GLuint shader) = nullptr;
  void(APIENTRY* GetShaderiv)(GLuint shader, GLenum pname, GLint* params) = nullptr;
  void(APIENTRY* GetShaderInfoLog)(GLuint shader, GLsizei maxLength, GLsizei* length, GLchar* infoLog) = nullptr;
  void(APIENTRY* DeleteShader)(GLuint shader) = nullptr;

  GLuint(APIENTRY* CreateProgram)() = nullptr;
  void(APIENTRY* AttachShader)(GLuint program, GLuint shader) = nullptr;
  void(APIENTRY* LinkProgram)(GLuint program) = nullptr;
  void(APIENTRY* GetProgramiv)(GLuint program, GLenum pname, GLint* params) = nullptr;
  void(APIENTRY* GetProgramInfoLog)(GLuint program, GLsizei maxLength, GLsizei* length, GLchar* infoLog) = nullptr;
  void(APIENTRY* UseProgram)(GLuint program) = nullptr;
  void(APIENTRY* DeleteProgram)(GLuint program) = nullptr;

  GLint(APIENTRY* GetUniformLocation)(GLuint program, const GLchar* name) = nullptr;
  void(APIENTRY* Uniform1i)(GLint location, GLint v0) = nullptr;
  void(APIENTRY* Uniform1f)(GLint location, GLfloat v0) = nullptr;
  void(APIENTRY* Uniform2f)(GLint location, GLfloat v0, GLfloat v1) = nullptr;
  void(APIENTRY* Uniform3f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) = nullptr;
  void(APIENTRY* Uniform4f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) = nullptr;
  void(APIENTRY* UniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) = nullptr;

  void(APIENTRY* GenVertexArrays)(GLsizei n, GLuint* arrays) = nullptr;
  void(APIENTRY* DeleteVertexArrays)(GLsizei n, const GLuint* arrays) = nullptr;
  void(APIENTRY* BindVertexArray)(GLuint array) = nullptr;

  void(APIENTRY* GenBuffers)(GLsizei n, GLuint* buffers) = nullptr;
  void(APIENTRY* DeleteBuffers)(GLsizei n, const GLuint* buffers) = nullptr;
  void(APIENTRY* BindBuffer)(GLenum target, GLuint buffer) = nullptr;
  void(APIENTRY* BufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage) = nullptr;
  void(APIENTRY* BufferSubData)(GLenum target, GLsizeiptr offset, GLsizeiptr size, const void* data) = nullptr;
  void(APIENTRY* EnableVertexAttribArray)(GLuint index) = nullptr;
  void(APIENTRY* VertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer) = nullptr;

  void(APIENTRY* ActiveTexture)(GLenum texture) = nullptr;
  void(APIENTRY* GenerateMipmap)(GLenum target) = nullptr;

  void(APIENTRY* GenFramebuffers)(GLsizei n, GLuint* framebuffers) = nullptr;
  void(APIENTRY* DeleteFramebuffers)(GLsizei n, const GLuint* framebuffers) = nullptr;
  void(APIENTRY* BindFramebuffer)(GLenum target, GLuint framebuffer) = nullptr;
  void(APIENTRY* FramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) = nullptr;
  GLenum(APIENTRY* CheckFramebufferStatus)(GLenum target) = nullptr;

  void(APIENTRY* BlendFuncSeparate)(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) = nullptr;
  void(APIENTRY* BlendEquation)(GLenum mode) = nullptr;
};

bool loadGlProcs(GlProcs& procs, std::string* outError = nullptr);

}  // namespace iota::rhi::gl
