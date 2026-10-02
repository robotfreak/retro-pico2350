#pragma once

#include <stdbool.h>
#include <stdint.h>

// Connects to a WiFi access point (station mode), bringing up the CYW43439
// driver on first use. Blocks (busy-waiting, servicing lwIP in the
// background) until connected or the timeout elapses.
bool net_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

// Resolves `host`, opens a TCP connection to it on `port`, and runs an
// interactive terminal session on the DVI console / PS2 keyboard until the
// remote end closes the connection or the user presses ESC. Handles basic
// Telnet IAC option negotiation (refuses everything) and a small subset of
// ANSI escape codes (SGR color, clear screen) often used by BBS menus.
bool net_telnet_session(const char *host, uint16_t port);
