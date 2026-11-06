/* Tetris with the rules of NES Tetris (1989), drawn with minicell.
 *
 * NES mechanics: 60.0988 frames per second, the NES gravity table, rotation
 * without wall kicks, no lock delay, no hard drop, the NES randomizer (one
 * reroll on a repeat), entry delay of 10-18 frames by lock height, a center-out
 * line clear animation, 40/100/300/1200 x (level + 1) points, a point per
 * soft-dropped row, the NES rule for the first level-up and the NES level
 * palettes, where pieces of one color group differ slightly in shade.
 *
 * Terminals report key presses but not releases, so the NES delayed
 * auto-shift is replaced by the terminal's key repeat, and the NES rule that
 * Down must be released between pieces is approximated by a 100 ms gap.
 *
 * Keys: Left/Right move, Down soft drop, Up or X rotate clockwise, Z rotate
 * counterclockwise, P pause, Enter start, Q or Escape quit.
 */
#define _POSIX_C_SOURCE 200809L
#include "minicell.h"

#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    BOARD_W = 10,
    BOARD_H = 20,
    SPAWN_X = 5,
    CLEAR_FRAMES = 20, /* five animation steps of four frames */
    LAYOUT_W = 58,     /* statistics, board and side panel */
    LAYOUT_H = 22,
};

static const int64_t FRAME_NS = 16639267;          /* NTSC NES: 60.0988 Hz */
static const int64_t DOWN_RELEASE_NS = 100000000; /* no Down for 100 ms: released */

/* --- Pieces --------------------------------------------------------------- */

/* The NES spawn table order. */
enum { PIECE_T, PIECE_J, PIECE_Z, PIECE_O, PIECE_S, PIECE_L, PIECE_I, PIECE_COUNT };

typedef struct {
    int x, y;
} point;

typedef struct {
    char name;
    int group;        /* NES color group: 0 T/O/I, 1 J/S, 2 L/Z */
    int basic_color;  /* for terminals with fewer than 256 colors */
    int orientations; /* 4 rotate about the center; 2 toggle; 1 never turns */
    point shape[2][4];
} piece_def;

/* Blocks relative to the pivot, y down, in the NES spawn orientation. */
static const piece_def pieces[PIECE_COUNT] = {
    [PIECE_T] = {'T', 0, 5, 4, {{{-1, 0}, {0, 0}, {1, 0}, {0, 1}}}},
    [PIECE_J] = {'J', 1, 4, 4, {{{-1, 0}, {0, 0}, {1, 0}, {1, 1}}}},
    [PIECE_Z] = {'Z', 2, 1, 2, {{{-1, 0}, {0, 0}, {0, 1}, {1, 1}}, {{1, -1}, {1, 0}, {0, 0}, {0, 1}}}},
    [PIECE_O] = {'O', 0, 3, 1, {{{-1, 0}, {0, 0}, {-1, 1}, {0, 1}}}},
    [PIECE_S] = {'S', 1, 2, 2, {{{0, 0}, {1, 0}, {-1, 1}, {0, 1}}, {{0, -1}, {0, 0}, {1, 0}, {1, 1}}}},
    [PIECE_L] = {'L', 2, 7, 4, {{{-1, 0}, {0, 0}, {1, 0}, {-1, 1}}}},
    [PIECE_I] = {'I', 0, 6, 2, {{{-2, 0}, {-1, 0}, {0, 0}, {1, 0}}, {{0, -2}, {0, -1}, {0, 0}, {0, 1}}}},
};

/* Block i of piece p in orientation r (0-3, clockwise from spawn). */
static point block(int p, int r, int i) {
    const piece_def *d = &pieces[p];
    if (d->orientations == 2)
        return d->shape[r % 2][i];
    point b = d->shape[0][i];
    for (int k = 0; k < r % d->orientations; k++)
        b = (point){-b.y, b.x}; /* quarter turn clockwise, y pointing down */
    return b;
}

/* --- NES rules ------------------------------------------------------------- */

static int gravity_frames(int level) {
    static const int frames[] = {48, 43, 38, 33, 28, 23, 18, 13, 8, 6, 5, 5, 5, 4, 4,
                                 4,  3,  3,  3,  2,  2,  2,  2,  2, 2, 2, 2, 2, 2};
    return level < (int)(sizeof(frames) / sizeof(frames[0])) ? frames[level] : 1;
}

/* Lines needed for the first level-up; later levels come every 10 lines. */
static int first_level_up(int start_level) {
    int a = start_level * 10 + 10, b = start_level * 10 - 50;
    if (b < 100)
        b = 100;
    return a < b ? a : b;
}

/* Entry delay: 10 frames when locked in the bottom two rows, +2 per 4 rows. */
static int entry_delay(int lowest_row) {
    int height = BOARD_H - 1 - lowest_row;
    int frames = 10 + 2 * ((height + 2) / 4);
    return frames > 18 ? 18 : frames;
}

/* --- Game state ------------------------------------------------------------ */

enum { MODE_MENU, MODE_PLAYING, MODE_PAUSED, MODE_OVER };
enum { ACT_LEFT, ACT_RIGHT, ACT_DOWN, ACT_CW, ACT_CCW };

typedef struct {
    int mode;
    uint8_t board[BOARD_H][BOARD_W]; /* 0 empty, otherwise piece + 1 */
    int start_level, level, lines, score;
    int stats[PIECE_COUNT];
    int tetrises;     /* four lines cleared at once */
    int tetris_flash; /* frames left to show "TETRIS!" */
    uint32_t rng;

    bool active;
    int piece, rot, x, y, next;
    int gravity;        /* frames since the piece last fell */
    int soft_rows;      /* consecutive soft-dropped rows, scored at lock */
    int delay;          /* entry delay frames left */
    int clearing;       /* line clear animation frames left */
    bool full[BOARD_H]; /* rows being cleared */

    int actions[32]; /* key presses since the last frame */
    int nactions;
    int64_t last_down;
    bool down_blocked;
} game;

static uint32_t next_random(game *g) {
    g->rng ^= g->rng << 13; /* xorshift32 */
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return g->rng;
}

/* NES: a random index 0-7; on 7 or a repeat, one reroll folded into 0-6. */
static int random_piece(game *g, int previous) {
    int r = (int)(next_random(g) % 8);
    if (r == 7 || r == previous)
        r = (int)((next_random(g) % 8 + (previous < 0 ? 0 : previous)) % 7);
    return r;
}

static bool fits(const game *g, int p, int r, int x, int y) {
    for (int i = 0; i < 4; i++) {
        point b = block(p, r, i);
        int bx = x + b.x, by = y + b.y;
        if (bx < 0 || bx >= BOARD_W || by >= BOARD_H)
            return false;
        if (by >= 0 && g->board[by][bx])
            return false;
    }
    return true;
}

static void new_game(game *g, int64_t now) {
    int start_level = g->start_level;
    uint32_t rng = g->rng;
    memset(g, 0, sizeof(*g));
    g->mode = MODE_PLAYING;
    g->start_level = g->level = start_level;
    g->rng = rng;
    g->next = random_piece(g, -1);
    g->last_down = now - DOWN_RELEASE_NS;
}

static void spawn(game *g, int64_t now) {
    g->piece = g->next;
    g->next = random_piece(g, g->piece);
    g->rot = 0;
    g->x = SPAWN_X;
    g->y = 0;
    g->gravity = 0;
    g->soft_rows = 0;
    g->stats[g->piece]++;
    g->active = true;
    g->down_blocked = now - g->last_down < DOWN_RELEASE_NS;
    if (!fits(g, g->piece, g->rot, g->x, g->y))
        g->mode = MODE_OVER;
}

static void lock_piece(game *g) {
    int lowest = 0;
    for (int i = 0; i < 4; i++) {
        point b = block(g->piece, g->rot, i);
        int bx = g->x + b.x, by = g->y + b.y;
        if (by < 0) {
            g->mode = MODE_OVER; /* locked above the visible field */
            return;
        }
        g->board[by][bx] = (uint8_t)(g->piece + 1);
        if (by > lowest)
            lowest = by;
    }
    g->score += g->soft_rows;
    g->active = false;
    g->delay = entry_delay(lowest);

    bool any = false;
    for (int y = 0; y < BOARD_H; y++) {
        g->full[y] = true;
        for (int x = 0; x < BOARD_W && g->full[y]; x++)
            g->full[y] = g->board[y][x] != 0;
        any |= g->full[y];
    }
    if (any)
        g->clearing = CLEAR_FRAMES;
}

static void collapse_rows(game *g) {
    static const int points[] = {0, 40, 100, 300, 1200};
    int cleared = 0;
    for (int y = BOARD_H - 1; y >= 0; y--) {
        if (!g->full[y])
            continue;
        memmove(g->board[1], g->board[0], (size_t)y * BOARD_W);
        memset(g->board[0], 0, BOARD_W);
        memmove(g->full + 1, g->full, (size_t)y * sizeof(bool));
        g->full[0] = false;
        cleared++;
        y++; /* the row above moved into this one */
    }
    g->score += points[cleared] * (g->level + 1);
    g->lines += cleared;
    if (cleared == 4) {
        g->tetrises++;
        g->tetris_flash = 120; /* two seconds */
    }
    int first = first_level_up(g->start_level);
    if (g->lines >= first) {
        int level = g->start_level + 1 + (g->lines - first) / 10;
        if (level > g->level)
            g->level = level;
    }
}

static void try_move(game *g, int dx, int dr) {
    int r = (g->rot + dr) % 4;
    if (fits(g, g->piece, r, g->x + dx, g->y)) {
        g->x += dx;
        g->rot = r;
    }
}

/* Moves the piece one row down, or locks it. */
static void drop(game *g) {
    if (fits(g, g->piece, g->rot, g->x, g->y + 1))
        g->y++;
    else
        lock_piece(g);
}

static void frame(game *g, int64_t now) {
    int nactions = g->nactions;
    g->nactions = 0;
    if (g->mode != MODE_PLAYING)
        return;
    if (g->down_blocked && now - g->last_down >= DOWN_RELEASE_NS)
        g->down_blocked = false;
    if (g->tetris_flash)
        g->tetris_flash--;

    if (g->clearing) {
        if (--g->clearing == 0)
            collapse_rows(g);
        return;
    }
    if (!g->active) {
        if (g->delay > 0)
            g->delay--;
        else
            spawn(g, now);
        return;
    }

    for (int i = 0; i < nactions && g->active; i++) {
        switch (g->actions[i]) {
        case ACT_LEFT:
            try_move(g, -1, 0);
            break;
        case ACT_RIGHT:
            try_move(g, 1, 0);
            break;
        case ACT_CW:
            try_move(g, 0, 1);
            break;
        case ACT_CCW:
            try_move(g, 0, 3);
            break;
        case ACT_DOWN:
            if (g->down_blocked)
                break;
            g->gravity = 0;
            if (fits(g, g->piece, g->rot, g->x, g->y + 1)) {
                g->y++;
                g->soft_rows++;
            } else {
                lock_piece(g);
            }
            break;
        }
    }
    if (g->active && ++g->gravity >= gravity_frames(g->level)) {
        g->gravity = 0;
        g->soft_rows = 0; /* gravity ends a soft drop */
        drop(g);
    }
}

/* --- Input ----------------------------------------------------------------- */

static void queue(game *g, int action) {
    if (g->nactions < (int)(sizeof(g->actions) / sizeof(g->actions[0])))
        g->actions[g->nactions++] = action;
}

/* Returns false to quit. */
static bool handle_key(game *g, const mc_event *ev, int64_t now) {
    int key = ev->key;
    uint32_t ch = key == MC_KeyRune ? ev->rune : 0;
    if (ch >= 'A' && ch <= 'Z')
        ch += 'a' - 'A';
    if (key == MC_KeyEsc || key == MC_KeyCtrlC || ch == 'q')
        return false;

    switch (g->mode) {
    case MODE_MENU:
        if ((key == MC_KeyLeft || key == MC_KeyDown) && g->start_level > 0)
            g->start_level--;
        else if ((key == MC_KeyRight || key == MC_KeyUp) && g->start_level < 19)
            g->start_level++;
        else if (key == MC_KeyEnter)
            new_game(g, now);
        break;
    case MODE_OVER:
        if (key == MC_KeyEnter)
            g->mode = MODE_MENU;
        break;
    case MODE_PAUSED:
        if (ch == 'p' || key == MC_KeyEnter)
            g->mode = MODE_PLAYING;
        break;
    case MODE_PLAYING:
        if (ch == 'p' || key == MC_KeyEnter)
            g->mode = MODE_PAUSED;
        else if (key == MC_KeyLeft || ch == 'h')
            queue(g, ACT_LEFT);
        else if (key == MC_KeyRight || ch == 'l')
            queue(g, ACT_RIGHT);
        else if (key == MC_KeyUp || ch == 'x' || ch == 'k')
            queue(g, ACT_CW);
        else if (ch == 'z')
            queue(g, ACT_CCW);
        else if (key == MC_KeyDown || ch == 'j') {
            g->last_down = now;
            queue(g, ACT_DOWN);
        }
        break;
    }
    return true;
}

/* --- Drawing --------------------------------------------------------------- */

typedef struct {
    mc_screen *s;
    int colors;
    int ox, oy; /* top-left corner of the layout */
} view;

static mc_style plain(void) {
    return mc_style_default();
}

static mc_style bold(void) {
    return mc_style_attributes(mc_style_default(), MC_AttrBold);
}

typedef struct {
    int r, g, b;
} rgb;

/* c mixed with white for amount > 0, with black for amount < 0. */
static rgb shade(rgb c, double amount) {
    if (amount < 0)
        return (rgb){(int)(c.r * (1 + amount)), (int)(c.g * (1 + amount)), (int)(c.b * (1 + amount))};
    return (rgb){c.r + (int)((255 - c.r) * amount), c.g + (int)((255 - c.g) * amount),
                 c.b + (int)((255 - c.b) * amount)};
}

/* The closest color of the 256-color palette's cube and gray ramp. */
static int nearest_color(rgb c) {
    static const int cube[6] = {0, 95, 135, 175, 215, 255};
    int best = 16;
    long best_distance = LONG_MAX;
    for (int i = 16; i < 256; i++) {
        int k = i - 16, gray = 8 + 10 * (i - 232);
        rgb p = i < 232 ? (rgb){cube[k / 36], cube[k / 6 % 6], cube[k % 6]} : (rgb){gray, gray, gray};
        long distance = (long)(p.r - c.r) * (p.r - c.r) + (long)(p.g - c.g) * (p.g - c.g) +
                        (long)(p.b - c.b) * (p.b - c.b);
        if (distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

/* The block color of a piece at a level, after the two colors NES Tetris
   gives each level (palette entries $21 $12, $29 $1A, ...). J/S take the
   second color, L/Z the first, and T/O/I, white with a border of the first
   color on the NES, a pale tint of it. Each piece then takes the closest
   shade not used by another piece, so pieces of a group differ slightly. */
static int piece_color(int piece, int level) {
    static const rgb nes[10][2] = {
        {{76, 154, 236}, {48, 50, 236}}, {{116, 196, 0}, {8, 124, 0}},
        {{228, 84, 236}, {136, 20, 176}}, {{76, 208, 32}, {48, 50, 236}},
        {{56, 204, 108}, {160, 20, 100}}, {{120, 124, 236}, {56, 204, 108}},
        {{84, 84, 84}, {152, 34, 32}},    {{92, 0, 48}, {92, 30, 228}},
        {{152, 34, 32}, {48, 50, 236}},   {{212, 136, 32}, {152, 34, 32}},
    };
    static int cache[10][PIECE_COUNT];
    static bool cached[10];
    int (*colors)[PIECE_COUNT] = &cache[level % 10];
    if (!cached[level % 10]) {
        const rgb *c = nes[level % 10];
        for (int p = 0; p < PIECE_COUNT; p++) {
            const piece_def *d = &pieces[p];
            rgb base = d->group == 0 ? shade(c[0], 0.35) : d->group == 1 ? c[1] : c[0];
            /* Shades 0, +4%, -4%, +8%, -8%, ... until one is not taken. */
            for (int step = 0; step <= 50; step++) {
                double amount = 0.04 * ((step + 1) / 2) * (step % 2 ? 1 : -1);
                (*colors)[p] = nearest_color(shade(base, amount));
                bool unique = true;
                for (int q = 0; q < p; q++)
                    unique &= (*colors)[q] != (*colors)[p];
                if (unique)
                    break;
            }
        }
        cached[level % 10] = true;
    }
    return (*colors)[piece];
}

/* One block is two terminal cells wide, as terminal cells are tall. */
static void draw_block(const view *v, int sx, int sy, int piece, int level) {
    if (v->colors < 8) {
        mc_screen_put_str(v->s, sx, sy, "[]");
        return;
    }
    int color = v->colors >= 256 ? piece_color(piece, level) : pieces[piece].basic_color;
    mc_screen_put_str_styled(v->s, sx, sy, "  ", mc_style_background(plain(), color));
}

static void draw_box(const view *v, int x1, int y1, int x2, int y2) {
    mc_screen *s = v->s;
    for (int x = x1 + 1; x < x2; x++) {
        mc_screen_set_content(s, x, y1, MC_RuneHLine, plain());
        mc_screen_set_content(s, x, y2, MC_RuneHLine, plain());
    }
    for (int y = y1 + 1; y < y2; y++) {
        mc_screen_set_content(s, x1, y, MC_RuneVLine, plain());
        mc_screen_set_content(s, x2, y, MC_RuneVLine, plain());
    }
    mc_screen_set_content(s, x1, y1, MC_RuneULCorner, plain());
    mc_screen_set_content(s, x2, y1, MC_RuneURCorner, plain());
    mc_screen_set_content(s, x1, y2, MC_RuneLLCorner, plain());
    mc_screen_set_content(s, x2, y2, MC_RuneLRCorner, plain());
}

/* Text at a position relative to the layout. */
static void draw_label(const view *v, int x, int y, mc_style style, const char *text) {
    mc_screen_put_str_styled(v->s, v->ox + x, v->oy + y, text, style);
}

/* Board cell (x, y) on the screen. */
static int board_x(const view *v, int x) {
    return v->ox + 17 + 2 * x;
}

static int board_y(const view *v, int y) {
    return v->oy + 1 + y;
}

static void draw_message(const view *v, int row, const char *text, mc_style style) {
    int x = board_x(v, 0) + (2 * BOARD_W - (int)strlen(text)) / 2;
    mc_screen_put_str_styled(v->s, x, board_y(v, row), text, style);
}

static void draw_board(const view *v, const game *g) {
    draw_box(v, board_x(v, 0) - 1, board_y(v, 0) - 1, board_x(v, BOARD_W), board_y(v, BOARD_H));
    if (g->mode == MODE_PAUSED) {
        draw_message(v, 9, "PAUSE", bold());
        return;
    }
    if (g->mode == MODE_MENU) {
        draw_message(v, 6, "TETRIS", bold());
        char level[16];
        snprintf(level, sizeof(level), "LEVEL < %2d >", g->start_level);
        draw_message(v, 9, level, bold());
        draw_message(v, 14, "ENTER: START", bold());
        return;
    }

    /* Line clear: the cleared rows disappear from the center outward. */
    int gap = g->clearing ? (CLEAR_FRAMES - g->clearing) / 4 + 1 : 0;
    for (int y = 0; y < BOARD_H; y++) {
        for (int x = 0; x < BOARD_W; x++) {
            bool erased = g->full[y] && g->clearing && x >= BOARD_W / 2 - gap &&
                          x < BOARD_W / 2 + gap;
            if (g->board[y][x] && !erased)
                draw_block(v, board_x(v, x), board_y(v, y), g->board[y][x] - 1, g->level);
        }
    }
    if (g->active) {
        for (int i = 0; i < 4; i++) {
            point b = block(g->piece, g->rot, i);
            if (g->y + b.y >= 0)
                draw_block(v, board_x(v, g->x + b.x), board_y(v, g->y + b.y), g->piece,
                           g->level);
        }
    }
    if (g->mode == MODE_OVER) {
        draw_message(v, 8, " GAME  OVER ", bold());
        draw_message(v, 10, " ENTER: MENU ", bold());
    }
}

static void draw_panels(const view *v, const game *g) {
    /* Statistics, left. */
    draw_box(v, v->ox, v->oy, v->ox + 14, v->oy + 10);
    char buf[16];
    draw_label(v, 2, 1, bold(), "STATISTICS");
    for (int p = 0; p < PIECE_COUNT; p++) {
        snprintf(buf, sizeof(buf), "%c  %5d", pieces[p].name, g->stats[p]);
        draw_label(v, 3, 2 + p, plain(), buf);
    }

    /* Score, lines, level and the next piece, right. */
    int px = 17 + 2 * BOARD_W + 2;
    draw_box(v, v->ox + px, v->oy, v->ox + px + 15, v->oy + 10);
    draw_label(v, px + 2, 1, bold(), "SCORE");
    snprintf(buf, sizeof(buf), "%06d", g->score);
    draw_label(v, px + 2, 2, plain(), buf);
    draw_label(v, px + 2, 4, bold(), "LINES");
    snprintf(buf, sizeof(buf), "%03d", g->lines);
    draw_label(v, px + 2, 5, plain(), buf);
    draw_label(v, px + 2, 7, bold(), "LEVEL");
    snprintf(buf, sizeof(buf), "%02d", g->mode == MODE_MENU ? g->start_level : g->level);
    draw_label(v, px + 2, 8, plain(), buf);

    draw_box(v, v->ox + px, v->oy + 11, v->ox + px + 15, v->oy + 16);
    draw_label(v, px + 2, 12, bold(), "NEXT");
    if (g->mode == MODE_PLAYING || g->mode == MODE_OVER) {
        for (int i = 0; i < 4; i++) {
            point b = block(g->next, 0, i);
            draw_block(v, v->ox + px + 7 + 2 * b.x, v->oy + 13 + b.y, g->next, g->level);
        }
    }

    draw_box(v, v->ox + px, v->oy + 17, v->ox + px + 15, v->oy + 20);
    draw_label(v, px + 2, 18, bold(), "TETRISES");
    snprintf(buf, sizeof(buf), "%03d", g->tetrises);
    draw_label(v, px + 2, 19, plain(), buf);
    if (g->tetris_flash && g->mode == MODE_PLAYING) {
        mc_attrmask blink = (g->tetris_flash / 8) % 2 ? MC_AttrReverse : MC_AttrNone;
        draw_label(v, px + 6, 19, mc_style_attributes(plain(), MC_AttrBold | blink), "TETRIS!");
    }

    static const char *const help[] = {"<- -> move", "DOWN  drop", "UP X  rotate",
                                       "Z     rotate", "P     pause", "Q     quit"};
    for (int i = 0; i < 6; i++)
        draw_label(v, 1, 12 + i, plain(), help[i]);
}

static void draw(mc_screen *s, const game *g) {
    int width, height;
    mc_screen_size(s, &width, &height);
    mc_screen_clear(s);
    if (width < LAYOUT_W || height < LAYOUT_H) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Need %dx%d. Q: quit", LAYOUT_W, LAYOUT_H);
        mc_screen_put_str(s, 0, 0, msg);
        return;
    }
    view v = {s, mc_screen_colors(s), (width - LAYOUT_W) / 2, (height - LAYOUT_H) / 2};
    draw_panels(&v, g);
    draw_board(&v, g);
}

/* --- Main loop ------------------------------------------------------------- */

static volatile sig_atomic_t quit;

static void on_signal(int sig) {
    (void)sig;
    quit = 1;
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    mc_screen *s = mc_screen_init(NULL);
    if (!s) {
        perror("mc_screen_init");
        return 1;
    }

    game g = {.mode = MODE_MENU};
    g.rng = (uint32_t)now_ns() ^ (uint32_t)getpid() ^ 0x9e3779b9u;
    if (!g.rng)
        g.rng = 1;

    int rc = 0;
    int64_t next_frame = now_ns();
    while (!quit) {
        int64_t now = now_ns();
        if (now >= next_frame) {
            frame(&g, now);
            draw(s, &g);
            if (mc_screen_show(s) < 0) {
                rc = 1;
                break;
            }
            next_frame += FRAME_NS;
            if (now - next_frame > 10 * FRAME_NS)
                next_frame = now + FRAME_NS; /* far behind, e.g. after a stop */
            continue;
        }
        mc_event ev;
        int n = mc_screen_poll_event(s, &ev, (int)((next_frame - now + 999999) / 1000000));
        if (n < 0) {
            rc = 1;
            break;
        }
        if (n > 0 && ev.type == MC_EventKey && !handle_key(&g, &ev, now_ns()))
            break;
    }
    mc_screen_fini(s);
    if (rc)
        perror("tetris");
    return rc;
}
