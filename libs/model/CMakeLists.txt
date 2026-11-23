add_library(iota_model STATIC
  src/measured_var.cpp
  src/property.cpp
  src/scene.cpp
  src/command_stack.cpp
)
target_include_directories(iota_model PUBLIC include)
target_link_libraries(iota_model PUBLIC iota_core)
iota_configure_target(iota_model)
