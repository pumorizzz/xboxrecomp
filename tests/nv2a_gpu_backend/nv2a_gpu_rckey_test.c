/* The pixel shaders of the combiner states a game uses (nv2a_gpu_shaderc --rc-keys,
 * rc_keys_test.txt): each state has a ps_rc of its own, and a state without one is
 * drawn by the general ps_rc. Run for real on WARP. The back end must draw every
 * state with its own shader and the same bits as the frozen reference
 * (ps_reference.hlsl, as tests/nv2a_gpu_backend nv2a_gpu_ps_test.c): the Steam
 * Deck's frame depends on the own shaders, and a wrong picture would be the cost.
 *
 *   (h1) every line of the key file reads and writes back as it is; a quad drawn
 *        after programming the state through methods makes the back end look up
 *        exactly that key, and the key compiled into the build is the file's.
 *   (h2) three quads per state, everything outside the key at random (factors,
 *        final constants, fog, the diffuse colour, alpha test, SET_CLIP_MIN/MAX,
 *        z past the ends, wrap or clamp, linear or swizzled textures, X8, the
 *        clip-plane mode): the reference into D1 and the back end into D2 give the
 *        same guest bytes, and the back end drew each with a shader of its own.
 *   (h3) RECOMP_GPU_RASTER_PSCHECK in the back end with the general ps_rc as the
 *        reference: the same quads, no 8-bit and no float difference.
 *   (h4) random combiner programs (states the file does not hold): drawn by the
 *        general shader, counted, with the reference's pixels.
 *   (h5) at 1x: the DXBC built into the back end for line i is D3DCompile of
 *        nv2a_gpu_shaders.hlsl with that key's macros, ps_rc, byte for byte, and
 *        there are as many as lines.
 *   (h6) no own shader keeps registers in an indexable array, has a switch (the
 *        register selects are folded) or a loop unless a stage reads a swizzled
 *        render target; their temporary registers against the general one's.
 *   (h7) with RECOMP_GPU_RASTER_RC_GENERIC=1 (the third ctest): no own draw, the
 *        same pixels.
 *
 * Bit for bit holds on WARP, which these tests run on. On NVIDIA the main
 * Fallout state's own shader differs from the general one by 1 in an 8-bit
 * channel in tens of pixels a frame (RECOMP_GPU_RASTER_PSCHECK shows it; the
 * driver's compiler, not fxc's flags): accepted, see "Pixel shader cost" in
 * docs/technical/pushbuffer-executor.md.
 *
 * Harness as nv2a_gpu_ps_test.c; Windows only. */
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
#include "d3d/nv2a_gpu_rc_key.h"
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
extern void nv2a_pb_exec_report(void);
extern int nv2a_gpu_test_ps_override(const void *code, size_t len);
extern const void *nv2a_gpu_test_ps_blob(int variant, size_t *len, const char **name);
extern int nv2a_gpu_test_pscheck_setup(const char *path, uint32_t every);
extern int nv2a_gpu_test_pscheck_setup_code(const void *code, size_t len, uint32_t every);
extern void nv2a_gpu_test_pscheck(uint64_t *checked, uint64_t *differ8, uint64_t *differf);
extern void nv2a_gpu_test_ps_counts(int counts[5]);
extern void nv2a_gpu_test_rc_counts(uint64_t *own, uint64_t *generic, uint32_t *states_missed);
extern void nv2a_gpu_test_rc_last_key(uint32_t w[NV2A_RC_KEY_WORDS]);
extern void nv2a_gpu_test_rc_weak_hash(int on);
extern int nv2a_gpu_test_rc_own_slots(void);

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

/* ---- the key file ------------------------------------------------------- */

#define MAX_KEYS 64
static uint32_t s_key[MAX_KEYS][NV2A_RC_KEY_WORDS];
static char s_text[MAX_KEYS][1024];
static int s_nkeys;

static int load_keys(void)
{
    size_t n = 0;
    char *txt = read_file(RC_KEYS_PATH, &n), *line, *next;
    if (!txt)
        return 0;
    for (line = txt; line; line = next) {
        char *p = line;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (!*p || *p == '#')
            continue;
        {
            size_t l = strlen(p);
            while (l && (p[l - 1] == '\r' || p[l - 1] == ' '))
                p[--l] = 0;
        }
        if (s_nkeys >= MAX_KEYS || !nv2a_rc_key_parse(p, s_key[s_nkeys])) {
            printf("FAIL: a bad line in %s: %s\n", RC_KEYS_PATH, p);
            return 0;
        }
        snprintf(s_text[s_nkeys], sizeof s_text[0], "%s", p);
        s_nkeys++;
    }
    free(txt);
    return 1;
}

/* (h8): set, the stages from n on get random words -- the shader never reads them
 * and the key must not hold them. */
static int s_junk_after_n;

/* The state programmed through methods; everything outside the key at random. */
static void key_program(const uint32_t w[NV2A_RC_KEY_WORDS])
{
    Nv2aCombiner rc;
    uint32_t s;
    memset(&rc, 0, sizeof rc);
    for (s = 0; s < 8; s++) {
        rc.color_icw[s] = w[5 + s];
        rc.alpha_icw[s] = w[13 + s];
        rc.color_ocw[s] = w[21 + s];
        rc.alpha_ocw[s] = w[29 + s];
        if (s_junk_after_n && s >= (w[0] & 0xFFu)) {
            rc.color_icw[s] = rnd();
            rc.alpha_icw[s] = rnd();
            rc.color_ocw[s] = rnd();
            rc.alpha_ocw[s] = rnd();
        }
        rc.factor0[s] = rnd();
        rc.factor1[s] = rnd();
    }
    rc.control = w[0];
    rc.stage_program = w[1];
    rc.final0 = w[2];
    rc.final1 = w[3];
    rc.final_c0 = rnd();
    rc.final_c1 = rnd();
    program_methods(&rc);
    m(0x17F8, rnd() & 0xFFFF);               /* SET_SHADER_CLIP_PLANE_MODE */
}

/* A stage texture of the key's class k (TXF_*): kind 0/1 linear or swizzled for a
 * texture, 2/3 the live target linear or swizzled; X8, wrap or clamp at random. */
static void key_stage_tex(uint32_t st, uint32_t k)
{
    uint32_t b = 0x1B00 + 0x40 * st, off, fmt;
    uint32_t kind = (k & TXF_RT) ? ((k & TXF_SWZ) ? 3u : 2u) : (rnd() & 1);
    int x8 = (int)(rnd() & 1);
    switch (kind) {
    case 0:  off = TEXL; fmt = x8 ? 0x1E : 0x12; break;
    case 1:  off = TEXS; fmt = x8 ? 0x07 : 0x06; break;
    case 2:  off = RTT;  fmt = x8 ? 0x1E : 0x12; break;
    default: off = RTT;  fmt = x8 ? 0x07 : 0x06; break;
    }
    m(b + 0x00, off);
    m(b + 0x04, (fmt << 8) | (1u << 16) | (6u << 20) | (6u << 24) | ((k & TXF_CUBE) ? 4u : 0u));
    m(b + 0x10, (uint32_t)PITCH << 16);
    m(b + 0x1C, ((uint32_t)W << 16) | H);
    m(b + 0x08, ((rnd() & 1) ? 1u : 3u) | (((rnd() & 1) ? 1u : 3u) << 8));
    m(b + 0x14, (k & TXF_BILIN) ? ((2u << 24) | (2u << 16)) : ((1u << 24) | (1u << 16)));
}

/* One quad of a scene for key w: the same random numbers whatever is drawn. */
static void key_scene(const uint32_t w[NV2A_RC_KEY_WORDS], Quad *q)
{
    uint32_t st;
    random_quad(q, (int)(rnd() % 3 == 0));
    for (st = 0; st < 4; st++) {
        uint32_t mode = (w[1] >> (5 * st)) & 0x1F, k = (w[4] >> (8 * st)) & 0xFF;
        if (mode >= 1 && mode <= 3) {
            if (k & TXF_VALID)
                key_stage_tex(st, k);
            else
                stage_off(st);
        } else if (rnd() % 3) {              /* no texture read: any state */
            stage_tex(st, rnd() % 5, 1);
        } else {
            stage_off(st);
        }
    }
    m(0x02A8, rnd());                        /* fog colour */
    random_state();
    key_program(w);
}

/* A scene of key w through the reference and the back end (pixels the same), again
 * with fresh random state until the back end looked up a combiner state: a quad
 * the random clip window or culling throws away never reaches the shader pick. */
static void draw_own(const uint32_t w[NV2A_RC_KEY_WORDS], Quad *q, const char *what)
{
    int tries;
    for (tries = 0; tries < 8; tries++) {
        uint64_t o0, g0, o1, g1;
        uint32_t miss;
        nv2a_gpu_test_rc_counts(&o0, &g0, &miss);
        key_scene(w, q);
        if (!same_both_ways(q, what, tries))
            return;
        nv2a_gpu_test_rc_counts(&o1, &g1, &miss);
        if (o1 != o0 || g1 != g0)
            return;
    }
    printf("FAIL %s: no draw of it reached a combiner shader in 8 tries\n", what);
    s_fail = 1;
}

/* A line of the disassembly that is `word` (after blanks, alone or with operands). */
static int has_line(const char *dis, const char *word)
{
    const char *p = dis;
    size_t n = strlen(word);
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && (*p == ' ' || *p == '\t')) { p++; l--; }
        if (l >= n && strncmp(p, word, n) == 0 && (l == n || p[n] == ' ' || p[n] == '\r'))
            return 1;
        if (!e)
            break;
        p = e + 1;
    }
    return 0;
}

int main(void)
{
    const char *sc = getenv("RECOMP_GPU_RASTER_SCALE");
    const char *gen = getenv("RECOMP_GPU_RASTER_RC_GENERIC");
    int generic = gen && *gen && strcmp(gen, "0") != 0;
    int k, i, j, ndiff = 0;
    uint32_t st, seed;
    uint64_t own0, gen0, own1, gen1;
    uint32_t miss;
    Quad q;

    _putenv("RECOMP_GPU_RASTER=warp");
    _putenv("RECOMP_FPS_LIMIT=0");
    s_ref = read_file(PS_REF_PATH, &s_ref_len);
    if (!s_ref || !load_keys()) {
        printf("FAIL: no %s or no keys\n", PS_REF_PATH);
        return 1;
    }
    if (!nv2a_gpu_d3d11_install()) {
        printf("SKIP: no D3D11 WARP device\n");
        return 0;
    }
    CHECK(SUCCEEDED(D3DCompile(s_ref, s_ref_len, "nv2a_gpu_d3d11", NULL, NULL, "ps_main",
                               "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &s_ref_ps, NULL)));
    printf("%d states in %s%s\n", s_nkeys, RC_KEYS_PATH, generic ? " (RC_GENERIC)" : "");

    /* (h1) text and what is built in */
    CHECK(s_nkeys > 0 && s_nkeys <= 40);
    CHECK(s_nkeys == NV2A_GPU_RC_KEYS);
    for (k = 0; k < s_nkeys; k++) {
        char back[1024];
        int n = nv2a_rc_key_format(s_key[k], back, sizeof back);
        if (n < 0 || strcmp(back, s_text[k]) != 0) {
            printf("FAIL (h1) line %d does not read and write back as it is:\n  %s\n  %s\n", k + 1,
                   s_text[k], back);
            s_fail = 1;
        }
        if (k < NV2A_GPU_RC_KEYS && memcmp(k_rc_key[k], s_key[k], sizeof s_key[k]) != 0) {
            printf("FAIL (h1) the key %d built into the back end is not the file's\n", k + 1);
            s_fail = 1;
        }
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
    {
        static const float I[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
        for (i = 0; i < 16; i++)
            mf(0x0680 + 4 * i, I[i]);        /* composite */
        for (i = 0; i < 4; i++)
            mf(0x0A20 + 4 * i, 0.0f);        /* SET_VIEWPORT_OFFSET */
        m(0x1E94, 0);                        /* fixed function until the first COMBINER_CONTROL */
    }
    m(0x0304, 0);                            /* no blend */

    /* (h1) + (h2) */
    seed = s_rng;
    nv2a_gpu_test_rc_counts(&own0, &gen0, &miss);
    for (k = 0; k < s_nkeys; k++)
        for (j = 0; j < 3; j++) {
            uint32_t got[NV2A_RC_KEY_WORDS];
            char what[64];
            key_scene(s_key[k], &q);
            snprintf(what, sizeof what, "state %d", k + 1);
            ndiff += !same_both_ways(&q, what, j);
            if (!generic) {
                nv2a_gpu_test_rc_last_key(got);
                if (memcmp(got, s_key[k], sizeof got) != 0) {
                    printf("FAIL (h1) state %d: the back end looked up another key\n", k + 1);
                    s_fail = 1;
                }
            }
        }
    nv2a_gpu_test_rc_counts(&own1, &gen1, &miss);
    printf("(h2) %d states x 3 quads: %d differ; own draws %llu, generic %llu\n", s_nkeys, ndiff,
           (unsigned long long)(own1 - own0), (unsigned long long)(gen1 - gen0));
    if (generic) {                           /* (h7) */
        CHECK(own1 == own0);
        CHECK(gen1 - gen0 == (uint64_t)s_nkeys * 3);
    } else {
        CHECK(own1 - own0 == (uint64_t)s_nkeys * 3);
        CHECK(gen1 == gen0);
    }

    /* (h8) the key holds nothing of the stages from n on. Two ways: nv2a_rc_key_make
     * given words there must leave them out (a key that kept them would be one
     * state per junk and never match the file), and a draw whose stages from n on
     * are random must look up the file's key and draw with its shader. */
    {
        int checked_keys = 0;
        for (k = 0; k < s_nkeys; k++) {
            uint32_t n = s_key[k][0] & 0xFFu, c[8], a[8], o[8], p[8], txf[4], got[NV2A_RC_KEY_WORDS];
            if (n >= 8)
                continue;
            for (st = 0; st < 8; st++) {
                c[st] = s_key[k][5 + st];  a[st] = s_key[k][13 + st];
                o[st] = s_key[k][21 + st]; p[st] = s_key[k][29 + st];
                if (st >= n) {
                    c[st] = 0xA5A5A5A5u ^ st; a[st] = 0x5A5A5A5Au ^ st;
                    o[st] = 0xC3C3C3C3u ^ st; p[st] = 0x3C3C3C3Cu ^ st;
                }
            }
            for (st = 0; st < 4; st++)
                txf[st] = (s_key[k][4] >> (8 * st)) & 0xFFu;
            nv2a_rc_key_make(got, c, a, o, p, s_key[k][0], s_key[k][1], s_key[k][2], s_key[k][3], txf);
            if (memcmp(got, s_key[k], sizeof got) != 0) {
                printf("FAIL (h8) nv2a_rc_key_make kept words of the stages from n=%u on, state %d\n",
                       (unsigned)n, k + 1);
                s_fail = 1;
            }
            checked_keys++;
        }
        CHECK(checked_keys > 0);
        if (!generic) {
            s_junk_after_n = 1;
            nv2a_gpu_test_rc_counts(&own0, &gen0, &miss);
            for (k = 0; k < s_nkeys; k++) {
                uint32_t got[NV2A_RC_KEY_WORDS];
                char what[64];
                if ((s_key[k][0] & 0xFFu) >= 8)
                    continue;
                snprintf(what, sizeof what, "state %d, junk from n", k + 1);
                draw_own(s_key[k], &q, what);
                nv2a_gpu_test_rc_last_key(got);
                if (memcmp(got, s_key[k], sizeof got) != 0) {
                    char gs[1024];
                    if (nv2a_rc_key_format(got, gs, sizeof gs) < 0)
                        gs[0] = 0;
                    printf("FAIL (h8) state %d: junk in the stages from n=%u on changed the key looked up:\n"
                           "  file: %s\n  got:  %s\n",
                           k + 1, (unsigned)(s_key[k][0] & 0xFFu), s_text[k], gs);
                    s_fail = 1;
                }
            }
            s_junk_after_n = 0;
            nv2a_gpu_test_rc_counts(&own1, &gen1, &miss);
            CHECK(own1 - own0 == (uint64_t)checked_keys);
            CHECK(gen1 == gen0);
        }
        printf("(h8) %d states with stages past n: key holds none of them\n", checked_keys);
    }

    /* (h9) the table tells keys apart by the whole key, not by where they start. The
     * hash spreads states over the slots, so two that differ late (an aicw, the
     * pair of the file) never meet; the hook starts every state from the first 5
     * words, so they do, and the one in the table must still be found for each. */
    if (!generic) {
        uint64_t o0, g0, o1, g1;
        nv2a_gpu_test_rc_weak_hash(1);
        CHECK(nv2a_gpu_test_rc_own_slots() == NV2A_GPU_RC_KEYS);
        nv2a_gpu_test_rc_counts(&o0, &g0, &miss);
        for (k = 0; k < s_nkeys; k++) {
            uint32_t got[NV2A_RC_KEY_WORDS];
            char what[64];
            snprintf(what, sizeof what, "state %d, one start slot", k + 1);
            draw_own(s_key[k], &q, what);
            nv2a_gpu_test_rc_last_key(got);
            if (memcmp(got, s_key[k], sizeof got) != 0) {
                printf("FAIL (h9) state %d: another key looked up\n", k + 1);
                s_fail = 1;
            }
        }
        nv2a_gpu_test_rc_counts(&o1, &g1, &miss);
        printf("(h9) weak hash: %d own slots of %d states; own draws %llu\n",
               nv2a_gpu_test_rc_own_slots(), NV2A_GPU_RC_KEYS, (unsigned long long)(o1 - o0));
        CHECK(o1 - o0 == (uint64_t)s_nkeys);
        CHECK(g1 == g0);
        nv2a_gpu_test_rc_weak_hash(0);
        CHECK(nv2a_gpu_test_rc_own_slots() == NV2A_GPU_RC_KEYS);
    }

    /* (h3) */
    if (!generic) {
        uint64_t checked = 0, d8 = 0, df = 0;
        CHECK(nv2a_gpu_test_pscheck_setup_code(k_ps_rc, sizeof k_ps_rc, 1));
        s_rng = seed;
        for (k = 0; k < s_nkeys; k++) {
            uint64_t c0, e0, f0;
            nv2a_gpu_test_pscheck(&c0, &e0, &f0);
            for (j = 0; j < 3; j++) {
                key_scene(s_key[k], &q);
                quad_into(D2, &q, 0);
            }
            nv2a_gpu_test_pscheck(&checked, &d8, &df);
            if (df != f0 || d8 != e0)
                printf("(h3) state %d: float differ %llu, 8-bit differ %llu pixels\n", k + 1,
                       (unsigned long long)(df - f0), (unsigned long long)(d8 - e0));
        }
        nv2a_gpu_test_pscheck(&checked, &d8, &df);
        printf("(h3) against the general ps_rc: %llu draws, 8-bit differ %llu, float differ %llu\n",
               (unsigned long long)checked, (unsigned long long)d8, (unsigned long long)df);
        CHECK(checked > 0);
        CHECK(d8 == 0);
        CHECK(df == 0);
        nv2a_gpu_test_pscheck_setup(NULL, 0);
    }

    /* (h4) states the file does not hold */
    {
        int nd = 0;
        nv2a_gpu_test_rc_counts(&own0, &gen0, &miss);
        for (i = 0; i < 8; i++) {
            uint32_t got[NV2A_RC_KEY_WORDS];
            random_quad(&q, 0);
            for (st = 0; st < 4; st++)
                if (rnd() % 5)
                    stage_tex(st, rnd() % 5, 1);
                else
                    stage_off(st);
            random_state();
            random_program();
            nd += !same_both_ways(&q, "a state without a shader", i);
            nv2a_gpu_test_rc_counts(&own1, &gen1, &miss);
            CHECK(own1 == own0 && gen1 == gen0 + 1);
            own0 = own1; gen0 = gen1;
            if (!generic) {
                nv2a_gpu_test_rc_last_key(got);
                for (k = 0; k < s_nkeys; k++)
                    CHECK(memcmp(got, s_key[k], sizeof got) != 0);
            }
        }
        printf("(h4) 8 random programs: %d differ, %u states without a shader\n", nd, (unsigned)miss);
        ndiff += nd;
        CHECK(generic || miss >= 1);
    }
    CHECK(ndiff == 0);

    /* (h5), (h6) */
    {
        int v, nown = 0, gens = 0, maxown = 0, gtemp = 0;
        char *src;
        size_t n = 0;
        const char *dir = getenv("NV2A_GPU_PS_DISASM");
        src = (sc || generic) ? NULL : read_file(SHADERS_HLSL_PATH, &n);
        CHECK((sc || generic) || src != NULL);
        for (v = 0;; v++) {
            size_t len = 0;
            const char *name = NULL;
            const void *p = nv2a_gpu_test_ps_blob(v, &len, &name);
            ID3D11ShaderReflection *r = NULL;
            D3D11_SHADER_DESC d;
            if (!p)
                break;
            memset(&d, 0, sizeof d);
            if (SUCCEEDED(D3DReflect(p, len, &IID_ID3D11ShaderReflection, (void **)&r))) {
                r->lpVtbl->GetDesc(r, &d);
                r->lpVtbl->Release(r);
            }
            if (strcmp(name, "ps_rc") == 0) {
                gtemp = (int)d.TempRegisterCount;
                gens++;
            }
            if (strncmp(name, "ps_rck_", 7) == 0) {
                int idx = atoi(name + 7), swz = 0, ta = temp_arrays(p, len);
                ID3DBlob *dis = NULL;
                char macro[NV2A_RC_KEY_MACROS][16], value[NV2A_RC_KEY_MACROS][16];
                nown++;
                if ((int)d.TempRegisterCount > maxown)
                    maxown = (int)d.TempRegisterCount;
                for (st = 0; st < 4; st++) {
                    uint32_t mode = (s_key[idx][1] >> (5 * st)) & 0x1F;
                    if (mode >= 1 && mode <= 3 && (((s_key[idx][4] >> (8 * st)) & 0xFF) & TXF_SWZ))
                        swz = 1;
                }
                if (SUCCEEDED(D3DDisassemble(p, len, 0, NULL, &dis))) {
                    const char *t = (const char *)ID3D10Blob_GetBufferPointer(dis);
                    int sw = has_line(t, "switch"), lp = has_line(t, "loop");
                    printf("(h6) %s: %zu bytes, TempArrayCount %d, TempRegisterCount %u, switch %d, loop %d%s\n",
                           name, len, ta, (unsigned)d.TempRegisterCount, sw, lp,
                           swz ? " (a swizzled target is read)" : "");
                    CHECK(ta == 0);
                    CHECK(!sw);
                    CHECK(swz || !lp);
                    if (dir)
                        disasm_to(dir, name, p, len);
                    ID3D10Blob_Release(dis);
                }
                if (src) {                    /* (h5) */
                    D3D_SHADER_MACRO mac[NV2A_RC_KEY_MACROS + 1];
                    ID3DBlob *code = NULL, *err = NULL;
                    int c = nv2a_rc_key_macros(s_key[idx], macro, value, NV2A_RC_KEY_MACROS), same = 0;
                    for (j = 0; j < c; j++) {
                        mac[j].Name = macro[j];
                        mac[j].Definition = value[j];
                    }
                    mac[c].Name = NULL;
                    mac[c].Definition = NULL;
                    if (SUCCEEDED(D3DCompile(src, n, "nv2a_gpu_d3d11", mac, NULL, "ps_rc", "ps_4_0",
                                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
                        same = ID3D10Blob_GetBufferSize(code) == len
                            && memcmp(ID3D10Blob_GetBufferPointer(code), p, len) == 0;
                    if (!same)
                        printf("FAIL (h5) %s is not what the source compiles to with its key\n", name);
                    CHECK(same);
                    if (code) ID3D10Blob_Release(code);
                    if (err) ID3D10Blob_Release(err);
                }
            }
        }
        free(src);
        printf("(h5/h6) %d own shaders for %d states; TempRegisterCount: most of them %d, general ps_rc %d\n",
               nown, s_nkeys, maxown, gtemp);
        CHECK(nown == s_nkeys);
        CHECK(gens == 1 && maxown < gtemp);
    }

    nv2a_backend_register(NULL);
    if (s_ref_ps)
        ID3D10Blob_Release(s_ref_ps);
    printf(s_fail ? "FAIL\n" : "PASS\n");
    return s_fail;
}
