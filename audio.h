#ifndef AUDIO_H
#define AUDIO_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

void   audio_init(int frequency);
void   audio_deinit(void);
void   audio_sample(int16_t left, int16_t right);
size_t audio_sample_batch(const int16_t *data, size_t frames);

/* Volume: 0–100.  Uses OpenAL AL_GAIN on the source. */
void   audio_set_volume(int percent);
int    audio_get_volume(void);

/* Mute: sets gain to 0, restores on unmute. */
void   audio_set_mute(bool mute);
bool   audio_get_mute(void);

/* Non-blocking mode: when true, audio_write drops samples
 * instead of waiting for buffers (used during fast forward). */
void   audio_set_nonblocking(bool enabled);

/* Pull audio from cores using SET_AUDIO_CALLBACK (MAME, etc).
 * Called from main loop to refill OpenAL buffers. */
void   audio_callback_pull(void);

/* Dedicated audio thread for pull-model cores (MAME etc).
 * Start after audio_init() + set_state(true).
 * Stop before core_unload(). */
void   audio_start_thread(void);
void   audio_stop_thread(void);

#endif /* AUDIO_H */
