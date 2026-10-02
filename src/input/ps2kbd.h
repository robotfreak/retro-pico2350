#pragma once

// PS/2 keyboard driver (Scan Code Set 2) built on a PIO receiver.
//
// Wiring: data_gpio = KBD DATA pin, data_gpio+1 = KBD CLOCK pin.
// Both lines need the PS/2 device's own pull-ups (standard on every PS/2
// keyboard); add 10k pull-ups to 5V/3.3V-tolerant level shifting if your
// keyboard is 5V (recommended: run PS/2 at 3.3V via diode/resistor level
// shifting, or a dedicated level shifter, since the Pico is not 5V tolerant).

#include <stdint.h>
#include "pico/types.h" // for the "uint" typedef used below

// pio: 0 or 1 (which PIO block to use)
// data_gpio: GPIO number of the KBD DATA line; CLOCK must be data_gpio+1
void kbd_init(uint pio, uint data_gpio);

// Must be called regularly (e.g. once per main loop iteration) to drain
// the PIO FIFO and decode scan codes into ASCII.
void kbd_poll(void);

// Returns the next available ASCII character, or -1 if none is ready.
int kbd_getc_nonblock(void);

// Blocks (polling) until a character is available.
char kbd_getc_blocking(void);
