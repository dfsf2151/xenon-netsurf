/*
 * Audio output for the Xenon frontend.
 *
 * The console's kernel pulls 48 kHz, 6-channel, 256-sample frames through
 * XAudioRegisterRenderDriverClient. Decoded PCM is resampled and mixed into
 * that layout in the callback; decoding itself happens on the UI thread.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "xenon/audio.h"
#include "xenon/xenon.h"
#include "xsdk/kernel.h"
#include "xsdk/thread.h"

#include "minimp3/minimp3.h"
#include "minimp3/minimp3_ex.h"

enum {
	OUT_RATE = 48000,
	OUT_CHANNELS = 6,
	FRAME_SAMPLES = 256,
	FRAME_FLOATS = OUT_CHANNELS * FRAME_SAMPLES,
	FRAME_BYTES = FRAME_FLOATS * (int)sizeof(float),
};

static float *frame;
static uint32_t driver;
static volatile uint32_t callbacks;
static volatile int in_callback;

static int16_t *pcm;
static uint32_t pcm_frames;
static uint32_t pcm_channels;
static uint32_t pcm_rate;
static volatile uint64_t play_pos;
static volatile int playing;
static char status[96];

static uint32_t read_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static void audio_callback(const uint32_t *context)
{
	(void)context;
	const uint32_t current = __atomic_load_n(&driver, __ATOMIC_ACQUIRE);
	if (!current || !frame)
		return;
	__atomic_store_n(&in_callback, 1, __ATOMIC_RELEASE);

	for (int i = 0; i < FRAME_FLOATS; i++)
		frame[i] = 0.0f;

	if (__atomic_load_n(&playing, __ATOMIC_ACQUIRE) && pcm && pcm_frames > 0) {
		const uint64_t step = ((uint64_t)pcm_rate << 32) / OUT_RATE;
		uint64_t pos = __atomic_load_n(&play_pos, __ATOMIC_RELAXED);
		for (int i = 0; i < FRAME_SAMPLES; i++) {
			const uint32_t index = (uint32_t)(pos >> 32);
			if (index >= pcm_frames) {
				__atomic_store_n(&playing, 0, __ATOMIC_RELEASE);
				break;
			}
			const int16_t *s = pcm + (size_t)index * pcm_channels;
			const float l = (float)s[0] / 32768.0f;
			const float r = (float)s[pcm_channels > 1 ? 1 : 0] / 32768.0f;
			frame[0 * FRAME_SAMPLES + i] = l;
			frame[1 * FRAME_SAMPLES + i] = r;
			pos += step;
		}
		__atomic_store_n(&play_pos, pos, __ATOMIC_RELAXED);
	}

	__atomic_store_n(&in_callback, 0, __ATOMIC_RELEASE);
	(void)XAudioSubmitRenderDriverFrame(current, frame);
	(void)__atomic_add_fetch(&callbacks, 1, __ATOMIC_RELAXED);
}

bool xenon_audio_init(void)
{
	if (frame)
		return true;
	frame = (float *)MmAllocatePhysicalMemoryEx(0, FRAME_BYTES, PAGE_READWRITE, 0, UINT32_MAX, 0);
	if (!frame) {
		snprintf(status, sizeof(status), "no audio memory");
		return false;
	}
	memset(frame, 0, FRAME_BYTES);

	const XAUDIO_RENDER_DRIVER_CLIENT client = {
		.callback = audio_callback,
		.context = 0,
	};
	uint32_t registered = 0;
	const uint32_t result = XAudioRegisterRenderDriverClient(&client, &registered);
	if (result != ERROR_SUCCESS) {
		snprintf(status, sizeof(status), "XAudio failed 0x%08X", (unsigned)result);
		return false;
	}
	__atomic_store_n(&driver, registered, __ATOMIC_RELEASE);
	snprintf(status, sizeof(status), "48 kHz, 6 channels");
	fprintf(stderr, "xenon: audio driver 0x%08X, %s\n", (unsigned)registered, status);
	return true;
}

static void wait_for_callback(void)
{
	for (int i = 0; i < 200 && __atomic_load_n(&in_callback, __ATOMIC_ACQUIRE); i++)
		(void)xsdk_thread_sleep(1);
}

void xenon_audio_stop(void)
{
	__atomic_store_n(&playing, 0, __ATOMIC_RELEASE);
	wait_for_callback();
}

static void drop_pcm(void)
{
	if (!pcm)
		return;
	/* Stop the callback before freeing the buffer it may still be reading. */
	xenon_audio_stop();
	free(pcm);
	pcm = NULL;
	pcm_frames = 0;
}

static bool start_pcm(int16_t *samples, uint32_t frames, uint32_t channels, uint32_t rate)
{
	if (!samples || frames == 0 || channels == 0 || rate == 0)
		return false;
	drop_pcm();
	pcm = samples;
	pcm_frames = frames;
	pcm_channels = channels;
	pcm_rate = rate;
	__atomic_store_n(&play_pos, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&playing, 1, __ATOMIC_RELEASE);
	fprintf(stderr, "xenon: audio play %u frames, %u Hz, %u ch\n", (unsigned)frames, (unsigned)rate, (unsigned)channels);
	return true;
}

bool xenon_audio_play_wav(const uint8_t *data, size_t size)
{
	if (!data || size < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
		return false;

	uint32_t format = 0, channels = 0, rate = 0, bits = 0;
	const uint8_t *pcm_data = NULL;
	size_t pcm_size = 0;
	size_t at = 12;
	while (at + 8 <= size) {
		const uint32_t len = read_le32(data + at + 4);
		const uint8_t *body = data + at + 8;
		if (at + 8 + len > size)
			break;
		if (memcmp(data + at, "fmt ", 4) == 0 && len >= 16) {
			format = read_le16(body);
			channels = read_le16(body + 2);
			rate = read_le32(body + 4);
			bits = read_le16(body + 14);
		} else if (memcmp(data + at, "data", 4) == 0) {
			pcm_data = body;
			pcm_size = len;
		}
		at += 8 + len + (len & 1);
	}
	if (format != 1 || bits != 16 || pcm_data == NULL || channels < 1 || channels > 2)
		return false;

	const uint32_t frames = (uint32_t)(pcm_size / (2 * channels));
	int16_t *samples = (int16_t *)malloc((size_t)frames * channels * 2);
	if (!samples)
		return false;
	for (uint32_t i = 0; i < frames * channels; i++)
		samples[i] = (int16_t)read_le16(pcm_data + (size_t)i * 2);

	return start_pcm(samples, frames, channels, rate);
}

bool xenon_audio_play_mp3(const uint8_t *data, size_t size)
{
	if (!data || size < 4)
		return false;

	mp3dec_t decoder;
	mp3dec_file_info_t info;
	memset(&info, 0, sizeof(info));
	mp3dec_init(&decoder);
	if (mp3dec_load_buf(&decoder, data, size, &info, NULL, NULL) != 0 || !info.buffer)
		return false;

	const uint32_t frames = (uint32_t)(info.samples / (size_t)info.channels);
	fprintf(stderr, "xenon: mp3 %u samples, %d Hz, %d ch\n", (unsigned)info.samples,
		info.hz, info.channels);
	const bool ok = start_pcm(info.buffer, frames, (uint32_t)info.channels, (uint32_t)info.hz);
	if (!ok)
		free(info.buffer);
	return ok;
}

bool xenon_audio_play_tone(int frequency_hz, int milliseconds)
{
	if (frequency_hz <= 0 || milliseconds <= 0)
		return false;
	const uint32_t frames = (uint32_t)((uint64_t)OUT_RATE * (uint32_t)milliseconds / 1000);
	int16_t *samples = (int16_t *)malloc((size_t)frames * 2 * 2);
	if (!samples)
		return false;
	for (uint32_t i = 0; i < frames; i++) {
		const float t = (float)i / OUT_RATE;
		float envelope = 1.0f;
		const uint32_t edge = OUT_RATE / 50;
		if (i < edge)
			envelope = (float)i / edge;
		else if (i + edge > frames)
			envelope = (float)(frames - i) / edge;
		const int16_t value = (int16_t)(32767.0f * 0.35f * envelope *
			sinf(2.0f * 3.14159265f * frequency_hz * t));
		samples[i * 2] = value;
		samples[i * 2 + 1] = value;
	}
	return start_pcm(samples, frames, 2, OUT_RATE);
}

bool xenon_audio_play_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (!file) {
		snprintf(status, sizeof(status), "cannot open %s", path);
		return false;
	}
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return false;
	}
	const long size = ftell(file);
	if (size <= 0 || size > 32 * 1024 * 1024) {
		fclose(file);
		return false;
	}
	rewind(file);
	uint8_t *data = (uint8_t *)malloc((size_t)size);
	if (!data) {
		fclose(file);
		return false;
	}
	const size_t got = fread(data, 1, (size_t)size, file);
	fclose(file);
	if (got != (size_t)size) {
		free(data);
		return false;
	}

	const char *dot = strrchr(path, '.');
	bool ok = false;
	if (dot && strcasecmp(dot, ".wav") == 0)
		ok = xenon_audio_play_wav(data, size);
	else if (dot && strcasecmp(dot, ".mp3") == 0)
		ok = xenon_audio_play_mp3(data, size);
	free(data);

	if (ok)
		snprintf(status, sizeof(status), "playing %s", path);
	else
		snprintf(status, sizeof(status), "unsupported %s", path);
	return ok;
}

bool xenon_audio_playing(void)
{
	return __atomic_load_n(&playing, __ATOMIC_ACQUIRE) != 0;
}

const char *xenon_audio_status(void)
{
	return status;
}

uint32_t xenon_audio_callbacks(void)
{
	return __atomic_load_n(&callbacks, __ATOMIC_RELAXED);
}
