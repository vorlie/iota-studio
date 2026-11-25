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

add_library(iota_model STATIC
  src/measured_var.cpp
  src/property.cpp
  src/property_docs.cpp
  src/scene.cpp
  src/hierarchy_ops.cpp
  src/command_stack.cpp
)
target_include_directories(iota_model PUBLIC include)
target_link_libraries(iota_model PUBLIC iota_core)
iota_configure_target(iota_model)
