# The D3D11 back end's shaders (nv2a_gpu_shaders.hlsl), compiled when the
# toolkit is built: nv2a_gpu_shaderc, a host tool on D3DCompile -- the same
# compiler and flags as the back end used at run time, so the same code byte
# for byte -- writes nv2a_gpu_shaders.h, which nv2a_gpu_d3d11.c includes.
# Rebuilt only when the shader source (or the tool) changes.
#
#   include(<toolkit>/src/d3d/nv2a_gpu_shaders.cmake)
#   nv2a_gpu_shaders(<target that compiles nv2a_gpu_d3d11.c>)
#
# Windows only (Direct3D 11).

set(NV2A_GPU_SHADERS_SRC_DIR ${CMAKE_CURRENT_LIST_DIR})

# <entry>:<target>:<symbol>[:NAME=value,...] for each shader nv2a_gpu_d3d11.c uses.
set(NV2A_GPU_SHADERS_SPECS
    vs_main:vs_4_0:k_vs_main
    ps_screen:ps_4_0:k_ps_screen
    ps_ffp:ps_4_0:k_ps_ffp
    ps_rc:ps_4_0:k_ps_rc
    ps_rc:ps_4_0:k_ps_rc_tex:RC_TEX=1
    ps_rc:ps_4_0:k_ps_rc_rt:RC_TEX=2
    pvs:vs_4_0:k_pvs
    pps:ps_4_0:k_pps
    bvs:vs_4_0:k_bvs
    bps_up:ps_4_0:k_bps_up
    bps_down:ps_4_0:k_bps_down)

function(nv2a_gpu_shaders target)
    set(dir ${CMAKE_BINARY_DIR}/nv2a_gpu_shaders)
    if(NOT TARGET nv2a-gpu-shaders)
        # The tool runs during the build, and it is a Windows program: D3DCompile
        # lives in d3dcompiler_47.dll. A cross build (MinGW from Linux) can run it
        # only through an emulator; without one the build would stop much later,
        # at "nv2a-gpu-shaderc: not found".
        if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
            message(FATAL_ERROR
                "nv2a_gpu_shaders: the D3D11 back end's shaders are compiled during "
                "the build by nv2a-gpu-shaderc, a Windows tool on D3DCompile, and "
                "this is a cross build with no way to run it. Either build on "
                "Windows (MSVC or MinGW-w64), or set CMAKE_CROSSCOMPILING_EMULATOR "
                "to something that runs a Windows exe (tests/proton_run.sh, or "
                "wine) -- under Wine the shaders come from Wine's d3dcompiler, "
                "which may not give the same code as Microsoft's.")
        endif()
        set(hlsl ${NV2A_GPU_SHADERS_SRC_DIR}/nv2a_gpu_shaders.hlsl)
        set(out ${dir}/nv2a_gpu_shaders.h)
        file(MAKE_DIRECTORY ${dir})
        add_executable(nv2a-gpu-shaderc ${NV2A_GPU_SHADERS_SRC_DIR}/nv2a_gpu_shaderc.c)
        target_link_libraries(nv2a-gpu-shaderc PRIVATE d3dcompiler)
        if(MSVC)
            target_compile_definitions(nv2a-gpu-shaderc PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        add_custom_command(OUTPUT ${out}
            COMMAND nv2a-gpu-shaderc ${hlsl} ${out} ${NV2A_GPU_SHADERS_SPECS}
            DEPENDS nv2a-gpu-shaderc ${hlsl}
            COMMENT "nv2a_gpu_shaders.h: compiling the D3D11 back end's shaders"
            VERBATIM)
        add_custom_target(nv2a-gpu-shaders DEPENDS ${out})
    endif()
    add_dependencies(${target} nv2a-gpu-shaders)
    target_include_directories(${target} PRIVATE ${dir})
endfunction()
