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

// UTF-8 decoder state (see feed_high()).
static uint8_t utf8_buf[4];
static uint8_t utf8_len, utf8_need;
static uint32_t utf8_cp;
// Per-session encoding detection: stays AUTO until the data proves itself.
static enum { ENC_AUTO, ENC_UTF8, ENC_CP437 } enc;
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
    utf8_len = utf8_need = 0;
    enc = ENC_AUTO;
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

// ---------------------------------------------------------------------------
// UTF-8 -> CP437

// Unicode code points of CP437 0x80..0xFF.
static const uint16_t cp437_uni[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

// Unicode code points of the CP437 glyphs at 0x01..0x1F (smileys, card suits,
// arrows, ...), index = code - 1.
static const uint16_t cp437_low_uni[31] = {
    0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, 0x25D8,
    0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C, 0x25BA,
    0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8, 0x2191,
    0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
};

// Returns the CP437 byte for a Unicode code point; typographic look-alikes
// are mapped to ASCII, anything else becomes '?'.
static uint8_t unicode_to_cp437(uint32_t cp) {
    if (cp < 0x80) return (uint8_t)cp;
    if (cp <= 0xFFFF) {
        for (int i = 0; i < 128; i++) if (cp437_uni[i] == cp) return (uint8_t)(0x80 + i);
        for (int i = 0; i < 31; i++) if (cp437_low_uni[i] == cp) return (uint8_t)(i + 1);
    }
    switch (cp) {
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2010: case 0x2013: case 0x2014: return '-';
    case 0x2026: return '.';
    default: return '?';
    }
}

// A multi-byte UTF-8 sequence in progress. If the bytes turn out not to be
// valid UTF-8 (typical for raw CP437 art), they are drawn as CP437 bytes.
static void utf8_flush_raw(void) {
    enc = ENC_CP437; // bytes that are not valid UTF-8: the remote sends raw CP437
    for (uint8_t i = 0; i < utf8_len; i++) dvi_putc((char)utf8_buf[i]);
    utf8_len = utf8_need = 0;
}

// Handles a byte >= 0x80 in normal text.
static void feed_high(uint8_t b) {
    if (enc == ENC_CP437) { dvi_putc((char)b); return; }
    if (utf8_need) {
        utf8_buf[utf8_len++] = b;
        utf8_cp = (utf8_cp << 6) | (b & 0x3F);
        if (--utf8_need == 0) {
            // A valid 3-byte sequence (e.g. a box-drawing char) is practically
            // never produced by CP437 text, so lock to UTF-8.
            if (utf8_len == 3) enc = ENC_UTF8;
            dvi_putc((char)unicode_to_cp437(utf8_cp));
            utf8_len = 0;
        }
    } else if (b >= 0xC2 && b <= 0xF4) {
        utf8_need = (b >= 0xF0) ? 3 : (b >= 0xE0) ? 2 : 1;
        utf8_cp = b & (0x3F >> utf8_need);
        utf8_buf[0] = b;
        utf8_len = 1;
    } else {
        dvi_putc((char)b); // 0x80-0xC1, 0xF5-0xFF: raw CP437
    }
}

void ansi_feed(uint8_t b) {
    switch (state) {
    case S_NORMAL:
        if (utf8_need && (b & 0xC0) != 0x80) utf8_flush_raw(); // not a continuation byte
        if (b >= 0x80) { feed_high(b); return; }
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
