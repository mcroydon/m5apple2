/*
 * SPDX-License-Identifier: MIT
 * Performance counters and periodic logging for m5apple2.
 */

#ifndef APP_PERF_H
#define APP_PERF_H

#include <stdint.h>

typedef struct {
    int64_t window_start_us;
    uint64_t emulated_cycles;
    uint64_t cpu_step_us;
    uint64_t cpu_step_disk_us;
    uint64_t cpu_step_idle_us;
    uint64_t frame_compose_us;
    uint64_t frame_present_us;
    uint64_t dsk_probe_us;
    uint64_t sd_mount_us;
    uint32_t frames_presented;
    uint32_t text_frames;
    uint32_t text_frames_skipped;
    uint32_t graphics_frames;
    uint32_t dsk_probes;
    uint32_t sd_mounts;
    uint32_t disk_active_slices;
    uint32_t idle_slices;
    uint32_t drive_change_slices;
    uint32_t quarter_track_change_slices;
    uint32_t nibble_progress_slices;
    uint64_t nibble_advance_bytes;
    uint32_t nibble_wrap_slices;
} app_perf_counters_t;

/**
 * Zero all counters and set window_start_us to @p now_us.
 */
void app_perf_reset(app_perf_counters_t *perf, int64_t now_us);

/**
 * Log accumulated performance stats and reset the counters.
 *
 * @param perf            Counters accumulated since the last reset.
 * @param now_us          Current timestamp (microseconds).
 * @param speed_mode_name Short label for the current speed mode (e.g. "1x").
 * @param speed_mult      Current speed multiplier (1, 2, 4, ...).
 */
void app_perf_log(app_perf_counters_t *perf, int64_t now_us,
                  const char *speed_mode_name, unsigned speed_mult);

#endif /* APP_PERF_H */
