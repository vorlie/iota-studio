add_library(iota_engine STATIC
  src/layout.cpp
  src/font_atlas.cpp
  src/particle_system.cpp
  src/shake_eval.cpp
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
endif()

iota_configure_target(iota_engine)
