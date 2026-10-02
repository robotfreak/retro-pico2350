#pragma once

#include <stdbool.h>
#include <stdint.h>

// Optional PS/2 mouse driver. Host-initiates "enable data reporting" over
// bit-banged GPIO (required so the mouse actually streams movement data),
// then switches to PIO for ongoing packet reception.
//
// Wiring: data_gpio = MOUSE DATA pin, data_gpio+1 = MOUSE CLOCK pin.
// Must be on a different PIO state machine / GPIO pair than the keyboard.

// Returns true if a mouse responded to the enable command within the
// timeout, false if no mouse is connected (keyboard-only setups are fine).
bool mouse_init(uint pio, uint data_gpio);

// Must be called regularly to drain the PIO FIFO and assemble packets.
void mouse_poll(void);

// Non-blocking: returns true and fills dx/dy/buttons if a new movement
// packet has been decoded since the last call. buttons: bit0=left,
// bit1=right, bit2=middle. dx/dy are raw device counts (dy is positive
// "up" per the PS/2 convention; negate it if you want screen-down-positive).
bool mouse_get_event(int *dx, int *dy, uint8_t *buttons);
