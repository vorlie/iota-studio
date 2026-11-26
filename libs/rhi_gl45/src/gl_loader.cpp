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

#include "gl_loader.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <string>

namespace iota::rhi::gl {

namespace {

GLFWglproc resolveGlSymbol(const char* name) {
  GLFWglproc ptr = glfwGetProcAddress(name);
#ifdef _WIN32
  // wglGetProcAddress returns 0, 1, 2, 3, or -1 for invalid / core 1.1 symbols
  if (ptr == nullptr ||
      ptr == reinterpret_cast<GLFWglproc>(0x1) ||
      ptr == reinterpret_cast<GLFWglproc>(0x2) ||
      ptr == reinterpret_cast<GLFWglproc>(0x3) ||
      ptr == reinterpret_cast<GLFWglproc>(-1)) {
    static HMODULE openglModule = GetModuleHandleA("opengl32.dll");
    if (openglModule != nullptr) {
      ptr = reinterpret_cast<GLFWglproc>(GetProcAddress(openglModule, name));
    } else {
      ptr = nullptr;
    }
  }
#endif
  return ptr;
}

template <typename FnPtr>
bool loadFn(FnPtr& dst, const char* name, std::string* outError) {
  GLFWglproc sym = resolveGlSymbol(name);
  if (!sym) {
    if (outError) {
      *outError = std::string("Missing OpenGL symbol: ") + name;
    }
    return false;
  }
  dst = reinterpret_cast<FnPtr>(sym);
  return true;
}

template <typename FnPtr>
void loadOptionalFn(FnPtr& dst, const char* name) {
  GLFWglproc sym = resolveGlSymbol(name);
  dst = reinterpret_cast<FnPtr>(sym);
}

}  // namespace

bool loadGlProcs(GlProcs& p, std::string* outError) {
  const bool coreOk =
      loadFn(p.CreateShader, "glCreateShader", outError) &&
      loadFn(p.ShaderSource, "glShaderSource", outError) &&
      loadFn(p.CompileShader, "glCompileShader", outError) &&
      loadFn(p.GetShaderiv, "glGetShaderiv", outError) &&
      loadFn(p.GetShaderInfoLog, "glGetShaderInfoLog", outError) &&
      loadFn(p.DeleteShader, "glDeleteShader", outError) &&
      loadFn(p.CreateProgram, "glCreateProgram", outError) &&
      loadFn(p.AttachShader, "glAttachShader", outError) &&
      loadFn(p.LinkProgram, "glLinkProgram", outError) &&
      loadFn(p.GetProgramiv, "glGetProgramiv", outError) &&
      loadFn(p.GetProgramInfoLog, "glGetProgramInfoLog", outError) &&
      loadFn(p.UseProgram, "glUseProgram", outError) &&
      loadFn(p.DeleteProgram, "glDeleteProgram", outError) &&
      loadFn(p.GetUniformLocation, "glGetUniformLocation", outError) &&
      loadFn(p.Uniform1i, "glUniform1i", outError) &&
      loadFn(p.Uniform1f, "glUniform1f", outError) &&
      loadFn(p.Uniform2f, "glUniform2f", outError) &&
      loadFn(p.Uniform3f, "glUniform3f", outError) &&
      loadFn(p.Uniform4f, "glUniform4f", outError) &&
      loadFn(p.UniformMatrix4fv, "glUniformMatrix4fv", outError) &&
      loadFn(p.GenVertexArrays, "glGenVertexArrays", outError) &&
      loadFn(p.DeleteVertexArrays, "glDeleteVertexArrays", outError) &&
      loadFn(p.BindVertexArray, "glBindVertexArray", outError) &&
      loadFn(p.GenBuffers, "glGenBuffers", outError) &&
      loadFn(p.DeleteBuffers, "glDeleteBuffers", outError) &&
      loadFn(p.BindBuffer, "glBindBuffer", outError) &&
      loadFn(p.BufferData, "glBufferData", outError) &&
      loadFn(p.BufferSubData, "glBufferSubData", outError) &&
      loadFn(p.BufferStorage, "glBufferStorage", outError) &&
      loadFn(p.MapBufferRange, "glMapBufferRange", outError) &&
      loadFn(p.UnmapBuffer, "glUnmapBuffer", outError) &&
      loadFn(p.DrawElementsBaseVertex, "glDrawElementsBaseVertex", outError) &&
      loadFn(p.FenceSync, "glFenceSync", outError) &&
      loadFn(p.ClientWaitSync, "glClientWaitSync", outError) &&
      loadFn(p.DeleteSync, "glDeleteSync", outError) &&
      loadFn(p.EnableVertexAttribArray, "glEnableVertexAttribArray", outError) &&
      loadFn(p.VertexAttribPointer, "glVertexAttribPointer", outError) &&
      loadFn(p.ActiveTexture, "glActiveTexture", outError) &&
      loadFn(p.GenerateMipmap, "glGenerateMipmap", outError) &&
      loadFn(p.GenFramebuffers, "glGenFramebuffers", outError) &&
      loadFn(p.DeleteFramebuffers, "glDeleteFramebuffers", outError) &&
      loadFn(p.BindFramebuffer, "glBindFramebuffer", outError) &&
      loadFn(p.FramebufferTexture2D, "glFramebufferTexture2D", outError) &&
      loadFn(p.CheckFramebufferStatus, "glCheckFramebufferStatus", outError) &&
      loadFn(p.BlendFuncSeparate, "glBlendFuncSeparate", outError) &&
      loadFn(p.BlendEquation, "glBlendEquation", outError);

  if (coreOk) {
    loadOptionalFn(p.GenQueries, "glGenQueries");
    loadOptionalFn(p.DeleteQueries, "glDeleteQueries");
    loadOptionalFn(p.BeginQuery, "glBeginQuery");
    loadOptionalFn(p.EndQuery, "glEndQuery");
    loadOptionalFn(p.GetQueryObjectiv, "glGetQueryObjectiv");
    loadOptionalFn(p.GetQueryObjectui64v, "glGetQueryObjectui64v");
  }
  return coreOk;
}

}  // namespace iota::rhi::gl
