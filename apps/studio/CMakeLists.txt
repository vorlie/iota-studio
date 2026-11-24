add_executable(iota_studio
  src/main.cpp
  src/editor_app.hpp
  src/editor_app.cpp
  src/hierarchy_panel.hpp
  src/hierarchy_panel.cpp
  src/inspector_panel.hpp
  src/inspector_panel.cpp
  src/sound_keys_panel.hpp
  src/sound_keys_panel.cpp
  src/asset_browser_panel.hpp
  src/asset_browser_panel.cpp
  src/docs_panel.hpp
  src/docs_panel.cpp
  src/project_manager_modal.hpp
  src/project_manager_modal.cpp
  src/update_modal.hpp
  src/update_modal.cpp
)

if(WIN32)
  set(IOTA_STUDIO_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/iota_studio.ico")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/iota_studio.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/iota_studio.rc"
    @ONLY
  )
  target_sources(iota_studio PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/iota_studio.rc")
endif()

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
    shell32
)

iota_configure_target(iota_studio)
