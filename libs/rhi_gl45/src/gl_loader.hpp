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

#pragma once

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

#ifndef APIENTRY
#ifdef GLAPIENTRY
#define APIENTRY GLAPIENTRY
#else
#define APIENTRY
#endif
#endif

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
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_R11F_G11F_B10F
#define GL_R11F_G11F_B10F 0x8C3A
#endif
#ifndef GL_UNSIGNED_INT_10F_11F_11F_REV
#define GL_UNSIGNED_INT_10F_11F_11F_REV 0x8C3B
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#endif
#ifndef GL_MAP_UNSYNCHRONIZED_BIT
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#endif
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x0001
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif

namespace iota::rhi::gl {

using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
using GLsync = struct __GLsync*;
using GLuint64 = uint64_t;

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
  void(APIENTRY* BufferStorage)(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) = nullptr;
  void*(APIENTRY* MapBufferRange)(GLenum target, GLsizeiptr offset, GLsizeiptr length, GLbitfield access) = nullptr;
  GLboolean(APIENTRY* UnmapBuffer)(GLenum target) = nullptr;
  void(APIENTRY* DrawElementsBaseVertex)(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) = nullptr;
  GLsync(APIENTRY* FenceSync)(GLenum condition, GLbitfield flags) = nullptr;
  GLenum(APIENTRY* ClientWaitSync)(GLsync sync, GLbitfield flags, GLuint64 timeout) = nullptr;
  void(APIENTRY* DeleteSync)(GLsync sync) = nullptr;

  void(APIENTRY* GenQueries)(GLsizei n, GLuint* ids) = nullptr;
  void(APIENTRY* DeleteQueries)(GLsizei n, const GLuint* ids) = nullptr;
  void(APIENTRY* BeginQuery)(GLenum target, GLuint id) = nullptr;
  void(APIENTRY* EndQuery)(GLenum target) = nullptr;
  void(APIENTRY* GetQueryObjectiv)(GLuint id, GLenum pname, GLint* params) = nullptr;
  void(APIENTRY* GetQueryObjectui64v)(GLuint id, GLenum pname, GLuint64* params) = nullptr;
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
