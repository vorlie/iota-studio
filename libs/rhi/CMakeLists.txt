add_library(iota_rhi STATIC
  src/rhi.cpp
)

target_include_directories(iota_rhi
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(iota_rhi
  PUBLIC
    iota_core
    glm::glm
)

iota_configure_target(iota_rhi)
