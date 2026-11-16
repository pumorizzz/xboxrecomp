cmake_minimum_required(VERSION 3.20)
project(nv2a_vblank_grid_regression C)
enable_testing()
set(CMAKE_C_STANDARD 11)

# Pure arithmetic: the header alone, no runtime, no host clock.
add_executable(nv2a-vblank-grid-test nv2a_vblank_grid_test.c)
target_include_directories(nv2a-vblank-grid-test PRIVATE ../../src/kernel)
if(MSVC)
    target_compile_definitions(nv2a-vblank-grid-test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME nv2a-vblank-grid COMMAND nv2a-vblank-grid-test)
