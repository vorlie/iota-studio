#include "gl_loader.hpp"

#include <string>

namespace iota::rhi::gl {

namespace {

void* resolveGlSymbol(const char* name) {
  void* ptr = reinterpret_cast<void*>(wglGetProcAddress(name));
  // wglGetProcAddress returns 0, 1, 2, 3, or -1 for invalid / core 1.1 symbols
  if (ptr == nullptr ||
      ptr == reinterpret_cast<void*>(0x1) ||
      ptr == reinterpret_cast<void*>(0x2) ||
      ptr == reinterpret_cast<void*>(0x3) ||
      ptr == reinterpret_cast<void*>(-1)) {
    static HMODULE openglModule = GetModuleHandleA("opengl32.dll");
    if (openglModule != nullptr) {
      ptr = reinterpret_cast<void*>(GetProcAddress(openglModule, name));
    } else {
      ptr = nullptr;
    }
  }
  return ptr;
}

template <typename FnPtr>
bool loadFn(FnPtr& dst, const char* name, std::string* outError) {
  void* sym = resolveGlSymbol(name);
  if (!sym) {
    if (outError) {
      *outError = std::string("Missing OpenGL symbol: ") + name;
    }
    return false;
  }
  dst = reinterpret_cast<FnPtr>(sym);
  return true;
}

}  // namespace

bool loadGlProcs(GlProcs& p, std::string* outError) {
  return loadFn(p.CreateShader, "glCreateShader", outError) &&
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
}

}  // namespace iota::rhi::gl
