/*
 * nv2a-gpu-rckey-none: the header nv2a_gpu_shaderc writes when it is given no
 * --rc-keys (what every title without a combiner-state file builds with: the
 * other tests here set NV2A_GPU_RC_KEYS, so none of them compiles this case).
 *
 * The header (nv2a_gpu_shaders_none.h, one cheap spec, no keys) is included
 * here and must be well-formed C with NV2A_GPU_RC_KEYS 0 and the one-element
 * placeholders nv2a_gpu_d3d11.c indexes under `#if NV2A_GPU_RC_KEYS > 0` only.
 * A generator change that breaks the zero-key output -- a missing placeholder,
 * an empty initialiser, a macro left undefined -- fails to compile or run here
 * instead of in a port that has no keys. Every placeholder is used, so a
 * compiler that warns on unused static arrays does not.
 */
#include <stdio.h>
#include "nv2a_gpu_shaders_none.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    int fails = 0;

#if NV2A_GPU_RC_KEYS != 0
    printf("FAIL: NV2A_GPU_RC_KEYS is %d with no --rc-keys\n", NV2A_GPU_RC_KEYS);
    return 1;
#else
    CHECK(sizeof k_rc_key / sizeof k_rc_key[0] == 1);
    CHECK(sizeof k_rc_key[0] / sizeof k_rc_key[0][0] == 37);
    CHECK(k_rc_key[0][0] == 0 && k_rc_key[0][36] == 0);
    CHECK(sizeof k_rc_key_code / sizeof k_rc_key_code[0] == 1);
    CHECK(k_rc_key_code[0] == NULL);
    CHECK(sizeof k_rc_key_len / sizeof k_rc_key_len[0] == 1);
    CHECK(k_rc_key_len[0] == 0);
    CHECK(sizeof k_rc_key_name / sizeof k_rc_key_name[0] == 1);
    CHECK(k_rc_key_name[0][0] == 0);
    CHECK(sizeof k_vs_main > 16);
    CHECK(sizeof s_hlsl > 1000);
    printf("rckey-none: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
#endif
}
