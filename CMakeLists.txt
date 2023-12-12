cmake_minimum_required(VERSION 3.21)

project(libp2p C CXX)

if(PROJECT_IS_TOP_LEVEL)
  set (CMAKE_CXX_STANDARD 20)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CPM)
include(AddConfig4cpp)

option(SSL_ENABLED "SSL support (OpenSSL is required)" ON)
option(ZMTP_ENABLED "ZMTP (zmq) protocol support" ON)
option(BTC_ENABLED "Bitcoin network protocol support" ON)
option(RLPX_ENABLED "RPLx (Ethereum) network protocol support" ON)
option(TEST_ENABLED "Build tests" OFF)
option(SANITIZER_ENABLED "Build with address sanitizer" OFF)
option(PROFILE_ENABLED "Build for profiling" OFF)
option(BUILD_EXAMPLES "Build Examples" OFF)

if (SANITIZER_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

if (PROFILE_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fno-inline -fno-omit-frame-pointer")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-inline -fno-omit-frame-pointer")
endif()

add_and_build_config4cpp(libp2p_config4cpp_lib)

if (SSL_ENABLED OR BTC_ENABLED)
  find_package(OpenSSL)
endif()

if (ZMTP_ENABLED)
    CPMAddPackage(
        NAME libzmq
        GITHUB_REPOSITORY zeromq/libzmq
        GIT_TAG master
        OPTIONS "BUILD_TESTS OFF"
    )
    set(Sources ${Sources} ${libzmq_SOURCE_DIR}/src/zmtp_engine.cpp)
endif()

set(CMAKE_DEBUG_POSTFIX d)

if (WIN32)
  if(MSVC)
    SET (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/msvc/include)
  endif(MSVC)
  set(OS_WINDOWS 1)
  add_definitions(
    -D__STDC_LIMIT_MACROS
    -D__STDC_FORMAT_MACROS
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(OS_LINUX 1)
  set(OS_COMMONUNIX 1)
elseif (APPLE)
  set(OS_DARWIN 1)
  set(OS_COMMONUNIX 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(OS_FREEBSD 1) 
  set(OS_COMMONUNIX 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  set(OS_QNX 1) 
  set(OS_COMMONUNIX 1)
endif()

if (CMAKE_OSX_ARCHITECTURES)
  set(ARCH ${CMAKE_OSX_ARCHITECTURES})
else()
  set(ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()

if (ARCH STREQUAL "i386" OR ARCH STREQUAL "i686")
  set(ARCH_X86 1)
  set(ARCH_NAME "x86")
elseif (ARCH STREQUAL "x86_64" OR ARCH STREQUAL "AMD64")
  set(ARCH_X86_64 1)
  set(ARCH_NAME "x86_64")
elseif (ARCH STREQUAL "aarch64" OR ARCH STREQUAL "arm64")
  set(ARCH_AARCH64 1)
  set(ARCH_NAME "aarch64")
else()
  message(FATAL_ERROR "Unsupported processor architecture")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(OS_64 1)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(OS_32 1)
endif()

if(MSVC)
  add_definitions(
    -D_CRT_SECURE_NO_DEPRECATE
    -D_CRT_SECURE_NO_WARNINGS
    -D_CRT_NONSTDC_NO_DEPRECATE
  )
else(MSVC)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
endif(MSVC)


add_library(libp2p_project_options INTERFACE)

target_include_directories(libp2p_project_options INTERFACE 
  ${CMAKE_CURRENT_SOURCE_DIR}/src/include 
  ${CMAKE_CURRENT_BINARY_DIR}/src/include
)

add_subdirectory(src)

add_library(libp2p::asyncio ALIAS asyncio-0.5)
add_library(libp2p::config4cpp ALIAS libp2p_config4cpp_lib)
add_library(libp2p::asyncioextras ALIAS asyncioextras-0.5)
add_library(libp2p::p2p ALIAS p2p)
add_library(libp2p::p2putils ALIAS p2putils)
add_library(libp2p_all INTERFACE)
target_link_libraries(libp2p_all INTERFACE libp2p::asyncio libp2p::config4cpp libp2p::asyncioextras libp2p::p2p libp2p::p2putils)
add_library(libp2p::all ALIAS libp2p_all)
