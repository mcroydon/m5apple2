#include "apple2/apple2_machine.h"
#include "apple2/apple2_video.h"

#include <inttypes.h>
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
#define DSK_PROBE_FALLBACK_INSTRUCTIONS 3000000U
#define DOS_PROMPT_INSTRUCTIONS 17000000U
#define CUSTOM_DISK_INSTRUCTIONS 40000000U
#define DISK_READ_TRACE_ENTRIES 64U
#define PC_TRACE_ENTRIES 128U
#define APP_PC_TRACE_ENTRIES 64U
#define APP_BOOT_TRACE_LIMIT 512U

typedef struct {
    const uint8_t *disk;
    size_t disk_size;
    uint32_t total_reads;
    struct {
        uint8_t track;
        uint8_t file_sector;
    } recent_reads[DISK_READ_TRACE_ENTRIES];
} disk_read_trace_t;

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

static unsigned disk_stage1_preload_match_score(const apple2_machine_t *machine,
                                                const uint8_t *disk,
                                                disk_image_type_t image_type)
{
    static const uint8_t s_boot_track_physical_sectors[10] = {
        0x0, 0xD, 0xB, 0x9, 0x7, 0x5, 0x3, 0x1, 0xE, 0xC,
    };
    unsigned matched_bytes = 0U;

    for (unsigned sector = 0; sector < 10U; ++sector) {
        const uint16_t address = (uint16_t)(0x3600U + sector * 0x0100U);
        unsigned file_sector = sector;

        if (image_type == DISK_IMAGE_PO_PRODOS_ORDER ||
            image_type == DISK_IMAGE_DSK_PRODOS_ORDER) {
            file_sector =
                disk_find_file_sector_for_physical(s_prodos_track_order, s_boot_track_physical_sectors[sector]);
        }

        for (unsigned byte_index = 0; byte_index < 256U; ++byte_index) {
            if (machine->memory[(uint16_t)(address + byte_index)] ==
                disk[(size_t)file_sector * 0x0100U + byte_index]) {
                matched_bytes++;
            }
        }
    }

    return matched_bytes;
}

static int disk_score_dsk_order(const uint8_t *rom,
                                size_t rom_size,
                                const uint8_t *slot6_rom,
                                size_t slot6_rom_size,
                                const uint8_t *disk,
                                size_t disk_size,
                                bool prodos_order,
                                uint32_t instruction_limit,
                                apple2_cpu_state_t *cpu_out,
                                uint8_t *quarter_track_out,
                                uint32_t *nibble_pos_out,
                                bool *entered_stage2_out)
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

    for (uint32_t i = 0; i < instruction_limit; ++i) {
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
        const unsigned preload_match_score =
            disk_stage1_preload_match_score(&probe,
                                            disk,
                                            prodos_order ? DISK_IMAGE_DSK_PRODOS_ORDER
                                                         : DISK_IMAGE_DSK_DOS_ORDER);
        int score = 0;

        score += (int)disk_count_nonzero_range(&probe, 0x1D00U, 0x0400U);
        score += (int)disk_count_nonzero_range(&probe, 0x2A00U, 0x0100U);
        score += (int)preload_match_score;
        if (preload_match_score == (10U * 256U)) {
            score += 1024;
        }
        if (probe.disk2.nibble_pos[0] > 384U) {
            score += 512;
        }
        if (probe.disk2.quarter_track[0] != 0U) {
            score += 128;
        }
        if (probe.disk2.quarter_track[0] >= 4U) {
            score += 256;
        }
        if (cpu.pc >= 0xB000U && cpu.pc < 0xC000U) {
            score += 512;
        }
        if (cpu.pc >= 0x3000U && cpu.pc < 0x4000U) {
            score += 128;
        }
        if (entered_stage2) {
            score += 64;
        }
        if (cpu.pc >= 0xC600U && cpu.pc < 0xC700U) {
            score -= 128;
        }
        if (cpu.pc < 0x0100U) {
            score -= 512;
        }
        if (cpu.pc >= 0xFD00U) {
            score -= 256;
        }
        if (cpu_out != NULL) {
            *cpu_out = cpu;
        }
        if (quarter_track_out != NULL) {
            *quarter_track_out = probe.disk2.quarter_track[0];
        }
        if (nibble_pos_out != NULL) {
            *nibble_pos_out = probe.disk2.nibble_pos[0];
        }
        if (entered_stage2_out != NULL) {
            *entered_stage2_out = entered_stage2;
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
    apple2_cpu_state_t dos_cpu = { 0 };
    apple2_cpu_state_t prodos_cpu = { 0 };
    uint8_t dos_qt = 0U;
    uint8_t prodos_qt = 0U;
    bool dos_stage2 = false;
    bool prodos_stage2 = false;
    int dos_score =
        disk_score_dsk_order(rom,
                             rom_size,
                             slot6_rom,
                             slot6_rom_size,
                             disk,
                             disk_size,
                             false,
                             DSK_PROBE_INSTRUCTIONS,
                             &dos_cpu,
                             &dos_qt,
                             NULL,
                             &dos_stage2);
    int prodos_score =
        disk_score_dsk_order(rom,
                             rom_size,
                             slot6_rom,
                             slot6_rom_size,
                             disk,
                             disk_size,
                             true,
                             DSK_PROBE_INSTRUCTIONS,
                             &prodos_cpu,
                             &prodos_qt,
                             NULL,
                             &prodos_stage2);

    if (dos_cpu.pc >= 0xC600U && dos_cpu.pc < 0xC700U &&
        prodos_cpu.pc >= 0xC600U && prodos_cpu.pc < 0xC700U &&
        dos_qt == 0U && prodos_qt == 0U &&
        !dos_stage2 && !prodos_stage2) {
        dos_score =
            disk_score_dsk_order(rom,
                                 rom_size,
                                 slot6_rom,
                                 slot6_rom_size,
                                 disk,
                                 disk_size,
                                 false,
                                 DSK_PROBE_FALLBACK_INSTRUCTIONS,
                                 &dos_cpu,
                                 &dos_qt,
                                 NULL,
                                 &dos_stage2);
        prodos_score =
            disk_score_dsk_order(rom,
                                 rom_size,
                                 slot6_rom,
                                 slot6_rom_size,
                                 disk,
                                 disk_size,
                                 true,
                                 DSK_PROBE_FALLBACK_INSTRUCTIONS,
                                 &prodos_cpu,
                                 &prodos_qt,
                                 NULL,
                                 &prodos_stage2);
    }

    {
        int dos_rank = 0;
        int prodos_rank = 0;

        if (dos_stage2 || dos_qt >= 4U || (dos_cpu.pc >= 0x0200U && dos_cpu.pc < 0xC600U)) {
            dos_rank += 4;
        }
        if (dos_stage2) {
            dos_rank += 4;
        }
        if (dos_qt >= 4U) {
            dos_rank += 2;
        }
        if (dos_cpu.pc >= 0xB000U && dos_cpu.pc < 0xC000U) {
            dos_rank += 2;
        } else if (dos_cpu.pc >= 0x0200U && dos_cpu.pc < 0xC600U) {
            dos_rank += 1;
        }
        if (dos_cpu.pc >= 0xFD00U && dos_qt == 0U && !dos_stage2) {
            dos_rank -= 4;
        }
        if (dos_cpu.pc >= 0xC600U && dos_cpu.pc < 0xC700U && dos_qt == 0U && !dos_stage2) {
            dos_rank -= 2;
        }

        if (prodos_stage2 || prodos_qt >= 4U || (prodos_cpu.pc >= 0x0200U && prodos_cpu.pc < 0xC600U)) {
            prodos_rank += 4;
        }
        if (prodos_stage2) {
            prodos_rank += 4;
        }
        if (prodos_qt >= 4U) {
            prodos_rank += 2;
        }
        if (prodos_cpu.pc >= 0xB000U && prodos_cpu.pc < 0xC000U) {
            prodos_rank += 2;
        } else if (prodos_cpu.pc >= 0x0200U && prodos_cpu.pc < 0xC600U) {
            prodos_rank += 1;
        }
        if (prodos_cpu.pc >= 0xFD00U && prodos_qt == 0U && !prodos_stage2) {
            prodos_rank -= 4;
        }
        if (prodos_cpu.pc >= 0xC600U && prodos_cpu.pc < 0xC700U && prodos_qt == 0U && !prodos_stage2) {
            prodos_rank -= 2;
        }

        if (dos_rank > prodos_rank) {
            return DISK_IMAGE_DSK_DOS_ORDER;
        }
        if (prodos_rank > dos_rank) {
            return DISK_IMAGE_DSK_PRODOS_ORDER;
        }
    }

    if ((dos_stage2 || dos_qt >= 4U || (dos_cpu.pc >= 0x0200U && dos_cpu.pc < 0xC600U)) &&
        prodos_cpu.pc >= 0xFD00U &&
        prodos_qt == 0U &&
        !prodos_stage2) {
        return DISK_IMAGE_DSK_DOS_ORDER;
    }
    if ((prodos_stage2 || prodos_qt >= 4U || (prodos_cpu.pc >= 0x0200U && prodos_cpu.pc < 0xC600U)) &&
        dos_cpu.pc >= 0xFD00U &&
        dos_qt == 0U &&
        !dos_stage2) {
        return DISK_IMAGE_DSK_PRODOS_ORDER;
    }

    return (prodos_score >= dos_score) ? DISK_IMAGE_DSK_PRODOS_ORDER : DISK_IMAGE_DSK_DOS_ORDER;
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

static bool disk_screen_has_basic_prompt(const apple2_machine_t *machine)
{
    for (uint8_t row = 0; row < 24U; ++row) {
        if (disk_screen_row_has_prefix(machine, row, "]")) {
            return true;
        }
    }

    return false;
}

static bool disk_trace_read_sector(void *context,
                                   unsigned drive_index,
                                   uint8_t track,
                                   uint8_t file_sector,
                                   uint8_t *sector_data)
{
    disk_read_trace_t *trace = context;
    const size_t offset = (((size_t)track * 16U) + file_sector) * 256U;
    const uint32_t slot = trace != NULL ? (trace->total_reads % DISK_READ_TRACE_ENTRIES) : 0U;

    (void)drive_index;
    if (trace == NULL || sector_data == NULL || trace->disk == NULL || trace->disk_size != APPLE2_DISK2_IMAGE_SIZE) {
        return false;
    }
    if (track >= 35U || file_sector >= 16U || offset > trace->disk_size || 256U > (trace->disk_size - offset)) {
        return false;
    }

    memcpy(sector_data, &trace->disk[offset], 256U);
    trace->recent_reads[slot].track = track;
    trace->recent_reads[slot].file_sector = file_sector;
    trace->total_reads++;
    return true;
}

static void disk_dump_recent_sector_reads(const disk_read_trace_t *trace)
{
    uint32_t count;
    uint32_t start_slot;

    if (trace == NULL || trace->total_reads == 0U) {
        return;
    }

    count = trace->total_reads < DISK_READ_TRACE_ENTRIES ? trace->total_reads : DISK_READ_TRACE_ENTRIES;
    start_slot = trace->total_reads < DISK_READ_TRACE_ENTRIES
                     ? 0U
                     : (trace->total_reads % DISK_READ_TRACE_ENTRIES);

    fprintf(stderr, "recent sector reads (%" PRIu32 " total):\n", trace->total_reads);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = (start_slot + i) % DISK_READ_TRACE_ENTRIES;
        const uint32_t sequence = trace->total_reads - count + i;

        fprintf(stderr,
                "  %03" PRIu32 ": T%u S%u\n",
                sequence,
                trace->recent_reads[slot].track,
                trace->recent_reads[slot].file_sector);
    }
}

static bool disk_type_is_sector_image(disk_image_type_t type)
{
    switch (type) {
    case DISK_IMAGE_DSK_DOS_ORDER:
    case DISK_IMAGE_DSK_PRODOS_ORDER:
    case DISK_IMAGE_DO_DOS_ORDER:
    case DISK_IMAGE_PO_PRODOS_ORDER:
        return true;
    case DISK_IMAGE_NONE:
    default:
        return false;
    }
}

static apple2_disk2_image_order_t disk_image_order_for_type(disk_image_type_t type)
{
    switch (type) {
    case DISK_IMAGE_PO_PRODOS_ORDER:
    case DISK_IMAGE_DSK_PRODOS_ORDER:
        return APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL;
    case DISK_IMAGE_DSK_DOS_ORDER:
    case DISK_IMAGE_DO_DOS_ORDER:
    case DISK_IMAGE_NONE:
    default:
        return APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL;
    }
}

static void disk_dump_screen(const apple2_machine_t *machine)
{
    char row_text[41];

    for (uint8_t row = 0; row < 24U; ++row) {
        const uint16_t base = apple2_text_row_address(false, row);

        for (uint8_t column = 0; column < 40U; ++column) {
            bool inverse = false;
            const uint8_t ascii = apple2_text_code_to_ascii(machine->memory[(uint16_t)(base + column)], &inverse);

            row_text[column] = (ascii >= 32U && ascii <= 126U) ? (char)ascii : '.';
        }
        row_text[40] = '\0';
        fprintf(stderr, "row%02u: %s\n", row, row_text);
    }
}

static void disk_dump_recent_pcs(const uint16_t *pcs, size_t count, size_t next_index)
{
    if (count == 0U) {
        return;
    }

    fprintf(stderr, "recent pcs (%zu total):\n", count);
    for (size_t i = 0; i < count; ++i) {
        const size_t index = (next_index + i) % count;

        fprintf(stderr, "  %03zu: %04x\n", i, pcs[index]);
    }
}

static void disk_dump_bytes(const apple2_machine_t *machine, uint16_t base, uint16_t count)
{
    fprintf(stderr, "bytes %04x:", base);
    for (uint16_t i = 0; i < count; ++i) {
        fprintf(stderr, " %02x", machine->memory[(uint16_t)(base + i)]);
    }
    fputc('\n', stderr);
}

static void disk_trace_app_boot(const apple2_machine_t *machine, uint32_t step_index)
{
    static uint32_t emitted = 0U;
    const apple2_cpu_state_t cpu = apple2_machine_cpu_state(machine);

    if (emitted >= APP_BOOT_TRACE_LIMIT) {
        return;
    }
    if (!((cpu.pc >= 0x07FDU && cpu.pc < 0x0850U) ||
          (cpu.pc >= 0xB800U && cpu.pc < 0xBE00U))) {
        return;
    }

    fprintf(stderr,
            "appboot %03" PRIu32 " pc=%04x op=%02x %02x %02x a=%02x x=%02x y=%02x sp=%02x p=%02x\n",
            step_index,
            cpu.pc,
            machine->memory[cpu.pc],
            machine->memory[(uint16_t)(cpu.pc + 1U)],
            machine->memory[(uint16_t)(cpu.pc + 2U)],
            cpu.a,
            cpu.x,
            cpu.y,
            cpu.sp,
            cpu.p);
    emitted++;
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
    const char *boot_command = getenv("APPLE2_TEST_BOOT_COMMAND");
    const char *dump_screen = getenv("APPLE2_TEST_DUMP_SCREEN");
    const char *log_dsk_order = getenv("APPLE2_TEST_LOG_DSK_ORDER");
    const char *trace_sector_reads = getenv("APPLE2_TEST_TRACE_SECTOR_READS");
    const char *expect_not_basic_prompt = getenv("APPLE2_TEST_EXPECT_NOT_BASIC_PROMPT");
    const char *trace_pcs = getenv("APPLE2_TEST_TRACE_PCS");
    const char *trace_app_boot = getenv("APPLE2_TEST_TRACE_APP_BOOT");
    const char *force_direct_disk = getenv("APPLE2_TEST_FORCE_DIRECT_DISK");
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
    bool boot_command_started = false;
    bool boot_command_finished = false;
    bool entered_loaded_binary = false;
    uint16_t last_loaded_binary_pc = 0U;
    size_t boot_command_pos = 0;
    uint32_t custom_instruction_limit = CUSTOM_DISK_INSTRUCTIONS;
    const bool trace_sector_reads_enabled = trace_sector_reads != NULL && trace_sector_reads[0] != '\0';
    const bool require_not_basic_prompt = expect_not_basic_prompt != NULL && expect_not_basic_prompt[0] != '\0';
    const bool trace_pcs_enabled = trace_pcs != NULL && trace_pcs[0] != '\0';
    const bool trace_app_boot_enabled = trace_app_boot != NULL && trace_app_boot[0] != '\0';
    const bool force_direct_disk_enabled = force_direct_disk != NULL && force_direct_disk[0] != '\0';
    disk_read_trace_t disk_read_trace = { 0 };
    bool disk_read_trace_active = false;
    uint16_t recent_pcs[PC_TRACE_ENTRIES] = { 0 };
    size_t recent_pc_count = 0U;
    size_t recent_pc_next = 0U;
    uint16_t recent_app_pcs[APP_PC_TRACE_ENTRIES] = { 0 };
    size_t recent_app_pc_count = 0U;
    size_t recent_app_pc_next = 0U;

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
            if (log_dsk_order != NULL && log_dsk_order[0] != '\0') {
                fprintf(stderr, "probed .dsk order -> type=%d\n", (int)disk_type);
            }
        }

        switch (disk_type) {
        case DISK_IMAGE_DO_DOS_ORDER:
            if (!force_direct_disk_enabled &&
                (trace_sector_reads_enabled || require_not_basic_prompt) &&
                disk_type_is_sector_image(disk_type)) {
                disk_read_trace.disk = disk;
                disk_read_trace.disk_size = disk_size;
                loaded = apple2_disk2_attach_drive_reader(&machine.disk2,
                                                          0,
                                                          disk_trace_read_sector,
                                                          &disk_read_trace,
                                                          disk_size,
                                                          disk_image_order_for_type(disk_type));
                disk_read_trace_active = loaded;
            } else {
                loaded = apple2_machine_load_drive0_do(&machine, disk, disk_size);
            }
            break;
        case DISK_IMAGE_PO_PRODOS_ORDER:
        case DISK_IMAGE_DSK_PRODOS_ORDER:
            if (!force_direct_disk_enabled &&
                (trace_sector_reads_enabled || require_not_basic_prompt) &&
                disk_type_is_sector_image(disk_type)) {
                disk_read_trace.disk = disk;
                disk_read_trace.disk_size = disk_size;
                loaded = apple2_disk2_attach_drive_reader(&machine.disk2,
                                                          0,
                                                          disk_trace_read_sector,
                                                          &disk_read_trace,
                                                          disk_size,
                                                          disk_image_order_for_type(disk_type));
                disk_read_trace_active = loaded;
            } else {
                loaded = apple2_machine_load_drive0_po(&machine, disk, disk_size);
            }
            break;
        case DISK_IMAGE_DSK_DOS_ORDER:
            if (!force_direct_disk_enabled &&
                (trace_sector_reads_enabled || require_not_basic_prompt) &&
                disk_type_is_sector_image(disk_type)) {
                disk_read_trace.disk = disk;
                disk_read_trace.disk_size = disk_size;
                loaded = apple2_disk2_attach_drive_reader(&machine.disk2,
                                                          0,
                                                          disk_trace_read_sector,
                                                          &disk_read_trace,
                                                          disk_size,
                                                          disk_image_order_for_type(disk_type));
                disk_read_trace_active = loaded;
            } else {
                loaded = apple2_machine_load_drive0_dsk(&machine, disk, disk_size);
            }
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
            (expected_text != NULL || require_not_basic_prompt) ? custom_instruction_limit :
            expect_prodos_prompt ? DOS_PROMPT_INSTRUCTIONS : 20000000U;

        for (uint32_t i = 0; i < instruction_limit; ++i) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        if (trace_app_boot_enabled) {
            disk_trace_app_boot(&machine, i);
        }
        if (trace_pcs_enabled) {
            recent_pcs[recent_pc_next] = cpu.pc;
            recent_pc_next = (recent_pc_next + 1U) % PC_TRACE_ENTRIES;
            if (recent_pc_count < PC_TRACE_ENTRIES) {
                recent_pc_count++;
            }
        }
        if (cpu.pc >= 0xC600U && cpu.pc < 0xC700U) {
            entered_slot6 = true;
        }
        if (disk_size != 0U && cpu.pc >= 0x0800U && cpu.pc < 0x0C00U) {
            entered_boot1 = true;
        }
        if (disk_size != 0U && cpu.pc >= 0x07FDU && cpu.pc < 0x6900U) {
            entered_loaded_binary = true;
            last_loaded_binary_pc = cpu.pc;
            if (trace_pcs_enabled) {
                recent_app_pcs[recent_app_pc_next] = cpu.pc;
                recent_app_pc_next = (recent_app_pc_next + 1U) % APP_PC_TRACE_ENTRIES;
                if (recent_app_pc_count < APP_PC_TRACE_ENTRIES) {
                    recent_app_pc_count++;
                }
            }
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
        if (expected_text != NULL &&
            boot_command != NULL &&
            boot_command[0] != '\0' &&
            !boot_command_finished) {
            /* Inject boot command when the CPU is polling the keyboard
               (LDA $C000 = AD 00 C0) or when a BASIC prompt is visible.
               This handles both DOS prompts and custom loaders that
               wait for a keypress without showing a ] prompt. */
            const bool at_keyboard_poll =
                entered_loaded_binary &&
                (machine.key_latch & 0x80U) == 0U &&
                machine.memory[cpu.pc] == 0xADU &&
                machine.memory[(uint16_t)(cpu.pc + 1U)] == 0x00U &&
                machine.memory[(uint16_t)(cpu.pc + 2U)] == 0xC0U;
            const bool at_basic_prompt = disk_screen_has_basic_prompt(&machine);

            if (at_keyboard_poll || at_basic_prompt) {
                if (!boot_command_started) {
                    boot_command_started = true;
                }

                if ((machine.key_latch & 0x80U) == 0U) {
                    const char next = boot_command[boot_command_pos];

                    if (next != '\0') {
                        apple2_machine_set_key(&machine, (uint8_t)next);
                        boot_command_pos++;
                    } else {
                        apple2_machine_set_key(&machine, '\r');
                        boot_command_finished = true;
                    }
                }
            }
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
                    "Disk boot did not reach expected text \"%s\", pc=%04x a=%02x x=%02x y=%02x sp=%02x p=%02x "
                    "qt=%u np=%u row5=%02x row6=%02x "
                    "pcbytes=%02x %02x %02x %02x %02x %02x "
                    "zp44=%02x %02x %02x %02x %02x %02x "
                    "buf=%02x %02x %02x %02x %02x %02x %02x %02x "
                    "motor=%u q6=%u q7=%u latch=%02x ready=%u\n",
                    expected_text,
                    cpu.pc,
                    cpu.a,
                    cpu.x,
                    cpu.y,
                    cpu.sp,
                    cpu.p,
                    machine.disk2.quarter_track[0],
                    machine.disk2.nibble_pos[0],
                    machine.memory[apple2_text_row_address(false, 5U)],
                    machine.memory[apple2_text_row_address(false, 6U)],
                    machine.memory[cpu.pc],
                    machine.memory[(uint16_t)(cpu.pc + 1U)],
                    machine.memory[(uint16_t)(cpu.pc + 2U)],
                    machine.memory[(uint16_t)(cpu.pc + 3U)],
                    machine.memory[(uint16_t)(cpu.pc + 4U)],
                    machine.memory[(uint16_t)(cpu.pc + 5U)],
                    machine.memory[0x44U],
                    machine.memory[0x45U],
                    machine.memory[0x46U],
                    machine.memory[0x47U],
                    machine.memory[0x48U],
                    machine.memory[0x49U],
                    machine.memory[(uint16_t)(machine.memory[0x48U] | ((uint16_t)machine.memory[0x49U] << 8))],
                    machine.memory[(uint16_t)((machine.memory[0x48U] | ((uint16_t)machine.memory[0x49U] << 8)) + 1U)],
                    machine.memory[(uint16_t)((machine.memory[0x48U] | ((uint16_t)machine.memory[0x49U] << 8)) + 2U)],
                    machine.memory[(uint16_t)((machine.memory[0x48U] | ((uint16_t)machine.memory[0x49U] << 8)) + 3U)],
                    machine.memory[(uint16_t)((machine.memory[0x48U] | ((uint16_t)machine.memory[0x49U] << 8)) + 4U)],
                    machine.memory[(uint16_t)((machine.memory[0x48U] | ((uint16_t)machine.memory[0x49U] << 8)) + 5U)],
                    machine.memory[(uint16_t)((machine.memory[0x48U] | ((uint16_t)machine.memory[0x49U] << 8)) + 6U)],
                    machine.memory[(uint16_t)((machine.memory[0x48U] | ((uint16_t)machine.memory[0x49U] << 8)) + 7U)],
                    machine.disk2.motor_on ? 1 : 0,
                    machine.disk2.q6 ? 1 : 0,
                    machine.disk2.q7 ? 1 : 0,
                    machine.disk2.data_latch,
                    machine.disk2.data_ready ? 1 : 0);
            if (disk_read_trace_active) {
                disk_dump_recent_sector_reads(&disk_read_trace);
            }
            if (trace_pcs_enabled) {
                disk_dump_recent_pcs(recent_pcs, recent_pc_count, recent_pc_next);
                disk_dump_recent_pcs(recent_app_pcs, recent_app_pc_count, recent_app_pc_next);
            }
            disk_dump_bytes(&machine, 0x03D0U, 32U);
            disk_dump_bytes(&machine, 0x07FDU, 32U);
            disk_dump_bytes(&machine, 0x9D80U, 64U);
            disk_dump_bytes(&machine, 0xB7B0U, 64U);
            disk_dump_bytes(&machine, 0xB940U, 128U);
            disk_dump_bytes(&machine, 0xBDA0U, 64U);
            if (dump_screen != NULL && dump_screen[0] != '\0') {
                disk_dump_screen(&machine);
            }
            return 14;
        }

        puts("apple2 ROM+disk custom smoke passed");
        return 0;
    }
    if (disk_size != 0U && require_not_basic_prompt) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);

        if (disk_screen_has_basic_prompt(&machine) &&
            cpu.pc >= 0xB000U &&
            cpu.pc < 0xC000U) {
            fprintf(stderr,
                    "Disk boot fell back to BASIC prompt instead of app state, pc=%04x qt=%u np=%u entered_app=%u last_app_pc=%04x\n",
                    cpu.pc,
                    machine.disk2.quarter_track[0],
                    machine.disk2.nibble_pos[0],
                    entered_loaded_binary ? 1U : 0U,
                    last_loaded_binary_pc);
            if (disk_read_trace_active) {
                disk_dump_recent_sector_reads(&disk_read_trace);
            }
            if (trace_pcs_enabled) {
                disk_dump_recent_pcs(recent_pcs, recent_pc_count, recent_pc_next);
                disk_dump_recent_pcs(recent_app_pcs, recent_app_pc_count, recent_app_pc_next);
            }
            disk_dump_bytes(&machine, 0x03D0U, 32U);
            disk_dump_bytes(&machine, 0x07FDU, 32U);
            disk_dump_bytes(&machine, 0x9D80U, 64U);
            disk_dump_bytes(&machine, 0xB7B0U, 64U);
            disk_dump_bytes(&machine, 0xB940U, 128U);
            disk_dump_bytes(&machine, 0xBDA0U, 64U);
            if (dump_screen != NULL && dump_screen[0] != '\0') {
                disk_dump_screen(&machine);
            }
            return 16;
        }

        puts("apple2 ROM+disk custom smoke passed");
        return 0;
    }
    if (disk_size != 0U && !stage1_preloaded) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        fprintf(stderr,
                "Disk boot did not preload track 0 pages, pc=%04x p3600=%02x p3700=%02x p3f00=%02x type=%d\n",
                cpu.pc, machine.memory[0x3600], machine.memory[0x3700], machine.memory[0x3F00], (int)disk_type);
        if (disk_read_trace_active) {
            disk_dump_recent_sector_reads(&disk_read_trace);
        }
        return 7;
    }
    if (disk_size != 0U && !entered_stage2) {
        const apple2_cpu_state_t cpu = apple2_machine_cpu_state(&machine);
        fprintf(stderr,
                "Disk boot did not reach the DOS stage2 entry page, pc=%04x p3700=%02x p3701=%02x p3702=%02x\n",
                cpu.pc, machine.memory[0x3700], machine.memory[0x3701], machine.memory[0x3702]);
        if (disk_read_trace_active) {
            disk_dump_recent_sector_reads(&disk_read_trace);
        }
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
        if (disk_read_trace_active) {
            disk_dump_recent_sector_reads(&disk_read_trace);
        }
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
        if (disk_read_trace_active) {
            disk_dump_recent_sector_reads(&disk_read_trace);
        }
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
            if (disk_read_trace_active) {
                disk_dump_recent_sector_reads(&disk_read_trace);
            }
            return 11;
        }
    }

    puts(disk_size != 0U ? "apple2 ROM+disk prompt/input smoke passed" : "apple2 ROM smoke passed");
    return 0;
}
