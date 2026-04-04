/*
 * SPDX-License-Identifier: MIT
 * Disk picker UI for m5apple2.
 */

#ifdef ESP_PLATFORM

#include "app_picker.h"

#include <stdio.h>
#include <string.h>

#include "apple2/apple2_video.h"
#include "cardputer/cardputer_input.h"

#include "esp_log.h"

static const char *TAG = "app_picker";

/* ---------- text-page constants ---------------------------------------- */

#define PICKER_TEXT_ROWS      24U
#define PICKER_TEXT_COLUMNS   40U
#define PICKER_TEXT_BYTES     (PICKER_TEXT_ROWS * PICKER_TEXT_COLUMNS)
#define PICKER_TEXT_SCREEN_BASE  0x0400U
#define PICKER_TEXT_SCREEN_BYTES 0x0800U
#define PICKER_LIST_ROWS      15

/* ---------- item types ------------------------------------------------- */

typedef enum {
    PICKER_ITEM_EJECT = 0,
    PICKER_ITEM_BUILTIN,
    PICKER_ITEM_SD,
} picker_item_kind_t;

typedef struct {
    picker_item_kind_t kind;
    int sd_index;
} picker_item_t;

/* ---------- picker state ----------------------------------------------- */

typedef struct {
    bool active;
    uint8_t drive_index;
    int selected_item;
    int scroll_item;
    apple2_video_state_t saved_video;
    uint8_t saved_text_pages[PICKER_TEXT_SCREEN_BYTES];
    char status[PICKER_TEXT_COLUMNS + 1U];
    char browse_path[APP_SD_PATH_MAX];
    uint8_t path_depth;
    app_picker_builtin_t builtins[2];
    app_picker_flush_fn flush_fn;
    app_picker_restore_builtin_fn restore_builtin_fn;
} picker_state_t;

static picker_state_t s_picker;

/* ---------- text helpers ----------------------------------------------- */

static void picker_puts_at(apple2_machine_t *machine,
                           uint8_t row,
                           uint8_t column,
                           const char *text)
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
        machine->memory[address++] = (uint8_t)(ch | 0x80U);
        column++;
    }
}

static void picker_clear_text_screen(apple2_machine_t *machine)
{
    for (uint8_t row = 0; row < PICKER_TEXT_ROWS; ++row) {
        const uint16_t base = apple2_text_row_address(false, row);
        for (uint8_t column = 0; column < PICKER_TEXT_COLUMNS; ++column) {
            machine->memory[(uint16_t)(base + column)] = (uint8_t)(' ' | 0x80U);
        }
    }
}

/* ---------- item enumeration ------------------------------------------- */

static size_t picker_item_count(unsigned drive_index)
{
    size_t count = 1U + app_sd_disk_count();

    if (drive_index < 2U && s_picker.builtins[drive_index].present) {
        count++;
    }

    return count;
}

static picker_item_t picker_item_for_index(unsigned drive_index, size_t item_index)
{
    picker_item_t item = {
        .kind = PICKER_ITEM_EJECT,
        .sd_index = -1,
    };

    if (drive_index < 2U && s_picker.builtins[drive_index].present) {
        if (item_index == 0U) {
            item.kind = PICKER_ITEM_BUILTIN;
            return item;
        }
        item_index--;
    }

    if (item_index == 0U) {
        return item;
    }

    item_index--;
    if (item_index < app_sd_disk_count()) {
        item.kind = PICKER_ITEM_SD;
        item.sd_index = (int)item_index;
    }
    return item;
}

static int picker_selection_for_current_drive(unsigned drive_index,
                                              const apple2_machine_t *machine)
{
    int selection = 0;

    if (drive_index >= 2U) {
        return 0;
    }
    if (s_picker.builtins[drive_index].present) {
        if (app_sd_drive_index(drive_index) < 0 &&
            apple2_disk2_drive_loaded(&machine->disk2, drive_index)) {
            return 0;
        }
        selection = 1;
    }
    if (app_sd_drive_index(drive_index) >= 0) {
        selection += 1 + app_sd_drive_index(drive_index);
    }
    return selection;
}

static bool picker_item_is_current(unsigned drive_index,
                                   picker_item_t item,
                                   const apple2_machine_t *machine)
{
    if (drive_index >= 2U) {
        return false;
    }

    switch (item.kind) {
    case PICKER_ITEM_BUILTIN:
        return app_sd_drive_index(drive_index) < 0 &&
               s_picker.builtins[drive_index].present &&
               apple2_disk2_drive_loaded(&machine->disk2, drive_index);
    case PICKER_ITEM_EJECT:
        return app_sd_drive_index(drive_index) < 0 &&
               !apple2_disk2_drive_loaded(&machine->disk2, drive_index);
    case PICKER_ITEM_SD:
        return app_sd_drive_index(drive_index) == item.sd_index;
    default:
        return false;
    }
}

static void picker_format_item(unsigned drive_index,
                               picker_item_t item,
                               char *buffer,
                               size_t buffer_size)
{
    switch (item.kind) {
    case PICKER_ITEM_BUILTIN:
        snprintf(buffer,
                 buffer_size,
                 "EMBEDDED: %s",
                 s_picker.builtins[drive_index].label);
        break;
    case PICKER_ITEM_SD: {
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
    case PICKER_ITEM_EJECT:
    default:
        snprintf(buffer, buffer_size, "EJECT DRIVE");
        break;
    }
}

/* ---------- scroll synchronisation ------------------------------------- */

static void picker_sync_scroll(void)
{
    const int list_rows = PICKER_LIST_ROWS;
    const int item_count = (int)picker_item_count(s_picker.drive_index);

    if (item_count <= 0) {
        s_picker.selected_item = 0;
        s_picker.scroll_item = 0;
        return;
    }
    if (s_picker.selected_item < 0) {
        s_picker.selected_item = 0;
    } else if (s_picker.selected_item >= item_count) {
        s_picker.selected_item = item_count - 1;
    }
    if (s_picker.scroll_item > s_picker.selected_item) {
        s_picker.scroll_item = s_picker.selected_item;
    }
    if ((s_picker.scroll_item + list_rows) <= s_picker.selected_item) {
        s_picker.scroll_item = s_picker.selected_item - list_rows + 1;
    }
    if (s_picker.scroll_item < 0) {
        s_picker.scroll_item = 0;
    }
    if (item_count > list_rows && s_picker.scroll_item > (item_count - list_rows)) {
        s_picker.scroll_item = item_count - list_rows;
    }
}

/* ---------- status ----------------------------------------------------- */

static void picker_set_status(const char *status)
{
    if (status == NULL) {
        s_picker.status[0] = '\0';
        return;
    }

    snprintf(s_picker.status, sizeof(s_picker.status), "%s", status);
}

/* ---------- apply selection -------------------------------------------- */

static bool picker_apply_selection(apple2_machine_t *machine)
{
    const unsigned drive = (unsigned)s_picker.drive_index;
    const picker_item_t item =
        picker_item_for_index(drive, (size_t)s_picker.selected_item);

    if (s_picker.flush_fn != NULL) {
        s_picker.flush_fn(machine);
    }

    switch (item.kind) {
    case PICKER_ITEM_BUILTIN:
        app_sd_close_drive(machine, drive);
        app_sd_set_drive_index(drive, -1);
        if (s_picker.restore_builtin_fn != NULL &&
            s_picker.restore_builtin_fn(drive, machine)) {
            ESP_LOGI(TAG,
                     "Restored embedded disk in drive %u (%s)",
                     drive + 1U,
                     s_picker.builtins[drive].label);
            return true;
        }
        picker_set_status("RESTORE FAILED");
        return false;
    case PICKER_ITEM_SD:
        if (item.sd_index >= 0 && app_sd_mount_disk(machine, drive, (size_t)item.sd_index)) {
            return true;
        }
        picker_set_status("MOUNT FAILED");
        return false;
    case PICKER_ITEM_EJECT:
    default:
        app_sd_close_drive(machine, drive);
        app_sd_set_drive_index(drive, -1);
        apple2_disk2_unload_drive(&machine->disk2, drive);
        ESP_LOGI(TAG, "Ejected drive %u", drive + 1U);
        return true;
    }
}

/* ---------- render ----------------------------------------------------- */

void app_picker_render(apple2_machine_t *machine)
{
    char line[PICKER_TEXT_COLUMNS + 1U];
    const unsigned drive = (unsigned)s_picker.drive_index;
    const size_t item_count = picker_item_count(drive);
    const int list_rows = PICKER_LIST_ROWS;

    picker_clear_text_screen(machine);
    machine->video.text_mode = true;
    machine->video.mixed_mode = false;
    machine->video.page2 = false;
    machine->video.hires_mode = false;

    snprintf(line, sizeof(line), "SD DISK PICKER DRIVE %u", drive + 1U);
    picker_puts_at(machine, 0, 0, line);
    picker_puts_at(machine, 1, 0, "I/K MOVE  ENTER SELECT");
    picker_puts_at(machine, 2, 0, "ESC CANCEL  FN+0 RESCAN");
    picker_puts_at(machine, 3, 0, "FN+3/4 ORDER  FN+5/6 DRIVE");

    {
        const int sd_idx = app_sd_drive_index(drive);
        const app_sd_disk_entry_t *sd_entry =
            (sd_idx >= 0) ? app_sd_disk_entry((size_t)sd_idx) : NULL;

        if (sd_entry != NULL) {
            snprintf(line, sizeof(line), "CURRENT: %.31s", sd_entry->name);
        } else if (s_picker.builtins[drive].present &&
                   apple2_disk2_drive_loaded(&machine->disk2, drive)) {
            snprintf(line, sizeof(line), "CURRENT: %.31s", s_picker.builtins[drive].label);
        } else {
            snprintf(line, sizeof(line), "CURRENT: EMPTY");
        }
    }
    picker_puts_at(machine, 5, 0, line);

    snprintf(line,
             sizeof(line),
             "DSK ORDER: %s",
             app_sd_dsk_order_override_name(app_sd_dsk_order_override(drive)));
    picker_puts_at(machine, 6, 0, line);

    for (int row = 0; row < list_rows; ++row) {
        const int item_index = s_picker.scroll_item + row;

        if ((size_t)item_index >= item_count) {
            break;
        }

        {
            const picker_item_t item = picker_item_for_index(drive, (size_t)item_index);
            char item_label[PICKER_TEXT_COLUMNS + 1U];
            const bool selected = item_index == s_picker.selected_item;
            const bool current = picker_item_is_current(drive, item, machine);

            picker_format_item(drive, item, item_label, sizeof(item_label));
            snprintf(line,
                     sizeof(line),
                     "%c%c %-36.36s",
                     selected ? '>' : ' ',
                     current ? '*' : ' ',
                     item_label);
            picker_puts_at(machine, (uint8_t)(8 + row), 0, line);
        }
    }

    if (item_count > (size_t)list_rows) {
        snprintf(line,
                 sizeof(line),
                 "ITEM %d/%u",
                 s_picker.selected_item + 1,
                 (unsigned)item_count);
        picker_puts_at(machine, 23, 0, line);
    }
    if (s_picker.status[0] != '\0') {
        picker_puts_at(machine, 23, 14, s_picker.status);
    }
}

/* ---------- open / close / switch -------------------------------------- */

void app_picker_open(unsigned drive_index,
                     apple2_machine_t *machine,
                     const app_picker_builtin_t builtins[2],
                     app_picker_flush_fn flush_fn,
                     app_picker_restore_builtin_fn restore_builtin_fn)
{
    if (drive_index >= 2U) {
        return;
    }

    memcpy(s_picker.saved_text_pages,
           &machine->memory[PICKER_TEXT_SCREEN_BASE],
           PICKER_TEXT_SCREEN_BYTES);
    s_picker.saved_video = machine->video;
    s_picker.active = true;
    s_picker.drive_index = (uint8_t)drive_index;
    s_picker.flush_fn = flush_fn;
    s_picker.restore_builtin_fn = restore_builtin_fn;
    if (builtins != NULL) {
        memcpy(s_picker.builtins, builtins, sizeof(s_picker.builtins));
    } else {
        memset(s_picker.builtins, 0, sizeof(s_picker.builtins));
    }
    s_picker.selected_item = picker_selection_for_current_drive(drive_index, machine);
    s_picker.scroll_item = 0;
    snprintf(s_picker.browse_path, sizeof(s_picker.browse_path),
             "%s", APP_SD_MOUNT_POINT);
    s_picker.path_depth = 0;
    picker_set_status(NULL);
    picker_sync_scroll();
    app_picker_render(machine);
}

bool app_picker_is_active(void)
{
    return s_picker.active;
}

void app_picker_close(apple2_machine_t *machine)
{
    memcpy(&machine->memory[PICKER_TEXT_SCREEN_BASE],
           s_picker.saved_text_pages,
           PICKER_TEXT_SCREEN_BYTES);
    machine->video = s_picker.saved_video;
    memset(&s_picker, 0, sizeof(s_picker));
}

static void picker_switch_drive(unsigned drive_index,
                                apple2_machine_t *machine)
{
    if (!s_picker.active || drive_index >= 2U) {
        return;
    }

    s_picker.drive_index = (uint8_t)drive_index;
    s_picker.selected_item = picker_selection_for_current_drive(drive_index, machine);
    s_picker.scroll_item = 0;
    picker_set_status(NULL);
    picker_sync_scroll();
    app_picker_render(machine);
}

/* ---------- key handling ----------------------------------------------- */

void app_picker_handle_key(uint8_t ascii, apple2_machine_t *machine)
{
    switch (ascii) {
    case 0x1BU:
        app_picker_close(machine);
        return;
    case '\r': {
        const unsigned sel_drive = (unsigned)s_picker.drive_index;
        const picker_item_t sel_item =
            picker_item_for_index(sel_drive,
                                     (size_t)s_picker.selected_item);

        {
            const app_sd_disk_entry_t *sel_entry =
                (sel_item.sd_index >= 0) ? app_sd_disk_entry((size_t)sel_item.sd_index) : NULL;

        if (sel_item.kind == PICKER_ITEM_SD &&
            sel_entry != NULL &&
            sel_entry->is_directory) {
            if (s_picker.path_depth >= 4U) {
                picker_set_status("DEPTH LIMIT");
                app_picker_render(machine);
                return;
            }
            {
                size_t cur_len = strlen(s_picker.browse_path);
                size_t name_len = strlen(sel_entry->name);
                if (cur_len + 1U + name_len + 1U > sizeof(s_picker.browse_path)) {
                    return;
                }
                s_picker.browse_path[cur_len] = '/';
                memcpy(&s_picker.browse_path[cur_len + 1U],
                       sel_entry->name, name_len + 1U);
            }
            s_picker.path_depth++;
            app_sd_scan_directory(s_picker.browse_path);
            s_picker.selected_item = 0;
            s_picker.scroll_item = 0;
            picker_set_status(NULL);
            picker_sync_scroll();
            app_picker_render(machine);
            return;
        }

        if (picker_apply_selection(machine)) {
            app_picker_close(machine);
        } else {
            app_picker_render(machine);
        }
        return;
        }
    }
    case 'i':
    case 'I':
    case 0x0BU:
        s_picker.selected_item--;
        break;
    case 'k':
    case 'K':
    case 0x0AU:
        s_picker.selected_item++;
        break;
    case CARDPUTER_INPUT_CMD_SD_RESCAN:
        snprintf(s_picker.browse_path, sizeof(s_picker.browse_path),
                 "%s", APP_SD_MOUNT_POINT);
        s_picker.path_depth = 0;
        app_sd_rescan(machine);
        s_picker.selected_item = picker_selection_for_current_drive(s_picker.drive_index, machine);
        s_picker.scroll_item = 0;
        picker_set_status("RESCANNED");
        break;
    case CARDPUTER_INPUT_CMD_SD_ORDER1:
        app_sd_cycle_dsk_order(machine, 0U);
        picker_set_status("DRIVE 1 ORDER");
        break;
    case CARDPUTER_INPUT_CMD_SD_ORDER2:
        app_sd_cycle_dsk_order(machine, 1U);
        picker_set_status("DRIVE 2 ORDER");
        break;
    case CARDPUTER_INPUT_CMD_SD_PICKER1:
        picker_switch_drive(0U, machine);
        return;
    case CARDPUTER_INPUT_CMD_SD_PICKER2:
        picker_switch_drive(1U, machine);
        return;
    default:
        return;
    }

    picker_sync_scroll();
    app_picker_render(machine);
}

#endif /* ESP_PLATFORM */
