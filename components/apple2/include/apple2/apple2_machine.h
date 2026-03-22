#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apple2/apple2_disk2.h"
#include "apple2/apple2_video.h"
#include "apple2/cpu6502.h"

#define APPLE2_VIDEO_WIDTH 280
#define APPLE2_VIDEO_HEIGHT 192
#define APPLE2_LANGCARD_BANK_SIZE 0x1000U
#define APPLE2_LANGCARD_UPPER_SIZE 0x2000U
#define APPLE2_LANGCARD_RAM_SIZE 0x4000U
#define APPLE2_LANGCARD_ROM_SIZE 0x3000U

typedef struct {
    uint32_t cpu_hz;
} apple2_config_t;

typedef struct {
    uint8_t a;
    uint8_t x;
    uint8_t y;
    uint8_t sp;
    uint8_t p;
    uint16_t pc;
    uint64_t total_cycles;
} apple2_cpu_state_t;

typedef struct {
    apple2_config_t config;
    cpu6502_t cpu;
    uint64_t total_cycles;
    uint8_t memory[65536];
    uint8_t *langcard_ram;
    const uint8_t *langcard_rom;
    uint8_t key_latch;
    uint8_t floating_bus;
    uint8_t annunciator_state;
    uint8_t c8_slot;
    uint8_t langcard_write_count;
    uint32_t speaker_toggles;
    apple2_video_state_t video;
    apple2_disk2_t disk2;
    uint16_t system_rom_base;
    size_t system_rom_size;
    uint32_t flash_half_period_cycles;
    uint32_t flash_cycle_accum;
    bool langcard_bank1_active;
    bool langcard_read_enabled;
    bool langcard_write_enabled;
    bool motherboard_rom_loaded;
    bool system_rom_loaded;
    bool slot6_rom_loaded;
} apple2_machine_t;

void apple2_machine_init(apple2_machine_t *machine, const apple2_config_t *config);
void apple2_machine_reset(apple2_machine_t *machine);
bool apple2_machine_load_system_rom(apple2_machine_t *machine, const uint8_t *rom, size_t rom_size);
bool apple2_machine_load_slot6_rom(apple2_machine_t *machine, const uint8_t *rom, size_t rom_size);
bool apple2_machine_load_drive0_dsk(apple2_machine_t *machine, const uint8_t *image, size_t image_size);
bool apple2_machine_load_drive0_do(apple2_machine_t *machine, const uint8_t *image, size_t image_size);
bool apple2_machine_load_drive0_po(apple2_machine_t *machine, const uint8_t *image, size_t image_size);
bool apple2_machine_load_drive0_nib(apple2_machine_t *machine, const uint8_t *image, size_t image_size);
bool apple2_machine_load_drive1_dsk(apple2_machine_t *machine, const uint8_t *image, size_t image_size);
bool apple2_machine_load_drive1_do(apple2_machine_t *machine, const uint8_t *image, size_t image_size);
bool apple2_machine_load_drive1_po(apple2_machine_t *machine, const uint8_t *image, size_t image_size);
bool apple2_machine_load_drive1_nib(apple2_machine_t *machine, const uint8_t *image, size_t image_size);
void apple2_machine_set_key(apple2_machine_t *machine, uint8_t ascii);
void apple2_machine_step(apple2_machine_t *machine, uint32_t cycles);
uint32_t apple2_machine_step_instruction(apple2_machine_t *machine);
void apple2_machine_render(const apple2_machine_t *machine, uint8_t *pixels);
uint8_t apple2_machine_peek(const apple2_machine_t *machine, uint16_t address);
void apple2_machine_poke(apple2_machine_t *machine, uint16_t address, uint8_t value);
void apple2_machine_set_pc(apple2_machine_t *machine, uint16_t address);
apple2_cpu_state_t apple2_machine_cpu_state(const apple2_machine_t *machine);
const char *apple2_machine_status(const apple2_machine_t *machine);
