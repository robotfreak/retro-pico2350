// DVI text console output using the HSTX command expander/TMDS encoder on
// RP2350 (Pico 2 / Pico 2 W).
//
// The HSTX setup, TMDS sync symbols and scanline DMA/IRQ chaining below are
// taken directly from the official Raspberry Pi example
// (pico-examples/hstx/dvi_out_hstx_encoder), which is the verified-correct
// way to drive HSTX for 640x480 DVI. On top of that we render a simple
// 40x30 character text console instead of a static image.
//
// Wiring: GPIO 12-19 (HSTX-capable pins) go to a DVI/HDMI connector through
// current-limiting resistors (e.g. 270 ohm per pin), matching the Pico-DVI-Sock
// pinout:
//   GP12 D0+  GP13 D0-
//   GP14 CK+  GP15 CK-
//   GP16 D2+  GP17 D2-
//   GP18 D1+  GP19 D1-

#include <stdbool.h>
#include <string.h>

#include "dvi.h"
#include "font8x8_cp437.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/platform.h"
#include "pico/time.h"

// ----------------------------------------------------------------------------
// DVI timing constants (640x480 @ 60 Hz, from the official HSTX example)

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480

#define MODE_H_TOTAL_PIXELS ( \
    MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + \
    MODE_H_BACK_PORCH  + MODE_H_ACTIVE_PIXELS \
)
#define MODE_V_TOTAL_LINES  ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES \
)

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// Framebuffer (RGB332, one byte per pixel)

static uint8_t framebuf[MODE_V_ACTIVE_LINES][MODE_H_ACTIVE_PIXELS];

// ----------------------------------------------------------------------------
// HSTX command lists (padded with NOPs to be >= HSTX FIFO size)

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP
};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP
};

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS       | MODE_H_ACTIVE_PIXELS
};

// ----------------------------------------------------------------------------
// DMA scanline chaining (ping/pong)
//
// The two channels strictly alternate in hardware (each completion
// auto-chain-triggers the other), so counting invocations and toggling
// `dma_pong` each time is correct *as long as we get exactly one ISR
// invocation per real completion* - which holds even if both channels
// happen to finish close together (e.g. the short 9-word vactive_line
// command-list transfer completes almost immediately, often before the
// CPU has serviced the previous completion): the DMA_IRQ_0 line simply
// stays pending and the NVIC re-enters this handler again right away for
// the second one, so invocation count still matches completion count and
// the blind alternation stays correct. (An earlier attempt to instead pick
// the channel from DMA_INTS0 directly broke exactly this common case, by
// preferring channel 0 whenever its bit happened to also be set, which can
// silently swap the chronological order of the two completions without
// ever producing an invalid address - no bus fault, just scrambled
// scanline timing that a monitor can't lock onto.)
//
// What *can* actually desync the alternation is a much longer stall - long
// enough that one channel completes a second time before we have serviced
// its first completion at all (observed in practice under heavy concurrent
// bus traffic from the CYW43439 WiFi chip's own DMA during
// cyw43_arch_wifi_connect_timeout_ms()). That's the case this handler now
// guards against: if our `dma_pong` assumption doesn't match which channel
// hardware actually says is pending, trust hardware and resynchronize,
// instead of ploughing ahead with a stale assumption until `active_line`
// eventually goes out of range and faults (confirmed via SWD: ctrl_trig
// had AHB_ERROR+READ_ERROR set, read_addr sitting exactly at SRAM_END).

#define DMACH_PING 0
#define DMACH_PONG 1
#define DMACH_BOTH_MASK ((1u << DMACH_PING) | (1u << DMACH_PONG))

static bool dma_pong = false;
static uint v_scanline = 2;
static bool vactive_cmdlist_posted = false;

static void __not_in_flash_func(configure_channel_for_scanline)(dma_channel_hw_t *ch) {
    if (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        uint active_line = v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
        if (active_line >= MODE_V_ACTIVE_LINES) active_line = MODE_V_ACTIVE_LINES - 1; // defensive clamp
        ch->read_addr = (uintptr_t)&framebuf[active_line][0];
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

static void __not_in_flash_func(dma_irq_handler)(void) {
    // Normal case: trust the alternation, exactly like the original
    // handler - this is what correctly handles the common "both channels
    // finish close together" situation (see block comment above).
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    uint32_t pending = dma_hw->ints0 & DMACH_BOTH_MASK;

    // Resync only if reality disagrees with our assumption (the rare
    // missed-a-full-round case under heavy concurrent bus traffic).
    if (!(pending & (1u << ch_num))) {
        if (pending & (1u << DMACH_PING)) ch_num = DMACH_PING;
        else if (pending & (1u << DMACH_PONG)) ch_num = DMACH_PONG;
        else return; // nothing pending at all; spurious entry
    }

    dma_hw->ints0 = 1u << ch_num; // clear
    dma_pong = (ch_num == DMACH_PING); // next expected channel is the other one
    configure_channel_for_scanline(&dma_hw->ch[ch_num]);
}

// ----------------------------------------------------------------------------
// Console state

#define CHAR_CELL_W 8
#define CHAR_CELL_H 16

static int cursor_col = 0;
static int cursor_row = 0;
static uint8_t cur_fg = 0;
static uint8_t cur_bg = 0;

// Blinking cursor. The cursor cell is always the "not yet written" position
// right after the last character, so it's safe to just paint the whole cell
// solid (on) or background (off) - there is never real content under it.
#define CURSOR_BLINK_MS 500
static bool cursor_shown = false;    // true if a solid block is currently drawn
static absolute_time_t next_blink;

uint8_t dvi_rgb332(uint8_t r, uint8_t g, uint8_t b) {
    return (r & 0xc0) >> 6 | (g & 0xe0) >> 3 | (b & 0xe0) >> 0;
}

void dvi_set_colors(uint8_t fg, uint8_t bg) {
    cur_fg = fg;
    cur_bg = bg;
}

// Text shadow of the screen. It lets the cursor block be drawn/removed without
// destroying the glyph underneath, and lets line operations (insert/delete,
// partial erase) work on cells while the framebuffer stays the DMA source.
static uint8_t sh_ch[DVI_ROWS][DVI_COLS];
static uint8_t sh_fg[DVI_ROWS][DVI_COLS];
static uint8_t sh_bg[DVI_ROWS][DVI_COLS];

// Deferred line wrap: after writing the last column the cursor stays there
// and the wrap happens before the next printable character (like a real
// terminal), so an 80-char line followed by '\n' gives one line break.
static bool wrap_pending = false;

// Paints a glyph into the framebuffer only (no shadow update).
static void paint_cell(int col, int row, uint8_t c, uint8_t fg, uint8_t bg) {
    const uint8_t *glyph = font8x8_cp437[c];
    int x0 = col * CHAR_CELL_W;
    int y0 = row * CHAR_CELL_H;
    // 8x8 glyph, drawn 1x horizontally and 2x vertically (8x16 cell).
    for (int gy = 0; gy < 8; gy++) {
        uint8_t bits = glyph[gy];
        uint8_t *dst0 = &framebuf[y0 + gy * 2][x0];
        uint8_t *dst1 = &framebuf[y0 + gy * 2 + 1][x0];
        for (int gx = 0; gx < 8; gx++) {
            uint8_t px = (bits & (1u << gx)) ? fg : bg;
            dst0[gx] = px;
            dst1[gx] = px;
        }
    }
}

static void draw_char_at(int col, int row, char c, uint8_t fg, uint8_t bg) {
    sh_ch[row][col] = (uint8_t)c;
    sh_fg[row][col] = fg;
    sh_bg[row][col] = bg;
    paint_cell(col, row, (uint8_t)c, fg, bg);
}

// Cursor on = the cell's glyph with foreground/background swapped.
static void draw_cursor_cell(bool on) {
    uint8_t ch = sh_ch[cursor_row][cursor_col];
    uint8_t fg = sh_fg[cursor_row][cursor_col];
    uint8_t bg = sh_bg[cursor_row][cursor_col];
    if (on) paint_cell(cursor_col, cursor_row, ch, bg, fg);
    else    paint_cell(cursor_col, cursor_row, ch, fg, bg);
}

// Retracts the cursor block (if currently drawn) before the cursor moves or
// the screen content changes. Must be called before cursor_col/cursor_row
// change or any cell is rewritten.
static void hide_cursor(void) {
    if (cursor_shown) {
        draw_cursor_cell(false);
        cursor_shown = false;
    }
}

static void shadow_fill(void) {
    memset(sh_ch, ' ', sizeof(sh_ch));
    memset(sh_fg, cur_fg, sizeof(sh_fg));
    memset(sh_bg, cur_bg, sizeof(sh_bg));
}

// Scrolls the text rows top..bottom (inclusive) by n lines. up: content moves
// up (new blank rows appear at the bottom); otherwise it moves down.
static void region_scroll(int top, int bottom, int n, bool up) {
    int rows = bottom - top + 1;
    if (n > rows) n = rows;
    if (n <= 0) return;
    int keep = rows - n;
    int px_row = CHAR_CELL_H * MODE_H_ACTIVE_PIXELS;
    int blank_first;
    if (up) {
        if (keep) {
            memmove(&framebuf[top * CHAR_CELL_H][0], &framebuf[(top + n) * CHAR_CELL_H][0], keep * px_row);
            memmove(sh_ch[top], sh_ch[top + n], keep * DVI_COLS);
            memmove(sh_fg[top], sh_fg[top + n], keep * DVI_COLS);
            memmove(sh_bg[top], sh_bg[top + n], keep * DVI_COLS);
        }
        blank_first = top + keep;
    } else {
        if (keep) {
            memmove(&framebuf[(top + n) * CHAR_CELL_H][0], &framebuf[top * CHAR_CELL_H][0], keep * px_row);
            memmove(sh_ch[top + n], sh_ch[top], keep * DVI_COLS);
            memmove(sh_fg[top + n], sh_fg[top], keep * DVI_COLS);
            memmove(sh_bg[top + n], sh_bg[top], keep * DVI_COLS);
        }
        blank_first = top;
    }
    memset(&framebuf[blank_first * CHAR_CELL_H][0], cur_bg, n * px_row);
    memset(sh_ch[blank_first], ' ', n * DVI_COLS);
    memset(sh_fg[blank_first], cur_fg, n * DVI_COLS);
    memset(sh_bg[blank_first], cur_bg, n * DVI_COLS);
}

// Blanks cells [c0, c1) of one row with the current background.
static void erase_cells(int row, int c0, int c1) {
    for (int c = c0; c < c1; c++) draw_char_at(c, row, ' ', cur_fg, cur_bg);
}

void dvi_clear(void) {
    memset(framebuf, cur_bg, sizeof(framebuf));
    shadow_fill();
    cursor_col = 0;
    cursor_row = 0;
    wrap_pending = false;
    cursor_shown = false; // already blank, nothing to retract
    next_blink = make_timeout_time_ms(CURSOR_BLINK_MS);
}

static void newline(void) {
    wrap_pending = false;
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= DVI_ROWS) {
        region_scroll(0, DVI_ROWS - 1, 1, true);
        cursor_row = DVI_ROWS - 1;
    }
}

void dvi_putc(char c) {
    hide_cursor(); // about to change cursor_col/row and/or cell content
    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        wrap_pending = false;
        cursor_col = 0;
        return;
    case '\b':
        if (wrap_pending) { // cursor is logically past the last cell: erase it
            wrap_pending = false;
            draw_char_at(cursor_col, cursor_row, ' ', cur_fg, cur_bg);
        } else if (cursor_col > 0) {
            cursor_col--;
            draw_char_at(cursor_col, cursor_row, ' ', cur_fg, cur_bg);
        }
        return;
    default:
        break;
    }
    if (wrap_pending) newline();
    draw_char_at(cursor_col, cursor_row, c, cur_fg, cur_bg);
    if (cursor_col >= DVI_COLS - 1) wrap_pending = true;
    else cursor_col++;
}

void dvi_puts(const char *s) {
    while (*s) dvi_putc(*s++);
}

int dvi_get_col(void) {
    return cursor_col + (wrap_pending ? 1 : 0);
}

int dvi_get_row(void) {
    return cursor_row;
}

void dvi_goto(int col, int row) {
    hide_cursor();
    if (col < 0) col = 0;
    if (col >= DVI_COLS) col = DVI_COLS - 1;
    if (row < 0) row = 0;
    if (row >= DVI_ROWS) row = DVI_ROWS - 1;
    cursor_col = col;
    cursor_row = row;
    wrap_pending = false;
}

void dvi_get_colors(uint8_t *fg, uint8_t *bg) {
    *fg = cur_fg;
    *bg = cur_bg;
}

void dvi_erase_display(int mode) {
    hide_cursor();
    if (mode == 2 || mode == 3) {
        memset(framebuf, cur_bg, sizeof(framebuf));
        shadow_fill();
    } else if (mode == 0) {
        erase_cells(cursor_row, cursor_col, DVI_COLS);
        for (int r = cursor_row + 1; r < DVI_ROWS; r++) erase_cells(r, 0, DVI_COLS);
    } else {
        for (int r = 0; r < cursor_row; r++) erase_cells(r, 0, DVI_COLS);
        erase_cells(cursor_row, 0, cursor_col + 1);
    }
}

void dvi_erase_line(int mode) {
    hide_cursor();
    if (mode == 0)      erase_cells(cursor_row, cursor_col, DVI_COLS);
    else if (mode == 1) erase_cells(cursor_row, 0, cursor_col + 1);
    else                erase_cells(cursor_row, 0, DVI_COLS);
}

void dvi_erase_chars(int n) {
    hide_cursor();
    int end = cursor_col + n;
    erase_cells(cursor_row, cursor_col, end > DVI_COLS ? DVI_COLS : end);
}

void dvi_insert_lines(int n) {
    hide_cursor();
    region_scroll(cursor_row, DVI_ROWS - 1, n, false);
    cursor_col = 0;
    wrap_pending = false;
}

void dvi_delete_lines(int n) {
    hide_cursor();
    region_scroll(cursor_row, DVI_ROWS - 1, n, true);
    cursor_col = 0;
    wrap_pending = false;
}

void dvi_cursor_tick(void) {
    if (!time_reached(next_blink)) return;
    cursor_shown = !cursor_shown;
    draw_cursor_cell(cursor_shown);
    next_blink = make_timeout_time_ms(CURSOR_BLINK_MS);
}

// ----------------------------------------------------------------------------
// HSTX / DMA bring-up

void dvi_init(void) {
    cur_fg = dvi_rgb332(0, 255, 0);   // classic green phosphor
    cur_bg = dvi_rgb332(0, 0, 0);
    dvi_clear();

    // Configure HSTX's TMDS encoder for RGB332
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // Pixels (TMDS) come in 4 8-bit chunks. Control symbols (RAW) are an
    // entire 32-bit word.
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Serial output config: clock period of 5 cycles, pop from command
    // expander every 5 cycles, shift the output shiftreg by 2 every cycle.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // HSTX outputs 0 through 7 appear on GPIO 12 through 19 (Pico-DVI-Sock pinout).
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        static const int lane_to_output_bit[3] = {0, 6, 4};
        int bit = lane_to_output_bit[lane];
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, 0); // HSTX
    }

    // Claim channels 0/1 explicitly (even though we address them by fixed
    // number below) so the SDK's allocator won't later hand them to another
    // driver that claims channels dynamically - notably cyw43/WiFi on
    // Pico 2 W, which calls dma_claim_unused_channel() when a WIFI command
    // first brings up the radio.
    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);

    dma_channel_config c;
    c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PING, &c, &hstx_fifo_hw->fifo,
        vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PONG, &c, &hstx_fifo_hw->fifo,
        vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    // Belt-and-braces: make sure nothing (e.g. the cyw43/lwIP background
    // work IRQ, which the SDK already defaults to the lowest priority) can
    // ever delay refilling the HSTX FIFO. Scanline timing is tight (~32us
    // at 480p60) so this IRQ must always win if two happen to be pending
    // at once.
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(DMACH_PING);
}
