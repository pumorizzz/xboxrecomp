/* The whole-texture decode the back end uploads from
 * (nv2a_backend_decode_texture_face) against the texel-by-texel reference it
 * replaced (nv2a_backend_decode_texture_face_ref, the executor's own sampler):
 * the same texels, bit for bit, and the same refusals, for every format the
 * sampler converts -- linear and swizzled, the palette format, YUV, DXT1/3/5
 * -- over random bytes, at sizes that are square, wide, tall, odd (linear and
 * DXT), padded (pitch beyond the row), and one face of a cube.
 *
 * The fast decode walks the Morton index by masked increment and decodes a
 * DXT block once; a wrong mask step, a swapped half of the index, a block
 * palette off by a rounding or a texel copied to the wrong place shows here
 * as a mismatch. It also prints the speed of both, for the record. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nv2a_backend.h"

#define MEM_BYTES 0x800000u
#define TEX_AT    0x100000u
#define PAL_AT    0x010000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

static int s_fail;

static uint32_t s_rng = 0x12345678u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t s_fast[1024 * 1024], s_ref[1024 * 1024];

/* One texture, decoded both ways over the same random bytes. */
static void check(uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch,
                  uint32_t palette, uint32_t cube, uint32_t face)
{
    Nv2aTexture t;
    uint32_t i, bytes, n = w * h, bad = 0, first = 0;
    int rf, rr;

    memset(&t, 0, sizeof t);
    t.offset = TEX_AT;
    t.width = w;
    t.height = h;
    t.pitch = pitch;
    t.color = fmt;
    t.levels = 1;
    t.palette = palette;
    t.cube = cube;
    bytes = nv2a_backend_texture_bytes(&t);
    if (!bytes || TEX_AT + bytes > MEM_BYTES) {
        printf("FAIL fmt 0x%02X %ux%u: %u bytes\n", fmt, w, h, bytes);
        s_fail = 1;
        return;
    }
    for (i = 0; i < bytes; i++)
        s_mem[TEX_AT + i] = (uint8_t)rnd();
    memset(s_fast, 0xCD, (size_t)n * 4);
    memset(s_ref, 0xCD, (size_t)n * 4);
    rf = nv2a_backend_decode_texture_face(&t, face, s_fast);
    rr = nv2a_backend_decode_texture_face_ref(&t, face, s_ref);
    if (rf != rr) {
        printf("FAIL fmt 0x%02X %ux%u face %u: returned %d, the reference %d\n",
               fmt, w, h, face, rf, rr);
        s_fail = 1;
        return;
    }
    if (!rr)
        return;
    for (i = 0; i < n; i++)
        if (s_fast[i] != s_ref[i] && !bad++)
            first = i;
    if (bad) {
        printf("FAIL fmt 0x%02X %ux%u face %u: %u texels differ, first (%u,%u)"
               " %08X, the reference %08X\n", fmt, w, h, face, bad, first % w,
               first / w, s_fast[first], s_ref[first]);
        s_fail = 1;
    }
}

static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void speed(uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch)
{
    Nv2aTexture t;
    double t0, t1, t2;
    int k, reps = 4;

    memset(&t, 0, sizeof t);
    t.offset = TEX_AT; t.width = w; t.height = h; t.pitch = pitch;
    t.color = fmt; t.levels = 1;
    t0 = now_s();
    for (k = 0; k < reps; k++)
        nv2a_backend_decode_texture_face(&t, 0, s_fast);
    t1 = now_s();
    for (k = 0; k < reps; k++)
        nv2a_backend_decode_texture_face_ref(&t, 0, s_ref);
    t2 = now_s();
    printf("fmt 0x%02X %ux%u: %.0f Mtexel/s, the reference %.0f\n", fmt, w, h,
           (double)w * h * reps / 1e6 / (t1 - t0 > 1e-6 ? t1 - t0 : 1e-6),
           (double)w * h * reps / 1e6 / (t2 - t1 > 1e-6 ? t2 - t1 : 1e-6));
}

int main(void)
{
    /* Swizzled formats (and their linear twins below); sizes powers of two. */
    static const uint32_t swz[] = { 0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x19 };
    static const uint32_t lin[] = { 0x12, 0x1E, 0x3F, 0x40, 0x41, 0x10, 0x1C,
                                    0x11, 0x1D, 0x13, 0x1F, 0x24, 0x25 };
    static const uint32_t dxt[] = { 0x0C, 0x0E, 0x0F };
    static const uint32_t sq[][2] = { { 1, 1 }, { 2, 2 }, { 64, 64 }, { 256, 32 },
                                      { 8, 128 }, { 512, 4 }, { 1, 16 } };
    static const uint32_t dsz[][2] = { { 4, 4 }, { 64, 64 }, { 128, 32 },
                                       { 10, 6 }, { 3, 5 }, { 1, 1 }, { 20, 12 } };
    uint32_t i, j;

    for (i = 0; i < 1024; i++)
        s_mem[PAL_AT + i] = (uint8_t)rnd();

    for (i = 0; i < sizeof swz / sizeof swz[0]; i++)
        for (j = 0; j < sizeof sq / sizeof sq[0]; j++)
            check(swz[i], sq[j][0], sq[j][1], 0, 0, 0, 0);
    /* The palette format: with a palette, and refused without one. */
    for (j = 0; j < sizeof sq / sizeof sq[0]; j++)
        check(0x0B, sq[j][0], sq[j][1], 0, PAL_AT, 0, 0);
    check(0x0B, 64, 64, 0, 0, 0, 0);
    /* Linear: odd sizes, rows padded past their texels. */
    for (i = 0; i < sizeof lin / sizeof lin[0]; i++) {
        check(lin[i], 64, 64, 64 * 4, 0, 0, 0);
        check(lin[i], 13, 7, 13 * 4 + 12, 0, 0, 0);
        check(lin[i], 640, 3, 4096, 0, 0, 0);
    }
    for (i = 0; i < sizeof dxt / sizeof dxt[0]; i++)
        for (j = 0; j < sizeof dsz / sizeof dsz[0]; j++)
            check(dxt[i], dsz[j][0], dsz[j][1], 0, 0, 0, 0);
    /* One face of a cube, past the first. */
    check(0x06, 32, 32, 0, 0, 1, 3);
    check(0x0C, 32, 32, 0, 0, 1, 5);
    /* A format the sampler does not convert: both refuse. */
    check(0x2E, 16, 16, 64, 0, 0, 0);

    speed(0x06, 512, 512, 0);
    speed(0x05, 512, 512, 0);
    speed(0x12, 512, 512, 2048);
    speed(0x0C, 512, 512, 0);
    speed(0x0F, 512, 512, 0);

    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
