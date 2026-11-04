/* The executor's fixed-function path, seen through a render back end.
 *
 * Methods go in as a title's pushbuffer would deliver them, a fake back end
 * records what the executor hands it, and the numbers are checked by hand:
 *
 *   - a composite matrix and viewport offset put a triangle where the
 *     transform says, scaled by the surface's anti-aliasing factor;
 *   - fixed-function lighting replaces the vertex colour with the light sum;
 *   - the render state carries the stencil registers, with the hardware's
 *     reset values until the title writes them;
 *   - the semaphore release lands where the title said, method 0x0310 on
 *     subchannel 5 is latched into PGRAPH 0x400B10 (in the segment scan too
 *     once a semaphore is named), NO_OPERATION 5 wakes the title's fence
 *     wait, and clears and flips reach the back end;
 *   - a batch carries every interpolant (Nv2aVertexEx) and the combiner
 *     words, the back end is asked to sync before a flip is presented, and
 *     a visibility test reports the back end's pixel count.
 *
 * Guest memory is a buffer, addressed by the offsets the title would use
 * (the executor treats a DMA offset below the image as a plain address when
 * the image bounds are 0). */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define MEM_BYTES 0x10000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;   /* #189 made the stack placement runtime state */
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
static uint32_t s_fence_va;                           /* the mirrored fence word, if any */
uint32_t xbox_Nv2aFenceWordVa(void) { return s_fence_va; }
static int s_dma_walk = 1;                           /* RECOMP_PB_WALK=dma: 0x0310 is latched */
int nv2a_pb_dma_walk(void) { return s_dma_walk; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}

static int s_wakes;                                  /* nv2a_pb_set_wake's callback ran */
static void count_wake(void) { s_wakes++; }

static volatile uint32_t s_pgraph_400b10;
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    return offset == 0x400B10u ? &s_pgraph_400b10 : NULL;
}

/* ---- the fake back end ------------------------------------------------- */
static struct {
    int draws, clears, flips;
    Nv2aSurface surf;
    Nv2aRenderState rs;
    uint32_t count;
    Nv2aVertex v[8];
    int textured;
    uint32_t clear_flags, clear_argb;
    uint32_t kind;
    int has_ex, has_rc;
    Nv2aVertexEx ex0;
    int syncs, reports;
    int flips_at_sync;          /* flips counted when sync() was first called */
    uint32_t present_va;        /* what sync_surface() was asked for last */
} s_be;

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    (void)s; (void)rs; (void)zstencil;
    s_be.clears++;
    s_be.clear_flags = flags;
    s_be.clear_argb = argb;
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint32_t i;

    s_be.draws++;
    s_be.surf = *s;
    s_be.rs = *b->state;
    s_be.count = b->count;
    s_be.textured = b->texture != NULL;
    for (i = 0; i < b->count && i < 8; i++)
        s_be.v[i] = b->vertices[i];
    s_be.kind = b->kind;
    s_be.has_ex = b->vertices_ex != NULL;
    s_be.has_rc = b->rc != NULL;
    if (b->vertices_ex)
        s_be.ex0 = b->vertices_ex[0];
}

static void be_flip(void) { s_be.flips++; }
static void be_sync(uint32_t reason)
{
    (void)reason;
    if (!s_be.syncs++)
        s_be.flips_at_sync = s_be.flips;
}
/* A count the executor cannot produce itself (it draws nothing here), and a
 * different one each call -- 7, 14, ... -- so a count CLEAR_REPORT_VALUE
 * should have discarded shows in the report. */
static uint32_t be_report(void) { return 7u * (uint32_t)++s_be.reports; }

static void be_sync_surface(uint32_t va, uint32_t bytes) { (void)bytes; s_be.present_va = va; }

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip,
                                       be_sync, be_report, 0, be_sync_surface };

/* ---- driving the executor ---------------------------------------------- */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)
static int near_(float a, float b) { return fabsf(a - b) < 1e-3f; }

int main(void)
{
    uint32_t i;

    nv2a_backend_register(&s_backend);

    /* A 640x480 surface at 2x2 anti-aliasing: SET_SURFACE_CLIP is in logical
     * pixels, the real surface is 1280x960 with a 5120-byte pitch. */
    m(0x0208, 0x2008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, AA 2x2 */
    m(0x0200, 640u << 16);                   /* CLIP_HORIZONTAL: x 0, width 640 */
    m(0x0204, 480u << 16);                   /* CLIP_VERTICAL */
    m(0x020C, 5120);                         /* SET_SURFACE_PITCH */
    m(0x0210, 0x8000);                       /* SET_SURFACE_COLOR_OFFSET */

    /* Composite matrix: viewport scale for a 640x480 logical surface, y down.
     * clip = (320 x + 320, -240 y + 240, z, 1). */
    {
        const float M[16] = { 320, 0, 0, 320,
                              0, -240, 0, 240,
                              0, 0, 1, 0,
                              0, 0, 0, 1 };
        for (i = 0; i < 16; i++)
            mf(0x0680 + 4 * i, M[i]);
    }
    m(0x1E94, 0);                            /* TRANSFORM_EXECUTION_MODE: fixed */

    /* One triangle: float3 positions, D3DCOLOR diffuse, float3 normal. */
    putf(0x2000, -1); putf(0x2004, -1); putf(0x2008, 0);
    putf(0x200C,  1); putf(0x2010, -1); putf(0x2014, 0);
    putf(0x2018,  0); putf(0x201C,  1); putf(0x2020, 0);
    for (i = 0; i < 3; i++) {
        put32(0x3000 + 4 * i, 0xFF804020u);
        putf(0x4000 + 12 * i, 0); putf(0x4004 + 12 * i, 0); putf(0x4008 + 12 * i, 1);
    }
    m(0x1720, 0x2000); m(0x1760, 2 | (3 << 4) | (12 << 8));        /* attr 0: float3 */
    m(0x172C, 0x3000); m(0x176C, 0 | (4 << 4) | (4 << 8));         /* attr 3: D3DCOLOR */
    m(0x1728, 0x4000); m(0x1768, 2 | (3 << 4) | (12 << 8));        /* attr 2: float3 */

    /* Stencil: untouched, the hardware's reset values reach the back end. */
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    m(0x1810, 2u << 24);                     /* DRAW_ARRAYS: start 0, 3 vertices */
    m(0x17FC, 0);

    CHECK(s_be.draws == 1);
    CHECK(s_be.count == 3);
    CHECK(s_be.surf.aa_sx == 2 && s_be.surf.aa_sy == 2);
    CHECK(s_be.surf.width == 1280 && s_be.surf.height == 960);
    CHECK(s_be.surf.pitch == 5120 && s_be.surf.bytes_per_pixel == 4);
    CHECK(!s_be.textured);
    /* (-1,-1) -> (0, 480) -> x2 = (0, 960); (1,-1) -> (640, 480) -> (1280, 960);
     * (0, 1) -> (320, 0) -> (640, 0). */
    CHECK(near_(s_be.v[0].x, 0)    && near_(s_be.v[0].y, 960));
    CHECK(near_(s_be.v[1].x, 1280) && near_(s_be.v[1].y, 960));
    CHECK(near_(s_be.v[2].x, 640)  && near_(s_be.v[2].y, 0));
    CHECK(near_(s_be.v[0].rhw, 1.0f));
    CHECK(s_be.v[0].diffuse == 0xFF804020u);          /* the vertex colour, unlit */
    CHECK(s_be.rs.stencil_func == 0x207 && s_be.rs.stencil_func_mask == 0xFF);
    CHECK(s_be.rs.stencil_write_mask == 0xFF);
    CHECK(s_be.rs.stencil_fail == 0x1E00 && s_be.rs.stencil_zfail == 0x1E00
       && s_be.rs.stencil_zpass == 0x1E00);
    CHECK(s_be.rs.stencil_enable == 0);
    /* Every interpolant arrives alongside: a transformed batch, with the
     * combiner words and the unclamped diffuse colour as floats. */
    CHECK(s_be.kind == NV2A_BATCH_XF);
    CHECK(s_be.has_ex && s_be.has_rc);
    CHECK(near_(s_be.ex0.d0[0], 128.0f / 255.0f) && near_(s_be.ex0.d0[1], 64.0f / 255.0f)
       && near_(s_be.ex0.d0[2], 32.0f / 255.0f) && near_(s_be.ex0.d0[3], 1.0f));

    /* The viewport offset (c[59]) is added after the divide, then scaled. */
    mf(0x0A20, 10.0f);
    m(0x17FC, 5); m(0x1810, 2u << 24); m(0x17FC, 0);
    CHECK(s_be.draws == 2);
    CHECK(near_(s_be.v[0].x, 20.0f));                 /* (0 + 10) * 2 */
    mf(0x0A20, 0.0f);

    /* Stencil state written by the title arrives as written. */
    m(0x032C, 1);                                     /* SET_STENCIL_TEST_ENABLE */
    m(0x0364, 0x206);                                 /* FUNC: GL_GEQUAL */
    m(0x0368, 5);                                     /* FUNC_REF */
    m(0x0378, 0x1E02);                                /* OP_ZPASS: GL_INCR */
    m(0x17FC, 5); m(0x1810, 2u << 24); m(0x17FC, 0);
    CHECK(s_be.rs.stencil_enable == 1 && s_be.rs.stencil_func == 0x206);
    CHECK(s_be.rs.stencil_ref == 5 && s_be.rs.stencil_zpass == 0x1E02);

    /* Lighting. The triangle's normal is +z and the model-view matrix is the
     * identity, so vertex 0, at (-1, -1, 0), sees both lights head on (N.L = 1).
     * Every term stays below 1, so none of them hides in the clamp:
     *   light 0, infinite, diffuse r 0.5, with scene ambient r 0.1: r = 0.6;
     *   light 1, local at (-1, -1, 2), distance 2, diffuse g 0.8, attenuation
     *   1 / (1 + 0.25 d^2) = 1/2: g = 0.4;
     * alpha comes from the material. */
    {
        const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        for (i = 0; i < 16; i++)
            mf(0x0480 + 4 * i, I[i]);                 /* model-view */
    }
    m(0x0314, 1);                                     /* SET_LIGHTING_ENABLE */
    m(0x03BC, 1 | (2u << 2));                         /* light 0 infinite, light 1 local */
    mf(0x1000 + 0x0C, 0.5f);                          /* light 0 diffuse r */
    mf(0x1000 + 0x34 + 8, 1.0f);                      /* light 0 direction z */
    mf(0x1080 + 0x0C + 4, 0.8f);                      /* light 1 diffuse g */
    mf(0x1080 + 0x24, 10.0f);                         /* light 1 range */
    mf(0x1080 + 0x5C, -1.0f); mf(0x1080 + 0x60, -1.0f); mf(0x1080 + 0x64, 2.0f);
    mf(0x1080 + 0x68, 1.0f);                          /* constant attenuation */
    mf(0x1080 + 0x6C, 0.0f);                          /* linear */
    mf(0x1080 + 0x70, 0.25f);                         /* quadratic */
    mf(0x0A10, 0.1f);                                 /* scene ambient r */
    mf(0x03B4, 1.0f);                                 /* material alpha */
    m(0x17FC, 5); m(0x1810, 2u << 24); m(0x17FC, 0);
    CHECK(s_be.v[0].diffuse == 0xFF996600u);          /* (153, 102, 0) = (0.6, 0.4, 0) */
    mf(0x0A10, 0.0f);                                 /* without the ambient ... */
    m(0x17FC, 5); m(0x1810, 2u << 24); m(0x17FC, 0);
    CHECK(s_be.v[0].diffuse != 0xFF996600u);         /* ... the colour moves */
    mf(0x1080 + 0x24, 1.0f);                          /* light 1 out of range: only light 0 */
    m(0x17FC, 5); m(0x1810, 2u << 24); m(0x17FC, 0);
    CHECK((s_be.v[0].diffuse & 0x0000FF00u) == 0);
    m(0x0314, 0);

    /* Anti-aliasing off: the same transform lands unscaled. */
    m(0x0208, 0x0008);
    m(0x17FC, 5); m(0x1810, 2u << 24); m(0x17FC, 0);
    CHECK(s_be.surf.aa_sx == 1 && s_be.surf.width == 640);
    CHECK(near_(s_be.v[1].x, 640.0f) && near_(s_be.v[1].y, 480.0f));

    /* A screen-space batch (immediate mode: pre-transformed positions) is
     * handed over as NV2A_BATCH_SCREEN, its Ex vertices carrying the vertex
     * colour unpacked and the texel UVs as (u, v, 0, 1). Linear A8R8G8B8 on
     * stage 0, so the UVs are not scaled by a texture size. */
    m(0x1B04, 0x12u << 8);                            /* SET_TEXTURE_FORMAT: LIN_A8R8G8B8 */
    m(0x17FC, 5);
    {
        static const float pos[3][2] = { { 10, 10 }, { 200, 10 }, { 10, 200 } };
        for (i = 0; i < 3; i++) {
            mf(0x1A30, 0.5f); mf(0x1A34, 0.25f);      /* attr 3 (diffuse), r g */
            mf(0x1A38, 0.125f); mf(0x1A3C, 1.0f);     /* b a */
            mf(0x18C8, 3.0f + (float)i);              /* attr 9 (texcoord) u */
            mf(0x18CC, 4.0f);                         /* v */
            mf(0x1518, pos[i][0]); mf(0x151C, pos[i][1]);
            mf(0x1520, 0.0f); mf(0x1524, 1.0f);       /* w: the vertex is complete */
        }
    }
    m(0x17FC, 0);
    {
        /* vertex_color truncates: 0.5 -> 127, 0.25 -> 63, 0.125 -> 31. */
        Nv2aVertexEx expected, actual = s_be.ex0;
        memset(&expected, 0, sizeof expected);
        expected.x = 10.0f; expected.y = 10.0f; expected.z = 0.0f; expected.rhw = 1.0f;
        expected.d0[0] = 127.0f / 255.0f; expected.d0[1] = 63.0f / 255.0f;
        expected.d0[2] = 31.0f / 255.0f;  expected.d0[3] = 1.0f;
        expected.fog = 1.0f;
        expected.tex[0][0] = 3.0f; expected.tex[0][1] = 4.0f; expected.tex[0][3] = 1.0f;
        CHECK(s_be.kind == NV2A_BATCH_SCREEN);
        CHECK(memcmp(&expected, &actual, sizeof expected) == 0);
    }

    /* A garbage index (the walk is 32 bits wide now) must not reach outside
     * guest memory: start 0xFFFFFF at a 12-byte stride is 200 MB past the
     * array, and the batch is dropped instead of read. The test buffer is
     * 64 KB, so reading it would fault. */
    {
        int draws = s_be.draws;

        m(0x17FC, 5); m(0x1810, 0xFFFFFFu | (2u << 24)); m(0x17FC, 0);
        CHECK(s_be.draws == draws);
    }

    /* The semaphore release lands where the title said. */
    nv2a_pb_set_semaphore_target(0x5000);
    m(0x1D70, 0x1234);
    {
        uint32_t got;
        memcpy(&got, s_mem + 0x5000, 4);
        CHECK(got == 0x1234);
    }

    /* Method 0x0310 on subchannel 5 is latched into PGRAPH 0x400B10; on the
     * 3D subchannel it is SET_DITHER_ENABLE and is not. */
    nv2a_pb_exec_method(5, 0x0310, 0xABCD1234u);
    CHECK(s_pgraph_400b10 == 0xABCD1234u);
    nv2a_pb_exec_method(0, 0x0310, 7);
    CHECK(s_pgraph_400b10 == 0xABCD1234u);

    /* A project that mirrors the fence (no back end names the semaphore): the
     * latch keeps the marker's address and wrap bits but reports the fence bits
     * (2..6) of the mirrored word, so D3D's compare of the two comes out equal. */
    nv2a_pb_set_semaphore_target(0);
    s_fence_va = 0x6000;
    putf(0x6000, 0);
    put32(0x6000, 0x0002D42Fu);                       /* the mirrored word: low 5 bits 0x0F */
    nv2a_pb_exec_method(5, 0x0310, 0xABCD1234u | 0x7Cu);
    CHECK(((s_pgraph_400b10 >> 2) & 0x1Fu) == 0x0Fu);
    CHECK((s_pgraph_400b10 & ~0x7Cu) == (0xABCD1234u & ~0x7Cu));
    s_fence_va = 0;                                   /* no mirror: the marker as it is */
    nv2a_pb_exec_method(5, 0x0310, 0x11111111u);
    CHECK(s_pgraph_400b10 == 0x11111111u);
    nv2a_pb_set_semaphore_target(0x5000);

    /* The segment scan latches only for a named semaphore, and then the marker
     * as it is: the release that follows it in the kick tells the same story. */
    s_dma_walk = 0;
    nv2a_pb_set_semaphore_target(0);
    nv2a_pb_exec_method(5, 0x0310, 0x22222222u);
    CHECK(s_pgraph_400b10 == 0x11111111u);
    nv2a_pb_set_semaphore_target(0x5000);
    nv2a_pb_exec_method(5, 0x0310, 0x0004D3E7u);
    CHECK(s_pgraph_400b10 == 0x0004D3E7u);
    s_dma_walk = 1;

    /* NO_OPERATION 5 is the software method D3D's BlockOnFence sleeps on:
     * the wake runs there, and for no other value. */
    s_wakes = 0;
    m(0x0100, 0);
    m(0x0100, 4);
    CHECK(s_wakes == 0);
    nv2a_pb_set_wake(count_wake);
    m(0x0100, 5);
    CHECK(s_wakes == 1);
    nv2a_pb_set_wake(NULL);
    m(0x0100, 5);
    CHECK(s_wakes == 1);

    /* Clears and flips reach the back end. */
    m(0x1D90, 0xFF102030u);                           /* SET_COLOR_CLEAR_VALUE */
    m(0x1D94, 0xF3);                                  /* CLEAR_SURFACE */
    CHECK(s_be.clears == 1 && s_be.clear_flags == 0xF3 && s_be.clear_argb == 0xFF102030u);
    CHECK(s_be.flips == 0);
    s_be.syncs = 0;
    m(0x0130, 0);                                     /* FLIP_STALL */
    CHECK(s_be.flips == 1);
    {
        /* Written back before the flip reaches the back end (and before the
         * executor presents guest RAM): the first sync saw no flip yet. */
        int expected = 0, actual = s_be.syncs ? s_be.flips_at_sync : -1;
        CHECK(actual == expected);
    }
    {
        /* The surface the flip presents is the one written back for it. */
        uint32_t expected = 0x8000, actual = s_be.present_va;
        CHECK(actual == expected);
    }

    /* A visibility test's pixel count comes from the back end: what it
     * counted before CLEAR_REPORT_VALUE (7) is dropped, the next call's 14
     * is reported. */
    m(0x17C8, 0);                                     /* CLEAR_REPORT_VALUE */
    m(0x17D0, 0x7000);                                /* GET_REPORT */
    {
        uint32_t expected = 14, actual;
        memcpy(&actual, s_mem + 0x7000 + 8, 4);
        CHECK(actual == expected);
    }

    if (!s_fail)
        printf("nv2a fixed-function path through a back end: ok\n");
    return s_fail;
}
