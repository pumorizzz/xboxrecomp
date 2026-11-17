/*
 * nv2a_gpu_prof.c -- RECOMP_GPU_PROF: a timestamp profile of the Direct3D 11
 * back end. See nv2a_gpu_prof.h for what it reports and how it is switched on.
 *
 * Timing. A frame brackets its work in a TIMESTAMP_DISJOINT query; every
 * segment starts with a TIMESTAMP and the frame ends with one more, so a
 * segment lasts from its timestamp to the next and the segments add up to the
 * frame's span by construction. Where one segment ends and the next begins is
 * as exact as the GPU orders timestamps against the work around them: a
 * short segment after a long one may take some of its time. With stats on,
 * the pipeline-statistics query of one segment ends before the next one's
 * begins, which can serialise the pipeline; times without stats are the
 * honest ones.
 *
 * Reading back. A closed frame waits in a ring of GP_FRAMES; at each flip the
 * waiting frames are read oldest first with DONOTFLUSH and the first one not
 * ready stops the read until the next flip. The profile submits each frame's
 * closing queries itself (a Flush at the flip): left in the command buffer,
 * they ran only when the GPU next got work, and at a frame limit the GPU's
 * idle time went into the frame's last segment. Nothing here ever waits for the GPU: with every slot still out, a
 * frame is not profiled (dropped). A frame whose DISJOINT query says the
 * clock jumped (Disjoint, or a Frequency of 0) is thrown away (disjoint). A
 * frame whose query cannot be read -- GetData fails, as it does at once
 * (DXGI_ERROR_INVALID_CALL on WARP) for a query that was never ended -- is
 * thrown away too, but counted apart (read
 * errors): the driver's disjoint is the GPU's clock, a read error is the
 * profile's own fault, and folding one into the other hid a frame that never
 * ended its queries. A query that stays S_FALSE is not an error: its frame
 * keeps waiting (and blocks the ring until it is read).
 *
 * Folding. Per period (<secs>), per key (kind, target) -- at most GP_KEYS,
 * the rest folded into (kind, 0) -- the ticks, segments and counters; per
 * frame the span for mean / p50 / p95 / max, and the period from one frame's
 * end to the next's on the GPU. The report goes out as one block (TextBlock):
 * stderr is unbuffered in fbos and a line at a time would stall this thread.
 */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/text_block.h"
#include "nv2a_gpu_prof.h"

#define GP_FRAMES 8
#define GP_SEGS 256
#define GP_KEYS 64
#define GP_SAMPLES 4096                 /* frame spans and flip periods a period keeps */
#define GP_TARGETS 32
#define GP_TOP_TARGETS 12

int g_nv2a_gprof_on;

typedef struct {
    int kind;
    uint32_t va;
    ID3D11Query *ts;                    /* written where the segment begins */
    ID3D11Query *st;                    /* pipeline statistics, with stats only */
    uint64_t cnt[GPC_N];
    uint64_t t;                         /* read back */
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stat;
} GpSeg;

typedef struct {
    int nseg;
    uint32_t flip;                      /* nv2a_gprof_frame calls when it closed */
    ID3D11Query *dj, *ts_end;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT djd;
    uint64_t t_end;
    GpSeg seg[GP_SEGS];
} GpFrame;

typedef struct {
    int kind;
    uint32_t va;
    uint64_t segs;
    int64_t ticks;
    double ms;
    uint64_t cnt[GPC_N];
    uint64_t ps_inv, c_inv, c_prim;
} GpKey;

typedef struct { uint32_t va, w, h; int swz; } GpTarget;

static const char *const s_kind_name[GP_KINDS] = {
    "clear.color", "clear.zs", "z.init", "draw.screen", "draw.ffp", "draw.rc",
    "resolve", "upload", "writeback", "tex.upload", "ssaa", "present", "swap"
};

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static uint32_t s_sx = 1, s_sy = 1, s_msaa = 1;
static int s_stats, s_flatps;
static double s_secs;
static double s_qpc_ms;
static int64_t s_qpc_init, s_qpc_period, s_qpc_flip;

static GpFrame s_ring[GP_FRAMES];
static int s_tail, s_nwait;             /* waiting: s_tail .. s_tail + s_nwait - 1 */
static GpFrame *s_open;                 /* the frame being recorded */
static int s_skip;                      /* this frame is not profiled */
static uint32_t s_flips;
static int s_queries_made, s_stat_queries;

static GpTarget s_tgt[GP_TARGETS];
static int s_ntgt, s_tgt_next;

/* The GPU's previous frame end, for the end-to-end period. */
static int s_have_prev;
static uint64_t s_prev_end, s_prev_freq;
static uint32_t s_prev_flip;

/* The period being folded. */
static GpKey s_key[GP_KEYS];
static int s_nkey;
static struct {
    uint32_t frames, dropped, disjoint, read_errors, overflow;
    uint32_t last_error;                /* the HRESULT of the last failed GetData */
    int64_t span_ticks, seg_ticks, min_seg;
    int have_min;
    double span_ms[GP_SAMPLES];
    int nspan;
    double span_sum;
    double e2e_sum;
    uint32_t e2e_n;
    double host[GPH_N];
    uint32_t present_n;
    uint32_t host_flips;
    double flip_ms[GP_SAMPLES];
    int nflip;
    double flip_sum;
} s_p;

static int64_t qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (int64_t)t.QuadPart;
}

#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* A closed frame's queries go to the GPU at once (see "Reading back" above).
 * Tests count the submits: WARP runs queued commands without a Flush, so a
 * read with DONOTFLUSH there cannot tell whether the profile submitted. */
#ifdef NV2A_GPU_TEST_HOOKS
static uint32_t s_test_submits;
#define TEST_SUBMITTED() (s_test_submits++)
#else
#define TEST_SUBMITTED() ((void)0)
#endif
#define FRAME_SUBMIT() do { ID3D11DeviceContext_Flush(s_ctx); TEST_SUBMITTED(); } while (0)

/* RECOMP_GPU_PROF=<secs>[,stats][,flatps]: 0 when off. */
static double parse_env(int *stats, int *flatps)
{
    const char *e = getenv("RECOMP_GPU_PROF");
    char *end = NULL;
    double secs;

    *stats = *flatps = 0;
    if (!e || !*e)
        return 0.0;
    secs = strtod(e, &end);
    if (!(secs > 0.0))
        return 0.0;
    if (secs < 0.5)
        secs = 0.5;
    while (end && *end) {
        const char *tok;
        size_t n;
        while (*end == ',' || *end == ' ')
            end++;
        tok = end;
        while (*end && *end != ',')
            end++;
        n = (size_t)(end - tok);
        if (n == 5 && strncmp(tok, "stats", 5) == 0)
            *stats = 1;
        else if (n == 6 && strncmp(tok, "flatps", 6) == 0)
            *flatps = 1;
    }
    return secs;
}

int nv2a_gprof_flatps_requested(void)
{
    int st, fl;
    return parse_env(&st, &fl) > 0.0 && fl;
}

static void period_reset(void)
{
    memset(&s_p, 0, sizeof s_p);
    memset(s_key, 0, sizeof s_key);
    s_nkey = 0;
    s_qpc_period = qpc();
}

void nv2a_gprof_init(struct ID3D11Device *dev, struct ID3D11DeviceContext *ctx,
                     uint32_t sx, uint32_t sy, uint32_t msaa)
{
    LARGE_INTEGER f;

    s_secs = parse_env(&s_stats, &s_flatps);
    if (s_secs <= 0.0)
        return;
    s_dev = (ID3D11Device *)dev;
    s_ctx = (ID3D11DeviceContext *)ctx;
    s_sx = sx; s_sy = sy; s_msaa = msaa;
    QueryPerformanceFrequency(&f);
    s_qpc_ms = 1000.0 / (double)f.QuadPart;
    s_qpc_init = qpc();
    s_qpc_flip = 0;
    s_open = NULL;
    s_skip = 0;
    s_tail = s_nwait = 0;
    s_have_prev = 0;
    period_reset();
    g_nv2a_gprof_on = 1;
    fprintf(stderr, "[GPUPROF] on: report every %.1f s, stats %s, scale %ux%u, MSAA %u%s\n",
            s_secs, s_stats ? "on" : "off", sx, sy, msaa,
            s_flatps ? ", ABLATION flatps (picture is wrong)" : "");
}

void nv2a_gprof_target(uint32_t va, uint32_t w, uint32_t h, int swizzled)
{
    int i;
    GpTarget *t = NULL;

    for (i = 0; i < s_ntgt; i++)
        if (s_tgt[i].va == va) { t = &s_tgt[i]; break; }
    if (!t) {
        if (s_ntgt < GP_TARGETS) {
            t = &s_tgt[s_ntgt++];
        } else {
            t = &s_tgt[s_tgt_next];
            s_tgt_next = (s_tgt_next + 1) % GP_TARGETS;
        }
    }
    t->va = va; t->w = w; t->h = h; t->swz = swizzled != 0;
}

static ID3D11Query *make_query(D3D11_QUERY type)
{
    D3D11_QUERY_DESC d;
    ID3D11Query *q = NULL;

    d.Query = type;
    d.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateQuery(s_dev, &d, &q)))
        return NULL;
    s_queries_made++;
    if (type == D3D11_QUERY_PIPELINE_STATISTICS)
        s_stat_queries++;
    return q;
}

/* A query could not be made: the profile goes off rather than lie. */
static void give_up(void)
{
    fprintf(stderr, "[GPUPROF] a query could not be made; the profile is off\n");
    g_nv2a_gprof_on = 0;
}

static int seg_begin(GpFrame *f, int kind, uint32_t va)
{
    GpSeg *s = &f->seg[f->nseg];

    if (!s->ts && !(s->ts = make_query(D3D11_QUERY_TIMESTAMP)))
        return 0;
    if (s_stats && !s->st && !(s->st = make_query(D3D11_QUERY_PIPELINE_STATISTICS)))
        return 0;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s->ts);
    if (s_stats)
        ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)s->st);
    s->kind = kind;
    s->va = va;
    memset(s->cnt, 0, sizeof s->cnt);
    f->nseg++;
    return 1;
}

static void gprof_poll(int wait);

void nv2a_gprof_mark(int kind, uint32_t va)
{
    GpFrame *f = s_open;
    GpSeg *cur;

    if (s_skip)
        return;
    if (!f) {                           /* the frame's first GPU work */
        if (s_nwait == GP_FRAMES) {
            gprof_poll(0);
            if (s_nwait == GP_FRAMES) {
                s_skip = 1;
                s_p.dropped++;
                return;
            }
        }
        f = &s_ring[(s_tail + s_nwait) % GP_FRAMES];
        if ((!f->dj && !(f->dj = make_query(D3D11_QUERY_TIMESTAMP_DISJOINT)))
            || (!f->ts_end && !(f->ts_end = make_query(D3D11_QUERY_TIMESTAMP)))) {
            give_up();
            return;
        }
        f->nseg = 0;
        ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)f->dj);
        s_open = f;
        if (!seg_begin(f, kind, va))
            give_up();
        return;
    }
    cur = &f->seg[f->nseg - 1];
    if (cur->kind == kind && cur->va == va)
        return;
    if (f->nseg == GP_SEGS) {           /* full: the rest joins the last segment */
        s_p.overflow++;
        return;
    }
    if (s_stats)
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)cur->st);
    if (!seg_begin(f, kind, va))
        give_up();
}

void nv2a_gprof_count(int c, uint64_t n)
{
    if (s_open && !s_skip && s_open->nseg > 0)
        s_open->seg[s_open->nseg - 1].cnt[c] += n;
}

void nv2a_gprof_host(int h, double v)
{
    s_p.host[h] += v;
    if (h == GPH_PRESENT_MS)
        s_p.present_n++;
}

/* 1 read, 0 not yet, -1 failed. wait 0: one look, as at a flip; 1: polls up
 * to a bound, letting GetData flush (tests); 2: polls up to a bound with
 * DONOTFLUSH, so only what was already submitted can finish (tests). */
static int get_data(ID3D11Query *q, void *out, UINT size, int wait)
{
    long n = wait ? 1000000L : 1L;
    HRESULT hr;

    do {
        hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)q, out, size,
                                         wait == 1 ? 0u : (UINT)D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK)
            return 1;
        if (hr != S_FALSE) {
            s_p.last_error = (uint32_t)hr;
            return -1;
        }
    } while (--n > 0);
    return 0;
}

/* Every result of a waiting frame; 0 when one is not ready yet. *bad when
 * one failed: the frame is thrown away. */
static int frame_read(GpFrame *f, int wait, int *bad)
{
    int i, r;

    *bad = 0;
    r = get_data(f->dj, &f->djd, sizeof f->djd, wait);
    if (r <= 0) {
        *bad = r < 0;
        return r < 0;
    }
    for (i = 0; i < f->nseg; i++) {
        r = get_data(f->seg[i].ts, &f->seg[i].t, sizeof f->seg[i].t, wait);
        if (r <= 0) {
            *bad = r < 0;
            return r < 0;
        }
    }
    r = get_data(f->ts_end, &f->t_end, sizeof f->t_end, wait);
    if (r <= 0) {
        *bad = r < 0;
        return r < 0;
    }
    if (s_stats)
        for (i = 0; i < f->nseg; i++) {
            r = get_data(f->seg[i].st, &f->seg[i].stat, sizeof f->seg[i].stat, wait);
            if (r <= 0) {
                *bad = r < 0;
                return r < 0;
            }
        }
    return 1;
}

static GpKey *find_key(int kind, uint32_t va)
{
    int i;

    for (i = 0; i < s_nkey; i++)
        if (s_key[i].kind == kind && s_key[i].va == va)
            return &s_key[i];
    /* (kind, 0) always has room: a target key only while GP_KINDS are left. */
    if (va && s_nkey >= GP_KEYS - GP_KINDS)
        return find_key(kind, 0);
    if (s_nkey >= GP_KEYS)
        return &s_key[GP_KEYS - 1];
    s_key[s_nkey].kind = kind;
    s_key[s_nkey].va = va;
    return &s_key[s_nkey++];
}

static void frame_fold(GpFrame *f)
{
    uint64_t freq = f->djd.Frequency;
    int64_t span, sum = 0;
    double ms;
    int i, c;

    if (f->djd.Disjoint || !freq || f->nseg <= 0) {
        s_p.disjoint++;
        s_have_prev = 0;
        return;
    }
    s_p.frames++;
    for (i = 0; i < f->nseg; i++) {
        GpSeg *s = &f->seg[i];
        int64_t d = (int64_t)((i + 1 < f->nseg ? f->seg[i + 1].t : f->t_end) - s->t);
        GpKey *k = find_key(s->kind, s->va);
        k->segs++;
        k->ticks += d;
        k->ms += (double)d * 1000.0 / (double)freq;
        for (c = 0; c < GPC_N; c++)
            k->cnt[c] += s->cnt[c];
        if (s_stats) {
            k->ps_inv += s->stat.PSInvocations;
            k->c_inv += s->stat.CInvocations;
            k->c_prim += s->stat.CPrimitives;
        }
        sum += d;
        if (!s_p.have_min || d < s_p.min_seg) {
            s_p.min_seg = d;
            s_p.have_min = 1;
        }
    }
    span = (int64_t)(f->t_end - f->seg[0].t);
    s_p.span_ticks += span;
    s_p.seg_ticks += sum;
    ms = (double)span * 1000.0 / (double)freq;
    s_p.span_sum += ms;
    if (s_p.nspan < GP_SAMPLES)
        s_p.span_ms[s_p.nspan++] = ms;
    if (s_have_prev && f->flip == s_prev_flip + 1 && freq == s_prev_freq) {
        s_p.e2e_sum += (double)(int64_t)(f->t_end - s_prev_end) * 1000.0 / (double)freq;
        s_p.e2e_n++;
    }
    s_have_prev = 1;
    s_prev_end = f->t_end;
    s_prev_freq = freq;
    s_prev_flip = f->flip;
}

static void gprof_poll(int wait)
{
    while (s_nwait > 0) {
        GpFrame *f = &s_ring[s_tail];
        int bad;
        if (!frame_read(f, wait, &bad))
            break;
        if (bad) {
            s_p.read_errors++;
            s_have_prev = 0;
        } else {
            frame_fold(f);
        }
        s_tail = (s_tail + 1) % GP_FRAMES;
        s_nwait--;
    }
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* p in [0,1] of n samples (sorts a copy). */
static double pct(const double *v, int n, double p)
{
    static double tmp[GP_SAMPLES];
    int i;

    if (n <= 0)
        return 0.0;
    memcpy(tmp, v, (size_t)n * sizeof *v);
    qsort(tmp, (size_t)n, sizeof *tmp, cmp_double);
    i = (int)(p * (double)(n - 1) + 0.5);
    return tmp[i];
}

/* Appends to a line buffer. */
static void cat(char *b, size_t cap, const char *fmt, ...)
{
    size_t n = strlen(b);
    va_list ap;

    if (n + 1 >= cap)
        return;
    va_start(ap, fmt);
    vsnprintf(b + n, cap - n, fmt, ap);
    va_end(ap);
}

typedef struct {
    uint32_t va;
    double ms, kms[GP_KINDS];
    uint64_t px, ps_inv, zuse;
} GpTgtSum;

static int cmp_tgt(const void *a, const void *b)
{
    double x = ((const GpTgtSum *)a)->ms, y = ((const GpTgtSum *)b)->ms;
    return x > y ? -1 : x < y;
}

static void gprof_report(FILE *out)
{
    static char buf[16384];
    static GpTgtSum tg[GP_KEYS];
    TextBlock tb;
    char line[TEXT_BLOCK_LINE - 16];
    double fr = s_p.frames ? (double)s_p.frames : 1.0;
    double hf = s_p.host_flips ? (double)s_p.host_flips : 1.0;
    double span_mean = s_p.frames ? s_p.span_sum / fr : 0.0;
    double kms[GP_KINDS];
    uint64_t kseg[GP_KINDS], kcnt[GP_KINDS][GPC_N], kinv[GP_KINDS];
    uint64_t tot[GPC_N], inv = 0, tris;
    int i, k, c, ntg = 0;

    memset(kms, 0, sizeof kms);
    memset(kseg, 0, sizeof kseg);
    memset(kcnt, 0, sizeof kcnt);
    memset(kinv, 0, sizeof kinv);
    memset(tot, 0, sizeof tot);
    for (i = 0; i < s_nkey; i++) {
        const GpKey *e = &s_key[i];
        kms[e->kind] += e->ms;
        kseg[e->kind] += e->segs;
        kinv[e->kind] += e->ps_inv;
        inv += e->ps_inv;
        for (c = 0; c < GPC_N; c++) {
            kcnt[e->kind][c] += e->cnt[c];
            tot[c] += e->cnt[c];
        }
    }

    text_block_init(&tb, buf, sizeof buf, out);
    text_block_printf(&tb, "[GPUPROF] t %.1f s, %.1f s: %u frames read (%u dropped, %u disjoint,"
                           " %u read errors, %u overflow), scale %ux%u MSAA %u\n",
                      (double)(qpc() - s_qpc_init) * s_qpc_ms / 1000.0,
                      (double)(qpc() - s_qpc_period) * s_qpc_ms / 1000.0,
                      s_p.frames, s_p.dropped, s_p.disjoint, s_p.read_errors, s_p.overflow,
                      s_sx, s_sy, s_msaa);
    if (s_p.read_errors)
        text_block_printf(&tb, "[GPUPROF]   read errors: last GetData 0x%08X\n", s_p.last_error);
    if (s_flatps)
        text_block_printf(&tb, "[GPUPROF]   ABLATION flatps: picture is wrong\n");
    text_block_printf(&tb, "[GPUPROF]   host: flip period mean %.2f p50 %.2f p95 %.2f ms,"
                           " Present() mean %.2f ms, map stall %.2f ms/frame,"
                           " report waits %.1f/frame %.2f ms/frame\n",
                      s_p.nflip ? s_p.flip_sum / (double)s_p.nflip : 0.0,
                      pct(s_p.flip_ms, s_p.nflip, 0.5), pct(s_p.flip_ms, s_p.nflip, 0.95),
                      s_p.present_n ? s_p.host[GPH_PRESENT_MS] / (double)s_p.present_n : 0.0,
                      s_p.host[GPH_MAP_STALL_MS] / hf, s_p.host[GPH_REPORTS] / hf,
                      s_p.host[GPH_REPORT_WAIT_MS] / hf);
    line[0] = 0;
    cat(line, sizeof line, "[GPUPROF]   gpu span mean %.2f p50 %.2f p95 %.2f max %.2f ms;"
                           " end-to-end period mean %.2f ms",
        span_mean, pct(s_p.span_ms, s_p.nspan, 0.5), pct(s_p.span_ms, s_p.nspan, 0.95),
        pct(s_p.span_ms, s_p.nspan, 1.0),
        s_p.e2e_n ? s_p.e2e_sum / (double)s_p.e2e_n : 0.0);
    if (s_stats)
        cat(line, sizeof line, "; PS inv %.2f M/frame", (double)inv / 1e6 / fr);
    text_block_printf(&tb, "%s\n", line);

    for (k = 0; k < GP_KINDS; k++) {
        const uint64_t *n = kcnt[k];
        if (!kseg[k])
            continue;
        line[0] = 0;
        cat(line, sizeof line, "[GPUPROF]   %-11s %7.3f ms/frame %5.1f%% span, segs %.1f",
            s_kind_name[k], kms[k] / fr, span_mean > 0.0 ? 100.0 * kms[k] / fr / span_mean : 0.0,
            (double)kseg[k] / fr);
        if (n[GPC_DRAWS])
            cat(line, sizeof line, ", draws %.1f, tris %.0f, est %.2f Mpx",
                (double)n[GPC_DRAWS] / fr, (double)n[GPC_TRIS] / fr, (double)n[GPC_PX] / 1e6 / fr);
        if (n[GPC_CLEARS])
            cat(line, sizeof line, ", count %.1f, %.2f Mpx",
                (double)n[GPC_CLEARS] / fr, (double)n[GPC_CLEAR_PX] / 1e6 / fr);
        if (n[GPC_RESOLVES])
            cat(line, sizeof line, ", count %.1f, %.1f MB",
                (double)n[GPC_RESOLVES] / fr, (double)n[GPC_RESOLVE_BYTES] / 1048576.0 / fr);
        if (n[GPC_UPLOAD_BYTES])
            cat(line, sizeof line, ", %.2f MB up", (double)n[GPC_UPLOAD_BYTES] / 1048576.0 / fr);
        if (n[GPC_WB_BYTES])
            cat(line, sizeof line, ", %.2f MB back", (double)n[GPC_WB_BYTES] / 1048576.0 / fr);
        if (n[GPC_TEX_BYTES])
            cat(line, sizeof line, ", %.2f MB tex", (double)n[GPC_TEX_BYTES] / 1048576.0 / fr);
        if (s_stats && kinv[k])
            cat(line, sizeof line, ", PS %.3f Minv/frame, %.1f ns per 1000 PS inv",
                (double)kinv[k] / 1e6 / fr, kms[k] * 1e9 / (double)kinv[k]);
        text_block_printf(&tb, "%s\n", line);
    }

    /* By target: the keys with an address, heaviest first. */
    for (i = 0; i < s_nkey; i++) {
        const GpKey *e = &s_key[i];
        int j;
        if (!e->va)
            continue;
        for (j = 0; j < ntg; j++)
            if (tg[j].va == e->va)
                break;
        if (j == ntg) {
            memset(&tg[ntg], 0, sizeof tg[ntg]);
            tg[ntg++].va = e->va;
        }
        tg[j].ms += e->ms;
        tg[j].kms[e->kind] += e->ms;
        tg[j].px += e->cnt[GPC_PX] + e->cnt[GPC_CLEAR_PX];
        tg[j].ps_inv += e->ps_inv;
        tg[j].zuse += e->cnt[GPC_F_ZUSE];
    }
    qsort(tg, (size_t)ntg, sizeof tg[0], cmp_tgt);
    for (i = 0; i < ntg && i < GP_TOP_TARGETS; i++) {
        uint32_t w = 0, h = 0;
        int swz = 0, j, first = 1;
        for (j = 0; j < s_ntgt; j++)
            if (s_tgt[j].va == tg[i].va) {
                w = s_tgt[j].w; h = s_tgt[j].h; swz = s_tgt[j].swz;
                break;
            }
        line[0] = 0;
        cat(line, sizeof line, "[GPUPROF]   target 0x%08X %ux%u%s: %.2f ms (",
            tg[i].va, w, h, swz ? " swz" : "", tg[i].ms / fr);
        for (k = 0; k < GP_KINDS; k++)
            if (tg[i].kms[k] != 0.0) {
                cat(line, sizeof line, "%s%s %.2f", first ? "" : ", ", s_kind_name[k],
                    tg[i].kms[k] / fr);
                first = 0;
            }
        cat(line, sizeof line, "), est %.2f Mpx, zuse %.1f/frame", (double)tg[i].px / 1e6 / fr,
            (double)tg[i].zuse / fr);
        if (s_stats)
            cat(line, sizeof line, ", PS %.3f Minv", (double)tg[i].ps_inv / 1e6 / fr);
        text_block_printf(&tb, "%s\n", line);
    }

    tris = tot[GPC_TRIS];
    if (tris) {
        double t = (double)tris / 100.0;
        text_block_printf(&tb, "[GPUPROF]   draw flags, %% of %.0f tris/frame: zfar %.1f, zclip %.1f,"
                               " alpha test %.1f, clip planes %.1f, blend %.1f, depth write %.1f,"
                               " cull_enable %.1f\n",
                          (double)tris / fr, (double)tot[GPC_F_ZFAR] / t,
                          (double)tot[GPC_F_ZCLIP] / t, (double)tot[GPC_F_ATEST] / t,
                          (double)tot[GPC_F_CLIPPLANE] / t, (double)tot[GPC_F_BLEND] / t,
                          (double)tot[GPC_F_ZWRITE] / t, (double)tot[GPC_F_CULL] / t);
    }
    text_block_flush(&tb);
}

void nv2a_gprof_frame(void)
{
    int64_t now = qpc();

    s_flips++;
    if (s_qpc_flip) {
        double ms = (double)(now - s_qpc_flip) * s_qpc_ms;
        s_p.flip_sum += ms;
        if (s_p.nflip < GP_SAMPLES)
            s_p.flip_ms[s_p.nflip++] = ms;
    }
    s_qpc_flip = now;
    s_p.host_flips++;
    if (s_open) {
        GpFrame *f = s_open;
        if (s_stats)
            ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)f->seg[f->nseg - 1].st);
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)f->ts_end);
        ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)f->dj);
        /* Submitted now, not with the next frame's commands: left in the
         * command buffer, the end timestamp ran when the GPU next got work,
         * and the frame's last segment (swap) took in all the time the GPU
         * then sat idle -- at a frame limit, most of the frame. */
        FRAME_SUBMIT();
        f->flip = s_flips;
        s_nwait++;
        s_open = NULL;
    }
    s_skip = 0;
    gprof_poll(0);
    if ((double)(now - s_qpc_period) * s_qpc_ms >= s_secs * 1000.0) {
        gprof_report(stderr);
        period_reset();
    }
}

void nv2a_gprof_release(void)
{
    int i, j;

    for (i = 0; i < GP_FRAMES; i++) {
        GpFrame *f = &s_ring[i];
        REL(f->dj);
        REL(f->ts_end);
        for (j = 0; j < GP_SEGS; j++) {
            REL(f->seg[j].ts);
            REL(f->seg[j].st);
        }
        f->nseg = 0;
    }
    g_nv2a_gprof_on = 0;
    s_open = NULL;
    s_skip = 0;
    s_tail = s_nwait = 0;
    s_have_prev = 0;
    s_dev = NULL;                       /* the back end releases them next */
    s_ctx = NULL;
}

#ifdef NV2A_GPU_TEST_HOOKS
int nv2a_gpu_test_prof_queries(void)
{
    return s_queries_made;
}

static void test_summary(GprofTestFrame *f)
{
    int i, j, q = 0;

    memset(f, 0, sizeof *f);
    f->frames = s_p.frames;
    f->dropped = s_p.dropped;
    f->disjoint = s_p.disjoint;
    f->read_errors = s_p.read_errors;
    f->last_error = s_p.last_error;
    f->overflow = s_p.overflow;
    f->span_ticks = s_p.span_ticks;
    f->seg_ticks_sum = s_p.seg_ticks;
    f->min_seg_ticks_signed = s_p.have_min ? s_p.min_seg : 0;
    f->stat_queries = (uint32_t)s_stat_queries;
    for (i = 0; i < GP_FRAMES; i++) {
        q += s_ring[i].dj != NULL;
        q += s_ring[i].ts_end != NULL;
        for (j = 0; j < GP_SEGS; j++)
            q += (s_ring[i].seg[j].ts != NULL) + (s_ring[i].seg[j].st != NULL);
    }
    f->ring_queries = (uint32_t)q;
    f->waiting = (uint32_t)s_nwait;
    f->submits = s_test_submits;
}

void nv2a_gpu_test_prof_poll_noflush(GprofTestFrame *f)
{
    if (s_ctx && g_nv2a_gprof_on)
        gprof_poll(2);
    test_summary(f);
}

int nv2a_gpu_test_prof_drain(GprofTestZone *z, int max, GprofTestFrame *f)
{
    int i, n = 0;

    if (s_ctx && g_nv2a_gprof_on) {
        ID3D11DeviceContext_Flush(s_ctx);
        gprof_poll(1);
    }
    for (i = 0; i < s_nkey && n < max; i++, n++) {
        z[n].kind = s_key[i].kind;
        z[n].va = s_key[i].va;
        z[n].segs = s_key[i].segs;
        memcpy(z[n].count, s_key[i].cnt, sizeof z[n].count);
        z[n].ticks = s_key[i].ticks;
        z[n].ps_inv = s_key[i].ps_inv;
    }
    test_summary(f);
    return n;
}

void nv2a_gpu_test_prof_report(FILE *f)
{
    gprof_report(f);
}
#endif
