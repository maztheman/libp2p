add_library(p2putils STATIC
  LiteFlatHashTable.c
  HttpParse.cpp
  HttpRequestParse.cpp
  UriParse.cpp
)

target_link_libraries(p2putils PUBLIC libp2p_project_options)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set_property(TARGET p2putils PROPERTY POSITION_INDEPENDENT_CODE ON)
endif ()

install(
  TARGETS p2putils
  ARCHIVE DESTINATION lib
)
