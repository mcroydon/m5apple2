#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLE2_DISK2_IMAGE_SIZE (35U * 16U * 256U)
#define APPLE2_DISK2_NIB_TRACK_BYTES 6656U
#define APPLE2_DISK2_NIB_IMAGE_SIZE (35U * APPLE2_DISK2_NIB_TRACK_BYTES)
#define APPLE2_DISK2_MAX_TRACK_BYTES 8192U
#define APPLE2_DISK2_WOZ_TMAP_ENTRIES 160U

typedef enum {
    APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL = 0,
    APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL = 1,
} apple2_disk2_image_order_t;

typedef enum {
    APPLE2_DISK2_SOURCE_NONE = 0,
    APPLE2_DISK2_SOURCE_SECTOR_IMAGE,
    APPLE2_DISK2_SOURCE_SECTOR_READER,
    APPLE2_DISK2_SOURCE_NIB_IMAGE,
    APPLE2_DISK2_SOURCE_TRACK_READER,
} apple2_disk2_source_kind_t;

typedef bool (*apple2_disk2_read_sector_fn)(void *context,
                                            unsigned drive_index,
                                            uint8_t track,
                                            uint8_t file_sector,
                                            uint8_t *sector_data);

typedef bool (*apple2_disk2_read_track_fn)(void *context,
                                           unsigned drive_index,
                                           uint8_t quarter_track,
                                           uint8_t *track_data,
                                           uint16_t *track_length);

typedef struct {
    uint32_t offset;
    uint16_t byte_count;
} apple2_woz_track_t;

typedef struct {
    bool valid;
    bool is_woz2;
    uint8_t tmap[APPLE2_DISK2_WOZ_TMAP_ENTRIES];
    apple2_woz_track_t tracks[APPLE2_DISK2_WOZ_TMAP_ENTRIES];
} apple2_woz_image_t;

typedef struct {
    bool loaded[2];
    apple2_disk2_source_kind_t source_kind[2];
    const uint8_t *image[2];
    size_t image_size[2];
    apple2_disk2_image_order_t image_order[2];
    apple2_disk2_read_sector_fn read_sector[2];
    void *read_sector_context[2];
    apple2_disk2_read_track_fn read_track[2];
    void *read_track_context[2];
    bool motor_on;
    bool data_ready;
    uint8_t active_drive;
    bool q6;
    bool q7;
    uint8_t phase_mask[2];
    int8_t stepper_state[2];
    uint8_t quarter_track[2];
    uint32_t nibble_pos[2];
    uint32_t stream_accum[2];
    uint8_t data_latch;
    bool track_cache_valid;
    uint8_t track_cache_drive;
    uint8_t track_cache_quarter_track;
    uint16_t track_cache_length;
    uint8_t track_cache[APPLE2_DISK2_MAX_TRACK_BYTES];
} apple2_disk2_t;

bool apple2_woz_parse(apple2_woz_image_t *woz, const uint8_t *image, size_t image_size);
bool apple2_woz_get_track(const apple2_woz_image_t *woz,
                          const uint8_t *image,
                          size_t image_size,
                          uint8_t quarter_track,
                          const uint8_t **track_data,
                          uint16_t *track_length);

void apple2_disk2_init(apple2_disk2_t *disk2);
void apple2_disk2_reset(apple2_disk2_t *disk2);
bool apple2_disk2_load_drive(apple2_disk2_t *disk2, unsigned drive_index, const uint8_t *image, size_t image_size);
bool apple2_disk2_load_drive_with_order(apple2_disk2_t *disk2,
                                        unsigned drive_index,
                                        const uint8_t *image,
                                        size_t image_size,
                                        apple2_disk2_image_order_t image_order);
bool apple2_disk2_load_nib_drive(apple2_disk2_t *disk2,
                                 unsigned drive_index,
                                 const uint8_t *image,
                                 size_t image_size);
bool apple2_disk2_attach_drive_reader(apple2_disk2_t *disk2,
                                      unsigned drive_index,
                                      apple2_disk2_read_sector_fn read_sector,
                                      void *context,
                                      size_t image_size,
                                      apple2_disk2_image_order_t image_order);
bool apple2_disk2_attach_drive_track_reader(apple2_disk2_t *disk2,
                                            unsigned drive_index,
                                            apple2_disk2_read_track_fn read_track,
                                            void *context);
void apple2_disk2_unload_drive(apple2_disk2_t *disk2, unsigned drive_index);
bool apple2_disk2_drive_loaded(const apple2_disk2_t *disk2, unsigned drive_index);
void apple2_disk2_tick(apple2_disk2_t *disk2, uint32_t cpu_hz, uint32_t cycles);
uint8_t apple2_disk2_access(apple2_disk2_t *disk2, uint8_t reg);
