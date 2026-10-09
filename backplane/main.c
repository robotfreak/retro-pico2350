// Backplane controller (Raspberry Pi Pico 2 W): owns the CYW43439 WiFi chip
// and the lwIP stack, and exposes them to the main controller (video, SD,
// PS/2) through the framed UART protocol in src/proto/bp_proto.h.
//
// Wiring (3.3V logic, common GND):
//   backplane GPIO 0 (UART0 TX)  -> main controller GPIO 21 (UART1 RX)
//   backplane GPIO 1 (UART0 RX)  <- main controller GPIO 20 (UART1 TX)
// Debug output goes to USB-CDC (stdio over USB), not to the UART.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "bp_link.h"

#define LINK_UART   0
#define LINK_TX_GPIO 0
#define LINK_RX_GPIO 1

#define CONNECT_TIMEOUT_MS 10000
#define DNS_TIMEOUT_MS     8000
#define FWD_CHUNK          256

// ---------------------------------------------------------------------------
// TCP session state (one session at a time)

#define TCP_RING_SIZE 8192 // power of two

typedef enum { SESS_IDLE, SESS_RESOLVING, SESS_CONNECTING, SESS_OPEN } SessState;

static struct {
    SessState state;
    struct tcp_pcb *pcb;
    uint16_t port;
    ip_addr_t addr;
    absolute_time_t deadline;
    volatile bool dns_done, dns_ok, connected, closed;
    uint8_t ring[TCP_RING_SIZE];
    volatile uint16_t head, tail; // head: lwIP callback, tail: main loop
} sess;

static bool wifi_up;

static void send_state(uint8_t s) { bp_link_send(BP_EVT_TCP_STATE, &s, 1); }

static uint16_t ring_free(void) {
    return (uint16_t)((sess.tail - sess.head - 1) & (TCP_RING_SIZE - 1));
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;
    if (!p) { sess.closed = true; return ERR_OK; }
    if (p->tot_len > ring_free()) return ERR_MEM; // lwIP keeps the data and retries later
    for (struct pbuf *q = p; q; q = q->next) {
        const uint8_t *d = q->payload;
        for (uint16_t i = 0; i < q->len; i++) {
            sess.ring[sess.head] = d[i];
            sess.head = (uint16_t)((sess.head + 1) & (TCP_RING_SIZE - 1));
        }
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err) {
    (void)arg; (void)err;
    sess.pcb = NULL; // already freed by lwIP
    sess.closed = true;
    sess.connected = false;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
    (void)arg; (void)tpcb;
    if (err == ERR_OK) sess.connected = true; else sess.closed = true;
    return ERR_OK;
}

static void dns_found_cb(const char *name, const ip_addr_t *ip, void *arg) {
    (void)name; (void)arg;
    if (ip) { sess.addr = *ip; sess.dns_ok = true; }
    sess.dns_done = true;
}

static void session_teardown(void) {
    cyw43_arch_lwip_begin();
    if (sess.pcb) {
        tcp_arg(sess.pcb, NULL);
        tcp_recv(sess.pcb, NULL);
        tcp_err(sess.pcb, NULL);
        if (tcp_close(sess.pcb) != ERR_OK) tcp_abort(sess.pcb);
        sess.pcb = NULL;
    }
    cyw43_arch_lwip_end();
    sess.state = SESS_IDLE;
}

static void session_fail(uint8_t code) {
    session_teardown();
    send_state(code);
}

static void tcp_open(uint16_t port, const char *host) {
    if (sess.state != SESS_IDLE) session_teardown();
    if (!wifi_up) { send_state(BP_TCP_ERR_NOWIFI); return; }

    sess.port = port;
    sess.dns_done = sess.dns_ok = sess.connected = sess.closed = false;
    sess.head = sess.tail = 0;
    sess.state = SESS_RESOLVING;
    sess.deadline = make_timeout_time_ms(DNS_TIMEOUT_MS);
    send_state(BP_TCP_RESOLVING);

    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(host, &sess.addr, dns_found_cb, NULL);
    cyw43_arch_lwip_end();
    if (err == ERR_OK) { sess.dns_ok = sess.dns_done = true; }
    else if (err != ERR_INPROGRESS) { session_fail(BP_TCP_ERR_DNS); }
}

static void session_step(void) {
    switch (sess.state) {
    case SESS_IDLE:
        break;
    case SESS_RESOLVING:
        if (sess.dns_done) {
            if (!sess.dns_ok) { session_fail(BP_TCP_ERR_DNS); break; }
            cyw43_arch_lwip_begin();
            sess.pcb = tcp_new_ip_type(IP_GET_TYPE(&sess.addr));
            err_t err = ERR_MEM;
            if (sess.pcb) {
                tcp_recv(sess.pcb, tcp_recv_cb);
                tcp_err(sess.pcb, tcp_err_cb);
                err = tcp_connect(sess.pcb, &sess.addr, sess.port, tcp_connected_cb);
            }
            cyw43_arch_lwip_end();
            if (err != ERR_OK) { session_fail(sess.pcb ? BP_TCP_ERR_CONNECT : BP_TCP_ERR_MEM); break; }
            sess.state = SESS_CONNECTING;
            sess.deadline = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
            send_state(BP_TCP_CONNECTING);
        } else if (time_reached(sess.deadline)) {
            session_fail(BP_TCP_ERR_DNS);
        }
        break;
    case SESS_CONNECTING:
        if (sess.connected) {
            sess.state = SESS_OPEN;
            send_state(BP_TCP_CONNECTED);
        } else if (sess.closed) {
            session_fail(BP_TCP_ERR_CONNECT);
        } else if (time_reached(sess.deadline)) {
            cyw43_arch_lwip_begin();
            if (sess.pcb) { tcp_abort(sess.pcb); sess.pcb = NULL; }
            cyw43_arch_lwip_end();
            sess.state = SESS_IDLE;
            send_state(BP_TCP_ERR_TIMEOUT);
        }
        break;
    case SESS_OPEN: {
        // Forward received bytes to the main controller.
        while (sess.tail != sess.head) {
            uint8_t buf[FWD_CHUNK];
            uint16_t n = 0;
            while (n < FWD_CHUNK && sess.tail != sess.head) {
                buf[n++] = sess.ring[sess.tail];
                sess.tail = (uint16_t)((sess.tail + 1) & (TCP_RING_SIZE - 1));
            }
            bp_link_send(BP_EVT_TCP_DATA, buf, n);
        }
        if (sess.closed) { // ring is drained above, so no data is lost
            session_teardown();
            send_state(BP_TCP_CLOSED);
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Command handling

static void handle_frame(uint8_t type, const uint8_t *pl, uint16_t len) {
    switch (type) {
    case BP_CMD_PING: {
        uint8_t up = wifi_up;
        bp_link_send(BP_EVT_HELLO, &up, 1);
        break;
    }
    case BP_CMD_WIFI_CONNECT: {
        // ssid\0 pass\0 timeout_ms(u32 LE)
        const char *ssid = (const char *)pl;
        size_t sl = strnlen(ssid, len);
        if (sl + 1 >= len) break;
        const char *pass = ssid + sl + 1;
        size_t pl_len = strnlen(pass, len - sl - 1);
        size_t off = sl + 1 + pl_len + 1;
        uint32_t timeout = 30000;
        if (off + 4 <= len) memcpy(&timeout, pl + off, 4);
        if (sess.state != SESS_IDLE) session_teardown();
        printf("wifi connect '%s' (%u ms)\n", ssid, (unsigned)timeout);
        int rc = cyw43_arch_wifi_connect_timeout_ms(ssid, pass, CYW43_AUTH_WPA2_AES_PSK, timeout);
        printf("wifi connect -> %d\n", rc);
        wifi_up = (rc == 0);
        uint8_t res = wifi_up;
        bp_link_send(BP_EVT_WIFI, &res, 1);
        break;
    }
    case BP_CMD_TCP_OPEN: {
        if (len < 3) break;
        uint16_t port = (uint16_t)(pl[0] | (pl[1] << 8));
        char host[BP_MAX_PAYLOAD];
        size_t hl = strnlen((const char *)pl + 2, len - 2);
        memcpy(host, pl + 2, hl);
        host[hl] = '\0';
        printf("tcp open %s:%u\n", host, port);
        tcp_open(port, host);
        break;
    }
    case BP_CMD_TCP_DATA:
        if (sess.state == SESS_OPEN && sess.pcb && len) {
            cyw43_arch_lwip_begin();
            if (sess.pcb) {
                tcp_write(sess.pcb, pl, len, TCP_WRITE_FLAG_COPY);
                tcp_output(sess.pcb);
            }
            cyw43_arch_lwip_end();
        }
        break;
    case BP_CMD_TCP_CLOSE:
        if (sess.state != SESS_IDLE) session_teardown();
        break;
    }
}

static void heartbeat(void) {
    static absolute_time_t next;
    static bool on;
    if (time_reached(next)) {
        on = !on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
        next = make_timeout_time_ms(wifi_up ? 1000 : 250);
    }
}

int main(void) {
    stdio_init_all();
    if (cyw43_arch_init()) {
        printf("cyw43_arch_init failed\n");
        for (;;) tight_loop_contents();
    }
    cyw43_arch_enable_sta_mode();
    bp_link_init(LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO);
    printf("backplane ready\n");

    uint8_t up = 0;
    bp_link_send(BP_EVT_HELLO, &up, 1);

    for (;;) {
        uint8_t type;
        const uint8_t *pl;
        uint16_t len;
        while (bp_link_poll(&type, &pl, &len)) handle_frame(type, pl, len);
        session_step();
        heartbeat();
    }
}
