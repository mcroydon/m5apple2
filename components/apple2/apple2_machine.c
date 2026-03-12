#include "apple2/apple2_machine.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void apple2_machine_init(apple2_machine_t *machine, const apple2_config_t *config)
{
    memset(machine, 0, sizeof(*machine));
    machine->config = *config;
}

void apple2_machine_set_key(apple2_machine_t *machine, uint8_t ascii)
{
    machine->pending_key = ascii;
}

void apple2_machine_step(apple2_machine_t *machine, uint32_t cycles)
{
    machine->total_cycles += cycles;
}

const char *apple2_machine_status(const apple2_machine_t *machine)
{
    static char buffer[96];

    snprintf(buffer, sizeof(buffer),
             "Scaffold running, CPU=%" PRIu32 " Hz, cycles=%" PRIu64 ", key=%02x",
             machine->config.cpu_hz, machine->total_cycles, machine->pending_key);
    return buffer;
}
