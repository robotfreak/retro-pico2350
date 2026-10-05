// Retro-Pico: a tiny BASIC homecomputer on the Raspberry Pi Pico 2 / Pico 2 W
// (RP2350), with DVI video via HSTX and a PS/2 keyboard (+ optional mouse).
//
// Pin plan (chosen to avoid the fixed HSTX pins 12-19 and, on Pico 2 W, the
// CYW43439 Wi-Fi/BT pins 23/24/25/29):
//   DVI (HSTX, fixed)   : GPIO 12-19  -> DVI/HDMI connector, see dvi.c
//   PS/2 keyboard       : GPIO 2 (DATA), GPIO 3 (CLOCK)
//   PS/2 mouse (optional): GPIO 4 (DATA), GPIO 5 (CLOCK)
//   Debug UART0         : GPIO 0 (TX), GPIO 1 (RX) - 115200 8N1
//
// DEBUG BUILD NOTE: while chasing the "crashes/display blanks a few seconds
// into WiFi use" issue, net_init() (which brings up the CYW43439 driver and
// starts the onboard-LED heartbeat) is called unconditionally at boot,
// instead of lazily on first WIFI command. This is deliberate: it lets us
// see, via the UART log and the LED, whether merely having the WiFi driver
// active (no network traffic yet) already causes trouble, versus only
// actually connecting. See net.c for the DBG(...) checkpoints.

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
    bool wifi_ready = net_init();
    printf("net_init done (%s)\n", wifi_ready ? "ok" : "FAILED"); stdio_flush();

    printf("basic_init...\n"); stdio_flush();
    basic_init();
    printf("entering basic_repl\n"); stdio_flush();
    basic_repl(); // never returns
    return 0;
}
