/* SPDX-FileCopyrightText: 2026 azureblue
 * SPDX-License-Identifier: Apache-2.0 */
#include "check.h"
#include "utf8.h"

#include <string.h>

static void test_invalid(void) {
    static const char *const invalid[] = {
        "\xc0\x80",         /* overlong */
        "\xe0\x80\x80",     /* overlong */
        "\xed\xa0\x80",     /* surrogate */
        "\xf4\x90\x80\x80", /* above U+10FFFF */
        "\x80",             /* lone continuation byte */
        "\xff",
    };
    for (size_t i = 0; i < COUNT(invalid); i++) {
        uint32_t cp;
        size_t len;
        CHECK(mc_utf8_decode(invalid[i], strlen(invalid[i]), &cp, &len) == -1);
    }
}

/* Every scalar value encodes and decodes back; every prefix is incomplete. */
static void test_round_trip(void) {
    for (uint32_t cp = 0; cp <= 0x10ffff; cp++) {
        char buf[4];
        size_t n = mc_utf8_encode(cp, buf);
        if (cp >= 0xd800 && cp <= 0xdfff) {
            CHECK(n == 0);
            continue;
        }
        uint32_t got;
        size_t len;
        CHECK(mc_utf8_decode(buf, n, &got, &len) == 1 && got == cp && len == n);
        for (size_t k = 0; k < n; k++)
            CHECK(mc_utf8_decode(buf, k, &got, &len) == 0);
    }
}

int main(void) {
    test_invalid();
    test_round_trip();
    puts("utf8_test: ok");
    return 0;
}
