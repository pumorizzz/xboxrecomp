cmake_minimum_required(VERSION 3.20)
project(nv2a_fence_wake_test C)
# The real memory layout, as tests/memory_regressions.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(nv2a_fence_wake_test test_main.c)
target_include_directories(nv2a_fence_wake_test PRIVATE ../../src/kernel)
target_link_libraries(nv2a_fence_wake_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: RECOMP_PB_EXEC is read once.
add_test(NAME nv2a_fence_wake_exec COMMAND nv2a_fence_wake_test exec ${XBE})
add_test(NAME nv2a_fence_wake_no_exec COMMAND nv2a_fence_wake_test no-exec ${XBE})
