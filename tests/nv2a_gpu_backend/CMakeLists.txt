cmake_minimum_required(VERSION 3.20)
project(nv2a_gpu_backend_regression C)
enable_testing()
if(NOT WIN32)
    message(FATAL_ERROR "nv2a_gpu_backend runs the Direct3D 11 back end: Windows only")
endif()
add_executable(nv2a-gpu-backend-test nv2a_gpu_backend_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/d3d/nv2a_gpu_d3d11.c)
target_include_directories(nv2a-gpu-backend-test PRIVATE ../../src/kernel ../../src)
target_link_libraries(nv2a-gpu-backend-test PRIVATE d3d11 dxgi dxguid d3dcompiler)
# A failed readback, which WARP never produces by itself (see rt_writeback).
target_compile_definitions(nv2a-gpu-backend-test PRIVATE NV2A_GPU_TEST_HOOKS)
if(MSVC)
    target_compile_definitions(nv2a-gpu-backend-test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME nv2a-gpu-backend COMMAND nv2a-gpu-backend-test)
