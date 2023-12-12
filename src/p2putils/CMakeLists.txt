if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  add_definitions(-fPIC)
endif ()
 
add_executable(parsergen
  ParserGen.cpp
)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/p2putils)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/http.cpp ${CMAKE_CURRENT_BINARY_DIR}/include/p2putils/http.h
  COMMAND parsergen ${CMAKE_CURRENT_SOURCE_DIR}/http.l ${CMAKE_CURRENT_BINARY_DIR}/http.cpp ${CMAKE_CURRENT_BINARY_DIR}/include/p2putils/http.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/http.l
)

add_library(p2putils
  HttpParse.cpp
  UriParse.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/http.cpp
)

target_include_directories(p2putils PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/include)

install(
  TARGETS p2putils
  ARCHIVE DESTINATION lib
)
