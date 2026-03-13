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

static const char *TAG = "m5apple2";
static apple2_machine_t s_machine;
static apple2_machine_t s_probe_machine;
static cardputer_display_t s_display;
static uint8_t s_apple2_pixels[APPLE2_VIDEO_WIDTH * APPLE2_VIDEO_HEIGHT];

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

#define APP_DSK_PROBE_INSTRUCTIONS 900000U

typedef enum {
    APP_DISK_ORDER_DOS33 = 0,
    APP_DISK_ORDER_PRODOS = 1,
} app_disk_order_t;

typedef enum {
    APP_DISK_IMAGE_NONE = 0,
    APP_DISK_IMAGE_DO,
    APP_DISK_IMAGE_PO,
    APP_DISK_IMAGE_DSK,
} app_disk_image_type_t;

typedef struct {
    bool present;
    const uint8_t *image;
    size_t image_size;
    apple2_disk2_image_order_t image_order;
    char label[24];
} app_builtin_drive_t;

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
} app_sd_drive_file_t;

static app_builtin_drive_t s_builtin_drives[2];
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

static bool app_load_memory_drive(unsigned drive_index,
                                  const uint8_t *image,
                                  size_t image_size,
                                  apple2_disk2_image_order_t image_order)
{
    return apple2_disk2_load_drive_with_order(&s_machine.disk2,
                                              drive_index,
                                              image,
                                              image_size,
                                              image_order);
}

static void app_set_builtin_drive(unsigned drive_index,
                                  const uint8_t *image,
                                  size_t image_size,
                                  apple2_disk2_image_order_t image_order,
                                  const char *label)
{
    if (drive_index >= 2U) {
        return;
    }

    s_builtin_drives[drive_index].present = true;
    s_builtin_drives[drive_index].image = image;
    s_builtin_drives[drive_index].image_size = image_size;
    s_builtin_drives[drive_index].image_order = image_order;
    snprintf(s_builtin_drives[drive_index].label,
             sizeof(s_builtin_drives[drive_index].label),
             "%s",
             (label != NULL) ? label : "embedded");
}

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

    return app_load_memory_drive(drive_index, drive->image, drive->image_size, drive->image_order);
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
    return APP_DISK_IMAGE_NONE;
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

static bool app_sd_file_valid(FILE *file)
{
    long size;

    if (file == NULL) {
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        return false;
    }
    size = ftell(file);
    if (size < 0L) {
        return false;
    }
    rewind(file);
    return (size_t)size == APPLE2_DISK2_IMAGE_SIZE;
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

    err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot_config, &mount_config, &s_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return false;
    }

    s_sd_mounted = true;
    ESP_LOGI(TAG, "Mounted SD card at /sd");
    return true;
#endif
}

static bool app_sd_scan_directory(void)
{
    DIR *dir;
    struct dirent *entry;

    s_sd_disk_count = 0U;
    dir = opendir("/sd");
    if (dir == NULL) {
        ESP_LOGW(TAG, "Could not open /sd");
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        app_disk_image_type_t type;
        app_sd_disk_entry_t *disk;
        size_t name_len;

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
            (name_len + 4U) >= sizeof(disk->path)) {
            ESP_LOGW(TAG, "Skipping SD disk name that is too long: %s", entry->d_name);
            continue;
        }

        disk = &s_sd_disks[s_sd_disk_count++];
        memcpy(disk->path, "/sd/", 4U);
        memcpy(&disk->path[4], entry->d_name, name_len);
        disk->path[4U + name_len] = '\0';
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
    if (!app_sd_file_valid(probe.file)) {
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
    apple2_disk2_image_order_t image_order;

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
    if (!app_sd_file_valid(s_sd_drive_files[drive_index].file)) {
        ESP_LOGW(TAG, "SD disk %s is not a 140 KB image", disk->name);
        app_sd_close_drive(drive_index);
        return false;
    }

    switch (disk->type) {
    case APP_DISK_IMAGE_PO:
        image_order = APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL;
        break;
    case APP_DISK_IMAGE_DSK:
        image_order = app_sd_probe_file_order(disk->path)
                          ? APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL
                          : APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
        break;
    case APP_DISK_IMAGE_DO:
    default:
        image_order = APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
        break;
    }

    if (!apple2_disk2_attach_drive_reader(&s_machine.disk2,
                                          drive_index,
                                          app_sd_read_sector,
                                          &s_sd_drive_files[drive_index],
                                          APPLE2_DISK2_IMAGE_SIZE,
                                          image_order)) {
        app_sd_close_drive(drive_index);
        ESP_LOGW(TAG, "Failed to attach SD disk %s", disk->name);
        return false;
    }

    s_sd_drive_index[drive_index] = (int)disk_index;
    ESP_LOGI(TAG,
             "Mounted SD disk %s in drive %u (%s order)",
             disk->name,
             (unsigned)(drive_index + 1U),
             (image_order == APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL) ? "ProDOS" : "DOS");
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
#ifdef M5APPLE2_HAS_DOS33_DO
    {
        const size_t image_size = (size_t)(dos_3_3_do_end - dos_3_3_do_start);
        if (!app_load_memory_drive(0U,
                                   dos_3_3_do_start,
                                   image_size,
                                   APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL)) {
            ESP_LOGE(TAG, "Embedded DOS-order disk rejected, size=%u", (unsigned)image_size);
            return false;
        }
        ESP_LOGI(TAG, "Loaded embedded DOS-order disk (%u bytes)", (unsigned)image_size);
        app_set_builtin_drive(0U,
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
                                   dos_3_3_po_start,
                                   image_size,
                                   APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL)) {
            ESP_LOGE(TAG, "Embedded ProDOS-order disk rejected, size=%u", (unsigned)image_size);
            return false;
        }
        ESP_LOGI(TAG, "Loaded embedded ProDOS-order disk (%u bytes)", (unsigned)image_size);
        app_set_builtin_drive(0U,
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
        loaded = app_load_memory_drive(0U, dos_3_3_dsk_start, image_size, image_order);
        if (loaded) {
            ESP_LOGI(TAG,
                     "Loaded embedded .dsk disk (%u bytes, %s order)",
                     (unsigned)image_size,
                     (order == APP_DISK_ORDER_PRODOS) ? "ProDOS" : "DOS");
        }
#else
        loaded = app_load_memory_drive(0U,
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
        app_set_builtin_drive(0U, dos_3_3_dsk_start, image_size, image_order, "dos_3.3.dsk");
        return true;
    }
#else
    return false;
#endif
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
            uint64_t budget = (uint64_t)(now_us - last_cpu_tick_us) * apple2_config.cpu_hz / 1000000ULL;
            if (budget > (uint64_t)apple2_config.cpu_hz / 10ULL) {
                budget = (uint64_t)apple2_config.cpu_hz / 10ULL;
            }
            if (budget > 0U) {
                apple2_machine_step(&s_machine, (uint32_t)budget);
            }
        }
        last_cpu_tick_us = now_us;

        if ((now_us - last_frame_tick_us) >= 16666) {
            if (s_machine.video.text_mode) {
                ESP_ERROR_CHECK(cardputer_display_present_apple2_text40(&s_display,
                                                                        s_machine.memory,
                                                                        &s_machine.video));
            } else {
                apple2_machine_render(&s_machine, s_apple2_pixels);
                ESP_ERROR_CHECK(cardputer_display_present_apple2(&s_display,
                                                                 s_apple2_pixels,
                                                                 APPLE2_VIDEO_WIDTH,
                                                                 APPLE2_VIDEO_HEIGHT));
            }
            last_frame_tick_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
