add_library(iota_discord STATIC
  include/iota/discord/discord_presence.hpp
  src/discord_presence.cpp
)

target_include_directories(iota_discord
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

iota_configure_target(iota_discord)

if(IOTA_DISCORD_SDK_FOUND)
  target_compile_definitions(iota_discord PRIVATE IOTA_HAS_DISCORD_SDK=1)
  target_link_libraries(iota_discord PRIVATE discord_social_sdk)
  if(MSVC)
    target_link_libraries(iota_discord INTERFACE delayimp)
    target_link_options(iota_discord INTERFACE "/DELAYLOAD:discord_partner_sdk.dll")
  endif()
endif()
