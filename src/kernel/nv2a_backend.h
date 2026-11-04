/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * Register-combiner state, all four texture stages, the specular colour (d1)
 * and the per-vertex fog factor arrive too: Nv2aBatch.vertices_ex carries every
 * interpolant the executor's own pixel path reads, Nv2aBatch.rc the raw
 * combiner words, and Nv2aBatch.stages the four stage textures. A back end
 * that only needs stage 0 and the diffuse colour keeps reading
 * Nv2aBatch.vertices and Nv2aBatch.texture as before.
 *
 * A back end that draws somewhere other than guest RAM writes its colour
 * surfaces back from sync(): the executor calls it wherever the title, the
 * presenter or a dump may read what was drawn. report() hands over the
 * visibility-test (occlusion) pixel count. shadow asks the executor to keep
 * rasterising in software as well, for A/B comparison.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_combiner.h"

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
    uint32_t clip_x, clip_y;    /* clip rectangle origin, real pixels */
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
    uint32_t palette;           /* guest address of the CLUT (P8, colour 0x0B); 0: none */
    uint32_t cube;              /* 1: six faces, tex_face_stride bytes apart */
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    uint32_t zpass_enable;      /* SET_ZPASS_PIXEL_COUNT_ENABLE: count for report() */
    uint32_t zeta_format;       /* (SURFACE_FORMAT>>4)&0xF: 1 Z16, 2 Z24S8 */
} Nv2aRenderState;

/* Every interpolant of one vertex, 128 bytes. x/y/z/rhw as in Nv2aVertex.
 * d0/d1 are the diffuse and specular colours, r,g,b,a, not clamped. fog is
 * the fog factor already through the fog unit (1 = no fog). tex holds each
 * stage's coordinates as the program or fixed-function unit left them, not
 * divided by w; on the screen-space path tex[0] holds the texel UVs. */
typedef struct {
    float x, y, z, rhw;
    float d0[4], d1[4];
    float fog;
    float pad[3];
    float tex[4][4];
} Nv2aVertexEx;

#define NV2A_BATCH_SCREEN 0     /* pre-transformed: flat colour, texel replaces it */
#define NV2A_BATCH_XF     1     /* program / fixed-function: depth, combiners */

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
    const Nv2aVertexEx *vertices_ex;/* the same triangles, every interpolant */
    uint32_t           kind;        /* NV2A_BATCH_SCREEN / NV2A_BATCH_XF */
    uint32_t           use_rc;      /* the combiners decide the colour */
    uint32_t           textured;    /* stage 0 is sampled (no-combiner paths) */
    const Nv2aCombiner *rc;         /* raw combiner words */
    Nv2aTexture        stages[4];   /* each stage's texture ... */
    uint32_t           stage_valid; /* ... usable when bit i is set */
    uint32_t           clip_plane_mode; /* SET_SHADER_CLIP_PLANE_MODE */
    float              fog_color[4];/* r,g,b,a */
} Nv2aBatch;

/* sync() reasons. */
#define NV2A_BACKEND_SYNC_METHOD   0   /* a method that reads or orders the GPU */
#define NV2A_BACKEND_SYNC_SCAN_END 1   /* a pushbuffer scan ends (before DMA_GET) */
#define NV2A_BACKEND_SYNC_READ     2   /* the executor reads a surface (dumps) */
#define NV2A_BACKEND_SYNC_PRESENT  3   /* FLIP_STALL: the frame is presented from guest RAM */

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    /* Make guest RAM hold what was drawn so far (colour surfaces). */
    void (*sync)(uint32_t reason);
    /* Samples that passed the depth test with zpass_enable set, since the
     * previous call. Blocks until the GPU has them. */
    uint32_t (*report)(void);
    /* Non-zero: the executor also rasterises in software, into guest RAM. */
    int shadow;
    /* Optional, narrower than sync(): make guest RAM hold what was drawn
     * into [va, va+bytes) -- the surface a flip is about to present. Without
     * it the executor calls sync(NV2A_BACKEND_SYNC_PRESENT). */
    void (*sync_surface)(uint32_t va, uint32_t bytes);
    /* Optional: show the frame a flip presents -- the surface at `va` with
     * `pitch`, aa_sy 1 or 2 as for xbox_FramebufferWindowSetAA -- without
     * guest RAM. Returns 1 when the back end took care of it (presented it
     * itself, or there is nothing to present to); the executor then neither
     * writes the frame back for the presenter nor copies it to the window. */
    int (*present)(uint32_t va, uint32_t pitch, uint32_t aa_sy);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* The same for one face of a cube (face 0..5; 0 for a plain texture), with the
 * palette of a P8 texture. */
int nv2a_backend_decode_texture_face(const Nv2aTexture *tex, uint32_t face,
                                     uint32_t *argb_out);

/* Bytes level 0 spans in guest memory, over all faces of a cube. */
uint32_t nv2a_backend_texture_bytes(const Nv2aTexture *tex);

/* Whether [va, va+bytes) is guest memory a surface may be read from and
 * written to: not the title's image, not a thread stack, not outside RAM. */
int nv2a_backend_surface_ok(uint32_t va, uint32_t bytes);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* Called when the executor reaches NO_OPERATION 5, the software method whose
 * GPU interrupt wakes XDK D3D's BlockOnFence (xbox_Nv2aFenceWakeEvent sets
 * this). NULL: nothing to wake. */
void nv2a_pb_set_wake(void (*wake)(void));

#endif /* NV2A_BACKEND_H */
