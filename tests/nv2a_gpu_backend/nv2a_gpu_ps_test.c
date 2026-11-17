/* The D3D11 back end's pixel shaders against a frozen reference, run for real
 * on the WARP adapter (no window, no GPU needed). The pixel shader is most of
 * what the GPU path costs (the combiner path ~0.5 ns a pixel on an RTX 3080 Ti,
 * 23 ms a frame by Ruby's bar at 6x5 + MSAA 2); making it cheaper must not
 * change one bit of the picture.
 *
 *   (a) ps_reference.hlsl is s_hlsl as it was (a frozen copy, never edited):
 *       its content hash is the one recorded when it was cut (when its
 *       ps_main compiled to the back end's DXBC byte for byte, toolkit
 *       75bcd91; checked then, not here: the back end's source has changed
 *       since, by design), and it still compiles. Compiled once (~5 s on
 *       D3DCompile), that ps_main is the reference of (b), (d) and (e).
 *   (b) The back end's shaders give the reference's bytes on the same input:
 *       every draw and clear done twice, once through the reference
 *       (nv2a_gpu_test_ps_override) into D1 and once through the back end's
 *       own into D2, then guest RAM compared. Screen-space batches, fixed
 *       function without combiners, then random combiner programs (a fixed
 *       xorshift seed): any register 0-15 in every input, every input mapping,
 *       blue or alpha, dot / mux / blue-to-alpha, output scale 0x00-0x38 (0x28
 *       and 0x38 included, which leave the value as it is), 0-8 stages,
 *       per-stage constants, the final combiner on or off, stage modes
 *       0-5 with clip planes; stage textures linear, swizzled, a live target
 *       read linear (TXF_RT) and swizzled (TXF_SWZ) -- so each variant of the
 *       combiner shader (any stages, no target read linearly, only such
 *       targets) is drawn with, which the test checks -- wrap and clamp (sizes
 *       64, and 192 for a target at 3x3: the wrap without division),
 *       coordinates far outside, bilinear and point; alpha test; z outside
 *       SET_CLIP_MIN/MAX and past the far end. Fixed function hands the
 *       combiners no specular colour (v1 is 0 throughout).
 *   (c) The GPU combiners against the CPU's (nv2a_rc_eval, an independent
 *       implementation): constant diffuse, linear textures texel for pixel,
 *       curated programs (the D3D texture-stage forms and those of
 *       tests/nv2a_combiner). Within 1 per channel: the GPU may sum a dot
 *       product in another order, fuse a multiply and an add (WARP does), and
 *       round .5 the other way.
 *   (d) No combiner variant keeps the registers in an indexable array
 *       (TempArrayCount 0): NVIDIA keeps such an array in local memory, and
 *       that was most of what the combiner path cost. The reference does
 *       (TempArrayCount > 0), so the check can tell. NV2A_GPU_PS_DISASM=<dir>
 *       writes the disassembly of the reference and the variants there.
 *   (g) The shaders built into the back end (nv2a_gpu_shaders.h, compiled when
 *       the toolkit is built) are what nv2a_gpu_shaders.hlsl compiles to now,
 *       byte for byte: a header left behind by a source change would not be.
 *   (e) RECOMP_GPU_RASTER_PSCHECK in the back end itself: against the
 *       reference it counts draws and no 8-bit difference; against a broken
 *       copy (map_out's x * 2.0 made x * 2.5) it finds them. And it leaves
 *       the pipeline as it found it: the same 12 quads (the same seed) drawn
 *       one on top of another with the check into D1 and without into D2
 *       give the same bytes -- the check draws at the native size into its
 *       own target, and a draw after it left at that viewport would land in
 *       the top left 1/(sx*sy) of a scaled target.
 *
 * Harness as tests/nv2a_gpu_backend (nv2a_gpu_backend_test.c); Windows only. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "nv2a_combiner.h"
#include "d3d/nv2a_gpu_d3d11.h"
#include "d3d/d3d8_swizzle.h"
#include "nv2a_gpu_shaders.h"               /* the back end's shaders, built in */

#define W 64
#define H 64
#define PITCH (W * 4)
#define SURF(i) (0x10000u + (uint32_t)(i) * 0x10000u)
#define MEM_BYTES 0x300000u
#define POS_AT  0x200000u
#define DIFF_AT 0x210000u
#define TC_AT   0x220000u                /* + stage * 0x1000 */
#define D1      SURF(0)
#define D2      SURF(1)
#define RTT     SURF(2)                  /* a live target, sampled */
#define TEXL    SURF(3)                  /* linear 64x64 */
#define TEXS    SURF(4)                  /* swizzled 64x64 */
#define TEXL2   SURF(5)                  /* linear 64x64, another */
#define DC      SURF(6)                  /* (c) */
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
extern int nv2a_gpu_test_ps_override(const void *code, size_t len);
extern const void *nv2a_gpu_test_ps_blob(int variant, size_t *len, const char **name);
extern int nv2a_gpu_test_pscheck_setup(const char *path, uint32_t every);
extern int nv2a_gpu_test_pscheck_setup_code(const void *code, size_t len, uint32_t every);
extern void nv2a_gpu_test_pscheck(uint64_t *checked, uint64_t *differ8, uint64_t *differf);
extern void nv2a_gpu_test_ps_counts(int counts[5]);

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static void mf(uint32_t method, float f) { uint32_t u; memcpy(&u, &f, 4); m(method, u); }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777215.0f; }

static char *s_ref;                      /* ps_reference.hlsl */
static size_t s_ref_len;
static ID3DBlob *s_ref_ps;               /* its ps_main, compiled once in (a) */

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *p;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (char *)malloc((size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    if (p) {
        p[n] = 0;
        *len = (size_t)n;
    }
    return p;
}

/* Every dirty target into guest RAM (the report's READ sync). */
static void write_back_all(void)
{
    _putenv("RECOMP_FB_DUMP=nv2a_gpu_ps_no_such_dir/f_");
    nv2a_pb_exec_report();
    _putenv("RECOMP_FB_DUMP=");
}

/* ---- one quad ----------------------------------------------------------- */

typedef struct {
    float z[4];
    float d0[4][4];                     /* per corner */
    float tc[4][4][4];                  /* stage, corner */
    int textured;                       /* screen space: texcoords present */
} Quad;

static const int s_corner[6] = { 0, 1, 2, 1, 3, 2 };
static const float s_cx[4] = { 0, W, 0, W }, s_cy[4] = { 0, 0, H, H };

/* Vertex streams for the quad: positions, float colours (type 2) or a
 * D3DCOLOR when argb is not 0, four texcoord streams. */
static void quad_streams(const Quad *q, uint32_t argb)
{
    uint32_t i, st;
    for (i = 0; i < 6; i++) {
        int c = s_corner[i];
        float p[3];
        p[0] = s_cx[c]; p[1] = s_cy[c]; p[2] = q->z[c];
        memcpy(s_mem + POS_AT + 12u * i, p, 12);
        if (argb)
            memcpy(s_mem + DIFF_AT + 4u * i, &argb, 4);
        else
            memcpy(s_mem + DIFF_AT + 16u * i, q->d0[c], 16);
        for (st = 0; st < 4; st++)
            memcpy(s_mem + TC_AT + st * 0x1000u + 16u * i, q->tc[st][c], 16);
    }
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));            /* attr 0: float3 */
    m(0x172C, DIFF_AT);
    m(0x176C, argb ? (0 | (4 << 4) | (4 << 8)) : (2 | (4 << 4) | (16 << 8)));  /* attr 3 */
    for (st = 0; st < 4; st++) {
        m(0x1744 + 4 * st, TC_AT + st * 0x1000u);
        m(0x1784 + 4 * st, (st == 0 && !q->textured) ? 2 : (2 | (4 << 4) | (16 << 8)));
    }
}

static void draw_now(void)
{
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    m(0x1810, (5u << 24) | 0);               /* DRAW_ARRAYS 6 from 0 */
    m(0x17FC, 0);
}

/* Surface va cleared (colour, Z, stencil), then the quad. */
static void quad_into(uint32_t va, const Quad *q, uint32_t argb)
{
    m(0x0210, va);                           /* SET_SURFACE_COLOR_OFFSET */
    m(0x1D90, 0xFF203040u);                  /* SET_COLOR_CLEAR_VALUE */
    m(0x1D8C, 0xFFFFFF00u);                  /* ZSTENCIL_CLEAR_VALUE: z max, stencil 0 */
    m(0x1D94, 0xF3);                         /* CLEAR_SURFACE: colour, Z, stencil */
    quad_streams(q, argb);
    draw_now();
    nv2a_pb_exec_sync();
}

/* Texture stage st: kind 0 linear TEXL, 1 swizzled TEXS, 2 the live target
 * RTT read linear (TXF_RT), 3 RTT read swizzled (TXF_SWZ), 4 linear TEXL2;
 * X8 (opaque) twins at random; wrap or clamp, bilinear or point. */
static void stage_tex(uint32_t st, uint32_t kind, int random_state)
{
    uint32_t b = 0x1B00 + 0x40 * st, off, fmt;
    int x8 = random_state ? (int)(rnd() & 1) : 0;
    switch (kind) {
    case 0:  off = TEXL; fmt = x8 ? 0x1E : 0x12; break;
    case 1:  off = TEXS; fmt = x8 ? 0x07 : 0x06; break;
    case 2:  off = RTT;  fmt = x8 ? 0x1E : 0x12; break;
    case 3:  off = RTT;  fmt = x8 ? 0x07 : 0x06; break;
    default: off = TEXL2; fmt = 0x12; break;
    }
    m(b + 0x00, off);
    m(b + 0x04, (fmt << 8) | (1u << 16) | (6u << 20) | (6u << 24));
    m(b + 0x10, (uint32_t)PITCH << 16);
    m(b + 0x1C, ((uint32_t)W << 16) | H);
    if (random_state) {
        m(b + 0x08, ((rnd() & 1) ? 1u : 3u) | (((rnd() & 1) ? 1u : 3u) << 8));
        m(b + 0x14, (rnd() & 1) ? ((2u << 24) | (2u << 16)) : ((1u << 24) | (1u << 16)));
    } else {
        m(b + 0x08, 3u | (3u << 8));         /* clamp */
        m(b + 0x14, (1u << 24) | (1u << 16));/* point */
    }
}

static void stage_off(uint32_t st)
{
    m(0x1B00 + 0x40 * st, 0);                /* no offset: not valid */
}

static void random_quad(Quad *q, int far_z)
{
    uint32_t c, st, k;
    for (c = 0; c < 4; c++) {
        q->z[c] = far_z ? rndf(-2.0e6f, 2.0e7f) : rndf(0.0f, 1.6e7f);
        for (k = 0; k < 4; k++)
            q->d0[c][k] = rndf(-0.2f, 1.2f);
        for (st = 0; st < 4; st++) {
            q->tc[st][c][0] = rndf(-2.0f * W, 3.0f * W);
            q->tc[st][c][1] = rndf(-2.0f * H, 3.0f * H);
            q->tc[st][c][2] = rndf(-1.0f, 1.0f);
            q->tc[st][c][3] = (rnd() & 3) ? 1.0f : rndf(0.5f, 2.0f);
        }
    }
    q->textured = 1;
}

static void random_state(void)
{
    m(0x0300, rnd() & 1);                    /* alpha test */
    m(0x033C, 0x200 + rnd() % 8);
    m(0x0340, rnd() & 0xFF);
    m(0x030C, rnd() & 1);                    /* depth test */
    m(0x0354, 0x200 + rnd() % 8);
    m(0x035C, rnd() & 1);                    /* depth mask */
    if (rnd() % 3 == 0) {                    /* SET_CLIP_MIN/MAX: a window */
        float lo = rndf(0.0f, 8.0e6f);
        mf(0x0394, lo);
        mf(0x0398, lo + rndf(0.0f, 8.0e6f));
    } else {
        mf(0x0394, 0.0f);
        mf(0x0398, 16777215.0f);
    }
}

/* The quad through the reference into D1, through the back end into D2. */
static int same_both_ways(const Quad *q, const char *what, int idx)
{
    uint32_t i, a, b;
    if (!s_ref_ps || !nv2a_gpu_test_ps_override(ID3D10Blob_GetBufferPointer(s_ref_ps),
                                                ID3D10Blob_GetBufferSize(s_ref_ps))) {
        printf("FAIL: no reference pixel shader\n");
        s_fail = 1;
        return 0;
    }
    quad_into(D1, q, 0);
    nv2a_gpu_test_ps_override(NULL, 0);
    quad_into(D2, q, 0);
    write_back_all();
    for (i = 0; i < W * H; i++) {
        memcpy(&a, s_mem + D1 + 4 * i, 4);
        memcpy(&b, s_mem + D2 + 4 * i, 4);
        if (a != b) {
            printf("FAIL (b) %s %d: first difference at (%u,%u): reference %08X, back end %08X\n",
                   what, idx, i % W, i / W, a, b);
            s_fail = 1;
            return 0;
        }
    }
    return 1;
}

/* ---- (c): the CPU's combiners ------------------------------------------- */

enum { ZERO = 0, C0 = 1, C1 = 2, FOG = 3, V0 = 4, V1 = 5, T0 = 8, T1 = 9, R0 = 12, R1 = 13,
       SUM = 14, EF = 15 };
#define ALPHA 0x10
#define INVERT 0x20                         /* unsigned invert: 1 - x */
#define EXPAND 0x40                         /* 2x - 1 */
#define HALFBIAS 0x80                       /* x - 0.5 */
#define ICW(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define OCW(ab, cd, sum, fl) ((uint32_t)(cd) | (uint32_t)(ab) << 4 | (uint32_t)(sum) << 8 | (uint32_t)(fl) << 12)

typedef struct { const char *name; Nv2aCombiner rc; } Curated;

static void program_methods(const Nv2aCombiner *rc)
{
    uint32_t i;
    for (i = 0; i < 8; i++) {
        m(0x0260 + 4 * i, rc->alpha_icw[i]);
        m(0x0AC0 + 4 * i, rc->color_icw[i]);
        m(0x1E40 + 4 * i, rc->color_ocw[i]);
        m(0x0AA0 + 4 * i, rc->alpha_ocw[i]);
        m(0x0A60 + 4 * i, rc->factor0[i]);
        m(0x0A80 + 4 * i, rc->factor1[i]);
    }
    m(0x0288, rc->final0);
    m(0x028C, rc->final1);
    m(0x1E20, rc->final_c0);
    m(0x1E24, rc->final_c1);
    m(0x1E60, rc->control);
    m(0x1E70, rc->stage_program);
}

static void curated(Curated *c, int *n)
{
    Nv2aCombiner *r;
    int k = 0;
    /* alpha everywhere: r0.a = t0.a * v0.a, out alpha from r0 */
#define BEGIN(nm) (r = &c[k].rc, c[k++].name = (nm), memset(r, 0, sizeof *r), \
                   r->stage_program = 1 | (1 << 5), \
                   r->alpha_icw[0] = ICW(T0 | ALPHA, V0 | ALPHA, ZERO, ZERO), \
                   r->alpha_ocw[0] = OCW(0, 0, R0, 0), \
                   r->final1 = ICW(ZERO, ZERO, R0 | ALPHA, 0))
    BEGIN("modulate"); r->control = 1;
    r->color_icw[0] = ICW(T0, V0, ZERO, ZERO); r->color_ocw[0] = OCW(0, 0, R0, 0);
    r->final0 = ICW(ZERO, ZERO, ZERO, R0);
    BEGIN("modulate2x"); r->control = 1;
    r->color_icw[0] = ICW(T0, V0, ZERO, ZERO); r->color_ocw[0] = OCW(R0, 0, 0, 0x10);
    r->final0 = ICW(ZERO, ZERO, ZERO, R0);
    BEGIN("modulate4x"); r->control = 1;
    r->color_icw[0] = ICW(T0, V0, ZERO, ZERO); r->color_ocw[0] = OCW(R0, 0, 0, 0x20);
    r->final0 = ICW(ZERO, ZERO, ZERO, R0);
    BEGIN("modulate half"); r->control = 1;
    r->color_icw[0] = ICW(T0, V0, ZERO, ZERO); r->color_ocw[0] = OCW(R0, 0, 0, 0x30);
    r->final0 = ICW(ZERO, ZERO, ZERO, R0);
    BEGIN("addsigned"); r->control = 1;     /* t0*1 + v0*1 - 0.5 */
    r->color_icw[0] = ICW(T0, ZERO | INVERT, V0, ZERO | INVERT);
    r->color_ocw[0] = OCW(0, 0, R0, 0x08);
    r->final0 = ICW(ZERO, ZERO, ZERO, R0);
    BEGIN("dot3 expand"); r->control = 1;
    r->color_icw[0] = ICW(T0 | EXPAND, V0 | EXPAND, ZERO, ZERO);
    r->color_ocw[0] = OCW(R0, 0, 0, 0x02);
    r->final0 = ICW(ZERO, ZERO, ZERO, R0);
    BEGIN("lerp in the final"); r->control = 0;
    r->final0 = ICW(C0 | ALPHA, T0, T1, ZERO); r->final_c0 = 0x5A000000u;
    r->final1 = ICW(ZERO, ZERO, T0 | ALPHA, 0);
    BEGIN("E*F"); r->control = 0;
    r->final0 = ICW(ZERO, ZERO, ZERO, EF);
    r->final1 = ICW(T0, V0, T1 | ALPHA, 0);
    BEGIN("v1 + r0, clamped"); r->control = 1;
    r->color_icw[0] = ICW(T0, ZERO | INVERT, ZERO, ZERO); r->color_ocw[0] = OCW(R0, 0, 0, 0);
    r->final0 = ICW(ZERO, ZERO, ZERO, SUM); r->final1 = ICW(ZERO, ZERO, R0 | ALPHA, 0x80);
    BEGIN("two stages through r1"); r->control = 2;
    r->color_icw[0] = ICW(T0, V0, ZERO, ZERO); r->color_ocw[0] = OCW(R1, 0, 0, 0);
    r->color_icw[1] = ICW(R1, T1, ZERO, ZERO); r->color_ocw[1] = OCW(R0, 0, 0, 0);
    r->final0 = ICW(ZERO, ZERO, ZERO, R0);
    BEGIN("blue to alpha"); r->control = 1;
    r->color_icw[0] = ICW(T0, V0, ZERO, ZERO); r->color_ocw[0] = OCW(R0, 0, 0, 0x80);
    r->alpha_ocw[0] = 0;
    r->final0 = ICW(ZERO, ZERO, ZERO, R0);
    BEGIN("constants per stage"); r->control = 2 | 0x1000;
    r->factor0[0] = 0x80FF8040u; r->factor0[1] = 0xC0204080u;
    r->color_icw[0] = ICW(C0, T0, ZERO, ZERO); r->color_ocw[0] = OCW(R0, 0, 0, 0);
    r->color_icw[1] = ICW(C0, R0, ZERO, ZERO); r->color_ocw[1] = OCW(R0, 0, 0, 0);
    r->final0 = ICW(ZERO, ZERO, ZERO, R0);
    BEGIN("scale 0x28 (as is)"); r->control = 1;
    r->color_icw[0] = ICW(T0, V0, ZERO, ZERO); r->color_ocw[0] = OCW(R0, 0, 0, 0x28);
    r->final0 = ICW(ZERO, ZERO, ZERO, R0);
    /* tests/nv2a_combiner's three */
    BEGIN("nv2a_combiner modulate"); r->control = 1; r->stage_program = 1;
    r->color_icw[0] = ICW(T0, V0, ZERO, ZERO);
    r->alpha_icw[0] = ICW(T0 | ALPHA, V0 | ALPHA, ZERO, ZERO);
    r->color_ocw[0] = R0 << 8; r->alpha_ocw[0] = R0 << 8;
    r->final0 = ICW(ZERO, ZERO, ZERO, R0); r->final1 = ICW(ZERO, ZERO, R0 | ALPHA, 0);
    BEGIN("nv2a_combiner glow"); r->control = 0;
    r->final0 = ICW(C0 | ALPHA, T0, T1, ZERO);
    r->final1 = ICW(ZERO, ZERO, ZERO | INVERT, 0); r->final_c0 = 0x40000000u;
    BEGIN("nv2a_combiner bump"); r->control = 1;
    r->color_icw[0] = ICW(T0 | EXPAND, V0 | EXPAND, ZERO, ZERO);
    r->color_ocw[0] = (R0 << 4) | (2u << 12);
    r->final0 = 0; r->final1 = 0;
#undef BEGIN
    *n = k;
}

static void unpack_bgra(uint32_t v, float out[4])
{
    out[0] = (float)((v >> 16) & 0xFF) / 255.0f;
    out[1] = (float)((v >> 8) & 0xFF) / 255.0f;
    out[2] = (float)(v & 0xFF) / 255.0f;
    out[3] = (float)(v >> 24) / 255.0f;
}

static int to8(float v)
{
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    return (int)(v * 255.0f + 0.5f);
}

/* ---- (d): the shaders' registers ---------------------------------------- */

static int temp_arrays(const void *p, size_t n)
{
    ID3D11ShaderReflection *r = NULL;
    D3D11_SHADER_DESC d;
    if (FAILED(D3DReflect(p, n, &IID_ID3D11ShaderReflection, (void **)&r)))
        return -1;
    memset(&d, 0, sizeof d);
    r->lpVtbl->GetDesc(r, &d);
    r->lpVtbl->Release(r);
    return (int)d.TempArrayCount;
}

static void disasm_to(const char *dir, const char *name, const void *p, size_t n)
{
    ID3DBlob *t = NULL;
    char path[512];
    FILE *f;
    if (FAILED(D3DDisassemble(p, n, 0, NULL, &t)))
        return;
    snprintf(path, sizeof path, "%s/%s.asm", dir, name);
    f = fopen(path, "wb");
    if (f) {
        fwrite(ID3D10Blob_GetBufferPointer(t), 1, ID3D10Blob_GetBufferSize(t) - 1, f);
        fclose(f);
    }
    ID3D10Blob_Release(t);
}

static uint64_t fnv64(const char *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ (uint8_t)p[i]) * 0x100000001B3ull;
    return h;
}

/* The reference as cut at toolkit 75bcd91 (extract_hlsl.py), LF line ends
 * (.gitattributes); see (a). */
#define REF_FNV 0x24ACBD8D76EEDCA2ull

/* A random combiner program, sent as methods. */
static void random_program(void)
{
    Nv2aCombiner rc;
    uint32_t i, n = rnd() % 9;
    memset(&rc, 0, sizeof rc);
    for (i = 0; i < 8; i++) {
        rc.color_icw[i] = rnd();             /* any register, mapping, alpha bit */
        rc.alpha_icw[i] = rnd();
        rc.color_ocw[i] = (rnd() & 0xFFF) | ((rnd() & 0xFF) << 12);
        rc.alpha_ocw[i] = (rnd() & 0xFFF) | ((rnd() & 0x3C) << 12);
        rc.factor0[i] = rnd();
        rc.factor1[i] = rnd();
    }
    rc.control = n | ((rnd() & 1) << 8) | ((rnd() & 1) << 12) | ((rnd() & 1) << 16);
    if (rnd() & 3) {
        rc.final0 = rnd();
        rc.final1 = rnd();
    }
    rc.final_c0 = rnd();
    rc.final_c1 = rnd();
    for (i = 0; i < 4; i++) {
        static const uint32_t modes[8] = { 0, 1, 1, 2, 3, 4, 5, 1 };
        rc.stage_program |= modes[rnd() % 8] << (i * 5);
    }
    program_methods(&rc);
    m(0x17F8, rnd() & 0xFFFF);               /* SET_SHADER_CLIP_PLANE_MODE */
}

int main(void)
{
    const char *sc = getenv("RECOMP_GPU_RASTER_SCALE");
    int k = sc ? atoi(sc) : 1, i, ndiff = 0;
    uint32_t x, y, st;
    Quad q;

    if (k < 1) k = 1;
    _putenv("RECOMP_GPU_RASTER=warp");
    _putenv("RECOMP_FPS_LIMIT=0");
    s_ref = read_file(PS_REF_PATH, &s_ref_len);
    if (!s_ref) {
        printf("FAIL: no %s\n", PS_REF_PATH);
        return 1;
    }
    if (!nv2a_gpu_d3d11_install()) {
        printf("SKIP: no D3D11 WARP device\n");
        return 0;
    }

    /* (a) */
    {
        uint64_t h = fnv64(s_ref, s_ref_len);
        printf("(a) reference: %zu bytes, fnv %016llX\n", s_ref_len, (unsigned long long)h);
        CHECK(h == REF_FNV);
        CHECK(SUCCEEDED(D3DCompile(s_ref, s_ref_len, "nv2a_gpu_d3d11", NULL, NULL, "ps_main",
                                   "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &s_ref_ps,
                                   NULL)));
    }

    /* (g) the built-in shaders are the source's: what nv2a_gpu_shaders.hlsl
     * compiles to now (D3DCompile, the back end's flags), byte for byte, and
     * what the back end made its pixel shaders from. Once, at 1x (~35 s). */
    if (!sc) {
        static const D3D_SHADER_MACRO tex1[] = { { "RC_TEX", "1" }, { NULL, NULL } };
        static const D3D_SHADER_MACRO tex2[] = { { "RC_TEX", "2" }, { NULL, NULL } };
        static const struct {
            const char *entry, *target, *name; const D3D_SHADER_MACRO *macros;
            const unsigned char *code; size_t len;
        } built[] = {
            { "vs_main", "vs_4_0", "vs_main", NULL, k_vs_main, sizeof k_vs_main },
            { "ps_screen", "ps_4_0", "ps_screen", NULL, k_ps_screen, sizeof k_ps_screen },
            { "ps_ffp", "ps_4_0", "ps_ffp", NULL, k_ps_ffp, sizeof k_ps_ffp },
            { "ps_rc", "ps_4_0", "ps_rc", NULL, k_ps_rc, sizeof k_ps_rc },
            { "ps_rc", "ps_4_0", "ps_rc_tex", tex1, k_ps_rc_tex, sizeof k_ps_rc_tex },
            { "ps_rc", "ps_4_0", "ps_rc_rt", tex2, k_ps_rc_rt, sizeof k_ps_rc_rt },
        };
        size_t n = 0, b;
        char *src = read_file(SHADERS_HLSL_PATH, &n);
        CHECK(src != NULL);
        for (b = 0; src && b < sizeof built / sizeof built[0]; b++) {
            ID3DBlob *code = NULL, *err = NULL;
            int same = 0, v;
            if (SUCCEEDED(D3DCompile(src, n, "nv2a_gpu_d3d11", built[b].macros, NULL, built[b].entry,
                                     built[b].target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                                     &err)))
                same = ID3D10Blob_GetBufferSize(code) == built[b].len
                    && memcmp(ID3D10Blob_GetBufferPointer(code), built[b].code, built[b].len) == 0;
            printf("(g) %s: built in %zu bytes, %s the source's\n", built[b].name, built[b].len,
                   same ? "the same as" : "NOT the same as");
            CHECK(same);
            for (v = 0;; v++) {              /* the back end's own pixel shader of that name */
                size_t len = 0;
                const char *name = NULL;
                const void *p = nv2a_gpu_test_ps_blob(v, &len, &name);
                if (!p)
                    break;
                if (strcmp(name, built[b].name) == 0)
                    CHECK(len == built[b].len && memcmp(p, built[b].code, len) == 0);
            }
            if (code) ID3D10Blob_Release(code);
            if (err) ID3D10Blob_Release(err);
        }
        free(src);
    }

    m(0x0208, 0x0028);                       /* SET_SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    m(0x0200, (uint32_t)W << 16);            /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);            /* CLIP_VERTICAL */
    m(0x020C, PITCH | ((uint32_t)PITCH << 16));   /* SET_SURFACE_PITCH: colour, zeta */
    for (i = 0; i < W * H * 4; i++) {
        s_mem[TEXL + i] = (uint8_t)rnd();
        s_mem[TEXS + i] = (uint8_t)rnd();
        s_mem[TEXL2 + i] = (uint8_t)rnd();
    }

    /* The live target RTT: a screen-space copy of TEXL with a band of colour. */
    memset(&q, 0, sizeof q);
    q.textured = 1;
    for (i = 0; i < 4; i++) {
        q.tc[0][i][0] = s_cx[i]; q.tc[0][i][1] = s_cy[i]; q.tc[0][i][3] = 1.0f;
        q.d0[i][0] = q.d0[i][1] = q.d0[i][2] = q.d0[i][3] = 1.0f;
    }
    stage_tex(0, 0, 0);
    quad_into(RTT, &q, 0);

    /* (b) screen space: no composite matrix yet. */
    for (i = 0; i < 24; i++) {
        random_quad(&q, 0);
        q.textured = (int)(rnd() & 1);
        stage_tex(0, rnd() % 4, 1);
        m(0x0304, 0);                        /* no blend */
        ndiff += !same_both_ways(&q, "screen", i);
    }
    /* (b) fixed function, no combiners (before the first COMBINER_CONTROL). */
    {
        static const float I[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
        for (i = 0; i < 16; i++)
            mf(0x0680 + 4 * i, I[i]);        /* composite */
        for (i = 0; i < 4; i++)
            mf(0x0A20 + 4 * i, 0.0f);        /* SET_VIEWPORT_OFFSET */
        m(0x1E94, 0);                        /* fixed function */
    }
    for (i = 0; i < 40; i++) {
        random_quad(&q, (int)(rnd() & 1));
        stage_tex(0, rnd() % 4, 1);
        random_state();
        ndiff += !same_both_ways(&q, "ffp", i);
    }
    /* (b) combiners. */
    for (i = 0; i < 160; i++) {
        random_quad(&q, (int)(rnd() % 3 == 0));
        for (st = 0; st < 4; st++)
            if (rnd() % 5)
                stage_tex(st, rnd() % 5, 1);
            else
                stage_off(st);
        random_state();
        random_program();
        ndiff += !same_both_ways(&q, "combiners", i);
    }
    printf("(b) %d screen, 40 fixed-function, 160 combiner quads: %d differ\n", 24, ndiff);
    {
        /* every variant of the combiner shader was drawn with (and so checked) */
        int counts[5];
        nv2a_gpu_test_ps_counts(counts);
        printf("(b) shaders picked: screen %d, ffp %d, combiners %d / %d / %d\n", counts[0],
               counts[1], counts[2], counts[3], counts[4]);
        CHECK(counts[0] > 0 && counts[1] > 0);
        CHECK(counts[2] > 0 && counts[3] > 0 && counts[4] > 0);
    }

    /* (c) */
    {
        static Curated cur[24];
        int nc = 0, c;
        uint32_t d0 = 0xA0C08040u;
        float v0[4], v1[4] = { 0, 0, 0, 0 }, fog[4] = { 0, 0, 0, 1 };
        unpack_bgra(d0, v0);
        curated(cur, &nc);
        memset(&q, 0, sizeof q);
        q.textured = 1;
        for (i = 0; i < 4; i++)
            for (st = 0; st < 4; st++) {
                q.tc[st][i][0] = s_cx[i]; q.tc[st][i][1] = s_cy[i]; q.tc[st][i][3] = 1.0f;
            }
        m(0x0300, 0); m(0x030C, 0); m(0x035C, 0);
        mf(0x0394, 0.0f); mf(0x0398, 16777215.0f);
        stage_tex(0, 0, 0);
        stage_tex(1, 4, 0);
        stage_off(2);
        stage_off(3);
        for (c = 0; c < nc; c++) {
            int worst = 0, wx = 0, wy = 0, wc = 0, got8 = 0, want8 = 0;
            program_methods(&cur[c].rc);
            quad_into(DC, &q, d0);
            write_back_all();
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    float t[4][4], out[4];
                    uint32_t g, a0, a1;
                    int ch;
                    memset(t, 0, sizeof t);
                    memcpy(&a0, s_mem + TEXL + y * PITCH + 4 * x, 4);
                    memcpy(&a1, s_mem + TEXL2 + y * PITCH + 4 * x, 4);
                    unpack_bgra(a0, t[0]);
                    unpack_bgra(a1, t[1]);
                    nv2a_rc_eval(&cur[c].rc, v0, v1, fog, (const float (*)[4])t, out);
                    memcpy(&g, s_mem + DC + y * PITCH + 4 * x, 4);
                    for (ch = 0; ch < 4; ch++) {
                        static const int shift[4] = { 16, 8, 0, 24 };
                        int gv = (int)((g >> shift[ch]) & 0xFF), wv = to8(out[ch]);
                        int d = gv > wv ? gv - wv : wv - gv;
                        if (d > worst) {
                            worst = d; wx = (int)x; wy = (int)y; wc = ch; got8 = gv; want8 = wv;
                        }
                    }
                }
            if (worst > 1)
                printf("FAIL (c) %s: off by %d at (%d,%d) channel %d: GPU %d, nv2a_rc_eval %d\n",
                       cur[c].name, worst, wx, wy, wc, got8, want8);
            CHECK(worst <= 1);
        }
        printf("(c) %d programs against nv2a_rc_eval\n", nc);
    }

    /* (d) */
    {
        const char *dir = getenv("NV2A_GPU_PS_DISASM");
        int v, nrc = 0;
        if (s_ref_ps) {
            int ta = temp_arrays(ID3D10Blob_GetBufferPointer(s_ref_ps),
                                 ID3D10Blob_GetBufferSize(s_ref_ps));
            printf("(d) reference ps_main: TempArrayCount %d\n", ta);
            CHECK(ta > 0);
            if (dir)
                disasm_to(dir, "reference_ps_main", ID3D10Blob_GetBufferPointer(s_ref_ps),
                          ID3D10Blob_GetBufferSize(s_ref_ps));
        }
        for (v = 0;; v++) {
            size_t n = 0;
            const char *name = NULL;
            const void *p = nv2a_gpu_test_ps_blob(v, &n, &name);
            int ta;
            if (!p)
                break;
            ta = temp_arrays(p, n);
            printf("(d) %s: TempArrayCount %d\n", name, ta);
            if (dir)
                disasm_to(dir, name, p, n);
            if (strstr(name, "rc")) {
                nrc++;
                CHECK(ta == 0);
            }
        }
        CHECK(nrc > 0);
    }

    /* (e) */
    {
        uint64_t checked = 0, d8 = 0, df = 0;
        char *bad;
        char bad_path[64];                   /* per process: the three run side by side */
        const char *at;
        FILE *f;
        static Curated cur[24];
        static uint8_t with[W * H * 4];      /* D1, drawn with the check on */
        uint32_t seed = s_rng, pass;
        int nc = 0;

        /* The same 12 quads twice, one on top of another (no clear and no
         * read back between them: a write-back blits, and a blit puts the
         * pipeline back itself): the check on, into D1; off, into D2. */
        for (pass = 0; pass < 2; pass++) {
            s_rng = seed;
            if (pass == 0 && s_ref_ps)
                CHECK(nv2a_gpu_test_pscheck_setup_code(ID3D10Blob_GetBufferPointer(s_ref_ps),
                                                       ID3D10Blob_GetBufferSize(s_ref_ps), 1));
            m(0x0210, pass ? D2 : D1);
            m(0x1D90, 0xFF203040u);
            m(0x1D8C, 0xFFFFFF00u);
            m(0x1D94, 0xF3);
            for (i = 0; i < 12; i++) {
                random_quad(&q, 0);
                for (st = 0; st < 4; st++)
                    stage_tex(st, rnd() % 5, 1);
                random_state();
                random_program();
                quad_streams(&q, 0);
                draw_now();
                nv2a_pb_exec_sync();
            }
            write_back_all();
            if (pass == 0) {
                memcpy(with, s_mem + D1, sizeof with);
                nv2a_gpu_test_pscheck(&checked, &d8, &df);
                printf("(e) against the reference: %llu draws, 8-bit differ %llu, float differ %llu\n",
                       (unsigned long long)checked, (unsigned long long)d8,
                       (unsigned long long)df);
                CHECK(checked > 0);
                CHECK(d8 == 0);
                nv2a_gpu_test_pscheck_setup(NULL, 0);
            }
        }
        for (x = 0; x < W * H; x++)
            if (memcmp(with + 4 * x, s_mem + D2 + 4 * x, 4) != 0)
                break;
        if (x < W * H)
            printf("FAIL (e) 12 quads with the check and without: first difference at "
                   "(%u,%u): with it %08X, without %08X\n", x % W, x / W,
                   *(const uint32_t *)(with + 4 * x), *(const uint32_t *)(s_mem + D2 + 4 * x));
        else
            printf("(e) 12 quads with the check and without: the same\n");
        CHECK(x == W * H);

        bad = (char *)malloc(s_ref_len + 1);
        memcpy(bad, s_ref, s_ref_len + 1);
        at = strstr(bad, "x * 2.0");
        CHECK(at != NULL);
        if (at)
            bad[(at - bad) + 6] = '5';      /* x * 2.0 -> x * 2.5 */
        snprintf(bad_path, sizeof bad_path, "nv2a_gpu_ps_badref_%lu.hlsl",
                 (unsigned long)GetCurrentProcessId());
        f = fopen(bad_path, "wb");
        CHECK(f != NULL);
        if (f) {
            fwrite(bad, 1, s_ref_len, f);
            fclose(f);
        }
        CHECK(nv2a_gpu_test_pscheck_setup(bad_path, 1));
        curated(cur, &nc);
        m(0x0300, 0); m(0x030C, 0); m(0x035C, 0);
        mf(0x0394, 0.0f); mf(0x0398, 16777215.0f);
        memset(&q, 0, sizeof q);
        q.textured = 1;
        for (i = 0; i < 4; i++)
            for (st = 0; st < 4; st++) {
                q.tc[st][i][0] = s_cx[i]; q.tc[st][i][1] = s_cy[i]; q.tc[st][i][3] = 1.0f;
            }
        stage_tex(0, 0, 0);
        stage_tex(1, 4, 0);
        program_methods(&cur[1].rc);         /* modulate2x: map_out 0x10 */
        quad_into(D1, &q, 0xA0C08040u);
        nv2a_gpu_test_pscheck(&checked, &d8, &df);
        printf("(e) against a broken copy: %llu draws, 8-bit differ %llu\n",
               (unsigned long long)checked, (unsigned long long)d8);
        CHECK(checked > 0);
        CHECK(d8 > 0);
        nv2a_gpu_test_pscheck_setup(NULL, 0);
        remove(bad_path);
        free(bad);
    }

    nv2a_backend_register(NULL);
    if (s_ref_ps)
        ID3D10Blob_Release(s_ref_ps);
    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
