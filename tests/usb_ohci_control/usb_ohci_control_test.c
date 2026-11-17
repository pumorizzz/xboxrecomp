/*
 * OHCI control transfers and what wakes the controller.
 *
 * XInputGetCapabilities builds a control request (SETUP, IN, OUT) and waits 50
 * ms for it. The emulated controller used to find its work only on a 4 ms
 * tick that Windows stretches to 15.6 ms, and the DPC that finishes the
 * request ran on another such tick, so the request took 30 to 60 ms: past the
 * driver's timeout, which cancelled it, left the URB in the schedule, and let
 * its late completion write into a stack frame that had been reused (Fallout:
 * the row counter of BinkCopyToBuffer). The controller now answers within the
 * frame, as the hardware does.
 *
 * Harness as tests/kernel_events: the real memory layout on the synthetic XBE
 * from tools/conformance, KeInsertQueueDpc through the thunk dispatcher, and
 * the controller's registers and one service pass through test hooks
 * (OHCI_TEST_HOOKS / XBOX_KERNEL_TEST_HOOKS). No controller thread runs and no
 * time passes: a "frame" is an argument of the pass, and the wake events are
 * read with a zero timeout. Each check below fails without its rule:
 *
 *   1. Three stages queued together retire in one pass, newest first on the
 *      done queue, with the pad's capabilities in the IN buffer. (Pins the
 *      walker; no mutation of the wake rules changes it.)
 *   2. While WDH is set the controller touches neither HCCA done head nor the
 *      endpoint: the done queue belongs to the driver until it acknowledges.
 *   3. Acknowledging WDH wakes the controller: a stage queued behind it is
 *      owed a pass at once, not at the next tick.
 *   4. ControlListFilled and BulkListFilled wake it; a HcCommandStatus write
 *      without them does not.
 *   5. Setting the master interrupt enable wakes it (a pending interrupt is
 *      owed delivery).
 *   6. A pass with no frame does not move the frame clock, raise SF or poll
 *      the periodic list: on hardware the interrupt endpoints are polled once
 *      a frame while control and bulk are walked whenever they are filled, and
 *      the pad's polling rate must not follow the wakes. A pass with a frame
 *      does all three.
 *   7. A request whose stages are queued one at a time, as the XDK driver may
 *      do, finishes without a single frame tick: every stage is answered by
 *      the wake alone. Without the wakes each stage costs a tick, 15.6 ms on
 *      Windows, and three stages outrun the 50 ms timeout.
 *   8. KeInsertQueueDpc from passive level wakes the DPC drain at once.
 *   9. From an ISR (IRQL above DISPATCH) it does not: on the console a DPC
 *      does not start before its ISR returns, and the device model wakes the
 *      drain after the ISR (xbox_KeKickDpcs).
 *   9b. At DISPATCH_LEVEL it waits for the drop: on the console the DPC starts
 *      only once the IRQL is below DISPATCH, and waking it at once would run
 *      it beside the guest section that queued it.
 *  10. The IRQL dropping back to passive wakes whoever registered for it (the
 *      controller thread, holding an interrupt back for the guest's IRQL);
 *      unregistered, nothing is set.
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

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;

/* Test hooks (src/usb/ohci.c, src/kernel/kernel_bridge.c). */
extern void     ohci_test_begin(void);
extern void     ohci_test_write(uint32_t off, uint32_t v);
extern uint32_t ohci_test_reg(uint32_t off);
extern int      ohci_test_pass(int frame);
extern int      ohci_test_kicked(void);
extern int      xbox_KeTestDpcWakePending(void);

/* OHCI 1.0a. */
#define HcControl        0x04
#define HcCommandStatus  0x08
#define HcInterruptStatus 0x0C
#define HcInterruptEnable 0x10
#define HcHCCA           0x18
#define HcControlHeadED  0x20
#define HcFmNumber       0x3C
#define CTL_OPERATIONAL  (0x80u | 0x10u | 0x04u)   /* HCFS operational, CLE, PLE */
#define INTR_WDH         0x00000002u
#define INTR_SF          0x00000004u
#define INTR_MIE         0x80000000u
#define DP_SETUP 0u
#define DP_OUT   1u
#define DP_IN    2u
#define FRAME_STEP 4u   /* frames a tick is worth (OHCI_TICK_MS) */

enum { S_INSERT, S_REMOVE, N_SLOTS };
static const uint32_t ORD[N_SLOTS] = {
    119,   /* KeInsertQueueDpc */
    137,   /* KeRemoveQueueDpc */
};

static uint32_t blk;                       /* xbox_HeapAlloc(0x10000, 4096) */
#define THUNK_VA (blk + 0x4000)
#define STACK_VA (blk + 0x5000)
#define HCCA     (blk)
#define ED_CTL   (blk + 0x100)
#define ED_INT   (blk + 0x110)
#define TD_SETUP (blk + 0x200)
#define TD_IN    (blk + 0x210)
#define TD_OUT   (blk + 0x220)
#define TD_DUMMY (blk + 0x230)
#define TD_INT   (blk + 0x240)
#define TD_INT_END (blk + 0x250)
#define BUF_SETUP (blk + 0x300)
#define BUF_IN    (blk + 0x320)
#define BUF_INT   (blk + 0x360)
#define DPC_A    (blk + 0x8000)
#define DPC_B    (blk + 0x8040)

static uint32_t slot_va[N_SLOTS];
static int failures;

static uint32_t *G(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}
static uint8_t *GB(uint32_t va)
{
    return (uint8_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-72s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static uint32_t call(int slot, int nargs, const uint32_t *args)
{
    uint32_t *sp = G(STACK_VA);
    int i;

    sp[0] = 0xBEEF0001u;
    for (i = 0; i < nargs; i++)
        sp[1 + i] = args[i];
    g_esp = STACK_VA;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(slot_va[slot])();
    return g_eax;
}

static uint32_t ke_insert_dpc(uint32_t dpc)
{
    uint32_t args[3] = { dpc, 0, 0 };
    return call(S_INSERT, 3, args);
}

static uint32_t ke_remove_dpc(uint32_t dpc)
{
    uint32_t args[1] = { dpc };
    return call(S_REMOVE, 1, args);
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
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* ---- descriptors ------------------------------------------------------- */

static void put_td(uint32_t td, uint32_t dp, uint32_t cbp, uint32_t be, uint32_t next)
{
    G(td)[0] = (dp << 19) | (1u << 18) | (0xFu << 28);   /* rounding, not accessed */
    G(td)[1] = cbp;
    G(td)[2] = next;
    G(td)[3] = be;
}

/* A fresh control transfer on endpoint 0: SETUP, IN of 20 bytes, OUT of none,
 * then an empty dummy. `stages` is how many are on the endpoint (tail points
 * at the first one not yet queued). The controller is brought back to
 * operational with nothing pending. */
static void begin_transfer(int stages)
{
    static const uint8_t setup[8] = { 0xC1, 0x01, 0x00, 0x01, 0x00, 0x00, 0x14, 0x00 };
    uint32_t tdv[4] = { TD_SETUP, TD_IN, TD_OUT, TD_DUMMY };

    ohci_test_begin();
    memset(GB(HCCA), 0, 0x100);
    memset(GB(BUF_IN), 0xAA, 20);                 /* so a no-op pass cannot match */
    memcpy(GB(BUF_SETUP), setup, 8);
    put_td(TD_SETUP, DP_SETUP, BUF_SETUP, BUF_SETUP + 7, TD_IN);
    put_td(TD_IN, DP_IN, BUF_IN, BUF_IN + 19, TD_OUT);
    put_td(TD_OUT, DP_OUT, 0, 0, TD_DUMMY);
    put_td(TD_DUMMY, DP_OUT, 0, 0, 0);
    G(ED_CTL)[0] = 8u << 16;                      /* FA 0, EP 0, MPS 8 */
    G(ED_CTL)[1] = tdv[stages];                   /* tail */
    G(ED_CTL)[2] = TD_SETUP;                      /* head */
    G(ED_CTL)[3] = 0;
    ohci_test_write(HcHCCA, HCCA);
    ohci_test_write(HcControlHeadED, ED_CTL);
    ohci_test_write(HcControl, CTL_OPERATIONAL);
    (void)ohci_test_kicked();
}

static uint32_t cc_of(uint32_t td)
{
    return G(td)[0] >> 28;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    size_t xbe_len = 0;
    unsigned char *xbe;
    char d[200];
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }
    blk = xbox_HeapAlloc(0x10000, 4096);
    /* The controller maps addresses below the contiguous allocation into the
     * contiguous window (bus_resolve); the block must be above that. */
    if (xbox_ContiguousAllocatedBytes() > blk) {
        puts("precondition: the test block is inside the contiguous range");
        return 2;
    }
    for (i = 0; i < N_SLOTS; i++)
        *G(THUNK_VA + 4 * i) = 0x80000000u | ORD[i];
    xbox_kernel_set_thunk_address(THUNK_VA, N_SLOTS);
    xbox_kernel_bridge_init();
    for (i = 0; i < N_SLOTS; i++) {
        slot_va[i] = *G(THUNK_VA + 4 * i);
        if (!recomp_lookup_kernel(slot_va[i])) {
            printf("ordinal %u did not resolve to a bridge entry point\n", ORD[i]);
            return 2;
        }
    }

    /* 1. Three stages together, one pass. */
    begin_transfer(3);
    i = ohci_test_pass(1);
    snprintf(d, sizeof d, "retired %d, done head %08X, cc %X %X %X", i,
             G(HCCA)[0x84 / 4], cc_of(TD_SETUP), cc_of(TD_IN), cc_of(TD_OUT));
    check(i == 3 && G(HCCA)[0x84 / 4] == TD_OUT && G(TD_OUT)[2] == TD_IN
          && G(TD_IN)[2] == TD_SETUP && G(TD_SETUP)[2] == 0
          && cc_of(TD_SETUP) == 0 && cc_of(TD_IN) == 0 && cc_of(TD_OUT) == 0
          && (ohci_test_reg(HcInterruptStatus) & INTR_WDH),
          "three stages queued together retire in one pass, newest first", d);
    {
        static const uint8_t caps[20] = { 0x00, 0x14, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        check(memcmp(GB(BUF_IN), caps, 20) == 0, "the IN stage carries the pad's capabilities", NULL);
    }

    /* 2. WDH set: the controller leaves the done queue and the endpoint alone. */
    {
        uint32_t done = G(HCCA)[0x84 / 4];
        int a, b;
        put_td(TD_SETUP, DP_SETUP, BUF_SETUP, BUF_SETUP + 7, TD_IN);
        G(ED_CTL)[1] = TD_IN;
        G(ED_CTL)[2] = TD_SETUP;
        a = ohci_test_pass(0);
        b = ohci_test_pass(1);
        snprintf(d, sizeof d, "retired %d %d, done head %08X, head %08X cc %X", a, b,
                 G(HCCA)[0x84 / 4], G(ED_CTL)[2], cc_of(TD_SETUP));
        check(a == 0 && b == 0 && G(HCCA)[0x84 / 4] == done
              && (G(ED_CTL)[2] & ~0xFu) == TD_SETUP && cc_of(TD_SETUP) == 0xF,
              "while WDH is set a pass leaves the done queue and the endpoint alone", d);

        /* 3. The acknowledgement wakes the controller. */
        (void)ohci_test_kicked();
        check(ohci_test_kicked() == 0, "no wake pending before the acknowledgement", NULL);
        ohci_test_write(HcInterruptStatus, INTR_WDH);
        check(ohci_test_kicked() == 1, "acknowledging WDH wakes the controller", NULL);
        check(G(HCCA)[0x84 / 4] == 0 && !(ohci_test_reg(HcInterruptStatus) & INTR_WDH),
              "the acknowledgement clears the done head and WDH", NULL);
    }

    /* 4. CLF / BLF wake; a write without them does not. */
    (void)ohci_test_kicked();
    ohci_test_write(HcCommandStatus, 0x02u);
    check(ohci_test_kicked() == 1, "ControlListFilled wakes the controller", NULL);
    ohci_test_write(HcCommandStatus, 0x04u);
    check(ohci_test_kicked() == 1, "BulkListFilled wakes the controller", NULL);
    ohci_test_write(HcCommandStatus, 0x00u);
    check(ohci_test_kicked() == 0, "a HcCommandStatus write without CLF or BLF does not", NULL);

    /* 5. MIE wakes. */
    ohci_test_write(HcInterruptEnable, 0x00000002u);
    check(ohci_test_kicked() == 0, "enabling WDH alone does not wake the controller", NULL);
    ohci_test_write(HcInterruptEnable, INTR_MIE);
    check(ohci_test_kicked() == 1, "setting the master interrupt enable wakes it", NULL);

    /* 6. Frame clock and periodic list only with a frame. */
    begin_transfer(0);
    G(ED_INT)[0] = (1u << 7) | (8u << 16);        /* EP 1, MPS 8 */
    G(ED_INT)[1] = TD_INT_END;
    G(ED_INT)[2] = TD_INT;
    G(ED_INT)[3] = 0;
    put_td(TD_INT, DP_IN, BUF_INT, BUF_INT + 19, TD_INT_END);
    put_td(TD_INT_END, DP_IN, 0, 0, 0);
    for (i = 0; i < 32; i++)
        G(HCCA)[i] = ED_INT;
    G(ED_CTL)[2] = TD_DUMMY;                      /* control endpoint empty */
    G(ED_CTL)[1] = TD_DUMMY;
    {
        int a = ohci_test_pass(0);
        uint32_t fm0 = ohci_test_reg(HcFmNumber);
        snprintf(d, sizeof d, "retired %d, FmNumber %u, SF %d, interrupt TD cc %X", a, fm0,
                 !!(ohci_test_reg(HcInterruptStatus) & INTR_SF), cc_of(TD_INT));
        check(a == 0 && fm0 == 0 && !(ohci_test_reg(HcInterruptStatus) & INTR_SF)
              && cc_of(TD_INT) == 0xF && G(HCCA)[0x80 / 4] == 0,
              "a pass without a frame: no clock, no SF, no periodic poll", d);
        a = ohci_test_pass(1);
        snprintf(d, sizeof d, "retired %d, FmNumber %u, HCCA frame %u, SF %d, interrupt TD cc %X", a,
                 ohci_test_reg(HcFmNumber), G(HCCA)[0x80 / 4],
                 !!(ohci_test_reg(HcInterruptStatus) & INTR_SF), cc_of(TD_INT));
        check(a == 1 && ohci_test_reg(HcFmNumber) == FRAME_STEP && G(HCCA)[0x80 / 4] == FRAME_STEP
              && (ohci_test_reg(HcInterruptStatus) & INTR_SF) && cc_of(TD_INT) == 0,
              "a pass with a frame: the clock moves, SF rises, the periodic list is polled", d);
    }

    /* 7. Stages one at a time, answered by the wakes alone. The "driver"
     * queues the next stage and writes CLF while WDH is still set (the pass
     * that follows has nothing to do), then acknowledges, as a DPC that
     * submits before it returns the done queue would. */
    {
        int frames = 0, stage = 0, pending_ack = 0, finished = 0, guard = 0;
        uint32_t fm0;
        uint32_t tail_of[3] = { TD_IN, TD_OUT, TD_DUMMY };

        begin_transfer(1);                         /* SETUP only */
        fm0 = ohci_test_reg(HcFmNumber);
        ohci_test_write(HcCommandStatus, 0x02u);   /* CLF */
        while (!finished && guard++ < 20) {
            if (ohci_test_kicked())
                (void)ohci_test_pass(0);
            else {
                frames++;
                (void)ohci_test_pass(1);
            }
            if (pending_ack) {
                ohci_test_write(HcInterruptStatus, INTR_WDH);
                pending_ack = 0;
            } else if (ohci_test_reg(HcInterruptStatus) & INTR_WDH) {
                if (stage == 2 && cc_of(TD_OUT) == 0) {
                    ohci_test_write(HcInterruptStatus, INTR_WDH);   /* the last one */
                    finished = 1;
                } else {
                    stage++;
                    G(ED_CTL)[1] = tail_of[stage];     /* queue the next stage */
                    ohci_test_write(HcCommandStatus, 0x02u);
                    pending_ack = 1;
                }
            }
        }
        snprintf(d, sizeof d, "control transfer took %d frame ticks (%d frames), want 0; stage %d",
                 frames, frames * FRAME_STEP, stage);
        check(finished && frames == 0 && ohci_test_reg(HcFmNumber) == fm0
              && cc_of(TD_SETUP) == 0 && cc_of(TD_IN) == 0 && cc_of(TD_OUT) == 0,
              "stages queued one at a time finish on the wakes alone, no frame tick", d);
    }

    /* 8. A DPC queued at passive level wakes the drain. */
    G(DPC_A)[0] = 0; G(DPC_A)[1] = 0;
    (void)xbox_KeTestDpcWakePending();
    check(ke_insert_dpc(DPC_A) == 1, "KeInsertQueueDpc queues the DPC", NULL);
    check(xbox_KeTestDpcWakePending() == 1, "KeInsertQueueDpc at passive level wakes the DPC drain", NULL);
    ke_remove_dpc(DPC_A);

    /* 9. From an ISR it does not; the device model wakes it after the ISR. */
    G(DPC_B)[0] = 0; G(DPC_B)[1] = 0;
    {
        int saved = xbox_IrqlEnterInterrupt(16);
        uint32_t r = ke_insert_dpc(DPC_B);
        xbox_IrqlLeaveInterrupt(saved);
        check(r == 1, "KeInsertQueueDpc from an ISR queues the DPC", NULL);
    }
    check(xbox_KeTestDpcWakePending() == 0, "a DPC queued from an ISR does not wake the drain", NULL);
    xbox_KeKickDpcs();
    check(xbox_KeTestDpcWakePending() == 1, "xbox_KeKickDpcs wakes it after the ISR", NULL);
    ke_remove_dpc(DPC_B);

    /* 9b. At DISPATCH_LEVEL the wake waits for the drop below DISPATCH. */
    G(DPC_A)[0] = 0; G(DPC_A)[1] = 0;
    {
        int saved = xbox_IrqlEnterInterrupt(2);
        uint32_t r = ke_insert_dpc(DPC_A);
        check(r == 1, "KeInsertQueueDpc at DISPATCH_LEVEL queues the DPC", NULL);
        check(xbox_KeTestDpcWakePending() == 0, "a DPC queued at DISPATCH does not wake the drain yet", NULL);
        xbox_IrqlLeaveInterrupt(saved);
        check(xbox_KeTestDpcWakePending() == 1, "the drop below DISPATCH wakes it", NULL);
    }
    ke_remove_dpc(DPC_A);

    /* 10. The IRQL dropping wakes whoever asked. */
    {
        HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        int saved;

        xbox_IrqlWakeOnOpen(ev);
        saved = xbox_IrqlEnterInterrupt(2);
        check(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "raising the IRQL wakes nobody", NULL);
        xbox_IrqlLeaveInterrupt(saved);
        check(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0,
              "the IRQL dropping to passive wakes the registered event", NULL);
        xbox_IrqlWakeOnOpen(NULL);
        saved = xbox_IrqlEnterInterrupt(2);
        xbox_IrqlLeaveInterrupt(saved);
        check(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "unregistered, a drop wakes nobody", NULL);
        CloseHandle(ev);
    }

    xbox_MemoryLayoutShutdown();
    free(xbe);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
