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
 * clear colour. And when the title draws into T again after taking its
 * bytes, D gets what was drawn last -- dropping the stale pixels once T is
 * sampled must not take the fresh ones along. (In between it draws
 * elsewhere, as a title must: D3D frees a surface only once the GPU is done
 * with it, and the back end looks for taken bytes when the target changes.)
 * Nor may a writeback in the same epoch as a look write stale pixels: T is
 * sampled, then taken, then -- no epoch boundary in between -- decoded as a
 * DXT1 texture, which writes T back first. T's bytes stay the CPU's.
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
#include "d3d/d3d8_swizzle.h"

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
void xbox_FramebufferWindowFit(uint32_t cw, uint32_t ch, int32_t out[4])
{
    out[0] = out[1] = 0;
    out[2] = (int32_t)cw;
    out[3] = (int32_t)ch;
}

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_sync(void);
extern void nv2a_pb_exec_report(void);
extern int nv2a_gpu_test_fail_maps;
extern int nv2a_gpu_test_bc_uploads;

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static void mf(uint32_t method, float f) { uint32_t u; memcpy(&u, &f, 4); m(method, u); }

/* A quad over the whole surface at va, fixed function, white, sampling
 * stage 0 -- a W x H texture at tex, linear A8R8G8B8 unless fmt says else.
 * With sync 0 the executor's scan does not end: the back end's epoch stays. */
static void draw_quad(uint32_t va, uint32_t tex, uint32_t fmt, int sync);

static void draw_textured_at(uint32_t va, uint32_t tex) { draw_quad(va, tex, 0x12, 1); }

/* Fixed function, white diffuse, stage 0 the W x H texture at tex; vertices
 * `n` of P (positions, texcoords = positions). */
static void fixed_state(uint32_t tex, uint32_t fmt, const float (*P)[3], uint32_t n)
{
    static const float I[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    uint32_t i, white = 0xFFFFFFFFu;

    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, I[i]);            /* composite */
    for (i = 0; i < 4; i++)
        mf(0x0A20 + 4 * i, 0.0f);            /* SET_VIEWPORT_OFFSET */
    mf(0x0394, 0.0f);                        /* SET_CLIP_MIN */
    mf(0x0398, 16777215.0f);                 /* SET_CLIP_MAX */
    m(0x1E94, 0);                            /* fixed function */
    for (i = 0; i < n; i++) {
        float tc[2];
        /* Texels for a linear texture, 0..1 for the rest (W == H). */
        float sc = fmt == 0x12 ? 1.0f : 1.0f / W;
        tc[0] = P[i][0] * sc; tc[1] = P[i][1] * sc;
        memcpy(s_mem + POS_AT + 12u * i, P[i], 12);
        memcpy(s_mem + DIFF_AT + 4u * i, &white, 4);
        memcpy(s_mem + TC_AT + 8u * i, tc, 8);
    }
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));   /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));    /* attr 3: D3DCOLOR */
    m(0x1744, TC_AT);   m(0x1784, 2 | (2 << 4) | (8 << 8));    /* attr 9: float2 */
    m(0x1B00, tex);                          /* SET_TEXTURE_OFFSET */
    m(0x1B04, (fmt << 8) | (1u << 16) | (6u << 20) | (6u << 24));  /* FORMAT, 1 level, 64x64 */
    m(0x1B10, (uint32_t)PITCH << 16);        /* CONTROL1: pitch */
    m(0x1B1C, ((uint32_t)W << 16) | H);      /* IMAGE_RECT */
}

static void draw_quad(uint32_t va, uint32_t tex, uint32_t fmt, int sync)
{
    static const float P[6][3] = {
        { 0, 0, 0 }, { W, 0, 0 }, { 0, H, 0 }, { W, 0, 0 }, { W, H, 0 }, { 0, H, 0 },
    };

    fixed_state(tex, fmt, P, 6);
    if (sync)
        m(0x0210, va);                       /* SET_SURFACE_COLOR_OFFSET (a new epoch) */
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    m(0x1810, (5u << 24) | 0);               /* DRAW_ARRAYS 6 from 0 */
    m(0x17FC, 0);
    if (sync)
        nv2a_pb_exec_sync();
}

/* A triangle (0,0) (W,0) (0,H) on the surface at va, as draw_quad. */
static void draw_tri(uint32_t va, uint32_t tex)
{
    static const float P[3][3] = { { 0, 0, 0 }, { W, 0, 0 }, { 0, H, 0 } };

    fixed_state(tex, 0x12, P, 3);
    m(0x0210, va);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    m(0x1810, (2u << 24) | 0);               /* DRAW_ARRAYS 3 from 0 */
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

static void clear_surface_to(uint32_t va, uint32_t argb)
{
    m(0x0210, va);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x1D90, argb);                         /* SET_COLOR_CLEAR_VALUE */
    m(0x1D94, 0xF0);                         /* CLEAR_SURFACE: colour */
}

static void clear_surface_at(uint32_t va) { clear_surface_to(va, CLEAR_ARGB); }

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

    /* Taken, then drawn into again: the fresh clear survives. */
    clear_surface_at(SURF(19));              /* T */
    memset(s_mem + SURF(19), 0xAB, PITCH * H);
    clear_surface_at(SURF(21));              /* work elsewhere meanwhile */
    clear_surface_to(SURF(19), 0xFF405060u);
    draw_textured_at(SURF(20), SURF(19));    /* D */
    write_back_all();
    CHECK(surface_is(SURF(20), 0xFF405060u));

    /* Sampled, taken, written back -- all in one epoch. */
    clear_surface_at(SURF(22));              /* T */
    m(0x0210, SURF(23));                     /* D, then no epoch boundary: */
    draw_quad(SURF(23), SURF(22), 0x12, 0);  /* T looked at, sampled */
    memset(s_mem + SURF(22), 0xAB, PITCH * H);
    draw_quad(SURF(23), SURF(22), 0x0C, 0);  /* DXT1 at T: T written back first */
    nv2a_pb_exec_sync();
    CHECK(surface_is(SURF(22), 0xABABABABu));

    /* A failed readback keeps the target dirty for the next one. Every other
     * target clean first, so the one failure lands on this one. */
    write_back_all();
    clear_surface_at(SURF(16));
    nv2a_gpu_test_fail_maps = 1;
    write_back_all();
    CHECK(surface_is(SURF(16), 0));          /* the Map failed: nothing yet */
    write_back_all();
    CHECK(surface_is(SURF(16), CLEAR_ARGB));

    /* The internal scale (RECOMP_GPU_RASTER_SCALE, _MSAA; ctest runs this
     * twice): a white triangle across black, cut along the diagonal, comes
     * back to guest RAM with the edge pixels between the two -- each the
     * average of its block drawn at the higher resolution. At scale 1 a pixel
     * is either colour. The interior stays exact either way. */
    {
        const char *sc = getenv("RECOMP_GPU_RASTER_SCALE");
        int scaled = sc && atoi(sc) > 1;
        uint32_t x, y, v, mixed = 0, solid_ok = 1;
        memset(s_mem + SURF(12), 0xFF, PITCH * H);   /* a white texture */
        clear_surface_to(SURF(24), 0xFF000000u);
        draw_tri(SURF(24), SURF(12));
        write_back_all();
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                memcpy(&v, s_mem + SURF(24) + y * PITCH + 4 * x, 4);
                if ((v & 0xFFFFFF) != 0 && (v & 0xFFFFFF) != 0xFFFFFF)
                    mixed++;
                if (x + y + 2 < W && v != 0xFFFFFFFFu)      /* well inside */
                    solid_ok = 0;
                if (x + y > W + 1 && v != 0xFF000000u)       /* well outside */
                    solid_ok = 0;
            }
        printf("diagonal edge: %u pixels between black and white (scaled %d)\n",
               mixed, scaled);
        CHECK(solid_ok);
        CHECK(scaled ? mixed > W / 2 : mixed == 0);

        /* That target sampled as a texture, texel for pixel, onto D: read at
         * the high resolution, D comes down to the very bytes the target did.
         * Read at the surface's own (or from the wrong texels), the edge
         * would differ. */
        draw_textured_at(SURF(13), SURF(24));
        write_back_all();
        CHECK(memcmp(s_mem + SURF(13), s_mem + SURF(24), PITCH * H) == 0);
    }

    /* DXT1, DXT3 and DXT5 go to the GPU as their blocks (BC1/2/3) and the
     * GPU decodes them: a quad texel for pixel over random blocks -- DXT1's
     * three-colour mode with its transparent black among them -- comes back
     * within 2 per channel of the CPU's decode (d3d8_dxt_decode_texel; the
     * two round 5:6:5 endpoints and the thirds each their own way). */
    {
        static const uint32_t fmts[3] = { 0x0C, 0x0E, 0x0F };
        uint32_t k, x, y, v, want, bc0 = (uint32_t)nv2a_gpu_test_bc_uploads;
        uint32_t s = 0x9E3779B9u, tex = SURF(11);
        for (k = 0; k < 3; k++) {
            uint32_t bytes = (W / 4) * (H / 4) * d3d8_format_dxt_block_bytes(fmts[k]);
            uint32_t worst = 0, ch;
            for (x = 0; x < bytes; x++) {
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                s_mem[tex + x] = (uint8_t)s;
            }
            clear_surface_to(SURF(13), 0xFF000000u);
            draw_quad(SURF(13), tex, fmts[k], 1);
            write_back_all();
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    memcpy(&v, s_mem + SURF(13) + y * PITCH + 4 * x, 4);
                    d3d8_dxt_decode_texel(s_mem + tex, fmts[k], x, y, W, &want);
                    for (ch = 0; ch < 32; ch += 8) {
                        int d = (int)((v >> ch) & 0xFF) - (int)((want >> ch) & 0xFF);
                        if ((uint32_t)abs(d) > worst)
                            worst = (uint32_t)abs(d);
                    }
                }
            printf("DXT 0x%02X on the GPU: worst channel off by %u\n", fmts[k], worst);
            CHECK(worst <= 2);
            tex += 0x4000;
        }
        CHECK((uint32_t)nv2a_gpu_test_bc_uploads == bc0 + 3);
    }

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
