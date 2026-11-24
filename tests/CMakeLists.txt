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
    glfw
    doctest::doctest
)

iota_configure_target(iota_tests)

add_test(NAME iota_tests COMMAND iota_tests)
