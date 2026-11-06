/* The D3D11 back end, run for real on the WARP adapter (no window, no GPU
 * needed).
 *
 * It gives up a target whose memory the title took back.
 * Target A is cleared on the GPU and left dirty, as Fallout leaves its
 * shadow atlas; then the CPU writes A's bytes, as when the title frees the
 * atlas and the heap (RECOMP_HEAP_RECLAIM) hands them to a texture. Nine more
 * targets push A, then B, out of the back end's cache of eight:
 *
 *   - A's bytes keep what the CPU wrote. The back end used to write the
 *     stale clear colour over them -- over a texture, in the game: debris
 *     drawn with wood-coloured splinters, the smoke over the robots gone.
 *   - B, untouched by the CPU, still goes back into guest RAM with its
 *     clear colour: a target the title still owns loses nothing.
 *
 * It hands over to the software rasteriser when the device is lost
 * (RECOMP_GPU_RASTER_LOSE_AT stands in for a driver reset): a clear before
 * the loss stays on the GPU, guest RAM untouched; after the flip that finds
 * the device gone, a clear lands in guest RAM -- the executor draws again.
 * Before, a lost device left the window frozen on its last frame.
 *
 * A dirty target sampled as a texture is checked the same way: target T is
 * cleared on the GPU, its bytes taken by the CPU, then drawn from as a
 * linear A8R8G8B8 texture onto D -- D gets the CPU's bytes, not the stale
 * clear colour.
 *
 * A readback whose Map fails keeps its target dirty: the next one writes it
 * (NV2A_GPU_TEST_HOOKS fails the Map, which WARP never does by itself).
 *
 * Harness as tests/nv2a_points; Windows only. */
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
#define MEM_BYTES 0x200000u
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
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }
void *xbox_FramebufferWindowHandle(void) { return NULL; }
void xbox_FramebufferWindowExternal(int on) { (void)on; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_sync(void);
extern void nv2a_pb_exec_report(void);
extern int nv2a_gpu_test_fail_maps;

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static void mf(uint32_t method, float f) { uint32_t u; memcpy(&u, &f, 4); m(method, u); }

/* A quad over the whole surface at va, fixed function, white, sampling
 * stage 0 -- a linear A8R8G8B8 texture W x H at tex. */
static void draw_textured_at(uint32_t va, uint32_t tex)
{
    static const float I[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    static const float P[6][3] = {
        { 0, 0, 0 }, { W, 0, 0 }, { 0, H, 0 }, { W, 0, 0 }, { W, H, 0 }, { 0, H, 0 },
    };
    uint32_t i, white = 0xFFFFFFFFu;

    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, I[i]);            /* composite */
    for (i = 0; i < 4; i++)
        mf(0x0A20 + 4 * i, 0.0f);            /* SET_VIEWPORT_OFFSET */
    mf(0x0394, 0.0f);                        /* SET_CLIP_MIN */
    mf(0x0398, 16777215.0f);                 /* SET_CLIP_MAX */
    m(0x1E94, 0);                            /* fixed function */
    for (i = 0; i < 6; i++) {
        float tc[2];
        tc[0] = P[i][0]; tc[1] = P[i][1];
        memcpy(s_mem + POS_AT + 12u * i, P[i], 12);
        memcpy(s_mem + DIFF_AT + 4u * i, &white, 4);
        memcpy(s_mem + TC_AT + 8u * i, tc, 8);
    }
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));   /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));    /* attr 3: D3DCOLOR */
    m(0x1744, TC_AT);   m(0x1784, 2 | (2 << 4) | (8 << 8));    /* attr 9: float2 */
    m(0x1B00, tex);                          /* SET_TEXTURE_OFFSET */
    m(0x1B04, (0x12u << 8) | (1u << 16));    /* FORMAT: linear A8R8G8B8, 1 level */
    m(0x1B10, (uint32_t)PITCH << 16);        /* CONTROL1: pitch */
    m(0x1B1C, ((uint32_t)W << 16) | H);      /* IMAGE_RECT */
    m(0x0210, va);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    m(0x1810, (5u << 24) | 0);               /* DRAW_ARRAYS 6 from 0 */
    m(0x17FC, 0);
    nv2a_pb_exec_sync();
}

/* Every target the back end holds dirty, into guest RAM (the report's READ
 * sync, which RECOMP_FB_DUMP asks for; the dump itself has nowhere to go). */
static void write_back_all(void)
{
    _putenv("RECOMP_FB_DUMP=nv2a_gpu_backend_no_such_dir/f_");
    nv2a_pb_exec_report();
    _putenv("RECOMP_FB_DUMP=");
}

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

#define CLEAR_ARGB 0xFF102030u

static void clear_surface_at(uint32_t va)
{
    m(0x0210, va);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x1D90, CLEAR_ARGB);                   /* SET_COLOR_CLEAR_VALUE */
    m(0x1D94, 0xF0);                         /* CLEAR_SURFACE: colour */
}

/* Every dword of the surface at va equals want. */
static int surface_is(uint32_t va, uint32_t want)
{
    uint32_t i, v;
    for (i = 0; i < W * H; i++) {
        memcpy(&v, s_mem + va + 4u * i, 4);
        if (v != want)
            return 0;
    }
    return 1;
}

int main(void)
{
    int i;

    _putenv("RECOMP_GPU_RASTER=warp");
    _putenv("RECOMP_GPU_RASTER_LOSE_AT=3");
    _putenv("RECOMP_FPS_LIMIT=0");
    if (!nv2a_gpu_d3d11_install()) {
        printf("SKIP: no D3D11 WARP device\n");
        return 0;
    }
    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, no AA */
    m(0x0200, (uint32_t)W << 16);            /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);            /* CLIP_VERTICAL */
    m(0x020C, PITCH);                        /* SET_SURFACE_PITCH */

    clear_surface_at(SURF(0));               /* A: dirty on the GPU */
    clear_surface_at(SURF(1));               /* B: dirty, left alone */
    memset(s_mem + SURF(0), 0xAB, PITCH * H); /* A's bytes taken by the title */

    for (i = 2; i < 11; i++)                 /* nine more: A, then B, evicted */
        clear_surface_at(SURF(i));

    CHECK(surface_is(SURF(0), 0xABABABABu));
    CHECK(surface_is(SURF(1), CLEAR_ARGB));

    /* A dirty target sampled as a texture after the CPU took its bytes. */
    clear_surface_at(SURF(14));              /* T */
    memset(s_mem + SURF(14), 0xAB, PITCH * H);
    draw_textured_at(SURF(15), SURF(14));    /* D */
    write_back_all();
    CHECK(surface_is(SURF(15), 0xABABABABu));

    /* A failed readback keeps the target dirty for the next one. */
    clear_surface_at(SURF(16));
    nv2a_gpu_test_fail_maps = 1;
    write_back_all();
    CHECK(surface_is(SURF(16), 0));          /* the Map failed: nothing yet */
    write_back_all();
    CHECK(surface_is(SURF(16), CLEAR_ARGB));

    /* Device loss at the third flip. */
    clear_surface_at(SURF(17));
    CHECK(surface_is(SURF(17), 0));          /* on the GPU, not in guest RAM */
    for (i = 0; i < 3; i++)
        m(0x0130, 0);                        /* FLIP_STALL */
    clear_surface_at(SURF(18));
    CHECK(surface_is(SURF(18), CLEAR_ARGB)); /* the software path drew it */

    nv2a_backend_register(NULL);
    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
