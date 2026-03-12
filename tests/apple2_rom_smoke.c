#include "apple2/apple2_machine.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    FILE *rom_file = fopen("roms/apple2plus.rom", "rb");
    FILE *slot6_file = fopen("roms/disk2.rom", "rb");
    uint8_t rom[0x8000];
    uint8_t slot6_rom[0x0100];
    size_t rom_size;
    size_t slot6_rom_size = 0;
    apple2_machine_t machine;
    bool entered_slot6 = false;

    if (rom_file == NULL) {
        perror("roms/apple2plus.rom");
        return 1;
    }

    rom_size = fread(rom, 1, sizeof(rom), rom_file);
    fclose(rom_file);
    if (slot6_file != NULL) {
        slot6_rom_size = fread(slot6_rom, 1, sizeof(slot6_rom), slot6_file);
        fclose(slot6_file);
    }

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    if (!apple2_machine_load_system_rom(&machine, rom, rom_size)) {
        fprintf(stderr, "unsupported ROM size: %zu\n", rom_size);
        return 2;
    }
    if (slot6_rom_size != 0U && !apple2_machine_load_slot6_rom(&machine, slot6_rom, slot6_rom_size)) {
        fprintf(stderr, "unsupported slot 6 ROM size: %zu\n", slot6_rom_size);
        return 4;
    }

    for (uint32_t i = 0; i < 200000U; ++i) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        if (cpu.pc >= 0xC600U && cpu.pc < 0xC700U) {
            entered_slot6 = true;
            break;
        }
        apple2_machine_step_instruction(&machine);
    }

    if (!entered_slot6) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        fprintf(stderr, "ROM did not reach slot 6 boot code, pc=%04x\n", cpu.pc);
        return 3;
    }

    puts("apple2 ROM smoke passed");
    return 0;
}
