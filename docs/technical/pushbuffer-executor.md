# Pushbuffer Executor

A title that statically links the XDK's Direct3D never calls the toolkit's
D3D8 layer. It writes NV2A methods into a pushbuffer and kicks the GPU, and the
only way to see its frames is to execute that pushbuffer. `RECOMP_PB_SCAN`
surveys it; `RECOMP_PB_EXEC` executes it, on the NV2A poll thread, into guest
memory with a CPU rasteriser or into a render back end the game project
registers (`nv2a_backend.h`).

The rasteriser itself — vertex programs (`nv2a_vsh_interp.c`), register
combiners (`nv2a_combiner.c`), four texture stages, near-plane clipping and a
threaded fill (`nv2a_pb_exec.c`) — arrived in v0.13.0. This page covers what
sits around it, in the order the problems appeared while bringing up
*X-Men Legends*: reading the ring, answering the title while it runs,
fixed-function transform and lighting, anti-aliased surfaces, and the back-end
interface.

## Reading the Pushbuffer Like the DMA Engine

The executor used to walk the bytes between the previous and the current
`DMA_PUT` in a straight line, following `CALL`s since v0.13.0 but still treating
each PUT-to-PUT span on its own:

| Case | Straight-line behaviour |
|---|---|
| `JUMP` | ended the span; the target was read only if it was a `CALL`'s |
| PUT below the last PUT (the ring wrapped) | the ring's bounds were learned from the PUTs seen, so the first lap lost a segment and every later wrap scanned "a little short of the end" |
| the title writes `DMA_GET` | not noticed |

XDK D3D sends state blocks as CALLs into pre-built pushbuffers and the ring
wraps many times a second, so every lost segment is state the executor never
saw: wrong textures, world-space geometry pushed through D3D's
two-instruction pre-transformed program (`mov oPos, v0; mov oT0, v9`).

`nv2a_pb_scan.c` now keeps its own GET — a physical address, like the register
— and walks the way the DMA engine does: it follows `JUMP` (old and new style),
`CALL` and `RETURN` (one level of subroutine, as on NV2A), and stops when GET
reaches PUT. The ring's wrap is itself a JUMP back to the start, so following
it replaces learning the ring's bounds. A jump or call outside RAM means the
walk is out of step; it snaps GET to PUT and logs `[PB] bad target ...` with
the last commands it decoded. If the title moves GET itself (XDK D3D resets the
ring by writing `DMA_PUT` and `DMA_GET` together) the walker resyncs, and it
starts at the title's first GET, so device state sent before the first
kickoff — the depth function, for one — is not lost.

`tests/nv2a_pb_walk` checks the walk with a stubbed executor: `CALL` and
`RETURN`, `JUMP`, non-increasing methods, a resync, and a bad target.

## Answering the Title While It Runs

XDK D3D asks the GPU questions while it submits, and the executor has to
answer them from inside the walk, because the thread that executes the ring is
the thread that would otherwise answer. (Under the DMA walk the kickoff is the
exception: a thread of its own answers it, see below.)

- **Kickoff.** Every kickoff sets `0x100410` bit 16 and spins until it clears.
  With the bits cleared only between passes of the poll loop, one pass can be a
  whole rendered frame, so a level load — thousands of kickoffs — looked
  frozen. The walker now clears them every 256 words
  (`xbox_Nv2aAckBusyBits`). That still left the kick waiting whenever the
  executor's thread was not walking: asleep in the flip stall until the vblank
  tick, or in the back end's Present. In *Fallout* the title spun in KickOff
  for the rest of each frame, and its frames came in 4 + 29 ms pairs although
  the flips were on the grid. Under `RECOMP_PB_WALK=dma` a thread of its own
  clears the kick bit, as the FIFO does on hardware. It costs a host core
  spinning on `Sleep(0)`; a 1 ms sleep would make each of the ~450 kicks a
  second spin the title that long.
- **Fence mirrors.** A project that registers `xbox_Nv2aMirrorFence` has the
  poll thread copy the title's "submitted" fence into its "consumed" one. The
  walk now follows every CALL and JUMP, so a pass can be long, and a mirror
  ticked only between passes lags the title by a whole walk. D3D then takes its
  fence wait, which a mirrored fence cannot satisfy (Burnout 3 hung there at its
  first menu). `xbox_Nv2aAckBusyBits` ticks the mirrors as well, so they keep
  pace from inside the walk.
- **Semaphore.** `BACK_END_WRITE_SEMAPHORE_RELEASE` (`0x1D70`) writes its value
  where `nv2a_pb_set_semaphore_target()` was told, once everything before it
  has run. `SET_SEMAPHORE_OFFSET` is not added: in *X-Men Legends* it held
  `0xFF000000` at times. How a title's fence wait uses it is in
  [D3D8LTCG Device Context](d3d8ltcg-device-context.md#when-something-does-read-the-pushbuffer).
- **Method `0x0310` on subchannel 5.** D3D's `KickOff` ends every kick with it,
  carrying `((pushbuffer address * 8 | fence & 0x1F) << 2) | wrap & 3`. The
  GPU latches it in PGRAPH `0x400B10`, and D3D waits for the fence bits there.
  The executor latches the value. Without it `0x400B10` read 0 and the wait
  ended only when the semaphore was a multiple of 32.
- **`DMA_GET`.** It is the GPU's fetch pointer — nothing at or past it has been
  read — and D3D's fence wait relies on that: when its fence is far enough
  ahead of GET it patches a "wake me" software method into the pushbuffer at
  the fence and sleeps on an event. GET used to move only when a walk reached
  PUT, so it lagged the executor by up to a whole kick; D3D then patched
  commands the executor had already passed and slept for good. The walker now
  publishes its position before every command, and finishes on PUT. A GET that
  is not the value it last wrote means the title moved it (the ring reset), and
  the walk follows it rather than overwriting it.

## Switches Read Once

The NV2A poll thread paid for `getenv()` on every method — it scans the whole
environment, and a 3D frame is millions of methods. Debug switches are read
once, and the answer "not set" is cached as well as "set": a cache that only
remembered "set" ran `getenv` on every pass of the default path, where neither
`RECOMP_PB_SCAN` nor `RECOMP_PB_EXEC` is set. The method inventories (the
survey's and the unhandled-method ranking) are indexed by method instead of
searched: the survey's search was about 40% of the thread in a *X-Men Legends*
level.

`RECOMP_FB_DUMP` is read once for the capture after each batch that drew; the
periodic report reads it every time (once a report — tests turn it on there to
make the report download what a back end drew).

## The Report Is One Write

A title may make stderr unbuffered (*Fallout: Brotherhood of Steel* does, so a
crash loses nothing), and the UCRT then formats each `fprintf` into a file or
a pipe a character at a time, one `WriteFile` per byte. The periodic report
runs on the executor thread every ten seconds, and the game waits on its fence
meanwhile: its ~5 KB stood the executor still for 300-400 ms under a
PowerShell `Start-Process` redirect and 80 ms in a console. The report, the
D3D11 back end's `[GPURAST]` line and the kernel's `[KERNEL] summary` are now
collected by `text_block.h` and leave in one `fwrite`. Test: `tests/nv2a_points`
counts the `WriteFile` calls of the C runtime.

## Fixed-Function Transform

Batches drawn with `SET_TRANSFORM_EXECUTION_MODE` (`0x1E94`) on "fixed" used to
count as "not screen-space" and be skipped unless their positions already
happened to be pixels:

```
rasterised 0 triangles; 8073 batches skipped as not screen-space
```

D3D uploads a composite matrix — world × view × projection × viewport — with
methods `0x0680`–`0x06BC`, and a viewport offset with `0x0A20`–`0x0A2C`, which
the executor keeps in the constant file at `c[59]`. With a composite matrix
uploaded and a fixed-function mode, `ffp_vertex()` transforms attribute 0,
divides by w and adds the offset, and hands the result on in the shape a vertex
program produces. So a fixed-function batch shares everything after the
transform with a program batch: near-plane clipping, the four texture stages,
the combiners and the depth buffer. The 16 floats apply as
`clip[i] = Σ M[4i+j] · pos[j]` (translation in `M[3]`, `M[7]`, `M[11]`),
**not** as a row vector times the matrix; the other order puts every vertex
near `(0, 0)`. `RECOMP_FFP_TRACE` prints the matrix and a few raw and
transformed vertices, which is how the order was found.

Texture coordinates are taken from the attribute streams as they are, with no
texture-coordinate generation or texture matrix; there is no fog coordinate
or specular.

### Points, polygon offset, zero products

- **Points** (`BEGIN_END` 1) become squares of the vertex's point size in
  surface pixels: the program's `oPts.x`, else `SET_POINT_SIZE`, capped at
  63.875. With `SET_POINT_SMOOTH_ENABLE` (D3D's point sprites) every stage's
  texture coordinates run 0..1 across the square. Fallout draws its particles
  this way (steam, radiation, laser glow, smoke). `SET_POINT_PARAMS`
  attenuation is not modelled. Lines are still dropped.
- **Polygon offset** (`SET_POLY_OFFSET_FILL_ENABLE`, D3D's ZBIAS): window z
  moves by `factor * max(|dz/dx|, |dz/dy|) + units`, z in depth-buffer steps.
  Decals on the floor (scorch marks) need it not to flicker.
- **Zero products.** The vertex program's MUL and MAD give 0 when a factor is
  0, even against infinity or NaN, as the NV2A does. Fallout's lighting
  program multiplies unused lights' colours, which hold leftovers up to
  infinity, by a zero intensity; with IEEE NaN there, crates and robots came
  out saturated red. Dot products have no such rule.
- **Current attribute values.** A vertex program input with no array
  (`SET_VERTEX_DATA_ARRAY_FORMAT` size 0) reads the last `SET_VERTEX_DATA*`
  written to it, not 0,0,0,1. Fallout colours its radioactive liquid that way.
- **Swizzled surfaces.** `Nv2aSurface.swizzled` is set for a
  `SET_SURFACE_FORMAT` of type 2. The D3D11 back end still draws such a
  target linearly, and a swizzled texture over it reads pixel (u,v) directly;
  over a pitch target it reads `swizzle_offset(u,v)`. Fallout's character
  shadows are drawn into a 512x128 swizzled target this way. Written back to
  guest RAM, the target stays linear; the software rasteriser does not draw
  swizzled surfaces.

### Lighting

With `SET_LIGHTING_ENABLE` on, the diffuse colour is computed per vertex from
the normal and up to eight lights, and the vertex's own colour stream is
ignored unless `SET_COLOR_MATERIAL` routes it in. Using the colour stream
anyway is what drew every lit mesh — characters, streets — black. XDK D3D
pre-multiplies the material into the light colours and folds emission and
ambient into `SCENE_AMBIENT_COLOR`, so the hardware sum is

```
colour = scene_ambient + Σ atten_i · (ambient_i + diffuse_i · max(0, N·L_i))
```

in eye space, alpha from `SET_MATERIAL_ALPHA`. The registers are read from a
mirror of the last value written to every 3D-class method (`s_reg`), because
lights and material colours are the kind of state the executor does not model
one field at a time. Normals go through the model-view matrix itself rather than
the inverse transpose (exact for rotations and uniform scale); there is no
specular, no spot cone, no back-face colour and no skinning.

### Normals facing away

Some meshes store normals that face away from the camera, and that is
authentic: a write watch traced the road normal `(0, -1, 0.03)` to the title's
file reader (*X-Men Legends*), and it equals `(v1-v0)×(v2-v0)` of the first
triangle. The model-view matrix mirrors (determinant −1) and culling is off, so
with `max(0, N·L)` those surfaces got only the ambient term.

What makes such a mesh look right on hardware is not established:
*X-Men Legends* writes `0x17C4` (two-sided lighting) as 0, and xemu uses front
colours only. So by default `lit_color` lights the normal as stored.
`RECOMP_FFP_FLIP_NORMALS` flips an eye-space normal that points away from the
eye instead — two-sided lighting with the back material equal to the front, per
vertex and not per face. It is a compatibility option, not hardware behaviour:
a game project sets it for a mesh it has seen need it, so it cannot change how
another title is lit. Check against xemu or hardware if a surface ever looks
lit from the wrong side.

## Anti-Aliased Surfaces

With multisampling on, the surface is 2× (or 2×2) the logical size, and
`SET_SURFACE_FORMAT` bits 12–15 say which. The executor used the logical
640-pixel width with the real 5120-byte pitch, got 8 bytes per pixel, and wrote
`0 pixels`. The clip rectangle is now scaled by that factor, and so are the
positions that come out of a vertex program or the composite matrix (the
viewport the title programs is in logical pixels). The near-plane clipper takes
its viewport scale and offset from the same place, so clipped triangles land
where unclipped ones do. The framebuffer window takes every other pixel of a
2×-wide surface instead of showing it black, and the host-side depth buffer is
wide enough for 1280×960.

## Index Width

The index list was `uint16_t` and held 4096 entries, but `DRAW_ARRAYS` starts
are 24-bit, so vertices past 65535 wrapped to the start of the buffer and big
batches were cut short. Indices are 32-bit now, batches may hold 65536, and
`ARRAY_ELEMENT32` (`0x1808`) is handled. An index comes from the pushbuffer, so
`fetch_attr` checks every vertex read against guest memory (ordinary RAM and the
contiguous window); a read outside it returns no vertex and the batch is dropped.
`SET_VERTEX_DATA_ARRAY_FORMAT` size 7
(`3W`) reads as three components.

## Render Back Ends

`nv2a_backend.h` is the interface. A game project registers `clear`, `draw` and
`flip`, and receives:

- the colour surface and its anti-aliasing factor;
- triangle lists in surface pixels, **already clipped at the near plane** by the
  executor, with per-vertex colour, `1/w`, and texel-space UVs — whichever path
  produced them (program, fixed-function or pre-transformed);
- the texture the batch samples (`nv2a_backend_decode_texture` decodes every
  format the executor can sample), with its mip level count and filter word;
- blend, alpha, depth, colour-mask, cull and stencil state in the NV2A's own
  enum values, plus the zeta format and whether visibility-test counting is on.

Next to those, `Nv2aBatch` carries everything the executor's own pixel path
reads, so a back end can shade exactly as the CPU rasteriser does:

- `vertices_ex`: the same triangles with every interpolant (`Nv2aVertexEx`):
  diffuse and specular colours unclamped, the fog factor already through the
  fog unit, and all four stages' texture coordinates as the program left them
  (not divided by w; texel UVs in `tex[0]` on the screen-space path);
- `kind`: `NV2A_BATCH_SCREEN` (flat colour, the texel replaces it, no depth) or
  `NV2A_BATCH_XF` (program or fixed-function: depth, alpha test, combiners);
- `use_rc` and `rc`, the raw register-combiner words; `textured`;
- `stages[4]` / `stage_valid`, each stage's texture with its palette and cube
  flag (`nv2a_backend_decode_texture_face` decodes one face, P8 included;
  `nv2a_backend_texture_bytes` gives the guest span for a cache);
- `clip_plane_mode` and the fog colour.

The surface carries its clip origin (`clip_x`, `clip_y`) as well as its size.

Five optional members (`sync`, `sync_surface`, `present`, `report`, `shadow`)
let a back end draw somewhere other than guest RAM:

- `sync(reason)`: make guest RAM hold what was drawn. Called at every method
  that drains the executor's pipeline (surfaces, flips — so before a flip is
  presented — reports, semaphores, clears, other subchannels), at the end of
  every pushbuffer scan before `DMA_GET` is published, and before the
  executor's own surface dumps (`NV2A_BACKEND_SYNC_READ`).
  `nv2a_backend_surface_ok` says whether a range may be written. A back end
  may treat the method and scan-end reasons as ordering points only and write
  back lazily, as the toolkit's does.
- `sync_surface(va, bytes)`: at `FLIP_STALL`, just before the presenter reads
  guest RAM, the surface it presents. Without it the executor calls
  `sync(NV2A_BACKEND_SYNC_PRESENT)`.
- `present(va, pitch, aa_sy)`: at `FLIP_STALL`, on the executor's thread right
  after the flip's vblank tick (or at once with no limit), show the presented
  surface without guest RAM. When it returns 1 the executor neither syncs the
  surface for the presenter nor copies it to the framebuffer window (the flip
  dumps still get a `NV2A_BACKEND_SYNC_READ` first).
- `report()`: the visibility-test pixel count since the previous call, added
  to what `GET_REPORT` writes; `CLEAR_REPORT_VALUE` discards it.
- `shadow`: the executor keeps rasterising in software as well, into guest RAM,
  as if no back end were registered — for comparing the two.

All calls come from the NV2A poll thread, one at a time, so a back end can
create its window and device lazily and pump messages from `flip`.

Things a back end should get right. These come from the Direct3D 11 back end
in `src/d3d/nv2a_gpu_d3d11.c`, written for *Fallout: Brotherhood of Steel*
(`tests/nv2a_gpu_backend` runs it on WARP):

- **Window z is compared as it is.** `SET_CLIP_MIN`/`MAX` only clip -- a pixel
  whose window z falls outside them is not drawn, on the executor's own
  rasteriser and in the D3D11 back end alike. The executor also clips each
  triangle at the `SET_CLIP_MIN` plane in clip space before handing it on,
  so a back end never gets the part in front of the near plane: cut only at
  w = 1e-3, that part reached 1e5 pixels and more off the surface, and the
  GPU drew slivers of it as needles and slabs fanning out from whatever the
  camera was close to (`tests/nv2a_near_clip`). A title
  narrows the range for a low-detail layer so it is cut off near the eye; a back
  end that rescales each draw's z over its own range makes that layer about 237
  times deeper (the figure from that back end's title) and puts it in front of
  everything near.
- **Stencil.** A title can mark pixels with a stencil-writing mesh and then draw
  a full-screen quad that only the marked pixels pass. A back end that ignores
  `stencil_*` draws that quad over the whole surface.
- **Cull from projected positions.** D3D11 decides facing after clipping, so a
  triangle that reaches behind the eye keeps its true winding; the NV2A decides
  from the projected, mirrored one. Decide culling per triangle from the
  projected screen positions and draw with the rasteriser's culling off.

The toolkit's own back end is `src/d3d/nv2a_gpu_d3d11.c` (`RECOMP_GPU_RASTER`):
it rasterises on the GPU, runs the combiners in a pixel shader and keeps depth
and stencil on the GPU. Its pixel shaders come from one source
(`src/d3d/nv2a_gpu_shaders.hlsl`), one per path, picked on the CPU per draw:
`ps_screen` for screen-space batches, `ps_ffp` for the fixed-function
stand-in, and `ps_rc` for the register combiners in three variants by what the
stages sample (`RC_TEX`: anything; no render target read linearly; only such
targets). They are compiled when the toolkit is built (see "Pixel shader
cost" below). A game project registers it by calling
`nv2a_gpu_d3d11_install()` before the title starts.

Its colour surfaces go back to guest RAM only when something reads them there:
the presented surface at a flip, every target before the executor's dumps, a
target a texture has to be decoded from, and a target being dropped. A texture
that is a live render target's pixels (linear 32-bit, the target's pitch) is
sampled from the target on the GPU, with no writeback and no CPU decode — the
same idea as xemu binding a surface as a texture and Dolphin's EFB copies to
texture. Fallout: Brotherhood of Steel draws its scene into one 1280x480
surface and filters it into the presented one that way. In Carbon on an
RTX 3080 Ti that took the back end from 46 to 66-67 fps (software path
17.7-18.6), each step measured on its own:

| Step | fps | writebacks / 10 s | readback stall / 10 s |
|---|---|---|---|
| write back at every sync, every texture through guest RAM | 46 | 6,170 (3.6 GB) | 1.8 s |
| live targets sampled as textures | 58 | 7,780 (4.5 GB) | 2.2 s |
| and lazy writeback (all dirty targets at a flip) | 66 | 1,980 (2.5 GB) | 1.9 s |
| and only the presented surface at a flip | 67 | 1,340 (1.7 GB) | 1.8 s |

Two more steps took the readback out of the frame altogether:

- **Presenting from the GPU.** With a framebuffer window (`RECOMP_FB_WINDOW`)
  the back end puts a swap chain on it and draws the presented target into
  the back buffer (every other pixel of a 2x-wide anti-aliased surface, as
  the GDI path takes them); `fb_present.c` keeps the title bar, the keyboard
  and the close button and stops blitting
  (`xbox_FramebufferWindowHandle`, `xbox_FramebufferWindowExternal`). The
  back buffer is the window's client area, resized with it; the 640x480 frame
  goes, filtered, into the largest rectangle of the display aspect
  (`xbox_FramebufferWindowFit`, bars around it), as the GDI path stretches
  it. `RECOMP_FB_ASPECT=16:9` (or `xbox_FramebufferWindowSetAspect`, for a
  title patched to draw 16:9 anamorphically) sets the aspect,
  `RECOMP_FB_SIZE=2560x1440` the picture's size (shrunk to the work area),
  F11 or `RECOMP_FB_FULLSCREEN=1` borderless full screen.
- **Internal resolution and MSAA.** `RECOMP_GPU_RASTER_SCALE=<n>|<x>x<y>`
  (1-8) makes every target and depth buffer that many times the surface,
  `RECOMP_GPU_RASTER_MSAA=<n>` multisamples them (lowered to what the device
  has; compare forces both off). Vertices stay in surface pixels and the
  viewport is scaled. Guest RAM keeps the surface's own size: rows the CPU
  wrote go up through a native texture drawn point by point over each block
  (`bps_up`), a writeback resolves and averages each block down (`bps_down`,
  `rt_read_native`), so shadows, `rt_taken` and the software path compare
  native bytes as before. A target sampled as a texture is read at the high
  resolution (`rt_texel`; a swizzled view takes each block's centre), and the
  presenter draws the high-resolution frame. All targets and depth buffers
  share one size, as D3D11 wants a target and its depth buffer equal: native
  1280 x `s_gh`, where `s_gh` starts at 512 and grows -- everything written
  back and made again -- when a surface reaches lower. Occlusion counts are
  divided back to pixels. Scale 1 without MSAA is the old path exactly.
  Cost is GPU time per pixel (the combiner shader; see "Pixel shader cost"): Fallout's menu,
  uncapped on an RTX 3080 Ti, 1x ~1,800 fps, 3x3 + 4x MSAA ~122, 4x3 + 4x ~85.
  `RECOMP_GPU_RASTER_SSAA=<n>` (1-4) draws the targets at scale x n (at most 8
  a side, otherwise n is lowered with a log line); the presenter first averages
  each n x n block into a texture of the shown scale (`ssaa_reduce`, the same
  `bps_down`), and that is what is scaled to the window; the writeback still
  averages the whole block, and MSAA is resolved first.
  `tests/nv2a_gpu_backend` runs a second time at 3x3 + 4x MSAA and a third at 2x
  with 3x3 SSAA; each also reads a linear target and a swizzled one as swizzled textures.
- **GPU profile.** `RECOMP_GPU_PROF=<secs>[,stats][,flatps]` (`nv2a_gpu_prof.c`,
  off by default: then each hook is one test of a flag and no query is made)
  prints every `<secs>` where the GPU's time per frame goes. The back end marks
  each piece of GPU work with its kind (`clear.color clear.zs z.init draw.screen
  draw.ffp draw.rc resolve upload writeback tex.upload ssaa present swap`) and
  target; a timestamp is written only where (kind, target) changes, so a
  segment is a run of one key and the segments add up to the frame's span. A
  frame is bracketed in a `TIMESTAMP_DISJOINT` query and read back from a ring
  of 8 with `DONOTFLUSH` a few flips later -- never waited for; with all 8 out
  a frame is dropped, a disjoint one thrown away; a frame whose `GetData`
  fails (a query never ended) is thrown away too but counted apart, as `read
  errors`, so it cannot pass for the driver's disjoint. The profile submits each
  frame's closing queries itself (a `Flush` at the flip: they come after
  `Present`, which submitted only what came before): left in the command buffer until the next frame's work, the
  end timestamp ran late, and at a frame limit the GPU's idle time went into
  the last segment (span = the flip period). The report (one block, one
  `fwrite`) gives per kind ms/frame, % of the span, segments, draws, triangles,
  clears, resolves and MB; per target (12 heaviest) ms by kind; the share of
  triangles drawn with zfar / zclip / alpha test / clip planes / blend / depth
  write / cull_enable; host flip period, `Present()`, readback stalls and
  visibility-report waits. Pixels (`est Mpx`) are each draw's bounding box
  inside the scissor at the internal scale, not what was shaded. Segment
  boundaries are as exact as the GPU orders timestamps against neighbouring
  work. `stats` adds a pipeline-statistics query per segment (pixel shader
  invocations, ns per 1000 of them) and may serialise the GPU: times without it
  are the honest ones. `flatps` swaps a flat pixel shader in for transformed
  draws, to see what the ubershader costs; the picture is wrong then.
  `tests/nv2a_gpu_backend`: `nv2a-gpu-prof` (stats), `-ts`, `-scaled`, and
  `-lost` (the device lost at the second flip with the profile on).
- **Pixel shader cost.** The profile showed where Fallout's frame went by
  Ruby's bar in Carbon at 6x5 + MSAA 2 on an RTX 3080 Ti: a GPU span of
  24.6 ms, 22.8 of it combiner draws, ~518 ns per 1000 pixel shader
  invocations; a flat pixel shader (`flatps`) drew the same transformed
  draws in 5.4 ms against 19.1. The ubershader kept the combiners' sixteen
  registers in an array indexed by the combiner words (`float4 R[16]`); an array indexed at run
  time is an indexable temp, which NVIDIA keeps in local memory, so every
  register read and write of every stage went out to memory. Three exact
  changes, each measured on its own:
  - the registers are fifteen `float4`s (r0 always reads zero) behind
    `[forcecase] switch`es on the register number -- uniform, as it comes
    from the combiner words -- span 24.6 -> 9.6 ms, 518 -> 188 ns per 1000
    invocations (a chain of `?:` instead compiled faster but drew at 304
    ns per 1000);
  - one shader per path instead of one with every path's branches
    (`ps_screen`, `ps_ffp`, `ps_rc`), the CPU picks: 9.6 -> 7.2 ms, 135 ns
    per 1000;
  - `wrapc` divides only when it must (a coordinate already inside needs no
    wrap, a power-of-two size wraps with `&`; a signed division is a long
    sequence and it ran four times per bilinear tap), and `ps_rc` has
    variants by what the stages sample (`RC_TEX`), so Fallout's filter pass
    (the 1280x480 scene read as a texture into the presented 640x480) runs
    without the texture paths it never takes: 0.8 -> 0.6-0.7 ms for that
    pass, the span 7.2 -> 6.9 ms.
  By the bar the span went 24.6 -> 6.9 ms, the combiner draws 22.8 -> 5.5 ms,
  518 -> 134 ns per 1000 invocations (spans without `stats`). The combiner
  shader takes D3DCompile ~14 s (ps_rc, 295 KB of DXBC; ~11.6 s before the
  `RC_TEX` variants; two to three times the ubershader's 5.2: the switches are long); the
  title enables the combiners when it creates its device, so the first frame
  needs it, and compiled in the background the first frame still waited
  ~14 s. So the shaders are compiled when the toolkit is built:
  `src/d3d/nv2a_gpu_shaderc.c`, a host tool on D3DCompile with the flags the
  back end used at run time, writes `nv2a_gpu_shaders.h` (the code of each
  shader and the source) from `nv2a_gpu_shaders.hlsl`;
  `src/d3d/nv2a_gpu_shaders.cmake` adds it to a target. The back end only
  creates them (`pipeline made in 0.01 s`); Fallout reaches control 16.0 s
  after start instead of 20.3. Only the blit, present and diagnostic shaders
  are still compiled at run time.
  The picture is the same bit for bit. `tests/nv2a_gpu_backend/ps_reference.hlsl`
  is a frozen copy of the ubershader (never edited); `nv2a-gpu-ps`, `-scaled`
  and `-ssaa` draw random combiner programs, FFP and screen batches (wrap and
  clamp, swizzled and render-target stages, alpha test, z outside the clip
  range) once through the reference and once through the back end's own and
  compare guest memory byte for byte, check the combiners against
  `nv2a_rc_eval` within 1 per channel, check that no variant has an
  indexable temp, and that the embedded code is what the source compiles to.
  On the GPU `RECOMP_GPU_RASTER_PSCHECK=<ref.hlsl>[,<every>]` draws each draw
  of every `<every>`-th frame (default 30) a second time through the picked
  shader and through `ps_main` of the reference file into a float target and
  prints every 10 s how many pixels differ (`[PSCHECK] ... float differ k,
  8-bit differ j`); by the bar both were 0. Diagnostics only: it reads back
  per draw.
- **Texture decodes off the frame.** A texture new to the back end's cache is
  decoded on the executor's thread mid-frame. Texel by texel through the
  executor's sampler that ran at ~19 Mtexel/s: Fallout's first street in
  Carbon decoded 16 textures (1.45 Mpixel) in 76 ms and 5 (1.08 Mpixel) in
  65 ms, hitches of 81 and 74 ms. `nv2a_backend_decode_texture_face` now
  decodes a whole face -- the swizzled index by masked increment, each DXT
  block once, the colour conversion shared with the sampler
  (`texel_convert`) -- bit for bit what the sampler gives
  (`tests/nv2a_tex_decode` against `nv2a_backend_decode_texture_face_ref`),
  ~350 Mtexel/s: the same two frames decode in 7 and 5 ms and stay at the
  frame limit. DXT1/3/5 that are not cubes and have sides in whole blocks are
  not decoded at all: they upload as BC1/2/3 and the GPU decodes them
  (within 1 of `d3d8_dxt_decode_texel` per channel on WARP, the test allows
  2). Fallout's streets use none (`DXT as BC` in `[GPURAST]`).
  Headless there is nothing to show, so nothing is read back. Reading the
  frame a flip later from a ring of staging textures had not helped (1.6 s
  per 10 s, same fps); not reading it at all does.
- **Swizzled views of a target.** Fallout draws its HUD into a linear
  512x128 target and samples it as swizzled A8R8G8B8. The back end now reads
  texel (u,v) as dword `swizzle_offset(u,v)` of the target on the GPU, which is
  what the software sampler reads from guest RAM; before, every such draw
  wrote the target back first (two GPU round trips a frame, 1.3 s per 10 s).

| Step | Carbon fps | writebacks / 10 s | readback stall / 10 s |
|---|---|---|---|
| presented surface written back at each flip (pass 3) | 66 | 1,330 | 1.8 s |
| present from the GPU, swizzled HUD target read on the GPU | 71-77 | 5 | 0.07 s |

The fps line counts with `RECOMP_FPS_LIMIT=200` (the cap is 60 by default; a
completely uncapped run spins the menus at 2,000 flips per second and the
scripted start press misses them). These figures predate the vblank grid; a
limit above 60 now counts as 60, so the same measurement needs `0`. By the Ruby/bar area in Carbon the back end
went from 39-41 to 43-45 fps; what remained there was CPU work: the executor
thread spent about 80% of its time in the vertex-program interpreter. Two
exact changes took that down: the interpreter decodes a program once (a
program word, an instruction or the start slot marks the decode stale)
instead of extracting every field of every instruction for every vertex, and
a batch transforms each vertex its index list names once, reusing the result
for repeats (not when the program writes constants). Same-session A/B with
`RECOMP_FPS_LIMIT=200`: Carbon spawn 77 -> 146 fps, the Ruby/bar spot 43 -> 98.

Later, with point sprites and the zero-product rule in, the executor thread
was busy 99% of the time by the bar, about 63% of it in the interpreter, while
the title waited on the GPU fence and the software rasteriser's workers sat
idle under the back end. A vertex's run reads only its attributes, the decoded
program and the constants, so the vertices a batch runs are now shared out
over several threads (`RECOMP_VSH_THREADS`, default half the logical
processors): the executor marks which positions run and which reuse another's
result, the runs go in chunks to the executor and its helpers, then the
repeats are copied. Each helper takes the executor's MXCSR, so the results
are the same bit for bit (`tests/nv2a_vsh_threads`, flush-to-zero included).
A program that writes constants (CXT_WRITE_EN) still runs its vertices one by
one, in order, and a batch with 256 or fewer to run stays on the executor.
Same point by the bar, no frame limit: 66 fps on one thread, 130 on eight;
about three quarters of the vertices ran threaded, none in programs that write
constants.

**Compare and far-reaching triangles.** The executor clips at the near plane
at w = 1e-3 and hands over screen positions, so a triangle that crosses the
near plane can have a vertex millions of pixels away. The software rasteriser's
float edge functions and barycentrics lose such a triangle: in Fallout's
"camera wedge" frames (the camera inside geometry, which happens on the
software path alone too) about a quarter of a million pixels per frame are
covered differently than the same arithmetic in double gives, and depth is off
by more than 1000 units on more. The GPU clips in clip space. So
`RECOMP_GPU_RASTER=compare` prints `far-reaching triangles N` with each flip: a
frame that differs while N is not 0 is that, not a GPU fault. Outside those
frames Carbon compares at 0.00–0.02%.

## Switches

| Variable | Does |
|---|---|
| `RECOMP_PB_SCAN` | read-only survey of the pushbuffer; ranks what is not implemented |
| `RECOMP_PB_EXEC` | execute it |
| `RECOMP_PB_WALK=dma` | walk the pushbuffer like the DMA engine (JUMPs, wraps, GET per command, the `0x0310` latch) instead of scanning each submitted segment; see [The DMA Walk Is Opt-In](#the-dma-walk-is-opt-in); when the title registered the fence event (`xbox_Nv2aFenceWakeEvent`), kickoffs are answered by a thread of their own and the fence event is set only by `NO_OPERATION 5` (plus the 2 s heartbeat while `DMA_PUT` stands) |
| `RECOMP_PB_EXEC_VERBOSE` | per-batch trace |
| `RECOMP_VSH_THREADS=<n>` | threads that run a batch's vertex programs, the executor included (default half the logical processors, at most 16); `1` runs every vertex on the executor thread |
| `RECOMP_FFP_TRACE` | composite matrix and sample vertices of the first fixed-function batches |
| `RECOMP_GPU_RASTER` | rasterise on the GPU through the D3D11 back end (`src/d3d/nv2a_gpu_d3d11.c`), once the game project calls `nv2a_gpu_d3d11_install()`: `1` a hardware adapter (WARP if none), `warp` the WARP adapter, `compare` the software path draws and the GPU draws alongside, printing per flip how far they differ and how many far-reaching triangles the frame had (`<RECOMP_FB_DUMP>gpu_/sw_<flip>_<surface>.bmp` for the first frames that differ by more than 1%). Unset or `0`: off |
| `RECOMP_GPU_RASTER_SYNC=all` | the D3D11 back end writes its surfaces back at every executor sync, not only when they are read (for a title whose CPU reads what the GPU drew) |
| `RECOMP_GPU_RASTER_SCALE=<n>\|<x>x<y>` | render resolution as a multiple of 640x480, 1-8 each way (see "Internal resolution and MSAA" above) |
| `RECOMP_GPU_RASTER_MSAA=<n>` | 2, 4 or 8 samples, lowered to what the device supports |
| `RECOMP_GPU_RASTER_SSAA=<n>` | 1-4: drawn at scale x n, each n x n block averaged down before the window; scale x n at most 8 a side |
| `RECOMP_GPU_RASTER_PRESENT=0` | the D3D11 back end leaves the framebuffer window to GDI and guest RAM (the presented surface is written back at every flip) |
| `RECOMP_GPU_RASTER_PRESENT_DUMP=<prefix>` | every `RECOMP_GPU_RASTER_PRESENT_DUMP_EVERY`-th (default 30) presented frame as `<prefix>NNNNNN.bmp`, 640x480 as the window shows it, HUD and menus included |
| `RECOMP_GPU_RASTER_RTTEX=0` | the D3D11 back end reads every texture through guest RAM, render targets included |
| `RECOMP_GPU_RASTER_PSCHECK=<ref.hlsl>[,<every>]` | every `<every>`-th (default 30) frame the D3D11 back end draws each draw again through its pixel shader and through `ps_main` of the reference file (`tests/nv2a_gpu_backend/ps_reference.hlsl`) and prints every 10 s a `[PSCHECK]` line: draws, pixels, float and 8-bit differences, the first that differs; diagnostics only (reads back per draw); see "Pixel shader cost" above. Unset: off |
| `RECOMP_GPU_RASTER_LOSE_AT=<flip>` | act out a lost D3D11 device at that flip: the software rasteriser takes over from the next frame, as after a driver reset |
| `RECOMP_GPU_PROF=<secs>[,stats][,flatps]` | every `<secs>` (at least 0.5) a `[GPUPROF]` block: the D3D11 back end's GPU time per frame by pass and render target, from timestamp queries; `stats` adds pixel shader invocations, `flatps` a flat pixel shader for transformed draws (wrong picture, diagnostics only); see "GPU profile" above. Unset or `0`: off |
| `RECOMP_FPS_LIMIT=<hz>` | the flip stall completes on a tick of the 60 Hz vblank grid (the host's performance counter from the first flip or vblank), at least ceil(60/hz) ticks after the previous flip; a frame that misses its tick waits for the next one, as on a vsynced console, so frame times are whole 1/60 s (39 fps shows as 30, never 4 + 28 ms pairs). Default 60; a limit above 60 counts as 60; `0` off — the flip completes at once and a title pacing itself on the flip runs its menus at hundreds of fps. With `RECOMP_VBLANK` the guest's vblank count follows the same grid |
| `RECOMP_FRAME_SKIP=0\|1\|auto` | frame skip (see [Frame Skip](#frame-skip)): the flip waits for every tick of the grid whatever the limit, and only some flips are shown. `1`: L shows a second on the grid (`RECOMP_FPS_LIMIT` = L, active below 60), Present as before; `auto`: the same cadence, and a frame is shown only when the back end can present it without waiting (an outside limiter), on a flip-model swap chain. Needs a back end with `pace` (the D3D11 one, not with `compare` or `SYNC=all`); otherwise one `[GPU] RECOMP_FRAME_SKIP=… is not active: …` line. Unset or `0`: off, the flip waits ceil(60/L) ticks |
| `RECOMP_GPU_RASTER_PRESENT_HZ=<n>` | diagnostics for `RECOMP_FRAME_SKIP=auto`: the D3D11 back end acts out a limiter at n Hz in front of its swap chain (the host clock cut into 1/n s slots, one show a slot), to see frame skip at work without one |
| `RECOMP_GPU_RASTER_FRAME_LATENCY=<1-3>` | `RECOMP_FRAME_SKIP=auto`: frames the flip-model chain may hold (`SetMaximumFrameLatency`, default 2). At 1, a frame whose GPU work and the compositor's next vblank outlast a flip period keeps the waitable unsignalled at the next flip, and auto skipped frames with no limiter at all (6x5 + MSAA 2 in a 143 Hz window: 465-582 of 600 shown; at 2: 593-600). 2 costs a frame of queue only while the GPU is slower than a flip period; an outside limiter still fills it |
| `RECOMP_FRAME_SKIP_TRACE=1` | why a skipped window is drawn after all, where a stale read or texture came from (`[FSKIP]` lines, the first 60 and every 500th) |
| `RECOMP_FFP_FLIP_NORMALS` | light the side of a fixed-function mesh that faces the viewer, even when the title left two-sided lighting off. Not hardware behaviour: a compatibility option for a game project to turn on, off by default |

## Frame Skip

A title whose logic counts frames needs them 1/60 s apart. Fallout: BoS's
Tutorial charge does not count at a native 22.2 ms (45 fps) or 33 ms (30 fps),
so a lower `RECOMP_FPS_LIMIT` -- or an outside limiter: a Steam Deck's frame
limit, NVIDIA's Max Frame Rate, RTSS -- broke it. With `RECOMP_FRAME_SKIP` the
flip stall still completes on every tick (`vblank_grid_pace_hz`), so the
semaphore, `NO_OPERATION 5`, the frame counter and `flip()` see 60 a second,
and the back end's `pace(va, tick, cadence_show, cadence_next)` decides at each
`FLIP_STALL` whether that flip is shown. The cadence is
`vblank_grid_shown`: tick k belongs to slot floor(k·L/60) and a flip is shown
when its slot is past the last shown one -- divisors of 60 are even (30: every
other tick), the rest alternate (45: 1-1-2, 40: 1-2, 50: 1-1-1-1-2). A flip that
is not shown is not presented, written back, copied to the window or dumped
(`RECOMP_FB_DUMP_FLIPS`). The `[GPU] … fps (N flips, shown M)` line counts both;
the window title says `FPS: 45.0 (game 60.0)`.

The D3D11 back end does not draw what is not shown. A *window* runs from one
boundary to the next -- the first full overwrite after a flip of the surface
that flip showed (or the one before, when the two alternate): a colour clear
of all four channels over the whole target, or a draw
that passes the coverage test (one triangle, or two forming a convex quad,
holding the four corner pixel centres; opaque: no blend, all channels, no
alpha/depth/stencil test, no clip-plane stage, z inside `SET_CLIP_MIN/MAX`). In
a skipped window clears and draws go into a journal (surface, state, combiners
and a copy of every vertex -- the executor reuses its buffers) and the GPU sees
none of it. At the next boundary the journal is dropped, unless a target it
wrote is not *self-contained*: the last shown window read it (a texture, a
blend, a show, a writeback, a depth/stencil test) before overwriting it whole,
or did not touch it; a write nothing reads does not count (Fallout's filter
writes depth into the zeta-0 buffer with the test off every frame). It is
drawn (replayed) instead then -- at that boundary, even when the next window
is skipped too -- and also when
the frame turns out to be shown after all, when something observes the GPU's
work in a skipped window (a writeback, a visibility count from a logged draw)
and when the 256 MB arena is full. A dropped target read before it was
overwritten is a *stale read*: said once; the target is then drawn in skipped
windows until a shown window overwrites it first again (in boot_check one
each at the chapter intro and Carbon's loading screen, where the shown
surface changes; `RECOMP_FRAME_SKIP_TRACE=1` says why a window is drawn and
where a stale read or texture came from). The 10 s `[GPURAST] frame skip:` line counts shown,
skipped, not ready (presenter / GPU), logged ops, windows dropped, replayed
(ready / observed / not self-contained / arena full), mispredicted and stale
reads, stale textures (a logged draw keeps a hash of each texture it samples
from guest RAM; changed by the time it is drawn, it is counted and said once).
The arena figure is its size; the MB before it is what was logged over the
period. In Fallout every window begins with `clear F3` over its 640x480 target
and nothing reads across windows, except the loading screen, whose filter
reads the previous frame: its one clear is replayed (stage3 traces of the
port's task, 210 windows).

With `auto` the swap chain is a flip-model one (`FLIP_DISCARD`, two buffers,
`SetMaximumFrameLatency(2)` -- `RECOMP_GPU_RASTER_FRAME_LATENCY` -- and its
waitable object), and a frame is shown when
the waitable is signalled and the GPU has finished the last shown frame (a
`D3D11_QUERY_EVENT`). No thread of its own: DXGI must not run beside the
immediate context, and DXVK holds the context lock through part of Present.
The mode of the next window is a forecast -- the cadence, and with `auto` the
presenter as a rate (shows a tick, 0.75 under a limit of 45) and a credit in its
phase: a tick accrues the rate, a show spends one, a flip held back says the
credit is just short of one; the next window is expected shown when the credit
reaches one by its flip. (An average of the interval between shows swung with
45's 1-1-2 and had shown windows logged and drawn late.) A frame that is only in
guest RAM (a movie the CPU drew) goes up into a texture and through the same
shader, as GDI cannot draw into a flip-model window. Without the flip model
or the waitable `auto` falls back to the cadence on the old chain. Frame skip
is the GPU path's only: the software rasteriser shows every flip. Tests:
`tests/nv2a_vblank_grid` (the cadence) and `tests/nv2a_gpu_backend`
`nv2a-gpu-skip-*` (the executor and the back end on WARP).

## The DMA Walk Is Opt-In

`RECOMP_PB_WALK=dma` turns on the DMA-engine walk; without it the executor
scans each segment between the previous `DMA_PUT` and the new one, as it did
before the walk existed.

The walk is the more faithful of the two, and that is the reason it is opt-in.
Told exactly how far the GPU has got (`DMA_GET` per command and the `0x0310`
latch), XDK D3D's fence code takes its "GPU far behind" path: it patches a
software method (`NO_OPERATION` with a value) into the pushbuffer and sleeps on
an event that the GPU's interrupt for that method sets. Until the runtime
delivers that interrupt, a title that runs under the segment scan can freeze
under the walk on the first screen whose pushbuffer makes the walk long
enough. Burnout 3 did, at its Crash Nav menu, with the main thread in
`KeWaitForSingleObject` under D3D's fence wait.

Turn the walk on for a project that delivers the GPU's software-method
interrupt, or to bring up a title the segment scan draws wrongly.

The segment scan also loses commands. It learns the ring's end from the
highest `DMA_PUT` it has seen, so on a lap whose closing JUMP lies beyond that
the commands in between are never executed. In *Fallout: Brotherhood of
Steel* that was a menu frame without its Bink background, or a wedge of stray
vertices in a level, a few times a minute.

## Handing the Fence to the Executor

A fence mirror (`xbox_Nv2aMirrorFence`) writes "everything submitted is done"
as soon as it is submitted. Under `RECOMP_PB_EXEC` that runs ahead of the
executor, and XDK D3D reuses a dynamic vertex buffer or texture as soon as the
fence passes the kick that drew from it, so the title overwrites what the
executor has yet to read.

`xbox_Nv2aFenceWakeEvent(device_ptr_va, event_off)` names the event D3D's
`BlockOnFence` sleeps on. With it registered and the executor on:

- the mirror stands down, and the fence word takes the title's own
  `BACK_END_WRITE_SEMAPHORE_RELEASE` values as the executor reaches them
  (`nv2a_pb_set_semaphore_target`);
- the `0x0310` marker is latched into `0x400B10` as the title wrote it, in the
  segment scan as well as in the walk, so the latch and the fence agree;
- the event is set at `NO_OPERATION 5` (`nv2a_pb_set_wake`), where the GPU's
  interrupt would set it. The segment scan sets it again whenever `DMA_GET` is
  published (GET only after a whole segment, so D3D can patch a marker the
  executor has already passed). The DMA walk publishes GET before every
  command, so there the event means `NO_OPERATION 5` was executed, plus a
  heartbeat when `DMA_PUT` has stood for 2 s.

A spurious wake is not harmless: *Fallout*'s `BlockOnFence` returns after one
wake without re-reading the fence, so its Swap ran a frame early. Under the
DMA walk, with the kick answered on its own thread, this guards against a
latent race rather than a measured one. It showed in the experiment that
answered the kick inside the flip's wait while the event was still set after
every pass: the title then ran 31 frames ahead of the executor. Setting the
event only at `NO_OPERATION 5` keeps its Swap behind the fence, as on
hardware.

The event qualifies for a host shadow even without `RECOMP_TITLE_KEVENTS`. So does any KEVENT on a guest stack (kernel-replacement.md).
`tests/nv2a_fence_wake` waits on it on one thread and sets it from another, and checks it does not qualify without `RECOMP_PB_EXEC`. `kick-flip` checks a kickoff is answered while the executor waits for a flip's tick, `get-wake` that a walk's pass without `NO_OPERATION 5` leaves the event unset.
Fallout registers `(0xB5E58, 0x1DBC)` and runs the walk.

### Lost kicks

Guest threads run on several host cores at once, which the Xbox's one CPU
never did, and XDK D3D is not thread-safe. In Fallout a loader thread and the
main thread raced: a fence marker went into the pushbuffer, D3D's write
pointer moved past it, and no `DMA_PUT` was ever stored for it -- the main
thread then slept in `BlockOnFence` for a fence the GPU could not reach (the
Tutorial froze on its loading screen).

With the fence owned by the executor, the ack thread watches D3D's write
pointer (`xbox_Nv2aLostKickPoll`): when the GPU has consumed everything it was
given and the pointer has stood further on, in the contiguous window and
within 64 KB of `DMA_PUT`, for 250 ms, it stores `DMA_PUT` itself -- by
compare-and-swap, so a kick D3D makes meanwhile wins. The bytes are complete
(D3D moves its pointer after writing them). Not hardware behaviour: recovery
from a race the hardware never sees, logged as `[NV2A] lost kick`.

The write pointer is read at `device + 0`, the device being the one
`xbox_Nv2aFenceWakeEvent` names -- XDK D3D's layout. A title whose D3D keeps
it elsewhere gets no recovery and no harm: the value read is not a pointer
ahead of `DMA_PUT` in the window, and nothing is kicked.
`tests/nv2a_fence_wake` steps the poll through a fake `DMA_PUT`.
