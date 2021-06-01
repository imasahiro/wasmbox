cmake_minimum_required(VERSION 3.9)
project(WasmBox LANGUAGES C VERSION 0.0.1)

set(CMAKE_C_STANDARD 11)

add_library(WasmBox src/wasmbox.c)

set(INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${INCLUDE_DIRS})

enable_testing()
file(GLOB_RECURSE TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test/*")

foreach (SOURCE ${TEST_SOURCES})
    get_filename_component(TARGET ${SOURCE} NAME_WE)
    if ("${SOURCE}" MATCHES ".c$")
        string(REGEX REPLACE "\\.c$" "" BASENAME ${SOURCE})
        add_executable(${TARGET} ${SOURCE} ${HEADER})
        target_link_libraries(${TARGET} WasmBox)
        add_test(NAME "test_${TARGET}" COMMAND "${TARGET}")
        set_tests_properties("test_${TARGET}" PROPERTIES TIMEOUT 10)
    endif()
endforeach()
