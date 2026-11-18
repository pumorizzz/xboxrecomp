/* The GPU profile of the D3D11 back end (RECOMP_GPU_PROF, src/d3d/nv2a_gpu_prof.c),
 * run for real on the WARP adapter (no window, no GPU needed). It answers
 * "where do the GPU's milliseconds go"; a profile that puts work under the
 * wrong pass or target, or counts it twice, sends the fix to the wrong place.
 *
 * Three frames: frame 1 clears target A, draws two quads into it back to back,
 * clears B with depth and stencil, then draws A as a texture onto C; frame 2
 * has no GPU work at all; frame 3 draws into C again.
 *
 *   - The profile submits each frame's closing queries itself, one Flush per
 *     closed frame (two here). Left in the command buffer until the next
 *     frame's work, the end timestamp ran late, and in the game the GPU's
 *     idle time at a frame limit went into the frame's last segment (span =
 *     the flip period, swap 30 of 33 ms at 1x). Both frames also come back as
 *     a flip reads them -- DONOTFLUSH, nothing flushed by the test -- though
 *     WARP runs queued commands without a Flush, so on WARP only the count
 *     of submits tells.
 *   - Two frames are read, none dropped: a frame without GPU work is not
 *     profiled (it would put empty frames into the span statistics), and the
 *     test's few frames never fill the ring of eight.
 *   - clear.color counts both colour clears and their area at the internal
 *     scale (64x64 surface pixels times sx*sy each); clear.zs counts the one
 *     depth clear over the whole depth buffer (it clears the resource, not
 *     the scissor).
 *   - The two quads into A with nothing between them are one draw.ffp
 *     segment of two draws: a segment is a run of one (kind, target), and a
 *     new timestamp at every draw would cost queries for nothing.
 *   - At 3x3 with 4x MSAA, sampling A resolves it (whole texture, sx*sy*4 bytes
 *     a pixel times the samples); at 1x there is no resolve at all.
 *   - The segments of a frame add up to its span and none is negative --
 *     each lasts from its own timestamp to the next.
 *   A frame whose clock WARP reports disjoint is thrown away whole; then only
 *   frames + disjoint == 2 is checked and the rest says SKIP. A frame whose
 *   queries cannot be read (read errors) or never come back (still out) is
 *   not a disjoint one: both must be 0 before any SKIP. They were counted as
 *   disjoint once, and a frame that never ended its end timestamp or its
 *   DISJOINT query (GetData fails on WARP) went into the SKIP as a pass.
 *   - With stats, the two quads' pixel shader invocations are at least their
 *     pixels at the internal scale (one invocation a pixel even with MSAA) and
 *     at most twice that (2x2 quads along the diagonal). Without stats, no
 *     statistics query is ever made and nothing is counted.
 *   - The report names the passes and A's address, in more than four lines.
 *
 * With RECOMP_GPU_RASTER_LOSE_AT=2 the device is lost at the second flip, with
 * frame 1's queries possibly still out: the profile lets go of its queries
 * before the back end releases the device and switches itself off, and
 * nothing afterwards (frame 3 on the software path, the read-back, the
 * report) touches the released context.
 *
 * Harness as tests/nv2a_gpu_backend (nv2a_gpu_backend_test.c); Windows only. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "d3d/nv2a_gpu_d3d11.h"
#include "d3d/nv2a_gpu_prof.h"

#define W 64
#define H 64
#define PITCH (W * 4)
#define SURF(i) (0x10000u + (uint32_t)(i) * 0x10000u)
#define MEM_BYTES 0x240000u
#define POS_AT  0x1A0000u
#define DIFF_AT 0x1B0000u
#define TC_AT   0x1C0000u
#define PAT_TEX SURF(28)                 /* 0x1D0000: linear 64x64 */
#define A SURF(0)
#define B SURF(1)
#define C SURF(2)
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
extern void nv2a_gpu_test_target_dims(uint32_t *w, uint32_t *h);

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static void mf(uint32_t method, float f) { uint32_t u; memcpy(&u, &f, 4); m(method, u); }

/* A quad over the whole surface, fixed function, white, stage 0 the linear
 * A8R8G8B8 W x H texture at tex (as nv2a_gpu_backend_test.c's draw_quad).
 * With sync it sets the surface to va and ends the scan; without, it draws
 * into the current surface. */
static void draw_quad(uint32_t va, uint32_t tex, int sync)
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
        memcpy(s_mem + POS_AT + 12u * i, P[i], 12);
        memcpy(s_mem + DIFF_AT + 4u * i, &white, 4);
        memcpy(s_mem + TC_AT + 8u * i, P[i], 8);
    }
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));   /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));    /* attr 3: D3DCOLOR */
    m(0x1744, TC_AT);   m(0x1784, 2 | (2 << 4) | (8 << 8));    /* attr 9: float2 */
    m(0x1B00, tex);                          /* SET_TEXTURE_OFFSET */
    m(0x1B04, (0x12u << 8) | (1u << 16) | (6u << 20) | (6u << 24));
    m(0x1B10, (uint32_t)PITCH << 16);        /* CONTROL1: pitch */
    m(0x1B1C, ((uint32_t)W << 16) | H);      /* IMAGE_RECT */
    if (sync)
        m(0x0210, va);                       /* SET_SURFACE_COLOR_OFFSET (a new epoch) */
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    m(0x1810, (5u << 24) | 0);               /* DRAW_ARRAYS 6 from 0 */
    m(0x17FC, 0);
    if (sync)
        nv2a_pb_exec_sync();
}

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static GprofTestZone s_z[64];
static int s_nz;

static const GprofTestZone *zone(int kind, uint32_t va)
{
    int i;
    for (i = 0; i < s_nz; i++)
        if (s_z[i].kind == kind && s_z[i].va == va)
            return &s_z[i];
    return NULL;
}

/* Counter c of every zone of a kind. */
static uint64_t kind_sum(int kind, int c)
{
    uint64_t n = 0;
    int i;
    for (i = 0; i < s_nz; i++)
        if (s_z[i].kind == kind)
            n += s_z[i].count[c];
    return n;
}

static int kind_zones(int kind)
{
    int i, n = 0;
    for (i = 0; i < s_nz; i++)
        n += s_z[i].kind == kind;
    return n;
}

/* The report into s_rep (through a file, as it goes to stderr in the game);
 * its line count, -1 when no file could be made. */
static char s_rep[16384];

static int report_lines(void)
{
    size_t n;
    int lines = 0;
    char *p;
    FILE *t = tmpfile();
    const char *named = NULL;

    if (!t) {                                /* MSVC's tmpfile wants the drive root */
        named = "nv2a_gpu_prof_report.txt";
        t = fopen(named, "w+b");
    }
    CHECK(t != NULL);
    if (!t)
        return -1;
    nv2a_gpu_test_prof_report(t);
    rewind(t);
    n = fread(s_rep, 1, sizeof s_rep - 1, t);
    s_rep[n] = 0;
    fclose(t);
    if (named)
        remove(named);
    for (p = s_rep; *p; p++)
        lines += *p == '\n';
    printf("report, %d lines:\n%s", lines, s_rep);
    return lines;
}

int main(void)
{
    const char *sc = getenv("RECOMP_GPU_RASTER_SCALE"), *ms = getenv("RECOMP_GPU_RASTER_MSAA");
    const char *pe = getenv("RECOMP_GPU_PROF");
    uint64_t k = sc ? (uint64_t)atoi(sc) : 1, msaa = ms ? (uint64_t)atoi(ms) : 1;
    int stats = pe && strstr(pe, "stats") != NULL;
    uint32_t tw = 0, th = 0, i;
    GprofTestFrame f;
    const GprofTestZone *za;

    if (k < 1) k = 1;
    if (msaa < 1) msaa = 1;
    _putenv("RECOMP_GPU_RASTER=warp");
    _putenv("RECOMP_FPS_LIMIT=0");
    if (!nv2a_gpu_d3d11_install()) {
        printf("SKIP: no D3D11 WARP device\n");
        return 0;
    }
    nv2a_gpu_test_target_dims(&tw, &th);
    CHECK(tw == 1280u * k);
    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8, no AA */
    m(0x0200, (uint32_t)W << 16);            /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);            /* CLIP_VERTICAL */
    m(0x020C, PITCH);                        /* SET_SURFACE_PITCH */
    for (i = 0; i < W * H; i++) {
        uint32_t p = 0xFF000000u | (i * 2654435761u >> 8);
        memcpy(s_mem + PAT_TEX + 4u * i, &p, 4);
    }

    /* Frame 1. */
    m(0x0210, A);                            /* SET_SURFACE_COLOR_OFFSET */
    m(0x1D90, 0xFF102030u);                  /* SET_COLOR_CLEAR_VALUE */
    m(0x1D94, 0xF0);                         /* CLEAR_SURFACE: colour */
    draw_quad(A, PAT_TEX, 0);
    draw_quad(A, PAT_TEX, 0);
    m(0x0210, B);
    m(0x1D94, 0xF3);                         /* colour, Z and stencil */
    draw_quad(C, A, 1);                      /* A sampled as a texture */
    m(0x0130, 0);                            /* FLIP_STALL */
    /* Frame 2: no GPU work. */
    m(0x0130, 0);
    /* Frame 3. */
    draw_quad(C, PAT_TEX, 1);
    m(0x0130, 0);

    if (getenv("RECOMP_GPU_RASTER_LOSE_AT")) {
        /* The device was lost at flip 2: the profile is off, its queries gone. */
        nv2a_gpu_test_prof_poll_noflush(&f);
        printf("after the loss: profile %s, %u queries in the ring, %u frames out,"
               " %u read\n", g_nv2a_gprof_on ? "on" : "off", f.ring_queries, f.waiting,
               f.frames);
        CHECK(g_nv2a_gprof_on == 0);
        CHECK(f.ring_queries == 0);
        CHECK(f.waiting == 0);
        CHECK(nv2a_gpu_test_prof_queries() > 0);   /* it was on before */
        s_nz = nv2a_gpu_test_prof_drain(s_z, 64, &f);
        CHECK(f.ring_queries == 0);
        CHECK(report_lines() > 0);
        nv2a_backend_register(NULL);
        printf(s_fail ? "FAIL\n" : "PASS\n");
        return s_fail;
    }

    /* As the flips would read them: no Flush from here. */
    nv2a_gpu_test_prof_poll_noflush(&f);
    printf("read without a flush: %u frames, %u disjoint, %u read errors (last 0x%08X),"
           " %u still out, %u submitted\n",
           f.frames, f.disjoint, f.read_errors, f.last_error, f.waiting, f.submits);
    CHECK(f.read_errors == 0);
    CHECK(f.frames + f.disjoint == 2);
    CHECK(f.waiting == 0);
    CHECK(f.submits == 2);                   /* frames 1 and 3, each at its flip */

    s_nz = nv2a_gpu_test_prof_drain(s_z, 64, &f);
    printf("profile: %u frames, %u dropped, %u disjoint, %u read errors, %u still out,"
           " %d zones, %d queries"
           " (%u statistics), span %lld ticks, segments %lld, shortest %lld\n",
           f.frames, f.dropped, f.disjoint, f.read_errors, f.waiting, s_nz,
           nv2a_gpu_test_prof_queries(),
           f.stat_queries, (long long)f.span_ticks, (long long)f.seg_ticks_sum,
           (long long)f.min_seg_ticks_signed);
    CHECK(nv2a_gpu_test_prof_queries() > 0);
    CHECK(f.frames + f.disjoint == 2);
    CHECK(f.dropped == 0);
    CHECK(f.overflow == 0);
    CHECK(f.read_errors == 0);               /* not the driver's disjoint: never a SKIP */
    CHECK(f.waiting == 0);
    if (f.disjoint) {
        printf("SKIP counts and timing (WARP disjoint: %u frames thrown away)\n", f.disjoint);
        nv2a_backend_register(NULL);
        printf(s_fail ? "FAIL\n" : "PASS\n");
        return s_fail;
    }
    CHECK(f.frames == 2);

    CHECK(kind_sum(GP_CLEAR_COLOR, GPC_CLEARS) == 2);
    CHECK(kind_sum(GP_CLEAR_COLOR, GPC_CLEAR_PX) == 2ull * W * H * k * k);
    CHECK(kind_sum(GP_CLEAR_ZS, GPC_CLEARS) == 1);
    CHECK(kind_sum(GP_CLEAR_ZS, GPC_CLEAR_PX) == (uint64_t)tw * th);

    za = zone(GP_DRAW_FFP, A);
    CHECK(za != NULL);
    if (za) {
        printf("draw.ffp at A: %llu segments, %llu draws, %llu PS invocations\n",
               (unsigned long long)za->segs, (unsigned long long)za->count[GPC_DRAWS],
               (unsigned long long)za->ps_inv);
        CHECK(za->segs == 1 && za->count[GPC_DRAWS] == 2);
        CHECK(za->count[GPC_TRIS] == 4);
        if (stats) {
            uint64_t px = 2ull * W * H * k * k;
            CHECK(za->ps_inv >= px && za->ps_inv <= 2 * px);
        }
    }
    CHECK(zone(GP_DRAW_FFP, C) != NULL);

    if (k * k * msaa > 1 && msaa > 1) {
        const GprofTestZone *zr = zone(GP_RESOLVE, A);
        CHECK(zr != NULL);
        if (zr) {
            CHECK(zr->count[GPC_RESOLVES] >= 1);
            CHECK(zr->count[GPC_RESOLVE_BYTES]
                  == zr->count[GPC_RESOLVES] * (uint64_t)tw * th * 4 * msaa);
        }
    } else {
        CHECK(kind_zones(GP_RESOLVE) == 0);
    }

    CHECK(f.min_seg_ticks_signed >= 0);
    CHECK(f.seg_ticks_sum == f.span_ticks);
    CHECK(f.span_ticks > 0);

    if (!stats) {
        CHECK(f.stat_queries == 0);
        for (i = 0; i < (uint32_t)s_nz; i++)
            CHECK(s_z[i].ps_inv == 0);
    } else {
        CHECK(f.stat_queries > 0);
    }

    {
        int lines = report_lines();
        CHECK(lines > 4);
        CHECK(strstr(s_rep, "[GPUPROF]") != NULL);
        CHECK(strstr(s_rep, "draw.ffp") != NULL);
        CHECK(strstr(s_rep, "clear.zs") != NULL);
        CHECK(strstr(s_rep, "0x00010000") != NULL);
    }

    nv2a_backend_register(NULL);
    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
