#pragma once

// ANSI / VT100 terminal emulation on top of the DVI text console.
//
// Supported: cursor movement (CUU/CUD/CUF/CUB/CNL/CPL/CHA/VPA/CUP/HVP,
// save/restore), erase (ED/EL/ECH), insert/delete lines, SGR (16 colours,
// bold, reverse), device status report (ESC[6n) and the usual C0 controls.
// Everything else (modes, scroll regions, OSC strings, ...) is parsed and
// ignored. Bytes >= 0x80 are drawn as CP437 glyphs.

#include <stdint.h>

// `reply` is called to send bytes back to the remote end (cursor position
// reports).
typedef void (*ansi_reply_fn)(const uint8_t *data, uint16_t len);

// Resets the emulator state and switches the console to the default ANSI
// colours. The previous console colours are remembered for ansi_end().
void ansi_begin(ansi_reply_fn reply);

// Restores the console colours that were active at ansi_begin().
void ansi_end(void);

// Feeds one byte received from the remote end.
void ansi_feed(uint8_t b);
