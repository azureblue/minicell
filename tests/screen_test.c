/* White-box screen tests: output goes to a pipe instead of a terminal. */
#include "../src/screen.c"

#include "check.h"

#include <signal.h>

typedef struct {
    mc_screen *s;
    int out; /* read end of the pipe the screen writes to */
} fixture;

static fixture open_fixture(const char *term, int width, int height) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    mc_screen *s = calloc(1, sizeof(*s));
    CHECK(s);
    s->fd = fds[1];
    s->term = malloc(strlen(term) + 1);
    CHECK(s->term);
    strcpy(s->term, term);
    s->colors = detect_colors(term);
    s->legacy = is_legacy(term);
    s->style = mc_style_default();
    s->cursor_x = s->cursor_y = s->shown_x = s->shown_y = -1;
    s->width = width;
    s->height = height;
    s->back = malloc(cell_count(s) * sizeof(cell));
    s->front = malloc(cell_count(s) * sizeof(cell));
    CHECK(s->back && s->front);
    fill(s->back, cell_count(s), s->style);
    s->redraw = true;
    return (fixture){s, fds[0]};
}

static void close_fixture(fixture f) {
    close(f.out);
    destroy(f.s);
}

/* Returns everything written since the last call, NUL-terminated. */
static const char *output(fixture f) {
    static char buf[65536];
    ssize_t n = read(f.out, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    return buf;
}

static uint32_t char_at(const mc_screen *s, int x, int y) {
    return s->back[(size_t)y * s->width + x].ch;
}

/* What tcell's draw() wraps every update in, for a 12x3 screen. */
#define BEGIN "\033[?2026h\033[4;13H\033[?25l"
#define END "\033[4;13H\033[?25l\033[?2026l"

static void test_colors(void) {
    static const struct {
        const char *term, *colorterm;
        int colors;
    } cases[] = {
        {"xterm-256color", "", 256},  {"xterm", "", 8},       {"any-terminal", "", 256},
        {"tmux-256color", "", 256},   {"screen", "", 256},    {"linux", "", 8},
        {"xterm-color", "", 8},       {"rxvt-16color", "", 16}, {"vt100", "", 0},
        {"xterm", "truecolor", 256},  {"xterm", "256", 256},  {"", "", 256},
    };
    for (size_t i = 0; i < COUNT(cases); i++) {
        CHECK(setenv("COLORTERM", cases[i].colorterm, 1) == 0);
        CHECK(detect_colors(cases[i].term) == cases[i].colors);
    }
    CHECK(setenv("NO_COLOR", "1", 1) == 0);
    CHECK(detect_colors("xterm-256color") == 0);
    CHECK(unsetenv("NO_COLOR") == 0 && unsetenv("COLORTERM") == 0);

    CHECK(is_legacy("linux") && is_legacy("vt220") && is_legacy("ansi"));
    CHECK(!is_legacy("xterm-256color") && !is_legacy("tmux-256color"));
}

static void test_diff(void) {
    fixture f = open_fixture("xterm-256color", 12, 3);
    mc_screen *s = f.s;
    const char *out = NULL;
    CHECK(mc_screen_show(s) == 0 && (out = output(f)));
    CHECK(strncmp(out, BEGIN "\033[m\033[H\033[J", strlen(BEGIN) + 9) == 0);
    CHECK(mc_screen_show(s) == 0 && !*output(f)); /* nothing changed */

    CHECK(mc_screen_put_str(s, 0, 0, "żółw") == 4);
    CHECK(mc_screen_show(s) == 0);
    CHECK(strcmp(output(f), BEGIN "\033[1;1H\033[mżółw" END) == 0);

    /* Only the changed cell is written. */
    CHECK(mc_screen_put_str(s, 2, 0, "o") == 1);
    CHECK(mc_screen_show(s) == 0 && strcmp(output(f), BEGIN "\033[1;3H\033[mo" END) == 0);

    /* Redrawing the same content produces no output. */
    mc_screen_clear(s);
    CHECK(mc_screen_put_str(s, 0, 0, "żóow") == 4);
    CHECK(mc_screen_show(s) == 0 && !*output(f));

    CHECK(mc_screen_sync(s) == 0 && strstr(output(f), "\033[H\033[J"));
    close_fixture(f);
}

static void test_clipping(void) {
    fixture f = open_fixture("xterm-256color", 12, 3);
    mc_screen *s = f.s;
    CHECK(mc_screen_put_str(s, 10, 1, "abc") == 3);
    CHECK(char_at(s, 10, 1) == 'a' && char_at(s, 11, 1) == 'b');
    CHECK(mc_screen_put_str(s, -1, 2, "xy") == 2);
    CHECK(char_at(s, 0, 2) == 'y');
    CHECK(mc_screen_put_str(s, 0, 7, "off screen") == 10);
    CHECK(mc_screen_set_content(s, 99, 99, 'z', mc_style_default()) == 0);
    close_fixture(f);
}

static void test_put(void) {
    fixture f = open_fixture("xterm-256color", 12, 3);
    mc_screen *s = f.s;
    mc_style st = mc_style_default();
    const char *rest = mc_screen_put(s, 0, 0, "ąb", st);
    CHECK(rest && strcmp(rest, "b") == 0 && char_at(s, 0, 0) == 0x105);
    rest = mc_screen_put(s, 0, 0, "", st);
    CHECK(rest && !*rest);
    CHECK(mc_screen_set_content(s, 1, 0, MC_RuneULCorner, st) == 0);
    CHECK(char_at(s, 1, 0) == 0x250c);
    close_fixture(f);
}

static void test_rejected_input(void) {
    fixture f = open_fixture("xterm-256color", 12, 3);
    mc_screen *s = f.s;
    mc_style st = mc_style_default();
    errno = 0;
    CHECK(!mc_screen_put(s, 0, 0, "\033[2J", st) && errno == EINVAL);
    CHECK(!mc_screen_put(s, 0, 0, "\xc2\x9b", st) && errno == EINVAL); /* C1 CSI */
    CHECK(!mc_screen_put(s, 0, 0, "\xff", st) && errno == EILSEQ);
    CHECK(mc_screen_put_str(s, 0, 0, "ok\n") == -1 && errno == EINVAL);
    CHECK(mc_screen_set_content(s, 0, 0, 0xd800, st) == -1 && errno == EINVAL);
    CHECK(mc_screen_set_style(s, mc_style_foreground(st, 256)) == -1 && errno == EINVAL);
    CHECK(mc_screen_set_style(s, mc_style_background(st, -2)) == -1 && errno == EINVAL);
    CHECK(mc_screen_set_style(s, mc_style_attributes(st, 1 << 7)) == -1 && errno == EINVAL);
    close_fixture(f);
}

static void test_styles(void) {
    fixture f = open_fixture("xterm-256color", 12, 3);
    mc_screen *s = f.s;
    mc_style st = mc_style_default();
    CHECK(mc_screen_show(s) == 0 && output(f));
    CHECK(mc_screen_put_str_styled(s, 0, 0, "a", mc_style_foreground(st, 3)) == 1);
    CHECK(mc_screen_put_str_styled(s, 1, 0, "b", mc_style_foreground(st, 12)) == 1);
    CHECK(mc_screen_put_str_styled(s, 2, 0, "c", mc_style_background(st, 200)) == 1);
    CHECK(mc_screen_put_str_styled(s, 3, 0, "d",
                                   mc_style_attributes(st, MC_AttrBold | MC_AttrUnderline)) == 1);
    CHECK(mc_screen_show(s) == 0);
    CHECK(strcmp(output(f), BEGIN "\033[1;1H\033[m\033[33ma\033[m\033[38;5;12mb"
                                  "\033[m\033[48;5;200mc\033[m\033[1m\033[4md" END) == 0);
    close_fixture(f);
}

/* Colors the terminal lacks are drawn in the default color. */
static void test_limited_palette(void) {
    fixture f = open_fixture("linux", 12, 3);
    mc_screen *s = f.s;
    mc_style st = mc_style_default();
    CHECK(mc_screen_show(s) == 0 && output(f));
    CHECK(mc_screen_put_str_styled(s, 0, 0, "x", mc_style_foreground(st, 2)) == 1);
    CHECK(mc_screen_put_str_styled(s, 1, 0, "y", mc_style_foreground(st, 200)) == 1);
    CHECK(mc_screen_show(s) == 0);
    CHECK(strcmp(output(f), BEGIN "\033[1;1H\033[m\033[32mx\033[my" END) == 0);
    close_fixture(f);
}

static void test_engage(void) {
    fixture f = open_fixture("xterm-256color", 12, 3);
    outbuf o = {0};
    engage(f.s, &o);
    disengage(f.s, &o);
    CHECK(out_flush(&o, f.s->fd) == 0);
    CHECK(strcmp(output(f), "\033[?1049h\033[22;2t\033[?1h\033=\033[?25l\033[?7l"
                            "\033[H\033[J"
                            "\033[?25h\033[?1l\033>\033[m\033[?7h\033[23;2t\033[H\033[J\033[?1049l") ==
          0);
    close_fixture(f);

    f = open_fixture("linux", 12, 3); /* legacy: no title save and restore */
    engage(f.s, &o);
    disengage(f.s, &o);
    CHECK(out_flush(&o, f.s->fd) == 0);
    CHECK(!strstr(output(f), "\033[22;2t"));
    close_fixture(f);
}

static void test_cursor(void) {
    fixture f = open_fixture("xterm-256color", 12, 3);
    mc_screen *s = f.s;
    CHECK(mc_screen_show(s) == 0 && output(f));
    mc_screen_show_cursor(s, 4, 2);
    CHECK(mc_screen_show(s) == 0);
    CHECK(strcmp(output(f), BEGIN "\033[3;5H\033[?25h\033[?2026l") == 0);
    CHECK(mc_screen_show(s) == 0 && !*output(f));
    mc_screen_hide_cursor(s);
    CHECK(mc_screen_show(s) == 0 && strcmp(output(f), BEGIN END) == 0);
    close_fixture(f);
}

static void test_write_failure(void) {
    fixture f = open_fixture("xterm-256color", 12, 3);
    mc_screen *s = f.s;
    CHECK(mc_screen_show(s) == 0);
    signal(SIGPIPE, SIG_IGN);
    close(f.out);
    f.out = open("/dev/null", O_RDONLY);
    CHECK(mc_screen_put_str(s, 0, 0, "z") == 1);
    CHECK(mc_screen_show(s) == -1 && errno == EPIPE && s->redraw);
    close_fixture(f);
}

int main(void) {
    test_colors();
    test_diff();
    test_clipping();
    test_put();
    test_rejected_input();
    test_styles();
    test_limited_palette();
    test_engage();
    test_cursor();
    test_write_failure();
    puts("screen_test: ok");
    return 0;
}
