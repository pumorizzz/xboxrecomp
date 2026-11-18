/*
 * NV2A rasterisation on the host GPU, through Direct3D 11. See
 * nv2a_gpu_d3d11.h for what it is and how it is switched on.
 *
 * Everything below runs on the NV2A poll thread, one call at a time, except
 * nv2a_gpu_d3d11_install(), which creates the device on the main thread
 * before the title starts. Colour surfaces live in offscreen render targets
 * that mirror guest RAM; with the framebuffer window open (RECOMP_FB_WINDOW)
 * a swap chain on it shows the presented target directly (see "presenting
 * from the GPU").
 *
 * Colour surfaces. One BGRA8 render target per (guest address,
 * pitch, bytes per pixel; size: see "Internal resolution" below), with a CPU
 * copy (the shadow) of what guest RAM held when the target last matched it. The first time a target is used in
 * an epoch -- epochs end at every executor sync and every flip -- the guest
 * rows are compared with the shadow and the rows the title (or the CPU) has
 * changed are uploaded. A target goes back into guest RAM (and the shadow)
 * only when something reads it there: the flip's present (sync_surface, the
 * presented surface only), the executor's dumps (sync READ), a texture that
 * must be decoded from guest RAM, or a target being dropped. A texture that
 * is a live target's pixels is sampled from the target itself. 16-bit
 * surfaces are R5G6B5 in guest RAM and expanded in the target, the way the
 * software path reads them. RECOMP_GPU_RASTER_SYNC=all writes back at every
 * executor sync instead; RECOMP_GPU_RASTER_RTTEX=0 reads every texture
 * through guest RAM.
 *
 * Depth and stencil live only here, one D32_FLOAT_S8X24 buffer per zeta
 * address; guest zeta memory is never read or written (nor is it on the
 * software path).
 *
 * Textures are decoded on the CPU with the executor's own colour conversion
 * (nv2a_backend_decode_texture_face) into BGRA8 texture arrays (six slices
 * for a cube) -- DXT1/3/5 in whole blocks go up as BC1/2/3 instead and the
 * GPU decodes them -- cached by what the title programmed and re-hashed once
 * per epoch. The pixel shader samples them by hand -- Load, the title's wrap
 * mode, the executor's bilinear taps -- so the GPU reads the texels the
 * software path would.
 *
 * Pixels. One source (nv2a_gpu_shaders.hlsl), one pixel shader per path,
 * chosen on the CPU per draw (pick_ps): ps_screen (flat colour, the texel
 * replaces it), ps_ffp (the fixed-function stand-in: diffuse times stage 0)
 * and ps_rc, the register combiners ported line by line from nv2a_rc_eval /
 * rc_stage_fetch / rc_texel, in three variants by what the stages sample
 * (RC_TEX: any; no render target read linearly; only such targets), and a
 * shader of its own for each combiner state the game uses (a file of keys,
 * NV2A_GPU_RC_KEYS in nv2a_gpu_shaders.cmake; nv2a_gpu_rc_key.h): the same
 * source with that state's words as literals, so only its code is compiled.
 * Why: on RDNA (the Steam Deck) registers are assigned to the whole shader by
 * its worst path -- fifteen float4s, four texels, A to D -- and the occupancy
 * that leaves cost a combiner pixel ~45x a flat one. A state without a shader
 * of its own is drawn by the general one and named in the log (RC_STATS lists
 * every state; RC_GENERIC=1 turns the own shaders off).
 * Constants are the raw combiner words. The combiners' registers are fifteen
 * float4s behind uniform switches, not an array: an array indexed at run
 * time is an indexable temp, which NVIDIA keeps in local memory, and that
 * was most of what a combiner pixel cost (by Ruby's bar at 6x5 + MSAA 2,
 * 518 -> 134 ns per 1000 pixel shader invocations, the frame's GPU span
 * 24.6 -> 6.9 ms). Each shader is the previous ubershader's arithmetic, bit
 * for bit (tests/nv2a_gpu_backend ps_reference.hlsl, RECOMP_GPU_RASTER_PSCHECK)
 * -- the general shader and, on WARP, every own shader. On NVIDIA a state's
 * own shader can differ from the general one by 1 in an 8-bit channel (tens
 * of pixels a frame, in Fallout's main state; the HLSL compiler's flags and
 * optimisation levels give the same code and do not change it, so the
 * driver's compiler fuses or orders the literal-folded arithmetic its own
 * way); accepted as a known difference, RC_GENERIC=1 draws the general one.
 * They are compiled when the toolkit is built (nv2a_gpu_shaderc) and only
 * created here: D3DCompile takes ~14 s over ps_rc, and the title enables
 * the combiners when it creates its device, so the very first frame needs
 * it -- compiling at start, or in the background, held the first frame that
 * long.
 *
 * ponytail: every target and depth buffer has one size, RT_W native pixels
 * wide and s_gh high (RT_W x RT_H at scale 1 without MSAA), times the internal
 * scale; bigger surfaces are clamped. Level 0 only, no mip selection, as the
 * software path. A swizzled colour surface is drawn linearly on the GPU and
 * sampled as such (rt_as_texture); written back to guest RAM it stays linear.
 */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_3.h>               /* IDXGISwapChain2: the frame latency waitable */
#include <d3dcompiler.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/nv2a_backend.h"
#include "../kernel/text_block.h"
#include "d3d8_swizzle.h"
#include "nv2a_gpu_d3d11.h"
#include "nv2a_gpu_prof.h"
#include "../video/video_player.h"   /* the framebuffer window it presents into */

extern ptrdiff_t xbox_GetMemoryOffset(void);
/* fb_present.c: the window's own copy of a guest RAM frame (its dumps). */
extern void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch);

#define RT_W 1280                       /* = NV_ZBUF_W in nv2a_pb_exec.c */
#define RT_H 1024                       /* = NV_ZBUF_H */
#define MAX_RTS 8
#define MAX_ZBUFS 4
#define MAX_TEXS 1024
#define TEX_EVICT_FLIPS 300u
#define VB_BYTES (32u << 20)
#define VTX_BYTES ((uint32_t)sizeof(Nv2aVertexEx))
#define MAX_QUERIES 4096
#define MAX_DS_STATES 64
#define MAX_BLEND_STATES 64

#define EPOCH_NONE 0xFFFFFFFFu          /* never equal to s_epoch */

/* ---- the shaders ------------------------------------------------------ */

/* s_hlsl (src/d3d/nv2a_gpu_shaders.hlsl) and its shaders compiled at build
 * time (k_vs_main, k_ps_*): generated by nv2a_gpu_shaderc. */
#include "nv2a_gpu_shaders.h"
#include "nv2a_gpu_rc_key.h"

/* ---- device state ----------------------------------------------------- */

typedef struct {
    float vsc[4];                       /* inv_zmax, kind, 0, 0 */
} VsConsts;

typedef struct {
    uint32_t cicw[8], aicw[8], cocw[8], aocw[8], fac0[8], fac1[8];
    uint32_t fin[4], ctl[4], at[4];
    float    fogc[4];
    uint32_t tex[4][4];
    float    zr[4];
    float    rsc[4];                    /* s_sx, s_sy, 0, 0 */
} PsConsts;

/* ctl[3] flags */
#define PSF_RC        1u
#define PSF_TEXTURED  2u
#define PSF_ALPHATEST 4u
#define PSF_ZFAR_FAIL 8u                /* depth test LESS/EQUAL/LEQUAL: z > zmax fails */
#define PSF_ZCLIP     64u               /* window z outside SET_CLIP_MIN/MAX is not drawn */
/* tex[i][2] flags: TXF_* in nv2a_gpu_rc_key.h */

typedef struct {
    int used;
    uint32_t va, pitch, bpp, W, H;
    int swizzled;                       /* drawn as a swizzled surface (see rt_as_texture) */
    ID3D11Texture2D *tex;               /* single-sampled: sampled, read back, presented */
    ID3D11Texture2D *mtex;              /* multisampled, drawn into (RECOMP_GPU_RASTER_MSAA) */
    ID3D11RenderTargetView *rtv;        /* on mtex when multisampled, else on tex */
    ID3D11ShaderResourceView *srv;      /* the target sampled as a texture */
    int stale;                          /* mtex drawn since tex was resolved from it */
    uint8_t *shadow;                    /* RT_H rows of W*bpp bytes */
    int dirty, need_full;
    uint32_t dx0, dy0, dx1, dy1;        /* dirty rectangle, half-open */
    uint32_t epoch_checked, last_use;
    uint32_t taken_epoch;               /* rt_taken compared in this epoch */
} GpuRt;

typedef struct {
    int used;
    uint32_t va;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    uint32_t last_use;
} GpuZ;

typedef struct {
    int used, ok;
    uint32_t offset, color, width, height, pitch, palette, cube, levels;
    uint32_t lo, hi;                    /* guest bytes level 0 spans */
    uint64_t hash;
    uint32_t epoch_checked, last_flip;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} GpuTex;

typedef struct { uint32_t key[12]; ID3D11DepthStencilState *s; } DsEntry;
typedef struct { uint32_t key[5]; ID3D11BlendState *s; } BlendEntry;

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static int s_compare;
static ID3D11VertexShader *s_vs;
/* One pixel shader per path, from one source (s_hlsl): pick_ps chooses. */
static ID3D11PixelShader *s_ps_screen, *s_ps_ffp;
/* The combiner shader by what its stages sample (RC_TEX in the source): 0 any,
 * 1 no render target read linearly, 2 only such targets. */
static ID3D11PixelShader *s_ps_rc[3];
#ifdef NV2A_GPU_TEST_HOOKS
static int s_ps_picked[5];               /* screen, ffp, rc 0, 1, 2 */
#define PS_PICKED(i) (s_ps_picked[i]++)
#else
#define PS_PICKED(i) ((void)0)
#endif
static ID3D11PixelShader *s_ps_flat;    /* RECOMP_GPU_PROF=...,flatps only */
static ID3D11PixelShader *s_ps_bound;   /* what PSSetShader last set for the rasteriser */
#ifdef NV2A_GPU_TEST_HOOKS
/* nv2a_gpu_test_ps_override: every draw and clear through this one; the last
 * one created is kept (s_ps_ovr_cache) for the next call with the same code. */
static ID3D11PixelShader *s_ps_override, *s_ps_ovr_cache;
static const void *s_ovr_code;
static size_t s_ovr_len;
#define MAX_PS_BLOBS (12 + NV2A_GPU_RC_KEYS)
static const void *s_ps_code[MAX_PS_BLOBS]; /* the back end's own (nv2a_gpu_shaders.h) */
static size_t s_ps_len[MAX_PS_BLOBS];
static const char *s_ps_names[MAX_PS_BLOBS];
static int s_nps_blobs;
#endif

/* RECOMP_GPU_RASTER_PSCHECK=<ref.hlsl>[,<every>]: on every <every>-th flip each
 * draw is drawn twice more into s_pc_tex (float, native size) -- once through
 * the pixel shader the back end picked, once through ps_main of the reference
 * file -- and the two compared. Diagnostics only: it reads back per draw. */
static ID3D11PixelShader *s_ps_ref;
static ID3D11Texture2D *s_pc_tex, *s_pc_stage[2];
static ID3D11RenderTargetView *s_pc_rtv;
static uint32_t s_pc_every;
typedef struct {
    uint64_t draws, pixels, differf, differ8;
    uint32_t worst;
    int have_first;
    uint32_t va, kind, ctl[4], icw, ocw;
} PsCheck;
static PsCheck s_pc, s_pc_total;        /* this report's, since setup */
static DWORD s_pc_tick;
static ID3D11InputLayout *s_il;
static ID3D11Buffer *s_vb, *s_cb_vs, *s_cb_ps;
static uint32_t s_vb_pos;
static ID3D11RasterizerState *s_rast;
static ID3D11Texture2D *s_staging;
static ID3D11Texture2D *s_white;
static ID3D11ShaderResourceView *s_white_srv;
static VsConsts s_vsc_last;
static PsConsts s_psc_last;
static int s_vsc_valid, s_psc_valid;

static GpuRt s_rt[MAX_RTS];
static GpuZ s_z[MAX_ZBUFS];
static GpuTex s_tex[MAX_TEXS];
static DsEntry s_ds[MAX_DS_STATES];
static int s_nds;
static BlendEntry s_bl[MAX_BLEND_STATES];
static int s_nbl;

/* compare: triangles this frame with a vertex beyond +-FAR_COORD pixels.
 * The near-plane clip (w = 1e-3) puts such vertices millions of pixels out,
 * and the software rasteriser's float edge functions and barycentrics lose
 * the triangle there: in Fallout's "camera wedge" frames it misplaces about
 * a quarter of a million pixels per frame against the same arithmetic in
 * double, where the GPU clips in clip space. A frame that differs with these
 * present is that, not a GPU fault. */
#define FAR_COORD 1e5f
static uint32_t s_far_tris;

static ID3D11Query *s_qpool[MAX_QUERIES];
static int s_qfree;                     /* entries [0, s_qfree) are free */
static int s_qmade;
static ID3D11Query *s_qpend[MAX_QUERIES];
static int s_npend;
static uint64_t s_zpass_acc;            /* resolved samples, not yet reported */

/* Internal resolution and anti-aliasing (RECOMP_GPU_RASTER_SCALE=<n> or
 * <x>x<y>, RECOMP_GPU_RASTER_MSAA=<samples>).
 *
 * Every target and depth buffer is s_sx x s_sy times the surface it stands
 * for, with s_msaa samples a pixel. Vertices stay in surface pixels: the
 * viewport is scaled, so a triangle covers s_sx*s_sy times the pixels and its
 * edges come out at the higher resolution. What crosses to guest RAM stays at
 * the surface's own size -- rows the CPU wrote are drawn up into the target
 * point by point (s_up, rt_upload_rows), and a writeback averages each
 * s_sx x s_sy block down (s_down, rt_read_native) -- so the title, the
 * software path and the shadows compared with guest RAM see 640x480 as
 * before. A target sampled as a texture is read at the high resolution
 * (rt_texel), so the title's own downsample of its scene keeps the detail,
 * and the presenter draws the high-resolution frame. Multisampled targets are
 * resolved (rt_resolve) before anything reads them.
 *
 * All targets and depth buffers have one size, as D3D11 requires a target
 * and its depth buffer to match: the native width RT_W and a native height
 * s_gh, which starts at 512 and grows (every target goes back to guest RAM and
 * is made again) when a surface reaches further down. Scale 1 without MSAA is
 * the old path exactly: RT_W x RT_H, uploads and readbacks direct. compare
 * forces it.
 *
 * SSAA (RECOMP_GPU_RASTER_SSAA=<n>): the targets are drawn at scale x n, so
 * s_sx/s_sy above are the drawn scale; the writeback still averages the whole
 * s_sx x s_sy block. The presenter first averages each n x n block into s_ss
 * (ssaa_reduce) at the shown scale s_ox x s_oy, then scales that to the
 * window. */
static uint32_t s_sx = 1, s_sy = 1, s_msaa = 1;
static uint32_t s_ox = 1, s_oy = 1;     /* the shown scale: s_sx / s_ssaa */
static uint32_t s_ssaa = 1;             /* RECOMP_GPU_RASTER_SSAA: n x n drawn per shown texel */
static ID3D11Texture2D *s_ss;           /* RT_W*s_ox x RT_H*s_oy: the frame averaged down */
static ID3D11ShaderResourceView *s_ss_srv;   /* Texture2DArray, as the presenter reads */
static ID3D11RenderTargetView *s_ss_rtv;
#define MAX_SIDE 8                      /* scale x SSAA a side: 1280*8 = 10240 wide */
static int s_scaled;                    /* s_sx * s_sy * s_msaa > 1 */
static uint32_t s_gh = RT_H;            /* native height every target has */
static ID3D11Texture2D *s_up, *s_down;  /* native size: rows going up, blocks coming down */
static ID3D11ShaderResourceView *s_up_srv;
static ID3D11RenderTargetView *s_down_rtv;
static ID3D11VertexShader *s_bvs;
static ID3D11PixelShader *s_bps_up, *s_bps_down;
static ID3D11Buffer *s_cb_blit;

static uint32_t s_epoch = 1, s_use_clock, s_flip_no;
static uint32_t *s_scratch;             /* RT_W*RT_H texels: conversions, decodes */
static size_t s_scratch_n;

static struct {
    uint32_t flips, draws, writebacks, uploads, decodes, bc_uploads, rt_tex, presents, taken;
    uint32_t rc_own, rc_generic;        /* combiner draws by a shader of their own / the general one */
    uint32_t wb_why[4];                 /* texture, dropped target, sync, present */
    uint64_t wb_bytes, up_bytes;
    double stall_ms;
    DWORD last_tick;
} s_st;
static double s_qpc_ms;

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static uint32_t *scratch(size_t texels)
{
    if (texels > s_scratch_n) {
        uint32_t *p = (uint32_t *)realloc(s_scratch, texels * 4);
        if (!p)
            return NULL;
        s_scratch = p;
        s_scratch_n = texels;
    }
    return s_scratch;
}

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * s_qpc_ms;
}

#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static uint32_t pack565(uint32_t argb)
{
    return ((argb >> 8) & 0xF800u) | ((argb >> 5) & 0x07E0u) | ((argb >> 3) & 0x001Fu);
}

static uint32_t expand565(uint32_t t)
{
    return 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
         | ((t & 0x001Fu) << 3);
}

static int ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1)
{
    return a0 < b1 && b0 < a1;
}

/* ---- colour surfaces -------------------------------------------------- */

static uint32_t rt_row_bytes(const GpuRt *r) { return r->W * r->bpp; }
static uint32_t rt_end(const GpuRt *r) { return r->va + r->H * r->pitch; }

/* The guest bytes the dirty rectangle covers. A target's whole span is wider
 * than what was drawn: Fallout's 512-wide HUD target reaches 464 rows down
 * (one of its clips does), over the palettised icons it then samples, and
 * testing the span wrote it back -- a GPU round trip -- for every icon. */
static uint32_t rt_dirty_lo(const GpuRt *r)
{
    return r->va + r->dy0 * r->pitch + r->dx0 * r->bpp;
}

static uint32_t rt_dirty_hi(const GpuRt *r)
{
    return r->dy1 ? r->va + (r->dy1 - 1) * r->pitch + r->dx1 * r->bpp : r->va;
}

/* ---- the internal scale: rows up, blocks down ---------------------------- */

/* bps_up: a native texel drawn over its s_sx x s_sy block of the target.
 * bps_down: the block averaged back to one texel -- the anti-aliased colour
 * guest RAM gets; also the SSAA reduction for the presenter. In
 * nv2a_gpu_shaders.hlsl, compiled at build time (k_bvs, k_bps_up, k_bps_down). */

static ID3D11BlendState *blend_state(int enable, uint32_t src, uint32_t dst,
                                     uint32_t eq, UINT8 mask);
static void bind_pipeline(void);

/* A full-target triangle through `ps`, reading `src`, into `rtv` (vw x vh),
 * inside `rc`; then the rasteriser's pipeline back. */
static void blit(ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *src,
                 ID3D11PixelShader *ps, uint32_t vw, uint32_t vh, const D3D11_RECT *rc,
                 uint32_t bx, uint32_t by)
{
    static const float zero[4] = { 0, 0, 0, 0 };
    ID3D11ShaderResourceView *none = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    float bk[4];

    bk[0] = (float)bx; bk[1] = (float)by; bk[2] = bk[3] = 0.0f;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_blit, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, bk, sizeof bk);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_blit, 0);
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)vw; vp.Height = (float)vh;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    /* Target first: a view of the source still bound as the target would be
     * dropped. */
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx,
        blend_state(0, 0, 0, 0, D3D11_COLOR_WRITE_ENABLE_ALL), zero, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, rc);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_bvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 3, 1, &s_cb_blit);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    bind_pipeline();
}

/* The multisampled target into the one that is read. */
static void rt_resolve(GpuRt *r)
{
    if (r->mtex && r->stale) {
        GPROF_MARK(GP_RESOLVE, r->va);
        GPROF_COUNT(GPC_RESOLVES, 1);
        GPROF_COUNT(GPC_RESOLVE_BYTES, (uint64_t)RT_W * s_sx * s_gh * s_sy * 4 * s_msaa);
        ID3D11DeviceContext_ResolveSubresource(s_ctx, (ID3D11Resource *)r->tex, 0,
                                               (ID3D11Resource *)r->mtex, 0,
                                               DXGI_FORMAT_B8G8R8A8_UNORM);
        r->stale = 0;
    }
}

/* Native rectangle (x0,y0)-(x1,y1) of target r into s_staging at (x0,y0). */
static void rt_read_native(GpuRt *r, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    D3D11_BOX box;
    ID3D11Resource *from = (ID3D11Resource *)r->tex;

    GPROF_MARK(GP_WRITEBACK, r->va);
    GPROF_COUNT(GPC_WB_BYTES, (uint64_t)(x1 - x0) * (y1 - y0) * 4);
    box.left = x0; box.right = x1; box.top = y0; box.bottom = y1;
    box.front = 0; box.back = 1;
    if (s_scaled) {
        D3D11_RECT rc;
        rt_resolve(r);
        GPROF_MARK(GP_WRITEBACK, r->va);
        rc.left = (LONG)x0; rc.top = (LONG)y0; rc.right = (LONG)x1; rc.bottom = (LONG)y1;
        blit(s_down_rtv, r->srv, s_bps_down, RT_W, RT_H, &rc, s_sx, s_sy);
        from = (ID3D11Resource *)s_down;
    }
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_staging, 0,
                                              x0, y0, 0, from, 0, &box);
}

/* SSAA: the presented frame, ow x oh texels at the shown scale, each the
 * average of its s_ssaa x s_ssaa block of the target -- into s_ss. */
static void ssaa_reduce(GpuRt *r, uint32_t ow, uint32_t oh)
{
    D3D11_RECT rc;
    rt_resolve(r);
    GPROF_MARK(GP_SSAA, r->va);
    rc.left = 0; rc.top = 0;
    rc.right = (LONG)(ow < RT_W * s_ox ? ow : RT_W * s_ox);
    rc.bottom = (LONG)(oh < RT_H * s_oy ? oh : RT_H * s_oy);
    blit(s_ss_rtv, r->srv, s_bps_down, RT_W * s_ox, RT_H * s_oy, &rc, s_ssaa, s_ssaa);
}

/* Guest rows [y0, y1) into the target and the shadow. */
static void rt_upload_rows(GpuRt *r, uint32_t y0, uint32_t y1)
{
    const uint8_t *src = guest(r->va) + (size_t)y0 * r->pitch;
    uint32_t rb = rt_row_bytes(r), y, x;
    ID3D11Resource *to = (ID3D11Resource *)(s_scaled ? s_up : r->tex);
    D3D11_BOX box;

    if (y0 >= y1)
        return;
    GPROF_MARK(GP_UPLOAD, r->va);
    GPROF_COUNT(GPC_UPLOAD_BYTES, (uint64_t)(y1 - y0) * rb);
    box.left = 0; box.right = r->W; box.top = y0; box.bottom = y1;
    box.front = 0; box.back = 1;
    if (r->bpp == 4) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, to, 0, &box, src, r->pitch, 0);
    } else {
        uint32_t *tmp = scratch((size_t)r->W * (y1 - y0));
        if (!tmp)
            return;
        for (y = 0; y < y1 - y0; y++) {
            const uint16_t *s16 = (const uint16_t *)(src + (size_t)y * r->pitch);
            for (x = 0; x < r->W; x++)
                tmp[(size_t)y * r->W + x] = expand565(s16[x]);
        }
        ID3D11DeviceContext_UpdateSubresource(s_ctx, to, 0, &box, tmp, r->W * 4, 0);
    }
    if (s_scaled) {                     /* each native texel over its block */
        D3D11_RECT rc;
        rc.left = 0; rc.right = (LONG)(r->W * s_sx);
        rc.top = (LONG)(y0 * s_sy); rc.bottom = (LONG)(y1 * s_sy);
        blit(r->rtv, s_up_srv, s_bps_up, RT_W * s_sx, s_gh * s_sy, &rc, s_sx, s_sy);
        r->stale = 1;
    }
    for (y = y0; y < y1; y++)
        memcpy(r->shadow + (size_t)y * rb, src + (size_t)(y - y0) * r->pitch, rb);
    s_st.uploads++;
    s_st.up_bytes += (uint64_t)(y1 - y0) * rb;
}

/* First use in an epoch: upload the rows guest RAM changed. */
static int rt_check(GpuRt *r)
{
    const uint8_t *g;
    uint32_t rb = rt_row_bytes(r), y, run = 0;
    int in_run = 0;

    if (r->epoch_checked == s_epoch && !r->need_full)
        return 1;
    /* Dirty: the target is newer than guest RAM until it is written back
     * (sync does that before an epoch ends), so guest rows must not be
     * uploaded over it. */
    if (r->dirty && !r->need_full)
        return 1;
    if (!nv2a_backend_surface_ok(r->va, r->H * r->pitch))
        return 0;
    r->epoch_checked = s_epoch;
    if (r->need_full) {
        r->need_full = 0;
        rt_upload_rows(r, 0, r->H);
        return 1;
    }
    g = guest(r->va);
    for (y = 0; y < r->H; y++) {
        int differs = memcmp(g + (size_t)y * r->pitch, r->shadow + (size_t)y * rb, rb) != 0;
        if (differs && !in_run) {
            in_run = 1;
            run = y;
        } else if (!differs && in_run) {
            in_run = 0;
            rt_upload_rows(r, run, y);
        }
    }
    if (in_run)
        rt_upload_rows(r, run, r->H);
    return 1;
}

static void tex_invalidate_range(uint32_t lo, uint32_t hi);

/* A mapped copy of rectangle (x0,y0)-(x1,y1) of target r into guest RAM and
 * the shadow; then everything that mirrors those bytes looks again. */
static void rt_store(GpuRt *r, const D3D11_MAPPED_SUBRESOURCE *m, uint32_t x0,
                     uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint32_t y, x, rb = rt_row_bytes(r);
    int i;

    for (y = y0; y < y1; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)m->pData
                                                 + (size_t)y * m->RowPitch);
        uint8_t *dst = guest(r->va) + (size_t)y * r->pitch;
        uint8_t *sh = r->shadow + (size_t)y * rb;
        if (r->bpp == 4) {
            memcpy(dst + x0 * 4, src + x0, (x1 - x0) * 4);
            memcpy(sh + x0 * 4, src + x0, (x1 - x0) * 4);
        } else {
            uint16_t *d16 = (uint16_t *)dst, *s16 = (uint16_t *)sh;
            for (x = x0; x < x1; x++)
                d16[x] = s16[x] = (uint16_t)pack565(src[x]);
        }
    }
    s_st.writebacks++;
    s_st.wb_bytes += (uint64_t)(y1 - y0) * (x1 - x0) * r->bpp;
    tex_invalidate_range(r->va, rt_end(r));
    for (i = 0; i < MAX_RTS; i++)
        if (&s_rt[i] != r && s_rt[i].used
            && ranges_overlap(s_rt[i].va, rt_end(&s_rt[i]), r->va, rt_end(r)))
            s_rt[i].epoch_checked = EPOCH_NONE;
}

/* The dirty rectangle, clipped to the target; 0 when there is nothing to
 * write or it is not memory a surface may use. Clears dirty. */
static int rt_take_dirty(GpuRt *r, uint32_t *x0, uint32_t *y0, uint32_t *x1,
                         uint32_t *y1)
{
    if (!r->dirty)
        return 0;
    r->dirty = 0;
    *x0 = r->dx0; *y0 = r->dy0;
    *x1 = r->dx1 < r->W ? r->dx1 : r->W;
    *y1 = r->dy1 < r->H ? r->dy1 : r->H;
    if (*x0 >= *x1 || *y0 >= *y1)
        return 0;
    return nv2a_backend_surface_ok(r->va + *y0 * r->pitch, (*y1 - *y0) * r->pitch);
}

/* Has the title taken a dirty target's memory for something else?
 *
 * Between writebacks nothing here writes guest RAM under the dirty
 * rectangle, so there it still equals the shadow -- unless the CPU (or a
 * texture upload) wrote it. That happens when the title frees a target and
 * the heap hands the bytes out again (RECOMP_HEAP_RECLAIM): Fallout's shadow
 * atlas moves between allocations, and the target left behind stays dirty
 * for good. Writing it back then laid stale pixels over the new owner -- a
 * texture, vertices, objects -- and sampling it served the stale pixels in
 * place of the new texels. Such a target drops what it drew and takes guest
 * RAM as it is. Compared once an epoch where it is sampled or drawn into --
 * that happens per draw -- but before every writeback: a writeback is rare,
 * and the CPU may have taken the bytes since this epoch's look. */
static int rt_taken(GpuRt *r, int every_time)
{
    uint32_t rb = rt_row_bytes(r), y, x0, x1, y1;
    const uint8_t *g = guest(r->va);

    if (!r->dirty || r->need_full || s_compare || (!every_time && r->taken_epoch == s_epoch))
        return 0;
    r->taken_epoch = s_epoch;
    x0 = r->dx0;
    x1 = r->dx1 < r->W ? r->dx1 : r->W;
    y1 = r->dy1 < r->H ? r->dy1 : r->H;
    if (x0 >= x1 || r->dy0 >= y1
        || !nv2a_backend_surface_ok(r->va + r->dy0 * r->pitch, (y1 - r->dy0) * r->pitch))
        return 0;
    for (y = r->dy0; y < y1; y++)
        if (memcmp(g + (size_t)y * r->pitch + x0 * r->bpp,
                   r->shadow + (size_t)y * rb + x0 * r->bpp, (x1 - x0) * r->bpp) != 0) {
            r->dirty = 0;
            r->need_full = 1;           /* rt_check uploads guest RAM whole */
            r->epoch_checked = EPOCH_NONE;
            s_st.taken++;
            return 1;
        }
    return 0;
}

/* tests/nv2a_gpu_backend builds with NV2A_GPU_TEST_HOOKS: the next
 * nv2a_gpu_test_fail_maps readbacks fail, as a Map can on a real device and
 * never does on WARP. Not in a title's build. */
#ifdef NV2A_GPU_TEST_HOOKS
int nv2a_gpu_test_fail_maps;
#define TEST_MAP_FAILS() (nv2a_gpu_test_fail_maps > 0 ? (nv2a_gpu_test_fail_maps--, 1) : 0)
int nv2a_gpu_test_bc_uploads;           /* DXT textures uploaded as BC, so far */
#define TEST_BC_UPLOAD() (nv2a_gpu_test_bc_uploads++)
#else
#define TEST_MAP_FAILS() 0
#define TEST_BC_UPLOAD() ((void)0)
#endif

/* The dirty rectangle into guest RAM and the shadow, now. */
static void rt_writeback(GpuRt *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t x0, x1, y0, y1;
    double t0;

    if (rt_taken(r, 1) || !rt_take_dirty(r, &x0, &y0, &x1, &y1))
        return;
    rt_read_native(r, x0, y0, x1, y1);
    t0 = now_ms();
    if (TEST_MAP_FAILS()
        || FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_staging, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        r->dirty = 1;                   /* still newer than guest RAM: keep it so */
        return;
    }
    s_st.stall_ms += now_ms() - t0;
    GPROF_HOST(GPH_MAP_STALL_MS, now_ms() - t0);
    rt_store(r, &m, x0, y0, x1, y1);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
}

static GpuRt *s_last_acquired;          /* rt_acquire's previous answer */

static void rt_release(GpuRt *r)
{
    if (r == s_last_acquired)
        s_last_acquired = NULL;
    REL(r->srv);
    REL(r->rtv);
    REL(r->mtex);
    REL(r->tex);
    free(r->shadow);
    memset(r, 0, sizeof *r);
}

static void rt_writeback(GpuRt *r);

/* A surface reaches below s_gh: every target goes back to guest RAM and
 * away, the depth buffers with them (D3D11 wants a target and its depth
 * buffer the same size), and all of them are made again at the new height.
 * Once or twice early on; what was only on the GPU -- depth -- is lost then. */
static void grow_targets(uint32_t h)
{
    int i;

    h = (h + 63u) & ~63u;
    if (h > RT_H)
        h = RT_H;
    fprintf(stderr, "[GPURAST] targets grow to %ux%u (%ux%u at the internal scale)\n",
            RT_W, h, RT_W * s_sx, h * s_sy);
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used) {
            rt_writeback(&s_rt[i]);
            rt_release(&s_rt[i]);
        }
    for (i = 0; i < MAX_ZBUFS; i++) {
        REL(s_z[i].dsv);
        REL(s_z[i].tex);
        s_z[i].used = 0;
    }
    s_gh = h;
}

static void rt_grow(GpuRt *r, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (x0 >= x1 || y0 >= y1)
        return;
    if (!r->dirty) {
        r->dirty = 1;
        r->dx0 = x0; r->dy0 = y0; r->dx1 = x1; r->dy1 = y1;
        return;
    }
    if (x0 < r->dx0) r->dx0 = x0;
    if (y0 < r->dy0) r->dy0 = y0;
    if (x1 > r->dx1) r->dx1 = x1;
    if (y1 > r->dy1) r->dy1 = y1;
}

/* A target or depth buffer that could not be made: said once. */
static void say_no_mem(uint32_t w, uint32_t h)
{
    static int said_mem;
    if (said_mem)
        return;
    said_mem = 1;
    fprintf(stderr, "[GPURAST] a %ux%u target (%ux MSAA) could not be made: "
                    "out of video memory? lower the scale, SSAA or MSAA\n", w, h, s_msaa);
}

/* The target for a surface, matched to guest RAM. NULL: cannot draw. */
static GpuRt *rt_acquire(const Nv2aSurface *s)
{
    static int said_clamp, said_bad;
    uint32_t bpp = s->bytes_per_pixel, W, H, i;
    GpuRt *r = NULL, *victim = NULL;

    if ((bpp != 2 && bpp != 4) || !s->pitch || !s->color_va)
        return NULL;
    W = s->pitch / bpp;
    H = s->clip_y + s->height;
    if (W > RT_W || H > RT_H) {
        if (!said_clamp++)
            fprintf(stderr, "[GPURAST] surface 0x%08X %ux%u clamped to %ux%u\n",
                    s->color_va, W, H, RT_W, RT_H);
        if (W > RT_W) W = RT_W;
        if (H > RT_H) H = RT_H;
    }
    if (!W || !H)
        return NULL;
    if (H > s_gh)
        grow_targets(H);
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used && s_rt[i].va == s->color_va && s_rt[i].pitch == s->pitch
            && s_rt[i].bpp == bpp) {
            r = &s_rt[i];
            break;
        }
    /* Another dirty target over the same bytes goes back first, and away. */
    for (i = 0; i < MAX_RTS; i++) {
        GpuRt *o = &s_rt[i];
        if (!o->used || o == r || !o->dirty)
            continue;
        if (ranges_overlap(o->va, rt_end(o), s->color_va, s->color_va + H * s->pitch)) {
            if (!s_compare) {           /* compare: guest RAM is the software path's */
                s_st.wb_why[1]++;
                rt_writeback(o);
            }
            rt_release(o);
            if (r)
                r->epoch_checked = EPOCH_NONE;
        }
    }
    if (!r) {
        D3D11_TEXTURE2D_DESC d;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        for (i = 0; i < MAX_RTS; i++)
            if (!s_rt[i].used) { victim = &s_rt[i]; break; }
        if (!victim) {
            victim = &s_rt[0];
            for (i = 1; i < MAX_RTS; i++)
                if (s_rt[i].last_use < victim->last_use)
                    victim = &s_rt[i];
            if (!s_compare) {
                s_st.wb_why[1] += victim->dirty != 0;
                rt_writeback(victim);
            }
            rt_release(victim);
        }
        r = victim;
        memset(&d, 0, sizeof d);
        d.Width = RT_W * s_sx; d.Height = s_gh * s_sy; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        memset(&vd, 0, sizeof vd);
        vd.Format = d.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.ArraySize = 1;
        r->shadow = (uint8_t *)malloc((size_t)RT_H * RT_W * 4);
        if (!r->shadow
            || FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &r->tex))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->tex,
                                                            &vd, &r->srv))) {
            say_no_mem(d.Width, d.Height);
            rt_release(r);
            return NULL;
        }
        if (s_msaa > 1) {
            d.SampleDesc.Count = s_msaa;
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &r->mtex))) {
                say_no_mem(d.Width, d.Height);
                rt_release(r);
                return NULL;
            }
        }
        if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev,
                       (ID3D11Resource *)(r->mtex ? r->mtex : r->tex), NULL, &r->rtv))) {
            rt_release(r);
            return NULL;
        }
        r->used = 1;
        r->va = s->color_va; r->pitch = s->pitch; r->bpp = bpp;
        r->W = W; r->H = H;
        r->need_full = 1;               /* a new target is uploaded whole */
        if (g_nv2a_gprof_on)
            nv2a_gprof_target(r->va, r->W, r->H, s->swizzled != 0);
    } else if (H > r->H) {
        /* The title draws further down than before: those rows join. */
        if (!nv2a_backend_surface_ok(r->va, H * r->pitch))
            return NULL;
        rt_upload_rows(r, r->H, H);
        r->H = H;
        if (g_nv2a_gprof_on)
            nv2a_gprof_target(r->va, r->W, r->H, s->swizzled != 0);
    }
    r->swizzled = s->swizzled != 0;
    r->last_use = ++s_use_clock;
    /* Before drawing on: if the title took these bytes meanwhile, what was
     * drawn before is gone, and dropping it later would take what is drawn
     * now along with it. Only when the target changes: it is acquired for
     * every draw, and while the title keeps drawing into it it cannot have
     * freed it -- D3D waits for the fence before it lets go of a surface, so
     * its bytes change hands only after work went elsewhere. */
    if (r != s_last_acquired)
        rt_taken(r, 0);
    s_last_acquired = r;
    if (!rt_check(r)) {
        if (!said_bad++)
            fprintf(stderr, "[GPURAST] surface 0x%08X is not guest memory a"
                            " surface may use; not drawn\n", r->va);
        return NULL;
    }
    return r;
}

static GpuZ *z_acquire(uint32_t va)
{
    D3D11_TEXTURE2D_DESC d;
    GpuZ *z = NULL;
    int i;

    for (i = 0; i < MAX_ZBUFS; i++)
        if (s_z[i].used && s_z[i].va == va) {
            s_z[i].last_use = ++s_use_clock;
            return &s_z[i];
        }
    for (i = 0; i < MAX_ZBUFS; i++)
        if (!s_z[i].used) { z = &s_z[i]; break; }
    if (!z) {
        z = &s_z[0];
        for (i = 1; i < MAX_ZBUFS; i++)
            if (s_z[i].last_use < z->last_use)
                z = &s_z[i];
        REL(z->dsv);
        REL(z->tex);
        z->used = 0;
    }
    memset(&d, 0, sizeof d);
    d.Width = RT_W * s_sx; d.Height = s_gh * s_sy; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    d.SampleDesc.Count = s_msaa;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &z->tex))
        || FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)z->tex,
                                                      NULL, &z->dsv))) {
        say_no_mem(d.Width, d.Height);
        REL(z->dsv);
        REL(z->tex);
        return NULL;
    }
    GPROF_MARK(GP_Z_INIT, va);
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                              1.0f, 0);
    z->used = 1;
    z->va = va;
    z->last_use = ++s_use_clock;
    return z;
}

/* ---- textures ---------------------------------------------------------- */

static uint64_t hash_bytes(uint64_t h, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 31;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static void tex_invalidate_range(uint32_t lo, uint32_t hi)
{
    int i;
    for (i = 0; i < MAX_TEXS; i++)
        if (s_tex[i].used && ranges_overlap(s_tex[i].lo, s_tex[i].hi, lo, hi))
            s_tex[i].epoch_checked = EPOCH_NONE;
}

static void tex_release(GpuTex *t)
{
    REL(t->srv);
    REL(t->tex);
    memset(t, 0, sizeof *t);
}

static uint64_t tex_hash(const Nv2aTexture *d, uint32_t bytes)
{
    uint64_t h = 0xCBF29CE484222325ull ^ bytes;
    h = hash_bytes(h, guest(d->offset), bytes);
    if (d->color == 0x0B && d->palette && nv2a_backend_surface_ok(d->palette, 1024))
        h = hash_bytes(h, guest(d->palette), 1024);
    return h;
}

/* The BC format a DXT texture uploads to as it is, or DXGI_FORMAT_UNKNOWN.
 *
 * DXT1/3/5 are BC1/2/3 block for block (in the same order, a row of blocks
 * after another), so the GPU decodes them and the CPU does not -- a new DXT
 * texture used to cost the executor's thread a whole decode mid-frame. The
 * GPU's colours are within a step or two of d3d8_dxt_decode_texel's (both
 * round the 5:6:5 endpoints and the thirds their own way; the test holds them
 * to +-2). DXT1's three-colour mode, index 3 transparent black, and DXT3/5
 * colour always four-colour, are BC1/2/3's rules too.
 * D3D11 wants a BC texture's sides in whole blocks; a cube stays on the CPU,
 * the face stride being the title's to choose. */
static DXGI_FORMAT tex_bc_format(const Nv2aTexture *d)
{
    if (d->cube || (d->width & 3) || (d->height & 3))
        return DXGI_FORMAT_UNKNOWN;
    switch (d->color) {
    case 0x0C: return DXGI_FORMAT_BC1_UNORM;
    case 0x0E: return DXGI_FORMAT_BC2_UNORM;
    case 0x0F: return DXGI_FORMAT_BC3_UNORM;
    default:   return DXGI_FORMAT_UNKNOWN;
    }
}

/* Decode every face into the entry's texture array. */
static int tex_decode(GpuTex *t, const Nv2aTexture *d)
{
    uint32_t faces = d->cube ? 6 : 1, f;
    uint32_t *buf;
    DXGI_FORMAT bc = tex_bc_format(d);

    if (!t->tex) {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        memset(&td, 0, sizeof td);
        td.Width = d->width; td.Height = d->height; td.MipLevels = 1;
        td.ArraySize = faces;
        td.Format = bc != DXGI_FORMAT_UNKNOWN ? bc : DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        memset(&vd, 0, sizeof vd);
        vd.Format = td.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.ArraySize = faces;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &t->tex))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t->tex,
                                                            &vd, &t->srv))) {
            REL(t->srv);
            REL(t->tex);
            return 0;
        }
    }
    GPROF_MARK(GP_TEX_UPLOAD, 0);
    GPROF_COUNT(GPC_TEX_BYTES, bc != DXGI_FORMAT_UNKNOWN
                ? (uint64_t)(d->width / 4) * (d->height / 4) * d3d8_format_dxt_block_bytes(d->color)
                : (uint64_t)d->width * d->height * 4 * faces);
    if (bc != DXGI_FORMAT_UNKNOWN) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)t->tex, 0, NULL,
                                              guest(d->offset),
                                              (d->width / 4)
                                              * d3d8_format_dxt_block_bytes(d->color), 0);
        s_st.bc_uploads++;
        TEST_BC_UPLOAD();
        return 1;
    }
    buf = scratch((size_t)d->width * d->height);
    if (!buf)
        return 0;
    for (f = 0; f < faces; f++) {
        if (!nv2a_backend_decode_texture_face(d, f, buf))
            return 0;
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)t->tex,
                                              f /* mip 0 of slice f */, NULL,
                                              buf, d->width * 4, 0);
    }
    s_st.decodes++;
    return 1;
}

/* The view for a stage's texture, or NULL when it cannot be sampled. */
static ID3D11ShaderResourceView *tex_resolve(const Nv2aTexture *d)
{
    GpuTex *t = NULL, *victim = NULL;
    uint32_t bytes;
    int i;

    if (!d->width || !d->height || d->width > 4096 || d->height > 4096)
        return NULL;
    for (i = 0; i < MAX_TEXS; i++) {
        GpuTex *e = &s_tex[i];
        if (!e->used) {
            if (!victim)
                victim = e;
            continue;
        }
        if (e->offset == d->offset && e->color == d->color && e->width == d->width
            && e->height == d->height && e->pitch == d->pitch
            && e->palette == d->palette && e->cube == d->cube
            && e->levels == d->levels) {
            t = e;
            break;
        }
    }
    if (t) {
        t->last_flip = s_flip_no;
        if (t->epoch_checked == s_epoch)
            return t->ok ? t->srv : NULL;
        bytes = t->hi - t->lo;
        if (!nv2a_backend_surface_ok(d->offset, bytes)) {
            t->epoch_checked = s_epoch;
            t->ok = 0;
            return NULL;
        }
        {
            uint64_t h = tex_hash(d, bytes);
            t->epoch_checked = s_epoch;
            if (h != t->hash || !t->ok) {
                t->hash = h;
                t->ok = tex_decode(t, d);
            }
        }
        return t->ok ? t->srv : NULL;
    }
    if (!victim) {                      /* full: the least recently used goes */
        victim = &s_tex[0];
        for (i = 1; i < MAX_TEXS; i++)
            if (s_tex[i].last_flip < victim->last_flip)
                victim = &s_tex[i];
        tex_release(victim);
    }
    t = victim;
    bytes = nv2a_backend_texture_bytes(d);
    t->used = 1;
    t->offset = d->offset; t->color = d->color; t->width = d->width;
    t->height = d->height; t->pitch = d->pitch; t->palette = d->palette;
    t->cube = d->cube; t->levels = d->levels;
    t->lo = d->offset; t->hi = d->offset + bytes;
    t->last_flip = s_flip_no;
    t->epoch_checked = s_epoch;
    if (!bytes || !nv2a_backend_surface_ok(d->offset, bytes)) {
        t->ok = 0;
        return NULL;
    }
    t->hash = tex_hash(d, bytes);
    t->ok = tex_decode(t, d);
    return t->ok ? t->srv : NULL;
}

/* ---- pipeline state ---------------------------------------------------- */

static D3D11_COMPARISON_FUNC gl_cmp(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default:    return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP gl_sop(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

/* Depth and stencil for an XF batch; NULL key fields mean "off". */
static ID3D11DepthStencilState *ds_state(const Nv2aRenderState *rs, int depth_on)
{
    uint32_t key[12];
    D3D11_DEPTH_STENCIL_DESC d;
    ID3D11DepthStencilState *st = NULL;
    int i;

    memset(key, 0, sizeof key);
    if (depth_on) {
        key[0] = 1;
        key[1] = rs->depth_test_enable ? 1u : 0u;
        key[2] = rs->depth_test_enable ? rs->depth_func : 0u;
        key[3] = rs->depth_write ? 1u : 0u;
        key[4] = rs->stencil_enable ? 1u : 0u;
        if (rs->stencil_enable) {
            key[5] = rs->stencil_func;
            key[6] = rs->stencil_func_mask & 0xFF;
            key[7] = rs->stencil_write_mask & 0xFF;
            key[8] = rs->stencil_fail;
            key[9] = rs->stencil_zfail;
            key[10] = rs->stencil_zpass;
        }
    }
    for (i = 0; i < s_nds; i++)
        if (!memcmp(s_ds[i].key, key, sizeof key))
            return s_ds[i].s;
    memset(&d, 0, sizeof d);
    if (key[0]) {
        if (key[1]) {
            d.DepthEnable = TRUE;
            d.DepthFunc = gl_cmp(key[2]);
            d.DepthWriteMask = key[3] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        } else if (key[3]) {            /* the software path writes depth untested */
            d.DepthEnable = TRUE;
            d.DepthFunc = D3D11_COMPARISON_ALWAYS;
            d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        }
        if (key[4]) {
            d.StencilEnable = TRUE;
            d.StencilReadMask = (UINT8)key[6];
            d.StencilWriteMask = (UINT8)key[7];
            d.FrontFace.StencilFunc = gl_cmp(key[5]);
            d.FrontFace.StencilFailOp = gl_sop(key[8]);
            d.FrontFace.StencilDepthFailOp = gl_sop(key[9]);
            d.FrontFace.StencilPassOp = gl_sop(key[10]);
            d.BackFace = d.FrontFace;
        }
    }
    if (!d.DepthEnable) {
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    }
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &d, &st)))
        return NULL;
    if (s_nds < MAX_DS_STATES) {
        memcpy(s_ds[s_nds].key, key, sizeof key);
        s_ds[s_nds++].s = st;
    } else {
        /* Full: replace the last slot. */
        REL(s_ds[MAX_DS_STATES - 1].s);
        memcpy(s_ds[MAX_DS_STATES - 1].key, key, sizeof key);
        s_ds[MAX_DS_STATES - 1].s = st;
    }
    return st;
}

static D3D11_BLEND gl_blend(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP gl_blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static UINT8 write_mask(uint32_t cm)
{
    UINT8 m = 0;
    if (cm & 0x01000000u) m |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    if (cm & 0x00010000u) m |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (cm & 0x00000100u) m |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (cm & 0x00000001u) m |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    return m;
}

static ID3D11BlendState *blend_state(int enable, uint32_t src, uint32_t dst,
                                     uint32_t eq, UINT8 mask)
{
    uint32_t key[5];
    D3D11_BLEND_DESC d;
    ID3D11BlendState *st = NULL;
    int i;

    key[0] = enable ? 1u : 0u;
    key[1] = enable ? src : 0u;
    key[2] = enable ? dst : 0u;
    key[3] = enable ? eq : 0u;
    key[4] = mask;
    for (i = 0; i < s_nbl; i++)
        if (!memcmp(s_bl[i].key, key, sizeof key))
            return s_bl[i].s;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = enable ? TRUE : FALSE;
    d.RenderTarget[0].SrcBlend = enable ? gl_blend(src, 0) : D3D11_BLEND_ONE;
    d.RenderTarget[0].DestBlend = enable ? gl_blend(dst, 0) : D3D11_BLEND_ZERO;
    d.RenderTarget[0].BlendOp = enable ? gl_blend_op(eq) : D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].SrcBlendAlpha = enable ? gl_blend(src, 1) : D3D11_BLEND_ONE;
    d.RenderTarget[0].DestBlendAlpha = enable ? gl_blend(dst, 1) : D3D11_BLEND_ZERO;
    d.RenderTarget[0].BlendOpAlpha = d.RenderTarget[0].BlendOp;
    d.RenderTarget[0].RenderTargetWriteMask = mask;
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &st)))
        return NULL;
    if (s_nbl < MAX_BLEND_STATES) {
        memcpy(s_bl[s_nbl].key, key, sizeof key);
        s_bl[s_nbl++].s = st;
    } else {
        REL(s_bl[MAX_BLEND_STATES - 1].s);
        memcpy(s_bl[MAX_BLEND_STATES - 1].key, key, sizeof key);
        s_bl[MAX_BLEND_STATES - 1].s = st;
    }
    return st;
}

/* ---- the combiners' states (nv2a_gpu_rc_key.h) ------------------------------
 *
 * A state with a pixel shader of its own (compiled at build time for the
 * states a game uses) is drawn by it; every other state, or all of them with
 * RECOMP_GPU_RASTER_RC_GENERIC=1, by the general ps_rc. One table holds both
 * the own states and the ones met without a shader, with their draws and
 * pixels (RECOMP_GPU_RASTER_RC_STATS). The key is computed only when it is
 * of use: own shaders exist, or the statistics are on. */
#ifndef NV2A_GPU_RC_KEYS
#define NV2A_GPU_RC_KEYS 0
#endif
#define RC_SLOTS 256
#define RC_MISS_LOGGED 64               /* states without a shader named in the log */
#define RC_TOP 32                       /* lines of the statistics */

typedef struct {
    int used;
    uint32_t w[NV2A_RC_KEY_WORDS];
    ID3D11PixelShader *ps;              /* NULL: no shader of its own */
    uint64_t draws, px;
} RcState;

static RcState s_rcs[RC_SLOTS];
static uint32_t s_rc_nown;              /* states with a shader of their own */
static uint32_t s_rc_nmiss;             /* distinct states met without one */
static int s_rc_stats, s_rc_generic;
static int s_rc_last = -1;              /* the table slot pick_ps chose for this draw */
#ifdef NV2A_GPU_TEST_HOOKS
static uint64_t s_rc_own_all, s_rc_gen_all;
static uint32_t s_rc_lastkey[NV2A_RC_KEY_WORDS];
static int s_rc_weak_hash;              /* test: the slot from the first 5 words only */
#endif

/* The slot of key w, added when new (*added = 1); -1 when the table is full. */
static int rc_slot(const uint32_t w[NV2A_RC_KEY_WORDS], int *added)
{
    uint32_t h = nv2a_rc_key_hash(w), i, n;

#ifdef NV2A_GPU_TEST_HOOKS
    if (s_rc_weak_hash) {               /* keys alike in 5 words start in one slot */
        int k;
        h = 2166136261u;
        for (k = 0; k < 5; k++) {
            h ^= w[k];
            h *= 16777619u;
        }
    }
#endif
    i = h % RC_SLOTS;

    *added = 0;
    for (n = 0; n < RC_SLOTS; n++, i = (i + 1) % RC_SLOTS) {
        if (!s_rcs[i].used) {
            s_rcs[i].used = 1;
            memcpy(s_rcs[i].w, w, sizeof s_rcs[i].w);
            *added = 1;
            return (int)i;
        }
        if (!memcmp(s_rcs[i].w, w, sizeof s_rcs[i].w))
            return (int)i;
    }
    return -1;
}

/* A state without a shader of its own met for the first time: named in the
 * log, so the game's key file can be completed from it. */
static void rc_note_miss(const uint32_t w[NV2A_RC_KEY_WORDS])
{
    char key[512];

    s_rc_nmiss++;
    if (!s_rc_nown)
        return;                         /* no own shaders: every state is a miss */
    if (s_rc_nmiss <= RC_MISS_LOGGED && nv2a_rc_key_format(w, key, sizeof key) > 0)
        fprintf(stderr, "[GPURAST] combiner state with no shader of its own (the generic"
                        " one draws it): %s\n", key);
    else if (s_rc_nmiss == RC_MISS_LOGGED + 1)
        fprintf(stderr, "[GPURAST] further combiner states without a shader are not listed\n");
}

static void rc_states_release(void)
{
    int i;

    for (i = 0; i < RC_SLOTS; i++)
        REL(s_rcs[i].ps);
    memset(s_rcs, 0, sizeof s_rcs);
    s_rc_nown = s_rc_nmiss = 0;
    s_rc_last = -1;
}

/* The cumulative table, top RC_TOP by pixels, into the report's block. */
static void rc_report(TextBlock *tb)
{
    int order[RC_SLOTS], n = 0, i, j;
    double total = 0.0;

    for (i = 0; i < RC_SLOTS; i++)
        if (s_rcs[i].used && s_rcs[i].draws) {
            total += (double)s_rcs[i].px;
            order[n++] = i;
        }
    for (j = 0; j < n && j < RC_TOP; j++) {     /* selection: the RC_TOP largest first */
        int best = j, k;
        for (k = j + 1; k < n; k++)
            if (s_rcs[order[k]].px > s_rcs[order[best]].px)
                best = k;
        i = order[j]; order[j] = order[best]; order[best] = i;
    }
    if (n > RC_TOP)
        n = RC_TOP;
    for (j = 0; j < n; j++) {
        char key[512];
        const RcState *r = &s_rcs[order[j]];
        if (nv2a_rc_key_format(r->w, key, sizeof key) < 0)
            continue;
        text_block_printf(tb, "[GPURAST] rc state %u: draws %llu, %.2f Mpx (%.1f%%), %s: %s\n",
                          (unsigned)order[j], (unsigned long long)r->draws,
                          (double)r->px / 1.0e6, total > 0.0 ? 100.0 * (double)r->px / total : 0.0,
                          r->ps ? "own" : "generic", key);
    }
}

/* The pixel shader for a draw or clear with these constants. */
static ID3D11PixelShader *pick_ps(const PsConsts *pc)
{
    s_rc_last = -1;
#ifdef NV2A_GPU_TEST_HOOKS
    if (s_ps_override)
        return s_ps_override;
#endif
    if (pc->at[3] == 0) {               /* screen space; a colour clear too */
        PS_PICKED(0);
        return s_ps_screen;
    }
    if (s_ps_flat)                      /* RECOMP_GPU_PROF=...,flatps: the ablation */
        return s_ps_flat;
    if (pc->ctl[3] & PSF_RC) {
        int rt = 0, other = 0, st, v;
        if (!s_rc_generic && (s_rc_nown || s_rc_stats)) {
            uint32_t w[NV2A_RC_KEY_WORDS], tf[4];
            int slot, added;
            for (st = 0; st < 4; st++)
                tf[st] = pc->tex[st][2];
            nv2a_rc_key_make(w, pc->cicw, pc->aicw, pc->cocw, pc->aocw, pc->ctl[0],
                             pc->ctl[1], pc->fin[0], pc->fin[1], tf);
#ifdef NV2A_GPU_TEST_HOOKS
            memcpy(s_rc_lastkey, w, sizeof w);
#endif
            slot = rc_slot(w, &added);
            if (slot >= 0) {
                s_rcs[slot].draws++;
                s_rc_last = slot;
                if (s_rcs[slot].ps) {
                    s_st.rc_own++;
#ifdef NV2A_GPU_TEST_HOOKS
                    s_rc_own_all++;
#endif
                    return s_rcs[slot].ps;
                }
                if (added)
                    rc_note_miss(w);
            }
        }
        s_st.rc_generic++;
#ifdef NV2A_GPU_TEST_HOOKS
        s_rc_gen_all++;
#endif
        for (st = 0; st < 4; st++) {
            uint32_t fl = pc->tex[st][2];
            if (!(fl & TXF_VALID))
                continue;
            if ((fl & TXF_RT) && !(fl & TXF_SWZ))
                rt = 1;
            else
                other = 1;
        }
        v = rt && other ? 0 : rt ? 2 : 1;
        PS_PICKED(2 + v);
        return s_ps_rc[v];
    }
    PS_PICKED(1);
    return s_ps_ffp;
}

static void use_ps(ID3D11PixelShader *ps)
{
    if (ps != s_ps_bound) {
        ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
        s_ps_bound = ps;
    }
}

static void set_consts(const VsConsts *v, const PsConsts *p)
{
    D3D11_MAPPED_SUBRESOURCE m;

    if (!s_vsc_valid || memcmp(v, &s_vsc_last, sizeof *v)) {
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_vs, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, v, sizeof *v);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_vs, 0);
            s_vsc_last = *v;
            s_vsc_valid = 1;
        }
    }
    if (!s_psc_valid || memcmp(p, &s_psc_last, sizeof *p)) {
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_ps, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, p, sizeof *p);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_ps, 0);
            s_psc_last = *p;
            s_psc_valid = 1;
        }
    }
}

/* ---- occlusion queries (visibility tests) ------------------------------- */

static void query_resolve_one(ID3D11Query *q)
{
    UINT64 n = 0;
    HRESULT hr;
    double t0 = g_nv2a_gprof_on ? now_ms() : 0.0;

    while ((hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)q, &n,
                                             sizeof n, 0)) == S_FALSE)
        YieldProcessor();
    GPROF_HOST(GPH_REPORT_WAIT_MS, now_ms() - t0);
    if (hr == S_OK)                     /* samples at the internal scale: divided into
                                         * pixels once, in gpu_report -- per query, each
                                         * lost up to a pixel */
        s_zpass_acc += n;
    s_qpool[s_qfree++] = q;
}

static void query_resolve_all(void)
{
    int i;
    for (i = 0; i < s_npend; i++)
        query_resolve_one(s_qpend[i]);
    s_npend = 0;
}

static ID3D11Query *query_begin(void)
{
    ID3D11Query *q = NULL;

    if (s_npend == MAX_QUERIES)
        query_resolve_all();
    if (s_qfree > 0) {
        q = s_qpool[--s_qfree];
    } else if (s_qmade < MAX_QUERIES) {
        D3D11_QUERY_DESC d;
        d.Query = D3D11_QUERY_OCCLUSION;
        d.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(s_dev, &d, &q)))
            return NULL;
        s_qmade++;
    } else {
        return NULL;
    }
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)q);
    return q;
}

static void query_end(ID3D11Query *q)
{
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)q);
    s_qpend[s_npend++] = q;
}

/* ---- drawing ------------------------------------------------------------ */

/* Triangle-list vertices through the ring buffer; each Draw wrapped in an
 * occlusion query when `query`. */
static void draw_vertices(const Nv2aVertexEx *v, uint32_t n, int query)
{
    const uint32_t max_verts = (VB_BYTES / VTX_BYTES) / 3 * 3;

    while (n >= 3) {
        uint32_t chunk = n < max_verts ? n - n % 3 : max_verts;
        uint32_t bytes = chunk * VTX_BYTES;
        D3D11_MAP mt = D3D11_MAP_WRITE_NO_OVERWRITE;
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11Query *q = NULL;

        if (s_vb_pos == 0 || s_vb_pos + bytes > VB_BYTES) {
            s_vb_pos = 0;
            mt = D3D11_MAP_WRITE_DISCARD;
        }
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0, mt, 0, &m)))
            return;
        memcpy((uint8_t *)m.pData + s_vb_pos, v, bytes);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
        if (query)
            q = query_begin();
        ID3D11DeviceContext_Draw(s_ctx, chunk, s_vb_pos / VTX_BYTES);
        if (q)
            query_end(q);
        s_vb_pos += bytes;
        v += chunk;
        n -= chunk;
    }
}

/* ---- RECOMP_GPU_RASTER_PSCHECK -------------------------------------------- */

static ID3DBlob *compile_src(const char *src, size_t len, const char *entry,
                             const char *target);

static void pscheck_release(void)
{
    REL(s_pc_stage[1]);
    REL(s_pc_stage[0]);
    REL(s_pc_rtv);
    REL(s_pc_tex);
    REL(s_ps_ref);
    s_pc_every = 0;
}

/* Off with code NULL; else that compiled ps_main (from `what`) is the
 * reference, every <every>-th flip. 0 when it cannot be set up (said once on
 * stderr). */
static int pscheck_setup_code(const void *code, size_t len, const char *what, uint32_t every)
{
    D3D11_TEXTURE2D_DESC d;

    pscheck_release();
    memset(&s_pc, 0, sizeof s_pc);
    memset(&s_pc_total, 0, sizeof s_pc_total);
    if (!code || !every)
        return 1;
    memset(&d, 0, sizeof d);
    d.Width = RT_W; d.Height = RT_H; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreatePixelShader(s_dev, code, len, NULL, &s_ps_ref))
        || FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &s_pc_tex))
        || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_pc_tex,
                                                      NULL, &s_pc_rtv))) {
        fprintf(stderr, "[PSCHECK] reference %s could not be set up; off\n", what);
        pscheck_release();
        return 0;
    }
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &s_pc_stage[0]))
        || FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &s_pc_stage[1]))) {
        fprintf(stderr, "[PSCHECK] no staging textures; off\n");
        pscheck_release();
        return 0;
    }
    s_pc_every = every;
    s_pc_tick = GetTickCount();
    fprintf(stderr, "[PSCHECK] on: reference %s, every %u-th frame\n", what, every);
    return 1;
}

/* Off with path NULL; else ps_main of the file at path is the reference
 * (RECOMP_GPU_RASTER_PSCHECK), as pscheck_setup_code. */
static int pscheck_setup(const char *path, uint32_t every)
{
    ID3DBlob *b = NULL;
    char *src = NULL;
    long n = 0;
    FILE *f;
    int ok;

    if (!path || !every)
        return pscheck_setup_code(NULL, 0, NULL, 0);
    f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        src = n > 0 ? (char *)malloc((size_t)n) : NULL;
        if (src && fread(src, 1, (size_t)n, f) != (size_t)n) {
            free(src);
            src = NULL;
        }
        fclose(f);
    }
    if (src) {
        b = compile_src(src, (size_t)n, "ps_main", "ps_4_0");
        free(src);
    }
    if (!b) {
        pscheck_setup_code(NULL, 0, NULL, 0);
        fprintf(stderr, "[PSCHECK] reference %s could not be set up; off\n", path);
        return 0;
    }
    ok = pscheck_setup_code(ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b),
                            path, every);
    REL(b);
    return ok;
}

/* An 8-bit target's value of a float the shader wrote. */
static int unorm8(float v)
{
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    return (int)(v * 255.0f + 0.5f);
}

/* The draw just made, again through `ps` and through the reference, at the
 * native size into s_pc_tex: pixels (x0,y0)-(x1,y1) compared. -7777 is the
 * clear: a pixel the shader discarded keeps it. */
static void pscheck_draw(const Nv2aBatch *b, const PsConsts *pc, ID3D11PixelShader *ps,
                         const D3D11_RECT *rc, uint32_t va, uint32_t x0, uint32_t y0,
                         uint32_t x1, uint32_t y1)
{
    static const float sentinel[4] = { -7777.0f, -7777.0f, -7777.0f, -7777.0f };
    static const float zero[4] = { 0, 0, 0, 0 };
    D3D11_MAPPED_SUBRESOURCE ma, mb;
    D3D11_VIEWPORT vp;
    D3D11_BOX box;
    uint32_t x, y, k, differ8 = 0, differf = 0, px = 0;
    int got_a, got_b;

    if (x1 > RT_W) x1 = RT_W;
    if (y1 > RT_H) y1 = RT_H;
    if (x0 >= x1 || y0 >= y1)
        return;
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)RT_W; vp.Height = (float)RT_H;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    box.left = x0; box.right = x1; box.top = y0; box.bottom = y1;
    box.front = 0; box.back = 1;
    for (k = 0; k < 2; k++) {
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_pc_rtv, NULL);
        ID3D11DeviceContext_OMSetBlendState(s_ctx,
            blend_state(0, 0, 0, 0, D3D11_COLOR_WRITE_ENABLE_ALL), zero, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, rc);
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_pc_rtv, sentinel);
        ID3D11DeviceContext_PSSetShader(s_ctx, k ? s_ps_ref : ps, NULL, 0);
        draw_vertices(b->vertices_ex, b->count - b->count % 3, 0);
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_pc_stage[k], 0,
                                                  x0, y0, 0, (ID3D11Resource *)s_pc_tex, 0,
                                                  &box);
    }
    got_a = SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_pc_stage[0], 0,
                                              D3D11_MAP_READ, 0, &ma));
    got_b = SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_pc_stage[1], 0,
                                              D3D11_MAP_READ, 0, &mb));
    if (got_a && got_b)
        for (y = y0; y < y1; y++) {
            const float *a = (const float *)((const uint8_t *)ma.pData + (size_t)y * ma.RowPitch);
            const float *r = (const float *)((const uint8_t *)mb.pData + (size_t)y * mb.RowPitch);
            for (x = x0; x < x1; x++) {
                const float *pa = a + 4 * x, *pr = r + 4 * x;
                int da = pa[0] == -7777.0f, dr = pr[0] == -7777.0f, worst = 0, c;
                if (da && dr)
                    continue;
                px++;
                if (memcmp(pa, pr, 16) != 0)
                    differf++;
                if (da != dr) {
                    worst = 255;
                } else {
                    for (c = 0; c < 4; c++) {
                        int d = unorm8(pa[c]) - unorm8(pr[c]);
                        if (d < 0) d = -d;
                        if (d > worst) worst = d;
                    }
                }
                if (worst) {
                    differ8++;
                    if ((uint32_t)worst > s_pc.worst)
                        s_pc.worst = (uint32_t)worst;
                }
            }
        }
    if (got_a)
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_pc_stage[0], 0);
    if (got_b)
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_pc_stage[1], 0);
    bind_pipeline();
    s_pc.draws++;
    s_pc.pixels += px;
    s_pc.differf += differf;
    s_pc.differ8 += differ8;
    if (differ8 && !s_pc.have_first) {
        s_pc.have_first = 1;
        s_pc.va = va;
        s_pc.kind = pc->at[3];
        memcpy(s_pc.ctl, pc->ctl, sizeof s_pc.ctl);
        s_pc.icw = pc->cicw[0];
        s_pc.ocw = pc->cocw[0];
    }
    s_pc_total.draws++;
    s_pc_total.pixels += px;
    s_pc_total.differf += differf;
    s_pc_total.differ8 += differ8;
    if (s_pc.worst > s_pc_total.worst)
        s_pc_total.worst = s_pc.worst;
}

/* Every 10 s, one block. */
static void pscheck_report(void)
{
    char line[512];
    TextBlock tb;
    DWORD now = GetTickCount();

    if (!s_pc_every || now - s_pc_tick < 10000)
        return;
    s_pc_tick = now;
    text_block_init(&tb, line, sizeof line, stderr);
    if (s_pc.have_first)
        text_block_printf(&tb, "[PSCHECK] draws %llu, pixels %llu, float differ %llu, 8-bit differ"
                               " %llu (worst %u), first: target 0x%08X, kind %u, ctl %08X %08X"
                               " %08X %08X, icw/ocw s0 %08X %08X\n",
                          (unsigned long long)s_pc.draws, (unsigned long long)s_pc.pixels,
                          (unsigned long long)s_pc.differf, (unsigned long long)s_pc.differ8,
                          s_pc.worst, s_pc.va, s_pc.kind, s_pc.ctl[0], s_pc.ctl[1], s_pc.ctl[2],
                          s_pc.ctl[3], s_pc.icw, s_pc.ocw);
    else
        text_block_printf(&tb, "[PSCHECK] draws %llu, pixels %llu, float differ %llu, 8-bit differ"
                               " %llu (worst %u)\n",
                          (unsigned long long)s_pc.draws, (unsigned long long)s_pc.pixels,
                          (unsigned long long)s_pc.differf, (unsigned long long)s_pc.differ8,
                          s_pc.worst);
    text_block_flush(&tb);
    memset(&s_pc, 0, sizeof s_pc);
}

static void set_scissor(const Nv2aSurface *s, const GpuRt *r, D3D11_RECT *rc)
{
    uint32_t x1 = s->clip_x + s->width, y1 = s->clip_y + s->height;
    if (x1 > r->W) x1 = r->W;
    if (y1 > r->H) y1 = r->H;
    rc->left = (LONG)(s->clip_x < x1 ? s->clip_x : x1);
    rc->top = (LONG)(s->clip_y < y1 ? s->clip_y : y1);
    rc->right = (LONG)x1;
    rc->bottom = (LONG)y1;
    {                                   /* rc stays in surface pixels */
        D3D11_RECT sc;
        sc.left = rc->left * (LONG)s_sx; sc.right = rc->right * (LONG)s_sx;
        sc.top = rc->top * (LONG)s_sy; sc.bottom = rc->bottom * (LONG)s_sy;
        ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    }
}

static float zmax_of(const Nv2aRenderState *rs)
{
    return rs->zeta_format == 1 ? 65535.0f : 16777215.0f;
}

static void exec_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                      uint32_t flags, uint32_t argb, uint32_t zs)
{
    if (flags & 0xF0) {
        GpuRt *r = rt_acquire(s);
        if (r) {
            static const float black[4] = { 0, 0, 0, 0 };
            Nv2aVertexEx v[6];
            VsConsts vc;
            PsConsts pc;
            D3D11_RECT rc;
            float c[4], x0, y0, x1, y1;
            uint32_t col = r->bpp == 2 ? expand565(pack565(argb)) : argb, i;
            ID3D11ShaderResourceView *none[4] = { NULL, NULL, NULL, NULL };

            c[0] = (float)((col >> 16) & 0xFF) / 255.0f;
            c[1] = (float)((col >> 8) & 0xFF) / 255.0f;
            c[2] = (float)(col & 0xFF) / 255.0f;
            c[3] = (float)(col >> 24) / 255.0f;
            set_scissor(s, r, &rc);
            x0 = (float)rc.left; y0 = (float)rc.top;
            x1 = (float)rc.right; y1 = (float)rc.bottom;
            memset(v, 0, sizeof v);
            v[0].x = x0; v[0].y = y0; v[1].x = x1; v[1].y = y0; v[2].x = x0; v[2].y = y1;
            v[3].x = x1; v[3].y = y0; v[4].x = x1; v[4].y = y1; v[5].x = x0; v[5].y = y1;
            for (i = 0; i < 6; i++) {
                v[i].rhw = 1.0f;
                memcpy(v[i].d0, c, sizeof c);
            }
            memset(&vc, 0, sizeof vc);
            memset(&pc, 0, sizeof pc);
            for (i = 0; i < 4; i++)
                none[i] = s_white_srv;
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, none);
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->rtv, NULL);
            ID3D11DeviceContext_OMSetBlendState(s_ctx,
                blend_state(0, 0, 0, 0, D3D11_COLOR_WRITE_ENABLE_ALL), black, 0xFFFFFFFFu);
            ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ds_state(rs, 0), 0);
            set_consts(&vc, &pc);
            use_ps(pick_ps(&pc));
            GPROF_MARK(GP_CLEAR_COLOR, r->va);
            GPROF_COUNT(GPC_CLEARS, 1);
            GPROF_COUNT(GPC_CLEAR_PX, (uint64_t)(rc.right - rc.left) * (uint64_t)(rc.bottom - rc.top)
                                      * s_sx * s_sy);
            draw_vertices(v, 6, 0);
            r->stale = 1;
            rt_grow(r, (uint32_t)rc.left, (uint32_t)rc.top, (uint32_t)rc.right,
                    (uint32_t)rc.bottom);
        }
    }
    if (flags & 0x03) {
        GpuZ *z = z_acquire(rs->zeta_va);
        if (z) {
            UINT cf = 0;
            float zc = rs->zeta_format == 1 ? (float)(zs & 0xFFFF) : (float)(zs >> 8);
            if (flags & 1) cf |= D3D11_CLEAR_DEPTH;
            if (flags & 2) cf |= D3D11_CLEAR_STENCIL;
            GPROF_MARK(GP_CLEAR_ZS, z->va);
            GPROF_COUNT(GPC_CLEARS, 1);
            GPROF_COUNT(GPC_CLEAR_PX, (uint64_t)RT_W * s_sx * s_gh * s_sy);
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv, cf,
                                                      zc / zmax_of(rs),
                                                      (UINT8)(zs & 0xFF));
        }
    }
}

static int s_rt_tex = 1;                /* RECOMP_GPU_RASTER_RTTEX=0: always via guest RAM */
static int s_sync_all;                  /* RECOMP_GPU_RASTER_SYNC=all: write back at every sync */

/* A texture that is a live render target's pixels: the title drew a surface
 * and now samples it (Fallout draws its scene into 0x80778000 at 1280x480 and
 * then reads it as a LIN_A8R8G8B8 texture to filter it down into 0x809D0000).
 * The target is sampled on the GPU as it is, with no writeback to guest RAM
 * and no CPU decode. Linear 32-bit formats with the target's own pitch only;
 * not the surface being drawn (a target cannot be read and written at once).
 * `ox`/`oy`: where texel (0,0) sits in the target. */
static GpuRt *rt_as_texture(const Nv2aTexture *t, const Nv2aSurface *dst,
                            uint32_t *ox, uint32_t *oy, int *swz)
{
    uint32_t i, off;
    int sw = t->color == 0x06 || t->color == 0x07;

    *swz = 0;
    if (!s_rt_tex || t->cube)
        return NULL;
    if (!sw && ((t->color != 0x12 && t->color != 0x1E) || !t->pitch))
        return NULL;
    for (i = 0; i < MAX_RTS; i++) {
        GpuRt *r = &s_rt[i];
        if (!r->used || r->bpp != 4 || t->offset < r->va || t->offset >= rt_end(r))
            continue;
        if (sw && r->swizzled) {
            /* A swizzled texture over a swizzled surface: the GPU stored
             * pixel (x,y) where the texture reads texel (x,y), so the target
             * is sampled as it is. Fallout's character shadows: points drawn
             * into a 512x128 swizzled surface, then laid on the floor as a
             * swizzled A8R8G8B8 texture. Read through the linear view below,
             * the shadows came out as scattered dots. */
            if (t->offset != r->va || t->width > r->W || t->height > r->H)
                continue;
            *ox = *oy = 0;
        } else if (sw) {
            /* A swizzled 32-bit texture over a target drawn as a linear
             * (pitch) surface: texel (u,v) is dword swizzle_offset(u,v) from
             * the base, exactly as the software sampler reads guest RAM. */
            uint32_t row = r->pitch / 4;
            if (t->offset != r->va || !row
                || ((uint64_t)t->width * t->height + row - 1) / row > r->H)
                continue;
            *ox = *oy = 0;
            *swz = 1;
        } else {
            if (r->pitch != t->pitch)
                continue;
            off = t->offset - r->va;
            if ((off % r->pitch) % 4)
                continue;
            *oy = off / r->pitch;
            *ox = (off % r->pitch) / 4;
            if (*ox + t->width > r->W || *oy + t->height > r->H)
                continue;
        }
        if (ranges_overlap(r->va, rt_end(r), dst->color_va,
                           dst->color_va + (dst->clip_y + dst->height) * dst->pitch))
            return NULL;                /* the surface being drawn: via guest RAM */
        rt_taken(r, 0);
        if (!rt_check(r))               /* clean: catch up with CPU writes first */
            return NULL;
        rt_resolve(r);
        return r;
    }
    return NULL;
}

/* The stages a batch actually samples. */
static uint32_t stages_needed(const Nv2aBatch *b)
{
    uint32_t need = 0, st;

    if (b->kind == NV2A_BATCH_XF && b->use_rc) {
        for (st = 0; st < 4; st++) {
            uint32_t mode = (b->rc->stage_program >> (st * 5)) & 0x1F;
            if (mode >= 1 && mode <= 3)
                need |= 1u << st;
        }
    } else if (b->textured) {
        need = 1;
    }
    return need & b->stage_valid;
}

static void exec_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    ID3D11ShaderResourceView *srv[4];
    VsConsts vc;
    PsConsts pc;
    D3D11_RECT rc;
    float bf[4];
    uint32_t need, st, i, blend_on;
    UINT8 mask;
    GpuRt *r;
    GpuZ *z = NULL;
    ID3D11PixelShader *ps;
    int xf = b->kind == NV2A_BATCH_XF;

    if ((s->bytes_per_pixel != 2 && s->bytes_per_pixel != 4) || !b->vertices_ex
        || b->count < 3)
        return;
    need = stages_needed(b);
    r = rt_acquire(s);
    if (!r)
        return;
    if (xf) {
        z = z_acquire(rs->zeta_va);
    }

    memset(&vc, 0, sizeof vc);
    memset(&pc, 0, sizeof pc);
    vc.vsc[0] = 1.0f / zmax_of(rs);
    vc.vsc[1] = xf ? 1.0f : 0.0f;

    for (st = 0; st < 4; st++) {
        const Nv2aTexture *t = &b->stages[st];
        uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF, fl = 0;
        uint32_t ox = 0, oy = 0;
        int swz = 0;
        GpuRt *src = NULL;
        srv[st] = NULL;
        if (need & (1u << st)) {
            src = rt_as_texture(t, s, &ox, &oy, &swz);
            if (src) {
                srv[st] = src->srv;
                fl |= TXF_RT;
                if (t->color == 0x1E || t->color == 0x07)
                    fl |= TXF_OPAQUE;
                if (swz) {
                    fl |= TXF_SWZ;
                    ox = src->pitch / 4;        /* .w: the target's row, in texels */
                }
                s_st.rt_tex++;
            } else {
                /* A texture drawn earlier into a target goes back to guest
                 * RAM before the stage is decoded from there. */
                uint32_t lo = t->offset, hi = lo + nv2a_backend_texture_bytes(t);
                if (!s_compare)
                    for (i = 0; i < MAX_RTS; i++)
                        if (s_rt[i].used && s_rt[i].dirty
                            && ranges_overlap(rt_dirty_lo(&s_rt[i]), rt_dirty_hi(&s_rt[i]),
                                              lo, hi))
                        {
                            s_st.wb_why[0]++;
                            rt_writeback(&s_rt[i]);
                        }
                srv[st] = tex_resolve(t);
            }
        }
        if (srv[st])
            fl |= TXF_VALID;
        else
            srv[st] = s_white_srv;
        if (d3d8_format_is_swizzled(t->color) || d3d8_format_dxt_block_bytes(t->color))
            fl |= TXF_NORM;
        if (mag == 2 || min == 2 || min == 4 || min == 6)
            fl |= TXF_BILIN;
        if (t->cube)
            fl |= TXF_CUBE;
        pc.tex[st][0] = (t->width & 0xFFFF) | ((t->height & 0xFFFF) << 16);
        pc.tex[st][1] = (t->addr_u & 0xFF) | ((t->addr_v & 0xFF) << 8);
        pc.tex[st][2] = fl;
        pc.tex[st][3] = swz ? ox : (ox & 0xFFFF) | ((oy & 0xFFFF) << 16);
    }
    if (b->rc) {
        memcpy(pc.cicw, b->rc->color_icw, sizeof pc.cicw);
        memcpy(pc.aicw, b->rc->alpha_icw, sizeof pc.aicw);
        memcpy(pc.cocw, b->rc->color_ocw, sizeof pc.cocw);
        memcpy(pc.aocw, b->rc->alpha_ocw, sizeof pc.aocw);
        memcpy(pc.fac0, b->rc->factor0, sizeof pc.fac0);
        memcpy(pc.fac1, b->rc->factor1, sizeof pc.fac1);
        pc.fin[0] = b->rc->final0;
        pc.fin[1] = b->rc->final1;
        pc.fin[2] = b->rc->final_c0;
        pc.fin[3] = b->rc->final_c1;
        pc.ctl[0] = b->rc->control;
        pc.ctl[1] = b->rc->stage_program;
    }
    pc.ctl[2] = b->clip_plane_mode;
    pc.ctl[3] = (xf && b->use_rc ? PSF_RC : 0u) | (b->textured ? PSF_TEXTURED : 0u)
              | (xf && rs->alpha_test_enable ? PSF_ALPHATEST : 0u)
              | (xf && z && rs->depth_test_enable && rs->depth_func >= 0x201
                 && rs->depth_func <= 0x203 ? PSF_ZFAR_FAIL : 0u)
              | (xf && rs->depth_max > rs->depth_min ? PSF_ZCLIP : 0u);
    pc.zr[0] = rs->depth_min;
    pc.zr[1] = rs->depth_max;
    pc.rsc[0] = (float)s_sx;
    pc.rsc[1] = (float)s_sy;
    pc.at[0] = rs->alpha_func;
    pc.at[1] = rs->alpha_ref;
    pc.at[2] = b->stage_valid;
    pc.at[3] = b->kind;
    memcpy(pc.fogc, b->fog_color, sizeof pc.fogc);

    blend_on = rs->blend_enable && !(rs->blend_src == 1 && rs->blend_dst == 0);
    /* D3D11 has one blend factor for both ends; blend_factor() in the
     * executor gives CONSTANT_COLOR (0x8001/2) the colour per channel and
     * CONSTANT_ALPHA (0x8003/4) its alpha in every channel. With only the
     * alpha forms in use the factor is (A,A,A,A); otherwise it is (R,G,B,A),
     * exact for the colour forms, and for a mixed pair the alpha form gets
     * the colour channels on RGB (its alpha channel is still A). */
    if ((rs->blend_src == 0x8003 || rs->blend_src == 0x8004
         || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        && rs->blend_src != 0x8001 && rs->blend_src != 0x8002
        && rs->blend_dst != 0x8001 && rs->blend_dst != 0x8002) {
        bf[0] = bf[1] = bf[2] = bf[3] = (float)(rs->blend_color >> 24) / 255.0f;
    } else {
        bf[0] = (float)((rs->blend_color >> 16) & 0xFF) / 255.0f;
        bf[1] = (float)((rs->blend_color >> 8) & 0xFF) / 255.0f;
        bf[2] = (float)(rs->blend_color & 0xFF) / 255.0f;
        bf[3] = (float)(rs->blend_color >> 24) / 255.0f;
    }
    /* A 16-bit surface has no alpha: the software path reads destination
     * alpha as 1 (put_pixel), so the target keeps the 1 it was uploaded with. */
    mask = write_mask(rs->color_mask);
    if (r->bpp == 2)
        mask &= (UINT8)~D3D11_COLOR_WRITE_ENABLE_ALPHA;

    /* Target first: a view of a resource still bound as the render target
     * would be dropped by the runtime. */
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->rtv, z ? z->dsv : NULL);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    ID3D11DeviceContext_OMSetBlendState(s_ctx,
        blend_state((int)blend_on, rs->blend_src, rs->blend_dst, rs->blend_eq,
                    mask), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ds_state(rs, z != NULL),
                                               rs->stencil_ref & 0xFF);
    set_scissor(s, r, &rc);
    set_consts(&vc, &pc);
    GPROF_MARK(!xf ? GP_DRAW_SCREEN : b->use_rc ? GP_DRAW_RC : GP_DRAW_FFP, r->va);
    ps = pick_ps(&pc);
    use_ps(ps);
    draw_vertices(b->vertices_ex, b->count - b->count % 3,
                  xf && rs->zpass_enable && !s_compare);
    r->stale = 1;
    /* Dirty is what the triangles cover, not the whole clip: the bounding box
     * of the vertices, inside the scissor. */
    {
        /* A NaN vertex is skipped: compared against it, every test is false,
         * and a batch whose first vertex was NaN left no dirty rectangle --
         * what it drew never reached guest RAM (stippled floor tiles in the
         * frame resolved from there). Bounds are clamped as floats, before a
         * far-off coordinate is converted. */
        float bx0 = FLT_MAX, by0 = FLT_MAX, bx1 = -FLT_MAX, by1 = -FLT_MAX;
        LONG gx0, gy0, gx1, gy1;
        for (i = 0; i < b->count; i++) {
            const Nv2aVertexEx *v = &b->vertices_ex[i];
            if (v->x != v->x || v->y != v->y)
                continue;
            if (v->x < bx0) bx0 = v->x;
            if (v->x > bx1) bx1 = v->x;
            if (v->y < by0) by0 = v->y;
            if (v->y > by1) by1 = v->y;
        }
        if (bx0 > bx1)                  /* nothing but NaN: nothing drawn */
            bx0 = bx1 = (float)rc.left, by0 = by1 = (float)rc.top;
        bx0 = floorf(bx0); by0 = floorf(by0);
        bx1 = ceilf(bx1) + 1.0f; by1 = ceilf(by1) + 1.0f;
        gx0 = bx0 <= (float)rc.left ? rc.left : bx0 >= (float)rc.right ? rc.right : (LONG)bx0;
        gy0 = by0 <= (float)rc.top ? rc.top : by0 >= (float)rc.bottom ? rc.bottom : (LONG)by0;
        gx1 = bx1 <= (float)gx0 ? gx0 : bx1 >= (float)rc.right ? rc.right : (LONG)bx1;
        gy1 = by1 <= (float)gy0 ? gy0 : by1 >= (float)rc.bottom ? rc.bottom : (LONG)by1;
        rt_grow(r, (uint32_t)gx0, (uint32_t)gy0, (uint32_t)gx1, (uint32_t)gy1);
        if (s_rc_last >= 0)
            s_rcs[s_rc_last].px += (uint64_t)(gx1 - gx0) * (uint64_t)(gy1 - gy0) * s_sx * s_sy;
        if (s_pc_every && (s_flip_no % s_pc_every) == 0)
            pscheck_draw(b, &pc, ps, &rc, r->va, (uint32_t)gx0, (uint32_t)gy0,
                         (uint32_t)gx1, (uint32_t)gy1);
        if (g_nv2a_gprof_on) {
            uint64_t tris = b->count / 3;
            int clip_planes = 0;
            if (xf && b->use_rc && b->rc)
                for (st = 0; st < 4; st++)
                    if (((b->rc->stage_program >> (st * 5)) & 0x1F) == 5)
                        clip_planes = 1;
            nv2a_gprof_count(GPC_DRAWS, 1);
            nv2a_gprof_count(GPC_TRIS, tris);
            nv2a_gprof_count(GPC_PX, (uint64_t)(gx1 - gx0) * (uint64_t)(gy1 - gy0) * s_sx * s_sy);
            if (pc.ctl[3] & PSF_ZFAR_FAIL) nv2a_gprof_count(GPC_F_ZFAR, tris);
            if (pc.ctl[3] & PSF_ZCLIP) nv2a_gprof_count(GPC_F_ZCLIP, tris);
            if (pc.ctl[3] & PSF_ALPHATEST) nv2a_gprof_count(GPC_F_ATEST, tris);
            if (clip_planes) nv2a_gprof_count(GPC_F_CLIPPLANE, tris);
            if (blend_on) nv2a_gprof_count(GPC_F_BLEND, tris);
            if (rs->depth_write && z) nv2a_gprof_count(GPC_F_ZWRITE, tris);
            if (rs->cull_enable) nv2a_gprof_count(GPC_F_CULL, tris);
            if (z && (rs->depth_test_enable || rs->depth_write || rs->stencil_enable))
                nv2a_gprof_count(GPC_F_ZUSE, 1);
        }
    }
    s_st.draws++;
    if (s_compare)
        for (i = 0; i + 2 < b->count; i += 3) {
            uint32_t k;
            for (k = 0; k < 3; k++) {
                const Nv2aVertexEx *v = &b->vertices_ex[i + k];
                if (fabsf(v->x) > FAR_COORD || fabsf(v->y) > FAR_COORD)
                    break;
            }
            if (k < 3)
                s_far_tris++;
        }
}

/* ---- frame skip (RECOMP_FRAME_SKIP) --------------------------------------
 *
 * The game flips on every tick of the 60 Hz grid; the executor asks pace()
 * at each flip whether it is shown (the cadence of RECOMP_FPS_LIMIT and, with
 * auto, whether the swap chain can take a frame without waiting). A frame
 * that is not shown need not be drawn at all -- that is the point: at 30
 * shows a second the GPU draws half the frames.
 *
 * Windows. A window runs from one boundary to the next: the first full
 * overwrite after a flip of P, the surface the last flip showed (and the one
 * before it, when the two alternate: double buffering) -- a colour clear of
 * all four channels over the whole target, or a draw that passes the coverage
 * test (fs_covers). Only the first after a flip: a second full overwrite of P
 * in the same frame would split it in two, and the window committed last would
 * know only its half. In Fallout every frame starts with `clear F3` over
 * 0x00778000 and its depth buffer (stage3 traces: 210 of 210 windows). Each
 * window is SHOW (drawn as it comes) or SKIP; the mode of the next one is the
 * forecast pace() makes (cadence_next, and with auto the presenter's rate and
 * phase, see gpu_pace).
 *
 * SKIP: clears and draws go into a journal (the arena: surface, state,
 * combiners, every vertex copied -- the executor reuses its buffers) and the
 * GPU sees nothing: no target acquired, no texture decoded, no guest RAM
 * compared. At the next boundary the journal is dropped and the targets it
 * wrote are marked dropped -- unless one of them is not self-contained, when
 * it is drawn after all (replayed): a window that reads what an earlier one
 * left needs it. It is replayed too, exactly as logged, when pace() shows the
 * frame after all (the forecast was wrong), when something observes the GPU's
 * work in a SKIP window (a writeback, the presented surface, a visibility
 * count from a logged draw), and when the arena is full.
 *
 * Self-contained: each target (colour by address, depth by address) is, when
 * the last SHOW window read nothing of it before overwriting it whole -- a
 * read being a texture, a blend, a show, a writeback, a depth or stencil test.
 * A partial write read later in the window is not (the read sees the old
 * pixels around it); a partial write nothing reads is (Fallout's filter writes
 * depth into the zeta-0 buffer with the test off, every frame, and no draw
 * ever tests it: counted as a read before, every skipped window was drawn).
 * An untouched target is not. Reading a dropped target before it was
 * overwritten (the guess was wrong) is a stale read: said once per address;
 * the window it happened in reads the target first, so the target is not
 * self-contained until a shown window overwrites it first again. It used to
 * stay so for good: one miss at Carbon's loading screen (the shown surface
 * changes there, the guess cannot know) and every skipped window after it
 * was drawn. RECOMP_FRAME_SKIP_TRACE=1 says why a window is drawn and
 * where a stale read or texture came from.
 *
 * A logged draw keeps a hash of each texture it samples from guest RAM (and
 * its palette); drawn later, a texture whose bytes changed meanwhile (the
 * title's CPU went on: a movie, a dynamic texture) is counted and said once
 * per address -- the frame is drawn from what is there now.
 *
 * Off (pace not registered) with RECOMP_GPU_RASTER=compare and SYNC=all. */
#define FS_MAX_T 32
#define FS_RATE_N 64                    /* flips the presenter's rate is taken over */
#define FS_ARENA_MAX ((size_t)256 << 20)
#define FS_RT 0
#define FS_Z 1
#define FS_FULL 1                       /* overwritten whole before anything read it */
#define FS_OTHER 2                      /* read before it was overwritten whole */
#define FS_WROTE 3                      /* written in part, nothing read yet */
#define FS_READ 4                       /* fs_mark: an access that reads */
#define FS_SHOW 0
#define FS_SKIP 1
#define FSR_READY 0                     /* replayed: shown after all */
#define FSR_OBSERVED 1                  /* something read the GPU's work */
#define FSR_NOTSELF 2                   /* a written target is not self-contained */
#define FSR_OVERFLOW 3                  /* the arena is full */
#define FS_OP_CLEAR 1
#define FS_OP_DRAW 2

typedef struct { uint32_t kind, va, end, cls; } FsT;

typedef struct {
    uint32_t type, bytes, pad[2];
} FsOp;

typedef struct {
    FsOp op;
    Nv2aSurface s;
    Nv2aRenderState rs;
    uint32_t flags, argb, zs;
} FsClear;

typedef struct {
    FsOp op;
    Nv2aSurface s;
    Nv2aBatch b;
    Nv2aRenderState rs;
    Nv2aCombiner rc;
    uint32_t has_rc;
    uint32_t hashed;                    /* bit i: stage i read from guest RAM, thash[i] */
    uint32_t window_no, flip_no;        /* where it was logged (RECOMP_FRAME_SKIP_TRACE) */
    uint64_t thash[4];
} FsDraw;

/* Counters, cumulative (the 10 s report prints the difference). logged_bytes
 * is what went into the arena over the period, not what it holds: the
 * journal starts empty at every window. */
typedef struct {
    uint32_t shown, skipped, nr_presenter, nr_gpu, logged, dropped_windows;
    uint32_t replayed[4], mispredicted_skip, mispredicted_show, stale, tex_changed;
    uint64_t logged_bytes;
} FsStats;

/* Texture hashes, once per (address, size, palette) while logging a window
 * or drawing a journal: a window samples the same few textures many times. */
#define FS_HCACHE 64
typedef struct { uint32_t off, bytes, palette, color; uint64_t h; } FsHash;

static struct {
    int on;                             /* pace() was called: this section runs */
    int mode, next_mode, replaying, first_pace;
    uint8_t *arena;
    size_t used, cap;
    uint32_t ops;
    int zpass;                          /* the journal holds a visibility-counted draw */
    FsT jt[FS_MAX_T];                   /* targets the journal writes */
    int njt, jt_lost;
    FsT win[FS_MAX_T];                  /* this window's first accesses */
    int nwin;
    FsT selfc[FS_MAX_T];                /* self-contained in the last SHOW window */
    int nselfc;
    FsT dropped[FS_MAX_T];              /* written by a dropped journal, not since */
    int ndropped;
    uint32_t stale_said[64];
    int nstale_said;
    uint32_t tex_said[64];              /* textures said changed */
    int ntex_said;
    FsHash hc[FS_HCACHE];
    int nhc;
    uint32_t pva[2];                    /* the surfaces the last two flips showed */
    int armed;                          /* no boundary since the last flip */
    FsT seen[FS_MAX_T];                 /* every target the last SHOW window touched */
    int nseen;
    uint32_t window_no;                 /* boundaries so far (RECOMP_FRAME_SKIP_TRACE) */
    double R;                           /* auto: the presenter's rate, shows per tick */
    uint8_t r_shown[FS_RATE_N], r_gap[FS_RATE_N];   /* ... over the last FS_RATE_N flips */
    int r_at, r_n;
    double credit;                      /* auto: shows owed by it, in its phase */
    uint64_t k_shown;
    uint64_t k_prev;                    /* the previous flip's tick */
} s_fs;
static FsStats s_fst, s_fst_said;

/* RECOMP_FRAME_SKIP_TRACE=1: why a window is drawn after all and where a
 * stale texture came from, the first 60 times and every 500th after. */
static int s_fs_trace = -1;
static uint32_t s_fs_trace_n;
static int fs_trace_on(void)
{
    if (s_fs_trace < 0) {
        const char *e = getenv("RECOMP_FRAME_SKIP_TRACE");
        s_fs_trace = e && *e && strcmp(e, "0") != 0;
    }
    return s_fs_trace && (s_fs_trace_n++ < 60 || s_fs_trace_n % 500 == 0);
}
#define FS_TRACE(...) do { if (fs_trace_on()) fprintf(stderr, __VA_ARGS__); } while (0)

static int fs_find(const FsT *a, int n, uint32_t kind, uint32_t va)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i].kind == kind && a[i].va == va)
            return i;
    return -1;
}

static void fs_del(FsT *a, int *n, int i)
{
    memmove(&a[i], &a[i + 1], (size_t)(*n - i - 1) * sizeof *a);
    (*n)--;
}

/* Into a list once; a full list forgets its oldest entry. 0: it was full. */
static int fs_add(FsT *a, int *n, uint32_t kind, uint32_t va, uint32_t end, uint32_t cls)
{
    int full = 0;
    if (fs_find(a, *n, kind, va) >= 0)
        return 1;
    if (*n == FS_MAX_T) {
        fs_del(a, n, 0);
        full = 1;
    }
    a[*n].kind = kind; a[*n].va = va; a[*n].end = end; a[*n].cls = cls;
    (*n)++;
    return !full;
}

static void fs_stale(uint32_t va)
{
    int i;
    s_fst.stale++;
    for (i = 0; i < s_fs.nselfc; )
        if (s_fs.selfc[i].va == va)
            fs_del(s_fs.selfc, &s_fs.nselfc, i);
        else
            i++;
    for (i = 0; i < s_fs.nstale_said; i++)
        if (s_fs.stale_said[i] == va)
            return;
    if (s_fs.nstale_said < 64)
        s_fs.stale_said[s_fs.nstale_said++] = va;
    fprintf(stderr, "[GPURAST] frame skip: 0x%08X read before it was drawn again after a"
                    " dropped window (a stale read)\n", va);
}

/* An access to a target in this window: FS_FULL (overwritten whole),
 * FS_WROTE (in part) or FS_READ. Its state goes FS_WROTE -> FS_FULL or
 * FS_OTHER and stays there; a full overwrite makes a dropped target whole. */
static void fs_mark(uint32_t kind, uint32_t va, uint32_t end, uint32_t cls)
{
    int i = fs_find(s_fs.win, s_fs.nwin, kind, va);
    uint32_t to = cls == FS_READ ? FS_OTHER : cls;
    if (i < 0)
        fs_add(s_fs.win, &s_fs.nwin, kind, va, end, to);
    else if (s_fs.win[i].cls == FS_WROTE && to != FS_WROTE)
        s_fs.win[i].cls = to;
    if (cls == FS_FULL && (i = fs_find(s_fs.dropped, s_fs.ndropped, kind, va)) >= 0)
        fs_del(s_fs.dropped, &s_fs.ndropped, i);
}

/* Something reads the target `va` (depth) or the guest bytes [lo, hi)
 * (colour: a texture, a blend, a show, a writeback). */
static void fs_read(uint32_t kind, uint32_t lo, uint32_t hi)
{
    int i;
    for (i = 0; i < s_fs.ndropped; ) {
        FsT *d = &s_fs.dropped[i];
        if (d->kind == kind && (kind == FS_Z ? d->va == lo : ranges_overlap(d->va, d->end, lo, hi))) {
            FS_TRACE("[FSKIP] stale read of %s 0x%08X (read 0x%08X-0x%08X) in window %u at"
                     " flip %u, P 0x%08X/0x%08X%s\n", kind == FS_Z ? "depth" : "colour", d->va,
                     lo, hi, s_fs.window_no, s_flip_no, s_fs.pva[0], s_fs.pva[1],
                     s_fs.replaying ? ", drawing a journal" : "");
            fs_stale(d->va);
            fs_del(s_fs.dropped, &s_fs.ndropped, i);
            continue;
        }
        i++;
    }
    if (kind == FS_Z) {
        fs_mark(FS_Z, lo, lo, FS_READ);
        return;
    }
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used && ranges_overlap(s_rt[i].va, rt_end(&s_rt[i]), lo, hi))
            fs_mark(FS_RT, s_rt[i].va, rt_end(&s_rt[i]), FS_READ);
    /* and a target only this window's work made: not acquired yet in a
     * replay, or written in part */
    for (i = 0; i < s_fs.nwin; i++)
        if (s_fs.win[i].kind == FS_RT && ranges_overlap(s_fs.win[i].va, s_fs.win[i].end, lo, hi))
            fs_mark(FS_RT, s_fs.win[i].va, s_fs.win[i].end, FS_READ);
}

static uint32_t fs_surf_end(const Nv2aSurface *s)
{
    return s->color_va + (s->clip_y + s->height) * s->pitch;
}

/* The clip rectangle is the whole target: (0,0), as wide as the pitch, as
 * tall as the target has ever been. */
static int fs_target_rect(const Nv2aSurface *s, uint32_t *w, uint32_t *h)
{
    uint32_t bpp = s->bytes_per_pixel, W, H, i;
    if ((bpp != 2 && bpp != 4) || !s->pitch || s->clip_x || s->clip_y)
        return 0;
    W = s->pitch / bpp;
    H = s->height;
    if (W > RT_W) W = RT_W;
    if (H > RT_H) H = RT_H;
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used && s_rt[i].va == s->color_va && s_rt[i].pitch == s->pitch
            && s_rt[i].H > H)
            H = s_rt[i].H;
    *w = W;
    *h = H;
    return s->width >= W && s->height >= H && H;
}

static int fs_full_rect(const Nv2aSurface *s)
{
    uint32_t w, h;
    return fs_target_rect(s, &w, &h);
}

/* Nothing of what was there shows through: no blend, all four channels, no
 * alpha, depth or stencil test, no clip-plane stage. */
/* Blending reads the target: on, unless ONE/ZERO through an equation that
 * then ignores the destination (ADD, SUBTRACT, REVERSE_SUBTRACT; MIN and MAX
 * compare with it). */
static int fs_blend_reads(const Nv2aRenderState *rs)
{
    if (!rs->blend_enable)
        return 0;
    if (rs->blend_src == 1 && rs->blend_dst == 0
        && (rs->blend_eq == 0x8006 || rs->blend_eq == 0x800A || rs->blend_eq == 0x800B))
        return 0;
    return 1;
}

static int fs_opaque(const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    uint32_t st;
    if (fs_blend_reads(rs))
        return 0;
    if ((rs->color_mask & 0x01010101u) != 0x01010101u)
        return 0;
    if (b->kind != NV2A_BATCH_XF)
        return 1;
    if (rs->alpha_test_enable || rs->stencil_enable)
        return 0;
    if (rs->depth_test_enable && rs->depth_func != 0x207)
        return 0;
    if (b->rc)
        for (st = 0; st < 4; st++)
            if (((b->rc->stage_program >> (st * 5)) & 0x1F) == 5)
                return 0;
    return 1;
}

static float fs_edge(const Nv2aVertexEx *a, const Nv2aVertexEx *b, float px, float py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/* The four corner pixel centres all inside the convex polygon v[0..n-1]
 * (n 3 or 4, in order); NaN fails every test. */
static int fs_poly_has(const Nv2aVertexEx *const *v, int n, const float *cx, const float *cy)
{
    int c, i, pos = 0, neg = 0;
    for (i = 0; i < n; i++) {           /* convex and not degenerate */
        const Nv2aVertexEx *a = v[i], *b = v[(i + 1) % n], *d = v[(i + 2) % n];
        float z = fs_edge(a, b, d->x, d->y);
        if (z > 0.0f) pos = 1;
        else if (z < 0.0f) neg = 1;
        else return 0;
    }
    if (pos == neg)
        return 0;
    for (c = 0; c < 4; c++)
        for (i = 0; i < n; i++) {
            float e = fs_edge(v[i], v[(i + 1) % n], cx[c], cy[c]);
            if (!(pos ? e >= 0.0f : e <= 0.0f))
                return 0;
        }
    return 1;
}

static int fs_same(const Nv2aVertexEx *a, const Nv2aVertexEx *b)
{
    return a->x == b->x && a->y == b->y;
}

/* The draw overwrites the whole target: opaque (fs_opaque), its clip the
 * whole target, and one triangle -- or two that share an edge into a convex
 * quad -- holding the four corner pixel centres; with SET_CLIP_MIN/MAX in
 * force their z inside it. */
static int fs_covers(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    uint32_t n = b->count - b->count % 3, t, W, H;
    float cx[4], cy[4];
    int zclip = b->kind == NV2A_BATCH_XF && rs->depth_max > rs->depth_min;

    if (!b->vertices_ex || n < 3 || !fs_opaque(b) || !fs_target_rect(s, &W, &H))
        return 0;
    cx[0] = 0.5f; cy[0] = 0.5f; cx[1] = (float)W - 0.5f; cy[1] = 0.5f;
    cx[2] = 0.5f; cy[2] = (float)H - 0.5f; cx[3] = (float)W - 0.5f; cy[3] = (float)H - 0.5f;
    for (t = 0; t < n && t < 48; t += 3) {
        const Nv2aVertexEx *v = &b->vertices_ex[t];
        const Nv2aVertexEx *p[4];
        int i;
        if (zclip) {
            int ok = 1;
            for (i = 0; i < 3 && ok; i++)
                ok = v[i].z >= rs->depth_min && v[i].z <= rs->depth_max;
            if (!ok)
                continue;
        }
        p[0] = &v[0]; p[1] = &v[1]; p[2] = &v[2];
        if (fs_poly_has(p, 3, cx, cy))
            return 1;
        if (t + 6 <= n) {               /* the next triangle completing a quad */
            const Nv2aVertexEx *w = &v[3];
            int sh[3], ns = 0, u = -1, j, a, k;
            for (i = 0; i < 3; i++) {
                sh[i] = -1;
                for (j = 0; j < 3; j++)
                    if (fs_same(&w[i], &v[j]))
                        sh[i] = j;
                if (sh[i] >= 0) ns++; else u = i;
            }
            if (ns != 2 || u < 0)
                continue;
            if (zclip && !(w[u].z >= rs->depth_min && w[u].z <= rs->depth_max))
                continue;
            /* v's vertex not shared, then around: a, s1, w[u], s2 */
            for (a = 0; a < 3; a++) {
                for (k = 0; k < 3; k++)
                    if (sh[k] == a)
                        break;
                if (k == 3)
                    break;
            }
            p[0] = &v[a]; p[1] = &v[(a + 1) % 3]; p[2] = &w[u]; p[3] = &v[(a + 2) % 3];
            if (fs_poly_has(p, 4, cx, cy))
                return 1;
        }
    }
    return 0;
}

static int fs_is_p(uint32_t va)
{
    return va && (va == s_fs.pva[0] || va == s_fs.pva[1]);
}

static int fs_z_full(const Nv2aRenderState *rs, uint32_t flags)
{
    return (flags & 1) && (rs->zeta_format == 1 || (flags & 2));
}

/* A clear writes the whole target only over the whole clip rectangle (the
 * hardware clears that rectangle, depth included, whatever the back end's
 * own D3D11 clear covers). */
static void fs_track_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags)
{
    int full = fs_full_rect(s);
    if (flags & 0xF0)
        fs_mark(FS_RT, s->color_va, fs_surf_end(s),
                (flags & 0xF0) == 0xF0 && full ? FS_FULL : FS_WROTE);
    if (flags & 0x03)
        fs_mark(FS_Z, rs->zeta_va, rs->zeta_va,
                fs_z_full(rs, flags) && full ? FS_FULL : FS_WROTE);
}

static uint32_t stages_needed(const Nv2aBatch *b);

static void fs_track_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    uint32_t need = stages_needed(b), st;

    for (st = 0; st < 4; st++)
        if (need & (1u << st))
            fs_read(FS_RT, b->stages[st].offset,
                    b->stages[st].offset + nv2a_backend_texture_bytes(&b->stages[st]));
    if (fs_blend_reads(rs))
        fs_read(FS_RT, s->color_va, fs_surf_end(s));
    fs_mark(FS_RT, s->color_va, fs_surf_end(s), fs_covers(s, b) ? FS_FULL : FS_WROTE);
    if (b->kind == NV2A_BATCH_XF) {
        if ((rs->depth_test_enable && rs->depth_func != 0x207) || rs->stencil_enable)
            fs_read(FS_Z, rs->zeta_va, rs->zeta_va);
        else if (rs->depth_write)
            fs_mark(FS_Z, rs->zeta_va, rs->zeta_va, FS_WROTE);
    }
}

static void fs_journal_reset(void)
{
    s_fs.used = 0;
    s_fs.ops = 0;
    s_fs.zpass = 0;
    s_fs.njt = 0;
    s_fs.jt_lost = 0;
    s_fs.nhc = 0;
}

/* The hash of a stage's texture (palette included) from guest RAM, through
 * the cache; 0 in *ok when it is not in guest memory a surface may read. */
static uint64_t fs_tex_hash(const Nv2aTexture *t, int *ok)
{
    uint32_t bytes = nv2a_backend_texture_bytes(t);
    uint64_t h;
    int i;
    *ok = 0;
    if (!bytes || !nv2a_backend_surface_ok(t->offset, bytes))
        return 0;
    *ok = 1;
    for (i = 0; i < s_fs.nhc; i++)
        if (s_fs.hc[i].off == t->offset && s_fs.hc[i].bytes == bytes
            && s_fs.hc[i].palette == t->palette && s_fs.hc[i].color == t->color)
            return s_fs.hc[i].h;
    h = tex_hash(t, bytes);
    if (s_fs.nhc < FS_HCACHE) {
        FsHash *e = &s_fs.hc[s_fs.nhc++];
        e->off = t->offset; e->bytes = bytes; e->palette = t->palette; e->color = t->color;
        e->h = h;
    }
    return h;
}

/* A stage drawn from a target (this window's work or an earlier one's), not
 * from guest RAM: nothing to hash. */
static int fs_tex_on_gpu(const Nv2aTexture *t)
{
    uint32_t lo = t->offset, hi = lo + nv2a_backend_texture_bytes(t);
    int i;
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used && ranges_overlap(s_rt[i].va, rt_end(&s_rt[i]), lo, hi))
            return 1;
    for (i = 0; i < s_fs.njt; i++)
        if (s_fs.jt[i].kind == FS_RT && ranges_overlap(s_fs.jt[i].va, s_fs.jt[i].end, lo, hi))
            return 1;
    return 0;
}

static void fs_tex_changed(uint32_t va)
{
    int i;
    s_fst.tex_changed++;
    for (i = 0; i < s_fs.ntex_said; i++)
        if (s_fs.tex_said[i] == va)
            return;
    if (s_fs.ntex_said < 64)
        s_fs.tex_said[s_fs.ntex_said++] = va;
    fprintf(stderr, "[GPURAST] frame skip: texture 0x%08X changed between a skipped frame and"
                    " its drawing (a stale texture); drawn from what is there now\n", va);
}

/* Room for `bytes` (16-aligned) in the arena; NULL when it is full. */
static uint8_t *fs_alloc(size_t bytes)
{
    bytes = (bytes + 15) & ~(size_t)15;
    if (s_fs.used + bytes > FS_ARENA_MAX)
        return NULL;
    if (s_fs.used + bytes > s_fs.cap) {
        size_t cap = s_fs.cap ? s_fs.cap : ((size_t)8 << 20);
        uint8_t *p;
        while (cap < s_fs.used + bytes)
            cap *= 2;
        if (cap > FS_ARENA_MAX)
            cap = FS_ARENA_MAX;
        p = (uint8_t *)realloc(s_fs.arena, cap);
        if (!p)
            return NULL;
        s_fs.arena = p;
        s_fs.cap = cap;
    }
    s_fs.used += bytes;
    s_fst.logged++;
    s_fst.logged_bytes += bytes;
    s_fs.ops++;
    return s_fs.arena + s_fs.used - bytes;
}

static void fs_jt(uint32_t kind, uint32_t va, uint32_t end)
{
    if (!fs_add(s_fs.jt, &s_fs.njt, kind, va, end, 0))
        s_fs.jt_lost = 1;
}

static int fs_log_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags,
                        uint32_t argb, uint32_t zs)
{
    FsClear *c = (FsClear *)fs_alloc(sizeof(FsClear));
    if (!c)
        return 0;
    c->op.type = FS_OP_CLEAR;
    c->op.bytes = (uint32_t)((sizeof(FsClear) + 15) & ~(size_t)15);
    c->s = *s; c->rs = *rs; c->flags = flags; c->argb = argb; c->zs = zs;
    if (flags & 0xF0)
        fs_jt(FS_RT, s->color_va, fs_surf_end(s));
    if (flags & 0x03)
        fs_jt(FS_Z, rs->zeta_va, rs->zeta_va);
    return 1;
}

#define FS_DRAW_HEAD (((sizeof(FsDraw) + 15) / 16) * 16)

static int fs_log_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    size_t vbytes = b->vertices_ex ? (size_t)b->count * VTX_BYTES : 0;
    FsDraw *d = (FsDraw *)fs_alloc(FS_DRAW_HEAD + vbytes);
    const Nv2aRenderState *rs = b->state;
    if (!d)
        return 0;
    d->op.type = FS_OP_DRAW;
    d->op.bytes = (uint32_t)((FS_DRAW_HEAD + vbytes + 15) & ~(size_t)15);
    d->s = *s;
    d->b = *b;
    d->rs = *rs;
    d->has_rc = b->rc != NULL;
    if (b->rc)
        d->rc = *b->rc;
    if (vbytes)
        memcpy((uint8_t *)d + FS_DRAW_HEAD, b->vertices_ex, vbytes);
    d->hashed = 0;
    d->window_no = s_fs.window_no;
    d->flip_no = s_flip_no;
    {
        uint32_t need = stages_needed(b), st;
        for (st = 0; st < 4; st++) {
            int ok;
            if (!(need & (1u << st)) || fs_tex_on_gpu(&b->stages[st]))
                continue;
            d->thash[st] = fs_tex_hash(&b->stages[st], &ok);
            if (ok)
                d->hashed |= 1u << st;
        }
    }
    fs_jt(FS_RT, s->color_va, fs_surf_end(s));
    if (b->kind == NV2A_BATCH_XF && (rs->depth_write || rs->stencil_enable))
        fs_jt(FS_Z, rs->zeta_va, rs->zeta_va);
    if (b->kind == NV2A_BATCH_XF && rs->zpass_enable)
        s_fs.zpass = 1;
    return 1;
}

/* The journal drawn, in order, as it was logged; the window goes on as SHOW. */
static void fs_replay(int why)
{
    size_t off = 0;
    uint32_t ops = s_fs.ops;

    s_fs.replaying = 1;
    s_fs.nhc = 0;                       /* hashes of what is in guest RAM now */
    while (off < s_fs.used) {
        FsOp *op = (FsOp *)(s_fs.arena + off);
        if (op->type == FS_OP_CLEAR) {
            FsClear *c = (FsClear *)op;
            fs_track_clear(&c->s, &c->rs, c->flags);
            exec_clear(&c->s, &c->rs, c->flags, c->argb, c->zs);
        } else {
            FsDraw *d = (FsDraw *)op;
            Nv2aBatch b = d->b;
            b.state = &d->rs;
            b.rc = d->has_rc ? &d->rc : NULL;
            b.vertices_ex = b.vertices_ex ? (const Nv2aVertexEx *)((uint8_t *)d + FS_DRAW_HEAD)
                                          : NULL;
            b.vertices = NULL;
            b.texture = NULL;
            if (d->hashed) {
                uint32_t st;
                for (st = 0; st < 4; st++) {
                    int ok;
                    if ((d->hashed & (1u << st))
                        && fs_tex_hash(&b.stages[st], &ok) != d->thash[st]) {
                        FS_TRACE("[FSKIP] stale texture 0x%08X (stage %u, fmt 0x%02X %ux%u) into"
                                 " 0x%08X: logged in window %u at flip %u, drawn in window %u at"
                                 " flip %u (%s)\n", b.stages[st].offset, st, b.stages[st].color,
                                 b.stages[st].width, b.stages[st].height, d->s.color_va,
                                 d->window_no, d->flip_no, s_fs.window_no, s_flip_no,
                                 why == FSR_READY ? "shown after all" : why == FSR_OBSERVED
                                 ? "observed" : why == FSR_NOTSELF ? "not self-contained"
                                 : "arena full");
                        fs_tex_changed(b.stages[st].offset);
                    }
                }
            }
            fs_track_draw(&d->s, &b);
            exec_draw(&d->s, &b);
        }
        off += op->bytes;
    }
    s_fs.replaying = 0;
    if (ops)
        s_fst.replayed[why]++;
    fs_journal_reset();
    s_fs.mode = FS_SHOW;
}

/* The journal away; what it wrote is dropped until overwritten. */
static void fs_drop(void)
{
    int i;
    for (i = 0; i < s_fs.njt; i++)
        fs_add(s_fs.dropped, &s_fs.ndropped, s_fs.jt[i].kind, s_fs.jt[i].va,
               s_fs.jt[i].end, 0);
    if (s_fs.ops)
        s_fst.dropped_windows++;
    fs_journal_reset();
}

/* Every target the journal writes was self-contained in the last SHOW window. */
static int fs_journal_self_contained(void)
{
    int i;
    if (s_fs.jt_lost) {
        FS_TRACE("[FSKIP] window %u drawn: more than %d targets written\n", s_fs.window_no,
                 FS_MAX_T);
        return 0;
    }
    for (i = 0; i < s_fs.njt; i++)
        if (fs_find(s_fs.selfc, s_fs.nselfc, s_fs.jt[i].kind, s_fs.jt[i].va) < 0) {
            FS_TRACE("[FSKIP] window %u drawn: %s 0x%08X not self-contained in the last shown"
                     " window (%s)\n", s_fs.window_no, s_fs.jt[i].kind == FS_Z ? "depth" : "colour",
                     s_fs.jt[i].va, fs_find(s_fs.seen, s_fs.nseen, s_fs.jt[i].kind,
                                            s_fs.jt[i].va) >= 0 ? "read before overwritten"
                                                                 : "untouched there");
            return 0;
        }
    return 1;
}

/* A boundary: the window that ends is committed (SHOW) or its journal is
 * dropped or drawn; the next one starts in next_mode. A journal that cannot
 * be dropped is drawn here even when the next window is skipped too: carried
 * on, it would sample guest RAM textures windows later (the title's CPU goes
 * on writing them). */
static void fs_boundary(void)
{
    int i;
    s_fs.armed = 0;
    s_fs.window_no++;
    if (s_fs.mode == FS_SHOW) {
        memcpy(s_fs.seen, s_fs.win, sizeof s_fs.win);
        s_fs.nseen = s_fs.nwin;
        s_fs.nselfc = 0;
        for (i = 0; i < s_fs.nwin; i++)
            if (s_fs.win[i].cls == FS_FULL || s_fs.win[i].cls == FS_WROTE)
                fs_add(s_fs.selfc, &s_fs.nselfc, s_fs.win[i].kind, s_fs.win[i].va,
                       s_fs.win[i].end, s_fs.win[i].cls);
    } else if (s_fs.ops) {
        if (fs_journal_self_contained())
            fs_drop();
        else
            fs_replay(FSR_NOTSELF);
    }
    s_fs.nwin = 0;
    s_fs.mode = s_fs.next_mode;
}

/* Something reads the GPU's work in a SKIP window: draw the journal first. */
static void fs_observe(void)
{
    if (s_fs.on && !s_fs.replaying && s_fs.mode == FS_SKIP && s_fs.ops)
        fs_replay(FSR_OBSERVED);
}

static void fs_release(void)
{
    free(s_fs.arena);
    memset(&s_fs, 0, sizeof s_fs);
}

static void gpu_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                      uint32_t flags, uint32_t argb, uint32_t zs)
{
    if (s_fs.on && !s_fs.replaying) {
        if (s_fs.armed && (flags & 0xF0) == 0xF0 && fs_is_p(s->color_va) && fs_full_rect(s))
            fs_boundary();
        if (s_fs.mode == FS_SKIP) {
            if (fs_log_clear(s, rs, flags, argb, zs))
                return;
            fs_replay(FSR_OVERFLOW);
        }
        fs_track_clear(s, rs, flags);
    }
    exec_clear(s, rs, flags, argb, zs);
}

static void gpu_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    if (s_fs.on && !s_fs.replaying) {
        if (s_fs.armed && fs_is_p(s->color_va) && fs_covers(s, b))
            fs_boundary();
        if (s_fs.mode == FS_SKIP) {
            if (fs_log_draw(s, b))
                return;
            fs_replay(FSR_OVERFLOW);
        }
        fs_track_draw(s, b);
    }
    exec_draw(s, b);
}

/* ---- sync, reports, flips ----------------------------------------------- */

static void gpu_sync(uint32_t reason)
{
    int i;

    if (s_compare)
        return;
    if (reason == NV2A_BACKEND_SYNC_READ || reason == NV2A_BACKEND_SYNC_PRESENT) {
        fs_observe();
        if (s_fs.on)
            for (i = 0; i < MAX_RTS; i++)
                if (s_rt[i].used && s_rt[i].dirty)
                    fs_read(FS_RT, s_rt[i].va, rt_end(&s_rt[i]));
    }
    /* Lazily: guest RAM gets the targets when something reads them there --
     * the presenter at a flip, the executor's dumps -- and, in gpu_draw, when
     * a stage samples one through guest RAM. The scan ends and the methods
     * that drain the executor only end the epoch. RECOMP_GPU_RASTER_SYNC=all
     * writes back at every one of them, for a title whose CPU reads what the
     * GPU drew. */
    if (s_sync_all || reason == NV2A_BACKEND_SYNC_READ
        || reason == NV2A_BACKEND_SYNC_PRESENT)
        for (i = 0; i < MAX_RTS; i++)
            if (s_rt[i].used && s_rt[i].dirty) {
                s_st.wb_why[2]++;
                rt_writeback(&s_rt[i]);
            }
    if (reason == NV2A_BACKEND_SYNC_SCAN_END || reason == NV2A_BACKEND_SYNC_METHOD)
        s_epoch++;
}

/* The flip's present: only the targets over the surface it reads. */
static void gpu_sync_surface(uint32_t va, uint32_t bytes)
{
    int i;

    if (s_compare)
        return;
    fs_observe();
    if (s_fs.on)
        fs_read(FS_RT, va, va + bytes);
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used && s_rt[i].dirty
            && ranges_overlap(s_rt[i].va, rt_end(&s_rt[i]), va, va + bytes)) {
            s_st.wb_why[3]++;
            rt_writeback(&s_rt[i]);
        }
}

static uint32_t gpu_report(void)
{
    uint64_t n;

    if (s_compare)
        return 0;
    /* A count from a draw only logged: draw the journal so it is real. */
    if (s_fs.on && !s_fs.replaying && s_fs.mode == FS_SKIP && s_fs.zpass)
        fs_replay(FSR_OBSERVED);
    GPROF_HOST(GPH_REPORTS, 1);
    query_resolve_all();
    n = s_zpass_acc / ((uint64_t)s_sx * s_sy * s_msaa);
    s_zpass_acc = 0;
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

/* 24-bit bottom-up BMP of w*h ARGB texels, as dump_surface_bmp writes. */
static void write_bmp(const char *path, const uint32_t *argb, uint32_t w, uint32_t h)
{
    uint32_t row_bytes = w * 3, pad = (4 - (row_bytes & 3)) & 3, y, x;
    uint32_t filesz = 54 + (row_bytes + pad) * h;
    uint8_t hdr[54];
    FILE *f = fopen(path, "wb");

    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = h; y-- > 0; ) {
        for (x = 0; x < w; x++) {
            uint32_t v = argb[(size_t)y * w + x];
            uint8_t bgr[3];
            bgr[0] = (uint8_t)v; bgr[1] = (uint8_t)(v >> 8); bgr[2] = (uint8_t)(v >> 16);
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = { 0, 0, 0 };
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
}

/* RECOMP_GPU_RASTER=compare: what the GPU drew this frame against what the
 * software path left in guest RAM. */
static void compare_flip(void)
{
    static int dumped;
    const char *prefix = getenv("RECOMP_FB_DUMP");
    int i;

    for (i = 0; i < MAX_RTS; i++) {
        GpuRt *r = &s_rt[i];
        D3D11_BOX box;
        D3D11_MAPPED_SUBRESOURCE m;
        uint32_t x0, y0, x1, y1, x, y, w, h, maxd = 0;
        uint64_t differ = 0, total;
        uint32_t *gbuf = NULL, *sbuf = NULL;

        if (!r->used || !r->dirty)
            continue;
        x0 = r->dx0; y0 = r->dy0;
        x1 = r->dx1 < r->W ? r->dx1 : r->W;
        y1 = r->dy1 < r->H ? r->dy1 : r->H;
        r->dirty = 0;
        if (x0 >= x1 || y0 >= y1
            || !nv2a_backend_surface_ok(r->va + y0 * r->pitch, (y1 - y0) * r->pitch))
            continue;
        w = x1 - x0; h = y1 - y0;
        box.left = x0; box.right = x1; box.top = y0; box.bottom = y1;
        box.front = 0; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_staging, 0,
                                                  x0, y0, 0, (ID3D11Resource *)r->tex,
                                                  0, &box);
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_staging, 0,
                                           D3D11_MAP_READ, 0, &m)))
            continue;
        if (prefix && dumped < 4) {
            gbuf = (uint32_t *)malloc((size_t)w * h * 4);
            sbuf = (uint32_t *)malloc((size_t)w * h * 4);
        }
        for (y = y0; y < y1; y++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData
                                                     + (size_t)y * m.RowPitch);
            const uint8_t *g = guest(r->va) + (size_t)y * r->pitch;
            for (x = x0; x < x1; x++) {
                uint32_t gv, sv, k, d, worst = 0;
                if (r->bpp == 4) {
                    gv = src[x];
                    sv = ((const uint32_t *)g)[x];
                } else {
                    gv = expand565(pack565(src[x]));
                    sv = expand565(((const uint16_t *)g)[x]);
                }
                for (k = 0; k < 32; k += 8) {
                    int a = (int)((gv >> k) & 0xFF), c = (int)((sv >> k) & 0xFF);
                    d = (uint32_t)(a > c ? a - c : c - a);
                    if (d > worst)
                        worst = d;
                }
                if (worst > 4)
                    differ++;
                if (worst > maxd)
                    maxd = worst;
                if (gbuf && sbuf) {
                    gbuf[(size_t)(y - y0) * w + (x - x0)] = gv;
                    sbuf[(size_t)(y - y0) * w + (x - x0)] = sv;
                }
            }
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
        total = (uint64_t)w * h;
        fprintf(stderr, "[GPURAST] compare flip %u: surface 0x%08X %.2f%% pixels"
                        " differ >4, max delta %u, far-reaching triangles %u\n",
                s_flip_no, r->va, total ? 100.0 * (double)differ / (double)total : 0.0,
                maxd, s_far_tris);
        if (gbuf && sbuf && total && differ * 100 > total) {
            char path[512];
            snprintf(path, sizeof path, "%sgpu_%05u_%08X.bmp", prefix, s_flip_no, r->va);
            write_bmp(path, gbuf, w, h);
            snprintf(path, sizeof path, "%ssw_%05u_%08X.bmp", prefix, s_flip_no, r->va);
            write_bmp(path, sbuf, w, h);
            dumped++;
        }
        free(gbuf);
        free(sbuf);
    }
    s_far_tris = 0;
    /* Start the next frame from what the software path drew. */
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used) {
            s_rt[i].dirty = 0;
            s_rt[i].need_full = 1;
        }
}

static void tex_evict(void)
{
    int i;
    for (i = 0; i < MAX_TEXS; i++)
        if (s_tex[i].used && s_flip_no - s_tex[i].last_flip > TEX_EVICT_FLIPS)
            tex_release(&s_tex[i]);
}

static void release_all(void);

/* RECOMP_GPU_RASTER_LOSE_AT=<flip>: behave as if the device were lost at that
 * flip -- the one way to run the path below without a driver reset. */
static uint32_t s_lose_at;

/* A lost device (driver reset, TDR, update, the adapter gone): every call
 * fails from then on, Present included, and the window would stand frozen.
 * The executor's own rasteriser takes over from the next frame: the back end
 * unregisters and lets go of the device, and the window presents guest RAM
 * again. Checked at the flip, after which the executor calls the back end no
 * more this frame -- not inside a draw that is still using the context. */
static int gpu_lost(void)
{
    HRESULT why = ID3D11Device_GetDeviceRemovedReason(s_dev);

    if (SUCCEEDED(why) && !(s_lose_at && s_flip_no >= s_lose_at))
        return 0;
    fprintf(stderr, "[GPURAST] D3D11 device lost (0x%08lX) at flip %u; the software"
                    " rasteriser takes over\n", (unsigned long)why, s_flip_no);
    nv2a_backend_register(NULL);
    xbox_FramebufferWindowExternal(0);
    release_all();
    return 1;
}

static void gpu_flip(void)
{
    DWORD now = GetTickCount();

    if (s_compare)
        compare_flip();
    s_epoch++;
    s_flip_no++;
    s_st.flips++;
    tex_evict();
    if (!s_st.last_tick)
        s_st.last_tick = now;
    if (now - s_st.last_tick >= 10000) {
        char line[1536];
        TextBlock tb;
        text_block_init(&tb, line, sizeof line, stderr);
        text_block_printf(&tb, "[GPURAST] %.1fs: flips %u, draws %u, writebacks %u (%.1f MB),"
                        " guest->rt uploads %u (%.1f MB), tex decodes %u, DXT as BC %u,"
                        " rt as texture %u, GPU presents %u, readback stall ms %.1f"
                        " (writebacks for: textures %u, dropped targets %u, syncs %u,"
                        " presents %u), dropped as taken by the title %u, combiners: own %u, generic %u"
                        " draws (%u states without a shader)\n",
                (double)(now - s_st.last_tick) / 1000.0, s_st.flips, s_st.draws,
                s_st.writebacks, (double)s_st.wb_bytes / 1048576.0,
                s_st.uploads, (double)s_st.up_bytes / 1048576.0,
                s_st.decodes, s_st.bc_uploads, s_st.rt_tex, s_st.presents, s_st.stall_ms,
                s_st.wb_why[0], s_st.wb_why[1], s_st.wb_why[2], s_st.wb_why[3],
                s_st.taken, s_st.rc_own, s_st.rc_generic, s_rc_nmiss);
        if (s_fs.on) {
            FsStats d = s_fst, o = s_fst_said;
            text_block_printf(&tb, "[GPURAST] frame skip: shown %u, skipped %u, not ready %u"
                            " (presenter %u / gpu %u), logged ops %u (%.1f MB over the"
                            " period, arena %.1f MB), windows"
                            " dropped %u, replayed %u (ready %u / observed %u / not"
                            " self-contained %u / arena full %u), mispredicted %u (shown"
                            " a skipped window %u, drew a hidden one %u), stale reads %u,"
                            " stale textures %u\n",
                    d.shown - o.shown, d.skipped - o.skipped,
                    d.nr_presenter + d.nr_gpu - o.nr_presenter - o.nr_gpu,
                    d.nr_presenter - o.nr_presenter, d.nr_gpu - o.nr_gpu,
                    d.logged - o.logged, (double)(d.logged_bytes - o.logged_bytes) / 1048576.0,
                    (double)s_fs.cap / 1048576.0,
                    d.dropped_windows - o.dropped_windows,
                    d.replayed[0] + d.replayed[1] + d.replayed[2] + d.replayed[3]
                    - o.replayed[0] - o.replayed[1] - o.replayed[2] - o.replayed[3],
                    d.replayed[0] - o.replayed[0], d.replayed[1] - o.replayed[1],
                    d.replayed[2] - o.replayed[2], d.replayed[3] - o.replayed[3],
                    d.mispredicted_skip + d.mispredicted_show
                    - o.mispredicted_skip - o.mispredicted_show,
                    d.mispredicted_skip - o.mispredicted_skip,
                    d.mispredicted_show - o.mispredicted_show, d.stale - o.stale,
                    d.tex_changed - o.tex_changed);
            s_fst_said = s_fst;
        }
        text_block_flush(&tb);
        if (s_rc_stats) {
            char rline[8192];           /* RC_TOP lines of up to ~400 bytes */
            TextBlock tbr;
            text_block_init(&tbr, rline, sizeof rline, stderr);
            rc_report(&tbr);
            text_block_flush(&tbr);
        }
        memset(&s_st, 0, sizeof s_st);
        s_st.last_tick = now;
    }
    if (g_nv2a_gprof_on)
        nv2a_gprof_frame();
    pscheck_report();
    gpu_lost();
}

/* ---- install ------------------------------------------------------------ */

static Nv2aBackend s_be;
static void present_release(void);

static void release_all(void)
{
    int i;
    nv2a_gprof_release();               /* its queries first: some may be out */
    pscheck_release();
    fs_release();                       /* the journal: nothing of it is drawn now */
#ifdef NV2A_GPU_TEST_HOOKS
    s_ps_override = NULL;
    REL(s_ps_ovr_cache);
    s_ovr_code = NULL;
    s_nps_blobs = 0;
#endif
    rc_states_release();
    for (i = 0; i < MAX_RTS; i++)
        rt_release(&s_rt[i]);
    for (i = 0; i < MAX_ZBUFS; i++) {
        REL(s_z[i].dsv);
        REL(s_z[i].tex);
        s_z[i].used = 0;
    }
    for (i = 0; i < MAX_TEXS; i++)
        tex_release(&s_tex[i]);
    for (i = 0; i < s_nds; i++)
        REL(s_ds[i].s);
    s_nds = 0;
    for (i = 0; i < s_nbl; i++)
        REL(s_bl[i].s);
    s_nbl = 0;
    for (i = 0; i < s_qfree; i++)
        REL(s_qpool[i]);
    s_qfree = 0;
    /* Unresolved too: a lost device lets go mid-frame, with queries out. */
    for (i = 0; i < s_npend; i++)
        REL(s_qpend[i]);
    s_npend = 0;
    s_qmade = 0;
    REL(s_white_srv);
    REL(s_white);
    present_release();
    REL(s_cb_blit);
    REL(s_ss_rtv);
    REL(s_ss_srv);
    REL(s_ss);
    REL(s_bps_down);
    REL(s_bps_up);
    REL(s_bvs);
    REL(s_down_rtv);
    REL(s_down);
    REL(s_up_srv);
    REL(s_up);
    REL(s_staging);
    REL(s_rast);
    REL(s_cb_ps);
    REL(s_cb_vs);
    REL(s_vb);
    REL(s_il);
    REL(s_ps_flat);
    for (i = 0; i < 3; i++)
        REL(s_ps_rc[i]);
    REL(s_ps_ffp);
    REL(s_ps_screen);
    REL(s_vs);
    REL(s_ctx);
    REL(s_dev);
}

/* The state the rasteriser never changes; the presenter puts it back. */
static void bind_pipeline(void)
{
    UINT stride = VTX_BYTES, offset = 0;
    ID3D11Buffer *cbs[2];
    D3D11_VIEWPORT vp;

    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_il);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps_screen, NULL, 0);
    s_ps_bound = s_ps_screen;
    cbs[0] = s_cb_vs;
    cbs[1] = s_cb_ps;
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 2, cbs);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 2, cbs);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rast);
    /* The vertex shader maps surface pixels onto RT_W x RT_H; the targets
     * may be shorter (s_gh), which only clips. */
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)(RT_W * s_sx); vp.Height = (float)(RT_H * s_sy);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
}

static ID3DBlob *compile_src(const char *src, size_t len, const char *entry,
                             const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, len, "nv2a_gpu_d3d11", NULL, NULL,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPURAST] %s (%s) does not compile: 0x%08lX\n%s\n", entry,
                target, (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        REL(code);
    }
    REL(err);
    return code;
}

/* A pixel shader of nv2a_gpu_shaders.h into *out; tests see its code
 * (nv2a_gpu_test_ps_blob) under `name`. */
static int make_ps(const void *code, size_t len, const char *name, ID3D11PixelShader **out)
{
    int ok = SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, code, len, NULL, out));
#ifdef NV2A_GPU_TEST_HOOKS
    if (ok && s_nps_blobs < MAX_PS_BLOBS) {
        s_ps_code[s_nps_blobs] = code;
        s_ps_len[s_nps_blobs] = len;
        s_ps_names[s_nps_blobs++] = name;
    }
#else
    (void)name;
#endif
    return ok;
}

/* ---- presenting from the GPU --------------------------------------------
 *
 * The framebuffer window (src/video/fb_present.c, RECOMP_FB_WINDOW) shows
 * frames with GDI from guest RAM, which with this back end meant reading the
 * presented frame back from the GPU at every flip. Instead a swap chain goes
 * on that window and the presented target is drawn into its back buffer
 * directly: the whole frame -- 640x480 times the surface's anti-aliasing
 * factor and the internal scale, or s_ss under SSAA -- filtered into the
 * rectangle xbox_FramebufferWindowFit gives (the display aspect, bars around it).
 * The window thread keeps the title bar, the keyboard and the close button.
 * Without a window there is nothing to show, so nothing is read back at all.
 * The frame reaches guest RAM only when something reads it there (the
 * executor's dumps, a texture decoded from it, a target being dropped).
 * RECOMP_GPU_RASTER_PRESENT=0 keeps the window on GDI and guest RAM. The
 * swap chain is created the way upstream gpu_present.c (MIT) creates its
 * own; Present runs on this thread, the window's messages on its own.
 *
 * With RECOMP_FRAME_SKIP=auto the chain is a flip-model one (FLIP_DISCARD,
 * two buffers) with a frame latency waitable object at latency 2
 * (RECOMP_GPU_RASTER_FRAME_LATENCY; at 1 a slow GPU frame missed the next
 * flip's waitable with no limiter at all): pace()
 * shows a frame only when the waitable is signalled -- the chain takes a
 * frame without blocking -- and the GPU has finished the last shown one (a
 * D3D11_QUERY_EVENT after each Present). An outside limiter (gamescope's FIFO
 * on Steam Deck, NVIDIA's Max Frame Rate, RTSS) then holds back the shows,
 * not the executor: the frames in between are skipped. No thread of its own:
 * DXGI must not run beside the immediate context, and DXVK keeps the context
 * locked through part of Present (stage3 docs-report). GDI cannot draw into a
 * flip-model window, so a frame that is only in guest RAM (a Bink movie, a
 * loading screen the CPU drew) goes up into a texture and through the same
 * shader. Without the flip model or the waitable (an old system, an error)
 * auto falls back to this bitblt chain and the cadence alone. Frame skip 0
 * and 1 keep the bitblt chain. */
#define PRESENT_W 640
#define PRESENT_H 480

/* The present shader (pvs/pps in nv2a_gpu_shaders.hlsl, k_pvs/k_pps): the
 * frame bilinear into the rectangle, as described there. */

static int s_present_on = 1, s_present_failed;
static void write_bmp(const char *path, const uint32_t *argb, uint32_t w, uint32_t h);

/* RECOMP_GPU_RASTER_PRESENT_DUMP=<prefix>: every RECOMP_GPU_RASTER_PRESENT_DUMP_EVERY-th
 * present (default 30) as <prefix>NNNNNN.bmp, 640x480 as the window shows it --
 * HUD, menus and dialogs included, which the executor's own dump (the largest
 * surface drawn) leaves out when a title composes into a second surface. */
static void present_dump(GpuRt *r, uint32_t sx, uint32_t sy)
{
    static const char *prefix = (const char *)-1;
    static int every;
    static uint32_t n;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t *buf, x, y, w = PRESENT_W * sx, h = PRESENT_H * sy;
    char path[512];

    if (prefix == (const char *)-1) {
        const char *e = getenv("RECOMP_GPU_RASTER_PRESENT_DUMP_EVERY");
        prefix = getenv("RECOMP_GPU_RASTER_PRESENT_DUMP");
        every = e ? atoi(e) : 30;
        if (every < 1)
            every = 1;
    }
    if (!prefix || (n++ % (uint32_t)every) != 0)
        return;
    if (w > r->W) w = r->W;
    if (h > r->H) h = r->H;
    rt_read_native(r, 0, 0, w, h);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return;
    buf = (uint32_t *)malloc((size_t)PRESENT_W * PRESENT_H * 4);
    if (buf) {
        for (y = 0; y < PRESENT_H; y++)
            for (x = 0; x < PRESENT_W; x++) {
                uint32_t px = x * sx, py = y * sy;
                buf[(size_t)y * PRESENT_W + x] = (px < w && py < h)
                    ? ((const uint32_t *)((const uint8_t *)m.pData + (size_t)py * m.RowPitch))[px]
                    : 0;
            }
        snprintf(path, sizeof path, "%s%06u.bmp", prefix, n - 1);
        write_bmp(path, buf, PRESENT_W, PRESENT_H);
        free(buf);
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
}
static IDXGISwapChain *s_chain;
static ID3D11RenderTargetView *s_bb_rtv;
static uint32_t s_bb_w, s_bb_h;          /* back buffer = the client area */
static ID3D11VertexShader *s_pvs;
static ID3D11PixelShader *s_pps;
static ID3D11Buffer *s_cb_present;
static int s_fs_auto;                    /* RECOMP_FRAME_SKIP=auto: a flip-model chain */
static int s_flip_model;                 /* the chain is FLIP_DISCARD with a waitable */
static HANDLE s_waitable;                /* its frame latency waitable object */
static int s_have_token;                 /* the waitable was taken, nothing presented on it */
static ID3D11Query *s_present_ev;        /* D3D11_QUERY_EVENT after the last shown frame */
static int s_present_ev_issued;          /* ... ended at least once: one never issued is ready */
static ID3D11Texture2D *s_gdi_tex;       /* a guest RAM frame, for the flip-model chain */
static ID3D11ShaderResourceView *s_gdi_srv;
static double s_present_hz;              /* RECOMP_GPU_RASTER_PRESENT_HZ: a limiter acted out */
static double s_hz_slot_now, s_hz_slot_shown = -1.0;   /* its slots: now, the last shown */
#ifdef NV2A_GPU_TEST_HOOKS
static int s_test_gate = -1;             /* nv2a_gpu_test_present_gate: -1 the real one */
static int s_test_gpu_busy;              /* nv2a_gpu_test_gpu_busy */
static int s_test_present_delay_ms;      /* nv2a_gpu_test_present_delay */
#endif

static void client_size(HWND hwnd, uint32_t *w, uint32_t *h)
{
    RECT cr;

    if (!GetClientRect(hwnd, &cr) || cr.right < 1 || cr.bottom < 1) {
        *w = *h = 0;                    /* minimised */
        return;
    }
    *w = (uint32_t)cr.right;
    *h = (uint32_t)cr.bottom;
}

/* The flip-model chain with its waitable; 0 (and nothing made) when either
 * cannot be had. */
static int present_setup_flip(IDXGIFactory *xf, HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    IDXGISwapChain2 *c2 = NULL;
    HRESULT hr;

    memset(&sd, 0, sizeof sd);
    sd.BufferDesc.Width = s_bb_w;
    sd.BufferDesc.Height = s_bb_h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    hr = IDXGIFactory_CreateSwapChain(xf, (IUnknown *)s_dev, &sd, &s_chain);
    if (SUCCEEDED(hr))
        hr = IDXGISwapChain_QueryInterface(s_chain, &IID_IDXGISwapChain2, (void **)&c2);
    if (SUCCEEDED(hr)) {
        /* RECOMP_GPU_RASTER_FRAME_LATENCY=<1-3> (default 2): frames the chain may
         * hold. At 1 a frame whose GPU work plus the compositor's next vblank
         * outlasts a flip period (6x5 + MSAA 2 in a 143 Hz window: 465-582 of
         * 600 shown) keeps the waitable unsignalled at the next flip, and auto
         * skipped frames with no limiter at all; at 2, 593-600 of 600. The
         * price: a frame more in the queue, only when the GPU is slower than a
         * flip period; an outside limiter still fills the queue and shows. */
        const char *fl = getenv("RECOMP_GPU_RASTER_FRAME_LATENCY");
        UINT lat = fl ? (UINT)atoi(fl) : 2;
        IDXGISwapChain2_SetMaximumFrameLatency(c2, lat < 1 ? 1 : lat > 3 ? 3 : lat);
        s_waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject(c2);
        REL(c2);
    }
    if (SUCCEEDED(hr) && s_waitable) {
        s_flip_model = 1;
        return 1;
    }
    fprintf(stderr, "[GPURAST] frame skip auto: no flip-model swap chain with a waitable"
                    " (0x%08lX); auto is off, the frames are shown by the cadence alone\n",
            (unsigned long)hr);
    REL(s_chain);
    return 0;
}

static int present_setup(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    D3D11_BUFFER_DESC bd;
    D3D11_QUERY_DESC qd;
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *xa = NULL;
    IDXGIFactory *xf = NULL;
    ID3D11Texture2D *back = NULL;
    int ok = 0;

    if (FAILED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&xd))
        || FAILED(IDXGIDevice_GetAdapter(xd, &xa))
        || FAILED(IDXGIAdapter_GetParent(xa, &IID_IDXGIFactory, (void **)&xf)))
        goto done;
    client_size(hwnd, &s_bb_w, &s_bb_h);
    if (!s_bb_w) {
        s_bb_w = PRESENT_W;
        s_bb_h = PRESENT_H;
    }
    if (!(s_fs_auto && present_setup_flip(xf, hwnd))) {
        memset(&sd, 0, sizeof sd);
        sd.BufferDesc.Width = s_bb_w;
        sd.BufferDesc.Height = s_bb_h;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = hwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (FAILED(IDXGIFactory_CreateSwapChain(xf, (IUnknown *)s_dev, &sd, &s_chain)))
            goto done;
    }
    IDXGIFactory_MakeWindowAssociation(xf, hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(IDXGISwapChain_GetBuffer(s_chain, 0, &IID_ID3D11Texture2D, (void **)&back))
        || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)back, NULL,
                                                      &s_bb_rtv)))
        goto done;
    if (FAILED(ID3D11Device_CreateVertexShader(s_dev, k_pvs, sizeof k_pvs, NULL, &s_pvs))
        || !make_ps(k_pps, sizeof k_pps, "pps", &s_pps))
        goto done;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_present)))
        goto done;
    if (s_fs_auto && !s_present_ev) {   /* only readiness reads it: 0/1 as before */
        s_present_ev_issued = 0;
        qd.Query = D3D11_QUERY_EVENT;
        qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &s_present_ev)))
            s_present_ev = NULL;
    }
    ok = 1;
    fprintf(stderr, "[GPURAST] presenting into the framebuffer window from the GPU%s\n",
            s_flip_model ? " (flip model, waitable, RECOMP_GPU_RASTER_FRAME_LATENCY frames)" : "");
done:
    REL(back);
    REL(xf);
    REL(xa);
    REL(xd);
    return ok;
}

static void present_release(void)
{
    REL(s_gdi_srv);
    REL(s_gdi_tex);
    REL(s_present_ev);
    s_present_ev_issued = 0;
    REL(s_cb_present);
    REL(s_pps);
    REL(s_pvs);
    REL(s_bb_rtv);
    REL(s_chain);
    if (s_waitable)
        CloseHandle(s_waitable);
    s_waitable = NULL;
    s_flip_model = 0;
    s_have_token = 0;
}

/* A frame that is only in guest RAM (640x480 at va: 32-bit, a 2x-wide
 * anti-aliased 32-bit surface read every other pixel and every aa_sy-th row,
 * or R5G6B5) into s_gdi_tex, as fb_present.c's GDI path reads it. */
static int gdi_upload(uint32_t va, uint32_t pitch, uint32_t aa_sy)
{
    uint32_t bpp = pitch / PRESENT_W, x, y, rows = PRESENT_H * (bpp == 8 ? aa_sy : 1);
    uint32_t *buf = scratch((size_t)PRESENT_W * PRESENT_H);

    if (!buf || (bpp != 2 && bpp != 4 && bpp != 8)
        || !nv2a_backend_surface_ok(va, rows * pitch))
        return 0;
    if (!s_gdi_tex) {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        memset(&td, 0, sizeof td);
        td.Width = PRESENT_W; td.Height = PRESENT_H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        memset(&vd, 0, sizeof vd);
        vd.Format = td.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.ArraySize = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_gdi_tex))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_gdi_tex,
                                                            &vd, &s_gdi_srv))) {
            REL(s_gdi_srv);
            REL(s_gdi_tex);
            return 0;
        }
    }
    for (y = 0; y < PRESENT_H; y++) {
        const uint8_t *row = guest(va) + (size_t)y * pitch * (bpp == 8 ? aa_sy : 1);
        uint32_t *dst = buf + (size_t)y * PRESENT_W;
        for (x = 0; x < PRESENT_W; x++)
            dst[x] = bpp == 4 ? ((const uint32_t *)row)[x]
                   : bpp == 8 ? ((const uint32_t *)row)[x * 2]
                   : expand565(((const uint16_t *)row)[x]);
    }
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_gdi_tex, 0, NULL, buf,
                                          PRESENT_W * 4, 0);
    return 1;
}

/* What the shader shows (`src`, the frame fsw x fsh texels of it) into the
 * window, and Present. 0: the chain is gone (the guest RAM path takes over). */
static int present_frame(HWND hwnd, ID3D11ShaderResourceView *src, float fsw, float fsh)
{
    static const float zero[4] = { 0, 0, 0, 0 };
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    D3D11_RECT rc;
    float pk[12];
    int32_t fit[4];
    uint32_t cw, ch;

    /* The back buffer follows the window: resized, maximised, F11. */
    client_size(hwnd, &cw, &ch);
    if (!cw)                            /* minimised: nothing to draw into */
        return 1;
    if (cw != s_bb_w || ch != s_bb_h) {
        ID3D11Texture2D *back = NULL;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        REL(s_bb_rtv);
        if (s_flip_model)               /* every reference to the buffers gone first */
            ID3D11DeviceContext_Flush(s_ctx);
        if (FAILED(IDXGISwapChain_ResizeBuffers(s_chain, 0, cw, ch, DXGI_FORMAT_UNKNOWN,
                       s_flip_model ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0))
            || FAILED(IDXGISwapChain_GetBuffer(s_chain, 0, &IID_ID3D11Texture2D,
                                               (void **)&back))
            || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)back,
                                                          NULL, &s_bb_rtv))) {
            REL(back);
            fprintf(stderr, "[GPURAST] swap chain resize to %ux%u failed; presenting"
                            " through guest RAM\n", cw, ch);
            present_release();
            s_present_failed = 1;
            xbox_FramebufferWindowExternal(0);
            bind_pipeline();
            return 0;
        }
        REL(back);
        s_bb_w = cw;
        s_bb_h = ch;
    }
    xbox_FramebufferWindowFit(s_bb_w, s_bb_h, fit);
    memset(pk, 0, sizeof pk);
    pk[4] = (float)fit[0]; pk[5] = (float)fit[1];
    pk[6] = (float)fit[2]; pk[7] = (float)fit[3];
    pk[8] = fsw;
    pk[9] = fsh;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_present, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, pk, sizeof pk);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_present, 0);
    }
    vp.TopLeftX = (float)fit[0]; vp.TopLeftY = (float)fit[1];
    vp.Width = (float)fit[2]; vp.Height = (float)fit[3];
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    rc.left = fit[0]; rc.top = fit[1];
    rc.right = fit[0] + fit[2]; rc.bottom = fit[1] + fit[3];
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_bb_rtv, zero);   /* the bars */
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_bb_rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx,
        blend_state(0, 0, 0, 0, D3D11_COLOR_WRITE_ENABLE_ALL), zero, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &rc);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_pvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_pps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 2, 1, &s_cb_present);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    GPROF_MARK(GP_SWAP, 0);
    {
        double t0 = g_nv2a_gprof_on ? now_ms() : 0.0;
        int gated = 0;
#ifdef NV2A_GPU_TEST_HOOKS
        gated = s_test_gate >= 0;
#endif
        /* Frame skip 0/1 on a flip-model chain (auto without pace, the limit
         * off): wait for room as the bitblt chain's Present did. pace() took
         * the waitable already when it showed this frame. */
        if (s_flip_model && !s_have_token && !gated && s_waitable)
            WaitForSingleObject(s_waitable, 100);
        IDXGISwapChain_Present(s_chain, 0, 0);
#ifdef NV2A_GPU_TEST_HOOKS
        if (s_test_present_delay_ms > 0)    /* a Present that blocks, as RTSS's or a */
            Sleep((DWORD)s_test_present_delay_ms);   /* driver limiter's does; WARP never */
#endif
        s_have_token = 0;
        GPROF_HOST(GPH_PRESENT_MS, now_ms() - t0);
    }
    if (s_present_ev) {                 /* auto only (present_setup) */
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_present_ev);
        ID3D11DeviceContext_Flush(s_ctx);
        s_present_ev_issued = 1;
    }
    xbox_FramebufferWindowShown();
    return 1;
}

static int gpu_present(uint32_t va, uint32_t pitch, uint32_t aa_sy)
{
    GpuRt *r = NULL;
    ID3D11ShaderResourceView *src;
    HWND hwnd;
    uint32_t sx, sy;
    int i;

    if (s_compare || !s_present_on || s_present_failed)
        return 0;
    fs_observe();                       /* not in a SKIP window: pace() replays first */
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used && s_rt[i].va == va && s_rt[i].pitch == pitch) {
            r = &s_rt[i];
            break;
        }
    if (!r) {                           /* not drawn here: the guest RAM path */
        hwnd = (HWND)xbox_FramebufferWindowHandle();
        /* auto: the window is the flip-model chain's from the first frame on,
         * a movie before anything was drawn included -- GDI and a flip-model
         * chain do not share a window. */
        if (s_fs_auto && !s_chain && hwnd && !present_setup(hwnd)) {
            fprintf(stderr, "[GPURAST] no swap chain on the window; presenting through"
                            " guest RAM\n");
            present_release();
            s_present_failed = 1;
            return 0;
        }
        if (s_chain && s_flip_model) {
            /* GDI cannot draw into a flip-model window: the frame goes up. */
            if (hwnd && gdi_upload(va, pitch, aa_sy ? aa_sy : 1)) {
                xbox_FramebufferWindowExternal(1);
                xbox_FramebufferWindowPresent(va, pitch);   /* the window's own copy (dumps) */
                if (present_frame(hwnd, s_gdi_srv, (float)PRESENT_W, (float)PRESENT_H)) {
                    bind_pipeline();
                    s_st.presents++;
                    return 1;
                }
            }
            return 0;
        }
        if (s_chain)
            xbox_FramebufferWindowExternal(0);
        return 0;
    }
    hwnd = (HWND)xbox_FramebufferWindowHandle();
    if (!hwnd) {                        /* headless: nothing to show to */
        if (getenv("RECOMP_FB_WINDOW"))
            return 0;
        if (rt_check(r)) {
            sx = pitch / (PRESENT_W * r->bpp);
            present_dump(r, sx ? sx : 1, aa_sy ? aa_sy : 1);
        }
        return 1;
    }
    if (!s_chain && !present_setup(hwnd)) {
        fprintf(stderr, "[GPURAST] no swap chain on the window; presenting through"
                        " guest RAM\n");
        present_release();
        s_present_failed = 1;
        return 0;
    }
    if (!rt_check(r))                   /* clean: catch up with CPU writes first */
        return 0;
    xbox_FramebufferWindowExternal(1);
    sx = pitch / (PRESENT_W * r->bpp) ? pitch / (PRESENT_W * r->bpp) : 1;
    sy = aa_sy ? aa_sy : 1;
    rt_resolve(r);
    src = r->srv;
    if (s_ssaa > 1) {                   /* before the presenter takes over the pipeline */
        ssaa_reduce(r, PRESENT_W * sx * s_ox, PRESENT_H * sy * s_oy);
        src = s_ss_srv;
    }
    if (g_nv2a_gprof_on)
        nv2a_gprof_target(r->va, r->W, r->H, r->swizzled);
    GPROF_MARK(GP_PRESENT, r->va);
    if (!present_frame(hwnd, src, (float)(PRESENT_W * sx * s_ox), (float)(PRESENT_H * sy * s_oy)))
        return 0;
    present_dump(r, sx, sy);
    bind_pipeline();
    s_st.presents++;
    return 1;
}

/* auto: can the chain take a frame now without anyone waiting? The GPU has
 * finished the last shown frame (its event) and the waitable is signalled --
 * taken here, so it is kept (s_have_token) until a Present uses it. why: 1
 * the presenter, 2 the GPU. */
static int present_ready(int *why)
{
    int busy = 0;
#ifdef NV2A_GPU_TEST_HOOKS
    busy = s_test_gpu_busy;
#endif
    if (s_chain && !s_flip_model) {     /* the bitblt chain: auto is off */
        static int said;
        if (!said++)
            fprintf(stderr, "[GPURAST] frame skip auto without a flip-model chain: shown by"
                            " the cadence alone\n");
        return 1;
    }
    /* The presenter first: a waitable taken while the GPU is still busy is
     * kept for the next try, not lost. A test's gate stands in for the
     * waitable only. */
#ifdef NV2A_GPU_TEST_HOOKS
    if (s_test_gate == 0) {
        *why = 1;
        return 0;
    }
#endif
    if (s_present_hz > 0.0) {
        /* A limiter at hz, acted out: time cut into 1/hz slots of the host's
         * clock, one show a slot, the slot counted when pace() decides (not
         * when Present returns, later: that lost a slot in four at 45). */
        s_hz_slot_now = floor(now_ms() * s_present_hz / 1000.0);
        if (s_hz_slot_shown >= 0.0 && s_hz_slot_now <= s_hz_slot_shown) {
            *why = 1;
            return 0;
        }
    }
#ifdef NV2A_GPU_TEST_HOOKS
    if (s_test_gate < 0)
#endif
    {
        if (s_waitable && !s_have_token) {
            if (WaitForSingleObject(s_waitable, 0) != WAIT_OBJECT_0) {
                *why = 1;
                return 0;
            }
            s_have_token = 1;
        }
    }
    if (busy || (s_present_ev && s_present_ev_issued
                 && ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_present_ev,
                                                NULL, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)
                    != S_OK)) {
        *why = 2;
        return 0;
    }
    return 1;
}

/* The executor's frame skip asks at each flip whether it is shown; see
 * "frame skip" above. Shown: the cadence wants it and (auto) the chain can
 * take it now. A window that was logged is drawn first. The forecast for the
 * next window: the cadence, and with auto the presenter as a rate R (shows a
 * tick over the last 64 flips the cadence wanted shown: 0.75 under a limit
 * of 45, 1 with none; an average that leans on recent flips swings with 1-1-2) and a credit in its phase -- R a tick accrues, a show spends
 * one, and a flip it holds back says the credit is just short of one. The next
 * window is expected shown when the credit by its flip reaches one. An EMA of
 * the interval between shows forecast this before: at 45 the intervals go
 * 1-1-2, the EMA swung between them, and a window that was then shown was
 * often logged and drawn at the flip ("shown a skipped window", 150 in 10 s),
 * which in turn held the next show back on the GPU. */
#define FS_CREDIT_EDGE 0.05
static int gpu_pace(uint32_t va, uint64_t tick, int cad, int cad_next)
{
    int ready = 1, why = 0, show, next;
    uint64_t gap;

    if (!s_fs.on) {
        const char *e = getenv("RECOMP_GPU_RASTER_PRESENT_HZ");
        s_fs.on = 1;
        s_fs.first_pace = 1;
        s_fs.mode = s_fs.next_mode = FS_SHOW;
        s_fs.R = 1.0;
        s_fs.credit = 1.0;
        s_present_hz = e ? atof(e) : 0.0;
        fprintf(stderr, "[GPURAST] frame skip on (%s)%s\n",
                s_fs_auto ? "auto: shown when the chain can take a frame" : "the cadence",
                s_present_hz > 0.0 ? ", a presenter limited by RECOMP_GPU_RASTER_PRESENT_HZ" : "");
    }
    /* P: what this flip showed, and what the one before showed -- the same
     * surface for a single-buffered title (Fallout), the two buffers in turn
     * for a double-buffered one. Not the last two different ones: a surface
     * shown once (Carbon's loading screen) would stay P for good, and the
     * filter that overwrites it every frame became a second boundary. */
    s_fs.pva[1] = s_fs.pva[0];
    s_fs.pva[0] = va;
    s_fs.armed = 1;
    gap = s_fs.k_prev && tick > s_fs.k_prev ? tick - s_fs.k_prev : 1;
    if (cad && s_fs_auto) {
        ready = present_ready(&why);
        s_fs.credit += (double)gap * s_fs.R;
        if (s_fs.credit > 1.0 + s_fs.R)
            s_fs.credit = 1.0 + s_fs.R;   /* a limiter banks no shows */
    }
    show = cad && ready;
    if (show) {
        if (s_present_hz > 0.0 && s_fs_auto)
            s_hz_slot_shown = s_hz_slot_now;
        if (s_fs.mode == FS_SKIP) {     /* forecast wrong: draw what was logged */
            s_fst.mispredicted_skip++;
            fs_replay(FSR_READY);
        }
        fs_read(FS_RT, va, va + 1);     /* shown: a read */
        if (s_fs_auto)                  /* one show spent; sooner than owed: its phase */
            s_fs.credit = s_fs.credit >= 1.0 ? s_fs.credit - 1.0 : 0.0;
        s_fs.k_shown = tick;
        s_fst.shown++;
    } else {
        if (s_fs.mode == FS_SHOW && !s_fs.first_pace)
            s_fst.mispredicted_show++;  /* drawn for nothing */
        s_fst.skipped++;
        if (cad && !ready) {
            /* not owed a show yet after all: just short of one -- below the
             * forecast's threshold by an edge more, or the flip a whole limiter
             * cycle later (45: three shows on) lands exactly on the threshold
             * and is forecast shown (tests/nv2a_gpu_backend s26c, 60 -> 45) */
            if (s_fs.credit > 1.0 - 2.0 * FS_CREDIT_EDGE)
                s_fs.credit = 1.0 - 2.0 * FS_CREDIT_EDGE;
            if (why == 1)
                s_fst.nr_presenter++;
            else
                s_fst.nr_gpu++;
        }
    }
    if (cad && s_fs_auto) {             /* the rate: shows over ticks, the last FS_RATE_N flips */
        int i;
        unsigned sh = 0, tk = 0;
        s_fs.r_shown[s_fs.r_at] = (uint8_t)show;
        s_fs.r_gap[s_fs.r_at] = (uint8_t)(gap > 255 ? 255 : gap);
        s_fs.r_at = (s_fs.r_at + 1) % FS_RATE_N;
        if (s_fs.r_n < FS_RATE_N)
            s_fs.r_n++;
        for (i = 0; i < s_fs.r_n; i++) {
            sh += s_fs.r_shown[i];
            tk += s_fs.r_gap[i];
        }
        s_fs.R = tk ? (double)sh / (double)tk : 1.0;
    }
    s_fs.first_pace = 0;
    next = cad_next;
    if (next && s_fs_auto)              /* owed a show by the next flip, as far away as this */
        next = s_fs.credit + (double)gap * s_fs.R >= 1.0 - FS_CREDIT_EDGE;   /* a rate a
                                                    hair under the limiter's still in phase */
    s_fs.k_prev = tick;
    s_fs.next_mode = next ? FS_SHOW : FS_SKIP;    return show;
}

#ifdef NV2A_GPU_TEST_HOOKS
/* tests/nv2a_gpu_backend: every target's size at the internal scale (the
 * profile's whole-buffer clears and resolves cover it). */
void nv2a_gpu_test_target_dims(uint32_t *w, uint32_t *h)
{
    *w = RT_W * s_sx;
    *h = s_gh * s_sy;
}

/* Frame skip's counters, cumulative: shown, skipped, not ready (presenter),
 * not ready (gpu), logged ops, windows dropped, replayed ready, observed, not
 * self-contained, arena full, mispredicted (a skipped window shown), (a
 * hidden one drawn), stale reads, and the window mode now (1 SKIP), the
 * forecast interval I x 100, the journal's ops now. */
void nv2a_gpu_test_skip_stats(uint32_t out[16])
{
    out[0] = s_fst.shown; out[1] = s_fst.skipped;
    out[2] = s_fst.nr_presenter; out[3] = s_fst.nr_gpu;
    out[4] = s_fst.logged; out[5] = s_fst.dropped_windows;
    out[6] = s_fst.replayed[0]; out[7] = s_fst.replayed[1];
    out[8] = s_fst.replayed[2]; out[9] = s_fst.replayed[3];
    out[10] = s_fst.mispredicted_skip; out[11] = s_fst.mispredicted_show;
    out[12] = s_fst.stale; out[13] = (uint32_t)s_fs.mode;
    out[14] = (uint32_t)(100.0 / (s_fs.R > 0.01 ? s_fs.R : 0.01) + 0.5); out[15] = s_fs.ops;
}

/* The presenter's half of readiness: -1 the real one (the waitable, or
 * RECOMP_GPU_RASTER_PRESENT_HZ), 0 never ready (an outside limiter holding
 * every buffer), 1 always. */
void nv2a_gpu_test_present_gate(int g) { s_test_gate = g; }

/* The GPU's half: 1 the last shown frame's event never completes. */
void nv2a_gpu_test_gpu_busy(int busy) { s_test_gpu_busy = busy; }

/* Every Present then takes ms longer (0: as it is), as one that blocks in an
 * outside limiter does -- WARP's never does. */
void nv2a_gpu_test_present_delay(int ms) { s_test_present_delay_ms = ms; }

/* Whether the chain is the flip-model one; how many frames were Presented;
 * whether pace() holds a waitable it took and nothing Presented on yet;
 * stale textures so far; whether the show event exists. */
int nv2a_gpu_test_flip_model(void) { return s_flip_model; }
uint32_t nv2a_gpu_test_presents(void) { return s_st.presents; }
int nv2a_gpu_test_have_token(void) { return s_have_token; }
uint32_t nv2a_gpu_test_tex_changed(void) { return s_fst.tex_changed; }
int nv2a_gpu_test_present_event(void) { return s_present_ev != NULL; }

/* Texel (x,y) of the last guest RAM frame the flip-model chain uploaded
 * (gdi_upload). 0: none. */
int nv2a_gpu_test_gdi_pixel(uint32_t x, uint32_t y, uint32_t *argb)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box;
    if (!s_gdi_tex || x >= PRESENT_W || y >= PRESENT_H)
        return 0;
    box.left = x; box.right = x + 1; box.top = y; box.bottom = y + 1;
    box.front = 0; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_staging, 0, x, y, 0,
                                              (ID3D11Resource *)s_gdi_tex, 0, &box);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return 0;
    *argb = ((const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch))[x];
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
    return 1;
}

/* Native pixel (x,y) of the target at va as the GPU holds it now (each
 * s_sx x s_sy block averaged). 0: no such target. */
int nv2a_gpu_test_rt_pixel(uint32_t va, uint32_t x, uint32_t y, uint32_t *argb)
{
    D3D11_MAPPED_SUBRESOURCE m;
    int i;
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used && s_rt[i].va == va) {
            rt_resolve(&s_rt[i]);
            rt_read_native(&s_rt[i], x, y, x + 1, y + 1);
            if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_staging, 0,
                                               D3D11_MAP_READ, 0, &m)))
                return 0;
            *argb = ((const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch))[x];
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
            return 1;
        }
    return 0;
}

/* Every draw and clear through this pixel shader (compiled DXBC, the test's)
 * instead of the back end's own; NULL goes back. 0: not a pixel shader. The
 * last one created is kept: the same code again creates nothing. */
int nv2a_gpu_test_ps_override(const void *code, size_t len)
{
    if (!code) {
        s_ps_override = NULL;
        return 1;
    }
    if (s_ps_ovr_cache && code == s_ovr_code && len == s_ovr_len) {
        s_ps_override = s_ps_ovr_cache;
        return 1;
    }
    s_ps_override = NULL;
    REL(s_ps_ovr_cache);
    s_ovr_code = NULL;
    if (FAILED(ID3D11Device_CreatePixelShader(s_dev, code, len, NULL, &s_ps_ovr_cache)))
        return 0;
    s_ovr_code = code;
    s_ovr_len = len;
    s_ps_override = s_ps_ovr_cache;
    return 1;
}

/* The back end's own pixel shaders, as compiled: variant 0..N-1, NULL past the last. */
const void *nv2a_gpu_test_ps_blob(int variant, size_t *len, const char **name)
{
    if (variant < 0 || variant >= s_nps_blobs)
        return NULL;
    *len = s_ps_len[variant];
    if (name)
        *name = s_ps_names[variant];
    return s_ps_code[variant];
}

/* Combiner draws by a shader of their own and by the general one since
 * install, and the distinct states met without one. */
void nv2a_gpu_test_rc_counts(uint64_t *own, uint64_t *generic, uint32_t *states_missed)
{
    *own = s_rc_own_all;
    *generic = s_rc_gen_all;
    *states_missed = s_rc_nmiss;
}

/* The key of the last combiner draw pick_ps looked up (own shaders or
 * RECOMP_GPU_RASTER_RC_STATS on). */
void nv2a_gpu_test_rc_last_key(uint32_t w[NV2A_RC_KEY_WORDS])
{
    memcpy(w, s_rc_lastkey, sizeof s_rc_lastkey);
}

/* RECOMP_GPU_RASTER_RC_GENERIC from a test. */
void nv2a_gpu_test_rc_generic(int on)
{
    s_rc_generic = on != 0;
}

/* The state table's slot from the first 5 key words only, every state moved to
 * its slot under that (its shader and counters with it): keys that differ later
 * than word 4 now start in the same slot, which the real hash never does on
 * purpose -- so the compare of a whole key in rc_slot is what tells them apart. */
void nv2a_gpu_test_rc_weak_hash(int on)
{
    static RcState old[RC_SLOTS];
    int i, added;

    memcpy(old, s_rcs, sizeof old);
    memset(s_rcs, 0, sizeof s_rcs);
    s_rc_weak_hash = on != 0;
    s_rc_last = -1;
    for (i = 0; i < RC_SLOTS; i++)
        if (old[i].used) {
            int slot = rc_slot(old[i].w, &added);
            if (slot >= 0) {
                s_rcs[slot].ps = old[i].ps;
                s_rcs[slot].draws = old[i].draws;
                s_rcs[slot].px = old[i].px;
            }
        }
}

/* States in the table with a shader of their own. */
int nv2a_gpu_test_rc_own_slots(void)
{
    int i, n = 0;

    for (i = 0; i < RC_SLOTS; i++)
        n += s_rcs[i].used && s_rcs[i].ps;
    return n;
}

/* How often pick_ps chose each shader: screen, ffp, combiners 0, 1, 2. */
void nv2a_gpu_test_ps_counts(int counts[5])
{
    memcpy(counts, s_ps_picked, sizeof s_ps_picked);
}

/* RECOMP_GPU_RASTER_PSCHECK from a test: set up from a file as the variable
 * does (path NULL: off), or from ps_main the test compiled once; and read. */
int nv2a_gpu_test_pscheck_setup(const char *path, uint32_t every)
{
    return pscheck_setup(path, every);
}

int nv2a_gpu_test_pscheck_setup_code(const void *code, size_t len, uint32_t every)
{
    return pscheck_setup_code(code, len, "(the test's)", every);
}

void nv2a_gpu_test_pscheck(uint64_t *checked, uint64_t *differ8, uint64_t *differf)
{
    *checked = s_pc_total.draws;
    *differ8 = s_pc_total.differ8;
    *differf = s_pc_total.differf;
}

/* tests/nv2a_gpu_backend: what the presenter shows for the W x H surface at
 * va under SSAA -- (W*s_ox) x (H*s_oy) texels into out. 0 without SSAA or
 * without that target. */
int nv2a_gpu_test_ssaa_frame(uint32_t va, uint32_t pitch, uint32_t W, uint32_t H,
                             uint32_t *out, uint32_t *ow, uint32_t *oh)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box;
    GpuRt *r = NULL;
    uint32_t y;
    int i;

    if (s_ssaa < 2)
        return 0;
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used && s_rt[i].va == va && s_rt[i].pitch == pitch) {
            r = &s_rt[i];
            break;
        }
    if (!r)
        return 0;
    *ow = W * s_ox;
    *oh = H * s_oy;
    if (*ow > RT_W || *oh > RT_H)       /* s_staging's size */
        return 0;
    ssaa_reduce(r, *ow, *oh);
    box.left = 0; box.top = 0; box.front = 0;
    box.right = *ow; box.bottom = *oh; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_staging, 0,
                                              0, 0, 0, (ID3D11Resource *)s_ss, 0, &box);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return 0;
    for (y = 0; y < *oh; y++)
        memcpy(out + (size_t)y * *ow, (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
               (size_t)*ow * 4);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_staging, 0);
    return 1;
}
#endif

static int create_pipeline(void)
{
    static const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "FOG",      0, DXGI_FORMAT_R32_FLOAT,          0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    D3D11_SUBRESOURCE_DATA sd;
    static const uint32_t white = 0xFFFFFFFFu;
    int ok = 0;

    /* Compiled at build time (nv2a_gpu_shaders.cmake): D3DCompile took ~12 s
     * here for the combiner shader alone. */
    if (!make_ps(k_ps_screen, sizeof k_ps_screen, "ps_screen", &s_ps_screen)
        || !make_ps(k_ps_ffp, sizeof k_ps_ffp, "ps_ffp", &s_ps_ffp)
        || !make_ps(k_ps_rc, sizeof k_ps_rc, "ps_rc", &s_ps_rc[0])
        || !make_ps(k_ps_rc_tex, sizeof k_ps_rc_tex, "ps_rc_tex", &s_ps_rc[1])
        || !make_ps(k_ps_rc_rt, sizeof k_ps_rc_rt, "ps_rc_rt", &s_ps_rc[2]))
        goto done;
#if NV2A_GPU_RC_KEYS > 0
    {
        /* The states of the game's key file, each with its own ps_rc. One that
         * does not create is left to the general shader. */
        int ki;
        for (ki = 0; ki < NV2A_GPU_RC_KEYS; ki++) {
            ID3D11PixelShader *kps = NULL;
            int added, slot;
            if (!make_ps(k_rc_key_code[ki], k_rc_key_len[ki], k_rc_key_name[ki], &kps)) {
                fprintf(stderr, "[GPURAST] %s did not create; its combiner state uses the"
                                " general shader\n", k_rc_key_name[ki]);
                continue;
            }
            slot = rc_slot(k_rc_key[ki], &added);
            if (slot < 0 || !added) {
                REL(kps);
                continue;
            }
            s_rcs[slot].ps = kps;
            s_rc_nown++;
        }
    }
#endif
    if (FAILED(ID3D11Device_CreateVertexShader(s_dev, k_vs_main, sizeof k_vs_main, NULL, &s_vs))
        || FAILED(ID3D11Device_CreateInputLayout(s_dev, il, (UINT)(sizeof il / sizeof il[0]),
                                                 k_vs_main, sizeof k_vs_main, &s_il)))
        goto done;
    {
        /* RECOMP_GPU_RASTER_PSCHECK=<ref.hlsl>[,<every>] */
        const char *e = getenv("RECOMP_GPU_RASTER_PSCHECK");
        if (e && *e) {
            char path[1024];
            const char *comma = strrchr(e, ',');
            uint32_t every = 30;
            size_t n = strlen(e);
            if (comma && comma[1] >= '0' && comma[1] <= '9') {
                every = (uint32_t)strtoul(comma + 1, NULL, 10);
                n = (size_t)(comma - e);
            }
            if (n >= sizeof path)
                n = sizeof path - 1;
            memcpy(path, e, n);
            path[n] = 0;
            pscheck_setup(path, every ? every : 1);
        }
    }
    if (nv2a_gprof_flatps_requested()) {
        /* The profile's ablation: a flat pixel shader for transformed draws,
         * to see what the ubershader costs. Diagnostics only. */
        static const char flat[] =
            "\nfloat4 ps_flat(PSIn i) : SV_Target { return saturate(i.d0); }\n";
        size_t n = sizeof s_hlsl - 1;
        char *src = (char *)malloc(n + sizeof flat);
        ID3DBlob *fb = NULL;
        if (src) {
            memcpy(src, s_hlsl, n);
            memcpy(src + n, flat, sizeof flat);
            fb = compile_src(src, n + sizeof flat - 1, "ps_flat", "ps_4_0");
            free(src);
        }
        if (fb) {
            if (FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(fb),
                                                      ID3D10Blob_GetBufferSize(fb), NULL,
                                                      &s_ps_flat)))
                s_ps_flat = NULL;
            REL(fb);
        }
        if (!s_ps_flat)
            fprintf(stderr, "[GPUPROF] flatps: the flat shader could not be made\n");
    }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb)))
        goto done;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = (UINT)((sizeof(VsConsts) + 15) & ~15u);
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_vs)))
        goto done;
    bd.ByteWidth = (UINT)((sizeof(PsConsts) + 15) & ~15u);
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_ps)))
        goto done;

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rast)))
        goto done;

    memset(&td, 0, sizeof td);
    td.Width = RT_W; td.Height = RT_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_staging)))
        goto done;

    if (s_scaled) {
        D3D11_RENDER_TARGET_VIEW_DESC rv;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        memset(&vd, 0, sizeof vd);
        vd.Format = td.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.ArraySize = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_up))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_up,
                                                            &vd, &s_up_srv)))
            goto done;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        memset(&rv, 0, sizeof rv);
        rv.Format = td.Format;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_down))
            || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_down,
                                                          &rv, &s_down_rtv)))
            goto done;
        if (FAILED(ID3D11Device_CreateVertexShader(s_dev, k_bvs, sizeof k_bvs, NULL, &s_bvs))
            || !make_ps(k_bps_up, sizeof k_bps_up, "bps_up", &s_bps_up)
            || !make_ps(k_bps_down, sizeof k_bps_down, "bps_down", &s_bps_down))
            goto done;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = 16;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_blit)))
            goto done;
        if (s_ssaa > 1) {               /* the frame averaged down, as presented */
            td.Width = RT_W * s_ox; td.Height = RT_H * s_oy;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ss))
                || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_ss,
                                                                &vd, &s_ss_srv))
                || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_ss,
                                                              &rv, &s_ss_rtv)))
                goto done;
        }
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;   /* as the white texture below wants */
    }


    td.Width = td.Height = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.CPUAccessFlags = 0;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = &white;
    sd.SysMemPitch = 4;
    sd.SysMemSlicePitch = 4;
    memset(&vd, 0, sizeof vd);
    vd.Format = td.Format;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    vd.Texture2DArray.MipLevels = 1;
    vd.Texture2DArray.ArraySize = 1;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &s_white))
        || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_white,
                                                        &vd, &s_white_srv)))
        goto done;

    bind_pipeline();
    ok = 1;
done:
    return ok;
}

int nv2a_gpu_d3d11_install(void)
{
    static const D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    const char *e = getenv("RECOMP_GPU_RASTER");
    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
    D3D_DRIVER_TYPE type;
    DXGI_ADAPTER_DESC ad;
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *xa = NULL;
    LARGE_INTEGER f;
    HRESULT hr;

    if (!e || !*e || strcmp(e, "0") == 0)
        return 0;
    s_compare = strcmp(e, "compare") == 0;
    {
        const char *rt = getenv("RECOMP_GPU_RASTER_RTTEX");
        const char *sy = getenv("RECOMP_GPU_RASTER_SYNC");
        s_rt_tex = !(rt && strcmp(rt, "0") == 0);
        s_sync_all = sy && strcmp(sy, "all") == 0;
        s_present_on = !(getenv("RECOMP_GPU_RASTER_PRESENT")
                         && strcmp(getenv("RECOMP_GPU_RASTER_PRESENT"), "0") == 0);
    }
    {
        const char *la = getenv("RECOMP_GPU_RASTER_LOSE_AT");
        s_lose_at = la ? (uint32_t)strtoul(la, NULL, 10) : 0;
    }
    type = strcmp(e, "warp") == 0 ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    QueryPerformanceFrequency(&f);
    s_qpc_ms = 1000.0 / (double)f.QuadPart;

    hr = D3D11CreateDevice(NULL, type, NULL, 0, fls, (UINT)(sizeof fls / sizeof fls[0]),
                           D3D11_SDK_VERSION, &s_dev, &fl, &s_ctx);
    if (FAILED(hr) && type == D3D_DRIVER_TYPE_HARDWARE) {
        fprintf(stderr, "[GPURAST] no hardware D3D11 device (0x%08lX); trying WARP\n",
                (unsigned long)hr);
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, fls,
                               (UINT)(sizeof fls / sizeof fls[0]), D3D11_SDK_VERSION,
                               &s_dev, &fl, &s_ctx);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[GPURAST] D3D11CreateDevice failed (0x%08lX); software"
                        " rasteriser stays\n", (unsigned long)hr);
        release_all();
        return 0;
    }
    memset(&ad, 0, sizeof ad);
    if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&xd))
        && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &xa)))
        IDXGIAdapter_GetDesc(xa, &ad);
    REL(xa);
    REL(xd);
    if (ad.VendorId == 0x1414)
        fprintf(stderr, "[GPURAST] warning: %ls is a Microsoft software adapter;"
                        " expect it to be slow\n", ad.Description);
    {
        /* RECOMP_GPU_RASTER_SCALE=<n> | <x>x<y> (1-8), RECOMP_GPU_RASTER_SSAA=<n> (1-4),
         * RECOMP_GPU_RASTER_MSAA=<samples>, lowered to what the device has for both
         * formats. */
        const char *sc = getenv("RECOMP_GPU_RASTER_SCALE"), *ms = getenv("RECOMP_GPU_RASTER_MSAA");
        const char *sa = getenv("RECOMP_GPU_RASTER_SSAA");
        unsigned ss = sa ? (unsigned)atoi(sa) : 1;
        unsigned a = 1, b = 1, n = 1;
        UINT q1 = 0, q2 = 0;
        if (sc && sscanf(sc, "%ux%u", &a, &b) != 2) {
            if (sscanf(sc, "%u", &a) == 1)
                b = a;
            else
                a = b = 1;
        }
        if (ms)
            n = (unsigned)atoi(ms);
        s_sx = a < 1 ? 1 : a > 8 ? 8 : a;
        s_sy = b < 1 ? 1 : b > 8 ? 8 : b;
        a = s_sx; b = s_sy;
        ss = ss < 1 ? 1 : ss > 4 ? 4 : ss;
        if (a * ss > MAX_SIDE || b * ss > MAX_SIDE) {
            unsigned m = MAX_SIDE / (a > b ? a : b);
            fprintf(stderr, "[GPURAST] SSAA %ux lowered to %ux: scale x SSAA is at most %u a side\n",
                    ss, m, MAX_SIDE);
            ss = m;
        }
        s_ox = a; s_oy = b; s_ssaa = ss;
        s_sx = a * ss; s_sy = b * ss;
        s_msaa = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
        while (s_msaa > 1
               && (FAILED(ID3D11Device_CheckMultisampleQualityLevels(s_dev,
                              DXGI_FORMAT_B8G8R8A8_UNORM, s_msaa, &q1)) || !q1
                   || FAILED(ID3D11Device_CheckMultisampleQualityLevels(s_dev,
                              DXGI_FORMAT_D32_FLOAT_S8X24_UINT, s_msaa, &q2)) || !q2))
            s_msaa /= 2;
        if (s_compare) {                /* compares with the software path pixel for pixel */
            s_sx = s_sy = s_msaa = 1;
            s_ox = s_oy = s_ssaa = 1;
        }
        s_scaled = s_sx * s_sy * s_msaa > 1;
        s_gh = s_scaled ? 512 : RT_H;
        if (s_scaled)
            fprintf(stderr, "[GPURAST] internal resolution %ux%u per surface pixel,"
                            " %ux MSAA\n", s_sx, s_sy, s_msaa);
        if (s_ssaa > 1)
            fprintf(stderr, "[GPURAST] SSAA %ux: each %ux%u block averaged, shown at"
                            " %ux%u per surface pixel\n", s_ssaa, s_ssaa, s_ssaa, s_ox, s_oy);
    }
    {
        LARGE_INTEGER c0, c1;
        int made;
        QueryPerformanceCounter(&c0);
        made = create_pipeline();
        QueryPerformanceCounter(&c1);
        fprintf(stderr, "[GPURAST] pipeline made in %.2f s (shaders compiled at build time)\n",
                (double)(c1.QuadPart - c0.QuadPart) * s_qpc_ms / 1000.0);
        if (!made) {
            fprintf(stderr, "[GPURAST] pipeline setup failed; software rasteriser stays\n");
            release_all();
            return 0;
        }
    }
    nv2a_gprof_init((struct ID3D11Device *)s_dev, (struct ID3D11DeviceContext *)s_ctx,
                    s_sx, s_sy, s_msaa);

    s_be.clear = gpu_clear;
    s_be.draw = gpu_draw;
    s_be.flip = gpu_flip;
    s_be.sync = gpu_sync;
    s_be.report = gpu_report;
    s_be.shadow = s_compare;
    s_be.sync_surface = s_sync_all ? NULL : gpu_sync_surface;
    s_be.present = gpu_present;
    /* Frame skip needs the frame's work to be the GPU's alone: compare draws
     * every frame in software too, SYNC=all writes every one back. */
    s_be.pace = s_compare || s_sync_all ? NULL : gpu_pace;
    {
        const char *fsk = getenv("RECOMP_FRAME_SKIP");
        const char *lim = getenv("RECOMP_FPS_LIMIT");
        /* As the executor's frame_skip_active: auto and a limit (L > 0); without
         * one the flip model, its waitable and the event stay off too. */
        s_fs_auto = s_be.pace && fsk && strcmp(fsk, "auto") == 0 && (lim ? atoi(lim) : 60) > 0;
    }
    {
        const char *rcs = getenv("RECOMP_GPU_RASTER_RC_STATS");
        const char *gn = getenv("RECOMP_GPU_RASTER_RC_GENERIC");
        s_rc_stats = rcs && *rcs && strcmp(rcs, "0") != 0;
        s_rc_generic = gn && *gn && strcmp(gn, "0") != 0;
        if (s_rc_generic)
            fprintf(stderr, "[GPURAST] RC_GENERIC: combiners drawn by the general shader only\n");
        fprintf(stderr, "[GPURAST] combiner shaders: %u states built in\n", s_rc_nown);
    }
    nv2a_backend_register(&s_be);
    fprintf(stderr, "[GPURAST] D3D11 back end on: %ls, FL %u_%u%s\n", ad.Description,
            ((unsigned)fl >> 12) & 0xF, ((unsigned)fl >> 8) & 0xF,
            s_compare ? " (compare: the software path draws, the GPU checks)" : "");
    return 1;
}
