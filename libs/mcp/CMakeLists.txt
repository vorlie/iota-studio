add_library(iota_mcp STATIC
  src/working_copy.cpp
  src/png_encode.cpp
  src/studio_session.cpp
  src/live_bridge.cpp
  src/tool_registry.cpp
  src/mcp_server.cpp
)

target_include_directories(iota_mcp
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(iota_mcp
  PUBLIC
    iota_core
    iota_model
    iota_preset_io
    iota_dsp
    iota_audio
    iota_rhi
    iota_rhi_gl45
    iota_engine
    iota_export
    nlohmann_json::nlohmann_json
    stb_hdr
    glfw
)

if(WIN32)
  target_link_libraries(iota_mcp PRIVATE advapi32)
endif()

iota_configure_target(iota_mcp)
