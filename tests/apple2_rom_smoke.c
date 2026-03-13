#include "apple2/apple2_machine.h"
#include "apple2/apple2_video.h"

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    DISK_IMAGE_NONE = 0,
    DISK_IMAGE_DSK_DOS_ORDER,
    DISK_IMAGE_DSK_PRODOS_ORDER,
    DISK_IMAGE_DO_DOS_ORDER,
    DISK_IMAGE_PO_PRODOS_ORDER,
} disk_image_type_t;

#define DSK_PROBE_INSTRUCTIONS 900000U
#define DOS_PROMPT_INSTRUCTIONS 17000000U
#define CUSTOM_DISK_INSTRUCTIONS 40000000U

static const uint8_t s_prodos_track_order[16] = {
    0x0, 0x2, 0x4, 0x6, 0x8, 0xA, 0xC, 0xE,
    0x1, 0x3, 0x5, 0x7, 0x9, 0xB, 0xD, 0xF,
};

static unsigned disk_count_nonzero_range(const apple2_machine_t *machine, uint16_t base, uint16_t size)
{
    unsigned nonzero = 0;

    for (uint16_t i = 0; i < size; ++i) {
        if (machine->memory[(uint16_t)(base + i)] != 0U) {
            nonzero++;
        }
    }

    return nonzero;
}

static unsigned disk_find_file_sector_for_physical(const uint8_t *track_order, uint8_t physical_sector)
{
    for (unsigned file_sector = 0; file_sector < 16U; ++file_sector) {
        if (track_order[file_sector] == physical_sector) {
            return file_sector;
        }
    }

    return 0U;
}

static int disk_score_dsk_order(const uint8_t *rom,
                                size_t rom_size,
                                const uint8_t *slot6_rom,
                                size_t slot6_rom_size,
                                const uint8_t *disk,
                                size_t disk_size,
                                bool prodos_order)
{
    apple2_machine_t probe;
    bool entered_stage2 = false;

    apple2_machine_init(&probe, &(apple2_config_t){ .cpu_hz = 1020484U });
    if (!apple2_machine_load_system_rom(&probe, rom, rom_size) ||
        (slot6_rom_size != 0U && !apple2_machine_load_slot6_rom(&probe, slot6_rom, slot6_rom_size))) {
        return INT_MIN / 2;
    }
    if (!(prodos_order
              ? apple2_machine_load_drive0_po(&probe, disk, disk_size)
              : apple2_machine_load_drive0_dsk(&probe, disk, disk_size))) {
        return INT_MIN / 2;
    }

    for (uint32_t i = 0; i < DSK_PROBE_INSTRUCTIONS; ++i) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&probe);

        if (cpu.pc >= 0x3700U && cpu.pc < 0x4000U) {
            entered_stage2 = true;
        }
        if (entered_stage2) {
            if (cpu.pc < 0x0100U) {
                break;
            }
            if (cpu.pc >= 0xFD00U && probe.disk2.nibble_pos[0] <= 384U) {
                break;
            }
        }
        apple2_machine_step_instruction(&probe);
    }

    {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&probe);
        int score = 0;

        score += (int)disk_count_nonzero_range(&probe, 0x1D00U, 0x0400U);
        score += (int)disk_count_nonzero_range(&probe, 0x2A00U, 0x0100U);
        if (probe.disk2.nibble_pos[0] > 384U) {
            score += 512;
        }
        if (probe.disk2.quarter_track[0] != 0U) {
            score += 128;
        }
        if (cpu.pc >= 0x3000U && cpu.pc < 0x4000U) {
            score += 128;
        }
        if (entered_stage2) {
            score += 64;
        }
        if (cpu.pc < 0x0100U) {
            score -= 512;
        }
        if (cpu.pc >= 0xFD00U) {
            score -= 256;
        }
        return score;
    }
}

static disk_image_type_t disk_probe_dsk_order(const uint8_t *rom,
                                              size_t rom_size,
                                              const uint8_t *slot6_rom,
                                              size_t slot6_rom_size,
                                              const uint8_t *disk,
                                              size_t disk_size)
{
    const int dos_score = disk_score_dsk_order(rom, rom_size, slot6_rom, slot6_rom_size, disk, disk_size, false);
    const int prodos_score = disk_score_dsk_order(rom, rom_size, slot6_rom, slot6_rom_size, disk, disk_size, true);

    return (prodos_score > dos_score) ? DISK_IMAGE_DSK_PRODOS_ORDER : DISK_IMAGE_DSK_DOS_ORDER;
}

static bool disk_stage1_preload_complete(const apple2_machine_t *machine,
                                         const uint8_t *disk,
                                         disk_image_type_t image_type)
{
    static const uint8_t s_boot_track_physical_sectors[10] = {
        0x0, 0xD, 0xB, 0x9, 0x7, 0x5, 0x3, 0x1, 0xE, 0xC,
    };

    for (unsigned sector = 0; sector < 10U; ++sector) {
        const uint16_t address = (uint16_t)(0x3600U + sector * 0x0100U);
        unsigned file_sector = sector;

        if (image_type == DISK_IMAGE_PO_PRODOS_ORDER ||
            image_type == DISK_IMAGE_DSK_PRODOS_ORDER) {
            file_sector =
                disk_find_file_sector_for_physical(s_prodos_track_order, s_boot_track_physical_sectors[sector]);
        }

        const uint8_t *expected = &disk[file_sector * 0x0100U];
        if (memcmp(&machine->memory[address], expected, 0x0100U) != 0) {
            return false;
        }
    }

    return true;
}

static bool disk_screen_row_has_prefix(const apple2_machine_t *machine, uint8_t row, const char *prefix)
{
    uint16_t address = apple2_text_row_address(false, row);

    while (*prefix != '\0') {
        if ((machine->memory[address++] & 0x7FU) != (uint8_t)*prefix++) {
            return false;
        }
    }

    return true;
}

static bool disk_screen_contains(const apple2_machine_t *machine, const char *text)
{
    const size_t length = strlen(text);

    if (length == 0U || length > 40U) {
        return false;
    }

    for (uint8_t row = 0; row < 24U; ++row) {
        const uint16_t base = apple2_text_row_address(false, row);
        for (uint8_t column = 0; column <= (40U - length); ++column) {
            bool matched = true;

            for (size_t i = 0; i < length; ++i) {
                if ((machine->memory[(uint16_t)(base + column + i)] & 0x7FU) != (uint8_t)text[i]) {
                    matched = false;
                    break;
                }
            }

            if (matched) {
                return true;
            }
        }
    }

    return false;
}

static const char *disk_extension(const char *path)
{
    const char *extension = strrchr(path, '.');

    return extension != NULL ? extension : "";
}

static disk_image_type_t disk_type_for_path(const char *path)
{
    const char *extension = disk_extension(path);

    if (strcmp(extension, ".do") == 0 || strcmp(extension, ".DO") == 0) {
        return DISK_IMAGE_DO_DOS_ORDER;
    }
    if (strcmp(extension, ".po") == 0 || strcmp(extension, ".PO") == 0) {
        return DISK_IMAGE_PO_PRODOS_ORDER;
    }
    if (strcmp(extension, ".dsk") == 0 || strcmp(extension, ".DSK") == 0) {
        return DISK_IMAGE_DSK_PRODOS_ORDER;
    }

    return DISK_IMAGE_NONE;
}

int main(void)
{
    FILE *rom_file = fopen("roms/apple2plus.rom", "rb");
    FILE *slot6_file = fopen("roms/disk2.rom", "rb");
    const char *disk_override = getenv("APPLE2_TEST_DISK");
    const char *expected_text = getenv("APPLE2_TEST_EXPECT_TEXT");
    const char *instruction_limit_override = getenv("APPLE2_TEST_INSTRUCTION_LIMIT");
    const char *disk_path = NULL;
    FILE *disk_file = NULL;
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
    bool advanced_loader = false;
    bool dos_prompt_ready = false;
    bool dos_command_echoed = false;
    bool expected_text_ready = false;
    uint32_t custom_instruction_limit = CUSTOM_DISK_INSTRUCTIONS;

    if (expected_text != NULL && expected_text[0] == '\0') {
        expected_text = NULL;
    }
    if (instruction_limit_override != NULL && instruction_limit_override[0] != '\0') {
        const unsigned long parsed = strtoul(instruction_limit_override, NULL, 10);

        if (parsed != 0UL && parsed <= UINT32_MAX) {
            custom_instruction_limit = (uint32_t)parsed;
        }
    }

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
    if (disk_override != NULL && disk_override[0] != '\0') {
        disk_path = disk_override;
        disk_type = disk_type_for_path(disk_path);
        if (disk_type == DISK_IMAGE_NONE) {
            fprintf(stderr, "unsupported disk extension for %s\n", disk_path);
            return 12;
        }
        disk_file = fopen(disk_path, "rb");
        if (disk_file == NULL) {
            perror(disk_path);
            return 13;
        }
        disk_size = fread(disk, 1, sizeof(disk), disk_file);
        fclose(disk_file);
    } else {
        disk_path = "roms/dos_3.3.do";
        disk_file = fopen(disk_path, "rb");
        if (disk_file != NULL) {
            disk_size = fread(disk, 1, sizeof(disk), disk_file);
            fclose(disk_file);
            disk_type = DISK_IMAGE_DO_DOS_ORDER;
        } else {
            disk_path = "roms/dos_3.3.po";
            disk_file = fopen(disk_path, "rb");
            if (disk_file != NULL) {
                disk_size = fread(disk, 1, sizeof(disk), disk_file);
                fclose(disk_file);
                disk_type = DISK_IMAGE_PO_PRODOS_ORDER;
            } else {
                disk_path = "roms/dos_3.3.dsk";
                disk_file = fopen(disk_path, "rb");
                if (disk_file != NULL) {
                    disk_size = fread(disk, 1, sizeof(disk), disk_file);
                    fclose(disk_file);
                    disk_type = DISK_IMAGE_DSK_PRODOS_ORDER;
                } else {
                    disk_path = NULL;
                }
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

        if (disk_type == DISK_IMAGE_DSK_PRODOS_ORDER) {
            disk_type = disk_probe_dsk_order(rom, rom_size, slot6_rom, slot6_rom_size, disk, disk_size);
        }

        switch (disk_type) {
        case DISK_IMAGE_DO_DOS_ORDER:
            loaded = apple2_machine_load_drive0_do(&machine, disk, disk_size);
            break;
        case DISK_IMAGE_PO_PRODOS_ORDER:
        case DISK_IMAGE_DSK_PRODOS_ORDER:
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
            fprintf(stderr, "unsupported disk image size: %zu (%s)\n",
                    disk_size,
                    disk_path != NULL ? disk_path : "unknown");
            return 5;
        }
    }

    {
        const bool expect_prodos_prompt =
            expected_text == NULL &&
            disk_size != 0U &&
            (disk_type == DISK_IMAGE_PO_PRODOS_ORDER || disk_type == DISK_IMAGE_DSK_PRODOS_ORDER);
        const uint32_t instruction_limit =
            expected_text != NULL ? custom_instruction_limit :
            expect_prodos_prompt ? DOS_PROMPT_INSTRUCTIONS : 1500000U;

        for (uint32_t i = 0; i < instruction_limit; ++i) {
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
        if (disk_size != 0U &&
            entered_stage2 &&
            machine.disk2.quarter_track[0] >= 4U &&
            cpu.pc < 0xFD00U) {
            advanced_loader = true;
        }
        if (disk_size != 0U &&
            entered_stage2 &&
            cpu.pc >= 0xFD00U &&
            machine.disk2.nibble_pos[0] <= 384U) {
            break;
        }
        if (expected_text != NULL && disk_screen_contains(&machine, expected_text)) {
            expected_text_ready = true;
            break;
        }
        if (expect_prodos_prompt &&
            cpu.pc >= 0xFD1BU &&
            cpu.pc <= 0xFD24U &&
            disk_screen_contains(&machine, "DOS VERSION 3.3")) {
            for (uint8_t row = 0; row < 24U; ++row) {
                if (disk_screen_row_has_prefix(&machine, row, "]")) {
                    dos_prompt_ready = true;
                    break;
                }
            }
            if (dos_prompt_ready) {
                break;
            }
        }
        if (disk_size == 0U && entered_slot6) {
            break;
        }
        apple2_machine_step_instruction(&machine);
    }
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
    if (expected_text != NULL) {
        if (!expected_text_ready) {
            const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
            fprintf(stderr,
                    "Disk boot did not reach expected text \"%s\", pc=%04x qt=%u np=%u row5=%02x row6=%02x\n",
                    expected_text,
                    cpu.pc,
                    machine.disk2.quarter_track[0],
                    machine.disk2.nibble_pos[0],
                    machine.memory[apple2_text_row_address(false, 5U)],
                    machine.memory[apple2_text_row_address(false, 6U)]);
            return 14;
        }

        puts("apple2 ROM+disk custom smoke passed");
        return 0;
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
    if (disk_size != 0U &&
        (disk_type == DISK_IMAGE_PO_PRODOS_ORDER || disk_type == DISK_IMAGE_DSK_PRODOS_ORDER) &&
        !advanced_loader) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        fprintf(stderr,
                "Disk loader did not seek beyond track 0, pc=%04x qt=%u np=%u type=%d p1d00=%02x\n",
                cpu.pc, machine.disk2.quarter_track[0], machine.disk2.nibble_pos[0], (int)disk_type,
                machine.memory[0x1D00]);
        return 9;
    }
    if (disk_size != 0U &&
        (disk_type == DISK_IMAGE_PO_PRODOS_ORDER || disk_type == DISK_IMAGE_DSK_PRODOS_ORDER) &&
        !dos_prompt_ready) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        fprintf(stderr,
                "Disk boot did not reach the DOS prompt, pc=%04x qt=%u np=%u row0=%02x row5=%02x\n",
                cpu.pc, machine.disk2.quarter_track[0], machine.disk2.nibble_pos[0],
                machine.memory[apple2_text_row_address(false, 0U)],
                machine.memory[apple2_text_row_address(false, 5U)]);
        return 10;
    }
    if (disk_size != 0U &&
        (disk_type == DISK_IMAGE_PO_PRODOS_ORDER || disk_type == DISK_IMAGE_DSK_PRODOS_ORDER)) {
        static const char s_test_command[] = "PRINT 1\r";
        size_t command_pos = 0;

        for (uint32_t i = 0; i < 200000U; ++i) {
            if (command_pos < (sizeof(s_test_command) - 1U) && (machine.key_latch & 0x80U) == 0U) {
                apple2_machine_set_key(&machine, (uint8_t)s_test_command[command_pos++]);
            }
            if (command_pos == (sizeof(s_test_command) - 1U) &&
                disk_screen_contains(&machine, "]PRINT 1")) {
                dos_command_echoed = true;
                break;
            }
            apple2_machine_step_instruction(&machine);
        }

        if (!dos_command_echoed) {
            const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
            fprintf(stderr,
                    "DOS prompt did not echo keyboard input, pc=%04x key=%02x row5=%02x row6=%02x\n",
                    cpu.pc, machine.key_latch,
                    machine.memory[apple2_text_row_address(false, 5U)],
                    machine.memory[apple2_text_row_address(false, 6U)]);
            return 11;
        }
    }

    puts(disk_size != 0U ? "apple2 ROM+disk prompt/input smoke passed" : "apple2 ROM smoke passed");
    return 0;
}
