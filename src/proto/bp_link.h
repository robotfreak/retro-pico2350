#pragma once

// UART transport for the main <-> backplane link (see bp_proto.h).
// Interrupt-driven RX into a ring buffer so the 32-byte hardware FIFO never
// overflows while the caller is busy (e.g. rendering to the display).

#include <stdbool.h>
#include <stdint.h>
#include "bp_proto.h"

void bp_link_init(unsigned uart_num, unsigned tx_gpio, unsigned rx_gpio);

// Sends one frame (blocking on the TX FIFO).
void bp_link_send(uint8_t type, const void *payload, uint16_t len);

// Polls the RX ring; returns true if a complete frame was received, then
// *type / *payload / *len are valid until the next call.
bool bp_link_poll(uint8_t *type, const uint8_t **payload, uint16_t *len);
