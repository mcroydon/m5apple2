#include "apple2/apple2_disk2.h"

#include <string.h>

#define APPLE2_DISK2_TRACKS 35U
#define APPLE2_DISK2_SECTORS 16U
#define APPLE2_DISK2_SECTOR_SIZE 256U
#define APPLE2_DISK2_MAX_QUARTER_TRACK ((APPLE2_DISK2_TRACKS - 1U) * 4U)
#define APPLE2_DISK2_BYTES_PER_SECOND 33280U
#define APPLE2_DISK2_TRACK_SKEW 6U

#define APPLE2_WOZ_HEADER_SIZE 12U
#define APPLE2_WOZ_CHUNK_HEADER_SIZE 8U
#define APPLE2_WOZ_V1_TRACK_SIZE 6656U
#define APPLE2_WOZ_V1_TRACK_BYTES_OFFSET 6646U
#define APPLE2_WOZ_V1_TRACK_BITS_OFFSET 6648U
#define APPLE2_WOZ_V2_TRACK_COUNT APPLE2_DISK2_WOZ_TMAP_ENTRIES
#define APPLE2_WOZ_V2_TRACK_ENTRY_SIZE 8U

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

static const int8_t s_stepper_state_for_mask[16] = {
    -1, 0, 2, 1,
    4, -1, 3, -1,
    6, 7, -1, -1,
    5, -1, -1, -1,
};

static bool disk2_latch_current_byte(apple2_disk2_t *disk2);

static inline void disk2_latch_prepared_byte(apple2_disk2_t *disk2)
{
    disk2->data_latch = disk2->track_cache[disk2->nibble_pos[disk2->active_drive]];
    disk2->data_ready = true;
}

static uint8_t disk2_track_cache_key_for_drive(const apple2_disk2_t *disk2,
                                               uint8_t drive,
                                               uint8_t quarter_track)
{
    switch (disk2->source_kind[drive]) {
    case APPLE2_DISK2_SOURCE_SECTOR_IMAGE:
    case APPLE2_DISK2_SOURCE_SECTOR_READER:
    case APPLE2_DISK2_SOURCE_NIB_IMAGE:
        return (uint8_t)(quarter_track & 0xFCU);
    case APPLE2_DISK2_SOURCE_TRACK_READER:
    case APPLE2_DISK2_SOURCE_NONE:
    default:
        return quarter_track;
    }
}

static inline uint16_t disk2_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}

static inline uint32_t disk2_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static inline uint16_t disk2_bits_to_bytes(uint32_t bit_count)
{
    return (uint16_t)((bit_count + 7U) / 8U);
}

static inline void disk2_encode44(uint8_t value, uint8_t *out_high, uint8_t *out_low)
{
    *out_high = (uint8_t)(((value >> 1) & 0x55U) | 0xAAU);
    *out_low = (uint8_t)((value & 0x55U) | 0xAAU);
}

static uint8_t disk2_find_file_sector_for_physical(const uint8_t *track_order, uint8_t physical_sector)
{
    for (uint8_t file_sector = 0; file_sector < APPLE2_DISK2_SECTORS; ++file_sector) {
        if (track_order[file_sector] == physical_sector) {
            return file_sector;
        }
    }

    return 0U;
}

static bool disk2_boot_track_sequence(const uint8_t *sector0,
                                      const uint8_t *default_track_order,
                                      uint8_t *sequence)
{
    bool seen[APPLE2_DISK2_SECTORS] = { false };
    size_t count = 0;
    const uint8_t initial_index = sector0[0xFFU];

    if (initial_index >= APPLE2_DISK2_SECTORS) {
        return false;
    }

    sequence[count++] = 0U;
    seen[0] = true;

    for (int index = initial_index; index >= 0; --index) {
        const uint8_t physical_sector = sector0[0x4DU + (uint8_t)index];

        if (physical_sector >= APPLE2_DISK2_SECTORS || seen[physical_sector]) {
            continue;
        }
        sequence[count++] = physical_sector;
        seen[physical_sector] = true;
    }

    for (uint8_t order_index = 0; order_index < APPLE2_DISK2_SECTORS; ++order_index) {
        const uint8_t physical_sector = default_track_order[order_index];

        if (seen[physical_sector]) {
            continue;
        }
        sequence[count++] = physical_sector;
        seen[physical_sector] = true;
    }

    return count == APPLE2_DISK2_SECTORS;
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
    for (uint8_t i = 0; i < 29U; ++i) {
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

static void disk2_clear_drive_source(apple2_disk2_t *disk2, unsigned drive_index)
{
    disk2->source_kind[drive_index] = APPLE2_DISK2_SOURCE_NONE;
    disk2->image[drive_index] = NULL;
    disk2->image_size[drive_index] = 0U;
    disk2->image_order[drive_index] = APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
    disk2->read_sector[drive_index] = NULL;
    disk2->read_sector_context[drive_index] = NULL;
    disk2->read_track[drive_index] = NULL;
    disk2->read_track_context[drive_index] = NULL;
}

static bool disk2_set_track_cache(apple2_disk2_t *disk2,
                                  uint8_t drive,
                                  uint8_t quarter_track,
                                  const uint8_t *track_data,
                                  uint16_t track_length)
{
    if (track_data == NULL || track_length == 0U || track_length > APPLE2_DISK2_MAX_TRACK_BYTES) {
        disk2->track_cache_valid = false;
        return false;
    }

    memcpy(disk2->track_cache, track_data, track_length);
    disk2->track_cache_valid = true;
    disk2->track_cache_drive = drive;
    disk2->track_cache_key = disk2_track_cache_key_for_drive(disk2, drive, quarter_track);
    disk2->track_cache_length = track_length;
    return true;
}

static bool disk2_build_sector_track_cache(apple2_disk2_t *disk2, uint8_t drive, uint8_t quarter_track)
{
    const uint8_t track = (uint8_t)(quarter_track / 4U);
    const uint8_t *track_order =
        (disk2->image_order[drive] == APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL)
            ? s_dos33_track_order
            : s_prodos_track_order;
    uint8_t boot_track_order[APPLE2_DISK2_SECTORS];
    uint8_t skewed_track_order[APPLE2_DISK2_SECTORS];
    const uint8_t *physical_sequence = track_order;
    uint8_t sector_data[APPLE2_DISK2_SECTOR_SIZE];
    size_t pos = 0;

    if (track == 0U) {
        const uint8_t *sector0 = NULL;

        if (disk2->source_kind[drive] == APPLE2_DISK2_SOURCE_SECTOR_IMAGE) {
            sector0 = &disk2->image[drive][0];
        } else if (disk2->source_kind[drive] == APPLE2_DISK2_SOURCE_SECTOR_READER &&
                   disk2->read_sector[drive] != NULL &&
                   disk2->read_sector[drive](disk2->read_sector_context[drive],
                                            drive,
                                            0U,
                                            0U,
                                            sector_data)) {
            sector0 = sector_data;
        }

        if (sector0 != NULL && disk2_boot_track_sequence(sector0, track_order, boot_track_order)) {
            physical_sequence = boot_track_order;
        }
    } else {
        const uint8_t track_skew = (uint8_t)((track * APPLE2_DISK2_TRACK_SKEW) % APPLE2_DISK2_SECTORS);

        for (uint8_t i = 0; i < APPLE2_DISK2_SECTORS; ++i) {
            skewed_track_order[i] = track_order[(uint8_t)((i + track_skew) % APPLE2_DISK2_SECTORS)];
        }
        physical_sequence = skewed_track_order;
    }

    for (uint8_t order_index = 0; order_index < APPLE2_DISK2_SECTORS; ++order_index) {
        const uint8_t physical_sector = physical_sequence[order_index];
        const uint8_t file_sector = disk2_find_file_sector_for_physical(track_order, physical_sector);
        const uint8_t *source = NULL;

        if (disk2->source_kind[drive] == APPLE2_DISK2_SOURCE_SECTOR_IMAGE) {
            const size_t sector_offset =
                ((size_t)track * APPLE2_DISK2_SECTORS + file_sector) * APPLE2_DISK2_SECTOR_SIZE;
            source = &disk2->image[drive][sector_offset];
        } else if (disk2->source_kind[drive] == APPLE2_DISK2_SOURCE_SECTOR_READER &&
                   disk2->read_sector[drive] != NULL) {
            if (!disk2->read_sector[drive](disk2->read_sector_context[drive],
                                           drive,
                                           track,
                                           file_sector,
                                           sector_data)) {
                disk2->track_cache_valid = false;
                return false;
            }
            source = sector_data;
        } else {
            disk2->track_cache_valid = false;
            return false;
        }

        pos += disk2_append_sector(&disk2->track_cache[pos],
                                   0xFEU,
                                   track,
                                   physical_sector,
                                   source);
    }

    if (pos < APPLE2_DISK2_NIB_TRACK_BYTES) {
        memset(&disk2->track_cache[pos], 0xFF, APPLE2_DISK2_NIB_TRACK_BYTES - pos);
        pos = APPLE2_DISK2_NIB_TRACK_BYTES;
    }

    disk2->track_cache_valid = true;
    disk2->track_cache_drive = drive;
    disk2->track_cache_key = disk2_track_cache_key_for_drive(disk2, drive, quarter_track);
    disk2->track_cache_length = (uint16_t)pos;
    return true;
}

static bool disk2_build_track_cache(apple2_disk2_t *disk2)
{
    const uint8_t drive = disk2->active_drive;
    const uint8_t quarter_track = disk2->quarter_track[drive];
    const uint8_t cache_key = disk2_track_cache_key_for_drive(disk2, drive, quarter_track);

    if (drive >= 2U || !disk2->loaded[drive]) {
        return false;
    }
    if (disk2->track_cache_valid &&
        disk2->track_cache_drive == drive &&
        disk2->track_cache_key == cache_key) {
        return true;
    }

    switch (disk2->source_kind[drive]) {
    case APPLE2_DISK2_SOURCE_SECTOR_IMAGE:
    case APPLE2_DISK2_SOURCE_SECTOR_READER:
        return disk2_build_sector_track_cache(disk2, drive, quarter_track);

    case APPLE2_DISK2_SOURCE_NIB_IMAGE: {
        const uint8_t track = (uint8_t)(quarter_track / 4U);
        const size_t offset = (size_t)track * APPLE2_DISK2_NIB_TRACK_BYTES;
        return disk2_set_track_cache(disk2,
                                     drive,
                                     quarter_track,
                                     &disk2->image[drive][offset],
                                     APPLE2_DISK2_NIB_TRACK_BYTES);
    }
    case APPLE2_DISK2_SOURCE_TRACK_READER: {
        uint16_t track_length = 0;

        if (disk2->read_track[drive] == NULL ||
            !disk2->read_track[drive](disk2->read_track_context[drive],
                                      drive,
                                      quarter_track,
                                      disk2->track_cache,
                                      &track_length) ||
            track_length == 0U ||
            track_length > APPLE2_DISK2_MAX_TRACK_BYTES) {
            disk2->track_cache_valid = false;
            return false;
        }

        disk2->track_cache_valid = true;
        disk2->track_cache_drive = drive;
        disk2->track_cache_key = cache_key;
        disk2->track_cache_length = track_length;
        return true;
    }

    case APPLE2_DISK2_SOURCE_NONE:
    default:
        disk2->track_cache_valid = false;
        return false;
    }
}

static void disk2_set_phase(apple2_disk2_t *disk2, uint8_t phase_index, bool enabled)
{
    const uint8_t mask = (uint8_t)(1U << phase_index);
    const uint8_t drive = disk2->active_drive;
    uint8_t *phase_mask = &disk2->phase_mask[drive];
    uint8_t *quarter_track = &disk2->quarter_track[drive];
    int8_t *stepper_state = &disk2->stepper_state[drive];
    const int8_t old_state = *stepper_state;
    const uint8_t old_cache_key = disk2_track_cache_key_for_drive(disk2, drive, *quarter_track);

    if (enabled) {
        *phase_mask |= mask;
    } else {
        *phase_mask &= (uint8_t)~mask;
    }

    {
        const int8_t new_state = s_stepper_state_for_mask[*phase_mask & 0x0FU];

        if (new_state >= 0) {
            if (old_state >= 0) {
                const uint8_t delta = (uint8_t)((new_state - old_state) & 0x07U);

                    if (delta != 0U && delta != 4U) {
                        if (delta < 4U) {
                            uint8_t steps = delta;
                            while (steps-- != 0U && *quarter_track < APPLE2_DISK2_MAX_QUARTER_TRACK) {
                                (*quarter_track)++;
                            }
                        } else {
                            uint8_t steps = (uint8_t)(8U - delta);
                            while (steps-- != 0U && *quarter_track > 0U) {
                                (*quarter_track)--;
                            }
                        }

                        if (disk2_track_cache_key_for_drive(disk2, drive, *quarter_track) != old_cache_key) {
                            disk2->track_cache_valid = false;
                        }

                        disk2->stream_accum[drive] = 0U;
                        if (disk2->motor_on) {
                            (void)disk2_latch_current_byte(disk2);
                        } else {
                        disk2->data_ready = false;
                        disk2->data_latch = 0x00U;
                    }
                }
            }

            *stepper_state = new_state;
        } else {
            *stepper_state = -1;
        }
    }
}

static bool disk2_prepare_track(apple2_disk2_t *disk2)
{
    if (!disk2->motor_on) {
        return false;
    }
    if (!disk2_build_track_cache(disk2) || disk2->track_cache_length == 0U) {
        return false;
    }

    {
        uint32_t *nibble_pos = &disk2->nibble_pos[disk2->active_drive];

        if (*nibble_pos >= disk2->track_cache_length) {
            *nibble_pos = 0U;
        }
    }
    return true;
}

static bool disk2_latch_current_byte(apple2_disk2_t *disk2)
{
    if (!disk2_prepare_track(disk2)) {
        disk2->data_latch = 0x00U;
        disk2->data_ready = false;
        return false;
    }

    disk2_latch_prepared_byte(disk2);
    return true;
}

bool apple2_woz_parse(apple2_woz_image_t *woz, const uint8_t *image, size_t image_size)
{
    const bool is_woz1 = image != NULL && image_size >= APPLE2_WOZ_HEADER_SIZE &&
                         memcmp(image, "WOZ1", 4) == 0;
    const bool is_woz2 = image != NULL && image_size >= APPLE2_WOZ_HEADER_SIZE &&
                         memcmp(image, "WOZ2", 4) == 0;
    size_t offset = APPLE2_WOZ_HEADER_SIZE;
    bool have_info = false;
    bool have_tmap = false;
    bool have_trks = false;

    if (woz == NULL) {
        return false;
    }

    memset(woz, 0, sizeof(*woz));
    memset(woz->tmap, 0xFF, sizeof(woz->tmap));

    if (!is_woz1 && !is_woz2) {
        return false;
    }
    if (image[4] != 0xFFU || image[5] != 0x0AU || image[6] != 0x0DU || image[7] != 0x0AU) {
        return false;
    }

    woz->is_woz2 = is_woz2;
    while ((offset + APPLE2_WOZ_CHUNK_HEADER_SIZE) <= image_size) {
        const uint8_t *chunk_header = &image[offset];
        const uint32_t chunk_size = disk2_le32(&chunk_header[4]);
        const size_t chunk_data_offset = offset + APPLE2_WOZ_CHUNK_HEADER_SIZE;
        const uint8_t *chunk_data;

        if (chunk_data_offset > image_size || chunk_size > (image_size - chunk_data_offset)) {
            return false;
        }
        chunk_data = &image[chunk_data_offset];

        if (memcmp(chunk_header, "INFO", 4) == 0) {
            if (chunk_size < 2U) {
                return false;
            }
            have_info = chunk_data[1] == 1U;
        } else if (memcmp(chunk_header, "TMAP", 4) == 0) {
            if (chunk_size < APPLE2_DISK2_WOZ_TMAP_ENTRIES) {
                return false;
            }
            memcpy(woz->tmap, chunk_data, APPLE2_DISK2_WOZ_TMAP_ENTRIES);
            have_tmap = true;
        } else if (memcmp(chunk_header, "TRKS", 4) == 0) {
            if (is_woz1) {
                const size_t track_count = chunk_size / APPLE2_WOZ_V1_TRACK_SIZE;

                if (chunk_size == 0U || (chunk_size % APPLE2_WOZ_V1_TRACK_SIZE) != 0U) {
                    return false;
                }
                for (size_t track_index = 0;
                     track_index < track_count && track_index < APPLE2_DISK2_WOZ_TMAP_ENTRIES;
                     ++track_index) {
                    const size_t track_offset = chunk_data_offset + (track_index * APPLE2_WOZ_V1_TRACK_SIZE);
                    const uint8_t *track_entry = &image[track_offset];
                    uint16_t byte_count = disk2_le16(&track_entry[APPLE2_WOZ_V1_TRACK_BYTES_OFFSET]);
                    const uint32_t bit_count = disk2_le16(&track_entry[APPLE2_WOZ_V1_TRACK_BITS_OFFSET]);

                    if (byte_count == 0U && bit_count != 0U) {
                        byte_count = disk2_bits_to_bytes(bit_count);
                    }
                    if (byte_count == 0U) {
                        continue;
                    }
                    if (byte_count > APPLE2_WOZ_V1_TRACK_BYTES_OFFSET ||
                        byte_count > APPLE2_DISK2_MAX_TRACK_BYTES) {
                        return false;
                    }

                    woz->tracks[track_index].offset = (uint32_t)track_offset;
                    woz->tracks[track_index].byte_count = byte_count;
                }
            } else {
                if (chunk_size < (APPLE2_WOZ_V2_TRACK_COUNT * APPLE2_WOZ_V2_TRACK_ENTRY_SIZE)) {
                    return false;
                }
                for (size_t track_index = 0; track_index < APPLE2_WOZ_V2_TRACK_COUNT; ++track_index) {
                    const uint8_t *track_entry =
                        &chunk_data[track_index * APPLE2_WOZ_V2_TRACK_ENTRY_SIZE];
                    const uint16_t start_block = disk2_le16(track_entry);
                    const uint16_t block_count = disk2_le16(&track_entry[2]);
                    const uint32_t bit_count = disk2_le32(&track_entry[4]);
                    const uint16_t byte_count = disk2_bits_to_bytes(bit_count);
                    const uint32_t track_offset = (uint32_t)start_block * 512U;

                    if (start_block == 0U || block_count == 0U || bit_count == 0U) {
                        continue;
                    }
                    if (byte_count > (uint32_t)block_count * 512U ||
                        byte_count > APPLE2_DISK2_MAX_TRACK_BYTES ||
                        track_offset > image_size ||
                        byte_count > (image_size - track_offset)) {
                        return false;
                    }

                    woz->tracks[track_index].offset = track_offset;
                    woz->tracks[track_index].byte_count = byte_count;
                }
            }
            have_trks = true;
        }

        offset = chunk_data_offset + chunk_size;
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

    woz->valid = true;
    return true;
}

bool apple2_woz_get_track(const apple2_woz_image_t *woz,
                          const uint8_t *image,
                          size_t image_size,
                          uint8_t quarter_track,
                          const uint8_t **track_data,
                          uint16_t *track_length)
{
    uint8_t track_index = 0xFFU;

    if (track_data == NULL || track_length == NULL || woz == NULL || image == NULL || !woz->valid) {
        return false;
    }
    if (quarter_track < APPLE2_DISK2_WOZ_TMAP_ENTRIES) {
        track_index = woz->tmap[quarter_track];
    }
    if (track_index == 0xFFU ||
        track_index >= APPLE2_DISK2_WOZ_TMAP_ENTRIES ||
        woz->tracks[track_index].byte_count == 0U) {
        return false;
    }
    if (woz->tracks[track_index].offset > image_size ||
        woz->tracks[track_index].byte_count > (image_size - woz->tracks[track_index].offset)) {
        return false;
    }

    *track_data = &image[woz->tracks[track_index].offset];
    *track_length = woz->tracks[track_index].byte_count;
    return true;
}

void apple2_disk2_init(apple2_disk2_t *disk2)
{
    memset(disk2, 0, sizeof(*disk2));
    apple2_disk2_reset(disk2);
}

void apple2_disk2_reset(apple2_disk2_t *disk2)
{
    disk2->motor_on = false;
    disk2->data_ready = false;
    disk2->active_drive = 0;
    disk2->q6 = false;
    disk2->q7 = false;
    disk2->data_latch = 0;
    disk2->track_cache_valid = false;
    memset(disk2->phase_mask, 0, sizeof(disk2->phase_mask));
    for (unsigned drive = 0; drive < 2U; ++drive) {
        disk2->stepper_state[drive] = -1;
    }
    memset(disk2->quarter_track, 0, sizeof(disk2->quarter_track));
    memset(disk2->nibble_pos, 0, sizeof(disk2->nibble_pos));
    memset(disk2->stream_accum, 0, sizeof(disk2->stream_accum));
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
    if (drive_index >= 2U || image_size != APPLE2_DISK2_IMAGE_SIZE || image == NULL) {
        return false;
    }

    disk2_clear_drive_source(disk2, drive_index);
    disk2->source_kind[drive_index] = APPLE2_DISK2_SOURCE_SECTOR_IMAGE;
    disk2->image[drive_index] = image;
    disk2->image_size[drive_index] = image_size;
    disk2->image_order[drive_index] = image_order;
    disk2->loaded[drive_index] = true;
    disk2->track_cache_valid = false;
    return true;
}

bool apple2_disk2_load_nib_drive(apple2_disk2_t *disk2,
                                 unsigned drive_index,
                                 const uint8_t *image,
                                 size_t image_size)
{
    if (drive_index >= 2U || image_size != APPLE2_DISK2_NIB_IMAGE_SIZE || image == NULL) {
        return false;
    }

    disk2_clear_drive_source(disk2, drive_index);
    disk2->source_kind[drive_index] = APPLE2_DISK2_SOURCE_NIB_IMAGE;
    disk2->image[drive_index] = image;
    disk2->image_size[drive_index] = image_size;
    disk2->loaded[drive_index] = true;
    disk2->track_cache_valid = false;
    return true;
}

bool apple2_disk2_attach_drive_reader(apple2_disk2_t *disk2,
                                      unsigned drive_index,
                                      apple2_disk2_read_sector_fn read_sector,
                                      void *context,
                                      size_t image_size,
                                      apple2_disk2_image_order_t image_order)
{
    if (drive_index >= 2U || image_size != APPLE2_DISK2_IMAGE_SIZE || read_sector == NULL) {
        return false;
    }

    disk2_clear_drive_source(disk2, drive_index);
    disk2->source_kind[drive_index] = APPLE2_DISK2_SOURCE_SECTOR_READER;
    disk2->image_size[drive_index] = image_size;
    disk2->image_order[drive_index] = image_order;
    disk2->read_sector[drive_index] = read_sector;
    disk2->read_sector_context[drive_index] = context;
    disk2->loaded[drive_index] = true;
    disk2->track_cache_valid = false;
    return true;
}

bool apple2_disk2_attach_drive_track_reader(apple2_disk2_t *disk2,
                                            unsigned drive_index,
                                            apple2_disk2_read_track_fn read_track,
                                            void *context)
{
    if (drive_index >= 2U || read_track == NULL) {
        return false;
    }

    disk2_clear_drive_source(disk2, drive_index);
    disk2->source_kind[drive_index] = APPLE2_DISK2_SOURCE_TRACK_READER;
    disk2->read_track[drive_index] = read_track;
    disk2->read_track_context[drive_index] = context;
    disk2->loaded[drive_index] = true;
    disk2->track_cache_valid = false;
    return true;
}

void apple2_disk2_unload_drive(apple2_disk2_t *disk2, unsigned drive_index)
{
    if (drive_index >= 2U) {
        return;
    }

    disk2->loaded[drive_index] = false;
    disk2_clear_drive_source(disk2, drive_index);
    disk2->quarter_track[drive_index] = 0U;
    disk2->nibble_pos[drive_index] = 0U;
    disk2->stream_accum[drive_index] = 0U;
    disk2->phase_mask[drive_index] = 0U;
    disk2->stepper_state[drive_index] = -1;
    if (disk2->active_drive == drive_index) {
        disk2->track_cache_valid = false;
    }
}

void apple2_disk2_tick(apple2_disk2_t *disk2, uint32_t cpu_hz, uint32_t cycles)
{
    uint8_t drive;
    uint32_t *stream_accum;
    uint32_t accum;
    uint32_t advance_bytes;
    uint32_t *nibble_pos;
    uint16_t track_length;

    if (disk2 == NULL || cycles == 0U || cpu_hz == 0U || !disk2->motor_on) {
        return;
    }
    drive = disk2->active_drive;
    if (!disk2->loaded[drive]) {
        return;
    }

    stream_accum = &disk2->stream_accum[drive];
    accum = *stream_accum + (APPLE2_DISK2_BYTES_PER_SECOND * cycles);
    if (accum < cpu_hz) {
        *stream_accum = accum;
        return;
    }
    *stream_accum = accum;
    advance_bytes = *stream_accum / cpu_hz;
    if (advance_bytes == 0U) {
        return;
    }
    if (!disk2_prepare_track(disk2)) {
        return;
    }
    track_length = disk2->track_cache_length;
    *stream_accum -= advance_bytes * cpu_hz;

    nibble_pos = &disk2->nibble_pos[drive];
    if (advance_bytes >= track_length) {
        advance_bytes %= track_length;
    }
    *nibble_pos += advance_bytes;
    if (*nibble_pos >= track_length) {
        *nibble_pos -= track_length;
    }
    disk2_latch_prepared_byte(disk2);
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
    case 0x8:
        if (disk2->motor_on) {
            disk2->motor_on = false;
            disk2->stream_accum[disk2->active_drive] = 0U;
            disk2->data_ready = false;
            disk2->data_latch = 0x00U;
        }
        break;
    case 0x9:
        if (!disk2->motor_on) {
            disk2->motor_on = true;
            disk2->stream_accum[disk2->active_drive] = 0U;
            (void)disk2_latch_current_byte(disk2);
        }
        break;
    case 0xA:
        if (disk2->active_drive != 0U) {
            disk2->active_drive = 0;
            disk2->track_cache_valid = false;
            disk2->stream_accum[disk2->active_drive] = 0U;
            (void)disk2_latch_current_byte(disk2);
        }
        break;
    case 0xB:
        if (disk2->active_drive != 1U) {
            disk2->active_drive = 1;
            disk2->track_cache_valid = false;
            disk2->stream_accum[disk2->active_drive] = 0U;
            (void)disk2_latch_current_byte(disk2);
        }
        break;
    case 0xC:
        disk2->q6 = false;
        if (!disk2->q7) {
            if (!disk2->data_ready && !disk2->track_cache_valid) {
                (void)disk2_latch_current_byte(disk2);
            }
            const uint8_t value = disk2->data_latch;

            if (disk2->data_ready) {
                disk2->data_ready = false;
                disk2->data_latch &= 0x7FU;
            }
            return value;
        }
        break;
    case 0xD:
        disk2->q6 = true;
        if (!disk2->q7) {
            disk2->data_ready = false;
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
