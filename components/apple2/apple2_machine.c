#include "apple2/apple2_machine.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define APPLE2_ROM_BASE 0xD000U
#define APPLE2_ROM_SIZE 0x3000U
#define APPLE2_PLUS_ROM_BASE 0xB000U
#define APPLE2_PLUS_ROM_SIZE 0x5000U
#define APPLE2_SLOT6_ROM_BASE 0xC600U
#define APPLE2_SLOT6_ROM_SIZE 0x0100U

static uint8_t apple2_bus_read(void *context, uint16_t address)
{
    apple2_machine_t *machine = context;

    switch (address) {
    case 0xC000:
        return machine->key_latch;
    case 0xC010:
        machine->key_latch &= 0x7FU;
        return machine->key_latch;
    case 0xC030:
        machine->speaker_toggles++;
        return 0;
    case 0xC050:
        machine->video.text_mode = false;
        return 0;
    case 0xC051:
        machine->video.text_mode = true;
        return 0;
    case 0xC052:
        machine->video.mixed_mode = false;
        return 0;
    case 0xC053:
        machine->video.mixed_mode = true;
        return 0;
    case 0xC054:
        machine->video.page2 = false;
        return 0;
    case 0xC055:
        machine->video.page2 = true;
        return 0;
    case 0xC056:
        machine->video.hires_mode = false;
        return 0;
    case 0xC057:
        machine->video.hires_mode = true;
        return 0;
    default:
        break;
    }

    if (address >= 0xC0E0U && address <= 0xC0EFU) {
        return apple2_disk2_access(&machine->disk2, (uint8_t)(address & 0x0FU));
    }

    if ((address >= 0xC100U && address < APPLE2_SLOT6_ROM_BASE) ||
        (address >= 0xC700U && address < APPLE2_ROM_BASE)) {
        return 0x00;
    }

    return machine->memory[address];
}

static void apple2_bus_write(void *context, uint16_t address, uint8_t value)
{
    apple2_machine_t *machine = context;

    switch (address) {
    case 0xC010:
        machine->key_latch &= 0x7FU;
        return;
    case 0xC030:
        machine->speaker_toggles++;
        return;
    case 0xC050:
        machine->video.text_mode = false;
        return;
    case 0xC051:
        machine->video.text_mode = true;
        return;
    case 0xC052:
        machine->video.mixed_mode = false;
        return;
    case 0xC053:
        machine->video.mixed_mode = true;
        return;
    case 0xC054:
        machine->video.page2 = false;
        return;
    case 0xC055:
        machine->video.page2 = true;
        return;
    case 0xC056:
        machine->video.hires_mode = false;
        return;
    case 0xC057:
        machine->video.hires_mode = true;
        return;
    default:
        break;
    }

    if (address >= 0xC0E0U && address <= 0xC0EFU) {
        (void)apple2_disk2_access(&machine->disk2, (uint8_t)(address & 0x0FU));
        return;
    }

    if (address >= APPLE2_ROM_BASE) {
        return;
    }
    if (address >= APPLE2_SLOT6_ROM_BASE && address < (APPLE2_SLOT6_ROM_BASE + APPLE2_SLOT6_ROM_SIZE)) {
        return;
    }
    machine->memory[address] = value;
}

void apple2_machine_init(apple2_machine_t *machine, const apple2_config_t *config)
{
    cpu6502_bus_t bus = {
        .context = machine,
        .read = apple2_bus_read,
        .write = apple2_bus_write,
    };

    memset(machine, 0, sizeof(*machine));
    machine->config = *config;
    machine->video.text_mode = true;
    apple2_disk2_init(&machine->disk2);
    cpu6502_init(&machine->cpu, &bus);
    apple2_machine_reset(machine);
}

void apple2_machine_reset(apple2_machine_t *machine)
{
    machine->key_latch = 0;
    machine->speaker_toggles = 0;
    machine->video.text_mode = true;
    machine->video.mixed_mode = false;
    machine->video.page2 = false;
    machine->video.hires_mode = false;
    machine->video.flash_state = false;
    apple2_disk2_reset(&machine->disk2);
    cpu6502_reset(&machine->cpu);
    machine->total_cycles = machine->cpu.total_cycles;
}

bool apple2_machine_load_system_rom(apple2_machine_t *machine, const uint8_t *rom, size_t rom_size)
{
    if (rom_size != APPLE2_ROM_SIZE && rom_size != 0x4000U && rom_size != APPLE2_PLUS_ROM_SIZE) {
        return false;
    }

    if (rom_size == APPLE2_ROM_SIZE) {
        memcpy(&machine->memory[APPLE2_ROM_BASE], rom, APPLE2_ROM_SIZE);
        machine->slot6_rom_loaded = false;
    } else if (rom_size == APPLE2_PLUS_ROM_SIZE) {
        memcpy(&machine->memory[APPLE2_PLUS_ROM_BASE], rom, APPLE2_PLUS_ROM_SIZE);
        machine->slot6_rom_loaded = true;
    } else {
        memcpy(&machine->memory[0xC000], rom, 0x4000U);
        machine->slot6_rom_loaded = true;
    }

    machine->system_rom_loaded = true;
    cpu6502_reset(&machine->cpu);
    machine->total_cycles = machine->cpu.total_cycles;
    return true;
}

bool apple2_machine_load_slot6_rom(apple2_machine_t *machine, const uint8_t *rom, size_t rom_size)
{
    if (rom_size != APPLE2_SLOT6_ROM_SIZE) {
        return false;
    }

    memcpy(&machine->memory[APPLE2_SLOT6_ROM_BASE], rom, APPLE2_SLOT6_ROM_SIZE);
    machine->slot6_rom_loaded = true;
    return true;
}

bool apple2_machine_load_drive0_dsk(apple2_machine_t *machine, const uint8_t *image, size_t image_size)
{
    return apple2_disk2_load_drive_with_order(&machine->disk2,
                                              0,
                                              image,
                                              image_size,
                                              APPLE2_DISK2_IMAGE_ORDER_DSK_PHYSICAL);
}

bool apple2_machine_load_drive0_do(apple2_machine_t *machine, const uint8_t *image, size_t image_size)
{
    return apple2_disk2_load_drive_with_order(&machine->disk2,
                                              0,
                                              image,
                                              image_size,
                                              APPLE2_DISK2_IMAGE_ORDER_DO_LOGICAL);
}

void apple2_machine_set_key(apple2_machine_t *machine, uint8_t ascii)
{
    if (ascii >= 'a' && ascii <= 'z') {
        ascii = (uint8_t)(ascii - 'a' + 'A');
    }
    machine->key_latch = (uint8_t)(ascii | 0x80U);
}

uint32_t apple2_machine_step_instruction(apple2_machine_t *machine)
{
    const uint32_t cycles = cpu6502_step(&machine->cpu);
    machine->total_cycles = machine->cpu.total_cycles;
    if (machine->config.cpu_hz >= 4U) {
        const uint64_t flash_ticks = machine->total_cycles / (machine->config.cpu_hz / 4U);
        machine->video.flash_state = (flash_ticks & 1U) != 0U;
    }
    return cycles;
}

void apple2_machine_step(apple2_machine_t *machine, uint32_t cycles)
{
    uint32_t elapsed = 0;
    while (elapsed < cycles) {
        elapsed += apple2_machine_step_instruction(machine);
    }
}

void apple2_machine_render(const apple2_machine_t *machine, uint8_t *pixels)
{
    apple2_video_render(machine->memory, &machine->video, pixels);
}

uint8_t apple2_machine_peek(const apple2_machine_t *machine, uint16_t address)
{
    return machine->memory[address];
}

void apple2_machine_poke(apple2_machine_t *machine, uint16_t address, uint8_t value)
{
    apple2_bus_write(machine, address, value);
}

void apple2_machine_set_pc(apple2_machine_t *machine, uint16_t address)
{
    cpu6502_set_pc(&machine->cpu, address);
}

apple2_cpu_state_t apple2_machine_cpu_state(const apple2_machine_t *machine)
{
    return (apple2_cpu_state_t){
        .a = machine->cpu.a,
        .x = machine->cpu.x,
        .y = machine->cpu.y,
        .sp = machine->cpu.sp,
        .p = machine->cpu.p,
        .pc = machine->cpu.pc,
        .total_cycles = machine->cpu.total_cycles,
    };
}

const char *apple2_machine_status(const apple2_machine_t *machine)
{
    static char buffer[128];

    snprintf(buffer, sizeof(buffer),
             "ROM:%s slot6:%s d0:%s PC=%04x A=%02x X=%02x Y=%02x P=%02x cycles=%" PRIu64,
             machine->system_rom_loaded ? "yes" : "no",
             machine->slot6_rom_loaded ? "yes" : "no",
             apple2_disk2_drive_loaded(&machine->disk2, 0) ? "yes" : "no",
             machine->cpu.pc, machine->cpu.a, machine->cpu.x, machine->cpu.y,
             machine->cpu.p, machine->total_cycles);
    return buffer;
}
