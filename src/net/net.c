// WiFi + Telnet client for the main controller. The network stack lives on
// the separate backplane controller (Pico 2 W, see backplane/main.c); this
// file talks to it over a UART using the framed protocol in
// src/proto/bp_proto.h and only keeps the terminal logic (IAC negotiation,
// ANSI subset, keyboard -> remote) locally.

#include "net.h"
#include "dvi.h"
#include "ps2kbd.h"
#include "bp_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#define LINK_UART    1
#define LINK_TX_GPIO 20
#define LINK_RX_GPIO 21

static bool backplane_ok = false;
static bool wifi_connected = false;

#define NET_DBG(...) do { \
    printf("[%8u] ", (unsigned)to_ms_since_boot(get_absolute_time())); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    stdio_flush(); \
} while (0)

// Waits up to timeout_ms for a frame of type `want`; other frames are
// discarded. Returns false on timeout. *pl / *len point into the parser.
static bool wait_frame(uint8_t want, uint32_t timeout_ms, const uint8_t **pl, uint16_t *len) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline)) {
        uint8_t t;
        if (bp_link_poll(&t, pl, len) && t == want) return true;
        dvi_cursor_tick();
        net_heartbeat_tick();
    }
    return false;
}

bool net_init(void) {
    bp_link_init(LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO);
    // The backplane may already be up (it said HELLO before we listened), so ask.
    for (int attempt = 0; attempt < 3 && !backplane_ok; attempt++) {
        bp_link_send(BP_CMD_PING, NULL, 0);
        const uint8_t *pl; uint16_t len;
        if (wait_frame(BP_EVT_HELLO, 300, &pl, &len)) {
            backplane_ok = true;
            wifi_connected = len >= 1 && pl[0];
        }
    }
    NET_DBG("net_init: backplane %s", backplane_ok ? "found" : "NOT FOUND");
    return backplane_ok;
}

void net_heartbeat_tick(void) {
#ifdef PICO_DEFAULT_LED_PIN
    static absolute_time_t next_toggle;
    static bool on = false, first = true;
    if (first) { gpio_init(PICO_DEFAULT_LED_PIN); gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT); }
    if (first || time_reached(next_toggle)) {
        first = false;
        on = !on;
        gpio_put(PICO_DEFAULT_LED_PIN, on);
        next_toggle = make_timeout_time_ms(500);
    }
#endif
}

bool net_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms) {
    if (!backplane_ok) {
        NET_DBG("net_wifi_connect: no backplane");
        return false;
    }
    uint8_t buf[BP_MAX_PAYLOAD];
    size_t sl = strlen(ssid), pl_ = strlen(password);
    if (sl + pl_ + 2 + 4 > sizeof(buf)) return false;
    memcpy(buf, ssid, sl + 1);
    memcpy(buf + sl + 1, password, pl_ + 1);
    memcpy(buf + sl + pl_ + 2, &timeout_ms, 4);
    bp_link_send(BP_CMD_WIFI_CONNECT, buf, (uint16_t)(sl + pl_ + 2 + 4));

    const uint8_t *pl; uint16_t len;
    wifi_connected = wait_frame(BP_EVT_WIFI, timeout_ms + 3000, &pl, &len) && len >= 1 && pl[0];
    return wifi_connected;
}

// ----------------------------------------------------------------------------
// Telnet IAC negotiation + minimal ANSI (SGR / clear-screen) filtering

typedef enum { TS_NORMAL, TS_ESC, TS_CSI, TS_IAC, TS_IAC_CMD } TermState;

static void handle_sgr(const char *params) {
    static const struct { uint8_t r, g, b; } pal[8] = {
        {0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
        {64, 96, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
    };
    int vals[8];
    int n = 0;
    const char *p = params;
    while (*p && n < 8) {
        vals[n++] = atoi(p);
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    if (n == 0) vals[n++] = 0;
    for (int i = 0; i < n; i++) {
        int v = vals[i];
        if (v == 0) {
            dvi_set_colors(dvi_rgb332(0, 255, 0), 0);
        } else if (v >= 30 && v <= 37) {
            int idx = v - 30;
            dvi_set_colors(dvi_rgb332(pal[idx].r, pal[idx].g, pal[idx].b), 0);
        }
        // Background colors (40-47) and bright variants (90-97) are not
        // mapped in this first version.
    }
}

static void telnet_send(const uint8_t *data, uint16_t len) {
    bp_link_send(BP_CMD_TCP_DATA, data, len);
}

bool net_telnet_session(const char *host, uint16_t port) {
    NET_DBG("net_telnet_session: host='%s' port=%u", host, (unsigned)port);
    if (!backplane_ok) {
        dvi_puts("?NO BACKPLANE\n");
        return false;
    }
    if (!wifi_connected) {
        dvi_puts("?NO WIFI - USE WIFI \"SSID\",\"PASSWORD\" FIRST\n");
        return false;
    }

    uint8_t req[BP_MAX_PAYLOAD];
    size_t hl = strlen(host);
    if (hl + 3 > sizeof(req)) return false;
    req[0] = (uint8_t)(port & 0xFF);
    req[1] = (uint8_t)(port >> 8);
    memcpy(req + 2, host, hl + 1);
    bp_link_send(BP_CMD_TCP_OPEN, req, (uint16_t)(hl + 3));

    // Follow the backplane's state events until connected or failed.
    for (bool connected = false; !connected; ) {
        const uint8_t *pl; uint16_t len;
        if (!wait_frame(BP_EVT_TCP_STATE, 25000, &pl, &len) || len < 1) {
            dvi_puts("?TIMEOUT\n");
            bp_link_send(BP_CMD_TCP_CLOSE, NULL, 0);
            return false;
        }
        switch (pl[0]) {
        case BP_TCP_RESOLVING:  dvi_puts("RESOLVING...\n"); break;
        case BP_TCP_CONNECTING: dvi_puts("CONNECTING...\n"); break;
        case BP_TCP_CONNECTED:  connected = true; break;
        case BP_TCP_ERR_DNS:    dvi_puts("?HOST NOT FOUND\n"); return false;
        case BP_TCP_ERR_TIMEOUT: dvi_puts("?TIMEOUT\n"); return false;
        case BP_TCP_ERR_MEM:    dvi_puts("?NO MEMORY\n"); return false;
        case BP_TCP_ERR_NOWIFI: dvi_puts("?NO WIFI\n"); wifi_connected = false; return false;
        default:                dvi_puts("?CONNECT FAILED\n"); return false;
        }
    }

    dvi_puts("CONNECTED. (ESC TO QUIT)\n");

    TermState term_state = TS_NORMAL;
    char csi_buf[16];
    int csi_len = 0;
    uint8_t iac_cmd_byte = 0;
    bool pending_cr = false;

    bool closed = false;
    while (!closed) {
        dvi_cursor_tick();
        net_heartbeat_tick();
        int c = kbd_getc_nonblock();
        if (c == 27) break; // ESC quits back to BASIC
        if (c >= 0) {
            uint8_t ch = (uint8_t)c;
            if (ch == '\n') {
                static const uint8_t crlf[2] = {'\r', '\n'};
                telnet_send(crlf, 2);
            } else {
                telnet_send(&ch, 1);
            }
        }

        uint8_t ft;
        const uint8_t *fpl;
        uint16_t flen;
        while (bp_link_poll(&ft, &fpl, &flen)) {
            if (ft == BP_EVT_TCP_STATE && flen >= 1 && fpl[0] != BP_TCP_CONNECTED) closed = true;
            if (ft != BP_EVT_TCP_DATA) continue;
            for (uint16_t k = 0; k < flen; k++) {
                uint8_t b = fpl[k];

            switch (term_state) {
            case TS_NORMAL:
                if (b == 0xFF) { term_state = TS_IAC; break; }
                if (b == 0x1B) { term_state = TS_ESC; pending_cr = false; break; }
                if (b == '\r') { pending_cr = true; dvi_putc('\n'); break; }
                if (b == '\n') {
                    if (pending_cr) { pending_cr = false; break; }
                    dvi_putc('\n');
                    break;
                }
                pending_cr = false;
                if (b == '\b' || (b >= 32 && b < 127)) dvi_putc((char)b);
                break;
            case TS_ESC:
                if (b == '[') { term_state = TS_CSI; csi_len = 0; }
                else term_state = TS_NORMAL;
                break;
            case TS_CSI:
                if ((b >= '0' && b <= '9') || b == ';') {
                    if (csi_len < (int)sizeof(csi_buf) - 1) csi_buf[csi_len++] = (char)b;
                } else {
                    csi_buf[csi_len] = '\0';
                    if (b == 'm') handle_sgr(csi_buf);
                    else if (b == 'J' || b == 'H' || b == 'f') dvi_clear();
                    term_state = TS_NORMAL;
                }
                break;
            case TS_IAC:
                if (b == 0xFF) { // literal 0xFF data byte
                    term_state = TS_NORMAL;
                } else {
                    iac_cmd_byte = b;
                    term_state = TS_IAC_CMD;
                }
                break;
            case TS_IAC_CMD: {
                uint8_t reply_cmd = 0;
                if (iac_cmd_byte == 0xFB /* WILL */) reply_cmd = 0xFE; // DONT
                else if (iac_cmd_byte == 0xFD /* DO */) reply_cmd = 0xFC; // WONT
                if (reply_cmd) {
                    uint8_t reply[3] = {0xFF, reply_cmd, b};
                    telnet_send(reply, 3);
                }
                term_state = TS_NORMAL;
                break;
            }
            } // switch
            } // for k
        } // while poll
    } // while !closed

    dvi_puts("\n?CONNECTION CLOSED\n");
    bp_link_send(BP_CMD_TCP_CLOSE, NULL, 0);
    return true;
}
