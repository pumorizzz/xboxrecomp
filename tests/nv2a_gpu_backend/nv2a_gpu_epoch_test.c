/* The D3D11 back end's texture hash and its epochs, on the WARP adapter.
 *
 * (e1) The hash of a texture's guest bytes sees every byte. The back end
 * decodes a texture again only when its hash changed since the last epoch,
 * so a byte the hash skips is a stale texture in the game (a lit sprite that
 * never changes, a HUD that stays on the old frame). The hash runs four
 * independent chains over 32 bytes a step (one chain is bound by the latency
 * of its multiply), then 8 bytes at a time, then byte by byte for the tail:
 *   - a 64x64 A8R8G8B8 linear texture: one byte changed at offsets 0..39
 *     (every chain's first words and the second step), at the middle and at
 *     the last byte; after each change the texture is drawn again and the
 *     target, written back, equals the texture, texel for texel;
 *   - a 9x3 texture, 108 bytes (three 32-byte steps, an 8-byte step and a
 *     4-byte tail): every one of its bytes changed in turn the same way.
 * Before each draw the target is cleared to another colour, so a stale
 * buffer cannot match by luck, and nv2a_gpu_test_hash_stats shows the hash
 * ran again every time (a changed texture is hashed once an epoch).
 *
 * The epoch rule (nv2a_gpu_d3d11.c, method_tells_title in nv2a_pb_exec.c): an
 * epoch ends where the title can learn how far the GPU has got, and not at a
 * method that only orders it.
 *   (e2) a texture drawn, a boundary (scan end, semaphore, NOP 5, the kickoff
 *        marker, a flip, a report), the CPU rewrites it, drawn again: new texels;
 *   (e3) the same for a target the GPU cleared and the CPU then took;
 *   (e4) fifty draws of one texture among NOP 0, WAIT_FOR_IDLE, surface and
 *        clear methods: hashed once, no epoch ended, no write-back, same picture;
 *   (e5) as (e4) with RECOMP_GPU_RASTER_EPOCH=method: hashed fifty times;
 *   (e6) RECOMP_GPU_RASTER_EPOCH_VERIFY=1 counts a texture the CPU rewrote
 *        inside an epoch, and does not count one across a boundary; the same
 *        for a clean target the CPU rewrote (stale targets, rt_check) and for
 *        a dirty target the CPU took (stale taken, rt_taken);
 *   (e7) RECOMP_GPU_RASTER_SYNC=all: WAIT_FOR_IDLE writes a dirty target back.
 * e5-e7 run in the same exe under the switch (ctest sets it).
 *
 * Harness as tests/nv2a_gpu_backend/nv2a_gpu_backend_test.c; Windows only. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "d3d/nv2a_gpu_d3d11.h"

#define W 64
#define H 64
#define PITCH (W * 4)
#define SURF(i) (0x10000u + (uint32_t)(i) * 0x10000u)
#define MEM_BYTES 0x240000u
#define POS_AT  0x1A0000u
#define DIFF_AT 0x1B0000u
#define TC_AT   0x1C0000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor and the back end expect the runtime to provide -- */
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
void xbox_FramebufferWindowShown(void) {}
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }
void *xbox_FramebufferWindowHandle(void) { return NULL; }
void xbox_FramebufferWindowExternal(int on) { (void)on; }
void xbox_FramebufferWindowFit(uint32_t cw, uint32_t ch, int32_t out[4])
{
    out[0] = out[1] = 0;
    out[2] = (int32_t)cw;
    out[3] = (int32_t)ch;
}

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_sync(void);
extern void nv2a_pb_exec_report(void);
extern void nv2a_gpu_test_hash_stats(uint64_t out[4]);
extern void nv2a_gpu_test_epoch_stats(uint32_t out[8]);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#define SEM_AT 0x1F0000u

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static void mf(uint32_t method, float f) { uint32_t u; memcpy(&u, &f, 4); m(method, u); }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

/* Fixed function, white diffuse, a w x h linear A8R8G8B8 texture at tex
 * sampled 1:1 by a w x h rectangle at the surface's origin. */
static void draw_rect_tex_nosync(uint32_t target, uint32_t tex, uint32_t w, uint32_t h)
{
    static const float I[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    float P[6][3], x1 = (float)w, y1 = (float)h;
    uint32_t i, white = 0xFFFFFFFFu;

    static const float Q[6][2] = { {0,0},{1,0},{0,1},{1,0},{1,1},{0,1} };
    for (i = 0; i < 6; i++) {
        P[i][0] = Q[i][0] * x1; P[i][1] = Q[i][1] * y1; P[i][2] = 0.0f;
    }
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, I[i]);            /* composite */
    for (i = 0; i < 4; i++)
        mf(0x0A20 + 4 * i, 0.0f);            /* SET_VIEWPORT_OFFSET */
    mf(0x0394, 0.0f);                        /* SET_CLIP_MIN */
    mf(0x0398, 16777215.0f);                 /* SET_CLIP_MAX */
    m(0x1E94, 0);                            /* fixed function */
    for (i = 0; i < 6; i++) {
        float tc[2];
        tc[0] = P[i][0]; tc[1] = P[i][1];    /* linear: texels */
        memcpy(s_mem + POS_AT + 12u * i, P[i], 12);
        memcpy(s_mem + DIFF_AT + 4u * i, &white, 4);
        memcpy(s_mem + TC_AT + 8u * i, tc, 8);
    }
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));   /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));    /* attr 3: D3DCOLOR */
    m(0x1744, TC_AT);   m(0x1784, 2 | (2 << 4) | (8 << 8));    /* attr 9: float2 */
    m(0x1B00, tex);                          /* SET_TEXTURE_OFFSET */
    m(0x1B04, (0x12u << 8) | (1u << 16) | (6u << 20) | (6u << 24));
    m(0x1B10, (w * 4u) << 16);               /* CONTROL1: pitch */
    m(0x1B1C, (w << 16) | h);                /* IMAGE_RECT */
    m(0x0210, target);                       /* SET_SURFACE_COLOR_OFFSET: no epoch ends */
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    m(0x1810, (5u << 24) | 0);               /* DRAW_ARRAYS 6 from 0 */
    m(0x17FC, 0);
}

/* The same, then the scan ends: a new epoch. */
static void draw_rect_tex(uint32_t target, uint32_t tex, uint32_t w, uint32_t h)
{
    draw_rect_tex_nosync(target, tex, w, h);
    nv2a_pb_exec_sync();
}

static void write_back_all(void)
{
    _putenv("RECOMP_FB_DUMP=nv2a_gpu_epoch_no_such_dir/f_");
    nv2a_pb_exec_report();
    _putenv("RECOMP_FB_DUMP=");
}

static void clear_to(uint32_t va, uint32_t argb)
{
    m(0x0210, va);
    m(0x1D90, argb);                         /* SET_COLOR_CLEAR_VALUE */
    m(0x1D94, 0xF0);                         /* CLEAR_SURFACE: colour */
}

/* The w x h texels at tex against the same pixels of the target at va. */
static int same_texels(uint32_t va, uint32_t tex, uint32_t w, uint32_t h)
{
    uint32_t x, y, a, b;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            memcpy(&a, s_mem + va + y * PITCH + 4u * x, 4);
            memcpy(&b, s_mem + tex + (y * w + x) * 4u, 4);
            if (a != b) {
                printf("  texel (%u,%u): target 0x%08X, texture 0x%08X\n", x, y, a, b);
                return 0;
            }
        }
    return 1;
}

/* Change one byte at `off`, draw, write back: the target shows the new texel. */
static void flip_byte_case(uint32_t target, uint32_t tex, uint32_t w, uint32_t h, uint32_t off,
                           uint32_t *round)
{
    uint64_t a[4], b[4];

    s_mem[tex + off] ^= (uint8_t)(0x5A + (*round & 7));
    clear_to(target, 0xFF000000u | (0x010203u * (++*round & 0x3F)));
    nv2a_gpu_test_hash_stats(a);
    draw_rect_tex(target, tex, w, h);
    nv2a_gpu_test_hash_stats(b);
    write_back_all();
    if (!same_texels(target, tex, w, h) || b[0] != a[0] + 1) {
        printf("FAIL: %ux%u texture, byte %u changed: %s\n", w, h, off,
               b[0] != a[0] + 1 ? "no new hash this epoch" : "the draw shows the old texel");
        s_fail = 1;
    }
}

/* ---- the epoch rule ------------------------------------------------------ */

#define NBOUND 6
static const char *const bound_name[NBOUND] = {
    "scan end", "semaphore", "NOP 5", "kickoff marker", "flip", "report"
};

/* What the title can see of the GPU's progress: each of these ends an epoch. */
static void boundary(int b)
{
    switch (b) {
    case 0: nv2a_pb_exec_sync(); break;                  /* SCAN_END */
    case 1: m(0x1D70, 0x77); break;                      /* the fence semaphore */
    case 2: m(0x0100, 5); break;                         /* NO_OPERATION 5 */
    case 3: nv2a_pb_exec_method(5, 0x0310, 0x1234); break;   /* KickOff's marker */
    case 4: m(0x0130, 0); break;                         /* FLIP_STALL */
    default: m(0x17C8, 0); break;                        /* a report */
    }
}

static uint32_t sum3(const uint32_t *e) { return e[0] + e[1] + e[2]; }

/* (e2) A texture drawn, one boundary the title can see, the CPU rewrites it,
 * drawn again: the new texels show. Were that boundary only an ordering
 * method the epoch would go on and the cached decode would be drawn. */
static void e2_tex(void)
{
    uint32_t tex = SURF(9), target = SURF(8), b, i;
    uint64_t a[4], c[4];

    nv2a_pb_set_semaphore_target(SEM_AT);
    for (b = 0; b < NBOUND; b++) {
        nv2a_pb_exec_sync();
        for (i = 0; i < 16 * 16 * 4; i++)
            s_mem[tex + i] = (uint8_t)(i * 5 + b);
        clear_to(target, 0xFF102030u);
        draw_rect_tex_nosync(target, tex, 16, 16);
        boundary((int)b);
        for (i = 0; i < 16 * 16 * 4; i += 7)
            s_mem[tex + i] ^= (uint8_t)(0x33 + b);
        clear_to(target, 0xFF405060u);
        nv2a_gpu_test_hash_stats(a);
        draw_rect_tex_nosync(target, tex, 16, 16);
        nv2a_gpu_test_hash_stats(c);
        write_back_all();
        if (!same_texels(target, tex, 16, 16) || c[0] != a[0] + 1) {
            printf("FAIL (e2): after a %s the rewritten texture is %s\n", bound_name[b],
                   c[0] != a[0] + 1 ? "not hashed again" : "drawn as it was");
            s_fail = 1;
        }
    }
}

/* (e3) A target the GPU cleared, sampled as a texture; one boundary; the CPU
 * writes its bytes; sampled again: the draw takes the CPU's bytes. */
static void e3_target(void)
{
    uint32_t src = SURF(10), dst = SURF(11), b, i, v;

    nv2a_pb_set_semaphore_target(SEM_AT);
    for (b = 0; b < NBOUND; b++) {
        uint32_t fill = 0x01010101u * (0xA0 + b);
        nv2a_pb_exec_sync();
        clear_to(src, 0xFF102030u);                      /* dirty on the GPU */
        draw_rect_tex_nosync(dst, src, W, H);            /* sampled: looked at */
        boundary((int)b);
        for (i = 0; i < W * H; i++)
            memcpy(s_mem + src + 4u * i, &fill, 4);      /* the CPU took the bytes */
        clear_to(dst, 0xFF405060u);
        draw_rect_tex_nosync(dst, src, W, H);
        write_back_all();
        memcpy(&v, s_mem + dst, 4);
        memcpy(&i, s_mem + dst + PITCH * (H - 1) + 4u * (W - 1), 4);
        if (v != fill || i != fill) {
            printf("FAIL (e3): after a %s the target taken by the CPU is sampled as %08X, want %08X\n",
                   bound_name[b], v, fill);
            s_fail = 1;
        }
    }
}

/* (e4, e5) Fifty draws of one texture with only ordering methods between
 * them. By the rule the texture is hashed once and no epoch ends; with
 * RECOMP_GPU_RASTER_EPOCH=method every one of them ends one and the texture
 * is hashed fifty times. The picture is the texture either way. Also: an
 * ordering method does not write a dirty target back (a sync that did would
 * be the old kind of sync). */
static void e4_order(int every)
{
    uint32_t tex = SURF(7), target = SURF(6), probe = SURF(14), i;
    uint64_t a[4], c[4];
    uint32_t ea[8], ec[8];

    for (i = 0; i < 16 * 16 * 4; i++)
        s_mem[tex + i] = (uint8_t)(i * 3 + 1);
    nv2a_pb_exec_sync();
    clear_to(target, 0xFF102030u);
    nv2a_gpu_test_hash_stats(a);
    nv2a_gpu_test_epoch_stats(ea);
    for (i = 0; i < 50; i++) {
        draw_rect_tex_nosync(target, tex, 16, 16);
        m(0x0100, 0);                        /* NOP */
        m(0x0110, 0);                        /* WAIT_FOR_IDLE */
        m(0x0210, target);                   /* SET_SURFACE_COLOR_OFFSET */
        m(0x1D90, 0xFF000000u + i);          /* SET_COLOR_CLEAR_VALUE */
        m(0x1D94, 0);                        /* CLEAR_SURFACE, nothing asked */
    }
    nv2a_gpu_test_hash_stats(c);
    nv2a_gpu_test_epoch_stats(ec);
    if (every) {
        if (c[0] - a[0] != 50 || sum3(ec) - sum3(ea) < 49) {
            printf("FAIL (e5): hashes %u, epochs %u; want 50 and 49 or more\n",
                   (uint32_t)(c[0] - a[0]), sum3(ec) - sum3(ea));
            s_fail = 1;
        }
    } else if (c[0] - a[0] != 1 || sum3(ec) != sum3(ea)) {
        printf("FAIL (e4): hashes %u, epochs %u (methods %u, scan ends %u, flips %u); want 1 and 0\n",
               (uint32_t)(c[0] - a[0]), sum3(ec) - sum3(ea), ec[0] - ea[0], ec[1] - ea[1],
               ec[2] - ea[2]);
        s_fail = 1;
    }
    write_back_all();
    if (!same_texels(target, tex, 16, 16)) {
        printf("FAIL (e4/e5): the picture is not the texture\n");
        s_fail = 1;
    }
    if (!every) {                            /* lazy: an ordering method writes nothing back */
        uint32_t v;
        clear_to(probe, 0xFF778899u);
        m(0x0110, 0);
        memcpy(&v, s_mem + probe, 4);
        if (v != 0) {
            printf("FAIL (e4): WAIT_FOR_IDLE wrote a dirty target back\n");
            s_fail = 1;
        }
        write_back_all();
    }
}

/* (e6) RECOMP_GPU_RASTER_EPOCH_VERIFY=1: the CPU rewrites a texture inside
 * an epoch -- the rule's hole -- and it is counted; across a boundary it is
 * not. */
static void e6_verify(void)
{
    uint32_t tex = SURF(13), target = SURF(12), i;
    uint32_t a[8], c[8];

    for (i = 0; i < 16 * 16 * 4; i++)
        s_mem[tex + i] = (uint8_t)(i * 11 + 2);
    nv2a_pb_exec_sync();
    draw_rect_tex_nosync(target, tex, 16, 16);
    draw_rect_tex_nosync(target, tex, 16, 16);       /* unchanged: nothing found */
    nv2a_gpu_test_epoch_stats(a);
    CHECK(a[5] == 0);
    s_mem[tex + 100] ^= 0x5A;                        /* no boundary */
    draw_rect_tex_nosync(target, tex, 16, 16);
    nv2a_gpu_test_epoch_stats(c);
    CHECK(c[5] == a[5] + 1);
    nv2a_pb_exec_sync();                             /* with one: seen, not stale */
    s_mem[tex + 200] ^= 0x5A;
    draw_rect_tex_nosync(target, tex, 16, 16);
    draw_rect_tex_nosync(target, tex, 16, 16);
    nv2a_gpu_test_epoch_stats(a);
    CHECK(a[5] == c[5]);
    CHECK(a[6] == 0 && a[7] == 0);

    /* A clean target the CPU rewrites inside an epoch (rt_check's early exit):
     * the GPU cleared it, a read wrote it back (clean, shadow equal), a draw
     * sampled it; then the CPU changes a texel and it is sampled again without
     * a boundary -> counted as a stale target. With a boundary: found by the
     * check itself, not counted. */
    {
        uint32_t src = SURF(15), dst = SURF(16), fill = 0xFF314159u;

        nv2a_pb_exec_sync();
        clear_to(src, 0xFF102030u);
        write_back_all();
        draw_rect_tex_nosync(dst, src, W, H);
        draw_rect_tex_nosync(dst, src, W, H);
        nv2a_gpu_test_epoch_stats(a);
        CHECK(a[6] == 0);
        memcpy(s_mem + src + PITCH * 5 + 4u * 7, &fill, 4);   /* no boundary */
        draw_rect_tex_nosync(dst, src, W, H);
        nv2a_gpu_test_epoch_stats(c);
        CHECK(c[6] == a[6] + 1);
        CHECK(c[7] == a[7]);
        nv2a_pb_exec_sync();                         /* with one: the check takes it */
        fill ^= 0x00FF00FFu;
        memcpy(s_mem + src + PITCH * 6 + 4u * 9, &fill, 4);
        draw_rect_tex_nosync(dst, src, W, H);
        draw_rect_tex_nosync(dst, src, W, H);
        nv2a_gpu_test_epoch_stats(a);
        CHECK(a[6] == c[6]);
        write_back_all();
    }

    /* A dirty target the CPU takes inside an epoch (rt_taken's look): cleared on
     * the GPU, sampled (looked at), the CPU writes its bytes without a
     * boundary, sampled again -> counted as taken. Across a boundary it is
     * the ordinary check's business. */
    {
        uint32_t src = SURF(17), dst = SURF(18), fill = 0x5A5A5A5Au;

        nv2a_pb_exec_sync();
        clear_to(src, 0xFF102030u);
        draw_rect_tex_nosync(dst, src, W, H);
        nv2a_gpu_test_epoch_stats(a);
        CHECK(a[7] == 0);
        for (i = 0; i < W * H; i++)
            memcpy(s_mem + src + 4u * i, &fill, 4);  /* the CPU took the bytes */
        draw_rect_tex_nosync(dst, src, W, H);
        nv2a_gpu_test_epoch_stats(c);
        CHECK(c[7] == a[7] + 1);
        CHECK(c[6] == a[6]);
        write_back_all();
        nv2a_pb_exec_sync();
        clear_to(src, 0xFF102030u);
        draw_rect_tex_nosync(dst, src, W, H);
        nv2a_pb_exec_sync();                         /* with one: not counted */
        for (i = 0; i < W * H; i++)
            memcpy(s_mem + src + 4u * i, &fill, 4);
        draw_rect_tex_nosync(dst, src, W, H);
        nv2a_gpu_test_epoch_stats(a);
        CHECK(a[7] == c[7]);
    }
}

/* (e7) RECOMP_GPU_RASTER_SYNC=all: an ordering method writes a dirty target
 * back to guest RAM. */
static void e7_sync_all(void)
{
    uint32_t probe = SURF(14), v;

    nv2a_pb_exec_sync();
    clear_to(probe, 0xFF778899u);
    memcpy(&v, s_mem + probe, 4);
    CHECK(v == 0);
    m(0x0110, 0);                            /* WAIT_FOR_IDLE */
    memcpy(&v, s_mem + probe, 4);
    CHECK(v == 0xFF778899u);
}

int main(void)
{
    uint32_t tex = SURF(3), target = SURF(2), x, y, off, round = 0;
    const char *ep = getenv("RECOMP_GPU_RASTER_EPOCH");
    int mode = (ep && strcmp(ep, "method") == 0) ? 1
             : getenv("RECOMP_GPU_RASTER_EPOCH_VERIFY") ? 2
             : getenv("RECOMP_GPU_RASTER_SYNC") ? 3 : 0;
    static const uint32_t big[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                    20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
                                    37, 38, 39, 4096 + 13, 8192 + 31, 8192 + 32, PITCH * H - 1 };

    _putenv("RECOMP_GPU_RASTER=warp");
    _putenv("RECOMP_FPS_LIMIT=0");
    if (!nv2a_gpu_d3d11_install()) {
        printf("SKIP: no D3D11 WARP device\n");
        return 0;
    }
    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, no AA */
    m(0x0200, (uint32_t)W << 16);            /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);            /* CLIP_VERTICAL */
    m(0x020C, PITCH);                        /* SET_SURFACE_PITCH */

    if (mode == 1) {
        e4_order(1);
        printf(s_fail ? "FAIL\n" : "PASS\n");
        return s_fail;
    }
    if (mode == 2) {
        e6_verify();
        printf(s_fail ? "FAIL\n" : "PASS\n");
        return s_fail;
    }
    if (mode == 3) {
        e7_sync_all();
        printf(s_fail ? "FAIL\n" : "PASS\n");
        return s_fail;
    }

    /* (e1) 64x64: the texels are distinct. */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint32_t t = 0xFF000000u | (y << 16) | (x << 8) | ((x * 37u + y * 101u) & 0xFFu);
            memcpy(s_mem + tex + (y * W + x) * 4u, &t, 4);
        }
    clear_to(target, 0xFF102030u);
    draw_rect_tex(target, tex, W, H);
    write_back_all();
    CHECK(same_texels(target, tex, W, H));
    for (x = 0; x < sizeof big / sizeof big[0]; x++)
        flip_byte_case(target, tex, W, H, big[x], &round);

    /* (e1) 9x3 = 108 bytes: three 32-byte steps, an 8-byte step, a 4-byte tail. */
    tex = SURF(5);
    target = SURF(4);
    for (off = 0; off < 108; off++)
        s_mem[tex + off] = (uint8_t)(off * 7 + 3);
    clear_to(target, 0xFF102030u);
    draw_rect_tex(target, tex, 9, 3);
    write_back_all();
    CHECK(same_texels(target, tex, 9, 3));
    for (off = 0; off < 108; off++)
        flip_byte_case(target, tex, 9, 3, off, &round);

    e2_tex();
    e3_target();
    e4_order(0);

    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
