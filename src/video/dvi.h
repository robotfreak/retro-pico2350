#pragma once

#include <stdint.h>

// Text console geometry (640x480 framebuffer, 8x16 pixel cells -> 80x30 chars)
#define DVI_COLS 80
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

// Cursor positioning and screen editing (0-based coordinates, clamped to the
// screen). Used by the ANSI terminal emulation.
int  dvi_get_row(void);
void dvi_goto(int col, int row);
void dvi_get_colors(uint8_t *fg, uint8_t *bg);
// mode: 0 = cursor to end, 1 = start to cursor, 2 = everything (cursor stays).
void dvi_erase_display(int mode);
void dvi_erase_line(int mode);
void dvi_erase_chars(int n);        // blank n cells from the cursor
void dvi_insert_lines(int n);       // at cursor row, pushes the rest down
void dvi_delete_lines(int n);       // at cursor row, pulls the rest up

// Current cursor column (0-based), useful for PRINT tab-zone alignment.
int dvi_get_col(void);

// Call regularly (e.g. from an input-wait loop) to blink the text cursor.
// Cheap to call often: it only does anything once the blink interval has
// elapsed.
void dvi_cursor_tick(void);
