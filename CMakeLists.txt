cmake_minimum_required(VERSION 3.9)
project(WasmBox LANGUAGES C VERSION 0.0.1)

set(CMAKE_C_STANDARD 11)

add_library(WasmBox src/wasmbox.c src/input-stream.c src/leb128.c src/interpreter.c src/allocator.c)

set(INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${INCLUDE_DIRS})

enable_testing()
add_executable(TestRunner "test/runner.c" ${HEADER})
target_link_libraries(TestRunner WasmBox)

file(GLOB_RECURSE TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test/*")
foreach (SOURCE ${TEST_SOURCES})
    get_filename_component(TARGET ${SOURCE} NAME_WE)
    if ("${SOURCE}" MATCHES ".*_test.c$")
        string(REGEX REPLACE "\\.c$" "" BASENAME ${SOURCE})
        add_executable(${TARGET} ${SOURCE} ${HEADER})
        target_link_libraries(${TARGET} WasmBox)
        add_test(NAME "test_${TARGET}" COMMAND "${TARGET}")
        set_tests_properties("test_${TARGET}" PROPERTIES TIMEOUT 10)
    endif()

    if ("${SOURCE}" MATCHES ".wasm$")
        add_test(NAME "test_wasm_${TARGET}" COMMAND TestRunner "${SOURCE}" "${SOURCE}.result")
        set_tests_properties("test_wasm_${TARGET}" PROPERTIES TIMEOUT 10)
    endif()
endforeach()
