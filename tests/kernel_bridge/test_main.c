/*
 * The kernel bridge.
 *
 * A title does not call the kernel by name. Its indirect calls land on a
 * thunk table that xbox_kernel_bridge_init() has rewritten to synthetic VAs,
 * and kernel_thunk_dispatch turns each of those into a bridge_* function.
 * That is the path the runtime actually executes, and it is easy to leave
 * untested: bridge_* functions are static and read guest CPU state rather
 * than taking arguments, so they cannot be called directly.
 *
 * They are reachable all the same. recomp_lookup_kernel is not static: given
 * a buffer standing in for guest memory it selects the slot and hands back
 * the dispatcher. tests/kernel_directory uses the same seam for the directory
 * ABI; this one covers kernel memory ordinals.
 *
 * Checked here with ordinal 173, MmGetPhysicalAddress, which had two
 * implementations that disagreed -- the bridge translating through the
 * contiguous window and xbox_MmGetPhysicalAddress returning its argument
 * unchanged. The bridge now calls the latter, and that delegation is what
 * these checks pin: asserting only on xbox_MmGetPhysicalAddress leaves a
 * bridge that stops delegating entirely green.
 */
/* kernel.h brings the NT type vocabulary: <windows.h> on Windows, the
 * shim in platform/xbox_winnt.h elsewhere. Nothing below is
 * platform-specific. */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; a regression harness has no title, and a
 * static archive resolves only what gets pulled in. Nothing here calls a
 * guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* The seam, plus the guest CPU state the dispatcher reads and writes. */
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const uint32_t THUNK_VA = 0x10000, STACK_VA = 0x20000;

    uint8_t *mem = VirtualAlloc(NULL, 16 * 1024 * 1024,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(mem != NULL, "guest memory stand-in allocated", NULL);
    if (!mem) {
        printf("\n%d failure(s); the rest needs guest memory.\n", failures);
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;

    /* The imports, in the form an XBE stores them: 0x80000000 | ordinal. */
    *(uint32_t *)(mem + THUNK_VA) = 0x80000000u | 173u;
    *(uint32_t *)(mem + THUNK_VA + 4) = 0x80000000u | 197u;
    *(uint32_t *)(mem + THUNK_VA + 8) = 0x80000000u | 189u;    /* NtCreateEvent */
    *(uint32_t *)(mem + THUNK_VA + 12) = 0x80000000u | 187u;   /* NtClose */
    *(uint32_t *)(mem + THUNK_VA + 16) = 0x80000000u | 225u;   /* NtSetEvent */
    xbox_kernel_set_thunk_address(THUNK_VA, 5);
    xbox_kernel_bridge_init();

    /* bridge_init rewrites the entry in place to a synthetic VA. */
    recomp_func_t fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA));
    check(fn != NULL, "ordinal 173 resolves to a bridge entry point",
          "the thunk entry was not rewritten to a synthetic VA");

    if (fn) {
        /* stdcall: the guest pushed a return address, then the argument. */
        uint32_t *sp = (uint32_t *)(mem + STACK_VA);
        sp[0] = 0xBEEF0001u;          /* guest return address */
        sp[1] = 0x80001000u;          /* inside the contiguous window */
        g_esp = STACK_VA;
        g_eax = 0xDEADBEEFu;
        fn();

        char detail[128];
        snprintf(detail, sizeof detail,
                 "bridge returned 0x%08X, expected 0x00001000", g_eax);
        check(g_eax == 0x00001000u,
              "ordinal 173 translates through the contiguous window", detail);

        /* And the argument must come off the guest stack. A wrong
         * stdcall_args_for_ordinal entry corrupts the caller's frame away
         * from the call, with nothing naming the ordinal. */
        snprintf(detail, sizeof detail, "esp is 0x%08X, expected 0x%08X",
                 g_esp, STACK_VA + 8);
        check(g_esp == STACK_VA + 8,
              "ordinal 173 pops its stdcall argument", detail);
    }

    /* Ordinal 197, NtDuplicateObject(Source, &Target, Options), on the
     * guest's pseudo-handles. They are 32-bit -1 (current process) and -2
     * (current thread); widened without sign extension they are no handle
     * at all, and the duplicate fails (Windows) or faults (POSIX shim). A
     * title duplicating NtCurrentThread() to keep a handle to itself is the
     * case that found this. */
    fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 4));
    check(fn != NULL, "ordinal 197 resolves to a bridge entry point", NULL);
    if (fn) {
        static const struct { uint32_t src; const char *what; } pseudo[] = {
            { 0xFFFFFFFEu, "NtDuplicateObject(NtCurrentThread()) succeeds" },
            { 0xFFFFFFFFu, "NtDuplicateObject(NtCurrentProcess()) succeeds" },
        };
        const uint32_t TARGET_VA = 0x30000;
        for (size_t k = 0; k < sizeof pseudo / sizeof pseudo[0]; k++) {
            uint32_t *sp = (uint32_t *)(mem + STACK_VA);
            sp[0] = 0xBEEF0002u;      /* guest return address */
            sp[1] = pseudo[k].src;    /* SourceHandle */
            sp[2] = TARGET_VA;        /* TargetHandle (out) */
            sp[3] = 0x2u;             /* DUPLICATE_SAME_ACCESS */
            *(uint32_t *)(mem + TARGET_VA) = 0;
            g_esp = STACK_VA;
            g_eax = 0xDEADBEEFu;
            fn();

            char detail[128];
            snprintf(detail, sizeof detail,
                     "status 0x%08X, target 0x%08X", g_eax,
                     *(uint32_t *)(mem + TARGET_VA));
            check(g_eax == 0 && *(uint32_t *)(mem + TARGET_VA) != 0,
                  pseudo[k].what, detail);
            snprintf(detail, sizeof detail, "esp is 0x%08X, expected 0x%08X",
                     g_esp, STACK_VA + 16);
            check(g_esp == STACK_VA + 16,
                  "ordinal 197 pops its three stdcall arguments", detail);
        }
    }

    /* A handle closed twice must not take a newer object with it. Fallout:
     * Brotherhood of Steel closes its .clp archives twice at every load; when
     * the freed token went straight to the next object, the second close
     * destroyed that object -- a Bink worker's event or mutex, with the
     * worker waiting on it for good. Event A is made and closed, event B is
     * made, A is closed again: B keeps its own token and can still be set. */
    {
        /* The dispatcher a lookup returns serves the entry looked up last, so
         * each call looks its entry up first. */
#define CALL(off) do { recomp_func_t f_ = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + (off))); \
                       g_esp = STACK_VA; if (f_) f_(); } while (0)
        const uint32_t HANDLE_VA = 0x30010;
        uint32_t *sp = (uint32_t *)(mem + STACK_VA);
        uint32_t a, b;
        char detail[128];

        sp[0] = 0xBEEF0003u; sp[1] = HANDLE_VA; sp[2] = 0; sp[3] = 1; sp[4] = 0;
        CALL(8);                                          /* NtCreateEvent(&A) */
        a = *(uint32_t *)(mem + HANDLE_VA);
        sp[0] = 0xBEEF0004u; sp[1] = a;
        CALL(12);                                         /* NtClose(A) */
        sp[0] = 0xBEEF0003u; sp[1] = HANDLE_VA; sp[2] = 0; sp[3] = 1; sp[4] = 0;
        CALL(8);                                          /* NtCreateEvent(&B) */
        b = *(uint32_t *)(mem + HANDLE_VA);
        sp[0] = 0xBEEF0004u; sp[1] = a;
        CALL(12);                                         /* NtClose(A), again */
        sp[0] = 0xBEEF0005u; sp[1] = b; sp[2] = 0;
        g_eax = 0xDEADBEEFu;
        CALL(16);                                         /* NtSetEvent(B) */
        {
            uint32_t set_b = g_eax;

            /* NtDuplicateObject(A', &D, CLOSE_SOURCE | SAME_ACCESS) closes A''s
             * host handle, and its token has to go with it. Windows may give
             * the duplicate the very value A' had, so a slot left behind named
             * the live duplicate: a close through the dead token A' -- a no-op
             * on the kernel -- destroyed D. */
            uint32_t a2, d;
            sp[0] = 0xBEEF0003u; sp[1] = HANDLE_VA; sp[2] = 0; sp[3] = 1; sp[4] = 0;
            CALL(8);                                      /* NtCreateEvent(&A') */
            a2 = *(uint32_t *)(mem + HANDLE_VA);
            sp[0] = 0xBEEF0006u; sp[1] = a2; sp[2] = 0x30020; sp[3] = 0x3u;
            CALL(4);                                      /* NtDuplicateObject */
            d = *(uint32_t *)(mem + 0x30020);
            sp[0] = 0xBEEF0004u; sp[1] = a2;
            CALL(12);                                     /* NtClose(A'), stale */
            sp[0] = 0xBEEF0005u; sp[1] = d; sp[2] = 0;
            g_eax = 0xDEADBEEFu;
            CALL(16);                                     /* NtSetEvent(D) */
            snprintf(detail, sizeof detail, "A' 0x%08X, D 0x%08X, NtSetEvent(D) 0x%08X",
                     a2, d, g_eax);
            check(d && d != a2 && g_eax == 0,
                  "a close through a DUPLICATE_CLOSE_SOURCE'd token spares the duplicate", detail);
            g_eax = set_b;
        }
#undef CALL
        {

            snprintf(detail, sizeof detail, "A 0x%08X, B 0x%08X", a, b);
            check(a && b && a != b, "a new object does not get a just-closed token", detail);
            snprintf(detail, sizeof detail, "NtSetEvent(B) status 0x%08X", g_eax);
            check(g_eax == 0, "closing A twice leaves B working", detail);
        }
    }

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
