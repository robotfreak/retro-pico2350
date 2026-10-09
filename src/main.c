// Retro-Pico: a tiny BASIC homecomputer on the Raspberry Pi Pico 2 / Pico 2 W
// (RP2350), with DVI video via HSTX and a PS/2 keyboard (+ optional mouse).
//
// Pin plan (chosen to avoid the fixed HSTX pins 12-19 and, on Pico 2 W, the
// SD card pins):
//   DVI (HSTX, fixed)   : GPIO 12-19  -> DVI/HDMI connector, see dvi.c
//   PS/2 keyboard       : GPIO 2 (DATA), GPIO 3 (CLOCK)
//   PS/2 mouse (optional): GPIO 4 (DATA), GPIO 5 (CLOCK)
//   Debug UART0         : GPIO 0 (TX), GPIO 1 (RX) - 115200 8N1
//   Backplane link UART1: GPIO 20 (TX), GPIO 21 (RX) - to the Pico 2 W
//                         running backplane/ (WiFi + TCP/IP), see net.c
//

#include "pico/stdlib.h"
#include <stdio.h>

#include "dvi.h"
#include "ps2kbd.h"
#include "ps2mouse.h"
#include "basic.h"
#include "net.h"

#define KBD_PIO     0
#define KBD_DATA_GPIO 2

#define MOUSE_PIO   1
#define MOUSE_DATA_GPIO 4

int main(void) {
    stdio_init_all();
    sleep_ms(1500); // give a USB-serial adapter time to enumerate/attach
    printf("\n\n=== RETRO-PICO BOOT ===\n");
    stdio_flush();

    printf("dvi_init...\n"); stdio_flush();
    dvi_init();
    printf("dvi_init done\n"); stdio_flush();

    printf("kbd_init...\n"); stdio_flush();
    kbd_init(KBD_PIO, KBD_DATA_GPIO);
    printf("kbd_init done\n"); stdio_flush();

    printf("mouse_init...\n"); stdio_flush();
    bool has_mouse = mouse_init(MOUSE_PIO, MOUSE_DATA_GPIO); // ok if no mouse is connected
    printf("mouse_init done (mouse %s)\n", has_mouse ? "found" : "not found"); stdio_flush();

    printf("net_init...\n"); stdio_flush();
    bool wifi_ready = net_init(); // talks to the backplane controller
    printf("net_init done (backplane %s)\n", wifi_ready ? "ok" : "NOT FOUND"); stdio_flush();

    printf("basic_init...\n"); stdio_flush();
    basic_init();
    printf("entering basic_repl\n"); stdio_flush();
    basic_repl(); // never returns
    return 0;
}
