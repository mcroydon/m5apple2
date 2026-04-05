/*
 * SPDX-License-Identifier: MIT
 * SD card disk image management for m5apple2.
 */

#ifndef APP_SD_H
#define APP_SD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apple2/apple2_machine.h"

#define APP_SD_MOUNT_POINT "/sd"
#define APP_SD_PATH_MAX    128U
#define APP_SD_NAME_MAX    40U
#define APP_SD_MAX_IMAGES  16U

typedef enum {
    APP_DISK_IMAGE_NONE = 0,
    APP_DISK_IMAGE_DO,
    APP_DISK_IMAGE_PO,
    APP_DISK_IMAGE_DSK,
    APP_DISK_IMAGE_NIB,
    APP_DISK_IMAGE_WOZ,
} app_disk_image_type_t;

typedef enum {
    APP_SD_DSK_ORDER_AUTO = 0,
    APP_SD_DSK_ORDER_DOS33,
    APP_SD_DSK_ORDER_PRODOS,
} app_sd_dsk_order_override_t;

typedef struct {
    char path[APP_SD_PATH_MAX];
    char name[APP_SD_NAME_MAX];
    app_disk_image_type_t type;
    bool is_directory;
} app_sd_disk_entry_t;

/**
 * Callback type for restoring a builtin (embedded) drive image.
 * Returns true on success.
 */
typedef bool (*app_sd_restore_builtin_fn)(unsigned drive_index, void *context);

/**
 * Callback type for flushing dirty disk tracks before a drive swap.
 */
typedef void (*app_sd_flush_fn)(void *context);

/**
 * Initialise the SD card subsystem: mount filesystem, perform initial scan,
 * and auto-mount default drives.
 *
 * @param machine         The emulator machine (needed for disk2 access).
 * @param restore_builtin Callback to restore an embedded drive image (may be NULL).
 * @param flush           Callback to flush dirty tracks (may be NULL).
 * @param callback_ctx    Opaque context forwarded to both callbacks.
 */
void app_sd_init(apple2_machine_t *machine,
                 app_sd_restore_builtin_fn restore_builtin,
                 app_sd_flush_fn flush,
                 void *callback_ctx);

/**
 * Return true if a builtin drive is registered for the given drive index.
 */
bool app_sd_has_builtin(unsigned drive_index);

/**
 * Register the presence of a builtin (embedded) drive for the given index.
 * The SD module records this so it knows whether builtins are available when
 * cycling drives or restoring defaults.
 */
void app_sd_set_has_builtin(unsigned drive_index, bool present);

/** Scan a directory on the SD card and populate the disk list. */
bool app_sd_scan_directory(const char *dir_path);

/** Mount a specific disk image into a drive. */
bool app_sd_mount_disk(apple2_machine_t *machine, unsigned drive_index, size_t disk_index);

/** Close (unmount) an SD-backed drive. */
void app_sd_close_drive(apple2_machine_t *machine, unsigned drive_index);

/** Cycle to the next SD disk image in the given drive. */
void app_sd_cycle_drive(apple2_machine_t *machine, unsigned drive_index);

/** Full rescan of the SD card and auto-mount default drives. */
void app_sd_rescan(apple2_machine_t *machine);

/** Restore default drives (builtins + first SD images). */
void app_sd_restore_default_drives(apple2_machine_t *machine);

/** Number of disk entries found during last scan. */
size_t app_sd_disk_count(void);

/** Return the disk entry at the given index (NULL if out of range). */
const app_sd_disk_entry_t *app_sd_disk_entry(size_t index);

/** Return the SD disk index currently mounted in the given drive (-1 if none). */
int app_sd_drive_index(unsigned drive_index);

/** Set the SD drive index (used by picker when restoring builtins). */
void app_sd_set_drive_index(unsigned drive_index, int disk_index);

/** Return true if the SD filesystem is mounted. */
bool app_sd_is_mounted(void);

/** Cycle the .dsk order override for a drive and remount. */
void app_sd_cycle_dsk_order(apple2_machine_t *machine, unsigned drive_index);

/** Return the human-readable name for a .dsk order override. */
const char *app_sd_dsk_order_override_name(app_sd_dsk_order_override_t override);

/** Return the current .dsk order override for a drive. */
app_sd_dsk_order_override_t app_sd_dsk_order_override(unsigned drive_index);

/** Return a short string describing the disk image type. */
const char *app_sd_disk_image_type_name(app_disk_image_type_t type);

/** Return the image order used by the SD drive file for the given drive. */
apple2_disk2_image_order_t app_sd_drive_image_order(unsigned drive_index);

/** Flush dirty tracks through the disk2 subsystem. */
void app_sd_flush_dirty_tracks(apple2_machine_t *machine);

/**
 * Probe a memory-resident .dsk image to determine its sector order.
 * Returns true if the image appears to be ProDOS-ordered, false for DOS.
 */
bool app_sd_probe_dsk_image_is_prodos(const uint8_t *image, size_t image_size);

/** Performance counters updated by the SD module. */
typedef struct {
    uint64_t dsk_probe_us;
    uint64_t sd_mount_us;
    uint32_t dsk_probes;
    uint32_t sd_mounts;
} app_sd_perf_t;

/** Read (and optionally reset) the SD performance counters. */
app_sd_perf_t app_sd_perf_read(bool reset);

/**
 * Pre-allocate the sector image cache from heap.  Call this BEFORE
 * display/keyboard init to get a contiguous 143 KB block before
 * DMA buffers fragment the heap.
 */
void app_sd_pre_allocate_cache(void);

#endif /* APP_SD_H */
