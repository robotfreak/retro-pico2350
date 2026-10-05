// WiFi connection + a minimal interactive Telnet client, built on lwIP's
// raw API and pico_cyw43_arch_lwip_threadsafe_background (lwIP is serviced
// automatically via a background IRQ; our main-line code only needs to wrap
// calls INTO lwIP with cyw43_arch_lwip_begin()/end(), per the official
// pico-examples pico_w/wifi/tcp_client pattern).

#include "net.h"
#include "dvi.h"
#include "ps2kbd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

static bool cyw43_ready = false;
static bool wifi_connected = false;

// NET_DBG: timestamped debug line to stdio (UART), flushed immediately so
// it still shows up even if a hang/crash follows right after this call.
#define NET_DBG(...) do { \
    printf("[%8u] ", (unsigned)to_ms_since_boot(get_absolute_time())); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    stdio_flush(); \
} while (0)

bool net_init(void) {
    NET_DBG("net_init: calling cyw43_arch_init()");
    if (cyw43_arch_init()) {
        NET_DBG("net_init: cyw43_arch_init() FAILED");
        return false;
    }
    NET_DBG("net_init: cyw43_arch_init() ok, enabling STA mode");
    cyw43_arch_enable_sta_mode();
    cyw43_ready = true;
    NET_DBG("net_init: done");
    return true;
}

void net_heartbeat_tick(void) {
    static absolute_time_t next_toggle;
    static bool on = false;
    static bool first = true;
    if (!cyw43_ready) return;
    if (first || time_reached(next_toggle)) {
        first = false;
        on = !on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
        next_toggle = make_timeout_time_ms(500);
    }
}

bool net_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms) {
    if (!cyw43_ready) {
        NET_DBG("net_wifi_connect: net_init() was not called - aborting");
        return false;
    }
    NET_DBG("net_wifi_connect: connecting to '%s' (timeout %u ms)...", ssid, (unsigned)timeout_ms);
    int err = cyw43_arch_wifi_connect_timeout_ms(ssid, password, CYW43_AUTH_WPA2_AES_PSK, timeout_ms);
    NET_DBG("net_wifi_connect: cyw43_arch_wifi_connect_timeout_ms returned %d", err);
    wifi_connected = (err == 0);
    return wifi_connected;
}

// ----------------------------------------------------------------------------
// DNS resolution

typedef struct {
    volatile bool done;
    ip_addr_t addr;
    err_t result;
} dns_result_t;

static void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name;
    dns_result_t *r = (dns_result_t *)arg;
    if (ipaddr) {
        r->addr = *ipaddr;
        r->result = ERR_OK;
    } else {
        r->result = ERR_VAL;
    }
    r->done = true;
}

static bool resolve_host(const char *host, ip_addr_t *out, uint32_t timeout_ms) {
    dns_result_t r = {0};
    NET_DBG("resolve_host: dns_gethostbyname('%s')", host);
    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(host, &r.addr, dns_found_cb, &r);
    cyw43_arch_lwip_end();
    NET_DBG("resolve_host: dns_gethostbyname returned %d", err);
    if (err == ERR_OK) {
        *out = r.addr;
        return true;
    }
    if (err != ERR_INPROGRESS) return false;

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!r.done) {
        if (time_reached(deadline)) {
            NET_DBG("resolve_host: timed out waiting for DNS callback");
            return false;
        }
        net_heartbeat_tick();
        sleep_ms(10);
    }
    NET_DBG("resolve_host: callback fired, result=%d", r.result);
    if (r.result != ERR_OK) return false;
    *out = r.addr;
    return true;
}

// ----------------------------------------------------------------------------
// Telnet session state

#define RX_BUF_SIZE 512

typedef struct {
    struct tcp_pcb *pcb;
    volatile bool connected;
    volatile bool closed;
    uint8_t rx_buf[RX_BUF_SIZE];
    volatile uint16_t rx_head, rx_tail;
} telnet_state_t;

static err_t telnet_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)err;
    telnet_state_t *st = (telnet_state_t *)arg;
    if (!p) {
        st->closed = true;
        return ERR_OK;
    }
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const uint8_t *data = (const uint8_t *)q->payload;
        for (uint16_t i = 0; i < q->len; i++) {
            uint16_t next = (uint16_t)((st->rx_head + 1) % RX_BUF_SIZE);
            if (next != st->rx_tail) {
                st->rx_buf[st->rx_head] = data[i];
                st->rx_head = next;
            } // else: drop byte on overflow
        }
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void telnet_err_cb(void *arg, err_t err) {
    telnet_state_t *st = (telnet_state_t *)arg;
    NET_DBG("telnet_err_cb: err=%d", err);
    st->closed = true;
    st->pcb = NULL; // lwIP has already freed the pcb when this fires
}

static err_t telnet_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)tpcb;
    telnet_state_t *st = (telnet_state_t *)arg;
    NET_DBG("telnet_connected_cb: err=%d", err);
    if (err != ERR_OK) {
        st->closed = true;
        return ERR_OK;
    }
    st->connected = true;
    return ERR_OK;
}

static void telnet_send(telnet_state_t *st, const uint8_t *data, uint16_t len) {
    cyw43_arch_lwip_begin();
    if (st->pcb) {
        tcp_write(st->pcb, data, len, TCP_WRITE_FLAG_COPY);
        tcp_output(st->pcb);
    }
    cyw43_arch_lwip_end();
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

bool net_telnet_session(const char *host, uint16_t port) {
    NET_DBG("net_telnet_session: host='%s' port=%u", host, (unsigned)port);
    if (!wifi_connected) {
        dvi_puts("?NO WIFI - USE WIFI \"SSID\",\"PASSWORD\" FIRST\n");
        return false;
    }

    dvi_puts("RESOLVING...\n");
    ip_addr_t addr;
    if (!resolve_host(host, &addr, 8000)) {
        dvi_puts("?HOST NOT FOUND\n");
        return false;
    }
    NET_DBG("net_telnet_session: resolved ok");

    static telnet_state_t st; // static: avoids a large stack frame, one session at a time
    memset(&st, 0, sizeof(st));

    NET_DBG("net_telnet_session: tcp_new_ip_type + tcp_connect...");
    cyw43_arch_lwip_begin();
    st.pcb = tcp_new_ip_type(IP_GET_TYPE(&addr));
    if (!st.pcb) {
        cyw43_arch_lwip_end();
        NET_DBG("net_telnet_session: tcp_new_ip_type FAILED (out of memory?)");
        dvi_puts("?NO MEMORY\n");
        return false;
    }
    tcp_arg(st.pcb, &st);
    tcp_recv(st.pcb, telnet_recv_cb);
    tcp_err(st.pcb, telnet_err_cb);
    err_t err = tcp_connect(st.pcb, &addr, port, telnet_connected_cb);
    cyw43_arch_lwip_end();
    NET_DBG("net_telnet_session: tcp_connect returned %d", err);
    if (err != ERR_OK) {
        dvi_puts("?CONNECT FAILED\n");
        return false;
    }

    dvi_puts("CONNECTING...\n");
    absolute_time_t deadline = make_timeout_time_ms(10000);
    while (!st.connected && !st.closed) {
        if (time_reached(deadline)) {
            NET_DBG("net_telnet_session: connect timed out, aborting");
            cyw43_arch_lwip_begin();
            if (st.pcb) tcp_abort(st.pcb);
            cyw43_arch_lwip_end();
            dvi_puts("?TIMEOUT\n");
            return false;
        }
        net_heartbeat_tick();
        sleep_ms(10);
    }
    NET_DBG("net_telnet_session: connect wait loop exited (connected=%d closed=%d)", st.connected, st.closed);
    if (st.closed) {
        dvi_puts("?CONNECT FAILED\n");
        return false;
    }

    dvi_puts("CONNECTED. (ESC TO QUIT)\n");

    TermState term_state = TS_NORMAL;
    char csi_buf[16];
    int csi_len = 0;
    uint8_t iac_cmd_byte = 0;
    bool pending_cr = false;

    while (!st.closed) {
        dvi_cursor_tick();
        net_heartbeat_tick();
        int c = kbd_getc_nonblock();
        if (c == 27) break; // ESC quits back to BASIC
        if (c >= 0) {
            uint8_t ch = (uint8_t)c;
            if (ch == '\n') {
                static const uint8_t crlf[2] = {'\r', '\n'};
                telnet_send(&st, crlf, 2);
            } else {
                telnet_send(&st, &ch, 1);
            }
        }

        while (st.rx_tail != st.rx_head) {
            uint8_t b = st.rx_buf[st.rx_tail];
            st.rx_tail = (uint16_t)((st.rx_tail + 1) % RX_BUF_SIZE);

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
                    telnet_send(&st, reply, 3);
                }
                term_state = TS_NORMAL;
                break;
            }
            }
        }
    }

    dvi_puts("\n?CONNECTION CLOSED\n");
    cyw43_arch_lwip_begin();
    if (st.pcb) {
        tcp_arg(st.pcb, NULL);
        tcp_recv(st.pcb, NULL);
        tcp_err(st.pcb, NULL);
        tcp_close(st.pcb);
    }
    cyw43_arch_lwip_end();
    return true;
}
