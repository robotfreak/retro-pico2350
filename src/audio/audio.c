#include "audio.h"
#include "audio_proto.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define AUDIO_I2C      i2c1
#define AUDIO_SDA_GPIO 6
#define AUDIO_SCL_GPIO 7
#define AUDIO_I2C_HZ   400000
#define AUDIO_TIMEOUT_US 5000

static bool present = false;

static bool probe(void) {
    uint8_t st;
    int r = i2c_read_timeout_us(AUDIO_I2C, AUDIO_I2C_ADDR, &st, 1, false, AUDIO_TIMEOUT_US);
    return r == 1 && (st & AUDIO_STATUS_MASK) == AUDIO_STATUS_MAGIC;
}

void audio_init(void) {
    i2c_init(AUDIO_I2C, AUDIO_I2C_HZ);
    gpio_set_function(AUDIO_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(AUDIO_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(AUDIO_SDA_GPIO); // external 2.2-4.7k pull-ups recommended
    gpio_pull_up(AUDIO_SCL_GPIO);
    present = probe();
}

bool audio_ready(void) {
    if (!present) present = probe();
    return present;
}

static bool send(const uint8_t *buf, size_t len) {
    if (!audio_ready()) return false;
    int r = i2c_write_timeout_us(AUDIO_I2C, AUDIO_I2C_ADDR, buf, len, false, AUDIO_TIMEOUT_US);
    if (r != (int)len) { present = false; return false; }
    return true;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

bool audio_note_on(int ch, int freq_hz) {
    freq_hz = clampi(freq_hz, 0, 20000);
    uint8_t b[4] = { AUDIO_CMD_NOTE_ON, (uint8_t)ch, (uint8_t)freq_hz, (uint8_t)(freq_hz >> 8) };
    return send(b, sizeof b);
}

bool audio_note_off(int ch) {
    uint8_t b[2] = { AUDIO_CMD_NOTE_OFF, (uint8_t)ch };
    return send(b, sizeof b);
}

bool audio_voice(int ch, int waves, int attack_ms, int decay_ms, int sustain_pct,
                 int release_ms, int volume_pct) {
    attack_ms = clampi(attack_ms, 1, 10000);
    decay_ms = clampi(decay_ms, 1, 10000);
    release_ms = clampi(release_ms, 1, 10000);
    uint16_t sus = (uint16_t)(clampi(sustain_pct, 0, 100) * 65535 / 100);
    uint16_t vol = (uint16_t)(clampi(volume_pct, 0, 100) * 160); // 100% = 16000, leaves headroom for 8 voices
    uint8_t b[13] = { AUDIO_CMD_VOICE, (uint8_t)ch, (uint8_t)waves,
        (uint8_t)attack_ms, (uint8_t)(attack_ms >> 8),
        (uint8_t)decay_ms, (uint8_t)(decay_ms >> 8),
        (uint8_t)sus, (uint8_t)(sus >> 8),
        (uint8_t)release_ms, (uint8_t)(release_ms >> 8),
        (uint8_t)vol, (uint8_t)(vol >> 8) };
    return send(b, sizeof b);
}

bool audio_master_volume(int pct) {
    uint8_t b[2] = { AUDIO_CMD_MASTER, (uint8_t)(clampi(pct, 0, 100) * 255 / 100) };
    return send(b, sizeof b);
}

bool audio_all_off(void) {
    uint8_t b[1] = { AUDIO_CMD_ALL_OFF };
    return send(b, sizeof b);
}
