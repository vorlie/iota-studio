cmake_minimum_required(VERSION 3.24)

# Required for CMake 4.x compatibility with third-party FetchContent projects
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(IotaStudio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(IOTA_BUILD_TESTS "Build automated doctest suite" ON)

include(FetchContent)

# --- Third-party build flags (suppress unneeded examples/tests/tools) ---
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
set(KISSFFT_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(KISSFFT_OPENMP OFF CACHE BOOL "" FORCE)
set(KISSFFT_STATIC ON CACHE BOOL "" FORCE)
set(KISSFFT_DATATYPE "float" CACHE STRING "" FORCE)

set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.93.0-docking
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.26
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  kissfft
  GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
  GIT_TAG v131
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.0.2
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 6b5799bae8a1fe3792691ef4b72ae26434c3e799
)

FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.12
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(
  nlohmann_json
  glfw
  imgui
  glm
  miniaudio
  kissfft
  miniz
  stb
  doctest
)

# --- Wrapper targets for header-only / source-only third-party libs ---
add_library(imgui_docking STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
)
target_include_directories(imgui_docking PUBLIC
  "${imgui_SOURCE_DIR}"
  "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(imgui_docking PUBLIC glfw opengl32)

add_library(miniaudio_hdr INTERFACE)
target_include_directories(miniaudio_hdr INTERFACE "${miniaudio_SOURCE_DIR}")

add_library(stb_hdr INTERFACE)
target_include_directories(stb_hdr INTERFACE "${stb_SOURCE_DIR}")

add_library(kissfft_real STATIC
  "${kissfft_SOURCE_DIR}/kiss_fft.c"
  "${kissfft_SOURCE_DIR}/tools/kiss_fftr.c"
)
target_include_directories(kissfft_real PUBLIC
  "${kissfft_SOURCE_DIR}"
  "${kissfft_SOURCE_DIR}/tools"
)
target_compile_definitions(kissfft_real PUBLIC kiss_fft_scalar=float)

function(iota_configure_target target_name)
  target_compile_features(${target_name} PUBLIC cxx_std_20)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive- /EHsc /utf-8 /wd4201 /wd4100 /wd4244)
    target_compile_definitions(${target_name} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# --- Project modules ---
add_subdirectory(libs/core)
add_subdirectory(libs/model)
add_subdirectory(libs/preset_io)
add_subdirectory(libs/dsp)
add_subdirectory(libs/audio)
add_subdirectory(libs/rhi)
add_subdirectory(libs/rhi_gl45)
add_subdirectory(libs/engine)
add_subdirectory(libs/export)
add_subdirectory(apps/studio)

if(IOTA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
