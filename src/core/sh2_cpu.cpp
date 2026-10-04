#include "mercury/core/sh2_cpu.hpp"

#include <sstream>

namespace mercury::core {

namespace {

// Field extraction. The SH-2 encoding is regular: nnnn is almost always bits
// 11-8 and mmmm bits 7-4, so these three cover most of the instruction set.
constexpr int n_of(std::uint16_t op) { return (op >> 8) & 0x0F; }
constexpr int m_of(std::uint16_t op) { return (op >> 4) & 0x0F; }
constexpr std::uint32_t imm8(std::uint16_t op) { return op & 0x00FF; }
// Sign-extended 8-bit displacement, in instructions (the hardware doubles it).
constexpr std::int32_t disp8(std::uint16_t op) {
    return static_cast<std::int8_t>(op & 0x00FF);
}
// Sign-extended 12-bit displacement, for BRA and BSR.
constexpr std::int32_t disp12(std::uint16_t op) {
    const std::int32_t value = op & 0x0FFF;
    return (value & 0x800) != 0 ? value - 0x1000 : value;
}

constexpr std::uint32_t sign_extend8(std::uint32_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
}
constexpr std::uint32_t sign_extend16(std::uint32_t value) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

}  // namespace

void Sh2Cpu::reset() noexcept {
    registers_ = Sh2Registers{};
    delay_pending_ = false;
    delay_target_ = 0;
    pending_level_ = 0;
    pending_vector_ = 0;
    halted_ = false;
    interrupts_taken_ = 0;
    invalidate_fetch_window();
    // The vector table lives at address 0 on reset, because VBR is 0. Entry 0
    // is the starting PC and entry 1 the starting stack pointer - which is why
    // a 32X cartridge carries an SH-2 vector table in its header and why a
    // core that starts at 0 instead runs the table as code.
    if (direct_ != nullptr || bus_.read32) {
        registers_.pc = read32(0x00000000);
        registers_.r[15] = read32(0x00000004);
    }
    registers_.sr = kInterruptMask;
}

std::uint8_t Sh2Cpu::read8(std::uint32_t address) const {
    if (direct_ != nullptr) return direct_->read8(address);
    return bus_.read8 ? bus_.read8(address) : 0xFF;
}
std::uint16_t Sh2Cpu::read16(std::uint32_t address) const {
    if (direct_ != nullptr) return direct_->read16(address);
    return bus_.read16 ? bus_.read16(address) : 0xFFFF;
}
std::uint32_t Sh2Cpu::read32(std::uint32_t address) const {
    if (direct_ != nullptr) return direct_->read32(address);
    return bus_.read32 ? bus_.read32(address) : 0xFFFFFFFF;
}
std::uint16_t Sh2Cpu::fetch_slow(std::uint32_t address) noexcept {
    if (direct_ != nullptr) {
        std::uint32_t base = 0;
        std::uint32_t size = 0;
        if (const std::uint8_t* data = direct_->fetch_window(address, base, size); data != nullptr) {
            fetch_data_ = data;
            fetch_base_ = base;
            fetch_size_ = size & ~1u;
            const std::uint32_t into = (address & ~1u) - base;
            if (into < fetch_size_) {
                return static_cast<std::uint16_t>((data[into] << 8) | data[into + 1]);
            }
        }
    }
    return read16(address);
}

void Sh2Cpu::write8(std::uint32_t address, std::uint8_t value) const {
    if (direct_ != nullptr) {
        direct_->write8(address, value);
    } else if (bus_.write8) {
        bus_.write8(address, value);
    }
}
void Sh2Cpu::write16(std::uint32_t address, std::uint16_t value) const {
    if (direct_ != nullptr) {
        direct_->write16(address, value);
    } else if (bus_.write16) {
        bus_.write16(address, value);
    }
}
void Sh2Cpu::write32(std::uint32_t address, std::uint32_t value) const {
    if (direct_ != nullptr) {
        direct_->write32(address, value);
    } else if (bus_.write32) {
        bus_.write32(address, value);
    }
}

void Sh2Cpu::request_interrupt(int level, int vector) noexcept {
    // Highest level wins while one is already waiting.
    if (level > pending_level_) {
        pending_level_ = level;
        pending_vector_ = vector;
    }
}

void Sh2Cpu::branch_after_delay(std::uint32_t target) noexcept {
    delay_pending_ = true;
    delay_target_ = target;
}

void Sh2Cpu::take_interrupt(Sh2StepResult& result) noexcept {
    // SR and PC go on the stack, then the handler address comes from the
    // vector table at VBR.
    registers_.r[15] -= 4;
    write32(registers_.r[15], registers_.sr);
    registers_.r[15] -= 4;
    write32(registers_.r[15], registers_.pc);
    registers_.sr = (registers_.sr & ~kInterruptMask) |
                    ((static_cast<std::uint32_t>(pending_level_) << 4) & kInterruptMask);
    registers_.pc = read32(registers_.vbr + static_cast<std::uint32_t>(pending_vector_) * 4);
    pending_level_ = 0;
    pending_vector_ = 0;
    halted_ = false;
    ++interrupts_taken_;
    result.executed = true;
    result.cycles = 13;
    result.message = "interrupt";
}

Sh2StepResult Sh2Cpu::step() noexcept {
    Sh2StepResult result;
    result.pc_before = registers_.pc;

    // An interrupt is taken between instructions, and never between a branch
    // and its delay slot - the pair is indivisible on hardware.
    const int mask = static_cast<int>((registers_.sr & kInterruptMask) >> 4);
    if (pending_level_ > mask && !delay_pending_) {
        take_interrupt(result);
        result.pc_after = registers_.pc;
        return result;
    }

    if (halted_) {
        result.executed = true;
        result.cycles = 1;
        result.pc_after = registers_.pc;
        result.message = "halted";
        return result;
    }

    const std::uint16_t opcode = fetch(registers_.pc);
    result.opcode = opcode;
    result.in_delay_slot = delay_pending_;

    // Take the pending branch AFTER this instruction, not before it. Captured
    // here because the delay slot instruction may itself queue nothing and
    // must not see the flag still set.
    const bool completing_delay = delay_pending_;
    const std::uint32_t delay_target = delay_target_;
    delay_pending_ = false;

    // A DELAY SLOT RUNS WITH PC ALREADY AT THE BRANCH TARGET.
    //
    // The slot's opcode comes from its own address, fetched above, but PC
    // itself has moved on. Nothing notices except the PC-relative
    // instructions - MOV.W @(disp,PC), MOV.L @(disp,PC) and MOVA - which then
    // count from the target rather than from the slot. Compilers put exactly
    // those in delay slots all the time, loading a constant from the literal
    // pool that follows the code being jumped to.
    //
    // Counting from the slot read the wrong literal. After Burner's
    // `BRA $0600ACEC / MOV.W @($1E,PC),R6` loaded $DEAE instead of $0FF9,
    // ORed it into a palette-range mask, and a copy loop later walked that
    // mask into a zero count and copied memory for ever. ares
    // (`inDelaySlot() ? PPC - 2 : PC`) and PicoDrive's interpreter (PC left
    // at the target while the slot's opcode is read from the delay address)
    // both count from the target.
    registers_.pc = completing_delay ? delay_target : registers_.pc + 2;
    execute(opcode, result);

    if (completing_delay) {
        // A branch in a delay slot is illegal on hardware and its behaviour is
        // undefined; taking the outer branch is the sane reading and keeps the
        // core from losing track of where it is.
        registers_.pc = delay_target;
        delay_pending_ = false;
    }

    result.pc_after = registers_.pc;
    return result;
}

void Sh2Cpu::execute(std::uint16_t op, Sh2StepResult& result) noexcept {
    auto& r = registers_.r;
    const int n = n_of(op);
    const int m = m_of(op);
    result.executed = true;
    result.cycles = 1;

    switch (op >> 12) {
        case 0x0:
            switch (op & 0x0F) {
                case 0x2:  // STC control register, Rn
                    switch ((op >> 4) & 0x0F) {
                        case 0x0: r[n] = registers_.sr; return;
                        case 0x1: r[n] = registers_.gbr; return;
                        case 0x2: r[n] = registers_.vbr; return;
                        default: break;
                    }
                    break;
                case 0x3:
                    switch ((op >> 4) & 0x0F) {
                        case 0x0:  // BSRF Rn - subroutine, PC-relative by register
                            registers_.pr = registers_.pc + 2;
                            branch_after_delay(registers_.pc + 2 + r[n]);
                            result.cycles = 2;
                            return;
                        case 0x2:  // BRAF Rn
                            branch_after_delay(registers_.pc + 2 + r[n]);
                            result.cycles = 2;
                            return;
                        default: break;
                    }
                    break;
                case 0x4: write8(r[0] + r[n], static_cast<std::uint8_t>(r[m])); return;
                case 0x5: write16(r[0] + r[n], static_cast<std::uint16_t>(r[m])); return;
                case 0x6: write32(r[0] + r[n], r[m]); return;
                case 0x7:  // MUL.L - 32x32, low 32 bits into MACL
                    registers_.macl = r[n] * r[m];
                    result.cycles = 2;
                    return;
                case 0x8:
                    switch ((op >> 4) & 0x0F) {
                        case 0x0: set_t_flag(false); return;                      // CLRT
                        case 0x1: set_t_flag(true); return;                       // SETT
                        case 0x2: registers_.mach = 0; registers_.macl = 0; return;  // CLRMAC
                        default: break;
                    }
                    break;
                case 0x9:
                    switch ((op >> 4) & 0x0F) {
                        case 0x0: return;  // NOP
                        case 0x1:          // DIV0U
                            registers_.sr &= ~(kFlagQ | kFlagM | kFlagT);
                            return;
                        case 0x2: r[n] = t_flag() ? 1u : 0u; return;  // MOVT Rn
                        default: break;
                    }
                    break;
                case 0xA:  // STS system register, Rn
                    switch ((op >> 4) & 0x0F) {
                        case 0x0: r[n] = registers_.mach; return;
                        case 0x1: r[n] = registers_.macl; return;
                        case 0x2: r[n] = registers_.pr; return;
                        default: break;
                    }
                    break;
                case 0xB:
                    switch ((op >> 4) & 0x0F) {
                        case 0x0:  // RTS
                            branch_after_delay(registers_.pr);
                            result.cycles = 2;
                            return;
                        case 0x1:  // SLEEP
                            halted_ = true;
                            result.cycles = 3;
                            return;
                        case 0x2: {  // RTE
                            const std::uint32_t pc = read32(r[15]);
                            r[15] += 4;
                            const std::uint32_t sr = read32(r[15]);
                            r[15] += 4;
                            registers_.sr = sr & 0x000003F3;
                            branch_after_delay(pc);
                            result.cycles = 4;
                            return;
                        }
                        default: break;
                    }
                    break;
                case 0xC: r[n] = sign_extend8(read8(r[0] + r[m])); return;
                case 0xD: r[n] = sign_extend16(read16(r[0] + r[m])); return;
                case 0xE: r[n] = read32(r[0] + r[m]); return;
                case 0xF: {  // MAC.L @Rm+,@Rn+
                    const auto a = static_cast<std::int64_t>(static_cast<std::int32_t>(read32(r[m])));
                    r[m] += 4;
                    const auto b = static_cast<std::int64_t>(static_cast<std::int32_t>(read32(r[n])));
                    r[n] += 4;
                    std::int64_t acc = (static_cast<std::int64_t>(registers_.mach) << 32) |
                                       registers_.macl;
                    acc += a * b;
                    registers_.mach = static_cast<std::uint32_t>(static_cast<std::uint64_t>(acc) >> 32);
                    registers_.macl = static_cast<std::uint32_t>(acc);
                    result.cycles = 3;
                    return;
                }
                default: break;
            }
            break;

        case 0x1:  // MOV.L Rm,@(disp,Rn)
            write32(r[n] + (op & 0x0F) * 4, r[m]);
            return;

        case 0x2:
            switch (op & 0x0F) {
                case 0x0: write8(r[n], static_cast<std::uint8_t>(r[m])); return;
                case 0x1: write16(r[n], static_cast<std::uint16_t>(r[m])); return;
                case 0x2: write32(r[n], r[m]); return;
                case 0x4: r[n] -= 1; write8(r[n], static_cast<std::uint8_t>(r[m])); return;
                case 0x5: r[n] -= 2; write16(r[n], static_cast<std::uint16_t>(r[m])); return;
                case 0x6: r[n] -= 4; write32(r[n], r[m]); return;
                case 0x7: {  // DIV0S - set up a signed step division
                    const bool q = (r[n] & 0x80000000u) != 0;
                    const bool mm = (r[m] & 0x80000000u) != 0;
                    registers_.sr = (registers_.sr & ~(kFlagQ | kFlagM | kFlagT)) |
                                    (q ? kFlagQ : 0u) | (mm ? kFlagM : 0u) |
                                    ((q != mm) ? kFlagT : 0u);
                    return;
                }
                case 0x8: set_t_flag((r[n] & r[m]) == 0); return;  // TST
                case 0x9: r[n] &= r[m]; return;
                case 0xA: r[n] ^= r[m]; return;
                case 0xB: r[n] |= r[m]; return;
                case 0xC: {  // CMP/STR - true if ANY byte is equal
                    const std::uint32_t diff = r[n] ^ r[m];
                    const bool any = ((diff & 0xFF000000u) == 0) || ((diff & 0x00FF0000u) == 0) ||
                                     ((diff & 0x0000FF00u) == 0) || ((diff & 0x000000FFu) == 0);
                    set_t_flag(any);
                    return;
                }
                case 0xD:  // XTRCT - the middle 32 bits of the 64-bit pair
                    r[n] = ((r[m] << 16) & 0xFFFF0000u) | ((r[n] >> 16) & 0x0000FFFFu);
                    return;
                case 0xE:  // MULU.W
                    registers_.macl = static_cast<std::uint32_t>(r[n] & 0xFFFFu) *
                                      static_cast<std::uint32_t>(r[m] & 0xFFFFu);
                    result.cycles = 2;
                    return;
                case 0xF:  // MULS.W
                    registers_.macl = static_cast<std::uint32_t>(
                        static_cast<std::int32_t>(static_cast<std::int16_t>(r[n])) *
                        static_cast<std::int32_t>(static_cast<std::int16_t>(r[m])));
                    result.cycles = 2;
                    return;
                default: break;
            }
            break;

        case 0x3:
            switch (op & 0x0F) {
                case 0x0: set_t_flag(r[n] == r[m]); return;  // CMP/EQ
                case 0x2: set_t_flag(r[n] >= r[m]); return;  // CMP/HS, unsigned
                case 0x3:                                     // CMP/GE, signed
                    set_t_flag(static_cast<std::int32_t>(r[n]) >= static_cast<std::int32_t>(r[m]));
                    return;
                case 0x4: {
                    // DIV1 - one step of a restoring division, transcribed
                    // from the Hitachi manual's reference implementation
                    // rather than reasoned about. The Q/M/T interplay here is
                    // the easiest thing in the instruction set to get subtly
                    // wrong, and a division that is wrong only for some
                    // operands is far harder to find later than one that
                    // never works.
                    const bool old_q = (registers_.sr & kFlagQ) != 0;
                    const bool mflag = (registers_.sr & kFlagM) != 0;
                    bool q = (r[n] & 0x80000000u) != 0;
                    const std::uint32_t divisor = r[m];

                    r[n] = (r[n] << 1) | (t_flag() ? 1u : 0u);
                    const std::uint32_t before = r[n];
                    bool wrapped = false;
                    if (old_q == mflag) {
                        r[n] -= divisor;
                        wrapped = r[n] > before;
                    } else {
                        r[n] += divisor;
                        wrapped = r[n] < before;
                    }
                    // THE NEW Q IS q ^ carry ^ M, whichever branch ran.
                    //
                    // Work the manual's four nested cases through and they
                    // collapse to that one expression: the add/subtract choice
                    // is made by `old_q == M`, but the Q that comes out depends
                    // on M and NOT on old_q. This was written as two branches
                    // that each dropped the M term, which inverted Q on every
                    // step where Q was already set - half the steps of a
                    // typical divide.
                    //
                    // A 32-step DIV1 chain then yields alternating quotient
                    // bits, and the tell-tale is $5555 / $AAAA turning up in a
                    // game's registers: Motocross Championship's master
                    // computed $5555 where a size of $A0 belonged, asked its
                    // allocator for $AAAA00 bytes, got a null pointer back and
                    // stopped talking to the 68000. Software division is how an
                    // SH-2 divides when it is not using the on-chip divider, so
                    // this is load bearing far beyond one game.
                    q = (q != wrapped) != mflag;

                    registers_.sr = (registers_.sr & ~kFlagQ) | (q ? kFlagQ : 0u);
                    set_t_flag(q == mflag);
                    return;
                }
                case 0x5: {  // DMULU.L
                    const auto product = static_cast<std::uint64_t>(r[n]) *
                                         static_cast<std::uint64_t>(r[m]);
                    registers_.mach = static_cast<std::uint32_t>(product >> 32);
                    registers_.macl = static_cast<std::uint32_t>(product);
                    result.cycles = 2;
                    return;
                }
                case 0x6: set_t_flag(r[n] > r[m]); return;  // CMP/HI, unsigned
                case 0x7:                                    // CMP/GT, signed
                    set_t_flag(static_cast<std::int32_t>(r[n]) > static_cast<std::int32_t>(r[m]));
                    return;
                case 0x8: r[n] -= r[m]; return;
                case 0xA: {
                    // SUBC. Two borrows can happen - one from the subtract and
                    // one from taking T away - and T is set if EITHER did.
                    const std::uint32_t difference = r[n] - r[m];
                    const std::uint32_t before = r[n];
                    r[n] = difference - (t_flag() ? 1u : 0u);
                    set_t_flag(before < difference || difference < r[n]);
                    return;
                }
                case 0xB: {  // SUBV - signed overflow into T
                    const auto a = static_cast<std::int32_t>(r[n]);
                    const auto b = static_cast<std::int32_t>(r[m]);
                    const auto value = static_cast<std::int64_t>(a) - b;
                    set_t_flag(value > 2147483647LL || value < -2147483648LL);
                    r[n] = static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
                    return;
                }
                case 0xC: r[n] += r[m]; return;
                case 0xD: {  // DMULS.L
                    const auto product = static_cast<std::int64_t>(static_cast<std::int32_t>(r[n])) *
                                         static_cast<std::int64_t>(static_cast<std::int32_t>(r[m]));
                    registers_.mach = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(product) >> 32);
                    registers_.macl = static_cast<std::uint32_t>(product);
                    result.cycles = 2;
                    return;
                }
                case 0xE: {  // ADDC
                    const std::uint32_t carry = t_flag() ? 1u : 0u;
                    const std::uint64_t sum = static_cast<std::uint64_t>(r[n]) + r[m] + carry;
                    set_t_flag(sum > 0xFFFFFFFFull);
                    r[n] = static_cast<std::uint32_t>(sum);
                    return;
                }
                case 0xF: {  // ADDV
                    const auto a = static_cast<std::int32_t>(r[n]);
                    const auto b = static_cast<std::int32_t>(r[m]);
                    const auto sum = static_cast<std::int64_t>(a) + b;
                    set_t_flag(sum > 2147483647LL || sum < -2147483648LL);
                    r[n] = static_cast<std::uint32_t>(static_cast<std::int32_t>(sum));
                    return;
                }
                default: break;
            }
            break;

        case 0x4:
            switch (op & 0xFF) {
                case 0x00: set_t_flag((r[n] & 0x80000000u) != 0); r[n] <<= 1; return;  // SHLL
                case 0x01: set_t_flag((r[n] & 1u) != 0); r[n] >>= 1; return;           // SHLR
                case 0x02: r[n] -= 4; write32(r[n], registers_.mach); return;
                case 0x03: r[n] -= 4; write32(r[n], registers_.sr); return;
                case 0x04: {  // ROTL
                    const bool top = (r[n] & 0x80000000u) != 0;
                    r[n] = (r[n] << 1) | (top ? 1u : 0u);
                    set_t_flag(top);
                    return;
                }
                case 0x05: {  // ROTR
                    const bool bottom = (r[n] & 1u) != 0;
                    r[n] = (r[n] >> 1) | (bottom ? 0x80000000u : 0u);
                    set_t_flag(bottom);
                    return;
                }
                case 0x06: registers_.mach = read32(r[n]); r[n] += 4; return;
                case 0x07: registers_.sr = read32(r[n]) & 0x000003F3; r[n] += 4; result.cycles = 3; return;
                case 0x08: r[n] <<= 2; return;   // SHLL2
                case 0x09: r[n] >>= 2; return;   // SHLR2
                case 0x0A: registers_.mach = r[n]; return;
                case 0x0B:  // JSR @Rn
                    registers_.pr = registers_.pc + 2;
                    branch_after_delay(r[n]);
                    result.cycles = 2;
                    return;
                case 0x0E: registers_.sr = r[n] & 0x000003F3; return;  // LDC Rm,SR
                case 0x10:  // DT - decrement and test, the loop instruction
                    r[n] -= 1;
                    set_t_flag(r[n] == 0);
                    return;
                case 0x11: set_t_flag(static_cast<std::int32_t>(r[n]) >= 0); return;  // CMP/PZ
                case 0x12: r[n] -= 4; write32(r[n], registers_.macl); return;
                case 0x13: r[n] -= 4; write32(r[n], registers_.gbr); return;
                case 0x15: set_t_flag(static_cast<std::int32_t>(r[n]) > 0); return;  // CMP/PL
                case 0x16: registers_.macl = read32(r[n]); r[n] += 4; return;
                case 0x17: registers_.gbr = read32(r[n]); r[n] += 4; result.cycles = 3; return;
                case 0x18: r[n] <<= 8; return;   // SHLL8
                case 0x19: r[n] >>= 8; return;   // SHLR8
                case 0x1A: registers_.macl = r[n]; return;
                case 0x1B: {  // TAS.B @Rn - the atomic one, used for locks
                    const std::uint8_t value = read8(r[n]);
                    set_t_flag(value == 0);
                    write8(r[n], static_cast<std::uint8_t>(value | 0x80));
                    result.cycles = 4;
                    return;
                }
                case 0x1E: registers_.gbr = r[n]; return;
                case 0x20: set_t_flag((r[n] & 0x80000000u) != 0); r[n] <<= 1; return;  // SHAL
                case 0x21:                                                             // SHAR
                    set_t_flag((r[n] & 1u) != 0);
                    r[n] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[n]) >> 1);
                    return;
                case 0x22: r[n] -= 4; write32(r[n], registers_.pr); return;
                case 0x23: r[n] -= 4; write32(r[n], registers_.vbr); return;
                case 0x24: {  // ROTCL - rotate left THROUGH T
                    const bool top = (r[n] & 0x80000000u) != 0;
                    r[n] = (r[n] << 1) | (t_flag() ? 1u : 0u);
                    set_t_flag(top);
                    return;
                }
                case 0x25: {  // ROTCR
                    const bool bottom = (r[n] & 1u) != 0;
                    r[n] = (r[n] >> 1) | (t_flag() ? 0x80000000u : 0u);
                    set_t_flag(bottom);
                    return;
                }
                case 0x26: registers_.pr = read32(r[n]); r[n] += 4; return;
                case 0x27: registers_.vbr = read32(r[n]); r[n] += 4; result.cycles = 3; return;
                case 0x28: r[n] <<= 16; return;  // SHLL16
                case 0x29: r[n] >>= 16; return;  // SHLR16
                case 0x2A: registers_.pr = r[n]; return;
                case 0x2B:  // JMP @Rn
                    branch_after_delay(r[n]);
                    result.cycles = 2;
                    return;
                case 0x2E: registers_.vbr = r[n]; return;
                default: break;
            }
            if ((op & 0x0F) == 0x0F) {  // MAC.W @Rm+,@Rn+
                const auto a = static_cast<std::int32_t>(static_cast<std::int16_t>(read16(r[m])));
                r[m] += 2;
                const auto b = static_cast<std::int32_t>(static_cast<std::int16_t>(read16(r[n])));
                r[n] += 2;
                std::int64_t acc = (static_cast<std::int64_t>(registers_.mach) << 32) |
                                   registers_.macl;
                acc += static_cast<std::int64_t>(a) * b;
                registers_.mach = static_cast<std::uint32_t>(static_cast<std::uint64_t>(acc) >> 32);
                registers_.macl = static_cast<std::uint32_t>(acc);
                result.cycles = 3;
                return;
            }
            break;

        case 0x5:  // MOV.L @(disp,Rm),Rn
            r[n] = read32(r[m] + (op & 0x0F) * 4);
            return;

        case 0x6:
            switch (op & 0x0F) {
                case 0x0: r[n] = sign_extend8(read8(r[m])); return;
                case 0x1: r[n] = sign_extend16(read16(r[m])); return;
                case 0x2: r[n] = read32(r[m]); return;
                case 0x3: r[n] = r[m]; return;
                case 0x4: r[n] = sign_extend8(read8(r[m])); if (n != m) r[m] += 1; return;
                case 0x5: r[n] = sign_extend16(read16(r[m])); if (n != m) r[m] += 2; return;
                case 0x6: r[n] = read32(r[m]); if (n != m) r[m] += 4; return;
                case 0x7: r[n] = ~r[m]; return;
                case 0x8:  // SWAP.B - the two low bytes
                    r[n] = (r[m] & 0xFFFF0000u) | ((r[m] & 0x00FFu) << 8) |
                           ((r[m] & 0xFF00u) >> 8);
                    return;
                case 0x9:  // SWAP.W
                    r[n] = ((r[m] & 0x0000FFFFu) << 16) | ((r[m] >> 16) & 0x0000FFFFu);
                    return;
                case 0xA: {
                    // NEGC - negate with borrow, the second half of a 64-bit
                    // negation. Same two-borrow shape as SUBC.
                    const std::uint32_t negated = 0u - r[m];
                    r[n] = negated - (t_flag() ? 1u : 0u);
                    set_t_flag(0u < negated || negated < r[n]);
                    return;
                }
                case 0xB: r[n] = 0u - r[m]; return;
                case 0xC: r[n] = r[m] & 0x000000FFu; return;  // EXTU.B
                case 0xD: r[n] = r[m] & 0x0000FFFFu; return;  // EXTU.W
                case 0xE: r[n] = sign_extend8(r[m]); return;  // EXTS.B
                case 0xF: r[n] = sign_extend16(r[m]); return; // EXTS.W
                default: break;
            }
            break;

        case 0x7:  // ADD #imm,Rn - the immediate is SIGNED
            r[n] += sign_extend8(imm8(op));
            return;

        case 0x8:
            switch ((op >> 8) & 0x0F) {
                case 0x0: write8(r[m] + (op & 0x0F), static_cast<std::uint8_t>(r[0])); return;
                case 0x1: write16(r[m] + (op & 0x0F) * 2, static_cast<std::uint16_t>(r[0])); return;
                case 0x4: r[0] = sign_extend8(read8(r[m] + (op & 0x0F))); return;
                case 0x5: r[0] = sign_extend16(read16(r[m] + (op & 0x0F) * 2)); return;
                case 0x8: set_t_flag(r[0] == sign_extend8(imm8(op))); return;  // CMP/EQ #imm,R0
                case 0x9:  // BT - no delay slot
                    if (t_flag()) {
                        registers_.pc = registers_.pc + 2 +
                                        static_cast<std::uint32_t>(disp8(op) * 2);
                        result.cycles = 3;
                    }
                    return;
                case 0xB:  // BF
                    if (!t_flag()) {
                        registers_.pc = registers_.pc + 2 +
                                        static_cast<std::uint32_t>(disp8(op) * 2);
                        result.cycles = 3;
                    }
                    return;
                case 0xD:  // BT/S - with a delay slot
                    if (t_flag()) {
                        branch_after_delay(registers_.pc + 2 +
                                           static_cast<std::uint32_t>(disp8(op) * 2));
                        result.cycles = 2;
                    }
                    return;
                case 0xF:  // BF/S
                    if (!t_flag()) {
                        branch_after_delay(registers_.pc + 2 +
                                           static_cast<std::uint32_t>(disp8(op) * 2));
                        result.cycles = 2;
                    }
                    return;
                default: break;
            }
            break;

        case 0x9:  // MOV.W @(disp,PC),Rn
            r[n] = sign_extend16(read16(registers_.pc + 2 + (op & 0x00FF) * 2));
            return;

        case 0xA:  // BRA
            branch_after_delay(registers_.pc + 2 + static_cast<std::uint32_t>(disp12(op) * 2));
            result.cycles = 2;
            return;

        case 0xB:  // BSR
            registers_.pr = registers_.pc + 2;
            branch_after_delay(registers_.pc + 2 + static_cast<std::uint32_t>(disp12(op) * 2));
            result.cycles = 2;
            return;

        case 0xC:
            switch ((op >> 8) & 0x0F) {
                case 0x0: write8(registers_.gbr + (op & 0xFF), static_cast<std::uint8_t>(r[0])); return;
                case 0x1: write16(registers_.gbr + (op & 0xFF) * 2, static_cast<std::uint16_t>(r[0])); return;
                case 0x2: write32(registers_.gbr + (op & 0xFF) * 4, r[0]); return;
                case 0x3: {  // TRAPA #imm
                    r[15] -= 4;
                    write32(r[15], registers_.sr);
                    r[15] -= 4;
                    write32(r[15], registers_.pc + 2);
                    registers_.pc = read32(registers_.vbr + (op & 0xFF) * 4);
                    result.cycles = 8;
                    return;
                }
                case 0x4: r[0] = sign_extend8(read8(registers_.gbr + (op & 0xFF))); return;
                case 0x5: r[0] = sign_extend16(read16(registers_.gbr + (op & 0xFF) * 2)); return;
                case 0x6: r[0] = read32(registers_.gbr + (op & 0xFF) * 4); return;
                case 0x7:  // MOVA @(disp,PC),R0 - the ADDRESS, not the contents
                    r[0] = ((registers_.pc + 2) & 0xFFFFFFFCu) + (op & 0xFF) * 4;
                    return;
                case 0x8: set_t_flag((r[0] & imm8(op)) == 0); return;
                case 0x9: r[0] &= imm8(op); return;
                case 0xA: r[0] ^= imm8(op); return;
                case 0xB: r[0] |= imm8(op); return;
                case 0xC: {  // TST.B #imm,@(R0,GBR)
                    const std::uint8_t value = read8(registers_.gbr + r[0]);
                    set_t_flag((value & imm8(op)) == 0);
                    result.cycles = 3;
                    return;
                }
                case 0xD: {  // AND.B
                    const std::uint32_t address = registers_.gbr + r[0];
                    write8(address, static_cast<std::uint8_t>(read8(address) & imm8(op)));
                    result.cycles = 3;
                    return;
                }
                case 0xE: {  // XOR.B
                    const std::uint32_t address = registers_.gbr + r[0];
                    write8(address, static_cast<std::uint8_t>(read8(address) ^ imm8(op)));
                    result.cycles = 3;
                    return;
                }
                case 0xF: {  // OR.B
                    const std::uint32_t address = registers_.gbr + r[0];
                    write8(address, static_cast<std::uint8_t>(read8(address) | imm8(op)));
                    result.cycles = 3;
                    return;
                }
                default: break;
            }
            break;

        case 0xD:  // MOV.L @(disp,PC),Rn - PC is masked to a longword boundary
            r[n] = read32(((registers_.pc + 2) & 0xFFFFFFFCu) + (op & 0x00FF) * 4);
            return;

        case 0xE:  // MOV #imm,Rn - signed
            r[n] = sign_extend8(imm8(op));
            return;

        default:
            break;
    }

    result.unimplemented = true;
    result.message = "unimplemented SH-2 opcode";  // which one is in result.opcode
}

std::string Sh2Cpu::describe() const {
    std::ostringstream out;
    out << "SH-2: PC=" << std::hex << registers_.pc << " SR=" << registers_.sr
        << " PR=" << registers_.pr << (halted_ ? " (halted)" : "");
    return out.str();
}

}  // namespace mercury::core
