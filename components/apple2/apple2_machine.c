#include "apple2/apple2_machine.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
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

static inline uint8_t *apple2_langcard_bank(apple2_machine_t *machine)
{
    if (machine->langcard_ram == NULL) {
        return NULL;
    }
    return machine->langcard_bank1_active ? machine->langcard_ram
                                          : &machine->langcard_ram[APPLE2_LANGCARD_BANK_SIZE];
}

static inline uint8_t *apple2_langcard_upper(apple2_machine_t *machine)
{
    return (machine->langcard_ram == NULL) ? NULL : &machine->langcard_ram[2U * APPLE2_LANGCARD_BANK_SIZE];
}

static bool apple2_langcard_ensure_storage(apple2_machine_t *machine)
{
    if (machine->langcard_ram != NULL) {
        return true;
    }

    machine->langcard_ram = calloc(1U, APPLE2_LANGCARD_RAM_SIZE);
    return machine->langcard_ram != NULL;
}

#if !defined(ESP_PLATFORM)
static bool apple2_langcard_trace_enabled(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char *enabled = getenv("APPLE2_TRACE_LANGCARD");

        cached = (enabled != NULL && enabled[0] != '\0') ? 1 : 0;
    }

    return cached != 0;
}

static void apple2_langcard_trace_access(const apple2_machine_t *machine,
                                         uint16_t address,
                                         bool write_cycle)
{
    if (!apple2_langcard_trace_enabled()) {
        return;
    }

    fprintf(stderr,
            "LC %c %04x bank=%u read=%u write=%u count=%u pc=%04x a=%02x x=%02x y=%02x p=%02x\n",
            write_cycle ? 'W' : 'R',
            address,
            machine->langcard_bank1_active ? 1U : 2U,
            machine->langcard_read_enabled ? 1U : 0U,
            machine->langcard_write_enabled ? 1U : 0U,
            machine->langcard_write_count,
            machine->cpu.pc,
            machine->cpu.a,
            machine->cpu.x,
            machine->cpu.y,
            machine->cpu.p);
}

static void apple2_langcard_trace_write(const apple2_machine_t *machine, uint16_t address, uint8_t value)
{
    if (!apple2_langcard_trace_enabled()) {
        return;
    }

    fprintf(stderr,
            "LC RAM %04x=%02x bank=%u read=%u write=%u pc=%04x\n",
            address,
            value,
            machine->langcard_bank1_active ? 1U : 2U,
            machine->langcard_read_enabled ? 1U : 0U,
            machine->langcard_write_enabled ? 1U : 0U,
            machine->cpu.pc);
}

static bool apple2_langcard_disabled(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char *disabled = getenv("APPLE2_DISABLE_LANGCARD");

        cached = (disabled != NULL && disabled[0] != '\0') ? 1 : 0;
    }

    return cached != 0;
}

static bool apple2_disk_io_trace_enabled(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char *enabled = getenv("APPLE2_TRACE_DISK_IO");

        cached = (enabled != NULL && enabled[0] != '\0') ? 1 : 0;
    }

    return cached != 0;
}

static void apple2_disk_io_trace(const apple2_machine_t *machine,
                                 uint16_t address,
                                 bool write_cycle,
                                 uint8_t value)
{
    if (!apple2_disk_io_trace_enabled()) {
        return;
    }

    fprintf(stderr,
            "DISK %c %04x=%02x pc=%04x drive=%u qt=%u np=%" PRIu32 " q6=%u q7=%u ready=%u latch=%02x motor=%u\n",
            write_cycle ? 'W' : 'R',
            address,
            value,
            machine->cpu.pc,
            machine->disk2.active_drive,
            machine->disk2.quarter_track[machine->disk2.active_drive],
            machine->disk2.nibble_pos[machine->disk2.active_drive],
            machine->disk2.q6 ? 1U : 0U,
            machine->disk2.q7 ? 1U : 0U,
            machine->disk2.data_ready ? 1U : 0U,
            machine->disk2.data_latch,
            machine->disk2.motor_on ? 1U : 0U);
}
#else
static bool apple2_langcard_trace_enabled(void)
{
    return false;
}

static void apple2_langcard_trace_access(const apple2_machine_t *machine,
                                         uint16_t address,
                                         bool write_cycle)
{
    (void)machine;
    (void)address;
    (void)write_cycle;
}

static void apple2_langcard_trace_write(const apple2_machine_t *machine, uint16_t address, uint8_t value)
{
    (void)machine;
    (void)address;
    (void)value;
}

static bool apple2_langcard_disabled(void)
{
    return false;
}

static void apple2_disk_io_trace(const apple2_machine_t *machine,
                                 uint16_t address,
                                 bool write_cycle,
                                 uint8_t value)
{
    (void)machine;
    (void)address;
    (void)write_cycle;
    (void)value;
}
#endif

static void apple2_langcard_sync_memory(apple2_machine_t *machine)
{
    if (!machine->system_rom_loaded || machine->langcard_rom == NULL) {
        return;
    }

    if (machine->langcard_read_enabled) {
        uint8_t *bank = apple2_langcard_bank(machine);
        uint8_t *upper = apple2_langcard_upper(machine);

        if (bank == NULL || upper == NULL) {
            return;
        }
        memcpy(&machine->memory[APPLE2_ROM_BASE],
               bank,
               APPLE2_LANGCARD_BANK_SIZE);
        memcpy(&machine->memory[APPLE2_ROM_BASE + APPLE2_LANGCARD_BANK_SIZE],
               upper,
               APPLE2_LANGCARD_UPPER_SIZE);
        return;
    }

    memcpy(&machine->memory[APPLE2_ROM_BASE], machine->langcard_rom, APPLE2_LANGCARD_ROM_SIZE);
}

static void apple2_langcard_reset(apple2_machine_t *machine)
{
    const bool was_ram_visible = machine->langcard_read_enabled;

    machine->langcard_bank1_active = false;
    machine->langcard_read_enabled = false;
    machine->langcard_write_enabled = false;
    machine->langcard_write_count = 0U;
    if (was_ram_visible) {
        apple2_langcard_sync_memory(machine);
    }
}

static void apple2_langcard_access(apple2_machine_t *machine, uint16_t address, bool write_cycle)
{
    if (apple2_langcard_disabled()) {
        return;
    }
    if (!apple2_langcard_ensure_storage(machine)) {
        machine->langcard_bank1_active = false;
        machine->langcard_read_enabled = false;
        machine->langcard_write_enabled = false;
        machine->langcard_write_count = 0U;
        return;
    }

    const uint8_t soft_switch = (uint8_t)(address & 0x0FU);
    const uint8_t action = (uint8_t)(soft_switch & 0x03U);

    machine->langcard_bank1_active = (soft_switch & 0x08U) != 0U;
    machine->langcard_read_enabled = action == 0U || action == 0x03U;

    if ((soft_switch & 0x01U) != 0U) {
        if (write_cycle) {
            machine->langcard_write_count = 0U;
            machine->langcard_write_enabled = false;
        } else {
            if (machine->langcard_write_count < 2U) {
                machine->langcard_write_count++;
            }
            machine->langcard_write_enabled = machine->langcard_write_count >= 2U;
        }
    } else {
        machine->langcard_write_count = 0U;
        machine->langcard_write_enabled = false;
    }

    apple2_langcard_sync_memory(machine);
    apple2_langcard_trace_access(machine, address, write_cycle);
}

static void apple2_langcard_write(apple2_machine_t *machine, uint16_t address, uint8_t value)
{
    if (apple2_langcard_disabled()) {
        return;
    }
    if (!apple2_langcard_ensure_storage(machine)) {
        return;
    }

    if (address < APPLE2_ROM_BASE) {
        return;
    }

    if (address < (APPLE2_ROM_BASE + APPLE2_LANGCARD_BANK_SIZE)) {
        uint8_t *bank = apple2_langcard_bank(machine);

        if (bank == NULL) {
            return;
        }
        bank[address - APPLE2_ROM_BASE] = value;
    } else {
        uint8_t *upper = apple2_langcard_upper(machine);

        if (upper == NULL) {
            return;
        }
        upper[address - (APPLE2_ROM_BASE + APPLE2_LANGCARD_BANK_SIZE)] = value;
    }

    if (machine->langcard_read_enabled) {
        machine->memory[address] = value;
    }

    apple2_langcard_trace_write(machine, address, value);
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

    machine->flash_cycle_accum -= machine->flash_half_period_cycles;
    machine->video.flash_state = !machine->video.flash_state;
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

    /* Fast path: ~99% of reads are to RAM below the I/O space. */
    if (address < 0xC000U) {
        machine->floating_bus = machine->memory[address];
        return machine->memory[address];
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

    if (address <= 0xC00FU) {
        return apple2_bus_value(machine, machine->key_latch);
    }
    if (address >= 0xC010U && address <= 0xC01FU) {
        machine->key_latch &= 0x7FU;
        return apple2_bus_value(machine, machine->key_latch);
    }

    switch (address) {
    case 0xC030:
        machine->speaker_toggles++;
        if (machine->speaker_toggle_fn != NULL) {
            machine->speaker_toggle_fn(machine->speaker_toggle_context,
                                        machine->total_cycles);
        }
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
    case 0xC061:
    case 0xC062:
    case 0xC063:
        return apple2_bus_value(machine, 0x00U); /* Push buttons: not pressed. */
    case 0xC064:
    case 0xC065:
    case 0xC066:
    case 0xC067:
        return apple2_bus_value(machine, 0x00U); /* Paddle timers: expired. */
    default:
        break;
    }

    if (address >= 0xC080U && address <= 0xC08FU) {
        apple2_langcard_access(machine, address, false);
        return apple2_bus_value(machine, machine->floating_bus);
    }

    if (address >= 0xC0E0U && address <= 0xC0EFU) {
        const uint8_t value = apple2_disk2_access(&machine->disk2, (uint8_t)(address & 0x0FU));

        apple2_disk_io_trace(machine, address, false, value);
        return apple2_bus_value(machine, value);
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
    /* Fast path: most writes are to RAM below the I/O space. */
    if (address < 0xC000U) {
        machine->memory[address] = value;
        return;
    }

    machine->floating_bus = value;

    if (address <= 0xC00FU) {
        return;
    }
    if (address >= 0xC010U && address <= 0xC01FU) {
        machine->key_latch &= 0x7FU;
        return;
    }

    switch (address) {
    case 0xC030:
        machine->speaker_toggles++;
        if (machine->speaker_toggle_fn != NULL) {
            machine->speaker_toggle_fn(machine->speaker_toggle_context,
                                        machine->total_cycles);
        }
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

    if (address >= 0xC080U && address <= 0xC08FU) {
        apple2_langcard_access(machine, address, true);
        return;
    }

    if (address >= 0xC0E0U && address <= 0xC0EFU) {
        if ((address & 0x0FU) == 0x0DU && machine->disk2.q7) {
            machine->disk2.data_latch = value;
        }
        const uint8_t ignored = apple2_disk2_access(&machine->disk2, (uint8_t)(address & 0x0FU));

        apple2_disk_io_trace(machine, address, true, ignored);
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

    if (address >= APPLE2_ROM_BASE && machine->langcard_write_enabled) {
        apple2_langcard_write(machine, address, value);
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
        .memory = machine->memory,
        .data_latch = &machine->floating_bus,
        .disk2 = &machine->disk2,
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
    apple2_langcard_reset(machine);
    apple2_disk2_reset(&machine->disk2);
    cpu6502_reset(&machine->cpu);
    machine->total_cycles = machine->cpu.total_cycles;
}

bool apple2_machine_load_system_rom(apple2_machine_t *machine, const uint8_t *rom, size_t rom_size)
{
    const uint8_t *langcard_rom = NULL;

    if (rom_size != APPLE2_ROM_SIZE && rom_size != 0x4000U && rom_size != APPLE2_PLUS_ROM_SIZE) {
        return false;
    }

    if (rom_size == APPLE2_ROM_SIZE) {
        memcpy(&machine->memory[APPLE2_ROM_BASE], rom, APPLE2_ROM_SIZE);
        machine->system_rom_base = APPLE2_ROM_BASE;
        machine->system_rom_size = APPLE2_ROM_SIZE;
        machine->motherboard_rom_loaded = false;
        machine->slot6_rom_loaded = false;
        langcard_rom = rom;
    } else if (rom_size == APPLE2_PLUS_ROM_SIZE) {
        memcpy(&machine->memory[0xC000], &rom[0xC000U - APPLE2_FULL_DUMP_BASE], 0x4000U);
        machine->system_rom_base = APPLE2_ROM_BASE;
        machine->system_rom_size = APPLE2_ROM_SIZE;
        machine->motherboard_rom_loaded = true;
        machine->slot6_rom_loaded = true;
        langcard_rom = &rom[APPLE2_ROM_BASE - APPLE2_FULL_DUMP_BASE];
    } else {
        memcpy(&machine->memory[0xC000], rom, 0x4000U);
        machine->system_rom_base = APPLE2_ROM_BASE;
        machine->system_rom_size = APPLE2_ROM_SIZE;
        machine->motherboard_rom_loaded = true;
        machine->slot6_rom_loaded = true;
        langcard_rom = &rom[APPLE2_ROM_BASE - 0xC000U];
    }

    machine->langcard_rom = langcard_rom;
    machine->system_rom_loaded = true;
    apple2_langcard_reset(machine);
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

void apple2_machine_set_speaker_callback(apple2_machine_t *machine,
                                          apple2_speaker_toggle_fn fn,
                                          void *context)
{
    machine->speaker_toggle_fn = fn;
    machine->speaker_toggle_context = context;
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
