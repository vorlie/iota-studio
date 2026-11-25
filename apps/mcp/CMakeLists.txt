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

add_executable(iota_mcp_app
  src/main.cpp
)

set_target_properties(iota_mcp_app PROPERTIES
  OUTPUT_NAME "iota_mcp"
)

target_link_libraries(iota_mcp_app
  PRIVATE
    iota_mcp
)

iota_configure_target(iota_mcp_app)
