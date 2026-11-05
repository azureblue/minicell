// SPDX-FileCopyrightText: 2026 azureblue
// SPDX-License-Identifier: Apache-2.0
// The public header must compile as C++11 and link against the C library.
#include "minicell.h"

int main() {
    mc_style style = mc_style_attributes(mc_style_foreground(mc_style_default(), 6),
                                         MC_AttrBold | MC_AttrUnderline);
    mc_event ev = mc_event();
    bool ok = style.fg == 6 && style.bg == MC_ColorDefault && (style.attrs & MC_AttrBold) &&
              ev.type == MC_EventNone && MC_KeyCtrlC == 'C' && MC_RuneHLine == 0x2500;
    return ok ? 0 : 1;
}
