#include "apple2/apple2_machine.h"
#include "apple2/apple2_video.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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
    assert(disk2.track_cache_length != 0U);
    assert(first_byte == disk2.track_cache[0]);

    apple2_disk2_unload_drive(&disk2, 1);
    assert(!apple2_disk2_drive_loaded(&disk2, 1));
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

int main(void)
{
    test_cpu_program();
    test_decimal_adc();
    test_soft_switches();
    test_video_addresses();
    test_text_render();
    test_full_apple2plus_rom_layout();
    test_separate_slot6_rom_layout();
    test_drive0_dsk_loading();
    test_drive1_and_reader_loading();
    test_disk2_stepper_quarter_tracks();
    puts("apple2 core tests passed");
    return 0;
}
