/*
 * SPDX-License-Identifier: MIT
 * SD card disk image management for m5apple2.
 */

#ifdef ESP_PLATFORM

#include "app_sd.h"

#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "apple2/apple2_disk2.h"
#include "apple2/apple2_machine.h"

static const char *TAG = "m5apple2_sd";

/* ---- Forward declarations ---- */

static bool app_sd_read_sector(void *context,
                               unsigned drive_index,
                               uint8_t track,
                               uint8_t file_sector,
                               uint8_t *sector_data);
static bool app_sd_write_sector(void *context,
                                unsigned drive_index,
                                uint8_t track,
                                uint8_t file_sector,
                                const uint8_t *sector_data);
static bool app_sd_read_track(void *context,
                              unsigned drive_index,
                              uint8_t quarter_track,
                              uint8_t *track_data,
                              uint16_t *track_length);

/* ---- Constants ---- */

#define APP_SD_MOUNT_RETRY_DELAY_MS  25
#define APP_SD_READ_RETRIES          3U
#define APP_SD_READ_RETRY_DELAY_MS   1U
#define APP_SD_TRACK_BYTES           (16U * 256U)

#define APP_DSK_PROBE_INSTRUCTIONS          900000U
#define APP_DSK_PROBE_FALLBACK_INSTRUCTIONS 3000000U

/* ---- Internal types ---- */

typedef enum {
    APP_DISK_ORDER_DOS33 = 0,
    APP_DISK_ORDER_PRODOS = 1,
} app_disk_order_t;

typedef enum {
    APP_DISK_SOURCE_MEMORY = 0,
    APP_DISK_SOURCE_READER = 1,
} app_disk_source_kind_t;

typedef struct {
    app_disk_source_kind_t kind;
    const uint8_t *image;
    apple2_disk2_read_sector_fn read_sector;
    void *context;
    size_t image_size;
} app_disk_source_t;

typedef struct {
    int score;
    uint16_t pc;
    uint8_t quarter_track;
    uint32_t nibble_pos;
    bool entered_stage2;
} app_dsk_probe_result_t;

typedef struct {
    FILE *file;
    app_disk_image_type_t type;
    apple2_disk2_image_order_t image_order;
    uint8_t *image_data;
    size_t image_size;
    apple2_woz_image_t *woz;
    bool sector_track_valid;
    uint8_t sector_track_index;
    uint8_t *sector_track_data;
} app_sd_drive_file_t;

/* ---- Module state ---- */

static app_sd_disk_entry_t s_sd_disks[APP_SD_MAX_IMAGES];
static app_sd_drive_file_t s_sd_drive_files[2];
static sdmmc_card_t *s_sd_card;
static size_t s_sd_disk_count;
static int s_sd_drive_index[2] = { -1, -1 };
static app_sd_dsk_order_override_t s_sd_dsk_order_override[2];
static bool s_sd_mounted;
static bool s_has_builtin[2];

static apple2_machine_t *s_probe_machine;  /* points to main machine during init */

static app_sd_restore_builtin_fn s_restore_builtin;
static app_sd_flush_fn s_flush;
static void *s_callback_ctx;

static app_sd_perf_t s_perf;

static const uint8_t s_prodos_track_order[16] = {
    0x0, 0x2, 0x4, 0x6, 0x8, 0xA, 0xC, 0xE,
    0x1, 0x3, 0x5, 0x7, 0x9, 0xB, 0xD, 0xF,
};

/* ---- Utility helpers ---- */

static app_disk_image_type_t app_disk_image_type_from_path(const char *path)
{
    const char *extension = strrchr(path, '.');

    if (extension == NULL) {
        return APP_DISK_IMAGE_NONE;
    }
    if (strcasecmp(extension, ".do") == 0) {
        return APP_DISK_IMAGE_DO;
    }
    if (strcasecmp(extension, ".po") == 0) {
        return APP_DISK_IMAGE_PO;
    }
    if (strcasecmp(extension, ".dsk") == 0) {
        return APP_DISK_IMAGE_DSK;
    }
    if (strcasecmp(extension, ".nib") == 0) {
        return APP_DISK_IMAGE_NIB;
    }
    if (strcasecmp(extension, ".woz") == 0) {
        return APP_DISK_IMAGE_WOZ;
    }
    return APP_DISK_IMAGE_NONE;
}

static long app_file_size(FILE *file)
{
    long size;

    if (file == NULL) {
        return -1L;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        return -1L;
    }
    size = ftell(file);
    if (size < 0L) {
        return -1L;
    }
    rewind(file);
    return size;
}

static bool app_read_exact(FILE *file, void *buffer, size_t size)
{
    return file != NULL && buffer != NULL && fread(buffer, 1, size, file) == size;
}

const char *app_sd_disk_image_type_name(app_disk_image_type_t type)
{
    switch (type) {
    case APP_DISK_IMAGE_DO:
        return "DO";
    case APP_DISK_IMAGE_PO:
        return "PO";
    case APP_DISK_IMAGE_DSK:
        return "DSK";
    case APP_DISK_IMAGE_NIB:
        return "NIB";
    case APP_DISK_IMAGE_WOZ:
        return "WOZ";
    case APP_DISK_IMAGE_NONE:
    default:
        return "none";
    }
}

/* ---- WOZ file parsing ---- */

static bool app_sd_parse_woz_file(FILE *file, apple2_woz_image_t *woz)
{
    uint8_t file_header[12];
    long file_size;
    bool is_woz1;
    long chunk_offset = 12L;
    bool have_info = false;
    bool have_tmap = false;
    bool have_trks = false;

    if (file == NULL || woz == NULL) {
        return false;
    }

    memset(woz, 0, sizeof(*woz));
    memset(woz->tmap, 0xFF, sizeof(woz->tmap));

    file_size = app_file_size(file);
    if (file_size < 12L || !app_read_exact(file, file_header, sizeof(file_header))) {
        return false;
    }

    is_woz1 = memcmp(file_header, "WOZ1", 4) == 0;
    woz->is_woz2 = memcmp(file_header, "WOZ2", 4) == 0;
    if ((!is_woz1 && !woz->is_woz2) ||
        file_header[4] != 0xFFU ||
        file_header[5] != 0x0AU ||
        file_header[6] != 0x0DU ||
        file_header[7] != 0x0AU) {
        return false;
    }

    while ((chunk_offset + 8L) <= file_size) {
        uint8_t chunk_header[8];
        uint32_t chunk_size;
        const long chunk_data_offset = chunk_offset + 8L;

        if (fseek(file, chunk_offset, SEEK_SET) != 0 || !app_read_exact(file, chunk_header, sizeof(chunk_header))) {
            return false;
        }
        chunk_size = ((uint32_t)chunk_header[4]) |
                     ((uint32_t)chunk_header[5] << 8) |
                     ((uint32_t)chunk_header[6] << 16) |
                     ((uint32_t)chunk_header[7] << 24);
        if (chunk_size > (uint32_t)(file_size - chunk_data_offset)) {
            return false;
        }

        if (memcmp(chunk_header, "INFO", 4) == 0) {
            uint8_t info_header[2];

            if (chunk_size < sizeof(info_header) ||
                fseek(file, chunk_data_offset, SEEK_SET) != 0 ||
                !app_read_exact(file, info_header, sizeof(info_header))) {
                return false;
            }
            have_info = info_header[1] == 1U;
        } else if (memcmp(chunk_header, "TMAP", 4) == 0) {
            if (chunk_size < APPLE2_DISK2_WOZ_TMAP_ENTRIES ||
                fseek(file, chunk_data_offset, SEEK_SET) != 0 ||
                !app_read_exact(file, woz->tmap, APPLE2_DISK2_WOZ_TMAP_ENTRIES)) {
                return false;
            }
            have_tmap = true;
        } else if (memcmp(chunk_header, "TRKS", 4) == 0) {
            if (is_woz1) {
                const size_t track_count = (size_t)chunk_size / 6656U;

                if (chunk_size == 0U || (chunk_size % 6656U) != 0U) {
                    return false;
                }
                for (size_t track_index = 0;
                     track_index < track_count && track_index < APPLE2_DISK2_WOZ_TMAP_ENTRIES;
                     ++track_index) {
                    const long track_offset = chunk_data_offset + (long)(track_index * 6656U);
                    uint8_t track_meta[4];
                    uint16_t byte_count;
                    uint32_t bit_count;

                    if (fseek(file, track_offset + 6646L, SEEK_SET) != 0 ||
                        !app_read_exact(file, track_meta, sizeof(track_meta))) {
                        return false;
                    }
                    byte_count = (uint16_t)track_meta[0] | (uint16_t)((uint16_t)track_meta[1] << 8);
                    bit_count = (uint32_t)track_meta[2] | ((uint32_t)track_meta[3] << 8);
                    if (byte_count == 0U && bit_count != 0U) {
                        byte_count = (uint16_t)((bit_count + 7U) / 8U);
                    }
                    if (byte_count == 0U) {
                        continue;
                    }
                    if (byte_count > 6646U || byte_count > APPLE2_DISK2_MAX_TRACK_BYTES) {
                        return false;
                    }

                    woz->tracks[track_index].offset = (uint32_t)track_offset;
                    woz->tracks[track_index].byte_count = byte_count;
                }
            } else {
                if (chunk_size < (APPLE2_DISK2_WOZ_TMAP_ENTRIES * 8U)) {
                    return false;
                }
                for (size_t track_index = 0; track_index < APPLE2_DISK2_WOZ_TMAP_ENTRIES; ++track_index) {
                    uint8_t track_entry[8];
                    uint16_t start_block;
                    uint16_t block_count;
                    uint32_t bit_count;
                    uint16_t byte_count;
                    uint32_t track_offset;

                    if (fseek(file, chunk_data_offset + (long)(track_index * 8U), SEEK_SET) != 0 ||
                        !app_read_exact(file, track_entry, sizeof(track_entry))) {
                        return false;
                    }
                    start_block = (uint16_t)track_entry[0] | (uint16_t)((uint16_t)track_entry[1] << 8);
                    block_count = (uint16_t)track_entry[2] | (uint16_t)((uint16_t)track_entry[3] << 8);
                    bit_count = ((uint32_t)track_entry[4]) |
                                ((uint32_t)track_entry[5] << 8) |
                                ((uint32_t)track_entry[6] << 16) |
                                ((uint32_t)track_entry[7] << 24);
                    if (start_block == 0U || block_count == 0U || bit_count == 0U) {
                        continue;
                    }

                    byte_count = (uint16_t)((bit_count + 7U) / 8U);
                    track_offset = (uint32_t)start_block * 512U;
                    if (byte_count > (uint32_t)block_count * 512U ||
                        byte_count > APPLE2_DISK2_MAX_TRACK_BYTES ||
                        track_offset > (uint32_t)file_size ||
                        byte_count > (uint32_t)file_size - track_offset) {
                        return false;
                    }

                    woz->tracks[track_index].offset = track_offset;
                    woz->tracks[track_index].byte_count = byte_count;
                }
            }
            have_trks = true;
        }

        chunk_offset = chunk_data_offset + (long)chunk_size;
    }

    if (!have_info || !have_tmap || !have_trks) {
        return false;
    }
    for (size_t i = 0; i < APPLE2_DISK2_WOZ_TMAP_ENTRIES; ++i) {
        const uint8_t track_index = woz->tmap[i];

        if (track_index == 0xFFU) {
            continue;
        }
        if (track_index >= APPLE2_DISK2_WOZ_TMAP_ENTRIES ||
            woz->tracks[track_index].byte_count == 0U) {
            return false;
        }
    }

    rewind(file);
    woz->valid = true;
    return true;
}

/* ---- SD read helpers ---- */

static bool app_sd_read_file_with_retries(FILE *file,
                                          long offset,
                                          void *buffer,
                                          size_t size,
                                          unsigned drive_index,
                                          const char *label,
                                          unsigned item_index)
{
    for (unsigned attempt = 0; attempt < APP_SD_READ_RETRIES; ++attempt) {
        clearerr(file);
        if (fseek(file, offset, SEEK_SET) == 0 && fread(buffer, 1, size, file) == size) {
            if (attempt > 0U) {
                ESP_LOGW(TAG,
                         "Recovered SD read drive=%u %s=%u offset=%ld size=%u after %u retries",
                         drive_index + 1U,
                         label,
                         item_index,
                         offset,
                         (unsigned)size,
                         attempt);
            }
            return true;
        }
        if ((attempt + 1U) < APP_SD_READ_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(APP_SD_READ_RETRY_DELAY_MS));
        }
    }

    ESP_LOGW(TAG,
             "SD read failed drive=%u %s=%u offset=%ld size=%u",
             drive_index + 1U,
             label,
             item_index,
             offset,
             (unsigned)size);
    return false;
}

static bool app_sd_file_valid(FILE *file, app_disk_image_type_t type, apple2_woz_image_t *woz)
{
    const long size = app_file_size(file);

    if (size < 0L) {
        return false;
    }

    switch (type) {
    case APP_DISK_IMAGE_DO:
    case APP_DISK_IMAGE_PO:
    case APP_DISK_IMAGE_DSK:
        return (size_t)size == APPLE2_DISK2_IMAGE_SIZE;
    case APP_DISK_IMAGE_NIB:
        return (size_t)size == APPLE2_DISK2_NIB_IMAGE_SIZE;
    case APP_DISK_IMAGE_WOZ: {
        apple2_woz_image_t parsed;

        return app_sd_parse_woz_file(file, (woz != NULL) ? woz : &parsed);
    }
    case APP_DISK_IMAGE_NONE:
    default:
        return false;
    }
}

/* ---- DSK order probing ---- */

#if defined(M5APPLE2_HAS_APPLE2PLUS_ROM) && defined(M5APPLE2_HAS_DISK2_ROM)

extern const uint8_t apple2plus_rom_start[] asm("_binary_apple2plus_rom_start");
extern const uint8_t apple2plus_rom_end[] asm("_binary_apple2plus_rom_end");
extern const uint8_t disk2_rom_start[] asm("_binary_disk2_rom_start");
extern const uint8_t disk2_rom_end[] asm("_binary_disk2_rom_end");

static unsigned app_count_nonzero_range(const apple2_machine_t *machine, uint16_t base, uint16_t size)
{
    unsigned nonzero = 0;

    for (uint16_t i = 0; i < size; ++i) {
        if (machine->memory[(uint16_t)(base + i)] != 0U) {
            nonzero++;
        }
    }

    return nonzero;
}

static unsigned app_find_file_sector_for_physical(const uint8_t *track_order, uint8_t physical_sector)
{
    for (unsigned file_sector = 0; file_sector < 16U; ++file_sector) {
        if (track_order[file_sector] == physical_sector) {
            return file_sector;
        }
    }

    return 0U;
}

static bool app_probe_read_sector_source(const app_disk_source_t *source,
                                         uint8_t track,
                                         uint8_t file_sector,
                                         uint8_t *sector_data)
{
    if (source->kind == APP_DISK_SOURCE_READER) {
        return source->read_sector != NULL &&
               source->read_sector(source->context, 0U, track, file_sector, sector_data);
    }

    if (source->image == NULL || source->image_size != APPLE2_DISK2_IMAGE_SIZE) {
        return false;
    }

    memcpy(sector_data,
           &source->image[(((size_t)track * 16U) + file_sector) * 256U],
           256U);
    return true;
}

static unsigned app_stage1_preload_match_score_source(const apple2_machine_t *machine,
                                                      const app_disk_source_t *source,
                                                      app_disk_order_t order)
{
    static const uint8_t s_boot_track_physical_sectors[10] = {
        0x0, 0xD, 0xB, 0x9, 0x7, 0x5, 0x3, 0x1, 0xE, 0xC,
    };
    unsigned matched_bytes = 0U;
    uint8_t sector_data[256];

    for (unsigned sector = 0; sector < 10U; ++sector) {
        const uint16_t address = (uint16_t)(0x3600U + sector * 0x0100U);
        unsigned file_sector = sector;

        if (order == APP_DISK_ORDER_PRODOS) {
            file_sector =
                app_find_file_sector_for_physical(s_prodos_track_order, s_boot_track_physical_sectors[sector]);
        }
        if (!app_probe_read_sector_source(source, 0U, (uint8_t)file_sector, sector_data)) {
            return 0U;
        }
        for (unsigned byte_index = 0; byte_index < 256U; ++byte_index) {
            if (machine->memory[(uint16_t)(address + byte_index)] == sector_data[byte_index]) {
                matched_bytes++;
            }
        }
    }

    return matched_bytes;
}

static bool app_attach_probe_drive(apple2_machine_t *probe,
                                   const app_disk_source_t *source,
                                   app_disk_order_t order)
{
    const apple2_disk2_image_order_t image_order =
        (order == APP_DISK_ORDER_PRODOS)
            ? APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL
            : APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;

    if (source->kind == APP_DISK_SOURCE_READER) {
        return apple2_disk2_attach_drive_reader(&probe->disk2,
                                                0,
                                                source->read_sector,
                                                source->context,
                                                source->image_size,
                                                image_order);
    }

    return apple2_disk2_load_drive_with_order(&probe->disk2,
                                              0,
                                              source->image,
                                              source->image_size,
                                              image_order);
}

static void app_release_probe_machine(void)
{
    /* The probe machine is borrowed from the caller (s_machine) — don't free it.
       Just clear the pointer so we don't use stale state. */
    s_probe_machine = NULL;
}

static int app_score_dsk_order_source(const app_disk_source_t *source,
                                      app_disk_order_t order,
                                      uint32_t instruction_limit,
                                      app_dsk_probe_result_t *result)
{
    apple2_machine_t *probe;
    bool entered_stage2 = false;

    if (s_probe_machine == NULL) {
        ESP_LOGW(TAG, "Skipping .dsk deep probe: no probe machine available");
        return INT_MIN / 2;
    }
    probe = s_probe_machine;

    /* A full probe machine is too large for the ESP-IDF main task stack. Keep
       the reusable probe machine on the heap instead of in .bss or on stack. */
    apple2_machine_init(probe, &(apple2_config_t){ .cpu_hz = CONFIG_M5APPLE2_CPU_HZ });
    if (!apple2_machine_load_system_rom(probe,
                                        apple2plus_rom_start,
                                        (size_t)(apple2plus_rom_end - apple2plus_rom_start)) ||
        !apple2_machine_load_slot6_rom(probe,
                                       disk2_rom_start,
                                       (size_t)(disk2_rom_end - disk2_rom_start))) {
        return INT_MIN / 2;
    }

    if (!app_attach_probe_drive(probe, source, order)) {
        return INT_MIN / 2;
    }

    for (uint32_t i = 0; i < instruction_limit; ++i) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(probe);

        if (cpu.pc >= 0x3700U && cpu.pc < 0x4000U) {
            entered_stage2 = true;
        }
        if (entered_stage2) {
            if (cpu.pc < 0x0100U) {
                break;
            }
            if (cpu.pc >= 0xFD00U && probe->disk2.nibble_pos[0] <= 384U) {
                break;
            }
        }
        apple2_machine_step_instruction(probe);
        if (instruction_limit > APP_DSK_PROBE_INSTRUCTIONS && (i % 4096U) == 0U) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(probe);
        const unsigned preload_match_score = app_stage1_preload_match_score_source(probe, source, order);
        int score = 0;

        score += (int)app_count_nonzero_range(probe, 0x1D00U, 0x0400U);
        score += (int)app_count_nonzero_range(probe, 0x2A00U, 0x0100U);
        score += (int)preload_match_score;
        if (preload_match_score == (10U * 256U)) {
            score += 1024;
        }
        if (probe->disk2.nibble_pos[0] > 384U) {
            score += 512;
        }
        if (probe->disk2.quarter_track[0] != 0U) {
            score += 128;
        }
        if (probe->disk2.quarter_track[0] >= 4U) {
            score += 256;
        }
        if (cpu.pc >= 0xB000U && cpu.pc < 0xC000U) {
            score += 512;
        }
        if (cpu.pc >= 0x3000U && cpu.pc < 0x4000U) {
            score += 128;
        }
        if (entered_stage2) {
            score += 64;
        }
        if (cpu.pc >= 0xC600U && cpu.pc < 0xC700U) {
            score -= 128;
        }
        if (cpu.pc < 0x0100U) {
            score -= 512;
        }
        if (cpu.pc >= 0xFD00U) {
            score -= 256;
        }
        if (result != NULL) {
            result->score = score;
            result->pc = cpu.pc;
            result->quarter_track = probe->disk2.quarter_track[0];
            result->nibble_pos = probe->disk2.nibble_pos[0];
            result->entered_stage2 = entered_stage2;
        }
        return score;
    }
}

static bool app_dsk_probe_result_is_slot6_stall(const app_dsk_probe_result_t *result)
{
    return result != NULL &&
           result->pc >= 0xC600U &&
           result->pc < 0xC700U &&
           result->quarter_track == 0U &&
           !result->entered_stage2;
}

static bool app_dsk_probe_result_is_monitor_fallback(const app_dsk_probe_result_t *result)
{
    return result != NULL &&
           result->pc >= 0xFD00U &&
           result->quarter_track == 0U &&
           !result->entered_stage2;
}

static bool app_dsk_probe_result_is_advanced_loader(const app_dsk_probe_result_t *result)
{
    return result != NULL &&
           (result->entered_stage2 ||
            result->quarter_track >= 4U ||
            (result->pc >= 0x0200U && result->pc < 0xC600U));
}

static int app_dsk_probe_result_progress_rank(const app_dsk_probe_result_t *result)
{
    int rank = 0;

    if (result == NULL) {
        return INT_MIN / 4;
    }
    if (app_dsk_probe_result_is_advanced_loader(result)) {
        rank += 4;
    }
    if (result->entered_stage2) {
        rank += 4;
    }
    if (result->quarter_track >= 4U) {
        rank += 2;
    }
    if (result->nibble_pos > 384U) {
        rank += 1;
    }
    if (result->pc >= 0xB000U && result->pc < 0xC000U) {
        rank += 2;
    } else if (result->pc >= 0x0200U && result->pc < 0xC600U) {
        rank += 1;
    }
    if (app_dsk_probe_result_is_monitor_fallback(result)) {
        rank -= 4;
    }
    if (app_dsk_probe_result_is_slot6_stall(result)) {
        rank -= 2;
    }

    return rank;
}

static bool app_dsk_probe_score_valid(int score)
{
    return score > (INT_MIN / 4);
}

static app_disk_order_t app_probe_dsk_order_source(const app_disk_source_t *source)
{
    app_dsk_probe_result_t dos_result = { 0 };
    app_dsk_probe_result_t prodos_result = { 0 };
    int dos_score =
        app_score_dsk_order_source(source, APP_DISK_ORDER_DOS33, APP_DSK_PROBE_INSTRUCTIONS, &dos_result);
    int prodos_score =
        app_score_dsk_order_source(source, APP_DISK_ORDER_PRODOS, APP_DSK_PROBE_INSTRUCTIONS, &prodos_result);

    if (app_dsk_probe_result_is_slot6_stall(&dos_result) &&
        app_dsk_probe_result_is_slot6_stall(&prodos_result)) {
        dos_score = app_score_dsk_order_source(source,
                                               APP_DISK_ORDER_DOS33,
                                               APP_DSK_PROBE_FALLBACK_INSTRUCTIONS,
                                               &dos_result);
        prodos_score = app_score_dsk_order_source(source,
                                                  APP_DISK_ORDER_PRODOS,
                                                  APP_DSK_PROBE_FALLBACK_INSTRUCTIONS,
                                                  &prodos_result);
        ESP_LOGI(TAG,
                 "Deep-probed .dsk order: dos=%d@%04x qt=%u np=%" PRIu32
                 " prodos=%d@%04x qt=%u np=%" PRIu32,
                 dos_score,
                 dos_result.pc,
                 dos_result.quarter_track,
                 dos_result.nibble_pos,
                 prodos_score,
                 prodos_result.pc,
                 prodos_result.quarter_track,
                 prodos_result.nibble_pos);
    }

    {
        const int dos_rank = app_dsk_probe_result_progress_rank(&dos_result);
        const int prodos_rank = app_dsk_probe_result_progress_rank(&prodos_result);

        if (dos_rank > prodos_rank) {
            ESP_LOGI(TAG,
                     "Probed .dsk order: dos=%d prodos=%d (DOS progress wins %d>%d)",
                     dos_score,
                     prodos_score,
                     dos_rank,
                     prodos_rank);
            return APP_DISK_ORDER_DOS33;
        }
        if (prodos_rank > dos_rank) {
            ESP_LOGI(TAG,
                     "Probed .dsk order: dos=%d prodos=%d (ProDOS progress wins %d>%d)",
                     dos_score,
                     prodos_score,
                     prodos_rank,
                     dos_rank);
            return APP_DISK_ORDER_PRODOS;
        }
    }

    if (!app_dsk_probe_score_valid(dos_score) && !app_dsk_probe_score_valid(prodos_score)) {
        ESP_LOGW(TAG,
                 "Probed .dsk order unavailable: dos=%d prodos=%d, defaulting to DOS",
                 dos_score,
                 prodos_score);
        return APP_DISK_ORDER_DOS33;
    }
    if (!app_dsk_probe_score_valid(prodos_score) && app_dsk_probe_score_valid(dos_score)) {
        ESP_LOGI(TAG,
                 "Probed .dsk order: dos=%d prodos=%d (DOS wins, ProDOS probe unavailable)",
                 dos_score,
                 prodos_score);
        return APP_DISK_ORDER_DOS33;
    }
    if (!app_dsk_probe_score_valid(dos_score) && app_dsk_probe_score_valid(prodos_score)) {
        ESP_LOGI(TAG,
                 "Probed .dsk order: dos=%d prodos=%d (ProDOS wins, DOS probe unavailable)",
                 dos_score,
                 prodos_score);
        return APP_DISK_ORDER_PRODOS;
    }

    if (app_dsk_probe_result_is_advanced_loader(&dos_result) &&
        app_dsk_probe_result_is_monitor_fallback(&prodos_result)) {
        ESP_LOGI(TAG, "Probed .dsk order: dos=%d prodos=%d (advanced DOS wins)", dos_score, prodos_score);
        return APP_DISK_ORDER_DOS33;
    }
    if (app_dsk_probe_result_is_advanced_loader(&prodos_result) &&
        app_dsk_probe_result_is_monitor_fallback(&dos_result)) {
        ESP_LOGI(TAG, "Probed .dsk order: dos=%d prodos=%d (advanced ProDOS wins)", dos_score, prodos_score);
        return APP_DISK_ORDER_PRODOS;
    }

    ESP_LOGI(TAG, "Probed .dsk order: dos=%d prodos=%d", dos_score, prodos_score);
    return (prodos_score >= dos_score) ? APP_DISK_ORDER_PRODOS : APP_DISK_ORDER_DOS33;
}

#endif /* M5APPLE2_HAS_APPLE2PLUS_ROM && M5APPLE2_HAS_DISK2_ROM */

static bool app_sd_probe_file_order(const char *path)
{
    app_sd_drive_file_t probe = { 0 };
    app_disk_source_t source = {
        .kind = APP_DISK_SOURCE_READER,
        .read_sector = app_sd_read_sector,
        .context = &probe,
        .image_size = APPLE2_DISK2_IMAGE_SIZE,
    };
    app_disk_order_t order = APP_DISK_ORDER_DOS33;
    const int64_t start_us = esp_timer_get_time();

    probe.file = fopen(path, "rb");
    if (probe.file == NULL) {
        return false;
    }
    if (!app_sd_file_valid(probe.file, APP_DISK_IMAGE_DSK, NULL)) {
        fclose(probe.file);
        return false;
    }
#if defined(M5APPLE2_HAS_APPLE2PLUS_ROM) && defined(M5APPLE2_HAS_DISK2_ROM)
    order = app_probe_dsk_order_source(&source);
#endif
    fclose(probe.file);
    free(probe.sector_track_data);
    app_release_probe_machine();
    s_perf.dsk_probe_us += (uint64_t)(esp_timer_get_time() - start_us);
    s_perf.dsk_probes++;
    return order == APP_DISK_ORDER_PRODOS;
}

bool app_sd_probe_dsk_image_is_prodos(const uint8_t *image, size_t image_size)
{
#if defined(M5APPLE2_HAS_APPLE2PLUS_ROM) && defined(M5APPLE2_HAS_DISK2_ROM)
    const app_disk_source_t source = {
        .kind = APP_DISK_SOURCE_MEMORY,
        .image = image,
        .image_size = image_size,
    };
    const app_disk_order_t order = app_probe_dsk_order_source(&source);

    app_release_probe_machine();
    return order == APP_DISK_ORDER_PRODOS;
#else
    (void)image;
    (void)image_size;
    return false;
#endif
}

/* ---- SD sector/track I/O callbacks ---- */

static bool app_sd_load_sector_image(app_sd_drive_file_t *drive, unsigned drive_index)
{
    if (drive == NULL || drive->file == NULL) {
        return false;
    }
    if (drive->image_data == NULL) {
        drive->image_data = malloc(APPLE2_DISK2_IMAGE_SIZE);
        if (drive->image_data == NULL) {
            return false;
        }
    }
    drive->image_size = APPLE2_DISK2_IMAGE_SIZE;
    if (!app_sd_read_file_with_retries(drive->file,
                                       0L,
                                       drive->image_data,
                                       drive->image_size,
                                       drive_index,
                                       "image",
                                       0U)) {
        return false;
    }
    fclose(drive->file);
    drive->file = NULL;
    return true;
}

static bool app_sd_attach_sector_image(apple2_machine_t *machine,
                                       unsigned drive_index,
                                       apple2_disk2_image_order_t image_order)
{
    app_sd_drive_file_t *drive = &s_sd_drive_files[drive_index];

    if (app_sd_load_sector_image(drive, drive_index)) {
        return apple2_disk2_load_drive_with_order(&machine->disk2,
                                                  drive_index,
                                                  drive->image_data,
                                                  drive->image_size,
                                                  image_order);
    }
    if (drive->image_data == NULL) {
        ESP_LOGW(TAG,
                 "Falling back to streamed sector reads for drive %u (no room for %u-byte cache)",
                 drive_index + 1U,
                 (unsigned)APPLE2_DISK2_IMAGE_SIZE);
    } else {
        ESP_LOGW(TAG,
                 "Falling back to streamed sector reads for drive %u after preload failed",
                 drive_index + 1U);
        free(drive->image_data);
        drive->image_data = NULL;
        drive->image_size = 0U;
    }
    return apple2_disk2_attach_drive_reader(&machine->disk2,
                                            drive_index,
                                            app_sd_read_sector,
                                            drive,
                                            APPLE2_DISK2_IMAGE_SIZE,
                                            image_order);
}

static bool app_sd_read_sector(void *context,
                               unsigned drive_index,
                               uint8_t track,
                               uint8_t file_sector,
                               uint8_t *sector_data)
{
    app_sd_drive_file_t *drive = context;
    const size_t sector_offset = (size_t)file_sector * 256U;

    (void)drive_index;
    if (drive == NULL || drive->file == NULL || sector_data == NULL) {
        ESP_LOGW(TAG, "read_sector: null ptr drive=%p file=%p data=%p",
                 (void *)drive, drive ? (void *)drive->file : NULL, (void *)sector_data);
        return false;
    }
    if (track >= 35U || file_sector >= 16U) {
        ESP_LOGW(TAG, "read_sector: out of range T%u S%u", track, file_sector);
        return false;
    }
    if (drive->sector_track_data == NULL) {
        drive->sector_track_data = malloc(APP_SD_TRACK_BYTES);
        if (drive->sector_track_data == NULL) {
            ESP_LOGW(TAG, "read_sector: failed to alloc %u-byte track buffer",
                     (unsigned)APP_SD_TRACK_BYTES);
            drive->sector_track_valid = false;
            return false;
        }
    }
    if (!drive->sector_track_valid || drive->sector_track_index != track) {
        const long offset = (long)((size_t)track * APP_SD_TRACK_BYTES);

        if (!app_sd_read_file_with_retries(drive->file,
                                           offset,
                                           drive->sector_track_data,
                                           APP_SD_TRACK_BYTES,
                                           drive_index,
                                           "track",
                                           track)) {
            drive->sector_track_valid = false;
            return false;
        }
        drive->sector_track_index = track;
        drive->sector_track_valid = true;
    }

    memcpy(sector_data, &drive->sector_track_data[sector_offset], 256U);
    return true;
}

static bool app_sd_write_sector(void *context,
                                unsigned drive_index,
                                uint8_t track,
                                uint8_t file_sector,
                                const uint8_t *sector_data)
{
    const int disk_index =
        (drive_index < 2U) ? s_sd_drive_index[drive_index] : -1;
    const char *path;
    FILE *file;
    long offset;

    (void)context;
    if (disk_index < 0 || (size_t)disk_index >= s_sd_disk_count) {
        ESP_LOGW(TAG, "SD write: no disk mapped for drive %u", drive_index + 1U);
        return false;
    }
    if (track >= 35U || file_sector >= 16U) {
        ESP_LOGW(TAG, "SD write: out of range track=%u sector=%u", track, file_sector);
        return false;
    }

    path = s_sd_disks[disk_index].path;
    offset = (long)(((size_t)track * 16U + file_sector) * 256U);

    file = fopen(path, "r+b");
    if (file == NULL) {
        ESP_LOGW(TAG, "SD write: failed to open %s", path);
        return false;
    }
    if (fseek(file, offset, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "SD write: seek failed offset=%ld %s", offset, path);
        fclose(file);
        return false;
    }
    if (fwrite(sector_data, 1, 256U, file) != 256U) {
        ESP_LOGW(TAG, "SD write: write failed offset=%ld %s", offset, path);
        fclose(file);
        return false;
    }
    fclose(file);
    return true;
}

static bool app_sd_read_track(void *context,
                              unsigned drive_index,
                              uint8_t quarter_track,
                              uint8_t *track_data,
                              uint16_t *track_length)
{
    app_sd_drive_file_t *drive = context;
    long offset;
    size_t length;

    if (drive == NULL || drive->file == NULL || track_data == NULL || track_length == NULL) {
        return false;
    }

    switch (drive->type) {
    case APP_DISK_IMAGE_NIB:
        offset = (long)(((size_t)(quarter_track / 4U)) * APPLE2_DISK2_NIB_TRACK_BYTES);
        length = APPLE2_DISK2_NIB_TRACK_BYTES;
        break;

    case APP_DISK_IMAGE_WOZ: {
        const uint8_t track_index =
            (drive->woz != NULL && quarter_track < APPLE2_DISK2_WOZ_TMAP_ENTRIES)
                ? drive->woz->tmap[quarter_track]
                : 0xFFU;

        if (track_index == 0xFFU ||
            track_index >= APPLE2_DISK2_WOZ_TMAP_ENTRIES ||
            drive->woz == NULL ||
            drive->woz->tracks[track_index].byte_count == 0U) {
            return false;
        }
        offset = (long)drive->woz->tracks[track_index].offset;
        length = drive->woz->tracks[track_index].byte_count;
        break;
    }

    default:
        return false;
    }

    if (length == 0U || length > APPLE2_DISK2_MAX_TRACK_BYTES) {
        return false;
    }
    if (!app_sd_read_file_with_retries(drive->file,
                                       offset,
                                       track_data,
                                       length,
                                       drive_index,
                                       "quarter_track",
                                       quarter_track)) {
        return false;
    }

    *track_length = (uint16_t)length;
    return true;
}

/* ---- SPI host selection ---- */

static spi_host_device_t app_sd_spi_host(void)
{
    spi_host_device_t host_id = (CONFIG_M5APPLE2_SD_HOST_ID == 3) ? SPI3_HOST : SPI2_HOST;
    const spi_host_device_t lcd_host_id = (CONFIG_M5APPLE2_LCD_HOST_ID == 3) ? SPI3_HOST : SPI2_HOST;
    const bool pins_match = CONFIG_M5APPLE2_SD_PIN_MOSI == CONFIG_M5APPLE2_LCD_PIN_MOSI &&
                            CONFIG_M5APPLE2_SD_PIN_CLK == CONFIG_M5APPLE2_LCD_PIN_CLK &&
                            CONFIG_M5APPLE2_SD_PIN_CS == CONFIG_M5APPLE2_LCD_PIN_CS;

    if (host_id == lcd_host_id && !pins_match) {
        const spi_host_device_t fallback = (lcd_host_id == SPI3_HOST) ? SPI2_HOST : SPI3_HOST;

        ESP_LOGW(TAG,
                 "SD SPI host %d conflicts with LCD host %d on different pins, using host %d",
                 (int)host_id,
                 (int)lcd_host_id,
                 (int)fallback);
        host_id = fallback;
    }

    return host_id;
}

/* ---- Filesystem mount ---- */

static bool app_sd_try_mount_filesystem(spi_host_device_t host_id,
                                        const spi_bus_config_t *bus_config,
                                        const esp_vfs_fat_sdmmc_mount_config_t *mount_config,
                                        int max_freq_khz,
                                        esp_err_t *err_out)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_err_t err;

    err = spi_bus_initialize(host_id, bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        if (err_out != NULL) {
            *err_out = err;
        }
        return false;
    }

    host.slot = host_id;
    host.max_freq_khz = max_freq_khz;
    slot_config.host_id = host_id;
    slot_config.gpio_cs = CONFIG_M5APPLE2_SD_PIN_CS;

    ESP_LOGI(TAG,
             "SD init host=%d cs=%d mosi=%d miso=%d clk=%d freq=%dkHz",
             (int)host_id,
             CONFIG_M5APPLE2_SD_PIN_CS,
             CONFIG_M5APPLE2_SD_PIN_MOSI,
             CONFIG_M5APPLE2_SD_PIN_MISO,
             CONFIG_M5APPLE2_SD_PIN_CLK,
             max_freq_khz);

    err = esp_vfs_fat_sdspi_mount(APP_SD_MOUNT_POINT, &host, &slot_config, mount_config, &s_sd_card);
    if (err_out != NULL) {
        *err_out = err;
    }
    return err == ESP_OK;
}

static bool app_sd_mount_filesystem(void)
{
#if !CONFIG_M5APPLE2_SD_ENABLE
    return false;
#else
    const spi_host_device_t host_id = app_sd_spi_host();
    const spi_bus_config_t bus_config = {
        .mosi_io_num = CONFIG_M5APPLE2_SD_PIN_MOSI,
        .miso_io_num = CONFIG_M5APPLE2_SD_PIN_MISO,
        .sclk_io_num = CONFIG_M5APPLE2_SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err;
    const int configured_freq_khz = CONFIG_M5APPLE2_SD_MAX_FREQ_KHZ;
    static const int s_fallback_freqs_khz[] = { 10000, 4000, 1000 };

    if (s_sd_mounted) {
        return true;
    }

    s_sd_card = NULL;
    if (app_sd_try_mount_filesystem(host_id, &bus_config, &mount_config, configured_freq_khz, &err)) {
        s_sd_mounted = true;
        ESP_LOGI(TAG, "Mounted SD card at %s", APP_SD_MOUNT_POINT);
        return true;
    }
    ESP_LOGW(TAG, "SD mount attempt failed at %dkHz: %s", configured_freq_khz, esp_err_to_name(err));
    s_sd_card = NULL;
    (void)spi_bus_free(host_id);

    for (size_t i = 0; i < (sizeof(s_fallback_freqs_khz) / sizeof(s_fallback_freqs_khz[0])); ++i) {
        const int freq_khz = s_fallback_freqs_khz[i];

        if (freq_khz >= configured_freq_khz) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_SD_MOUNT_RETRY_DELAY_MS));
        if (app_sd_try_mount_filesystem(host_id, &bus_config, &mount_config, freq_khz, &err)) {
            s_sd_mounted = true;
            ESP_LOGI(TAG, "Mounted SD card at %s", APP_SD_MOUNT_POINT);
            return true;
        }
        ESP_LOGW(TAG, "SD mount attempt failed at %dkHz: %s", freq_khz, esp_err_to_name(err));
        s_sd_card = NULL;
        (void)spi_bus_free(host_id);
    }

    ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
    return false;
#endif
}

/* ---- Directory scanning ---- */

static int app_sd_disk_compare(const void *lhs, const void *rhs)
{
    const app_sd_disk_entry_t *left = lhs;
    const app_sd_disk_entry_t *right = rhs;

    if (left->is_directory != right->is_directory) {
        return left->is_directory ? -1 : 1;
    }
    return strcasecmp(left->name, right->name);
}

bool app_sd_scan_directory(const char *dir_path)
{
    DIR *dir;
    struct dirent *entry;

    s_sd_disk_count = 0U;
    dir = opendir(dir_path);
    if (dir == NULL) {
        ESP_LOGW(TAG, "Could not open %s", dir_path);
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        app_disk_image_type_t type;
        app_sd_disk_entry_t *disk;
        char candidate_path[APP_SD_PATH_MAX];
        size_t name_len;
        int path_len;

        if (entry->d_name[0] == '.') {
            continue;
        }

        if (s_sd_disk_count >= APP_SD_MAX_IMAGES) {
            ESP_LOGW(TAG, "Ignoring extra SD entries beyond %u", (unsigned)APP_SD_MAX_IMAGES);
            break;
        }

        name_len = strnlen(entry->d_name, sizeof(((app_sd_disk_entry_t *)0)->path));
        if (name_len == 0U || name_len >= sizeof(((app_sd_disk_entry_t *)0)->name)) {
            ESP_LOGW(TAG, "Skipping SD entry name that is too long: %s", entry->d_name);
            continue;
        }

        path_len = snprintf(candidate_path, sizeof(candidate_path),
                            "%s/%s", dir_path, entry->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(candidate_path)) {
            ESP_LOGW(TAG, "Skipping SD entry path that is too long: %s/%s",
                     dir_path, entry->d_name);
            continue;
        }

        if (entry->d_type == DT_DIR) {
            disk = &s_sd_disks[s_sd_disk_count++];
            memcpy(disk->path, candidate_path, (size_t)path_len + 1U);
            memcpy(disk->name, entry->d_name, name_len);
            disk->name[name_len] = '\0';
            disk->type = APP_DISK_IMAGE_NONE;
            disk->is_directory = true;
            continue;
        }

        type = app_disk_image_type_from_path(entry->d_name);
        if (type == APP_DISK_IMAGE_NONE) {
            continue;
        }

        {
            FILE *candidate_file = fopen(candidate_path, "rb");
            if (candidate_file == NULL) {
                ESP_LOGW(TAG, "Skipping unreadable SD disk candidate: %s", entry->d_name);
                continue;
            }
            if (!app_sd_file_valid(candidate_file, type, NULL)) {
                fclose(candidate_file);
                ESP_LOGI(TAG, "Skipping unsupported SD disk candidate: %s", entry->d_name);
                continue;
            }
            fclose(candidate_file);
        }

        disk = &s_sd_disks[s_sd_disk_count++];
        memcpy(disk->path, candidate_path, (size_t)path_len + 1U);
        memcpy(disk->name, entry->d_name, name_len);
        disk->name[name_len] = '\0';
        disk->type = type;
        disk->is_directory = false;
    }

    closedir(dir);
    qsort(s_sd_disks, s_sd_disk_count, sizeof(s_sd_disks[0]), app_sd_disk_compare);
    ESP_LOGI(TAG, "Found %u SD entry(ies) in %s", (unsigned)s_sd_disk_count, dir_path);
    return true;
}

/* ---- Drive management ---- */

void app_sd_close_drive(apple2_machine_t *machine, unsigned drive_index)
{
    if (drive_index >= 2U) {
        return;
    }
    apple2_disk2_unload_drive(&machine->disk2, drive_index);
    if (s_sd_drive_files[drive_index].file != NULL) {
        fclose(s_sd_drive_files[drive_index].file);
        s_sd_drive_files[drive_index].file = NULL;
    }
    s_sd_drive_files[drive_index].type = APP_DISK_IMAGE_NONE;
    s_sd_drive_files[drive_index].image_order = APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
    /* Keep the pre-allocated image buffer for reuse — freeing it risks
       heap fragmentation preventing re-allocation on the next mount. */
    s_sd_drive_files[drive_index].image_size = 0U;
    s_sd_drive_files[drive_index].sector_track_valid = false;
    free(s_sd_drive_files[drive_index].sector_track_data);
    s_sd_drive_files[drive_index].sector_track_data = NULL;
    free(s_sd_drive_files[drive_index].woz);
    s_sd_drive_files[drive_index].woz = NULL;
}

bool app_sd_mount_disk(apple2_machine_t *machine, unsigned drive_index, size_t disk_index)
{
    const app_sd_disk_entry_t *disk;
    apple2_disk2_image_order_t image_order = APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
    bool attached = false;
    const int64_t mount_start_us = esp_timer_get_time();

    if (drive_index >= 2U || disk_index >= s_sd_disk_count) {
        return false;
    }

    disk = &s_sd_disks[disk_index];
    app_sd_close_drive(machine, drive_index);
    s_sd_drive_files[drive_index].file = fopen(disk->path, "rb");
    if (s_sd_drive_files[drive_index].file == NULL) {
        ESP_LOGW(TAG, "Failed to open SD disk %s", disk->name);
        return false;
    }
    s_sd_drive_files[drive_index].type = disk->type;
    s_sd_drive_files[drive_index].image_order = APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
    if (disk->type == APP_DISK_IMAGE_WOZ && s_sd_drive_files[drive_index].woz == NULL) {
        s_sd_drive_files[drive_index].woz = calloc(1U, sizeof(*s_sd_drive_files[drive_index].woz));
        if (s_sd_drive_files[drive_index].woz == NULL) {
            app_sd_close_drive(machine, drive_index);
            ESP_LOGW(TAG, "Failed to allocate WOZ metadata for %s", disk->name);
            return false;
        }
    }
    if (!app_sd_file_valid(s_sd_drive_files[drive_index].file,
                           disk->type,
                           s_sd_drive_files[drive_index].woz)) {
        ESP_LOGW(TAG, "SD disk %s is not a supported image", disk->name);
        app_sd_close_drive(machine, drive_index);
        return false;
    }

    switch (disk->type) {
    case APP_DISK_IMAGE_PO:
        image_order = APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL;
        s_sd_drive_files[drive_index].image_order = image_order;
        attached = app_sd_attach_sector_image(machine, drive_index, image_order);
        break;
    case APP_DISK_IMAGE_DSK:
        switch (s_sd_dsk_order_override[drive_index]) {
        case APP_SD_DSK_ORDER_DOS33:
            image_order = APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
            break;
        case APP_SD_DSK_ORDER_PRODOS:
            image_order = APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL;
            break;
        case APP_SD_DSK_ORDER_AUTO:
        default:
            image_order = app_sd_probe_file_order(disk->path)
                              ? APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL
                              : APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
            break;
        }
        s_sd_drive_files[drive_index].image_order = image_order;
        attached = app_sd_attach_sector_image(machine, drive_index, image_order);
        break;
    case APP_DISK_IMAGE_NIB:
    case APP_DISK_IMAGE_WOZ:
        attached = apple2_disk2_attach_drive_track_reader(&machine->disk2,
                                                          drive_index,
                                                          app_sd_read_track,
                                                          &s_sd_drive_files[drive_index]);
        break;
    case APP_DISK_IMAGE_DO:
    default:
        image_order = APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
        s_sd_drive_files[drive_index].image_order = image_order;
        attached = app_sd_attach_sector_image(machine, drive_index, image_order);
        break;
    }

    if (!attached) {
        app_sd_close_drive(machine, drive_index);
        ESP_LOGW(TAG, "Failed to attach SD disk %s", disk->name);
        return false;
    }

    s_sd_drive_index[drive_index] = (int)disk_index;

    switch (disk->type) {
    case APP_DISK_IMAGE_PO:
    case APP_DISK_IMAGE_DSK:
    case APP_DISK_IMAGE_DO:
        apple2_disk2_attach_drive_writer(&machine->disk2, drive_index,
                                          app_sd_write_sector,
                                          &s_sd_drive_files[drive_index]);
        break;
    default:
        break;
    }

    s_perf.sd_mount_us += (uint64_t)(esp_timer_get_time() - mount_start_us);
    s_perf.sd_mounts++;
    switch (disk->type) {
    case APP_DISK_IMAGE_PO:
    case APP_DISK_IMAGE_DSK:
    case APP_DISK_IMAGE_DO:
        if (disk->type == APP_DISK_IMAGE_DSK) {
            ESP_LOGI(TAG,
                     "Mounted SD disk %s in drive %u (%s order, override=%s)",
                     disk->name,
                     (unsigned)(drive_index + 1U),
                     (image_order == APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL) ? "ProDOS" : "DOS",
                     app_sd_dsk_order_override_name(s_sd_dsk_order_override[drive_index]));
        } else {
            ESP_LOGI(TAG,
                     "Mounted SD disk %s in drive %u (%s order)",
                     disk->name,
                     (unsigned)(drive_index + 1U),
                     (image_order == APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL) ? "ProDOS" : "DOS");
        }
        break;
    case APP_DISK_IMAGE_NIB:
        ESP_LOGI(TAG, "Mounted SD disk %s in drive %u (NIB)", disk->name, (unsigned)(drive_index + 1U));
        break;
    case APP_DISK_IMAGE_WOZ:
        ESP_LOGI(TAG, "Mounted SD disk %s in drive %u (WOZ)", disk->name, (unsigned)(drive_index + 1U));
        break;
    case APP_DISK_IMAGE_NONE:
    default:
        break;
    }
    return true;
}

void app_sd_restore_default_drives(apple2_machine_t *machine)
{
    app_sd_close_drive(machine, 0U);
    app_sd_close_drive(machine, 1U);
    s_sd_drive_index[0] = -1;
    s_sd_drive_index[1] = -1;

    if (s_has_builtin[0]) {
        if (s_restore_builtin != NULL) {
            (void)s_restore_builtin(0U, s_callback_ctx);
        }
    } else {
        apple2_disk2_unload_drive(&machine->disk2, 0U);
    }

    if (s_has_builtin[1]) {
        if (s_restore_builtin != NULL) {
            (void)s_restore_builtin(1U, s_callback_ctx);
        }
    } else {
        apple2_disk2_unload_drive(&machine->disk2, 1U);
    }

    if (s_sd_disk_count == 0U) {
        return;
    }

    if (s_has_builtin[0]) {
        (void)app_sd_mount_disk(machine, 1U, 0U);
    } else {
        (void)app_sd_mount_disk(machine, 0U, 0U);
        if (s_sd_disk_count > 1U) {
            (void)app_sd_mount_disk(machine, 1U, 1U);
        }
    }
}

void app_sd_rescan(apple2_machine_t *machine)
{
#if !CONFIG_M5APPLE2_SD_ENABLE
    (void)machine;
    return;
#else
    if (!app_sd_mount_filesystem()) {
        return;
    }
    if (!app_sd_scan_directory(APP_SD_MOUNT_POINT)) {
        return;
    }
    app_sd_restore_default_drives(machine);
#endif
}

void app_sd_cycle_drive(apple2_machine_t *machine, unsigned drive_index)
{
#if !CONFIG_M5APPLE2_SD_ENABLE
    (void)machine;
    (void)drive_index;
    ESP_LOGW(TAG, "SD disk library is disabled in menuconfig");
    return;
#else
    int next_index;

    if (!s_sd_mounted || s_sd_disk_count == 0U || drive_index >= 2U) {
        ESP_LOGW(TAG, "No SD disk library available for drive %u", (unsigned)(drive_index + 1U));
        return;
    }

    app_sd_flush_dirty_tracks(machine);

    next_index = s_sd_drive_index[drive_index];
    if (++next_index >= (int)s_sd_disk_count) {
        next_index = -1;
    }

    if (next_index < 0) {
        app_sd_close_drive(machine, drive_index);
        s_sd_drive_index[drive_index] = -1;
        if (s_has_builtin[drive_index]) {
            if (s_restore_builtin != NULL && s_restore_builtin(drive_index, s_callback_ctx)) {
                ESP_LOGI(TAG,
                         "Restored embedded disk in drive %u",
                         (unsigned)(drive_index + 1U));
            }
        } else {
            apple2_disk2_unload_drive(&machine->disk2, drive_index);
            ESP_LOGI(TAG, "Ejected drive %u", (unsigned)(drive_index + 1U));
        }
        return;
    }

    if (!app_sd_mount_disk(machine, drive_index, (size_t)next_index)) {
        app_sd_close_drive(machine, drive_index);
        if (s_has_builtin[drive_index]) {
            if (s_restore_builtin != NULL) {
                (void)s_restore_builtin(drive_index, s_callback_ctx);
            }
        } else {
            apple2_disk2_unload_drive(&machine->disk2, drive_index);
        }
    }
#endif
}

void app_sd_cycle_dsk_order(apple2_machine_t *machine, unsigned drive_index)
{
#if !CONFIG_M5APPLE2_SD_ENABLE
    (void)machine;
    (void)drive_index;
    ESP_LOGW(TAG, "SD disk library is disabled in menuconfig");
    return;
#else
    const int disk_index =
        (drive_index < 2U) ? s_sd_drive_index[drive_index] : -1;
    app_sd_dsk_order_override_t previous_override;

    if (drive_index >= 2U || disk_index < 0 || (size_t)disk_index >= s_sd_disk_count) {
        ESP_LOGW(TAG, "No mounted SD disk available for drive %u order override", (unsigned)(drive_index + 1U));
        return;
    }
    if (s_sd_disks[disk_index].type != APP_DISK_IMAGE_DSK) {
        ESP_LOGW(TAG, "Drive %u order override only applies to .dsk images", (unsigned)(drive_index + 1U));
        return;
    }

    previous_override = s_sd_dsk_order_override[drive_index];
    s_sd_dsk_order_override[drive_index] =
        (app_sd_dsk_order_override_t)(((unsigned)s_sd_dsk_order_override[drive_index] + 1U) % 3U);
    if (!app_sd_mount_disk(machine, drive_index, (size_t)disk_index)) {
        s_sd_dsk_order_override[drive_index] = previous_override;
        (void)app_sd_mount_disk(machine, drive_index, (size_t)disk_index);
        ESP_LOGW(TAG, "Failed to apply .dsk order override on drive %u", (unsigned)(drive_index + 1U));
        return;
    }

    ESP_LOGI(TAG,
             "Drive %u .dsk order override -> %s",
             (unsigned)(drive_index + 1U),
             app_sd_dsk_order_override_name(s_sd_dsk_order_override[drive_index]));
#endif
}

/* ---- Public accessors ---- */

size_t app_sd_disk_count(void)
{
    return s_sd_disk_count;
}

const app_sd_disk_entry_t *app_sd_disk_entry(size_t index)
{
    if (index >= s_sd_disk_count) {
        return NULL;
    }
    return &s_sd_disks[index];
}

int app_sd_drive_index(unsigned drive_index)
{
    if (drive_index >= 2U) {
        return -1;
    }
    return s_sd_drive_index[drive_index];
}

void app_sd_set_drive_index(unsigned drive_index, int disk_index)
{
    if (drive_index < 2U) {
        s_sd_drive_index[drive_index] = disk_index;
    }
}

bool app_sd_is_mounted(void)
{
    return s_sd_mounted;
}

bool app_sd_has_builtin(unsigned drive_index)
{
    if (drive_index >= 2U) {
        return false;
    }
    return s_has_builtin[drive_index];
}

void app_sd_set_has_builtin(unsigned drive_index, bool present)
{
    if (drive_index < 2U) {
        s_has_builtin[drive_index] = present;
    }
}

const char *app_sd_dsk_order_override_name(app_sd_dsk_order_override_t override)
{
    switch (override) {
    case APP_SD_DSK_ORDER_DOS33:
        return "DOS";
    case APP_SD_DSK_ORDER_PRODOS:
        return "ProDOS";
    case APP_SD_DSK_ORDER_AUTO:
    default:
        return "auto";
    }
}

app_sd_dsk_order_override_t app_sd_dsk_order_override(unsigned drive_index)
{
    if (drive_index >= 2U) {
        return APP_SD_DSK_ORDER_AUTO;
    }
    return s_sd_dsk_order_override[drive_index];
}

apple2_disk2_image_order_t app_sd_drive_image_order(unsigned drive_index)
{
    if (drive_index >= 2U) {
        return APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
    }
    return s_sd_drive_files[drive_index].image_order;
}

void app_sd_flush_dirty_tracks(apple2_machine_t *machine)
{
    if (s_flush != NULL) {
        s_flush(s_callback_ctx);
    } else {
        if (!apple2_disk2_flush(&machine->disk2)) {
            ESP_LOGW(TAG, "disk flush failed");
        }
    }
}

app_sd_perf_t app_sd_perf_read(bool reset)
{
    app_sd_perf_t result = s_perf;

    if (reset) {
        memset(&s_perf, 0, sizeof(s_perf));
    }
    return result;
}

/* ---- Init ---- */

void app_sd_init(apple2_machine_t *machine,
                 app_sd_restore_builtin_fn restore_builtin,
                 app_sd_flush_fn flush,
                 void *callback_ctx)
{
    s_restore_builtin = restore_builtin;
    s_flush = flush;
    s_callback_ctx = callback_ctx;
    s_probe_machine = machine;  /* borrow main machine for .dsk order probing */

    /* Pre-allocate sector image cache for drive 1 before the heap gets
       fragmented by SD/FAT and other subsystems.  Only one drive needs
       the full 143 KB cache; drive 2 can use streamed reads or share
       the cache if drive 1 is swapped out. */
    if (s_sd_drive_files[0].image_data == NULL) {
        s_sd_drive_files[0].image_data = malloc(APPLE2_DISK2_IMAGE_SIZE);
        if (s_sd_drive_files[0].image_data != NULL) {
            s_sd_drive_files[0].image_size = APPLE2_DISK2_IMAGE_SIZE;
            ESP_LOGI(TAG, "Pre-allocated %u-byte sector cache for drive 1",
                     (unsigned)APPLE2_DISK2_IMAGE_SIZE);
        }
    }

    ESP_LOGI(TAG, "Free heap before SD init: %lu bytes (largest block: %lu)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    app_sd_rescan(machine);
    ESP_LOGI(TAG, "Free heap after SD init: %lu bytes (largest block: %lu)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

#endif /* ESP_PLATFORM */
