add_library(iota_dsp STATIC
  src/sample_providers.cpp
  src/sound_keys.cpp
  src/mvar_evaluator.cpp
)

target_include_directories(iota_dsp
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(iota_dsp
  PUBLIC
    iota_core
    iota_model
  PRIVATE
    kissfft_real
)

iota_configure_target(iota_dsp)
