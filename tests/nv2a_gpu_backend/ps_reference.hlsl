// Frozen copy of s_hlsl (src/d3d/nv2a_gpu_d3d11.c) at toolkit 75bcd91, the golden
// reference for tests/nv2a_gpu_backend (nv2a_gpu_ps_test.c) and
// RECOMP_GPU_RASTER_PSCHECK; never edit.
cbuffer VSC : register(b0) { float4 vsc; };
cbuffer PSC : register(b1) {
    uint4 cicw[2]; uint4 aicw[2]; uint4 cocw[2]; uint4 aocw[2];
    uint4 fac0[2]; uint4 fac1[2];
    uint4 fin;
    uint4 ctl;
    uint4 at;
    float4 fogc;
    uint4 tex[4];
    float4 zr;
    float4 rsc;
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
    noperspective float zn : ZRANGE;
    noperspective float zw : ZWINDOW;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
};
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
int wrapc(int c, int n, uint mode) {
    if (n <= 0) return 0;
    if (mode == 1) { int r = c - (c / n) * n; return r < 0 ? r + n : r; }
    return clamp(c, 0, n - 1);
}
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
    if (p.z & 32) {
        uint i = swz((uint)u, (uint)v, (uint)w, (uint)h);
        int2 s = int2(rsc.xy);
        c = load_st(st, (int)(i % p.w) * s.x + s.x / 2, (int)(i / p.w) * s.y + s.y / 2, face);
    } else {
        c = load_st(st, u + (int)(p.w & 0xFFFF), v + (int)(p.w >> 16), face);
    }
    if (p.z & 16) c.a = 1.0;
    return c;
}
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
float4 rc_texel(uint st, float u, float v, uint face) {
    uint4 p = tex[st];
    if (!(p.z & 8)) return float4(1.0, 1.0, 1.0, 1.0);
    if (p.z & 1) { u *= (float)(p.x & 0xFFFF); v *= (float)(p.x >> 16); }
    if ((p.z & 64) && !(p.z & 32)) return rt_texel(st, u, v, face, (p.z & 2) != 0);
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
}
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
float4 rc_eval(float4 v0, float4 v1, float4 fog, float4 t0, float4 t1,
               float4 t2, float4 t3) {
    float4 R[16];
    uint n = min(ctl.x & 0xFF, 8u), flags = ctl.x >> 8, s, k;
    [unroll] for (k = 0; k < 16; k++) R[k] = float4(0.0, 0.0, 0.0, 0.0);
    R[3] = fog; R[4] = v0; R[5] = v1;
    R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;
    R[12].a = (ctl.y & 0x1F) ? t0.a : 1.0;
    [loop] for (s = 0; s < n; s++) {
        uint icw = W8(cicw, s), ocw = W8(cocw, s);
        uint ai = W8(aicw, s), ao = W8(aocw, s);
        uint fl = ocw >> 12, afl = ao >> 12, d;
        bool mux;
        float3 A, B, C, D, pab, pcd, sum, ab, cd, ms;
        float aA, aB, aC, aD, sab, scd, ssum, aab, acd, ams;
        R[1] = unpack(W8(fac0, (flags & 0x010) ? s : 0));
        R[2] = unpack(W8(fac1, (flags & 0x100) ? s : 0));
        mux = (flags & 1) ? R[12].a >= 0.5 : ((((int)(R[12].a * 255.0)) & 1) != 0);
        A = read_rgb(R[(icw >> 24) & 0xF], icw >> 24);
        B = read_rgb(R[(icw >> 16) & 0xF], icw >> 16);
        C = read_rgb(R[(icw >> 8) & 0xF], icw >> 8);
        D = read_rgb(R[icw & 0xF], icw);
        aA = read_alpha(R[(ai >> 24) & 0xF], ai >> 24);
        aB = read_alpha(R[(ai >> 16) & 0xF], ai >> 16);
        aC = read_alpha(R[(ai >> 8) & 0xF], ai >> 8);
        aD = read_alpha(R[ai & 0xF], ai);
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
        if (d != 0) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.b; }
        d = ocw & 0xF;
        if (d != 0) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.b; }
        d = (ocw >> 8) & 0xF;
        if (d != 0) R[d].rgb = ms;
        d = (ao >> 4) & 0xF;
        if (d != 0) R[d].a = aab;
        d = ao & 0xF;
        if (d != 0) R[d].a = acd;
        d = (ao >> 8) & 0xF;
        if (d != 0) R[d].a = ams;
        R[0] = float4(0.0, 0.0, 0.0, 0.0);
    }
    float4 o;
    if (fin.x != 0 || fin.y != 0) {
        uint f0 = fin.x, f1 = fin.y, ff = f1 & 0xFF;
        float3 a, b, E, F, A, B, C, D;
        R[1] = unpack(fin.z); R[2] = unpack(fin.w);
        a = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;
        b = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;
        R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0.0);
        E = read_rgb(R[(f1 >> 24) & 0xF], f1 >> 24);
        F = read_rgb(R[(f1 >> 16) & 0xF], f1 >> 16);
        R[15] = float4(E * F, 0.0);
        A = read_rgb(R[(f0 >> 24) & 0xF], f0 >> 24);
        B = read_rgb(R[(f0 >> 16) & 0xF], f0 >> 16);
        C = read_rgb(R[(f0 >> 8) & 0xF], f0 >> 8);
        D = read_rgb(R[f0 & 0xF], f0);
        o.rgb = D + C * (1.0 - A) + B * A;
        o.a = read_alpha(R[(f1 >> 8) & 0xF], f1 >> 8);
    } else {
        o = R[12];
    }
    return saturate(o);
}
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
float4 ps_main(PSIn i) : SV_Target {
    uint kind = at.w, fl = ctl.w;
    float4 col;
    if (kind == 0) {
        col = i.fc;
        if ((fl & 2) && (tex[0].z & 8)) {
            float2 uv = max(i.t0.xy, 0.0);
            if ((tex[0].z & 64) && !(tex[0].z & 32))
                col = rt_texel(0, uv.x, uv.y, 0, false);
            else
                col = texel_at(0, (int)uv.x, (int)uv.y, 0);
        }
        return col;
    }
    if ((fl & 8) && i.zn > 1.0000005) discard;
    if ((fl & 64) && (i.zw < zr.x || i.zw > zr.y)) discard;
    if (fl & 1) {
        float4 t0, t1, t2, t3;
        if (!stage_fetch(0, i.t0, t0)) discard;
        if (!stage_fetch(1, i.t1, t1)) discard;
        if (!stage_fetch(2, i.t2, t2)) discard;
        if (!stage_fetch(3, i.t3, t3)) discard;
        col = rc_eval(i.d0, i.d1, float4(fogc.rgb, i.fog), t0, t1, t2, t3);
    } else {
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
    }
    if ((fl & 4) && !alpha_ok(col.a)) discard;
    return col;
}
