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

int main(void)
{
    test_cpu_program();
    test_decimal_adc();
    test_soft_switches();
    test_video_addresses();
    test_text_render();
    puts("apple2 core tests passed");
    return 0;
}
