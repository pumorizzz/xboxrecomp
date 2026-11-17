/*
 * text_block.h -- the lines of a periodic report written to a stream in one go.
 *
 * fbos makes stderr unbuffered (setvbuf _IONBF, so a crash loses nothing),
 * and the UCRT then formats each fprintf into a file or a pipe a character at
 * a time -- a WriteFile per byte. A 5 KB report was 5300 of them: 300-400 ms
 * on the executor thread every ten seconds under a PowerShell Start-Process
 * redirect, 80 ms in a console, while the game waited on its fence. Lines are
 * formatted into the caller's buffer and leave in one fwrite; a full block is
 * written and started again, so nothing is lost but the end of a single line
 * longer than TEXT_BLOCK_LINE. Test: tests/nv2a_points.
 */
#ifndef TEXT_BLOCK_H
#define TEXT_BLOCK_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TEXT_BLOCK_LINE 1024   /* one append, formatted on the stack */

typedef struct { char *buf; size_t cap, len; FILE *out; } TextBlock;

#if defined(__GNUC__)
static inline void text_block_printf(TextBlock *t, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#endif

static inline void text_block_init(TextBlock *t, char *buf, size_t cap, FILE *out)
{
    t->buf = buf;
    t->cap = cap;
    t->len = 0;
    t->out = out;
}

/* No fflush: the callers flush the stream themselves, as they did before. */
static inline void text_block_flush(TextBlock *t)
{
    if (t->len > 0) {
        fwrite(t->buf, 1, t->len, t->out);
        t->len = 0;
    }
}

static inline void text_block_printf(TextBlock *t, const char *fmt, ...)
{
    char line[TEXT_BLOCK_LINE];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line) {            /* cut: keep the line a line */
        n = (int)sizeof line - 1;
        line[n - 1] = '\n';
    }
    if (t->len + (size_t)n > t->cap)
        text_block_flush(t);
    if ((size_t)n > t->cap) {               /* longer than the block */
        fwrite(line, 1, (size_t)n, t->out);
        return;
    }
    memcpy(t->buf + t->len, line, (size_t)n);
    t->len += (size_t)n;
}

#endif
