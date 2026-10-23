// Audio controller firmware: Pico 2 + Pimoroni Pico Audio Pack.
// I2C slave (I2C0, GPIO 4 = SDA, GPIO 5 = SCL, address AUDIO_I2C_ADDR) that
// drives the synth from the audiotest demo. Protocol: src/proto/audio_proto.h.
// Audio Pack I2S pins: DATA = GPIO 9, BCLK = GPIO 10, LRCLK = GPIO 11.

#include <stdio.h>

#include "hardware/i2c.h"
#include "pico/i2c_slave.h"
#include "pico/stdlib.h"

#include "audio_proto.h"
#include "audio.hpp"
#include "synth.hpp"

#define PICO_AUDIO_PACK_I2S_DATA 9
#define PICO_AUDIO_PACK_I2S_BCLK 10

#define I2C_SDA_GPIO 4
#define I2C_SCL_GPIO 5
#define I2C_HZ       400000

synth::AudioChannel synth::channels[CHANNEL_COUNT];

// ---- I2C slave: ISR collects one frame per write transaction ----------------

#define FRAME_MAX   16
#define QUEUE_LEN   32   // power of two

struct Frame { uint8_t len; uint8_t data[FRAME_MAX]; };

static Frame queue[QUEUE_LEN];
static volatile uint8_t q_head = 0, q_tail = 0;
static Frame cur;
static volatile uint8_t status = AUDIO_STATUS_MAGIC;

static void i2c_handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    switch (event) {
    case I2C_SLAVE_RECEIVE: {
        uint8_t b = i2c_read_byte_raw(i2c);
        if (cur.len < FRAME_MAX) cur.data[cur.len++] = b;
        break;
    }
    case I2C_SLAVE_REQUEST:
        i2c_write_byte_raw(i2c, status);
        break;
    case I2C_SLAVE_FINISH:
        if (cur.len) {
            uint8_t next = (q_head + 1) & (QUEUE_LEN - 1);
            if (next != q_tail) { queue[q_head] = cur; q_head = next; } // drop on overflow
            cur.len = 0;
        }
        break;
    default:
        break;
    }
}

// ---- command handling ---------------------------------------------------------

static uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static uint8_t map_waves(uint8_t w) {
    uint8_t r = 0;
    if (w & AUDIO_WAVE_SINE)     r |= synth::Waveform::SINE;
    if (w & AUDIO_WAVE_SQUARE)   r |= synth::Waveform::SQUARE;
    if (w & AUDIO_WAVE_SAW)      r |= synth::Waveform::SAW;
    if (w & AUDIO_WAVE_TRIANGLE) r |= synth::Waveform::TRIANGLE;
    if (w & AUDIO_WAVE_NOISE)    r |= synth::Waveform::NOISE;
    return r;
}

static void handle(const Frame &f) {
    const uint8_t *d = f.data;
    switch (d[0]) {
    case AUDIO_CMD_NOTE_ON:
        if (f.len >= 4 && d[1] < CHANNEL_COUNT) {
            uint16_t hz = u16(d + 2);
            auto &c = synth::channels[d[1]];
            if (hz == 0) { c.trigger_release(); }
            else { c.frequency = hz; c.trigger_attack(); }
        }
        break;
    case AUDIO_CMD_NOTE_OFF:
        if (f.len >= 2 && d[1] < CHANNEL_COUNT) synth::channels[d[1]].trigger_release();
        break;
    case AUDIO_CMD_VOICE:
        if (f.len >= 13 && d[1] < CHANNEL_COUNT) {
            auto &c = synth::channels[d[1]];
            c.waveforms  = map_waves(d[2]);
            c.attack_ms  = u16(d + 3);
            c.decay_ms   = u16(d + 5);
            c.sustain    = u16(d + 7);
            c.release_ms = u16(d + 9);
            c.volume     = u16(d + 11);
        }
        break;
    case AUDIO_CMD_MASTER:
        if (f.len >= 2) synth::volume = (d[1] << 8) | d[1];
        break;
    case AUDIO_CMD_ALL_OFF:
        for (int i = 0; i < CHANNEL_COUNT; i++) synth::channels[i].trigger_release();
        break;
    }
}

int main() {
    stdio_init_all();
    struct audio_buffer_pool *ap = init_audio(synth::sample_rate, PICO_AUDIO_PACK_I2S_DATA, PICO_AUDIO_PACK_I2S_BCLK);

    // Default voice on every channel so SOUND works without VOICE.
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        auto &c = synth::channels[i];
        c.waveforms  = synth::Waveform::SQUARE;
        c.attack_ms  = 5;
        c.decay_ms   = 100;
        c.sustain    = 0xafff;
        c.release_ms = 100;
        c.volume     = 10000;
    }

    gpio_set_function(I2C_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_GPIO);
    gpio_pull_up(I2C_SCL_GPIO);
    i2c_init(i2c0, I2C_HZ);
    i2c_slave_init(i2c0, AUDIO_I2C_ADDR, &i2c_handler);

    while (true) {
        while (q_tail != q_head) {
            handle(queue[q_tail]);
            q_tail = (q_tail + 1) & (QUEUE_LEN - 1);
        }
        status = AUDIO_STATUS_MAGIC | (synth::is_audio_playing() ? AUDIO_STATUS_PLAYING : 0);
        update_buffer(ap, synth::get_audio_frame);
    }
    return 0;
}
