cmake_minimum_required(VERSION 3.20)
project(nv2a_near_clip_regression C)
enable_testing()
add_executable(nv2a-near-clip-test nv2a_near_clip_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c)
target_include_directories(nv2a-near-clip-test PRIVATE ../../src/kernel ../../src)
if(MSVC)
    target_compile_definitions(nv2a-near-clip-test PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_link_libraries(nv2a-near-clip-test PRIVATE m pthread)
endif()
add_test(NAME nv2a-near-clip COMMAND nv2a-near-clip-test)
