/*
 * vblank_grid.h -- the 60 Hz vblank grid, as pure arithmetic.
 *
 * On the console a flip waits for the vblank, so frame times are whole
 * 1/60 s: a frame that misses its vblank is shown on the next one (39 fps
 * shows as 30), and D3DPRESENT_INTERVAL_TWO means no sooner than 2 vblanks.
 * A limiter that lets a late frame through at once and then spaces the next
 * one by a fixed period instead hands the game 4 + 28 ms pairs at 60 fps and
 * 5 + 61 ms at 30, which broke input at the Tutorial's knife robot.
 *
 * The grid is 60 Hz of the host's performance counter counted from one base;
 * a flip completes on the first tick after it stalls, no sooner than
 * ceil(60/hz) ticks after the previous flip, and a late frame waits for the
 * next tick. The guest's vblank count (kernel_bridge.c) reads the same ticks.
 * Test: tests/nv2a_vblank_grid.
 */
#ifndef VBLANK_GRID_H
#define VBLANK_GRID_H

#include <stdint.h>

#define VBLANK_GRID_HZ 60

/* Ticks between flips: ceil(60/hz), 1 at or above 60 Hz; 0 = no limit (hz <= 0). */
static inline uint64_t vblank_grid_gap(int hz)
{
    return hz <= 0 ? 0 : hz >= VBLANK_GRID_HZ ? 1
                       : (uint64_t)(VBLANK_GRID_HZ + hz - 1) / (uint64_t)hz;
}

/* The tick t falls in: floor((t-base)*60/freq), split so it cannot overflow
 * at any counter rate; t <= base (another thread set base a moment after
 * this one read the counter) is tick 0. */
static inline uint64_t vblank_grid_tick(int64_t t, int64_t base, int64_t freq)
{
    uint64_t d, f = (uint64_t)freq;
    if (t <= base)
        return 0;
    d = (uint64_t)(t - base);
    return d / f * VBLANK_GRID_HZ + d % f * VBLANK_GRID_HZ / f;
}

/* The first counter value in tick k: base + ceil(k*freq/60), split the same way.
 * vblank_grid_tick(vblank_grid_time(k)) == k exactly; no drift over hours. */
static inline int64_t vblank_grid_time(uint64_t k, int64_t base, int64_t freq)
{
    uint64_t f = (uint64_t)freq;
    return base + (int64_t)(k / VBLANK_GRID_HZ * f
                            + (k % VBLANK_GRID_HZ * f + VBLANK_GRID_HZ - 1) / VBLANK_GRID_HZ);
}

/* When a flip stalled at `now` completes: the first tick after now, and no
 * sooner than vblank_grid_gap(hz) ticks after *last (the previous flip's tick,
 * 0 = none yet); *last becomes the tick it lands on. No limit: at once, *last
 * untouched. A long stall restarts from now -- never a burst of catch-up flips. */
static inline int64_t vblank_grid_flip_at(int64_t now, int64_t base, int64_t freq,
                                          int hz, uint64_t *last)
{
    uint64_t k, gap = vblank_grid_gap(hz);
    if (!gap)
        return now;
    k = vblank_grid_tick(now, base, freq) + 1;
    if (*last && k < *last + gap)
        k = *last + gap;
    *last = k;
    return vblank_grid_time(k, base, freq);
}

/*
 * Frame skip (RECOMP_FRAME_SKIP): the game keeps flipping on every tick of the
 * 60 Hz grid -- its dt stays 16.7 ms, which the Tutorial charge needs (at a
 * native 22.2 ms or 33 ms dt it does not count) -- while only some of those
 * flips are shown, L per second. Tick k belongs to cadence slot
 * floor(k*L/60); a flip is shown when its slot is past the slot of the last
 * shown flip. Divisors of 60 give an even cadence (30 -> every 2nd tick, 20 ->
 * every 3rd); others alternate (45 -> 1-1-2, 40 -> 1-2, 50 -> 1-1-1-1-2) and
 * average L. Shown at once on readiness (Steam Deck), the intervals are even
 * but the content still steps 1-1-2 ticks of game time: unavoidable while the
 * game's tick differs from the display's.
 */

/* The cadence slot of tick k at L shows per second: floor(k*L/60), split so it
 * cannot overflow. L <= 0 or >= 60: every tick is its own slot. */
static inline uint64_t vblank_grid_slot(uint64_t k, int L)
{
    if (L <= 0 || L >= VBLANK_GRID_HZ)
        return k;
    return k / VBLANK_GRID_HZ * (uint64_t)L + k % VBLANK_GRID_HZ * (uint64_t)L / VBLANK_GRID_HZ;
}

/* 1: the flip on tick k is shown, given the tick of the last shown flip
 * (last_shown; 0 = none yet shows tick 0's slot as taken, so the first flip
 * past it is shown). Every flip at L <= 0 or L >= 60. A late flip that skipped
 * past several slots is shown once, never twice in one slot. */
static inline int vblank_grid_shown(uint64_t k, uint64_t last_shown, int L)
{
    if (L <= 0 || L >= VBLANK_GRID_HZ)
        return 1;
    return vblank_grid_slot(k, L) > vblank_grid_slot(last_shown, L);
}

/* The rate the flip pacer waits for: 60 while frame skip is active (the game
 * flips on every tick, the cadence picks which are shown), the limit otherwise. */
static inline int vblank_grid_pace_hz(int limit, int skip)
{
    return skip ? VBLANK_GRID_HZ : limit;
}

#endif
