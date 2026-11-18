/*
 * nv2a_gpu_rc_key.h -- the state of the register combiners that picks a pixel shader.
 *
 * Why. The one combiner shader (ps_rc in nv2a_gpu_shaders.hlsl) runs every
 * stage of every program through switches on all fifteen registers, mapping
 * fields, dot and mux paths and every texture mode. On RDNA (the Steam Deck)
 * the registers are assigned to the whole shader by its worst path, so the
 * occupancy is low and a combiner pixel cost ~45x a flat one. A game uses a
 * handful of programs; a shader compiled for one of them (nv2a_gpu_shaderc
 * --rc-keys, the file the port lists) carries only that program's code.
 *
 * The key is what such a shader is specialised on, and nothing else: the
 * combiner words of the stages in use, the stage program, the final combiner,
 * the control bits that change the code, and a class of each stage's texture
 * (valid, bilinear, swizzled view of a target, render target, cube). Factors,
 * final constants, fog, alpha test, clip range, the size, wrap modes and
 * the normalised/opaque flags are constants of the shader's cbuffer and stay
 * out of it. Words of stages >= n are 0, so two programs that differ only
 * past n have one key.
 *
 * A key's shader draws what the general one draws: bit for bit on WARP, and on
 * NVIDIA within 1 in an 8-bit channel in tens of pixels a frame (the driver's
 * compiler; accepted, see "Pixel shader cost" in docs/technical/pushbuffer-executor.md).
 *
 * Header-only C11, no D3D: the back end, nv2a_gpu_shaderc and the tests all
 * include it, so the three agree on one layout.
 *
 * Text form, one line (what the back end logs and the key file holds):
 *   rc n=2 ctl=00011102 prog=00001 fin=0000000C:00001C80 tex=00000049
 *      s0=C8C40000:D8D41010:000100C0:000100C0 s1=...      (stages < n)
 * A '#' line and an empty line are comments in a key file.
 */
#ifndef NV2A_GPU_RC_KEY_H
#define NV2A_GPU_RC_KEY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tex[st][2] flags of PsConsts (nv2a_gpu_d3d11.c) and the shader's p.z */
#define TXF_NORM   1u
#define TXF_BILIN  2u
#define TXF_CUBE   4u
#define TXF_VALID  8u
#define TXF_OPAQUE 16u                  /* alpha reads 1 (X8R8G8B8) */
#define TXF_SWZ    32u                  /* swizzled view of a linear target: .w is its row */
#define TXF_RT     64u                  /* a render target, stored at the internal scale */

#define NV2A_RC_KEY_WORDS 37
#define NV2A_RC_KEY_MAX_STAGES 8
#define NV2A_RC_KEY_MACROS 39

/* w[0] n | control bits 8/12/16 (mux MSB, constants per stage), w[1] stage
 * program, w[2..3] final combiner, w[4] texture classes (8 bits a stage),
 * w[5..12] cicw, [13..20] aicw, [21..28] cocw, [29..36] aocw. */
static inline void nv2a_rc_key_make(uint32_t w[NV2A_RC_KEY_WORDS],
                                    const uint32_t cicw[8], const uint32_t aicw[8],
                                    const uint32_t cocw[8], const uint32_t aocw[8],
                                    uint32_t control, uint32_t prog,
                                    uint32_t final0, uint32_t final1,
                                    const uint32_t txf[4])
{
    uint32_t n = control & 0xFFu, st, s;

    if (n > NV2A_RC_KEY_MAX_STAGES)
        n = NV2A_RC_KEY_MAX_STAGES;
    memset(w, 0, NV2A_RC_KEY_WORDS * sizeof w[0]);
    w[0] = n | (control & 0x00011100u);
    w[1] = prog & 0xFFFFFu;
    w[2] = final0;
    w[3] = final1;
    for (st = 0; st < 4; st++) {
        uint32_t mode = (prog >> (5 * st)) & 0x1Fu, fl = txf[st], k = 0;
        if (mode >= 1 && mode <= 3 && (fl & TXF_VALID))
            k = fl & (TXF_VALID | TXF_BILIN | TXF_SWZ | TXF_RT | (mode == 3 ? TXF_CUBE : 0u));
        w[4] |= k << (8 * st);
    }
    for (s = 0; s < n; s++) {
        w[5 + s] = cicw[s];
        w[13 + s] = aicw[s];
        w[21 + s] = cocw[s];
        w[29 + s] = aocw[s];
    }
}

/* One line, no newline; the length, or -1 when it did not fit. */
static inline int nv2a_rc_key_format(const uint32_t w[NV2A_RC_KEY_WORDS], char *buf, size_t cap)
{
    size_t len = 0;
    uint32_t n = w[0] & 0xFFu, s;
    int r;

    if (n > NV2A_RC_KEY_MAX_STAGES)
        return -1;
    r = snprintf(buf, cap, "rc n=%u ctl=%08X prog=%05X fin=%08X:%08X tex=%08X",
                 (unsigned)n, (unsigned)w[0], (unsigned)w[1], (unsigned)w[2],
                 (unsigned)w[3], (unsigned)w[4]);
    if (r < 0 || (size_t)r >= cap)
        return -1;
    len = (size_t)r;
    for (s = 0; s < n; s++) {
        r = snprintf(buf + len, cap - len, " s%u=%08X:%08X:%08X:%08X", (unsigned)s,
                     (unsigned)w[5 + s], (unsigned)w[13 + s], (unsigned)w[21 + s],
                     (unsigned)w[29 + s]);
        if (r < 0 || (size_t)r >= cap - len)
            return -1;
        len += (size_t)r;
    }
    return (int)len;
}

/* The literal `lit` at *p (blanks before it skipped), then a number in `base`
 * of at least one digit; 1 and *p after it, or 0. No sscanf: the CRT's
 * deprecation warning would come into every file that includes this. */
static inline int nv2a_rc_key_field(const char **p, const char *lit, int base, uint32_t *out)
{
    const char *s = *p;
    char *e;
    unsigned long v;
    size_t n;

    while (*lit == ' ')
        lit++;
    n = strlen(lit);
    while (*s == ' ' || *s == '\t')
        s++;
    if (strncmp(s, lit, n))
        return 0;
    s += n;
    if (!((*s >= '0' && *s <= '9') || (base == 16 && ((*s >= 'A' && *s <= 'F') || (*s >= 'a' && *s <= 'f')))))
        return 0;
    v = strtoul(s, &e, base);
    if (v > 0xFFFFFFFFul)
        return 0;
    *out = (uint32_t)v;
    *p = e;
    return 1;
}

/* The inverse of nv2a_rc_key_format: 1 for a well-formed line (leading and
 * trailing blanks allowed), else 0. n <= 8, stages s0..s(n-1) in order. */
static inline int nv2a_rc_key_parse(const char *line, uint32_t w[NV2A_RC_KEY_WORDS])
{
    uint32_t n, ctl, prog, f0, f1, tex, s;
    uint32_t t[NV2A_RC_KEY_WORDS];
    const char *p = line;

    if (!nv2a_rc_key_field(&p, "rc n=", 10, &n) || !nv2a_rc_key_field(&p, " ctl=", 16, &ctl)
        || !nv2a_rc_key_field(&p, " prog=", 16, &prog) || !nv2a_rc_key_field(&p, " fin=", 16, &f0)
        || !nv2a_rc_key_field(&p, ":", 16, &f1) || !nv2a_rc_key_field(&p, " tex=", 16, &tex))
        return 0;
    if (n > NV2A_RC_KEY_MAX_STAGES || (ctl & 0xFFu) != n || (ctl & ~0x0001111Fu) != 0
        || (prog & ~0xFFFFFu) != 0)
        return 0;
    memset(t, 0, sizeof t);
    t[0] = ctl;
    t[1] = prog;
    t[2] = f0;
    t[3] = f1;
    t[4] = tex;
    for (s = 0; s < n; s++) {
        uint32_t idx;
        if (!nv2a_rc_key_field(&p, " s", 10, &idx) || idx != s)
            return 0;
        if (!nv2a_rc_key_field(&p, "=", 16, &t[5 + s]) || !nv2a_rc_key_field(&p, ":", 16, &t[13 + s])
            || !nv2a_rc_key_field(&p, ":", 16, &t[21 + s]) || !nv2a_rc_key_field(&p, ":", 16, &t[29 + s]))
            return 0;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p)
        return 0;
    memcpy(w, t, sizeof t);
    return 1;
}

/* FNV-1a over the words' bytes. */
static inline uint32_t nv2a_rc_key_hash(const uint32_t w[NV2A_RC_KEY_WORDS])
{
    uint32_t h = 2166136261u;
    int i, b;

    for (i = 0; i < NV2A_RC_KEY_WORDS; i++)
        for (b = 0; b < 32; b += 8) {
            h ^= (w[i] >> b) & 0xFFu;
            h *= 16777619u;
        }
    return h;
}

/* The key as shader macros (NV2A_RC_KEY_MACROS, or fewer when max is smaller):
 * RCK=1, RCK_N, RCK_CTL, RCK_PROG, RCK_F0, RCK_F1, RCK_TEX, RCK_C0..7 (cicw),
 * RCK_A0..7 (aicw), RCK_O0..7 (cocw), RCK_P0..7 (aocw); values 0x%08Xu.
 * Returns the count. */
static inline int nv2a_rc_key_macros(const uint32_t w[NV2A_RC_KEY_WORDS], char name[][16],
                                     char value[][16], int max)
{
    static const char *const head[7] = { "RCK", "RCK_N", "RCK_CTL", "RCK_PROG", "RCK_F0",
                                         "RCK_F1", "RCK_TEX" };
    static const char grp[4] = { 'C', 'A', 'O', 'P' };
    int n = 0, i, g;

    for (i = 0; i < 7 && n < max; i++, n++) {
        snprintf(name[n], 16, "%s", head[i]);
        if (i == 0)
            snprintf(value[n], 16, "1");
        else
            snprintf(value[n], 16, "0x%08Xu",
                     (unsigned)(i == 1 ? (w[0] & 0xFFu) : w[i - 2]));
    }
    for (g = 0; g < 4; g++)
        for (i = 0; i < 8 && n < max; i++, n++) {
            snprintf(name[n], 16, "RCK_%c%d", grp[g], i);
            snprintf(value[n], 16, "0x%08Xu", (unsigned)w[5 + g * 8 + i]);
        }
    return n;
}

#endif /* NV2A_GPU_RC_KEY_H */
