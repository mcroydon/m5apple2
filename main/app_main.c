#include <inttypes.h>
#include <limits.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "apple2/apple2_machine.h"
#include "apple2/apple2_video.h"
#include "cardputer/cardputer_display.h"
#include "cardputer/cardputer_keyboard.h"

#ifndef CONFIG_M5APPLE2_PERF_LOG_INTERVAL_MS
#define CONFIG_M5APPLE2_PERF_LOG_INTERVAL_MS 2000
#endif

static const char *TAG = "m5apple2";
static apple2_machine_t s_machine;
static apple2_machine_t s_probe_machine;
static cardputer_display_t s_display;
static uint8_t s_apple2_pixels[APPLE2_VIDEO_WIDTH * APPLE2_VIDEO_HEIGHT];
typedef struct {
    int64_t window_start_us;
    uint64_t emulated_cycles;
    uint64_t cpu_step_us;
    uint64_t frame_compose_us;
    uint64_t frame_present_us;
    uint32_t frames_presented;
    uint32_t text_frames;
    uint32_t graphics_frames;
} app_perf_counters_t;

static app_perf_counters_t s_perf;

#ifdef CONFIG_M5APPLE2_CARDPUTER_VARIANT_ORIGINAL
#define APP_FRAME_INTERVAL_US 33333
#else
#define APP_FRAME_INTERVAL_US 16666
#endif

#ifdef M5APPLE2_HAS_APPLE2PLUS_ROM
extern const uint8_t apple2plus_rom_start[] asm("_binary_apple2plus_rom_start");
extern const uint8_t apple2plus_rom_end[] asm("_binary_apple2plus_rom_end");
#endif
#ifdef M5APPLE2_HAS_DISK2_ROM
extern const uint8_t disk2_rom_start[] asm("_binary_disk2_rom_start");
extern const uint8_t disk2_rom_end[] asm("_binary_disk2_rom_end");
#endif
#ifdef M5APPLE2_HAS_DOS33_DO
extern const uint8_t dos_3_3_do_start[] asm("_binary_dos_3_3_do_start");
extern const uint8_t dos_3_3_do_end[] asm("_binary_dos_3_3_do_end");
#endif
#ifdef M5APPLE2_HAS_DOS33_PO
extern const uint8_t dos_3_3_po_start[] asm("_binary_dos_3_3_po_start");
extern const uint8_t dos_3_3_po_end[] asm("_binary_dos_3_3_po_end");
#endif
#ifdef M5APPLE2_HAS_DOS33_DSK
extern const uint8_t dos_3_3_dsk_start[] asm("_binary_dos_3_3_dsk_start");
extern const uint8_t dos_3_3_dsk_end[] asm("_binary_dos_3_3_dsk_end");
#endif
#ifdef M5APPLE2_HAS_DOS33_NIB
extern const uint8_t dos_3_3_nib_start[] asm("_binary_dos_3_3_nib_start");
extern const uint8_t dos_3_3_nib_end[] asm("_binary_dos_3_3_nib_end");
#endif
#ifdef M5APPLE2_HAS_DOS33_WOZ
extern const uint8_t dos_3_3_woz_start[] asm("_binary_dos_3_3_woz_start");
extern const uint8_t dos_3_3_woz_end[] asm("_binary_dos_3_3_woz_end");
#endif

#define APP_DSK_PROBE_INSTRUCTIONS 900000U
#define APP_SD_MOUNT_POINT "/sd"
#define APP_PERF_LOG_INTERVAL_US ((int64_t)CONFIG_M5APPLE2_PERF_LOG_INTERVAL_MS * 1000LL)

typedef enum {
    APP_DISK_ORDER_DOS33 = 0,
    APP_DISK_ORDER_PRODOS = 1,
} app_disk_order_t;

typedef enum {
    APP_DISK_IMAGE_NONE = 0,
    APP_DISK_IMAGE_DO,
    APP_DISK_IMAGE_PO,
    APP_DISK_IMAGE_DSK,
    APP_DISK_IMAGE_NIB,
    APP_DISK_IMAGE_WOZ,
} app_disk_image_type_t;

typedef struct {
    bool present;
    app_disk_image_type_t type;
    const uint8_t *image;
    size_t image_size;
    apple2_disk2_image_order_t image_order;
    char label[24];
} app_builtin_drive_t;

typedef struct {
    const uint8_t *image;
    size_t image_size;
    apple2_woz_image_t woz;
} app_builtin_woz_drive_t;

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

#define APP_SD_MAX_IMAGES 16U
#define APP_SD_PATH_MAX 128U
#define APP_SD_NAME_MAX 40U

typedef struct {
    char path[APP_SD_PATH_MAX];
    char name[APP_SD_NAME_MAX];
    app_disk_image_type_t type;
} app_sd_disk_entry_t;

typedef struct {
    FILE *file;
    app_disk_image_type_t type;
    apple2_woz_image_t *woz;
} app_sd_drive_file_t;

static app_builtin_drive_t s_builtin_drives[2];
static app_builtin_woz_drive_t *s_builtin_woz_drives[2];
static app_sd_disk_entry_t s_sd_disks[APP_SD_MAX_IMAGES];
static app_sd_drive_file_t s_sd_drive_files[2];
static sdmmc_card_t *s_sd_card;
static size_t s_sd_disk_count;
static int s_sd_drive_index[2] = { -1, -1 };
static bool s_sd_mounted;

#if defined(M5APPLE2_HAS_APPLE2PLUS_ROM) && defined(M5APPLE2_HAS_DISK2_ROM)
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

static int app_score_dsk_order_source(const app_disk_source_t *source, app_disk_order_t order)
{
    apple2_machine_t *probe = &s_probe_machine;
    bool entered_stage2 = false;

    /* A full probe machine is too large for the ESP-IDF main task stack. */
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

    for (uint32_t i = 0; i < APP_DSK_PROBE_INSTRUCTIONS; ++i) {
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
    }

    {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(probe);
        int score = 0;

        score += (int)app_count_nonzero_range(probe, 0x1D00U, 0x0400U);
        score += (int)app_count_nonzero_range(probe, 0x2A00U, 0x0100U);
        if (probe->disk2.nibble_pos[0] > 384U) {
            score += 512;
        }
        if (probe->disk2.quarter_track[0] != 0U) {
            score += 128;
        }
        if (cpu.pc >= 0x3000U && cpu.pc < 0x4000U) {
            score += 128;
        }
        if (entered_stage2) {
            score += 64;
        }
        if (cpu.pc < 0x0100U) {
            score -= 512;
        }
        if (cpu.pc >= 0xFD00U) {
            score -= 256;
        }
        return score;
    }
}

static app_disk_order_t app_probe_dsk_order_source(const app_disk_source_t *source)
{
    const int dos_score = app_score_dsk_order_source(source, APP_DISK_ORDER_DOS33);
    const int prodos_score = app_score_dsk_order_source(source, APP_DISK_ORDER_PRODOS);

    ESP_LOGI(TAG, "Probed .dsk order: dos=%d prodos=%d", dos_score, prodos_score);
    return (prodos_score > dos_score) ? APP_DISK_ORDER_PRODOS : APP_DISK_ORDER_DOS33;
}

#ifdef M5APPLE2_HAS_DOS33_DSK
static app_disk_order_t app_probe_dsk_order(const uint8_t *image, size_t image_size)
{
    const app_disk_source_t source = {
        .kind = APP_DISK_SOURCE_MEMORY,
        .image = image,
        .image_size = image_size,
    };

    return app_probe_dsk_order_source(&source);
}
#endif
#endif

static void app_puts_at(uint8_t row, uint8_t column, const char *text)
{
    uint16_t address = apple2_text_row_address(false, row);
    address = (uint16_t)(address + column);

    while (*text != '\0' && column < 40U) {
        char ch = *text++;
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        if (ch < 32 || ch > 95) {
            ch = ' ';
        }
        s_machine.memory[address++] = (uint8_t)(ch | 0x80U);
        column++;
    }
}

static void app_show_status_screen(const char *line1, const char *line2, const char *line3)
{
    for (uint8_t row = 0; row < 24U; ++row) {
        const uint16_t base = apple2_text_row_address(false, row);
        for (uint8_t column = 0; column < 40U; ++column) {
            s_machine.memory[(uint16_t)(base + column)] = (uint8_t)(' ' | 0x80U);
        }
    }

    app_puts_at(2, 12, "M5APPLE2");
    if (line1 != NULL) {
        app_puts_at(8, 2, line1);
    }
    if (line2 != NULL) {
        app_puts_at(10, 2, line2);
    }
    if (line3 != NULL) {
        app_puts_at(12, 2, line3);
    }
    app_puts_at(20, 2, "ESC RESETS THE EMULATOR");
}

static bool app_builtin_woz_read_track(void *context,
                                       unsigned drive_index,
                                       uint8_t quarter_track,
                                       uint8_t *track_data,
                                       uint16_t *track_length)
{
    const app_builtin_woz_drive_t *drive = context;
    const uint8_t *source = NULL;

    (void)drive_index;
    if (drive == NULL || track_data == NULL || track_length == NULL) {
        return false;
    }
    if (!apple2_woz_get_track(&drive->woz,
                              drive->image,
                              drive->image_size,
                              quarter_track,
                              &source,
                              track_length)) {
        return false;
    }

    memcpy(track_data, source, *track_length);
    return true;
}

static bool app_load_memory_woz_drive(unsigned drive_index, const uint8_t *image, size_t image_size)
{
    app_builtin_woz_drive_t *drive;

    if (drive_index >= 2U) {
        return false;
    }
    if (s_builtin_woz_drives[drive_index] == NULL) {
        s_builtin_woz_drives[drive_index] = calloc(1U, sizeof(*s_builtin_woz_drives[drive_index]));
        if (s_builtin_woz_drives[drive_index] == NULL) {
            return false;
        }
    }

    drive = s_builtin_woz_drives[drive_index];
    if (!apple2_woz_parse(&drive->woz, image, image_size)) {
        return false;
    }

    drive->image = image;
    drive->image_size = image_size;
    return apple2_disk2_attach_drive_track_reader(&s_machine.disk2,
                                                  drive_index,
                                                  app_builtin_woz_read_track,
                                                  drive);
}

static bool app_load_memory_drive(unsigned drive_index,
                                  app_disk_image_type_t type,
                                  const uint8_t *image,
                                  size_t image_size,
                                  apple2_disk2_image_order_t image_order)
{
    switch (type) {
    case APP_DISK_IMAGE_DO:
    case APP_DISK_IMAGE_PO:
    case APP_DISK_IMAGE_DSK:
        return apple2_disk2_load_drive_with_order(&s_machine.disk2,
                                                  drive_index,
                                                  image,
                                                  image_size,
                                                  image_order);
    case APP_DISK_IMAGE_NIB:
        return apple2_disk2_load_nib_drive(&s_machine.disk2, drive_index, image, image_size);
    case APP_DISK_IMAGE_WOZ:
        return app_load_memory_woz_drive(drive_index, image, image_size);
    case APP_DISK_IMAGE_NONE:
    default:
        return false;
    }
}

#if defined(M5APPLE2_HAS_DOS33_DO) || defined(M5APPLE2_HAS_DOS33_PO) || defined(M5APPLE2_HAS_DOS33_DSK) || \
    defined(M5APPLE2_HAS_DOS33_NIB) || defined(M5APPLE2_HAS_DOS33_WOZ)
static void app_set_builtin_drive(unsigned drive_index,
                                  app_disk_image_type_t type,
                                  const uint8_t *image,
                                  size_t image_size,
                                  apple2_disk2_image_order_t image_order,
                                  const char *label)
{
    if (drive_index >= 2U) {
        return;
    }

    s_builtin_drives[drive_index].present = true;
    s_builtin_drives[drive_index].type = type;
    s_builtin_drives[drive_index].image = image;
    s_builtin_drives[drive_index].image_size = image_size;
    s_builtin_drives[drive_index].image_order = image_order;
    snprintf(s_builtin_drives[drive_index].label,
             sizeof(s_builtin_drives[drive_index].label),
             "%s",
             (label != NULL) ? label : "embedded");
}
#endif

static bool app_restore_builtin_drive(unsigned drive_index)
{
    const app_builtin_drive_t *drive;

    if (drive_index >= 2U) {
        return false;
    }

    drive = &s_builtin_drives[drive_index];
    if (!drive->present) {
        apple2_disk2_unload_drive(&s_machine.disk2, drive_index);
        return false;
    }

    return app_load_memory_drive(drive_index,
                                 drive->type,
                                 drive->image,
                                 drive->image_size,
                                 drive->image_order);
}

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

static bool app_sd_read_sector(void *context,
                               unsigned drive_index,
                               uint8_t track,
                               uint8_t file_sector,
                               uint8_t *sector_data)
{
    app_sd_drive_file_t *drive = context;
    const long offset = (long)((((size_t)track * 16U) + file_sector) * 256U);

    (void)drive_index;
    if (drive == NULL || drive->file == NULL || sector_data == NULL) {
        return false;
    }
    if (fseek(drive->file, offset, SEEK_SET) != 0) {
        return false;
    }
    return fread(sector_data, 1, 256U, drive->file) == 256U;
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

    (void)drive_index;
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
    if (fseek(drive->file, offset, SEEK_SET) != 0) {
        return false;
    }
    if (fread(track_data, 1, length, drive->file) != length) {
        return false;
    }

    *track_length = (uint16_t)length;
    return true;
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

static void app_sd_close_drive(unsigned drive_index)
{
    if (drive_index >= 2U) {
        return;
    }
    if (s_sd_drive_files[drive_index].file != NULL) {
        fclose(s_sd_drive_files[drive_index].file);
        s_sd_drive_files[drive_index].file = NULL;
    }
    s_sd_drive_files[drive_index].type = APP_DISK_IMAGE_NONE;
    free(s_sd_drive_files[drive_index].woz);
    s_sd_drive_files[drive_index].woz = NULL;
}

static int app_sd_disk_compare(const void *lhs, const void *rhs)
{
    const app_sd_disk_entry_t *left = lhs;
    const app_sd_disk_entry_t *right = rhs;

    return strcasecmp(left->name, right->name);
}

static bool app_sd_mount_filesystem(void)
{
#if !CONFIG_M5APPLE2_SD_ENABLE
    return false;
#else
    const spi_host_device_t host_id = (CONFIG_M5APPLE2_SD_HOST_ID == 3) ? SPI3_HOST : SPI2_HOST;
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
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_err_t err;

    if (s_sd_mounted) {
        return true;
    }

    err = spi_bus_initialize(host_id, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    host.slot = host_id;
    slot_config.host_id = host_id;
    slot_config.gpio_cs = CONFIG_M5APPLE2_SD_PIN_CS;

    err = esp_vfs_fat_sdspi_mount(APP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return false;
    }

    s_sd_mounted = true;
    ESP_LOGI(TAG, "Mounted SD card at %s", APP_SD_MOUNT_POINT);
    return true;
#endif
}

static bool app_sd_scan_directory(void)
{
    DIR *dir;
    struct dirent *entry;

    s_sd_disk_count = 0U;
    dir = opendir(APP_SD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "Could not open %s", APP_SD_MOUNT_POINT);
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        app_disk_image_type_t type;
        app_sd_disk_entry_t *disk;
        FILE *candidate_file;
        char candidate_path[APP_SD_PATH_MAX];
        size_t name_len;
        size_t path_len;

        if (entry->d_name[0] == '.') {
            continue;
        }
        type = app_disk_image_type_from_path(entry->d_name);
        if (type == APP_DISK_IMAGE_NONE) {
            continue;
        }
        if (s_sd_disk_count >= APP_SD_MAX_IMAGES) {
            ESP_LOGW(TAG, "Ignoring extra SD disks beyond %u entries", (unsigned)APP_SD_MAX_IMAGES);
            break;
        }
        name_len = strnlen(entry->d_name, sizeof(disk->path));
        if (name_len == 0U ||
            name_len >= sizeof(disk->name) ||
            (name_len + sizeof(APP_SD_MOUNT_POINT)) >= sizeof(disk->path)) {
            ESP_LOGW(TAG, "Skipping SD disk name that is too long: %s", entry->d_name);
            continue;
        }

        memcpy(candidate_path, APP_SD_MOUNT_POINT "/", sizeof(APP_SD_MOUNT_POINT));
        memcpy(&candidate_path[sizeof(APP_SD_MOUNT_POINT)],
               entry->d_name,
               name_len);
        candidate_path[sizeof(APP_SD_MOUNT_POINT) + name_len] = '\0';
        path_len = sizeof(APP_SD_MOUNT_POINT) + name_len + 1U;

        candidate_file = fopen(candidate_path, "rb");
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

        disk = &s_sd_disks[s_sd_disk_count++];
        memcpy(disk->path, candidate_path, path_len);
        memcpy(disk->name, entry->d_name, name_len);
        disk->name[name_len] = '\0';
        disk->type = type;
    }

    closedir(dir);
    qsort(s_sd_disks, s_sd_disk_count, sizeof(s_sd_disks[0]), app_sd_disk_compare);
    ESP_LOGI(TAG, "Found %u SD disk image(s)", (unsigned)s_sd_disk_count);
    return true;
}

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
    return order == APP_DISK_ORDER_PRODOS;
}

static bool app_sd_mount_disk(unsigned drive_index, size_t disk_index)
{
    const app_sd_disk_entry_t *disk;
    apple2_disk2_image_order_t image_order = APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
    bool attached = false;

    if (drive_index >= 2U || disk_index >= s_sd_disk_count) {
        return false;
    }

    disk = &s_sd_disks[disk_index];
    app_sd_close_drive(drive_index);
    s_sd_drive_files[drive_index].file = fopen(disk->path, "rb");
    if (s_sd_drive_files[drive_index].file == NULL) {
        ESP_LOGW(TAG, "Failed to open SD disk %s", disk->name);
        return false;
    }
    s_sd_drive_files[drive_index].type = disk->type;
    if (disk->type == APP_DISK_IMAGE_WOZ && s_sd_drive_files[drive_index].woz == NULL) {
        s_sd_drive_files[drive_index].woz = calloc(1U, sizeof(*s_sd_drive_files[drive_index].woz));
        if (s_sd_drive_files[drive_index].woz == NULL) {
            app_sd_close_drive(drive_index);
            ESP_LOGW(TAG, "Failed to allocate WOZ metadata for %s", disk->name);
            return false;
        }
    }
    if (!app_sd_file_valid(s_sd_drive_files[drive_index].file,
                           disk->type,
                           s_sd_drive_files[drive_index].woz)) {
        ESP_LOGW(TAG, "SD disk %s is not a supported image", disk->name);
        app_sd_close_drive(drive_index);
        return false;
    }

    switch (disk->type) {
    case APP_DISK_IMAGE_PO:
        image_order = APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL;
        attached = apple2_disk2_attach_drive_reader(&s_machine.disk2,
                                                    drive_index,
                                                    app_sd_read_sector,
                                                    &s_sd_drive_files[drive_index],
                                                    APPLE2_DISK2_IMAGE_SIZE,
                                                    image_order);
        break;
    case APP_DISK_IMAGE_DSK:
        image_order = app_sd_probe_file_order(disk->path)
                          ? APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL
                          : APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
        attached = apple2_disk2_attach_drive_reader(&s_machine.disk2,
                                                    drive_index,
                                                    app_sd_read_sector,
                                                    &s_sd_drive_files[drive_index],
                                                    APPLE2_DISK2_IMAGE_SIZE,
                                                    image_order);
        break;
    case APP_DISK_IMAGE_NIB:
    case APP_DISK_IMAGE_WOZ:
        attached = apple2_disk2_attach_drive_track_reader(&s_machine.disk2,
                                                          drive_index,
                                                          app_sd_read_track,
                                                          &s_sd_drive_files[drive_index]);
        break;
    case APP_DISK_IMAGE_DO:
    default:
        image_order = APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
        attached = apple2_disk2_attach_drive_reader(&s_machine.disk2,
                                                    drive_index,
                                                    app_sd_read_sector,
                                                    &s_sd_drive_files[drive_index],
                                                    APPLE2_DISK2_IMAGE_SIZE,
                                                    image_order);
        break;
    }

    if (!attached) {
        app_sd_close_drive(drive_index);
        ESP_LOGW(TAG, "Failed to attach SD disk %s", disk->name);
        return false;
    }

    s_sd_drive_index[drive_index] = (int)disk_index;
    switch (disk->type) {
    case APP_DISK_IMAGE_PO:
    case APP_DISK_IMAGE_DSK:
    case APP_DISK_IMAGE_DO:
        ESP_LOGI(TAG,
                 "Mounted SD disk %s in drive %u (%s order)",
                 disk->name,
                 (unsigned)(drive_index + 1U),
                 (image_order == APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL) ? "ProDOS" : "DOS");
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

static void app_sd_restore_default_drives(void)
{
    app_sd_close_drive(0U);
    app_sd_close_drive(1U);
    s_sd_drive_index[0] = -1;
    s_sd_drive_index[1] = -1;

    if (s_builtin_drives[0].present) {
        (void)app_restore_builtin_drive(0U);
    } else {
        apple2_disk2_unload_drive(&s_machine.disk2, 0U);
    }

    if (s_builtin_drives[1].present) {
        (void)app_restore_builtin_drive(1U);
    } else {
        apple2_disk2_unload_drive(&s_machine.disk2, 1U);
    }

    if (s_sd_disk_count == 0U) {
        return;
    }

    if (s_builtin_drives[0].present) {
        (void)app_sd_mount_disk(1U, 0U);
    } else {
        (void)app_sd_mount_disk(0U, 0U);
        if (s_sd_disk_count > 1U) {
            (void)app_sd_mount_disk(1U, 1U);
        }
    }
}

static void app_sd_rescan(void)
{
#if !CONFIG_M5APPLE2_SD_ENABLE
    return;
#else
    if (!app_sd_mount_filesystem()) {
        return;
    }
    if (!app_sd_scan_directory()) {
        return;
    }
    app_sd_restore_default_drives();
#endif
}

static void app_sd_cycle_drive(unsigned drive_index)
{
#if !CONFIG_M5APPLE2_SD_ENABLE
    (void)drive_index;
    ESP_LOGW(TAG, "SD disk library is disabled in menuconfig");
    return;
#else
    int next_index;

    if (!s_sd_mounted || s_sd_disk_count == 0U || drive_index >= 2U) {
        ESP_LOGW(TAG, "No SD disk library available for drive %u", (unsigned)(drive_index + 1U));
        return;
    }

    next_index = s_sd_drive_index[drive_index];
    if (++next_index >= (int)s_sd_disk_count) {
        next_index = -1;
    }

    if (next_index < 0) {
        app_sd_close_drive(drive_index);
        s_sd_drive_index[drive_index] = -1;
        if (s_builtin_drives[drive_index].present) {
            if (app_restore_builtin_drive(drive_index)) {
                ESP_LOGI(TAG,
                         "Restored embedded disk in drive %u (%s)",
                         (unsigned)(drive_index + 1U),
                         s_builtin_drives[drive_index].label);
            }
        } else {
            apple2_disk2_unload_drive(&s_machine.disk2, drive_index);
            ESP_LOGI(TAG, "Ejected drive %u", (unsigned)(drive_index + 1U));
        }
        return;
    }

    if (!app_sd_mount_disk(drive_index, (size_t)next_index)) {
        app_sd_close_drive(drive_index);
        if (s_builtin_drives[drive_index].present) {
            (void)app_restore_builtin_drive(drive_index);
        } else {
            apple2_disk2_unload_drive(&s_machine.disk2, drive_index);
        }
    }
#endif
}

static bool app_load_system_rom(void)
{
#ifdef M5APPLE2_HAS_APPLE2PLUS_ROM
    const size_t rom_size = (size_t)(apple2plus_rom_end - apple2plus_rom_start);
    if (!apple2_machine_load_system_rom(&s_machine, apple2plus_rom_start, rom_size)) {
        ESP_LOGE(TAG, "Embedded ROM rejected, size=%u", (unsigned)rom_size);
        return false;
    }
    ESP_LOGI(TAG, "Loaded embedded Apple II ROM (%u bytes)", (unsigned)rom_size);
    return true;
#else
    return false;
#endif
}

static bool app_load_slot6_rom(void)
{
#ifdef M5APPLE2_HAS_DISK2_ROM
    const size_t rom_size = (size_t)(disk2_rom_end - disk2_rom_start);
    if (!apple2_machine_load_slot6_rom(&s_machine, disk2_rom_start, rom_size)) {
        ESP_LOGE(TAG, "Embedded Disk II ROM rejected, size=%u", (unsigned)rom_size);
        return false;
    }
    ESP_LOGI(TAG, "Loaded embedded Disk II ROM (%u bytes)", (unsigned)rom_size);
    return true;
#else
    return false;
#endif
}

static bool app_load_drive0_image(void)
{
#ifdef M5APPLE2_HAS_DOS33_WOZ
    {
        const size_t image_size = (size_t)(dos_3_3_woz_end - dos_3_3_woz_start);

        if (!app_load_memory_drive(0U,
                                   APP_DISK_IMAGE_WOZ,
                                   dos_3_3_woz_start,
                                   image_size,
                                   APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL)) {
            ESP_LOGE(TAG, "Embedded .woz disk rejected, size=%u", (unsigned)image_size);
            return false;
        }
        ESP_LOGI(TAG, "Loaded embedded .woz disk (%u bytes)", (unsigned)image_size);
        app_set_builtin_drive(0U,
                              APP_DISK_IMAGE_WOZ,
                              dos_3_3_woz_start,
                              image_size,
                              APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL,
                              "dos_3.3.woz");
        return true;
    }
#endif
#ifdef M5APPLE2_HAS_DOS33_NIB
    {
        const size_t image_size = (size_t)(dos_3_3_nib_end - dos_3_3_nib_start);

        if (!app_load_memory_drive(0U,
                                   APP_DISK_IMAGE_NIB,
                                   dos_3_3_nib_start,
                                   image_size,
                                   APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL)) {
            ESP_LOGE(TAG, "Embedded .nib disk rejected, size=%u", (unsigned)image_size);
            return false;
        }
        ESP_LOGI(TAG, "Loaded embedded .nib disk (%u bytes)", (unsigned)image_size);
        app_set_builtin_drive(0U,
                              APP_DISK_IMAGE_NIB,
                              dos_3_3_nib_start,
                              image_size,
                              APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL,
                              "dos_3.3.nib");
        return true;
    }
#endif
#ifdef M5APPLE2_HAS_DOS33_DO
    {
        const size_t image_size = (size_t)(dos_3_3_do_end - dos_3_3_do_start);
        if (!app_load_memory_drive(0U,
                                   APP_DISK_IMAGE_DO,
                                   dos_3_3_do_start,
                                   image_size,
                                   APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL)) {
            ESP_LOGE(TAG, "Embedded DOS-order disk rejected, size=%u", (unsigned)image_size);
            return false;
        }
        ESP_LOGI(TAG, "Loaded embedded DOS-order disk (%u bytes)", (unsigned)image_size);
        app_set_builtin_drive(0U,
                              APP_DISK_IMAGE_DO,
                              dos_3_3_do_start,
                              image_size,
                              APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL,
                              "dos_3.3.do");
        return true;
    }
#endif
#ifdef M5APPLE2_HAS_DOS33_PO
    {
        const size_t image_size = (size_t)(dos_3_3_po_end - dos_3_3_po_start);
        if (!app_load_memory_drive(0U,
                                   APP_DISK_IMAGE_PO,
                                   dos_3_3_po_start,
                                   image_size,
                                   APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL)) {
            ESP_LOGE(TAG, "Embedded ProDOS-order disk rejected, size=%u", (unsigned)image_size);
            return false;
        }
        ESP_LOGI(TAG, "Loaded embedded ProDOS-order disk (%u bytes)", (unsigned)image_size);
        app_set_builtin_drive(0U,
                              APP_DISK_IMAGE_PO,
                              dos_3_3_po_start,
                              image_size,
                              APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL,
                              "dos_3.3.po");
        return true;
    }
#endif
#ifdef M5APPLE2_HAS_DOS33_DSK
    {
        const size_t image_size = (size_t)(dos_3_3_dsk_end - dos_3_3_dsk_start);
        bool loaded = false;
        apple2_disk2_image_order_t image_order = APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;

#if defined(M5APPLE2_HAS_APPLE2PLUS_ROM) && defined(M5APPLE2_HAS_DISK2_ROM)
        const app_disk_order_t order = app_probe_dsk_order(dos_3_3_dsk_start, image_size);
        image_order = (order == APP_DISK_ORDER_PRODOS)
                          ? APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL
                          : APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
        loaded = app_load_memory_drive(0U,
                                       APP_DISK_IMAGE_DSK,
                                       dos_3_3_dsk_start,
                                       image_size,
                                       image_order);
        if (loaded) {
            ESP_LOGI(TAG,
                     "Loaded embedded .dsk disk (%u bytes, %s order)",
                     (unsigned)image_size,
                     (order == APP_DISK_ORDER_PRODOS) ? "ProDOS" : "DOS");
        }
#else
        loaded = app_load_memory_drive(0U,
                                       APP_DISK_IMAGE_DSK,
                                       dos_3_3_dsk_start,
                                       image_size,
                                       APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL);
        if (loaded) {
            ESP_LOGI(TAG, "Loaded embedded .dsk disk (%u bytes, DOS fallback)", (unsigned)image_size);
        }
#endif
        if (!loaded) {
            ESP_LOGE(TAG, "Embedded .dsk disk rejected, size=%u", (unsigned)image_size);
            return false;
        }
        app_set_builtin_drive(0U,
                              APP_DISK_IMAGE_DSK,
                              dos_3_3_dsk_start,
                              image_size,
                              image_order,
                              "dos_3.3.dsk");
        return true;
    }
#else
    return false;
#endif
}

static void app_perf_reset(int64_t now_us)
{
    memset(&s_perf, 0, sizeof(s_perf));
    s_perf.window_start_us = now_us;
}

static void app_perf_log_if_due(int64_t now_us)
{
    const int64_t elapsed_us = now_us - s_perf.window_start_us;

    if (APP_PERF_LOG_INTERVAL_US <= 0 || elapsed_us < APP_PERF_LOG_INTERVAL_US) {
        return;
    }

    {
        const uint64_t effective_khz =
            (elapsed_us > 0) ? (s_perf.emulated_cycles * 1000ULL) / (uint64_t)elapsed_us : 0ULL;
        const uint32_t fps_x10 =
            (elapsed_us > 0) ? (uint32_t)((uint64_t)s_perf.frames_presented * 10000000ULL / (uint64_t)elapsed_us)
                             : 0U;
        const uint32_t cpu_pct =
            (elapsed_us > 0) ? (uint32_t)(s_perf.cpu_step_us * 100ULL / (uint64_t)elapsed_us) : 0U;
        const uint32_t compose_pct =
            (elapsed_us > 0) ? (uint32_t)(s_perf.frame_compose_us * 100ULL / (uint64_t)elapsed_us) : 0U;
        const uint32_t present_pct =
            (elapsed_us > 0) ? (uint32_t)(s_perf.frame_present_us * 100ULL / (uint64_t)elapsed_us) : 0U;

        ESP_LOGI(TAG,
                 "perf apple=%" PRIu64 ".%03" PRIu64 "MHz fps=%" PRIu32 ".%" PRIu32
                 " cpu=%" PRIu32 "%% compose=%" PRIu32 "%% present=%" PRIu32
                 "%% frames=%" PRIu32 " text=%" PRIu32 " gfx=%" PRIu32,
                 effective_khz / 1000ULL,
                 effective_khz % 1000ULL,
                 fps_x10 / 10U,
                 fps_x10 % 10U,
                 cpu_pct,
                 compose_pct,
                 present_pct,
                 s_perf.frames_presented,
                 s_perf.text_frames,
                 s_perf.graphics_frames);
    }

    app_perf_reset(now_us);
}

void app_main(void)
{
    apple2_config_t apple2_config = {
        .cpu_hz = CONFIG_M5APPLE2_CPU_HZ,
    };
    bool rom_loaded = false;
    bool slot6_loaded = false;
    bool drive0_loaded = false;
    int64_t last_cpu_tick_us;
    int64_t last_frame_tick_us;

    apple2_machine_init(&s_machine, &apple2_config);

    ESP_ERROR_CHECK(cardputer_display_init(&s_display));
    ESP_ERROR_CHECK(cardputer_keyboard_init());
    ESP_LOGI(TAG, "Cardputer display ready: %" PRIu16 "x%" PRIu16,
             s_display.native_width, s_display.native_height);

    rom_loaded = app_load_system_rom();
    slot6_loaded = app_load_slot6_rom();
    drive0_loaded = app_load_drive0_image();
    app_sd_rescan();
    if (rom_loaded && !slot6_loaded) {
        ESP_LOGW(TAG, "System ROM loaded without a separate Disk II ROM");
    }
    if (drive0_loaded && !rom_loaded) {
        ESP_LOGW(TAG, "Disk image loaded without a system ROM");
    }
    if (drive0_loaded && !slot6_loaded && !s_machine.slot6_rom_loaded) {
        ESP_LOGW(TAG, "Disk image loaded without Disk II ROM support");
    }
    if (!rom_loaded) {
        app_show_status_screen("PLACE APPLE2PLUS.ROM IN", "roms/ AND REBUILD.", "RUNS WITHOUT ROM SHOW STATUS.");
    }

    last_cpu_tick_us = esp_timer_get_time();
    last_frame_tick_us = last_cpu_tick_us;
    app_perf_reset(last_cpu_tick_us);

    while (true) {
        uint8_t ascii = 0;
        if (cardputer_keyboard_poll_ascii(&ascii)) {
            if (ascii == CARDPUTER_INPUT_CMD_SD_DRIVE1) {
                app_sd_cycle_drive(0U);
            } else if (ascii == CARDPUTER_INPUT_CMD_SD_DRIVE2) {
                app_sd_cycle_drive(1U);
            } else if (ascii == CARDPUTER_INPUT_CMD_SD_RESCAN) {
                app_sd_rescan();
            } else if (ascii == 0x1BU) {
                apple2_machine_reset(&s_machine);
                if (!rom_loaded) {
                    app_show_status_screen("PLACE APPLE2PLUS.ROM IN", "roms/ AND REBUILD.", "RUNS WITHOUT ROM SHOW STATUS.");
                }
            } else {
                apple2_machine_set_key(&s_machine, ascii);
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (rom_loaded) {
            const uint64_t previous_cycles = s_machine.total_cycles;
            uint64_t budget = (uint64_t)(now_us - last_cpu_tick_us) * apple2_config.cpu_hz / 1000000ULL;

            if (budget > (uint64_t)apple2_config.cpu_hz / 10ULL) {
                budget = (uint64_t)apple2_config.cpu_hz / 10ULL;
            }
            if (budget > 0U) {
                const int64_t step_start_us = esp_timer_get_time();
                apple2_machine_step(&s_machine, (uint32_t)budget);
                s_perf.cpu_step_us += (uint64_t)(esp_timer_get_time() - step_start_us);
                s_perf.emulated_cycles += s_machine.total_cycles - previous_cycles;
            }
        }
        last_cpu_tick_us = now_us;

        if ((now_us - last_frame_tick_us) >= APP_FRAME_INTERVAL_US) {
            const int64_t compose_start_us = esp_timer_get_time();
            if (s_machine.video.text_mode) {
                s_perf.text_frames++;
                s_perf.frame_compose_us += (uint64_t)(esp_timer_get_time() - compose_start_us);
                {
                    const int64_t present_start_us = esp_timer_get_time();

                    ESP_ERROR_CHECK(cardputer_display_present_apple2_text40(&s_display,
                                                                            s_machine.memory,
                                                                            &s_machine.video));
                    s_perf.frame_present_us += (uint64_t)(esp_timer_get_time() - present_start_us);
                }
            } else {
                apple2_machine_render(&s_machine, s_apple2_pixels);
                s_perf.graphics_frames++;
                s_perf.frame_compose_us += (uint64_t)(esp_timer_get_time() - compose_start_us);
                {
                    const int64_t present_start_us = esp_timer_get_time();

                    ESP_ERROR_CHECK(cardputer_display_present_apple2(&s_display,
                                                                     s_apple2_pixels,
                                                                     APPLE2_VIDEO_WIDTH,
                                                                     APPLE2_VIDEO_HEIGHT));
                    s_perf.frame_present_us += (uint64_t)(esp_timer_get_time() - present_start_us);
                }
            }
            s_perf.frames_presented++;
            last_frame_tick_us = now_us;
        }

        app_perf_log_if_due(now_us);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
