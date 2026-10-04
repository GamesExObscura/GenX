#include "mercury/core/z80_cpu.hpp"

#include <iomanip>
#include <sstream>
#include <utility>

namespace mercury::core {

namespace {

std::string hex8(std::uint8_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return out.str();
}

std::string hex16(std::uint16_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << value;
    return out.str();
}

bool has_even_parity(std::uint8_t value) {
    bool parity = true;
    while (value != 0) {
        parity = !parity;
        value &= static_cast<std::uint8_t>(value - 1);
    }
    return parity;
}

}  // namespace

void Z80Cpu::attach_bus(Z80BusCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void Z80Cpu::set_stack_pointer(const std::uint16_t value) noexcept {
    registers_.sp = value;
}

void Z80Cpu::reset() noexcept {
    registers_ = {};
    shadow_registers_ = {};
    registers_.sp = 0xFFFF;
    registers_.pc = 0x0000;
    interrupt_mode_ = Z80InterruptMode::mode0;
    iff1_ = false;
    iff2_ = false;
    halted_ = false;
    maskable_interrupt_pending_ = false;
    maskable_interrupt_line_ = false;
    non_maskable_interrupt_pending_ = false;
    ei_delay_ = 0;
    pending_wait_cycles_ = 0;
    applied_io_wait_cycles_ = 0;
}

Z80StepResult Z80Cpu::step() noexcept {
    pending_wait_cycles_ = 0;
    applied_io_wait_cycles_ = 0;

    if (!ready()) {
        return {
            .executed = false,
            .unimplemented = true,
            .message = "Z80 bus callbacks are not attached.",
        };
    }

    if (const auto interrupt_result = service_interrupts(); interrupt_result.executed) {
        finish_step();
        return interrupt_result;
    }

    if (halted_) {
        Z80StepResult result;
        result.executed = true;
        result.cycles = 4;
        result.pc_before = registers_.pc;
        result.pc_after = registers_.pc;
        result.message = "HALT state";
        finish_step();
        return result;
    }

    Z80StepResult result;
    result.executed = true;
    result.pc_before = registers_.pc;
    const auto step_pc_before = result.pc_before;

    const auto opcode = fetch8();
    result.opcode = opcode;
    result.length = 1;

    if (opcode == 0xDD || opcode == 0xFD) {
        result = step_indexed_prefixed(opcode, step_pc_before);
        result.pc_before = step_pc_before;
        result.pc_after = registers_.pc;
        finish_step();
        return result;
    }

    if (opcode >= 0x80 && opcode <= 0xBF) {
        const auto selector = static_cast<std::uint8_t>(opcode & 0x07);
        const auto operand = read_alu_operand(selector);

        switch (opcode & 0xF8) {
            case 0x80: {
                const auto lhs = registers_.a;
                const auto value = static_cast<std::uint8_t>(lhs + operand);
                registers_.a = value;
                set_add_flags(lhs, operand, 0, value);
                result.cycles = selector == 6 ? 7 : 4;
                result.message = "ADD A,r";
                break;
            }
            case 0x88: {
                const auto carry_in = static_cast<std::uint8_t>((registers_.f & kFlagCarry) != 0 ? 1 : 0);
                const auto lhs = registers_.a;
                const auto value = static_cast<std::uint8_t>(lhs + operand + carry_in);
                registers_.a = value;
                set_add_flags(lhs, operand, carry_in, value);
                result.cycles = selector == 6 ? 7 : 4;
                result.message = "ADC A,r";
                break;
            }
            case 0x90: {
                const auto lhs = registers_.a;
                const auto value = static_cast<std::uint8_t>(lhs - operand);
                registers_.a = value;
                set_subtract_flags(lhs, operand, 0, value);
                result.cycles = selector == 6 ? 7 : 4;
                result.message = "SUB r";
                break;
            }
            case 0x98: {
                const auto carry_in = static_cast<std::uint8_t>((registers_.f & kFlagCarry) != 0 ? 1 : 0);
                const auto lhs = registers_.a;
                const auto value = static_cast<std::uint8_t>(lhs - operand - carry_in);
                registers_.a = value;
                set_subtract_flags(lhs, operand, carry_in, value);
                result.cycles = selector == 6 ? 7 : 4;
                result.message = "SBC A,r";
                break;
            }
            case 0xA0:
                registers_.a = static_cast<std::uint8_t>(registers_.a & operand);
                set_logic_flags(registers_.a, true);
                result.cycles = selector == 6 ? 7 : 4;
                result.message = "AND r";
                break;
            case 0xA8:
                registers_.a = static_cast<std::uint8_t>(registers_.a ^ operand);
                set_logic_flags(registers_.a, false);
                result.cycles = selector == 6 ? 7 : 4;
                result.message = "XOR r";
                break;
            case 0xB0:
                registers_.a = static_cast<std::uint8_t>(registers_.a | operand);
                set_logic_flags(registers_.a, false);
                result.cycles = selector == 6 ? 7 : 4;
                result.message = "OR r";
                break;
            case 0xB8:
                set_compare_flags(registers_.a, operand);
                result.cycles = selector == 6 ? 7 : 4;
                result.message = "CP r";
                break;
            default:
                break;
        }

        result.pc_after = registers_.pc;
        finish_step();
        return result;
    }

    if (opcode >= 0x40 && opcode <= 0x7F && opcode != 0x76) {
        auto& destination = [&]() -> std::uint8_t& {
            switch ((opcode >> 3) & 0x07) {
                case 0x00:
                    return registers_.b;
                case 0x01:
                    return registers_.c;
                case 0x02:
                    return registers_.d;
                case 0x03:
                    return registers_.e;
                case 0x04:
                    return registers_.h;
                case 0x05:
                    return registers_.l;
                case 0x07:
                    return registers_.a;
                default:
                    return registers_.a;
            }
        }();

        const auto source_selector = static_cast<std::uint8_t>(opcode & 0x07);
        if (((opcode >> 3) & 0x07) == 0x06) {
            write8(hl(), read_alu_operand(source_selector));
        } else {
            destination = read_alu_operand(source_selector);
        }

        result.cycles = source_selector == 0x06 || ((opcode >> 3) & 0x07) == 0x06 ? 7 : 4;
        result.message = "LD r,r";
        result.pc_after = registers_.pc;
        finish_step();
        return result;
    }

    switch (opcode) {
        case 0x00:
            result.cycles = 4;
            result.message = "NOP";
            break;
        case 0x01:
            set_bc(fetch16());
            result.length = 3;
            result.cycles = 10;
            result.message = "LD BC,nn";
            break;
        case 0x02:
            write8(bc(), registers_.a);
            result.cycles = 7;
            result.message = "LD (BC),A";
            break;
        case 0x08:
            std::swap(registers_.a, shadow_registers_.a);
            std::swap(registers_.f, shadow_registers_.f);
            result.cycles = 4;
            result.message = "EX AF,AF'";
            break;
        case 0x07: {
            const bool old_bit7 = (registers_.a & 0x80) != 0;
            registers_.a = static_cast<std::uint8_t>((registers_.a << 1) | (old_bit7 ? 1 : 0));
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow)) |
                (old_bit7 ? kFlagCarry : 0x00));
            result.cycles = 4;
            result.message = "RLCA";
            break;
        }
        case 0x04: {
            const auto operand = registers_.b;
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            registers_.b = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "INC B";
            break;
        }
        case 0x05: {
            const auto operand = registers_.b;
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            registers_.b = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "DEC B";
            break;
        }
        case 0x06:
            registers_.b = fetch8();
            result.length = 2;
            result.cycles = 7;
            result.message = "LD B,n";
            break;
        case 0x09: {
            const auto lhs = hl();
            const auto rhs = bc();
            const auto wide = static_cast<std::uint32_t>(lhs) + rhs;
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow));
            if (((lhs & 0x0FFF) + (rhs & 0x0FFF)) > 0x0FFF) {
                flags |= kFlagHalfCarry;
            }
            if (wide > 0xFFFF) {
                flags |= kFlagCarry;
            }
            set_hl(static_cast<std::uint16_t>(wide));
            registers_.f = flags;
            result.cycles = 11;
            result.message = "ADD HL,BC";
            break;
        }
        case 0x0A:
            registers_.a = read8(bc());
            result.cycles = 7;
            result.message = "LD A,(BC)";
            break;
        case 0x0F: {
            const bool old_bit0 = (registers_.a & 0x01) != 0;
            registers_.a = static_cast<std::uint8_t>((registers_.a >> 1) | (old_bit0 ? 0x80 : 0x00));
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow)) |
                (old_bit0 ? kFlagCarry : 0x00));
            result.cycles = 4;
            result.message = "RRCA";
            break;
        }
        case 0x0C: {
            const auto operand = registers_.c;
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            registers_.c = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "INC C";
            break;
        }
        case 0x0D: {
            const auto operand = registers_.c;
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            registers_.c = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "DEC C";
            break;
        }
        case 0x03:
            set_bc(static_cast<std::uint16_t>(bc() + 1));
            result.cycles = 6;
            result.message = "INC BC";
            break;
        case 0x0E:
            registers_.c = fetch8();
            result.length = 2;
            result.cycles = 7;
            result.message = "LD C,n";
            break;
        case 0x0B:
            set_bc(static_cast<std::uint16_t>(bc() - 1));
            result.cycles = 6;
            result.message = "DEC BC";
            break;
        case 0x10: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            registers_.b = static_cast<std::uint8_t>(registers_.b - 1);
            result.length = 2;
            if (registers_.b != 0) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc + displacement);
                result.cycles = 13;
                result.message = "DJNZ e";
            } else {
                result.cycles = 8;
                result.message = "DJNZ e (not taken)";
            }
            break;
        }
        case 0x11:
            set_de(fetch16());
            result.length = 3;
            result.cycles = 10;
            result.message = "LD DE,nn";
            break;
        case 0x12:
            write8(de(), registers_.a);
            result.cycles = 7;
            result.message = "LD (DE),A";
            break;
        case 0x14: {
            const auto operand = registers_.d;
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            registers_.d = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "INC D";
            break;
        }
        case 0x15: {
            const auto operand = registers_.d;
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            registers_.d = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "DEC D";
            break;
        }
        case 0x13:
            set_de(static_cast<std::uint16_t>(de() + 1));
            result.cycles = 6;
            result.message = "INC DE";
            break;
        case 0x1B:
            set_de(static_cast<std::uint16_t>(de() - 1));
            result.cycles = 6;
            result.message = "DEC DE";
            break;
        case 0x16:
            registers_.d = fetch8();
            result.length = 2;
            result.cycles = 7;
            result.message = "LD D,n";
            break;
        case 0x18: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            registers_.pc = static_cast<std::uint16_t>(registers_.pc + displacement);
            result.length = 2;
            result.cycles = 12;
            result.message = "JR e";
            break;
        }
        case 0x19: {
            const auto lhs = hl();
            const auto rhs = de();
            const auto wide = static_cast<std::uint32_t>(lhs) + rhs;
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow));
            if (((lhs & 0x0FFF) + (rhs & 0x0FFF)) > 0x0FFF) {
                flags |= kFlagHalfCarry;
            }
            if (wide > 0xFFFF) {
                flags |= kFlagCarry;
            }
            set_hl(static_cast<std::uint16_t>(wide));
            registers_.f = flags;
            result.cycles = 11;
            result.message = "ADD HL,DE";
            break;
        }
        case 0x17: {
            const bool old_bit7 = (registers_.a & 0x80) != 0;
            const bool carry_in = carry_flag();
            registers_.a = static_cast<std::uint8_t>((registers_.a << 1) | (carry_in ? 1 : 0));
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow)) |
                (old_bit7 ? kFlagCarry : 0x00));
            result.cycles = 4;
            result.message = "RLA";
            break;
        }
        case 0x1A:
            registers_.a = read8(de());
            result.cycles = 7;
            result.message = "LD A,(DE)";
            break;
        case 0x1F: {
            const bool old_bit0 = (registers_.a & 0x01) != 0;
            const bool carry_in = carry_flag();
            registers_.a = static_cast<std::uint8_t>((registers_.a >> 1) | (carry_in ? 0x80 : 0x00));
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow)) |
                (old_bit0 ? kFlagCarry : 0x00));
            result.cycles = 4;
            result.message = "RRA";
            break;
        }
        case 0x20: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            result.length = 2;
            if (!zero_flag()) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc + displacement);
                result.cycles = 12;
                result.message = "JR NZ,e";
            } else {
                result.cycles = 7;
                result.message = "JR NZ,e (not taken)";
            }
            break;
        }
        case 0x1E:
            registers_.e = fetch8();
            result.length = 2;
            result.cycles = 7;
            result.message = "LD E,n";
            break;
        case 0x1C: {
            const auto operand = registers_.e;
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            registers_.e = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "INC E";
            break;
        }
        case 0x1D: {
            const auto operand = registers_.e;
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            registers_.e = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "DEC E";
            break;
        }
        case 0x21:
            set_hl(fetch16());
            result.length = 3;
            result.cycles = 10;
            result.message = "LD HL,nn";
            break;
        case 0x23:
            set_hl(static_cast<std::uint16_t>(hl() + 1));
            result.cycles = 6;
            result.message = "INC HL";
            break;
        case 0x22: {
            const auto address = fetch16();
            write8(address, registers_.l);
            write8(static_cast<std::uint16_t>(address + 1), registers_.h);
            result.length = 3;
            result.cycles = 16;
            result.message = "LD (nn),HL";
            break;
        }
        case 0x24: {
            const auto operand = registers_.h;
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            registers_.h = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "INC H";
            break;
        }
        case 0x25: {
            const auto operand = registers_.h;
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            registers_.h = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "DEC H";
            break;
        }
        case 0x26:
            registers_.h = fetch8();
            result.length = 2;
            result.cycles = 7;
            result.message = "LD H,n";
            break;
        case 0x27: {
            // DAA decides its correction from the ORIGINAL accumulator, and
            // both the add and subtract paths use the same two conditions.
            // The half-carry result is the bit-4 difference between the value
            // before and after correction, and carry is sticky: once set it
            // stays set, and a source value above 0x99 sets it regardless of
            // whether this was an add or a subtract.
            const std::uint8_t a_before = registers_.a;
            const bool subtract = (registers_.f & kFlagSubtract) != 0;
            const bool half_carry = (registers_.f & kFlagHalfCarry) != 0;
            const bool carry = (registers_.f & kFlagCarry) != 0;

            std::uint8_t a_after = a_before;
            if (subtract) {
                if (half_carry || (a_before & 0x0F) > 0x09) {
                    a_after = static_cast<std::uint8_t>(a_after - 0x06);
                }
                if (carry || a_before > 0x99) {
                    a_after = static_cast<std::uint8_t>(a_after - 0x60);
                }
            } else {
                if (half_carry || (a_before & 0x0F) > 0x09) {
                    a_after = static_cast<std::uint8_t>(a_after + 0x06);
                }
                if (carry || a_before > 0x99) {
                    a_after = static_cast<std::uint8_t>(a_after + 0x60);
                }
            }

            std::uint8_t flags =
                static_cast<std::uint8_t>(registers_.f & (kFlagCarry | kFlagSubtract));
            if (a_before > 0x99) {
                flags |= kFlagCarry;
            }
            if (((a_before ^ a_after) & 0x10) != 0) {
                flags |= kFlagHalfCarry;
            }
            if (a_after == 0) {
                flags |= kFlagZero;
            }
            if ((a_after & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (has_even_parity(a_after)) {
                flags |= kFlagParityOverflow;
            }

            registers_.a = a_after;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "DAA";
            break;
        }
        case 0x2A: {
            const auto address = fetch16();
            const auto low = read8(address);
            const auto high = read8(static_cast<std::uint16_t>(address + 1));
            set_hl(static_cast<std::uint16_t>((high << 8) | low));
            result.length = 3;
            result.cycles = 16;
            result.message = "LD HL,(nn)";
            break;
        }
        case 0x2B:
            set_hl(static_cast<std::uint16_t>(hl() - 1));
            result.cycles = 6;
            result.message = "DEC HL";
            break;
        case 0x2E:
            registers_.l = fetch8();
            result.length = 2;
            result.cycles = 7;
            result.message = "LD L,n";
            break;
        case 0x2F:
            registers_.a = static_cast<std::uint8_t>(~registers_.a);
            registers_.f = static_cast<std::uint8_t>((registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow | kFlagCarry)) |
                                                     kFlagHalfCarry |
                                                     kFlagSubtract);
            result.cycles = 4;
            result.message = "CPL";
            break;
        case 0x2C: {
            const auto operand = registers_.l;
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            registers_.l = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "INC L";
            break;
        }
        case 0x2D: {
            const auto operand = registers_.l;
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            registers_.l = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "DEC L";
            break;
        }
        case 0x29: {
            const auto lhs = hl();
            const auto rhs = hl();
            const auto wide = static_cast<std::uint32_t>(lhs) + rhs;
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow));
            if (((lhs & 0x0FFF) + (rhs & 0x0FFF)) > 0x0FFF) {
                flags |= kFlagHalfCarry;
            }
            if (wide > 0xFFFF) {
                flags |= kFlagCarry;
            }
            set_hl(static_cast<std::uint16_t>(wide));
            registers_.f = flags;
            result.cycles = 11;
            result.message = "ADD HL,HL";
            break;
        }
        case 0x31:
            registers_.sp = fetch16();
            result.length = 3;
            result.cycles = 10;
            result.message = "LD SP,nn";
            break;
        case 0x32: {
            const auto address = fetch16();
            write8(address, registers_.a);
            result.length = 3;
            result.cycles = 13;
            result.message = "LD (nn),A";
            break;
        }
        case 0x34: {
            const auto operand = read8(hl());
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            write8(hl(), value);
            registers_.f = flags;
            result.cycles = 11;
            result.message = "INC (HL)";
            break;
        }
        case 0x35: {
            const auto operand = read8(hl());
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            write8(hl(), value);
            registers_.f = flags;
            result.cycles = 11;
            result.message = "DEC (HL)";
            break;
        }
        case 0x36:
            write8(hl(), fetch8());
            result.length = 2;
            result.cycles = 10;
            result.message = "LD (HL),n";
            break;
        case 0x33:
            registers_.sp = static_cast<std::uint16_t>(registers_.sp + 1);
            result.cycles = 6;
            result.message = "INC SP";
            break;
        case 0x3B:
            registers_.sp = static_cast<std::uint16_t>(registers_.sp - 1);
            result.cycles = 6;
            result.message = "DEC SP";
            break;
        case 0x37:
            registers_.f = static_cast<std::uint8_t>((registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow)) |
                                                     kFlagCarry);
            result.cycles = 4;
            result.message = "SCF";
            break;
        case 0x3F: {
            const bool old_carry = (registers_.f & kFlagCarry) != 0;
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow)) |
                (old_carry ? kFlagHalfCarry : 0x00) |
                (old_carry ? 0x00 : kFlagCarry));
            result.cycles = 4;
            result.message = "CCF";
            break;
        }
        case 0x3A: {
            const auto address = fetch16();
            registers_.a = read8(address);
            result.length = 3;
            result.cycles = 13;
            result.message = "LD A,(nn)";
            break;
        }
        case 0x39: {
            const auto lhs = hl();
            const auto rhs = registers_.sp;
            const auto wide = static_cast<std::uint32_t>(lhs) + rhs;
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow));
            if (((lhs & 0x0FFF) + (rhs & 0x0FFF)) > 0x0FFF) {
                flags |= kFlagHalfCarry;
            }
            if (wide > 0xFFFF) {
                flags |= kFlagCarry;
            }
            set_hl(static_cast<std::uint16_t>(wide));
            registers_.f = flags;
            result.cycles = 11;
            result.message = "ADD HL,SP";
            break;
        }
        case 0x3E:
            registers_.a = fetch8();
            result.length = 2;
            result.cycles = 7;
            result.message = "LD A,n";
            break;
        case 0x3C: {
            const auto operand = registers_.a;
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            registers_.a = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "INC A";
            break;
        }
        case 0x3D: {
            const auto operand = registers_.a;
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            registers_.a = value;
            registers_.f = flags;
            result.cycles = 4;
            result.message = "DEC A";
            break;
        }
        case 0x28: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            result.length = 2;
            if (zero_flag()) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc + displacement);
                result.cycles = 12;
                result.message = "JR Z,e";
            } else {
                result.cycles = 7;
                result.message = "JR Z,e (not taken)";
            }
            break;
        }
        case 0x30: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            result.length = 2;
            if (!carry_flag()) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc + displacement);
                result.cycles = 12;
                result.message = "JR NC,e";
            } else {
                result.cycles = 7;
                result.message = "JR NC,e (not taken)";
            }
            break;
        }
        case 0x38: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            result.length = 2;
            if (carry_flag()) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc + displacement);
                result.cycles = 12;
                result.message = "JR C,e";
            } else {
                result.cycles = 7;
                result.message = "JR C,e (not taken)";
            }
            break;
        }
        case 0x70:
            write8(hl(), registers_.b);
            result.cycles = 7;
            result.message = "LD (HL),B";
            break;
        case 0x71:
            write8(hl(), registers_.c);
            result.cycles = 7;
            result.message = "LD (HL),C";
            break;
        case 0x72:
            write8(hl(), registers_.d);
            result.cycles = 7;
            result.message = "LD (HL),D";
            break;
        case 0x73:
            write8(hl(), registers_.e);
            result.cycles = 7;
            result.message = "LD (HL),E";
            break;
        case 0x74:
            write8(hl(), registers_.h);
            result.cycles = 7;
            result.message = "LD (HL),H";
            break;
        case 0x75:
            write8(hl(), registers_.l);
            result.cycles = 7;
            result.message = "LD (HL),L";
            break;
        case 0x77:
            write8(hl(), registers_.a);
            result.cycles = 7;
            result.message = "LD (HL),A";
            break;
        case 0x78:
            registers_.a = registers_.b;
            result.cycles = 4;
            result.message = "LD A,B";
            break;
        case 0x79:
            registers_.a = registers_.c;
            result.cycles = 4;
            result.message = "LD A,C";
            break;
        case 0xAF:
            registers_.a = 0x00;
            registers_.f = static_cast<std::uint8_t>(Z80Cpu::kFlagZero | Z80Cpu::kFlagParityOverflow);
            result.cycles = 4;
            result.message = "XOR A";
            break;
        case 0xC6: {
            const auto operand = fetch8();
            const auto lhs = registers_.a;
            const auto value = static_cast<std::uint8_t>(lhs + operand);
            registers_.a = value;
            set_add_flags(lhs, operand, 0, value);
            result.length = 2;
            result.cycles = 7;
            result.message = "ADD A,n";
            break;
        }
        case 0xC3:
            registers_.pc = fetch16();
            result.length = 3;
            result.cycles = 10;
            result.message = "JP nn";
            break;
        case 0xC0:
            if (!zero_flag()) {
                registers_.pc = pop16();
                result.cycles = 11;
                result.message = "RET NZ";
            } else {
                result.cycles = 5;
                result.message = "RET NZ (not taken)";
            }
            break;
        case 0xC2: {
            const auto target = fetch16();
            result.length = 3;
            if (!zero_flag()) {
                registers_.pc = target;
                result.cycles = 10;
                result.message = "JP NZ,nn";
            } else {
                result.cycles = 10;
                result.message = "JP NZ,nn (not taken)";
            }
            break;
        }
        case 0xCA: {
            const auto target = fetch16();
            result.length = 3;
            if (zero_flag()) {
                registers_.pc = target;
                result.cycles = 10;
                result.message = "JP Z,nn";
            } else {
                result.cycles = 10;
                result.message = "JP Z,nn (not taken)";
            }
            break;
        }
        case 0xC4: {
            const auto target = fetch16();
            result.length = 3;
            if (!zero_flag()) {
                push16(registers_.pc);
                registers_.pc = target;
                result.cycles = 17;
                result.message = "CALL NZ,nn";
            } else {
                result.cycles = 10;
                result.message = "CALL NZ,nn (not taken)";
            }
            break;
        }
        case 0xC9:
            registers_.pc = pop16();
            result.cycles = 10;
            result.message = "RET";
            break;
        case 0xC7:
            push16(registers_.pc);
            registers_.pc = 0x0000;
            result.cycles = 11;
            result.message = "RST 00H";
            break;
        case 0xC1:
            set_bc(pop16());
            result.cycles = 10;
            result.message = "POP BC";
            break;
        case 0xCD: {
            const auto target = fetch16();
            push16(registers_.pc);
            registers_.pc = target;
            result.length = 3;
            result.cycles = 17;
            result.message = "CALL nn";
            break;
        }
        case 0xC5:
            push16(bc());
            result.cycles = 11;
            result.message = "PUSH BC";
            break;
        case 0xCE: {
            const auto operand = fetch8();
            const auto carry_in = static_cast<std::uint8_t>((registers_.f & kFlagCarry) != 0 ? 1 : 0);
            const auto lhs = registers_.a;
            const auto value = static_cast<std::uint8_t>(lhs + operand + carry_in);
            registers_.a = value;
            set_add_flags(lhs, operand, carry_in, value);
            result.length = 2;
            result.cycles = 7;
            result.message = "ADC A,n";
            break;
        }
        case 0xCF:
            push16(registers_.pc);
            registers_.pc = 0x0008;
            result.cycles = 11;
            result.message = "RST 08H";
            break;
        case 0xC8:
            if (zero_flag()) {
                registers_.pc = pop16();
                result.cycles = 11;
                result.message = "RET Z";
            } else {
                result.cycles = 5;
                result.message = "RET Z (not taken)";
            }
            break;
        case 0xCC: {
            const auto target = fetch16();
            result.length = 3;
            if (zero_flag()) {
                push16(registers_.pc);
                registers_.pc = target;
                result.cycles = 17;
                result.message = "CALL Z,nn";
            } else {
                result.cycles = 10;
                result.message = "CALL Z,nn (not taken)";
            }
            break;
        }
        case 0xD3: {
            const auto port = fetch8();
            out(port, registers_.a);
            result.length = 2;
            result.cycles = 11;
            result.message = "OUT (n),A";
            break;
        }
        case 0xD0:
            if (!carry_flag()) {
                registers_.pc = pop16();
                result.cycles = 11;
                result.message = "RET NC";
            } else {
                result.cycles = 5;
                result.message = "RET NC (not taken)";
            }
            break;
        case 0xD2: {
            const auto target = fetch16();
            result.length = 3;
            if (!carry_flag()) {
                registers_.pc = target;
                result.cycles = 10;
                result.message = "JP NC,nn";
            } else {
                result.cycles = 10;
                result.message = "JP NC,nn (not taken)";
            }
            break;
        }
        case 0xD7:
            push16(registers_.pc);
            registers_.pc = 0x0010;
            result.cycles = 11;
            result.message = "RST 10H";
            break;
        case 0xD4: {
            const auto target = fetch16();
            result.length = 3;
            if (!carry_flag()) {
                push16(registers_.pc);
                registers_.pc = target;
                result.cycles = 17;
                result.message = "CALL NC,nn";
            } else {
                result.cycles = 10;
                result.message = "CALL NC,nn (not taken)";
            }
            break;
        }
        case 0xD6: {
            const auto operand = fetch8();
            const auto lhs = registers_.a;
            const auto value = static_cast<std::uint8_t>(lhs - operand);
            registers_.a = value;
            set_subtract_flags(lhs, operand, 0, value);
            result.length = 2;
            result.cycles = 7;
            result.message = "SUB n";
            break;
        }
        case 0xDB: {
            const auto port = fetch8();
            registers_.a = in(port);
            result.length = 2;
            result.cycles = 11;
            result.message = "IN A,(n)";
            break;
        }
        case 0xD1:
            set_de(pop16());
            result.cycles = 10;
            result.message = "POP DE";
            break;
        case 0xD5:
            push16(de());
            result.cycles = 11;
            result.message = "PUSH DE";
            break;
        case 0xDE: {
            const auto operand = fetch8();
            const auto carry_in = static_cast<std::uint8_t>((registers_.f & kFlagCarry) != 0 ? 1 : 0);
            const auto lhs = registers_.a;
            const auto value = static_cast<std::uint8_t>(lhs - operand - carry_in);
            registers_.a = value;
            set_subtract_flags(lhs, operand, carry_in, value);
            result.length = 2;
            result.cycles = 7;
            result.message = "SBC A,n";
            break;
        }
        case 0xDF:
            push16(registers_.pc);
            registers_.pc = 0x0018;
            result.cycles = 11;
            result.message = "RST 18H";
            break;
        case 0xD8:
            if (carry_flag()) {
                registers_.pc = pop16();
                result.cycles = 11;
                result.message = "RET C";
            } else {
                result.cycles = 5;
                result.message = "RET C (not taken)";
            }
            break;
        case 0xD9:
            std::swap(registers_.b, shadow_registers_.b);
            std::swap(registers_.c, shadow_registers_.c);
            std::swap(registers_.d, shadow_registers_.d);
            std::swap(registers_.e, shadow_registers_.e);
            std::swap(registers_.h, shadow_registers_.h);
            std::swap(registers_.l, shadow_registers_.l);
            result.cycles = 4;
            result.message = "EXX";
            break;
        case 0xDA: {
            const auto target = fetch16();
            result.length = 3;
            if (carry_flag()) {
                registers_.pc = target;
                result.cycles = 10;
                result.message = "JP C,nn";
            } else {
                result.cycles = 10;
                result.message = "JP C,nn (not taken)";
            }
            break;
        }
        case 0xDC: {
            const auto target = fetch16();
            result.length = 3;
            if (carry_flag()) {
                push16(registers_.pc);
                registers_.pc = target;
                result.cycles = 17;
                result.message = "CALL C,nn";
            } else {
                result.cycles = 10;
                result.message = "CALL C,nn (not taken)";
            }
            break;
        }
        case 0xE6: {
            const auto operand = fetch8();
            registers_.a = static_cast<std::uint8_t>(registers_.a & operand);
            set_logic_flags(registers_.a, true);
            result.length = 2;
            result.cycles = 7;
            result.message = "AND n";
            break;
        }
        case 0xE7:
            push16(registers_.pc);
            registers_.pc = 0x0020;
            result.cycles = 11;
            result.message = "RST 20H";
            break;
        case 0xEE: {
            const auto operand = fetch8();
            registers_.a = static_cast<std::uint8_t>(registers_.a ^ operand);
            set_logic_flags(registers_.a, false);
            result.length = 2;
            result.cycles = 7;
            result.message = "XOR n";
            break;
        }
        case 0xEF:
            push16(registers_.pc);
            registers_.pc = 0x0028;
            result.cycles = 11;
            result.message = "RST 28H";
            break;
        case 0xE1:
            set_hl(pop16());
            result.cycles = 10;
            result.message = "POP HL";
            break;
        case 0xE3: {
            const auto low = read8(registers_.sp);
            const auto high = read8(static_cast<std::uint16_t>(registers_.sp + 1));
            const auto stack_value = static_cast<std::uint16_t>((high << 8) | low);
            write8(registers_.sp, registers_.l);
            write8(static_cast<std::uint16_t>(registers_.sp + 1), registers_.h);
            set_hl(stack_value);
            result.cycles = 19;
            result.message = "EX (SP),HL";
            break;
        }
        case 0xE0:
            if (!parity_overflow_flag()) {
                registers_.pc = pop16();
                result.cycles = 11;
                result.message = "RET PO";
            } else {
                result.cycles = 5;
                result.message = "RET PO (not taken)";
            }
            break;
        case 0xE2: {
            const auto target = fetch16();
            result.length = 3;
            if (!parity_overflow_flag()) {
                registers_.pc = target;
                result.cycles = 10;
                result.message = "JP PO,nn";
            } else {
                result.cycles = 10;
                result.message = "JP PO,nn (not taken)";
            }
            break;
        }
        case 0xE4: {
            const auto target = fetch16();
            result.length = 3;
            if (!parity_overflow_flag()) {
                push16(registers_.pc);
                registers_.pc = target;
                result.cycles = 17;
                result.message = "CALL PO,nn";
            } else {
                result.cycles = 10;
                result.message = "CALL PO,nn (not taken)";
            }
            break;
        }
        case 0xE5:
            push16(hl());
            result.cycles = 11;
            result.message = "PUSH HL";
            break;
        case 0xE8:
            if (parity_overflow_flag()) {
                registers_.pc = pop16();
                result.cycles = 11;
                result.message = "RET PE";
            } else {
                result.cycles = 5;
                result.message = "RET PE (not taken)";
            }
            break;
        case 0xE9:
            registers_.pc = hl();
            result.cycles = 4;
            result.message = "JP (HL)";
            break;
        case 0xEB: {
            const auto old_de = de();
            set_de(hl());
            set_hl(old_de);
            result.cycles = 4;
            result.message = "EX DE,HL";
            break;
        }
        case 0xEA: {
            const auto target = fetch16();
            result.length = 3;
            if (parity_overflow_flag()) {
                registers_.pc = target;
                result.cycles = 10;
                result.message = "JP PE,nn";
            } else {
                result.cycles = 10;
                result.message = "JP PE,nn (not taken)";
            }
            break;
        }
        case 0xEC: {
            const auto target = fetch16();
            result.length = 3;
            if (parity_overflow_flag()) {
                push16(registers_.pc);
                registers_.pc = target;
                result.cycles = 17;
                result.message = "CALL PE,nn";
            } else {
                result.cycles = 10;
                result.message = "CALL PE,nn (not taken)";
            }
            break;
        }
        case 0xFE: {
            const auto value = fetch8();
            set_compare_flags(registers_.a, value);
            result.length = 2;
            result.cycles = 7;
            result.message = "CP n";
            break;
        }
        case 0xCB:
            result = step_cb_prefixed();
            result.pc_before = step_pc_before;
            break;
        case 0xED:
            result = step_ed_prefixed();
            result.pc_before = step_pc_before;
            break;
        case 0xF3:
            iff1_ = false;
            iff2_ = false;
            ei_delay_ = 0;
            result.cycles = 4;
            result.message = "DI";
            break;
        case 0xF1:
            set_af(pop16());
            result.cycles = 10;
            result.message = "POP AF";
            break;
        case 0xF0:
            if (!sign_flag()) {
                registers_.pc = pop16();
                result.cycles = 11;
                result.message = "RET P";
            } else {
                result.cycles = 5;
                result.message = "RET P (not taken)";
            }
            break;
        case 0xF2: {
            const auto target = fetch16();
            result.length = 3;
            if (!sign_flag()) {
                registers_.pc = target;
                result.cycles = 10;
                result.message = "JP P,nn";
            } else {
                result.cycles = 10;
                result.message = "JP P,nn (not taken)";
            }
            break;
        }
        case 0xF4: {
            const auto target = fetch16();
            result.length = 3;
            if (!sign_flag()) {
                push16(registers_.pc);
                registers_.pc = target;
                result.cycles = 17;
                result.message = "CALL P,nn";
            } else {
                result.cycles = 10;
                result.message = "CALL P,nn (not taken)";
            }
            break;
        }
        case 0xF5:
            push16(af());
            result.cycles = 11;
            result.message = "PUSH AF";
            break;
        case 0xF6: {
            const auto operand = fetch8();
            registers_.a = static_cast<std::uint8_t>(registers_.a | operand);
            set_logic_flags(registers_.a, false);
            result.length = 2;
            result.cycles = 7;
            result.message = "OR n";
            break;
        }
        case 0xF7:
            push16(registers_.pc);
            registers_.pc = 0x0030;
            result.cycles = 11;
            result.message = "RST 30H";
            break;
        case 0xF8:
            if (sign_flag()) {
                registers_.pc = pop16();
                result.cycles = 11;
                result.message = "RET M";
            } else {
                result.cycles = 5;
                result.message = "RET M (not taken)";
            }
            break;
        case 0xFB:
            ei_delay_ = 2;
            result.cycles = 4;
            result.message = "EI";
            break;
        case 0xFA: {
            const auto target = fetch16();
            result.length = 3;
            if (sign_flag()) {
                registers_.pc = target;
                result.cycles = 10;
                result.message = "JP M,nn";
            } else {
                result.cycles = 10;
                result.message = "JP M,nn (not taken)";
            }
            break;
        }
        case 0xFC: {
            const auto target = fetch16();
            result.length = 3;
            if (sign_flag()) {
                push16(registers_.pc);
                registers_.pc = target;
                result.cycles = 17;
                result.message = "CALL M,nn";
            } else {
                result.cycles = 10;
                result.message = "CALL M,nn (not taken)";
            }
            break;
        }
        case 0xFF:
            push16(registers_.pc);
            registers_.pc = 0x0038;
            result.cycles = 11;
            result.message = "RST 38H";
            break;
        case 0xF9:
            registers_.sp = hl();
            result.cycles = 6;
            result.message = "LD SP,HL";
            break;
        case 0x76:
            halted_ = true;
            result.cycles = 4;
            result.message = "HALT";
            break;
        default:
            result.unimplemented = true;
            result.cycles = 0;
            result.message = "Unimplemented opcode 0x" + hex8(opcode);
            break;
    }

    result.pc_after = registers_.pc;
    finish_step();
    return result;
}

void Z80Cpu::request_maskable_interrupt() noexcept {
    maskable_interrupt_pending_ = true;
}

void Z80Cpu::set_maskable_interrupt_line(const bool active) noexcept {
    maskable_interrupt_line_ = active;
}

void Z80Cpu::request_non_maskable_interrupt() noexcept {
    non_maskable_interrupt_pending_ = true;
}

const Z80Registers& Z80Cpu::registers() const noexcept {
    return registers_;
}

Z80InterruptMode Z80Cpu::interrupt_mode() const noexcept {
    return interrupt_mode_;
}

bool Z80Cpu::interrupts_enabled() const noexcept {
    return iff1_;
}

bool Z80Cpu::halted() const noexcept {
    return halted_;
}

std::string Z80Cpu::describe() const {
    std::ostringstream out;
    out << "Z80: PC=" << hex16(registers_.pc)
        << " SP=" << hex16(registers_.sp)
        << " AF=" << hex8(registers_.a) << hex8(registers_.f)
        << " BC=" << hex16(bc())
        << " DE=" << hex16(de())
        << " HL=" << hex16(hl())
        << " IM=" << static_cast<int>(interrupt_mode_)
        << " IFF1=" << iff1_
        << " IFF2=" << iff2_
        << " HALT=" << halted_;
    return out.str();
}

bool Z80Cpu::ready() const noexcept {
    return static_cast<bool>(callbacks_.read8) &&
           static_cast<bool>(callbacks_.write8) &&
           static_cast<bool>(callbacks_.in) &&
           static_cast<bool>(callbacks_.out);
}

std::uint8_t Z80Cpu::read8(std::uint16_t address) noexcept {
    return callbacks_.read8 ? callbacks_.read8(address) : 0xFF;
}

void Z80Cpu::write8(std::uint16_t address, std::uint8_t value) noexcept {
    if (callbacks_.write8) {
        callbacks_.write8(address, value);
    }
}

std::uint8_t Z80Cpu::in(std::uint8_t port) noexcept {
    if (callbacks_.io_wait_cycles) {
        const auto wait_cycles = callbacks_.io_wait_cycles(port, false);
        pending_wait_cycles_ += wait_cycles;
        if (wait_cycles != 0 && callbacks_.advance_io_timing) {
            callbacks_.advance_io_timing(wait_cycles);
            applied_io_wait_cycles_ += wait_cycles;
        }
    }

    // Run the devices forward to the point inside this instruction where the
    // read samples the bus. Without this the whole instruction is executed
    // against a frozen device state and every flag transition lands exactly on
    // an instruction boundary, where a pending interrupt always preempts the
    // read - so a game polling a status port can never win the race. Counted
    // as already-applied, not as extra wait states, so the instruction's total
    // length is unchanged.
    if (callbacks_.io_read_phase_cycles && callbacks_.advance_io_timing) {
        const auto phase_cycles = callbacks_.io_read_phase_cycles();
        if (phase_cycles != 0) {
            callbacks_.advance_io_timing(phase_cycles);
            applied_io_wait_cycles_ += phase_cycles;
        }
    }

    return callbacks_.in ? callbacks_.in(port) : 0xFF;
}

void Z80Cpu::out(std::uint8_t port, std::uint8_t value) noexcept {
    if (callbacks_.io_wait_cycles) {
        const auto wait_cycles = callbacks_.io_wait_cycles(port, true);
        pending_wait_cycles_ += wait_cycles;
        if (wait_cycles != 0 && callbacks_.advance_io_timing) {
            callbacks_.advance_io_timing(wait_cycles);
            applied_io_wait_cycles_ += wait_cycles;
        }
    }
    if (callbacks_.out) {
        callbacks_.out(port, value);
    }
}

std::uint32_t Z80Cpu::take_pending_wait_cycles() noexcept {
    const auto wait_cycles = pending_wait_cycles_;
    pending_wait_cycles_ = 0;
    return wait_cycles;
}

std::uint32_t Z80Cpu::take_applied_io_wait_cycles() noexcept {
    const auto wait_cycles = applied_io_wait_cycles_;
    applied_io_wait_cycles_ = 0;
    return wait_cycles;
}

std::uint8_t Z80Cpu::fetch8() noexcept {
    const auto value = read8(registers_.pc);
    registers_.pc = static_cast<std::uint16_t>(registers_.pc + 1);
    // R increments only the low 7 bits each fetch; bit 7 is preserved
    // across the increment so that explicit writes to R remain stable.
    registers_.r = static_cast<std::uint8_t>((registers_.r & 0x80) |
                                             ((registers_.r + 1) & 0x7F));
    return value;
}

std::uint16_t Z80Cpu::fetch16() noexcept {
    const auto low = fetch8();
    const auto high = fetch8();
    return static_cast<std::uint16_t>((high << 8) | low);
}

std::uint16_t* Z80Cpu::select_index_register(std::uint8_t prefix) noexcept {
    switch (prefix) {
        case 0xDD:
            return &registers_.ix;
        case 0xFD:
            return &registers_.iy;
        default:
            return nullptr;
    }
}

std::uint16_t Z80Cpu::indexed_address(std::uint8_t prefix, std::int8_t displacement) noexcept {
    const auto* index = select_index_register(prefix);
    if (index == nullptr) {
        return 0x0000;
    }

    return static_cast<std::uint16_t>(*index + displacement);
}

std::uint16_t Z80Cpu::hl() const noexcept {
    return static_cast<std::uint16_t>((registers_.h << 8) | registers_.l);
}

void Z80Cpu::set_hl(std::uint16_t value) noexcept {
    registers_.h = static_cast<std::uint8_t>(value >> 8);
    registers_.l = static_cast<std::uint8_t>(value & 0xFF);
}

std::uint16_t Z80Cpu::bc() const noexcept {
    return static_cast<std::uint16_t>((registers_.b << 8) | registers_.c);
}

void Z80Cpu::set_bc(std::uint16_t value) noexcept {
    registers_.b = static_cast<std::uint8_t>(value >> 8);
    registers_.c = static_cast<std::uint8_t>(value & 0xFF);
}

std::uint16_t Z80Cpu::de() const noexcept {
    return static_cast<std::uint16_t>((registers_.d << 8) | registers_.e);
}

void Z80Cpu::set_de(std::uint16_t value) noexcept {
    registers_.d = static_cast<std::uint8_t>(value >> 8);
    registers_.e = static_cast<std::uint8_t>(value & 0xFF);
}

std::uint16_t Z80Cpu::af() const noexcept {
    return static_cast<std::uint16_t>((registers_.a << 8) | registers_.f);
}

void Z80Cpu::set_af(std::uint16_t value) noexcept {
    registers_.a = static_cast<std::uint8_t>(value >> 8);
    registers_.f = static_cast<std::uint8_t>(value & 0xFF);
}

std::uint8_t Z80Cpu::read_alu_operand(std::uint8_t selector) noexcept {
    switch (selector & 0x07) {
        case 0x00:
            return registers_.b;
        case 0x01:
            return registers_.c;
        case 0x02:
            return registers_.d;
        case 0x03:
            return registers_.e;
        case 0x04:
            return registers_.h;
        case 0x05:
            return registers_.l;
        case 0x06:
            return read8(hl());
        case 0x07:
            return registers_.a;
        default:
            return 0xFF;
    }
}

bool Z80Cpu::zero_flag() const noexcept {
    return (registers_.f & kFlagZero) != 0;
}

bool Z80Cpu::carry_flag() const noexcept {
    return (registers_.f & kFlagCarry) != 0;
}

bool Z80Cpu::parity_overflow_flag() const noexcept {
    return (registers_.f & kFlagParityOverflow) != 0;
}

bool Z80Cpu::sign_flag() const noexcept {
    return (registers_.f & kFlagSign) != 0;
}

bool Z80Cpu::condition_holds(std::uint8_t condition_code) const noexcept {
    switch (condition_code & 0x07) {
        case 0x00:
            return !zero_flag();
        case 0x01:
            return zero_flag();
        case 0x02:
            return !carry_flag();
        case 0x03:
            return carry_flag();
        case 0x04:
            return !parity_overflow_flag();
        case 0x05:
            return parity_overflow_flag();
        case 0x06:
            return !sign_flag();
        case 0x07:
            return sign_flag();
        default:
            return false;
    }
}

void Z80Cpu::set_compare_flags(std::uint8_t lhs, std::uint8_t rhs) noexcept {
    const auto result = static_cast<std::uint8_t>(lhs - rhs);
    std::uint8_t flags = kFlagSubtract;

    if (result == 0) {
        flags |= kFlagZero;
    }
    if ((result & 0x80) != 0) {
        flags |= kFlagSign;
    }
    if ((lhs & 0x0F) < (rhs & 0x0F)) {
        flags |= kFlagHalfCarry;
    }
    if (lhs < rhs) {
        flags |= kFlagCarry;
    }

    const auto signed_lhs = static_cast<std::int16_t>(static_cast<std::int8_t>(lhs));
    const auto signed_rhs = static_cast<std::int16_t>(static_cast<std::int8_t>(rhs));
    const auto signed_result = static_cast<std::int16_t>(signed_lhs - signed_rhs);
    if (signed_result < -128 || signed_result > 127) {
        flags |= kFlagParityOverflow;
    }

    registers_.f = flags;
}

void Z80Cpu::set_or_flags(std::uint8_t value) noexcept {
    std::uint8_t flags = 0x00;

    if (value == 0) {
        flags |= kFlagZero;
    }
    if ((value & 0x80) != 0) {
        flags |= kFlagSign;
    }
    if (has_even_parity(value)) {
        flags |= kFlagParityOverflow;
    }

    registers_.f = flags;
}

void Z80Cpu::set_add_flags(std::uint8_t lhs, std::uint8_t rhs, std::uint8_t carry_in, std::uint8_t result) noexcept {
    std::uint8_t flags = 0x00;
    const auto wide_result = static_cast<std::uint16_t>(lhs) + static_cast<std::uint16_t>(rhs) + carry_in;

    if (result == 0) {
        flags |= kFlagZero;
    }
    if ((result & 0x80) != 0) {
        flags |= kFlagSign;
    }
    if (((lhs & 0x0F) + (rhs & 0x0F) + carry_in) > 0x0F) {
        flags |= kFlagHalfCarry;
    }
    if ((~(lhs ^ rhs) & (lhs ^ result) & 0x80) != 0) {
        flags |= kFlagParityOverflow;
    }
    if (wide_result > 0xFF) {
        flags |= kFlagCarry;
    }

    registers_.f = flags;
}

void Z80Cpu::set_subtract_flags(std::uint8_t lhs, std::uint8_t rhs, std::uint8_t carry_in, std::uint8_t result) noexcept {
    std::uint8_t flags = kFlagSubtract;
    const auto full_rhs = static_cast<std::uint16_t>(rhs) + carry_in;

    if (result == 0) {
        flags |= kFlagZero;
    }
    if ((result & 0x80) != 0) {
        flags |= kFlagSign;
    }
    if ((lhs & 0x0F) < ((rhs & 0x0F) + carry_in)) {
        flags |= kFlagHalfCarry;
    }
    if (((lhs ^ rhs) & (lhs ^ result) & 0x80) != 0) {
        flags |= kFlagParityOverflow;
    }
    if (static_cast<std::uint16_t>(lhs) < full_rhs) {
        flags |= kFlagCarry;
    }

    registers_.f = flags;
}

void Z80Cpu::set_logic_flags(std::uint8_t value, bool set_half_carry) noexcept {
    std::uint8_t flags = 0x00;

    if (value == 0) {
        flags |= kFlagZero;
    }
    if ((value & 0x80) != 0) {
        flags |= kFlagSign;
    }
    if (has_even_parity(value)) {
        flags |= kFlagParityOverflow;
    }
    if (set_half_carry) {
        flags |= kFlagHalfCarry;
    }

    registers_.f = flags;
}

void Z80Cpu::push16(std::uint16_t value) noexcept {
    registers_.sp = static_cast<std::uint16_t>(registers_.sp - 1);
    write8(registers_.sp, static_cast<std::uint8_t>(value >> 8));
    registers_.sp = static_cast<std::uint16_t>(registers_.sp - 1);
    write8(registers_.sp, static_cast<std::uint8_t>(value & 0xFF));
}

std::uint16_t Z80Cpu::pop16() noexcept {
    const auto low = read8(registers_.sp);
    registers_.sp = static_cast<std::uint16_t>(registers_.sp + 1);
    const auto high = read8(registers_.sp);
    registers_.sp = static_cast<std::uint16_t>(registers_.sp + 1);
    return static_cast<std::uint16_t>((high << 8) | low);
}

void Z80Cpu::finish_step() noexcept {
    if (ei_delay_ > 0) {
        --ei_delay_;
        if (ei_delay_ == 0) {
            iff1_ = true;
            iff2_ = true;
        }
    }
}

Z80StepResult Z80Cpu::service_interrupts() noexcept {
    if (non_maskable_interrupt_pending_) {
        non_maskable_interrupt_pending_ = false;
        halted_ = false;
        // NMI saves IFF1 into IFF2 before disabling interrupts; RETN at the
        // end of the handler copies it back. Clearing IFF1 without saving it
        // meant a paused game resumed with interrupts permanently off.
        iff2_ = iff1_;
        iff1_ = false;
        const auto pc_before = registers_.pc;
        push16(registers_.pc);
        registers_.pc = 0x0066;

        return {
            .executed = true,
            .cycles = 11,
            .pc_before = pc_before,
            .pc_after = registers_.pc,
            .message = "NMI accepted",
        };
    }

    if ((maskable_interrupt_pending_ || maskable_interrupt_line_) && iff1_ && ei_delay_ == 0) {
        maskable_interrupt_pending_ = false;
        halted_ = false;
        // Per the Zilog Z80 manual: when a maskable interrupt is accepted,
        // IFF1 is cleared but IFF2 retains its current value. IFF2 is the
        // "shadow" copy that exists specifically so RETN can restore IFF1
        // after an NMI. Clearing both here was an off-by-one that made
        // LD A,I report the wrong IFF state via the PV flag, and could
        // prevent later RETN sequences from restoring IFF1 correctly.
        iff1_ = false;

        switch (interrupt_mode_) {
            case Z80InterruptMode::mode1: {
                const auto pc_before = registers_.pc;
                push16(registers_.pc);
                registers_.pc = 0x0038;
                return {
                    .executed = true,
                    .cycles = 13,
                    .pc_before = pc_before,
                    .pc_after = registers_.pc,
                    .message = "INT accepted in IM 1",
                };
            }
            case Z80InterruptMode::mode0: {
                // The interrupting device places an instruction on the data
                // bus. Nothing on the SMS/GG drives it, so it floats to FFh,
                // which is RST 38h - the same destination as IM 1.
                const auto pc_before = registers_.pc;
                push16(registers_.pc);
                registers_.pc = 0x0038;
                return {
                    .executed = true,
                    .cycles = 13,
                    .pc_before = pc_before,
                    .pc_after = registers_.pc,
                    .message = "INT accepted in IM 0",
                };
            }
            case Z80InterruptMode::mode2: {
                // Vector table lookup: the high byte comes from I, the low byte
                // from the data bus (FFh here, as above). The word at that
                // address is the handler.
                const auto pc_before = registers_.pc;
                push16(registers_.pc);
                const auto vector = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(registers_.i) << 8) | 0x00FF);
                const auto low = read8(vector);
                const auto high = read8(static_cast<std::uint16_t>(vector + 1));
                registers_.pc = static_cast<std::uint16_t>((high << 8) | low);
                return {
                    .executed = true,
                    .cycles = 19,
                    .pc_before = pc_before,
                    .pc_after = registers_.pc,
                    .message = "INT accepted in IM 2",
                };
            }
        }
    }

    return {};
}

Z80StepResult Z80Cpu::step_cb_prefixed() noexcept {
    Z80StepResult result;
    result.executed = true;
    result.prefix = 0xCB;
    result.length = 2;

    const auto opcode = fetch8();
    result.opcode = opcode;

    const auto operation = static_cast<std::uint8_t>((opcode >> 6) & 0x03);
    const auto bit_or_shift = static_cast<std::uint8_t>((opcode >> 3) & 0x07);
    const auto selector = static_cast<std::uint8_t>(opcode & 0x07);
    const bool memory_operand = selector == 0x06;

    auto read_target = [&]() -> std::uint8_t {
        switch (selector) {
            case 0x00:
                return registers_.b;
            case 0x01:
                return registers_.c;
            case 0x02:
                return registers_.d;
            case 0x03:
                return registers_.e;
            case 0x04:
                return registers_.h;
            case 0x05:
                return registers_.l;
            case 0x06:
                return read8(hl());
            case 0x07:
                return registers_.a;
            default:
                return 0xFF;
        }
    };

    auto write_target = [&](std::uint8_t value) {
        switch (selector) {
            case 0x00:
                registers_.b = value;
                break;
            case 0x01:
                registers_.c = value;
                break;
            case 0x02:
                registers_.d = value;
                break;
            case 0x03:
                registers_.e = value;
                break;
            case 0x04:
                registers_.h = value;
                break;
            case 0x05:
                registers_.l = value;
                break;
            case 0x06:
                write8(hl(), value);
                break;
            case 0x07:
                registers_.a = value;
                break;
            default:
                break;
        }
    };

    auto value = read_target();

    switch (operation) {
        case 0x00: {
            const bool old_carry = (value & 0x01) != 0;
            const bool old_msb = (value & 0x80) != 0;
            std::uint8_t result_value = value;

            switch (bit_or_shift) {
                case 0x00:
                    result_value = static_cast<std::uint8_t>((value << 1) | (old_msb ? 1 : 0));
                    result.message = "RLC r";
                    break;
                case 0x01:
                    result_value = static_cast<std::uint8_t>((value >> 1) | (old_carry ? 0x80 : 0x00));
                    result.message = "RRC r";
                    break;
                case 0x02: {
                    const bool carry_in = carry_flag();
                    result_value = static_cast<std::uint8_t>((value << 1) | (carry_in ? 1 : 0));
                    result.message = "RL r";
                    break;
                }
                case 0x03: {
                    const bool carry_in = carry_flag();
                    result_value = static_cast<std::uint8_t>((value >> 1) | (carry_in ? 0x80 : 0x00));
                    result.message = "RR r";
                    break;
                }
                case 0x04:
                    result_value = static_cast<std::uint8_t>(value << 1);
                    result.message = "SLA r";
                    break;
                case 0x05:
                    result_value = static_cast<std::uint8_t>((value >> 1) | (value & 0x80));
                    result.message = "SRA r";
                    break;
                case 0x06:
                    result_value = static_cast<std::uint8_t>((value << 1) | 0x01);
                    result.message = "SLL r";
                    break;
                case 0x07:
                    result_value = static_cast<std::uint8_t>(value >> 1);
                    result.message = "SRL r";
                    break;
                default:
                    break;
            }

            write_target(result_value);
            std::uint8_t flags = 0x00;
            if (result_value == 0) {
                flags |= kFlagZero;
            }
            if ((result_value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (has_even_parity(result_value)) {
                flags |= kFlagParityOverflow;
            }
            // The carry out is whichever bit got shifted off the end, and that
            // depends on the direction of the operation, not on its index:
            // the left-going ops (RLC/RL/SLA/SLL, even codes) push out bit 7,
            // the right-going ops (RRC/RR/SRA/SRL, odd codes) push out bit 0.
            // old_carry holds the original bit 0.
            const bool carry_out = ((bit_or_shift & 0x01) == 0) ? old_msb : old_carry;
            if (carry_out) {
                flags |= kFlagCarry;
            }
            registers_.f = flags;
            result.cycles = memory_operand ? 15 : 8;
            break;
        }
        case 0x01: {
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            flags |= kFlagHalfCarry;
            const auto mask = static_cast<std::uint8_t>(1U << bit_or_shift);
            const bool bit_set = (value & mask) != 0;
            if (!bit_set) {
                flags |= kFlagZero | kFlagParityOverflow;
            }
            if (bit_or_shift == 7 && bit_set) {
                flags |= kFlagSign;
            }
            registers_.f = flags;
            result.cycles = memory_operand ? 12 : 8;
            result.message = "BIT b,r";
            break;
        }
        case 0x02: {
            value = static_cast<std::uint8_t>(value & ~(1U << bit_or_shift));
            write_target(value);
            result.cycles = memory_operand ? 15 : 8;
            result.message = "RES b,r";
            break;
        }
        case 0x03: {
            value = static_cast<std::uint8_t>(value | (1U << bit_or_shift));
            write_target(value);
            result.cycles = memory_operand ? 15 : 8;
            result.message = "SET b,r";
            break;
        }
        default:
            result.unimplemented = true;
            result.cycles = 0;
            result.message = "Unimplemented CB-prefixed opcode 0x" + hex8(opcode);
            break;
    }

    return result;
}

Z80StepResult Z80Cpu::step_ed_prefixed() noexcept {
    Z80StepResult result;
    result.executed = true;
    result.prefix = 0xED;
    result.length = 2;

    const auto opcode = fetch8();
    result.opcode = opcode;

    switch (opcode) {
        case 0x43: {
            const auto address = fetch16();
            write8(address, registers_.c);
            write8(static_cast<std::uint16_t>(address + 1), registers_.b);
            result.length = 4;
            result.cycles = 20;
            result.message = "LD (nn),BC";
            break;
        }
        case 0x42:
        case 0x52:
        case 0x62:
        case 0x72: {
            const std::uint16_t rhs = [&]() -> std::uint16_t {
                switch (opcode) {
                    case 0x42:
                        return bc();
                    case 0x52:
                        return de();
                    case 0x62:
                        return hl();
                    case 0x72:
                        return registers_.sp;
                    default:
                        return 0;
                }
            }();
            const auto carry_in = static_cast<std::uint32_t>(carry_flag() ? 1 : 0);
            const auto lhs = hl();
            const auto wide_rhs = static_cast<std::uint32_t>(rhs) + carry_in;
            const auto wide_result = static_cast<std::uint32_t>(lhs) - wide_rhs;
            const auto result16 = static_cast<std::uint16_t>(wide_result & 0xFFFF);
            std::uint8_t flags = kFlagSubtract;
            if (result16 == 0) {
                flags |= kFlagZero;
            }
            if ((result16 & 0x8000) != 0) {
                flags |= kFlagSign;
            }
            if ((lhs & 0x0FFF) < ((rhs & 0x0FFF) + carry_in)) {
                flags |= kFlagHalfCarry;
            }
            if (((lhs ^ rhs) & (lhs ^ result16) & 0x8000) != 0) {
                flags |= kFlagParityOverflow;
            }
            if (static_cast<std::uint32_t>(lhs) < wide_rhs) {
                flags |= kFlagCarry;
            }
            set_hl(result16);
            registers_.f = flags;
            result.cycles = 15;
            result.message = "SBC HL,ss";
            break;
        }
        case 0x44:
        case 0x4C:
        case 0x54:
        case 0x5C:
        case 0x64:
        case 0x6C:
        case 0x74:
        case 0x7C: {
            const auto operand = registers_.a;
            const auto value = static_cast<std::uint8_t>(0x00 - operand);
            registers_.a = value;
            set_subtract_flags(0x00, operand, 0, value);
            result.cycles = 8;
            result.message = "NEG";
            break;
        }
        case 0x46:
        case 0x4E:
        case 0x66:
        case 0x6E:
            interrupt_mode_ = Z80InterruptMode::mode0;
            result.cycles = 8;
            result.message = "IM 0";
            break;
        case 0x4B: {
            const auto address = fetch16();
            set_bc(static_cast<std::uint16_t>(read8(address) |
                                              (static_cast<std::uint16_t>(read8(static_cast<std::uint16_t>(address + 1))) << 8)));
            result.length = 4;
            result.cycles = 20;
            result.message = "LD BC,(nn)";
            break;
        }
        case 0x4A:
        case 0x5A:
        case 0x6A:
        case 0x7A: {
            const std::uint16_t rhs = [&]() -> std::uint16_t {
                switch (opcode) {
                    case 0x4A:
                        return bc();
                    case 0x5A:
                        return de();
                    case 0x6A:
                        return hl();
                    case 0x7A:
                        return registers_.sp;
                    default:
                        return 0;
                }
            }();
            const auto carry_in = static_cast<std::uint32_t>(carry_flag() ? 1 : 0);
            const auto lhs = hl();
            const auto wide_result = static_cast<std::uint32_t>(lhs) + static_cast<std::uint32_t>(rhs) + carry_in;
            const auto result16 = static_cast<std::uint16_t>(wide_result & 0xFFFF);
            std::uint8_t flags = 0x00;
            if (result16 == 0) {
                flags |= kFlagZero;
            }
            if ((result16 & 0x8000) != 0) {
                flags |= kFlagSign;
            }
            if (((lhs & 0x0FFF) + (rhs & 0x0FFF) + carry_in) > 0x0FFF) {
                flags |= kFlagHalfCarry;
            }
            if ((~(lhs ^ rhs) & (lhs ^ result16) & 0x8000) != 0) {
                flags |= kFlagParityOverflow;
            }
            if (wide_result > 0xFFFF) {
                flags |= kFlagCarry;
            }
            set_hl(result16);
            registers_.f = flags;
            result.cycles = 15;
            result.message = "ADC HL,ss";
            break;
        }
        case 0x53: {
            const auto address = fetch16();
            write8(address, registers_.e);
            write8(static_cast<std::uint16_t>(address + 1), registers_.d);
            result.length = 4;
            result.cycles = 20;
            result.message = "LD (nn),DE";
            break;
        }
        case 0x56:
        case 0x76:
            interrupt_mode_ = Z80InterruptMode::mode1;
            result.cycles = 8;
            result.message = "IM 1";
            break;
        case 0x5B: {
            const auto address = fetch16();
            set_de(static_cast<std::uint16_t>(read8(address) |
                                              (static_cast<std::uint16_t>(read8(static_cast<std::uint16_t>(address + 1))) << 8)));
            result.length = 4;
            result.cycles = 20;
            result.message = "LD DE,(nn)";
            break;
        }
        case 0x63: {
            const auto address = fetch16();
            write8(address, registers_.l);
            write8(static_cast<std::uint16_t>(address + 1), registers_.h);
            result.length = 4;
            result.cycles = 20;
            result.message = "LD (nn),HL";
            break;
        }
        case 0x6B: {
            const auto address = fetch16();
            set_hl(static_cast<std::uint16_t>(read8(address) |
                                              (static_cast<std::uint16_t>(read8(static_cast<std::uint16_t>(address + 1))) << 8)));
            result.length = 4;
            result.cycles = 20;
            result.message = "LD HL,(nn)";
            break;
        }
        case 0x73: {
            const auto address = fetch16();
            write8(address, static_cast<std::uint8_t>(registers_.sp & 0x00FF));
            write8(static_cast<std::uint16_t>(address + 1), static_cast<std::uint8_t>(registers_.sp >> 8));
            result.length = 4;
            result.cycles = 20;
            result.message = "LD (nn),SP";
            break;
        }
        case 0x5E:
        case 0x7E:
            interrupt_mode_ = Z80InterruptMode::mode2;
            result.cycles = 8;
            result.message = "IM 2";
            break;
        case 0x7B: {
            const auto address = fetch16();
            registers_.sp = static_cast<std::uint16_t>(read8(address) |
                                                       (static_cast<std::uint16_t>(read8(static_cast<std::uint16_t>(address + 1))) << 8));
            result.length = 4;
            result.cycles = 20;
            result.message = "LD SP,(nn)";
            break;
        }
        case 0xA0: {
            const auto value = read8(hl());
            write8(de(), value);
            set_hl(static_cast<std::uint16_t>(hl() + 1));
            set_de(static_cast<std::uint16_t>(de() + 1));
            set_bc(static_cast<std::uint16_t>(bc() - 1));
            registers_.f = static_cast<std::uint8_t>(
                registers_.f & (kFlagSign | kFlagZero | kFlagCarry));
            if (bc() != 0) {
                registers_.f |= kFlagParityOverflow;
            }
            result.cycles = 16;
            result.message = "LDI";
            break;
        }
        case 0xA8: {
            const auto value = read8(hl());
            write8(de(), value);
            set_hl(static_cast<std::uint16_t>(hl() - 1));
            set_de(static_cast<std::uint16_t>(de() - 1));
            set_bc(static_cast<std::uint16_t>(bc() - 1));
            registers_.f = static_cast<std::uint8_t>(
                registers_.f & (kFlagSign | kFlagZero | kFlagCarry));
            if (bc() != 0) {
                registers_.f |= kFlagParityOverflow;
            }
            result.cycles = 16;
            result.message = "LDD";
            break;
        }
        case 0xB0: {
            const auto value = read8(hl());
            write8(de(), value);
            set_hl(static_cast<std::uint16_t>(hl() + 1));
            set_de(static_cast<std::uint16_t>(de() + 1));
            set_bc(static_cast<std::uint16_t>(bc() - 1));
            registers_.f = static_cast<std::uint8_t>(
                registers_.f & (kFlagSign | kFlagZero | kFlagCarry));
            if (bc() != 0) {
                registers_.f |= kFlagParityOverflow;
            }

            if (bc() != 0) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc - 2);
                result.cycles = 21;
                result.message = "LDIR (repeat)";
            } else {
                result.cycles = 16;
                result.message = "LDIR";
            }
            break;
        }
        case 0xB8: {
            const auto value = read8(hl());
            write8(de(), value);
            set_hl(static_cast<std::uint16_t>(hl() - 1));
            set_de(static_cast<std::uint16_t>(de() - 1));
            set_bc(static_cast<std::uint16_t>(bc() - 1));
            registers_.f = static_cast<std::uint8_t>(
                registers_.f & (kFlagSign | kFlagZero | kFlagCarry));
            if (bc() != 0) {
                registers_.f |= kFlagParityOverflow;
            }

            if (bc() != 0) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc - 2);
                result.cycles = 21;
                result.message = "LDDR (repeat)";
            } else {
                result.cycles = 16;
                result.message = "LDDR";
            }
            break;
        }
        case 0xA3: {
            out(registers_.c, read8(hl()));
            set_hl(static_cast<std::uint16_t>(hl() + 1));
            registers_.b = static_cast<std::uint8_t>(registers_.b - 1);
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & kFlagCarry) |
                (registers_.b == 0 ? kFlagZero : 0x00) |
                kFlagSubtract);
            result.cycles = 16;
            result.message = "OUTI";
            break;
        }
        case 0xAB: {
            out(registers_.c, read8(hl()));
            set_hl(static_cast<std::uint16_t>(hl() - 1));
            registers_.b = static_cast<std::uint8_t>(registers_.b - 1);
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & kFlagCarry) |
                (registers_.b == 0 ? kFlagZero : 0x00) |
                kFlagSubtract);
            result.cycles = 16;
            result.message = "OUTD";
            break;
        }
        case 0xB3: {
            out(registers_.c, read8(hl()));
            set_hl(static_cast<std::uint16_t>(hl() + 1));
            registers_.b = static_cast<std::uint8_t>(registers_.b - 1);
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & kFlagCarry) |
                (registers_.b == 0 ? kFlagZero : 0x00) |
                kFlagSubtract);

            if (registers_.b != 0) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc - 2);
                result.cycles = 21;
                result.message = "OTIR (repeat)";
            } else {
                result.cycles = 16;
                result.message = "OTIR";
            }
            break;
        }
        case 0xBB: {
            out(registers_.c, read8(hl()));
            set_hl(static_cast<std::uint16_t>(hl() - 1));
            registers_.b = static_cast<std::uint8_t>(registers_.b - 1);
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & kFlagCarry) |
                (registers_.b == 0 ? kFlagZero : 0x00) |
                kFlagSubtract);

            if (registers_.b != 0) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc - 2);
                result.cycles = 21;
                result.message = "OTDR (repeat)";
            } else {
                result.cycles = 16;
                result.message = "OTDR";
            }
            break;
        }
        case 0x47:
            registers_.i = registers_.a;
            result.cycles = 9;
            result.message = "LD I,A";
            break;
        case 0x4F:
            registers_.r = registers_.a;
            result.cycles = 9;
            result.message = "LD R,A";
            break;
        case 0x57: {
            registers_.a = registers_.i;
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (registers_.a == 0) flags |= kFlagZero;
            if ((registers_.a & 0x80) != 0) flags |= kFlagSign;
            if (iff2_) flags |= kFlagParityOverflow;
            registers_.f = flags;
            result.cycles = 9;
            result.message = "LD A,I";
            break;
        }
        case 0x5F: {
            registers_.a = registers_.r;
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (registers_.a == 0) flags |= kFlagZero;
            if ((registers_.a & 0x80) != 0) flags |= kFlagSign;
            if (iff2_) flags |= kFlagParityOverflow;
            registers_.f = flags;
            result.cycles = 9;
            result.message = "LD A,R";
            break;
        }
        case 0x67: {
            // RRD: rotate-right one nibble through A and (HL)
            const auto address = hl();
            const auto mem = read8(address);
            const std::uint8_t a_low = static_cast<std::uint8_t>(registers_.a & 0x0F);
            const std::uint8_t a_high = static_cast<std::uint8_t>(registers_.a & 0xF0);
            const std::uint8_t mem_low = static_cast<std::uint8_t>(mem & 0x0F);
            const std::uint8_t mem_high = static_cast<std::uint8_t>((mem >> 4) & 0x0F);
            const std::uint8_t new_mem = static_cast<std::uint8_t>((a_low << 4) | mem_high);
            registers_.a = static_cast<std::uint8_t>(a_high | mem_low);
            write8(address, new_mem);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (registers_.a == 0) flags |= kFlagZero;
            if ((registers_.a & 0x80) != 0) flags |= kFlagSign;
            if (has_even_parity(registers_.a)) flags |= kFlagParityOverflow;
            registers_.f = flags;
            result.cycles = 18;
            result.message = "RRD";
            break;
        }
        case 0x6F: {
            // RLD: rotate-left one nibble through A and (HL)
            const auto address = hl();
            const auto mem = read8(address);
            const std::uint8_t a_low = static_cast<std::uint8_t>(registers_.a & 0x0F);
            const std::uint8_t a_high = static_cast<std::uint8_t>(registers_.a & 0xF0);
            const std::uint8_t mem_low = static_cast<std::uint8_t>(mem & 0x0F);
            const std::uint8_t mem_high = static_cast<std::uint8_t>((mem >> 4) & 0x0F);
            const std::uint8_t new_mem = static_cast<std::uint8_t>((mem_low << 4) | a_low);
            registers_.a = static_cast<std::uint8_t>(a_high | mem_high);
            write8(address, new_mem);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (registers_.a == 0) flags |= kFlagZero;
            if ((registers_.a & 0x80) != 0) flags |= kFlagSign;
            if (has_even_parity(registers_.a)) flags |= kFlagParityOverflow;
            registers_.f = flags;
            result.cycles = 18;
            result.message = "RLD";
            break;
        }
        case 0x45:
        case 0x55:
        case 0x65:
        case 0x75: {
            // RETN: pop PC, copy IFF2 -> IFF1
            registers_.pc = pop16();
            iff1_ = iff2_;
            result.cycles = 14;
            result.message = "RETN";
            break;
        }
        case 0x4D:
        case 0x5D:
        case 0x6D:
        case 0x7D: {
            // RETI: pop PC. On a single Z80 system this is equivalent to RET
            // but is still a distinct instruction; we keep IFF state intact.
            registers_.pc = pop16();
            result.cycles = 14;
            result.message = "RETI";
            break;
        }
        case 0x40:
        case 0x48:
        case 0x50:
        case 0x58:
        case 0x60:
        case 0x68:
        case 0x70:
        case 0x78: {
            const auto port_value = in(registers_.c);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (port_value == 0) flags |= kFlagZero;
            if ((port_value & 0x80) != 0) flags |= kFlagSign;
            if (has_even_parity(port_value)) flags |= kFlagParityOverflow;
            registers_.f = flags;
            switch (opcode) {
                case 0x40: registers_.b = port_value; result.message = "IN B,(C)"; break;
                case 0x48: registers_.c = port_value; result.message = "IN C,(C)"; break;
                case 0x50: registers_.d = port_value; result.message = "IN D,(C)"; break;
                case 0x58: registers_.e = port_value; result.message = "IN E,(C)"; break;
                case 0x60: registers_.h = port_value; result.message = "IN H,(C)"; break;
                case 0x68: registers_.l = port_value; result.message = "IN L,(C)"; break;
                case 0x70: result.message = "IN (C)"; break;
                case 0x78: registers_.a = port_value; result.message = "IN A,(C)"; break;
            }
            result.cycles = 12;
            break;
        }
        case 0x41:
        case 0x49:
        case 0x51:
        case 0x59:
        case 0x61:
        case 0x69:
        case 0x71:
        case 0x79: {
            std::uint8_t value = 0x00;
            switch (opcode) {
                case 0x41: value = registers_.b; result.message = "OUT (C),B"; break;
                case 0x49: value = registers_.c; result.message = "OUT (C),C"; break;
                case 0x51: value = registers_.d; result.message = "OUT (C),D"; break;
                case 0x59: value = registers_.e; result.message = "OUT (C),E"; break;
                case 0x61: value = registers_.h; result.message = "OUT (C),H"; break;
                case 0x69: value = registers_.l; result.message = "OUT (C),L"; break;
                case 0x71: value = 0x00; result.message = "OUT (C),0"; break;
                case 0x79: value = registers_.a; result.message = "OUT (C),A"; break;
            }
            out(registers_.c, value);
            result.cycles = 12;
            break;
        }
        case 0xA1: {
            // CPI: compare A with (HL), inc HL, dec BC. Set flags.
            const auto value = read8(hl());
            const std::uint8_t cmp = static_cast<std::uint8_t>(registers_.a - value);
            set_hl(static_cast<std::uint16_t>(hl() + 1));
            set_bc(static_cast<std::uint16_t>(bc() - 1));
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            flags |= kFlagSubtract;
            if (cmp == 0) flags |= kFlagZero;
            if ((cmp & 0x80) != 0) flags |= kFlagSign;
            if ((registers_.a & 0x0F) < (value & 0x0F)) flags |= kFlagHalfCarry;
            if (bc() != 0) flags |= kFlagParityOverflow;
            registers_.f = flags;
            result.cycles = 16;
            result.message = "CPI";
            break;
        }
        case 0xA9: {
            // CPD
            const auto value = read8(hl());
            const std::uint8_t cmp = static_cast<std::uint8_t>(registers_.a - value);
            set_hl(static_cast<std::uint16_t>(hl() - 1));
            set_bc(static_cast<std::uint16_t>(bc() - 1));
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            flags |= kFlagSubtract;
            if (cmp == 0) flags |= kFlagZero;
            if ((cmp & 0x80) != 0) flags |= kFlagSign;
            if ((registers_.a & 0x0F) < (value & 0x0F)) flags |= kFlagHalfCarry;
            if (bc() != 0) flags |= kFlagParityOverflow;
            registers_.f = flags;
            result.cycles = 16;
            result.message = "CPD";
            break;
        }
        case 0xB1: {
            // CPIR: like CPI but repeat until BC=0 or A==(HL).
            const auto value = read8(hl());
            const std::uint8_t cmp = static_cast<std::uint8_t>(registers_.a - value);
            set_hl(static_cast<std::uint16_t>(hl() + 1));
            set_bc(static_cast<std::uint16_t>(bc() - 1));
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            flags |= kFlagSubtract;
            if (cmp == 0) flags |= kFlagZero;
            if ((cmp & 0x80) != 0) flags |= kFlagSign;
            if ((registers_.a & 0x0F) < (value & 0x0F)) flags |= kFlagHalfCarry;
            if (bc() != 0) flags |= kFlagParityOverflow;
            registers_.f = flags;
            if (bc() != 0 && cmp != 0) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc - 2);
                result.cycles = 21;
                result.message = "CPIR (repeat)";
            } else {
                result.cycles = 16;
                result.message = "CPIR";
            }
            break;
        }
        case 0xB9: {
            const auto value = read8(hl());
            const std::uint8_t cmp = static_cast<std::uint8_t>(registers_.a - value);
            set_hl(static_cast<std::uint16_t>(hl() - 1));
            set_bc(static_cast<std::uint16_t>(bc() - 1));
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            flags |= kFlagSubtract;
            if (cmp == 0) flags |= kFlagZero;
            if ((cmp & 0x80) != 0) flags |= kFlagSign;
            if ((registers_.a & 0x0F) < (value & 0x0F)) flags |= kFlagHalfCarry;
            if (bc() != 0) flags |= kFlagParityOverflow;
            registers_.f = flags;
            if (bc() != 0 && cmp != 0) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc - 2);
                result.cycles = 21;
                result.message = "CPDR (repeat)";
            } else {
                result.cycles = 16;
                result.message = "CPDR";
            }
            break;
        }
        case 0xA2: {
            // INI: (HL) <- in(C); HL++; B--; flags
            const auto value = in(registers_.c);
            write8(hl(), value);
            set_hl(static_cast<std::uint16_t>(hl() + 1));
            registers_.b = static_cast<std::uint8_t>(registers_.b - 1);
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & kFlagCarry) |
                (registers_.b == 0 ? kFlagZero : 0x00) |
                kFlagSubtract);
            result.cycles = 16;
            result.message = "INI";
            break;
        }
        case 0xAA: {
            const auto value = in(registers_.c);
            write8(hl(), value);
            set_hl(static_cast<std::uint16_t>(hl() - 1));
            registers_.b = static_cast<std::uint8_t>(registers_.b - 1);
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & kFlagCarry) |
                (registers_.b == 0 ? kFlagZero : 0x00) |
                kFlagSubtract);
            result.cycles = 16;
            result.message = "IND";
            break;
        }
        case 0xB2: {
            const auto value = in(registers_.c);
            write8(hl(), value);
            set_hl(static_cast<std::uint16_t>(hl() + 1));
            registers_.b = static_cast<std::uint8_t>(registers_.b - 1);
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & kFlagCarry) |
                (registers_.b == 0 ? kFlagZero : 0x00) |
                kFlagSubtract);
            if (registers_.b != 0) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc - 2);
                result.cycles = 21;
                result.message = "INIR (repeat)";
            } else {
                result.cycles = 16;
                result.message = "INIR";
            }
            break;
        }
        case 0xBA: {
            const auto value = in(registers_.c);
            write8(hl(), value);
            set_hl(static_cast<std::uint16_t>(hl() - 1));
            registers_.b = static_cast<std::uint8_t>(registers_.b - 1);
            registers_.f = static_cast<std::uint8_t>(
                (registers_.f & kFlagCarry) |
                (registers_.b == 0 ? kFlagZero : 0x00) |
                kFlagSubtract);
            if (registers_.b != 0) {
                registers_.pc = static_cast<std::uint16_t>(registers_.pc - 2);
                result.cycles = 21;
                result.message = "INDR (repeat)";
            } else {
                result.cycles = 16;
                result.message = "INDR";
            }
            break;
        }
        default:
            // Unrecognized ED-prefixed opcodes execute as NOP on real Z80
            // hardware (no exception is raised). Several Sonic GG instances of
            // bytes like ED 0E / ED 2A appear in data sections that are never
            // reached, but raising an unimplemented fault on those would stop
            // emulation if execution ever crossed into data. Treat them as
            // 8-cycle two-byte NOPs to match hardware.
            result.cycles = 8;
            result.message = "ED NOP (opcode 0x" + hex8(opcode) + ")";
            break;
    }

    return result;
}

Z80StepResult Z80Cpu::step_indexed_prefixed(std::uint8_t prefix, std::uint16_t pc_before) noexcept {
    Z80StepResult result;
    result.executed = true;
    result.prefix = prefix;
    result.length = 2;

    auto* index = select_index_register(prefix);
    if (index == nullptr) {
        result.unimplemented = true;
        result.message = "Invalid index prefix";
        return result;
    }

    auto index_high = [&]() -> std::uint8_t {
        return static_cast<std::uint8_t>(*index >> 8);
    };

    auto index_low = [&]() -> std::uint8_t {
        return static_cast<std::uint8_t>(*index & 0x00FF);
    };

    auto set_index_high = [&](std::uint8_t value) {
        *index = static_cast<std::uint16_t>((static_cast<std::uint16_t>(value) << 8) | (*index & 0x00FF));
    };

    auto set_index_low = [&](std::uint8_t value) {
        *index = static_cast<std::uint16_t>((*index & 0xFF00) | value);
    };

    // Undocumented but well-defined: DD/FD CB d op encodings whose low three
    // bits are not 6 also copy the result into that plain register. The target
    // is always the real B/C/D/E/H/L/A - never IXH/IXL - because the index
    // prefix only redirects the memory operand here.
    auto store_indexed_cb_side_effect = [&](std::uint8_t cb_opcode, std::uint8_t value) {
        switch (cb_opcode & 0x07) {
            case 0x00:
                registers_.b = value;
                break;
            case 0x01:
                registers_.c = value;
                break;
            case 0x02:
                registers_.d = value;
                break;
            case 0x03:
                registers_.e = value;
                break;
            case 0x04:
                registers_.h = value;
                break;
            case 0x05:
                registers_.l = value;
                break;
            case 0x07:
                registers_.a = value;
                break;
            default:
                break;
        }
    };

    auto read_index_register_selector = [&](std::uint8_t selector) -> std::uint8_t {
        switch (selector) {
            case 0x00:
                return registers_.b;
            case 0x01:
                return registers_.c;
            case 0x02:
                return registers_.d;
            case 0x03:
                return registers_.e;
            case 0x04:
                return index_high();
            case 0x05:
                return index_low();
            case 0x07:
                return registers_.a;
            default:
                return 0x00;
        }
    };

    auto write_index_register_selector = [&](std::uint8_t selector, std::uint8_t value) {
        switch (selector) {
            case 0x00:
                registers_.b = value;
                break;
            case 0x01:
                registers_.c = value;
                break;
            case 0x02:
                registers_.d = value;
                break;
            case 0x03:
                registers_.e = value;
                break;
            case 0x04:
                set_index_high(value);
                break;
            case 0x05:
                set_index_low(value);
                break;
            case 0x07:
                registers_.a = value;
                break;
            default:
                break;
        }
    };

    const auto opcode = fetch8();
    result.opcode = opcode;

    if (opcode >= 0x40 && opcode <= 0x7F && opcode != 0x76) {
        const auto destination_selector = static_cast<std::uint8_t>((opcode >> 3) & 0x07);
        const auto source_selector = static_cast<std::uint8_t>(opcode & 0x07);
        const bool touches_index_byte = destination_selector == 0x04 || destination_selector == 0x05 ||
                                        source_selector == 0x04 || source_selector == 0x05;

        if (touches_index_byte && destination_selector != 0x06 && source_selector != 0x06) {
            write_index_register_selector(destination_selector, read_index_register_selector(source_selector));
            result.cycles = 8;
            result.message = "LD r,r";
            result.pc_before = pc_before;
            return result;
        }
    }

    if (opcode >= 0x80 && opcode <= 0xBF) {
        const auto selector = static_cast<std::uint8_t>(opcode & 0x07);
        if (selector == 0x04 || selector == 0x05) {
            const auto operand = read_index_register_selector(selector);
            switch (opcode & 0xF8) {
                case 0x80: {
                    const auto lhs = registers_.a;
                    const auto value = static_cast<std::uint8_t>(lhs + operand);
                    registers_.a = value;
                    set_add_flags(lhs, operand, 0, value);
                    result.message = "ADD A,r";
                    break;
                }
                case 0x88: {
                    const auto carry_in = static_cast<std::uint8_t>((registers_.f & kFlagCarry) != 0 ? 1 : 0);
                    const auto lhs = registers_.a;
                    const auto value = static_cast<std::uint8_t>(lhs + operand + carry_in);
                    registers_.a = value;
                    set_add_flags(lhs, operand, carry_in, value);
                    result.message = "ADC A,r";
                    break;
                }
                case 0x90: {
                    const auto lhs = registers_.a;
                    const auto value = static_cast<std::uint8_t>(lhs - operand);
                    registers_.a = value;
                    set_subtract_flags(lhs, operand, 0, value);
                    result.message = "SUB r";
                    break;
                }
                case 0x98: {
                    const auto carry_in = static_cast<std::uint8_t>((registers_.f & kFlagCarry) != 0 ? 1 : 0);
                    const auto lhs = registers_.a;
                    const auto value = static_cast<std::uint8_t>(lhs - operand - carry_in);
                    registers_.a = value;
                    set_subtract_flags(lhs, operand, carry_in, value);
                    result.message = "SBC A,r";
                    break;
                }
                case 0xA0:
                    registers_.a = static_cast<std::uint8_t>(registers_.a & operand);
                    set_logic_flags(registers_.a, true);
                    result.message = "AND r";
                    break;
                case 0xA8:
                    registers_.a = static_cast<std::uint8_t>(registers_.a ^ operand);
                    set_logic_flags(registers_.a, false);
                    result.message = "XOR r";
                    break;
                case 0xB0:
                    registers_.a = static_cast<std::uint8_t>(registers_.a | operand);
                    set_logic_flags(registers_.a, false);
                    result.message = "OR r";
                    break;
                case 0xB8:
                    set_compare_flags(registers_.a, operand);
                    result.message = "CP r";
                    break;
                default:
                    break;
            }

            result.cycles = 8;
            result.pc_before = pc_before;
            return result;
        }
    }

    switch (opcode) {
        case 0x09:
        case 0x19:
        case 0x29:
        case 0x39: {
            const auto lhs = *index;
            std::uint16_t rhs = 0x0000;
            switch ((opcode >> 4) & 0x03) {
                case 0x00:
                    rhs = bc();
                    break;
                case 0x01:
                    rhs = de();
                    break;
                case 0x02:
                    rhs = *index;
                    break;
                case 0x03:
                    rhs = registers_.sp;
                    break;
                default:
                    break;
            }

            const auto wide = static_cast<std::uint32_t>(lhs) + rhs;
            std::uint8_t flags = static_cast<std::uint8_t>(
                registers_.f & (kFlagSign | kFlagZero | kFlagParityOverflow));
            if (((lhs & 0x0FFF) + (rhs & 0x0FFF)) > 0x0FFF) {
                flags |= kFlagHalfCarry;
            }
            if (wide > 0xFFFF) {
                flags |= kFlagCarry;
            }
            registers_.f = flags;
            *index = static_cast<std::uint16_t>(wide & 0xFFFF);
            result.cycles = 15;
            result.message = prefix == 0xFD ? "ADD IY,ss" : "ADD IX,ss";
            break;
        }
        case 0x21:
            *index = fetch16();
            result.length = 4;
            result.cycles = 14;
            result.message = prefix == 0xFD ? "LD IY,nn" : "LD IX,nn";
            break;
        case 0x22: {
            const auto address = fetch16();
            write8(address, static_cast<std::uint8_t>(*index & 0x00FF));
            write8(static_cast<std::uint16_t>(address + 1), static_cast<std::uint8_t>(*index >> 8));
            result.length = 4;
            result.cycles = 20;
            result.message = prefix == 0xFD ? "LD (nn),IY" : "LD (nn),IX";
            break;
        }
        case 0x23:
            *index = static_cast<std::uint16_t>(*index + 1);
            result.cycles = 10;
            result.message = prefix == 0xFD ? "INC IY" : "INC IX";
            break;
        case 0x24: {
            const auto operand = index_high();
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            set_index_high(value);
            registers_.f = flags;
            result.cycles = 8;
            result.message = prefix == 0xFD ? "INC IYH" : "INC IXH";
            break;
        }
        case 0x25: {
            const auto operand = index_high();
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            set_index_high(value);
            registers_.f = flags;
            result.cycles = 8;
            result.message = prefix == 0xFD ? "DEC IYH" : "DEC IXH";
            break;
        }
        case 0x26:
            set_index_high(fetch8());
            result.length = 3;
            result.cycles = 11;
            result.message = prefix == 0xFD ? "LD IYH,n" : "LD IXH,n";
            break;
        case 0x2A: {
            const auto address = fetch16();
            const auto low = read8(address);
            const auto high = read8(static_cast<std::uint16_t>(address + 1));
            *index = static_cast<std::uint16_t>((high << 8) | low);
            result.length = 4;
            result.cycles = 20;
            result.message = prefix == 0xFD ? "LD IY,(nn)" : "LD IX,(nn)";
            break;
        }
        case 0x2B:
            *index = static_cast<std::uint16_t>(*index - 1);
            result.cycles = 10;
            result.message = prefix == 0xFD ? "DEC IY" : "DEC IX";
            break;
        case 0x2C: {
            const auto operand = index_low();
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            set_index_low(value);
            registers_.f = flags;
            result.cycles = 8;
            result.message = prefix == 0xFD ? "INC IYL" : "INC IXL";
            break;
        }
        case 0x2D: {
            const auto operand = index_low();
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            set_index_low(value);
            registers_.f = flags;
            result.cycles = 8;
            result.message = prefix == 0xFD ? "DEC IYL" : "DEC IXL";
            break;
        }
        case 0x2E:
            set_index_low(fetch8());
            result.length = 3;
            result.cycles = 11;
            result.message = prefix == 0xFD ? "LD IYL,n" : "LD IXL,n";
            break;
        case 0x34: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            const auto address = indexed_address(prefix, displacement);
            const auto operand = read8(address);
            const auto value = static_cast<std::uint8_t>(operand + 1);
            std::uint8_t flags = static_cast<std::uint8_t>(registers_.f & kFlagCarry);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if (((operand & 0x0F) + 1) > 0x0F) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x7F) {
                flags |= kFlagParityOverflow;
            }
            write8(address, value);
            registers_.f = flags;
            result.length = 3;
            result.cycles = 23;
            result.message = prefix == 0xFD ? "INC (IY+d)" : "INC (IX+d)";
            break;
        }
        case 0x35: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            const auto address = indexed_address(prefix, displacement);
            const auto operand = read8(address);
            const auto value = static_cast<std::uint8_t>(operand - 1);
            std::uint8_t flags = static_cast<std::uint8_t>((registers_.f & kFlagCarry) | kFlagSubtract);
            if (value == 0) {
                flags |= kFlagZero;
            }
            if ((value & 0x80) != 0) {
                flags |= kFlagSign;
            }
            if ((operand & 0x0F) == 0x00) {
                flags |= kFlagHalfCarry;
            }
            if (operand == 0x80) {
                flags |= kFlagParityOverflow;
            }
            write8(address, value);
            registers_.f = flags;
            result.length = 3;
            result.cycles = 23;
            result.message = prefix == 0xFD ? "DEC (IY+d)" : "DEC (IX+d)";
            break;
        }
        case 0xE5:
            push16(*index);
            result.cycles = 15;
            result.message = prefix == 0xFD ? "PUSH IY" : "PUSH IX";
            break;
        case 0xE1:
            *index = pop16();
            result.cycles = 14;
            result.message = prefix == 0xFD ? "POP IY" : "POP IX";
            break;
        case 0xE3: {
            const auto low = read8(registers_.sp);
            const auto high = read8(static_cast<std::uint16_t>(registers_.sp + 1));
            const auto memory_value = static_cast<std::uint16_t>((high << 8) | low);
            write8(registers_.sp, static_cast<std::uint8_t>(*index & 0x00FF));
            write8(static_cast<std::uint16_t>(registers_.sp + 1), static_cast<std::uint8_t>(*index >> 8));
            *index = memory_value;
            result.cycles = 23;
            result.message = prefix == 0xFD ? "EX (SP),IY" : "EX (SP),IX";
            break;
        }
        case 0xF9:
            registers_.sp = *index;
            result.cycles = 10;
            result.message = prefix == 0xFD ? "LD SP,IY" : "LD SP,IX";
            break;
        case 0xE9:
            registers_.pc = *index;
            result.cycles = 8;
            result.message = prefix == 0xFD ? "JP (IY)" : "JP (IX)";
            break;
        case 0x77: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            write8(indexed_address(prefix, displacement), registers_.a);
            result.length = 3;
            result.cycles = 19;
            result.message = prefix == 0xFD ? "LD (IY+d),A" : "LD (IX+d),A";
            break;
        }
        case 0x36: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            const auto value = fetch8();
            write8(indexed_address(prefix, displacement), value);
            result.length = 4;
            result.cycles = 19;
            result.message = prefix == 0xFD ? "LD (IY+d),n" : "LD (IX+d),n";
            break;
        }
        case 0x7E: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            registers_.a = read8(indexed_address(prefix, displacement));
            result.length = 3;
            result.cycles = 19;
            result.message = prefix == 0xFD ? "LD A,(IY+d)" : "LD A,(IX+d)";
            break;
        }
        case 0x86:
        case 0x8E:
        case 0x96:
        case 0x9E:
        case 0xA6:
        case 0xAE:
        case 0xB6:
        case 0xBE: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            const auto operand = read8(indexed_address(prefix, displacement));
            const auto mnemonic_suffix = prefix == 0xFD ? "(IY+d)" : "(IX+d)";

            switch (opcode) {
                case 0x86: {
                    const auto lhs = registers_.a;
                    const auto value = static_cast<std::uint8_t>(lhs + operand);
                    registers_.a = value;
                    set_add_flags(lhs, operand, 0, value);
                    result.message = "ADD A," + std::string(mnemonic_suffix);
                    break;
                }
                case 0x8E: {
                    const auto carry_in = static_cast<std::uint8_t>((registers_.f & kFlagCarry) != 0 ? 1 : 0);
                    const auto lhs = registers_.a;
                    const auto value = static_cast<std::uint8_t>(lhs + operand + carry_in);
                    registers_.a = value;
                    set_add_flags(lhs, operand, carry_in, value);
                    result.message = "ADC A," + std::string(mnemonic_suffix);
                    break;
                }
                case 0x96: {
                    const auto lhs = registers_.a;
                    const auto value = static_cast<std::uint8_t>(lhs - operand);
                    registers_.a = value;
                    set_subtract_flags(lhs, operand, 0, value);
                    result.message = "SUB " + std::string(mnemonic_suffix);
                    break;
                }
                case 0x9E: {
                    const auto carry_in = static_cast<std::uint8_t>((registers_.f & kFlagCarry) != 0 ? 1 : 0);
                    const auto lhs = registers_.a;
                    const auto value = static_cast<std::uint8_t>(lhs - operand - carry_in);
                    registers_.a = value;
                    set_subtract_flags(lhs, operand, carry_in, value);
                    result.message = "SBC A," + std::string(mnemonic_suffix);
                    break;
                }
                case 0xA6:
                    registers_.a = static_cast<std::uint8_t>(registers_.a & operand);
                    set_logic_flags(registers_.a, true);
                    result.message = "AND " + std::string(mnemonic_suffix);
                    break;
                case 0xAE:
                    registers_.a = static_cast<std::uint8_t>(registers_.a ^ operand);
                    set_logic_flags(registers_.a, false);
                    result.message = "XOR " + std::string(mnemonic_suffix);
                    break;
                case 0xB6:
                    registers_.a = static_cast<std::uint8_t>(registers_.a | operand);
                    set_logic_flags(registers_.a, false);
                    result.message = "OR " + std::string(mnemonic_suffix);
                    break;
                case 0xBE:
                    set_compare_flags(registers_.a, operand);
                    result.message = "CP " + std::string(mnemonic_suffix);
                    break;
                default:
                    break;
            }

            result.length = 3;
            result.cycles = 19;
            break;
        }
        case 0x46:
        case 0x4E:
        case 0x56:
        case 0x5E:
        case 0x66:
        case 0x6E: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            const auto value = read8(indexed_address(prefix, displacement));
            switch ((opcode >> 3) & 0x07) {
                case 0x00:
                    registers_.b = value;
                    break;
                case 0x01:
                    registers_.c = value;
                    break;
                case 0x02:
                    registers_.d = value;
                    break;
                case 0x03:
                    registers_.e = value;
                    break;
                case 0x04:
                    registers_.h = value;
                    break;
                case 0x05:
                    registers_.l = value;
                    break;
                default:
                    break;
            }
            result.length = 3;
            result.cycles = 19;
            result.message = prefix == 0xFD ? "LD r,(IY+d)" : "LD r,(IX+d)";
            break;
        }
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0x74:
        case 0x75: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            std::uint8_t value = 0x00;
            // For LD (IX+d),r the destination is the memory operand, so the
            // SOURCE register is the low three bits. Using (opcode >> 3) here
            // selects the destination field instead, which is always 6 across
            // 0x70-0x75 and made every one of these stores write zero.
            // The source is also the real B/C/D/E/H/L - the index prefix
            // redirects only the memory operand, never the source register.
            switch (opcode & 0x07) {
                case 0x06:
                    value = 0x00;
                    break;
                case 0x00:
                    value = registers_.b;
                    break;
                case 0x01:
                    value = registers_.c;
                    break;
                case 0x02:
                    value = registers_.d;
                    break;
                case 0x03:
                    value = registers_.e;
                    break;
                case 0x04:
                    value = registers_.h;
                    break;
                case 0x05:
                    value = registers_.l;
                    break;
                default:
                    break;
            }
            write8(indexed_address(prefix, displacement), value);
            result.length = 3;
            result.cycles = 19;
            result.message = prefix == 0xFD ? "LD (IY+d),r" : "LD (IX+d),r";
            break;
        }
        case 0xCB: {
            const auto displacement = static_cast<std::int8_t>(fetch8());
            const auto cb_opcode = fetch8();
            const auto address = indexed_address(prefix, displacement);
            auto value = read8(address);
            result.length = 4;
            result.opcode = cb_opcode;

            if ((cb_opcode & 0xC0) == 0x80) {
                const auto bit = static_cast<std::uint8_t>((cb_opcode >> 3) & 0x07);
                value = static_cast<std::uint8_t>(value & ~(1U << bit));
                write8(address, value);
                result.cycles = 23;
                result.message = prefix == 0xFD ? "RES b,(IY+d)" : "RES b,(IX+d)";
            } else if ((cb_opcode & 0xC0) == 0xC0) {
                const auto bit = static_cast<std::uint8_t>((cb_opcode >> 3) & 0x07);
                value = static_cast<std::uint8_t>(value | (1U << bit));
                write8(address, value);
                result.cycles = 23;
                result.message = prefix == 0xFD ? "SET b,(IY+d)" : "SET b,(IX+d)";
            } else if ((cb_opcode & 0xC0) == 0x40) {
                const auto bit = static_cast<std::uint8_t>((cb_opcode >> 3) & 0x07);
                std::uint8_t flags = registers_.f & kFlagCarry;
                flags |= kFlagHalfCarry;
                if ((value & (1U << bit)) == 0) {
                    flags |= kFlagZero;
                }
                if (bit == 7 && (value & 0x80) != 0) {
                    flags |= kFlagSign;
                }
                if ((value & (1U << bit)) == 0) {
                    flags |= kFlagParityOverflow;
                }
                registers_.f = flags;
                result.cycles = 20;
                result.message = prefix == 0xFD ? "BIT b,(IY+d)" : "BIT b,(IX+d)";
            } else {
                // Rotate/shift group (cb_opcode 0x00-0x3F) on (IX+d)/(IY+d).
                const auto operation = static_cast<std::uint8_t>((cb_opcode >> 3) & 0x07);
                const bool old_msb = (value & 0x80) != 0;
                const bool old_lsb = (value & 0x01) != 0;
                const bool carry_in = carry_flag();
                std::uint8_t shifted = value;
                bool carry_out = false;

                switch (operation) {
                    case 0x00:
                        shifted = static_cast<std::uint8_t>((value << 1) | (old_msb ? 1 : 0));
                        carry_out = old_msb;
                        result.message = "RLC";
                        break;
                    case 0x01:
                        shifted = static_cast<std::uint8_t>((value >> 1) | (old_lsb ? 0x80 : 0x00));
                        carry_out = old_lsb;
                        result.message = "RRC";
                        break;
                    case 0x02:
                        shifted = static_cast<std::uint8_t>((value << 1) | (carry_in ? 1 : 0));
                        carry_out = old_msb;
                        result.message = "RL";
                        break;
                    case 0x03:
                        shifted = static_cast<std::uint8_t>((value >> 1) | (carry_in ? 0x80 : 0x00));
                        carry_out = old_lsb;
                        result.message = "RR";
                        break;
                    case 0x04:
                        shifted = static_cast<std::uint8_t>(value << 1);
                        carry_out = old_msb;
                        result.message = "SLA";
                        break;
                    case 0x05:
                        shifted = static_cast<std::uint8_t>((value >> 1) | (value & 0x80));
                        carry_out = old_lsb;
                        result.message = "SRA";
                        break;
                    case 0x06:
                        shifted = static_cast<std::uint8_t>((value << 1) | 0x01);
                        carry_out = old_msb;
                        result.message = "SLL";
                        break;
                    default:
                        shifted = static_cast<std::uint8_t>(value >> 1);
                        carry_out = old_lsb;
                        result.message = "SRL";
                        break;
                }

                result.message += (prefix == 0xFD) ? " (IY+d)" : " (IX+d)";
                write8(address, shifted);

                std::uint8_t flags = 0x00;
                if (shifted == 0) {
                    flags |= kFlagZero;
                }
                if ((shifted & 0x80) != 0) {
                    flags |= kFlagSign;
                }
                if (has_even_parity(shifted)) {
                    flags |= kFlagParityOverflow;
                }
                if (carry_out) {
                    flags |= kFlagCarry;
                }
                registers_.f = flags;

                store_indexed_cb_side_effect(cb_opcode, shifted);
                result.cycles = 23;
            }
            break;
        }
        default:
            // A DD/FD prefix in front of an instruction that has no IX/IY form
            // is ignored by real hardware: the prefix burns 4 cycles and the
            // opcode then runs as its normal, HL-based self. Rewinding PC (and
            // the matching R increment) re-fetches that opcode on the next
            // step, which also makes DD/FD chains re-prefix correctly.
            registers_.pc = static_cast<std::uint16_t>(registers_.pc - 1);
            registers_.r = static_cast<std::uint8_t>((registers_.r & 0x80) |
                                                     ((registers_.r - 1) & 0x7F));
            result.length = 1;
            result.cycles = 4;
            result.message = std::string(prefix == 0xFD ? "FD" : "DD") + " prefix ignored";
            break;
    }

    result.pc_before = pc_before;
    return result;
}

}  // namespace mercury::core
