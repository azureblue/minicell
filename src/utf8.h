/* SPDX-FileCopyrightText: 2026 azureblue
 * SPDX-License-Identifier: Apache-2.0 */
/* Strict UTF-8 coding. */
#ifndef MC_UTF8_H
#define MC_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Decodes one code point. Returns 1 on success, 0 if s is a truncated
   sequence, -1 if s is invalid. */
int mc_utf8_decode(const char *s, size_t n, uint32_t *cp, size_t *len);

/* Returns the number of bytes written (1-4), or 0 for a non-scalar value. */
size_t mc_utf8_encode(uint32_t cp, char out[4]);

#endif
