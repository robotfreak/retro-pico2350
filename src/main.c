// Retro-Pico: a tiny BASIC homecomputer on the Raspberry Pi Pico 2 / Pico 2 W
// (RP2350), with DVI video via HSTX and a PS/2 keyboard (+ optional mouse).
//
// Pin plan (chosen to avoid the fixed HSTX pins 12-19 and, on Pico 2 W, the
// CYW43439 Wi-Fi/BT pins 23/24/25/29):
//   DVI (HSTX, fixed)   : GPIO 12-19  -> DVI/HDMI connector, see dvi.c
//   PS/2 keyboard       : GPIO 2 (DATA), GPIO 3 (CLOCK)
//   PS/2 mouse (optional): GPIO 4 (DATA), GPIO 5 (CLOCK)
//   Debug UART0         : GPIO 0 (TX), GPIO 1 (RX) - 115200 8N1

#include "pico/stdlib.h"

#include "dvi.h"
#include "ps2kbd.h"
#include "ps2mouse.h"
#include "basic.h"

#define KBD_PIO     0
#define KBD_DATA_GPIO 2

#define MOUSE_PIO   1
#define MOUSE_DATA_GPIO 4

int main(void) {
    stdio_init_all();

    dvi_init();
    kbd_init(KBD_PIO, KBD_DATA_GPIO);
    mouse_init(MOUSE_PIO, MOUSE_DATA_GPIO); // ok to ignore return value: no mouse is fine

    basic_init();
    basic_repl(); // never returns
    return 0;
}
