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
  src/shader_editor_panel.hpp
  src/shader_editor_panel.cpp
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
    iota_discord
    glfw
    imgui_docking
    imgui_color_text_edit_lib
    comdlg32
    shell32
)

if(IOTA_BUILD_MCP)
  target_compile_definitions(iota_studio PRIVATE IOTA_ENABLE_MCP=1)
  target_link_libraries(iota_studio PRIVATE iota_mcp)
endif()

if(IOTA_DISCORD_SDK_FOUND AND EXISTS "${IOTA_DISCORD_SDK_RUNTIME_LIB}")
  add_custom_command(TARGET iota_studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${IOTA_DISCORD_SDK_RUNTIME_LIB}"
      "$<TARGET_FILE_DIR:iota_studio>"
    COMMENT "Copying Discord Social SDK runtime library"
  )
  if(UNIX AND NOT APPLE)
    set_target_properties(iota_studio PROPERTIES
      BUILD_RPATH "$ORIGIN"
      INSTALL_RPATH "$ORIGIN"
    )
  endif()
endif()

iota_configure_target(iota_studio)
