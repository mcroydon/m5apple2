#include "apple2/apple2_machine.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    DISK_IMAGE_NONE = 0,
    DISK_IMAGE_DSK_DOS_ORDER,
    DISK_IMAGE_DO_DOS_ORDER,
    DISK_IMAGE_PO_PHYSICAL_ORDER,
} disk_image_type_t;

static bool disk_stage1_preload_complete(const apple2_machine_t *machine,
                                         const uint8_t *disk,
                                         disk_image_type_t image_type)
{
    static const uint8_t s_physical_track0_boot_sectors[10] = {
        0x0, 0xD, 0xB, 0x9, 0x7, 0x5, 0x3, 0x1, 0xE, 0xC,
    };

    for (unsigned sector = 0; sector < 10U; ++sector) {
        const uint16_t address = (uint16_t)(0x3600U + sector * 0x0100U);
        unsigned file_sector = sector;

        if (image_type == DISK_IMAGE_PO_PHYSICAL_ORDER) {
            file_sector = s_physical_track0_boot_sectors[sector];
        }

        const uint8_t *expected = &disk[file_sector * 0x0100U];
        if (memcmp(&machine->memory[address], expected, 0x0100U) != 0) {
            return false;
        }
    }

    return true;
}

int main(void)
{
    FILE *rom_file = fopen("roms/apple2plus.rom", "rb");
    FILE *slot6_file = fopen("roms/disk2.rom", "rb");
    FILE *disk_file = fopen("roms/dos_3.3.do", "rb");
    uint8_t rom[0x8000];
    uint8_t slot6_rom[0x0100];
    uint8_t disk[APPLE2_DISK2_IMAGE_SIZE];
    size_t rom_size;
    size_t slot6_rom_size = 0;
    size_t disk_size = 0;
    disk_image_type_t disk_type = DISK_IMAGE_NONE;
    apple2_machine_t machine;
    bool entered_slot6 = false;
    bool entered_boot1 = false;
    bool stage1_preloaded = false;
    bool entered_stage2 = false;

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
    if (disk_file != NULL) {
        disk_size = fread(disk, 1, sizeof(disk), disk_file);
        fclose(disk_file);
        disk_type = DISK_IMAGE_DO_DOS_ORDER;
    } else {
        disk_file = fopen("roms/dos_3.3.po", "rb");
        if (disk_file != NULL) {
            disk_size = fread(disk, 1, sizeof(disk), disk_file);
            fclose(disk_file);
            disk_type = DISK_IMAGE_PO_PHYSICAL_ORDER;
        } else {
            disk_file = fopen("roms/dos_3.3.dsk", "rb");
            if (disk_file != NULL) {
                disk_size = fread(disk, 1, sizeof(disk), disk_file);
                fclose(disk_file);
                disk_type = DISK_IMAGE_DSK_DOS_ORDER;
            }
        }
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
    if (disk_size != 0U) {
        bool loaded = false;

        switch (disk_type) {
        case DISK_IMAGE_DO_DOS_ORDER:
            loaded = apple2_machine_load_drive0_do(&machine, disk, disk_size);
            break;
        case DISK_IMAGE_PO_PHYSICAL_ORDER:
            loaded = apple2_machine_load_drive0_po(&machine, disk, disk_size);
            break;
        case DISK_IMAGE_DSK_DOS_ORDER:
            loaded = apple2_machine_load_drive0_dsk(&machine, disk, disk_size);
            break;
        case DISK_IMAGE_NONE:
        default:
            loaded = false;
            break;
        }

        if (!loaded) {
            fprintf(stderr, "unsupported disk image size: %zu\n", disk_size);
            return 5;
        }
    }

    for (uint32_t i = 0; i < 1500000U; ++i) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        if (cpu.pc >= 0xC600U && cpu.pc < 0xC700U) {
            entered_slot6 = true;
        }
        if (disk_size != 0U && cpu.pc >= 0x0800U && cpu.pc < 0x0C00U) {
            entered_boot1 = true;
        }
        if (disk_size != 0U && cpu.pc >= 0x3700U && cpu.pc < 0x3800U) {
            entered_stage2 = true;
        }
        if (disk_size != 0U && disk_stage1_preload_complete(&machine, disk, disk_type)) {
            stage1_preloaded = true;
        }
        if (disk_size != 0U && stage1_preloaded && entered_stage2) {
            break;
        }
        if (disk_size == 0U && entered_slot6) {
            break;
        }
        apple2_machine_step_instruction(&machine);
    }

    if (!entered_slot6) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        fprintf(stderr, "ROM did not reach slot 6 boot code, pc=%04x\n", cpu.pc);
        return 3;
    }
    if (disk_size != 0U && !entered_boot1) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        fprintf(stderr, "Disk boot did not reach DOS boot code, pc=%04x m800=%02x m801=%02x\n",
                cpu.pc, machine.memory[0x0800], machine.memory[0x0801]);
        return 6;
    }
    if (disk_size != 0U && !stage1_preloaded) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        fprintf(stderr,
                "Disk boot did not preload track 0 pages, pc=%04x p3600=%02x p3700=%02x p3f00=%02x type=%d\n",
                cpu.pc, machine.memory[0x3600], machine.memory[0x3700], machine.memory[0x3F00], (int)disk_type);
        return 7;
    }
    if (disk_size != 0U && !entered_stage2) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        fprintf(stderr,
                "Disk boot did not reach the DOS stage2 entry page, pc=%04x p3700=%02x p3701=%02x p3702=%02x\n",
                cpu.pc, machine.memory[0x3700], machine.memory[0x3701], machine.memory[0x3702]);
        return 8;
    }

    puts(disk_size != 0U ? "apple2 ROM+disk stage2 smoke passed" : "apple2 ROM smoke passed");
    return 0;
}
