#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    CPU6502_FLAG_CARRY = 0x01,
    CPU6502_FLAG_ZERO = 0x02,
    CPU6502_FLAG_IRQ_DISABLE = 0x04,
    CPU6502_FLAG_DECIMAL = 0x08,
    CPU6502_FLAG_BREAK = 0x10,
    CPU6502_FLAG_UNUSED = 0x20,
    CPU6502_FLAG_OVERFLOW = 0x40,
    CPU6502_FLAG_NEGATIVE = 0x80,
};

typedef uint8_t (*cpu6502_read_fn)(void *context, uint16_t address);
typedef void (*cpu6502_write_fn)(void *context, uint16_t address, uint8_t value);

typedef struct {
    void *context;
    uint8_t *memory;
    uint8_t *data_latch;
    cpu6502_read_fn read;
    cpu6502_write_fn write;
} cpu6502_bus_t;

typedef struct {
    uint8_t a;
    uint8_t x;
    uint8_t y;
    uint8_t sp;
    uint8_t p;
    uint16_t pc;
    uint64_t total_cycles;
    bool irq_pending;
    bool nmi_pending;
    cpu6502_bus_t bus;
} cpu6502_t;

void cpu6502_init(cpu6502_t *cpu, const cpu6502_bus_t *bus);
void cpu6502_reset(cpu6502_t *cpu);
uint32_t cpu6502_step(cpu6502_t *cpu);
void cpu6502_request_irq(cpu6502_t *cpu);
void cpu6502_request_nmi(cpu6502_t *cpu);
void cpu6502_clear_irq(cpu6502_t *cpu);
void cpu6502_set_pc(cpu6502_t *cpu, uint16_t address);
