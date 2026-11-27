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

add_library(iota_core STATIC
  src/types.cpp
  src/utf8_path.cpp
  src/zip_archive.cpp
  src/procedural_assets.cpp
  src/update_checker.cpp
)
target_include_directories(iota_core PUBLIC include)
target_compile_definitions(iota_core PUBLIC IOTA_STUDIO_VERSION="${PROJECT_VERSION}")
target_link_libraries(iota_core PRIVATE miniz nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(iota_core PRIVATE winhttp)
elseif(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  target_link_libraries(iota_core PRIVATE Threads::Threads)
  find_package(CURL QUIET)
  if(CURL_FOUND)
    target_compile_definitions(iota_core PRIVATE IOTA_HAS_LIBCURL=1)
    target_link_libraries(iota_core PRIVATE CURL::libcurl)
    message(STATUS "UpdateChecker backend: libcurl")
  else()
    message(STATUS "libcurl not found; UpdateChecker disabled on non-Windows")
  endif()
endif()
iota_configure_target(iota_core)
