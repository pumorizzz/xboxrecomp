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

# Frame skip (RECOMP_FRAME_SKIP): the executor's cadence and the back end's
# journal, drop and replay, and auto's readiness on a flip-model chain (a
# hidden window). One scenario a run (the limit and the mode are read once);
# see the head of nv2a_gpu_skip_test.c. At 1x, and the ones that check
# pixels again at 3x3 with 4x MSAA.
add_executable(nv2a-gpu-skip-test nv2a_gpu_skip_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/d3d/nv2a_gpu_d3d11.c
    ../../src/d3d/nv2a_gpu_prof.c)
target_include_directories(nv2a-gpu-skip-test PRIVATE ../../src/kernel ../../src)
target_link_libraries(nv2a-gpu-skip-test PRIVATE d3d11 dxgi dxguid d3dcompiler user32)
target_compile_definitions(nv2a-gpu-skip-test PRIVATE NV2A_GPU_TEST_HOOKS)
if(MSVC)
    target_compile_definitions(nv2a-gpu-skip-test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
foreach(s s1 s1b s2 s3 s4 s5 s6 s7 s7b s8 s8b s9 s10 s10b s11 s12 s13 s14 s15 s16 s17 s18 s19 s19b s20 s21 s22 s22b s23 s24 s25 s25b s26 s26b s26c s27)
    add_test(NAME nv2a-gpu-skip-${s} COMMAND nv2a-gpu-skip-test ${s})
    # s11: the executor must not wait for a presenter that never gets ready.
    set_tests_properties(nv2a-gpu-skip-${s} PROPERTIES TIMEOUT 120)
endforeach()
foreach(s s1 s1b s2 s4 s5 s13 s14 s22 s22b s23)
    add_test(NAME nv2a-gpu-skip-${s}-scaled COMMAND nv2a-gpu-skip-test ${s})
    set_tests_properties(nv2a-gpu-skip-${s}-scaled PROPERTIES TIMEOUT 120
        ENVIRONMENT "RECOMP_GPU_RASTER_SCALE=3;RECOMP_GPU_RASTER_MSAA=4")
endforeach()

# The shaders of combiner states (nv2a_gpu_shaderc --rc-keys): rc_keys_test.txt lists
# the states; the back end draws each with a ps_rc of its own, and the tests above
# draw through them too. nv2a-gpu-rckey checks each against the frozen reference
# (ps_reference.hlsl), the general ps_rc (PSCHECK) and the source (see the head of
# nv2a_gpu_rckey_test.c); -scaled at 3x3 with 4x MSAA, -generic with
# RECOMP_GPU_RASTER_RC_GENERIC=1 (no own shader draws, the same pixels).
add_executable(nv2a-gpu-rckey-test nv2a_gpu_rckey_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/d3d/nv2a_gpu_d3d11.c
    ../../src/d3d/nv2a_gpu_prof.c)
target_include_directories(nv2a-gpu-rckey-test PRIVATE ../../src/kernel ../../src)
target_link_libraries(nv2a-gpu-rckey-test PRIVATE d3d11 dxgi dxguid d3dcompiler)
target_compile_definitions(nv2a-gpu-rckey-test PRIVATE NV2A_GPU_TEST_HOOKS
    PS_REF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ps_reference.hlsl"
    SHADERS_HLSL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/nv2a_gpu_shaders.hlsl"
    RC_KEYS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/rc_keys_test.txt")
if(MSVC)
    target_compile_definitions(nv2a-gpu-rckey-test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME nv2a-gpu-rckey COMMAND nv2a-gpu-rckey-test)
add_test(NAME nv2a-gpu-rckey-scaled COMMAND nv2a-gpu-rckey-test)
set_tests_properties(nv2a-gpu-rckey-scaled PROPERTIES
    ENVIRONMENT "RECOMP_GPU_RASTER_SCALE=3;RECOMP_GPU_RASTER_MSAA=4")
add_test(NAME nv2a-gpu-rckey-generic COMMAND nv2a-gpu-rckey-test)
set_tests_properties(nv2a-gpu-rckey-generic PROPERTIES
    ENVIRONMENT "RECOMP_GPU_RASTER_RC_GENERIC=1")

# The texture hash sees every byte, and the back end's epochs (see the head of
# nv2a_gpu_epoch_test.c): its own exe, one scenario a run.
add_executable(nv2a-gpu-epoch-test nv2a_gpu_epoch_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/d3d/nv2a_gpu_d3d11.c
    ../../src/d3d/nv2a_gpu_prof.c)
target_include_directories(nv2a-gpu-epoch-test PRIVATE ../../src/kernel ../../src)
target_link_libraries(nv2a-gpu-epoch-test PRIVATE d3d11 dxgi dxguid d3dcompiler)
target_compile_definitions(nv2a-gpu-epoch-test PRIVATE NV2A_GPU_TEST_HOOKS)
if(MSVC)
    target_compile_definitions(nv2a-gpu-epoch-test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME nv2a-gpu-epoch COMMAND nv2a-gpu-epoch-test)
set_tests_properties(nv2a-gpu-epoch PROPERTIES TIMEOUT 300)
# The same exe, one switch each: the old rule (every method ends an epoch), the
# rule's check, and write-back at every sync.
add_test(NAME nv2a-gpu-epoch-method COMMAND nv2a-gpu-epoch-test)
set_tests_properties(nv2a-gpu-epoch-method PROPERTIES TIMEOUT 300
    ENVIRONMENT "RECOMP_GPU_RASTER_EPOCH=method")
add_test(NAME nv2a-gpu-epoch-verify COMMAND nv2a-gpu-epoch-test)
set_tests_properties(nv2a-gpu-epoch-verify PROPERTIES TIMEOUT 300
    ENVIRONMENT "RECOMP_GPU_RASTER_EPOCH_VERIFY=1")
add_test(NAME nv2a-gpu-epoch-syncall COMMAND nv2a-gpu-epoch-test)
set_tests_properties(nv2a-gpu-epoch-syncall PROPERTIES TIMEOUT 300
    ENVIRONMENT "RECOMP_GPU_RASTER_SYNC=all")

set(NV2A_GPU_RC_KEYS ${CMAKE_CURRENT_SOURCE_DIR}/rc_keys_test.txt)
# nv2a_gpu_d3d11.c's shaders, compiled at build time (src/d3d/nv2a_gpu_shaders.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/nv2a_gpu_shaders.cmake)
nv2a_gpu_shaders(nv2a-gpu-backend-test)
nv2a_gpu_shaders(nv2a-gpu-prof-test)
nv2a_gpu_shaders(nv2a-gpu-ps-test)
nv2a_gpu_shaders(nv2a-gpu-skip-test)
nv2a_gpu_shaders(nv2a-gpu-rckey-test)
nv2a_gpu_shaders(nv2a-gpu-epoch-test)

# The generator's two edges the tests above never reach, as every one of them builds
# with NV2A_GPU_RC_KEYS set:
# - nv2a-gpu-rckey-none: the header with NO --rc-keys (any title without a state
#   file) is built here beside the others, and a small TU includes it
#   (nv2a_gpu_rckey_none_test.c);
# - nv2a-gpu-rckey-keyfile: a state line nv2a_rc_key_make would not build is refused
#   with its line number (rckey_shaderc_check.cmake).
set(rckey_none_dir ${CMAKE_CURRENT_BINARY_DIR}/rckey_none)
file(MAKE_DIRECTORY ${rckey_none_dir})
add_custom_command(OUTPUT ${rckey_none_dir}/nv2a_gpu_shaders_none.h
    COMMAND nv2a-gpu-shaderc ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/nv2a_gpu_shaders.hlsl
            ${rckey_none_dir}/nv2a_gpu_shaders_none.h vs_main:vs_4_0:k_vs_main
    DEPENDS nv2a-gpu-shaderc ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/nv2a_gpu_shaders.hlsl
    COMMENT "nv2a_gpu_shaders_none.h: the header without combiner states"
    VERBATIM)
add_executable(nv2a-gpu-rckey-none-test nv2a_gpu_rckey_none_test.c
    ${rckey_none_dir}/nv2a_gpu_shaders_none.h)
target_include_directories(nv2a-gpu-rckey-none-test PRIVATE ${rckey_none_dir})
add_test(NAME nv2a-gpu-rckey-none COMMAND nv2a-gpu-rckey-none-test)
add_test(NAME nv2a-gpu-rckey-keyfile
    COMMAND ${CMAKE_COMMAND}
        -DSHADERC=$<TARGET_FILE:nv2a-gpu-shaderc>
        -DHLSL=${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/nv2a_gpu_shaders.hlsl
        -DDIR=${CMAKE_CURRENT_BINARY_DIR}/rckey_keyfile
        -P ${CMAKE_CURRENT_SOURCE_DIR}/rckey_shaderc_check.cmake)
