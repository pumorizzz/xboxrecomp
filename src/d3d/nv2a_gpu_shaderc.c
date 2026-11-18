/*
 * nv2a_gpu_shaderc -- the D3D11 back end's shaders compiled when the toolkit
 * is built, so the title does not wait for them at start (the combiner shader
 * takes D3DCompile ~12 s: its registers kept in registers, see rc_eval).
 *
 *   nv2a_gpu_shaderc <in.hlsl> <out.h> [--rc-keys <file>]
 *                    <entry>:<target>:<symbol>[:<NAME>=<value>,...]...
 *
 * Each entry point is compiled with D3DCompile as the back end compiled it at
 * run time (D3DCOMPILE_OPTIMIZATION_LEVEL3, source name "nv2a_gpu_d3d11"),
 * on a pool of threads (one per logical CPU at most). out.h gets the code of
 * each as `static const unsigned char <symbol>[]` and the source itself as
 * `static const char s_hlsl[]` (what is still compiled at run time from it).
 *
 * --rc-keys <file>: the register combiner states a game uses (nv2a_gpu_rc_key.h
 * text, one `rc ...` line each; empty lines and '#' lines are comments). For
 * line i the combiner shader ps_rc is compiled with the key's values as macros
 * (RCK, RCK_N, RCK_C0.. -- nv2a_rc_key_macros) into `k_ps_rck_<i>`, a shader
 * that holds that state's code only. out.h then also has
 *   #define NV2A_GPU_RC_KEYS N
 *   k_rc_key[N][37]          the key words, the line above each array is the key
 *   k_rc_key_code[N], k_rc_key_len[N], k_rc_key_name[N]
 * (one zeroed element when N is 0). A line that does not parse, or repeats an
 * earlier one, or that nv2a_rc_key_make would not build from its own fields (not
 * canonical: it could never match a draw), stops the build with its number.
 * A host tool of the build (nv2a_gpu_shaders.cmake); exit 1 when one shader does
 * not compile, with the compiler's message.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_gpu_rc_key.h"

#define MAX_MACROS 48

typedef struct {
    char entry[64], target[16], symbol[64];
    D3D_SHADER_MACRO macros[MAX_MACROS + 1];
    char names[MAX_MACROS][64], values[MAX_MACROS][64];
    const char *src;
    size_t len;
    ID3DBlob *code, *err;
    HRESULT hr;
    double secs;
    int key;                            /* the rc key line index, or -1 */
} Spec;

static LONG s_next;
static int s_n;
static Spec *s_spec;

static int parse(Spec *s, const char *arg)
{
    char buf[512], *p, *f[4];
    int n = 0, k = 0;

    memset(s, 0, sizeof *s);
    s->key = -1;
    if (strlen(arg) >= sizeof buf)
        return 0;
    strcpy(buf, arg);
    for (p = buf; n < 4; n++) {
        f[n] = p;
        p = n < 3 ? strchr(p, ':') : NULL;
        if (!p) { n++; break; }
        *p++ = 0;
    }
    if (n < 3)
        return 0;
    strncpy(s->entry, f[0], sizeof s->entry - 1);
    strncpy(s->target, f[1], sizeof s->target - 1);
    strncpy(s->symbol, f[2], sizeof s->symbol - 1);
    if (n == 4) {
        char *m = f[3], *next;
        for (; m && *m && k < MAX_MACROS; m = next) {
            char *eq;
            next = strchr(m, ',');
            if (next)
                *next++ = 0;
            eq = strchr(m, '=');
            if (eq)
                *eq++ = 0;
            strncpy(s->names[k], m, 63);
            strncpy(s->values[k], eq ? eq : "1", 63);
            s->macros[k].Name = s->names[k];
            s->macros[k].Definition = s->values[k];
            k++;
        }
    }
    return 1;
}

static void compile_one(Spec *s)
{
    LARGE_INTEGER a, b, f;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    s->hr = D3DCompile(s->src, s->len, "nv2a_gpu_d3d11", s->macros[0].Name ? s->macros : NULL,
                       NULL, s->entry, s->target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                       &s->code, &s->err);
    QueryPerformanceCounter(&b);
    s->secs = (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart;
}

/* A worker takes the next spec until there are none. */
static DWORD WINAPI worker(void *arg)
{
    LONG i;

    (void)arg;
    while ((i = InterlockedIncrement(&s_next) - 1) < s_n)
        compile_one(&s_spec[i]);
    return 0;
}

static char *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)*len + 1);
    if (!buf || fread(buf, 1, (size_t)*len, f) != (size_t)*len) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[*len] = 0;
    return buf;
}

/* The keys of a file; exits 1 with the line number on a bad or repeated line. */
static int read_keys(const char *path, uint32_t (**keys)[NV2A_RC_KEY_WORDS])
{
    long len;
    char *txt = read_file(path, &len), *line, *next;
    int n = 0, cap = 0, ln = 0, i;

    if (!txt) {
        fprintf(stderr, "nv2a_gpu_shaderc: cannot read %s\n", path);
        exit(1);
    }
    *keys = NULL;
    for (line = txt; line; line = next, ln++) {
        const char *p = line;
        uint32_t w[NV2A_RC_KEY_WORDS];
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (!*p || *p == '#')
            continue;
        if (!nv2a_rc_key_parse(p, w)) {
            fprintf(stderr, "%s(%d): not a combiner state line: %s\n", path, ln + 1, p);
            exit(1);
        }
        /* The back end builds its key with nv2a_rc_key_make, which masks the texture
         * classes by stage mode and drops what the shader never branches on. A line
         * that make would not produce can never match a draw: its shader would be
         * built and never used, so it is refused here. */
        {
            uint32_t txf[4], canon[NV2A_RC_KEY_WORDS], st;
            for (st = 0; st < 4; st++)
                txf[st] = (w[4] >> (8 * st)) & 0xFFu;
            nv2a_rc_key_make(canon, w + 5, w + 13, w + 21, w + 29, w[0], w[1], w[2], w[3], txf);
            if (memcmp(canon, w, sizeof w)) {
                char c[1024];
                if (nv2a_rc_key_format(canon, c, sizeof c) < 0)
                    c[0] = 0;
                fprintf(stderr, "%s(%d): not a state the back end builds (a texture class the stage "
                                "mode does not read, or a bit outside the key); it would be: %s\n",
                        path, ln + 1, c);
                exit(1);
            }
        }
        for (i = 0; i < n; i++)
            if (!memcmp((*keys)[i], w, sizeof w)) {
                fprintf(stderr, "%s(%d): the same state as line %d of the keys\n", path, ln + 1, i + 1);
                exit(1);
            }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            *keys = (uint32_t (*)[NV2A_RC_KEY_WORDS])realloc(*keys, (size_t)cap * sizeof **keys);
        }
        memcpy((*keys)[n++], w, sizeof w);
    }
    free(txt);
    return n;
}

int main(int argc, char **argv)
{
    HANDLE th[64];
    char *src;
    long len;
    int nfix = 0, nkeys = 0, i, a, fail = 0, nth;
    size_t k;
    FILE *f;
    char tmp[1024];
    const char *keyfile = NULL;
    const char *specs[256];
    uint32_t (*keys)[NV2A_RC_KEY_WORDS] = NULL;
    SYSTEM_INFO si;
    LARGE_INTEGER t0, t1, qf;

    if (argc < 4) {
        fprintf(stderr, "usage: nv2a_gpu_shaderc <in.hlsl> <out.h> [--rc-keys <file>]"
                        " <entry>:<target>:<symbol>[:NAME=value,...]...\n");
        return 1;
    }
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&t0);
    for (a = 3; a < argc; a++) {
        if (!strcmp(argv[a], "--rc-keys") && a + 1 < argc) {
            keyfile = argv[++a];
        } else if (nfix < 256) {
            specs[nfix++] = argv[a];
        }
    }
    src = read_file(argv[1], &len);
    if (!src) {
        fprintf(stderr, "nv2a_gpu_shaderc: cannot read %s\n", argv[1]);
        return 1;
    }
    if (keyfile)
        nkeys = read_keys(keyfile, &keys);
    s_n = nfix + nkeys;
    s_spec = (Spec *)calloc((size_t)(s_n ? s_n : 1), sizeof(Spec));
    if (!s_spec)
        return 1;
    for (i = 0; i < nfix; i++)
        if (!parse(&s_spec[i], specs[i])) {
            fprintf(stderr, "nv2a_gpu_shaderc: bad spec %s\n", specs[i]);
            return 1;
        }
    for (i = 0; i < nkeys; i++) {
        Spec *s = &s_spec[nfix + i];
        char nm[MAX_MACROS][16], val[MAX_MACROS][16];
        int m, c = nv2a_rc_key_macros(keys[i], nm, val, MAX_MACROS);
        memset(s, 0, sizeof *s);
        strcpy(s->entry, "ps_rc");
        strcpy(s->target, "ps_4_0");
        snprintf(s->symbol, sizeof s->symbol, "k_ps_rck_%02d", i);
        s->key = i;
        for (m = 0; m < c; m++) {
            strcpy(s->names[m], nm[m]);
            strcpy(s->values[m], val[m]);
            s->macros[m].Name = s->names[m];
            s->macros[m].Definition = s->values[m];
        }
    }
    for (i = 0; i < s_n; i++) {
        s_spec[i].src = src;
        s_spec[i].len = (size_t)len;
    }
    GetSystemInfo(&si);
    nth = (int)si.dwNumberOfProcessors;
    if (nth < 1) nth = 1;
    if (nth > 64) nth = 64;
    if (nth > s_n) nth = s_n;
    for (i = 0; i < nth; i++) {
        th[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!th[i])
            worker(NULL);
    }
    for (i = 0; i < nth; i++)
        if (th[i]) {
            WaitForSingleObject(th[i], INFINITE);
            CloseHandle(th[i]);
        }
    for (i = 0; i < s_n; i++)
        if (FAILED(s_spec[i].hr) || !s_spec[i].code) {
            fprintf(stderr, "nv2a_gpu_shaderc: %s (%s) does not compile: 0x%08lX\n%s\n",
                    s_spec[i].symbol, s_spec[i].target, (unsigned long)s_spec[i].hr,
                    s_spec[i].err ? (const char *)s_spec[i].err->lpVtbl->GetBufferPointer(s_spec[i].err)
                                  : "");
            fail = 1;
        }
    if (fail)
        return 1;
    for (i = nfix; i < s_n; i++)
        printf("nv2a_gpu_shaderc: ps_rck_%02d (n=%u): %.1f s, %zu bytes\n", i - nfix,
               (unsigned)(keys[i - nfix][0] & 0xFF), s_spec[i].secs,
               s_spec[i].code->lpVtbl->GetBufferSize(s_spec[i].code));

    /* Written beside and moved over: a build stopped halfway leaves no half
     * header behind. */
    snprintf(tmp, sizeof tmp, "%s.tmp", argv[2]);
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "nv2a_gpu_shaderc: cannot write %s\n", tmp);
        return 1;
    }
    fprintf(f, "/* Generated by nv2a_gpu_shaderc from nv2a_gpu_shaders.hlsl: do not edit. */\n");
    fprintf(f, "#include <stddef.h>\n#include <stdint.h>\n");
    fprintf(f, "static const char s_hlsl[] =\n\"");
    for (k = 0; k < (size_t)len; k++) {
        unsigned char c = (unsigned char)src[k];
        if (c == '\n')
            fputs("\\n\"\n\"", f);
        else if (c == '\r')
            ;
        else if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c < 0x20 || c >= 0x7F)
            fprintf(f, "\\%03o", c);
        else
            fputc(c, f);
    }
    fprintf(f, "\";\n");
    for (i = 0; i < s_n; i++) {
        const unsigned char *b = (const unsigned char *)s_spec[i].code->lpVtbl->GetBufferPointer(s_spec[i].code);
        size_t sz = s_spec[i].code->lpVtbl->GetBufferSize(s_spec[i].code);
        if (s_spec[i].key >= 0) {
            char key[1024];
            if (nv2a_rc_key_format(keys[s_spec[i].key], key, sizeof key) < 0)
                key[0] = 0;
            fprintf(f, "/* %s */\n", key);
        }
        fprintf(f, "/* %s, %s, %zu bytes */\nstatic const unsigned char %s[] = {", s_spec[i].entry,
                s_spec[i].target, sz, s_spec[i].symbol);
        for (k = 0; k < sz; k++)
            fprintf(f, "%s0x%02X,", (k % 16) ? "" : "\n", b[k]);
        fprintf(f, "\n};\n");
    }
    fprintf(f, "#define NV2A_GPU_RC_KEYS %d\n", nkeys);
    fprintf(f, "static const uint32_t k_rc_key[%d][%d] = {\n", nkeys ? nkeys : 1, NV2A_RC_KEY_WORDS);
    for (i = 0; i < (nkeys ? nkeys : 1); i++) {
        int w;
        fprintf(f, "    {");
        for (w = 0; w < NV2A_RC_KEY_WORDS; w++)
            fprintf(f, "%s0x%08Xu", w ? "," : "", nkeys ? (unsigned)keys[i][w] : 0u);
        fprintf(f, "},\n");
    }
    fprintf(f, "};\nstatic const unsigned char *const k_rc_key_code[%d] = {", nkeys ? nkeys : 1);
    for (i = 0; i < nkeys; i++)
        fprintf(f, "%sk_ps_rck_%02d", i ? ", " : "", i);
    fprintf(f, "%s};\nstatic const size_t k_rc_key_len[%d] = {", nkeys ? "" : "0", nkeys ? nkeys : 1);
    for (i = 0; i < nkeys; i++)
        fprintf(f, "%ssizeof k_ps_rck_%02d", i ? ", " : "", i);
    fprintf(f, "%s};\nstatic const char *const k_rc_key_name[%d] = {", nkeys ? "" : "0", nkeys ? nkeys : 1);
    for (i = 0; i < nkeys; i++)
        fprintf(f, "%s\"ps_rck_%02d\"", i ? ", " : "", i);
    fprintf(f, "%s};\n", nkeys ? "" : "\"\"");
    fclose(f);
    if (!MoveFileExA(tmp, argv[2], MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "nv2a_gpu_shaderc: cannot write %s\n", argv[2]);
        return 1;
    }
    QueryPerformanceCounter(&t1);
    printf("nv2a_gpu_shaderc: %d shaders (%d combiner states) in %.1f s\n", s_n, nkeys,
           (double)(t1.QuadPart - t0.QuadPart) / (double)qf.QuadPart);
    return 0;
}
