/* ---------------------------------------------------------------
 * audio.c — OpenAL audio with volume/mute control via AL_GAIN.
 * --------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __APPLE__
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

#define BUFSIZE 1024*4
#define NUMBUFFERS 4

#ifndef MIN
#define MIN(x,y)	((x) <= (y) ? (x) : (y))
#endif

#include "audio.h"
#include "utils.h"
#include "libretro.h"

extern struct retro_audio_callback audio_cb;
extern bool audio_cb_active;

/* Bytes recebidos do core via audio_sample_batch durante um pull.
 * Usado como sinal de progresso para sair do loop quando o core para
 * de produzir áudio. Acessado apenas da main thread. */
static volatile size_t pull_bytes_received = 0;

/* ── Dedicated audio thread for pull-model cores ── */
static pthread_t audio_thread;
static volatile bool audio_thread_running = false;

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
static int  current_volume = 100;   /* 0-200 */
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

void audio_callback_pull(void)
{
	if (!audio_cb_active || !audio_cb.callback || !al)
		return;

	/* Libera buffers ja tocados de volta para o pool antes de pedir mais audio */
	unqueue_buffers();

	const size_t target_bytes = (size_t)BUFSIZE * 2;
	int max_iter = 1024;
	int stale = 0;

	pull_bytes_received = 0;
	while (pull_bytes_received < target_bytes && max_iter-- > 0) {
		ALint queued = 0, processed = 0;
		alGetSourcei(al->source, AL_BUFFERS_QUEUED, &queued);
		alGetSourcei(al->source, AL_BUFFERS_PROCESSED, &processed);

		if ((queued - processed) >= NUMBUFFERS && processed == 0)
			break;

		size_t before = pull_bytes_received;
		audio_cb.callback();

		if (pull_bytes_received == before) {
			if (++stale >= 4)
				break;
		} else {
			stale = 0;
		}

		unqueue_buffers();
	}
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

	pull_bytes_received += frames * sizeof(int16_t) * 2;

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
	if (percent > 200) percent = 200;
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

/* ── Dedicated audio thread ── */

static void *audio_thread_func(void *arg)
{
	(void)arg;

	while (audio_thread_running) {
		if (!audio_cb_active || !audio_cb.callback || !al) {
			/* Core not ready yet or callback cleared — wait a bit */
			struct timespec ts = {0, 1000000}; /* 1 ms */
			nanosleep(&ts, NULL);
			continue;
		}

		/* Call the core's audio callback.
		 * The core will call audio_sample_batch -> audio_write -> get_buffer.
		 * When nonblocking==false (normal play), get_buffer blocks until an
		 * OpenAL buffer is free, naturally pacing this loop to the hardware
		 * audio rate (~48000 Hz / BUFSIZE).  This is exactly how RetroArch
		 * paces its audio thread. */
		audio_cb.callback();

		/* During fast-forward, nonblocking==true and get_buffer never blocks.
		 * Without a sleep the thread would spin at 100% CPU. */
		if (nonblocking) {
			struct timespec ts = {0, 1000000}; /* 1 ms */
			nanosleep(&ts, NULL);
		}
	}

	return NULL;
}

void audio_start_thread(void)
{
	if (audio_thread_running)
		return;

	audio_thread_running = true;
	if (pthread_create(&audio_thread, NULL, audio_thread_func, NULL) != 0) {
		audio_thread_running = false;
		log_printf("audio", "failed to create audio thread");
		return;
	}

	log_printf("audio", "audio thread started");
}

void audio_stop_thread(void)
{
	if (!audio_thread_running)
		return;

	audio_thread_running = false;
	pthread_join(audio_thread, NULL);
	log_printf("audio", "audio thread stopped");
}
