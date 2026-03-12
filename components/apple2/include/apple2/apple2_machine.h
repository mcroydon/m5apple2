#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLE2_VIDEO_WIDTH 280
#define APPLE2_VIDEO_HEIGHT 192

typedef struct {
    uint32_t cpu_hz;
} apple2_config_t;

typedef struct {
    apple2_config_t config;
    uint64_t total_cycles;
    uint8_t pending_key;
} apple2_machine_t;

void apple2_machine_init(apple2_machine_t *machine, const apple2_config_t *config);
void apple2_machine_set_key(apple2_machine_t *machine, uint8_t ascii);
void apple2_machine_step(apple2_machine_t *machine, uint32_t cycles);
const char *apple2_machine_status(const apple2_machine_t *machine);

