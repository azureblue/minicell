/* SPDX-FileCopyrightText: 2026 azureblue
 * SPDX-License-Identifier: Apache-2.0 */
#include "check.h"
#include "input.h"
#include "utf8.h"

#include <string.h>

/* Feeds UTF-8 bytes one character at a time and collects the events. If
   expire is set, the escape timeout then elapses. Returns the event count. */
static int feed(const char *bytes, bool expire, mc_event *evs, int max) {
    mc_input in;
    mc_input_init(&in);
    int n = 0;
    size_t len = strlen(bytes);
    while (len) {
        uint32_t r;
        size_t k;
        CHECK(mc_utf8_decode(bytes, len, &r, &k) == 1);
        bytes += k;
        len -= k;
        if (mc_input_feed(&in, r, &evs[n]))
            CHECK(++n <= max);
    }
    if (expire && mc_input_wait_ms(&in) && mc_input_expire(&in, &evs[n]))
        CHECK(++n <= max);
    return n;
}

typedef struct {
    const char *input;
    int key;
    int mods;
    uint32_t rune;
} key_case;

static void check_single_keys(const key_case *cases, size_t ncases) {
    for (size_t i = 0; i < ncases; i++) {
        mc_event ev[4];
        int n = feed(cases[i].input, true, ev, 3);
        if (n != 1 || ev[0].key != cases[i].key || ev[0].mods != cases[i].mods ||
            ev[0].rune != cases[i].rune) {
            fprintf(stderr, "case %zu: %d events, key=%d mods=%d rune=%u\n", i, n,
                    n ? ev[0].key : -1, n ? ev[0].mods : -1, n ? (unsigned)ev[0].rune : 0);
            exit(1);
        }
        CHECK(ev[0].type == MC_EventKey);
    }
}

static void test_plain_keys(void) {
    static const key_case cases[] = {
        {"a", MC_KeyRune, MC_ModNone, 'a'},
        {"ą", MC_KeyRune, MC_ModNone, 0x105},
        {"\t", MC_KeyTab, MC_ModNone, 0},
        {"\r", MC_KeyEnter, MC_ModNone, 0},
        {"\177", MC_KeyBackspace, MC_ModNone, 0},
        {"\b", MC_KeyBackspace, MC_ModNone, 0},
        {"\001", MC_KeyCtrlA, MC_ModCtrl, 0},
        {"\003", MC_KeyCtrlC, MC_ModCtrl, 0},
        {"\032", MC_KeyCtrlZ, MC_ModCtrl, 0},
        {"\034", MC_KeyRune, MC_ModCtrl, '\\'},
    };
    check_single_keys(cases, COUNT(cases));

    /* NUL is Ctrl-Space; it cannot be part of a C string. */
    mc_input in;
    mc_input_init(&in);
    mc_event ev;
    CHECK(mc_input_feed(&in, 0, &ev));
    CHECK(ev.key == MC_KeyRune && ev.rune == ' ' && ev.mods == MC_ModCtrl);
}

static void test_csi_keys(void) {
    static const key_case cases[] = {
        {"\033[A", MC_KeyUp, MC_ModNone, 0},
        {"\033[H", MC_KeyHome, MC_ModNone, 0},
        {"\033[Z", MC_KeyBacktab, MC_ModNone, 0},
        {"\033[3~", MC_KeyDelete, MC_ModNone, 0},
        {"\033[15~", MC_KeyF5, MC_ModNone, 0},
        {"\033[24~", MC_KeyF12, MC_ModNone, 0},
        /* xterm modifiers: parameter = 1 + Shift 1, Alt 2, Ctrl 4, Meta 8 */
        {"\033[1;5D", MC_KeyLeft, MC_ModCtrl, 0},
        {"\033[1;2A", MC_KeyUp, MC_ModShift, 0},
        {"\033[1;4C", MC_KeyRight, MC_ModShift | MC_ModAlt, 0},
        {"\033[1;9B", MC_KeyDown, MC_ModMeta, 0},
        {"\033[3;5~", MC_KeyDelete, MC_ModCtrl, 0},
        {"\033[24;2~", MC_KeyF12, MC_ModShift, 0},
        {"\033[1;5P", MC_KeyF1, MC_ModCtrl, 0},
        /* rxvt */
        {"\033[a", MC_KeyUp, MC_ModShift, 0},
        {"\033[3$", MC_KeyDelete, MC_ModShift, 0},
        {"\033[5^", MC_KeyPgUp, MC_ModCtrl, 0},
        /* xterm modifyOtherKeys */
        {"\033[27;5;105~", MC_KeyCtrlI, MC_ModCtrl, 0}, /* legacy: Ctrl+i is KeyCtrlI */
    };
    check_single_keys(cases, COUNT(cases));
}

static void test_ss3_and_linux_keys(void) {
    static const key_case cases[] = {
        {"\033OA", MC_KeyUp, MC_ModNone, 0},
        {"\033OP", MC_KeyF1, MC_ModNone, 0},
        {"\033OM", MC_KeyEnter, MC_ModNone, 0},
        {"\033Oj", MC_KeyRune, MC_ModNone, '*'},
        {"\033O5A", MC_KeyUp, MC_ModCtrl, 0},
        {"\033O1;3B", MC_KeyDown, MC_ModAlt, 0},
        {"\033[[A", MC_KeyF1, MC_ModNone, 0},
        {"\033[[E", MC_KeyF5, MC_ModNone, 0},
        {"\033\t", MC_KeyBacktab, MC_ModNone, 0},
    };
    check_single_keys(cases, COUNT(cases));
}

static void test_escape_and_alt(void) {
    static const key_case cases[] = {
        {"\033", MC_KeyEsc, MC_ModNone, 0},
        {"\033a", MC_KeyRune, MC_ModAlt, 'a'},
        {"\033ą", MC_KeyRune, MC_ModNone, 0x105}, /* as in tcell: U+00A0+ cancels ESC */
        {"\033\001", MC_KeyRune, MC_ModAlt | MC_ModCtrl, 'a'},
        {"\033\033", MC_KeyEsc, MC_ModAlt, 0},
        {"\033\033[A", MC_KeyUp, MC_ModAlt, 0},
        {"\033[", MC_KeyRune, MC_ModAlt, '['}, /* timed out after the introducer */
    };
    check_single_keys(cases, COUNT(cases));

    mc_input in;
    mc_input_init(&in);
    mc_event ev;
    CHECK(!mc_input_feed(&in, 0x1b, &ev) && mc_input_wait_ms(&in) == 50);
    CHECK(!mc_input_feed(&in, '[', &ev) && mc_input_wait_ms(&in) == 1000);
    CHECK(!mc_input_feed(&in, '1', &ev) && !mc_input_expire(&in, &ev));
    CHECK(mc_input_wait_ms(&in) == 0);
}

static void test_ignored_and_combined(void) {
    mc_event ev[4];
    CHECK(feed("\033[99~", true, ev, 3) == 0);             /* unknown key */
    CHECK(feed("\033[<0;1;1M", true, ev, 3) == 0);         /* mouse report */
    CHECK(feed("\033[?62;22c", true, ev, 3) == 0);         /* device attributes */
    CHECK(feed("\033]11;rgb:0/0/0\007", true, ev, 3) == 0); /* OSC reply */
    CHECK(feed("\033]0;x\033\\a", true, ev, 3) == 1 && ev[0].rune == 'a');
    CHECK(feed("\033[Ax\033OB", true, ev, 3) == 3); /* several keys in one read */
    CHECK(ev[0].key == MC_KeyUp && ev[1].rune == 'x' && ev[2].key == MC_KeyDown);
}

int main(void) {
    test_plain_keys();
    test_csi_keys();
    test_ss3_and_linux_keys();
    test_escape_and_alt();
    test_ignored_and_combined();
    puts("input_test: ok");
    return 0;
}
