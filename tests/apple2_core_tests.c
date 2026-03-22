#include "apple2/apple2_machine.h"
#include "apple2/apple2_video.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void test_put_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)(value >> 8);
}

static void test_put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
    out[2] = (uint8_t)((value >> 16) & 0xFFU);
    out[3] = (uint8_t)(value >> 24);
}

static uint8_t test_decode44(uint8_t high, uint8_t low)
{
    return (uint8_t)(((high & 0x55U) << 1) | (low & 0x55U));
}

static size_t test_collect_track_sector_headers(const apple2_disk2_t *disk2,
                                                uint8_t *sectors,
                                                size_t max_sectors)
{
    size_t count = 0;

    for (uint16_t i = 0; (uint16_t)(i + 13U) <= disk2->track_cache_length; ++i) {
        if (disk2->track_cache[i] == 0xD5U &&
            disk2->track_cache[i + 1U] == 0xAAU &&
            disk2->track_cache[i + 2U] == 0x96U) {
            if (count < max_sectors) {
                sectors[count] = test_decode44(disk2->track_cache[i + 7U], disk2->track_cache[i + 8U]);
            }
            count++;
            i = (uint16_t)(i + 12U);
        }
    }

    return count;
}

static void fill_reset_vector(apple2_machine_t *machine, uint16_t address)
{
    machine->memory[0xFFFC] = (uint8_t)(address & 0xFFU);
    machine->memory[0xFFFD] = (uint8_t)(address >> 8);
}

static void test_cpu_program(void)
{
    apple2_machine_t machine;
    const uint8_t program[] = {
        0xA9, 0x01,       /* LDA #$01 */
        0x69, 0x02,       /* ADC #$02 */
        0x8D, 0x00, 0x04, /* STA $0400 */
        0xA2, 0x03,       /* LDX #$03 */
        0xCA,             /* DEX */
        0xD0, 0xFD,       /* BNE $0209 */
        0xEA,             /* NOP */
    };

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    memcpy(&machine.memory[0x0200], program, sizeof(program));
    fill_reset_vector(&machine, 0x0200);
    apple2_machine_reset(&machine);

    for (int i = 0; i < 11; ++i) {
        apple2_machine_step_instruction(&machine);
    }

    const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
    assert(machine.memory[0x0400] == 0x03);
    assert(cpu.x == 0x00);
    assert(cpu.a == 0x03);
    assert(cpu.pc == 0x020D);
}

static void test_decimal_adc(void)
{
    apple2_machine_t machine;
    const uint8_t program[] = {
        0xF8,       /* SED */
        0xA9, 0x15, /* LDA #$15 */
        0x69, 0x27, /* ADC #$27 */
        0xD8,       /* CLD */
        0xEA,       /* NOP */
    };

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    memcpy(&machine.memory[0x0300], program, sizeof(program));
    fill_reset_vector(&machine, 0x0300);
    apple2_machine_reset(&machine);

    for (int i = 0; i < 5; ++i) {
        apple2_machine_step_instruction(&machine);
    }

    const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
    assert(cpu.a == 0x42);
    assert((cpu.p & CPU6502_FLAG_DECIMAL) == 0U);
}

static void test_undocumented_nops(void)
{
    apple2_machine_t machine;
    const uint8_t program[] = {
        0xA2, 0x01,       /* LDX #$01 */
        0x04, 0x10,       /* NOP $10 */
        0x3C, 0xFF, 0x03, /* NOP $03FF,X -> $0400 with page cross */
        0xEA,             /* NOP */
    };
    uint32_t cycles = 0;

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    memcpy(&machine.memory[0x0200], program, sizeof(program));
    machine.memory[0x0010] = 0xAAU;
    machine.memory[0x0400] = 0x55U;
    fill_reset_vector(&machine, 0x0200);
    apple2_machine_reset(&machine);

    cycles = apple2_machine_step_instruction(&machine);
    assert(cycles == 2U);
    assert(apple2_machine_cpu_state(&machine).pc == 0x0202U);

    cycles = apple2_machine_step_instruction(&machine);
    assert(cycles == 3U);
    assert(apple2_machine_cpu_state(&machine).pc == 0x0204U);

    cycles = apple2_machine_step_instruction(&machine);
    assert(cycles == 5U);
    assert(apple2_machine_cpu_state(&machine).pc == 0x0207U);

    cycles = apple2_machine_step_instruction(&machine);
    assert(cycles == 2U);
    assert(apple2_machine_cpu_state(&machine).pc == 0x0208U);
}

static void test_soft_switches(void)
{
    apple2_machine_t machine;

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });

    apple2_machine_poke(&machine, 0xC050, 0);
    apple2_machine_poke(&machine, 0xC053, 0);
    apple2_machine_poke(&machine, 0xC055, 0);
    apple2_machine_poke(&machine, 0xC057, 0);

    assert(!machine.video.text_mode);
    assert(machine.video.mixed_mode);
    assert(machine.video.page2);
    assert(machine.video.hires_mode);

    apple2_machine_poke(&machine, 0xC059, 0);
    apple2_machine_poke(&machine, 0xC05D, 0);
    assert(machine.annunciator_state == 0x05U);
    apple2_machine_poke(&machine, 0xC058, 0);
    apple2_machine_poke(&machine, 0xC05C, 0);
    assert(machine.annunciator_state == 0x00U);

    apple2_machine_set_key(&machine, 'a');
    assert(machine.key_latch == 0xC1);
    apple2_machine_poke(&machine, 0xC010, 0);
    assert((machine.key_latch & 0x80U) == 0U);
}

static void test_video_addresses(void)
{
    assert(apple2_text_row_address(false, 0) == 0x0400);
    assert(apple2_text_row_address(false, 8) == 0x0428);
    assert(apple2_text_row_address(true, 23) == 0x0BD0);

    assert(apple2_hires_line_address(false, 0) == 0x2000);
    assert(apple2_hires_line_address(false, 8) == 0x2080);
    assert(apple2_hires_line_address(true, 64) == 0x4028);
}

static void test_text_render(void)
{
    apple2_machine_t machine;
    uint8_t pixels[APPLE2_VIDEO_WIDTH * APPLE2_VIDEO_HEIGHT];

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    machine.memory[0x0400] = 0xC1; /* 'A' */
    apple2_machine_render(&machine, pixels);

    assert(pixels[2] == APPLE2_COLOR_WHITE);
    assert(pixels[0] == APPLE2_COLOR_BLACK);
    assert(pixels[APPLE2_VIDEO_WIDTH * 3U + 3U] == APPLE2_COLOR_WHITE);
}

static void test_text_code_mapping(void)
{
    bool inverse = false;

    assert(apple2_text_code_to_ascii(0x01U, &inverse) == 'A');
    assert(inverse);

    assert(apple2_text_code_to_ascii(0x31U, &inverse) == '1');
    assert(inverse);

    assert(apple2_text_code_to_ascii(0x71U, &inverse) == '1');
    assert(!inverse);

    assert(apple2_text_code_to_ascii(0xA0U, &inverse) == ' ');
    assert(!inverse);

    assert(apple2_text_code_to_ascii(0xC1U, &inverse) == 'A');
    assert(!inverse);

    assert(apple2_text_code_is_flashing(0x60U));
    assert(apple2_text_code_is_flashing(0x7FU));
    assert(!apple2_text_code_is_flashing(0xA0U));
    assert(apple2_text_code_is_flash_space(0x60U));
    assert(!apple2_text_code_is_flash_space(0x61U));
}

static void test_flash_cursor_render(void)
{
    apple2_machine_t machine;
    uint8_t pixels[APPLE2_VIDEO_WIDTH * APPLE2_VIDEO_HEIGHT];

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    machine.memory[0x0400] = 0x60U; /* Flashing space: the Monitor/BASIC cursor cell. */

    machine.video.flash_state = false;
    apple2_machine_render(&machine, pixels);
    assert(pixels[0] == APPLE2_COLOR_BLACK);

    machine.video.flash_state = true;
    apple2_machine_render(&machine, pixels);
    assert(pixels[0] == APPLE2_COLOR_WHITE);
}

static void test_flash_timing(void)
{
    apple2_machine_t machine;
    uint32_t half_period_cycles;

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    memset(&machine.memory[0x0200], 0xEA, 4096U);
    fill_reset_vector(&machine, 0x0200);
    apple2_machine_reset(&machine);

    half_period_cycles = machine.flash_half_period_cycles;
    assert(!machine.video.flash_state);

    apple2_machine_step(&machine, half_period_cycles);
    assert(machine.video.flash_state);

    apple2_machine_step(&machine, half_period_cycles);
    assert(!machine.video.flash_state);
}

static void test_full_apple2plus_rom_layout(void)
{
    apple2_machine_t machine;
    uint8_t rom[0x5000];
    const uint8_t program[] = {
        0xAD, 0x00, 0xC8,       /* LDA $C800 */
        0x8D, 0x00, 0x03,       /* STA $0300 */
        0xAD, 0xFF, 0xCF,       /* LDA $CFFF */
        0x8D, 0x01, 0x03,       /* STA $0301 */
        0xEA,                   /* NOP */
    };

    memset(rom, 0, sizeof(rom));
    rom[0x1000] = 0x4C; /* C000 content should map for full images. */
    rom[0x1600] = 0xA9; /* C600 content should map too. */
    rom[0x1800] = 0x5A; /* C800 motherboard ROM should stay visible. */
    rom[0x1FFF] = 0xA5; /* CFFF should read underlying ROM and reset slot C8 select. */
    rom[0x4FFC] = 0x62; /* RESET -> $FA62 */
    rom[0x4FFD] = 0xFA;

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    assert(apple2_machine_load_system_rom(&machine, rom, sizeof(rom)));

    assert(machine.system_rom_loaded);
    assert(machine.slot6_rom_loaded);
    assert(machine.memory[0xC000] == 0x4C);
    assert(machine.memory[0xC600] == 0xA9);
    assert(machine.memory[0xFFFC] == 0x62);
    assert(machine.memory[0xFFFD] == 0xFA);
    assert(apple2_machine_cpu_state(&machine).pc == 0xFA62);

    apple2_machine_poke(&machine, 0xC600, 0);
    assert(machine.c8_slot == 0x06U);
    apple2_machine_poke(&machine, 0xCFFF, 0);
    assert(machine.c8_slot == 0x00U);

    apple2_machine_poke(&machine, 0xC800, 0x11);
    assert(machine.memory[0xC800] == 0x5A);

    apple2_machine_poke(&machine, 0xB700, 0x77);
    assert(machine.memory[0xB700] == 0x77);

    memcpy(&machine.memory[0x0200], program, sizeof(program));
    fill_reset_vector(&machine, 0x0200);
    apple2_machine_reset(&machine);
    for (int i = 0; i < 5; ++i) {
        apple2_machine_step_instruction(&machine);
    }

    assert(machine.memory[0x0300] == 0x5A);
    assert(machine.memory[0x0301] == 0xA5);
    assert(machine.c8_slot == 0x00U);
}

static void test_separate_slot6_rom_layout(void)
{
    apple2_machine_t machine;
    uint8_t system_rom[0x3000];
    uint8_t slot6_rom[0x0100];

    memset(system_rom, 0, sizeof(system_rom));
    memset(slot6_rom, 0, sizeof(slot6_rom));
    system_rom[0x2FFC] = 0x62; /* RESET -> $FA62 */
    system_rom[0x2FFD] = 0xFA;
    slot6_rom[0x00] = 0xA2;
    slot6_rom[0xFF] = 0x60;

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    assert(apple2_machine_load_system_rom(&machine, system_rom, sizeof(system_rom)));
    assert(!machine.slot6_rom_loaded);
    assert(machine.memory[0xC600] == 0x00);

    assert(apple2_machine_load_slot6_rom(&machine, slot6_rom, sizeof(slot6_rom)));
    assert(machine.slot6_rom_loaded);
    assert(machine.memory[0xC600] == 0xA2);
    assert(machine.memory[0xC6FF] == 0x60);
    assert(apple2_machine_cpu_state(&machine).pc == 0xFA62);
}

static void test_language_card_soft_switches(void)
{
    apple2_machine_t machine;
    uint8_t rom[APPLE2_LANGCARD_ROM_SIZE];
    const uint8_t program[] = {
        0xAD, 0x81, 0xC0, /* LDA $C081 */
        0xAD, 0x81, 0xC0, /* LDA $C081 */
        0xA9, 0x11,       /* LDA #$11 */
        0x8D, 0x00, 0xD0, /* STA $D000 */
        0xAD, 0x83, 0xC0, /* LDA $C083 */
        0xAD, 0x00, 0xD0, /* LDA $D000 */
        0x8D, 0x00, 0x04, /* STA $0400 */
        0xAD, 0x89, 0xC0, /* LDA $C089 */
        0xAD, 0x89, 0xC0, /* LDA $C089 */
        0xA9, 0x22,       /* LDA #$22 */
        0x8D, 0x00, 0xD0, /* STA $D000 */
        0xAD, 0x8B, 0xC0, /* LDA $C08B */
        0xAD, 0x00, 0xD0, /* LDA $D000 */
        0x8D, 0x01, 0x04, /* STA $0401 */
        0xAD, 0x83, 0xC0, /* LDA $C083 */
        0xAD, 0x00, 0xD0, /* LDA $D000 */
        0x8D, 0x02, 0x04, /* STA $0402 */
        0xEA,             /* NOP */
    };

    memset(rom, 0x00, sizeof(rom));
    rom[0x2FFCU] = 0x00U;
    rom[0x2FFDU] = 0x02U;

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    assert(apple2_machine_load_system_rom(&machine, rom, sizeof(rom)));

    memcpy(&machine.memory[0x0200], program, sizeof(program));
    machine.memory[0xD000] = 0xA5U;
    apple2_machine_reset(&machine);

    for (int i = 0; i < 32; ++i) {
        apple2_machine_step_instruction(&machine);
        if (apple2_machine_cpu_state(&machine).pc == 0x0232U) {
            break;
        }
    }

    assert(machine.memory[0x0400] == 0x11U);
    assert(machine.memory[0x0401] == 0x22U);
    assert(machine.memory[0x0402] == 0x11U);
}

static void test_drive0_dsk_loading(void)
{
    apple2_machine_t machine;
    uint8_t image[APPLE2_DISK2_IMAGE_SIZE];

    memset(image, 0xA5, sizeof(image));
    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });

    assert(apple2_machine_load_drive0_dsk(&machine, image, sizeof(image)));
    assert(apple2_disk2_drive_loaded(&machine.disk2, 0));
    assert(machine.disk2.image_order[0] == APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL);
    assert(apple2_machine_load_drive0_do(&machine, image, sizeof(image)));
    assert(machine.disk2.image_order[0] == APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL);
    assert(apple2_machine_load_drive0_po(&machine, image, sizeof(image)));
    assert(machine.disk2.image_order[0] == APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL);
    assert(!apple2_machine_load_drive0_dsk(&machine, image, sizeof(image) - 1U));
}

static void test_drive0_nib_loading(void)
{
    apple2_machine_t machine;
    uint8_t image[APPLE2_DISK2_NIB_IMAGE_SIZE];
    uint8_t first_byte = 0;

    memset(image, 0x00, sizeof(image));
    for (uint8_t track = 0; track < 35U; ++track) {
        memset(&image[(size_t)track * APPLE2_DISK2_NIB_TRACK_BYTES],
               (int)(0x80U | track),
               APPLE2_DISK2_NIB_TRACK_BYTES);
    }

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    assert(apple2_machine_load_drive0_nib(&machine, image, sizeof(image)));
    assert(machine.disk2.source_kind[0] == APPLE2_DISK2_SOURCE_NIB_IMAGE);

    (void)apple2_disk2_access(&machine.disk2, 0x9U);
    first_byte = apple2_disk2_access(&machine.disk2, 0xCU);
    assert(first_byte == 0x80U);
    assert(machine.disk2.track_cache_length == APPLE2_DISK2_NIB_TRACK_BYTES);

    machine.disk2.quarter_track[0] = 4U;
    machine.disk2.nibble_pos[0] = 0U;
    machine.disk2.track_cache_valid = false;
    first_byte = apple2_disk2_access(&machine.disk2, 0xCU);
    assert(first_byte == 0x81U);
}

static bool test_disk_reader(void *context,
                             unsigned drive_index,
                             uint8_t track,
                             uint8_t file_sector,
                             uint8_t *sector_data)
{
    const uint8_t *image = context;
    const size_t offset = (((size_t)track * 16U) + file_sector) * 256U;

    (void)drive_index;
    memcpy(sector_data, &image[offset], 256U);
    return true;
}

static bool test_track_reader(void *context,
                              unsigned drive_index,
                              uint8_t quarter_track,
                              uint8_t *track_data,
                              uint16_t *track_length)
{
    const uint8_t value = (uint8_t)(uintptr_t)context;

    (void)drive_index;
    for (uint16_t i = 0; i < 32U; ++i) {
        track_data[i] = (uint8_t)(value + quarter_track + i);
    }
    *track_length = 32U;
    return true;
}

typedef struct {
    const uint8_t *image;
    size_t image_size;
    apple2_woz_image_t woz;
} test_woz_reader_t;

static bool test_woz_track_reader(void *context,
                                  unsigned drive_index,
                                  uint8_t quarter_track,
                                  uint8_t *track_data,
                                  uint16_t *track_length)
{
    const test_woz_reader_t *reader = context;
    const uint8_t *source = NULL;

    (void)drive_index;
    if (!apple2_woz_get_track(&reader->woz,
                              reader->image,
                              reader->image_size,
                              quarter_track,
                              &source,
                              track_length)) {
        return false;
    }

    memcpy(track_data, source, *track_length);
    return true;
}

static void test_drive1_and_reader_loading(void)
{
    apple2_machine_t machine;
    apple2_disk2_t disk2;
    uint8_t image[APPLE2_DISK2_IMAGE_SIZE];
    uint8_t first_byte = 0;

    memset(image, 0x00, sizeof(image));
    for (size_t i = 0; i < sizeof(image); ++i) {
        image[i] = (uint8_t)i;
    }

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    assert(apple2_machine_load_drive1_dsk(&machine, image, sizeof(image)));
    assert(apple2_disk2_drive_loaded(&machine.disk2, 1));
    assert(machine.disk2.image_order[1] == APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL);
    assert(apple2_machine_load_drive1_po(&machine, image, sizeof(image)));
    assert(machine.disk2.image_order[1] == APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL);

    apple2_disk2_init(&disk2);
    assert(apple2_disk2_attach_drive_reader(&disk2,
                                            1,
                                            test_disk_reader,
                                            image,
                                            sizeof(image),
                                            APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL));
    disk2.active_drive = 1U;
    (void)apple2_disk2_access(&disk2, 0x9U);
    first_byte = apple2_disk2_access(&disk2, 0xCU);

    assert(disk2.track_cache_valid);
    assert(disk2.track_cache_length == APPLE2_DISK2_NIB_TRACK_BYTES);
    assert(first_byte == disk2.track_cache[0]);

    apple2_disk2_unload_drive(&disk2, 1);
    assert(!apple2_disk2_drive_loaded(&disk2, 1));
}

static void test_track_reader_loading(void)
{
    apple2_disk2_t disk2;
    uint8_t first_byte = 0;

    apple2_disk2_init(&disk2);
    assert(apple2_disk2_attach_drive_track_reader(&disk2,
                                                  1,
                                                  test_track_reader,
                                                  (void *)(uintptr_t)0x20U));
    assert(disk2.source_kind[1] == APPLE2_DISK2_SOURCE_TRACK_READER);
    disk2.active_drive = 1U;

    (void)apple2_disk2_access(&disk2, 0x9U);
    first_byte = apple2_disk2_access(&disk2, 0xCU);
    assert(first_byte == 0x20U);
    assert(disk2.track_cache_length == 32U);

    disk2.quarter_track[1] = 2U;
    disk2.nibble_pos[1] = 0U;
    disk2.track_cache_valid = false;
    first_byte = apple2_disk2_access(&disk2, 0xCU);
    assert(first_byte == 0x22U);
}

static size_t test_build_woz1(uint8_t *image, size_t capacity, uint8_t track0_value, uint8_t track1_value)
{
    const size_t info_offset = 12U;
    const size_t info_size = 60U;
    const size_t tmap_offset = info_offset + 8U + info_size;
    const size_t trks_offset = tmap_offset + 8U + APPLE2_DISK2_WOZ_TMAP_ENTRIES;
    const size_t total_size = trks_offset + 8U + (2U * 6656U);

    assert(capacity >= total_size);
    memset(image, 0x00, capacity);
    memcpy(image, "WOZ1", 4);
    image[4] = 0xFFU;
    image[5] = 0x0AU;
    image[6] = 0x0DU;
    image[7] = 0x0AU;

    memcpy(&image[info_offset], "INFO", 4);
    test_put_le32(&image[info_offset + 4U], (uint32_t)info_size);
    image[info_offset + 8U] = 1U;
    image[info_offset + 9U] = 1U;

    memcpy(&image[tmap_offset], "TMAP", 4);
    test_put_le32(&image[tmap_offset + 4U], APPLE2_DISK2_WOZ_TMAP_ENTRIES);
    memset(&image[tmap_offset + 8U], 0xFF, APPLE2_DISK2_WOZ_TMAP_ENTRIES);
    image[tmap_offset + 8U] = 0U;
    image[tmap_offset + 9U] = 0U;
    image[tmap_offset + 10U] = 0U;
    image[tmap_offset + 11U] = 0U;
    image[tmap_offset + 12U] = 1U;
    image[tmap_offset + 13U] = 1U;
    image[tmap_offset + 14U] = 1U;
    image[tmap_offset + 15U] = 1U;

    memcpy(&image[trks_offset], "TRKS", 4);
    test_put_le32(&image[trks_offset + 4U], 2U * 6656U);
    memset(&image[trks_offset + 8U], track0_value, 6646U);
    test_put_le16(&image[trks_offset + 8U + 6646U], 6646U);
    test_put_le16(&image[trks_offset + 8U + 6648U], (uint16_t)(6646U * 8U));
    memset(&image[trks_offset + 8U + 6656U], track1_value, 6646U);
    test_put_le16(&image[trks_offset + 8U + 6656U + 6646U], 6646U);
    test_put_le16(&image[trks_offset + 8U + 6656U + 6648U], (uint16_t)(6646U * 8U));
    return total_size;
}

static size_t test_build_woz2(uint8_t *image, size_t capacity, uint8_t track0_value, uint8_t track1_value)
{
    const size_t info_offset = 12U;
    const size_t info_size = 60U;
    const size_t tmap_offset = info_offset + 8U + info_size;
    const size_t trks_offset = tmap_offset + 8U + APPLE2_DISK2_WOZ_TMAP_ENTRIES;
    const size_t data_offset = 1536U;
    const size_t total_size = data_offset + 1024U;

    assert(capacity >= total_size);
    memset(image, 0x00, capacity);
    memcpy(image, "WOZ2", 4);
    image[4] = 0xFFU;
    image[5] = 0x0AU;
    image[6] = 0x0DU;
    image[7] = 0x0AU;

    memcpy(&image[info_offset], "INFO", 4);
    test_put_le32(&image[info_offset + 4U], (uint32_t)info_size);
    image[info_offset + 8U] = 2U;
    image[info_offset + 9U] = 1U;

    memcpy(&image[tmap_offset], "TMAP", 4);
    test_put_le32(&image[tmap_offset + 4U], APPLE2_DISK2_WOZ_TMAP_ENTRIES);
    memset(&image[tmap_offset + 8U], 0xFF, APPLE2_DISK2_WOZ_TMAP_ENTRIES);
    image[tmap_offset + 8U] = 0U;
    image[tmap_offset + 9U] = 0U;
    image[tmap_offset + 10U] = 0U;
    image[tmap_offset + 11U] = 0U;
    image[tmap_offset + 12U] = 1U;
    image[tmap_offset + 13U] = 1U;
    image[tmap_offset + 14U] = 1U;
    image[tmap_offset + 15U] = 1U;

    memcpy(&image[trks_offset], "TRKS", 4);
    test_put_le32(&image[trks_offset + 4U], 160U * 8U);
    test_put_le16(&image[trks_offset + 8U], 3U);
    test_put_le16(&image[trks_offset + 10U], 1U);
    test_put_le32(&image[trks_offset + 12U], 32U);
    test_put_le16(&image[trks_offset + 16U], 4U);
    test_put_le16(&image[trks_offset + 18U], 1U);
    test_put_le32(&image[trks_offset + 20U], 32U);

    memset(&image[data_offset], track0_value, 4U);
    memset(&image[data_offset + 512U], track1_value, 4U);
    return total_size;
}

static void test_woz_loading(void)
{
    apple2_disk2_t disk2;
    apple2_woz_image_t woz;
    test_woz_reader_t reader;
    uint8_t woz1[12U + 8U + 60U + 8U + APPLE2_DISK2_WOZ_TMAP_ENTRIES + 8U + (2U * 6656U)];
    uint8_t woz2[2560U];
    size_t woz1_size;
    size_t woz2_size;
    const uint8_t *track_data = NULL;
    uint16_t track_length = 0;

    woz1_size = test_build_woz1(woz1, sizeof(woz1), 0xD5U, 0xA5U);
    assert(apple2_woz_parse(&woz, woz1, woz1_size));
    assert(!woz.is_woz2);
    assert(apple2_woz_get_track(&woz, woz1, woz1_size, 0U, &track_data, &track_length));
    assert(track_length == 6646U);
    assert(track_data[0] == 0xD5U);

    woz2_size = test_build_woz2(woz2, sizeof(woz2), 0x96U, 0x97U);
    assert(apple2_woz_parse(&reader.woz, woz2, woz2_size));
    reader.image = woz2;
    reader.image_size = woz2_size;

    apple2_disk2_init(&disk2);
    assert(apple2_disk2_attach_drive_track_reader(&disk2, 0, test_woz_track_reader, &reader));

    (void)apple2_disk2_access(&disk2, 0x9U);
    assert(apple2_disk2_access(&disk2, 0xCU) == 0x96U);
    disk2.quarter_track[0] = 4U;
    disk2.nibble_pos[0] = 0U;
    disk2.track_cache_valid = false;
    assert(apple2_disk2_access(&disk2, 0xCU) == 0x97U);
}

static void test_disk2_stepper_quarter_tracks(void)
{
    apple2_machine_t machine;

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });

    assert(machine.disk2.quarter_track[0] == 0U);
    (void)apple2_disk2_access(&machine.disk2, 0x1U); /* phase 0 on */
    (void)apple2_disk2_access(&machine.disk2, 0x3U); /* phase 1 on */
    (void)apple2_disk2_access(&machine.disk2, 0x0U); /* phase 0 off */
    (void)apple2_disk2_access(&machine.disk2, 0x5U); /* phase 2 on */
    (void)apple2_disk2_access(&machine.disk2, 0x2U); /* phase 1 off */
    assert(machine.disk2.quarter_track[0] == 4U);

    (void)apple2_disk2_access(&machine.disk2, 0x3U); /* phase 1 on */
    (void)apple2_disk2_access(&machine.disk2, 0x4U); /* phase 2 off */
    (void)apple2_disk2_access(&machine.disk2, 0x1U); /* phase 0 on */
    (void)apple2_disk2_access(&machine.disk2, 0x2U); /* phase 1 off */
    assert(machine.disk2.quarter_track[0] == 0U);
}

static void test_disk2_redundant_switches_do_not_reset_state(void)
{
    apple2_disk2_t disk2;
    uint8_t image[APPLE2_DISK2_NIB_IMAGE_SIZE];

    memset(image, 0x80, sizeof(image));
    apple2_disk2_init(&disk2);
    assert(apple2_disk2_load_nib_drive(&disk2, 0, image, sizeof(image)));
    assert(apple2_disk2_load_nib_drive(&disk2, 1, image, sizeof(image)));

    disk2.motor_on = true;
    disk2.active_drive = 0U;
    disk2.track_cache_valid = true;
    disk2.track_cache_drive = 0U;
    disk2.track_cache_key = 0U;
    disk2.track_cache_length = APPLE2_DISK2_NIB_TRACK_BYTES;
    disk2.stream_accum[0] = 1234U;
    disk2.data_ready = true;
    disk2.data_latch = 0xAAU;

    (void)apple2_disk2_access(&disk2, 0x9U);
    assert(disk2.motor_on);
    assert(disk2.stream_accum[0] == 1234U);
    assert(disk2.data_ready);
    assert(disk2.data_latch == 0xAAU);

    (void)apple2_disk2_access(&disk2, 0xAU);
    assert(disk2.active_drive == 0U);
    assert(disk2.track_cache_valid);
    assert(disk2.stream_accum[0] == 1234U);
    assert(disk2.data_ready);
    assert(disk2.data_latch == 0xAAU);
}

static void test_keyboard_latch_aliasing(void)
{
    apple2_machine_t machine;

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    apple2_machine_set_key(&machine, 'B');
    assert(machine.key_latch == 0xC2U);

    /* All addresses $C000-$C00F should return the key latch. */
    for (uint16_t addr = 0xC000U; addr <= 0xC00FU; ++addr) {
        const uint8_t program[] = {
            0xAD, (uint8_t)(addr & 0xFFU), (uint8_t)(addr >> 8), /* LDA addr */
            0xEA,                                                  /* NOP */
        };
        memcpy(&machine.memory[0x0200], program, sizeof(program));
        fill_reset_vector(&machine, 0x0200);
        apple2_machine_reset(&machine);
        apple2_machine_set_key(&machine, 'B');
        apple2_machine_step_instruction(&machine);
        assert(apple2_machine_cpu_state(&machine).a == 0xC2U);
    }
}

static void test_keyboard_strobe_aliasing(void)
{
    apple2_machine_t machine;

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });

    /* Reading any address $C010-$C01F should clear the strobe. */
    for (uint16_t addr = 0xC010U; addr <= 0xC01FU; ++addr) {
        apple2_machine_set_key(&machine, 'X');
        assert((machine.key_latch & 0x80U) != 0U);

        const uint8_t program[] = {
            0xAD, (uint8_t)(addr & 0xFFU), (uint8_t)(addr >> 8), /* LDA addr */
            0xEA,                                                  /* NOP */
        };
        memcpy(&machine.memory[0x0200], program, sizeof(program));
        fill_reset_vector(&machine, 0x0200);
        apple2_machine_reset(&machine);
        apple2_machine_set_key(&machine, 'X');
        apple2_machine_step_instruction(&machine);
        assert((machine.key_latch & 0x80U) == 0U);
    }

    /* Writing to $C010-$C01F should also clear the strobe. */
    apple2_machine_set_key(&machine, 'Z');
    assert((machine.key_latch & 0x80U) != 0U);
    apple2_machine_poke(&machine, 0xC015, 0x00);
    assert((machine.key_latch & 0x80U) == 0U);
}

static void test_disk2_sector_tracks_keep_dos_interleave(void)
{
    static const uint8_t s_expected_track1_order[16] = {
        0x0, 0xD, 0xB, 0x9, 0x7, 0x5, 0x3, 0x1,
        0xE, 0xC, 0xA, 0x8, 0x6, 0x4, 0x2, 0xF,
    };
    apple2_disk2_t disk2;
    uint8_t image[APPLE2_DISK2_IMAGE_SIZE];
    uint8_t sectors[16];

    memset(image, 0x00, sizeof(image));
    apple2_disk2_init(&disk2);
    assert(apple2_disk2_load_drive_with_order(&disk2,
                                              0,
                                              image,
                                              sizeof(image),
                                              APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL));

    disk2.quarter_track[0] = 4U; /* Track 1. */
    (void)apple2_disk2_access(&disk2, 0x9U);

    assert(disk2.track_cache_valid);
    assert(test_collect_track_sector_headers(&disk2, sectors, 16U) == 16U);
    assert(memcmp(sectors, s_expected_track1_order, sizeof(sectors)) == 0);
}

static void test_game_io_stubs(void)
{
    apple2_machine_t machine;

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });

    /* Push buttons $C061-$C063 should return $00 (not pressed). */
    for (uint16_t addr = 0xC061U; addr <= 0xC063U; ++addr) {
        const uint8_t program[] = {
            0xAD, (uint8_t)(addr & 0xFFU), (uint8_t)(addr >> 8), /* LDA addr */
            0xEA,                                                  /* NOP */
        };
        memcpy(&machine.memory[0x0200], program, sizeof(program));
        fill_reset_vector(&machine, 0x0200);
        apple2_machine_reset(&machine);
        apple2_machine_step_instruction(&machine);
        assert((apple2_machine_cpu_state(&machine).a & 0x80U) == 0x00U);
    }

    /* Paddle timers $C064-$C067 should return $00 (timer expired). */
    for (uint16_t addr = 0xC064U; addr <= 0xC067U; ++addr) {
        const uint8_t program[] = {
            0xAD, (uint8_t)(addr & 0xFFU), (uint8_t)(addr >> 8), /* LDA addr */
            0xEA,                                                  /* NOP */
        };
        memcpy(&machine.memory[0x0200], program, sizeof(program));
        fill_reset_vector(&machine, 0x0200);
        apple2_machine_reset(&machine);
        apple2_machine_step_instruction(&machine);
        assert((apple2_machine_cpu_state(&machine).a & 0x80U) == 0x00U);
    }
}

static void test_decimal_sbc_flags(void)
{
    apple2_machine_t machine;

    /* Test 1: $50 - $30 = $20 in BCD.
       Binary: $50 - $30 - (1 - carry=1) = $20.
       N flag from binary result: 0 (bit 7 of $20 = 0).
       Z flag from binary result: 0 ($20 != 0). */
    {
        const uint8_t program[] = {
            0xF8,       /* SED */
            0x38,       /* SEC */
            0xA9, 0x50, /* LDA #$50 */
            0xE9, 0x30, /* SBC #$30 */
            0xD8,       /* CLD */
            0xEA,       /* NOP */
        };

        apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
        memcpy(&machine.memory[0x0300], program, sizeof(program));
        fill_reset_vector(&machine, 0x0300);
        apple2_machine_reset(&machine);

        for (int i = 0; i < 6; ++i) {
            apple2_machine_step_instruction(&machine);
        }

        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        assert(cpu.a == 0x20U);  /* BCD result. */
        assert((cpu.p & CPU6502_FLAG_NEGATIVE) == 0U);  /* Binary $20: not negative. */
        assert((cpu.p & CPU6502_FLAG_ZERO) == 0U);       /* Binary $20: not zero. */
        assert((cpu.p & CPU6502_FLAG_CARRY) != 0U);      /* No borrow. */
    }

    /* Test 2: $32 - $32 = $00 in BCD.
       Binary: $32 - $32 = $00.
       Z flag from binary result: 1 ($00 == 0).
       N flag from binary result: 0. */
    {
        const uint8_t program[] = {
            0xF8,       /* SED */
            0x38,       /* SEC */
            0xA9, 0x32, /* LDA #$32 */
            0xE9, 0x32, /* SBC #$32 */
            0xD8,       /* CLD */
            0xEA,       /* NOP */
        };

        apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
        memcpy(&machine.memory[0x0300], program, sizeof(program));
        fill_reset_vector(&machine, 0x0300);
        apple2_machine_reset(&machine);

        for (int i = 0; i < 6; ++i) {
            apple2_machine_step_instruction(&machine);
        }

        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        assert(cpu.a == 0x00U);  /* BCD result. */
        assert((cpu.p & CPU6502_FLAG_ZERO) != 0U);       /* Binary $00: zero. */
        assert((cpu.p & CPU6502_FLAG_NEGATIVE) == 0U);    /* Binary $00: not negative. */
        assert((cpu.p & CPU6502_FLAG_CARRY) != 0U);       /* No borrow. */
    }

    /* Test 3: $10 - $20 = $90 in BCD (borrow).
       Binary: $10 - $20 = $F0 (wraps).
       N flag from binary result: 1 (bit 7 of $F0 = 1).
       Z flag from binary result: 0 ($F0 != 0).
       Carry: 0 (borrow occurred). */
    {
        const uint8_t program[] = {
            0xF8,       /* SED */
            0x38,       /* SEC */
            0xA9, 0x10, /* LDA #$10 */
            0xE9, 0x20, /* SBC #$20 */
            0xD8,       /* CLD */
            0xEA,       /* NOP */
        };

        apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
        memcpy(&machine.memory[0x0300], program, sizeof(program));
        fill_reset_vector(&machine, 0x0300);
        apple2_machine_reset(&machine);

        for (int i = 0; i < 6; ++i) {
            apple2_machine_step_instruction(&machine);
        }

        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        assert(cpu.a == 0x90U);  /* BCD result. */
        assert((cpu.p & CPU6502_FLAG_NEGATIVE) != 0U);  /* Binary $F0: negative. */
        assert((cpu.p & CPU6502_FLAG_ZERO) == 0U);       /* Binary $F0: not zero. */
        assert((cpu.p & CPU6502_FLAG_CARRY) == 0U);      /* Borrow occurred. */
    }
}

static void test_disk2_write_mode(void)
{
    apple2_disk2_t disk2;
    uint8_t image[APPLE2_DISK2_NIB_IMAGE_SIZE];

    memset(image, 0x80, sizeof(image));
    apple2_disk2_init(&disk2);
    assert(apple2_disk2_load_nib_drive(&disk2, 0, image, sizeof(image)));

    /* Motor on. */
    (void)apple2_disk2_access(&disk2, 0x9U);
    assert(disk2.motor_on);
    assert(!disk2.q6);
    assert(!disk2.q7);

    /* Enter write mode: Q6=1 (reg D), Q7=1 (reg F). */
    (void)apple2_disk2_access(&disk2, 0xDU); /* Q6 = true */
    (void)apple2_disk2_access(&disk2, 0xFU); /* Q7 = true */
    assert(disk2.q6);
    assert(disk2.q7);
    assert(disk2.write_mode);

    /* Exit write mode: Q7=0 (reg E). */
    (void)apple2_disk2_access(&disk2, 0xEU); /* Q7 = false */
    assert(!disk2.write_mode);
}

/* Test helper: capture sectors written by flush. */
typedef struct {
    uint8_t sectors[16][256];
    uint8_t track[16];
    uint8_t file_sector[16];
    unsigned count;
} test_write_capture_t;

static bool test_write_sector(void *context, unsigned drive_index, uint8_t track,
                               uint8_t file_sector, const uint8_t *sector_data)
{
    test_write_capture_t *cap = context;
    (void)drive_index;
    if (cap->count < 16U) {
        cap->track[cap->count] = track;
        cap->file_sector[cap->count] = file_sector;
        memcpy(cap->sectors[cap->count], sector_data, 256U);
        cap->count++;
    }
    return true;
}

static void test_disk2_write_flush_roundtrip(void)
{
    /* Create a sector image with known data. */
    uint8_t image[APPLE2_DISK2_IMAGE_SIZE];
    for (size_t i = 0; i < sizeof(image); ++i) {
        image[i] = (uint8_t)(i & 0xFFU);
    }

    apple2_disk2_t disk2;
    apple2_disk2_init(&disk2);
    assert(apple2_disk2_load_drive_with_order(&disk2, 0, image, sizeof(image),
                                               APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL));

    /* Build track 0 cache by turning motor on. */
    (void)apple2_disk2_access(&disk2, 0x9U);
    assert(disk2.track_cache_valid);

    /* Attach write callback. */
    test_write_capture_t capture;
    memset(&capture, 0, sizeof(capture));
    apple2_disk2_attach_drive_writer(&disk2, 0, test_write_sector, &capture);

    /* Mark dirty and flush. */
    disk2.track_cache_dirty = true;
    assert(apple2_disk2_flush(&disk2));
    assert(!disk2.track_cache_dirty);

    /* Should have decoded all 16 sectors from track 0. */
    assert(capture.count == 16U);

    /* Verify the first sector's data matches the original image.
       File sector 0 corresponds to the first 256 bytes of the image (track 0, sector 0). */
    bool found_sector0 = false;
    for (unsigned i = 0; i < capture.count; ++i) {
        if (capture.track[i] == 0U && capture.file_sector[i] == 0U) {
            assert(memcmp(capture.sectors[i], &image[0], 256U) == 0);
            found_sector0 = true;
            break;
        }
    }
    assert(found_sector0);
}

static void test_disk2_tick_multi_advance(void)
{
    apple2_disk2_t disk2;
    uint8_t image[APPLE2_DISK2_NIB_IMAGE_SIZE];
    const uint32_t cpu_hz = 1020484U;

    /* Fill NIB image with sequential pattern so we can detect position. */
    for (size_t i = 0; i < sizeof(image); ++i) {
        image[i] = (uint8_t)(0x80U | (i & 0x7FU));
    }
    apple2_disk2_init(&disk2);
    assert(apple2_disk2_load_nib_drive(&disk2, 0, image, sizeof(image)));

    /* Turn motor on and prepare track. */
    (void)apple2_disk2_access(&disk2, 0x9U); /* Motor on. */
    assert(disk2.motor_on);

    /* Feed a large cycle count that should advance more than 1 byte.
       At 33280 bytes/sec and 1020484 Hz, one byte = ~30.6 cycles.
       Feed 200 cycles -> should advance ~6 bytes. */
    const uint32_t initial_pos = disk2.nibble_pos[0];
    apple2_disk2_tick(&disk2, cpu_hz, 200U);
    const uint32_t after_pos = disk2.nibble_pos[0];
    assert(after_pos - initial_pos >= 5U);
    assert(after_pos - initial_pos <= 8U);
}

static void test_disk2_eager_cache_on_seek(void)
{
    apple2_disk2_t disk2;
    uint8_t image[APPLE2_DISK2_IMAGE_SIZE];

    memset(image, 0x00, sizeof(image));
    apple2_disk2_init(&disk2);
    assert(apple2_disk2_load_drive_with_order(&disk2, 0, image, sizeof(image),
                                               APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL));

    /* Motor on, build initial track cache at track 0. */
    (void)apple2_disk2_access(&disk2, 0x9U);
    assert(disk2.track_cache_valid);
    assert(disk2.quarter_track[0] == 0U);

    /* Step to track 1 (quarter_track 4) using full stepper sequence:
       phase 0 on, phase 1 on, phase 0 off, phase 2 on, phase 1 off. */
    (void)apple2_disk2_access(&disk2, 0x1U); /* Phase 0 on. */
    (void)apple2_disk2_access(&disk2, 0x3U); /* Phase 1 on. */
    (void)apple2_disk2_access(&disk2, 0x0U); /* Phase 0 off. */
    (void)apple2_disk2_access(&disk2, 0x5U); /* Phase 2 on. */
    (void)apple2_disk2_access(&disk2, 0x2U); /* Phase 1 off. */
    assert(disk2.quarter_track[0] == 4U);

    /* Cache should already be valid for the new track (eager rebuild). */
    assert(disk2.track_cache_valid);
}

int main(void)
{
    test_cpu_program();
    test_decimal_adc();
    test_undocumented_nops();
    test_soft_switches();
    test_keyboard_latch_aliasing();
    test_keyboard_strobe_aliasing();
    test_video_addresses();
    test_text_render();
    test_text_code_mapping();
    test_flash_cursor_render();
    test_flash_timing();
    test_full_apple2plus_rom_layout();
    test_separate_slot6_rom_layout();
    test_language_card_soft_switches();
    test_drive0_dsk_loading();
    test_drive0_nib_loading();
    test_drive1_and_reader_loading();
    test_track_reader_loading();
    test_woz_loading();
    test_disk2_stepper_quarter_tracks();
    test_disk2_redundant_switches_do_not_reset_state();
    test_disk2_sector_tracks_keep_dos_interleave();
    test_game_io_stubs();
    test_decimal_sbc_flags();
    test_disk2_tick_multi_advance();
    test_disk2_write_mode();
    test_disk2_write_flush_roundtrip();
    test_disk2_eager_cache_on_seek();
    puts("apple2 core tests passed");
    return 0;
}
