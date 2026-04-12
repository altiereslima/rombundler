/* ---------------------------------------------------------------
 * audio.c — OpenAL audio with volume/mute control via AL_GAIN.
 * --------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

#ifdef __APPLE__
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

#define BUFSIZE 1024*8
#define NUMBUFFERS 4

#ifndef MIN
#define MIN(x,y)	((x) <= (y) ? (x) : (y))
#endif

#include "audio.h"
#include "utils.h"

typedef struct al
{
	ALuint source;
	ALuint *buffers;
	ALuint *res_buf;
	ALCdevice *handle;
	ALCcontext *ctx;
	size_t res_ptr;
	size_t tmpbuf_ptr;
	size_t resample_cap_frames;
	uint64_t resample_accum;
	int16_t *resample_buf;
	int input_rate;
	int rate;
	bool needs_resample;
	uint8_t tmpbuf[BUFSIZE];
} al_t;

al_t* al = NULL;

/* Volume state */
static int  current_volume = 100;   /* 0-100 */
static bool current_mute   = false;
static bool nonblocking    = false;

#define MAX_DIRECT_AUDIO_RATE 192000
#define DEFAULT_OUTPUT_RATE    48000

static void apply_gain(void)
{
	if (!al) return;
	float gain = current_mute ? 0.0f : (float)current_volume / 100.0f;
	alSourcef(al->source, AL_GAIN, gain);
}

static bool unqueue_buffers()
{
	ALint val;

	alGetSourcei(al->source, AL_BUFFERS_PROCESSED, &val);

	if (val <= 0)
		return false;

	alSourceUnqueueBuffers(al->source, val, &al->res_buf[al->res_ptr]);
	al->res_ptr += val;
	return true;
}

static bool get_buffer(ALuint *buffer)
{
	if (!al->res_ptr)
	{
		if (nonblocking) {
			/* Try once, drop audio if no buffer available */
			if (!unqueue_buffers())
				return false;
		} else {
			for (;;)
			{
				if (unqueue_buffers())
					break;
			}
		}
	}

	*buffer = al->res_buf[--al->res_ptr];
	return true;
}

static size_t fill_internal_buf(const void *buf, size_t size)
{
	size_t read_size = MIN(BUFSIZE - al->tmpbuf_ptr, size);
	memcpy(al->tmpbuf + al->tmpbuf_ptr, buf, read_size);
	al->tmpbuf_ptr += read_size;
	return read_size;
}

static bool ensure_resample_capacity(size_t frames)
{
	int16_t *new_buf = NULL;
	size_t samples = 0;

	if (!al)
		return false;
	if (frames <= al->resample_cap_frames)
		return true;

	samples = frames * 2;
	new_buf = (int16_t *)realloc(al->resample_buf, samples * sizeof(int16_t));
	if (!new_buf)
		return false;

	al->resample_buf = new_buf;
	al->resample_cap_frames = frames;
	return true;
}

size_t audio_write(const void *buf_, unsigned size) {
	if (!al || !buf_ || !size)
		return size;

	const uint8_t *buf = (const uint8_t*)buf_;
	size_t written = 0;

	while (size)
	{
		ALint val;
		ALuint buffer;
		size_t rc = fill_internal_buf(buf, size);

		written += rc;
		buf += rc;
		size -= rc;

		if (al->tmpbuf_ptr != BUFSIZE)
			break;

		if (!get_buffer(&buffer))
			break;

		alBufferData(buffer, AL_FORMAT_STEREO16, al->tmpbuf, BUFSIZE, al->rate);
		al->tmpbuf_ptr = 0;
		alSourceQueueBuffers(al->source, 1, &buffer);
		if (alGetError() != AL_NO_ERROR)
			return -1;

		alGetSourcei(al->source, AL_SOURCE_STATE, &val);
		if (val != AL_PLAYING)
			alSourcePlay(al->source);

		if (alGetError() != AL_NO_ERROR)
			return -1;
	}

	return written;
}

void audio_deinit() {
	if (!al)
		return;

	alSourceStop(al->source);
	alDeleteSources(1, &al->source);

	if (al->buffers)
		alDeleteBuffers(NUMBUFFERS, al->buffers);

	free(al->buffers);
	free(al->res_buf);
	free(al->resample_buf);
	alcMakeContextCurrent(NULL);

	if (al->ctx)
		alcDestroyContext(al->ctx);
	if (al->handle)
		alcCloseDevice(al->handle);
	free(al);
	al = NULL;
}

void audio_init(int rate) {
	al = (al_t*)calloc(1, sizeof(al_t));
	if (!al)
		return;

	al->handle = alcOpenDevice(NULL);
	if (!al->handle)
		die("Could not init audio handle");

	al->ctx = alcCreateContext(al->handle, NULL);
	if (!al->ctx)
		die("Could not init audio context");

	alcMakeContextCurrent(al->ctx);

	al->input_rate = rate > 0 ? rate : DEFAULT_OUTPUT_RATE;
	al->rate = al->input_rate;
	al->needs_resample = al->input_rate > MAX_DIRECT_AUDIO_RATE;
	if (al->needs_resample) {
		al->rate = DEFAULT_OUTPUT_RATE;
		log_printf("audio", "clamping core sample_rate=%d to output_rate=%d with lightweight resampler",
			al->input_rate, al->rate);
	} else {
		log_printf("audio", "using direct audio rate=%d", al->rate);
	}
	al->buffers = (ALuint*)calloc(NUMBUFFERS, sizeof(ALuint));
	al->res_buf = (ALuint*)calloc(NUMBUFFERS, sizeof(ALuint));
	if (!al->buffers || !al->res_buf)
		die("Could not init audio buffers");

	alGenSources(1, &al->source);
	alGenBuffers(NUMBUFFERS, al->buffers);

	memcpy(al->res_buf, al->buffers, NUMBUFFERS * sizeof(ALuint));
	al->res_ptr = NUMBUFFERS;

	/* Apply saved volume */
	apply_gain();
}

void audio_sample(int16_t left, int16_t right) {
	int16_t buf[2] = {left, right};
	(void)audio_sample_batch(buf, 1);
}

size_t audio_sample_batch(const int16_t *data, size_t frames) {
	if (!al || !data || frames == 0)
		return 0;

	if (al->needs_resample) {
		size_t out_frames = 0;
		size_t max_out_frames = (frames * (size_t)al->rate) / (size_t)al->input_rate + 8;
		size_t i = 0;

		if (!ensure_resample_capacity(max_out_frames))
			return 0;

		for (i = 0; i < frames; i++) {
			al->resample_accum += (uint64_t)al->rate;
			if (al->resample_accum < (uint64_t)al->input_rate)
				continue;

			do {
				al->resample_buf[out_frames * 2 + 0] = data[i * 2 + 0];
				al->resample_buf[out_frames * 2 + 1] = data[i * 2 + 1];
				out_frames++;
				al->resample_accum -= (uint64_t)al->input_rate;
			} while (al->resample_accum >= (uint64_t)al->input_rate);
		}

		if (out_frames > 0) {
			size_t written = audio_write(al->resample_buf, (unsigned)(out_frames * 4));
			if (written == (size_t)-1)
				return 0;
		}

		/* Consumimos todos os frames de entrada, mesmo gerando menos frames de saida. */
		return frames;
	}

	size_t written = audio_write(data, (unsigned)(frames * 4));

	if (written == (size_t)-1)
		return 0;

	/* Libretro expects the number of frames consumed, not bytes. */
	return written / 4;
}

/* ── Volume / Mute API ── */

void audio_set_volume(int percent) {
	if (percent < 0)   percent = 0;
	if (percent > 100) percent = 100;
	current_volume = percent;
	apply_gain();
}

int audio_get_volume(void) {
	return current_volume;
}

void audio_set_mute(bool mute) {
	current_mute = mute;
	apply_gain();
}

bool audio_get_mute(void) {
	return current_mute;
}

void audio_set_nonblocking(bool enabled) {
	nonblocking = enabled;
}
