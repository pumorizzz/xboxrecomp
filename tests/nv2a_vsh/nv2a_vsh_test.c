/* The Xbox D3D screen-space epilogue, as every title's programs end:
 *
 *   mov o[0], v0                      position (stand-in for the DP4s)
 *   mul o[0].xyz, R12, c[58]  + rcc R1.x, R12.w
 *   mad o[0].xyz, R12, R1.x, c[59]
 *
 * The rcc is paired with a MAC op that writes no temp, and must still land
 * in R1; sent to the instruction's temp field instead, R1.x stayed 0 and
 * every vertex came out at c[59]. */
#include "nv2a_vsh_interp.h"
#include <math.h>
#include <stdio.h>

enum { T = 1, V = 2, C = 3 };             /* source mux */
#define XYZW 0x1B                         /* swizzle x,y,z,w */
#define WWWW 0xFF
#define XXXX 0x00

static void src(uint32_t w[4], int which, int mux, int reg, int swz)
{
    if (which == 0) { w[1] |= swz; w[2] |= (mux << 26) | (reg << 28); }
    else if (which == 1) { w[2] |= (swz << 17) | (mux << 11) | (reg << 13); }
    else { w[2] |= (swz << 2) | (reg >> 2); w[3] |= (mux << 28) | ((reg & 3) << 30); }
}

int main(void)
{
    uint32_t i0[4] = {0}, i1[4] = {0}, i2[4] = {0};
    const float c58[4] = {320, -240, 1000, 0}, c59[4] = {320, 240, 0, 0};
    float in[NV2A_VSH_INPUTS][4] = {{0}};
    Nv2aVshOutput o;

    /* mov o[0].xyzw, v0 */
    i0[1] = (1u << 21);                         /* MAC mov, input v0 */
    src(i0, 0, V, 0, XYZW);
    i0[3] |= (0xFu << 12) | (1u << 11);         /* o[0] mask xyzw */

    /* mul o[0].xyz, R12, c[58] ; rcc R1.x, R12.w (temp field says R7) */
    i1[1] = (3u << 25) | (2u << 21) | (58u << 13);
    src(i1, 0, T, 12, XYZW); src(i1, 1, C, 0, XYZW); src(i1, 2, T, 12, WWWW);
    i1[3] |= (7u << 20) | (8u << 16) | (0xEu << 12) | (1u << 11);

    /* mad o[0].xyz, R12, R1.x, c[59] ; final */
    i2[1] = (4u << 21) | (59u << 13);
    src(i2, 0, T, 12, XYZW); src(i2, 1, T, 1, XXXX); src(i2, 2, C, 0, XYZW);
    i2[3] |= (0xEu << 12) | (1u << 11) | 1u;

    nv2a_vsh_set_instruction(0, i0);
    nv2a_vsh_set_instruction(1, i1);
    nv2a_vsh_set_instruction(2, i2);
    nv2a_vsh_set_constant(58, c58);
    nv2a_vsh_set_constant(59, c59);

    in[0][0] = 1.0f; in[0][1] = 0.5f; in[0][2] = 0.25f; in[0][3] = 2.0f;
    if (!nv2a_vsh_run((const float (*)[4])in, &o)) {
        puts("FAIL: program did not run");
        return 1;
    }
    /* x = 1*320/2 + 320, y = 0.5*-240/2 + 240, z = 0.25*1000/2, w kept */
    if (fabsf(o.pos[0] - 480) > 1e-3f || fabsf(o.pos[1] - 180) > 1e-3f
        || fabsf(o.pos[2] - 125) > 1e-3f || o.pos[3] != 2.0f) {
        printf("FAIL: pos %g %g %g %g, want 480 180 125 2\n",
               o.pos[0], o.pos[1], o.pos[2], o.pos[3]);
        return 1;
    }

    /* The interpreter decodes a program once and reuses the decode: a program
     * changed after a run -- the title uploading the next shader into the
     * same slots -- must run as changed, by either upload path. Here the
     * final mad loses its c[59] offset (c[60] is zero). */
    {
        const float c60[4] = {0, 0, 0, 0};
        uint32_t j2[4];
        int k;

        nv2a_vsh_set_constant(60, c60);
        for (k = 0; k < 4; k++)
            j2[k] = i2[k];
        j2[1] = (4u << 21) | (60u << 13);
        nv2a_vsh_set_instruction(2, j2);
        if (!nv2a_vsh_run((const float (*)[4])in, &o) || fabsf(o.pos[0] - 160) > 1e-3f) {
            printf("FAIL: after set_instruction pos.x %g, want 160\n", o.pos[0]);
            return 1;
        }
        /* Back to c[59] through the word-by-word upload (PROGRAM_LOAD). */
        nv2a_vsh_set_load_slot(2);
        for (k = 0; k < 4; k++)
            nv2a_vsh_program_word(i2[k]);
        if (!nv2a_vsh_run((const float (*)[4])in, &o) || fabsf(o.pos[0] - 480) > 1e-3f) {
            printf("FAIL: after program words pos.x %g, want 480\n", o.pos[0]);
            return 1;
        }
        /* A new start slot runs from there: from slot 2 alone, R12 is 0 and
         * the mad gives c[59]. */
        nv2a_vsh_set_start_slot(2);
        if (!nv2a_vsh_run((const float (*)[4])in, &o) || fabsf(o.pos[0] - 320) > 1e-3f) {
            printf("FAIL: from start slot 2 pos.x %g, want 320\n", o.pos[0]);
            return 1;
        }
        nv2a_vsh_set_start_slot(0);
    }
    puts("ok");
    return 0;
}
