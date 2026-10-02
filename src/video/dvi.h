#pragma once

#include <stdint.h>

// Text console geometry (640x480 framebuffer, 16x16 pixel cells -> 40x30 chars)
#define DVI_COLS 40
#define DVI_ROWS 30

// Bring up HSTX, DMA and the scanline IRQ, and start DVI output.
// After this returns, the console is live and driven entirely by DMA + IRQ;
// the caller is free to run other code (e.g. the BASIC interpreter) on core0.
void dvi_init(void);

// Clear the screen and home the cursor.
void dvi_clear(void);

// Write one character to the console (handles \n, \r, \b and line wrap/scroll).
void dvi_putc(char c);

// Write a NUL-terminated string.
void dvi_puts(const char *s);

// Set foreground/background colour for subsequently drawn characters.
// Colours are RGB332 (0bRRRGGGBB).
void dvi_set_colors(uint8_t fg, uint8_t bg);

uint8_t dvi_rgb332(uint8_t r, uint8_t g, uint8_t b);

// Current cursor column (0-based), useful for PRINT tab-zone alignment.
int dvi_get_col(void);
