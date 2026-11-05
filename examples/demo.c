/* SPDX-FileCopyrightText: 2026 azureblue
 * SPDX-License-Identifier: Apache-2.0 */
/* minicell demo: draws a frame, some text and symbols and the palette, and shows
   the last key pressed. Exit with q, Escape or Ctrl-C. */
#define _POSIX_C_SOURCE 200809L
#include "minicell.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t quit;

static void on_signal(int sig) {
    (void)sig;
    quit = 1;
}

static void draw_box(mc_screen *s, int x1, int y1, int x2, int y2, mc_style style) {
    for (int x = x1 + 1; x < x2; x++) {
        mc_screen_set_content(s, x, y1, MC_RuneHLine, style);
        mc_screen_set_content(s, x, y2, MC_RuneHLine, style);
    }
    for (int y = y1 + 1; y < y2; y++) {
        mc_screen_set_content(s, x1, y, MC_RuneVLine, style);
        mc_screen_set_content(s, x2, y, MC_RuneVLine, style);
    }
    mc_screen_set_content(s, x1, y1, MC_RuneULCorner, style);
    mc_screen_set_content(s, x2, y1, MC_RuneURCorner, style);
    mc_screen_set_content(s, x1, y2, MC_RuneLLCorner, style);
    mc_screen_set_content(s, x2, y2, MC_RuneLRCorner, style);
}

static void draw(mc_screen *s, const char *status) {
    int width, height;
    mc_screen_size(s, &width, &height);
    mc_screen_clear(s);
    if (width < 30 || height < 8) {
        mc_screen_put_str(s, 0, 0, "Too small. q: quit");
        return;
    }

    int colors = mc_screen_colors(s);
    mc_style title = mc_style_attributes(
        mc_style_foreground(mc_style_default(), colors > 81 ? 81 : 6), MC_AttrBold);

    draw_box(s, 0, 0, width - 1, height - 1, title);
    mc_screen_put_str_styled(s, 2, 1, "minicell demo", title);
    mc_screen_put_str(s, 2, 2, "Latin: ą ć ę ł ń ó ś ź ż");

    static const uint32_t runes[] = {MC_RuneDiamond, MC_RuneBullet, MC_RunePlMinus,
                                     MC_RuneLEqual,  MC_RuneGEqual, MC_RunePi,
                                     MC_RuneBoard,   MC_RuneCkBoard, MC_RuneBlock};
    for (size_t i = 0; i < sizeof(runes) / sizeof(runes[0]); i++)
        mc_screen_set_content(s, 2 + 2 * (int)i, 3, runes[i], mc_style_default());

    for (int c = 0; c < colors && 2 + c < width - 1; c++)
        mc_screen_set_content(s, 2 + c, 5, ' ', mc_style_background(mc_style_default(), c));

    char info[64];
    const char *term = *mc_screen_term(s) ? mc_screen_term(s) : "no TERM";
    snprintf(info, sizeof(info), "%s, %d colors", term, colors);
    mc_screen_put_str(s, 2, height - 3, info);
    mc_screen_put_str(s, 2, height - 2, status);
}

int main(int argc, char **argv) {
    const char *term = NULL;
    if (argc == 3 && strcmp(argv[1], "--term") == 0) {
        term = argv[2];
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--term NAME]\n", argv[0]);
        return 2;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    mc_screen *s = mc_screen_init(term);
    if (!s) {
        perror("mc_screen_init");
        return 1;
    }

    char status[80] = "Press a key. q / Esc: quit";
    int rc = 0;
    draw(s, status);
    while (!quit) {
        if (mc_screen_show(s) < 0) {
            rc = 1;
            break;
        }
        mc_event ev;
        int n = mc_screen_poll_event(s, &ev, -1);
        if (n < 0) {
            rc = 1;
            break;
        }
        if (n == 0)
            continue;
        if (ev.type == MC_EventKey) {
            if (ev.key == MC_KeyEsc || ev.key == MC_KeyCtrlC ||
                (ev.key == MC_KeyRune && ev.rune == 'q'))
                break;
            snprintf(status, sizeof(status), "key=%d rune=U+%04X mods=%d", ev.key,
                     (unsigned)ev.rune, ev.mods);
        }
        draw(s, status);
    }
    mc_screen_fini(s);
    if (rc)
        perror("minicell");
    return rc;
}
