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
    ../../src/d3d/nv2a_gpu_d3d11.c
    ../../src/d3d/nv2a_gpu_prof.c)
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

# The GPU profile (RECOMP_GPU_PROF, src/d3d/nv2a_gpu_prof.c) on the same back
# end: its own exe, as the one above loses the device at the third flip.
add_executable(nv2a-gpu-prof-test nv2a_gpu_prof_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/d3d/nv2a_gpu_d3d11.c
    ../../src/d3d/nv2a_gpu_prof.c)
target_include_directories(nv2a-gpu-prof-test PRIVATE ../../src/kernel ../../src)
target_link_libraries(nv2a-gpu-prof-test PRIVATE d3d11 dxgi dxguid d3dcompiler)
target_compile_definitions(nv2a-gpu-prof-test PRIVATE NV2A_GPU_TEST_HOOKS)
if(MSVC)
    target_compile_definitions(nv2a-gpu-prof-test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# With pipeline statistics (pixel shader invocations), without them (timestamps
# only: no statistics query is made), and at 3x3 with 4x MSAA (resolves).
add_test(NAME nv2a-gpu-prof COMMAND nv2a-gpu-prof-test)
set_tests_properties(nv2a-gpu-prof PROPERTIES ENVIRONMENT "RECOMP_GPU_PROF=100000,stats")
add_test(NAME nv2a-gpu-prof-ts COMMAND nv2a-gpu-prof-test)
set_tests_properties(nv2a-gpu-prof-ts PROPERTIES ENVIRONMENT "RECOMP_GPU_PROF=100000")
add_test(NAME nv2a-gpu-prof-scaled COMMAND nv2a-gpu-prof-test)
set_tests_properties(nv2a-gpu-prof-scaled PROPERTIES
    ENVIRONMENT "RECOMP_GPU_PROF=100000,stats;RECOMP_GPU_RASTER_SCALE=3;RECOMP_GPU_RASTER_MSAA=4")
# The device lost at the second flip with the profile on: it lets go of its
# queries first and switches off; nothing afterwards touches the context.
add_test(NAME nv2a-gpu-prof-lost COMMAND nv2a-gpu-prof-test)
set_tests_properties(nv2a-gpu-prof-lost PROPERTIES
    ENVIRONMENT "RECOMP_GPU_PROF=100000;RECOMP_GPU_RASTER_LOSE_AT=2")
# Under ctest -j: where tmpfile() fails, the four write their report through
# one file in the build directory (report_lines), so one at a time.
set_tests_properties(nv2a-gpu-prof nv2a-gpu-prof-ts nv2a-gpu-prof-scaled nv2a-gpu-prof-lost
    PROPERTIES RESOURCE_LOCK nv2a_gpu_prof_report)

# The pixel shaders against a frozen copy of s_hlsl (ps_reference.hlsl), bit for
# bit, and the combiners against nv2a_rc_eval; at 1x, 3x3 + 4x MSAA, 2x + 3x3 SSAA.
# Each takes minutes on WARP (the combiner shaders run on the CPU); they can
# run side by side (ctest -j 3).
add_executable(nv2a-gpu-ps-test nv2a_gpu_ps_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/d3d/nv2a_gpu_d3d11.c
    ../../src/d3d/nv2a_gpu_prof.c)
target_include_directories(nv2a-gpu-ps-test PRIVATE ../../src/kernel ../../src)
target_link_libraries(nv2a-gpu-ps-test PRIVATE d3d11 dxgi dxguid d3dcompiler)
target_compile_definitions(nv2a-gpu-ps-test PRIVATE NV2A_GPU_TEST_HOOKS
    PS_REF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ps_reference.hlsl"
    SHADERS_HLSL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/nv2a_gpu_shaders.hlsl")
if(MSVC)
    target_compile_definitions(nv2a-gpu-ps-test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME nv2a-gpu-ps COMMAND nv2a-gpu-ps-test)
add_test(NAME nv2a-gpu-ps-scaled COMMAND nv2a-gpu-ps-test)
set_tests_properties(nv2a-gpu-ps-scaled PROPERTIES
    ENVIRONMENT "RECOMP_GPU_RASTER_SCALE=3;RECOMP_GPU_RASTER_MSAA=4")
add_test(NAME nv2a-gpu-ps-ssaa COMMAND nv2a-gpu-ps-test)
set_tests_properties(nv2a-gpu-ps-ssaa PROPERTIES
    ENVIRONMENT "RECOMP_GPU_RASTER_SCALE=2;RECOMP_GPU_RASTER_SSAA=3")

# nv2a_gpu_d3d11.c's shaders, compiled at build time (src/d3d/nv2a_gpu_shaders.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/nv2a_gpu_shaders.cmake)
nv2a_gpu_shaders(nv2a-gpu-backend-test)
nv2a_gpu_shaders(nv2a-gpu-prof-test)
nv2a_gpu_shaders(nv2a-gpu-ps-test)
