# Copyright (C) 2026 Vorlie
# This file is part of Iota Studio.
#
# Iota Studio is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Iota Studio is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#
# See the LICENSE file for the full license text.
# SPDX-License-Identifier: GPL-3.0-or-later

set(IOTA_STUDIO_PLATFORM_SOURCES
  src/platform_desktop.hpp
)
if(EMSCRIPTEN)
  list(APPEND IOTA_STUDIO_PLATFORM_SOURCES src/platform_web.cpp)
else()
  list(APPEND IOTA_STUDIO_PLATFORM_SOURCES src/platform_desktop.cpp)
endif()

add_executable(iota_studio
  src/main.cpp
  ${IOTA_STUDIO_PLATFORM_SOURCES}
  src/editor_app.hpp
  src/editor_app.cpp
  src/hierarchy_panel.hpp
  src/hierarchy_panel.cpp
  src/inspector_panel.hpp
  src/inspector_panel.cpp
  src/sound_keys_panel.hpp
  src/sound_keys_panel.cpp
  src/shader_editor_panel.hpp
  src/shader_editor_panel.cpp
  src/asset_browser_panel.hpp
  src/asset_browser_panel.cpp
  src/docs_panel.hpp
  src/docs_panel.cpp
  src/project_manager_modal.hpp
  src/project_manager_modal.cpp
  src/update_modal.hpp
  src/update_modal.cpp
)

if(WIN32)
  set(IOTA_STUDIO_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/iota_studio.ico")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/iota_studio.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/iota_studio.rc"
    @ONLY
  )
  target_sources(iota_studio PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/iota_studio.rc")
  if(MSVC)
    target_sources(iota_studio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources/iota_studio.manifest")
  endif()
endif()

target_link_libraries(iota_studio
  PRIVATE
    iota_core
    iota_model
    iota_preset_io
    iota_dsp
    iota_audio
    iota_rhi
    iota_engine
    iota_discord
    glfw
    imgui_docking
    imgui_color_text_edit_lib
    stb_hdr
)

if(EMSCRIPTEN)
  target_link_libraries(iota_studio PRIVATE iota_rhi_webgl2)
else()
  target_link_libraries(iota_studio PRIVATE iota_rhi_gl45)
endif()

if(IOTA_ENABLE_VIDEO_EXPORT)
  target_compile_definitions(iota_studio PRIVATE IOTA_ENABLE_VIDEO_EXPORT=1)
  target_link_libraries(iota_studio PRIVATE iota_export)
endif()

if(WIN32)
  target_link_libraries(iota_studio PRIVATE comdlg32 shell32)
elseif(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  find_package(Fontconfig QUIET)
  if(Fontconfig_FOUND)
    target_compile_definitions(iota_studio PRIVATE IOTA_HAS_FONTCONFIG=1)
    target_link_libraries(iota_studio PRIVATE Fontconfig::Fontconfig)
  endif()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(IOTA_WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    if(IOTA_WAYLAND_CLIENT_FOUND)
      target_compile_definitions(iota_studio PRIVATE IOTA_HAS_WAYLAND_CLIENT=1)
      target_link_libraries(iota_studio PRIVATE PkgConfig::IOTA_WAYLAND_CLIENT)
    endif()
  endif()
endif()

if(IOTA_BUILD_MCP)
  target_compile_definitions(iota_studio PRIVATE IOTA_ENABLE_MCP=1)
  target_link_libraries(iota_studio PRIVATE iota_mcp)
endif()

if(IOTA_DISCORD_SDK_FOUND)
  add_custom_command(TARGET iota_studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:discord_social_sdk>"
      "$<TARGET_FILE_DIR:iota_studio>"
    COMMENT "Copying Discord Social SDK runtime library"
  )
  if(UNIX AND NOT APPLE)
    set_target_properties(iota_studio PROPERTIES
      BUILD_RPATH "$ORIGIN"
      INSTALL_RPATH "$ORIGIN"
    )
  endif()
endif()

iota_configure_target(iota_studio)

if(EMSCRIPTEN)
  set_target_properties(iota_studio PROPERTIES SUFFIX ".js")
  target_compile_options(iota_studio PRIVATE -Wno-dollar-in-identifier-extension)
  target_link_options(iota_studio PRIVATE
    "-sMAX_WEBGL_VERSION=2"
    "-sMIN_WEBGL_VERSION=2"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=134217728"
    "-sSTACK_SIZE=2097152"
    "-sNO_EXIT_RUNTIME=1"
    "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','IDBFS','UTF8ToString','stringToUTF8','lengthBytesUTF8','HEAPF32','HEAPU8']"
    "-lidbfs.js"
    "--preload-file" "${CMAKE_SOURCE_DIR}/assets/fonts@/assets/fonts"
  )
endif()

if(NOT EMSCRIPTEN)
  install(TARGETS iota_studio RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
if(IOTA_DISCORD_SDK_FOUND AND NOT EMSCRIPTEN)
  install(FILES "$<TARGET_FILE:discord_social_sdk>" DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  install(FILES "${CMAKE_SOURCE_DIR}/assets/icon.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps"
    RENAME "iota-studio.png"
  )
  install(FILES "${CMAKE_SOURCE_DIR}/assets/icon.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/iota-studio/assets"
  )
  install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/io.github.vorlie.iota_studio.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
  )
  install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/io.github.vorlie.iota_studio.metainfo.xml"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
  )
endif()
