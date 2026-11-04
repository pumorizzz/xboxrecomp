/* Points and polygon offset reach the back end as the hardware draws them.
 *
 * Fixed-function transform with an identity composite, so a vertex's x and y
 * are surface pixels and its z is window z as given. The point's w is 2 (its
 * position is given doubled): a point's size is in surface pixels whatever
 * its depth, and an earlier version scaled it by w -- in a 3D scene, by a few
 * hundred, so steam and smoke covered the screen.
 *
 *   - A point (BEGIN_END 1) of SET_POINT_SIZE 8 px becomes a square 8 px on a
 *     side centred on the vertex. With point sprites on (POINT_SMOOTH_ENABLE)
 *     its texture coordinates run 0..1 from the top left corner; with them
 *     off it keeps the vertex's own. Points used to be dropped, and with
 *     them Fallout's steam and other particles.
 *   - With SET_POLY_OFFSET_FILL_ENABLE a triangle whose depth rises by 1 per
 *     pixel across x moves by factor * 1 + units; without it, not at all.
 *
 * Harness as tests/nv2a_near_clip. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 256
#define H 256
#define COLOR_AT 0x10000u
#define POS_AT   0x60000u
#define DIFF_AT  0x70000u
#define MEM_BYTES 0x80000u
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

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_sync(void);

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }

/* ---- the fake back end: every vertex it is handed ----------------------- */
#define MAX_V 64
static struct {
    uint32_t n;
    Nv2aVertexEx v[MAX_V];
} s_be;

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    (void)s; (void)rs; (void)flags; (void)argb; (void)zstencil;
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint32_t i;
    (void)s;
    if (!b->vertices_ex)
        return;
    for (i = 0; i < b->count && s_be.n < MAX_V; i++)
        s_be.v[s_be.n++] = b->vertices_ex[i];
}

static void be_flip(void) {}
static void be_sync(uint32_t reason) { (void)reason; }
static uint32_t be_report(void) { return 0; }
static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip, be_sync, be_report };

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static void setup(void)
{
    static const float I[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    int i;

    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, no AA */
    m(0x0200, (uint32_t)W << 16);            /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);            /* CLIP_VERTICAL */
    m(0x020C, W * 4);                        /* SET_SURFACE_PITCH */
    m(0x0210, COLOR_AT);                     /* SET_SURFACE_COLOR_OFFSET */
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, I[i]);            /* composite */
    for (i = 0; i < 4; i++)
        mf(0x0A20 + 4 * i, 0.0f);            /* SET_VIEWPORT_OFFSET */
    mf(0x0394, 0.0f);                        /* SET_CLIP_MIN */
    mf(0x0398, 16777215.0f);                 /* SET_CLIP_MAX */
    m(0x1E94, 0);                            /* TRANSFORM_EXECUTION_MODE: fixed */
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));       /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* attr 3: D3DCOLOR */
}

static void put_vertices(const float (*p)[3], int n)
{
    int i;
    for (i = 0; i < n; i++) {
        uint32_t c = 0xFFFFFFFFu;
        memcpy(s_mem + POS_AT + 12u * (uint32_t)i, p[i], 12);
        memcpy(s_mem + DIFF_AT + 4u * (uint32_t)i, &c, 4);
    }
}

static void draw(uint32_t prim, uint32_t count)
{
    s_be.n = 0;
    m(0x17FC, prim);                         /* BEGIN_END */
    m(0x1810, ((count - 1) << 24) | 0);      /* DRAW_ARRAYS from 0 */
    m(0x17FC, 0);
    nv2a_pb_exec_sync();
}

/* The point's square: 6 vertices, x 96..104, y 76..84; with sprites, each
 * corner's texture coordinates are its offset from the top left over 8. */
static void check_point(int sprite)
{
    uint32_t i;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;

    CHECK(s_be.n == 6);
    for (i = 0; i < s_be.n; i++) {
        const Nv2aVertexEx *v = &s_be.v[i];
        if (v->x < x0) x0 = v->x;
        if (v->x > x1) x1 = v->x;
        if (v->y < y0) y0 = v->y;
        if (v->y > y1) y1 = v->y;
        CHECK(fabsf(v->z - 50.0f) < 1e-3f);
        if (sprite) {
            CHECK(fabsf(v->tex[0][0] - (v->x - 96.0f) / 8.0f) < 1e-4f);
            CHECK(fabsf(v->tex[0][1] - (v->y - 76.0f) / 8.0f) < 1e-4f);
        } else {
            CHECK(v->tex[0][0] == 0.0f && v->tex[0][1] == 0.0f);
        }
    }
    CHECK(fabsf(x0 - 96.0f) < 1e-3f && fabsf(x1 - 104.0f) < 1e-3f);
    CHECK(fabsf(y0 - 76.0f) < 1e-3f && fabsf(y1 - 84.0f) < 1e-3f);
}

int main(void)
{
    static const float PT[1][4] = { { 200.0f, 160.0f, 100.0f, 2.0f } };
    static const float TRI[3][3] = {
        { 10.0f, 10.0f, 100.0f }, { 110.0f, 10.0f, 200.0f }, { 10.0f, 110.0f, 100.0f },
    };
    uint32_t i;

    nv2a_backend_register(&s_backend);
    setup();

    /* A point of 8 px (6.3 fixed point: 64), sprites on, then off. */
    memcpy(s_mem + POS_AT, PT[0], 16);
    m(0x1760, 2 | (4 << 4) | (16 << 8));     /* attr 0: float4 */
    m(0x043C, 64);                           /* SET_POINT_SIZE */
    m(0x031C, 1);                            /* SET_POINT_SMOOTH_ENABLE: sprites */
    draw(1, 1);
    check_point(1);
    m(0x031C, 0);
    draw(1, 1);
    check_point(0);

    /* Polygon offset: dz/dx = 1, dz/dy = 0, so z moves by 2 * 1 + 3. */
    m(0x1760, 2 | (3 << 4) | (12 << 8));     /* attr 0: float3 again */
    put_vertices(TRI, 3);
    mf(0x0384, 2.0f);                        /* SET_POLYGON_OFFSET_SCALE_FACTOR */
    mf(0x0388, 3.0f);                        /* SET_POLYGON_OFFSET_BIAS */
    m(0x0338, 1);                            /* SET_POLY_OFFSET_FILL_ENABLE */
    draw(5, 3);
    CHECK(s_be.n == 3);
    for (i = 0; i < s_be.n && i < 3; i++)
        CHECK(fabsf(s_be.v[i].z - (TRI[i][2] + 5.0f)) < 1e-3f);
    m(0x0338, 0);
    draw(5, 3);
    CHECK(s_be.n == 3);
    for (i = 0; i < s_be.n && i < 3; i++)
        CHECK(fabsf(s_be.v[i].z - TRI[i][2]) < 1e-3f);

    nv2a_backend_register(NULL);
    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
