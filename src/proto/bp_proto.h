#pragma once

// Wire protocol between the main controller (Pico 2: video, SD, PS/2) and
// the backplane controller (Pico 2 W: WiFi + TCP/IP), carried over a UART.
//
// Frame:  SOF | type | len_lo | len_hi | payload[len] | crc8
//   - SOF  = 0xA5
//   - crc8 = CRC-8 (poly 0x07) over type, len_lo, len_hi and payload
//   - a receiver that sees a bad CRC / oversized length drops the frame and
//     hunts for the next SOF, so the link re-syncs after line noise.
//
// Strings in payloads are NUL-terminated.

#include <stdint.h>
#include <stddef.h>

#define BP_SOF          0xA5
#define BP_MAX_PAYLOAD  512

// Physical link (same on both boards: TX of one goes to RX of the other).
#define BP_UART_BAUD    921600

// Main controller -> backplane
#define BP_CMD_PING         0x01  // (none)                    -> BP_EVT_HELLO
#define BP_CMD_WIFI_CONNECT 0x02  // ssid\0 pass\0 timeout_ms(u32 LE) -> BP_EVT_WIFI
#define BP_CMD_TCP_OPEN     0x03  // port(u16 LE) host\0        -> BP_EVT_TCP_STATE...
#define BP_CMD_TCP_DATA     0x04  // raw bytes to send
#define BP_CMD_TCP_CLOSE    0x05  // (none)

// Backplane -> main controller
#define BP_EVT_HELLO        0x81  // sent at boot and in reply to PING; payload: wifi_up(u8)
#define BP_EVT_WIFI         0x82  // result(u8): 1 = connected, 0 = failed
#define BP_EVT_TCP_STATE    0x83  // one of BP_TCP_*
#define BP_EVT_TCP_DATA     0x84  // raw bytes received

// BP_EVT_TCP_STATE values
#define BP_TCP_RESOLVING    1
#define BP_TCP_CONNECTING   2
#define BP_TCP_CONNECTED    3
#define BP_TCP_CLOSED       4  // remote closed / session ended
#define BP_TCP_ERR_NOWIFI   5
#define BP_TCP_ERR_DNS      6
#define BP_TCP_ERR_CONNECT  7
#define BP_TCP_ERR_TIMEOUT  8
#define BP_TCP_ERR_MEM      9

static inline uint8_t bp_crc8_update(uint8_t crc, uint8_t b) {
    crc ^= b;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    return crc;
}

// Incremental frame parser. Feed bytes with bp_parser_feed(); it returns 1
// when a complete, CRC-valid frame is available in p->type / p->payload / p->len.
typedef struct {
    enum { BPS_SOF, BPS_TYPE, BPS_LEN_LO, BPS_LEN_HI, BPS_PAYLOAD, BPS_CRC } state;
    uint8_t  type;
    uint16_t len, pos;
    uint8_t  crc;
    uint8_t  payload[BP_MAX_PAYLOAD];
} bp_parser_t;

static inline void bp_parser_init(bp_parser_t *p) { p->state = BPS_SOF; }

static inline int bp_parser_feed(bp_parser_t *p, uint8_t b) {
    switch (p->state) {
    case BPS_SOF:
        if (b == BP_SOF) { p->state = BPS_TYPE; p->crc = 0; }
        break;
    case BPS_TYPE:
        p->type = b; p->crc = bp_crc8_update(p->crc, b); p->state = BPS_LEN_LO;
        break;
    case BPS_LEN_LO:
        p->len = b; p->crc = bp_crc8_update(p->crc, b); p->state = BPS_LEN_HI;
        break;
    case BPS_LEN_HI:
        p->len |= (uint16_t)(b << 8); p->crc = bp_crc8_update(p->crc, b);
        p->pos = 0;
        if (p->len > BP_MAX_PAYLOAD) p->state = BPS_SOF;
        else p->state = p->len ? BPS_PAYLOAD : BPS_CRC;
        break;
    case BPS_PAYLOAD:
        p->payload[p->pos++] = b; p->crc = bp_crc8_update(p->crc, b);
        if (p->pos >= p->len) p->state = BPS_CRC;
        break;
    case BPS_CRC:
        p->state = BPS_SOF;
        return b == p->crc;
    }
    return 0;
}
