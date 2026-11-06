/* Rules of examples/tetris.c, tested without a terminal. */
#define main tetris_main
#include "../examples/tetris.c"
#undef main

#include "check.h"

static bool has_block(int p, int r, int x, int y) {
    for (int i = 0; i < 4; i++) {
        point b = block(p, r, i);
        if (b.x == x && b.y == y)
            return true;
    }
    return false;
}

static void test_rotation(void) {
    /* T spawns pointing down; clockwise it points left, then up. */
    CHECK(has_block(PIECE_T, 0, 0, 1) && has_block(PIECE_T, 0, -1, 0));
    CHECK(has_block(PIECE_T, 1, -1, 0) && has_block(PIECE_T, 1, 0, -1) &&
          has_block(PIECE_T, 1, 0, 1));
    CHECK(has_block(PIECE_T, 2, 0, -1) && !has_block(PIECE_T, 2, 0, 1));
    CHECK(has_block(PIECE_T, 3, 1, 0));
    /* S, Z and I have two orientations, O one. */
    for (int i = 0; i < 4; i++) {
        point a = block(PIECE_I, 1, i), b = block(PIECE_I, 3, i);
        CHECK(a.x == b.x && a.y == b.y && a.x == 0);
        point o = block(PIECE_O, 2, i), o0 = block(PIECE_O, 0, i);
        CHECK(o.x == o0.x && o.y == o0.y);
    }
}

static void test_tables(void) {
    CHECK(gravity_frames(0) == 48 && gravity_frames(9) == 6 && gravity_frames(10) == 5);
    CHECK(gravity_frames(18) == 3 && gravity_frames(19) == 2 && gravity_frames(29) == 1);
    CHECK(first_level_up(0) == 10 && first_level_up(9) == 100 && first_level_up(15) == 100);
    CHECK(first_level_up(18) == 130 && first_level_up(19) == 140);
    CHECK(entry_delay(19) == 10 && entry_delay(18) == 10 && entry_delay(17) == 12);
    CHECK(entry_delay(0) == 18);
}

static void run_frames(game *g, int n, int64_t *now) {
    for (int i = 0; i < n; i++) {
        *now += FRAME_NS;
        frame(g, *now);
    }
}

static void test_tetris(void) {
    int64_t now = 1000000000000;
    game g = {.start_level = 5, .rng = 12345};
    new_game(&g, now);
    for (int y = BOARD_H - 4; y < BOARD_H; y++)
        for (int x = 1; x < BOARD_W; x++)
            g.board[y][x] = PIECE_O + 1;
    g.next = PIECE_I;
    run_frames(&g, 1, &now); /* spawn */
    CHECK(g.active && g.piece == PIECE_I && g.stats[PIECE_I] == 1);

    /* Vertical I into the empty column 0, then soft drop to the bottom. */
    queue(&g, ACT_CW);
    for (int i = 0; i < 5; i++)
        queue(&g, ACT_LEFT);
    run_frames(&g, 1, &now);
    CHECK(g.rot == 1 && g.x == 0);
    while (g.active) {
        queue(&g, ACT_DOWN);
        run_frames(&g, 1, &now);
    }
    CHECK(g.clearing == CLEAR_FRAMES && g.full[BOARD_H - 1] && !g.full[BOARD_H - 5]);
    CHECK(g.score == 18); /* soft-dropped rows: the pivot fell from row 0 to 18 */
    run_frames(&g, CLEAR_FRAMES, &now);
    CHECK(g.lines == 4 && g.score == 18 + 1200 * 6 && g.level == 5);
    CHECK(g.tetrises == 1 && g.tetris_flash > 0);
    for (int x = 0; x < BOARD_W; x++)
        CHECK(!g.board[BOARD_H - 1][x]);
}

static void test_down_must_be_released(void) {
    int64_t now = 1000000000000;
    game g = {.rng = 1};
    new_game(&g, now);
    g.last_down = now; /* Down still held when the piece spawns */
    run_frames(&g, 1, &now);
    CHECK(g.active && g.down_blocked);
    queue(&g, ACT_DOWN);
    run_frames(&g, 1, &now);
    CHECK(g.y == 0 && g.soft_rows == 0);
    run_frames(&g, 7, &now); /* more than 100 ms without Down */
    CHECK(!g.down_blocked);
}

static void test_game_over(void) {
    int64_t now = 1000000000000;
    game g = {.rng = 7};
    new_game(&g, now);
    for (int x = 0; x < BOARD_W; x++)
        g.board[1][x] = PIECE_O + 1;
    g.board[1][0] = 0; /* not full, so nothing clears */
    run_frames(&g, 1, &now);
    CHECK(g.mode == MODE_OVER || g.active);
    while (g.mode == MODE_PLAYING)
        run_frames(&g, 1, &now);
    CHECK(g.mode == MODE_OVER);
}

/* Within a level every piece has its own color; levels repeat every 10. */
static void test_palette(void) {
    for (int level = 0; level < 10; level++) {
        for (int p = 0; p < PIECE_COUNT; p++)
            for (int q = 0; q < p; q++)
                CHECK(piece_color(p, level) != piece_color(q, level));
        CHECK(piece_color(PIECE_J, level) == piece_color(PIECE_J, level + 10));
    }
}

int main(void) {
    test_palette();
    test_rotation();
    test_tables();
    test_tetris();
    test_down_must_be_released();
    test_game_over();
    puts("tetris_test: ok");
    return 0;
}
