add_library(p2p STATIC
  p2pformat.cpp
  p2pproto.cpp
  p2p.cpp
)

target_link_libraries(p2p PUBLIC libp2p_project_options)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set_property(TARGET p2p PROPERTY POSITION_INDEPENDENT_CODE ON)
endif ()

