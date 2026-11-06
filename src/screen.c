/* Terminal setup, cell buffers, rendering and the event loop.
 * Copyright 2026 azureblue; parts derived from tcell, Copyright 2026
 * The TCell Authors. Apache License 2.0.
 *
 * The palette rules, engage, draw and disengage are modified C ports of
 * tcell v3.6.0 tscreen.go; terminal queries, mouse, paste and true color
 * were left out. */
#define _POSIX_C_SOURCE 200809L
#include "minicell.h"

#include "input.h"
#include "utf8.h"
#include "vt.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum {
    RESIZE_CHECK_MS = 100, /* poll the window size at least this often */
    UTF8_TIMEOUT_MS = 50,  /* drop a UTF-8 character still incomplete after this */
};

typedef struct {
    uint32_t ch;
    mc_style style;
} cell;

struct mc_screen {
    int fd;
    struct termios saved_termios;
    char *term;
    int colors;
    bool legacy; /* no xterm extensions such as title save/restore */

    int width, height;
    cell *back;  /* what the application has drawn */
    cell *front; /* what the terminal shows, valid unless redraw is set */
    bool redraw; /* terminal state unknown: clear and repaint everything */

    mc_style style;
    int cursor_x, cursor_y; /* requested cursor; -1 when hidden */
    int shown_x, shown_y;   /* cursor as last drawn */

    mc_input parser;
    char input[1024]; /* bytes read but not yet decoded */
    size_t input_len;
    int64_t input_time; /* when the last input byte arrived */
};

/* --- Output buffer ------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len, cap;
    bool failed; /* sticky allocation failure */
} outbuf;

static void out_append(outbuf *o, const char *p, size_t n) {
    if (o->failed)
        return;
    if (n > o->cap - o->len) {
        size_t cap = o->cap ? o->cap : 4096;
        while (cap - o->len < n)
            cap *= 2;
        char *data = realloc(o->data, cap);
        if (!data) {
            o->failed = true;
            return;
        }
        o->data = data;
        o->cap = cap;
    }
    memcpy(o->data + o->len, p, n);
    o->len += n;
}

static void out_puts(outbuf *o, const char *s) {
    out_append(o, s, strlen(s));
}

static void out_printf(outbuf *o, const char *format, int a, int b) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), format, a, b);
    out_append(o, buf, (size_t)n);
}

static void out_rune(outbuf *o, uint32_t ch) {
    char buf[4];
    out_append(o, buf, mc_utf8_encode(ch, buf));
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) {
            if (k == 0)
                errno = EIO;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static int out_flush(outbuf *o, int fd) {
    int rc = 0;
    if (o->failed) {
        errno = ENOMEM;
        rc = -1;
    } else if (o->len) {
        rc = write_all(fd, o->data, o->len);
    }
    free(o->data);
    *o = (outbuf){0};
    return rc;
}

/* --- Cells -------------------------------------------------------------- */

static bool style_equal(mc_style a, mc_style b) {
    return a.fg == b.fg && a.bg == b.bg && a.attrs == b.attrs;
}

static bool cell_equal(cell a, cell b) {
    return a.ch == b.ch && style_equal(a.style, b.style);
}

static size_t cell_count(const mc_screen *s) {
    return (size_t)s->width * (size_t)s->height;
}

static void fill(cell *cells, size_t n, mc_style style) {
    for (size_t i = 0; i < n; i++)
        cells[i] = (cell){' ', style};
}

/* --- Terminal setup ------------------------------------------------------- */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool has_prefix(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && strcmp(s + n - k, suffix) == 0;
}

/* The palette size rules of tcell v3 tScreen.Init. True color terminals get
   their 256-color palette, as minicell has no RGB colors yet. */
static int detect_colors(const char *term) {
    const char *colorterm = getenv("COLORTERM");
    const char *no_color = getenv("NO_COLOR");
    if (!colorterm)
        colorterm = "";
    if (no_color && *no_color)
        return 0;
    if (!strcmp(colorterm, "truecolor") || !strcmp(colorterm, "direct") ||
        !strcmp(colorterm, "24bit") || has_suffix(term, "-direct") ||
        has_suffix(term, "-truecolor"))
        return 256;
    if (has_suffix(term, "-256color") || strstr(colorterm, "256"))
        return 256;
    if (has_suffix(term, "-88color"))
        return 88;
    if (has_suffix(term, "-16color"))
        return 16;
    if (strstr(term, "color") || *colorterm)
        return 8;
    if (strstr(term, "mono") || has_suffix(term, "-m"))
        return 0;
    if (strstr(term, "ansi") || !strcmp(term, "dtterm") || !strcmp(term, "xterm") ||
        !strcmp(term, "aixterm") || !strcmp(term, "linux"))
        return 8;
    if (has_prefix(term, "vt") || !strcmp(term, "sun"))
        return 0;
    return 256; /* any other terminal */
}

static bool is_legacy(const char *term) {
    return has_prefix(term, "vt") || strstr(term, "ansi") || !strcmp(term, "linux") ||
           !strcmp(term, "sun") || !strcmp(term, "sun-color");
}

/* Returns 1 if the size changed (the screen is then blank), 0 if not. */
static int update_size(mc_screen *s) {
    struct winsize ws;
    if (ioctl(s->fd, TIOCGWINSZ, &ws) < 0)
        return -1;
    if (!ws.ws_col || !ws.ws_row) {
        errno = EINVAL;
        return -1;
    }
    if (ws.ws_col == s->width && ws.ws_row == s->height)
        return 0;
    size_t n = (size_t)ws.ws_col * ws.ws_row;
    cell *back = malloc(n * sizeof(cell));
    cell *front = malloc(n * sizeof(cell));
    if (!back || !front) {
        free(back);
        free(front);
        return -1;
    }
    fill(back, n, s->style);
    free(s->back);
    free(s->front);
    s->back = back;
    s->front = front;
    s->width = ws.ws_col;
    s->height = ws.ws_row;
    s->redraw = true;
    return 1;
}

static int enter_raw_mode(mc_screen *s) {
    struct termios raw = s->saved_termios;
    raw.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(s->fd, TCSANOW, &raw);
}

static void destroy(mc_screen *s) {
    if (s->fd >= 0)
        close(s->fd);
    free(s->term);
    free(s->back);
    free(s->front);
    free(s);
}

/* Without the startup queries of tcell's engage, which minicell skips. */
static void engage(mc_screen *s, outbuf *o) {
    out_puts(o, VT_ENTER_CA);
    if (!s->legacy)
        out_puts(o, VT_SAVE_TITLE);
    out_puts(o, VT_ENTER_KEYPAD);
    out_puts(o, VT_ENABLE_ALT_CHARS);
    out_puts(o, VT_SHOW_CURSOR_OFF);
    out_puts(o, VT_AUTO_MARGIN_OFF);
    out_puts(o, VT_CLEAR);
}

static void disengage(mc_screen *s, outbuf *o) {
    out_puts(o, VT_SHOW_CURSOR_ON);
    out_puts(o, VT_EXIT_KEYPAD);
    out_puts(o, VT_SGR0);
    out_puts(o, VT_AUTO_MARGIN_ON);
    if (!s->legacy)
        out_puts(o, VT_RESTORE_TITLE);
    out_puts(o, VT_CLEAR);
    out_puts(o, VT_EXIT_CA);
}

mc_screen *mc_screen_init(const char *term) {
    if (!term)
        term = getenv("TERM");
    mc_screen *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->fd = -1;
    s->term = malloc(strlen(term ? term : "") + 1);
    if (!s->term) {
        destroy(s);
        return NULL;
    }
    strcpy(s->term, term ? term : "");
    s->colors = detect_colors(s->term);
    s->legacy = is_legacy(s->term);
    s->style = mc_style_default();
    s->cursor_x = s->cursor_y = -1;
    s->shown_x = s->shown_y = -1;
    mc_input_init(&s->parser);

    s->fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (s->fd < 0 || tcgetattr(s->fd, &s->saved_termios) < 0 || update_size(s) < 0 ||
        enter_raw_mode(s) < 0) {
        int saved_errno = errno;
        destroy(s);
        errno = saved_errno;
        return NULL;
    }

    outbuf o = {0};
    engage(s, &o);
    if (out_flush(&o, s->fd) < 0) {
        int saved_errno = errno;
        mc_screen_fini(s);
        errno = saved_errno;
        return NULL;
    }
    return s;
}

void mc_screen_fini(mc_screen *s) {
    if (!s)
        return;
    int saved_errno = errno;
    outbuf o = {0};
    disengage(s, &o);
    out_flush(&o, s->fd);
    while (tcsetattr(s->fd, TCSANOW, &s->saved_termios) < 0 && errno == EINTR) {
    }
    destroy(s);
    errno = saved_errno;
}

const char *mc_screen_term(const mc_screen *s) {
    return s->term;
}

int mc_screen_colors(const mc_screen *s) {
    return s->colors;
}

void mc_screen_size(const mc_screen *s, int *width, int *height) {
    if (width)
        *width = s->width;
    if (height)
        *height = s->height;
}

/* --- Drawing -------------------------------------------------------------- */

static int check_style(mc_style style) {
    const mc_attrmask all = MC_AttrBold | MC_AttrBlink | MC_AttrReverse | MC_AttrDim |
                            MC_AttrItalic | MC_AttrStrikeThrough | MC_AttrUnderline;
    if (style.fg < MC_ColorDefault || style.fg > 255 || style.bg < MC_ColorDefault ||
        style.bg > 255 || (style.attrs & ~all)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int mc_screen_set_style(mc_screen *s, mc_style style) {
    if (check_style(style) < 0)
        return -1;
    s->style = style;
    return 0;
}

void mc_screen_clear(mc_screen *s) {
    fill(s->back, cell_count(s), s->style);
}

int mc_screen_set_content(mc_screen *s, int x, int y, uint32_t ch, mc_style style) {
    /* Control characters would change the terminal state; surrogates and
       values above U+10FFFF cannot be encoded. */
    bool control = ch < 0x20 || (ch >= 0x7f && ch < 0xa0);
    if (control || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff)) {
        errno = EINVAL;
        return -1;
    }
    if (check_style(style) < 0)
        return -1;
    if (x >= 0 && y >= 0 && x < s->width && y < s->height)
        s->back[(size_t)y * s->width + x] = (cell){ch, style};
    return 0;
}

const char *mc_screen_put(mc_screen *s, int x, int y, const char *str, mc_style style) {
    if (!*str)
        return str;
    uint32_t ch;
    size_t len;
    if (mc_utf8_decode(str, strlen(str), &ch, &len) != 1) {
        errno = EILSEQ;
        return NULL;
    }
    if (mc_screen_set_content(s, x, y, ch, style) < 0)
        return NULL;
    return str + len;
}

int mc_screen_put_str_styled(mc_screen *s, int x, int y, const char *str, mc_style style) {
    int count = 0;
    while (*str) {
        str = mc_screen_put(s, x + count, y, str, style);
        if (!str)
            return -1;
        count++;
    }
    return count;
}

int mc_screen_put_str(mc_screen *s, int x, int y, const char *str) {
    return mc_screen_put_str_styled(s, x, y, str, s->style);
}

void mc_screen_show_cursor(mc_screen *s, int x, int y) {
    s->cursor_x = x;
    s->cursor_y = y;
}

void mc_screen_hide_cursor(mc_screen *s) {
    mc_screen_show_cursor(s, -1, -1);
}

/* --- Output: tcell v3 tScreen.draw ---------------------------------------- */

/* sendFgBg: colors the terminal lacks are left at the default. */
static void emit_colors(const mc_screen *s, outbuf *o, mc_style style) {
    if (style.fg >= 0 && style.fg < s->colors)
        out_printf(o, style.fg < 8 ? VT_SET_FG8 : VT_SET_FG256, style.fg, 0);
    if (style.bg >= 0 && style.bg < s->colors)
        out_printf(o, style.bg < 8 ? VT_SET_BG8 : VT_SET_BG256, style.bg, 0);
}

static void emit_style(const mc_screen *s, outbuf *o, mc_style style) {
    static const struct {
        mc_attrmask attr;
        const char *seq;
    } attrs[] = {
        {MC_AttrBold, VT_BOLD},     {MC_AttrReverse, VT_REVERSE},
        {MC_AttrBlink, VT_BLINK},   {MC_AttrDim, VT_DIM},
        {MC_AttrItalic, VT_ITALIC}, {MC_AttrStrikeThrough, VT_STRIKE_THROUGH},
        {MC_AttrUnderline, VT_UNDERLINE},
    };
    out_puts(o, VT_SGR0);
    emit_colors(s, o, style);
    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++)
        if (style.attrs & attrs[i].attr)
            out_puts(o, attrs[i].seq);
}

static void move_to(outbuf *o, int x, int y) {
    out_printf(o, VT_SET_CURSOR_POSITION, y + 1, x + 1);
}

/* tcell parks the cursor past the bottom-right corner in case hiding fails. */
static void hide_cursor(const mc_screen *s, outbuf *o) {
    move_to(o, s->width, s->height);
    out_puts(o, VT_SHOW_CURSOR_OFF);
}

int mc_screen_show(mc_screen *s) {
    bool cursor = s->cursor_x >= 0 && s->cursor_y >= 0 && s->cursor_x < s->width &&
                  s->cursor_y < s->height;
    int cx = cursor ? s->cursor_x : -1, cy = cursor ? s->cursor_y : -1;
    bool changed = s->redraw || cx != s->shown_x || cy != s->shown_y;
    for (size_t i = 0; !changed && i < cell_count(s); i++)
        changed = !cell_equal(s->back[i], s->front[i]);
    if (!changed)
        return 0;

    outbuf o = {0};
    out_puts(&o, VT_SYNC_OUTPUT_ON);
    hide_cursor(s, &o);
    if (s->redraw) {
        out_puts(&o, VT_SGR0);
        emit_colors(s, &o, s->style);
        out_puts(&o, VT_CLEAR);
    }

    /* front is updated while emitting; if the write fails, redraw is set
       and the partially updated front buffer is never trusted. */
    bool pen_known = false;
    mc_style pen = mc_style_default();
    int px = -1, py = -1; /* terminal cursor position after the last write */
    for (int y = 0; y < s->height; y++) {
        for (int x = 0; x < s->width; x++) {
            size_t i = (size_t)y * s->width + x;
            cell c = s->back[i];
            if (!s->redraw && cell_equal(c, s->front[i]))
                continue;
            if (x != px || y != py)
                move_to(&o, x, y);
            if (!pen_known || !style_equal(pen, c.style)) {
                emit_style(s, &o, c.style);
                pen = c.style;
                pen_known = true;
            }
            out_rune(&o, c.ch);
            s->front[i] = c;
            px = x + 1;
            py = y;
        }
    }

    if (cursor) {
        move_to(&o, cx, cy);
        out_puts(&o, VT_SHOW_CURSOR_ON);
    } else {
        hide_cursor(s, &o);
    }
    out_puts(&o, VT_SYNC_OUTPUT_OFF);

    if (out_flush(&o, s->fd) < 0) {
        s->redraw = true;
        return -1;
    }
    s->redraw = false;
    s->shown_x = cx;
    s->shown_y = cy;
    return 0;
}

int mc_screen_sync(mc_screen *s) {
    s->redraw = true;
    return mc_screen_show(s);
}

/* --- Input ---------------------------------------------------------------- */

static int read_input(mc_screen *s) {
    if (s->input_len == sizeof(s->input))
        return 0; /* full; decoding will make room */
    ssize_t k = read(s->fd, s->input + s->input_len, sizeof(s->input) - s->input_len);
    if (k < 0)
        return errno == EINTR ? 0 : -1;
    if (k == 0) {
        errno = EIO; /* terminal hung up */
        return -1;
    }
    s->input_len += (size_t)k;
    s->input_time = now_ms();
    return 0;
}

static void consume_input(mc_screen *s, size_t n) {
    s->input_len -= n;
    memmove(s->input, s->input + n, s->input_len);
}

/* Feeds buffered input to the parser until it produces an event. A UTF-8
   sequence that stays incomplete for as long as an escape is dropped. */
static bool decode_input(mc_screen *s, int64_t now, mc_event *ev) {
    while (s->input_len) {
        uint32_t r;
        size_t len;
        int rc = mc_utf8_decode(s->input, s->input_len, &r, &len);
        if (rc == 0 && now - s->input_time < UTF8_TIMEOUT_MS)
            return false; /* wait for the rest of the character */
        if (rc != 1) {
            consume_input(s, 1); /* invalid UTF-8 is dropped, as in tcell */
            continue;
        }
        consume_input(s, len);
        if (mc_input_feed(&s->parser, r, ev))
            return true;
    }
    return false;
}

int mc_screen_poll_event(mc_screen *s, mc_event *ev, int timeout_ms) {
    int64_t deadline = timeout_ms < 0 ? INT64_MAX : now_ms() + timeout_ms;
    for (;;) {
        int resized = update_size(s);
        if (resized < 0)
            return -1;
        if (resized) {
            *ev = (mc_event){.type = MC_EventResize, .width = s->width, .height = s->height};
            return 1;
        }

        int64_t now = now_ms();
        if (decode_input(s, now, ev))
            return 1;
        /* Waiting for the rest of a UTF-8 character or of an escape sequence. */
        int pending = s->input_len ? UTF8_TIMEOUT_MS : mc_input_wait_ms(&s->parser);
        if (!s->input_len && pending && now - s->input_time >= pending) {
            if (mc_input_expire(&s->parser, ev))
                return 1;
            continue;
        }

        int64_t wait = RESIZE_CHECK_MS;
        if (pending && s->input_time + pending - now < wait)
            wait = s->input_time + pending - now;
        if (deadline - now < wait)
            wait = deadline - now;
        struct pollfd pfd = {s->fd, POLLIN, 0};
        int ready = poll(&pfd, 1, wait < 0 ? 0 : (int)wait);
        if (ready < 0)
            return errno == EINTR ? 0 : -1;
        if (ready && (pfd.revents & (POLLERR | POLLNVAL))) {
            errno = EIO;
            return -1;
        }
        if (ready) {
            if (read_input(s) < 0)
                return -1;
        } else if (now_ms() >= deadline) {
            return 0;
        }
    }
}
