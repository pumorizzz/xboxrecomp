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

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "exec";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int exec = !strcmp(mode, "exec");
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
