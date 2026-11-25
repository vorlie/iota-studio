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

add_executable(iota_tests
  test_main.cpp
  test_model_preset_io.cpp
  test_hierarchy.cpp
  test_dsp.cpp
  test_particles.cpp
  test_shake.cpp
  test_update_checker.cpp
  test_property_docs.cpp
  test_engine_gl.cpp
  test_video_export.cpp
  test_discord_presence.cpp
)

target_link_libraries(iota_tests
  PRIVATE
    iota_core
    iota_model
    iota_preset_io
    iota_dsp
    iota_audio
    iota_rhi
    iota_rhi_gl45
    iota_engine
    iota_export
    iota_discord
    glfw
    doctest::doctest
)

if(IOTA_BUILD_MCP)
  target_sources(iota_tests PRIVATE test_mcp.cpp)
  target_compile_definitions(iota_tests PRIVATE
    IOTA_ENABLE_MCP=1
    IOTA_MCP_EXE_PATH="$<TARGET_FILE:iota_mcp_app>"
  )
  target_link_libraries(iota_tests PRIVATE iota_mcp)
endif()

if(IOTA_DISCORD_SDK_FOUND AND EXISTS "${IOTA_DISCORD_SDK_RUNTIME_LIB}")
  add_custom_command(TARGET iota_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${IOTA_DISCORD_SDK_RUNTIME_LIB}"
      "$<TARGET_FILE_DIR:iota_tests>"
    COMMENT "Copying Discord Social SDK runtime library for tests"
  )
endif()

iota_configure_target(iota_tests)

add_test(NAME iota_tests COMMAND iota_tests)
