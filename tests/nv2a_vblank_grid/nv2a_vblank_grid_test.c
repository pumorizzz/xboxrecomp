/* The flip lands on a 60 Hz vblank grid, as it does on the console.
 *
 * On the hardware a flip waits for the vblank, so the game sees whole 1/60 s
 * frames; D3DPRESENT_INTERVAL_TWO means no sooner than two vblanks after the
 * previous flip. The old limiter let a late frame through at once and spaced
 * the next one by a fixed period from the old schedule: 4 + 28 ms pairs at
 * 60 fps and 5 + 61 ms at 30, which made the Tutorial's knife robot lose the
 * pad at 30 fps. This test drives vblank_grid.h with simulated frames (a
 * frame's drawing time, then the flip stall) against an independent reference
 * written with plain 64-bit arithmetic on short intervals.
 *
 *   - Every flip completes on the first counter value of a grid tick.
 *   - A frame that is ready in time flips exactly ceil(60/hz) ticks after the
 *     previous one: 60 -> 1, 30 -> 2, 20 -> 3, 45 and 59 -> 2, 1 -> 60; above
 *     60 the limit counts as 60.
 *   - A late frame waits for the next tick: at 60 a 20 ms or 25.6 ms frame is
 *     2 ticks, at 30 a 28 ms frame 2 and a 40 ms frame 3 (39 fps shows as 30).
 *   - With a limit of 30 no two flips are closer than 2 ticks, whatever the
 *     frame times alternate between.
 *   - A flip takes the first tick after the stall: it is not later than that
 *     unless the gap to the previous flip forces it.
 *   - A one second stall does not bank time: no burst of catch-up flips, the
 *     flip after it is on the first tick after the stall.
 *   - A limit of 0 (or below) completes at once and leaves the state alone.
 *   - The first flip is on the first tick strictly after the stall, within
 *     one tick of it; a stall exactly on a tick start goes to the next one.
 *   - tick(time(k)) == k and tick(time(k) - 1) == k - 1 at 10 MHz, the
 *     3.579545 MHz and 2.4 GHz counters, out to ten years: no overflow.
 *   - No drift: 60 * s ticks after the base is exactly s counter seconds.
 *   - The base set a moment after another thread read the counter (a time
 *     before the base) is tick 0 and the next flip is tick 1.
 *   - The gap for {-1, 0, 1, 20, 29, 30, 31, 45, 59, 60, 61, 120}.
 *   - Frame skip: the game flips on every tick and L flips a second are
 *     shown. For L in {20, 30, 40, 45, 50, 59, 60, 0, 120}: L*m shows in
 *     60*m ticks, every interval floor or ceil of 60/L (45 -> 1-1-2, 30 ->
 *     the even ticks), every flip at 0 and at 60 or more. Got wrong, the
 *     game's logic at 45 runs at a cadence the display does not keep, or a
 *     shown frame repeats.
 *   - Late flips: a flip is shown when it crossed into a new slot, never
 *     twice in one slot.
 *   - The slot does not overflow out to ten years; the pacer waits for 60
 *     while frame skip is active (the game's dt stays 16.7 ms -- the
 *     Tutorial charge does not count at 22 or 33 ms).
 *
 * Standalone: only vblank_grid.h, no runtime. CHECK as tests/nv2a_points. */
#include <stdint.h>
#include <stdio.h>

#include "vblank_grid.h"

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

#define MAXN 64

/* Independent reference: plain arithmetic, short intervals only. */
static int ref_on_grid(int64_t f, int64_t base, int64_t freq)
{
    /* f is the first counter value of the tick it falls in; the tick is floor,
     * not ceil: the first value of tick k is a hair past k * freq / 60. */
    int64_t k = (f - base) * 60 / freq;
    return base + ((k * freq + 59) / 60) == f;
}
static int64_t ref_tick(int64_t f, int64_t base, int64_t freq)
{
    return (f - base) * 60 / freq;
}

/* n frames: drawing time work[i] (ms), then the flip stall. Records the stall
 * time, the flip time and its tick. */
static void simulate(int64_t freq, int64_t base, int hz, const double *work, int n,
                     int64_t *stalls, int64_t *flips, int64_t *ticks)
{
    uint64_t last = 0;
    int64_t t = base + 12345;
    int i;
    for (i = 0; i < n; i++) {
        int64_t f;
        t += (int64_t)(work[i] * (double)freq / 1000.0);
        f = vblank_grid_flip_at(t, base, freq, hz, &last);
        CHECK(f >= t);
        stalls[i] = t;
        flips[i] = f;
        ticks[i] = ref_tick(f, base, freq);
        t = f;
    }
}

static void fill(double *w, int n, double ms)
{
    int i;
    for (i = 0; i < n; i++) w[i] = ms;
}

/* All intervals from the second flip on equal `want`. */
static int all_gaps(const int64_t *ticks, int n, int64_t want)
{
    int i;
    for (i = 1; i < n; i++)
        if (ticks[i] - ticks[i - 1] != want) return 0;
    return 1;
}

static void run(int64_t freq, int64_t base)
{
    double w[MAXN];
    int64_t st[MAXN], fl[MAXN], tk[MAXN];
    static const int hzs[] = { 60, 30, 20, 45, 59, 1, 120, 200 };
    static const int64_t gaps[] = { 1, 2, 3, 2, 2, 60, 1, 1 };
    int i, h, n = 30;

    /* P2 (+P1): a frame that is ready in time, every limit. */
    for (h = 0; h < 8; h++) {
        fill(w, n, 5);
        simulate(freq, base, hzs[h], w, n, st, fl, tk);
        for (i = 0; i < n; i++) CHECK(ref_on_grid(fl[i], base, freq));
        CHECK(all_gaps(tk, n, gaps[h]));
    }

    /* P3: a late frame waits for the next tick. */
    fill(w, n, 20);   simulate(freq, base, 60, w, n, st, fl, tk);
    CHECK(all_gaps(tk, n, 2));
    for (i = 0; i < n; i++) CHECK(ref_on_grid(fl[i], base, freq));
    fill(w, n, 25.6); simulate(freq, base, 60, w, n, st, fl, tk);
    CHECK(all_gaps(tk, n, 2));
    fill(w, n, 28);   simulate(freq, base, 30, w, n, st, fl, tk);
    CHECK(all_gaps(tk, n, 2));
    fill(w, n, 40);   simulate(freq, base, 30, w, n, st, fl, tk);
    CHECK(all_gaps(tk, n, 3));
    for (i = 0; i < n; i++) CHECK(ref_on_grid(fl[i], base, freq));

    /* P4: at 30 no interval under 2 ticks, whatever the frame times. */
    {
        static const double alt[4] = { 10, 30, 5, 61 };
        for (i = 0; i < n; i++) w[i] = alt[i & 3];
        simulate(freq, base, 30, w, n, st, fl, tk);
        for (i = 1; i < n; i++) CHECK(tk[i] - tk[i - 1] >= 2);
        for (i = 0; i < n; i++) CHECK(ref_on_grid(fl[i], base, freq));
        /* P5: the first tick after the stall, unless the gap forces later. */
        for (i = 0; i < n; i++) {
            int64_t kk = tk[i];
            int ok = vblank_grid_time((uint64_t)(kk - 1), base, freq) <= st[i];
            if (!ok && i > 0) ok = (kk - 1) < tk[i - 1] + 2;
            CHECK(ok);
        }
    }
    fill(w, n, 5);
    simulate(freq, base, 60, w, n, st, fl, tk);
    for (i = 0; i < n; i++)
        CHECK(vblank_grid_time((uint64_t)(tk[i] - 1), base, freq) <= st[i]);

    /* P6: a long stall restarts from now, no burst afterwards. */
    for (i = 0; i < 16; i++) w[i] = 0;
    for (i = 0; i < 5; i++) w[i] = 5;
    w[5] = 1000;
    simulate(freq, base, 30, w, 16, st, fl, tk);
    CHECK(tk[5] == ref_tick(st[5], base, freq) + 1);
    CHECK(tk[5] - tk[4] > 2);
    for (i = 6; i < 16; i++) CHECK(tk[i] - tk[i - 1] == 2);

    /* P7: no limit, at once, state untouched. */
    {
        static const int off[2] = { 0, -5 };
        for (h = 0; h < 2; h++) {
            uint64_t last = 77;
            int64_t t = base + 1000;
            for (i = 0; i < 20; i++) {
                CHECK(vblank_grid_flip_at(t, base, freq, off[h], &last) == t);
                t += (i % 3) * (freq / 1000);   /* some zero steps too */
            }
            CHECK(last == 77);
        }
    }

    /* P8: the first flip. */
    {
        int64_t t;
        for (i = 0; i < 20; i++) {
            uint64_t last = 0;
            int64_t f;
            t = base + 1 + (int64_t)i * (freq / 53) + i;
            f = vblank_grid_flip_at(t, base, freq, 60, &last);
            CHECK(f > t);
            CHECK(f - t <= (freq + 59) / 60);
            CHECK(ref_tick(f, base, freq) == ref_tick(t, base, freq) + 1);
            CHECK(ref_on_grid(f, base, freq));
        }
        {
            uint64_t last = 0;
            int64_t tk5 = vblank_grid_time(5, base, freq);
            int64_t f = vblank_grid_flip_at(tk5, base, freq, 60, &last);
            CHECK(f == vblank_grid_time(6, base, freq));
            CHECK(last == 6);
        }
    }

    /* P9: tick and time invert each other, out to ten years. */
    {
        uint64_t ks[9];
        int j;
        ks[0] = 0; ks[1] = 1; ks[2] = 59; ks[3] = 60; ks[4] = 61; ks[5] = 3599;
        ks[6] = 216000; ks[7] = 60ull * 86400 * 365 * 10; ks[8] = ks[7] + 1;
        for (j = 0; j < 9; j++) {
            CHECK(vblank_grid_tick(vblank_grid_time(ks[j], base, freq), base, freq) == ks[j]);
            if (ks[j] >= 1)
                CHECK(vblank_grid_tick(vblank_grid_time(ks[j], base, freq) - 1, base, freq)
                      == ks[j] - 1);
        }
    }

    /* P10: no drift. */
    CHECK(vblank_grid_time(60ull * 3600, base, freq) - base == 3600 * freq);
    CHECK(vblank_grid_time(60ull * 86400 * 365 * 10, base, freq) - base
          == 86400ll * 365 * 10 * freq);

    /* P11: the base set just after this thread read the counter. */
    {
        uint64_t last0 = 0;
        CHECK(vblank_grid_tick(base - 100, base, freq) == 0);
        CHECK(vblank_grid_flip_at(base - 100, base, freq, 60, &last0)
              == vblank_grid_time(1, base, freq));
    }
}

/* Frame-skip cadence (P13-P17), against plain floor(k*L/60). */
static int ref_shown(uint64_t k, uint64_t last, int L)
{
    if (L <= 0 || L >= 60) return 1;
    return (k * (uint64_t)L) / 60 > (last * (uint64_t)L) / 60;
}

static void cadence(void)
{
    static const int Ls[9] = { 20, 30, 40, 45, 50, 59, 60, 0, 120 };
    int li;
    for (li = 0; li < 9; li++) {
        int L = Ls[li], m = 7, shows = 0;
        uint64_t k, last = 0, prev_show = 0;
        int every = (L <= 0 || L >= 60);
        uint64_t lo = every ? 1 : 60 / (uint64_t)L, hi = every ? 1 : (60 + (uint64_t)L - 1) / (uint64_t)L;
        int seq[16], ns = 0;
        /* P13: one flip per tick from tick 1: L*m shows in 60*m ticks, every
         * interval floor or ceil of 60/L. */
        for (k = 1; k <= 60ull * (uint64_t)m; k++) {
            int s = vblank_grid_shown(k, last, L);
            CHECK(s == ref_shown(k, last, L));
            if (s) {
                if (prev_show) {
                    uint64_t gap = k - prev_show;
                    CHECK(gap == lo || gap == hi);
                    if (ns < 16) seq[ns++] = (int)gap;
                }
                prev_show = k;
                last = k;
                shows++;
            }
        }
        CHECK(shows == (every ? 60 * m : L * m));
        /* P14: 45 -> 1-1-2 repeating; 30 -> the even ticks. */
        if (L == 45) {
            int i;
            for (i = 0; i + 2 < ns; i += 3) {
                int a = seq[i], b = seq[i + 1], c = seq[i + 2];
                /* one 2 in every 3 intervals */
                CHECK(a + b + c == 4);
            }
        }
        if (L == 30) {
            last = 0;
            for (k = 1; k <= 120; k++)
                if (vblank_grid_shown(k, last, 30)) { CHECK(k % 2 == 0); last = k; }
        }
    }

    /* P15: late flips -- flips land on ticks 1,2,5,6,7,15,16 (gaps of 1..8).
     * A flip is shown when its slot is past the last shown one, so never two
     * in one slot, and a flip that crossed a slot boundary is shown. */
    {
        static const uint64_t ticks[] = { 1, 2, 5, 6, 7, 15, 16, 17, 18, 26, 27 };
        static const int Ls2[3] = { 30, 45, 20 };
        int li2, i;
        for (li2 = 0; li2 < 3; li2++) {
            int L = Ls2[li2];
            uint64_t last = 0;
            for (i = 0; i < (int)(sizeof ticks / sizeof ticks[0]); i++) {
                uint64_t k = ticks[i];
                int s = vblank_grid_shown(k, last, L);
                int crossed = vblank_grid_slot(k, L) != vblank_grid_slot(last, L);
                CHECK(s == crossed);
                if (s) last = k;
            }
        }
    }

    /* P16: slot has no overflow out to ten years of ticks. */
    {
        uint64_t k = 60ull * 86400 * 365 * 10 + 59;
        CHECK(vblank_grid_slot(k, 45) == (k / 60) * 45 + (59 * 45) / 60);
        CHECK(vblank_grid_slot(k, 0) == k);
    }

    /* P17: the pacer waits for 60 while frame skip is active. */
    CHECK(vblank_grid_pace_hz(30, 1) == 60);
    CHECK(vblank_grid_pace_hz(45, 1) == 60);
    CHECK(vblank_grid_pace_hz(30, 0) == 30);
    CHECK(vblank_grid_pace_hz(0, 0) == 0);
}

int main(void)
{
    static const int64_t freqs[] = { 10000000, 3579545, 2400000000LL };
    static const int64_t bases[] = { 987654321, 0x0000123456789ABCLL };
    static const int gin[12]  = { -1, 0, 1, 20, 29, 30, 31, 45, 59, 60, 61, 120 };
    static const int gout[12] = {  0, 0, 60, 3, 3, 2, 2, 2, 2, 1, 1, 1 };
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 2; j++)
            run(freqs[i], bases[j]);

    /* P12 */
    for (i = 0; i < 12; i++)
        CHECK(vblank_grid_gap(gin[i]) == (uint64_t)gout[i]);

    cadence();

    printf("nv2a_vblank_grid: %s\n", s_fail ? "FAILED" : "OK");
    return s_fail;
}
