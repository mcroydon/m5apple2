/*
 * SPDX-License-Identifier: MIT
 * Performance counters and periodic logging for m5apple2.
 */

#ifdef ESP_PLATFORM

#include "app_perf.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#ifndef CONFIG_M5APPLE2_DETAILED_PERF_PROFILE
#define CONFIG_M5APPLE2_DETAILED_PERF_PROFILE 0
#endif

static const char *TAG = "m5apple2";

void app_perf_reset(app_perf_counters_t *perf, int64_t now_us)
{
    memset(perf, 0, sizeof(*perf));
    perf->window_start_us = now_us;
}

void app_perf_log(app_perf_counters_t *perf, int64_t now_us,
                  const char *speed_mode_name, unsigned speed_mult)
{
    const int64_t elapsed_us = now_us - perf->window_start_us;

    {
        const uint64_t effective_khz =
            (elapsed_us > 0) ? (perf->emulated_cycles * 1000ULL) / (uint64_t)elapsed_us : 0ULL;
        const uint32_t fps_x10 =
            (elapsed_us > 0) ? (uint32_t)((uint64_t)perf->frames_presented * 10000000ULL / (uint64_t)elapsed_us)
                             : 0U;
        const uint32_t cpu_pct =
            (elapsed_us > 0) ? (uint32_t)(perf->cpu_step_us * 100ULL / (uint64_t)elapsed_us) : 0U;
        const uint32_t compose_pct =
            (elapsed_us > 0) ? (uint32_t)(perf->frame_compose_us * 100ULL / (uint64_t)elapsed_us) : 0U;
        const uint32_t present_pct =
            (elapsed_us > 0) ? (uint32_t)(perf->frame_present_us * 100ULL / (uint64_t)elapsed_us) : 0U;

        ESP_LOGI(TAG,
                 "perf apple=%" PRIu64 ".%03" PRIu64 "MHz mode=%s rate=%ux fps=%" PRIu32 ".%" PRIu32
                 " cpu=%" PRIu32 "%% compose=%" PRIu32 "%% present=%" PRIu32
                 "%% frames=%" PRIu32 " text=%" PRIu32 " skip=%" PRIu32 " gfx=%" PRIu32,
                 effective_khz / 1000ULL,
                 effective_khz % 1000ULL,
                 speed_mode_name,
                 speed_mult,
                 fps_x10 / 10U,
                 fps_x10 % 10U,
                 cpu_pct,
                 compose_pct,
                 present_pct,
                 perf->frames_presented,
                 perf->text_frames,
                 perf->text_frames_skipped,
                 perf->graphics_frames);
#if CONFIG_M5APPLE2_DETAILED_PERF_PROFILE
        {
            const uint32_t cpu_disk_pct =
                (elapsed_us > 0) ? (uint32_t)(perf->cpu_step_disk_us * 100ULL / (uint64_t)elapsed_us) : 0U;
            const uint32_t cpu_idle_pct =
                (elapsed_us > 0) ? (uint32_t)(perf->cpu_step_idle_us * 100ULL / (uint64_t)elapsed_us) : 0U;

        ESP_LOGI(TAG,
                 "perf path disk_cpu=%" PRIu32 "%% idle_cpu=%" PRIu32 "%% act=%" PRIu32
                 " idle=%" PRIu32 " drv=%" PRIu32 " qt=%" PRIu32 " nib=%" PRIu32
                 " adv=%" PRIu64 " wrap=%" PRIu32
                 " probe=%" PRIu32 "ms/%" PRIu32 " mount=%" PRIu32 "ms/%" PRIu32,
                 cpu_disk_pct,
                 cpu_idle_pct,
                 perf->disk_active_slices,
                 perf->idle_slices,
                 perf->drive_change_slices,
                 perf->quarter_track_change_slices,
                 perf->nibble_progress_slices,
                 perf->nibble_advance_bytes,
                 perf->nibble_wrap_slices,
                 (uint32_t)(perf->dsk_probe_us / 1000ULL),
                 perf->dsk_probes,
                 (uint32_t)(perf->sd_mount_us / 1000ULL),
                 perf->sd_mounts);
        }
#endif
    }

    app_perf_reset(perf, now_us);
}

#endif /* ESP_PLATFORM */
