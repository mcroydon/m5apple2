/*
 * SPDX-License-Identifier: MIT
 * Disk picker UI for m5apple2.
 *
 * Presents an on-screen list of available disk images (embedded builtins
 * and SD-card entries) and lets the user mount, eject, or browse
 * directories.  All rendering goes through the Apple II text-page memory
 * so the existing display pipeline can present it.
 */

#ifndef APP_PICKER_H
#define APP_PICKER_H

#include "app_sd.h"
#include "apple2/apple2_machine.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * Callback used by the picker to flush dirty disk tracks before
 * switching drive contents.
 */
typedef void (*app_picker_flush_fn)(apple2_machine_t *machine);

/**
 * Callback used by the picker to restore a builtin (embedded) drive
 * image.  Returns true on success.
 */
typedef bool (*app_picker_restore_builtin_fn)(unsigned drive_index,
                                              apple2_machine_t *machine);

/**
 * Descriptor for a single built-in (embedded) drive that the picker can
 * offer as a selection.
 */
typedef struct {
    bool present;
    char label[24];
} app_picker_builtin_t;

/**
 * Open the disk picker for the given drive (0 or 1).
 *
 * @param drive_index       Which drive slot to pick for (0 or 1).
 * @param machine           The emulator machine (text pages are saved).
 * @param builtins          Array of 2 builtin descriptors (may be NULL if
 *                          no builtins are registered).
 * @param flush_fn          Called before a drive swap to flush dirty tracks.
 * @param restore_builtin_fn Called to restore an embedded drive image.
 */
void app_picker_open(unsigned drive_index,
                     apple2_machine_t *machine,
                     const app_picker_builtin_t builtins[2],
                     app_picker_flush_fn flush_fn,
                     app_picker_restore_builtin_fn restore_builtin_fn);

/**
 * Return true when the picker overlay is active.
 */
bool app_picker_is_active(void);

/**
 * Feed a key press into the picker.  Handles navigation (I/K), selection
 * (Enter), cancel / back (ESC), drive switching (Fn+5/6), rescan, and
 * order cycling.
 */
void app_picker_handle_key(uint8_t key, apple2_machine_t *machine);

/**
 * Re-render the picker into the Apple II text page.  Call after any
 * state change that should be reflected on screen.
 */
void app_picker_render(apple2_machine_t *machine);

/**
 * Close the picker and restore the previously saved text pages and
 * video state.
 */
void app_picker_close(apple2_machine_t *machine);

#endif /* APP_PICKER_H */
