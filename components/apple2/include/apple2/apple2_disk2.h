#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLE2_DISK2_IMAGE_SIZE (35U * 16U * 256U)
#define APPLE2_DISK2_MAX_TRACK_BYTES 6656U

typedef enum {
    APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL = 0,
    APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL = 1,
} apple2_disk2_image_order_t;

typedef struct {
    bool loaded[2];
    const uint8_t *image[2];
    size_t image_size[2];
    apple2_disk2_image_order_t image_order[2];
    bool motor_on;
    uint8_t active_drive;
    bool q6;
    bool q7;
    uint8_t phase_mask;
    int8_t last_phase;
    uint8_t quarter_track[2];
    uint32_t nibble_pos[2];
    uint8_t data_latch;
    bool track_cache_valid;
    uint8_t track_cache_drive;
    uint8_t track_cache_track;
    uint16_t track_cache_length;
    uint8_t track_cache[APPLE2_DISK2_MAX_TRACK_BYTES];
} apple2_disk2_t;

void apple2_disk2_init(apple2_disk2_t *disk2);
void apple2_disk2_reset(apple2_disk2_t *disk2);
bool apple2_disk2_load_drive(apple2_disk2_t *disk2, unsigned drive_index, const uint8_t *image, size_t image_size);
bool apple2_disk2_load_drive_with_order(apple2_disk2_t *disk2,
                                        unsigned drive_index,
                                        const uint8_t *image,
                                        size_t image_size,
                                        apple2_disk2_image_order_t image_order);
bool apple2_disk2_drive_loaded(const apple2_disk2_t *disk2, unsigned drive_index);
uint8_t apple2_disk2_access(apple2_disk2_t *disk2, uint8_t reg);
