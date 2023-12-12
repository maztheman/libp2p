include(TestBigEndian)
TEST_BIG_ENDIAN(IS_BIGENDIAN)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/libp2pconfig.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/libp2pconfig.h
)


add_subdirectory(asyncio)
add_subdirectory(asyncioextras)
add_subdirectory(p2putils)
add_subdirectory(p2p)

if (BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if (TEST_ENABLED)
  add_subdirectory(test)
endif()
