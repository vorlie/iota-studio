cmake_minimum_required(VERSION 3.24)

# Required for CMake 4.x compatibility with third-party FetchContent projects
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(IotaStudio VERSION 1.11.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(IOTA_BUILD_TESTS "Build automated doctest suite" ON)
option(IOTA_BUILD_MCP "Build optional MCP server and Studio live bridge" OFF)
option(IOTA_ENABLE_DISCORD_RPC "Enable optional Discord Rich Presence via Discord Social SDK" ON)
set(IOTA_DISCORD_SDK_DIR "" CACHE PATH "Path to extracted Discord Social SDK directory")

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

FetchContent_Declare(
  imgui_color_text_edit
  GIT_REPOSITORY https://github.com/LimiNode/ImGuiColorTextEdit.git
  GIT_TAG 50cf9231acc1a1e32ee9f00ca274bbe5990e2c4f
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
  imgui_color_text_edit
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
target_compile_definitions(imgui_docking PUBLIC IMGUI_USE_WCHAR32)
target_link_libraries(imgui_docking PUBLIC glfw opengl32)

add_library(imgui_color_text_edit_lib STATIC
  "${imgui_color_text_edit_SOURCE_DIR}/ImGuiColorTextEdit.cpp"
  "${imgui_color_text_edit_SOURCE_DIR}/ImGuiColorTextEdit.h"
)
target_include_directories(imgui_color_text_edit_lib PUBLIC "${imgui_color_text_edit_SOURCE_DIR}")
target_link_libraries(imgui_color_text_edit_lib PUBLIC imgui_docking)
target_compile_features(imgui_color_text_edit_lib PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(imgui_color_text_edit_lib PRIVATE /W0 /EHsc /utf-8)
  target_compile_definitions(imgui_color_text_edit_lib PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

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

# --- Optional Discord Social SDK detection (non-fatal if absent) ---
set(IOTA_DISCORD_SDK_FOUND FALSE)
set(IOTA_DISCORD_SDK_RUNTIME_LIB "")
if(IOTA_ENABLE_DISCORD_RPC)
  set(_discord_candidates "")
  if(IOTA_DISCORD_SDK_DIR)
    list(APPEND _discord_candidates "${IOTA_DISCORD_SDK_DIR}")
  endif()
  list(APPEND _discord_candidates
    "${CMAKE_SOURCE_DIR}/local/discord_social_sdk"
    "${CMAKE_SOURCE_DIR}/third_party/discord_social_sdk"
  )

  set(_discord_sdk_root "")
  foreach(_cand IN LISTS _discord_candidates)
    if(EXISTS "${_cand}/include/discordpp.h")
      set(_discord_sdk_root "${_cand}")
      break()
    endif()
  endforeach()

  if(_discord_sdk_root)
    string(TOLOWER "${CMAKE_BUILD_TYPE}" _iota_build_type_lower)
    if(_iota_build_type_lower STREQUAL "debug")
      set(_discord_cfg "debug")
    else()
      set(_discord_cfg "release")
    endif()

    if(WIN32)
      set(_discord_imp "${_discord_sdk_root}/lib/${_discord_cfg}/discord_partner_sdk.lib")
      set(_discord_dll "${_discord_sdk_root}/bin/${_discord_cfg}/discord_partner_sdk.dll")
      if(NOT EXISTS "${_discord_imp}" OR NOT EXISTS "${_discord_dll}")
        set(_discord_imp "${_discord_sdk_root}/lib/release/discord_partner_sdk.lib")
        set(_discord_dll "${_discord_sdk_root}/bin/release/discord_partner_sdk.dll")
      endif()
      if(EXISTS "${_discord_imp}" AND EXISTS "${_discord_dll}")
        add_library(discord_social_sdk SHARED IMPORTED GLOBAL)
        set_target_properties(discord_social_sdk PROPERTIES
          IMPORTED_IMPLIB "${_discord_imp}"
          IMPORTED_LOCATION "${_discord_dll}"
          INTERFACE_INCLUDE_DIRECTORIES "${_discord_sdk_root}/include"
        )
        set(IOTA_DISCORD_SDK_FOUND TRUE)
        set(IOTA_DISCORD_SDK_RUNTIME_LIB "${_discord_dll}")
      endif()
    elseif(APPLE)
      set(_discord_dylib "${_discord_sdk_root}/lib/${_discord_cfg}/libdiscord_partner_sdk.dylib")
      if(NOT EXISTS "${_discord_dylib}")
        set(_discord_dylib "${_discord_sdk_root}/lib/release/libdiscord_partner_sdk.dylib")
      endif()
      if(EXISTS "${_discord_dylib}")
        add_library(discord_social_sdk SHARED IMPORTED GLOBAL)
        set_target_properties(discord_social_sdk PROPERTIES
          IMPORTED_LOCATION "${_discord_dylib}"
          INTERFACE_INCLUDE_DIRECTORIES "${_discord_sdk_root}/include"
        )
        set(IOTA_DISCORD_SDK_FOUND TRUE)
        set(IOTA_DISCORD_SDK_RUNTIME_LIB "${_discord_dylib}")
      endif()
    elseif(UNIX)
      set(_discord_so "${_discord_sdk_root}/lib/${_discord_cfg}/libdiscord_partner_sdk.so")
      if(NOT EXISTS "${_discord_so}")
        set(_discord_so "${_discord_sdk_root}/lib/release/libdiscord_partner_sdk.so")
      endif()
      if(EXISTS "${_discord_so}")
        add_library(discord_social_sdk SHARED IMPORTED GLOBAL)
        set_target_properties(discord_social_sdk PROPERTIES
          IMPORTED_LOCATION "${_discord_so}"
          INTERFACE_INCLUDE_DIRECTORIES "${_discord_sdk_root}/include"
        )
        set(IOTA_DISCORD_SDK_FOUND TRUE)
        set(IOTA_DISCORD_SDK_RUNTIME_LIB "${_discord_so}")
      endif()
    endif()
  endif()

  if(IOTA_DISCORD_SDK_FOUND)
    message(STATUS "Discord Social SDK enabled: ${_discord_sdk_root} (${IOTA_DISCORD_SDK_RUNTIME_LIB})")
  else()
    message(STATUS "Discord Social SDK not found; building iota_discord in stub mode")
  endif()
endif()

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
add_subdirectory(libs/discord)
if(IOTA_BUILD_MCP)
  add_subdirectory(libs/mcp)
  add_subdirectory(apps/mcp)
endif()
add_subdirectory(apps/studio)

if(IOTA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
