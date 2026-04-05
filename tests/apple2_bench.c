#include "apple2/apple2_machine.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void)
{
    FILE *rom_file = fopen("roms/apple2plus.rom", "rb");
    FILE *slot6_file = fopen("roms/disk2.rom", "rb");
    uint8_t rom[0x8000];
    uint8_t slot6_rom[0x0100];
    size_t rom_size;
    size_t slot6_rom_size;
    apple2_machine_t machine;
    const uint32_t bench_instructions = 20000000U;
    struct timespec start, end;
    double elapsed_s;
    double mips;
    double mhz;

    if (rom_file == NULL) {
        perror("roms/apple2plus.rom");
        return 1;
    }
    rom_size = fread(rom, 1, sizeof(rom), rom_file);
    fclose(rom_file);

    apple2_machine_init(&machine, &(apple2_config_t){ .cpu_hz = 1020484U });
    if (!apple2_machine_load_system_rom(&machine, rom, rom_size)) {
        fprintf(stderr, "Failed to load system ROM\n");
        return 2;
    }

    if (slot6_file != NULL) {
        slot6_rom_size = fread(slot6_rom, 1, sizeof(slot6_rom), slot6_file);
        fclose(slot6_file);
        apple2_machine_load_slot6_rom(&machine, slot6_rom, slot6_rom_size);
    }

    apple2_machine_reset(&machine);

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (uint32_t i = 0; i < bench_instructions; ++i) {
        apple2_machine_step_instruction(&machine);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed_s = (double)(end.tv_sec - start.tv_sec) +
                (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    mips = (double)bench_instructions / elapsed_s / 1e6;
    mhz = (double)machine.total_cycles / elapsed_s / 1e6;

    printf("%-24s %u\n", "instructions:", bench_instructions);
    printf("%-24s %" PRIu64 "\n", "cycles:", machine.total_cycles);
    printf("%-24s %.3f s\n", "elapsed:", elapsed_s);
    printf("%-24s %.2f\n", "MIPS:", mips);
    printf("%-24s %.2f\n", "emulated MHz:", mhz);

    return 0;
}
