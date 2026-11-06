/* Keyboard input parser.
 * Copyright 2026 The TCell Authors; modified C port by azureblue.
 * Apache License 2.0.
 *
 * Port of the legacy key handling in tcell v3.6.0 input.go; mouse, paste,
 * focus, query replies and the kitty and win32 keyboard protocols were left
 * out. Every terminal is assumed to send xterm (ECMA-48) sequences. */
#ifndef MC_INPUT_H
#define MC_INPUT_H

#include "minicell.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int state;
    char params[32]; /* CSI/SS3 parameter bytes */
    size_t nparams;
    bool overflow;   /* too many parameter bytes: ignore the sequence */
    bool intermediate;
    char esc_char;   /* introducer of a pending sequence, reported on timeout */
    bool escaped;    /* ESC ESC: the next key gets ModAlt */
} mc_input;

void mc_input_init(mc_input *in);

/* Feeds one decoded character. Returns true and fills *ev if a key is done. */
bool mc_input_feed(mc_input *in, uint32_t r, mc_event *ev);

/* Milliseconds to wait for more input before calling mc_input_expire():
   50 after a lone ESC, 1000 inside a sequence, 0 if nothing is pending. */
int mc_input_wait_ms(const mc_input *in);

/* Ends a pending sequence after the wait: a lone ESC becomes the Escape key.
   Returns true and fills *ev if that produced a key. */
bool mc_input_expire(mc_input *in, mc_event *ev);

#endif
