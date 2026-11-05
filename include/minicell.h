/* SPDX-FileCopyrightText: 2026 azureblue
 * SPDX-License-Identifier: Apache-2.0 */
/* minicell: a small terminal cell library for C and C++, modeled on tcell.
 *
 * All functions are single-threaded. The library opens /dev/tty itself,
 * installs no signal handlers and does not call setlocale(). The terminal
 * must use UTF-8. Functions returning int return -1 and set errno on error.
 */
#ifndef MINICELL_H
#define MINICELL_H

#include "minicell_tcell.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Styles ---------------------------------------------------------------- */

/* A palette index 0-255 or MC_ColorDefault. Indices the terminal does not
   have (see mc_screen_colors) are drawn in the default color. */
typedef int mc_color;
enum { MC_ColorDefault = -1 };

/* MC_Attr* from minicell_tcell.h. tcell keeps underline in a separate
   UnderlineStyle; minicell supports only the plain underline, as this bit. */
typedef unsigned mc_attrmask;
enum { MC_AttrUnderline = 1 << 6 };

typedef struct {
    mc_color fg, bg;
    mc_attrmask attrs;
} mc_style;

static inline mc_style mc_style_default(void) {
    mc_style s = {MC_ColorDefault, MC_ColorDefault, MC_AttrNone};
    return s;
}

static inline mc_style mc_style_foreground(mc_style s, mc_color c) {
    s.fg = c;
    return s;
}

static inline mc_style mc_style_background(mc_style s, mc_color c) {
    s.bg = c;
    return s;
}

/* Replaces the attribute mask, like tcell's Style.Attributes. */
static inline mc_style mc_style_attributes(mc_style s, mc_attrmask attrs) {
    s.attrs = attrs;
    return s;
}

/* --- Events ---------------------------------------------------------------- */

enum { MC_EventNone, MC_EventKey, MC_EventResize };

typedef struct {
    int type;      /* MC_Event* */
    int key;       /* MC_Key*; MC_KeyRune for printable characters */
    uint32_t rune; /* the character, when key is MC_KeyRune */
    int mods;      /* MC_Mod* bits */
    int width;     /* new size, for MC_EventResize */
    int height;
} mc_event;

/* --- Screen ---------------------------------------------------------------- */

typedef struct mc_screen mc_screen;

/* Opens /dev/tty and enters full-screen mode. As in tcell v3, every terminal
   is assumed to understand xterm (ECMA-48) sequences. term, or $TERM if
   term is NULL, only selects the number of colors and whether the terminal
   is a "legacy" one (linux, vt*, *ansi*, sun) without xterm extensions. */
mc_screen *mc_screen_init(const char *term);

/* Restores the terminal and frees the screen. Always call it before exit. */
void mc_screen_fini(mc_screen *s);

/* The terminal name in use, "" if none was given. */
const char *mc_screen_term(const mc_screen *s);

/* Palette size: 256, 88, 16, 8 or 0 (monochrome, or NO_COLOR is set),
   derived from the terminal name and $COLORTERM as tcell v3 does. */
int mc_screen_colors(const mc_screen *s);
void mc_screen_size(const mc_screen *s, int *width, int *height);

/* Style used by mc_screen_clear() and mc_screen_put_str(). */
int mc_screen_set_style(mc_screen *s, mc_style style);

/* Fills the screen with spaces in the current style. */
void mc_screen_clear(mc_screen *s);

/* Drawing. Changes go to an off-screen buffer until mc_screen_show().
 *
 * Text is UTF-8 and every character occupies exactly one cell. Wide
 * characters (CJK, emoji) and combining marks are not supported yet.
 * Anything outside the screen is silently clipped. Control characters are
 * rejected with EINVAL. */

/* Sets the cell at x, y to the character ch. */
int mc_screen_set_content(mc_screen *s, int x, int y, uint32_t ch, mc_style style);

/* Draws the first character of str. Returns the rest of str, or NULL on error. */
const char *mc_screen_put(mc_screen *s, int x, int y, const char *str, mc_style style);

/* Draws str on one line. Returns the number of characters, or -1 on error. */
int mc_screen_put_str(mc_screen *s, int x, int y, const char *str);
int mc_screen_put_str_styled(mc_screen *s, int x, int y, const char *str, mc_style style);

/* Places the visible cursor at x, y; it is hidden by default. */
void mc_screen_show_cursor(mc_screen *s, int x, int y);
void mc_screen_hide_cursor(mc_screen *s);

/* Writes the cells that changed since the last call to the terminal. */
int mc_screen_show(mc_screen *s);

/* Clears the terminal and redraws every cell, e.g. after outside garbage. */
int mc_screen_sync(mc_screen *s);

/* Waits up to timeout_ms (-1: forever) for an event. Returns 1 with *ev
   filled in, 0 on timeout or if interrupted by a signal, -1 on error.
   On resize the screen is cleared; redraw everything and call show. */
int mc_screen_poll_event(mc_screen *s, mc_event *ev, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
