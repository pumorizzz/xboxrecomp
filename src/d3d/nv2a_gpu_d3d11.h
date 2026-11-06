/*
 * NV2A rasterisation on the host GPU, through Direct3D 11.
 *
 * An opt-in render back end (nv2a_backend.h) for the pushbuffer executor:
 * the executor still decodes the pushbuffer, runs vertex programs and clips
 * on the CPU, and hands this back end triangles in surface pixels; this
 * rasterises them on the GPU -- depth and stencil, blending, the register
 * combiners in one ubershader -- and writes the colour surfaces back into
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
