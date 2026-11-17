/*
 * The executor-owned fence's three host-side helpers, on the real memory layout
 * (the synthetic XBE from tools/conformance; no title, no game files).
 *
 * One process per mode, because RECOMP_PB_EXEC is read once:
 *
 *   exec     RECOMP_PB_EXEC=1. The registered wake event is reported at
 *            device + offset, which is what lets D3D's fence wait sleep on a
 *            real host event that the executor sets.
 *   no-exec  No executor: the event is not reported. Nothing would ever set
 *            it then, and a title registering it unconditionally (Fallout's
 *            main.c does) would have its first fence wait sleep for good.
 *
 * The wake event itself, through the KeWaitForSingleObject bridge as the
 * title calls it, with RECOMP_TITLE_KEVENTS unset:
 *   exec     it is a host event of its own -- unset, a wait times out; another
 *            title-built event is not; a wait on one thread is released by
 *            xbox_KeSignalTitleEvent on another (the executor's NOP 5 / DMA_GET
 *            wake), and consumes the signal. Without that D3D's BlockOnFence
 *            loops on a failed wait or sleeps through a long frame for good.
 *            The same from a spawned guest thread whose TIB (heap) lies above
 *            the device: the event is found by mapping, not by the calling
 *            thread's TIB (Fallout's loading-screen thread, end of Carbon).
 *   no-exec  it is not: a wait fails at once rather than sleep on an event
 *            nothing would set.
 *
 * Both modes: xbox_KeSignalTitleEvent on a value that is no event the bridge
 * knows -- here the number of a live host event -- leaves it alone; it used
 * to fall back to SetEvent on the value itself.
 *
 * Both modes also step the lost-kick recovery (xbox_Nv2aLostKickPoll) through
 * a fake DMA_PUT:
 *
 *   1. D3D's pointer ahead of an idle GPU kicks after 250 ms, not before.
 *   2. A busy GPU (GET behind PUT), a pointer outside the contiguous window
 *      or behind PUT, and a pointer that keeps moving never kick.
 *   3. D3D kicking between the poll's read of DMA_PUT and its store: the
 *      store must not land. A plain store put DMA_PUT back behind D3D's
 *      kick, and the commands in between were never executed.
 *
 * Two more modes, the DMA walk (RECOMP_PB_WALK=dma) with the executor:
 *
 *   kick-flip RECOMP_PB_EXEC=1, RECOMP_PB_WALK=dma, RECOMP_FPS_LIMIT=1: a
 *            kickoff (0x100410 bit 16) is answered while the executor waits for
 *            a flip's vblank tick. D3D's KickOff spins on the bit; unanswered,
 *            the title loses the rest of the frame (Fallout at 60 fps: game
 *            frames in 4 + 29 ms pairs).
 *   get-wake RECOMP_PB_EXEC=1, RECOMP_PB_WALK=dma: only NO_OPERATION 5 (and the
 *            2 s heartbeat) sets the fence event; a walk's pass without it
 *            leaves the event unset. A spurious wake releases Fallout's
 *            BlockOnFence early, with the same 4 + 29 ms pairs as symptom.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
void xbox_KeSignalTitleEvent(uint32_t guest_va);   /* kernel_bridge.c */
#define ORD_KE_WAIT 159                            /* KeWaitForSingleObject */
#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT 0x00000102u
#endif

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void checkd(int ok, const char *what, const char *fmt, ...)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && fmt) {
        va_list ap;
        printf(" -- ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    putchar('\n');
    if (!ok) failures++;
}

static uint32_t *G(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

static uint32_t blk;                         /* xbox_HeapAlloc(0x10000, 4096) */
#define THUNK_VA  (blk)
#define STACK_MAIN (blk + 0x1000)
#define STACK_WAIT (blk + 0x2000)
#define ZERO_VA   (blk + 0x3000)             /* LARGE_INTEGER 0 */
#define LONG_VA   (blk + 0x3010)             /* LARGE_INTEGER -5 s, relative */
static uint32_t s_wait_va;                   /* KeWaitForSingleObject's thunk */

/* KeWaitForSingleObject through the thunk, as the title calls it; a stack of
 * its own per thread (g_esp/g_eax are per thread). stdcall: 5 args + return. */
static uint32_t ke_wait_on(uint32_t stack, uint32_t obj, uint32_t timeout_va, uint32_t *esp_after)
{
    uint32_t *sp = G(stack);
    sp[0] = 0xBEEF0001u;              /* return address */
    sp[1] = obj; sp[2] = 0; sp[3] = 0; sp[4] = 0; sp[5] = timeout_va;
    g_esp = stack;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(s_wait_va)();
    if (esp_after) *esp_after = g_esp;
    return g_eax;
}

/* Header of a KEVENT as XDK code writes it: Type, Absolute, Size (dwords),
 * Inserted, then SignalState and an empty wait list. */
static void build_event(uint32_t va, int type, int signalled)
{
    G(va)[0] = (uint32_t)type | (4u << 16);
    G(va)[1] = signalled ? 1u : 0u;
    G(va)[2] = va + 8;
    G(va)[3] = va + 8;
}

static volatile uint32_t s_wst = 0xDEADBEEFu, s_wesp;
static DWORD WINAPI waiter(void *p)
{
    uint32_t esp = 0;
    s_wst = ke_wait_on(STACK_WAIT, (uint32_t)(uintptr_t)p, LONG_VA, &esp);
    s_wesp = esp;
    return 0;
}

/* A spawned guest thread, as bridge_spawn_thread makes one: its TIB comes from
 * xbox_AllocThreadTib (the heap) and g_fs_base points at it. Fallout's device
 * -- and so the fence event at device + 0x1DBC -- is in the image's .data,
 * below every such TIB; its loading-screen thread waits on that event. */
static volatile uint32_t s_gt_tib, s_gt_va, s_gt_st = 0xDEADBEEFu;
static DWORD WINAPI guest_thread_waiter(void *p)
{
    /* The heap block xbox_AllocThreadTib would hand out (the test XBE has no
     * TLS, so it returns 0 here): allocated after the device, so above it. */
    s_gt_tib = xbox_HeapAlloc(0x40, 16);
    g_fs_base = s_gt_tib;
    s_gt_va = xbox_Nv2aFenceWakeEventVa();
    s_gt_st = ke_wait_on(STACK_WAIT, (uint32_t)(uintptr_t)p, ZERO_VA, NULL);
    return 0;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

#define RING 0x80100000u                  /* a pushbuffer stretch, contiguous VA */
#define PHYS(va) ((va) & 0x0FFFFFFFu)

static void lost_kick(void)
{
    XboxLostKick st;
    volatile uint32_t put_reg;
    uint32_t put = PHYS(RING), ptr = RING + 0x40, t;
    int kicked;

    /* 1. Idle GPU, pointer ahead: kick at 250 ms, not before. */
    memset(&st, 0, sizeof st);
    put_reg = put;
    kicked = xbox_Nv2aLostKickPoll(&st, ptr, &put_reg, put, put, 1000);
    kicked |= xbox_Nv2aLostKickPoll(&st, ptr, &put_reg, put, put, 1249);
    check(!kicked && put_reg == put, "idle GPU, pointer ahead: no kick before 250 ms");
    kicked = xbox_Nv2aLostKickPoll(&st, ptr, &put_reg, put, put, 1250);
    check(kicked && put_reg == PHYS(ptr), "idle GPU, pointer ahead: kicked to it at 250 ms");

    /* 2. Never a kick: busy GPU, pointer outside the window or behind PUT,
     * pointer still moving. */
    memset(&st, 0, sizeof st);
    put_reg = put;
    kicked = 0;
    for (t = 0; t <= 1000; t += 100)
        kicked |= xbox_Nv2aLostKickPoll(&st, ptr, &put_reg, put, put - 0x20, t);
    for (t = 0; t <= 1000; t += 100)
        kicked |= xbox_Nv2aLostKickPoll(&st, 0x00100040u, &put_reg, put, put, t);
    for (t = 0; t <= 1000; t += 100)
        kicked |= xbox_Nv2aLostKickPoll(&st, RING - 0x40, &put_reg, put, put, t);
    for (t = 0; t <= 1000; t += 100)
        kicked |= xbox_Nv2aLostKickPoll(&st, ptr + t, &put_reg, put, put, t);
    check(!kicked && put_reg == put, "busy GPU, foreign or moving pointer: no kick");

    /* 3. D3D kicked after the poll read PUT: its PUT stays. */
    memset(&st, 0, sizeof st);
    put_reg = put;
    xbox_Nv2aLostKickPoll(&st, ptr, &put_reg, put, put, 0);
    put_reg = PHYS(ptr) + 0x80;               /* D3D's own kick, further on */
    kicked = xbox_Nv2aLostKickPoll(&st, ptr, &put_reg, put, put, 300);
    check(!kicked && put_reg == PHYS(ptr) + 0x80, "D3D kicked meanwhile: DMA_PUT not put back");
}

/* kick-flip: XDK D3D's KickOff sets 0x100410 bit 16 and spins until the GPU
 * clears it (Fallout 0x000AB850). On hardware the FIFO answers at once, also
 * while PGRAPH sits on a FLIP_STALL waiting for vblank. Here the executor
 * waits for the flip's vblank tick in flip_pace; if nothing answers the kick
 * meanwhile, the title spins in KickOff for the rest of the frame (Fallout at
 * 60 fps: game frames in 4 + 29 ms pairs). RECOMP_FPS_LIMIT=1 makes the
 * second flip wait a whole second, so the wait cannot be missed. */
static void kick_during_flip(uint32_t scratch)
{
    volatile uint32_t *put = xbox_Nv2aRegPtr(0x800040u);
    volatile uint32_t *get = xbox_Nv2aRegPtr(0x800044u);
    volatile uint32_t *kick = xbox_Nv2aRegPtr(0x100410u);
    volatile uint32_t *intr = xbox_Nv2aRegPtr(0x600100u);
    uint32_t *pb = G(RING), get_at_kick, ms;
    DWORD t0;

    if (!put || !get || !kick || !intr) {
        check(0, "NV2A registers mapped");
        return;
    }
    pb[0] = 0x00040130u; pb[1] = 0;            /* FLIP_STALL */
    pb[2] = 0x00040130u; pb[3] = 0;            /* FLIP_STALL: 60 ticks after the first at 1 Hz */
    pb[4] = 0x00040100u; pb[5] = 0;            /* NO_OPERATION */
    *get = PHYS(RING);
    *put = PHYS(RING);                         /* the walk resyncs to GET */
    Sleep(100);
    *put = PHYS(RING) + 24;                    /* kick the three commands */
    Sleep(200);                                /* first flip done (<= 17 ms), second waiting */
    get_at_kick = *get;
    *kick |= 0x10000u;                         /* D3D's KickOff handshake */
    *intr |= 0x1u;                             /* a pending PCRTC interrupt: the poll loop's to clear, not the kick thread's */
    Sleep(100);
    check(*kick & 0x10000u,
          "no fence event registered yet: the kick thread leaves the kick alone");
    xbox_Nv2aFenceWakeEvent(scratch, 0x40);    /* the title registers it: the executor owns the fence */
    t0 = GetTickCount();
    while ((*kick & 0x10000u) && GetTickCount() - t0 < 500)
        Sleep(1);
    ms = GetTickCount() - t0;
    checkd(get_at_kick != PHYS(RING) + 24,
           "the executor is inside the second flip when the title kicks",
           "GET 0x%08X", get_at_kick);
    checkd(!(*kick & 0x10000u) && ms <= 50,
           "a kickoff is answered while the executor waits for the flip's vblank tick",
           "bit 16 %s after %u ms", (*kick & 0x10000u) ? "still set" : "cleared", ms);
    Sleep(300);                                /* still inside the flip's wait (~0.8 s after the kick) */
    checkd(*intr & 0x1u,
           "only the kick bit is answered meanwhile: a pending interrupt is left for the poll loop",
           "PCRTC_INTR_0 0x%08X", *intr);
    t0 = GetTickCount();
    while (*get != PHYS(RING) + 24 && GetTickCount() - t0 < 3000)
        Sleep(5);
    checkd(*get == PHYS(RING) + 24, "the flips complete and GET reaches PUT",
           "GET 0x%08X", *get);

    /* The kick thread stops with the layout: Shutdown waits for it before it
     * frees the register file, which the thread reads. Left running it faults
     * on the freed page a few ms later. */
    xbox_MemoryLayoutShutdown();
    Sleep(50);
    check(1, "the layout shuts down with the kick thread running, and the process survives it");
}

/* get-wake: Fallout's BlockOnFence (0x000AB9E0), GPU far from the fence's
 * marker, patches NO_OPERATION 5 after it and sleeps in KeWaitForSingleObject
 * ONCE (0x000ABB30 loops only on a failed wait), then returns as if the fence
 * had passed. So the event must mean "NOP 5 executed". With the DMA walk GET is
 * published command by command; a wake at the end of every pass then only
 * releases a sleeper whose fence is still ahead (Fallout: 233 of 710 sleeps,
 * game frames in 4 + 29 ms pairs). The heartbeat (DMA_PUT standing for 2 s)
 * stays: it recovers a NO_OPERATION 5 patched just behind the walk, and a
 * title that sleeps submits nothing, so it fires only then. */
static void get_wake(uint32_t va)
{
    volatile uint32_t *put = xbox_Nv2aRegPtr(0x800040u);
    volatile uint32_t *get = xbox_Nv2aRegPtr(0x800044u);
    uint32_t *pb = G(RING), st;
    DWORD t0;
    int beat = 0;

    if (!put || !get) {
        check(0, "NV2A registers mapped");
        return;
    }
    *get = PHYS(RING);
    *put = PHYS(RING);                         /* the walk resyncs to GET */
    t0 = GetTickCount();                       /* take the ack thread's beat first */
    while (GetTickCount() - t0 < 5000) {
        if (ke_wait_on(STACK_MAIN, va, ZERO_VA, NULL) == 0) { beat = 1; break; }
        Sleep(5);
    }
    checkd(beat, "while DMA_PUT stands the ack thread still sets the event every 2 s",
           "no wake in %u ms", (unsigned)(GetTickCount() - t0));
    pb[0] = 0x00040100u; pb[1] = 0;            /* NO_OPERATION 0: nothing to wake for */
    *put = PHYS(RING) + 8;
    t0 = GetTickCount();
    while (*get != PHYS(RING) + 8 && GetTickCount() - t0 < 1000)
        Sleep(1);
    Sleep(20);
    st = ke_wait_on(STACK_MAIN, va, ZERO_VA, NULL);
    checkd(*get == PHYS(RING) + 8 && st == STATUS_TIMEOUT,
           "a pass with no NO_OPERATION 5 leaves the fence event unset",
           "GET 0x%08X wait status 0x%08X", *get, st);
    pb[2] = 0x00040100u; pb[3] = 5;            /* NO_OPERATION 5: BlockOnFence's wake */
    *put = PHYS(RING) + 16;
    t0 = GetTickCount();
    while (*get != PHYS(RING) + 16 && GetTickCount() - t0 < 1000)
        Sleep(1);
    Sleep(20);
    st = ke_wait_on(STACK_MAIN, va, ZERO_VA, NULL);
    checkd(st == 0, "NO_OPERATION 5 sets it", "wait status 0x%08X", st);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "exec";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int kickflip = !strcmp(mode, "kick-flip");
    int getwake = !strcmp(mode, "get-wake");
    int exec = !strcmp(mode, "exec") || kickflip || getwake;
    size_t xbe_len = 0;
    unsigned char *xbe;
    uint32_t scratch, dev, va;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!exec && strcmp(mode, "no-exec")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }
    if (exec)
        set_env("RECOMP_PB_EXEC", "1");
    if (getwake)
        set_env("RECOMP_PB_WALK", "dma");
    if (kickflip) {
        set_env("RECOMP_PB_WALK", "dma");
        set_env("RECOMP_FPS_LIMIT", "1");
    }
    set_env("RECOMP_TITLE_KEVENTS", "");     /* the fence event must qualify on its own */
    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }
    printf("mode: %s\n", mode);
    if (kickflip) {
        /* The kick thread answers only while the executor owns the fence: the
         * title registers its fence event after the layout is up (main.c does), so
         * the test registers it itself, once the kick has been left unanswered. */
        scratch = xbox_HeapAlloc(0x1000, 16);
        dev = scratch + 0x100;
        *G(scratch) = dev;
        build_event(dev + 0x40, 1, 0);
        kick_during_flip(scratch);
        printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
        return failures ? 1 : 0;
    }

    /* The title's kernel calls go through the thunk dispatcher. */
    blk = xbox_HeapAlloc(0x10000, 4096);
    *G(THUNK_VA) = 0x80000000u | ORD_KE_WAIT;
    xbox_kernel_set_thunk_address(THUNK_VA, 1);
    xbox_kernel_bridge_init();
    s_wait_va = *G(THUNK_VA);
    if (!recomp_lookup_kernel(s_wait_va)) {
        printf("ordinal %u did not resolve to a bridge entry point\n", ORD_KE_WAIT);
        return 2;
    }
    G(ZERO_VA)[0] = G(ZERO_VA)[1] = 0;
    G(LONG_VA)[0] = 0xFD050F80u;             /* -5 s in 100 ns units, relative */
    G(LONG_VA)[1] = 0xFFFFFFFFu;

    /* A device pointer, the device, and at +0x40 a KEVENT header (type 1,
     * size 4) -- the shape D3D's fence wait sleeps on. */
    scratch = xbox_HeapAlloc(0x1000, 16);
    dev = scratch + 0x100;
    *G(scratch) = dev;
    build_event(dev + 0x40, 1, 0);
    xbox_Nv2aFenceWakeEvent(scratch, 0x40);
    va = xbox_Nv2aFenceWakeEventVa();
    if (exec)
        check(va == dev + 0x40, "executor owns the fence: wake event at device + 0x40");
    else
        check(va == 0, "no executor: no wake event to sleep on");

    if (getwake) {
        if (va == dev + 0x40)
            get_wake(va);
        printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
        return failures ? 1 : 0;
    }
    if (exec && va == dev + 0x40) {
        uint32_t st, esp = 0;
        HANDLE th;
        int done;

        /* The executor's ack thread sets this event itself: once when it
         * starts and again every 2 s while the title submits nothing (the
         * wake a lost one would be recovered by). Take that beat first, so the
         * checks below start just after one, with ~2 s to the next. */
        {
            DWORD t0 = GetTickCount();
            int beat = 0;
            while (GetTickCount() - t0 < 5000) {
                if (ke_wait_on(STACK_MAIN, va, ZERO_VA, NULL) == 0) { beat = 1; break; }
                Sleep(5);
            }
            printf("  (ack thread's heartbeat %s after %lu ms)\n", beat ? "seen" : "not seen",
                   (unsigned long)(GetTickCount() - t0));
        }

        /* 0. The segment scan (no RECOMP_PB_WALK) sets the event whenever a
         * submitted segment ends, not only at NO_OPERATION 5: a patch can
         * land behind GET, which it publishes only after the whole segment.
         * One NO_OPERATION 0 moves PUT; the event is set, though no marker
         * asked for it. */
        {
            volatile uint32_t *put = xbox_Nv2aRegPtr(0x800040u);
            volatile uint32_t *get = xbox_Nv2aRegPtr(0x800044u);
            uint32_t *pb = G(RING);
            DWORD t0;

            if (!put || !get) {
                check(0, "NV2A registers mapped");
            } else {
                *get = PHYS(RING);
                *put = PHYS(RING);
                Sleep(100);
                while (ke_wait_on(STACK_MAIN, va, ZERO_VA, NULL) == 0)
                    ;                              /* drain the pass's own wake */
                pb[0] = 0x00040100u; pb[1] = 0;    /* NO_OPERATION 0 */
                *put = PHYS(RING) + 8;
                t0 = GetTickCount();
                while (*get != PHYS(RING) + 8 && GetTickCount() - t0 < 1000)
                    Sleep(1);
                Sleep(20);
                st = ke_wait_on(STACK_MAIN, va, ZERO_VA, NULL);
                checkd(st == 0,
                       "segment scan: a pass that moved PUT sets the fence event",
                       "GET 0x%08X wait status 0x%08X", *get, st);
            }
        }

        /* 1. A host event of its own: unset, a zero-timeout wait times out. */
        st = ke_wait_on(STACK_MAIN, va, ZERO_VA, &esp);
        checkd(st == STATUS_TIMEOUT && esp == STACK_MAIN + 24,
               "fence event is a host event without RECOMP_TITLE_KEVENTS: unset, it times out",
               "status 0x%08X esp 0x%08X (va 0x%08X header 0x%08X)", st, esp, va, *G(va));

        /* 2. Control: another title-built event does not qualify. */
        build_event(dev + 0x80, 1, 0);
        st = ke_wait_on(STACK_MAIN, dev + 0x80, ZERO_VA, NULL);
        checkd(st != 0 && st != STATUS_TIMEOUT,
               "another title-built event does not qualify (only the named one)",
               "status 0x%08X", st);

        /* 3. A wait on one thread is released from another. */
        th = CreateThread(NULL, 0, waiter, (void *)(uintptr_t)va, 0, NULL);
        check(th != NULL && WaitForSingleObject(th, 200) == WAIT_TIMEOUT,
              "the wait blocks until the event is set");
        xbox_KeSignalTitleEvent(va);
        done = th && WaitForSingleObject(th, 500) == WAIT_OBJECT_0;   /* the call's doing, not the next beat's */
        if (th && !done)
            WaitForSingleObject(th, 10000);   /* never leave the thread on a guest stack */
        if (th) CloseHandle(th);
        checkd(done && s_wst == 0 && G(va)[1] == 0 && s_wesp == STACK_WAIT + 24,
               "xbox_KeSignalTitleEvent from another thread releases it; the wait consumes the signal",
               "done %d status 0x%08X SignalState %u", done, s_wst, G(va)[1]);

        /* 3b. The same event from a spawned guest thread whose TIB lies above
         * the device. fence_readable bounded by the calling thread's g_fs_base,
         * so there the event was "not the fence": the wait fell through to the
         * guest address as a HANDLE and failed at once, and BlockOnFence's
         * while (KeWaitForSingleObject(...)) spun for good (Fallout, end of the
         * Carbon load). Zero timeout: unset it times out, set it returns 0;
         * never STATUS_UNSUCCESSFUL. */
        th = CreateThread(NULL, 0, guest_thread_waiter, (void *)(uintptr_t)va, 0, NULL);
        if (th) {
            WaitForSingleObject(th, 10000);
            CloseHandle(th);
        }
        checkd(th && s_gt_tib > va,
               "setup: the guest thread's TIB lies above the device",
               "tib 0x%08X va 0x%08X", s_gt_tib, va);
        checkd(s_gt_va == va,
               "a guest thread with its own TIB sees the fence wake event",
               "xbox_Nv2aFenceWakeEventVa 0x%08X, want 0x%08X (g_fs_base 0x%08X)",
               s_gt_va, va, s_gt_tib);
        checkd(s_gt_st == STATUS_TIMEOUT || s_gt_st == 0,
               "its wait on the fence event is a real wait (times out or returns 0)",
               "status 0x%08X", s_gt_st);
    } else if (!exec) {
        /* 4. Not a host event: a wait fails at once, never sleeps. Not
         * signalled here: a wait must fail on its own. */
        uint32_t st = ke_wait_on(STACK_MAIN, dev + 0x40, ZERO_VA, NULL);
        checkd(st != 0 && st != STATUS_TIMEOUT && G(dev + 0x40)[1] == 0,
               "no executor: the fence event is no host event (a wait fails at once, never sleeps)",
               "status 0x%08X", st);
    }

    /* 5. Only an event the bridge knows is set: handed the number of a live
     * host event, the old fallback (bridge_resolve_handle passes an untagged
     * value through as a HANDLE) set that event. */
    {
        HANDLE h = CreateEventA(NULL, TRUE, FALSE, NULL);
        int ok = h && (uintptr_t)h <= 0xFFFFFFFFu;
        if (ok) {
            xbox_KeSignalTitleEvent((uint32_t)(uintptr_t)h);
            ok = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
        }
        checkd(ok, "KeSignalTitleEvent leaves a value that is no known event alone",
               "handle %p", (void *)h);
        if (h) CloseHandle(h);
    }

    lost_kick();

    printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
