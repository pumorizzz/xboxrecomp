/*
 * usb_pad_record_test -- the pad recorder in src/usb/usb_gamepad.c.
 *
 * One process per mode, because RECOMP_PB_EXEC is read once:
 *
 *   exec     the executor is on, so flips tick:
 *     1. F9 records nothing. F9 is RECOMP_MKB's capture toggle (mkb_input.c);
 *        recording on it made one key do both.
 *     2. F8 starts and stops a recording; each change of the report is a line
 *        "<flips since start> <bytes 2..19>", and stopping writes a last line
 *        with everything released.
 *     3. F10 plays it back: the recorded button is held for exactly its flips
 *        and the playback ends on the last line.
 *     4. A second replay starts from the first step again. Playback steps
 *        forward from where it was, and a position that is not reset made the
 *        second replay end at once.
 *   no-exec  there is no executor, so nothing flips and time cannot be kept:
 *     5. F8 neither starts a recording nor overwrites the file.
 *     6. F10 does not start a replay (every step would be at 0 and the first
 *        would be held for good).
 */
#include "usb_gamepad.h"
#include "xinput_xbox.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static uint8_t s_keys[256];
static uint32_t s_flips;
static XBOX_INPUT_STATE s_state;

int xbox_FramebufferKeyDown(int vk) { return vk >= 0 && vk < 256 && s_keys[vk]; }
uint32_t xbox_Nv2aFlipCount(void) { return s_flips; }
DWORD xbox_InputGetState(DWORD port, XBOX_INPUT_STATE *st)
{
    if (port != 0)
        return 1167;
    *st = s_state;
    return 0;
}

static void check(int ok, const char *what, const char *detail)
{
    if (!ok) {
        printf("FAIL: %s (%s)\n", what, detail ? detail : "");
        failures++;
    }
}

static uint8_t s_out[20];
static void report(void) { usb_gamepad_report(0, s_out, 20); }
static void press(int vk)
{
    s_keys[vk] = 1; report();
    s_keys[vk] = 0; report();
}

static char s_path[MAX_PATH + 64];
static int read_file(char *buf, size_t cap)
{
    FILE *f = fopen(s_path, "rb");
    size_t n;
    if (!f) return -1;
    n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    return (int)n;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "exec";
    int exec = !strcmp(mode, "exec");
    char buf[4096];
    size_t n;

    n = GetTempPathA(MAX_PATH, s_path);
    snprintf(s_path + n, sizeof s_path - n, "usb_pad_record_%lu.txt", (unsigned long)GetCurrentProcessId());
    DeleteFileA(s_path);
    _putenv_s("RECOMP_PAD_RECORD", s_path);
    _putenv_s("RECOMP_PB_EXEC", exec ? "1" : "");   /* an empty value removes it */
    printf("mode: %s\n", mode);

    if (exec) {
        char *p, *line;
        int step = 0, got[4] = {-1, -1, -1, -1}, ab[4] = {-1, -1, -1, -1};

        press(0x78);                                  /* F9 */
        check(read_file(buf, sizeof buf) < 0, "F9 records nothing", NULL);

        s_flips = 100;
        press(0x77);                                  /* F8 */
        check(read_file(buf, sizeof buf) > 0 && buf[0] == '#', "F8 starts a recording", buf);

        s_flips = 110; s_state.Gamepad.bAnalogButtons[0] = 0xFF; report();
        s_flips = 115; s_state.Gamepad.bAnalogButtons[0] = 0; report();
        s_flips = 120; press(0x77);                   /* stops */

        check(read_file(buf, sizeof buf) > 0, "the recording is there", NULL);
        for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
            if (*line == '#') continue;
            if (step < 4) {
                got[step] = atoi(line);
                p = strchr(line, ' ');
                ab[step] = (p && strlen(p + 1) >= 6) ? (int)strtoul((char[]){p[5], p[6], 0}, NULL, 16) : -2;
            }
            step++;
        }
        check(step == 4 && got[0] == 0 && got[1] == 10 && got[2] == 15 && got[3] == 20,
              "four steps at flips 0, 10, 15, 20", NULL);
        check(ab[0] == 0 && ab[1] == 0xFF && ab[2] == 0 && ab[3] == 0,
              "the analog A byte is 0, FF, 0, 0", NULL);

        s_flips = 200; press(0x79);                   /* F10 */
        s_flips = 205; report(); check(s_out[4] == 0, "replay: released at 5", NULL);
        s_flips = 210; report(); check(s_out[4] == 0xFF, "replay: A held at 10", NULL);
        s_flips = 215; report(); check(s_out[4] == 0, "replay: released at 15", NULL);
        s_flips = 220; report(); check(s_out[4] == 0, "replay: ends at 20", NULL);
        s_flips = 225; report(); check(s_out[4] == 0, "after the replay: released", NULL);

        s_flips = 300; press(0x79);
        s_flips = 310; report();
        check(s_out[4] == 0xFF, "a second replay starts from the first step", NULL);
    } else {
        char before[256], after[256];
        FILE *f = fopen(s_path, "wb");
        int k;
        if (!f) { printf("FAIL: cannot write %s\n", s_path); return 1; }
        fputs("# t\n0 0000FF", f);
        for (k = 0; k < 15; k++) fputs("00", f);
        fputs("\n10 ", f);
        for (k = 0; k < 18; k++) fputs("00", f);
        fputc('\n', f);
        fclose(f);
        read_file(before, sizeof before);

        press(0x77);
        read_file(after, sizeof after);
        check(!strcmp(before, after), "F8 without the executor leaves the file alone", after);

        s_flips = 0; press(0x79); report();
        check(s_out[4] == 0, "F10 without the executor starts no replay", NULL);
    }

    DeleteFileA(s_path);
    printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
