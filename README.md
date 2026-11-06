# minicell

A small terminal cell library for C and C++, modeled on
[tcell](https://github.com/gdamore/tcell). The only dependency is libc.

## Terminals

Like tcell v3, minicell has no terminal database. It assumes that every
terminal understands the xterm (ECMA-48) escape sequences, which is true of
practically all terminals in use today. Key input is decoded by a generic 
parser that understands every xterm modifier combination, plus the rxvt and 
Linux console variants.

`$TERM` (or the name passed to `mc_screen_init`) only selects the palette
size, by tcell's rules: `*-256color` → 256, `xterm`, `linux` and `*color` → 8,
`vt*` → none, anything else → 256. `COLORTERM` and `NO_COLOR` are honored.

All escape sequences, key tables and key, modifier, attribute and rune
constants are generated from tcell v3.6.0 sources. They are not stored in
this repository: `make regenerate` downloads the files pinned in
`SOURCES.sha256` and verifies their checksums. Building needs no network.

## Build

```sh
make             # build/libminicell.a and build/demo
./build/demo     # q, Escape or Ctrl-C to quit
make test        # unit tests, C++ header test, PTY test (needs python3)
```

```sh
cc  -std=c11   -Iinclude app.c   build/libminicell.a -o app
c++ -std=c++11 -Iinclude app.cpp build/libminicell.a -o app
```

## Example

```c
#include "minicell.h"
#include <stdio.h>

int main(void) {
    mc_screen *s = mc_screen_init(NULL); /* NULL: use $TERM */
    if (!s) {
        perror("mc_screen_init");
        return 1;
    }
    mc_style bold = mc_style_attributes(mc_style_default(), MC_AttrBold);
    for (;;) {
        mc_screen_clear(s);
        mc_screen_put_str_styled(s, 2, 1, "Witaj, świecie!", bold);
        mc_screen_put_str(s, 2, 2, "Press q to quit.");
        mc_screen_show(s);

        mc_event ev;
        if (mc_screen_poll_event(s, &ev, -1) < 0)
            break;
        if (ev.type == MC_EventKey && ev.key == MC_KeyRune && ev.rune == 'q')
            break;
    }
    mc_screen_fini(s);
    return 0;
}
```

## API overview

The names mirror tcell: `Screen.SetContent` is `mc_screen_set_content`,
`KeyUp` is `MC_KeyUp`, `ModCtrl` is `MC_ModCtrl`, and so on. See
`include/minicell.h` for the full reference.

| Function | tcell equivalent |
|---|---|
| `mc_screen_init(term)`, `mc_screen_fini(s)` | `NewScreen` + `Init`, `Fini` |
| `mc_screen_size`, `mc_screen_colors` | `Size`, `Colors` |
| `mc_screen_set_style`, `mc_screen_clear` | `SetStyle`, `Clear` |
| `mc_screen_set_content(s, x, y, ch, style)` | `SetContent` |
| `mc_screen_put`, `mc_screen_put_str`, `mc_screen_put_str_styled` | `Put`, `PutStr`, `PutStrStyled` |
| `mc_screen_show_cursor`, `mc_screen_hide_cursor` | `ShowCursor`, `HideCursor` |
| `mc_screen_show`, `mc_screen_sync` | `Show`, `Sync` |
| `mc_screen_poll_event(s, &ev, timeout_ms)` | `PollEvent` |

**Styles.** `mc_style` is a plain struct `{fg, bg, attrs}`. Colors are palette
indices (0–255 on xterm-256color, 0–7 on the Linux console) or
`MC_ColorDefault`. The helpers `mc_style_foreground`, `mc_style_background`
and `mc_style_attributes` return a modified copy, like tcell's `Style` methods.
Colors beyond the terminal's palette (`mc_screen_colors`) are drawn in the
default color.

**Text.** Strings are UTF-8 and every character occupies one cell. Wide
characters (CJK, emoji) and combining accents are not supported yet; use
precomposed letters such as `ą`. Drawing outside the screen is clipped
silently. Control characters are rejected with `EINVAL`, so rendered text can
never inject escape sequences.

**Events.** `mc_screen_poll_event` returns 1 with an event, 0 on timeout or
when a signal interrupts it, and -1 on error. Key events carry `key`, `rune`
and `mods` as tcell v3 reports them in its legacy keyboard mode, e.g. Ctrl+C
is `MC_KeyCtrlC` with `MC_ModCtrl`. A resize event clears the screen;
redraw and call `mc_screen_show`. A lone Escape is reported after 50 ms.

**Errors.** Functions returning `int` return -1 and set `errno`.

**Limits.** Single-threaded; one screen at a time. Not yet supported: mouse,
bracketed paste, true color, wide characters, combining marks, and tcell's
startup queries to the terminal (kitty keyboard protocol and similar). The
library opens `/dev/tty`, installs no signal handlers and does not call
`setlocale`. Always call `mc_screen_fini`, also on error
paths, to restore the terminal. `examples/demo.c` shows how to do that on
SIGINT/SIGTERM.

## Layout

| Path | Contents |
|---|---|
| `include/minicell.h` | Public API |
| `include/minicell_tcell.h` | Key, modifier, attribute and rune constants *(generated)* |
| `src/screen.c` | Terminal setup, cell buffers, rendering, event loop |
| `src/input.c` | Key input parser |
| `src/utf8.c` | UTF-8 encoding and decoding |
| `src/vt.h` | Escape sequences written to the terminal *(generated)* |
| `src/keytables.h` | Key sequences read from the terminal *(generated)* |
| `tools/generate.py` | Generator; `make regenerate` (Python 3, network on first run) |
| `SOURCES.sha256` | Checksums of the tcell files the generator uses; see `SOURCES.md` |

## License

Copyright 2026 azureblue. Licensed under the Apache License, Version 2.0; see
`LICENSE`. minicell contains code and data derived from tcell, Copyright 2026
The TCell Authors, also under Apache-2.0; see `NOTICE`. Files derived from tcell
state its copyright and the changes in their header.
