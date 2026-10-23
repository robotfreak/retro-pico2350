#pragma once
#include <stdbool.h>
#include <stdint.h>

// Driver for the external audio controller (see proto/audio_proto.h).
void audio_init(void);
bool audio_ready(void);                  // probes the slave if it was not found yet
bool audio_note_on(int ch, int freq_hz);
bool audio_note_off(int ch);
bool audio_voice(int ch, int waves, int attack_ms, int decay_ms, int sustain_pct,
                 int release_ms, int volume_pct);
bool audio_master_volume(int pct);
bool audio_all_off(void);
