/* Count the calls the code under test makes into the C runtime, by name
 * (tests/nv2a_points, tests/apu_irq): WriteFile (the report is a few writes,
 * not one per character) and getenv (a hot path must not walk the environment
 * on every call -- it is a linear search with a case-insensitive compare, and
 * long under Proton).
 *
 * The UCRT (ucrtbase.dll under /MD, the exe itself under /MT) reaches
 * WriteFile through its import table, and the exe reaches getenv through its
 * own: every entry of the name is pointed at a counter. Windows, /MD only for
 * getenv (a statically linked getenv has no import entry to patch: the hook
 * returns 0 and the count is skipped); elsewhere the count stays -1. */
#ifdef _WIN32
#include <windows.h>
#include <string.h>

static LONG s_writes, s_getenvs;
static BOOL (WINAPI *s_real_write)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static char *(__cdecl *s_real_getenv)(const char *);

static BOOL WINAPI counting_write(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, LPOVERLAPPED o)
{
    InterlockedIncrement(&s_writes);
    return s_real_write(h, b, n, w, o);
}

static char *__cdecl counting_getenv(const char *name)
{
    InterlockedIncrement(&s_getenvs);
    return s_real_getenv(name);
}

/* Every import entry of `name` in `mod` pointed at `repl`; the first original is kept in *real. */
static int patch_module(HMODULE mod, const char *name, void *repl, void **real)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *d;
    int n = 0;

    if (!mod)
        return 0;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *names, *funcs;
        if (!d->OriginalFirstThunk)
            continue;
        names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        funcs = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, funcs++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, name) != 0)
                continue;
            VirtualProtect(&funcs->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            if (!*real)
                *real = (void *)funcs->u1.Function;
            funcs->u1.Function = (ULONG_PTR)repl;
            VirtualProtect(&funcs->u1.Function, sizeof(void *), old, &old);
            n++;
        }
    }
    return n;
}

/* Count the calls of `name` ("WriteFile" or "getenv") from here on; the number
 * of import entries hooked (0: none found, nothing will be counted). */
int crt_count_hook(const char *name)
{
    void *repl = NULL, **real = NULL;
    if (strcmp(name, "WriteFile") == 0) {
        repl = (void *)counting_write;
        real = (void **)&s_real_write;
    } else if (strcmp(name, "getenv") == 0) {
        repl = (void *)counting_getenv;
        real = (void **)&s_real_getenv;
    } else {
        return 0;
    }
    return patch_module(GetModuleHandleA("ucrtbase.dll"), name, repl, real)
         + patch_module(GetModuleHandleA("ucrtbased.dll"), name, repl, real)
         + patch_module(GetModuleHandleA(NULL), name, repl, real);
}

long crt_count(const char *name)
{
    return strcmp(name, "getenv") == 0 ? s_getenvs : s_writes;
}

int crt_writes_hook(void) { return crt_count_hook("WriteFile"); }
long crt_writes(void) { return s_writes; }
#else
int crt_count_hook(const char *name) { (void)name; return 0; }
long crt_count(const char *name) { (void)name; return -1; }
int crt_writes_hook(void) { return 0; }
long crt_writes(void) { return -1; }
#endif
