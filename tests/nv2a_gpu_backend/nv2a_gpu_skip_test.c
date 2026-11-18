/* Frame skip (RECOMP_FRAME_SKIP) in the executor and the D3D11 back end, run
 * for real on the WARP adapter. The game must run as it does at 60 at any
 * display limit: the guest flips on every tick of the 60 Hz grid (its dt
 * stays 16.7 ms -- at a native 22 or 33 ms the Tutorial charge does not
 * count), and only some flips are shown. A frame that is not shown is not
 * drawn: its clears and draws go into a journal that is dropped at the next
 * frame's first full overwrite of the shown surface, or drawn after all when
 * something needs it. Got wrong, the picture shows a frame drawn from stale
 * targets (a missed window), a frame twice, or the game slows down.
 *
 * No clock: nv2a_pb_exec_test_tick makes each flip land a set number of
 * ticks after the previous one. One scenario per run (argv[1]); the limit and
 * the mode are set here before the first flip. Frames are drawn as Fallout
 * draws them (stage3 traces): a filter triangle from the shown surface P into
 * F right after the flip, then `clear F3` over P and its depth buffer (the
 * boundary), the scene into P, an atlas A cleared, drawn and sampled into P.
 * Each frame has its own colour, and what is checked is what the GPU holds
 * for P after a shown flip (nv2a_gpu_test_rt_pixel).
 *
 *   s1   L=30, 12 frames one tick apart: shown on the even ticks (6), 0 draws
 *        and clears reach the GPU in a skipped window, 5 windows dropped, 0
 *        replayed, 0 stale reads, the pacer waits for 60 (not 30), every
 *        shown frame is its own.
 *   s1b  L=45: 9 of 12 shown, intervals 1-1-2.
 *   s2   A target the frame reads before it writes it (it carries over from
 *        frame to frame): not self-contained, so a skipped window's journal
 *        is drawn at the next shown window -- no stale read, every pixel of
 *        what P samples from it is the previous frame's, the small rectangle
 *        where that frame put it included (the journal holds its own copy of
 *        the vertices: the executor reuses its buffer).
 *   s3   A frame drawn in a skipped window, then flips with no work at all:
 *        the next shown flip draws the journal first and shows that frame.
 *   s4   The atlas is self-contained, then shown frames read it before
 *        overwriting it: one stale read, said once, after which the atlas
 *        is drawn in skipped windows too (the frames that read it first see
 *        the right colour); cleared first again, it is dropped again.
 *   s21  Into a loading screen (the filter's F shown) and out: at most a
 *        stale read at the switch, then skipped windows dropped again.
 *   s5   Late flips (1,1,1,2,1,3,1,1 ticks): shown by slot, never twice in
 *        one; a flip the forecast took for a skipped one is shown after its
 *        journal is drawn.
 *   s6   The semaphore release, NOP 5 and the frame counter reach the title
 *        for every flip, shown or not.
 *   s7   RECOMP_FRAME_SKIP=0 at L=30: the flip waits 2 ticks, every flip
 *        shown, pace never asked; s7b: 1 at L=60: not active, every flip shown.
 *   s8   The coverage test, negatively: a target first written by a quad
 *        short of the far corner and one blended over is not self-contained
 *        -- its windows are drawn ("not self-contained").
 *   s9   In a skipped window a visibility report with no counted draw logged
 *        draws nothing; with one logged it draws the journal; so does a
 *        writeback (sync READ).
 *   s10  The device lost at a skipped flip (L=30) and with auto: no crash,
 *        the software path takes over and paces at the limit again.
 *   s11  auto, a flip-model chain on a hidden window: the presenter never
 *        ready for 12 frames -- the executor never waits (ctest's timeout), 1
 *        frame shown before; once ready, the newest frame is the one shown.
 *   s12  auto, the GPU busy with the last shown frame: shows held back as
 *        "not ready (gpu)".
 *   s13  auto, the presenter ready every 4th tick: the forecast interval
 *        learns 4, and a frame shown after a wrong forecast is drawn right.
 *   s14  Fallout's frame as boot_check met it: depth written into the zeta-0
 *        buffer with the test off, a second full overwrite of P, a loading
 *        screen first -- every skipped window in the level dropped, none drawn.
 *   s15  auto with the real waitable on a hidden window: frames go on, some
 *        Presented; a waitable taken while the GPU is busy is kept.
 *   s16  a frame only in guest RAM on the flip-model chain (32-bit, 2x-wide,
 *        R5G6B5, and 2x2 anti-aliased -- AA bits in SET_SURFACE_FORMAT, every
 *        other row read) goes up into a texture, texel for texel, and is
 *        Presented.
 *   s17  RECOMP_GPU_RASTER_PRESENT_HZ=20: at most ~20 shows a second.
 *   s18  a texture changed between a skipped window and its drawing: one
 *        stale texture.
 *   s19  ONE/ZERO through MIN, and a depth buffer cleared over part of its
 *        surface and then tested, are not full overwrites (s19b).
 *   s20  auto at RECOMP_FPS_LIMIT=0: no flip model, no show event.
 *   s25  flips 2 ticks apart (a game below 60): the next window is expected
 *        shown, not logged and drawn late (s25b with auto).
 *   s26  auto under an outside limit of 45 (1-1-2) and 30 (s26b): every
 *        window forecast right once warmed up; s26c: the limit changes
 *        60 -> 45 -> 30 -> 60 and the forecast settles on each.
 *   s27  RECOMP_GPU_RASTER_PRESENT_HZ=20 with a Present that blocks 45 ms:
 *        still ~20 shows a second.
 *   s22  The boundary a full-screen opaque draw over P (no clear), and a
 *        target overwritten whole by a draw then read in the same window:
 *        both pass the coverage test, so skipped windows are dropped (5 at
 *        L=30), none drawn. Taken for partial writes, no window ever ends
 *        (s22) or the target read after its draw is not self-contained and
 *        every skipped window is drawn (s22b: the boundary a clear).
 *   s23  L=20, two skipped windows in a row writing a target the next frame
 *        reads: each is drawn at its own boundary, not carried into the next
 *        skipped one -- carried, it would sample a texture the CPU rewrote
 *        meanwhile (a stale texture) and one drawing would stand for two.
 *   s24  auto with the real waitable on a hidden window, frames back to back:
 *        at times the waitable is not signalled and the show is held back as
 *        "not ready (presenter)" -- pace() never waits for it (waiting, the
 *        executor would stall at the outside limiter's rate).
 *
 * Harness as nv2a_gpu_backend_test.c; Windows only. */
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"
#include "d3d/nv2a_gpu_d3d11.h"

#define SURF(i) (0x10000u + (uint32_t)(i) * 0x10000u)
#define MEM_BYTES 0x800000u              /* s16's 2x2 anti-aliased frame: 5 MB at G_AT */
#define POS_AT  0x1A0000u
#define DIFF_AT 0x1B0000u
#define TC_AT   0x1C0000u
#define WHITE_TEX SURF(28)               /* white, linear 64x64 */
#define SEM_AT  (SURF(28) + 0x8000u)
#define P   SURF(0)                      /* shown: 64x64 */
#define F   SURF(1)                      /* the filter's output: 48x64 */
#define A   SURF(2)                      /* atlas: 32x32 */
#define T   SURF(3)                      /* s2's carried target / s8's: 64x64 */
#define ZB  SURF(10)                     /* P's depth buffer */
static uint8_t s_mem[MEM_BYTES];
static HWND s_hwnd;
static int s_counter_flips, s_wakes;

/* ---- what the executor and the back end expect the runtime to provide -- */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowShown(void) {}
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) { s_counter_flips++; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }
void *xbox_FramebufferWindowHandle(void) { return s_hwnd; }
void xbox_FramebufferWindowExternal(int on) { (void)on; }
void xbox_FramebufferWindowFit(uint32_t cw, uint32_t ch, int32_t out[4])
{
    out[0] = out[1] = 0;
    out[2] = (int32_t)cw;
    out[3] = (int32_t)ch;
}

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_sync(void);
extern void nv2a_pb_exec_report(void);
extern void nv2a_pb_exec_test_tick(int step);
extern int nv2a_pb_exec_test_pace_hz(void);
extern uint32_t nv2a_pb_exec_test_shown(void);
extern void nv2a_gpu_test_skip_stats(uint32_t out[16]);
extern void nv2a_gpu_test_present_gate(int g);
extern void nv2a_gpu_test_gpu_busy(int busy);
extern int nv2a_gpu_test_flip_model(void);
extern uint32_t nv2a_gpu_test_presents(void);
extern int nv2a_gpu_test_rt_pixel(uint32_t va, uint32_t x, uint32_t y, uint32_t *argb);
extern int nv2a_gpu_test_have_token(void);
extern uint32_t nv2a_gpu_test_tex_changed(void);
extern int nv2a_gpu_test_present_event(void);
extern int nv2a_gpu_test_gdi_pixel(uint32_t x, uint32_t y, uint32_t *argb);
extern void nv2a_gpu_test_present_delay(int ms);
extern void nv2a_gpu_test_ps_counts(int counts[5]);

enum { ST_SHOWN, ST_SKIPPED, ST_NR_PRESENTER, ST_NR_GPU, ST_LOGGED, ST_DROPPED,
       ST_RP_READY, ST_RP_OBSERVED, ST_RP_NOTSELF, ST_RP_FULL, ST_MP_SKIP, ST_MP_SHOW,
       ST_STALE, ST_MODE, ST_I100, ST_OPS };

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static void mf(uint32_t method, float f) { uint32_t u; memcpy(&u, &f, 4); m(method, u); }

static uint32_t st(int i)
{
    uint32_t s[16];
    nv2a_gpu_test_skip_stats(s);
    return s[i];
}

/* Draws and clears the GPU has executed so far (each picks a pixel shader). */
static int gpu_ops(void)
{
    int c[5];
    nv2a_gpu_test_ps_counts(c);
    return c[0] + c[1] + c[2] + c[3] + c[4];
}

static uint32_t px(uint32_t va, uint32_t x, uint32_t y)
{
    uint32_t v = 0xDEADBEEFu;
    if (!nv2a_gpu_test_rt_pixel(va, x, y, &v))
        return 0xDEADBEEFu;
    return v;
}

static uint32_t col(int i) { return 0xFF000000u | ((uint32_t)(i * 37 + 11) << 16 & 0xFF0000u)
                                  | ((uint32_t)(i * 91 + 5) << 8 & 0xFF00u) | ((uint32_t)i * 19u & 0xFFu); }
static uint32_t acol(int i) { return col(i + 100); }

/* The surface: colour at va, w x h, pitch; depth at ZB for P only. */
static void surface(uint32_t va, uint32_t w, uint32_t h, uint32_t pitch, int zeta)
{
    m(0x020C, pitch | (256u << 16));         /* SET_SURFACE_PITCH: colour, zeta */
    m(0x0200, w << 16);                      /* CLIP_HORIZONTAL */
    m(0x0204, h << 16);                      /* CLIP_VERTICAL */
    m(0x0214, zeta ? ZB : 0);                /* SURFACE_ZETA_OFFSET */
    m(0x0210, va);                           /* SET_SURFACE_COLOR_OFFSET */
}

static void clear(uint32_t flags, uint32_t argb)
{
    m(0x1D90, argb);                         /* SET_COLOR_CLEAR_VALUE */
    m(0x1D8C, 0xFFFFFF00u);                  /* ZSTENCIL_CLEAR_VALUE */
    m(0x1D94, flags);                        /* CLEAR_SURFACE */
}

/* A rectangle (x0,y0)-(x1,y1) on the current surface, fixed function, white
 * diffuse, stage 0 the linear A8R8G8B8 texture at tex (tw x th, tpitch)
 * from texel (u0,v0). Corner moved by `short_by` pixels in from (x1,y1): two
 * triangles that miss that corner. */
static void rect(float x0, float y0, float x1, float y1, uint32_t tex, uint32_t tw,
                 uint32_t th, uint32_t tpitch, float u0, float v0, float short_by)
{
    static const float I[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    const float Pp[6][2] = { {x0,y0},{x1,y0},{x0,y1},{x1,y0},{x1 - short_by,y1 - short_by},{x0,y1} };
    uint32_t i, white = 0xFFFFFFFFu;

    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, I[i]);            /* composite */
    for (i = 0; i < 4; i++)
        mf(0x0A20 + 4 * i, 0.0f);            /* SET_VIEWPORT_OFFSET */
    mf(0x0394, 0.0f);                        /* SET_CLIP_MIN */
    mf(0x0398, 16777215.0f);                 /* SET_CLIP_MAX */
    m(0x1E94, 0);                            /* fixed function */
    for (i = 0; i < 6; i++) {
        float p3[3], tc[2];
        p3[0] = Pp[i][0]; p3[1] = Pp[i][1]; p3[2] = 0.0f;
        tc[0] = Pp[i][0] - x0 + u0; tc[1] = Pp[i][1] - y0 + v0;
        memcpy(s_mem + POS_AT + 12u * i, p3, 12);
        memcpy(s_mem + DIFF_AT + 4u * i, &white, 4);
        memcpy(s_mem + TC_AT + 8u * i, tc, 8);
    }
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));   /* attr 0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));    /* attr 3: D3DCOLOR */
    m(0x1744, TC_AT);   m(0x1784, 2 | (2 << 4) | (8 << 8));    /* attr 9: float2 */
    m(0x1B00, tex);                          /* SET_TEXTURE_OFFSET */
    m(0x1B04, (0x12u << 8) | (1u << 16) | (6u << 20) | (6u << 24));
    m(0x1B10, tpitch << 16);                 /* CONTROL1: pitch */
    m(0x1B1C, (tw << 16) | th);              /* IMAGE_RECT */
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    m(0x1810, (5u << 24) | 0);               /* DRAW_ARRAYS 6 from 0 */
    m(0x17FC, 0);
}

static void blend(int on)
{
    m(0x0304, on);                           /* SET_BLEND_ENABLE */
    m(0x0344, 0x0302);                       /* SRC: SRC_ALPHA */
    m(0x0348, 0x0303);                       /* DST: ONE_MINUS_SRC_ALPHA */
}

static void flip(void) { m(0x0130, 0); }    /* FLIP_STALL */

/* Fallout's frame, frame i: the filter -- still the previous frame's window,
 * the boundary comes after it -- then the frame's own work. */
static void frame_head(void)
{
    surface(F, 48, 64, 192, 0);              /* the filter, from the frame shown last */
    rect(0, 0, 48, 64, P, 64, 64, 256, 0, 0, 0);
}

static void frame_body(int i)
{
    surface(P, 64, 64, 256, 1);
    clear(0xF3, col(i));                     /* the boundary */
    rect(40, 0, 64, 8, WHITE_TEX, 64, 64, 256, 0, 0, 0);   /* the scene */
    surface(A, 32, 32, 128, 0);
    clear(0xF0, acol(i));
    rect(0, 0, 8, 8, WHITE_TEX, 64, 64, 256, 0, 0, 0);
    surface(P, 64, 64, 256, 1);
    rect(0, 32, 32, 64, A, 32, 32, 128, 0, 0, 0);          /* the atlas into P */
    nv2a_pb_exec_sync();
}

static void frame(int i)
{
    frame_head();
    frame_body(i);
}

/* What a shown frame i must look like. */
static void check_frame(int i, int line)
{
    uint32_t a = px(P, 50, 50), b = px(P, 20, 50), c = px(P, 2, 34), d = px(P, 50, 2);
    if (a != col(i) || b != acol(i) || c != 0xFFFFFFFFu || d != 0xFFFFFFFFu) {
        printf("FAIL line %d: frame %d: P(50,50) %08X want %08X, (20,50) %08X want %08X,"
               " (2,34) %08X, (50,2) %08X want white\n", line, i, a, col(i), b, acol(i), c, d);
        s_fail = 1;
    }
}

static int shown_flip(void)
{
    uint32_t before = nv2a_pb_exec_test_shown();
    flip();
    return nv2a_pb_exec_test_shown() != before;
}

static void s1(int L)
{
    int i, shown = 0, last = 0, ok_gaps = 1;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 12; i++) {
        int before, skipped_window, s;
        frame_head();
        before = gpu_ops();
        frame_body(i);
        skipped_window = st(ST_MODE) == 1;
        if (skipped_window)
            CHECK(gpu_ops() == before);      /* not one clear or draw reached the GPU */
        s = shown_flip();
        if (s) {
            if (last && L == 45)
                ok_gaps &= (i - last == 1 || i - last == 2);
            if (L == 30)
                CHECK(i % 2 == 0);
            check_frame(i, __LINE__);
            last = i;
            shown++;
        }
    }
    printf("L=%d: shown %d of 12, pace %d Hz; dropped %u, replayed %u/%u/%u, stale %u,"
           " mispredicted %u/%u, logged %u\n", L, shown, nv2a_pb_exec_test_pace_hz(),
           st(ST_DROPPED), st(ST_RP_READY), st(ST_RP_OBSERVED), st(ST_RP_NOTSELF),
           st(ST_STALE), st(ST_MP_SKIP), st(ST_MP_SHOW), st(ST_LOGGED));
    CHECK(nv2a_pb_exec_test_pace_hz() == 60);
    CHECK(st(ST_STALE) == 0);
    CHECK(st(ST_RP_READY) + st(ST_RP_OBSERVED) + st(ST_RP_NOTSELF) + st(ST_RP_FULL) == 0);
    CHECK(s_counter_flips == 12);
    if (L == 30) {
        CHECK(shown == 6);
        CHECK(st(ST_DROPPED) == 5);
        CHECK(st(ST_LOGGED) > 0);
    } else {
        CHECK(shown == 9);
        CHECK(ok_gaps);
    }
}

/* s2: T carries over -- each frame samples T into P, then clears T to its
 * colour and puts a white 8x8 rectangle at a place of its own. */
static float s2_x(int i) { return (float)(8 + (i % 4) * 12); }

static void s2(void)
{
    int i, shown = 0;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 12; i++) {
        frame_head();                        /* other vertices through the executor's buffer */
        surface(P, 64, 64, 256, 1);
        clear(0xF3, col(i));
        rect(0, 0, 64, 32, T, 64, 64, 256, 0, 0, 0);       /* reads last frame's T */
        surface(T, 64, 64, 256, 0);
        clear(0xF0, acol(i));
        rect(s2_x(i), 0, s2_x(i) + 8, 8, WHITE_TEX, 64, 64, 256, 0, 0, 0);
        nv2a_pb_exec_sync();
        if (shown_flip()) {
            if (i > 1) {
                uint32_t in = px(P, (uint32_t)s2_x(i - 1) + 2, 2);
                uint32_t out = px(P, (uint32_t)s2_x(i - 1) + 10 < 64 ? (uint32_t)s2_x(i - 1) + 10 : 2, 20);
                if (in != 0xFFFFFFFFu || out != acol(i - 1)) {
                    printf("FAIL: frame %d samples T: %08X (want white), %08X (want %08X)\n",
                           i, in, out, acol(i - 1));
                    s_fail = 1;
                }
            }
            shown++;
        }
    }
    printf("s2: shown %d, replayed not self-contained %u, stale %u, dropped %u\n", shown,
           st(ST_RP_NOTSELF), st(ST_STALE), st(ST_DROPPED));
    CHECK(shown == 6);
    CHECK(st(ST_STALE) == 0);
    CHECK(st(ST_RP_NOTSELF) >= 4);
}

static void s3(void)
{
    int i;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 3; i++) {
        frame(i);
        flip();
    }
    /* tick 3 was skipped, its window logged; now only flips */
    CHECK(st(ST_MODE) == 1);
    CHECK(st(ST_OPS) > 0);
    CHECK(shown_flip());                     /* tick 4 */
    CHECK(st(ST_RP_READY) == 1);
    check_frame(3, __LINE__);
    CHECK(!shown_flip());                    /* tick 5 */
    CHECK(shown_flip());                     /* tick 6: nothing new */
    check_frame(3, __LINE__);
    CHECK(st(ST_STALE) == 0);
}

/* s4: every 4th frame from 6 on reads A before it clears it. */
static void s4(void)
{
    int i;
    nv2a_pb_exec_test_tick(1);
    uint32_t dr = 0;
    for (i = 1; i <= 22; i++) {
        int reads_first = i >= 6 && i < 14;
        int s;
        if (i == 15)
            dr = st(ST_DROPPED);
        surface(F, 48, 64, 192, 0);
        rect(0, 0, 48, 64, P, 64, 64, 256, 0, 0, 0);
        surface(P, 64, 64, 256, 1);
        clear(0xF3, col(i));
        if (reads_first)
            rect(0, 0, 32, 32, A, 32, 32, 128, 0, 0, 0);   /* last frame's atlas */
        surface(A, 32, 32, 128, 0);
        clear(0xF0, acol(i));
        surface(P, 64, 64, 256, 1);
        rect(0, 32, 32, 64, A, 32, 32, 128, 0, 0, 0);
        nv2a_pb_exec_sync();
        s = shown_flip();
        if (s && reads_first) {
            uint32_t v = px(P, 4, 4);
            printf("s4: frame %d reads the atlas first: %08X (frame %d's %08X), stale %u\n",
                   i, v, i - 1, acol(i - 1), st(ST_STALE));
            if (i > 6)
                CHECK(v == acol(i - 1));     /* drawn after the miss */
        }
        if (s)
            CHECK(px(P, 20, 50) == acol(i));
    }
    printf("s4: stale %u, windows dropped after the atlas is cleared first again: %u\n",
           st(ST_STALE), st(ST_DROPPED) - dr);
    CHECK(st(ST_STALE) == 1);
    /* the flag is the last shown window's, not for good: once the atlas is
     * cleared first again, its skipped windows are dropped again */
    CHECK(st(ST_DROPPED) - dr >= 3);
}

/* s21: Fallout's way into a loading screen and out (Carbon's load at 45 in
 * boot_check): gameplay, then frames that show the filter's output F, then
 * gameplay again. The switch may cost a stale read (F was dropped in the
 * window before it was first shown -- the one miss of the guess), but once
 * the shown windows behave as before, skipped ones are dropped again. A
 * stale read kept a target "not self-contained" for good, and from Carbon's
 * load on every skipped window was drawn. */
static void s21(void)
{
    int i;
    uint32_t dr = 0, rp = 0;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 30; i++) {
        if (i == 21) {
            dr = st(ST_DROPPED);
            rp = st(ST_RP_READY) + st(ST_RP_OBSERVED) + st(ST_RP_NOTSELF) + st(ST_RP_FULL);
        }
        if (i > 10 && i <= 14) {             /* the loading screen: F is shown */
            surface(P, 64, 64, 256, 1);
            clear(0xF3, col(i));
            frame_head();
            nv2a_pb_exec_sync();
            flip();
            continue;
        }
        frame(i);
        if (shown_flip() && i > 20)
            check_frame(i, __LINE__);
    }
    printf("s21: stale %u; after the load dropped %u, replayed %u\n", st(ST_STALE),
           st(ST_DROPPED) - dr,
           st(ST_RP_READY) + st(ST_RP_OBSERVED) + st(ST_RP_NOTSELF) + st(ST_RP_FULL) - rp);
    CHECK(st(ST_STALE) <= 2);
    CHECK(st(ST_DROPPED) - dr >= 4);
    CHECK(st(ST_RP_READY) + st(ST_RP_OBSERVED) + st(ST_RP_NOTSELF) + st(ST_RP_FULL) - rp == 0);
}

static void s5(void)
{
    static const int steps[8] = { 1, 1, 1, 2, 1, 3, 1, 1 };
    static const int want[8]  = { 0, 1, 0, 1, 1, 1, 1, 0 };   /* ticks 1,2,3,5,6,9,10,11 */
    int i;
    for (i = 0; i < 8; i++) {
        int s;
        nv2a_pb_exec_test_tick(steps[i]);
        frame(i + 1);
        s = shown_flip();
        CHECK(s == want[i]);
        if (s)
            check_frame(i + 1, __LINE__);
    }
    CHECK(st(ST_RP_READY) >= 1);             /* tick 9 came where the forecast said skip */
    CHECK(st(ST_STALE) == 0);
}

static void wake(void) { s_wakes++; }

static void s6(void)
{
    int i;
    uint32_t v;
    nv2a_pb_set_semaphore_target(SEM_AT);
    nv2a_pb_set_wake(wake);
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 12; i++) {
        frame(i);
        m(0x1D70, (uint32_t)i);              /* BACK_END_WRITE_SEMAPHORE_RELEASE */
        m(0x0100, 5);                        /* NO_OPERATION 5 */
        flip();
    }
    memcpy(&v, s_mem + SEM_AT, 4);
    CHECK(v == 12);
    CHECK(s_wakes == 12);
    CHECK(s_counter_flips == 12);
    CHECK(nv2a_pb_exec_test_shown() == 6);
}

static void s7(int L)
{
    int i;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 12; i++) {
        frame(i);
        CHECK(shown_flip());
        check_frame(i, __LINE__);
    }
    CHECK(nv2a_pb_exec_test_pace_hz() == L);
    CHECK(st(ST_SHOWN) + st(ST_SKIPPED) == 0);   /* pace never asked */
}

/* s8: T is drawn first each frame by a quad that misses its far corner (or
 * is blended): not a full overwrite, so a skipped window writing it is drawn. */
static void s8(int blended)
{
    int i;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 12; i++) {
        surface(P, 64, 64, 256, 1);
        clear(0xF3, col(i));
        surface(T, 64, 64, 256, 0);
        blend(blended);
        rect(0, 0, 64, 64, WHITE_TEX, 64, 64, 256, 0, 0, blended ? 0.0f : 0.75f);
        blend(0);
        surface(P, 64, 64, 256, 1);
        rect(0, 0, 32, 32, T, 64, 64, 256, 0, 0, 0);
        nv2a_pb_exec_sync();
        flip();
    }
    printf("s8 (%s): replayed not self-contained %u, dropped %u\n",
           blended ? "blended" : "short of a corner", st(ST_RP_NOTSELF), st(ST_DROPPED));
    CHECK(st(ST_RP_NOTSELF) >= 4);
}

static void s9(void)
{
    int i;
    uint32_t obs;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 3; i++) {
        frame(i);
        flip();
    }
    /* tick 4 shown; frame 5 is a skipped window */
    frame(4);
    flip();
    surface(F, 48, 64, 192, 0);
    rect(0, 0, 48, 64, P, 64, 64, 256, 0, 0, 0);
    surface(P, 64, 64, 256, 1);
    clear(0xF3, col(5));
    CHECK(st(ST_MODE) == 1);
    obs = st(ST_RP_OBSERVED);
    m(0x17C8, 0);                            /* CLEAR_REPORT_VALUE: nothing counted logged */
    CHECK(st(ST_RP_OBSERVED) == obs);
    CHECK(st(ST_MODE) == 1);
    m(0x17CC, 1);                            /* SET_ZPASS_PIXEL_COUNT_ENABLE */
    rect(40, 0, 64, 8, WHITE_TEX, 64, 64, 256, 0, 0, 0);
    m(0x17CC, 0);
    m(0x17C8, 0);                            /* a count from a logged draw */
    CHECK(st(ST_RP_OBSERVED) == obs + 1);
    CHECK(st(ST_MODE) == 0);
    flip();                                  /* tick 5, hidden */
    frame(6);                                /* boundary: tick 6's window is SHOW */
    flip();
    frame(7);                                /* SKIP again */
    CHECK(st(ST_MODE) == 1);
    obs = st(ST_RP_OBSERVED);
    _putenv("RECOMP_FB_DUMP=nv2a_gpu_skip_no_such_dir/f_");
    nv2a_pb_exec_report();                   /* sync(READ): a writeback */
    _putenv("RECOMP_FB_DUMP=");
    CHECK(st(ST_RP_OBSERVED) == obs + 1);
    CHECK(shown_flip() == 0);
}

static void s10(void)
{
    int i;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 6; i++) {
        frame(i);
        flip();
    }
    /* lost at the flip RECOMP_GPU_RASTER_LOSE_AT names: the executor draws now */
    surface(P, 64, 64, 256, 0);
    clear(0xF0, 0xFF123456u);
    {
        uint32_t v;
        memcpy(&v, s_mem + P, 4);
        CHECK(v == 0xFF123456u);
    }
    flip();
    printf("s10: after the loss the pacer waits for %d Hz\n", nv2a_pb_exec_test_pace_hz());
    CHECK(nv2a_pb_exec_test_pace_hz() != 60 || getenv("S10_AUTO"));
}

static int show_soon(int i);

static void s11(void)
{
    int i;
    uint32_t p0;
    nv2a_pb_exec_test_tick(1);
    CHECK(s_hwnd != NULL);
    nv2a_gpu_test_present_gate(1);
    frame(1);
    CHECK(shown_flip());
    CHECK(nv2a_gpu_test_flip_model());
    p0 = nv2a_gpu_test_presents();
    nv2a_gpu_test_present_gate(0);           /* an outside limiter holding every buffer */
    for (i = 2; i <= 13; i++) {
        frame(i);
        CHECK(!shown_flip());
    }
    CHECK(nv2a_gpu_test_presents() == p0);   /* nothing Presented: nobody waited */
    CHECK(st(ST_NR_PRESENTER) == 12);
    nv2a_gpu_test_present_gate(1);
    i = show_soon(14);
    CHECK(i >= 14);
    check_frame(i, __LINE__);                /* the newest, not the oldest held back */
    CHECK(nv2a_gpu_test_presents() == p0 + 1);
}

static void s12(void)
{
    int i;
    nv2a_pb_exec_test_tick(1);
    nv2a_gpu_test_present_gate(1);
    frame(1);
    CHECK(shown_flip());
    nv2a_gpu_test_gpu_busy(1);
    for (i = 2; i <= 5; i++) {
        frame(i);
        CHECK(!shown_flip());
    }
    CHECK(st(ST_NR_GPU) == 4);
    CHECK(st(ST_NR_PRESENTER) == 0);
    nv2a_gpu_test_gpu_busy(0);
    /* The real event now: WARP finishes the frame shown at tick 1 on its own
     * threads, so a show may wait a few frames for it. */
    for (i = 6; i < 200; i++) {
        frame(i);
        if (shown_flip())
            break;
        Sleep(5);
    }
    CHECK(i < 200);
    check_frame(i, __LINE__);
}

/* Frames until one is shown (the GPU's real event gates it too); its number. */
static int show_soon(int i)
{
    for (; i < 400; i++) {
        frame(i);
        if (shown_flip())
            return i;
        Sleep(5);
    }
    return -1;
}

static void s13(void)
{
    int i;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 40; i++) {
        nv2a_gpu_test_present_gate(i % 4 == 0);
        frame(i);
        if (shown_flip()) {
            CHECK(i % 4 == 0);
            check_frame(i, __LINE__);
        }
    }
    printf("s13: forecast interval %.2f ticks, mispredicted %u/%u, replayed on show %u\n",
           st(ST_I100) / 100.0, st(ST_MP_SKIP), st(ST_MP_SHOW), st(ST_RP_READY));
    CHECK(st(ST_I100) >= 330 && st(ST_I100) <= 420);
    /* Ready two ticks after a show: the forecast said skip, the window was
     * logged -- it is drawn first and shown right. */
    {
        uint32_t rp = st(ST_RP_READY);       /* the warm-up's own are not this check's */
        nv2a_gpu_test_present_gate(0);
        frame(41);
        CHECK(!shown_flip());
        nv2a_gpu_test_present_gate(1);
        frame(42);
        CHECK(st(ST_MODE) == 1);
        Sleep(100);                          /* WARP done with the frame shown at 40 */
        CHECK(shown_flip());
        CHECK(st(ST_RP_READY) == rp + 1);
    }
    check_frame(42, __LINE__);
    CHECK(st(ST_STALE) == 0);
}

/* s14: Fallout's frame as the game draws it (stage3 traces, boot_check at
 * 30/45): the filter is a program draw with depth writes on and the test off
 * into a surface with zeta 0 (so are the atlas's points), a full-screen quad
 * over P after its clear F3, and a loading screen first that showed F. Every
 * skipped window must be dropped: the zeta-0 buffer is written and never
 * tested (a write nothing reads), P is only what the last flips showed (not
 * the loading screen's F for good), and only the first full overwrite of P
 * after a flip is a boundary. Before, every skipped window was drawn ("not
 * self-contained") and frame skip saved no GPU work at all. */
static void s14_frame(int i)
{
    surface(F, 48, 64, 192, 0);              /* zeta 0 */
    m(0x035C, 1);                            /* DEPTH_MASK on, the test off */
    rect(0, 0, 48, 64, P, 64, 64, 256, 0, 0, 0);
    surface(P, 64, 64, 256, 1);
    clear(0xF3, col(i));
    m(0x035C, 0);                            /* the quad leaves depth alone */
    rect(0, 0, 64, 64, WHITE_TEX, 64, 64, 256, 0, 0, 0);   /* P overwritten whole again */
    surface(A, 32, 32, 128, 0);
    clear(0xF0, acol(i));
    m(0x035C, 1);
    rect(0, 0, 8, 8, WHITE_TEX, 64, 64, 256, 0, 0, 0);     /* the atlas's points */
    m(0x035C, 0);
    surface(P, 64, 64, 256, 1);
    rect(0, 32, 32, 64, A, 32, 32, 128, 0, 0, 0);
    nv2a_pb_exec_sync();
}

static void s14(void)
{
    int i, shown = 0;
    uint32_t rp0, dr0;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 3; i++) {               /* the loading screen: F is what is shown */
        surface(P, 64, 64, 256, 1);
        clear(0xF3, col(i));
        surface(F, 48, 64, 192, 0);
        rect(0, 0, 48, 64, P, 64, 64, 256, 0, 0, 0);
        nv2a_pb_exec_sync();
        flip();
    }
    for (i = 4; i <= 9; i++) {               /* into the level */
        s14_frame(i);
        flip();
    }
    rp0 = st(ST_RP_READY) + st(ST_RP_OBSERVED) + st(ST_RP_NOTSELF) + st(ST_RP_FULL);
    dr0 = st(ST_DROPPED);
    for (i = 10; i <= 33; i++) {
        s14_frame(i);
        if (shown_flip()) {
            uint32_t a = px(P, 50, 50), b = px(P, 20, 50);
            if (a != 0xFFFFFFFFu || b != acol(i)) {
                printf("FAIL: frame %d: P(50,50) %08X want white, (20,50) %08X want %08X\n",
                       i, a, b, acol(i));
                s_fail = 1;
            }
            shown++;
        }
    }
    printf("s14: shown %d of 24; in the level dropped %u, replayed %u; stale %u\n", shown,
           st(ST_DROPPED) - dr0,
           st(ST_RP_READY) + st(ST_RP_OBSERVED) + st(ST_RP_NOTSELF) + st(ST_RP_FULL) - rp0,
           st(ST_STALE));
    CHECK(shown == 12);
    CHECK(st(ST_DROPPED) - dr0 == 12);
    CHECK(st(ST_RP_READY) + st(ST_RP_OBSERVED) + st(ST_RP_NOTSELF) + st(ST_RP_FULL) == rp0);
    CHECK(st(ST_STALE) == 0);
}

/* s15: auto with the real waitable of a flip-model chain on a hidden window
 * (no test gate): frames go on, some are Presented, nobody hangs. A waitable
 * taken while the GPU is busy is kept for the next show, not lost (lost, the
 * chain would never be ready again at latency 1). */
static void s15(void)
{
    int i, kept = 0;
    uint32_t p0;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 30; i++) {
        frame(i);
        flip();
        Sleep(2);
    }
    p0 = nv2a_gpu_test_presents();
    printf("s15: %u presents in 30 frames, flip model %d\n", p0, nv2a_gpu_test_flip_model());
    CHECK(nv2a_gpu_test_flip_model());
    CHECK(p0 > 0);
    nv2a_gpu_test_gpu_busy(1);
    for (; i <= 200 && !kept; i++) {         /* until the waitable is signalled once */
        frame(i);
        CHECK(!shown_flip());
        kept = nv2a_gpu_test_have_token();
        Sleep(5);
    }
    CHECK(kept);
    frame(i);
    CHECK(!shown_flip());
    CHECK(nv2a_gpu_test_have_token());       /* still held: nothing Presented */
    nv2a_gpu_test_gpu_busy(0);
    i = show_soon(i + 1);
    CHECK(i > 0);
    CHECK(nv2a_gpu_test_presents() == p0 + 1);
    CHECK(!nv2a_gpu_test_have_token());      /* spent by that Present */
}

/* s16: a frame only in guest RAM (no draw since the start: a movie the CPU
 * writes) on the flip-model chain: GDI cannot draw into it, so it goes up
 * into a texture and through the present shader -- 32-bit, the 2x-wide
 * anti-aliased 32-bit (every other pixel, every other row at aa_sy 2) and
 * R5G6B5, as fb_present.c's GDI path reads them. The 2x2 one has the AA bits
 * in SET_SURFACE_FORMAT (2 = "square offset 4", aa_sy 2): pixel (x,y) is at
 * (2x,2y) and the odd rows hold something else -- read at the row pitch
 * instead of twice it, every other row of the picture comes out wrong. */
#define G_AT 0x240000u

static void s16(void)
{
    static const uint32_t pitches[4] = { 640 * 4, 640 * 8, 640 * 2, 640 * 8 };
    /* A8R8G8B8, A8R8G8B8, R5G6B5, A8R8G8B8 2x2 anti-aliased */
    static const uint32_t fmts[4] = { 0x0028, 0x0028, 0x0023, 0x2028 };
    int f;
    uint32_t p0 = 0;
    nv2a_pb_exec_test_tick(1);
    nv2a_gpu_test_present_gate(1);
    for (f = 0; f < 4; f++) {
        uint32_t x, y, pitch = pitches[f], v = 0, want;
        for (y = 0; y < 480; y++)
            for (x = 0; x < 640; x++) {
                uint32_t c = 0xFF000000u | (x << 14 & 0xFF0000u) | ((y * 3u) << 8 & 0xFF00u) | ((x ^ y) & 0xFFu);
                uint8_t *row = s_mem + G_AT + y * pitch * (f == 3 ? 2u : 1u);
                if (f == 0)
                    memcpy(row + 4 * x, &c, 4);
                else if (f == 1)
                    memcpy(row + 8 * x, &c, 4), memcpy(row + 8 * x + 4, "\0\0\0\0", 4);
                else if (f == 3) {           /* the odd row below: another colour */
                    uint32_t o = ~c | 0xFF000000u;
                    memcpy(row + 8 * x, &c, 4), memcpy(row + 8 * x + 4, &o, 4);
                    memcpy(row + pitch + 8 * x, &o, 4), memcpy(row + pitch + 8 * x + 4, &o, 4);
                } else {
                    uint16_t s = (uint16_t)(((c >> 8) & 0xF800u) | ((c >> 5) & 0x07E0u) | ((c >> 3) & 0x1Fu));
                    memcpy(row + 2 * x, &s, 2);
                }
            }
        m(0x0208, fmts[f]);                  /* SET_SURFACE_FORMAT */
        m(0x020C, pitch);
        m(0x0200, 640u << 16);
        m(0x0204, 480u << 16);
        m(0x0210, G_AT);                     /* nothing drawn: the flip shows guest RAM */
        p0 = nv2a_gpu_test_presents();
        CHECK(shown_flip());
        CHECK(nv2a_gpu_test_presents() == p0 + 1);
        for (y = 0; y < 480; y += 97)
            for (x = 0; x < 640; x += 131) {
                uint32_t c = 0xFF000000u | (x << 14 & 0xFF0000u) | ((y * 3u) << 8 & 0xFF00u) | ((x ^ y) & 0xFFu);
                want = c;
                if (f == 2) {                /* expand565 */
                    uint32_t s = ((c >> 8) & 0xF800u) | ((c >> 5) & 0x07E0u) | ((c >> 3) & 0x1Fu);
                    want = 0xFF000000u | ((s & 0xF800u) << 8) | ((s & 0x07E0u) << 5) | ((s & 0x1Fu) << 3);
                }
                if (!nv2a_gpu_test_gdi_pixel(x, y, &v) || v != want) {
                    printf("FAIL: format %d: (%u,%u) %08X want %08X\n", f, x, y, v, want);
                    s_fail = 1;
                    x = 640; y = 480;
                }
            }
    }
    CHECK(nv2a_gpu_test_flip_model());
}

/* s17: RECOMP_GPU_RASTER_PRESENT_HZ=20 (a limiter acted out): over ~0.6 s of
 * frames at most about 20 a second are shown, the rest held back as "not
 * ready (presenter)". */
static void s17(void)
{
    int i, shown = 0;
    DWORD t0 = GetTickCount(), ms;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 60; i++) {
        frame(i);
        shown += shown_flip();
        Sleep(10);
    }
    ms = GetTickCount() - t0;
    printf("s17: %d shown in %lu ms at 20 Hz, presenter held back %u\n", shown,
           (unsigned long)ms, st(ST_NR_PRESENTER));
    CHECK(shown >= 1);
    CHECK((double)shown <= (double)ms * 20.0 / 1000.0 + 2.0);
    /* and about that many: every 1/20 s slot that had a flip gets a show (the
     * slot counted from Present's return lost about one in four) */
    CHECK((double)shown >= (double)ms * 20.0 / 1000.0 - 3.0);
    CHECK(st(ST_NR_PRESENTER) > 0);
}

/* s25: a game that cannot hold 60 flips every 2 ticks. At L=30 (s25) every
 * flip is shown, and the next window must be expected shown too -- cadence
 * for tick + 1 said hidden, so every window was logged and then drawn at its
 * show ("shown a skipped window"), up to 2 GB of journal in 10 s at Ruby's
 * bar, which kept the game below 60. With auto (s25b) and the presenter
 * ready every 4th tick, the forecast learns 4 and the window before each
 * ready tick is drawn, not logged. */
static void s25(int autom)
{
    int i, shown = 0;
    uint32_t mp0 = 0, rp0 = 0, lg0 = 0;
    nv2a_pb_exec_test_tick(2);
    for (i = 1; i <= 40; i++) {
        if (i == 21) {
            mp0 = st(ST_MP_SKIP) + st(ST_MP_SHOW);
            rp0 = st(ST_RP_READY);
            lg0 = st(ST_LOGGED);
        }
        if (autom)
            nv2a_gpu_test_present_gate(i % 2 == 0);   /* ticks 4, 8, ...: every 4th tick */
        frame(i);
        if (autom && i % 2 == 0)
            Sleep(30);                       /* WARP done with the last shown frame */
        if (shown_flip()) {
            shown++;
            check_frame(i, __LINE__);
        }
    }
    printf("s25%s: shown %d of 40 flips 2 ticks apart; in the last 20: mispredicted %u,"
           " drawn after being logged %u, logged ops %u\n", autom ? "b" : "", shown,
           st(ST_MP_SKIP) + st(ST_MP_SHOW) - mp0, st(ST_RP_READY) - rp0, st(ST_LOGGED) - lg0);
    CHECK(shown == (autom ? 20 : 40));
    CHECK(st(ST_MP_SKIP) + st(ST_MP_SHOW) - mp0 == 0);
    CHECK(st(ST_RP_READY) - rp0 == 0);
    if (!autom)
        CHECK(st(ST_LOGGED) - lg0 == 0);
}

/* s18: a skipped window that must be drawn later (T carries over, as s2)
 * samples a texture X from guest RAM; the CPU rewrites X before it is drawn:
 * counted as a stale texture (said once). X left alone: none. */
#define X_TEX SURF(4)

static void s18(void)
{
    int i;
    nv2a_pb_exec_test_tick(1);
    for (i = 0; i < 64 * 64; i++)
        memcpy(s_mem + X_TEX + 4u * (uint32_t)i, "\x11\x22\x33\xFF", 4);
    for (i = 1; i <= 12; i++) {
        surface(P, 64, 64, 256, 1);
        clear(0xF3, col(i));
        rect(0, 0, 64, 32, T, 64, 64, 256, 0, 0, 0);       /* reads last frame's T */
        surface(T, 64, 64, 256, 0);
        clear(0xF0, acol(i));
        rect(0, 0, 8, 8, X_TEX, 64, 64, 256, 0, 0, 0);     /* X into T */
        surface(P, 64, 64, 256, 1);          /* P drawn last: the flip shows P, not T */
        rect(40, 40, 64, 64, WHITE_TEX, 64, 64, 256, 0, 0, 0);
        nv2a_pb_exec_sync();
        flip();
        if (i == 7)                          /* tick 7: a skipped window holds X */
            memcpy(s_mem + X_TEX, "\x44\x55\x66\xFF", 4);
    }
    printf("s18: stale textures %u, replayed not self-contained %u\n",
           nv2a_gpu_test_tex_changed(), st(ST_RP_NOTSELF));
    CHECK(st(ST_RP_NOTSELF) >= 4);
    CHECK(nv2a_gpu_test_tex_changed() == 1);
}

/* s19: what counts as a full overwrite. T drawn whole by ONE/ZERO through
 * MIN (it compares with what is there), and a depth buffer cleared over part
 * of the surface then tested: neither is self-contained, so their skipped
 * windows are drawn. */
static void s19(int depth)
{
    int i;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 12; i++) {
        surface(P, 64, 64, 256, 1);
        clear(0xF3, col(i));
        if (!depth) {
            surface(T, 64, 64, 256, 0);
            m(0x0304, 1);                    /* SET_BLEND_ENABLE */
            m(0x0344, 1);                    /* ONE */
            m(0x0348, 0);                    /* ZERO */
            m(0x0350, 0x8007);               /* MIN */
            rect(0, 0, 64, 64, WHITE_TEX, 64, 64, 256, 0, 0, 0);
            m(0x0304, 0);
            m(0x0350, 0x8006);
        } else {
            /* T's colour cleared whole (self-contained), its depth buffer only
             * over a 64x32 clip and then tested over all of T */
            surface(T, 64, 64, 256, 0);
            m(0x0214, SURF(11));             /* T's own depth buffer */
            clear(0xF0, acol(i));
            m(0x0204, 32u << 16);            /* CLIP_VERTICAL: the top half */
            clear(0x03, 0);
            m(0x0204, 64u << 16);
            m(0x030C, 1);                    /* DEPTH_TEST_ENABLE */
            m(0x0354, 0x203);                /* LEQUAL */
            rect(0, 0, 64, 64, WHITE_TEX, 64, 64, 256, 0, 0, 0);
            m(0x030C, 0);
        }
        surface(P, 64, 64, 256, 1);
        if (!depth)
            rect(0, 0, 32, 32, T, 64, 64, 256, 0, 0, 0);
        nv2a_pb_exec_sync();
        flip();
    }
    printf("s19 (%s): replayed not self-contained %u, dropped %u\n",
           depth ? "a partial depth clear, tested" : "ONE/ZERO through MIN",
           st(ST_RP_NOTSELF), st(ST_DROPPED));
    CHECK(st(ST_RP_NOTSELF) >= 4);
}

/* s20: auto at RECOMP_FPS_LIMIT=0 (no limit): frame skip is not active, so
 * neither is the flip model nor its show event -- the bitblt chain, as before. */
static void s20(void)
{
    int i;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 4; i++) {
        frame(i);
        CHECK(shown_flip());
    }
    CHECK(!nv2a_gpu_test_flip_model());
    CHECK(!nv2a_gpu_test_present_event());
    CHECK(nv2a_gpu_test_presents() == 4);
}

/* s22: frame i with the boundary a full-screen opaque draw over P (white),
 * or (s22b) Fallout's clear. Either way the atlas A is cleared, T is
 * overwritten whole by a draw sampling A, and P samples T in the same window
 * -- a read after a full overwrite, self-contained. */
static void s22_frame(int i, int draw_boundary)
{
    frame_head();
    surface(P, 64, 64, 256, 1);
    if (draw_boundary)
        rect(0, 0, 64, 64, WHITE_TEX, 64, 64, 256, 0, 0, 0);   /* the boundary */
    else
        clear(0xF3, col(i));                 /* the boundary */
    surface(A, 32, 32, 128, 0);
    clear(0xF0, acol(i));
    surface(T, 64, 64, 256, 0);
    rect(0, 0, 64, 64, A, 32, 32, 128, 0, 0, 0);           /* T overwritten whole */
    surface(P, 64, 64, 256, 1);
    rect(0, 32, 32, 64, T, 64, 64, 256, 0, 0, 0);          /* ... and read */
    nv2a_pb_exec_sync();
}

static void s22(int draw_boundary)
{
    int i, shown = 0;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 12; i++) {
        s22_frame(i, draw_boundary);
        if (shown_flip()) {
            uint32_t a = px(P, 50, 50), b = px(P, 20, 50);
            uint32_t wa = draw_boundary ? 0xFFFFFFFFu : col(i);
            if (a != wa || b != acol(i)) {
                printf("FAIL: frame %d: P(50,50) %08X want %08X, (20,50) %08X want %08X\n",
                       i, a, wa, b, acol(i));
                s_fail = 1;
            }
            shown++;
        }
    }
    printf("s22 (%s): shown %d, dropped %u, replayed %u/%u/%u, stale %u\n",
           draw_boundary ? "a draw for the boundary" : "a clear for the boundary", shown,
           st(ST_DROPPED), st(ST_RP_READY), st(ST_RP_OBSERVED), st(ST_RP_NOTSELF), st(ST_STALE));
    CHECK(shown == 6);
    CHECK(st(ST_DROPPED) == 5);
    CHECK(st(ST_RP_READY) + st(ST_RP_OBSERVED) + st(ST_RP_NOTSELF) + st(ST_RP_FULL) == 0);
    CHECK(st(ST_STALE) == 0);
}

/* s23: L=20 (one show in three ticks: two skipped windows in a row). As s2,
 * each frame samples last frame's T into P, then clears T and draws the
 * guest RAM texture X into it -- T is not self-contained, so every skipped
 * window is drawn at the boundary that ends it. In the second skipped window
 * of a pair, after its boundary, the CPU rewrites X: the first one was drawn
 * already (with what X was), so there is no stale texture. */
static void s23(void)
{
    int i, shown = 0, changed = 0, ends_skipped = 0;
    nv2a_pb_exec_test_tick(1);
    for (i = 0; i < 64 * 64; i++)
        memcpy(s_mem + X_TEX + 4u * (uint32_t)i, "\x11\x22\x33\xFF", 4);
    for (i = 1; i <= 18; i++) {
        int prev_skipped;
        frame_head();
        prev_skipped = st(ST_MODE) == 1 && st(ST_OPS) > 0;   /* the window this boundary ends */
        surface(P, 64, 64, 256, 1);
        clear(0xF3, col(i));                 /* the boundary */
        ends_skipped += prev_skipped;
        rect(0, 0, 64, 32, T, 64, 64, 256, 0, 0, 0);       /* reads last frame's T */
        if (prev_skipped && st(ST_MODE) == 1 && !changed) {
            memcpy(s_mem + X_TEX, "\x44\x55\x66\xFF", 4);
            changed = i;
        }
        surface(T, 64, 64, 256, 0);
        clear(0xF0, acol(i));
        rect(s2_x(i), 0, s2_x(i) + 8, 8, X_TEX, 64, 64, 256, 0, 0, 0);
        surface(P, 64, 64, 256, 1);          /* P drawn last: the flip shows P, not T */
        rect(40, 40, 64, 64, WHITE_TEX, 64, 64, 256, 0, 0, 0);
        nv2a_pb_exec_sync();
        if (shown_flip()) {
            if (i > 1) {
                uint32_t out = px(P, (uint32_t)s2_x(i - 1) + 10 < 64 ? (uint32_t)s2_x(i - 1) + 10 : 2, 20);
                if (out != acol(i - 1)) {
                    printf("FAIL: frame %d samples T: %08X (want %08X)\n", i, out, acol(i - 1));
                    s_fail = 1;
                }
            }
            shown++;
        }
    }
    printf("s23: shown %d of 18, X rewritten in frame %d; boundaries ending a skipped window %d,"
           " replayed not self-contained %u, stale textures %u, stale %u\n", shown, changed,
           ends_skipped, st(ST_RP_NOTSELF), nv2a_gpu_test_tex_changed(), st(ST_STALE));
    CHECK(shown == 6);
    CHECK(changed > 0);
    CHECK(ends_skipped >= 10);
    CHECK(st(ST_RP_NOTSELF) == (uint32_t)ends_skipped);   /* each drawn at its own boundary */
    CHECK(nv2a_gpu_test_tex_changed() == 0);
    CHECK(st(ST_STALE) == 0);
}

/* s24: auto with the real waitable, no test gate, frames back to back (no
 * sleep): right after a Present the waitable is not signalled yet, and
 * pace() must say "not ready (presenter)" then rather than wait for it. */
static void s24(void)
{
    int i;
    DWORD worst = 0;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= 120; i++) {
        DWORD t0;
        frame(i);
        t0 = GetTickCount();
        flip();
        if (GetTickCount() - t0 > worst)
            worst = GetTickCount() - t0;
    }
    printf("s24: %u presents in 120 frames, not ready (presenter) %u, (gpu) %u; slowest"
           " flip %lu ms\n", nv2a_gpu_test_presents(), st(ST_NR_PRESENTER), st(ST_NR_GPU),
           (unsigned long)worst);
    CHECK(nv2a_gpu_test_flip_model());
    CHECK(nv2a_gpu_test_presents() > 0);
    CHECK(st(ST_NR_PRESENTER) > 0);
}

/* s26: auto under an outside limit of 45 (or 30, s26b): the presenter takes a
 * frame when the limiter's slot -- floor(tick * hz / 60) -- moved on since the
 * last show, so 45 goes 1-1-2. After a warm-up every window is forecast right:
 * none logged and then shown ("shown a skipped window"), none drawn and then
 * held back, 3 of 4 (1 of 2) shown. The interval average this replaced swung
 * with 1-1-2: 150 of 375 shows a 10 s at 45 were logged windows drawn late. */
static void s26(int hz)
{
    int i, shown = 0, n = 120;
    uint32_t mp0 = 0, rp0 = 0;
    uint64_t last = 0;
    nv2a_pb_exec_test_tick(1);
    for (i = 1; i <= n; i++) {
        uint64_t t = (uint64_t)i;            /* one tick a flip */
        if (i == n / 2 + 1) {
            mp0 = st(ST_MP_SKIP) + st(ST_MP_SHOW);
            rp0 = st(ST_RP_READY);
            shown = 0;
        }
        nv2a_gpu_test_present_gate(t * (uint64_t)hz / 60 > last * (uint64_t)hz / 60);
        frame(i);
        Sleep(20);                           /* WARP done with the last shown frame */
        if (shown_flip()) {
            last = t;
            shown++;
            check_frame(i, __LINE__);
        }
    }
    printf("s26 at %d: shown %d of the last %d flips; mispredicted %u (shown a skipped window"
           " %u), drawn after being logged %u, rate %.2f\n", hz, shown, n / 2,
           st(ST_MP_SKIP) + st(ST_MP_SHOW) - mp0, st(ST_MP_SKIP), st(ST_RP_READY) - rp0,
           100.0 / st(ST_I100));
    CHECK(shown == n / 2 * hz / 60);
    CHECK(st(ST_MP_SKIP) + st(ST_MP_SHOW) - mp0 == 0);
    CHECK(st(ST_RP_READY) - rp0 == 0);
}

/* s26c: the presenter's rhythm changes under way, 60 -> 45 -> 30 -> 60 shows a
 * second, 160 flips each. Within each the forecast settles: over its last 60
 * flips no window is forecast wrong and the share shown is the rhythm's
 * (60, 45, 30, 60). Coming down from 60 the show credit stands high (one
 * tick's worth over a show); only a flip held back pulls it to just under one
 * and puts the forecast in the slower limiter's phase -- without that every
 * held-back flip of 45 was drawn for nothing, for good. */
static void s26c(void)
{
    static const int hzs[4] = { 60, 45, 30, 60 };
    int ph, i, k = 0, ok = 1;
    uint64_t last = 0;
    nv2a_pb_exec_test_tick(1);
    for (ph = 0; ph < 4; ph++) {
        int hz = hzs[ph], shown = 0;
        uint64_t base = (uint64_t)k;         /* the limiter's slots from here */
        uint32_t mp0 = 0, rp0 = 0;
        for (i = 0; i < 160; i++) {
            uint64_t t = (uint64_t)++k;
            if (i == 100) {
                mp0 = st(ST_MP_SKIP) + st(ST_MP_SHOW);
                rp0 = st(ST_RP_READY);
                shown = 0;
            }
            nv2a_gpu_test_present_gate(last < base
                || (t - base) * (uint64_t)hz / 60 > (last - base) * (uint64_t)hz / 60);
            frame(k);
            Sleep(15);                       /* WARP done with the last shown frame */
            if (shown_flip()) {
                last = t;
                shown++;
            }
        }
        printf("s26c at %d: shown %d of the last 60 flips, mispredicted %u, drawn after being"
               " logged %u, rate %.2f\n", hz, shown, st(ST_MP_SKIP) + st(ST_MP_SHOW) - mp0,
               st(ST_RP_READY) - rp0, 100.0 / st(ST_I100));
        if (shown != hz || st(ST_MP_SKIP) + st(ST_MP_SHOW) - mp0 != 0
            || st(ST_RP_READY) - rp0 != 0)
            ok = 0;
    }
    CHECK(ok);
}

/* s27: RECOMP_GPU_RASTER_PRESENT_HZ=20 with a Present that blocks 45 ms (an
 * outside limiter or a driver sleeping inside it) and flips close together:
 * the slots stay the host clock's, counted when pace() decides -- about 20
 * shows a second all the same. Counted from Present's return, the 50 ms slot
 * a show was decided in was mostly over by then, the next slot counted as
 * used, and every other slot lost (~10 a second). s17 cannot see it: WARP's
 * Present returns at once. */
static void s27(void)
{
    int i, shown = 0;
    DWORD t0 = GetTickCount(), ms;
    nv2a_pb_exec_test_tick(1);
    nv2a_gpu_test_present_delay(45);
    for (i = 1; i <= 120; i++) {
        frame(i);
        shown += shown_flip();
        Sleep(2);
    }
    nv2a_gpu_test_present_delay(0);
    ms = GetTickCount() - t0;
    printf("s27: %d shown of 120 flips in %lu ms at 20 Hz with Present 45 ms longer, presenter"
           " held back %u\n", shown, (unsigned long)ms, st(ST_NR_PRESENTER));
    CHECK((double)shown <= (double)ms * 20.0 / 1000.0 + 2.0);
    /* every slot that had a flip shows one: as many as the slots elapsed, or
     * the flips when they came further apart than a slot */
    CHECK((double)shown >= ((double)ms * 20.0 / 1000.0 < 120.0 ? (double)ms * 20.0 / 1000.0
                                                                 : 120.0) - 3.0);
}

static HWND hidden_window(void)
{
    WNDCLASSW wc;
    RECT r = { 0, 0, 640, 480 };
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"nv2a_gpu_skip_test";
    RegisterClassW(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, wc.lpszClassName, L"nv2a_gpu_skip_test", WS_OVERLAPPEDWINDOW,
                           0, 0, r.right - r.left, r.bottom - r.top, NULL, NULL,
                           wc.hInstance, NULL);
}

int main(int argc, char **argv)
{
    const char *sc = argc > 1 ? argv[1] : "s1";
    int i;

    _putenv("RECOMP_GPU_RASTER=warp");
    if (!strcmp(sc, "s7")) {
        _putenv("RECOMP_FPS_LIMIT=30");
        _putenv("RECOMP_FRAME_SKIP=0");
    } else if (!strcmp(sc, "s7b")) {
        _putenv("RECOMP_FPS_LIMIT=60");
        _putenv("RECOMP_FRAME_SKIP=1");
    } else if (!strcmp(sc, "s1b")) {
        _putenv("RECOMP_FPS_LIMIT=45");
        _putenv("RECOMP_FRAME_SKIP=1");
    } else if (!strcmp(sc, "s11") || !strcmp(sc, "s12") || !strcmp(sc, "s13")
               || !strcmp(sc, "s10b") || !strcmp(sc, "s15") || !strcmp(sc, "s16")
               || !strcmp(sc, "s17") || !strcmp(sc, "s24") || !strcmp(sc, "s25b")
               || !strcmp(sc, "s26") || !strcmp(sc, "s26b")
               || !strcmp(sc, "s26c") || !strcmp(sc, "s27")) {
        _putenv("RECOMP_FPS_LIMIT=60");
        _putenv("RECOMP_FRAME_SKIP=auto");
        s_hwnd = hidden_window();
    } else if (!strcmp(sc, "s20")) {
        _putenv("RECOMP_FPS_LIMIT=0");
        _putenv("RECOMP_FRAME_SKIP=auto");
        s_hwnd = hidden_window();
    } else if (!strcmp(sc, "s23")) {
        _putenv("RECOMP_FPS_LIMIT=20");
        _putenv("RECOMP_FRAME_SKIP=1");
    } else {
        _putenv("RECOMP_FPS_LIMIT=30");
        _putenv("RECOMP_FRAME_SKIP=1");
    }
    if (!strcmp(sc, "s10") || !strcmp(sc, "s10b"))
        _putenv("RECOMP_GPU_RASTER_LOSE_AT=3");
    if (!strcmp(sc, "s10b"))
        _putenv("S10_AUTO=1");
    if (!strcmp(sc, "s17") || !strcmp(sc, "s27"))
        _putenv("RECOMP_GPU_RASTER_PRESENT_HZ=20");
    if (!strcmp(sc, "s24"))                  /* a chain of one frame: back to back, its */
        _putenv("RECOMP_GPU_RASTER_FRAME_LATENCY=1");   /* waitable is not signalled yet */
    if (!nv2a_gpu_d3d11_install()) {
        printf("SKIP: no D3D11 WARP device\n");
        return 0;
    }
    m(0x0208, 0x0028);                       /* SET_SURFACE_FORMAT: A8R8G8B8, Z24S8 */
    for (i = 0; i < 64 * 64; i++)
        memcpy(s_mem + WHITE_TEX + 4u * (uint32_t)i, "\xFF\xFF\xFF\xFF", 4);

    if (!strcmp(sc, "s1")) s1(30);
    else if (!strcmp(sc, "s1b")) s1(45);
    else if (!strcmp(sc, "s2")) s2();
    else if (!strcmp(sc, "s3")) s3();
    else if (!strcmp(sc, "s4")) s4();
    else if (!strcmp(sc, "s5")) s5();
    else if (!strcmp(sc, "s6")) s6();
    else if (!strcmp(sc, "s7")) s7(30);
    else if (!strcmp(sc, "s7b")) s7(60);
    else if (!strcmp(sc, "s8")) { s8(0); }
    else if (!strcmp(sc, "s8b")) { s8(1); }
    else if (!strcmp(sc, "s9")) s9();
    else if (!strcmp(sc, "s10") || !strcmp(sc, "s10b")) s10();
    else if (!strcmp(sc, "s11")) s11();
    else if (!strcmp(sc, "s12")) s12();
    else if (!strcmp(sc, "s13")) s13();
    else if (!strcmp(sc, "s14")) s14();
    else if (!strcmp(sc, "s15")) s15();
    else if (!strcmp(sc, "s16")) s16();
    else if (!strcmp(sc, "s17")) { nv2a_gpu_test_present_gate(1); s17(); }
    else if (!strcmp(sc, "s18")) s18();
    else if (!strcmp(sc, "s19")) s19(0);
    else if (!strcmp(sc, "s19b")) s19(1);
    else if (!strcmp(sc, "s20")) s20();
    else if (!strcmp(sc, "s21")) s21();
    else if (!strcmp(sc, "s22")) s22(1);
    else if (!strcmp(sc, "s22b")) s22(0);
    else if (!strcmp(sc, "s23")) s23();
    else if (!strcmp(sc, "s24")) s24();
    else if (!strcmp(sc, "s25")) s25(0);
    else if (!strcmp(sc, "s25b")) s25(1);
    else if (!strcmp(sc, "s26")) s26(45);
    else if (!strcmp(sc, "s26b")) s26(30);
    else if (!strcmp(sc, "s26c")) s26c();
    else if (!strcmp(sc, "s27")) { nv2a_gpu_test_present_gate(1); s27(); }
    else { printf("unknown scenario %s\n", sc); return 2; }

    printf("nv2a_gpu_skip %s: %s\n", sc, s_fail ? "FAILED" : "OK");
    if (s_hwnd)
        DestroyWindow(s_hwnd);
    return s_fail;
}
