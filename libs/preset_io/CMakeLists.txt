add_library(iota_preset_io STATIC
  src/preset_io.cpp
)
target_include_directories(iota_preset_io PUBLIC include)
target_link_libraries(iota_preset_io
  PUBLIC iota_model iota_core
  PRIVATE nlohmann_json::nlohmann_json
)
iota_configure_target(iota_preset_io)
