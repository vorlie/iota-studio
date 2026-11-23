add_library(iota_export STATIC
  src/video_exporter.cpp
)

target_include_directories(iota_export
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(iota_export
  PUBLIC
    iota_core
    iota_model
    iota_audio
    iota_rhi
    iota_engine
)

iota_configure_target(iota_export)
