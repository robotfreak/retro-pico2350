#pragma once

#include <stdbool.h>
#include <stdint.h>

// Network access goes through the backplane controller (Pico 2 W) over a
// UART link, see src/proto/bp_proto.h and backplane/main.c.

// Initialises the link and checks that the backplane answers. Returns false
// if it does not (the rest of the system keeps working without networking).
bool net_init(void);

// Asks the backplane to join a WiFi access point (station mode). Blocks
// until connected or the timeout elapses.
bool net_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

// Resolves `host`, opens a TCP connection to it on `port`, and runs an
// interactive terminal session on the DVI console / PS2 keyboard until the
// remote end closes the connection or the user presses ESC. Handles basic
// Telnet IAC option negotiation (refuses everything) and a small subset of
// ANSI escape codes (SGR color, clear screen) often used by BBS menus.
bool net_telnet_session(const char *host, uint16_t port);

// Call regularly from wait loops: blinks the onboard LED as a firmware
// "still alive" indicator.
void net_heartbeat_tick(void);
