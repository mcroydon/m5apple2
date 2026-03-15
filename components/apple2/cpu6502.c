#include "apple2/cpu6502.h"

#include <string.h>

static inline uint8_t cpu_read(cpu6502_t *cpu, uint16_t address)
{
    if (cpu->bus.memory != NULL &&
        (address < 0xC000U || (address >= 0xC800U && address != 0xCFFFU))) {
        const uint8_t value = cpu->bus.memory[address];
        if (cpu->bus.data_latch != NULL) {
            *cpu->bus.data_latch = value;
        }
        return value;
    }
    return cpu->bus.read(cpu->bus.context, address);
}

static inline void cpu_write(cpu6502_t *cpu, uint16_t address, uint8_t value)
{
    if (cpu->bus.memory != NULL && address < 0xC000U) {
        cpu->bus.memory[address] = value;
        if (cpu->bus.data_latch != NULL) {
            *cpu->bus.data_latch = value;
        }
        return;
    }
    cpu->bus.write(cpu->bus.context, address, value);
}

static inline void cpu_set_flag(cpu6502_t *cpu, uint8_t flag, bool enabled)
{
    if (enabled) {
        cpu->p |= flag;
    } else {
        cpu->p &= (uint8_t)~flag;
    }
}

static inline bool cpu_flag(const cpu6502_t *cpu, uint8_t flag)
{
    return (cpu->p & flag) != 0;
}

static inline void cpu_set_nz(cpu6502_t *cpu, uint8_t value)
{
    cpu_set_flag(cpu, CPU6502_FLAG_ZERO, value == 0);
    cpu_set_flag(cpu, CPU6502_FLAG_NEGATIVE, (value & 0x80U) != 0U);
}

static inline uint8_t cpu_fetch_byte(cpu6502_t *cpu)
{
    return cpu_read(cpu, cpu->pc++);
}

static inline uint16_t cpu_fetch_word(cpu6502_t *cpu)
{
    const uint8_t lo = cpu_fetch_byte(cpu);
    const uint8_t hi = cpu_fetch_byte(cpu);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static inline uint16_t cpu_read_word_zp(cpu6502_t *cpu, uint8_t address)
{
    const uint8_t lo = cpu_read(cpu, address);
    const uint8_t hi = cpu_read(cpu, (uint8_t)(address + 1));
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static inline uint16_t cpu_read_word_bug(cpu6502_t *cpu, uint16_t address)
{
    const uint8_t lo = cpu_read(cpu, address);
    const uint16_t hi_address = (uint16_t)((address & 0xFF00U) | ((address + 1U) & 0x00FFU));
    const uint8_t hi = cpu_read(cpu, hi_address);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static inline void cpu_push(cpu6502_t *cpu, uint8_t value)
{
    cpu_write(cpu, (uint16_t)(0x0100U | cpu->sp), value);
    cpu->sp--;
}

static inline uint8_t cpu_pull(cpu6502_t *cpu)
{
    cpu->sp++;
    return cpu_read(cpu, (uint16_t)(0x0100U | cpu->sp));
}

static inline uint16_t cpu_addr_zp(cpu6502_t *cpu)
{
    return cpu_fetch_byte(cpu);
}

static inline uint16_t cpu_addr_zpx(cpu6502_t *cpu)
{
    return (uint8_t)(cpu_fetch_byte(cpu) + cpu->x);
}

static inline uint16_t cpu_addr_zpy(cpu6502_t *cpu)
{
    return (uint8_t)(cpu_fetch_byte(cpu) + cpu->y);
}

static inline uint16_t cpu_addr_abs(cpu6502_t *cpu)
{
    return cpu_fetch_word(cpu);
}

static inline uint16_t cpu_addr_absx(cpu6502_t *cpu, bool *page_crossed)
{
    const uint16_t base = cpu_fetch_word(cpu);
    const uint16_t address = (uint16_t)(base + cpu->x);
    *page_crossed = ((base ^ address) & 0xFF00U) != 0U;
    return address;
}

static inline uint16_t cpu_addr_absy(cpu6502_t *cpu, bool *page_crossed)
{
    const uint16_t base = cpu_fetch_word(cpu);
    const uint16_t address = (uint16_t)(base + cpu->y);
    *page_crossed = ((base ^ address) & 0xFF00U) != 0U;
    return address;
}

static inline uint16_t cpu_addr_indx(cpu6502_t *cpu)
{
    const uint8_t zp = (uint8_t)(cpu_fetch_byte(cpu) + cpu->x);
    return cpu_read_word_zp(cpu, zp);
}

static inline uint16_t cpu_addr_indy(cpu6502_t *cpu, bool *page_crossed)
{
    const uint8_t zp = cpu_fetch_byte(cpu);
    const uint16_t base = cpu_read_word_zp(cpu, zp);
    const uint16_t address = (uint16_t)(base + cpu->y);
    *page_crossed = ((base ^ address) & 0xFF00U) != 0U;
    return address;
}

static inline void cpu_compare(cpu6502_t *cpu, uint8_t lhs, uint8_t rhs)
{
    const uint16_t result = (uint16_t)lhs - rhs;
    cpu_set_flag(cpu, CPU6502_FLAG_CARRY, lhs >= rhs);
    cpu_set_nz(cpu, (uint8_t)result);
}

static void cpu_adc(cpu6502_t *cpu, uint8_t value)
{
    const uint8_t carry = cpu_flag(cpu, CPU6502_FLAG_CARRY) ? 1U : 0U;
    const uint8_t a = cpu->a;
    uint16_t sum = (uint16_t)a + value + carry;

    cpu_set_flag(cpu, CPU6502_FLAG_OVERFLOW,
                 ((uint8_t)~(a ^ value) & (uint8_t)(a ^ sum) & 0x80U) != 0U);

    if (cpu_flag(cpu, CPU6502_FLAG_DECIMAL)) {
        if (((a & 0x0FU) + (value & 0x0FU) + carry) > 9U) {
            sum += 0x06U;
        }
        cpu_set_flag(cpu, CPU6502_FLAG_CARRY, sum > 0x99U);
        if (sum > 0x99U) {
            sum += 0x60U;
        }
    } else {
        cpu_set_flag(cpu, CPU6502_FLAG_CARRY, sum > 0xFFU);
    }

    cpu->a = (uint8_t)sum;
    cpu_set_nz(cpu, cpu->a);
}

static void cpu_sbc(cpu6502_t *cpu, uint8_t value)
{
    const uint8_t carry = cpu_flag(cpu, CPU6502_FLAG_CARRY) ? 1U : 0U;
    const uint8_t a = cpu->a;
    const uint16_t diff = (uint16_t)a - value - (uint16_t)(1U - carry);

    cpu_set_flag(cpu, CPU6502_FLAG_OVERFLOW,
                 ((a ^ value) & (uint8_t)(a ^ diff) & 0x80U) != 0U);

    if (cpu_flag(cpu, CPU6502_FLAG_DECIMAL)) {
        int16_t lo = (int16_t)(a & 0x0FU) - (int16_t)(value & 0x0FU) - (int16_t)(1U - carry);
        int16_t hi = (int16_t)(a >> 4) - (int16_t)(value >> 4);
        if (lo < 0) {
            lo -= 0x06;
            hi--;
        }
        if (hi < 0) {
            hi -= 0x06;
        }
        cpu->a = (uint8_t)(((uint8_t)(hi << 4) & 0xF0U) | ((uint8_t)lo & 0x0FU));
    } else {
        cpu->a = (uint8_t)diff;
    }

    cpu_set_flag(cpu, CPU6502_FLAG_CARRY, diff < 0x100U);
    cpu_set_nz(cpu, cpu->a);
}

static uint8_t cpu_asl(cpu6502_t *cpu, uint8_t value)
{
    cpu_set_flag(cpu, CPU6502_FLAG_CARRY, (value & 0x80U) != 0U);
    value <<= 1;
    cpu_set_nz(cpu, value);
    return value;
}

static uint8_t cpu_lsr(cpu6502_t *cpu, uint8_t value)
{
    cpu_set_flag(cpu, CPU6502_FLAG_CARRY, (value & 0x01U) != 0U);
    value >>= 1;
    cpu_set_nz(cpu, value);
    return value;
}

static uint8_t cpu_rol(cpu6502_t *cpu, uint8_t value)
{
    const uint8_t carry_in = cpu_flag(cpu, CPU6502_FLAG_CARRY) ? 1U : 0U;
    cpu_set_flag(cpu, CPU6502_FLAG_CARRY, (value & 0x80U) != 0U);
    value = (uint8_t)((value << 1) | carry_in);
    cpu_set_nz(cpu, value);
    return value;
}

static uint8_t cpu_ror(cpu6502_t *cpu, uint8_t value)
{
    const uint8_t carry_in = cpu_flag(cpu, CPU6502_FLAG_CARRY) ? 0x80U : 0U;
    cpu_set_flag(cpu, CPU6502_FLAG_CARRY, (value & 0x01U) != 0U);
    value = (uint8_t)((value >> 1) | carry_in);
    cpu_set_nz(cpu, value);
    return value;
}

static uint32_t cpu_service_interrupt(cpu6502_t *cpu, uint16_t vector, bool set_break)
{
    cpu_push(cpu, (uint8_t)(cpu->pc >> 8));
    cpu_push(cpu, (uint8_t)(cpu->pc & 0xFFU));
    cpu_push(cpu, (uint8_t)((cpu->p & (uint8_t)~CPU6502_FLAG_BREAK) |
                            CPU6502_FLAG_UNUSED |
                            (set_break ? CPU6502_FLAG_BREAK : 0U)));
    cpu_set_flag(cpu, CPU6502_FLAG_IRQ_DISABLE, true);
    cpu->pc = (uint16_t)(cpu_read(cpu, vector) | ((uint16_t)cpu_read(cpu, (uint16_t)(vector + 1U)) << 8));
    return 7;
}

void cpu6502_init(cpu6502_t *cpu, const cpu6502_bus_t *bus)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->bus = *bus;
}

void cpu6502_reset(cpu6502_t *cpu)
{
    cpu->a = 0;
    cpu->x = 0;
    cpu->y = 0;
    cpu->sp = 0xFDU;
    cpu->p = CPU6502_FLAG_IRQ_DISABLE | CPU6502_FLAG_UNUSED;
    cpu->irq_pending = false;
    cpu->nmi_pending = false;
    cpu->pc = (uint16_t)(cpu_read(cpu, 0xFFFCU) | ((uint16_t)cpu_read(cpu, 0xFFFDU) << 8));
}

void cpu6502_request_irq(cpu6502_t *cpu)
{
    cpu->irq_pending = true;
}

void cpu6502_request_nmi(cpu6502_t *cpu)
{
    cpu->nmi_pending = true;
}

void cpu6502_clear_irq(cpu6502_t *cpu)
{
    cpu->irq_pending = false;
}

void cpu6502_set_pc(cpu6502_t *cpu, uint16_t address)
{
    cpu->pc = address;
}

uint32_t cpu6502_step(cpu6502_t *cpu)
{
    uint32_t cycles = 0;

    if (cpu->nmi_pending) {
        cpu->nmi_pending = false;
        cycles = cpu_service_interrupt(cpu, 0xFFFAU, false);
        cpu->total_cycles += cycles;
        return cycles;
    }
    if (cpu->irq_pending && !cpu_flag(cpu, CPU6502_FLAG_IRQ_DISABLE)) {
        cycles = cpu_service_interrupt(cpu, 0xFFFEU, false);
        cpu->total_cycles += cycles;
        return cycles;
    }

    const uint8_t opcode = cpu_fetch_byte(cpu);
    bool page_crossed = false;
    uint16_t address = 0;
    uint8_t value = 0;

    switch (opcode) {
    case 0x00:
        cpu->pc++;
        cycles = cpu_service_interrupt(cpu, 0xFFFEU, true);
        break;
    case 0x01:
        cpu->a |= cpu_read(cpu, cpu_addr_indx(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 6;
        break;
    case 0x05:
        cpu->a |= cpu_read(cpu, cpu_addr_zp(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 3;
        break;
    case 0x06:
        address = cpu_addr_zp(cpu);
        value = cpu_asl(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 5;
        break;
    case 0x08:
        cpu_push(cpu, (uint8_t)(cpu->p | CPU6502_FLAG_BREAK | CPU6502_FLAG_UNUSED));
        cycles = 3;
        break;
    case 0x09:
        cpu->a |= cpu_fetch_byte(cpu);
        cpu_set_nz(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x0A:
        cpu->a = cpu_asl(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x0D:
        cpu->a |= cpu_read(cpu, cpu_addr_abs(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x0E:
        address = cpu_addr_abs(cpu);
        value = cpu_asl(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 6;
        break;

    case 0x10: {
        const int8_t offset = (int8_t)cpu_fetch_byte(cpu);
        cycles = 2;
        if (!cpu_flag(cpu, CPU6502_FLAG_NEGATIVE)) {
            const uint16_t old_pc = cpu->pc;
            cpu->pc = (uint16_t)(cpu->pc + offset);
            cycles++;
            if ((old_pc ^ cpu->pc) & 0xFF00U) {
                cycles++;
            }
        }
        break;
    }
    case 0x11:
        address = cpu_addr_indy(cpu, &page_crossed);
        cpu->a |= cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 5 + (page_crossed ? 1U : 0U);
        break;
    case 0x15:
        cpu->a |= cpu_read(cpu, cpu_addr_zpx(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x16:
        address = cpu_addr_zpx(cpu);
        value = cpu_asl(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 6;
        break;
    case 0x18:
        cpu_set_flag(cpu, CPU6502_FLAG_CARRY, false);
        cycles = 2;
        break;
    case 0x19:
        address = cpu_addr_absy(cpu, &page_crossed);
        cpu->a |= cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0x1D:
        address = cpu_addr_absx(cpu, &page_crossed);
        cpu->a |= cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0x1E:
        address = cpu_addr_absx(cpu, &page_crossed);
        value = cpu_asl(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 7;
        break;

    case 0x20:
        address = cpu_fetch_word(cpu);
        cpu_push(cpu, (uint8_t)((cpu->pc - 1U) >> 8));
        cpu_push(cpu, (uint8_t)((cpu->pc - 1U) & 0xFFU));
        cpu->pc = address;
        cycles = 6;
        break;
    case 0x21:
        cpu->a &= cpu_read(cpu, cpu_addr_indx(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 6;
        break;
    case 0x24:
        value = cpu_read(cpu, cpu_addr_zp(cpu));
        cpu_set_flag(cpu, CPU6502_FLAG_ZERO, (cpu->a & value) == 0U);
        cpu_set_flag(cpu, CPU6502_FLAG_NEGATIVE, (value & 0x80U) != 0U);
        cpu_set_flag(cpu, CPU6502_FLAG_OVERFLOW, (value & 0x40U) != 0U);
        cycles = 3;
        break;
    case 0x25:
        cpu->a &= cpu_read(cpu, cpu_addr_zp(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 3;
        break;
    case 0x26:
        address = cpu_addr_zp(cpu);
        value = cpu_rol(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 5;
        break;
    case 0x28:
        cpu->p = (uint8_t)((cpu_pull(cpu) & (uint8_t)~CPU6502_FLAG_BREAK) | CPU6502_FLAG_UNUSED);
        cycles = 4;
        break;
    case 0x29:
        cpu->a &= cpu_fetch_byte(cpu);
        cpu_set_nz(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x2A:
        cpu->a = cpu_rol(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x2C:
        value = cpu_read(cpu, cpu_addr_abs(cpu));
        cpu_set_flag(cpu, CPU6502_FLAG_ZERO, (cpu->a & value) == 0U);
        cpu_set_flag(cpu, CPU6502_FLAG_NEGATIVE, (value & 0x80U) != 0U);
        cpu_set_flag(cpu, CPU6502_FLAG_OVERFLOW, (value & 0x40U) != 0U);
        cycles = 4;
        break;
    case 0x2D:
        cpu->a &= cpu_read(cpu, cpu_addr_abs(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x2E:
        address = cpu_addr_abs(cpu);
        value = cpu_rol(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 6;
        break;

    case 0x30: {
        const int8_t offset = (int8_t)cpu_fetch_byte(cpu);
        cycles = 2;
        if (cpu_flag(cpu, CPU6502_FLAG_NEGATIVE)) {
            const uint16_t old_pc = cpu->pc;
            cpu->pc = (uint16_t)(cpu->pc + offset);
            cycles++;
            if ((old_pc ^ cpu->pc) & 0xFF00U) {
                cycles++;
            }
        }
        break;
    }
    case 0x31:
        address = cpu_addr_indy(cpu, &page_crossed);
        cpu->a &= cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 5 + (page_crossed ? 1U : 0U);
        break;
    case 0x35:
        cpu->a &= cpu_read(cpu, cpu_addr_zpx(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x36:
        address = cpu_addr_zpx(cpu);
        value = cpu_rol(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 6;
        break;
    case 0x38:
        cpu_set_flag(cpu, CPU6502_FLAG_CARRY, true);
        cycles = 2;
        break;
    case 0x39:
        address = cpu_addr_absy(cpu, &page_crossed);
        cpu->a &= cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0x3D:
        address = cpu_addr_absx(cpu, &page_crossed);
        cpu->a &= cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0x3E:
        address = cpu_addr_absx(cpu, &page_crossed);
        value = cpu_rol(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 7;
        break;

    case 0x40:
        cpu->p = (uint8_t)((cpu_pull(cpu) & (uint8_t)~CPU6502_FLAG_BREAK) | CPU6502_FLAG_UNUSED);
        cpu->pc = cpu_pull(cpu);
        cpu->pc |= (uint16_t)cpu_pull(cpu) << 8;
        cycles = 6;
        break;
    case 0x41:
        cpu->a ^= cpu_read(cpu, cpu_addr_indx(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 6;
        break;
    case 0x45:
        cpu->a ^= cpu_read(cpu, cpu_addr_zp(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 3;
        break;
    case 0x46:
        address = cpu_addr_zp(cpu);
        value = cpu_lsr(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 5;
        break;
    case 0x48:
        cpu_push(cpu, cpu->a);
        cycles = 3;
        break;
    case 0x49:
        cpu->a ^= cpu_fetch_byte(cpu);
        cpu_set_nz(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x4A:
        cpu->a = cpu_lsr(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x4C:
        cpu->pc = cpu_addr_abs(cpu);
        cycles = 3;
        break;
    case 0x4D:
        cpu->a ^= cpu_read(cpu, cpu_addr_abs(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x4E:
        address = cpu_addr_abs(cpu);
        value = cpu_lsr(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 6;
        break;

    case 0x50: {
        const int8_t offset = (int8_t)cpu_fetch_byte(cpu);
        cycles = 2;
        if (!cpu_flag(cpu, CPU6502_FLAG_OVERFLOW)) {
            const uint16_t old_pc = cpu->pc;
            cpu->pc = (uint16_t)(cpu->pc + offset);
            cycles++;
            if ((old_pc ^ cpu->pc) & 0xFF00U) {
                cycles++;
            }
        }
        break;
    }
    case 0x51:
        address = cpu_addr_indy(cpu, &page_crossed);
        cpu->a ^= cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 5 + (page_crossed ? 1U : 0U);
        break;
    case 0x55:
        cpu->a ^= cpu_read(cpu, cpu_addr_zpx(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x56:
        address = cpu_addr_zpx(cpu);
        value = cpu_lsr(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 6;
        break;
    case 0x58:
        cpu_set_flag(cpu, CPU6502_FLAG_IRQ_DISABLE, false);
        cycles = 2;
        break;
    case 0x59:
        address = cpu_addr_absy(cpu, &page_crossed);
        cpu->a ^= cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0x5D:
        address = cpu_addr_absx(cpu, &page_crossed);
        cpu->a ^= cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0x5E:
        address = cpu_addr_absx(cpu, &page_crossed);
        value = cpu_lsr(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 7;
        break;

    case 0x60:
        cpu->pc = cpu_pull(cpu);
        cpu->pc |= (uint16_t)cpu_pull(cpu) << 8;
        cpu->pc++;
        cycles = 6;
        break;
    case 0x61:
        cpu_adc(cpu, cpu_read(cpu, cpu_addr_indx(cpu)));
        cycles = 6;
        break;
    case 0x65:
        cpu_adc(cpu, cpu_read(cpu, cpu_addr_zp(cpu)));
        cycles = 3;
        break;
    case 0x66:
        address = cpu_addr_zp(cpu);
        value = cpu_ror(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 5;
        break;
    case 0x68:
        cpu->a = cpu_pull(cpu);
        cpu_set_nz(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x69:
        cpu_adc(cpu, cpu_fetch_byte(cpu));
        cycles = 2;
        break;
    case 0x6A:
        cpu->a = cpu_ror(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x6C:
        cpu->pc = cpu_read_word_bug(cpu, cpu_addr_abs(cpu));
        cycles = 5;
        break;
    case 0x6D:
        cpu_adc(cpu, cpu_read(cpu, cpu_addr_abs(cpu)));
        cycles = 4;
        break;
    case 0x6E:
        address = cpu_addr_abs(cpu);
        value = cpu_ror(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 6;
        break;

    case 0x70: {
        const int8_t offset = (int8_t)cpu_fetch_byte(cpu);
        cycles = 2;
        if (cpu_flag(cpu, CPU6502_FLAG_OVERFLOW)) {
            const uint16_t old_pc = cpu->pc;
            cpu->pc = (uint16_t)(cpu->pc + offset);
            cycles++;
            if ((old_pc ^ cpu->pc) & 0xFF00U) {
                cycles++;
            }
        }
        break;
    }
    case 0x71:
        address = cpu_addr_indy(cpu, &page_crossed);
        cpu_adc(cpu, cpu_read(cpu, address));
        cycles = 5 + (page_crossed ? 1U : 0U);
        break;
    case 0x75:
        cpu_adc(cpu, cpu_read(cpu, cpu_addr_zpx(cpu)));
        cycles = 4;
        break;
    case 0x76:
        address = cpu_addr_zpx(cpu);
        value = cpu_ror(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 6;
        break;
    case 0x78:
        cpu_set_flag(cpu, CPU6502_FLAG_IRQ_DISABLE, true);
        cycles = 2;
        break;
    case 0x79:
        address = cpu_addr_absy(cpu, &page_crossed);
        cpu_adc(cpu, cpu_read(cpu, address));
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0x7D:
        address = cpu_addr_absx(cpu, &page_crossed);
        cpu_adc(cpu, cpu_read(cpu, address));
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0x7E:
        address = cpu_addr_absx(cpu, &page_crossed);
        value = cpu_ror(cpu, cpu_read(cpu, address));
        cpu_write(cpu, address, value);
        cycles = 7;
        break;

    case 0x81:
        cpu_write(cpu, cpu_addr_indx(cpu), cpu->a);
        cycles = 6;
        break;
    case 0x84:
        cpu_write(cpu, cpu_addr_zp(cpu), cpu->y);
        cycles = 3;
        break;
    case 0x85:
        cpu_write(cpu, cpu_addr_zp(cpu), cpu->a);
        cycles = 3;
        break;
    case 0x86:
        cpu_write(cpu, cpu_addr_zp(cpu), cpu->x);
        cycles = 3;
        break;
    case 0x88:
        cpu->y--;
        cpu_set_nz(cpu, cpu->y);
        cycles = 2;
        break;
    case 0x8A:
        cpu->a = cpu->x;
        cpu_set_nz(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x8C:
        cpu_write(cpu, cpu_addr_abs(cpu), cpu->y);
        cycles = 4;
        break;
    case 0x8D:
        cpu_write(cpu, cpu_addr_abs(cpu), cpu->a);
        cycles = 4;
        break;
    case 0x8E:
        cpu_write(cpu, cpu_addr_abs(cpu), cpu->x);
        cycles = 4;
        break;

    case 0x90: {
        const int8_t offset = (int8_t)cpu_fetch_byte(cpu);
        cycles = 2;
        if (!cpu_flag(cpu, CPU6502_FLAG_CARRY)) {
            const uint16_t old_pc = cpu->pc;
            cpu->pc = (uint16_t)(cpu->pc + offset);
            cycles++;
            if ((old_pc ^ cpu->pc) & 0xFF00U) {
                cycles++;
            }
        }
        break;
    }
    case 0x91:
        address = cpu_addr_indy(cpu, &page_crossed);
        cpu_write(cpu, address, cpu->a);
        cycles = 6;
        break;
    case 0x94:
        cpu_write(cpu, cpu_addr_zpx(cpu), cpu->y);
        cycles = 4;
        break;
    case 0x95:
        cpu_write(cpu, cpu_addr_zpx(cpu), cpu->a);
        cycles = 4;
        break;
    case 0x96:
        cpu_write(cpu, cpu_addr_zpy(cpu), cpu->x);
        cycles = 4;
        break;
    case 0x98:
        cpu->a = cpu->y;
        cpu_set_nz(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x99:
        address = cpu_addr_absy(cpu, &page_crossed);
        cpu_write(cpu, address, cpu->a);
        cycles = 5;
        break;
    case 0x9A:
        cpu->sp = cpu->x;
        cycles = 2;
        break;
    case 0x9D:
        address = cpu_addr_absx(cpu, &page_crossed);
        cpu_write(cpu, address, cpu->a);
        cycles = 5;
        break;

    case 0xA0:
        cpu->y = cpu_fetch_byte(cpu);
        cpu_set_nz(cpu, cpu->y);
        cycles = 2;
        break;
    case 0xA1:
        cpu->a = cpu_read(cpu, cpu_addr_indx(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 6;
        break;
    case 0xA2:
        cpu->x = cpu_fetch_byte(cpu);
        cpu_set_nz(cpu, cpu->x);
        cycles = 2;
        break;
    case 0xA4:
        cpu->y = cpu_read(cpu, cpu_addr_zp(cpu));
        cpu_set_nz(cpu, cpu->y);
        cycles = 3;
        break;
    case 0xA5:
        cpu->a = cpu_read(cpu, cpu_addr_zp(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 3;
        break;
    case 0xA6:
        cpu->x = cpu_read(cpu, cpu_addr_zp(cpu));
        cpu_set_nz(cpu, cpu->x);
        cycles = 3;
        break;
    case 0xA8:
        cpu->y = cpu->a;
        cpu_set_nz(cpu, cpu->y);
        cycles = 2;
        break;
    case 0xA9:
        cpu->a = cpu_fetch_byte(cpu);
        cpu_set_nz(cpu, cpu->a);
        cycles = 2;
        break;
    case 0xAA:
        cpu->x = cpu->a;
        cpu_set_nz(cpu, cpu->x);
        cycles = 2;
        break;
    case 0xAC:
        cpu->y = cpu_read(cpu, cpu_addr_abs(cpu));
        cpu_set_nz(cpu, cpu->y);
        cycles = 4;
        break;
    case 0xAD:
        cpu->a = cpu_read(cpu, cpu_addr_abs(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 4;
        break;
    case 0xAE:
        cpu->x = cpu_read(cpu, cpu_addr_abs(cpu));
        cpu_set_nz(cpu, cpu->x);
        cycles = 4;
        break;

    case 0xB0: {
        const int8_t offset = (int8_t)cpu_fetch_byte(cpu);
        cycles = 2;
        if (cpu_flag(cpu, CPU6502_FLAG_CARRY)) {
            const uint16_t old_pc = cpu->pc;
            cpu->pc = (uint16_t)(cpu->pc + offset);
            cycles++;
            if ((old_pc ^ cpu->pc) & 0xFF00U) {
                cycles++;
            }
        }
        break;
    }
    case 0xB1:
        address = cpu_addr_indy(cpu, &page_crossed);
        cpu->a = cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 5 + (page_crossed ? 1U : 0U);
        break;
    case 0xB4:
        cpu->y = cpu_read(cpu, cpu_addr_zpx(cpu));
        cpu_set_nz(cpu, cpu->y);
        cycles = 4;
        break;
    case 0xB5:
        cpu->a = cpu_read(cpu, cpu_addr_zpx(cpu));
        cpu_set_nz(cpu, cpu->a);
        cycles = 4;
        break;
    case 0xB6:
        cpu->x = cpu_read(cpu, cpu_addr_zpy(cpu));
        cpu_set_nz(cpu, cpu->x);
        cycles = 4;
        break;
    case 0xB8:
        cpu_set_flag(cpu, CPU6502_FLAG_OVERFLOW, false);
        cycles = 2;
        break;
    case 0xB9:
        address = cpu_addr_absy(cpu, &page_crossed);
        cpu->a = cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0xBA:
        cpu->x = cpu->sp;
        cpu_set_nz(cpu, cpu->x);
        cycles = 2;
        break;
    case 0xBC:
        address = cpu_addr_absx(cpu, &page_crossed);
        cpu->y = cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->y);
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0xBD:
        address = cpu_addr_absx(cpu, &page_crossed);
        cpu->a = cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->a);
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0xBE:
        address = cpu_addr_absy(cpu, &page_crossed);
        cpu->x = cpu_read(cpu, address);
        cpu_set_nz(cpu, cpu->x);
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;

    case 0xC0:
        cpu_compare(cpu, cpu->y, cpu_fetch_byte(cpu));
        cycles = 2;
        break;
    case 0xC1:
        cpu_compare(cpu, cpu->a, cpu_read(cpu, cpu_addr_indx(cpu)));
        cycles = 6;
        break;
    case 0xC4:
        cpu_compare(cpu, cpu->y, cpu_read(cpu, cpu_addr_zp(cpu)));
        cycles = 3;
        break;
    case 0xC5:
        cpu_compare(cpu, cpu->a, cpu_read(cpu, cpu_addr_zp(cpu)));
        cycles = 3;
        break;
    case 0xC6:
        address = cpu_addr_zp(cpu);
        value = (uint8_t)(cpu_read(cpu, address) - 1U);
        cpu_write(cpu, address, value);
        cpu_set_nz(cpu, value);
        cycles = 5;
        break;
    case 0xC8:
        cpu->y++;
        cpu_set_nz(cpu, cpu->y);
        cycles = 2;
        break;
    case 0xC9:
        cpu_compare(cpu, cpu->a, cpu_fetch_byte(cpu));
        cycles = 2;
        break;
    case 0xCA:
        cpu->x--;
        cpu_set_nz(cpu, cpu->x);
        cycles = 2;
        break;
    case 0xCC:
        cpu_compare(cpu, cpu->y, cpu_read(cpu, cpu_addr_abs(cpu)));
        cycles = 4;
        break;
    case 0xCD:
        cpu_compare(cpu, cpu->a, cpu_read(cpu, cpu_addr_abs(cpu)));
        cycles = 4;
        break;
    case 0xCE:
        address = cpu_addr_abs(cpu);
        value = (uint8_t)(cpu_read(cpu, address) - 1U);
        cpu_write(cpu, address, value);
        cpu_set_nz(cpu, value);
        cycles = 6;
        break;

    case 0xD0: {
        const int8_t offset = (int8_t)cpu_fetch_byte(cpu);
        cycles = 2;
        if (!cpu_flag(cpu, CPU6502_FLAG_ZERO)) {
            const uint16_t old_pc = cpu->pc;
            cpu->pc = (uint16_t)(cpu->pc + offset);
            cycles++;
            if ((old_pc ^ cpu->pc) & 0xFF00U) {
                cycles++;
            }
        }
        break;
    }
    case 0xD1:
        address = cpu_addr_indy(cpu, &page_crossed);
        cpu_compare(cpu, cpu->a, cpu_read(cpu, address));
        cycles = 5 + (page_crossed ? 1U : 0U);
        break;
    case 0xD5:
        cpu_compare(cpu, cpu->a, cpu_read(cpu, cpu_addr_zpx(cpu)));
        cycles = 4;
        break;
    case 0xD6:
        address = cpu_addr_zpx(cpu);
        value = (uint8_t)(cpu_read(cpu, address) - 1U);
        cpu_write(cpu, address, value);
        cpu_set_nz(cpu, value);
        cycles = 6;
        break;
    case 0xD8:
        cpu_set_flag(cpu, CPU6502_FLAG_DECIMAL, false);
        cycles = 2;
        break;
    case 0xD9:
        address = cpu_addr_absy(cpu, &page_crossed);
        cpu_compare(cpu, cpu->a, cpu_read(cpu, address));
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0xDD:
        address = cpu_addr_absx(cpu, &page_crossed);
        cpu_compare(cpu, cpu->a, cpu_read(cpu, address));
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0xDE:
        address = cpu_addr_absx(cpu, &page_crossed);
        value = (uint8_t)(cpu_read(cpu, address) - 1U);
        cpu_write(cpu, address, value);
        cpu_set_nz(cpu, value);
        cycles = 7;
        break;

    case 0xE0:
        cpu_compare(cpu, cpu->x, cpu_fetch_byte(cpu));
        cycles = 2;
        break;
    case 0xE1:
        cpu_sbc(cpu, cpu_read(cpu, cpu_addr_indx(cpu)));
        cycles = 6;
        break;
    case 0xE4:
        cpu_compare(cpu, cpu->x, cpu_read(cpu, cpu_addr_zp(cpu)));
        cycles = 3;
        break;
    case 0xE5:
        cpu_sbc(cpu, cpu_read(cpu, cpu_addr_zp(cpu)));
        cycles = 3;
        break;
    case 0xE6:
        address = cpu_addr_zp(cpu);
        value = (uint8_t)(cpu_read(cpu, address) + 1U);
        cpu_write(cpu, address, value);
        cpu_set_nz(cpu, value);
        cycles = 5;
        break;
    case 0xE8:
        cpu->x++;
        cpu_set_nz(cpu, cpu->x);
        cycles = 2;
        break;
    case 0xE9:
    case 0xEB:
        cpu_sbc(cpu, cpu_fetch_byte(cpu));
        cycles = 2;
        break;
    case 0xEA:
        cycles = 2;
        break;
    case 0xEC:
        cpu_compare(cpu, cpu->x, cpu_read(cpu, cpu_addr_abs(cpu)));
        cycles = 4;
        break;
    case 0xED:
        cpu_sbc(cpu, cpu_read(cpu, cpu_addr_abs(cpu)));
        cycles = 4;
        break;
    case 0xEE:
        address = cpu_addr_abs(cpu);
        value = (uint8_t)(cpu_read(cpu, address) + 1U);
        cpu_write(cpu, address, value);
        cpu_set_nz(cpu, value);
        cycles = 6;
        break;

    case 0xF0: {
        const int8_t offset = (int8_t)cpu_fetch_byte(cpu);
        cycles = 2;
        if (cpu_flag(cpu, CPU6502_FLAG_ZERO)) {
            const uint16_t old_pc = cpu->pc;
            cpu->pc = (uint16_t)(cpu->pc + offset);
            cycles++;
            if ((old_pc ^ cpu->pc) & 0xFF00U) {
                cycles++;
            }
        }
        break;
    }
    case 0xF1:
        address = cpu_addr_indy(cpu, &page_crossed);
        cpu_sbc(cpu, cpu_read(cpu, address));
        cycles = 5 + (page_crossed ? 1U : 0U);
        break;
    case 0xF5:
        cpu_sbc(cpu, cpu_read(cpu, cpu_addr_zpx(cpu)));
        cycles = 4;
        break;
    case 0xF6:
        address = cpu_addr_zpx(cpu);
        value = (uint8_t)(cpu_read(cpu, address) + 1U);
        cpu_write(cpu, address, value);
        cpu_set_nz(cpu, value);
        cycles = 6;
        break;
    case 0xF8:
        cpu_set_flag(cpu, CPU6502_FLAG_DECIMAL, true);
        cycles = 2;
        break;
    case 0xF9:
        address = cpu_addr_absy(cpu, &page_crossed);
        cpu_sbc(cpu, cpu_read(cpu, address));
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0xFD:
        address = cpu_addr_absx(cpu, &page_crossed);
        cpu_sbc(cpu, cpu_read(cpu, address));
        cycles = 4 + (page_crossed ? 1U : 0U);
        break;
    case 0xFE:
        address = cpu_addr_absx(cpu, &page_crossed);
        value = (uint8_t)(cpu_read(cpu, address) + 1U);
        cpu_write(cpu, address, value);
        cpu_set_nz(cpu, value);
        cycles = 7;
        break;

    default:
        cycles = 2;
        break;
    }

    cpu->total_cycles += cycles;
    return cycles;
}
