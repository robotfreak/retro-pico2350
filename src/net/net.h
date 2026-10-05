#pragma once

#include <stdbool.h>
#include <stdint.h>

// Brings up the CYW43439 driver and station mode. Call this once at boot
// (main.c does), *before* net_wifi_connect() or net_heartbeat_tick(). It is
// deliberately unconditional (not lazy) right now: that lets us tell, while
// debugging the "crashes a few seconds after WiFi starts" issue, whether
// merely having the WiFi chip driver active (LED heartbeat blinking, no
// network traffic) is enough to cause trouble, versus only actually
// connecting / passing traffic. Prints progress to stdio (UART) either way.
bool net_init(void);

// Connects to a WiFi access point (station mode). Blocks (busy-waiting,
// servicing lwIP in the background) until connected or the timeout elapses.
// Requires net_init() to have been called already.
bool net_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

// Resolves `host`, opens a TCP connection to it on `port`, and runs an
// interactive terminal session on the DVI console / PS2 keyboard until the
// remote end closes the connection or the user presses ESC. Handles basic
// Telnet IAC option negotiation (refuses everything) and a small subset of
// ANSI escape codes (SGR color, clear screen) often used by BBS menus.
bool net_telnet_session(const char *host, uint16_t port);

// Call regularly (e.g. from an input-wait loop) once net_init() has
// succeeded. Blinks the onboard LED (wired to the CYW43439 on Pico 2 W, not
// a plain GPIO) roughly every 500ms, purely as a "is the firmware still
// alive" indicator while debugging - if it stops blinking, the firmware has
// hung; if the board resets, it starts blinking from scratch after reboot.
void net_heartbeat_tick(void);
