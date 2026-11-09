/*
 * Where the framebuffer window puts the 640x480 frame (src/video/fb_present.c,
 * xbox_FramebufferWindowFit): the largest rectangle of the display aspect,
 * centred in the client area. The D3D11 back end draws its swap chain into the
 * same rectangle, so a widescreen-patched title (anamorphic 640x480) fills a
 * 2560x1440 picture, and a 4:3 one gets bars at the sides.
 */
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#include "keyboard.h"

/* What fb_present.c expects the runtime to provide. */
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
void xbox_InputKeySet(int vk, int down) { (void)vk; (void)down; }
void xbox_InputKeysClear(void) {}

void xbox_FramebufferWindowSetAspect(uint32_t n, uint32_t d);
void xbox_FramebufferWindowFit(uint32_t cw, uint32_t ch, int32_t out[4]);

static int failures;

static void expect(uint32_t cw, uint32_t ch, int x, int y, int w, int h, const char *what)
{
    int32_t r[4];

    xbox_FramebufferWindowFit(cw, ch, r);
    if (r[0] == x && r[1] == y && r[2] == w && r[3] == h) {
        printf("  PASS  %s\n", what);
        return;
    }
    printf("  FAIL  %s: %ux%u -> %d,%d %dx%d, want %d,%d %dx%d\n", what, cw, ch,
           r[0], r[1], r[2], r[3], x, y, w, h);
    failures++;
}

int main(void)
{
    printf("4:3 (default)\n");
    expect(640, 480, 0, 0, 640, 480, "640x480 shows the frame 1:1");
    expect(2560, 1440, 320, 0, 1920, 1440, "2560x1440 pillarboxes to 1920x1440");
    expect(800, 1000, 0, 200, 800, 600, "a tall window letterboxes");

    xbox_FramebufferWindowSetAspect(16, 9);
    printf("16:9\n");
    expect(2560, 1440, 0, 0, 2560, 1440, "2560x1440 is filled");
    expect(1920, 1080, 0, 0, 1920, 1080, "1920x1080 is filled");
    expect(640, 480, 0, 60, 640, 360, "640x480 letterboxes to 640x360");
    expect(2560, 1600, 0, 80, 2560, 1440, "16:10 letterboxes");
    expect(3440, 1440, 440, 0, 2560, 1440, "21:9 pillarboxes");

    xbox_FramebufferWindowSetAspect(0, 9);
    expect(2560, 1440, 0, 0, 2560, 1440, "a zero aspect term is ignored");

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
