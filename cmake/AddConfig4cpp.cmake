include(CPM)

function(add_and_build_config4cpp TARGET_INTERFACE)

CPMAddPackage("gh:maztheman/config4cpp#master")

find_program(MAKE_EXECUTABLE make)

execute_process(
    COMMAND ${MAKE_EXECUTABLE} -C ${config4cpp_SOURCE_DIR}
    WORKING_DIRECTORY ${config4cpp_BINARY_DIR}
)

add_library(${TARGET_INTERFACE} INTERFACE)
target_include_directories(${TARGET_INTERFACE} INTERFACE ${config4cpp_SOURCE_DIR}/include)
target_link_directories(${TARGET_INTERFACE} INTERFACE ${config4cpp_SOURCE_DIR}/lib)


endfunction()