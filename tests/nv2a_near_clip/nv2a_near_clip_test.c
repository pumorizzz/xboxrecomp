/* Triangles cut at the near plane before they are rasterised.
 *
 * The executor used to clip only at w = 1e-3. A vertex between there and the
 * title's near plane (SET_CLIP_MIN) projected millions of pixels off the
 * surface, and the GPU back end, handed triangles that long, drew slivers of
 * them across the frame -- the needles and slabs next to walls. The software
 * rasteriser hid it by discarding those pixels one by one.
 *
 * Here a fixed-function perspective (screen = 128 + 128 x / z, window
 * z = 1 - 1/z, so the near plane z = 1 is window z 0 = SET_CLIP_MIN) draws a
 * triangle with two corners at z = 2 and one at z = 0.00102, just in front of
 * the eye: it projects about 6e4 pixels down. What must be drawn is the part
 * at z >= 1, a quadrilateral computed here by clipping in eye space.
 *
 *   - Through a fake back end: every vertex it is handed lies at or behind
 *     the near plane and on the surface's scale, and the triangles cover the
 *     quadrilateral's area.
 *   - Through the software path (no back end): every pixel more than a pixel
 *     away from the quadrilateral's edges is drawn inside and not outside.
 *
 * Harness as tests/nv2a_raster_batch. */
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

#define COLOUR 0xFF30C060u

/* ---- the fake back end: what the executor hands over ---------------------- */
static struct {
    uint32_t verts;
    float max_xy, min_z;
    double area;
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
    for (i = 0; i < b->count; i++) {
        const Nv2aVertexEx *v = &b->vertices_ex[i];
        if (fabsf(v->x) > s_be.max_xy) s_be.max_xy = fabsf(v->x);
        if (fabsf(v->y) > s_be.max_xy) s_be.max_xy = fabsf(v->y);
        if (v->z < s_be.min_z) s_be.min_z = v->z;
        s_be.verts++;
    }
    for (i = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertexEx *a = &b->vertices_ex[i], *p = a + 1, *q = a + 2;
        s_be.area += fabs(((double)p->x - a->x) * ((double)q->y - a->y)
                          - ((double)q->x - a->x) * ((double)p->y - a->y)) / 2.0;
    }
}

static void be_flip(void) {}
static void be_sync(uint32_t reason) { (void)reason; }
static uint32_t be_report(void) { return 0; }
static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip, be_sync, be_report };

static const double TRI[3][3] = {
    { -1.0, -1.0, 2.0 }, { 1.0, -1.0, 2.0 }, { 0.0, 0.5, 0.00102 },
};

static void project(const double e[3], double s[2])
{
    s[0] = 128.0 + 128.0 * e[0] / e[2];
    s[1] = 128.0 + 128.0 * e[1] / e[2];
}

/* The part of TRI at z >= 1, on the surface. */
static int expected_polygon(double poly[4][2])
{
    int i, n = 0;
    for (i = 0; i < 3; i++) {
        const double *p = TRI[i], *q = TRI[(i + 1) % 3];
        if (p[2] >= 1.0)
            project(p, poly[n++]);
        if ((p[2] >= 1.0) != (q[2] >= 1.0)) {
            double t = (1.0 - p[2]) / (q[2] - p[2]), e[3];
            int k;
            for (k = 0; k < 3; k++)
                e[k] = p[k] + (q[k] - p[k]) * t;
            project(e, poly[n++]);
        }
    }
    return n;
}

/* Signed distance of (x, y) inside a convex polygon: > 0 inside, < 0 out. */
static double inside_by(const double poly[4][2], int n, double x, double y)
{
    double d = 1e30, area = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        const double *a = poly[i], *b = poly[(i + 1) % n];
        area += a[0] * b[1] - b[0] * a[1];
    }
    for (i = 0; i < n; i++) {
        const double *a = poly[i], *b = poly[(i + 1) % n];
        double ex = b[0] - a[0], ey = b[1] - a[1];
        double c = (ex * (y - a[1]) - ey * (x - a[0])) / sqrt(ex * ex + ey * ey);
        if (area < 0.0)
            c = -c;
        if (c < d)
            d = c;
    }
    return d;
}

/* The surface, the transform and the triangle, as a title would send them. */
static void draw_triangle(void)
{
    /* Rows: clip x = 128 x, clip y = 128 y, clip z = z - 1, w = z. */
    static const float M[16] = { 128, 0, 0, 0,   0, 128, 0, 0,
                                 0, 0, 1, -1,    0, 0, 1, 0 };
    int i;

    memset(s_mem + COLOR_AT, 0, W * H * 4);
    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, no AA */
    m(0x0200, (uint32_t)W << 16);            /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);            /* CLIP_VERTICAL */
    m(0x020C, W * 4);                        /* SET_SURFACE_PITCH */
    m(0x0210, COLOR_AT);                     /* SET_SURFACE_COLOR_OFFSET */
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, M[i]);            /* composite */
    mf(0x0A20, 128.0f);                      /* SET_VIEWPORT_OFFSET */
    mf(0x0A24, 128.0f);
    mf(0x0A28, 0.0f);                        /* window z = clip z / w = 1 - 1/z */
    mf(0x0A2C, 0.0f);
    mf(0x0394, 0.0f);                        /* SET_CLIP_MIN: the near plane */
    mf(0x0398, 1.0f);                        /* SET_CLIP_MAX */
    m(0x1E94, 0);                            /* TRANSFORM_EXECUTION_MODE: fixed */

    for (i = 0; i < 3; i++) {
        float p[3] = { (float)TRI[i][0], (float)TRI[i][1], (float)TRI[i][2] };
        uint32_t c = COLOUR;
        memcpy(s_mem + POS_AT + 12u * (uint32_t)i, p, 12);
        memcpy(s_mem + DIFF_AT + 4u * (uint32_t)i, &c, 4);
    }
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));       /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* attr 3: D3DCOLOR */
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    m(0x1810, (2u << 24) | 0);               /* DRAW_ARRAYS: 3 from 0 */
    m(0x17FC, 0);
    nv2a_pb_exec_sync();
}

static double polygon_area(const double poly[4][2], int n)
{
    double a = 0.0;
    int i;
    for (i = 0; i < n; i++)
        a += poly[i][0] * poly[(i + 1) % n][1] - poly[(i + 1) % n][0] * poly[i][1];
    return fabs(a) / 2.0;
}

int main(void)
{
    double poly[4][2], want_area;
    int n, x, y, bad = 0, inside = 0, fail = 0;

    n = expected_polygon(poly);
    want_area = polygon_area(poly, n);

    /* The back end's view. */
    s_be.min_z = 1e30f;
    nv2a_backend_register(&s_backend);
    draw_triangle();
    nv2a_backend_register(NULL);
    printf("back end: %u vertices, max |x|,|y| %.1f, min z %g, area %.1f of %.1f\n",
           s_be.verts, s_be.max_xy, s_be.min_z, s_be.area, want_area);
    if (!s_be.verts || s_be.max_xy > 4096.0f || s_be.min_z < -1e-4f
        || fabs(s_be.area - want_area) > 0.01 * want_area) {
        printf("FAIL: the back end got geometry in front of the near plane\n");
        fail = 1;
    }

    /* The software path's pixels. */
    draw_triangle();
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint32_t got;
            double d = inside_by(poly, n, x + 0.5, y + 0.5);
            if (fabs(d) < 1.0)
                continue;                    /* on an edge: either is fine */
            memcpy(&got, s_mem + COLOR_AT + 4u * (uint32_t)(y * W + x), 4);
            if (d > 0.0)
                inside++;
            if ((d > 0.0) != ((got & 0xFFFFFFu) != 0)) {
                if (bad++ < 8)
                    printf("  pixel %d,%d: got %08X, %s the near-clipped triangle\n",
                           x, y, got, d > 0.0 ? "inside" : "outside");
            }
        }
    }
    printf("%d pixels inside checked, %d wrong\n", inside, bad);
    if (bad || inside < 2000 || fail) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
