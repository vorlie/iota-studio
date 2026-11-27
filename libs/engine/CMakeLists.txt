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

add_library(iota_engine STATIC
  src/layout.cpp
  src/font_atlas.cpp
  src/particle_system.cpp
  src/shake_eval.cpp
  src/builtin_shader_library.cpp
  src/scene_renderer.cpp
)

target_include_directories(iota_engine
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(iota_engine
  PUBLIC
    iota_core
    iota_model
    iota_dsp
    iota_audio
    iota_rhi
  PRIVATE
    stb_hdr
)

if(WIN32)
  target_link_libraries(iota_engine PRIVATE dwrite)
elseif(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  find_package(Freetype QUIET)
  find_package(Fontconfig QUIET)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(IOTA_HARFBUZZ QUIET IMPORTED_TARGET harfbuzz)
  endif()
  if(Freetype_FOUND AND Fontconfig_FOUND AND TARGET PkgConfig::IOTA_HARFBUZZ)
    target_compile_definitions(iota_engine PRIVATE IOTA_HAS_FREETYPE_HARFBUZZ=1)
    target_link_libraries(iota_engine PRIVATE
      Freetype::Freetype
      Fontconfig::Fontconfig
      PkgConfig::IOTA_HARFBUZZ
    )
    message(STATUS "Typography backend: FreeType + HarfBuzz + Fontconfig")
  else()
    message(STATUS "FreeType/HarfBuzz/Fontconfig not all found; iota_engine using fallback text atlas")
  endif()
endif()

iota_configure_target(iota_engine)
