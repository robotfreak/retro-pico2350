#include "ps2mouse.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/time.h"

#include "ps2.pio.h"

static PIO mouse_pio;
static uint mouse_sm;
static uint data_pin, clk_pin;

// ----------------------------------------------------------------------------
// Bit-banged host-to-device write (Request-To-Send), used only during init.
// Standard PS/2 host-to-device timing: see Adam Chapweske, "PS/2 Mouse/
// Keyboard Protocol" for the reference timing diagram this follows.

#define PS2_TIMEOUT_US 30000

static inline void clk_lo(void)  { gpio_set_dir(clk_pin, GPIO_OUT); gpio_put(clk_pin, 0); }
static inline void clk_release(void) { gpio_set_dir(clk_pin, GPIO_IN); }
static inline void data_lo(void) { gpio_set_dir(data_pin, GPIO_OUT); gpio_put(data_pin, 0); }
static inline void data_hi(void) { gpio_set_dir(data_pin, GPIO_OUT); gpio_put(data_pin, 1); }
static inline void data_release(void) { gpio_set_dir(data_pin, GPIO_IN); }

static bool wait_clk(int level) {
    absolute_time_t deadline = make_timeout_time_us(PS2_TIMEOUT_US);
    while (gpio_get(clk_pin) != level) {
        if (time_reached(deadline)) return false;
    }
    return true;
}

// Returns true on ACK, false on timeout/no device.
static bool ps2_host_write(uint8_t byte) {
    uint8_t parity = 1; // odd parity: starts at 1, XOR in each data bit
    clk_lo();
    sleep_us(100);
    data_lo();
    clk_release();

    if (!wait_clk(0)) { data_release(); return false; }

    for (int i = 0; i < 8; i++) {
        uint8_t bit = (byte >> i) & 1;
        if (bit) data_hi(); else data_lo();
        parity ^= bit;
        if (!wait_clk(1)) { data_release(); return false; }
        if (!wait_clk(0)) { data_release(); return false; }
    }

    if (parity & 1) data_hi(); else data_lo();
    if (!wait_clk(1)) { data_release(); return false; }
    if (!wait_clk(0)) { data_release(); return false; }

    data_release(); // stop bit = released/high
    if (!wait_clk(1)) return false;
    if (!wait_clk(0)) return false; // device pulls clock low for ack
    bool ack = gpio_get(data_pin) == 0;
    if (!wait_clk(1)) return false;
    return ack;
}

// ----------------------------------------------------------------------------

bool mouse_init(uint pio_num, uint data_gpio) {
    data_pin = data_gpio;
    clk_pin = data_gpio + 1;

    gpio_init(data_pin);
    gpio_init(clk_pin);
    gpio_pull_up(data_pin);
    gpio_pull_up(clk_pin);
    gpio_set_dir(data_pin, GPIO_IN);
    gpio_set_dir(clk_pin, GPIO_IN);

    // 0xF4 = Enable Data Reporting. If this fails/times out, assume no
    // mouse is connected and leave the pins idle (pulled up, harmless).
    bool ok = ps2_host_write(0xF4);

    gpio_set_dir(data_pin, GPIO_IN);
    gpio_set_dir(clk_pin, GPIO_IN);

    if (!ok) return false;

    mouse_pio = pio_num ? pio1 : pio0;
    mouse_sm = pio_claim_unused_sm(mouse_pio, true);
    uint offset = pio_add_program(mouse_pio, &ps2_rx_program);
    pio_sm_set_consecutive_pindirs(mouse_pio, mouse_sm, data_pin, 2, false);
    ps2_rx_program_init(mouse_pio, mouse_sm, offset, data_pin);
    return true;
}

static uint8_t packet[3];
static int packet_idx = 0;
static bool have_event = false;
static int ev_dx, ev_dy;
static uint8_t ev_buttons;

void mouse_poll(void) {
    while (!pio_sm_is_rx_fifo_empty(mouse_pio, mouse_sm)) {
        uint8_t b = (uint8_t)(pio_sm_get(mouse_pio, mouse_sm) >> 24);

        // Resync: a valid first byte always has bit 3 set (always-1 bit).
        if (packet_idx == 0 && !(b & 0x08)) continue;

        packet[packet_idx++] = b;
        if (packet_idx == 3) {
            packet_idx = 0;
            uint8_t status = packet[0];
            int dx = packet[1];
            int dy = packet[2];
            if (status & 0x10) dx -= 256; // X sign bit
            if (status & 0x20) dy -= 256; // Y sign bit
            if (status & 0xC0) continue;  // overflow: discard packet
            ev_dx = dx;
            ev_dy = dy;
            ev_buttons = status & 0x07;
            have_event = true;
        }
    }
}

bool mouse_get_event(int *dx, int *dy, uint8_t *buttons) {
    mouse_poll();
    if (!have_event) return false;
    have_event = false;
    *dx = ev_dx;
    *dy = ev_dy;
    *buttons = ev_buttons;
    return true;
}
