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

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }

/* ---- the fake back end: every vertex it is handed ----------------------- */
#define MAX_V (NI + 64)
static struct {
    uint32_t n;
    Nv2aVertexEx *v;
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

int main(void)
{
    static Nv2aVertexEx one[NI];
    uint32_t w[3][4];
    unsigned int csr = _mm_getcsr();
    int k;

    s_be.v = (Nv2aVertexEx *)calloc(MAX_V, sizeof *s_be.v);
    if (!s_be.v)
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

    /* Flush to zero and denormals as zero on this thread; c[10] tiny, so
     * (v0 * c10)^2 is denormal and flushes to 0 -- on every thread. */
    _mm_setcsr(csr | 0x8040u);
    set_const(10, 1e-23f, 1e-23f, 1e-23f, 1e-23f);
    set_const(12, 0.0f, 0.0f, 0.0f, 0.0f);
    grid_same(one, 1e-23f, 0.0f);
    CHECK(s_be.n > 0 && s_be.v[s_be.n - 1].d0[0] == 0.0f);
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

    nv2a_backend_register(NULL);
    free(s_be.v);
    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
