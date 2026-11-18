# nv2a-gpu-rckey-keyfile: how nv2a_gpu_shaderc treats a combiner-state file.
# Run as: cmake -DSHADERC=<exe> -DHLSL=<nv2a_gpu_shaders.hlsl> -DDIR=<scratch dir>
#               -P rckey_shaderc_check.cmake
#
# The back end builds its key with nv2a_rc_key_make, which masks the texture
# classes by stage mode. A hand-edited line that make would not produce used to
# compile into a shader that no draw could ever match, with no message; the tool
# now refuses it with the line number. Here: a canonical line is accepted (and
# the header names it), a line with a texture class on a stage that reads no
# texture is refused (exit 1, "(<line>)"), and so is a repeated line.
# One cheap spec (vs_main) is compiled; the keys themselves are not (the
# non-canonical cases stop before any compile).

file(MAKE_DIRECTORY ${DIR})
set(good "rc n=1 ctl=00000001 prog=00001 fin=0000000C:00001C00 tex=0000000A s0=08040000:18140000:00000C00:00000C00")
set(bad  "rc n=1 ctl=00000001 prog=00000 fin=0000000C:00001C00 tex=0000000A s0=08040000:18140000:00000C00:00000C00")

function(run_shaderc keys_text out_var rc_var err_var)
    file(WRITE ${DIR}/keys.txt "${keys_text}")
    execute_process(COMMAND ${SHADERC} ${HLSL} ${DIR}/out.h --rc-keys ${DIR}/keys.txt
                            vs_main:vs_4_0:k_vs_main
                    RESULT_VARIABLE rc ERROR_VARIABLE err OUTPUT_VARIABLE out)
    set(${rc_var} ${rc} PARENT_SCOPE)
    set(${err_var} "${err}" PARENT_SCOPE)
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

# A canonical line: accepted, and the header has it.
file(REMOVE ${DIR}/out.h)
run_shaderc("# a comment\n\n${good}\n" out rc err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "a canonical state line was refused (${rc}): ${err}")
endif()
file(READ ${DIR}/out.h hdr)
if(NOT hdr MATCHES "#define NV2A_GPU_RC_KEYS 1")
    message(FATAL_ERROR "the header does not say NV2A_GPU_RC_KEYS 1")
endif()

# A texture class on a stage whose mode reads no texture: refused, line 3.
file(REMOVE ${DIR}/out.h)
run_shaderc("# a comment\n${good}\n${bad}\n" out rc err)
if(rc EQUAL 0)
    message(FATAL_ERROR "a non-canonical state line was accepted")
endif()
if(NOT err MATCHES "keys.txt\\(3\\)")
    message(FATAL_ERROR "the refusal does not name line 3: ${err}")
endif()
if(EXISTS ${DIR}/out.h)
    message(FATAL_ERROR "a refused file still wrote a header")
endif()

# A repeated line: refused, line 2.
run_shaderc("${good}\n${good}\n" out rc err)
if(rc EQUAL 0)
    message(FATAL_ERROR "a repeated state line was accepted")
endif()
if(NOT err MATCHES "keys.txt\\(2\\)")
    message(FATAL_ERROR "the refusal does not name line 2: ${err}")
endif()
message(STATUS "rckey-keyfile: OK")
