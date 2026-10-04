#include "mercury/core/m68k_cpu.hpp"

#include <cstdlib>
#include <sstream>
#include <iomanip>

namespace mercury::core {

namespace {

// The 68000 opcode map is organised by the top four bits; everything below
// dispatches from there. Sizes in the MOVE block are encoded differently from
// every other instruction, which is a classic source of bugs, so the two
// decodings are kept in separate helpers rather than shared.
constexpr std::uint8_t kMoveSizeByte = 0b01;
constexpr std::uint8_t kMoveSizeLong = 0b10;
constexpr std::uint8_t kMoveSizeWord = 0b11;

}  // namespace

void M68kCpu::attach_bus(M68kBusCallbacks callbacks) {
    bus_ = std::move(callbacks);
}

bool M68kCpu::ready() const noexcept {
    return static_cast<bool>(bus_.read16) && static_cast<bool>(bus_.write16);
}

void M68kCpu::reset() noexcept {
    registers_ = M68kRegisters{};
    halted_ = false;
    stopped_ = false;
    pending_interrupt_level_ = 0;
    if (!ready()) {
        return;
    }
    // Hardware reset loads SSP from vector 0 and PC from vector 1.
    registers_.sr = 0x2700;
    registers_.ssp = read32(0x000000);
    registers_.a[7] = registers_.ssp;
    registers_.pc = read32(0x000004);
}

// --- bus -------------------------------------------------------------------

void M68kCpu::note_address_error(std::uint32_t address, bool write, bool program) noexcept {
    if (address_error_) {
        return;  // keep the first one; the instruction is already aborting
    }
    address_error_ = true;
    // The frame records the address the program computed, all 32 bits of it -
    // not the 24 bits that actually reach the bus.
    address_error_address_ = address;
    address_error_write_ = write;
    address_error_program_ = program;
    address_error_pc_valid_ = false;
}

std::uint16_t M68kCpu::read16(std::uint32_t address) noexcept {
    if (!ready()) {
        return 0xFFFF;
    }
    if ((address & 1) != 0) {
        note_address_error(address, false);
        return 0;
    }
    return bus_.read16(address & kAddressMask);
}

void M68kCpu::write16(std::uint32_t address, std::uint16_t value) noexcept {
    if (!ready()) {
        return;
    }
    if ((address & 1) != 0) {
        note_address_error(address, true);
        return;
    }
    bus_.write16(address & kAddressMask, value);
}

std::uint8_t M68kCpu::read8(std::uint32_t address) noexcept {
    if (bus_.read8) {
        return bus_.read8(address & kAddressMask);
    }
    // A 16-bit bus fetches the containing word and selects a half. Even
    // addresses take the high byte because the part is big-endian.
    const auto word = read16(address & ~std::uint32_t{1});
    return static_cast<std::uint8_t>((address & 1) ? (word & 0xFF) : (word >> 8));
}

void M68kCpu::write8(std::uint32_t address, std::uint8_t value) noexcept {
    if (bus_.write8) {
        bus_.write8(address & kAddressMask, value);
        return;
    }
    const auto aligned = address & ~std::uint32_t{1};
    const auto word = read16(aligned);
    const auto merged = (address & 1)
        ? static_cast<std::uint16_t>((word & 0xFF00) | value)
        : static_cast<std::uint16_t>((word & 0x00FF) | (static_cast<std::uint16_t>(value) << 8));
    write16(aligned, merged);
}

std::uint32_t M68kCpu::read32(std::uint32_t address) noexcept {
    const auto high = read16(address);
    const auto low = read16(address + 2);
    return (static_cast<std::uint32_t>(high) << 16) | low;
}

void M68kCpu::write32(std::uint32_t address, std::uint32_t value) noexcept {
    write16(address, static_cast<std::uint16_t>(value >> 16));
    write16(address + 2, static_cast<std::uint16_t>(value & 0xFFFF));
}

std::uint16_t M68kCpu::fetch16() noexcept {
    const auto value = read16(registers_.pc);
    // THE PROGRAM COUNTER IS 32 BITS; ONLY THE BUS IS 24.
    //
    // read16 puts the low 24 bits on the bus, which is all the Mars Check
    // Program needed: its code runs off the top of work RAM and must carry on
    // at the bottom of the map, and it does, because $1000000 reaches the bus
    // as $000000. That case used to be handled here by masking the PC itself
    // to 24 bits - which threw away the top byte of every PC.
    //
    // The top byte is not decoration. Code entered at a sign-extended work-RAM
    // address ($FFFF0600 rather than $00FF0600) carries $FF in it, and every
    // PC-relative LEA inherits it. The Terminator (Sega CD) clears memory with
    // `LEA d(PC),A0 / ... / CMPA.W A1,A0 / BNE`: the word compare sign-extends
    // $FD00 to $FFFFFD00, which only equals A0 if A0's top byte is $FF too.
    // With the PC masked A0 was $00FF..., the loop never ended, ran on through
    // the interrupt jump table at $FFFD00, and the next VINT jumped into zeros.
    registers_.pc += 2;
    return value;
}

std::uint32_t M68kCpu::fetch32() noexcept {
    const auto high = fetch16();
    const auto low = fetch16();
    return (static_cast<std::uint32_t>(high) << 16) | low;
}

// --- size helpers ----------------------------------------------------------

std::uint32_t M68kCpu::mask_of(Size size) noexcept {
    switch (size) {
        case Size::byte: return 0x000000FF;
        case Size::word: return 0x0000FFFF;
        case Size::longword: default: return 0xFFFFFFFF;
    }
}

std::uint32_t M68kCpu::sign_bit_of(Size size) noexcept {
    switch (size) {
        case Size::byte: return 0x00000080;
        case Size::word: return 0x00008000;
        case Size::longword: default: return 0x80000000;
    }
}

std::uint32_t M68kCpu::sign_extend(std::uint32_t value, Size size) noexcept {
    switch (size) {
        case Size::byte:
            return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value & 0xFF)));
        case Size::word:
            return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value & 0xFFFF)));
        case Size::longword: default:
            return value;
    }
}

// --- instruction timing ----------------------------------------------------

// Whether instructions are charged the real effective-address calculation time.
// Off: it is the accurate behaviour but it costs working games, and a game that
// runs beats a number in a test report.
//
// Not a constant, because it is not one setting for the whole library. A few
// games only run WITH it (El. Viento) and a few only run WITHOUT it (Steel
// Empire, NBA Live 98), so the per-game quirk table in genesis/game_quirks.cpp
// switches it at load time. Default stays off: that is the setting that boots
// the most games, and any game not named in the table gets it.
bool g_charge_ea_time = false;

// Whether the unassigned line-A / line-F opcode rows take their hardware
// vectors. ON: games use these rows as an extended instruction set - Heavy Nova
// has line-F opcodes inline in its code with a handler installed at vector 11 -
// so executing them as data burns the CPU on garbage instead.
constexpr bool kTrapUnassignedOpcodeRows = true;

// Time to compute an effective address, from the 68000 user manual's table.
// Every memory-referencing instruction is "base cost plus this", and leaving it
// out is what makes an emulator run the CPU too fast relative to the video
// chip: raster effects then land on the wrong scanlines even though every
// instruction produces the right result.
void M68kCpu::set_charge_ea_time(bool on) noexcept {
    // MERCURY_EA_TIME=0|1 pins the switch for a whole run, overriding the
    // per-game table. Testing a raster-timing theory has to change this and
    // nothing else, and editing the quirk table to do it also changes which
    // entry the game matches.
    static const int forced = [] {
        const char* e = std::getenv("MERCURY_EA_TIME");
        return e == nullptr ? -1 : (e[0] == '0' ? 0 : 1);
    }();
    g_charge_ea_time = forced < 0 ? on : (forced != 0);
}

std::uint32_t M68kCpu::ea_cycles(std::uint8_t mode, std::uint8_t reg, Size size) noexcept {
    // OFF BY DEFAULT, on for the handful of games the quirk table names.
    // Charging the real address-calculation time makes the CPU slower relative
    // to the video chip, and while that matches the hardware manual it cost more
    // games library-wide than it gained: Steel Empire and NBA Live 98 stop
    // booting with it on, El. Viento only boots with it on. So it is a per-game
    // switch rather than one global answer.
    //
    // The table below is correct and always used by the callers that cost no
    // games: address-error, multiply and DMA timing call it regardless.
    if (!g_charge_ea_time) {
        return 0;
    }
    const bool wide = (size == Size::longword);
    switch (mode) {
        case 0b000: case 0b001: return 0;            // Dn / An
        case 0b010: return wide ? 8 : 4;             // (An)
        case 0b011: return wide ? 8 : 4;             // (An)+
        case 0b100: return wide ? 10 : 6;            // -(An)
        case 0b101: return wide ? 12 : 8;            // (d16,An)
        case 0b110: return wide ? 14 : 10;           // (d8,An,Xn)
        case 0b111:
            switch (reg) {
                case 0b000: return wide ? 12 : 8;    // (xxx).W
                case 0b001: return wide ? 16 : 12;   // (xxx).L
                case 0b010: return wide ? 12 : 8;    // (d16,PC)
                case 0b011: return wide ? 14 : 10;   // (d8,PC,Xn)
                case 0b100: return wide ? 8 : 4;     // #immediate
                default: return 0;
            }
        default: return 0;
    }
}

// MOVE's destination costs less than the same mode would as a source: the
// predecrement is folded into the write, so it does not carry the extra two
// cycles it does when reading.
std::uint32_t M68kCpu::move_dest_cycles(std::uint8_t mode, std::uint8_t reg, Size size) noexcept {
    if (mode == 0b100) {
        return size == Size::longword ? 8u : 4u;
    }
    return ea_cycles(mode, reg, size);
}

// LEA, PEA, JMP and JSR only compute an address - they never fetch the operand -
// so they use a separate, shorter table rather than the general one above.
std::uint32_t M68kCpu::control_ea_cycles(std::uint8_t mode, std::uint8_t reg) noexcept {
    switch (mode) {
        case 0b010: return 0;                        // (An)
        case 0b101: return 4;                        // (d16,An)
        case 0b110: return 8;                        // (d8,An,Xn)
        case 0b111:
            switch (reg) {
                case 0b000: return 4;                // (xxx).W
                case 0b001: return 8;                // (xxx).L
                case 0b010: return 4;                // (d16,PC)
                case 0b011: return 8;                // (d8,PC,Xn)
                default: return 0;
            }
        default: return 0;
    }
}

// JMP and JSR are cheaper again than LEA for the same mode: the address goes
// straight into the program counter rather than through a register write.
std::uint32_t M68kCpu::jump_ea_cycles(std::uint8_t mode, std::uint8_t reg) noexcept {
    switch (mode) {
        case 0b010: return 0;                        // (An)
        case 0b101: return 2;                        // (d16,An)
        case 0b110: return 6;                        // (d8,An,Xn)
        case 0b111:
            switch (reg) {
                case 0b000: return 2;                // (xxx).W
                case 0b001: return 4;                // (xxx).L
                case 0b010: return 2;                // (d16,PC)
                case 0b011: return 6;                // (d8,PC,Xn)
                default: return 0;
            }
        default: return 0;
    }
}

// True for the addressing modes that hold the operand in a register or in the
// instruction stream. Several timings differ for those because no bus cycle is
// needed to reach the operand.
bool M68kCpu::ea_is_immediate_or_register(std::uint8_t mode, std::uint8_t reg) noexcept {
    return mode == 0b000 || mode == 0b001 || (mode == 0b111 && reg == 0b100);
}

// --- effective address -----------------------------------------------------

M68kCpu::Operand M68kCpu::resolve(std::uint8_t mode, std::uint8_t reg, Size size, bool& fault,
                                  bool writing) noexcept {
    Operand operand;
    fault = false;
    // An address error aborts part-way through the instruction, so the cycles
    // it reports are the ones already spent. The address calculation has
    // happened by then; the operand fetch it was leading to has not. Record the
    // difference so the exception can charge for the right part.
    {
        const auto total = ea_cycles(mode, reg, size);
        const auto fetch = (size == Size::longword) ? 8u : 4u;
        ea_setup_cycles_ = total > fetch ? total - fetch : 0u;
    }
    const auto increment = [&](std::uint8_t index) -> std::uint32_t {
        // Byte access through A7 still moves the pointer by two: the stack has
        // to stay word aligned or the next exception frame is misaligned.
        if (size == Size::longword) return 4;
        if (size == Size::word) return 2;
        return index == 7 ? 2 : 1;
    };

    switch (mode) {
        case 0b000:  // Dn
            operand.is_register = true;
            operand.reg = &registers_.d[reg];
            return operand;
        case 0b001:  // An
            operand.is_register = true;
            operand.is_address_register = true;
            operand.reg = &registers_.a[reg];
            return operand;
        case 0b010:  // (An)
            operand.address = registers_.a[reg];
            return operand;
        case 0b011: {  // (An)+
            operand.address = registers_.a[reg];
            const bool aborts = writing && size != Size::byte && (operand.address & 1) != 0;
            if (!aborts) {
                registers_.a[reg] += increment(reg);
            }
            return operand;
        }
        case 0b100: {  // -(An)
            // Unlike (An)+, the decrement IS part of forming the address, so it
            // sticks even when the access that follows faults.
            if (writing && size == Size::longword) {
                // A long STORE goes low word first: An -= 2, store low, An -= 2,
                // store high. Reads keep the whole -4 up front because they run
                // high word first from the fully decremented address.
                registers_.a[reg] -= 2;
                operand.address = registers_.a[reg];
                operand.split_predecrement = &registers_.a[reg];
                return operand;
            }
            registers_.a[reg] -= increment(reg);
            operand.address = registers_.a[reg];
            return operand;
        }
        case 0b101: {  // (d16,An)
            const auto displacement = static_cast<std::int16_t>(fetch16());
            operand.address = registers_.a[reg] + static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
            return operand;
        }
        case 0b110: {  // (d8,An,Xn)
            const auto extension = fetch16();
            const auto index_reg = static_cast<std::uint8_t>((extension >> 12) & 0x07);
            const bool index_is_address = (extension & 0x8000) != 0;
            const bool index_is_long = (extension & 0x0800) != 0;
            const auto displacement = static_cast<std::int8_t>(extension & 0xFF);
            auto index = index_is_address ? registers_.a[index_reg] : registers_.d[index_reg];
            if (!index_is_long) {
                index = sign_extend(index, Size::word);
            }
            operand.address = registers_.a[reg] + index
                            + static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
            return operand;
        }
        case 0b111:
            switch (reg) {
                case 0b000:  // (xxx).W - sign extended
                    operand.address = sign_extend(fetch16(), Size::word);
                    return operand;
                case 0b001:  // (xxx).L
                    operand.address = fetch32();
                    return operand;
                case 0b010: {  // (d16,PC) - relative to the extension word itself
                    const auto base = registers_.pc;
                    const auto displacement = static_cast<std::int16_t>(fetch16());
                    operand.address = base + static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
                    return operand;
                }
                case 0b011: {  // (d8,PC,Xn)
                    const auto base = registers_.pc;
                    const auto extension = fetch16();
                    const auto index_reg = static_cast<std::uint8_t>((extension >> 12) & 0x07);
                    const bool index_is_address = (extension & 0x8000) != 0;
                    const bool index_is_long = (extension & 0x0800) != 0;
                    const auto displacement = static_cast<std::int8_t>(extension & 0xFF);
                    auto index = index_is_address ? registers_.a[index_reg] : registers_.d[index_reg];
                    if (!index_is_long) {
                        index = sign_extend(index, Size::word);
                    }
                    operand.address = base + index
                                    + static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
                    return operand;
                }
                case 0b100: {  // #immediate - the operand IS the extension word(s)
                    operand.address = registers_.pc;
                    if (size == Size::longword) {
                        registers_.pc += 4;
                    } else {
                        // Byte immediates occupy a full word, low half.
                        registers_.pc += 2;
                        if (size == Size::byte) {
                            operand.address += 1;
                        }
                    }
                    return operand;
                }
                default:
                    break;
            }
            break;
        default:
            break;
    }
    fault = true;
    return operand;
}

std::uint32_t M68kCpu::read_operand(const Operand& operand, Size size) noexcept {
    if (operand.is_register) {
        return *operand.reg & mask_of(size);
    }
    switch (size) {
        case Size::byte: return read8(operand.address);
        case Size::word: return read16(operand.address);
        case Size::longword: default: return read32(operand.address);
    }
}

void M68kCpu::write_operand(const Operand& operand, Size size, std::uint32_t value) noexcept {
    // Once an access has faulted the instruction is aborting: the destination
    // is never written and the flags are never updated. Address registers
    // already adjusted by a pre/post-increment DO keep their new value, which
    // is why those happen in resolve() and are deliberately not undone.
    if (address_error_) {
        return;
    }
    if (operand.is_register) {
        if (operand.is_address_register || size == Size::longword) {
            // Address registers are always written full width; a word write to
            // An sign extends into the whole register.
            *operand.reg = (operand.is_address_register && size == Size::word)
                ? sign_extend(value, Size::word)
                : value;
            return;
        }
        const auto mask = mask_of(size);
        *operand.reg = (*operand.reg & ~mask) | (value & mask);
        return;
    }
    switch (size) {
        case Size::byte: write8(operand.address, static_cast<std::uint8_t>(value)); return;
        case Size::word: write16(operand.address, static_cast<std::uint16_t>(value)); return;
        case Size::longword: default:
            if (operand.split_predecrement != nullptr) {
                write16(operand.address, static_cast<std::uint16_t>(value & 0xFFFF));
                if (address_error_) {
                    return;  // An keeps the single -2, which is the whole point
                }
                *operand.split_predecrement -= 2;
                write16(*operand.split_predecrement, static_cast<std::uint16_t>(value >> 16));
                return;
            }
            write32(operand.address, value);
            return;
    }
}

// --- flags -----------------------------------------------------------------

void M68kCpu::set_flag(std::uint16_t mask, bool on) noexcept {
    if (on) {
        registers_.sr |= mask;
    } else {
        registers_.sr = static_cast<std::uint16_t>(registers_.sr & ~mask);
    }
}

void M68kCpu::set_logic_flags(std::uint32_t result, Size size) noexcept {
    if (address_error_) {
        return;  // aborting instruction: SR is preserved for the exception frame
    }
    const auto masked = result & mask_of(size);
    set_flag(kStatusNegative, (masked & sign_bit_of(size)) != 0);
    set_flag(kStatusZero, masked == 0);
    set_flag(kStatusOverflow, false);
    set_flag(kStatusCarry, false);
}

// a + b = result. Overflow when both operands share a sign that the result
// does not; carry when the unsigned sum wrapped.
void M68kCpu::set_add_flags(std::uint32_t a, std::uint32_t b, std::uint32_t result, Size size) noexcept {
    if (address_error_) {
        return;
    }
    const auto mask = mask_of(size);
    const auto sign = sign_bit_of(size);
    const auto ma = a & mask, mb = b & mask, mr = result & mask;
    const bool carry = (static_cast<std::uint64_t>(ma) + mb) > mask;
    const bool overflow = ((~(ma ^ mb)) & (ma ^ mr) & sign) != 0;
    set_flag(kStatusNegative, (mr & sign) != 0);
    set_flag(kStatusZero, mr == 0);
    set_flag(kStatusOverflow, overflow);
    set_flag(kStatusCarry, carry);
    set_flag(kStatusExtend, carry);
}

// a - b = result, where a is the destination. Borrow is a carry out.
void M68kCpu::set_sub_flags(std::uint32_t a, std::uint32_t b, std::uint32_t result, Size size) noexcept {
    if (address_error_) {
        return;
    }
    const auto mask = mask_of(size);
    const auto sign = sign_bit_of(size);
    const auto ma = a & mask, mb = b & mask, mr = result & mask;
    const bool borrow = mb > ma;
    const bool overflow = ((ma ^ mb) & (ma ^ mr) & sign) != 0;
    set_flag(kStatusNegative, (mr & sign) != 0);
    set_flag(kStatusZero, mr == 0);
    set_flag(kStatusOverflow, overflow);
    set_flag(kStatusCarry, borrow);
    set_flag(kStatusExtend, borrow);
}

void M68kCpu::set_compare_flags(std::uint32_t a, std::uint32_t b, std::uint32_t result, Size size) noexcept {
    if (address_error_) {
        return;
    }
    const auto extend = flag(kStatusExtend);
    set_sub_flags(a, b, result, size);
    set_flag(kStatusExtend, extend);  // CMP does not touch X
}

bool M68kCpu::condition(std::uint8_t code) const noexcept {
    const bool c = flag(kStatusCarry);
    const bool v = flag(kStatusOverflow);
    const bool z = flag(kStatusZero);
    const bool n = flag(kStatusNegative);
    switch (code & 0x0F) {
        case 0x0: return true;              // T
        case 0x1: return false;             // F
        case 0x2: return !c && !z;          // HI
        case 0x3: return c || z;            // LS
        case 0x4: return !c;                // CC
        case 0x5: return c;                 // CS
        case 0x6: return !z;                // NE
        case 0x7: return z;                 // EQ
        case 0x8: return !v;                // VC
        case 0x9: return v;                 // VS
        case 0xA: return !n;                // PL
        case 0xB: return n;                 // MI
        case 0xC: return n == v;            // GE
        case 0xD: return n != v;            // LT
        case 0xE: return !z && (n == v);    // GT
        case 0xF: default: return z || (n != v);  // LE
    }
}

// --- exceptions ------------------------------------------------------------

void M68kCpu::set_supervisor(bool on) noexcept {
    const bool was = supervisor();
    if (was == on) {
        return;
    }
    // A7 aliases whichever stack pointer the current mode selects, so the
    // active value has to be parked before the other one is swapped in.
    if (was) {
        registers_.ssp = registers_.a[7];
        registers_.a[7] = registers_.usp;
    } else {
        registers_.usp = registers_.a[7];
        registers_.a[7] = registers_.ssp;
    }
    set_flag(kStatusSupervisor, on);
}

void M68kCpu::enter_exception(std::uint8_t vector, std::uint32_t stacked_pc) noexcept {
    if (vector < exception_tally_.size()) {
        ++exception_tally_[vector];
    }
    last_exception_vector_ = vector;
    last_exception_pc_ = stacked_pc;
    const auto saved_sr = registers_.sr;
    set_supervisor(true);
    set_flag(kStatusTrace, false);
    // Group 1/2 frame: PC then SR, pushed on the supervisor stack.
    registers_.a[7] -= 4;
    write32(registers_.a[7], stacked_pc);
    registers_.a[7] -= 2;
    write16(registers_.a[7], saved_sr);
    registers_.pc = read32(static_cast<std::uint32_t>(vector) * 4);
}

void M68kCpu::set_program_counter(std::uint32_t value) noexcept {
    registers_.pc = value;
    // An odd destination is an address error raised by the transferring
    // instruction itself. The PC still takes the odd value: the exception frame
    // records it, and the handler's own vector fetch replaces it.
    if ((value & 1) != 0) {
        note_address_error(value, false, true);   // instruction fetch
        address_error_pc_valid_ = true;
        address_error_pc_ = value - 4;
    }
}
void M68kCpu::raise(M68kException vector) noexcept {
    // Most group-2 exceptions stack the address of the NEXT instruction, so the
    // handler returns past the one that caused them. The three that come from
    // an opcode the CPU cannot execute - illegal, line A and line F - stack the
    // address OF that opcode instead, because their whole purpose is to let
    // software look at it. Games use the line rows as an extended instruction
    // set, with parameters inlined after the opcode; hand such a handler the
    // wrong PC and it reads its parameters from the wrong place.
    const bool faulting = vector == M68kException::illegal_instruction ||
                          vector == M68kException::line_a ||
                          vector == M68kException::line_f;
    enter_exception(static_cast<std::uint8_t>(vector), faulting ? instruction_pc_ : registers_.pc);
}

// Address and bus errors use the seven-word group-0 frame, not the short one:
// a status word describing the access, the address that faulted, the opcode,
// then the usual SR and PC.
void M68kCpu::take_address_error() noexcept {
    address_error_ = false;
    const auto saved_sr = registers_.sr;
    const auto fault_address = address_error_address_;
    const bool was_write = address_error_write_;

    set_supervisor(true);
    set_flag(kStatusTrace, false);

    // The frame's PC trails the fetch pointer by one word. With no extension
    // words that lands on the instruction itself; with one, one word past it.
    // Both cases come out right as (pc at fault) - 2.
    registers_.a[7] -= 4;
    // A faulting STORE happens a word later in the microcode than a faulting
    // load - the next prefetch has already been issued - so its frame records
    // the fetch pointer as-is while a load records one word back.
    const auto frame_pc = address_error_pc_valid_ ? address_error_pc_
                                                  : (registers_.pc - 2);
    write32(registers_.a[7], frame_pc);
    registers_.a[7] -= 2;
    write16(registers_.a[7], saved_sr);
    registers_.a[7] -= 2;
    write16(registers_.a[7], current_opcode_);
    registers_.a[7] -= 4;
    write32(registers_.a[7], fault_address);
    registers_.a[7] -= 2;
    // Special status word. Only the low five bits are defined - R/W in bit 4,
    // I/N in bit 3, function code in bits 2-0 - and the upper eleven are left
    // holding the instruction word, which is what real hardware (and the test
    // suite generated from it) shows.
    const bool was_supervisor = (saved_sr & kStatusSupervisor) != 0;
    // Bit 4 = read, bit 3 = the access was an instruction fetch rather than
    // data, bits 2-0 = function code. Program space is 6/2, data space 5/1.
    const std::uint16_t status = static_cast<std::uint16_t>(
        (current_opcode_ & 0xFFE0) |
        (was_write ? 0x0000 : 0x0010) |
        (address_error_program_ ? 0x0008 : 0x0000) |
        (was_supervisor ? (address_error_program_ ? 0x0006 : 0x0005)
                        : (address_error_program_ ? 0x0002 : 0x0001)));
    write16(registers_.a[7], status);

    registers_.pc = read32(static_cast<std::uint32_t>(M68kException::address_error) * 4);
    address_error_ = false;
}

void M68kCpu::set_interrupt_level(int level) noexcept {
    pending_interrupt_level_ = level < 0 ? 0 : (level > 7 ? 7 : level);
}

void M68kCpu::service_interrupt(int level) noexcept {
    stopped_ = false;
    enter_exception(static_cast<std::uint8_t>(24 + level), registers_.pc);  // autovector
    registers_.sr = static_cast<std::uint16_t>((registers_.sr & ~kStatusInterruptMask) |
                                               (static_cast<std::uint16_t>(level) << 8));
}

// --- step ------------------------------------------------------------------

M68kStepResult M68kCpu::step() noexcept {
    M68kStepResult result;
    if (!ready() || halted_) {
        result.message = halted_ ? "halted" : "no bus attached";
        return result;
    }

    // Level 7 is edge triggered and always gets through; the rest must beat the
    // mask in SR.
    const auto mask = static_cast<int>((registers_.sr & kStatusInterruptMask) >> 8);
    if (pending_interrupt_level_ > 0 &&
        (pending_interrupt_level_ == 7 || pending_interrupt_level_ > mask)) {
        const auto level = pending_interrupt_level_;
        result.pc_before = registers_.pc;
        service_interrupt(level);
        result.executed = true;
        result.cycles = 44;
        result.pc_after = registers_.pc;
        result.interrupt_serviced = level;
        result.message = "interrupt accepted";
        return result;
    }

    if (stopped_) {
        result.executed = true;
        result.cycles = 4;
        result.pc_before = registers_.pc;
        result.pc_after = registers_.pc;
        result.message = "stopped";
        return result;
    }

    result.pc_before = registers_.pc;
    instruction_pc_ = registers_.pc;
    address_error_ = false;
    // An instruction fetch from an odd address faults before anything executes.
    if ((registers_.pc & 1) != 0) {
        current_opcode_ = 0;
        note_address_error(registers_.pc, false, true);   // instruction fetch
        take_address_error();
        result.executed = true;
        result.cycles = 50;
        result.pc_after = registers_.pc;
        result.message = "address error on instruction fetch";
        return result;
    }
    const auto opcode = fetch16();
    current_opcode_ = opcode;
    result.opcode = opcode;
    cycles_this_step_ = 0;
    ea_setup_cycles_ = 0;

    const auto top = static_cast<std::uint8_t>((opcode >> 12) & 0x0F);
    bool handled = false;
    bool fault = false;

    switch (top) {
        case 0x1: case 0x2: case 0x3: {
            // MOVE / MOVEA. Size lives in bits 13-12 with its own encoding, and
            // the destination field is stored reg-then-mode, reversed from the
            // source. Both are easy to get backwards.
            const auto encoded = static_cast<std::uint8_t>((opcode >> 12) & 0x03);
            const Size size = encoded == kMoveSizeByte ? Size::byte
                            : encoded == kMoveSizeWord ? Size::word
                            : Size::longword;
            const auto src_mode = static_cast<std::uint8_t>((opcode >> 3) & 0x07);
            const auto src_reg = static_cast<std::uint8_t>(opcode & 0x07);
            const auto dst_mode = static_cast<std::uint8_t>((opcode >> 6) & 0x07);
            const auto dst_reg = static_cast<std::uint8_t>((opcode >> 9) & 0x07);

            const auto source = resolve(src_mode, src_reg, size, fault);
            if (fault) break;
            const auto value = read_operand(source, size);
            // If reading the source faulted, the instruction stops there - the
            // destination EA is never even computed, so its pre/post-increment
            // must not happen.
            if (address_error_) {
                handled = true;
                break;
            }
            if (dst_mode != 0b001) {
                // Flags come from the source and are committed before the
                // destination is written: a destination fault does not undo
                // them. MOVEA sets no flags at all.
                set_logic_flags(value, size);
            }
            const auto destination = resolve(dst_mode, dst_reg, size, fault, true);
            if (fault) break;
            if (dst_mode == 0b001) {
                write_operand(destination, Size::longword, sign_extend(value, size));
            } else {
                write_operand(destination, size, value);
            }
            // A STORE fault records a different PC than a load fault, and the
            // offset depends on the destination mode: -(An) reports the fetch
            // pointer as-is, and (xxx).l reports it four bytes back because the
            // absolute address words are not counted. Everything else lands on
            // the default (pc - 2). Measured against the 680x0 test suite.
            if (address_error_ && !address_error_pc_valid_) {
                if (dst_mode == 0b100) {
                    address_error_pc_ = registers_.pc;
                    address_error_pc_valid_ = true;
                } else if (dst_mode == 0b111 && dst_reg == 0b001) {
                    address_error_pc_ = registers_.pc - 4;
                    address_error_pc_valid_ = true;
                }
            }
            cycles_this_step_ = 4 + ea_cycles(src_mode, src_reg, size) +
                                move_dest_cycles(dst_mode, dst_reg, size);
            handled = true;
            break;
        }
        case 0x5: {  // ADDQ / SUBQ / Scc / DBcc
            const auto encoded = static_cast<std::uint8_t>((opcode >> 6) & 0x03);
            const auto mode = static_cast<std::uint8_t>((opcode >> 3) & 0x07);
            const auto reg = static_cast<std::uint8_t>(opcode & 0x07);
            if (encoded == 0x03) {
                if (mode == 0b001) {  // DBcc
                    const auto code = static_cast<std::uint8_t>((opcode >> 8) & 0x0F);
                    const auto base = registers_.pc;
                    const auto displacement = static_cast<std::int16_t>(fetch16());
                    if (!condition(code)) {
                        const auto counter = static_cast<std::uint16_t>((registers_.d[reg] & 0xFFFF) - 1);
                        registers_.d[reg] = (registers_.d[reg] & 0xFFFF0000u) | counter;
                        if (counter != 0xFFFF) {
                            set_program_counter(base + static_cast<std::uint32_t>(displacement));
                            cycles_this_step_ = 10;
                        } else {
                            cycles_this_step_ = 14;
                        }
                    } else {
                        cycles_this_step_ = 12;
                    }
                    handled = true;
                    break;
                }
                // Scc: set the byte to all ones or all zeroes.
                const auto code = static_cast<std::uint8_t>((opcode >> 8) & 0x0F);
                const auto target = resolve(mode, reg, Size::byte, fault);
                if (fault) break;
                write_operand(target, Size::byte, condition(code) ? 0xFF : 0x00);
                cycles_this_step_ = (mode == 0b000)
                    ? (condition(code) ? 6u : 4u)
                    : 8u + ea_cycles(mode, reg, Size::byte);
                handled = true;
                break;
            }
            const Size size = encoded == 0 ? Size::byte : encoded == 1 ? Size::word : Size::longword;
            auto amount = static_cast<std::uint32_t>((opcode >> 9) & 0x07);
            if (amount == 0) amount = 8;   // 0 encodes 8
            const bool subtract = (opcode & 0x0100) != 0;
            const auto target = resolve(mode, reg, size, fault);
            if (fault) break;
            const auto value = read_operand(target, size);
            if (address_error_) { handled = true; break; }
            const auto result = subtract ? value - amount : value + amount;
            if (target.is_address_register) {
                // ADDQ/SUBQ on an address register works on all 32 bits and
                // sets no flags at all, whatever the encoded size says.
                write_operand(target, Size::longword, subtract ? *target.reg - amount
                                                               : *target.reg + amount);
            } else {
                write_operand(target, size, result);
                if (subtract) set_sub_flags(value, amount, result, size);
                else set_add_flags(value, amount, result, size);
            }
            cycles_this_step_ = 4;
            handled = true;
            break;
        }
        case 0x9:    // SUB / SUBA / SUBX
        case 0xD: {  // ADD / ADDA / ADDX
            const bool subtract = (top == 0x9);
            const auto encoded = static_cast<std::uint8_t>((opcode >> 6) & 0x07);
            const auto reg = static_cast<std::uint8_t>((opcode >> 9) & 0x07);
            const auto mode = static_cast<std::uint8_t>((opcode >> 3) & 0x07);
            const auto ea_reg = static_cast<std::uint8_t>(opcode & 0x07);

            if (encoded == 0b011 || encoded == 0b111) {  // ADDA/SUBA
                const Size size = encoded == 0b011 ? Size::word : Size::longword;
                const auto source = resolve(mode, ea_reg, size, fault);
                if (fault) break;
                const auto value = sign_extend(read_operand(source, size), size);
                if (address_error_) { handled = true; break; }
                registers_.a[reg] = subtract ? registers_.a[reg] - value : registers_.a[reg] + value;
                cycles_this_step_ = (size == Size::word
                                        ? 8u
                                        : (ea_is_immediate_or_register(mode, ea_reg) ? 8u : 6u)) +
                                    ea_cycles(mode, ea_reg, size);
                handled = true;
                break;
            }
            // ADDX/SUBX hide inside the "<ea> is the destination" half of the
            // row: bits 5-4 are zero and bit 3 chooses register or predecrement
            // operands. Left to the generic path they would run as a plain
            // ADD/SUB, silently dropping the carry chain that makes multi-word
            // arithmetic work.
            if ((encoded & 0x04) != 0 && (opcode & 0x0030) == 0x0000) {
                const Size size = (encoded & 0x03) == 0 ? Size::byte
                                : (encoded & 0x03) == 1 ? Size::word : Size::longword;
                const bool memory = (opcode & 0x0008) != 0;
                std::uint32_t source_value = 0;
                std::uint32_t dest_value = 0;
                Operand destination;
                if (memory) {
                    bool source_fault = false, dest_fault = false;
                    const auto source_ea = resolve(0b100, ea_reg, size, source_fault);
                    if (source_fault) break;
                    source_value = read_operand(source_ea, size);
                    if (address_error_) { handled = true; break; }
                    destination = resolve(0b100, reg, size, dest_fault);
                    if (dest_fault) break;
                    dest_value = read_operand(destination, size);
                    if (address_error_) { handled = true; break; }
                } else {
                    source_value = registers_.d[ea_reg] & mask_of(size);
                    destination.is_register = true;
                    destination.reg = &registers_.d[reg];
                    dest_value = registers_.d[reg] & mask_of(size);
                }
                const std::uint32_t extend = flag(kStatusExtend) ? 1u : 0u;
                const auto result = subtract ? (dest_value - source_value - extend)
                                             : (dest_value + source_value + extend);
                const auto masked = result & mask_of(size);
                write_operand(destination, size, masked);
                if (subtract) set_sub_flags(dest_value, source_value, result, size);
                else set_add_flags(dest_value, source_value, result, size);
                // Z is only ever cleared by these: a zero result leaves an
                // earlier zero standing so a multi-word sum reports correctly.
                set_flag(kStatusZero, flag(kStatusZero) && masked == 0);
                cycles_this_step_ = memory ? 18 : 4;
                handled = true;
                break;
            }
            const Size size = (encoded & 0x03) == 0 ? Size::byte
                            : (encoded & 0x03) == 1 ? Size::word : Size::longword;
            const bool ea_is_destination = (encoded & 0x04) != 0;
            const auto source = resolve(mode, ea_reg, size, fault);
            if (fault) break;
            if (ea_is_destination) {
                const auto value = read_operand(source, size);
                if (address_error_) { handled = true; break; }
                const auto operand = registers_.d[reg] & mask_of(size);
                const auto result = subtract ? value - operand : value + operand;
                write_operand(source, size, result);
                if (subtract) set_sub_flags(value, operand, result, size);
                else set_add_flags(value, operand, result, size);
            } else {
                const auto value = read_operand(source, size);
                if (address_error_) { handled = true; break; }
                const auto operand = registers_.d[reg] & mask_of(size);
                const auto result = subtract ? operand - value : operand + value;
                const auto mask = mask_of(size);
                registers_.d[reg] = (registers_.d[reg] & ~mask) | (result & mask);
                if (subtract) set_sub_flags(operand, value, result, size);
                else set_add_flags(operand, value, result, size);
            }
            if (ea_is_destination) {
                // Read-modify-write through the EA: 8 base for byte/word, 12
                // for long, plus the address calculation.
                cycles_this_step_ = (size == Size::longword ? 12u : 8u) +
                                    ea_cycles(mode, ea_reg, size);
            } else {
                // Into a data register. The long form costs six unless the
                // operand needs no bus cycle to reach, in which case eight.
                const std::uint32_t base = size == Size::longword
                    ? (ea_is_immediate_or_register(mode, ea_reg) ? 8u : 6u)
                    : 4u;
                cycles_this_step_ = base + ea_cycles(mode, ea_reg, size);
            }
            handled = true;
            break;
        }
        case 0xB: {  // CMP / CMPA / EOR
            const auto encoded = static_cast<std::uint8_t>((opcode >> 6) & 0x07);
            const auto reg = static_cast<std::uint8_t>((opcode >> 9) & 0x07);
            const auto mode = static_cast<std::uint8_t>((opcode >> 3) & 0x07);
            const auto ea_reg = static_cast<std::uint8_t>(opcode & 0x07);

            if (encoded == 0b011 || encoded == 0b111) {  // CMPA
                const Size size = encoded == 0b011 ? Size::word : Size::longword;
                const auto source = resolve(mode, ea_reg, size, fault);
                if (fault) break;
                const auto value = sign_extend(read_operand(source, size), size);
                if (address_error_) { handled = true; break; }
                const auto result = registers_.a[reg] - value;
                set_compare_flags(registers_.a[reg], value, result, Size::longword);
                cycles_this_step_ = 6 + ea_cycles(mode, ea_reg, size);
                handled = true;
                break;
            }
            const Size size = (encoded & 0x03) == 0 ? Size::byte
                            : (encoded & 0x03) == 1 ? Size::word : Size::longword;

            // CMPM (Ay)+,(Ax)+ shares this row with EOR: same high bit, but an
            // address-register-indirect-with-postincrement EA. Treating it as
            // EOR writes to memory that should only be read.
            if ((encoded & 0x04) != 0 && mode == 0b001) {
                bool source_fault = false, dest_fault = false;
                const auto source_ea = resolve(0b011, ea_reg, size, source_fault);
                if (source_fault) break;
                const auto source_value = read_operand(source_ea, size);
                if (address_error_) { handled = true; break; }
                const auto dest_ea = resolve(0b011, reg, size, dest_fault);
                if (dest_fault) break;
                const auto dest_value = read_operand(dest_ea, size);
                if (address_error_) { handled = true; break; }
                set_compare_flags(dest_value, source_value, dest_value - source_value, size);
                cycles_this_step_ = 12;
                handled = true;
                break;
            }

            const auto source = resolve(mode, ea_reg, size, fault);
            if (fault) break;
            if ((encoded & 0x04) == 0) {  // CMP Dn,<ea>
                const auto value = read_operand(source, size);
                if (address_error_) { handled = true; break; }
                const auto operand = registers_.d[reg] & mask_of(size);
                set_compare_flags(operand, value, operand - value, size);
            } else {  // EOR <ea>,Dn  (destination is the EA)
                const auto value = read_operand(source, size);
                if (address_error_) { handled = true; break; }
                const auto result = value ^ (registers_.d[reg] & mask_of(size));
                write_operand(source, size, result);
                set_logic_flags(result, size);
            }
            if ((encoded & 0x04) == 0) {
                cycles_this_step_ = (size == Size::longword ? 6u : 4u) +
                                    ea_cycles(mode, ea_reg, size);
            } else {
                const bool to_register = (mode == 0b000);
                cycles_this_step_ = (size == Size::longword ? (to_register ? 8u : 12u)
                                                            : (to_register ? 4u : 8u)) +
                                    ea_cycles(mode, ea_reg, size);
            }
            handled = true;
            break;
        }
        case 0x8:    // OR / DIVU / DIVS / SBCD
        case 0xC: {  // AND / MULU / MULS / ABCD / EXG
            const auto encoded = static_cast<std::uint8_t>((opcode >> 6) & 0x07);
            const auto reg = static_cast<std::uint8_t>((opcode >> 9) & 0x07);
            const auto mode = static_cast<std::uint8_t>((opcode >> 3) & 0x07);
            const auto ea_reg = static_cast<std::uint8_t>(opcode & 0x07);
            const bool is_and = (top == 0xC);

            // ABCD/SBCD share the byte "<ea> is destination" slot, marked by
            // bits 8-4 being 10000. Bit 3 picks register or -(An) operands.
            if ((opcode & 0x01F0) == 0x0100) {
                const bool memory = (opcode & 0x0008) != 0;
                std::uint32_t source_value = 0;
                std::uint32_t dest_value = 0;
                Operand destination;
                if (memory) {
                    bool source_fault = false, dest_fault = false;
                    const auto source_ea = resolve(0b100, ea_reg, Size::byte, source_fault);
                    if (source_fault) break;
                    source_value = read_operand(source_ea, Size::byte);
                    if (address_error_) { handled = true; break; }
                    destination = resolve(0b100, reg, Size::byte, dest_fault);
                    if (dest_fault) break;
                    dest_value = read_operand(destination, Size::byte);
                    if (address_error_) { handled = true; break; }
                } else {
                    source_value = registers_.d[ea_reg] & 0xFF;
                    destination.is_register = true;
                    destination.reg = &registers_.d[reg];
                    dest_value = registers_.d[reg] & 0xFF;
                }
                const std::int32_t extend = flag(kStatusExtend) ? 1 : 0;
                std::uint8_t result = 0;
                bool carry = false;
                if (is_and) {  // ABCD
                    std::int32_t low = static_cast<std::int32_t>(dest_value & 0x0F) +
                                       static_cast<std::int32_t>(source_value & 0x0F) + extend;
                    std::int32_t high = static_cast<std::int32_t>((dest_value >> 4) & 0x0F) +
                                        static_cast<std::int32_t>((source_value >> 4) & 0x0F);
                    if (low > 9) { low -= 10; ++high; }
                    if (high > 9) { high -= 10; carry = true; }
                    result = static_cast<std::uint8_t>(((high & 0x0F) << 4) | (low & 0x0F));
                } else {  // SBCD
                    std::int32_t low = static_cast<std::int32_t>(dest_value & 0x0F) -
                                       static_cast<std::int32_t>(source_value & 0x0F) - extend;
                    std::int32_t high = static_cast<std::int32_t>((dest_value >> 4) & 0x0F) -
                                        static_cast<std::int32_t>((source_value >> 4) & 0x0F);
                    if (low < 0) { low += 10; --high; }
                    if (high < 0) { high += 10; carry = true; }
                    result = static_cast<std::uint8_t>(((high & 0x0F) << 4) | (low & 0x0F));
                }
                write_operand(destination, Size::byte, result);
                set_flag(kStatusCarry, carry);
                set_flag(kStatusExtend, carry);
                set_flag(kStatusNegative, (result & 0x80) != 0);
                if (result != 0) set_flag(kStatusZero, false);
                cycles_this_step_ = memory ? 18 : 6;
                handled = true;
                break;
            }

            if (encoded == 0b011 || encoded == 0b111) {
                // MULU/MULS (AND row) and DIVU/DIVS (OR row).
                const auto source = resolve(mode, ea_reg, Size::word, fault);
                if (fault) break;
                const auto value = read_operand(source, Size::word);
                if (address_error_) { handled = true; break; }
                const bool is_signed = (encoded == 0b111);
                if (is_and) {  // multiply
                    std::uint32_t product;
                    if (is_signed) {
                        const auto a = static_cast<std::int32_t>(static_cast<std::int16_t>(registers_.d[reg] & 0xFFFF));
                        const auto b = static_cast<std::int32_t>(static_cast<std::int16_t>(value & 0xFFFF));
                        product = static_cast<std::uint32_t>(a * b);
                    } else {
                        product = (registers_.d[reg] & 0xFFFF) * (value & 0xFFFF);
                    }
                    registers_.d[reg] = product;
                    set_logic_flags(product, Size::longword);
                    // Multiply cost depends on the SOURCE bit pattern, because
                    // the hardware does shift-and-add and only pays for the
                    // bits that need adding. MULU counts set bits; MULS counts
                    // transitions in the operand shifted up by one, which is
                    // the Booth encoding the signed unit uses.
                    const auto word = static_cast<std::uint16_t>(value & 0xFFFF);
                    std::uint32_t work = 0;
                    if (is_signed) {
                        const std::uint32_t pairs = static_cast<std::uint32_t>(word) << 1;
                        for (int bit = 0; bit < 16; ++bit) {
                            const auto pair = (pairs >> bit) & 0x03;
                            if (pair == 0x01 || pair == 0x02) ++work;
                        }
                    } else {
                        for (int bit = 0; bit < 16; ++bit) {
                            if ((word >> bit) & 1) ++work;
                        }
                    }
                    cycles_this_step_ = 38 + 2 * work +
                                        ea_cycles(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                                  static_cast<std::uint8_t>(opcode & 0x07),
                                                  Size::word);
                } else {       // divide
                    if ((value & 0xFFFF) == 0) {
                        raise(M68kException::divide_by_zero);
                        cycles_this_step_ = 38;
                        handled = true;
                        break;
                    }
                    if (is_signed) {
                        const auto dividend = static_cast<std::int32_t>(registers_.d[reg]);
                        const auto divisor = static_cast<std::int32_t>(static_cast<std::int16_t>(value & 0xFFFF));
                        const auto quotient = dividend / divisor;
                        const auto remainder = dividend % divisor;
                        set_flag(kStatusCarry, false);
                        if (quotient > 32767 || quotient < -32768) {
                            // Overflow leaves the destination alone and only
                            // sets V; N and Z keep their previous values.
                            set_flag(kStatusOverflow, true);
                        } else {
                            registers_.d[reg] = (static_cast<std::uint32_t>(remainder & 0xFFFF) << 16) |
                                                (static_cast<std::uint32_t>(quotient) & 0xFFFF);
                            set_logic_flags(static_cast<std::uint32_t>(quotient) & 0xFFFF, Size::word);
                        }
                    } else {
                        const auto dividend = registers_.d[reg];
                        const auto divisor = value & 0xFFFF;
                        const auto quotient = dividend / divisor;
                        const auto remainder = dividend % divisor;
                        set_flag(kStatusCarry, false);
                        if (quotient > 0xFFFF) {
                            set_flag(kStatusOverflow, true);
                        } else {
                            registers_.d[reg] = ((remainder & 0xFFFF) << 16) | (quotient & 0xFFFF);
                            set_logic_flags(quotient & 0xFFFF, Size::word);
                        }
                    }
                    cycles_this_step_ = 140;
                }
                handled = true;
                break;
            }

            // EXG lives in the AND row with specific opcode bits.
            if (is_and && (opcode & 0x0130) == 0x0100) {
                const auto opmode = static_cast<std::uint8_t>((opcode >> 3) & 0x1F);
                if (opmode == 0x08) { std::swap(registers_.d[reg], registers_.d[ea_reg]); handled = true; }
                else if (opmode == 0x09) { std::swap(registers_.a[reg], registers_.a[ea_reg]); handled = true; }
                else if (opmode == 0x11) { std::swap(registers_.d[reg], registers_.a[ea_reg]); handled = true; }
                if (handled) { cycles_this_step_ = 6; break; }
            }

            const Size size = (encoded & 0x03) == 0 ? Size::byte
                            : (encoded & 0x03) == 1 ? Size::word : Size::longword;
            const bool ea_is_destination = (encoded & 0x04) != 0;
            const auto operand_ea = resolve(mode, ea_reg, size, fault);
            if (fault) break;
            const auto value = read_operand(operand_ea, size);
            if (address_error_) { handled = true; break; }
            const auto other = registers_.d[reg] & mask_of(size);
            const auto result = is_and ? (value & other) : (value | other);
            if (ea_is_destination) {
                write_operand(operand_ea, size, result);
            } else {
                const auto mask = mask_of(size);
                registers_.d[reg] = (registers_.d[reg] & ~mask) | (result & mask);
            }
            set_logic_flags(result, size);
            if (ea_is_destination) {
                cycles_this_step_ = (size == Size::longword ? 12u : 8u) +
                                    ea_cycles(mode, ea_reg, size);
            } else {
                const std::uint32_t base = size == Size::longword
                    ? (ea_is_immediate_or_register(mode, ea_reg) ? 8u : 6u)
                    : 4u;
                cycles_this_step_ = base + ea_cycles(mode, ea_reg, size);
            }
            handled = true;
            break;
        }
        case 0x7: {  // MOVEQ
            if ((opcode & 0x0100) == 0) {
                const auto reg = static_cast<std::uint8_t>((opcode >> 9) & 0x07);
                const auto value = sign_extend(static_cast<std::uint8_t>(opcode & 0xFF), Size::byte);
                registers_.d[reg] = value;
                set_logic_flags(value, Size::longword);
                cycles_this_step_ = 4;
                handled = true;
            }
            break;
        }
        case 0x6: {  // Bcc / BSR / BRA
            const auto code = static_cast<std::uint8_t>((opcode >> 8) & 0x0F);
            std::int32_t displacement = static_cast<std::int8_t>(opcode & 0xFF);
            const auto base = registers_.pc;
            if ((opcode & 0xFF) == 0x00) {
                displacement = static_cast<std::int16_t>(fetch16());
            }
            if (code == 0x01) {  // BSR
                registers_.a[7] -= 4;
                write32(registers_.a[7], registers_.pc);
                set_program_counter(base + static_cast<std::uint32_t>(displacement));
                cycles_this_step_ = 18;
            } else if (condition(code)) {
                set_program_counter(base + static_cast<std::uint32_t>(displacement));
                cycles_this_step_ = 10;
            } else {
                cycles_this_step_ = 8;
            }
            handled = true;
            break;
        }
        case 0xE: {  // shifts and rotates
            const auto encoded = static_cast<std::uint8_t>((opcode >> 6) & 0x03);
            if (encoded == 0x03) {
                // Memory form: always one bit, always a word, EA is the target.
                const auto type = static_cast<std::uint8_t>((opcode >> 9) & 0x03);
                const bool left = (opcode & 0x0100) != 0;
                const auto target = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), Size::word, fault);
                if (fault) break;
                const auto value = read_operand(target, Size::word);
                if (address_error_) { handled = true; break; }
                std::uint32_t result = value & 0xFFFF;
                bool carry = false;
                const bool extend_in = flag(kStatusExtend);
                if (left) {
                    carry = (result & 0x8000) != 0;
                    result = (result << 1) & 0xFFFF;
                    if (type == 2 && extend_in) result |= 1;                 // ROXL
                    if (type == 3 && carry) result |= 1;                     // ROL
                } else {
                    carry = (result & 0x0001) != 0;
                    const bool msb = (result & 0x8000) != 0;
                    result = (result >> 1) & 0xFFFF;
                    if (type == 0 && msb) result |= 0x8000;                  // ASR keeps sign
                    if (type == 2 && extend_in) result |= 0x8000;            // ROXR
                    if (type == 3 && carry) result |= 0x8000;                // ROR
                }
                write_operand(target, Size::word, result);
                set_logic_flags(result, Size::word);
                set_flag(kStatusCarry, carry);
                // ASL is the only one of the four that can overflow: V is set
                // when the sign bit changes as a result of the shift.
                if (type == 0 && left) {
                    set_flag(kStatusOverflow, ((value ^ result) & 0x8000) != 0);
                }
                if (type != 3) set_flag(kStatusExtend, carry);  // RO* leaves X alone
                cycles_this_step_ = 8;
                handled = true;
                break;
            }

            const Size size = encoded == 0 ? Size::byte : encoded == 1 ? Size::word : Size::longword;
            const auto reg = static_cast<std::uint8_t>(opcode & 0x07);
            const bool left = (opcode & 0x0100) != 0;
            const bool count_in_register = (opcode & 0x0020) != 0;
            const auto type = static_cast<std::uint8_t>((opcode >> 3) & 0x03);
            auto count = static_cast<std::uint32_t>((opcode >> 9) & 0x07);
            if (count_in_register) {
                count = registers_.d[(opcode >> 9) & 0x07] % 64;
            } else if (count == 0) {
                count = 8;  // 0 encodes 8 for the immediate form
            }

            const auto mask = mask_of(size);
            const auto sign = sign_bit_of(size);
            const auto width = size == Size::byte ? 8u : size == Size::word ? 16u : 32u;
            std::uint32_t value = registers_.d[reg] & mask;
            const std::uint32_t original = value;
            bool carry = false;
            bool overflow = false;

            for (std::uint32_t i = 0; i < count; ++i) {
                const bool extend_in = flag(kStatusExtend);
                if (left) {
                    carry = (value & sign) != 0;
                    const auto before = value;
                    value = (value << 1) & mask;
                    if (type == 2 && extend_in) value |= 1;      // ROXL
                    if (type == 3 && carry) value |= 1;          // ROL
                    // ASL sets V if the sign bit ever changes during the shift.
                    if (type == 0 && ((before ^ value) & sign) != 0) overflow = true;
                } else {
                    carry = (value & 1) != 0;
                    const bool msb = (value & sign) != 0;
                    value = (value >> 1) & mask;
                    if (type == 0 && msb) value |= sign;          // ASR
                    if (type == 2 && extend_in) value |= sign;    // ROXR
                    if (type == 3 && carry) value |= sign;        // ROR
                }
                if (type != 3) set_flag(kStatusExtend, carry);
            }

            // For a right ASR/LSR the carry is bit (count-1) of the ORIGINAL
            // operand, taken unsigned within its width. Iterating gives the
            // same answer while the count fits, but once it exceeds the width
            // an arithmetic shift keeps feeding sign bits out - and hardware
            // reports 0 there, not the sign. X follows C.
            if (!left && (type == 0 || type == 1) && count > 0) {
                carry = count <= width && ((original >> (count - 1)) & 1) != 0;
                set_flag(kStatusExtend, carry);
            }

            registers_.d[reg] = (registers_.d[reg] & ~mask) | (value & mask);
            set_flag(kStatusNegative, (value & sign) != 0);
            set_flag(kStatusZero, (value & mask) == 0);
            set_flag(kStatusOverflow, overflow);
            // A zero shift count leaves the operand alone and clears C - except
            // for ROXL/ROXR, where C comes out as a copy of X. That exception is
            // in the manual and is what real code relies on to test X.
            set_flag(kStatusCarry, count == 0 ? (type == 2 && flag(kStatusExtend)) : carry);
            // Register shifts cost a fixed setup plus two cycles per bit, and
            // the setup is two cycles longer for a long operand.
            cycles_this_step_ = (size == Size::longword ? 8u : 6u) + 2 * count;
            handled = true;
            break;
        }
        case 0x0: {  // immediate operations and bit manipulation
            const auto ea_mode = static_cast<std::uint8_t>((opcode >> 3) & 0x07);
            const auto ea_reg = static_cast<std::uint8_t>(opcode & 0x07);

            // MOVEP sits in the bit-manipulation row and is told apart only by
            // its addressing mode being 001. It has to be checked first or a
            // sound driver's register pokes turn into BTSTs.
            if ((opcode & 0x0100) != 0 && ea_mode == 0b001) {
                const auto reg = static_cast<std::uint8_t>((opcode >> 9) & 0x07);
                const auto opmode = static_cast<std::uint8_t>((opcode >> 6) & 0x07);
                const auto displacement = static_cast<std::int16_t>(fetch16());
                auto address = registers_.a[ea_reg] +
                               static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
                const bool longword = (opmode & 0x01) != 0;
                const bool to_memory = (opmode & 0x02) != 0;
                const int bytes = longword ? 4 : 2;
                if (to_memory) {
                    for (int i = 0; i < bytes; ++i) {
                        const auto shift = (bytes - 1 - i) * 8;
                        write8(address, static_cast<std::uint8_t>((registers_.d[reg] >> shift) & 0xFF));
                        address += 2;  // every OTHER byte - that is the point
                    }
                } else {
                    std::uint32_t value = 0;
                    for (int i = 0; i < bytes; ++i) {
                        value = (value << 8) | read8(address);
                        address += 2;
                    }
                    if (longword) {
                        registers_.d[reg] = value;
                    } else {
                        registers_.d[reg] = (registers_.d[reg] & 0xFFFF0000) | (value & 0xFFFF);
                    }
                }
                cycles_this_step_ = longword ? 24 : 16;
                handled = true;
                break;
            }

            if ((opcode & 0x0100) != 0 || (opcode & 0x0F00) == 0x0800) {
                // BTST/BCHG/BCLR/BSET. Bit number comes from Dn (bit 8 set) or
                // from an immediate word (the 0000 1000 form).
                const bool from_register = (opcode & 0x0100) != 0;
                const auto operation = static_cast<std::uint8_t>((opcode >> 6) & 0x03);
                std::uint32_t bit = 0;
                if (from_register) {
                    bit = registers_.d[(opcode >> 9) & 0x07];
                } else {
                    bit = fetch16() & 0xFF;
                }
                // Register targets are 32 bits wide, memory targets are 8.
                const bool memory = ea_mode != 0b000;
                const Size size = memory ? Size::byte : Size::longword;
                bit &= memory ? 7u : 31u;
                const auto target = resolve(ea_mode, ea_reg, size, fault);
                if (fault) break;
                const auto value = read_operand(target, size);
                if (address_error_) { handled = true; break; }
                const std::uint32_t bit_mask = 1u << bit;
                set_flag(kStatusZero, (value & bit_mask) == 0);
                if (operation != 0) {
                    const auto result = operation == 1 ? (value ^ bit_mask)
                                      : operation == 2 ? (value & ~bit_mask)
                                                       : (value | bit_mask);
                    write_operand(target, size, result);
                }
                // BTST only reads, so it is cheapest; BCLR needs an extra pair
                // of cycles over BCHG/BSET because it has to invert the mask.
                // An immediate bit number costs four more than one in a
                // register, since it arrives as an extension word.
                if (memory) {
                    const std::uint32_t base = (operation == 0) ? 4u : 8u;
                    cycles_this_step_ = base + (from_register ? 0u : 4u) +
                                        ea_cycles(ea_mode, ea_reg, Size::byte);
                } else {
                    const std::uint32_t base = (operation == 0) ? 6u    // BTST
                                             : (operation == 2) ? 10u   // BCLR
                                                                : 8u;   // BCHG / BSET
                    cycles_this_step_ = base + (from_register ? 0u : 4u);
                }
                handled = true;
                break;
            }

            const auto operation = static_cast<std::uint8_t>((opcode >> 9) & 0x07);
            const auto encoded = static_cast<std::uint8_t>((opcode >> 6) & 0x03);
            if (encoded == 0x03) break;  // not an immediate ALU form
            const Size size = encoded == 0 ? Size::byte : encoded == 1 ? Size::word : Size::longword;

            // ORI/ANDI/EORI #imm,CCR and #imm,SR use EA 111100.
            if (ea_mode == 0b111 && ea_reg == 0b100 &&
                (operation == 0 || operation == 1 || operation == 5)) {
                const auto immediate = fetch16();
                const bool to_sr = (size == Size::word);
                if (to_sr && !supervisor()) {
                    raise(M68kException::privilege_violation);
                    handled = true;
                    break;
                }
                const std::uint16_t current = to_sr ? registers_.sr
                                                    : static_cast<std::uint16_t>(registers_.sr & 0x001F);
                const std::uint16_t operand = to_sr ? immediate
                                                    : static_cast<std::uint16_t>(immediate & 0x001F);
                std::uint16_t result = operation == 0 ? static_cast<std::uint16_t>(current | operand)
                                     : operation == 1 ? static_cast<std::uint16_t>(current & operand)
                                                      : static_cast<std::uint16_t>(current ^ operand);
                if (to_sr) {
                    // Only T, S, the interrupt mask and the five condition
                    // codes exist; bits 14, 12, 11, 7-5 read back as zero, so a
                    // plain OR/EOR into SR must be masked or those undefined
                    // bits stick.
                    result = static_cast<std::uint16_t>(result & kStatusWritableMask);
                    set_supervisor((result & kStatusSupervisor) != 0);
                    registers_.sr = result;
                } else {
                    registers_.sr = static_cast<std::uint16_t>((registers_.sr & 0xFF00) | (result & 0x001F));
                }
                cycles_this_step_ = 20;
                handled = true;
                break;
            }

            // The immediate operand comes first, then the destination EA.
            std::uint32_t immediate = 0;
            if (size == Size::longword) {
                immediate = fetch32();
            } else {
                immediate = fetch16() & mask_of(size);
            }
            const auto target = resolve(ea_mode, ea_reg, size, fault);
            if (fault) break;
            const auto value = read_operand(target, size);
            if (address_error_) { handled = true; break; }

            switch (operation) {
                case 0: {  // ORI
                    const auto result = value | immediate;
                    write_operand(target, size, result);
                    set_logic_flags(result, size);
                    break;
                }
                case 1: {  // ANDI
                    const auto result = value & immediate;
                    write_operand(target, size, result);
                    set_logic_flags(result, size);
                    break;
                }
                case 2: {  // SUBI
                    const auto result = value - immediate;
                    write_operand(target, size, result);
                    set_sub_flags(value, immediate, result, size);
                    break;
                }
                case 3: {  // ADDI
                    const auto result = value + immediate;
                    write_operand(target, size, result);
                    set_add_flags(value, immediate, result, size);
                    break;
                }
                case 5: {  // EORI
                    const auto result = value ^ immediate;
                    write_operand(target, size, result);
                    set_logic_flags(result, size);
                    break;
                }
                case 6: {  // CMPI - compares only
                    set_compare_flags(value, immediate, value - immediate, size);
                    break;
                }
                default:
                    break;
            }
            cycles_this_step_ = 8;
            handled = true;
            break;
        }
        case 0x4: {  // miscellaneous
            if (opcode == 0x4E71) {  // NOP
                cycles_this_step_ = 4;
                handled = true;
            } else if (opcode == 0x4E75) {  // RTS
                set_program_counter(read32(registers_.a[7]));
                registers_.a[7] += 4;
                cycles_this_step_ = 16;
                handled = true;
            } else if (opcode == 0x4E73) {  // RTE
                if (!supervisor()) {
                    raise(M68kException::privilege_violation);
                    handled = true;
                    break;
                }
                const auto sr = read16(registers_.a[7]);
                registers_.a[7] += 2;
                const auto pc = read32(registers_.a[7]);
                registers_.a[7] += 4;
                set_supervisor((sr & kStatusSupervisor) != 0);
                registers_.sr = sr;
                set_program_counter(pc);
                cycles_this_step_ = 20;
                handled = true;
            } else if ((opcode & 0xFFC0) == 0x4E80) {  // JSR
                const auto target = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), Size::longword, fault);
                if (fault) break;
                // An odd target faults BEFORE the return address is pushed, so
                // the stack holds only the exception frame. Pushing first and
                // faulting after leaves SP four bytes low.
                if ((target.address & 1) != 0) {
                    set_program_counter(target.address);
                    handled = true;
                    break;
                }
                registers_.a[7] -= 4;
                write32(registers_.a[7], registers_.pc);
                set_program_counter(target.address);
                cycles_this_step_ = 16 + jump_ea_cycles(
                    static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                    static_cast<std::uint8_t>(opcode & 0x07));
                handled = true;
            } else if ((opcode & 0xFFC0) == 0x4EC0) {  // JMP
                const auto target = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), Size::longword, fault);
                if (fault) break;
                set_program_counter(target.address);
                cycles_this_step_ = 8 + jump_ea_cycles(
                    static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                    static_cast<std::uint8_t>(opcode & 0x07));
                handled = true;
            } else if ((opcode & 0xF1C0) == 0x41C0) {  // LEA
                const auto reg = static_cast<std::uint8_t>((opcode >> 9) & 0x07);
                const auto source = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), Size::longword, fault);
                if (fault) break;
                registers_.a[reg] = source.address;
                cycles_this_step_ = 4 + control_ea_cycles(
                    static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                    static_cast<std::uint8_t>(opcode & 0x07));
                handled = true;
            } else if (opcode == 0x4E70) {  // RESET
                // Pulses the reset line to the peripherals. The CPU's own state
                // is untouched, so from in here it is a long NOP.
                if (!supervisor()) {
                    raise(M68kException::privilege_violation);
                } else {
                    cycles_this_step_ = 132;
                }
                handled = true;
            } else if (opcode == 0x4E72) {  // STOP #imm
                const auto immediate = fetch16();
                if (!supervisor()) {
                    raise(M68kException::privilege_violation);
                } else {
                    set_supervisor((immediate & kStatusSupervisor) != 0);
                    registers_.sr = static_cast<std::uint16_t>(immediate & kStatusWritableMask);
                    stopped_ = true;
                    cycles_this_step_ = 4;
                }
                handled = true;
            } else if (opcode == 0x4E76) {  // TRAPV
                if (flag(kStatusOverflow)) {
                    raise(M68kException::trapv);
                } else {
                    cycles_this_step_ = 4;
                }
                handled = true;
            } else if (opcode == 0x4E77) {  // RTR
                // Unlike RTE this restores only the condition codes, so the
                // supervisor bit and interrupt mask survive it.
                const auto ccr = read16(registers_.a[7]);
                registers_.a[7] += 2;
                const auto pc = read32(registers_.a[7]);
                registers_.a[7] += 4;
                registers_.sr = static_cast<std::uint16_t>((registers_.sr & 0xFF00) | (ccr & 0x001F));
                set_program_counter(pc);
                cycles_this_step_ = 20;
                handled = true;
            } else if ((opcode & 0xFFF0) == 0x4E40) {  // TRAP #vector
                raise(static_cast<M68kException>(32 + (opcode & 0x0F)));
                cycles_this_step_ = 38;
                handled = true;
            } else if ((opcode & 0xFFF8) == 0x4E50) {  // LINK An,#displacement
                const auto reg = static_cast<std::uint8_t>(opcode & 0x07);
                const auto displacement = static_cast<std::int16_t>(fetch16());
                registers_.a[7] -= 4;
                write32(registers_.a[7], registers_.a[reg]);
                registers_.a[reg] = registers_.a[7];
                registers_.a[7] += static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
                cycles_this_step_ = 16;
                handled = true;
            } else if ((opcode & 0xFFF8) == 0x4E58) {  // UNLK An
                const auto reg = static_cast<std::uint8_t>(opcode & 0x07);
                registers_.a[7] = registers_.a[reg];
                registers_.a[reg] = read32(registers_.a[7]);
                registers_.a[7] += 4;
                cycles_this_step_ = 12;
                handled = true;
            } else if ((opcode & 0xFFF0) == 0x4E60) {  // MOVE USP
                if (!supervisor()) {
                    raise(M68kException::privilege_violation);
                } else {
                    const auto reg = static_cast<std::uint8_t>(opcode & 0x07);
                    // In supervisor mode A7 is SSP, so USP is the parked copy.
                    if ((opcode & 0x08) != 0) {
                        registers_.a[reg] = registers_.usp;  // USP -> An
                    } else {
                        registers_.usp = registers_.a[reg];  // An -> USP
                    }
                    cycles_this_step_ = 4;
                }
                handled = true;
            } else if ((opcode & 0xFFF8) == 0x4840) {  // SWAP Dn
                const auto reg = static_cast<std::uint8_t>(opcode & 0x07);
                const auto value = (registers_.d[reg] >> 16) | (registers_.d[reg] << 16);
                registers_.d[reg] = value;
                set_logic_flags(value, Size::longword);
                cycles_this_step_ = 4;
                handled = true;
            } else if ((opcode & 0xFFC0) == 0x4840) {  // PEA <ea>
                const auto source = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), Size::longword, fault);
                if (fault) break;
                registers_.a[7] -= 4;
                write32(registers_.a[7], source.address);
                cycles_this_step_ = 12 + control_ea_cycles(
                    static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                    static_cast<std::uint8_t>(opcode & 0x07));
                handled = true;
            } else if ((opcode & 0xFFB8) == 0x4880) {  // EXT.w / EXT.l
                const auto reg = static_cast<std::uint8_t>(opcode & 0x07);
                if ((opcode & 0x0040) != 0) {
                    registers_.d[reg] = sign_extend(registers_.d[reg], Size::word);
                    set_logic_flags(registers_.d[reg], Size::longword);
                } else {
                    const auto extended = sign_extend(registers_.d[reg], Size::byte) & 0xFFFF;
                    registers_.d[reg] = (registers_.d[reg] & 0xFFFF0000) | extended;
                    set_logic_flags(extended, Size::word);
                }
                cycles_this_step_ = 4;
                handled = true;
            } else if ((opcode & 0xFB80) == 0x4880) {  // MOVEM
                const bool to_registers = (opcode & 0x0400) != 0;
                const Size size = (opcode & 0x0040) != 0 ? Size::longword : Size::word;
                const auto step = size == Size::longword ? 4u : 2u;
                const auto mode = static_cast<std::uint8_t>((opcode >> 3) & 0x07);
                const auto ea_reg = static_cast<std::uint8_t>(opcode & 0x07);
                const auto mask = fetch16();

                // MOVEM computes the address itself rather than going through
                // resolve(), because the pre/post-increment modes have to step
                // once per register, not once per instruction.
                std::uint32_t address = 0;
                if (mode == 0b011 || mode == 0b100) {
                    address = registers_.a[ea_reg];
                } else {
                    const auto operand = resolve(mode, ea_reg, size, fault);
                    if (fault) break;
                    address = operand.address;
                }

                std::uint32_t moved = 0;
                if (!to_registers && mode == 0b100) {
                    // Predecrement store runs the mask backwards: bit 0 is A7.
                    for (int i = 15; i >= 0; --i) {
                        if ((mask & (1u << (15 - i))) == 0) continue;
                        address -= step;
                        const auto value = i < 8 ? registers_.d[i] : registers_.a[i - 8];
                        if (size == Size::longword) write32(address, value);
                        else write16(address, static_cast<std::uint16_t>(value & 0xFFFF));
                        ++moved;
                    }
                    registers_.a[ea_reg] = address;
                } else if (!to_registers) {
                    for (int i = 0; i < 16; ++i) {
                        if ((mask & (1u << i)) == 0) continue;
                        const auto value = i < 8 ? registers_.d[i] : registers_.a[i - 8];
                        if (size == Size::longword) write32(address, value);
                        else write16(address, static_cast<std::uint16_t>(value & 0xFFFF));
                        address += step;
                        ++moved;
                    }
                } else {
                    for (int i = 0; i < 16; ++i) {
                        if ((mask & (1u << i)) == 0) continue;
                        // A word load sign extends into the whole register, for
                        // address and data registers alike.
                        const std::uint32_t value = size == Size::longword
                            ? read32(address)
                            : sign_extend(read16(address), Size::word);
                        if (i < 8) registers_.d[i] = value;
                        else registers_.a[i - 8] = value;
                        address += step;
                        ++moved;
                    }
                    if (mode == 0b011) {
                        registers_.a[ea_reg] = address;
                    }
                }
                cycles_this_step_ = 12 + moved * (size == Size::longword ? 8 : 4);
                handled = true;
            } else if ((opcode & 0xF1C0) == 0x4180) {  // CHK <ea>,Dn
                const auto reg = static_cast<std::uint8_t>((opcode >> 9) & 0x07);
                const auto source = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), Size::word, fault);
                if (fault) break;
                const auto bound = static_cast<std::int16_t>(read_operand(source, Size::word));
                if (address_error_) { handled = true; break; }
                const auto value = static_cast<std::int16_t>(registers_.d[reg] & 0xFFFF);
                if (value < 0 || value > bound) {
                    set_flag(kStatusNegative, value < 0);
                    raise(M68kException::chk);
                } else {
                    cycles_this_step_ = 10;
                }
                handled = true;
            } else if ((opcode & 0xFFC0) == 0x40C0) {  // MOVE SR,<ea>
                const auto destination = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                                 static_cast<std::uint8_t>(opcode & 0x07), Size::word, fault, true);
                if (fault) break;
                write_operand(destination, Size::word, registers_.sr);
                cycles_this_step_ = 6;
                handled = true;
            } else if ((opcode & 0xFFC0) == 0x44C0) {  // MOVE <ea>,CCR
                const auto source = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), Size::word, fault);
                if (fault) break;
                const auto value = read_operand(source, Size::word);
                if (address_error_) { handled = true; break; }
                registers_.sr = static_cast<std::uint16_t>((registers_.sr & 0xFF00) | (value & 0x001F));
                cycles_this_step_ = 12;
                handled = true;
            } else if ((opcode & 0xFFC0) == 0x46C0) {  // MOVE <ea>,SR
                if (!supervisor()) {
                    raise(M68kException::privilege_violation);
                    handled = true;
                    break;
                }
                const auto source = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), Size::word, fault);
                if (fault) break;
                const auto value = read_operand(source, Size::word);
                if (address_error_) { handled = true; break; }
                // The supervisor bit has to move first: it decides which of the
                // two stack pointers A7 is about to alias.
                set_supervisor((value & kStatusSupervisor) != 0);
                registers_.sr = static_cast<std::uint16_t>(value & kStatusWritableMask);
                cycles_this_step_ = 12;
                handled = true;
            } else if ((opcode & 0xFFC0) == 0x4AC0) {  // TAS <ea>
                if (opcode == 0x4AFC) {
                    raise(M68kException::illegal_instruction);
                    handled = true;
                    break;
                }
                const auto target = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), Size::byte, fault);
                if (fault) break;
                const auto value = read_operand(target, Size::byte);
                if (address_error_) { handled = true; break; }
                set_logic_flags(value, Size::byte);
                write_operand(target, Size::byte, value | 0x80);
                cycles_this_step_ = 14;
                handled = true;
            } else if ((opcode & 0xFFC0) == 0x4800) {  // NBCD <ea>
                const auto target = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), Size::byte, fault);
                if (fault) break;
                const auto value = read_operand(target, Size::byte);
                if (address_error_) { handled = true; break; }
                const auto extend = flag(kStatusExtend) ? 1u : 0u;
                std::int32_t low = 0 - static_cast<std::int32_t>(value & 0x0F) - static_cast<std::int32_t>(extend);
                std::int32_t high = 0 - static_cast<std::int32_t>((value >> 4) & 0x0F);
                bool borrow = false;
                if (low < 0) { low += 10; --high; }
                if (high < 0) { high += 10; borrow = true; }
                const auto result = static_cast<std::uint8_t>(((high & 0x0F) << 4) | (low & 0x0F));
                write_operand(target, Size::byte, result);
                set_flag(kStatusCarry, borrow);
                set_flag(kStatusExtend, borrow);
                set_flag(kStatusNegative, (result & 0x80) != 0);
                if (result != 0) set_flag(kStatusZero, false);
                cycles_this_step_ = 6;
                handled = true;
            } else if ((opcode & 0xFF00) == 0x4000 || (opcode & 0xFF00) == 0x4400 ||
                       (opcode & 0xFF00) == 0x4600) {  // NEGX / NEG / NOT
                const auto encoded = static_cast<std::uint8_t>((opcode >> 6) & 0x03);
                if (encoded == 0x03) {
                    break;  // the .w-only forms are handled above
                }
                const Size size = encoded == 0 ? Size::byte : encoded == 1 ? Size::word : Size::longword;
                const auto target = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                            static_cast<std::uint8_t>(opcode & 0x07), size, fault);
                if (fault) break;
                const auto value = read_operand(target, size);
                if (address_error_) { handled = true; break; }
                const auto mask = mask_of(size);
                if ((opcode & 0xFF00) == 0x4600) {  // NOT
                    const auto result = ~value & mask;
                    write_operand(target, size, result);
                    set_logic_flags(result, size);
                } else {
                    const bool with_extend = (opcode & 0xFF00) == 0x4000;
                    const std::uint32_t extend = (with_extend && flag(kStatusExtend)) ? 1u : 0u;
                    const auto result = (0u - value - extend) & mask;
                    write_operand(target, size, result);
                    set_sub_flags(0, value, result, size);
                    if (with_extend) {
                        // NEGX only ever clears Z; a zero result leaves an
                        // earlier zero standing, which is how multi-word
                        // negation accumulates its result.
                        set_flag(kStatusZero, flag(kStatusZero) && result == 0);
                    }
                }
                cycles_this_step_ = 4;
                handled = true;
            } else if ((opcode & 0xFF00) == 0x4A00) {  // TST
                const auto encoded = static_cast<std::uint8_t>((opcode >> 6) & 0x03);
                if (encoded != 0x03) {
                    const Size size = encoded == 0 ? Size::byte : encoded == 1 ? Size::word : Size::longword;
                    const auto source = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                                static_cast<std::uint8_t>(opcode & 0x07), size, fault);
                    if (fault) break;
                    set_logic_flags(read_operand(source, size), size);
                    cycles_this_step_ = 4 + ea_cycles(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                                      static_cast<std::uint8_t>(opcode & 0x07), size);
                    handled = true;
                }
            } else if ((opcode & 0xFF00) == 0x4200) {  // CLR
                const auto encoded = static_cast<std::uint8_t>((opcode >> 6) & 0x03);
                if (encoded != 0x03) {
                    const Size size = encoded == 0 ? Size::byte : encoded == 1 ? Size::word : Size::longword;
                    const auto destination = resolve(static_cast<std::uint8_t>((opcode >> 3) & 0x07),
                                                     static_cast<std::uint8_t>(opcode & 0x07), size, fault);
                    if (fault) break;
                    // CLR is a read-modify-write on the 68000: it reads the
                    // operand and throws the value away before writing zero.
                    // The read is observable - it can fault, and it costs a bus
                    // cycle - so it cannot be optimised out.
                    (void)read_operand(destination, size);
                    if (address_error_) { handled = true; break; }
                    write_operand(destination, size, 0);
                    set_logic_flags(0, size);
                    // CLR is a read-modify-write, so a memory destination pays
                    // the full write cost on top of the address calculation.
                    {
                        const auto clr_mode = static_cast<std::uint8_t>((opcode >> 3) & 0x07);
                        const auto clr_reg = static_cast<std::uint8_t>(opcode & 0x07);
                        const bool to_register = (clr_mode == 0b000);
                        cycles_this_step_ = (size == Size::longword ? (to_register ? 6u : 12u)
                                                                    : (to_register ? 4u : 8u)) +
                                            ea_cycles(clr_mode, clr_reg, size);
                    }
                    handled = true;
                }
            }
            break;
        }
        default:
            break;
    }

    if (kTrapUnassignedOpcodeRows && !handled && (top == 0xA || top == 0xF)) {
        // The two unassigned opcode rows are not "unimplemented" - the hardware
        // defines them. Line A and line F each take their own vector so software
        // can extend the instruction set or catch a wild jump, and a game whose
        // handler is installed expects to be re-entered there. Executing the
        // pattern as if it were an instruction instead lets a runaway PC keep
        // running, which is how a game that is otherwise drawing correctly ends
        // up burning most of its cycles on garbage.
        raise(top == 0xA ? M68kException::line_a : M68kException::line_f);
        result.executed = true;
        result.cycles = 34;
        result.pc_after = registers_.pc;
        result.message = top == 0xA ? "line-A trap" : "line-F trap";
        return result;
    }

    if (!handled) {
        // Unimplemented for now rather than silently wrong. The harness counts
        // these so coverage is always visible.
        result.executed = false;
        result.unimplemented = true;
        result.pc_after = registers_.pc;
        std::ostringstream out;
        out << "unimplemented opcode " << std::hex << std::uppercase << std::setw(4)
            << std::setfill('0') << opcode;
        result.message = out.str();
        return result;
    }

    // An address error raised anywhere in the instruction body aborts it here,
    // leaving whatever partial register updates already happened - which is
    // what the hardware does and what the test suite encodes.
    if (address_error_) {
        take_address_error();
        result.executed = true;
        result.cycles = 50 + ea_setup_cycles_;
        result.pc_after = registers_.pc;
        result.message = "address error";
        return result;
    }

    result.executed = true;
    result.cycles = cycles_this_step_;
    result.pc_after = registers_.pc;
    return result;
}

std::string M68kCpu::describe() const {
    std::ostringstream out;
    out << "68000: PC=" << std::hex << std::uppercase << std::setw(6) << std::setfill('0')
        << registers_.pc << " SR=" << std::setw(4) << registers_.sr
        << (supervisor() ? " [supervisor]" : " [user]");
    return out.str();
}

}  // namespace mercury::core


