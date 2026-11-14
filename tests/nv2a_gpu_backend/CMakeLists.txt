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
# The same at an internal resolution of 3x3 with 4x MSAA (RECOMP_GPU_RASTER_SCALE):
# the rows the CPU wrote go up through the scaled targets, writebacks come down
# averaged, a target sampled as a texture is read at the high resolution.
add_test(NAME nv2a-gpu-backend-scaled COMMAND nv2a-gpu-backend-test)
set_tests_properties(nv2a-gpu-backend-scaled PROPERTIES
    ENVIRONMENT "RECOMP_GPU_RASTER_SCALE=3;RECOMP_GPU_RASTER_MSAA=4")
# And at 2x shown with 3x3 SSAA (RECOMP_GPU_RASTER_SSAA): drawn at 6x, what the
# presenter shows is each 3x3 block averaged (nv2a_gpu_test_ssaa_frame).
add_test(NAME nv2a-gpu-backend-ssaa COMMAND nv2a-gpu-backend-test)
set_tests_properties(nv2a-gpu-backend-ssaa PROPERTIES
    ENVIRONMENT "RECOMP_GPU_RASTER_SCALE=2;RECOMP_GPU_RASTER_SSAA=3")
