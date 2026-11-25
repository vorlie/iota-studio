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

add_library(iota_discord STATIC
  include/iota/discord/discord_presence.hpp
  src/discord_presence.cpp
)

target_include_directories(iota_discord
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

iota_configure_target(iota_discord)

if(IOTA_DISCORD_SDK_FOUND)
  target_compile_definitions(iota_discord PRIVATE IOTA_HAS_DISCORD_SDK=1)
  target_link_libraries(iota_discord PRIVATE discord_social_sdk)
  if(MSVC)
    target_link_libraries(iota_discord INTERFACE delayimp)
    target_link_options(iota_discord INTERFACE "/DELAYLOAD:discord_partner_sdk.dll")
  endif()
endif()
