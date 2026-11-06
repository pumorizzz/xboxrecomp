cmake_minimum_required(VERSION 3.20)
project(nv2a_gpu_taken_regression C)
enable_testing()
if(NOT WIN32)
    message(FATAL_ERROR "nv2a_gpu_taken runs the Direct3D 11 back end: Windows only")
endif()
add_executable(nv2a-gpu-taken-test nv2a_gpu_taken_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/d3d/nv2a_gpu_d3d11.c)
target_include_directories(nv2a-gpu-taken-test PRIVATE ../../src/kernel ../../src)
target_link_libraries(nv2a-gpu-taken-test PRIVATE d3d11 dxgi dxguid d3dcompiler)
if(MSVC)
    target_compile_definitions(nv2a-gpu-taken-test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME nv2a-gpu-taken COMMAND nv2a-gpu-taken-test)
