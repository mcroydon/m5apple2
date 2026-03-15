#include "apple2/apple2_machine.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define APPLE2_ROM_BASE 0xD000U
#define APPLE2_ROM_SIZE 0x3000U
#define APPLE2_FULL_DUMP_BASE 0xB000U
#define APPLE2_PLUS_ROM_SIZE 0x5000U
#define APPLE2_BOARD_ROM_BASE 0xC800U
#define APPLE2_BOARD_ROM_SIZE 0x0800U
#define APPLE2_SLOT6_ROM_BASE 0xC600U
#define APPLE2_SLOT6_ROM_SIZE 0x0100U

static bool apple2_is_system_rom_address(const apple2_machine_t *machine, uint16_t address)
{
    const uint32_t end = (uint32_t)machine->system_rom_base + (uint32_t)machine->system_rom_size;

    if (!machine->system_rom_loaded || machine->system_rom_size == 0U) {
        return false;
    }

    return (uint32_t)address >= (uint32_t)machine->system_rom_base &&
           (uint32_t)address < end;
}

static bool apple2_is_board_rom_address(const apple2_machine_t *machine, uint16_t address)
{
    return machine->motherboard_rom_loaded &&
           address >= APPLE2_BOARD_ROM_BASE &&
           address < (APPLE2_BOARD_ROM_BASE + APPLE2_BOARD_ROM_SIZE);
}

static inline void apple2_machine_tick_flash(apple2_machine_t *machine, uint32_t cycles)
{
    if (machine->flash_half_period_cycles == 0U || cycles == 0U) {
        return;
    }

    machine->flash_cycle_accum += cycles;
    if (machine->flash_cycle_accum < machine->flash_half_period_cycles) {
        return;
    }

    while (machine->flash_cycle_accum >= machine->flash_half_period_cycles) {
        machine->flash_cycle_accum -= machine->flash_half_period_cycles;
        machine->video.flash_state = !machine->video.flash_state;
    }
}

static inline uint8_t apple2_bus_value(apple2_machine_t *machine, uint8_t value)
{
    machine->floating_bus = value;
    return value;
}

static inline void apple2_select_slot_c8(apple2_machine_t *machine, uint16_t address)
{
    if (address >= 0xC100U && address < 0xC800U) {
        machine->c8_slot = (uint8_t)((address >> 8) & 0x07U);
    }
}

static uint8_t apple2_bus_read(void *context, uint16_t address)
{
    apple2_machine_t *machine = context;

    if (address < 0xC000U) {
        return apple2_bus_value(machine, machine->memory[address]);
    }
    if (address >= APPLE2_ROM_BASE) {
        return apple2_bus_value(machine, machine->memory[address]);
    }
    if (address >= 0xC800U) {
        if (address == 0xCFFFU) {
            machine->c8_slot = 0U;
        }
        return apple2_bus_value(machine, machine->memory[address]);
    }

    switch (address) {
    case 0xC000:
        return apple2_bus_value(machine, machine->key_latch);
    case 0xC010:
        machine->key_latch &= 0x7FU;
        return apple2_bus_value(machine, machine->key_latch);
    case 0xC030:
        machine->speaker_toggles++;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC050:
        machine->video.text_mode = false;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC051:
        machine->video.text_mode = true;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC052:
        machine->video.mixed_mode = false;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC053:
        machine->video.mixed_mode = true;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC054:
        machine->video.page2 = false;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC055:
        machine->video.page2 = true;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC056:
        machine->video.hires_mode = false;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC057:
        machine->video.hires_mode = true;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC058:
        machine->annunciator_state &= (uint8_t)~0x01U;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC059:
        machine->annunciator_state |= 0x01U;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC05A:
        machine->annunciator_state &= (uint8_t)~0x02U;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC05B:
        machine->annunciator_state |= 0x02U;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC05C:
        machine->annunciator_state &= (uint8_t)~0x04U;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC05D:
        machine->annunciator_state |= 0x04U;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC05E:
        machine->annunciator_state &= (uint8_t)~0x08U;
        return apple2_bus_value(machine, machine->floating_bus);
    case 0xC05F:
        machine->annunciator_state |= 0x08U;
        return apple2_bus_value(machine, machine->floating_bus);
    default:
        break;
    }

    if (address >= 0xC0E0U && address <= 0xC0EFU) {
        return apple2_bus_value(machine, apple2_disk2_access(&machine->disk2, (uint8_t)(address & 0x0FU)));
    }

    if (address >= 0xC100U && address < 0xC800U) {
        apple2_select_slot_c8(machine, address);
        if (address >= APPLE2_SLOT6_ROM_BASE &&
            address < (APPLE2_SLOT6_ROM_BASE + APPLE2_SLOT6_ROM_SIZE) &&
            machine->slot6_rom_loaded) {
            return apple2_bus_value(machine, machine->memory[address]);
        }
        return apple2_bus_value(machine, machine->floating_bus);
    }

    return apple2_bus_value(machine, machine->memory[address]);
}

static void apple2_bus_write(void *context, uint16_t address, uint8_t value)
{
    apple2_machine_t *machine = context;
    machine->floating_bus = value;

    if (address < 0xC000U) {
        machine->memory[address] = value;
        return;
    }

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
    case 0xC058:
        machine->annunciator_state &= (uint8_t)~0x01U;
        return;
    case 0xC059:
        machine->annunciator_state |= 0x01U;
        return;
    case 0xC05A:
        machine->annunciator_state &= (uint8_t)~0x02U;
        return;
    case 0xC05B:
        machine->annunciator_state |= 0x02U;
        return;
    case 0xC05C:
        machine->annunciator_state &= (uint8_t)~0x04U;
        return;
    case 0xC05D:
        machine->annunciator_state |= 0x04U;
        return;
    case 0xC05E:
        machine->annunciator_state &= (uint8_t)~0x08U;
        return;
    case 0xC05F:
        machine->annunciator_state |= 0x08U;
        return;
    default:
        break;
    }

    if (address >= 0xC0E0U && address <= 0xC0EFU) {
        (void)apple2_disk2_access(&machine->disk2, (uint8_t)(address & 0x0FU));
        return;
    }

    if (address >= 0xC100U && address < 0xC800U) {
        apple2_select_slot_c8(machine, address);
        return;
    }

    if (address == 0xCFFFU) {
        machine->c8_slot = 0U;
        return;
    }

    if ((address >= APPLE2_SLOT6_ROM_BASE && address < (APPLE2_SLOT6_ROM_BASE + APPLE2_SLOT6_ROM_SIZE)) ||
        apple2_is_board_rom_address(machine, address) ||
        apple2_is_system_rom_address(machine, address)) {
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
    machine->floating_bus = 0;
    machine->annunciator_state = 0;
    machine->c8_slot = 0;
    machine->speaker_toggles = 0;
    machine->video.text_mode = true;
    machine->video.mixed_mode = false;
    machine->video.page2 = false;
    machine->video.hires_mode = false;
    machine->video.flash_state = false;
    machine->flash_half_period_cycles = machine->config.cpu_hz / 4U;
    if (machine->flash_half_period_cycles == 0U) {
        machine->flash_half_period_cycles = 1U;
    }
    machine->flash_cycle_accum = 0U;
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
        machine->system_rom_base = APPLE2_ROM_BASE;
        machine->system_rom_size = APPLE2_ROM_SIZE;
        machine->motherboard_rom_loaded = false;
        machine->slot6_rom_loaded = false;
    } else if (rom_size == APPLE2_PLUS_ROM_SIZE) {
        memcpy(&machine->memory[0xC000], &rom[0xC000U - APPLE2_FULL_DUMP_BASE], 0x4000U);
        machine->system_rom_base = APPLE2_ROM_BASE;
        machine->system_rom_size = APPLE2_ROM_SIZE;
        machine->motherboard_rom_loaded = true;
        machine->slot6_rom_loaded = true;
    } else {
        memcpy(&machine->memory[0xC000], rom, 0x4000U);
        machine->system_rom_base = APPLE2_ROM_BASE;
        machine->system_rom_size = APPLE2_ROM_SIZE;
        machine->motherboard_rom_loaded = true;
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

static bool apple2_machine_load_drive_with_order(apple2_machine_t *machine,
                                                 unsigned drive_index,
                                                 const uint8_t *image,
                                                 size_t image_size,
                                                 apple2_disk2_image_order_t image_order)
{
    return apple2_disk2_load_drive_with_order(&machine->disk2,
                                              drive_index,
                                              image,
                                              image_size,
                                              image_order);
}

bool apple2_machine_load_drive0_dsk(apple2_machine_t *machine, const uint8_t *image, size_t image_size)
{
    return apple2_machine_load_drive_with_order(machine,
                                                0,
                                                image,
                                                image_size,
                                                APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL);
}

bool apple2_machine_load_drive0_do(apple2_machine_t *machine, const uint8_t *image, size_t image_size)
{
    return apple2_machine_load_drive_with_order(machine,
                                                0,
                                                image,
                                                image_size,
                                                APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL);
}

bool apple2_machine_load_drive0_po(apple2_machine_t *machine, const uint8_t *image, size_t image_size)
{
    return apple2_machine_load_drive_with_order(machine,
                                                0,
                                                image,
                                                image_size,
                                                APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL);
}

bool apple2_machine_load_drive0_nib(apple2_machine_t *machine, const uint8_t *image, size_t image_size)
{
    return apple2_disk2_load_nib_drive(&machine->disk2, 0, image, image_size);
}

bool apple2_machine_load_drive1_dsk(apple2_machine_t *machine, const uint8_t *image, size_t image_size)
{
    return apple2_machine_load_drive_with_order(machine,
                                                1,
                                                image,
                                                image_size,
                                                APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL);
}

bool apple2_machine_load_drive1_do(apple2_machine_t *machine, const uint8_t *image, size_t image_size)
{
    return apple2_machine_load_drive_with_order(machine,
                                                1,
                                                image,
                                                image_size,
                                                APPLE2_DISK2_IMAGE_ORDER_DOS33_LOGICAL);
}

bool apple2_machine_load_drive1_po(apple2_machine_t *machine, const uint8_t *image, size_t image_size)
{
    return apple2_machine_load_drive_with_order(machine,
                                                1,
                                                image,
                                                image_size,
                                                APPLE2_DISK2_IMAGE_ORDER_PRODOS_LOGICAL);
}

bool apple2_machine_load_drive1_nib(apple2_machine_t *machine, const uint8_t *image, size_t image_size)
{
    return apple2_disk2_load_nib_drive(&machine->disk2, 1, image, image_size);
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
    machine->instruction_count++;
    machine->total_cycles = machine->cpu.total_cycles;
    apple2_disk2_tick(&machine->disk2, machine->config.cpu_hz, cycles);
    apple2_machine_tick_flash(machine, cycles);
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
             "ROM:%s slot6:%s d0:%s d1:%s PC=%04x A=%02x X=%02x Y=%02x P=%02x cycles=%" PRIu64,
             machine->system_rom_loaded ? "yes" : "no",
             machine->slot6_rom_loaded ? "yes" : "no",
             apple2_disk2_drive_loaded(&machine->disk2, 0) ? "yes" : "no",
             apple2_disk2_drive_loaded(&machine->disk2, 1) ? "yes" : "no",
             machine->cpu.pc, machine->cpu.a, machine->cpu.x, machine->cpu.y,
             machine->cpu.p, machine->total_cycles);
    return buffer;
}
