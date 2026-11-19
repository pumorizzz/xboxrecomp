/*
 * NV2A rasterisation on the host GPU, through Direct3D 11.
 *
 * An opt-in render back end (nv2a_backend.h) for the pushbuffer executor:
 * the executor still decodes the pushbuffer, runs vertex programs and clips
 * on the CPU, and hands this back end triangles in surface pixels; this
 * rasterises them on the GPU -- depth and stencil, blending, the register
 * combiners in a pixel shader -- and writes the colour surfaces back into
 * guest RAM when they are read there (the flip's present, the frame dumps,
 * a texture decoded from guest RAM); a texture that is a live render
 * target is sampled from the target on the GPU.
 *
 * RECOMP_GPU_RASTER: unset, empty or 0 -- off, nothing is registered;
 *   1       -- a hardware adapter (WARP if none can be created);
 *   warp    -- the WARP software adapter;
 *   compare -- the software path draws as usual and the GPU draws alongside
 *              without writing back; each flip prints how far they differ.
 * RECOMP_GPU_RASTER_SYNC=all -- write back at every executor sync (for a
 *   title whose CPU reads what the GPU drew); RECOMP_GPU_RASTER_RTTEX=0 --
 *   read every texture through guest RAM.
 * RECOMP_GPU_RASTER_EPOCH=method -- every executor method that drains the
 *   GPU ends an epoch (the rule before the epochs followed what the title can
 *   see; see nv2a_gpu_d3d11.c);
 *   RECOMP_GPU_RASTER_EPOCH_VERIFY=1 -- a diagnostic: what the epoch rule
 *   says is unchanged in guest RAM is looked at again, and a change is
 *   counted in the `[GPURAST] epochs` line (stale ...) and named once.
 * RECOMP_GPU_RASTER_SCALE=<n>|<x>x<y> -- render at n (1-8) times the surface
 *   each way, the title still sees its own size in guest RAM;
 *   RECOMP_GPU_RASTER_MSAA=<2|4|8> -- samples, lowered to what the device has;
 *   RECOMP_GPU_RASTER_SSAA=<n> -- 1-4: drawn at scale x n, each n x n block
 *   averaged down before the window (scale x n at most 8 a side).
 * RECOMP_GPU_RASTER_PRESENT=0 -- leave the framebuffer window to GDI and
 *   guest RAM instead of a swap chain on it;
 *   RECOMP_GPU_RASTER_PRESENT_DUMP=<prefix> -- every
 *   RECOMP_GPU_RASTER_PRESENT_DUMP_EVERY-th (default 30) shown frame as BMP.
 * RECOMP_GPU_PROF=<secs>[,stats][,flatps] -- a timestamp profile of the GPU
 *   work per frame, by pass and target (nv2a_gpu_prof.c).
 * RECOMP_GPU_RASTER_PSCHECK=<ref.hlsl>[,<every>] -- every <every>-th
 *   (default 30) frame each draw's pixel shader against ps_main of a
 *   reference shader, float and 8-bit differences counted.
 * RECOMP_GPU_RASTER_RC_STATS=1 -- every combiner state (nv2a_gpu_rc_key.h) with
 *   its draws and pixels, in the 10 s report (cumulative, top 32 by pixels);
 *   RECOMP_GPU_RASTER_RC_GENERIC=1 -- only the general combiner shader.
 * RECOMP_FRAME_SKIP=1|auto (the executor's frame skip): a frame the cadence
 *   or, with auto, the swap chain (a flip-model one with a waitable) cannot
 *   show is not drawn -- its work is logged and dropped, or drawn when
 *   something reads it; RECOMP_GPU_RASTER_PRESENT_HZ=<n> acts out a limiter
 *   at n Hz in front of the chain; RECOMP_GPU_RASTER_FRAME_LATENCY=<1-3>
 *   (default 2) the frames the chain may hold (docs: pushbuffer-executor.md
 *   "Frame Skip").
 * A lost device (driver reset) hands the frame back to the software
 * rasteriser at the next flip; RECOMP_GPU_RASTER_LOSE_AT=<flip> acts it out.
 * Needs RECOMP_PB_EXEC=1 to have any effect.
 */
#ifndef NV2A_GPU_D3D11_H
#define NV2A_GPU_D3D11_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create the device and register the back end if RECOMP_GPU_RASTER asks for
 * it. Call on the main thread before the title submits any work; afterwards
 * only the NV2A poll thread touches the device context. Returns 1 when the
 * back end is registered, 0 otherwise (the software path stays). */
int nv2a_gpu_d3d11_install(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_GPU_D3D11_H */
