/* The D3D11 back end gives up a target whose memory the title took back.
 *
 * Runs the real back end on the WARP adapter (no window, no GPU needed).
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
#define MEM_BYTES 0x100000u
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

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }

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

    nv2a_backend_register(NULL);
    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
