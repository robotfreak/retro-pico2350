#include "bp_link.h"

#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"

#define RX_RING_SIZE 4096 // power of two

static uart_inst_t *link_uart;
static uint8_t rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head, rx_tail;
static bp_parser_t parser;

static void __not_in_flash_func(on_uart_rx)(void) {
    while (uart_is_readable(link_uart)) {
        uint8_t b = (uint8_t)uart_getc(link_uart);
        uint16_t next = (uint16_t)((rx_head + 1) & (RX_RING_SIZE - 1));
        if (next != rx_tail) { // else: ring full, drop (CRC will reject the frame)
            rx_ring[rx_head] = b;
            rx_head = next;
        }
    }
}

void bp_link_init(unsigned uart_num, unsigned tx_gpio, unsigned rx_gpio) {
    link_uart = uart_get_instance(uart_num);
    uart_init(link_uart, BP_UART_BAUD);
    gpio_set_function(tx_gpio, GPIO_FUNC_UART);
    gpio_set_function(rx_gpio, GPIO_FUNC_UART);
    uart_set_format(link_uart, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(link_uart, true);
    bp_parser_init(&parser);

    unsigned irq = (uart_num == 0) ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(irq, on_uart_rx);
    irq_set_enabled(irq, true);
    uart_set_irq_enables(link_uart, true, false);
}

void bp_link_send(uint8_t type, const void *payload, uint16_t len) {
    uint8_t hdr[4] = { BP_SOF, type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    uint8_t crc = 0;
    for (int i = 1; i < 4; i++) crc = bp_crc8_update(crc, hdr[i]);
    const uint8_t *pl = (const uint8_t *)payload;
    for (uint16_t i = 0; i < len; i++) crc = bp_crc8_update(crc, pl[i]);

    uart_write_blocking(link_uart, hdr, sizeof(hdr));
    if (len) uart_write_blocking(link_uart, pl, len);
    uart_write_blocking(link_uart, &crc, 1);
}

bool bp_link_poll(uint8_t *type, const uint8_t **payload, uint16_t *len) {
    while (rx_tail != rx_head) {
        uint8_t b = rx_ring[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1) & (RX_RING_SIZE - 1));
        if (bp_parser_feed(&parser, b)) {
            *type = parser.type;
            *payload = parser.payload;
            *len = parser.len;
            return true;
        }
    }
    return false;
}
