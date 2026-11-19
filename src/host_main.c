/*
 * host_main.c -- PC test harness for the Virtual Infocom Z-machine
 * core. Runs a story file on stdin/stdout so the interpreter logic
 * can be proven with scripted input before (and independently of)
 * the Virtual Boy build. Quetzal saves go to a file (default
 * ./zm_save.qut, override with the ZM_SAVE environment variable).
 *
 * Usage: zmhost <story-file>
 * Exit status mirrors zm_run: 1 = clean @quit, 255 = fatal/EOF.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zmachine.h"

static FILE *save_fp;
static char save_path[512] = "zm_save.qut";
static char last_status[ZM_UPPER_COLS + 1];
static char char_buf[256];
static int char_pos, char_len;

static void h_put_char(Zm *zm, uint16_t c)
{
    (void)zm;
    if (c == 13)
        putchar('\n');
    else
        putchar((int)(c & 0xFF));
}

static void h_flush(Zm *zm)
{
    (void)zm;
    fflush(stdout);
}

static void h_draw_upper(Zm *zm, const uint16_t *cells, int width, int lines)
{
    char buf[ZM_UPPER_COLS + 1];
    int i, n;
    (void)zm;
    if (lines < 1)
        return;
    n = width > ZM_UPPER_COLS ? ZM_UPPER_COLS : width;
    for (i = 0; i < n; i++)
        buf[i] = (char)(cells[i] & 0xFF);
    buf[n] = 0;
    /* trim trailing spaces for display */
    while (n > 0 && buf[n - 1] == ' ')
        buf[--n] = 0;
    if (strcmp(buf, last_status) != 0) {
        strcpy(last_status, buf);
        printf("[STATUS] %s\n", buf);
        fflush(stdout);
    }
}

static void h_clear_screen(Zm *zm)
{
    (void)zm;
}

static void h_beep(Zm *zm)
{
    (void)zm;
}

static int h_get_line(Zm *zm, char *buf, int max, int *len_out,
                      const char *initial, int initial_len,
                      int timeout_tenths, int *term_out)
{
    int n = 0;
    (void)zm;
    (void)initial;
    (void)initial_len;
    (void)timeout_tenths;
    if (!fgets(buf, max + 1, stdin)) {
        *len_out = 0;
        return ZM_LINE_EOF;
    }
    n = (int)strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    *len_out = n;
    *term_out = 13;
    return ZM_LINE_OK;
}

static int h_get_char(Zm *zm)
{
    (void)zm;
    if (char_pos >= char_len) {
        if (!fgets(char_buf, sizeof char_buf, stdin))
            return ZM_LINE_EOF;
        char_len = (int)strlen(char_buf);
        char_pos = 0;
    }
    {
        char c = char_buf[char_pos++];
        if (c == '\n')
            return 13;
        return (uint8_t)c;
    }
}

static uint32_t h_rand32(Zm *zm)
{
    (void)zm;
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

static int h_save_begin(Zm *zm)
{
    (void)zm;
    save_fp = fopen(save_path, "wb");
    return save_fp ? 0 : -1;
}

static int h_save_byte(Zm *zm, int b)
{
    (void)zm;
    if (!save_fp)
        return -1;
    return fputc(b, save_fp) == EOF ? -1 : 0;
}

static int h_save_end(Zm *zm)
{
    (void)zm;
    if (save_fp)
        fclose(save_fp);
    save_fp = NULL;
    return 0;
}

static int h_restore_begin(Zm *zm)
{
    (void)zm;
    save_fp = fopen(save_path, "rb");
    return save_fp ? 0 : -1;
}

static int h_restore_byte(Zm *zm)
{
    int c;
    (void)zm;
    if (!save_fp)
        return -1;
    c = fgetc(save_fp);
    return c == EOF ? -1 : c;
}

static void h_restore_end(Zm *zm)
{
    (void)zm;
    if (save_fp)
        fclose(save_fp);
    save_fp = NULL;
}

static void h_fatal(Zm *zm, const char *msg)
{
    (void)zm;
    fprintf(stderr, "\n[FATAL] %s\n", msg);
}

#ifdef ZM_TRACE
void zm_trace_hook(uint32_t pc, int byte)
{
    fprintf(stderr, "  trace pc=0x%04X op=0x%02X\n", pc, byte);
}
#endif

static const ZmIO io = {
    .user = NULL,
    .put_char = h_put_char,
    .flush = h_flush,
    .draw_upper = h_draw_upper,
    .clear_screen = h_clear_screen,
    .beep = h_beep,
    .get_line = h_get_line,
    .get_char = h_get_char,
    .get_char_timed = NULL,
    .rand32 = h_rand32,
    .seed_rand = NULL,
    .save_begin = h_save_begin,
    .save_byte = h_save_byte,
    .save_end = h_save_end,
    .restore_begin = h_restore_begin,
    .restore_byte = h_restore_byte,
    .restore_end = h_restore_end,
    .fatal = h_fatal,
};

static Zm zm;
static uint8_t dyn_buf[65536];
static uint8_t undo_dyn_buf[65536];
static uint16_t undo_stack_buf[ZM_STACK_WORDS];
static ZmFrame undo_frames_buf[ZM_MAX_FRAMES];

int main(int argc, char **argv)
{
    FILE *f;
    uint8_t *story;
    long len;
    char err[128];
    const char *env;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: zmhost <story-file>\n");
        return 2;
    }
    env = getenv("ZM_SAVE");
    if (env) {
        strncpy(save_path, env, sizeof save_path - 1);
        save_path[sizeof save_path - 1] = 0;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    story = malloc((size_t)len);
    if (!story || fread(story, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);

    srand((unsigned)(time(NULL) ^ len));
    last_status[0] = 0;
    char_pos = char_len = 0;

    if (zm_init(&zm, story, (uint32_t)len, dyn_buf, sizeof dyn_buf,
                undo_dyn_buf, undo_stack_buf, undo_frames_buf,
                &io, err, sizeof err)) {
        fprintf(stderr, "init failed: %s\n", err);
        return 2;
    }
    rc = zm_run(&zm);
    fprintf(stderr, "\n[zm_run returned %d]\n", rc);
    return rc == 1 ? 0 : 255;
}

#ifdef ZM_TRACE
void zm_trace2_hook(uint32_t before, uint32_t after)
{
    fprintf(stderr, "  step 0x%04X -> next pc 0x%04X\n", before, after);
}
#endif
