add_library(iota_core STATIC
  src/types.cpp
  src/utf8_path.cpp
  src/zip_archive.cpp
  src/procedural_assets.cpp
  src/update_checker.cpp
)
target_include_directories(iota_core PUBLIC include)
target_compile_definitions(iota_core PUBLIC IOTA_STUDIO_VERSION="${PROJECT_VERSION}")
target_link_libraries(iota_core PRIVATE miniz nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(iota_core PRIVATE winhttp)
endif()
iota_configure_target(iota_core)
