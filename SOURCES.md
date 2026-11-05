# Sources

All generated files come from unmodified tcell sources, which are not stored
in this repository. `SOURCES.sha256` pins each file by its SHA-256 checksum.
`make regenerate` downloads them from GitHub into `build/tcell`, verifies the
checksums and regenerates the files. To check the sources by hand:

```sh
make regenerate
cd build/tcell && sha256sum -c ../../SOURCES.sha256
```

## tcell v3.6.0 (Apache-2.0)

https://github.com/gdamore/tcell/tree/v3.6.0

| File | Used for |
|---|---|
| `key.go` | Key and modifier values → `include/minicell_tcell.h` |
| `attr.go` | Attribute values → `include/minicell_tcell.h` |
| `runes.go` | Rune constants (line drawing) → `include/minicell_tcell.h` |
| `input.go` | `csiAllKeys`, `ss3Keys`, `linuxFKeys` → `src/keytables.h`; the parser in `src/input.c` is a port of its `scan` and `handleCsi` |
| `tscreen.go` | Output sequences → `src/vt.h`; `src/screen.c` follows its `Init` palette rules, `engage`, `draw` and `disengage` |
| `vt/mode.go` | Private mode numbers and their set/reset format → `src/vt.h` |

tcell's license is the Apache License 2.0, the same text as `LICENSE`.

minicell ports the legacy keyboard mode only. tcell's terminal queries,
mouse, bracketed paste, focus, kitty and win32 keyboard protocols, true color
and window titles beyond save/restore are not used. The generator targets
this exact tcell version; upgrading requires review.
