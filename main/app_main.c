#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "apple2/apple2_machine.h"
#include "apple2/apple2_video.h"
#include "cardputer/cardputer_display.h"
#include "cardputer/cardputer_keyboard.h"

#include "app_sd.h"

#if CONFIG_M5APPLE2_AUDIO_ENABLED
#include "cardputer/cardputer_audio.h"

static void app_speaker_toggle(void *context, uint64_t total_cycles)
{
    cardputer_audio_toggle((cardputer_audio_t *)context, total_cycles);
}
#endif

#ifndef CONFIG_M5APPLE2_PERF_LOG_INTERVAL_MS
#define CONFIG_M5APPLE2_PERF_LOG_INTERVAL_MS 2000
#endif
#ifndef CONFIG_M5APPLE2_DETAILED_PERF_PROFILE
#define CONFIG_M5APPLE2_DETAILED_PERF_PROFILE 0
#endif

static const char *TAG = "m5apple2";
static apple2_machine_t s_machine;
static cardputer_display_t s_display;
static uint8_t s_apple2_pixels[APPLE2_VIDEO_WIDTH * APPLE2_VIDEO_HEIGHT];
#include "app_perf.h"

static app_perf_counters_t s_perf;

#define APP_BOOT_TRACE_INTERVAL_US 200000LL
#define APP_BOOT_TRACE_DURATION_US 8000000LL
#define APP_BOOT_TRACE_MAX_LINES 40U
#define APP_BOOT_TRACE_PC_HISTORY 64U

typedef struct {
    bool active;
    bool loaded_code_dumped;
    bool monitor_dumped;
    int64_t until_us;
    int64_t next_log_us;
    uint32_t lines_emitted;
    size_t recent_pc_next;
    size_t recent_pc_count;
    uint16_t recent_pcs[APP_BOOT_TRACE_PC_HISTORY];
} app_boot_trace_t;

static app_boot_trace_t s_boot_trace;

#define APP_TEXT_ROWS 24U
#define APP_TEXT_COLUMNS 40U
#define APP_TEXT_BYTES (APP_TEXT_ROWS * APP_TEXT_COLUMNS)
#define APP_TEXT_SCREEN_BASE 0x0400U
#define APP_TEXT_SCREEN_BYTES 0x0800U

typedef struct {
    bool valid;
    bool displayed;
    bool page2;
    bool flash_state;
    uint8_t cells[APP_TEXT_BYTES];
} app_text_frame_cache_t;

static app_text_frame_cache_t s_text_cache;

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

#define APP_PERF_LOG_INTERVAL_US ((int64_t)CONFIG_M5APPLE2_PERF_LOG_INTERVAL_MS * 1000LL)

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

static app_builtin_drive_t s_builtin_drives[2];
static app_builtin_woz_drive_t *s_builtin_woz_drives[2];

typedef enum {
    APP_SD_PICKER_ITEM_EJECT = 0,
    APP_SD_PICKER_ITEM_BUILTIN,
    APP_SD_PICKER_ITEM_SD,
} app_sd_picker_item_kind_t;

typedef struct {
    app_sd_picker_item_kind_t kind;
    int sd_index;
} app_sd_picker_item_t;

typedef struct {
    bool active;
    uint8_t drive_index;
    int selected_item;
    int scroll_item;
    apple2_video_state_t saved_video;
    uint8_t saved_text_pages[APP_TEXT_SCREEN_BYTES];
    char status[APP_TEXT_COLUMNS + 1U];
    char browse_path[APP_SD_PATH_MAX];
    uint8_t path_depth;
} app_sd_picker_t;

static app_sd_picker_t s_sd_picker;

typedef enum {
    APP_SPEED_MODE_1X = 0,
    APP_SPEED_MODE_2X,
    APP_SPEED_MODE_4X,
    APP_SPEED_MODE_AUTO_DISK,
} app_speed_mode_t;

static app_speed_mode_t s_speed_mode = APP_SPEED_MODE_1X;

static void app_flush_dirty_tracks(void);
static bool app_restore_builtin_drive(unsigned drive_index);
static void app_text_cache_reset(void);

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

static void app_text_row_ascii(uint8_t row, char *buffer, size_t buffer_size)
{
    uint16_t base;

    if (buffer == NULL || buffer_size == 0U) {
        return;
    }
    if (row >= APP_TEXT_ROWS) {
        buffer[0] = '\0';
        return;
    }

    base = apple2_text_row_address(s_machine.video.page2, row);
    for (uint8_t column = 0; column < APP_TEXT_COLUMNS && (size_t)column + 1U < buffer_size; ++column) {
        bool inverse = false;
        const uint8_t ascii = apple2_text_code_to_ascii(s_machine.memory[(uint16_t)(base + column)], &inverse);

        buffer[column] = (ascii >= 32U && ascii <= 126U) ? (char)ascii : '.';
    }
    buffer[APP_TEXT_COLUMNS] = '\0';
    for (int column = APP_TEXT_COLUMNS - 1; column >= 0; --column) {
        if (buffer[column] != ' ') {
            break;
        }
        buffer[column] = '\0';
    }
}

static void app_boot_trace_arm(const char *reason)
{
    const int64_t now_us = esp_timer_get_time();

    s_boot_trace.active = true;
    s_boot_trace.loaded_code_dumped = false;
    s_boot_trace.monitor_dumped = false;
    s_boot_trace.until_us = now_us + APP_BOOT_TRACE_DURATION_US;
    s_boot_trace.next_log_us = now_us;
    s_boot_trace.lines_emitted = 0U;
    s_boot_trace.recent_pc_next = 0U;
    s_boot_trace.recent_pc_count = 0U;
    ESP_LOGI(TAG, "Boot trace armed: %s", reason);
}

static void app_boot_trace_record_pc(uint16_t pc)
{
    if (!s_boot_trace.active || s_boot_trace.monitor_dumped) {
        return;
    }

    s_boot_trace.recent_pcs[s_boot_trace.recent_pc_next] = pc;
    s_boot_trace.recent_pc_next = (s_boot_trace.recent_pc_next + 1U) % APP_BOOT_TRACE_PC_HISTORY;
    if (s_boot_trace.recent_pc_count < APP_BOOT_TRACE_PC_HISTORY) {
        s_boot_trace.recent_pc_count++;
    }
}

static void app_boot_trace_dump_recent_pcs(void)
{
    const size_t count = s_boot_trace.recent_pc_count;
    const size_t start = (count < APP_BOOT_TRACE_PC_HISTORY) ? 0U : s_boot_trace.recent_pc_next;

    for (size_t i = 0; i < count; ++i) {
        const size_t index = (start + i) % APP_BOOT_TRACE_PC_HISTORY;

        ESP_LOGI(TAG, "boottrace recent_pc[%u]=%04x", (unsigned)i, s_boot_trace.recent_pcs[index]);
    }
}

static void app_boot_trace_maybe_dump_transition(uint16_t pc)
{
    char row23[APP_TEXT_COLUMNS + 1U];

    if (!s_boot_trace.active) {
        return;
    }

    if (!s_boot_trace.loaded_code_dumped && pc >= 0x0800U && pc < 0xC000U) {
        ESP_LOGI(TAG,
                 "Boot trace entered loaded code pc=%04x bytes=%02x %02x %02x %02x",
                 pc,
                 s_machine.memory[pc],
                 s_machine.memory[(uint16_t)(pc + 1U)],
                 s_machine.memory[(uint16_t)(pc + 2U)],
                 s_machine.memory[(uint16_t)(pc + 3U)]);
        s_boot_trace.loaded_code_dumped = true;
    }

    if (!s_boot_trace.monitor_dumped && pc >= 0xFD1BU && pc <= 0xFD24U) {
        app_text_row_ascii(23U, row23, sizeof(row23));
        ESP_LOGI(TAG, "Boot trace monitor entry row23=\"%s\"", row23);
        app_boot_trace_dump_recent_pcs();
        s_boot_trace.monitor_dumped = true;
    }
}

static void app_boot_trace_poll(int64_t now_us)
{
    char row23[APP_TEXT_COLUMNS + 1U];
    const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&s_machine);
    const uint8_t drive = s_machine.disk2.active_drive;

    if (!s_boot_trace.active) {
        return;
    }
    if (now_us >= s_boot_trace.until_us || s_boot_trace.lines_emitted >= APP_BOOT_TRACE_MAX_LINES) {
        s_boot_trace.active = false;
        ESP_LOGI(TAG, "Boot trace complete");
        return;
    }
    if (now_us < s_boot_trace.next_log_us) {
        return;
    }

    app_text_row_ascii(23U, row23, sizeof(row23));
    ESP_LOGI(TAG,
             "boottrace pc=%04x a=%02x x=%02x y=%02x p=%02x sp=%02x drive=%u qt=%u nib=%" PRIu32
             " motor=%u q6=%u q7=%u latch=%02x text=%u row23=\"%s\"",
             cpu.pc,
             cpu.a,
             cpu.x,
             cpu.y,
             cpu.p,
             cpu.sp,
             (unsigned)(drive + 1U),
             s_machine.disk2.quarter_track[drive],
             s_machine.disk2.nibble_pos[drive],
             s_machine.disk2.motor_on ? 1U : 0U,
             s_machine.disk2.q6 ? 1U : 0U,
             s_machine.disk2.q7 ? 1U : 0U,
             s_machine.disk2.data_latch,
             s_machine.video.text_mode ? 1U : 0U,
             row23);
    s_boot_trace.lines_emitted++;
    s_boot_trace.next_log_us += APP_BOOT_TRACE_INTERVAL_US;
}

static void app_log_boot_drive_state(const char *action)
{
    for (unsigned drive_index = 0; drive_index < 2U; ++drive_index) {
        const int sd_idx = app_sd_drive_index(drive_index);

        if (!apple2_disk2_drive_loaded(&s_machine.disk2, drive_index)) {
            ESP_LOGI(TAG, "%s drive %u: empty", action, drive_index + 1U);
            continue;
        }

        if (sd_idx >= 0 && (size_t)sd_idx < app_sd_disk_count()) {
            const app_sd_disk_entry_t *disk = app_sd_disk_entry((size_t)sd_idx);

            switch (disk->type) {
            case APP_DISK_IMAGE_DO:
            case APP_DISK_IMAGE_PO:
            case APP_DISK_IMAGE_DSK:
                ESP_LOGI(TAG,
                         "%s drive %u: SD %s (%s, %s order)",
                         action,
                         drive_index + 1U,
                         disk->name,
                         app_sd_disk_image_type_name(disk->type),
                         (app_sd_drive_image_order(drive_index) ==
                          APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL)
                             ? "ProDOS"
                             : "DOS");
                break;
            default:
                ESP_LOGI(TAG,
                         "%s drive %u: SD %s (%s)",
                         action,
                         drive_index + 1U,
                         disk->name,
                         app_sd_disk_image_type_name(disk->type));
                break;
            }
            continue;
        }

        if (s_builtin_drives[drive_index].present) {
            switch (s_builtin_drives[drive_index].type) {
            case APP_DISK_IMAGE_DO:
            case APP_DISK_IMAGE_PO:
            case APP_DISK_IMAGE_DSK:
                ESP_LOGI(TAG,
                         "%s drive %u: builtin %s (%s, %s order)",
                         action,
                         drive_index + 1U,
                         s_builtin_drives[drive_index].label,
                         app_sd_disk_image_type_name(s_builtin_drives[drive_index].type),
                         (s_builtin_drives[drive_index].image_order ==
                          APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL)
                             ? "ProDOS"
                             : "DOS");
                break;
            default:
                ESP_LOGI(TAG,
                         "%s drive %u: builtin %s (%s)",
                         action,
                         drive_index + 1U,
                         s_builtin_drives[drive_index].label,
                         app_sd_disk_image_type_name(s_builtin_drives[drive_index].type));
                break;
            }
            continue;
        }

        ESP_LOGI(TAG, "%s drive %u: loaded, source unknown", action, drive_index + 1U);
    }
}

static void app_boot_slot6(void)
{
    if (!s_machine.slot6_rom_loaded) {
        ESP_LOGW(TAG, "Cannot boot slot 6 without Disk II ROM");
        return;
    }

    app_log_boot_drive_state("Boot slot6");
    app_boot_trace_arm("boot slot6");
    apple2_machine_set_pc(&s_machine, 0xC600U);
    ESP_LOGI(TAG, "Jumped to slot 6 boot ROM");
}

static void app_reset_and_boot_slot6(bool rom_loaded)
{
    memset(s_machine.memory, 0, 0xC000U);
    if (!rom_loaded) {
        app_text_cache_reset();
        app_show_status_screen("PLACE APPLE2PLUS.ROM IN", "roms/ AND REBUILD.", "RUNS WITHOUT ROM SHOW STATUS.");
        return;
    }
    apple2_machine_reset(&s_machine);
    app_text_cache_reset();
    app_log_boot_drive_state("Cold reset");
    app_boot_trace_arm("cold reset");
    ESP_LOGI(TAG, "Cold reset with current drive mounts");
}

static void app_clear_text_screen(void)
{
    for (uint8_t row = 0; row < APP_TEXT_ROWS; ++row) {
        const uint16_t base = apple2_text_row_address(false, row);
        for (uint8_t column = 0; column < APP_TEXT_COLUMNS; ++column) {
            s_machine.memory[(uint16_t)(base + column)] = (uint8_t)(' ' | 0x80U);
        }
    }
}

static void app_sd_picker_set_status(const char *status)
{
    if (status == NULL) {
        s_sd_picker.status[0] = '\0';
        return;
    }

    snprintf(s_sd_picker.status, sizeof(s_sd_picker.status), "%s", status);
}

static size_t app_sd_picker_item_count(unsigned drive_index)
{
    size_t count = 1U + app_sd_disk_count();

    if (drive_index < 2U && s_builtin_drives[drive_index].present) {
        count++;
    }

    return count;
}

static app_sd_picker_item_t app_sd_picker_item_for_index(unsigned drive_index, size_t item_index)
{
    app_sd_picker_item_t item = {
        .kind = APP_SD_PICKER_ITEM_EJECT,
        .sd_index = -1,
    };

    if (drive_index < 2U && s_builtin_drives[drive_index].present) {
        if (item_index == 0U) {
            item.kind = APP_SD_PICKER_ITEM_BUILTIN;
            return item;
        }
        item_index--;
    }

    if (item_index == 0U) {
        return item;
    }

    item_index--;
    if (item_index < app_sd_disk_count()) {
        item.kind = APP_SD_PICKER_ITEM_SD;
        item.sd_index = (int)item_index;
    }
    return item;
}

static int app_sd_picker_selection_for_current_drive(unsigned drive_index)
{
    int selection = 0;

    if (drive_index >= 2U) {
        return 0;
    }
    if (s_builtin_drives[drive_index].present) {
        if (app_sd_drive_index(drive_index) < 0 && apple2_disk2_drive_loaded(&s_machine.disk2, drive_index)) {
            return 0;
        }
        selection = 1;
    }
    if (app_sd_drive_index(drive_index) >= 0) {
        selection += 1 + app_sd_drive_index(drive_index);
    }
    return selection;
}

static bool app_sd_picker_item_is_current(unsigned drive_index, app_sd_picker_item_t item)
{
    if (drive_index >= 2U) {
        return false;
    }

    switch (item.kind) {
    case APP_SD_PICKER_ITEM_BUILTIN:
        return app_sd_drive_index(drive_index) < 0 &&
               s_builtin_drives[drive_index].present &&
               apple2_disk2_drive_loaded(&s_machine.disk2, drive_index);
    case APP_SD_PICKER_ITEM_EJECT:
        return app_sd_drive_index(drive_index) < 0 &&
               !apple2_disk2_drive_loaded(&s_machine.disk2, drive_index);
    case APP_SD_PICKER_ITEM_SD:
        return app_sd_drive_index(drive_index) == item.sd_index;
    default:
        return false;
    }
}

static void app_sd_picker_format_item(unsigned drive_index,
                                      app_sd_picker_item_t item,
                                      char *buffer,
                                      size_t buffer_size)
{
    switch (item.kind) {
    case APP_SD_PICKER_ITEM_BUILTIN:
        snprintf(buffer,
                 buffer_size,
                 "EMBEDDED: %s",
                 s_builtin_drives[drive_index].label);
        break;
    case APP_SD_PICKER_ITEM_SD: {
        const app_sd_disk_entry_t *entry = app_sd_disk_entry((size_t)item.sd_index);
        if (item.sd_index >= 0 && entry != NULL) {
            snprintf(buffer,
                     buffer_size,
                     "%s%s",
                     entry->is_directory ? "/" : "",
                     entry->name);
        } else {
            snprintf(buffer, buffer_size, "INVALID SD ENTRY");
        }
        break;
    }
    case APP_SD_PICKER_ITEM_EJECT:
    default:
        snprintf(buffer, buffer_size, "EJECT DRIVE");
        break;
    }
}

static void app_sd_picker_sync_scroll(void)
{
    const int list_rows = 15;
    const int item_count = (int)app_sd_picker_item_count(s_sd_picker.drive_index);

    if (item_count <= 0) {
        s_sd_picker.selected_item = 0;
        s_sd_picker.scroll_item = 0;
        return;
    }
    if (s_sd_picker.selected_item < 0) {
        s_sd_picker.selected_item = 0;
    } else if (s_sd_picker.selected_item >= item_count) {
        s_sd_picker.selected_item = item_count - 1;
    }
    if (s_sd_picker.scroll_item > s_sd_picker.selected_item) {
        s_sd_picker.scroll_item = s_sd_picker.selected_item;
    }
    if ((s_sd_picker.scroll_item + list_rows) <= s_sd_picker.selected_item) {
        s_sd_picker.scroll_item = s_sd_picker.selected_item - list_rows + 1;
    }
    if (s_sd_picker.scroll_item < 0) {
        s_sd_picker.scroll_item = 0;
    }
    if (item_count > list_rows && s_sd_picker.scroll_item > (item_count - list_rows)) {
        s_sd_picker.scroll_item = item_count - list_rows;
    }
}

static void app_sd_picker_render(void)
{
    char line[APP_TEXT_COLUMNS + 1U];
    const unsigned drive = (unsigned)s_sd_picker.drive_index;
    const size_t item_count = app_sd_picker_item_count(drive);
    const int list_rows = 15;

    app_clear_text_screen();
    s_machine.video.text_mode = true;
    s_machine.video.mixed_mode = false;
    s_machine.video.page2 = false;
    s_machine.video.hires_mode = false;

    snprintf(line, sizeof(line), "SD DISK PICKER DRIVE %u", drive + 1U);
    app_puts_at(0, 0, line);
    app_puts_at(1, 0, "I/K MOVE  ENTER SELECT");
    app_puts_at(2, 0, s_sd_picker.path_depth > 0U
                      ? "ESC BACK  FN+0 RESCAN"
                      : "ESC CANCEL  FN+0 RESCAN");
    app_puts_at(3, 0, "FN+3/4 ORDER  FN+5/6 DRIVE");

    {
        const int sd_idx = app_sd_drive_index(drive);
        const app_sd_disk_entry_t *sd_entry =
            (sd_idx >= 0) ? app_sd_disk_entry((size_t)sd_idx) : NULL;

        if (sd_entry != NULL) {
            snprintf(line, sizeof(line), "CURRENT: %.31s", sd_entry->name);
        } else if (s_builtin_drives[drive].present && apple2_disk2_drive_loaded(&s_machine.disk2, drive)) {
            snprintf(line, sizeof(line), "CURRENT: %.31s", s_builtin_drives[drive].label);
        } else {
            snprintf(line, sizeof(line), "CURRENT: EMPTY");
        }
    }
    app_puts_at(5, 0, line);

    snprintf(line,
             sizeof(line),
             "DSK ORDER: %s",
             app_sd_dsk_order_override_name(app_sd_dsk_order_override(drive)));
    app_puts_at(6, 0, line);

    for (int row = 0; row < list_rows; ++row) {
        const int item_index = s_sd_picker.scroll_item + row;

        if ((size_t)item_index >= item_count) {
            break;
        }

        {
            const app_sd_picker_item_t item = app_sd_picker_item_for_index(drive, (size_t)item_index);
            char item_label[APP_TEXT_COLUMNS + 1U];
            const bool selected = item_index == s_sd_picker.selected_item;
            const bool current = app_sd_picker_item_is_current(drive, item);

            app_sd_picker_format_item(drive, item, item_label, sizeof(item_label));
            snprintf(line,
                     sizeof(line),
                     "%c%c %-36.36s",
                     selected ? '>' : ' ',
                     current ? '*' : ' ',
                     item_label);
            app_puts_at((uint8_t)(8 + row), 0, line);
        }
    }

    if (item_count > (size_t)list_rows) {
        snprintf(line,
                 sizeof(line),
                 "ITEM %d/%u",
                 s_sd_picker.selected_item + 1,
                 (unsigned)item_count);
        app_puts_at(23, 0, line);
    }
    if (s_sd_picker.status[0] != '\0') {
        app_puts_at(23, 14, s_sd_picker.status);
    }

    app_text_cache_reset();
}

static void app_sd_picker_close(void)
{
    memcpy(&s_machine.memory[APP_TEXT_SCREEN_BASE],
           s_sd_picker.saved_text_pages,
           APP_TEXT_SCREEN_BYTES);
    s_machine.video = s_sd_picker.saved_video;
    memset(&s_sd_picker, 0, sizeof(s_sd_picker));
    app_text_cache_reset();
}

static bool app_sd_picker_apply_selection(void)
{
    const unsigned drive = (unsigned)s_sd_picker.drive_index;
    const app_sd_picker_item_t item =
        app_sd_picker_item_for_index(drive, (size_t)s_sd_picker.selected_item);

    app_flush_dirty_tracks();

    switch (item.kind) {
    case APP_SD_PICKER_ITEM_BUILTIN:
        app_sd_close_drive(&s_machine, drive);
        app_sd_set_drive_index(drive, -1);
        if (app_restore_builtin_drive(drive)) {
            ESP_LOGI(TAG,
                     "Restored embedded disk in drive %u (%s)",
                     drive + 1U,
                     s_builtin_drives[drive].label);
            return true;
        }
        app_sd_picker_set_status("RESTORE FAILED");
        return false;
    case APP_SD_PICKER_ITEM_SD:
        if (item.sd_index >= 0 && app_sd_mount_disk(&s_machine, drive, (size_t)item.sd_index)) {
            return true;
        }
        app_sd_picker_set_status("MOUNT FAILED");
        return false;
    case APP_SD_PICKER_ITEM_EJECT:
    default:
        app_sd_close_drive(&s_machine, drive);
        app_sd_set_drive_index(drive, -1);
        apple2_disk2_unload_drive(&s_machine.disk2, drive);
        ESP_LOGI(TAG, "Ejected drive %u", drive + 1U);
        return true;
    }
}

static void app_sd_picker_open(unsigned drive_index)
{
    if (drive_index >= 2U) {
        return;
    }

    memcpy(s_sd_picker.saved_text_pages,
           &s_machine.memory[APP_TEXT_SCREEN_BASE],
           APP_TEXT_SCREEN_BYTES);
    s_sd_picker.saved_video = s_machine.video;
    s_sd_picker.active = true;
    s_sd_picker.drive_index = (uint8_t)drive_index;
    s_sd_picker.selected_item = app_sd_picker_selection_for_current_drive(drive_index);
    s_sd_picker.scroll_item = 0;
    snprintf(s_sd_picker.browse_path, sizeof(s_sd_picker.browse_path),
             "%s", APP_SD_MOUNT_POINT);
    s_sd_picker.path_depth = 0;
    app_sd_picker_set_status(NULL);
    app_sd_picker_sync_scroll();
    app_sd_picker_render();
}

static void app_sd_picker_switch_drive(unsigned drive_index)
{
    if (!s_sd_picker.active || drive_index >= 2U) {
        return;
    }

    s_sd_picker.drive_index = (uint8_t)drive_index;
    s_sd_picker.selected_item = app_sd_picker_selection_for_current_drive(drive_index);
    s_sd_picker.scroll_item = 0;
    app_sd_picker_set_status(NULL);
    app_sd_picker_sync_scroll();
    app_sd_picker_render();
}

static void app_sd_picker_handle_input(uint8_t ascii)
{
    switch (ascii) {
    case 0x1BU:
        if (s_sd_picker.path_depth > 0U) {
            char *last_slash = strrchr(s_sd_picker.browse_path, '/');
            if (last_slash != NULL && last_slash != s_sd_picker.browse_path) {
                *last_slash = '\0';
            }
            s_sd_picker.path_depth--;
            app_sd_scan_directory(s_sd_picker.browse_path);
            s_sd_picker.selected_item = 0;
            s_sd_picker.scroll_item = 0;
            app_sd_picker_set_status(NULL);
            app_sd_picker_sync_scroll();
            app_sd_picker_render();
            return;
        }
        app_sd_picker_close();
        return;
    case '\r': {
        const unsigned sel_drive = (unsigned)s_sd_picker.drive_index;
        const app_sd_picker_item_t sel_item =
            app_sd_picker_item_for_index(sel_drive,
                                         (size_t)s_sd_picker.selected_item);

        {
            const app_sd_disk_entry_t *sel_entry =
                (sel_item.sd_index >= 0) ? app_sd_disk_entry((size_t)sel_item.sd_index) : NULL;

        if (sel_item.kind == APP_SD_PICKER_ITEM_SD &&
            sel_entry != NULL &&
            sel_entry->is_directory) {
            if (s_sd_picker.path_depth >= 4U) {
                app_sd_picker_set_status("DEPTH LIMIT");
                app_sd_picker_render();
                return;
            }
            {
                size_t cur_len = strlen(s_sd_picker.browse_path);
                size_t name_len = strlen(sel_entry->name);
                if (cur_len + 1U + name_len + 1U > sizeof(s_sd_picker.browse_path)) {
                    return;
                }
                s_sd_picker.browse_path[cur_len] = '/';
                memcpy(&s_sd_picker.browse_path[cur_len + 1U],
                       sel_entry->name, name_len + 1U);
            }
            s_sd_picker.path_depth++;
            app_sd_scan_directory(s_sd_picker.browse_path);
            s_sd_picker.selected_item = 0;
            s_sd_picker.scroll_item = 0;
            app_sd_picker_set_status(NULL);
            app_sd_picker_sync_scroll();
            app_sd_picker_render();
            return;
        }

        if (app_sd_picker_apply_selection()) {
            app_sd_picker_close();
        } else {
            app_sd_picker_render();
        }
        return;
        }
    }
    case 'i':
    case 'I':
    case 0x0BU:
        s_sd_picker.selected_item--;
        break;
    case 'k':
    case 'K':
    case 0x0AU:
        s_sd_picker.selected_item++;
        break;
    case CARDPUTER_INPUT_CMD_SD_RESCAN:
        snprintf(s_sd_picker.browse_path, sizeof(s_sd_picker.browse_path),
                 "%s", APP_SD_MOUNT_POINT);
        s_sd_picker.path_depth = 0;
        app_sd_rescan(&s_machine);
        s_sd_picker.selected_item = app_sd_picker_selection_for_current_drive(s_sd_picker.drive_index);
        s_sd_picker.scroll_item = 0;
        app_sd_picker_set_status("RESCANNED");
        break;
    case CARDPUTER_INPUT_CMD_SD_ORDER1:
        app_sd_cycle_dsk_order(&s_machine, 0U);
        app_sd_picker_set_status("DRIVE 1 ORDER");
        break;
    case CARDPUTER_INPUT_CMD_SD_ORDER2:
        app_sd_cycle_dsk_order(&s_machine, 1U);
        app_sd_picker_set_status("DRIVE 2 ORDER");
        break;
    case CARDPUTER_INPUT_CMD_SD_PICKER1:
        app_sd_picker_switch_drive(0U);
        return;
    case CARDPUTER_INPUT_CMD_SD_PICKER2:
        app_sd_picker_switch_drive(1U);
        return;
    default:
        return;
    }

    app_sd_picker_sync_scroll();
    app_sd_picker_render();
}

static void app_text_cache_reset(void)
{
    memset(&s_text_cache, 0, sizeof(s_text_cache));
}

static void app_text_cache_mark_hidden(void)
{
    s_text_cache.displayed = false;
}

static bool app_text_frame_dirty(const apple2_machine_t *machine)
{
    bool changed = !s_text_cache.valid || !s_text_cache.displayed ||
                   s_text_cache.page2 != machine->video.page2 ||
                   s_text_cache.flash_state != machine->video.flash_state;
    size_t offset = 0U;

    for (uint8_t row = 0; row < APP_TEXT_ROWS; ++row) {
        const uint16_t row_address = apple2_text_row_address(machine->video.page2, row);
        const uint8_t *source = &machine->memory[row_address];

        if (!changed && memcmp(&s_text_cache.cells[offset], source, APP_TEXT_COLUMNS) == 0) {
            offset += APP_TEXT_COLUMNS;
            continue;
        }

        memcpy(&s_text_cache.cells[offset], source, APP_TEXT_COLUMNS);
        changed = true;
        offset += APP_TEXT_COLUMNS;
    }

    s_text_cache.valid = true;
    s_text_cache.page2 = machine->video.page2;
    s_text_cache.flash_state = machine->video.flash_state;
    return changed;
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
    app_sd_set_has_builtin(drive_index, true);
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

static void app_flush_dirty_tracks(void)
{
    app_sd_flush_dirty_tracks(&s_machine);
}

static bool app_sd_restore_builtin_callback(unsigned drive_index, void *context)
{
    (void)context;
    return app_restore_builtin_drive(drive_index);
}

static void app_sd_flush_callback(void *context)
{
    apple2_machine_t *machine = context;
    if (!apple2_disk2_flush(&machine->disk2)) {
        ESP_LOGW(TAG, "disk flush failed");
    }
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

        {
            const bool is_prodos = app_sd_probe_dsk_image_is_prodos(dos_3_3_dsk_start, image_size);
            image_order = is_prodos
                              ? APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL
                              : APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
        }
        loaded = app_load_memory_drive(0U,
                                       APP_DISK_IMAGE_DSK,
                                       dos_3_3_dsk_start,
                                       image_size,
                                       image_order);
        if (loaded) {
            ESP_LOGI(TAG,
                     "Loaded embedded .dsk disk (%u bytes, %s order)",
                     (unsigned)image_size,
                     (image_order == APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL) ? "ProDOS" : "DOS");
        }
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

static const char *app_speed_mode_name(void)
{
    switch (s_speed_mode) {
    case APP_SPEED_MODE_2X:
        return "2x";
    case APP_SPEED_MODE_4X:
        return "4x";
    case APP_SPEED_MODE_AUTO_DISK:
        return "auto";
    case APP_SPEED_MODE_1X:
    default:
        return "1x";
    }
}

static uint8_t app_speed_multiplier(void)
{
    switch (s_speed_mode) {
    case APP_SPEED_MODE_2X:
        return 2U;
    case APP_SPEED_MODE_4X:
        return 4U;
    case APP_SPEED_MODE_AUTO_DISK:
        return s_machine.disk2.motor_on ? 4U : 1U;
    case APP_SPEED_MODE_1X:
    default:
        return 1U;
    }
}

static void app_toggle_speed_multiplier(void)
{
    s_speed_mode = (app_speed_mode_t)(((unsigned)s_speed_mode + 1U) % 4U);
    if (s_speed_mode == APP_SPEED_MODE_AUTO_DISK) {
        ESP_LOGI(TAG, "Emulation speed set to auto (1x idle / 4x disk)");
    } else {
        ESP_LOGI(TAG, "Emulation speed set to %s", app_speed_mode_name());
    }
}

static void app_perf_log_if_due(int64_t now_us)
{
    const int64_t elapsed_us = now_us - s_perf.window_start_us;

    if (APP_PERF_LOG_INTERVAL_US <= 0 || elapsed_us < APP_PERF_LOG_INTERVAL_US) {
        return;
    }

    {
        const app_sd_perf_t sd_perf = app_sd_perf_read(true);

        s_perf.dsk_probe_us += sd_perf.dsk_probe_us;
        s_perf.dsk_probes += sd_perf.dsk_probes;
        s_perf.sd_mount_us += sd_perf.sd_mount_us;
        s_perf.sd_mounts += sd_perf.sd_mounts;
    }
    app_perf_log(&s_perf, now_us, app_speed_mode_name(), (unsigned)app_speed_multiplier());
}

void app_main(void)
{
    apple2_config_t apple2_config = {
        .cpu_hz = CONFIG_M5APPLE2_CPU_HZ,
    };
    bool rom_loaded = false;
    bool slot6_loaded = false;
    bool drive0_loaded = false;
    bool prev_motor_on = false;
    int64_t last_cpu_tick_us;
    int64_t last_frame_tick_us;
    uint64_t cpu_cycle_credit = 0U;

    apple2_machine_init(&s_machine, &apple2_config);

    ESP_ERROR_CHECK(cardputer_display_init(&s_display));
    ESP_ERROR_CHECK(cardputer_keyboard_init());
    ESP_LOGI(TAG, "Cardputer display ready: %" PRIu16 "x%" PRIu16,
             s_display.native_width, s_display.native_height);

#if CONFIG_M5APPLE2_AUDIO_ENABLED
    cardputer_audio_t *audio = cardputer_audio_init();
    if (audio != NULL) {
        apple2_machine_set_speaker_callback(&s_machine, app_speaker_toggle, audio);
        ESP_LOGI(TAG, "Audio output enabled");
    }
#endif

    rom_loaded = app_load_system_rom();
    slot6_loaded = app_load_slot6_rom();
    drive0_loaded = app_load_drive0_image();
    app_sd_init(&s_machine,
                app_sd_restore_builtin_callback,
                app_sd_flush_callback,
                &s_machine);
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
    app_perf_reset(&s_perf, last_cpu_tick_us);
    app_text_cache_reset();

    while (true) {
        uint8_t ascii = 0;
        if (cardputer_keyboard_poll_ascii(&ascii)) {
            if (s_sd_picker.active) {
                app_sd_picker_handle_input(ascii);
            } else if (ascii == CARDPUTER_INPUT_CMD_SD_PICKER1) {
                app_sd_picker_open(0U);
            } else if (ascii == CARDPUTER_INPUT_CMD_SD_PICKER2) {
                app_sd_picker_open(1U);
            } else if (ascii == CARDPUTER_INPUT_CMD_RESET_BOOT_SLOT6) {
                app_reset_and_boot_slot6(rom_loaded);
            } else if (ascii == CARDPUTER_INPUT_CMD_BOOT_SLOT6) {
                app_boot_slot6();
            } else if (ascii == CARDPUTER_INPUT_CMD_SD_DRIVE1) {
                app_sd_cycle_drive(&s_machine, 0U);
            } else if (ascii == CARDPUTER_INPUT_CMD_SD_DRIVE2) {
                app_sd_cycle_drive(&s_machine, 1U);
            } else if (ascii == CARDPUTER_INPUT_CMD_SD_ORDER1) {
                app_sd_cycle_dsk_order(&s_machine, 0U);
            } else if (ascii == CARDPUTER_INPUT_CMD_SD_ORDER2) {
                app_sd_cycle_dsk_order(&s_machine, 1U);
            } else if (ascii == CARDPUTER_INPUT_CMD_SD_RESCAN) {
                app_sd_rescan(&s_machine);
            } else if (ascii == CARDPUTER_INPUT_CMD_SPEED_TOGGLE) {
                app_toggle_speed_multiplier();
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
        if (rom_loaded && !s_sd_picker.active) {
            const uint64_t target_cpu_hz =
                (uint64_t)apple2_config.cpu_hz * (uint64_t)app_speed_multiplier();
            const uint32_t max_step_slice_cycles =
                (target_cpu_hz / 120ULL) > 1024ULL ? (uint32_t)(target_cpu_hz / 120ULL) : 1024U;
            const uint64_t elapsed_credit =
                (uint64_t)(now_us - last_cpu_tick_us) * target_cpu_hz / 1000000ULL;

            cpu_cycle_credit += elapsed_credit;
            if (cpu_cycle_credit > target_cpu_hz / 10ULL) {
                cpu_cycle_credit = target_cpu_hz / 10ULL;
            }
            if (cpu_cycle_credit > 0U) {
                const int64_t step_start_us = esp_timer_get_time();
                unsigned step_slices = 0U;

                while (cpu_cycle_credit > 0U && step_slices < 8U) {
                    const uint32_t budget = (cpu_cycle_credit > max_step_slice_cycles)
                                                ? max_step_slice_cycles
                                                : (uint32_t)cpu_cycle_credit;
                    const uint64_t previous_cycles = s_machine.total_cycles;
                    uint64_t executed_cycles;
#if CONFIG_M5APPLE2_DETAILED_PERF_PROFILE
                    const bool motor_before = s_machine.disk2.motor_on;
                    const uint8_t drive_before = s_machine.disk2.active_drive;
                    const uint8_t quarter_track_before = s_machine.disk2.quarter_track[drive_before];
                    const uint32_t nibble_before = s_machine.disk2.nibble_pos[drive_before];
                    const int64_t slice_start_us = esp_timer_get_time();
#endif

                    if (s_boot_trace.active) {
                        executed_cycles = 0U;
                        while (executed_cycles < budget) {
                            const uint16_t pc = apple2_machine_cpu_state(&s_machine).pc;

                            app_boot_trace_maybe_dump_transition(pc);
                            app_boot_trace_record_pc(pc);
                            executed_cycles += apple2_machine_step_instruction(&s_machine);
                        }
                    } else {
                        apple2_machine_step(&s_machine, budget);
                        executed_cycles = s_machine.total_cycles - previous_cycles;
                    }
                    s_perf.emulated_cycles += executed_cycles;
#if CONFIG_M5APPLE2_DETAILED_PERF_PROFILE
                    {
                        const bool motor_after = s_machine.disk2.motor_on;
                        const uint8_t drive_after = s_machine.disk2.active_drive;
                        const uint8_t quarter_track_after = s_machine.disk2.quarter_track[drive_after];
                        const uint32_t nibble_after = s_machine.disk2.nibble_pos[drive_after];
                        const uint16_t track_length = s_machine.disk2.track_cache_length;
                        const uint64_t step_us = (uint64_t)(esp_timer_get_time() - slice_start_us);

                        if (motor_before || motor_after) {
                            s_perf.cpu_step_disk_us += step_us;
                            s_perf.disk_active_slices++;
                        } else {
                            s_perf.cpu_step_idle_us += step_us;
                            s_perf.idle_slices++;
                        }
                        if (drive_before != drive_after) {
                            s_perf.drive_change_slices++;
                        }
                        if (drive_before != drive_after || quarter_track_before != quarter_track_after) {
                            s_perf.quarter_track_change_slices++;
                        }
                        if (drive_before != drive_after || nibble_before != nibble_after) {
                            s_perf.nibble_progress_slices++;
                        }
                        if ((motor_before || motor_after) &&
                            drive_before == drive_after &&
                            quarter_track_before == quarter_track_after &&
                            track_length != 0U &&
                            nibble_before != nibble_after) {
                            if (nibble_after > nibble_before) {
                                s_perf.nibble_advance_bytes += (uint64_t)(nibble_after - nibble_before);
                            } else {
                                s_perf.nibble_advance_bytes +=
                                    (uint64_t)((uint32_t)track_length - nibble_before + nibble_after);
                                s_perf.nibble_wrap_slices++;
                            }
                        }
                    }
#endif
                    if (executed_cycles >= cpu_cycle_credit) {
                        cpu_cycle_credit = 0U;
                    } else {
                        cpu_cycle_credit -= executed_cycles;
                    }
                    step_slices++;
                }
                s_perf.cpu_step_us += (uint64_t)(esp_timer_get_time() - step_start_us);
#if CONFIG_M5APPLE2_AUDIO_ENABLED
                if (audio != NULL) {
                    cardputer_audio_flush(audio, s_machine.total_cycles,
                                          apple2_config.cpu_hz * app_speed_multiplier());
                }
#endif
                if (prev_motor_on && !s_machine.disk2.motor_on) {
                    app_flush_dirty_tracks();
                }
                prev_motor_on = s_machine.disk2.motor_on;
            }
        }
        last_cpu_tick_us = now_us;

        if ((now_us - last_frame_tick_us) >= APP_FRAME_INTERVAL_US) {
            if (s_machine.video.text_mode) {
                s_perf.text_frames++;
                if (app_text_frame_dirty(&s_machine)) {
                    const int64_t compose_start_us = esp_timer_get_time();

                    s_perf.frame_compose_us += (uint64_t)(esp_timer_get_time() - compose_start_us);
                    {
                        const int64_t present_start_us = esp_timer_get_time();

                        ESP_ERROR_CHECK(cardputer_display_present_apple2_text40(&s_display,
                                                                                s_machine.memory,
                                                                                &s_machine.video));
                        s_perf.frame_present_us += (uint64_t)(esp_timer_get_time() - present_start_us);
                    }
                    s_text_cache.displayed = true;
                    s_perf.frames_presented++;
                } else {
                    s_perf.text_frames_skipped++;
                }
            } else {
                const int64_t compose_start_us = esp_timer_get_time();

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
                app_text_cache_mark_hidden();
                s_perf.frames_presented++;
            }
            last_frame_tick_us += ((now_us - last_frame_tick_us) / APP_FRAME_INTERVAL_US) * APP_FRAME_INTERVAL_US;
        }

        app_boot_trace_poll(now_us);
        app_perf_log_if_due(now_us);
        if (s_sd_picker.active) {
            vTaskDelay(pdMS_TO_TICKS(1));
        } else if (cpu_cycle_credit < (uint64_t)(((uint64_t)apple2_config.cpu_hz / 120ULL) > 1024ULL
                                              ? (uint32_t)((uint64_t)apple2_config.cpu_hz / 120ULL)
                                              : 1024U) &&
            app_speed_multiplier() == 1U &&
            !s_machine.disk2.motor_on &&
            (last_frame_tick_us + APP_FRAME_INTERVAL_US - now_us) > 2000LL) {
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            taskYIELD();
        }
    }
}
