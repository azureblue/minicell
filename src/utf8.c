#include "utf8.h"

int mc_utf8_decode(const char *s, size_t n, uint32_t *cp, size_t *len) {
    if (!n)
        return 0;
    const unsigned char *p = (const unsigned char *)s;
    size_t need;
    uint32_t c, min;
    if (p[0] < 0x80) {
        *cp = p[0];
        *len = 1;
        return 1;
    } else if (p[0] >= 0xc2 && p[0] <= 0xdf) {
        need = 2, c = p[0] & 0x1f, min = 0x80;
    } else if (p[0] >= 0xe0 && p[0] <= 0xef) {
        need = 3, c = p[0] & 0x0f, min = 0x800;
    } else if (p[0] >= 0xf0 && p[0] <= 0xf4) {
        need = 4, c = p[0] & 0x07, min = 0x10000;
    } else {
        return -1;
    }
    for (size_t i = 1; i < need; i++) {
        if (i >= n)
            return 0;
        if ((p[i] & 0xc0) != 0x80)
            return -1;
        /* Reject overlong forms, surrogates and values above U+10FFFF early,
           so that a truncated invalid prefix is not reported as incomplete. */
        if (i == 1 && ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0) ||
                       (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90)))
            return -1;
        c = (c << 6) | (p[i] & 0x3f);
    }
    if (c < min)
        return -1;
    *cp = c;
    *len = need;
    return 1;
}

size_t mc_utf8_encode(uint32_t cp, char out[4]) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return 0;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}
