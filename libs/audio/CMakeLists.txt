add_library(iota_audio STATIC
  src/audio_decoder.cpp
  src/audio_engine.cpp
)

target_include_directories(iota_audio
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(iota_audio
  PUBLIC
    iota_core
  PRIVATE
    miniaudio_hdr
)

iota_configure_target(iota_audio)
