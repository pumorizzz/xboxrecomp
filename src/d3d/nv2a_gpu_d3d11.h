/*
 * NV2A rasterisation on the host GPU, through Direct3D 11.
 *
 * An opt-in render back end (nv2a_backend.h) for the pushbuffer executor:
 * the executor still decodes the pushbuffer, runs vertex programs and clips
 * on the CPU, and hands this back end triangles in surface pixels; this
 * rasterises them on the GPU -- depth and stencil, blending, the register
 * combiners in one ubershader -- and writes the colour surfaces back into
 * guest RAM at every point the executor syncs, so the presenter, the frame
 * dumps and render-to-texture keep reading guest RAM as before.
 *
 * RECOMP_GPU_RASTER: unset, empty or 0 -- off, nothing is registered;
 *   1       -- a hardware adapter (WARP if none can be created);
 *   warp    -- the WARP software adapter;
 *   compare -- the software path draws as usual and the GPU draws alongside
 *              without writing back; each flip prints how far they differ.
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
