#ifndef NETSURF_XENON_AUDIO_H
#define NETSURF_XENON_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool xenon_audio_init(void);
void xenon_audio_stop(void);
bool xenon_audio_play_wav(const uint8_t *data, size_t size);
bool xenon_audio_play_mp3(const uint8_t *data, size_t size);
bool xenon_audio_play_file(const char *path);
bool xenon_audio_play_tone(int frequency_hz, int milliseconds);
bool xenon_audio_playing(void);
const char *xenon_audio_status(void);
uint32_t xenon_audio_callbacks(void);

#endif
