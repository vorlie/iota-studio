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
