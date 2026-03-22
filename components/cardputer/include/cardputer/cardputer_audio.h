#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct cardputer_audio cardputer_audio_t;

/**
 * Initialise I2S audio output.
 * Selects ES8311 (Original) or NS4168 (ADV) based on Kconfig variant.
 * Returns NULL on failure.
 */
cardputer_audio_t *cardputer_audio_init(void);

/**
 * Record a speaker toggle at the given CPU cycle count.
 * Call from the emulation loop each time $C030 is accessed.
 */
void cardputer_audio_toggle(cardputer_audio_t *audio, uint64_t cpu_cycle);

/**
 * Fill the I2S output buffer with samples covering cycles up to `cpu_cycle`.
 * Call once per frame or CPU step slice from the main loop.
 */
void cardputer_audio_flush(cardputer_audio_t *audio, uint64_t cpu_cycle, uint32_t cpu_hz);

/**
 * Shut down I2S and free resources.
 */
void cardputer_audio_deinit(cardputer_audio_t *audio);
