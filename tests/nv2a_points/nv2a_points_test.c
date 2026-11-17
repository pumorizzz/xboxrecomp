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
 *   - A surface whose SET_SURFACE_FORMAT type is 2 reaches the back end as
 *     swizzled: Fallout draws its character shadows into one and samples it
 *     as a swizzled texture, which a back end must read pixel for pixel.
 *   - A vertex program input with no array reads its current value, the
 *     last SET_VERTEX_DATA4F_M: mov oD0, v3 with v3's array off gives the
 *     colour written there. It used to read 0,0,0,1, which drew Fallout's
 *     green radioactive liquid opaque black.
 *   - The periodic report asks the back end for a READ sync (a download of
 *     every target it drew) only when RECOMP_FB_DUMP will write the picture.
 *     It used to ask every report, a 10-120 ms hitch every ten seconds. The
 *     capture after a drawing batch reads RECOMP_FB_DUMP once: set later, a
 *     batch asks for no download, the report still does.
 *   - An index list that names a vertex twice gets it as that vertex both
 *     times, and the next batch's vertices are its own even with the same
 *     indices. (The executor transforms such a vertex once and reuses it;
 *     with nothing that changes per run, reuse itself is not visible from
 *     here -- only that it hands back the right vertex.) Unless the program
 *     writes constants (CXT_WRITE_EN): then each named vertex runs again --
 *     add c[10], c[10], c[10] doubles c[10] per run, and oD0 = c[10] counts
 *     the runs: 2 4 8 16 32 64 for indices 0 1 2 2 1 3, where reuse would
 *     hand back 8 and 4 for the second 2 and 1.
 *   - A flip with a back end that presents the frame itself (present()
 *     returns 1) neither writes the surface back nor hands it to the window;
 *     with present() returning 0 it does both.
 *   - 300 immediate-mode quads (SET_VERTEX4F, 1200 vertices) in one
 *     BEGIN(QUADS) all reach the back end, the last one included. The
 *     executor keeps 1024 immediate vertices; past them it dropped the rest,
 *     and Fallout's credits, all its 8-bit text in one batch, lost their
 *     lower lines.
 *
 *   - A point sprite through a vertex program has texcoord v growing down the
 *     screen whatever the program's c[58] holds (it is the game's data, not a
 *     viewport: Fallout's .y changes sign with the camera, and a sign-keyed
 *     square flipped asymmetric particles with it), and a zero c[58] does not
 *     drop the point.
 *   - A fixed-function sprite without a texcoord array is textured when stage
 *     0 has a texture: the rasteriser supplies the coordinates.
 *   - 40000 points in one batch, 240000 vertices, past the 196608 a back end's
 *     buffer holds: all reach the back end, in more than one draw(). Past the
 *     cap the rest used to be dropped.
 *   - The periodic report reaches stderr in a few writes, not one per
 *     character. fbos makes stderr unbuffered, and the UCRT then formats an
 *     fprintf into a file or a pipe a character at a time, a WriteFile each:
 *     the ~5 KB report stood the executor still for 300-400 ms every ten
 *     seconds under a Start-Process redirect and 80 ms in a console. Counted
 *     by hooking WriteFile in the C runtime's import table (crt_writes.c,
 *     Windows only; the count is skipped when no entry is found); the text is
 *     read back. text_block.h, which does it, is checked byte for byte.
 *
 * Harness as tests/nv2a_near_clip. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "text_block.h"

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
static uint32_t s_window_presents;
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; s_window_presents++; }
void xbox_Nv2aFrameCounterFlip(void) {}
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_sync(void);
extern void nv2a_pb_exec_report(void);

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }

/* ---- the fake back end: every vertex it is handed ----------------------- */
#define MAX_V 64
static struct {
    uint32_t n;
    Nv2aVertexEx v[MAX_V];
    uint32_t swizzled;                  /* the surface of the last draw */
    uint32_t total;                     /* every vertex, past MAX_V too */
    uint32_t draws;                     /* draw() calls */
    uint32_t non_xf;                    /* draws whose kind is not NV2A_BATCH_XF */
    uint32_t textured_draws;            /* draws with textured set */
    uint32_t textured;                  /* Nv2aBatch.textured of the last draw */
    float    max_y;
} s_be;

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    (void)s; (void)rs; (void)flags; (void)argb; (void)zstencil;
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint32_t i;
    s_be.draws++;
    if (b->kind != NV2A_BATCH_XF)
        s_be.non_xf++;
    if (b->textured)
        s_be.textured_draws++;
    s_be.swizzled = s->swizzled;
    s_be.textured = b->textured;
    if (!b->vertices_ex)
        return;
    for (i = 0; i < b->count; i++) {
        s_be.total++;
        if (b->vertices_ex[i].y > s_be.max_y)
            s_be.max_y = b->vertices_ex[i].y;
        if (s_be.n < MAX_V)
            s_be.v[s_be.n++] = b->vertices_ex[i];
    }
}

static void be_flip(void) {}
static uint32_t s_read_syncs;
static void be_sync(uint32_t reason) { if (reason == NV2A_BACKEND_SYNC_READ) s_read_syncs++; }
static uint32_t be_report(void) { return 0; }
static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip, be_sync, be_report };

/* The same, with a present() that answers s_present_ret. */
static int s_present_ret;
static uint32_t s_presents, s_surface_syncs;
static void be_sync_surface(uint32_t va, uint32_t bytes) { (void)va; (void)bytes; s_surface_syncs++; }
static int be_present(uint32_t va, uint32_t pitch, uint32_t aa_sy)
{
    (void)va; (void)pitch; (void)aa_sy;
    s_presents++;
    return s_present_ret;
}
static const Nv2aBackend s_backend_present = {
    be_clear, be_draw, be_flip, be_sync, be_report, 0, be_sync_surface, be_present
};

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

/* Triangles by index: ARRAY_ELEMENT16, two indices a word. */
static void draw_indexed(const uint16_t *ix, uint32_t n)
{
    uint32_t i;
    s_be.n = 0;
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    for (i = 0; i + 1 < n; i += 2)
        m(0x1800, ix[i] | ((uint32_t)ix[i + 1] << 16));
    m(0x17FC, 0);
    nv2a_pb_exec_sync();
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

    /* 40000 points in one batch: 240000 vertices, past the cap of the back
     * end's buffer. (attribute 0 is still float4, the point at POS_AT.) */
    {
        uint32_t before = s_be.total, draws = s_be.draws, k;
        uint32_t non_xf = s_be.non_xf, tex = s_be.textured_draws;
        s_be.n = 0;
        m(0x17FC, 1);                                /* BEGIN_END: points */
        for (k = 0; k < 40000; k += 2)
            m(0x1800, 0);                            /* ARRAY_ELEMENT16: vertex 0, twice */
        m(0x17FC, 0);
        nv2a_pb_exec_sync();
        CHECK(s_be.total - before == 40000u * 6);
        CHECK(s_be.draws - draws >= 2);
        /* Every draw of the batch is what the batch is: an XF batch, and
         * untextured as the first (no texture on stage 0 here). A flush from
         * backend_slot() used to take the kind and textured of whatever
         * batch came before. */
        CHECK(s_be.non_xf == non_xf);
        CHECK(s_be.textured_draws == tex);
    }

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
    CHECK(s_be.swizzled == 0);

    /* The same triangle into a swizzled surface (type 2, 256 = 2^8 square). */
    m(0x0208, 0x08080208);
    draw(5, 3);
    CHECK(s_be.n == 3 && s_be.swizzled == 1);
    m(0x0208, 0x0008);

    /* Indices 0 1 2 2 1 3: the second 2 and 1 come back as themselves; the
     * next batch, same indices, positions moved by 40. */
    {
        static const float Q[4][3] = {
            { 10.0f, 10.0f, 100.0f }, { 110.0f, 10.0f, 100.0f },
            { 10.0f, 110.0f, 100.0f }, { 110.0f, 110.0f, 100.0f },
        };
        static const uint16_t IX[6] = { 0, 1, 2, 2, 1, 3 };
        float q[4][3];
        int pass, k;

        memcpy(q, Q, sizeof q);
        for (pass = 0; pass < 2; pass++) {
            put_vertices((const float (*)[3])q, 4);
            draw_indexed(IX, 6);
            CHECK(s_be.n == 6);
            for (k = 0; k < 6 && (uint32_t)k < s_be.n; k++)
                CHECK(fabsf(s_be.v[k].x - q[IX[k]][0]) < 1e-3f
                      && fabsf(s_be.v[k].y - q[IX[k]][1]) < 1e-3f);
            for (k = 0; k < 4; k++)
                q[k][0] += 40.0f;
        }
    }

    /* Program mode: mov oPos, v0 ; mov oD0, v3 (final), v3 without an array. */
    {
        static const uint32_t prog[2][4] = {
            { 0, (1u << 21) | (0u << 9), 0, 0 },
            { 0, (1u << 21) | (3u << 9), 0, 0 },
        };
        const float col[4] = { 0.0f, 0.25f, 0.0f, 0.375f };
        uint32_t w[2][4];
        int k, j;

        memcpy(w, prog, sizeof w);
        for (k = 0; k < 2; k++) {
            w[k][1] |= 0x1Bu;                            /* A = v.xyzw */
            w[k][2] |= 2u << 26;                         /* A mux: input */
            w[k][3] |= (0xFu << 12) | (1u << 11);        /* o[] xyzw */
        }
        w[1][3] |= (3u << 3) | 1u;                       /* oD0, final */
        m(0x1E9C, 0);                                    /* PROGRAM_LOAD 0 */
        for (k = 0; k < 2; k++)
            for (j = 0; j < 4; j++)
                m(0x0B00 + 4u * (uint32_t)j, w[k][j]);
        m(0x1EA0, 0);                                    /* PROGRAM_START 0 */
        m(0x1E94, 2);                                    /* TRANSFORM_EXECUTION_MODE: program */
        m(0x176C, 2 | (0 << 4) | (0 << 8));              /* attr 3: no array */
        for (k = 0; k < 4; k++)
            mf(0x1A30 + 4u * (uint32_t)k, col[k]);       /* SET_VERTEX_DATA4F_M(3) */
        put_vertices(TRI, 3);
        m(0x0338, 0);
        draw(5, 3);
        CHECK(s_be.n == 3);
        for (i = 0; i < s_be.n && i < 3; i++)
            CHECK(s_be.v[i].d0[0] == 0.0f && s_be.v[i].d0[1] == 0.25f
                  && s_be.v[i].d0[2] == 0.0f && s_be.v[i].d0[3] == 0.375f);
    }

    /* A program that writes constants: no reuse.
     *   mov oPos, v0 ; add c[10], c[10], c[10] ; mov oD0, c[10] (final) */
    {
        static const float Q[4][3] = {
            { 10.0f, 10.0f, 100.0f }, { 110.0f, 10.0f, 100.0f },
            { 10.0f, 110.0f, 100.0f }, { 110.0f, 110.0f, 100.0f },
        };
        static const uint16_t IX[6] = { 0, 1, 2, 2, 1, 3 };
        uint32_t w[3][4];
        int k, j;

        memset(w, 0, sizeof w);
        w[0][1] = (1u << 21) | (0u << 9) | 0x1Bu;            /* mov, v0.xyzw */
        w[0][2] = 2u << 26;                                  /* A: input */
        w[0][3] = (0xFu << 12) | (1u << 11);                 /* o[0] xyzw */
        w[1][1] = (3u << 21) | (10u << 13) | 0x1Bu;          /* add, c[10] */
        w[1][2] = (3u << 26) | (0x1Bu << 2);                 /* A: const, C: .xyzw */
        w[1][3] = (3u << 28) | (0xFu << 12) | (10u << 3);    /* C: const; c[10] xyzw */
        w[2][1] = (1u << 21) | (10u << 13) | 0x1Bu;          /* mov, c[10] */
        w[2][2] = 3u << 26;                                  /* A: const */
        w[2][3] = (0xFu << 12) | (1u << 11) | (3u << 3) | 1u; /* oD0 xyzw, final */
        m(0x1E9C, 0);                                        /* PROGRAM_LOAD 0 */
        for (k = 0; k < 3; k++)
            for (j = 0; j < 4; j++)
                m(0x0B00 + 4u * (uint32_t)j, w[k][j]);
        m(0x1EA0, 0);                                        /* PROGRAM_START 0 */
        m(0x1E94, 2);                                        /* program mode */
        m(0x1E98, 1);                                        /* CXT_WRITE_EN */
        m(0x1760, 2 | (3 << 4) | (12 << 8));                 /* attr 0: float3 */
        m(0x1EA4, 10);                                       /* CONSTANT_LOAD c[10] */
        for (k = 0; k < 4; k++)
            mf(0x0B80 + 4u * (uint32_t)k, 1.0f);
        put_vertices(Q, 4);
        draw_indexed(IX, 6);
        CHECK(s_be.n == 6);
        for (k = 0; k < 6 && (uint32_t)k < s_be.n; k++)
            CHECK(s_be.v[k].d0[0] == (float)(2 << k));
        m(0x1E98, 0);
        m(0x1E94, 0);                                        /* fixed function again */
    }

    /* A point sprite through a vertex program, mov oPos, v0 (final), v0 =
     * (100, 80, 50, 2) already in surface pixels -- what the XDK's appended
     * screen transform hands the rasteriser. The surface's y runs down
     * whatever the program's viewport scale c[58] holds (Fallout's c[58] is
     * data that changes with the camera), so texcoord v must still grow
     * downward: 0 at y 76, 1 at y 84. c[58].y = +1, then -1 (the XDK's
     * viewport), then c[58] = (0, 0, 1, 0): the point is drawn all the same. */
    {
        static const float V0[4] = { 100.0f, 80.0f, 50.0f, 2.0f };
        static const float C58[3][4] = {
            { 1.0f, 1.0f, 1.0f, 0.0f }, { 1.0f, -1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f },
        };
        uint32_t w[4] = { 0, (1u << 21) | (0u << 9) | 0x1Bu, 2u << 26,
                          (0xFu << 12) | (1u << 11) | 1u };   /* mov oPos, v0; final */
        int k, pass;

        m(0x1E9C, 0);                                        /* PROGRAM_LOAD 0 */
        for (k = 0; k < 4; k++)
            m(0x0B00 + 4u * (uint32_t)k, w[k]);
        m(0x1EA0, 0);                                        /* PROGRAM_START 0 */
        m(0x1E94, 2);                                        /* program mode */
        m(0x1760, 2 | (4 << 4) | (16 << 8));                 /* attr 0: float4 */
        memcpy(s_mem + POS_AT, V0, 16);
        m(0x043C, 64);                                       /* SET_POINT_SIZE 8 */
        m(0x031C, 1);                                        /* sprites */
        for (pass = 0; pass < 3; pass++) {
            m(0x1EA4, 58);                                   /* c[58]: viewport scale */
            for (k = 0; k < 4; k++)
                mf(0x0B80 + 4u * (uint32_t)k, C58[pass][k]);
            m(0x1EA4, 59);                                   /* c[59]: offset */
            for (k = 0; k < 4; k++)
                mf(0x0B80 + 4u * (uint32_t)k, 0.0f);
            draw(1, 1);
            check_point(1);
        }
        m(0x031C, 0);
        m(0x1E94, 0);                                        /* fixed function again */
        m(0x1760, 2 | (3 << 4) | (12 << 8));                 /* attr 0: float3 */
    }

    /* A fixed-function sprite with no texcoord array: its texture coordinates
     * come from the rasteriser, so with a valid stage 0 texture the batch is
     * textured. */
    {
        static const float PT4[4] = { 200.0f, 160.0f, 100.0f, 2.0f };

        m(0x1B00, 0x20000u);                                 /* stage 0 offset */
        m(0x1B04, (0x06u << 8) | (3u << 20) | (3u << 24));   /* swizzled A8R8G8B8 8x8 */
        m(0x1760, 2 | (4 << 4) | (16 << 8));                 /* attr 0: float4 */
        memcpy(s_mem + POS_AT, PT4, 16);
        m(0x043C, 64);
        m(0x031C, 1);
        draw(1, 1);
        CHECK(s_be.n == 6 && s_be.textured == 1);
        m(0x031C, 0);
        m(0x1B00, 0);
        m(0x1760, 2 | (3 << 4) | (12 << 8));                 /* attr 0: float3 */
    }

    /* 300 immediate quads, 2 px each, 20 to a row: quad k is at
     * x 10 + 3 * (k % 20), y 10 + 3 * (k / 20), so the last row (14) ends at
     * y 52 + 2 = 54; cut at 256 quads, the last would be row 12, y 48.
     * Each quad reaches the back end as two triangles, 6 vertices. */
    {
        static const float CX[4] = { 0, 2, 2, 0 }, CY[4] = { 0, 0, 2, 2 };
        int k, c;

        s_be.n = s_be.total = 0;
        s_be.max_y = 0.0f;
        m(0x17FC, 8);                                    /* BEGIN_END: quads */
        for (k = 0; k < 300; k++)
            for (c = 0; c < 4; c++) {
                mf(0x1518, 10.0f + 3.0f * (float)(k % 20) + CX[c]);   /* SET_VERTEX4F */
                mf(0x151C, 10.0f + 3.0f * (float)(k / 20) + CY[c]);
                mf(0x1520, 100.0f);
                mf(0x1524, 1.0f);
            }
        m(0x17FC, 0);
        nv2a_pb_exec_sync();
        CHECK(s_be.total == 300 * 6);
        CHECK(fabsf(s_be.max_y - 54.0f) < 1e-3f);
        CHECK(s_be.n == MAX_V && s_be.v[0].y < 13.0f);   /* row 0 first */
    }

    /* present() 1: the back end showed the frame -- no writeback, no window
     * copy. present() 0: both. */
    nv2a_backend_register(&s_backend_present);
    s_present_ret = 1;
    s_window_presents = s_surface_syncs = s_presents = 0;
    m(0x0130, 0);                                        /* FLIP_STALL */
    CHECK(s_presents == 1 && s_surface_syncs == 0 && s_window_presents == 0);
    s_present_ret = 0;
    s_window_presents = s_surface_syncs = s_presents = 0;
    m(0x0130, 0);
    CHECK(s_presents == 1 && s_surface_syncs == 1 && s_window_presents == 1);
    nv2a_backend_register(&s_backend);

    /* No dump, no download: not for the draws above, not for the report. */
    nv2a_pb_exec_report();
    CHECK(s_read_syncs == 0);
    /* A dump (into a directory that is not there) still gets one. */
#ifdef _WIN32
    _putenv("RECOMP_FB_DUMP=nv2a_points_no_such_dir/f_");
#else
    setenv("RECOMP_FB_DUMP", "nv2a_points_no_such_dir/f_", 1);
#endif
    /* A batch that draws, now that it is on: the capture after a drawing batch
     * read the variable once, at the quads above, when it was off -- it asks
     * for no download. The report still does. The quad is 40 px: a batch
     * that spans less than OBJECT_SPACE_SPAN is taken for model units, not
     * screen space, and never reaches the capture. */
    m(0x17FC, 8);                                        /* BEGIN_END: quads */
    {
        int c;
        static const float CX[4] = { 0, 40, 40, 0 }, CY[4] = { 0, 0, 40, 40 };
        for (c = 0; c < 4; c++) {
            mf(0x1518, 10.0f + CX[c]);                   /* SET_VERTEX4F */
            mf(0x151C, 10.0f + CY[c]);
            mf(0x1520, 100.0f);
            mf(0x1524, 1.0f);
        }
    }
    m(0x17FC, 0);
    nv2a_pb_exec_sync();
    CHECK(s_read_syncs == 0);
    nv2a_pb_exec_report();
    CHECK(s_read_syncs == 1);

    /* The report into an unbuffered stderr that is a
     * file, as fbos sets it (main.c setvbuf _IONBF) and as the launcher and
     * every script redirect it: a few writes, not one per character. The UCRT
     * formats into an unbuffered non-console stream one character at a time
     * (a WriteFile each), and at ~5 KB a report that was a 300 ms stall of
     * the executor every ten seconds under a Start-Process redirect. */
    {
        extern int crt_writes_hook(void);
        extern long crt_writes(void);
        long before, writes;
        int hooked = crt_writes_hook();
#ifdef _WIN32
        _putenv("RECOMP_FB_DUMP=");
#else
        unsetenv("RECOMP_FB_DUMP");
#endif
        if (freopen("nv2a_points_report.txt", "w", stderr)) {
            setvbuf(stderr, NULL, _IONBF, 0);
            before = crt_writes();
            nv2a_pb_exec_report();
            writes = crt_writes() - before;
            printf("report: %ld WriteFile calls (%d import entries hooked)\n", writes, hooked);
            if (hooked > 0)
                CHECK(writes <= 16);
            else
                printf("report: no WriteFile import entry found -- write count not checked\n");

            /* The text itself, read back: a flush that wrote nothing is
             * 0 writes too. */
            fflush(stderr);
            {
                static char text[65536];
                FILE *rf = fopen("nv2a_points_report.txt", "rb");
                size_t len = rf ? fread(text, 1, sizeof text - 1, rf) : 0;
                size_t k, lines = 0;
                const char *p_surf, *p_batch;
                if (rf)
                    fclose(rf);
                text[len] = 0;
                for (k = 0; k < len; k++)
                    if (text[k] == '\n')
                        lines++;
                printf("report: %zu bytes, %zu lines\n", len, lines);
                p_surf = strstr(text, "[GPU] surface 0x");
                p_batch = strstr(text, "[GPU] batches: ");
                CHECK(p_surf != NULL);
                CHECK(p_batch != NULL);
                CHECK(strstr(text, "blend pairs (src/dst):") != NULL);
                CHECK(p_surf && p_batch && p_surf < p_batch);
                CHECK(len > 0 && text[len - 1] == '\n');
                CHECK(lines >= 20);   /* 20 lines, 21 when the fps line shows (clock() moved) */
            }
        } else {
            CHECK(!"freopen stderr");
        }
    }

    /* text_block.h byte for byte against a stream built here, not through it:
     * 20 short lines through a 64-byte block (a flush every 8), a line longer
     * than the block (written directly after a flush), a line longer than
     * TEXT_BLOCK_LINE (cut to 1023 bytes, still ending in a newline), a tail. */
    {
        static char expect[2048], got[2048], xs[101], ys[1501];
        char small[64];
        size_t elen = 0, glen = 0, k;
        TextBlock tb;
        FILE *f = fopen("nv2a_points_block.txt", "wb");
        CHECK(f != NULL);
        if (f) {
            FILE *rf;
            text_block_init(&tb, small, sizeof small, f);
            for (k = 0; k < 20; k++)
                text_block_printf(&tb, "line %02d\n", (int)k);
            memset(xs, 'x', 100);
            text_block_printf(&tb, "%s\n", xs);
            memset(ys, 'y', 1500);
            text_block_printf(&tb, "%s", ys);
            text_block_printf(&tb, "tail\n");
            text_block_flush(&tb);
            fclose(f);

            for (k = 0; k < 20; k++)
                elen += (size_t)sprintf(expect + elen, "line %02d\n", (int)k);
            memset(expect + elen, 'x', 100); elen += 100;
            expect[elen++] = '\n';
            memset(expect + elen, 'y', 1022); elen += 1022;
            expect[elen++] = '\n';
            memcpy(expect + elen, "tail\n", 5); elen += 5;

            rf = fopen("nv2a_points_block.txt", "rb");
            if (rf) {
                glen = fread(got, 1, sizeof got, rf);
                fclose(rf);
            }
            if (glen != elen || memcmp(got, expect, elen) != 0) {
                size_t d = 0;
                while (d < glen && d < elen && got[d] == expect[d])
                    d++;
                printf("text_block: got %zu bytes, expected %zu; first difference at %zu: got 0x%02x, expected 0x%02x\n",
                       glen, elen, d, d < glen ? (unsigned char)got[d] : 0u,
                       d < elen ? (unsigned char)expect[d] : 0u);
                s_fail = 1;
            }
            CHECK(elen == 1289);
        }
    }

    nv2a_backend_register(NULL);
    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
