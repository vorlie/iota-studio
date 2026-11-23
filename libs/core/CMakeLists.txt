add_library(iota_core STATIC
  src/types.cpp
  src/utf8_path.cpp
  src/zip_archive.cpp
  src/procedural_assets.cpp
)
target_include_directories(iota_core PUBLIC include)
target_link_libraries(iota_core PRIVATE miniz)
iota_configure_target(iota_core)
