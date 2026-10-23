// I2C protocol between the main controller (master, I2C1 GPIO 6=SDA 7=SCL)
// and the audio controller (Pico 2 + Pimoroni Pico Audio Pack, I2C slave on
// I2C0 GPIO 4=SDA 5=SCL). Every command is one I2C write transaction:
// byte 0 = command, followed by the arguments (16-bit values little endian).
// A 1-byte read returns the status byte (see AUDIO_STATUS_*).
#pragma once

#define AUDIO_I2C_ADDR      0x2A
#define AUDIO_CHANNELS      8

#define AUDIO_CMD_NOTE_ON   0x01  // ch(u8) freq_hz(u16)
#define AUDIO_CMD_NOTE_OFF  0x02  // ch(u8)
#define AUDIO_CMD_VOICE     0x03  // ch(u8) waves(u8) attack_ms(u16) decay_ms(u16)
                                  //   sustain(u16, 0..65535) release_ms(u16) volume(u16)
#define AUDIO_CMD_MASTER    0x04  // volume(u8, 0..255)
#define AUDIO_CMD_ALL_OFF   0x05  // (none)

// Waveform bit mask used in AUDIO_CMD_VOICE (slave maps these to synth waveforms)
#define AUDIO_WAVE_SINE     0x01
#define AUDIO_WAVE_SQUARE   0x02
#define AUDIO_WAVE_SAW      0x04
#define AUDIO_WAVE_TRIANGLE 0x08
#define AUDIO_WAVE_NOISE    0x10

#define AUDIO_STATUS_MAGIC  0xA0  // high nibble of the status byte
#define AUDIO_STATUS_MASK   0xF0
#define AUDIO_STATUS_PLAYING 0x01 // some channel is sounding
