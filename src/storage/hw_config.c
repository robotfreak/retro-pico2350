// Hardware configuration for carlk3's no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
// library. See that library's README ("Customizing for the hardware
// configuration") for the meaning of these structs.
//
// We use SPI1 on GPIO 8-11, chosen specifically to avoid this project's
// other fixed pin assignments: GPIO 12-19 (HSTX/DVI), GPIO 2-5 (PS/2
// keyboard+mouse), GPIO 0-1 (debug UART), GPIO 20-21 (backplane UART).

#include "hw_config.h"

static spi_t spi = {
    .hw_inst = spi1,
    .sck_gpio = 10,
    .mosi_gpio = 11,
    .miso_gpio = 8,
    // Conservative first-bring-up speed; the library's examples default to
    // much higher rates once wiring/signal integrity is confirmed.
    .baud_rate = 125 * 1000 * 1000 / 8, // ~15.6 MHz
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = 9,
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num) {
    return (num == 0) ? &sd_card : NULL;
}
