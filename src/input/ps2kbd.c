#include "ps2kbd.h"
#include "dvi.h"
#include "net.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "ps2.pio.h"

static PIO kbd_pio;
static uint kbd_sm;

void kbd_init(uint pio_num, uint data_gpio) {
    kbd_pio = pio_num ? pio1 : pio0;

    gpio_init(data_gpio);
    gpio_init(data_gpio + 1);
    gpio_pull_up(data_gpio);
    gpio_pull_up(data_gpio + 1);

    kbd_sm = pio_claim_unused_sm(kbd_pio, true);
    uint offset = pio_add_program(kbd_pio, &ps2_rx_program);
    pio_sm_set_consecutive_pindirs(kbd_pio, kbd_sm, data_gpio, 2, false);
    ps2_rx_program_init(kbd_pio, kbd_sm, offset, data_gpio);
}

// Scan Code Set 2, make-code -> ASCII (US QWERTY). 0 = unmapped/ignored.
#define K_BS  '\b'
#define K_TAB '\t'
#define K_ENT '\n'
#define K_ESC 27

static const char lower_tbl[0x84] = {
    [0x1c]='a', [0x32]='b', [0x21]='c', [0x23]='d', [0x24]='e', [0x2b]='f',
    [0x34]='g', [0x33]='h', [0x43]='i', [0x3b]='j', [0x42]='k', [0x4b]='l',
    [0x3a]='m', [0x31]='n', [0x44]='o', [0x4d]='p', [0x15]='q', [0x2d]='r',
    [0x1b]='s', [0x2c]='t', [0x3c]='u', [0x2a]='v', [0x1d]='w', [0x22]='x',
    [0x35]='y', [0x1a]='z',
    [0x45]='0', [0x16]='1', [0x1e]='2', [0x26]='3', [0x25]='4',
    [0x2e]='5', [0x36]='6', [0x3d]='7', [0x3e]='8', [0x46]='9',
    [0x0e]='`', [0x4e]='-', [0x55]='=', [0x5d]='\\',
    [0x54]='[', [0x5b]=']', [0x4c]=';', [0x52]='\'',
    [0x41]=',', [0x49]='.', [0x4a]='/',
    [0x29]=' ', [0x66]=K_BS, [0x0d]=K_TAB, [0x5a]=K_ENT, [0x76]=K_ESC,
};

static const char upper_tbl[0x84] = {
    [0x1c]='A', [0x32]='B', [0x21]='C', [0x23]='D', [0x24]='E', [0x2b]='F',
    [0x34]='G', [0x33]='H', [0x43]='I', [0x3b]='J', [0x42]='K', [0x4b]='L',
    [0x3a]='M', [0x31]='N', [0x44]='O', [0x4d]='P', [0x15]='Q', [0x2d]='R',
    [0x1b]='S', [0x2c]='T', [0x3c]='U', [0x2a]='V', [0x1d]='W', [0x22]='X',
    [0x35]='Y', [0x1a]='Z',
    [0x45]=')', [0x16]='!', [0x1e]='@', [0x26]='#', [0x25]='$',
    [0x2e]='%', [0x36]='^', [0x3d]='&', [0x3e]='*', [0x46]='(',
    [0x0e]='~', [0x4e]='_', [0x55]='+', [0x5d]='|',
    [0x54]='{', [0x5b]='}', [0x4c]=':', [0x52]='"',
    [0x41]='<', [0x49]='>', [0x4a]='?',
    [0x29]=' ', [0x66]=K_BS, [0x0d]=K_TAB, [0x5a]=K_ENT, [0x76]=K_ESC,
};

#define SC_LSHIFT 0x12
#define SC_RSHIFT 0x59
#define SC_LCTRL  0x14
#define SC_CAPS   0x58
#define SC_EXT    0xE0
#define SC_RELEASE 0xF0

static bool shift = false;
static bool ctrl = false;
static bool capslock = false;
static bool extended = false;
static bool release = false;

#define KBUF_SIZE 64
static char kbuf[KBUF_SIZE];
static volatile uint8_t kbuf_head = 0, kbuf_tail = 0;

static void kbuf_push(char c) {
    uint8_t next = (kbuf_head + 1) % KBUF_SIZE;
    if (next != kbuf_tail) { // drop on overflow
        kbuf[kbuf_head] = c;
        kbuf_head = next;
    }
}

void kbd_poll(void) {
    while (!pio_sm_is_rx_fifo_empty(kbd_pio, kbd_sm)) {
        uint8_t code = (uint8_t)(pio_sm_get(kbd_pio, kbd_sm) >> 24);

        if (code == SC_EXT) {
            extended = true;
            continue;
        }
        if (code == SC_RELEASE) {
            release = true;
            continue;
        }
        if (extended) {
            // Ignore extended keys (arrows, numpad enter, right ctrl/alt, ...)
            extended = false;
            release = false;
            continue;
        }
        if (release) {
            release = false;
            if (code == SC_LSHIFT || code == SC_RSHIFT) shift = false;
            else if (code == SC_LCTRL) ctrl = false;
            continue;
        }

        switch (code) {
        case SC_LSHIFT:
        case SC_RSHIFT:
            shift = true;
            break;
        case SC_LCTRL:
            ctrl = true;
            break;
        case SC_CAPS:
            capslock = !capslock;
            break;
        default: {
            if (code >= sizeof(lower_tbl)) break;
            char lo = lower_tbl[code];
            char up = upper_tbl[code];
            if (!lo && !up) break;
            bool is_letter = lo >= 'a' && lo <= 'z';
            bool use_upper = is_letter ? (shift != capslock) : shift;
            char c = use_upper ? up : lo;
            if (ctrl && is_letter) {
                c = (char)(use_upper ? (up - 'A' + 1) : (lo - 'a' + 1));
            }
            kbuf_push(c);
            break;
        }
        }
    }
}

int kbd_getc_nonblock(void) {
    kbd_poll();
    if (kbuf_head == kbuf_tail) return -1;
    char c = kbuf[kbuf_tail];
    kbuf_tail = (kbuf_tail + 1) % KBUF_SIZE;
    return (unsigned char)c;
}

char kbd_getc_blocking(void) {
    int c;
    while ((c = kbd_getc_nonblock()) < 0) {
        dvi_cursor_tick();
        net_heartbeat_tick();
        tight_loop_contents();
    }
    return (char)c;
}
