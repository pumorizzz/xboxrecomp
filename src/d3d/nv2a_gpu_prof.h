/*
 * nv2a_gpu_prof.h -- where the GPU's time per frame goes in the Direct3D 11
 * back end (nv2a_gpu_d3d11.c), by pass and render target.
 *
 * RECOMP_GPU_PROF=<secs>[,stats][,flatps]: every <secs> seconds (at least
 * 0.5) one block of [GPUPROF] lines on stderr. Unset, empty or 0 -- off: the
 * hooks below are one test of g_nv2a_gprof_on and no query is ever made.
 *   stats  -- a pipeline-statistics query around every segment (pixel shader
 *             invocations); it may serialise the GPU, so times read higher;
 *   flatps -- an ablation: every transformed draw runs a flat pixel shader
 *             (the interpolated diffuse), so the picture is wrong; what it
 *             saves is what the pixel shader costs.
 *
 * A segment is a run of GPU commands with one key (kind, target): the back
 * end calls GPROF_MARK before each piece of GPU work, and a timestamp is
 * written where the key changes. A frame is the segments between two flips;
 * its timestamps are read back a few flips later without waiting (eight
 * frames in flight; when all eight are still out a frame goes unprofiled).
 * Counters (GPROF_COUNT) go to the segment open at the time. Host times
 * (GPROF_HOST) are measured on the CPU.
 *
 * Everything but nv2a_gprof_init runs on the NV2A poll thread, as the back
 * end does.
 */
#ifndef NV2A_GPU_PROF_H
#define NV2A_GPU_PROF_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11Device;
struct ID3D11DeviceContext;

/* What a segment is doing. */
enum { GP_CLEAR_COLOR, GP_CLEAR_ZS, GP_Z_INIT, GP_DRAW_SCREEN, GP_DRAW_FFP, GP_DRAW_RC,
       GP_RESOLVE, GP_UPLOAD, GP_WRITEBACK, GP_TEX_UPLOAD, GP_SSAA, GP_PRESENT, GP_SWAP,
       GP_KINDS };

/* Counters per segment. GPC_PX: the draw's bounding box inside the scissor,
 * at the internal scale (an estimate of the pixels shaded). GPC_F_*: the
 * triangles drawn with that state. GPC_F_ZUSE: draws that use the depth
 * buffer (test, write or stencil). */
enum { GPC_DRAWS, GPC_TRIS, GPC_PX, GPC_CLEARS, GPC_CLEAR_PX, GPC_RESOLVES, GPC_RESOLVE_BYTES,
       GPC_UPLOAD_BYTES, GPC_WB_BYTES, GPC_TEX_BYTES,
       GPC_F_ZFAR, GPC_F_ZCLIP, GPC_F_ATEST, GPC_F_CLIPPLANE, GPC_F_BLEND, GPC_F_ZWRITE,
       GPC_F_CULL, GPC_F_ZUSE, GPC_N };

/* Host-side measures: milliseconds, or a count for GPH_REPORTS. */
enum { GPH_PRESENT_MS, GPH_MAP_STALL_MS, GPH_REPORT_WAIT_MS, GPH_REPORTS, GPH_N };

extern int g_nv2a_gprof_on;

#define GPROF_MARK(k, va)  do { if (g_nv2a_gprof_on) nv2a_gprof_mark((k), (va)); } while (0)
#define GPROF_COUNT(c, n)  do { if (g_nv2a_gprof_on) nv2a_gprof_count((c), (n)); } while (0)
#define GPROF_HOST(h, v)   do { if (g_nv2a_gprof_on) nv2a_gprof_host((h), (v)); } while (0)

/* Reads RECOMP_GPU_PROF once; switches the profile on if it asks. sx, sy:
 * the drawn internal scale; msaa: samples. */
void nv2a_gprof_init(struct ID3D11Device *dev, struct ID3D11DeviceContext *ctx,
                     uint32_t sx, uint32_t sy, uint32_t msaa);
/* RECOMP_GPU_PROF asks for the flatps ablation (read before the pipeline is
 * made, so the back end can compile the flat shader). */
int nv2a_gprof_flatps_requested(void);
/* A render target's native size, for the report's table of targets. */
void nv2a_gprof_target(uint32_t va, uint32_t w, uint32_t h, int swizzled);
void nv2a_gprof_mark(int kind, uint32_t va);
void nv2a_gprof_count(int c, uint64_t n);
void nv2a_gprof_host(int h, double v);
void nv2a_gprof_frame(void);      /* at every flip */
void nv2a_gprof_release(void);    /* before the device goes: off, queries freed */

#ifdef NV2A_GPU_TEST_HOOKS
/* tests/nv2a_gpu_backend: what the profile has gathered since init. */
typedef struct {
    int kind;
    uint32_t va;
    uint64_t segs;
    uint64_t count[GPC_N];
    int64_t ticks;
    uint64_t ps_inv;
} GprofTestZone;

typedef struct {
    uint32_t frames, dropped, disjoint, overflow;
    uint32_t read_errors;             /* frames thrown away: a GetData failed */
    uint32_t last_error;              /* its HRESULT */
    int64_t span_ticks, seg_ticks_sum, min_seg_ticks_signed;
    uint32_t stat_queries;            /* pipeline-statistics queries made */
    uint32_t ring_queries;            /* queries the ring holds now */
    uint32_t waiting;                 /* closed frames not read yet */
    uint32_t submits;                 /* frames closed with the profile's own Flush */
} GprofTestFrame;

int nv2a_gpu_test_prof_queries(void);
/* Reads the frames still out as a flip would -- DONOTFLUSH, no Flush of its
 * own -- polling each query a bounded number of times: only what the profile
 * itself submitted can finish. */
void nv2a_gpu_test_prof_poll_noflush(GprofTestFrame *f);
/* Flushes, waits (a bounded number of polls) for every frame still out, folds
 * them in; up to max zones into z; returns the number of zones. */
int nv2a_gpu_test_prof_drain(GprofTestZone *z, int max, GprofTestFrame *f);
void nv2a_gpu_test_prof_report(FILE *f);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NV2A_GPU_PROF_H */
