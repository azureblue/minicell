/* SPDX-FileCopyrightText: 2026 The TCell Authors
 * SPDX-FileCopyrightText: 2026 azureblue
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modified C port of tcell v3.6.0 input.go: scan, handleCsi, calcModifier,
 * postControlKey and the legacy rules of newEventKey. */
#include "input.h"

#include "keytables.h"

#include <string.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

enum {
    ESCAPE_TIMEOUT_MS = 50,     /* a lone ESC may still start a sequence */
    SEQUENCE_TIMEOUT_MS = 1000, /* bound for an incomplete sequence */
};

enum {
    ST_INIT,
    ST_ESC,
    ST_CSI,        /* ESC [ */
    ST_SS2,        /* ESC N */
    ST_SS3,        /* ESC O */
    ST_LINUX,      /* ESC [ [ - Linux console F1-F5 */
    ST_STRING,     /* OSC, DCS, SOS, PM, APC: swallowed up to ST or BEL */
    ST_STRING_ESC, /* ESC inside a string, possibly starting ST */
};

void mc_input_init(mc_input *in) {
    memset(in, 0, sizeof(*in));
}

/* tcell's calcModifier: xterm modifier parameter 1 + bitmask. */
static int calc_modifier(int n) {
    if (n < 1)
        return MC_ModNone;
    n--;
    int m = MC_ModNone;
    if (n & 1)
        m |= MC_ModShift;
    if (n & 2)
        m |= MC_ModAlt;
    if (n & 4)
        m |= MC_ModCtrl;
    if (n & 8)
        m |= MC_ModMeta;
    if (n & 16)
        m |= MC_ModHyper;
    if (n & 32)
        m |= MC_ModMeta;
    return m;
}

/* tcell's newEventKey with legacy (non-advanced) key reporting. */
static void normalize(int *key, uint32_t *ch, int *mods) {
    if (*key == MC_KeyRune) {
        if (*ch != 0 && (*ch < ' ' || *ch == 0x7f)) {
            *key = (int)*ch;
            if (*mods == MC_ModNone && *ch < ' ') {
                if (*key != MC_KeyBackspace && *key != MC_KeyTab && *key != MC_KeyEsc &&
                    *key != MC_KeyEnter)
                    *mods = MC_ModCtrl;
                *ch += 0x60;
            }
        }
        if (*mods == MC_ModCtrl) {
            if (*ch >= 'A' && *ch <= 'Z') {
                *key = MC_KeyCtrlA + (int)(*ch - 'A');
                *ch = 0;
            } else if (*ch >= 'a' && *ch <= 'z') {
                *key = MC_KeyCtrlA + (int)(*ch - 'a');
                *ch = 0;
            }
        }
        if (*mods == MC_ModShift && *ch)
            *mods = MC_ModNone;
    }
    if (*key == MC_KeyBackspace2)
        *key = MC_KeyBackspace;
}

/* tcell's postKey and post: ch is the text of a MC_KeyRune key, or 0. */
static bool post(mc_input *in, mc_event *ev, int key, uint32_t ch, int mods) {
    normalize(&key, &ch, &mods);
    if (in->escaped) {
        in->escaped = false;
        mods |= MC_ModAlt;
        normalize(&key, &ch, &mods);
    }
    *ev = (mc_event){.type = MC_EventKey, .key = key, .mods = mods};
    if (key == MC_KeyRune)
        ev->rune = ch;
    return true;
}

/* tcell's postControlKey for legacy reporting. */
static bool post_control(mc_input *in, mc_event *ev, uint32_t r, int mods) {
    if (r == 0)
        return post(in, ev, MC_KeyRune, ' ', mods | MC_ModCtrl);
    return post(in, ev, MC_KeyRune, r + 0x40, mods | MC_ModCtrl);
}

/* Splits the parameter bytes at ';', taking the number before any ':'. */
static size_t parse_params(const mc_input *in, int *p, size_t max) {
    if (!in->nparams || in->params[0] < '0' || in->params[0] > '9')
        return 0;
    size_t n = 0;
    const char *s = in->params, *end = in->params + in->nparams;
    while (n < max) {
        long v = 0;
        while (s < end && *s >= '0' && *s <= '9' && v < 0x7fffffff)
            v = v * 10 + (*s++ - '0');
        p[n++] = (int)v;
        while (s < end && *s != ';')
            s++; /* sub-parameters after ':' are not used */
        if (s == end)
            break;
        s++;
    }
    return n;
}

/* tcell's handleCsi, without the replies to queries minicell never sends,
   mouse, focus and the kitty and win32 keyboard protocols. */
static bool handle_csi(mc_input *in, char mode, mc_event *ev) {
    in->state = ST_INIT;
    if (in->overflow || in->intermediate)
        return false;
    if (in->nparams && strchr("<?>", in->params[0]))
        return false;
    int p[8];
    size_t np = parse_params(in, p, COUNT(p));
    int p0 = np ? p[0] : 0;

    if (mode == '[') {
        in->state = ST_LINUX;
        return false;
    }
    if (mode == '~' && np >= 2) {
        int mods = calc_modifier(p[1]);
        for (size_t i = 0; i < COUNT(csi_keys); i++)
            if (csi_keys[i].final == '~' && csi_keys[i].param == p0)
                return post(in, ev, csi_keys[i].key, 0, mods);
        if (p0 == 27 && np > 2 && p[2] > 0 && p[2] <= 0x10ffff) { /* modifyOtherKeys */
            if (p[2] < ' ' || p[2] == 0x7f)
                return post(in, ev, p[2], 0, mods);
            return post(in, ev, MC_KeyRune, (uint32_t)p[2], mods);
        }
    }
    for (size_t i = 0; i < COUNT(csi_keys); i++) {
        if (csi_keys[i].final == mode && csi_keys[i].param == p0) {
            int mods = csi_keys[i].mods;
            if (mode == '~' && np > 1 && mods == MC_ModNone)
                mods = calc_modifier(p[1]);
            return post(in, ev, csi_keys[i].key, 0, mods);
        }
    }
    /* An SS3 key sent as CSI 1 ; <modifier> <final>, e.g. Ctrl-Left. */
    for (size_t i = 0; i < COUNT(ss3_keys); i++)
        if (ss3_keys[i].final == mode && p0 == 1 && np > 1)
            return post(in, ev, ss3_keys[i].key, (uint32_t)ss3_keys[i].rune, calc_modifier(p[1]));
    return false;
}

/* A non-empty run of decimal digits, like Go's strconv.Atoi on these bytes. */
static bool parse_int(const char *s, size_t n, int *out) {
    if (!n || n > 9)
        return false;
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

/* SS3 <params> <final>: params are empty, "1;<mod>", ";<mod>" or "<mod>". */
static bool handle_ss3(mc_input *in, char final, mc_event *ev) {
    for (size_t i = 0; i < COUNT(ss3_keys); i++) {
        if (ss3_keys[i].final != final)
            continue;
        int key = ss3_keys[i].key;
        uint32_t ch = (uint32_t)ss3_keys[i].rune;
        if (!in->nparams)
            return post(in, ev, key, ch, MC_ModNone);
        const char *s = in->params, *end = s + in->nparams;
        const char *semi = memchr(s, ';', in->nparams);
        int m;
        if (in->overflow)
            return false;
        if (semi) {
            const char *second = semi + 1;
            const char *next = memchr(second, ';', (size_t)(end - second));
            bool first_ok = semi == s || (semi == s + 1 && s[0] == '1');
            if (!first_ok || !parse_int(second, (size_t)((next ? next : end) - second), &m))
                return false;
        } else if (!parse_int(s, in->nparams, &m)) {
            return false;
        }
        return post(in, ev, key, ch, calc_modifier(m));
    }
    return false;
}

static void add_param(mc_input *in, char c) {
    if (in->nparams < sizeof(in->params))
        in->params[in->nparams++] = c;
    else
        in->overflow = true;
}

static void start_sequence(mc_input *in, int state, char introducer) {
    in->state = state;
    in->nparams = 0;
    in->overflow = false;
    in->intermediate = false;
    in->esc_char = introducer;
}

bool mc_input_feed(mc_input *in, uint32_t r, mc_event *ev) {
    in->esc_char = 0;
    if (r >= 0xa0) {
        in->state = ST_INIT;
        return post(in, ev, MC_KeyRune, r, MC_ModNone);
    }
    if (r >= 0x80) { /* 8-bit C1 control: same as ESC + (r - 0x40) */
        in->state = ST_ESC;
        r -= 0x40;
    }
    switch (in->state) {
    case ST_INIT:
        switch (r) {
        case 0x1b:
            in->state = ST_ESC;
            return false;
        case '\t':
            return post(in, ev, MC_KeyTab, 0, MC_ModNone);
        case '\b':
        case 0x7f:
            return post(in, ev, MC_KeyBackspace, 0, MC_ModNone);
        case '\r':
            return post(in, ev, MC_KeyEnter, 0, MC_ModNone);
        default:
            if (r < ' ')
                return post_control(in, ev, r, MC_ModNone);
            return post(in, ev, MC_KeyRune, r, MC_ModNone);
        }

    case ST_ESC:
        switch (r) {
        case '[':
            start_sequence(in, ST_CSI, '[');
            return false;
        case 'O':
            start_sequence(in, ST_SS3, 'O');
            return false;
        case 'N':
            start_sequence(in, ST_SS2, 'N');
            return false;
        case ']':
        case 'P':
        case 'X':
        case '^':
        case '_':
            start_sequence(in, ST_STRING, (char)r);
            return false;
        case '\\': /* orphaned string terminator */
            in->state = ST_INIT;
            return false;
        case '\t': /* Linux console only */
            in->state = ST_INIT;
            return post(in, ev, MC_KeyBacktab, 0, MC_ModNone);
        case 0x1b: /* leading ESC to capture Alt */
            in->escaped = true;
            in->esc_char = 0x1b;
            return false;
        default: { /* Alt + key, legacy terminals */
            in->state = ST_INIT;
            int mods = MC_ModAlt;
            if (r < ' ') {
                mods |= MC_ModCtrl;
                r += 0x60;
            }
            return post(in, ev, MC_KeyRune, r, mods);
        }
        }

    case ST_CSI:
        if (r == 0x1b) {
            in->state = ST_ESC;
        } else if (r >= 0x30 && r <= 0x3f) {
            add_param(in, (char)r);
        } else if (r == '$' && in->nparams && in->params[0] != '?') { /* rxvt */
            return handle_csi(in, (char)r, ev);
        } else if (r >= 0x20 && r <= 0x2f) {
            in->intermediate = true;
        } else if (r >= 0x40 && r <= 0x7f) {
            return handle_csi(in, (char)r, ev);
        } else {
            in->state = ST_INIT;
        }
        return false;

    case ST_SS2:
        in->state = ST_INIT;
        return false;

    case ST_SS3:
        in->state = ST_INIT;
        if (r == 0x1b) {
            in->state = ST_ESC;
        } else if (r >= 0x30 && r <= 0x3f) {
            add_param(in, (char)r);
            in->state = ST_SS3;
        } else {
            return handle_ss3(in, (char)r, ev);
        }
        return false;

    case ST_LINUX:
        in->state = ST_INIT;
        for (size_t i = 0; i < COUNT(linux_keys); i++)
            if ((uint32_t)linux_keys[i].final == r)
                return post(in, ev, linux_keys[i].key, 0, MC_ModNone);
        return false;

    case ST_STRING:
        if (r == 0x1b)
            in->state = ST_STRING_ESC;
        else if (r == 0x07)
            in->state = ST_INIT;
        return false;

    case ST_STRING_ESC:
        in->state = (r == '\\' || r == 0x07) ? ST_INIT : ST_STRING;
        return false;
    }
    return false;
}

int mc_input_wait_ms(const mc_input *in) {
    if (in->state == ST_INIT)
        return 0;
    return in->state == ST_ESC ? ESCAPE_TIMEOUT_MS : SEQUENCE_TIMEOUT_MS;
}

bool mc_input_expire(mc_input *in, mc_event *ev) {
    int state = in->state;
    char esc_char = in->esc_char;
    in->state = ST_INIT;
    if (state == ST_ESC)
        return post(in, ev, MC_KeyEscape, 0, MC_ModNone);
    if (esc_char)
        return post(in, ev, MC_KeyRune, (unsigned char)esc_char, MC_ModAlt);
    return false;
}
