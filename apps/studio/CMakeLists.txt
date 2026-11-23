add_executable(iota_studio
  src/main.cpp
  src/editor_app.hpp
  src/editor_app.cpp
  src/inspector_panel.hpp
  src/inspector_panel.cpp
)

target_link_libraries(iota_studio
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
    imgui_docking
    comdlg32
)

iota_configure_target(iota_studio)
