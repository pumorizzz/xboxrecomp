/*
 * NV2A rasterisation on the host GPU, through Direct3D 11. See
 * nv2a_gpu_d3d11.h for what it is and how it is switched on.
 *
 * Everything below runs on the NV2A poll thread, one call at a time, except
 * nv2a_gpu_d3d11_install(), which creates the device on the main thread
 * before the title starts. There is no window and no swap chain: colour
 * surfaces live in offscreen render targets that mirror guest RAM.
 *
 * Colour surfaces. One 1280x1024 BGRA8 render target per (guest address,
 * pitch, bytes per pixel), with a CPU copy (the shadow) of what guest RAM
 * held when the target last matched it. The first time a target is used in
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
 * Pixels. One ubershader: the screen-space path (flat colour, the texel
 * replaces it), the fixed-function stand-in (diffuse times stage 0), and
 * the register combiners, ported line by line from nv2a_rc_eval /
 * rc_stage_fetch / rc_texel. Constants are the raw combiner words.
 *
 * ponytail: one render-target size (1280x1024, the executor's depth-buffer
 * size); bigger surfaces are clamped. Level 0 only, no mip selection, as the
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
#include <d3dcompiler.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/nv2a_backend.h"
#include "d3d8_swizzle.h"
#include "nv2a_gpu_d3d11.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void *xbox_FramebufferWindowHandle(void);       /* src/video/fb_present.c */
extern void xbox_FramebufferWindowExternal(int on);
extern void xbox_FramebufferWindowFit(uint32_t cw, uint32_t ch, int32_t out[4]);

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

static const char s_hlsl[] =
"cbuffer VSC : register(b0) { float4 vsc; };\n"
"cbuffer PSC : register(b1) {\n"
"    uint4 cicw[2]; uint4 aicw[2]; uint4 cocw[2]; uint4 aocw[2];\n"
"    uint4 fac0[2]; uint4 fac1[2];\n"
"    uint4 fin;\n"            /* final0, final1, final_c0, final_c1 */
"    uint4 ctl;\n"            /* control, stage_program, clip_plane_mode, flags */
"    uint4 at;\n"             /* alpha_func, alpha_ref, stage_valid, kind */
"    float4 fogc;\n"
"    uint4 tex[4];\n"         /* w | h<<16, addr_u | addr_v<<8, flags, origin x | y<<16 */
"    float4 zr;\n"           /* SET_CLIP_MIN, SET_CLIP_MAX (window z) */
"    float4 rsc;\n"          /* the internal scale of render targets: x, y */
"};\n"
"Texture2DArray<float4> T0 : register(t0);\n"
"Texture2DArray<float4> T1 : register(t1);\n"
"Texture2DArray<float4> T2 : register(t2);\n"
"Texture2DArray<float4> T3 : register(t3);\n"
"#define W8(a, i) a[(i) >> 2][(i) & 3]\n"
"struct VSIn {\n"
"    float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
"    float fog : FOG;\n"
"    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"    float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
"};\n"
"struct PSIn {\n"
"    float4 pos : SV_Position;\n"
"    noperspective float4 d0 : COLOR0;\n"
"    noperspective float4 d1 : COLOR1;\n"
"    noperspective float fog : FOG;\n"
"    nointerpolation float4 fc : COLOR2;\n"
"    noperspective float zn : ZRANGE;\n"   /* z / zmax, unclamped, screen-linear */
"    noperspective float zw : ZWINDOW;\n"  /* window z as the title produced it */
"    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"    float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
"};\n"
/* Surface pixels to clip space. A transformed vertex keeps its w, so texture
 * coordinates interpolate perspective-correct as on the software path;
 * colours and fog interpolate in screen space, as there. */
"PSIn vs_main(VSIn i) {\n"
"    PSIn o;\n"
"    float w = (vsc.y > 0.5 && i.pos.w > 0.0) ? 1.0 / i.pos.w : 1.0;\n"
"    float3 ndc = float3(i.pos.x / 640.0 - 1.0, 1.0 - i.pos.y / 512.0,\n"
"                        i.pos.z * vsc.x);\n"
"    o.pos = float4(ndc * w, w);\n"
"    o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog; o.fc = i.d0;\n"
"    o.zn = i.pos.z * vsc.x;\n"
"    o.zw = i.pos.z;\n"
"    o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"    return o;\n"
"}\n"
"float4 unpack(uint c) {\n"
"    return float4((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, c >> 24) / 255.0;\n"
"}\n"
"float4 load_st(uint st, int x, int y, uint face) {\n"
"    int4 c = int4(x, y, (int)face, 0);\n"
"    if (st == 0) return T0.Load(c);\n"
"    if (st == 1) return T1.Load(c);\n"
"    if (st == 2) return T2.Load(c);\n"
"    return T3.Load(c);\n"
"}\n"
/* wrap_coord: 1 wraps, everything else clamps. */
"int wrapc(int c, int n, uint mode) {\n"
"    if (n <= 0) return 0;\n"
"    if (mode == 1) { int r = c - (c / n) * n; return r < 0 ? r + n : r; }\n"
"    return clamp(c, 0, n - 1);\n"
"}\n"
/* swizzle_offset (d3d8_swizzle.h): x and y bits interleaved, the larger
 * dimension carrying on alone once the smaller one runs out. */
"uint swz(uint x, uint y, uint w, uint h) {\n"
"    uint o = 0, bit = 1, mb = 1, k;\n"
"    [loop] for (k = 0; k < 16 && (bit < w || bit < h); k++) {\n"
"        if (bit < w) { if (x & bit) o |= mb; mb <<= 1; }\n"
"        if (bit < h) { if (y & bit) o |= mb; mb <<= 1; }\n"
"        bit <<= 1;\n"
"    }\n"
"    return o;\n"
"}\n"
"float4 texel_at(uint st, int x, int y, uint face) {\n"
"    uint4 p = tex[st];\n"
"    int w = (int)(p.x & 0xFFFF), h = (int)(p.x >> 16);\n"
"    int u = wrapc(x, w, p.y & 0xFF), v = wrapc(y, h, (p.y >> 8) & 0xFF);\n"
"    float4 c;\n"
"    if (p.z & 32) {\n"           /* a render target read as a swizzled texture */
"        uint i = swz((uint)u, (uint)v, (uint)w, (uint)h);\n"
"        int2 s = int2(rsc.xy);\n"   /* the centre of the texel at the internal scale */
"        c = load_st(st, (int)(i % p.w) * s.x + s.x / 2, (int)(i / p.w) * s.y + s.y / 2, face);\n"
"    } else {\n"
"        c = load_st(st, u + (int)(p.w & 0xFFFF), v + (int)(p.w >> 16), face);\n"
"    }\n"
"    if (p.z & 16) c.a = 1.0;\n"   /* X8R8G8B8 read from a render target */
"    return c;\n"
"}\n"
/* A render target sampled as a texture (linear view), at the internal scale:
 * (u, v) in the texture's texels, continuous, so a target drawn larger is read
 * at its own resolution. At scale 1 these are exactly the taps below. */
"float4 rt_texel(uint st, float u, float v, uint face, bool bil) {\n"
"    uint4 p = tex[st];\n"
"    int2 s = int2(rsc.xy);\n"
"    int w = (int)(p.x & 0xFFFF) * s.x, h = (int)(p.x >> 16) * s.y;\n"
"    int ox = (int)(p.w & 0xFFFF) * s.x, oy = (int)(p.w >> 16) * s.y;\n"
"    uint mu = p.y & 0xFF, mv = (p.y >> 8) & 0xFF;\n"
"    float4 c;\n"
"    u *= rsc.x; v *= rsc.y;\n"
"    if (bil) {\n"
"        float fu = u - 0.5, fv = v - 0.5, iu = floor(fu), iv = floor(fv);\n"
"        float wu = fu - iu, wv = fv - iv;\n"
"        int x0 = wrapc((int)iu, w, mu) + ox, x1 = wrapc((int)iu + 1, w, mu) + ox;\n"
"        int y0 = wrapc((int)iv, h, mv) + oy, y1 = wrapc((int)iv + 1, h, mv) + oy;\n"
"        float4 c0 = load_st(st, x0, y0, face), c1 = load_st(st, x1, y0, face);\n"
"        float4 c2 = load_st(st, x0, y1, face), c3 = load_st(st, x1, y1, face);\n"
"        c = (c0 * (1.0 - wu) + c1 * wu) * (1.0 - wv) + (c2 * (1.0 - wu) + c3 * wu) * wv;\n"
"    } else {\n"
"        c = load_st(st, wrapc((int)floor(u), w, mu) + ox, wrapc((int)floor(v), h, mv) + oy, face);\n"
"    }\n"
"    if (p.z & 16) c.a = 1.0;\n"
"    return c;\n"
"}\n"
/* rc_texel: normalised (swizzled/DXT) or texel coordinates; an unusable
 * stage reads white; bilinear taps at floor(u - 0.5). */
"float4 rc_texel(uint st, float u, float v, uint face) {\n"
"    uint4 p = tex[st];\n"
"    if (!(p.z & 8)) return float4(1.0, 1.0, 1.0, 1.0);\n"
"    if (p.z & 1) { u *= (float)(p.x & 0xFFFF); v *= (float)(p.x >> 16); }\n"
"    if ((p.z & 64) && !(p.z & 32)) return rt_texel(st, u, v, face, (p.z & 2) != 0);\n"
"    if (p.z & 2) {\n"
"        float fu = u - 0.5, fv = v - 0.5;\n"
"        float iu = floor(fu), iv = floor(fv);\n"
"        float wu = fu - iu, wv = fv - iv;\n"
"        int x = (int)iu, y = (int)iv;\n"
"        float4 c0 = texel_at(st, x, y, face), c1 = texel_at(st, x + 1, y, face);\n"
"        float4 c2 = texel_at(st, x, y + 1, face), c3 = texel_at(st, x + 1, y + 1, face);\n"
"        return (c0 * (1.0 - wu) + c1 * wu) * (1.0 - wv) + (c2 * (1.0 - wu) + c3 * wu) * wv;\n"
"    }\n"
"    return texel_at(st, (int)floor(u), (int)floor(v), face);\n"
"}\n"
/* rc_stage_fetch: false when a clip-plane stage kills the pixel. */
"bool stage_fetch(uint st, float4 c, out float4 o) {\n"
"    uint mode = (ctl.y >> (st * 5)) & 0x1F;\n"
"    float q = c.w != 0.0 ? c.w : 1.0;\n"
"    o = float4(0.0, 0.0, 0.0, 0.0);\n"
"    if (mode == 0) { o = float4(0.0, 0.0, 0.0, 1.0); return true; }\n"
"    if (mode == 1 || mode == 2) { o = rc_texel(st, c.x / q, c.y / q, 0); return true; }\n"
"    if (mode == 3) {\n"
"        float x = c.x, y = c.y, z = c.z, ax = abs(x), ay = abs(y), az = abs(z);\n"
"        float ma, sc, tc; uint face;\n"
"        if (!(tex[st].z & 4)) { o = rc_texel(st, c.x, c.y, 0); return true; }\n"
"        if (ax >= ay && ax >= az) {\n"
"            face = x > 0.0 ? 0 : 1; ma = ax; sc = x > 0.0 ? -z : z; tc = -y;\n"
"        } else if (ay >= az) {\n"
"            face = y > 0.0 ? 2 : 3; ma = ay; sc = x; tc = y > 0.0 ? z : -z;\n"
"        } else {\n"
"            face = z > 0.0 ? 4 : 5; ma = az; sc = z > 0.0 ? x : -x; tc = -y;\n"
"        }\n"
"        if (ma == 0.0) ma = 1.0;\n"
"        o = rc_texel(st, (sc / ma + 1.0) * 0.5, (tc / ma + 1.0) * 0.5, face);\n"
"        return true;\n"
"    }\n"
"    if (mode == 4) { o = saturate(c); return true; }\n"
"    if (mode == 5) {\n"
"        [unroll] for (uint j = 0; j < 4; j++) {\n"
"            uint ge = (ctl.z >> (st * 4 + j)) & 1;\n"
"            if (ge != 0 ? c[j] >= 0.0 : c[j] < 0.0) return false;\n"
"        }\n"
"        return true;\n"
"    }\n"
"    return true;\n"
"}\n"
/* nv2a_rc_eval, line by line. */
"float map_in(float x, uint b) {\n"
"    float p = x < 0.0 ? 0.0 : x;\n"
"    uint m = (b >> 5) & 7;\n"
"    if (m == 0) return p;\n"
"    if (m == 1) return 1.0 - saturate(x);\n"
"    if (m == 2) return 2.0 * p - 1.0;\n"
"    if (m == 3) return -2.0 * p + 1.0;\n"
"    if (m == 4) return p - 0.5;\n"
"    if (m == 5) return -p + 0.5;\n"
"    if (m == 6) return x;\n"
"    return -x;\n"
"}\n"
"float3 map_out(float3 x, uint m) {\n"
"    if (m == 0x08) return x - 0.5;\n"
"    if (m == 0x10) return x * 2.0;\n"
"    if (m == 0x18) return (x - 0.5) * 2.0;\n"
"    if (m == 0x20) return x * 4.0;\n"
"    if (m == 0x30) return x * 0.5;\n"
"    return x;\n"
"}\n"
"float3 read_rgb(float4 r, uint b) {\n"
"    float3 v = (b & 0x10) ? r.aaa : r.rgb;\n"
"    return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));\n"
"}\n"
"float read_alpha(float4 r, uint b) {\n"
"    return map_in((b & 0x10) ? r.a : r.b, b);\n"
"}\n"
"float4 rc_eval(float4 v0, float4 v1, float4 fog, float4 t0, float4 t1,\n"
"               float4 t2, float4 t3) {\n"
"    float4 R[16];\n"
"    uint n = min(ctl.x & 0xFF, 8u), flags = ctl.x >> 8, s, k;\n"
"    [unroll] for (k = 0; k < 16; k++) R[k] = float4(0.0, 0.0, 0.0, 0.0);\n"
"    R[3] = fog; R[4] = v0; R[5] = v1;\n"
"    R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
"    R[12].a = (ctl.y & 0x1F) ? t0.a : 1.0;\n"
"    [loop] for (s = 0; s < n; s++) {\n"
"        uint icw = W8(cicw, s), ocw = W8(cocw, s);\n"
"        uint ai = W8(aicw, s), ao = W8(aocw, s);\n"
"        uint fl = ocw >> 12, afl = ao >> 12, d;\n"
"        bool mux;\n"
"        float3 A, B, C, D, pab, pcd, sum, ab, cd, ms;\n"
"        float aA, aB, aC, aD, sab, scd, ssum, aab, acd, ams;\n"
"        R[1] = unpack(W8(fac0, (flags & 0x010) ? s : 0));\n"
"        R[2] = unpack(W8(fac1, (flags & 0x100) ? s : 0));\n"
"        mux = (flags & 1) ? R[12].a >= 0.5 : ((((int)(R[12].a * 255.0)) & 1) != 0);\n"
"        A = read_rgb(R[(icw >> 24) & 0xF], icw >> 24);\n"
"        B = read_rgb(R[(icw >> 16) & 0xF], icw >> 16);\n"
"        C = read_rgb(R[(icw >> 8) & 0xF], icw >> 8);\n"
"        D = read_rgb(R[icw & 0xF], icw);\n"
"        aA = read_alpha(R[(ai >> 24) & 0xF], ai >> 24);\n"
"        aB = read_alpha(R[(ai >> 16) & 0xF], ai >> 16);\n"
"        aC = read_alpha(R[(ai >> 8) & 0xF], ai >> 8);\n"
"        aD = read_alpha(R[ai & 0xF], ai);\n"
"        pab = (fl & 2) ? dot(A, B).xxx : A * B;\n"
"        pcd = (fl & 1) ? dot(C, D).xxx : C * D;\n"
"        sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"        ab = clamp(map_out(pab, fl & 0x38), -1.0, 1.0);\n"
"        cd = clamp(map_out(pcd, fl & 0x38), -1.0, 1.0);\n"
"        ms = clamp(map_out(sum, fl & 0x38), -1.0, 1.0);\n"
"        sab = aA * aB; scd = aC * aD;\n"
"        ssum = (afl & 4) ? (mux ? scd : sab) : sab + scd;\n"
"        aab = clamp(map_out(sab.xxx, afl & 0x38).x, -1.0, 1.0);\n"
"        acd = clamp(map_out(scd.xxx, afl & 0x38).x, -1.0, 1.0);\n"
"        ams = clamp(map_out(ssum.xxx, afl & 0x38).x, -1.0, 1.0);\n"
"        d = (ocw >> 4) & 0xF;\n"
"        if (d != 0) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.b; }\n"
"        d = ocw & 0xF;\n"
"        if (d != 0) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.b; }\n"
"        d = (ocw >> 8) & 0xF;\n"
"        if (d != 0) R[d].rgb = ms;\n"
"        d = (ao >> 4) & 0xF;\n"
"        if (d != 0) R[d].a = aab;\n"
"        d = ao & 0xF;\n"
"        if (d != 0) R[d].a = acd;\n"
"        d = (ao >> 8) & 0xF;\n"
"        if (d != 0) R[d].a = ams;\n"
"        R[0] = float4(0.0, 0.0, 0.0, 0.0);\n"
"    }\n"
"    float4 o;\n"
"    if (fin.x != 0 || fin.y != 0) {\n"
"        uint f0 = fin.x, f1 = fin.y, ff = f1 & 0xFF;\n"
"        float3 a, b, E, F, A, B, C, D;\n"
"        R[1] = unpack(fin.z); R[2] = unpack(fin.w);\n"
"        a = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"        b = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"        R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0.0);\n"
"        E = read_rgb(R[(f1 >> 24) & 0xF], f1 >> 24);\n"
"        F = read_rgb(R[(f1 >> 16) & 0xF], f1 >> 16);\n"
"        R[15] = float4(E * F, 0.0);\n"
"        A = read_rgb(R[(f0 >> 24) & 0xF], f0 >> 24);\n"
"        B = read_rgb(R[(f0 >> 16) & 0xF], f0 >> 16);\n"
"        C = read_rgb(R[(f0 >> 8) & 0xF], f0 >> 8);\n"
"        D = read_rgb(R[f0 & 0xF], f0);\n"
"        o.rgb = D + C * (1.0 - A) + B * A;\n"
"        o.a = read_alpha(R[(f1 >> 8) & 0xF], f1 >> 8);\n"
"    } else {\n"
"        o = R[12];\n"
"    }\n"
"    return saturate(o);\n"
"}\n"
/* alpha_test_pass: the value as the software path rounds it. */
"bool alpha_ok(float a) {\n"
"    int v = (int)(saturate(a) * 255.0 + 0.5), r = (int)(at.y & 0xFF);\n"
"    uint f = at.x;\n"
"    if (f == 0x200) return false;\n"
"    if (f == 0x201) return v < r;\n"
"    if (f == 0x202) return v == r;\n"
"    if (f == 0x203) return v <= r;\n"
"    if (f == 0x204) return v > r;\n"
"    if (f == 0x205) return v != r;\n"
"    if (f == 0x206) return v >= r;\n"
"    return true;\n"
"}\n"
"float4 ps_main(PSIn i) : SV_Target {\n"
"    uint kind = at.w, fl = ctl.w;\n"
"    float4 col;\n"
"    if (kind == 0) {\n"            /* screen space: flat, the texel replaces it */
"        col = i.fc;\n"
"        if ((fl & 2) && (tex[0].z & 8)) {\n"
"            float2 uv = max(i.t0.xy, 0.0);\n"
"            if ((tex[0].z & 64) && !(tex[0].z & 32))\n"
"                col = rt_texel(0, uv.x, uv.y, 0, false);\n"
"            else\n"
"                col = texel_at(0, (int)uv.x, (int)uv.y, 0);\n"
"        }\n"
"        return col;\n"
"    }\n"
/* Past the far end of the depth range. The software path compares z as it
 * is, so with LESS / LEQUAL / EQUAL such a pixel fails against anything the
 * buffer can hold; D3D clamps it to 1.0 first, where LEQUAL against a
 * cleared buffer passes. Near-plane-clipped triangles reach far beyond. */
"    if ((fl & 8) && i.zn > 1.0000005) discard;\n"
/* SET_CLIP_MIN/MAX, as the executor's own rasteriser applies them. */
"    if ((fl & 64) && (i.zw < zr.x || i.zw > zr.y)) discard;\n"
"    if (fl & 1) {\n"               /* register combiners */
"        float4 t0, t1, t2, t3;\n"
"        if (!stage_fetch(0, i.t0, t0)) discard;\n"
"        if (!stage_fetch(1, i.t1, t1)) discard;\n"
"        if (!stage_fetch(2, i.t2, t2)) discard;\n"
"        if (!stage_fetch(3, i.t3, t3)) discard;\n"
"        col = rc_eval(i.d0, i.d1, float4(fogc.rgb, i.fog), t0, t1, t2, t3);\n"
"    } else {\n"                    /* stage 0 times diffuse */
"        col = i.d0;\n"
"        if ((fl & 2) && (tex[0].z & 8)) {\n"
"            float2 uv = i.t0.xy;\n"
"            if (tex[0].z & 1) uv *= float2(tex[0].x & 0xFFFF, tex[0].x >> 16);\n"
"            if ((tex[0].z & 64) && !(tex[0].z & 32))\n"
"                col *= rt_texel(0, uv.x, uv.y, 0, false);\n"
"            else\n"
"                col *= texel_at(0, (int)floor(uv.x), (int)floor(uv.y), 0);\n"
"        }\n"
"        col = saturate(col);\n"
"    }\n"
"    if ((fl & 4) && !alpha_ok(col.a)) discard;\n"
"    return col;\n"
"}\n";

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
/* tex[i][2] flags */
#define TXF_NORM   1u
#define TXF_BILIN  2u
#define TXF_CUBE   4u
#define TXF_VALID  8u
#define TXF_OPAQUE 16u                  /* alpha reads 1 (X8R8G8B8) */
#define TXF_SWZ    32u                  /* swizzled view of a linear target: .w is its row */
#define TXF_RT     64u                  /* a render target, stored at the internal scale */

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
static ID3D11PixelShader *s_ps;
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
static uint64_t s_zpass_acc;            /* resolved, not yet reported */

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
 * guest RAM gets; also the SSAA reduction for the presenter. */
static const char s_blit_hlsl[] =
"Texture2DArray<float4> bsrc : register(t0);\n"
"cbuffer B : register(b3) { float4 bk; };\n"   /* the block: s_sx, s_sy, or s_ssaa twice */
"float4 bvs(uint i : SV_VertexID) : SV_Position {\n"
"    float2 q = float2((i << 1) & 2, i & 2);\n"
"    return float4(q * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"}\n"
"float4 bps_up(float4 p : SV_Position) : SV_Target {\n"
"    return bsrc.Load(int4(int2(p.xy) / int2(bk.xy), 0, 0));\n"
"}\n"
"float4 bps_down(float4 p : SV_Position) : SV_Target {\n"
"    int2 s = int2(bk.xy), b = int2(p.xy) * s;\n"
"    float4 a = float4(0.0, 0.0, 0.0, 0.0);\n"
"    int x, y;\n"
"    [loop] for (y = 0; y < s.y; y++)\n"
"        [loop] for (x = 0; x < s.x; x++)\n"
"            a += bsrc.Load(int4(b + int2(x, y), 0, 0));\n"
"    return a / (float)(s.x * s.y);\n"
"}\n";

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

    box.left = x0; box.right = x1; box.top = y0; box.bottom = y1;
    box.front = 0; box.back = 1;
    if (s_scaled) {
        D3D11_RECT rc;
        rt_resolve(r);
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
    } else if (H > r->H) {
        /* The title draws further down than before: those rows join. */
        if (!nv2a_backend_surface_ok(r->va, H * r->pitch))
            return NULL;
        rt_upload_rows(r, r->H, H);
        r->H = H;
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

    while ((hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)q, &n,
                                             sizeof n, 0)) == S_FALSE)
        YieldProcessor();
    if (hr == S_OK)                     /* samples at the internal scale -> pixels */
        s_zpass_acc += n / ((UINT64)s_sx * s_sy * s_msaa);
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

static void gpu_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
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

static void gpu_draw(const Nv2aSurface *s, const Nv2aBatch *b)
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

/* ---- sync, reports, flips ----------------------------------------------- */

static void gpu_sync(uint32_t reason)
{
    int i;

    if (s_compare)
        return;
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
    query_resolve_all();
    n = s_zpass_acc;
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
                    " rasteriser takes over%c", (unsigned long)why, s_flip_no, 10);
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
        fprintf(stderr, "[GPURAST] %.1fs: flips %u, draws %u, writebacks %u (%.1f MB),"
                        " guest->rt uploads %u (%.1f MB), tex decodes %u, DXT as BC %u,"
                        " rt as texture %u, GPU presents %u, readback stall ms %.1f"
                        " (writebacks for: textures %u, dropped targets %u, syncs %u,"
                        " presents %u), dropped as taken by the title %u\n",
                (double)(now - s_st.last_tick) / 1000.0, s_st.flips, s_st.draws,
                s_st.writebacks, (double)s_st.wb_bytes / 1048576.0,
                s_st.uploads, (double)s_st.up_bytes / 1048576.0,
                s_st.decodes, s_st.bc_uploads, s_st.rt_tex, s_st.presents, s_st.stall_ms,
                s_st.wb_why[0], s_st.wb_why[1], s_st.wb_why[2], s_st.wb_why[3],
                s_st.taken);
        memset(&s_st, 0, sizeof s_st);
        s_st.last_tick = now;
    }
    gpu_lost();
}

/* ---- install ------------------------------------------------------------ */

static Nv2aBackend s_be;
static void present_release(void);

static void release_all(void)
{
    int i;
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
    REL(s_ps);
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
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
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

static ID3DBlob *compile(const char *entry, const char *target)
{
    return compile_src(s_hlsl, sizeof s_hlsl - 1, entry, target);
}

/* ---- presenting from the GPU --------------------------------------------
 *
 * The framebuffer window (src/video/fb_present.c, RECOMP_FB_WINDOW) shows
 * frames with GDI from guest RAM, which with this back end meant reading the
 * presented frame back from the GPU at every flip. Instead a swap chain goes
 * on that window and the presented target is drawn into its back buffer
 * directly: 640x480 like the GDI window, every other pixel of a 2x-wide
 * anti-aliased surface and every aa_sy-th row, as fb_present takes them.
 * The window thread keeps the title bar, the keyboard and the close button.
 * Without a window there is nothing to show, so nothing is read back at all.
 * The frame reaches guest RAM only when something reads it there (the
 * executor's dumps, a texture decoded from it, a target being dropped).
 * RECOMP_GPU_RASTER_PRESENT=0 keeps the window on GDI and guest RAM. The
 * swap chain is created the way upstream gpu_present.c (MIT) creates its
 * own; Present runs on this thread, the window's messages on its own. */
#define PRESENT_W 640
#define PRESENT_H 480

/* The back buffer is the window's client area; the 640x480 frame is drawn
 * into the rectangle xbox_FramebufferWindowFit gives (the display aspect,
 * bars around it), bilinear between the target's texels -- exactly the
 * frame's pixels when the rectangle is the target's size. vp: the rectangle;
 * fs: the frame in texels of what is drawn: the target, or s_ss under SSAA
 * (640x480 times a 2x-wide anti-aliased surface's 2, aa_sy and the scale). */
static const char s_present_hlsl[] =
"Texture2DArray<float4> src : register(t0);\n"
"cbuffer P : register(b2) { float4 pk; float4 vp; float4 fs; };\n"
"float4 pvs(uint i : SV_VertexID) : SV_Position {\n"
"    float2 q = float2((i << 1) & 2, i & 2);\n"
"    return float4(q * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"}\n"
"float3 tap(int2 c) {\n"
"    c = clamp(c, int2(0, 0), int2(fs.xy) - 1);\n"
"    return src.Load(int4(c, 0, 0)).rgb;\n"
"}\n"
"float4 pps(float4 p : SV_Position) : SV_Target {\n"
"    float2 s = (p.xy - vp.xy) / vp.zw * fs.xy - 0.5;\n"
"    int2 i = int2(floor(s));\n"
"    float2 f = s - floor(s);\n"
"    float3 a = lerp(tap(i), tap(i + int2(1, 0)), f.x);\n"
"    float3 b = lerp(tap(i + int2(0, 1)), tap(i + int2(1, 1)), f.x);\n"
"    return float4(lerp(a, b, f.y), 1.0);\n"
"}\n";

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

static int present_setup(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    D3D11_BUFFER_DESC bd;
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *xa = NULL;
    IDXGIFactory *xf = NULL;
    ID3D11Texture2D *back = NULL;
    ID3DBlob *vs = NULL, *ps = NULL;
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
    IDXGIFactory_MakeWindowAssociation(xf, hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(IDXGISwapChain_GetBuffer(s_chain, 0, &IID_ID3D11Texture2D, (void **)&back))
        || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)back, NULL,
                                                      &s_bb_rtv)))
        goto done;
    vs = compile_src(s_present_hlsl, sizeof s_present_hlsl - 1, "pvs", "vs_4_0");
    ps = compile_src(s_present_hlsl, sizeof s_present_hlsl - 1, "pps", "ps_4_0");
    if (!vs || !ps
        || FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vs),
                                                  ID3D10Blob_GetBufferSize(vs), NULL, &s_pvs))
        || FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(ps),
                                                 ID3D10Blob_GetBufferSize(ps), NULL, &s_pps)))
        goto done;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_present)))
        goto done;
    ok = 1;
    fprintf(stderr, "[GPURAST] presenting into the framebuffer window from the GPU\n");
done:
    REL(back);
    REL(vs);
    REL(ps);
    REL(xf);
    REL(xa);
    REL(xd);
    return ok;
}

static void present_release(void)
{
    REL(s_cb_present);
    REL(s_pps);
    REL(s_pvs);
    REL(s_bb_rtv);
    REL(s_chain);
}

static int gpu_present(uint32_t va, uint32_t pitch, uint32_t aa_sy)
{
    static const float zero[4] = { 0, 0, 0, 0 };
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    D3D11_RECT rc;
    GpuRt *r = NULL;
    ID3D11ShaderResourceView *src;
    HWND hwnd;
    float pk[12];
    int32_t fit[4];
    int i;

    if (s_compare || !s_present_on || s_present_failed)
        return 0;
    for (i = 0; i < MAX_RTS; i++)
        if (s_rt[i].used && s_rt[i].va == va && s_rt[i].pitch == pitch) {
            r = &s_rt[i];
            break;
        }
    if (!r) {                           /* not drawn here: the guest RAM path */
        if (s_chain)
            xbox_FramebufferWindowExternal(0);
        return 0;
    }
    hwnd = (HWND)xbox_FramebufferWindowHandle();
    if (!hwnd) {                        /* headless: nothing to show to */
        if (getenv("RECOMP_FB_WINDOW"))
            return 0;
        if (rt_check(r)) {
            uint32_t sx = pitch / (PRESENT_W * r->bpp);
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
    {
        /* The back buffer follows the window: resized, maximised, F11. */
        uint32_t cw, ch;
        client_size(hwnd, &cw, &ch);
        if (!cw)                        /* minimised: nothing to draw into */
            return 1;
        if (cw != s_bb_w || ch != s_bb_h) {
            ID3D11Texture2D *back = NULL;
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
            REL(s_bb_rtv);
            if (FAILED(IDXGISwapChain_ResizeBuffers(s_chain, 0, cw, ch,
                                                    DXGI_FORMAT_UNKNOWN, 0))
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
    }
    xbox_FramebufferWindowFit(s_bb_w, s_bb_h, fit);
    pk[0] = (float)(pitch / (PRESENT_W * r->bpp) ? pitch / (PRESENT_W * r->bpp) : 1);
    pk[1] = (float)(aa_sy ? aa_sy : 1);
    pk[2] = pk[3] = 0.0f;
    pk[4] = (float)fit[0]; pk[5] = (float)fit[1];
    pk[6] = (float)fit[2]; pk[7] = (float)fit[3];
    pk[8] = (float)PRESENT_W * pk[0] * (float)s_ox;
    pk[9] = (float)PRESENT_H * pk[1] * (float)s_oy;
    rt_resolve(r);
    src = r->srv;
    if (s_ssaa > 1) {                   /* before the presenter takes over the pipeline */
        ssaa_reduce(r, (uint32_t)pk[8], (uint32_t)pk[9]);
        src = s_ss_srv;
    }
    pk[10] = pk[11] = 0.0f;
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
    IDXGISwapChain_Present(s_chain, 0, 0);
    present_dump(r, (uint32_t)pk[0], (uint32_t)pk[1]);
    bind_pipeline();
    s_st.presents++;
    return 1;
}

#ifdef NV2A_GPU_TEST_HOOKS
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
    ID3DBlob *vs = compile("vs_main", "vs_4_0"), *ps = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    D3D11_SUBRESOURCE_DATA sd;
    static const uint32_t white = 0xFFFFFFFFu;
    int ok = 0;

    if (!vs)
        return 0;
    ps = compile("ps_main", "ps_4_0");
    if (!ps)
        goto done;
    if (FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vs),
                                               ID3D10Blob_GetBufferSize(vs), NULL, &s_vs))
        || FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(ps),
                                                 ID3D10Blob_GetBufferSize(ps), NULL, &s_ps))
        || FAILED(ID3D11Device_CreateInputLayout(s_dev, il, (UINT)(sizeof il / sizeof il[0]),
                                                 ID3D10Blob_GetBufferPointer(vs),
                                                 ID3D10Blob_GetBufferSize(vs), &s_il)))
        goto done;

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
        ID3DBlob *b;
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
        b = compile_src(s_blit_hlsl, sizeof s_blit_hlsl - 1, "bvs", "vs_4_0");
        if (!b || FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                                         ID3D10Blob_GetBufferSize(b), NULL,
                                                         &s_bvs))) {
            REL(b);
            goto done;
        }
        REL(b);
        b = compile_src(s_blit_hlsl, sizeof s_blit_hlsl - 1, "bps_up", "ps_4_0");
        if (!b || FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                                        ID3D10Blob_GetBufferSize(b), NULL,
                                                        &s_bps_up))) {
            REL(b);
            goto done;
        }
        REL(b);
        b = compile_src(s_blit_hlsl, sizeof s_blit_hlsl - 1, "bps_down", "ps_4_0");
        if (!b || FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                                        ID3D10Blob_GetBufferSize(b), NULL,
                                                        &s_bps_down))) {
            REL(b);
            goto done;
        }
        REL(b);
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
    REL(vs);
    REL(ps);
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
    if (!create_pipeline()) {
        fprintf(stderr, "[GPURAST] pipeline setup failed; software rasteriser stays\n");
        release_all();
        return 0;
    }

    s_be.clear = gpu_clear;
    s_be.draw = gpu_draw;
    s_be.flip = gpu_flip;
    s_be.sync = gpu_sync;
    s_be.report = gpu_report;
    s_be.shadow = s_compare;
    s_be.sync_surface = s_sync_all ? NULL : gpu_sync_surface;
    s_be.present = gpu_present;
    nv2a_backend_register(&s_be);
    fprintf(stderr, "[GPURAST] D3D11 back end on: %ls, FL %u_%u%s\n", ad.Description,
            ((unsigned)fl >> 12) & 0xF, ((unsigned)fl >> 8) & 0xF,
            s_compare ? " (compare: the software path draws, the GPU checks)" : "");
    return 1;
}
