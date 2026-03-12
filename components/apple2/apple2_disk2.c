#include "apple2/apple2_disk2.h"

#include <string.h>

#define APPLE2_DISK2_TRACKS 35U
#define APPLE2_DISK2_SECTORS 16U
#define APPLE2_DISK2_SECTOR_SIZE 256U
#define APPLE2_DISK2_MAX_QUARTER_TRACK ((APPLE2_DISK2_TRACKS - 1U) * 4U)

static const uint8_t s_disk2_gcr62[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
    0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
    0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
    0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};

static const uint8_t s_dos33_track_order[16] = {
    0x0, 0xD, 0xB, 0x9, 0x7, 0x5, 0x3, 0x1,
    0xE, 0xC, 0xA, 0x8, 0x6, 0x4, 0x2, 0xF,
};

static const uint8_t s_prodos_track_order[16] = {
    0x0, 0x2, 0x4, 0x6, 0x8, 0xA, 0xC, 0xE,
    0x1, 0x3, 0x5, 0x7, 0x9, 0xB, 0xD, 0xF,
};

static inline void disk2_encode44(uint8_t value, uint8_t *out_high, uint8_t *out_low)
{
    *out_high = (uint8_t)(((value >> 1) & 0x55U) | 0xAAU);
    *out_low = (uint8_t)((value & 0x55U) | 0xAAU);
}

static size_t disk2_append_sector(uint8_t *out, uint8_t volume, uint8_t track, uint8_t sector, const uint8_t *data)
{
    uint8_t aux[86];
    uint8_t upper[256];
    uint8_t hi;
    uint8_t lo;
    uint8_t checksum;
    uint8_t last;
    size_t pos = 0;

    memset(aux, 0, sizeof(aux));
    for (uint16_t i = 0; i < 256U; ++i) {
        upper[i] = (uint8_t)(data[i] >> 2);
    }
    /* DOS 3.3 6-and-2 packs the swapped low two bits for bytes i, i+86, i+172
       into the low, middle, and high pairs of a descending 86-byte aux table. */
    for (uint16_t i = 0; i < 86U; ++i) {
        const uint16_t aux_index = 85U - i;

        {
            const uint8_t low2 = (uint8_t)(((data[i] & 0x01U) << 1) | ((data[i] & 0x02U) >> 1));
            aux[aux_index] |= low2;
        }
        if ((i + 86U) < 256U) {
            const uint8_t low2 = (uint8_t)(((data[i + 86U] & 0x01U) << 1) |
                                           ((data[i + 86U] & 0x02U) >> 1));
            aux[aux_index] |= (uint8_t)(low2 << 2);
        }
        if ((i + 172U) < 256U) {
            const uint8_t low2 = (uint8_t)(((data[i + 172U] & 0x01U) << 1) |
                                           ((data[i + 172U] & 0x02U) >> 1));
            aux[aux_index] |= (uint8_t)(low2 << 4);
        }
    }
    for (uint8_t i = 0; i < 16U; ++i) {
        out[pos++] = 0xFFU;
    }

    out[pos++] = 0xD5U;
    out[pos++] = 0xAAU;
    out[pos++] = 0x96U;
    disk2_encode44(volume, &hi, &lo);
    out[pos++] = hi;
    out[pos++] = lo;
    disk2_encode44(track, &hi, &lo);
    out[pos++] = hi;
    out[pos++] = lo;
    disk2_encode44(sector, &hi, &lo);
    out[pos++] = hi;
    out[pos++] = lo;
    checksum = (uint8_t)(volume ^ track ^ sector);
    disk2_encode44(checksum, &hi, &lo);
    out[pos++] = hi;
    out[pos++] = lo;
    out[pos++] = 0xDEU;
    out[pos++] = 0xAAU;
    out[pos++] = 0xEBU;

    for (uint8_t i = 0; i < 8U; ++i) {
        out[pos++] = 0xFFU;
    }

    out[pos++] = 0xD5U;
    out[pos++] = 0xAAU;
    out[pos++] = 0xADU;

    last = 0;
    for (int i = 85; i >= 0; --i) {
        const uint8_t value = (uint8_t)(aux[i] & 0x3FU);
        out[pos++] = s_disk2_gcr62[last ^ value];
        last = value;
    }
    for (uint16_t i = 0; i < 256U; ++i) {
        const uint8_t value = (uint8_t)(upper[i] & 0x3FU);
        out[pos++] = s_disk2_gcr62[last ^ value];
        last = value;
    }
    out[pos++] = s_disk2_gcr62[last];

    out[pos++] = 0xDEU;
    out[pos++] = 0xAAU;
    out[pos++] = 0xEBU;

    for (uint8_t i = 0; i < 16U; ++i) {
        out[pos++] = 0xFFU;
    }

    return pos;
}

static bool disk2_build_track_cache(apple2_disk2_t *disk2)
{
    const uint8_t drive = disk2->active_drive;
    const uint8_t track = (uint8_t)(disk2->quarter_track[drive] / 4U);
    const uint8_t *track_order =
        (disk2->image_order[drive] == APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL)
            ? s_dos33_track_order
            : s_prodos_track_order;
    size_t pos = 0;

    if (drive >= 2U || !disk2->loaded[drive]) {
        return false;
    }
    if (disk2->track_cache_valid &&
        disk2->track_cache_drive == drive &&
        disk2->track_cache_track == track) {
        return true;
    }

    for (uint8_t order_index = 0; order_index < APPLE2_DISK2_SECTORS; ++order_index) {
        const uint8_t physical_sector = track_order[order_index];
        const uint8_t file_sector = order_index;
        const size_t sector_offset =
            ((size_t)track * APPLE2_DISK2_SECTORS + file_sector) * APPLE2_DISK2_SECTOR_SIZE;
        pos += disk2_append_sector(&disk2->track_cache[pos],
                                   0xFEU,
                                   track,
                                   physical_sector,
                                   &disk2->image[drive][sector_offset]);
    }

    disk2->track_cache_valid = true;
    disk2->track_cache_drive = drive;
    disk2->track_cache_track = track;
    disk2->track_cache_length = (uint16_t)pos;
    return true;
}

static void disk2_set_phase(apple2_disk2_t *disk2, uint8_t phase_index, bool enabled)
{
    const uint8_t mask = (uint8_t)(1U << phase_index);
    uint8_t *quarter_track = &disk2->quarter_track[disk2->active_drive];

    if (enabled) {
        if ((disk2->phase_mask & mask) == 0U) {
            if (disk2->last_phase >= 0) {
                const uint8_t delta = (uint8_t)((phase_index - disk2->last_phase) & 0x03U);
                if (delta == 1U && *quarter_track < APPLE2_DISK2_MAX_QUARTER_TRACK) {
                    (*quarter_track)++;
                    disk2->track_cache_valid = false;
                } else if (delta == 3U && *quarter_track > 0U) {
                    (*quarter_track)--;
                    disk2->track_cache_valid = false;
                }
            }
            disk2->last_phase = (int8_t)phase_index;
        }
        disk2->phase_mask |= mask;
    } else {
        disk2->phase_mask &= (uint8_t)~mask;
    }
}

static uint8_t disk2_read_next_byte(apple2_disk2_t *disk2)
{
    if (!disk2->motor_on) {
        disk2->data_latch = 0x00U;
        return disk2->data_latch;
    }
    if (!disk2_build_track_cache(disk2) || disk2->track_cache_length == 0U) {
        disk2->data_latch = 0x00U;
        return disk2->data_latch;
    }

    disk2->data_latch = disk2->track_cache[disk2->nibble_pos[disk2->active_drive] % disk2->track_cache_length];
    disk2->nibble_pos[disk2->active_drive] =
        (disk2->nibble_pos[disk2->active_drive] + 1U) % disk2->track_cache_length;
    return disk2->data_latch;
}

void apple2_disk2_init(apple2_disk2_t *disk2)
{
    memset(disk2, 0, sizeof(*disk2));
    apple2_disk2_reset(disk2);
}

void apple2_disk2_reset(apple2_disk2_t *disk2)
{
    disk2->motor_on = false;
    disk2->active_drive = 0;
    disk2->q6 = false;
    disk2->q7 = false;
    disk2->phase_mask = 0;
    disk2->last_phase = -1;
    disk2->data_latch = 0;
    disk2->track_cache_valid = false;
    memset(disk2->quarter_track, 0, sizeof(disk2->quarter_track));
    memset(disk2->nibble_pos, 0, sizeof(disk2->nibble_pos));
}

bool apple2_disk2_load_drive(apple2_disk2_t *disk2, unsigned drive_index, const uint8_t *image, size_t image_size)
{
    return apple2_disk2_load_drive_with_order(disk2,
                                              drive_index,
                                              image,
                                              image_size,
                                              APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL);
}

bool apple2_disk2_load_drive_with_order(apple2_disk2_t *disk2,
                                        unsigned drive_index,
                                        const uint8_t *image,
                                        size_t image_size,
                                        apple2_disk2_image_order_t image_order)
{
    if (drive_index >= 2U || image_size != APPLE2_DISK2_IMAGE_SIZE) {
        return false;
    }

    disk2->image[drive_index] = image;
    disk2->image_size[drive_index] = image_size;
    disk2->image_order[drive_index] = image_order;
    disk2->loaded[drive_index] = true;
    disk2->track_cache_valid = false;
    return true;
}

bool apple2_disk2_drive_loaded(const apple2_disk2_t *disk2, unsigned drive_index)
{
    return drive_index < 2U && disk2->loaded[drive_index];
}

uint8_t apple2_disk2_access(apple2_disk2_t *disk2, uint8_t reg)
{
    switch (reg & 0x0FU) {
    case 0x0: disk2_set_phase(disk2, 0, false); break;
    case 0x1: disk2_set_phase(disk2, 0, true); break;
    case 0x2: disk2_set_phase(disk2, 1, false); break;
    case 0x3: disk2_set_phase(disk2, 1, true); break;
    case 0x4: disk2_set_phase(disk2, 2, false); break;
    case 0x5: disk2_set_phase(disk2, 2, true); break;
    case 0x6: disk2_set_phase(disk2, 3, false); break;
    case 0x7: disk2_set_phase(disk2, 3, true); break;
    case 0x8: disk2->motor_on = false; break;
    case 0x9: disk2->motor_on = true; break;
    case 0xA: disk2->active_drive = 0; disk2->track_cache_valid = false; break;
    case 0xB: disk2->active_drive = 1; disk2->track_cache_valid = false; break;
    case 0xC:
        disk2->q6 = false;
        if (!disk2->q7) {
            return disk2_read_next_byte(disk2);
        }
        break;
    case 0xD:
        disk2->q6 = true;
        if (!disk2->q7) {
            disk2->data_latch = 0x00U;
        }
        break;
    case 0xE:
        disk2->q7 = false;
        break;
    case 0xF:
        disk2->q7 = true;
        break;
    default:
        break;
    }

    return disk2->data_latch;
}
