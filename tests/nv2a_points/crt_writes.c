/* Count the WriteFile calls the C runtime makes (tests/nv2a_points: the report is a few writes, not one per character).
 *
 * The UCRT (ucrtbase.dll under /MD, the exe itself under /MT) reaches
 * WriteFile through its import table; every entry named WriteFile there is
 * pointed at a counter. Windows only -- elsewhere the count stays -1. */
#ifdef _WIN32
#include <windows.h>
#include <string.h>

static LONG s_calls;
static BOOL (WINAPI *s_real)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);

static BOOL WINAPI counting(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, LPOVERLAPPED o)
{
    InterlockedIncrement(&s_calls);
    return s_real(h, b, n, w, o);
}

static int patch_module(HMODULE mod)
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
            if (strcmp((const char *)ibn->Name, "WriteFile") != 0)
                continue;
            VirtualProtect(&funcs->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            if (!s_real)
                s_real = (void *)funcs->u1.Function;
            funcs->u1.Function = (ULONG_PTR)counting;
            VirtualProtect(&funcs->u1.Function, sizeof(void *), old, &old);
            n++;
        }
    }
    return n;
}

int crt_writes_hook(void)
{
    return patch_module(GetModuleHandleA("ucrtbase.dll"))
         + patch_module(GetModuleHandleA("ucrtbased.dll"))
         + patch_module(GetModuleHandleA(NULL));
}

long crt_writes(void) { return s_calls; }
#else
int crt_writes_hook(void) { return 0; }
long crt_writes(void) { return -1; }
#endif
