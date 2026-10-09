#include "ansi.h"
#include "dvi.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAX_PARAMS 8

typedef enum { S_NORMAL, S_ESC, S_CSI, S_STR, S_STR_ESC } State;

static State state;
static ansi_reply_fn reply_fn;

static int params[MAX_PARAMS];
static int nparams;
static bool have_digit;
static bool csi_private; // saw '?', '<', '=' or '>' before the parameters

static uint8_t fg_idx, bg_idx;   // 0-7
static bool bold, fg_bright, reverse;
static int saved_col, saved_row;
static uint8_t entry_fg, entry_bg;

static const struct { uint8_t r, g, b; } palette[16] = {
    {0, 0, 0},       {170, 0, 0},     {0, 170, 0},     {170, 85, 0},
    {0, 0, 170},     {170, 0, 170},   {0, 170, 170},   {170, 170, 170},
    {85, 85, 85},    {255, 85, 85},   {85, 255, 85},   {255, 255, 85},
    {85, 85, 255},   {255, 85, 255},  {85, 255, 255},  {255, 255, 255},
};

static uint8_t pal(int i) {
    return dvi_rgb332(palette[i].r, palette[i].g, palette[i].b);
}

static void apply_colors(void) {
    int f = fg_idx + ((bold || fg_bright) ? 8 : 0);
    int b = bg_idx;
    if (reverse) dvi_set_colors(pal(b), pal(f));
    else         dvi_set_colors(pal(f), pal(b));
}

static void reset_attrs(void) {
    fg_idx = 7; bg_idx = 0;
    bold = fg_bright = reverse = false;
    apply_colors();
}

void ansi_begin(ansi_reply_fn reply) {
    reply_fn = reply;
    state = S_NORMAL;
    saved_col = saved_row = 0;
    dvi_get_colors(&entry_fg, &entry_bg);
    reset_attrs();
}

void ansi_end(void) {
    dvi_set_colors(entry_fg, entry_bg);
}

// Parameter n with a default for "absent or 0".
static int param(int n, int def) {
    if (n >= nparams || params[n] == 0) return def;
    return params[n];
}

static void do_sgr(void) {
    if (nparams == 0) { params[0] = 0; nparams = 1; }
    for (int i = 0; i < nparams; i++) {
        int v = params[i];
        if (v == 0) reset_attrs();
        else if (v == 1) bold = true;
        else if (v == 22) bold = false;
        else if (v == 7) reverse = true;
        else if (v == 27) reverse = false;
        else if (v >= 30 && v <= 37) { fg_idx = (uint8_t)(v - 30); fg_bright = false; }
        else if (v == 39) { fg_idx = 7; fg_bright = false; }
        else if (v >= 40 && v <= 47) bg_idx = (uint8_t)(v - 40);
        else if (v == 49) bg_idx = 0;
        else if (v >= 90 && v <= 97) { fg_idx = (uint8_t)(v - 90); fg_bright = true; }
        else if (v >= 100 && v <= 107) bg_idx = (uint8_t)(v - 100);
        else if ((v == 38 || v == 48) && i + 1 < nparams) {
            // Extended colours (38;5;n / 38;2;r;g;b) are not supported: skip them.
            i += (params[i + 1] == 5) ? 2 : (params[i + 1] == 2) ? 4 : 1;
        }
    }
    apply_colors();
}

static void do_csi(uint8_t final) {
    if (csi_private) return; // mode set/reset etc.: ignored
    int col = dvi_get_col(), row = dvi_get_row();
    if (col >= DVI_COLS) col = DVI_COLS - 1;
    switch (final) {
    case 'A': dvi_goto(col, row - param(0, 1)); break;
    case 'B': dvi_goto(col, row + param(0, 1)); break;
    case 'C': dvi_goto(col + param(0, 1), row); break;
    case 'D': dvi_goto(col - param(0, 1), row); break;
    case 'E': dvi_goto(0, row + param(0, 1)); break;
    case 'F': dvi_goto(0, row - param(0, 1)); break;
    case 'G': dvi_goto(param(0, 1) - 1, row); break;
    case 'd': dvi_goto(col, param(0, 1) - 1); break;
    case 'H':
    case 'f': dvi_goto(param(1, 1) - 1, param(0, 1) - 1); break;
    case 'J': dvi_erase_display(nparams ? params[0] : 0); break;
    case 'K': dvi_erase_line(nparams ? params[0] : 0); break;
    case 'X': dvi_erase_chars(param(0, 1)); break;
    case 'L': dvi_insert_lines(param(0, 1)); break;
    case 'M': dvi_delete_lines(param(0, 1)); break;
    case 'm': do_sgr(); break;
    case 's': saved_col = col; saved_row = row; break;
    case 'u': dvi_goto(saved_col, saved_row); break;
    case 'n':
        if (nparams && params[0] == 6 && reply_fn) {
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "\033[%d;%dR", row + 1, col + 1);
            reply_fn((const uint8_t *)buf, (uint16_t)n);
        }
        break;
    default: break;
    }
}

// CP437 -> ASCII look-alikes for the glyphs BBS screens use most.
static char cp437_to_ascii(uint8_t b) {
    switch (b) {
    case 0xB0: return '.';
    case 0xB1: return ':';
    case 0xB2: case 0xDB: return '#';
    case 0xDC: return '_';
    case 0xDF: return '^';
    case 0xDD: return '[';
    case 0xDE: return ']';
    case 0xB3: case 0xBA: return '|';
    case 0xC4: case 0xCD: return '-';
    case 0xFE: return '*';
    default:
        if (b >= 0xB4 && b <= 0xDA) return '+'; // other box-drawing pieces
        return '?';
    }
}

void ansi_feed(uint8_t b) {
    switch (state) {
    case S_NORMAL:
        if (b == 0x1B) { state = S_ESC; return; }
        if (b == '\r') { dvi_putc('\r'); return; }
        if (b == '\n') { dvi_putc('\n'); return; }
        if (b == '\b') { // pure cursor move, like a real terminal
            int c = dvi_get_col();
            dvi_goto(c - 1, dvi_get_row());
            return;
        }
        if (b == '\t') {
            int c = dvi_get_col();
            dvi_goto(((c / 8) + 1) * 8, dvi_get_row());
            return;
        }
        if (b >= 32 && b < 127) { dvi_putc((char)b); return; }
        if (b >= 0x80) { dvi_putc(cp437_to_ascii(b)); return; }
        return; // other C0 controls (BEL, ...) ignored
    case S_ESC:
        state = S_NORMAL;
        switch (b) {
        case '[':
            state = S_CSI;
            nparams = 0; have_digit = false; csi_private = false;
            memset(params, 0, sizeof(params));
            break;
        case ']': case 'P': case '^': case '_': state = S_STR; break;
        case '7': saved_col = dvi_get_col(); saved_row = dvi_get_row(); break;
        case '8': dvi_goto(saved_col, saved_row); break;
        case 'D': { // index: down one line, scrolling at the bottom
            int r = dvi_get_row();
            if (r == DVI_ROWS - 1) { int c = dvi_get_col(); dvi_goto(0, r); dvi_delete_lines(1); dvi_goto(c, r); }
            else dvi_goto(dvi_get_col(), r + 1);
            break;
        }
        case 'E': dvi_putc('\n'); break;
        case 'M': { // reverse index: up one line, scrolling at the top
            int r = dvi_get_row();
            if (r == 0) { int c = dvi_get_col(); dvi_insert_lines(1); dvi_goto(c, 0); }
            else dvi_goto(dvi_get_col(), r - 1);
            break;
        }
        case 'c': reset_attrs(); dvi_goto(0, 0); dvi_erase_display(2); break;
        default: break;
        }
        return;
    case S_CSI:
        if (b >= '0' && b <= '9') {
            if (nparams == 0) nparams = 1;
            if (!have_digit) have_digit = true;
            int *p = &params[nparams - 1];
            if (*p < 10000) *p = *p * 10 + (b - '0');
        } else if (b == ';') {
            if (nparams == 0) nparams = 1;
            if (nparams < MAX_PARAMS) nparams++;
            have_digit = false;
        } else if (b >= '<' && b <= '?') {
            csi_private = true;
        } else if (b >= 0x40 && b <= 0x7E) {
            do_csi(b);
            state = S_NORMAL;
        } else if (b == 0x1B) {
            state = S_ESC; // aborted sequence
        } // other bytes (intermediates, stray controls): ignore
        return;
    case S_STR: // OSC / DCS / ...: swallow until BEL or ST
        if (b == 0x07) state = S_NORMAL;
        else if (b == 0x1B) state = S_STR_ESC;
        return;
    case S_STR_ESC:
        state = (b == '\\') ? S_NORMAL : S_STR;
        return;
    }
}
