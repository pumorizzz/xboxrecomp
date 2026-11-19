/* A batch's vertices run on several threads come out exactly as on one.
 *
 * The executor shares a batch's vertex-program runs out to helper threads
 * (RECOMP_VSH_THREADS); a run reads only its attributes, the program and the
 * constants, so every vertex the back end gets must be the same bit for bit
 * as with one thread:
 *
 *   - An indexed grid (1600 vertices, 9126 indices, most named several
 *     times) through mov oPos, v0 ; mul R0, v0, c[10] ; mad oD0, R0, R0,
 *     c[12]: with 8 threads every vertex equals the 1-thread vertex, oD0 is
 *     (v0 * c10)^2 + c12, and the batch did run on several threads.
 *   - The same with flush-to-zero and denormals-are-zero set on the thread
 *     that runs the executor and c[10] tiny, so the squares are denormal: a
 *     helper rounds as the executor does, giving 0, not a denormal.
 *   - A program that writes constants (CXT_WRITE_EN): add c[10], c[10],
 *     c[1] ; mov oD0, c[10] counts the runs, so 600 vertices drawn in order
 *     come out 1, 2, 3 ... 600 even with 8 threads -- they run one by one.
 *   - The executor's own cost line counts batches by the unique vertices
 *     they ran (<=32 <=64 <=128 <=256 <=512 >512): a batch on 42 unique
 *     vertices lands in the <=64 bucket, one on 280 in the <=512 one
 *     (nv2a_pb_exec_vsh_buckets).
 *   - A small batch is shared out too (RECOMP_VSH_MIN_PARALLEL, here set
 *     with nv2a_pb_exec_set_vsh_min_parallel): 42 unique vertices, under the
 *     old threshold of 256, come out bit for bit as with one thread (t1), also
 *     with flush-to-zero and denormals (t2); "more than n" is shared and n is
 *     not (t3); the thread counts 1 and 8 are compared after another batch
 *     has been drawn, so a vertex left out would keep its old value.
 *   - The default threshold (no RECOMP_VSH_MIN_PARALLEL, set(-1)) shares out a
 *     batch of about 100 unique vertices, between 64 and 256 (t10): bit for
 *     bit as with one thread, and shared (nv2a_pb_exec_vsh_threaded grows).
 *   - The back end's form of a vertex (xf_to_backend) is made by the thread
 *     that ran the vertex, once for each different vertex; both Nv2aBatch
 *     arrays, vertices and vertices_ex, must equal the 1-thread ones, field
 *     for field, and the cache must be used (nv2a_pb_exec_xb_hits): a grid
 *     with repeated indices (t4), a texture whose size is not 1 so u and v
 *     scale (t5), fog (t6), the polygon offset, which draws copies (t7),
 *     triangles through the near plane, which are clipped into copies (t8),
 *     strips and fans (t9).
 *
 * Harness as tests/nv2a_points. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
#include "nv2a_backend.h"

#define W 256
#define H 256
#define GRID 40
#define NV (GRID * GRID)
#define NI ((GRID - 1) * (GRID - 1) * 6)
#define COLOR_AT 0x10000u
#define POS_AT   0x60000u
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
extern void nv2a_pb_exec_set_vsh_threads(int n);
extern uint32_t nv2a_pb_exec_vsh_threaded(void);
extern void nv2a_pb_exec_vsh_buckets(uint32_t out[12]);
extern void nv2a_pb_exec_set_vsh_min_parallel(int n);
extern uint32_t nv2a_pb_exec_xb_hits(void);

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }

/* ---- the fake back end: every vertex it is handed ----------------------- */
#define MAX_V (NI + 64)
static struct {
    uint32_t n;
    Nv2aVertexEx *v;
    Nv2aVertex *v1;             /* the same vertices as Nv2aBatch.vertices has them */
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
    for (i = 0; i < b->count && s_be.n < MAX_V; i++) {
        s_be.v1[s_be.n] = b->vertices[i];
        s_be.v[s_be.n++] = b->vertices_ex[i];
    }
}

static void be_flip(void) {}
static void be_sync(uint32_t reason) { (void)reason; }
static uint32_t be_report(void) { return 0; }
static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip, be_sync, be_report };

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static uint16_t s_ix[NI];

static void setup(void)
{
    int x, y, k = 0;

    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, no AA */
    m(0x0200, (uint32_t)W << 16);            /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);            /* CLIP_VERTICAL */
    m(0x020C, W * 4);                        /* SET_SURFACE_PITCH */
    m(0x0210, COLOR_AT);                     /* SET_SURFACE_COLOR_OFFSET */
    mf(0x0394, 0.0f);                        /* SET_CLIP_MIN */
    mf(0x0398, 16777215.0f);                 /* SET_CLIP_MAX */
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));       /* attr 0: float3 */

    /* A grid of vertices 6 px apart, z 100; two triangles a cell. */
    for (y = 0; y < GRID; y++)
        for (x = 0; x < GRID; x++) {
            float p[3];
            p[0] = 5.0f + 6.0f * (float)x + 0.37f * (float)y;
            p[1] = 5.0f + 6.0f * (float)y + 0.11f * (float)x;
            p[2] = 100.0f + (float)(x * y) * 0.01f;
            memcpy(s_mem + POS_AT + 12u * (uint32_t)(y * GRID + x), p, 12);
        }
    for (y = 0; y + 1 < GRID; y++)
        for (x = 0; x + 1 < GRID; x++) {
            uint16_t a = (uint16_t)(y * GRID + x), b = (uint16_t)(a + 1);
            uint16_t c = (uint16_t)(a + GRID), d = (uint16_t)(c + 1);
            s_ix[k++] = a; s_ix[k++] = b; s_ix[k++] = c;
            s_ix[k++] = c; s_ix[k++] = b; s_ix[k++] = d;
        }
}

static void load_program(const uint32_t (*w)[4], int n)
{
    int k, j;
    m(0x1E9C, 0);                                    /* PROGRAM_LOAD 0 */
    for (k = 0; k < n; k++)
        for (j = 0; j < 4; j++)
            m(0x0B00 + 4u * (uint32_t)j, w[k][j]);
    m(0x1EA0, 0);                                    /* PROGRAM_START 0 */
    m(0x1E94, 2);                                    /* TRANSFORM_EXECUTION_MODE: program */
}

static void set_const(uint32_t i, float x, float y, float z, float w)
{
    m(0x1EA4, i);                                    /* CONSTANT_LOAD */
    mf(0x0B80, x); mf(0x0B84, y); mf(0x0B88, z); mf(0x0B8C, w);
}

/* The grid as one indexed triangle batch: ARRAY_ELEMENT16, two a word. */
static void draw_grid(void)
{
    uint32_t i;
    s_be.n = 0;
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    for (i = 0; i + 1 < NI; i += 2)
        m(0x1800, s_ix[i] | ((uint32_t)s_ix[i + 1] << 16));
    m(0x17FC, 0);
    nv2a_pb_exec_sync();
}

/* The grid drawn with 1 thread, then with 8: the same vertices, bit for bit,
 * and the 8-thread run did share the work out. In between the grid is drawn
 * once more with c[12] = 99, so a vertex the 8-thread run left out would
 * keep that run's colour rather than happen to match. */
static void grid_same(Nv2aVertexEx *one, float c10, float c12)
{
    uint32_t before, i;

    nv2a_pb_exec_set_vsh_threads(1);
    draw_grid();
    CHECK(s_be.n == NI);
    memcpy(one, s_be.v, sizeof *one * NI);
    set_const(12, 99.0f, 99.0f, 99.0f, 99.0f);
    draw_grid();
    set_const(12, c12, c12 * 0.5f, 0.0f, 0.0f);
    before = nv2a_pb_exec_vsh_threaded();
    nv2a_pb_exec_set_vsh_threads(8);
    draw_grid();
    CHECK(s_be.n == NI);
    CHECK(nv2a_pb_exec_vsh_threaded() > before);
    CHECK(memcmp(one, s_be.v, sizeof *one * NI) == 0);
    for (i = 0; i < s_be.n; i++) {
        const float *p = (const float *)(s_mem + POS_AT + 12u * s_ix[i]);
        float r = p[0] * c10, e = r * r + c12;
        if (!(s_be.v[i].x == p[0] && s_be.v[i].d0[0] == e)) {
            CHECK(s_be.v[i].x == p[0] && s_be.v[i].d0[0] == e);
            break;
        }
    }
}

/* The first `ni` indices of the grid as a batch; how many different vertices
 * they name. */
static uint32_t draw_prefix(uint32_t ni)
{
    static uint8_t seen[NV];
    uint32_t i, uniq = 0;

    memset(seen, 0, sizeof seen);
    for (i = 0; i < ni; i++)
        if (!seen[s_ix[i]]) {
            seen[s_ix[i]] = 1;
            uniq++;
        }
    s_be.n = 0;
    m(0x17FC, 5);
    for (i = 0; i + 1 < ni; i += 2)
        m(0x1800, s_ix[i] | ((uint32_t)s_ix[i + 1] << 16));
    m(0x17FC, 0);
    nv2a_pb_exec_sync();
    return uniq;
}

/* A batch of 42 unique vertices and one of 280: each is one batch and its
 * vertices in the bucket for its size, and nothing in the others. */
static void buckets(void)
{
    static const uint32_t ni[2] = { 120, 1404 };
    static const int want[2] = { 1, 4 };
    int k, b;

    nv2a_pb_exec_set_vsh_threads(8);
    for (k = 0; k < 2; k++) {
        uint32_t a[12], z[12], uniq;
        nv2a_pb_exec_vsh_buckets(a);
        uniq = draw_prefix(ni[k]);
        nv2a_pb_exec_vsh_buckets(z);
        CHECK(uniq == (k ? 280u : 42u));
        CHECK(s_be.n == ni[k]);
        for (b = 0; b < 6; b++) {
            CHECK(z[b] - a[b] == (b == want[k] ? 1u : 0u));
            CHECK(z[6 + b] - a[6 + b] == (b == want[k] ? uniq : 0u));
        }
    }
}

/* The first `ni` indices drawn with 1 thread and with 8 under the threshold
 * `min_par`: the same vertices bit for bit, and shared out (or not) as
 * `want_threaded` says. In between another batch is drawn with c[12] = 99,
 * so a vertex the threaded run left out would keep the old colour rather
 * than happen to match. */
static void prefix_same(uint32_t ni, int min_par, float c10, float c12, int want_threaded)
{
    static Nv2aVertexEx one[NI];
    uint32_t before, after, uniq;

    nv2a_pb_exec_set_vsh_min_parallel(min_par);
    nv2a_pb_exec_set_vsh_threads(1);
    draw_prefix(ni);
    CHECK(s_be.n == ni);
    memcpy(one, s_be.v, sizeof *one * ni);
    set_const(12, 99.0f, 99.0f, 99.0f, 99.0f);
    draw_grid();
    set_const(12, c12, c12 * 0.5f, 0.0f, 0.0f);
    before = nv2a_pb_exec_vsh_threaded();
    nv2a_pb_exec_set_vsh_threads(8);
    uniq = draw_prefix(ni);
    after = nv2a_pb_exec_vsh_threaded();
    CHECK(s_be.n == ni);
    CHECK(want_threaded ? after - before == uniq : after == before);
    CHECK(memcmp(one, s_be.v, sizeof *one * ni) == 0);
    (void)c10;
}

/* Both arrays of the fake back end equal, field for field (the structs have padding). */
static int same_vertices(const Nv2aVertexEx *a, const Nv2aVertex *a1,
                         const Nv2aVertexEx *b, const Nv2aVertex *b1, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (memcmp(&a[i], &b[i], sizeof a[i]) != 0
            || a1[i].x != b1[i].x || a1[i].y != b1[i].y || a1[i].z != b1[i].z
            || a1[i].rhw != b1[i].rhw || a1[i].diffuse != b1[i].diffuse
            || a1[i].u != b1[i].u || a1[i].v != b1[i].v) {
            printf("  vertex %u differs\n", i);
            return 0;
        }
    }
    return 1;
}

static void draw_prim(uint32_t prim, uint32_t ni)
{
    uint32_t i;
    s_be.n = 0;
    m(0x17FC, prim);
    for (i = 0; i + 1 < ni; i += 2)
        m(0x1800, s_ix[i] | ((uint32_t)s_ix[i + 1] << 16));
    m(0x17FC, 0);
    nv2a_pb_exec_sync();
}

static void d_grid(void) { draw_grid(); }
static void d_tris(void) { draw_prim(5, 1404); }
static void d_strip(void) { draw_prim(6, 400); }
static void d_fan(void) { draw_prim(7, 400); }

/* attribute 0 as float4 with a w that is almost 0 for some vertices */
#define POS4_AT 0x68000u
static void d_near(void)
{
    int i;
    for (i = 0; i < NV; i++) {
        float p[4];
        memcpy(p, s_mem + POS_AT + 12u * (uint32_t)i, 12);
        p[3] = (i % 5 == 0) ? 0.0005f : 1.0f;
        memcpy(s_mem + POS4_AT + 16u * (uint32_t)i, p, 16);
    }
    m(0x1720, POS4_AT); m(0x1760, 2 | (4 << 4) | (16 << 8));      /* attr 0: float4 */
    draw_prim(5, 1404);
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));      /* back to float3 */
}

/* `draw` with 1 thread, then with 8: both arrays the same; want_hits: the
 * cache supplied corners. */
static void same_both(void (*draw)(void), const char *what, int want_hits)
{
    static Nv2aVertexEx one[MAX_V];
    static Nv2aVertex one1[MAX_V];
    uint32_t n1, hits;

    nv2a_pb_exec_set_vsh_threads(1);
    draw();
    n1 = s_be.n;
    memcpy(one, s_be.v, sizeof *one * n1);
    memcpy(one1, s_be.v1, sizeof *one1 * n1);
    set_const(12, 99.0f, 99.0f, 99.0f, 99.0f);          /* another batch in between */
    draw_grid();
    set_const(12, 0.25f, 0.125f, 0.0f, 0.0f);
    nv2a_pb_exec_set_vsh_threads(8);
    hits = nv2a_pb_exec_xb_hits();
    draw();
    if (s_be.n != n1 || n1 == 0 || !same_vertices(one, one1, s_be.v, s_be.v1, n1)) {
        printf("FAIL: %s: %u vertices with 1 thread, %u with 8, or they differ\n", what, n1, s_be.n);
        s_fail = 1;
    }
    if (want_hits && nv2a_pb_exec_xb_hits() == hits) {
        printf("FAIL: %s: no corner came from the cache\n", what);
        s_fail = 1;
    }
}

int main(void)
{
    static Nv2aVertexEx one[NI];
    uint32_t w[3][4];
    unsigned int csr = _mm_getcsr();
    int k;

    s_be.v = (Nv2aVertexEx *)calloc(MAX_V, sizeof *s_be.v);
    s_be.v1 = (Nv2aVertex *)calloc(MAX_V, sizeof *s_be.v1);
    if (!s_be.v || !s_be.v1)
        return 1;
    nv2a_backend_register(&s_backend);
    setup();

    /* mov oPos, v0 ; mul R0, v0, c[10] ; mad oD0, R0, R0, c[12] (final) */
    memset(w, 0, sizeof w);
    w[0][1] = (1u << 21) | (0u << 9) | 0x1Bu;                 /* mov, v0.xyzw */
    w[0][2] = 2u << 26;                                       /* A: input */
    w[0][3] = (0xFu << 12) | (1u << 11);                      /* o[0] xyzw */
    w[1][1] = (2u << 21) | (10u << 13) | (0u << 9) | 0x1Bu;   /* mul, c[10], v0 */
    w[1][2] = (2u << 26) | (0x1Bu << 17) | (3u << 11);        /* A: input, B: const */
    w[1][3] = (0xFu << 24) | (0u << 20);                      /* R0 xyzw */
    w[2][1] = (4u << 21) | (12u << 13) | 0x1Bu;               /* mad, c[12] */
    w[2][2] = (1u << 26) | (0x1Bu << 17) | (1u << 11) | (0x1Bu << 2); /* A, B: R0 */
    w[2][3] = (3u << 28) | (0xFu << 12) | (1u << 11) | (3u << 3) | 1u; /* C: const; oD0, final */
    load_program((const uint32_t (*)[4])w, 3);
    set_const(10, 0.013f, 0.007f, 0.003f, 0.5f);
    set_const(12, 0.25f, 0.125f, 0.0f, 0.0f);
    grid_same(one, 0.013f, 0.25f);
    buckets();
    /* (t1) 42 unique vertices, under the old threshold of 256: shared out and the same. */
    prefix_same(120, 16, 0.013f, 0.25f, 1);
    /* (t3) the threshold itself: "more than n" is shared, n is not. */
    prefix_same(120, 42, 0.013f, 0.25f, 0);
    prefix_same(120, 41, 0.013f, 0.25f, 1);
    nv2a_pb_exec_set_vsh_min_parallel(-1);
    /* (t10) the default threshold: a batch of ~100 unique vertices (65..256) is
     * shared out; with the old default of 256 it would run on one thread. */
    {
        uint32_t ni = 120, uniq = draw_prefix(ni);
        while (uniq < 100 && ni + 2 <= NI) {
            ni += 2;
            uniq = draw_prefix(ni);
        }
        CHECK(uniq >= 65 && uniq <= 256);
        prefix_same(ni, -1, 0.013f, 0.25f, 1);
    }
    /* (t4) repeated indices: both arrays, from the cache. */
    same_both(d_grid, "t4 grid with repeated indices", 1);

    /* Flush to zero and denormals as zero on this thread; c[10] tiny, so
     * (v0 * c10)^2 is denormal and flushes to 0 -- on every thread. */
    _mm_setcsr(csr | 0x8040u);
    set_const(10, 1e-23f, 1e-23f, 1e-23f, 1e-23f);
    set_const(12, 0.0f, 0.0f, 0.0f, 0.0f);
    grid_same(one, 1e-23f, 0.0f);
    CHECK(s_be.n > 0 && s_be.v[s_be.n - 1].d0[0] == 0.0f);
    /* (t2) the same on a small batch: a helper rounds as the executor does. */
    prefix_same(120, 16, 1e-23f, 0.0f, 1);
    CHECK(s_be.n == 120 && s_be.v[s_be.n - 1].d0[0] == 0.0f);
    nv2a_pb_exec_set_vsh_min_parallel(-1);
    _mm_setcsr(csr);

    /* A program that writes constants runs its vertices one by one, in order,
     * each index as often as it is named:
     *   mov oPos, v0 ; add c[10], c[10], v1 ; mov oD0, c[10] (final)
     * with v1 = 1 (no array, SET_VERTEX_DATA4F_M) and c[10] from 0. */
    memset(w, 0, sizeof w);
    w[0][1] = (1u << 21) | (0u << 9) | 0x1Bu;                 /* mov, v0.xyzw */
    w[0][2] = 2u << 26;                                       /* A: input */
    w[0][3] = (0xFu << 12) | (1u << 11);                      /* o[0] xyzw */
    w[1][1] = (3u << 21) | (10u << 13) | (1u << 9) | 0x1Bu;   /* add, c[10], v1 */
    w[1][2] = (3u << 26) | (0x1Bu << 2);                      /* A: const, C: .xyzw */
    w[1][3] = (2u << 28) | (0xFu << 12) | (10u << 3);         /* C: input; c[10] xyzw */
    w[2][1] = (1u << 21) | (10u << 13) | 0x1Bu;               /* mov, c[10] */
    w[2][2] = 3u << 26;                                       /* A: const */
    w[2][3] = (0xFu << 12) | (1u << 11) | (3u << 3) | 1u;     /* oD0 xyzw, final */
    load_program((const uint32_t (*)[4])w, 3);
    m(0x1764, 2 | (0 << 4) | (0 << 8));                       /* attr 1: no array */
    for (k = 0; k < 4; k++)
        mf(0x1A10 + 4u * (uint32_t)k, 1.0f);                  /* SET_VERTEX_DATA4F_M(1) */
    set_const(10, 0.0f, 0.0f, 0.0f, 0.0f);
    m(0x1E98, 1);                                             /* CXT_WRITE_EN */
    nv2a_pb_exec_set_vsh_threads(8);
    {
        /* The grid's first 600 indices: 1, 2, ... 600 in the order drawn. */
        uint32_t i;
        s_be.n = 0;
        m(0x17FC, 5);
        for (i = 0; i < 600; i += 2)
            m(0x1800, s_ix[i] | ((uint32_t)s_ix[i + 1] << 16));
        m(0x17FC, 0);
        nv2a_pb_exec_sync();
        CHECK(s_be.n == 600);
        for (k = 0; k < (int)s_be.n; k++)
            if (s_be.v[k].d0[0] != (float)(k + 1)) {
                CHECK(s_be.v[k].d0[0] == (float)(k + 1));
                break;
            }
    }
    m(0x1E98, 0);

    /* mov oPos, v0 ; mov oT0, v0 ; mov oFog, v0 ; mov oD0, v0 (final): the
     * texture coordinates, the fog distance and the colour are all v0. */
    {
        static const uint32_t outs[4] = { 0, 9, 5, 3 };
        uint32_t ww[4][4];
        int q;
        memset(ww, 0, sizeof ww);
        for (q = 0; q < 4; q++) {
            ww[q][1] = (1u << 21) | (0u << 9) | 0x1Bu;                  /* mov, v0.xyzw */
            ww[q][2] = 2u << 26;                                        /* A: input */
            ww[q][3] = (0xFu << 12) | (1u << 11) | (outs[q] << 3) | (q == 3 ? 1u : 0u);
        }
        load_program((const uint32_t (*)[4])ww, 4);
    }
    nv2a_pb_exec_set_vsh_threads(8);
    /* (t5) stage 0 a 32x32 swizzled A8R8G8B8: u and v scale by 32. */
    m(0x1B00, 0x20000);
    m(0x1B04, (0x06u << 8) | (1u << 16) | (5u << 20) | (5u << 24));
    same_both(d_tris, "t5 swizzled texture", 1);
    CHECK(s_be.v1[s_be.n - 1].u != s_be.v[s_be.n - 1].tex[0][0]);   /* it did scale */
    /* (t6) fog, linear: a factor between 0 and 1. */
    m(0x029C, 0x2601); m(0x02A4, 1); mf(0x09C0, 1.0f); mf(0x09C4, 0.003f);
    same_both(d_tris, "t6 fog", 1);
    CHECK(s_be.v[0].fog != 1.0f);
    /* (t7) the polygon offset: the triangles it moves are drawn from copies. */
    m(0x0338, 1); mf(0x0384, 1.5f); mf(0x0388, 2.0f);
    same_both(d_tris, "t7 polygon offset", 0);
    m(0x0338, 0);
    /* (t8) triangles through the near plane: clipped into copies. */
    same_both(d_near, "t8 near plane", 1);
    /* (t9) strips and fans. */
    same_both(d_strip, "t9 strip", 1);
    same_both(d_fan, "t9 fan", 1);

    nv2a_backend_register(NULL);
    free(s_be.v);
    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
