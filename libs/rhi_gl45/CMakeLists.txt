add_library(iota_rhi_gl45 STATIC
  src/gl_loader.cpp
  src/gl45_device.cpp
)

target_include_directories(iota_rhi_gl45
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(iota_rhi_gl45
  PUBLIC
    iota_rhi
  PRIVATE
    opengl32
)

iota_configure_target(iota_rhi_gl45)
