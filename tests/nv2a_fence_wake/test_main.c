/*
 * The executor-owned fence's two host-side helpers, on the real memory layout
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint32_t *G(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset());
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

    /* A device pointer, the device, and at +0x40 a KEVENT header (type 1,
     * size 4) -- the shape D3D's fence wait sleeps on. */
    scratch = xbox_HeapAlloc(0x1000, 16);
    dev = scratch + 0x100;
    *G(scratch) = dev;
    *G(dev + 0x40) = 1u | (4u << 16);
    xbox_Nv2aFenceWakeEvent(scratch, 0x40);
    va = xbox_Nv2aFenceWakeEventVa();
    if (exec)
        check(va == dev + 0x40, "executor owns the fence: wake event at device + 0x40");
    else
        check(va == 0, "no executor: no wake event to sleep on");

    lost_kick();

    printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
