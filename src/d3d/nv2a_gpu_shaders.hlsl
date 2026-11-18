/*
 * nv2a_gpu_shaders.hlsl -- the shaders of the Direct3D 11 back end
 * (nv2a_gpu_d3d11.c). Compiled when the toolkit is built (nv2a_gpu_shaderc,
 * nv2a_gpu_shaders.cmake) into nv2a_gpu_shaders.h, which also carries this
 * text as s_hlsl for what is still compiled at run time (RECOMP_GPU_PROF's
 * flatps).
 */
cbuffer VSC : register(b0) { float4 vsc; };
cbuffer PSC : register(b1) {
    uint4 cicw[2]; uint4 aicw[2]; uint4 cocw[2]; uint4 aocw[2];
    uint4 fac0[2]; uint4 fac1[2];
    uint4 fin; /* final0, final1, final_c0, final_c1 */
    uint4 ctl; /* control, stage_program, clip_plane_mode, flags */
    uint4 at; /* alpha_func, alpha_ref, stage_valid, kind */
    float4 fogc;
    uint4 tex[4]; /* w | h<<16, addr_u | addr_v<<8, flags, origin x | y<<16 */
    float4 zr; /* SET_CLIP_MIN, SET_CLIP_MAX (window z) */
    float4 rsc; /* the internal scale of render targets: x, y */
};
Texture2DArray<float4> T0 : register(t0);
Texture2DArray<float4> T1 : register(t1);
Texture2DArray<float4> T2 : register(t2);
Texture2DArray<float4> T3 : register(t3);
#define W8(a, i) a[(i) >> 2][(i) & 3]
struct VSIn {
    float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;
    float fog : FOG;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
};
struct PSIn {
    float4 pos : SV_Position;
    noperspective float4 d0 : COLOR0;
    noperspective float4 d1 : COLOR1;
    noperspective float fog : FOG;
    nointerpolation float4 fc : COLOR2;
    noperspective float zn : ZRANGE; /* z / zmax, unclamped, screen-linear */
    noperspective float zw : ZWINDOW; /* window z as the title produced it */
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
};
/* Surface pixels to clip space. A transformed vertex keeps its w, so texture
 * coordinates interpolate perspective-correct as on the software path;
 * colours and fog interpolate in screen space, as there. */
PSIn vs_main(VSIn i) {
    PSIn o;
    float w = (vsc.y > 0.5 && i.pos.w > 0.0) ? 1.0 / i.pos.w : 1.0;
    float3 ndc = float3(i.pos.x / 640.0 - 1.0, 1.0 - i.pos.y / 512.0,
                        i.pos.z * vsc.x);
    o.pos = float4(ndc * w, w);
    o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog; o.fc = i.d0;
    o.zn = i.pos.z * vsc.x;
    o.zw = i.pos.z;
    o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;
    return o;
}
float4 unpack(uint c) {
    return float4((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, c >> 24) / 255.0;
}
float4 load_st(uint st, int x, int y, uint face) {
    int4 c = int4(x, y, (int)face, 0);
    if (st == 0) return T0.Load(c);
    if (st == 1) return T1.Load(c);
    if (st == 2) return T2.Load(c);
    return T3.Load(c);
}
/* wrap_coord: 1 wraps, everything else clamps. A signed integer division is a
 * long sequence on the GPU and it ran for every tap -- four per bilinear
 * sample: most coordinates are already inside, and most sizes are powers of
 * two, where two's complement makes c & (n - 1) the wrapped value of any c.
 * The division is left for the rest, so every c, n > 0 gives what it gave. */
int wrapc(int c, int n, uint mode) {
    if (n <= 0) return 0;
    if (mode == 1) {
        if ((uint)c < (uint)n) return c;
        if ((n & (n - 1)) == 0) return c & (n - 1);
        { int r = c - (c / n) * n; return r < 0 ? r + n : r; }
    }
    return clamp(c, 0, n - 1);
}
/* swizzle_offset (d3d8_swizzle.h): x and y bits interleaved, the larger
 * dimension carrying on alone once the smaller one runs out. */
uint swz(uint x, uint y, uint w, uint h) {
    uint o = 0, bit = 1, mb = 1, k;
    [loop] for (k = 0; k < 16 && (bit < w || bit < h); k++) {
        if (bit < w) { if (x & bit) o |= mb; mb <<= 1; }
        if (bit < h) { if (y & bit) o |= mb; mb <<= 1; }
        bit <<= 1;
    }
    return o;
}
float4 texel_at(uint st, int x, int y, uint face) {
    uint4 p = tex[st];
    int w = (int)(p.x & 0xFFFF), h = (int)(p.x >> 16);
    int u = wrapc(x, w, p.y & 0xFF), v = wrapc(y, h, (p.y >> 8) & 0xFF);
    float4 c;
    if (p.z & 32) { /* a render target read as a swizzled texture */
        uint i = swz((uint)u, (uint)v, (uint)w, (uint)h);
        int2 s = int2(rsc.xy); /* the centre of the texel at the internal scale */
        c = load_st(st, (int)(i % p.w) * s.x + s.x / 2, (int)(i / p.w) * s.y + s.y / 2, face);
    } else {
        c = load_st(st, u + (int)(p.w & 0xFFFF), v + (int)(p.w >> 16), face);
    }
    if (p.z & 16) c.a = 1.0; /* X8R8G8B8 read from a render target */
    return c;
}
/* A render target sampled as a texture (linear view), at the internal scale:
 * (u, v) in the texture's texels, continuous, so a target drawn larger is read
 * at its own resolution. At scale 1 these are exactly the taps below. */
float4 rt_texel(uint st, float u, float v, uint face, bool bil) {
    uint4 p = tex[st];
    int2 s = int2(rsc.xy);
    int w = (int)(p.x & 0xFFFF) * s.x, h = (int)(p.x >> 16) * s.y;
    int ox = (int)(p.w & 0xFFFF) * s.x, oy = (int)(p.w >> 16) * s.y;
    uint mu = p.y & 0xFF, mv = (p.y >> 8) & 0xFF;
    float4 c;
    u *= rsc.x; v *= rsc.y;
    if (bil) {
        float fu = u - 0.5, fv = v - 0.5, iu = floor(fu), iv = floor(fv);
        float wu = fu - iu, wv = fv - iv;
        int x0 = wrapc((int)iu, w, mu) + ox, x1 = wrapc((int)iu + 1, w, mu) + ox;
        int y0 = wrapc((int)iv, h, mv) + oy, y1 = wrapc((int)iv + 1, h, mv) + oy;
        float4 c0 = load_st(st, x0, y0, face), c1 = load_st(st, x1, y0, face);
        float4 c2 = load_st(st, x0, y1, face), c3 = load_st(st, x1, y1, face);
        c = (c0 * (1.0 - wu) + c1 * wu) * (1.0 - wv) + (c2 * (1.0 - wu) + c3 * wu) * wv;
    } else {
        c = load_st(st, wrapc((int)floor(u), w, mu) + ox, wrapc((int)floor(v), h, mv) + oy, face);
    }
    if (p.z & 16) c.a = 1.0;
    return c;
}
/* rc_texel: normalised (swizzled/DXT) or texel coordinates; an unusable
 * stage reads white; bilinear taps at floor(u - 0.5).
 * RC_TEX picks the combiner shader's variant by what its stages sample (the
 * CPU knows, pick_ps): 0 any; 1 no render target read linearly (rt_texel
 * left out); 2 only such targets (the scene filtered down by Fallout, its
 * 0x80778000 -> 0x809D0000 pass: texel_at left out). The same lines,
 * fewer of them compiled. */
#ifndef RC_TEX
#define RC_TEX 0
#endif
float4 rc_texel(uint st, float u, float v, uint face) {
    uint4 p = tex[st];
    if (!(p.z & 8)) return float4(1.0, 1.0, 1.0, 1.0);
    if (p.z & 1) { u *= (float)(p.x & 0xFFFF); v *= (float)(p.x >> 16); }
#if RC_TEX == 2
    return rt_texel(st, u, v, face, (p.z & 2) != 0);
#else
#if RC_TEX == 0
    if ((p.z & 64) && !(p.z & 32)) return rt_texel(st, u, v, face, (p.z & 2) != 0);
#endif
    if (p.z & 2) {
        float fu = u - 0.5, fv = v - 0.5;
        float iu = floor(fu), iv = floor(fv);
        float wu = fu - iu, wv = fv - iv;
        int x = (int)iu, y = (int)iv;
        float4 c0 = texel_at(st, x, y, face), c1 = texel_at(st, x + 1, y, face);
        float4 c2 = texel_at(st, x, y + 1, face), c3 = texel_at(st, x + 1, y + 1, face);
        return (c0 * (1.0 - wu) + c1 * wu) * (1.0 - wv) + (c2 * (1.0 - wu) + c3 * wu) * wv;
    }
    return texel_at(st, (int)floor(u), (int)floor(v), face);
#endif
}
/* rc_stage_fetch: false when a clip-plane stage kills the pixel. */
bool stage_fetch(uint st, float4 c, out float4 o) {
    uint mode = (ctl.y >> (st * 5)) & 0x1F;
    float q = c.w != 0.0 ? c.w : 1.0;
    o = float4(0.0, 0.0, 0.0, 0.0);
    if (mode == 0) { o = float4(0.0, 0.0, 0.0, 1.0); return true; }
    if (mode == 1 || mode == 2) { o = rc_texel(st, c.x / q, c.y / q, 0); return true; }
    if (mode == 3) {
        float x = c.x, y = c.y, z = c.z, ax = abs(x), ay = abs(y), az = abs(z);
        float ma, sc, tc; uint face;
        if (!(tex[st].z & 4)) { o = rc_texel(st, c.x, c.y, 0); return true; }
        if (ax >= ay && ax >= az) {
            face = x > 0.0 ? 0 : 1; ma = ax; sc = x > 0.0 ? -z : z; tc = -y;
        } else if (ay >= az) {
            face = y > 0.0 ? 2 : 3; ma = ay; sc = x; tc = y > 0.0 ? z : -z;
        } else {
            face = z > 0.0 ? 4 : 5; ma = az; sc = z > 0.0 ? x : -x; tc = -y;
        }
        if (ma == 0.0) ma = 1.0;
        o = rc_texel(st, (sc / ma + 1.0) * 0.5, (tc / ma + 1.0) * 0.5, face);
        return true;
    }
    if (mode == 4) { o = saturate(c); return true; }
    if (mode == 5) {
        [unroll] for (uint j = 0; j < 4; j++) {
            uint ge = (ctl.z >> (st * 4 + j)) & 1;
            if (ge != 0 ? c[j] >= 0.0 : c[j] < 0.0) return false;
        }
        return true;
    }
    return true;
}
/* nv2a_rc_eval, line by line. */
float map_in(float x, uint b) {
    float p = x < 0.0 ? 0.0 : x;
    uint m = (b >> 5) & 7;
    if (m == 0) return p;
    if (m == 1) return 1.0 - saturate(x);
    if (m == 2) return 2.0 * p - 1.0;
    if (m == 3) return -2.0 * p + 1.0;
    if (m == 4) return p - 0.5;
    if (m == 5) return -p + 0.5;
    if (m == 6) return x;
    return -x;
}
float3 map_out(float3 x, uint m) {
    if (m == 0x08) return x - 0.5;
    if (m == 0x10) return x * 2.0;
    if (m == 0x18) return (x - 0.5) * 2.0;
    if (m == 0x20) return x * 4.0;
    if (m == 0x30) return x * 0.5;
    return x;
}
float3 read_rgb(float4 r, uint b) {
    float3 v = (b & 0x10) ? r.aaa : r.rgb;
    return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));
}
float read_alpha(float4 r, uint b) {
    return map_in((b & 0x10) ? r.a : r.b, b);
}
/* The combiners' registers, 1..15, as fifteen variables (register 0 is always
 * zero: nothing writes it). Which register an input or output names comes
 * from the combiner words -- the same for every pixel of a draw, so the
 * switches below are uniform. An array indexed at run time (float4 R[16])
 * is an indexable temp, which NVIDIA keeps in local memory: that was most of
 * what the combiner path cost (~490 ns per 1000 pixels by Ruby's bar). */
#define RREGS R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, R13, R14, R15
#define RPARAMS float4 R1, float4 R2, float4 R3, float4 R4, float4 R5, float4 R6, \
    float4 R7, float4 R8, float4 R9, float4 R10, float4 R11, float4 R12, float4 R13, \
    float4 R14, float4 R15
#define RINOUT inout float4 R1, inout float4 R2, inout float4 R3, inout float4 R4, \
    inout float4 R5, inout float4 R6, inout float4 R7, inout float4 R8, inout float4 R9, \
    inout float4 R10, inout float4 R11, inout float4 R12, inout float4 R13, \
    inout float4 R14, inout float4 R15
float4 rget(uint i, RPARAMS) {
    [forcecase] switch (i) {
    case 1: return R1;   case 2: return R2;   case 3: return R3;   case 4: return R4;
    case 5: return R5;   case 6: return R6;   case 7: return R7;   case 8: return R8;
    case 9: return R9;   case 10: return R10; case 11: return R11; case 12: return R12;
    case 13: return R13; case 14: return R14; case 15: return R15;
    default: return float4(0.0, 0.0, 0.0, 0.0);
    }
}
void rset_rgb(uint i, float3 v, RINOUT) {
    [forcecase] switch (i) {
    case 1: R1.rgb = v; break;   case 2: R2.rgb = v; break;   case 3: R3.rgb = v; break;
    case 4: R4.rgb = v; break;   case 5: R5.rgb = v; break;   case 6: R6.rgb = v; break;
    case 7: R7.rgb = v; break;   case 8: R8.rgb = v; break;   case 9: R9.rgb = v; break;
    case 10: R10.rgb = v; break; case 11: R11.rgb = v; break; case 12: R12.rgb = v; break;
    case 13: R13.rgb = v; break; case 14: R14.rgb = v; break; case 15: R15.rgb = v; break;
    default: break;
    }
}
void rset_a(uint i, float a, RINOUT) {
    [forcecase] switch (i) {
    case 1: R1.a = a; break;   case 2: R2.a = a; break;   case 3: R3.a = a; break;
    case 4: R4.a = a; break;   case 5: R5.a = a; break;   case 6: R6.a = a; break;
    case 7: R7.a = a; break;   case 8: R8.a = a; break;   case 9: R9.a = a; break;
    case 10: R10.a = a; break; case 11: R11.a = a; break; case 12: R12.a = a; break;
    case 13: R13.a = a; break; case 14: R14.a = a; break; case 15: R15.a = a; break;
    default: break;
    }
}
float4 rc_eval(float4 v0, float4 v1, float4 fog, float4 t0, float4 t1,
               float4 t2, float4 t3) {
    float4 R1 = 0.0, R2 = 0.0, R3 = 0.0, R4 = 0.0, R5 = 0.0, R6 = 0.0, R7 = 0.0;
    float4 R8 = 0.0, R9 = 0.0, R10 = 0.0, R11 = 0.0, R12 = 0.0, R13 = 0.0, R14 = 0.0;
    float4 R15 = 0.0;
    uint n = min(ctl.x & 0xFF, 8u), flags = ctl.x >> 8, s;
    R3 = fog; R4 = v0; R5 = v1;
    R8 = t0; R9 = t1; R10 = t2; R11 = t3;
    R12.a = (ctl.y & 0x1F) ? t0.a : 1.0;
    [loop] for (s = 0; s < n; s++) {
        uint icw = W8(cicw, s), ocw = W8(cocw, s);
        uint ai = W8(aicw, s), ao = W8(aocw, s);
        uint fl = ocw >> 12, afl = ao >> 12, d;
        bool mux;
        float3 A, B, C, D, pab, pcd, sum, ab, cd, ms;
        float aA, aB, aC, aD, sab, scd, ssum, aab, acd, ams;
        R1 = unpack(W8(fac0, (flags & 0x010) ? s : 0));
        R2 = unpack(W8(fac1, (flags & 0x100) ? s : 0));
        mux = (flags & 1) ? R12.a >= 0.5 : ((((int)(R12.a * 255.0)) & 1) != 0);
        A = read_rgb(rget((icw >> 24) & 0xF, RREGS), icw >> 24);
        B = read_rgb(rget((icw >> 16) & 0xF, RREGS), icw >> 16);
        C = read_rgb(rget((icw >> 8) & 0xF, RREGS), icw >> 8);
        D = read_rgb(rget(icw & 0xF, RREGS), icw);
        aA = read_alpha(rget((ai >> 24) & 0xF, RREGS), ai >> 24);
        aB = read_alpha(rget((ai >> 16) & 0xF, RREGS), ai >> 16);
        aC = read_alpha(rget((ai >> 8) & 0xF, RREGS), ai >> 8);
        aD = read_alpha(rget(ai & 0xF, RREGS), ai);
        pab = (fl & 2) ? dot(A, B).xxx : A * B;
        pcd = (fl & 1) ? dot(C, D).xxx : C * D;
        sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;
        ab = clamp(map_out(pab, fl & 0x38), -1.0, 1.0);
        cd = clamp(map_out(pcd, fl & 0x38), -1.0, 1.0);
        ms = clamp(map_out(sum, fl & 0x38), -1.0, 1.0);
        sab = aA * aB; scd = aC * aD;
        ssum = (afl & 4) ? (mux ? scd : sab) : sab + scd;
        aab = clamp(map_out(sab.xxx, afl & 0x38).x, -1.0, 1.0);
        acd = clamp(map_out(scd.xxx, afl & 0x38).x, -1.0, 1.0);
        ams = clamp(map_out(ssum.xxx, afl & 0x38).x, -1.0, 1.0);
        d = (ocw >> 4) & 0xF;
        if (d != 0) { rset_rgb(d, ab, RREGS); if (fl & 0x80) rset_a(d, ab.b, RREGS); }
        d = ocw & 0xF;
        if (d != 0) { rset_rgb(d, cd, RREGS); if (fl & 0x40) rset_a(d, cd.b, RREGS); }
        d = (ocw >> 8) & 0xF;
        if (d != 0) rset_rgb(d, ms, RREGS);
        d = (ao >> 4) & 0xF;
        if (d != 0) rset_a(d, aab, RREGS);
        d = ao & 0xF;
        if (d != 0) rset_a(d, acd, RREGS);
        d = (ao >> 8) & 0xF;
        if (d != 0) rset_a(d, ams, RREGS);
    }
    float4 o;
    if (fin.x != 0 || fin.y != 0) {
        uint f0 = fin.x, f1 = fin.y, ff = f1 & 0xFF;
        float3 a, b, E, F, A, B, C, D;
        R1 = unpack(fin.z); R2 = unpack(fin.w);
        a = (ff & 0x40) ? 1.0 - R5.rgb : R5.rgb;
        b = (ff & 0x20) ? 1.0 - R12.rgb : R12.rgb;
        R14 = float4((ff & 0x80) ? saturate(a + b) : a + b, 0.0);
        E = read_rgb(rget((f1 >> 24) & 0xF, RREGS), f1 >> 24);
        F = read_rgb(rget((f1 >> 16) & 0xF, RREGS), f1 >> 16);
        R15 = float4(E * F, 0.0);
        A = read_rgb(rget((f0 >> 24) & 0xF, RREGS), f0 >> 24);
        B = read_rgb(rget((f0 >> 16) & 0xF, RREGS), f0 >> 16);
        C = read_rgb(rget((f0 >> 8) & 0xF, RREGS), f0 >> 8);
        D = read_rgb(rget(f0 & 0xF, RREGS), f0);
        o.rgb = D + C * (1.0 - A) + B * A;
        o.a = read_alpha(rget((f1 >> 8) & 0xF, RREGS), f1 >> 8);
    } else {
        o = R12;
    }
    return saturate(o);
}
/* alpha_test_pass: the value as the software path rounds it. */
bool alpha_ok(float a) {
    int v = (int)(saturate(a) * 255.0 + 0.5), r = (int)(at.y & 0xFF);
    uint f = at.x;
    if (f == 0x200) return false;
    if (f == 0x201) return v < r;
    if (f == 0x202) return v == r;
    if (f == 0x203) return v <= r;
    if (f == 0x204) return v > r;
    if (f == 0x205) return v != r;
    if (f == 0x206) return v >= r;
    return true;
}
/* One source, one shader per path: the CPU picks it (pick_ps), so the GPU
 * does not carry the other paths' code and registers. Bodies as ps_main's
 * branches were, word for word. */
float4 ps_screen(PSIn i) : SV_Target { /* screen space: flat, the texel replaces it */
    uint fl = ctl.w;
    float4 col = i.fc;
    if ((fl & 2) && (tex[0].z & 8)) {
        float2 uv = max(i.t0.xy, 0.0);
        if ((tex[0].z & 64) && !(tex[0].z & 32))
            col = rt_texel(0, uv.x, uv.y, 0, false);
        else
            col = texel_at(0, (int)uv.x, (int)uv.y, 0);
    }
    return col;
}
/* Past the far end of the depth range. The software path compares z as it
 * is, so with LESS / LEQUAL / EQUAL such a pixel fails against anything the
 * buffer can hold; D3D clamps it to 1.0 first, where LEQUAL against a
 * cleared buffer passes. Near-plane-clipped triangles reach far beyond.
 * Then SET_CLIP_MIN/MAX, as the executor's own rasteriser applies them. */
void ps_zdiscard(PSIn i, uint fl) {
    if ((fl & 8) && i.zn > 1.0000005) discard;
    if ((fl & 64) && (i.zw < zr.x || i.zw > zr.y)) discard;
}
void ps_atest(float4 col, uint fl) {
    if ((fl & 4) && !alpha_ok(col.a)) discard;
}
float4 ps_ffp(PSIn i) : SV_Target { /* stage 0 times diffuse */
    uint fl = ctl.w;
    float4 col;
    ps_zdiscard(i, fl);
    col = i.d0;
    if ((fl & 2) && (tex[0].z & 8)) {
        float2 uv = i.t0.xy;
        if (tex[0].z & 1) uv *= float2(tex[0].x & 0xFFFF, tex[0].x >> 16);
        if ((tex[0].z & 64) && !(tex[0].z & 32))
            col *= rt_texel(0, uv.x, uv.y, 0, false);
        else
            col *= texel_at(0, (int)floor(uv.x), (int)floor(uv.y), 0);
    }
    col = saturate(col);
    ps_atest(col, fl);
    return col;
}
float4 ps_rc(PSIn i) : SV_Target { /* register combiners */
    uint fl = ctl.w;
    float4 col, t0, t1, t2, t3;
    ps_zdiscard(i, fl);
    if (!stage_fetch(0, i.t0, t0)) discard;
    if (!stage_fetch(1, i.t1, t1)) discard;
    if (!stage_fetch(2, i.t2, t2)) discard;
    if (!stage_fetch(3, i.t3, t3)) discard;
    col = rc_eval(i.d0, i.d1, float4(fogc.rgb, i.fog), t0, t1, t2, t3);
    ps_atest(col, fl);
    return col;
}

/* ---- presenting and the internal scale (nv2a_gpu_d3d11.c "presenting from
 * the GPU" and "rows up, blocks down"); compiled at build time too, so a
 * title's run compiles nothing but diagnostics (Proton's d3dcompiler_47 is
 * Wine's, on vkd3d-shader). Both read T0. */

/* The back buffer is the window's client area; the 640x480 frame is drawn
 * into the rectangle xbox_FramebufferWindowFit gives (the display aspect,
 * bars around it), bilinear between the target's texels -- exactly the
 * frame's pixels when the rectangle is the target's size. pvp: the rectangle;
 * pfs: the frame in texels of what is drawn: the target, or s_ss under SSAA
 * (640x480 times a 2x-wide anti-aliased surface's 2, aa_sy and the scale). */
cbuffer PRESENTC : register(b2) { float4 ppk; float4 pvp; float4 pfs; };
float4 pvs(uint i : SV_VertexID) : SV_Position {
    float2 q = float2((i << 1) & 2, i & 2);
    return float4(q * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 ptap(int2 c) {
    c = clamp(c, int2(0, 0), int2(pfs.xy) - 1);
    return T0.Load(int4(c, 0, 0)).rgb;
}
float4 pps(float4 p : SV_Position) : SV_Target {
    float2 s = (p.xy - pvp.xy) / pvp.zw * pfs.xy - 0.5;
    int2 i = int2(floor(s));
    float2 f = s - floor(s);
    float3 a = lerp(ptap(i), ptap(i + int2(1, 0)), f.x);
    float3 b = lerp(ptap(i + int2(0, 1)), ptap(i + int2(1, 1)), f.x);
    return float4(lerp(a, b, f.y), 1.0);
}

/* bps_up: a native texel drawn over its s_sx x s_sy block of the target.
 * bps_down: the block averaged back to one texel -- the anti-aliased colour
 * guest RAM gets; also the SSAA reduction for the presenter. bbk: the block. */
cbuffer BLITC : register(b3) { float4 bbk; };
float4 bvs(uint i : SV_VertexID) : SV_Position {
    float2 q = float2((i << 1) & 2, i & 2);
    return float4(q * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 bps_up(float4 p : SV_Position) : SV_Target {
    return T0.Load(int4(int2(p.xy) / int2(bbk.xy), 0, 0));
}
float4 bps_down(float4 p : SV_Position) : SV_Target {
    int2 s = int2(bbk.xy), b = int2(p.xy) * s;
    float4 a = float4(0.0, 0.0, 0.0, 0.0);
    int x, y;
    [loop] for (y = 0; y < s.y; y++)
        [loop] for (x = 0; x < s.x; x++)
            a += T0.Load(int4(b + int2(x, y), 0, 0));
    return a / (float)(s.x * s.y);
}
