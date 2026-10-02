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

#include <string.h>

#include "dvi.h"
#include "font8x8_basic.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/platform.h"

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
// DMA scanline chaining (ping/pong), identical logic to the official example

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static uint v_scanline = 2;
static bool vactive_cmdlist_posted = false;

static void __not_in_flash_func(dma_irq_handler)(void) {
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

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
        ch->read_addr = (uintptr_t)&framebuf[active_line][0];
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

// ----------------------------------------------------------------------------
// Console state

#define CHAR_CELL_W 16
#define CHAR_CELL_H 16

static int cursor_col = 0;
static int cursor_row = 0;
static uint8_t cur_fg = 0;
static uint8_t cur_bg = 0;

uint8_t dvi_rgb332(uint8_t r, uint8_t g, uint8_t b) {
    return (r & 0xc0) >> 6 | (g & 0xe0) >> 3 | (b & 0xe0) >> 0;
}

void dvi_set_colors(uint8_t fg, uint8_t bg) {
    cur_fg = fg;
    cur_bg = bg;
}

static void draw_char_at(int col, int row, char c, uint8_t fg, uint8_t bg) {
    if ((unsigned char)c > 0x7f) c = '?';
    const uint8_t *glyph = font8x8_basic[(unsigned char)c];
    int x0 = col * CHAR_CELL_W;
    int y0 = row * CHAR_CELL_H;
    for (int gy = 0; gy < 8; gy++) {
        uint8_t bits = glyph[gy];
        uint8_t row_px[8];
        for (int gx = 0; gx < 8; gx++) {
            row_px[gx] = (bits & (1u << gx)) ? fg : bg;
        }
        uint8_t *dst0 = &framebuf[y0 + gy * 2][x0];
        uint8_t *dst1 = &framebuf[y0 + gy * 2 + 1][x0];
        for (int gx = 0; gx < 8; gx++) {
            dst0[gx * 2]     = row_px[gx];
            dst0[gx * 2 + 1] = row_px[gx];
            dst1[gx * 2]     = row_px[gx];
            dst1[gx * 2 + 1] = row_px[gx];
        }
    }
}

static void scroll_console(void) {
    memmove(&framebuf[0][0], &framebuf[CHAR_CELL_H][0],
            (MODE_V_ACTIVE_LINES - CHAR_CELL_H) * MODE_H_ACTIVE_PIXELS);
    memset(&framebuf[MODE_V_ACTIVE_LINES - CHAR_CELL_H][0], cur_bg,
           CHAR_CELL_H * MODE_H_ACTIVE_PIXELS);
}

void dvi_clear(void) {
    memset(framebuf, cur_bg, sizeof(framebuf));
    cursor_col = 0;
    cursor_row = 0;
}

static void newline(void) {
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= DVI_ROWS) {
        scroll_console();
        cursor_row = DVI_ROWS - 1;
    }
}

void dvi_putc(char c) {
    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        cursor_col = 0;
        return;
    case '\b':
        if (cursor_col > 0) {
            cursor_col--;
            draw_char_at(cursor_col, cursor_row, ' ', cur_fg, cur_bg);
        }
        return;
    default:
        break;
    }
    draw_char_at(cursor_col, cursor_row, c, cur_fg, cur_bg);
    cursor_col++;
    if (cursor_col >= DVI_COLS) {
        newline();
    }
}

void dvi_puts(const char *s) {
    while (*s) dvi_putc(*s++);
}

int dvi_get_col(void) {
    return cursor_col;
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
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(DMACH_PING);
}
