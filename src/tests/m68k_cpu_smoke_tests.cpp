// Smoke tests for the 68000 core.
//
// These are hand-written and deliberately small: they prove the foundation
// (reset vectors, big-endian fetch, effective addressing, the two stack
// pointers, flags) before the SingleStepTests JSON suite goes in. That suite is
// the real oracle; these catch the gross mistakes early and stay useful as a
// fast check that nothing structural broke.

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "mercury/core/m68k_cpu.hpp"

namespace {

using mercury::core::M68kCpu;
using mercury::core::M68kBusCallbacks;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  PASS  %s\n", what.c_str());
    } else {
        std::printf("  FAIL  %s\n", what.c_str());
        ++g_failures;
    }
}

struct Machine {
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(0x10000, 0x00);
    M68kCpu cpu;

    Machine() {
        M68kBusCallbacks bus;
        bus.read16 = [this](std::uint32_t address) -> std::uint16_t {
            const auto a = address & 0xFFFF;
            return static_cast<std::uint16_t>((memory[a] << 8) | memory[(a + 1) & 0xFFFF]);
        };
        bus.write16 = [this](std::uint32_t address, std::uint16_t value) {
            const auto a = address & 0xFFFF;
            memory[a] = static_cast<std::uint8_t>(value >> 8);
            memory[(a + 1) & 0xFFFF] = static_cast<std::uint8_t>(value & 0xFF);
        };
        cpu.attach_bus(bus);
    }

    void poke16(std::uint32_t address, std::uint16_t value) {
        memory[address] = static_cast<std::uint8_t>(value >> 8);
        memory[address + 1] = static_cast<std::uint8_t>(value & 0xFF);
    }
    void poke32(std::uint32_t address, std::uint32_t value) {
        poke16(address, static_cast<std::uint16_t>(value >> 16));
        poke16(address + 2, static_cast<std::uint16_t>(value & 0xFFFF));
    }
    [[nodiscard]] std::uint16_t peek16(std::uint32_t address) const {
        return static_cast<std::uint16_t>((memory[address] << 8) | memory[address + 1]);
    }

    // Reset vectors then program at 0x1000.
    void boot(std::initializer_list<std::uint16_t> program) {
        poke32(0x0000, 0x00008000);  // SSP
        poke32(0x0004, 0x00001000);  // PC
        std::uint32_t at = 0x1000;
        for (auto word : program) {
            poke16(at, word);
            at += 2;
        }
        cpu.reset();
    }
};

}  // namespace

int main() {
    std::printf("-- 68000 smoke tests --\n");

    {   // Reset must load SSP and PC from the vector table, big-endian.
        Machine m;
        m.boot({0x4E71});
        check(m.cpu.registers().a[7] == 0x00008000, "reset loads SSP from vector 0");
        check(m.cpu.registers().pc == 0x00001000, "reset loads PC from vector 1");
        check(m.cpu.supervisor(), "reset enters supervisor mode");
    }

    {   // MOVEQ sign extends its byte to the full 32-bit register.
        Machine m;
        m.boot({0x7EFF});  // MOVEQ #-1,D7
        auto r = m.cpu.step();
        check(r.executed, "MOVEQ executes");
        check(m.cpu.registers().d[7] == 0xFFFFFFFF, "MOVEQ #-1 sign extends to FFFFFFFF");
        check(m.cpu.registers().sr & M68kCpu::kStatusNegative, "MOVEQ #-1 sets N");
    }

    {   // MOVE.W immediate to data register, then to memory via (xxx).W.
        Machine m;
        m.boot({0x303C, 0x1234,       // MOVE.W #$1234,D0
                0x31C0, 0x2000});     // MOVE.W D0,($2000).W
        (void)m.cpu.step();
        check((m.cpu.registers().d[0] & 0xFFFF) == 0x1234, "MOVE.W #imm,D0 loads the word");
        (void)m.cpu.step();
        check(m.peek16(0x2000) == 0x1234, "MOVE.W D0,(xxx).W stores big-endian");
    }

    {   // MOVE.B only touches the low byte of the destination register.
        Machine m;
        m.boot({0x203C, 0xAABB, 0xCCDD,   // MOVE.L #$AABBCCDD,D0
                0x1200});                  // MOVE.B D0,D1
        (void)m.cpu.step();
        m.cpu.mutable_registers().d[1] = 0x11223344;
        (void)m.cpu.step();
        check(m.cpu.registers().d[0] == 0xAABBCCDD, "MOVE.L #imm loads all 32 bits");
        check(m.cpu.registers().d[1] == 0x112233DD, "MOVE.B leaves the upper 24 bits of Dn alone");
    }

    {   // Predecrement/postincrement, including the A7 byte special case.
        Machine m;
        m.boot({0x4E71});
        m.cpu.mutable_registers().a[0] = 0x3000;
        m.poke16(0x1000, 0x1018);  // MOVE.B (A0)+,D0
        m.memory[0x3000] = 0x5A;
        m.cpu.mutable_registers().pc = 0x1000;
        (void)m.cpu.step();
        check((m.cpu.registers().d[0] & 0xFF) == 0x5A, "MOVE.B (A0)+ reads the byte");
        check(m.cpu.registers().a[0] == 0x3001, "(An)+ byte advances by 1");
    }

    {   // LEA computes an address without reading memory.
        Machine m;
        m.boot({0x41F9, 0x0001, 0x2340});  // LEA ($00012340).L,A0
        auto r = m.cpu.step();
        check(r.executed, "LEA executes");
        check(m.cpu.registers().a[0] == 0x00012340, "LEA loads the effective address");
    }

    {   // BSR pushes a return address that RTS consumes.
        Machine m;
        m.boot({0x6100, 0x0004,   // BSR.W +4  -> 0x1006
                0x4E71,           // NOP (skipped)
                0x4E75});         // RTS
        (void)m.cpu.step();
        check(m.cpu.registers().pc == 0x00001006, "BSR.W branches to the right target");
        const auto sp = m.cpu.registers().a[7];
        check(sp == 0x00008000 - 4, "BSR pushes a longword return address");
        (void)m.cpu.step();
        check(m.cpu.registers().pc == 0x00001004, "RTS returns to the instruction after BSR");
        check(m.cpu.registers().a[7] == 0x00008000, "RTS pops the stack");
    }

    {   // Conditional branch honours Z.
        Machine m;
        m.boot({0x7000,           // MOVEQ #0,D0   -> sets Z
                0x6702});         // BEQ.S +2      -> 0x1006
        (void)m.cpu.step();
        (void)m.cpu.step();
        check(m.cpu.registers().pc == 0x00001006, "BEQ taken when Z is set");
    }

    {   // TST and CLR set flags the documented way.
        Machine m;
        m.boot({0x4A80,    // TST.L D0
                0x4280});  // CLR.L D0
        m.cpu.mutable_registers().d[0] = 0x80000000;
        (void)m.cpu.step();
        check((m.cpu.registers().sr & M68kCpu::kStatusNegative) != 0, "TST.L sets N for a negative value");
        check((m.cpu.registers().sr & M68kCpu::kStatusZero) == 0, "TST.L clears Z for a non-zero value");
        (void)m.cpu.step();
        check(m.cpu.registers().d[0] == 0, "CLR.L zeroes the register");
        check((m.cpu.registers().sr & M68kCpu::kStatusZero) != 0, "CLR.L sets Z");
    }

    {   // Supervisor/user stack pointers are genuinely separate.
        Machine m;
        m.boot({0x4E71});
        m.cpu.mutable_registers().a[7] = 0x00008000;
        // Drop to user mode by clearing S through RTE's path is heavier than
        // needed here; exercise the swap through an exception instead.
        const auto ssp_before = m.cpu.registers().a[7];
        // LINE F IS NOT AN UNKNOWN OPCODE. The whole $Fxxx row is unassigned
        // on purpose and the hardware gives it vector 11, so software can
        // extend the instruction set or catch a wild jump. This check used to
        // expect "unimplemented" here, which was GenX reporting its own gap
        // rather than the processor's behaviour; the exception is the right
        // answer and the test moved to it.
        m.poke32(11 * 4, 0x00005000);  // the line-F vector
        m.poke16(0x1000, 0xF000);
        m.cpu.mutable_registers().pc = 0x1000;
        auto r = m.cpu.step();
        check(!r.unimplemented, "a line-F opcode is a defined trap, not an unknown instruction");
        check(m.cpu.registers().pc == 0x00005000, "and it is taken through vector 11");
        // Six bytes of frame: the return address and the old status register.
        check(m.cpu.registers().a[7] == ssp_before - 6, "the exception frame is on the stack");
    }

    {   // And a pattern in no row at all still reports itself rather than
        // being guessed at - which is what the check above used to be for.
        Machine m;
        m.boot({0x4E71});
        m.poke16(0x1000, 0x4AFC);  // the one genuinely illegal encoding
        m.cpu.mutable_registers().pc = 0x1000;
        const auto r = m.cpu.step();
        check(r.unimplemented || m.cpu.registers().pc != 0x1002,
              "an illegal encoding is trapped or reported, never executed as data");
    }

    // --- instructions the SingleStepTests suite does not cover -------------
    // The JSON oracle has no MOVEM, LINK, PEA, SWAP, EXT, NEG, TAS, TRAP, CHK,
    // MOVE-to-SR, ADDX, BCD or MOVEP cases, so everything below is the only
    // check those have. They are also the instructions a real ROM leans on
    // hardest - MOVEM in particular is in every subroutine prologue.

    {   // MOVEM round trip through the stack.
        Machine m;
        m.boot({0x48E7, 0xC000,   // movem.l d0-d1, -(a7)
                0x4CDF, 0x000C}); // movem.l (a7)+, d2-d3
        m.cpu.mutable_registers().d[0] = 0xAABBCCDD;
        m.cpu.mutable_registers().d[1] = 0x11223344;
        const auto sp_before = m.cpu.registers().a[7];
        (void)m.cpu.step();
        check(m.cpu.registers().a[7] == sp_before - 8, "MOVEM.L to -(A7) moves the stack by eight");
        (void)m.cpu.step();
        check(m.cpu.registers().d[2] == 0xAABBCCDD, "MOVEM.L restores D0's value into D2");
        check(m.cpu.registers().d[3] == 0x11223344, "MOVEM.L restores D1's value into D3");
        check(m.cpu.registers().a[7] == sp_before, "MOVEM.L from (A7)+ returns the stack");
    }

    {   // LINK and UNLK build and tear down a frame.
        Machine m;
        m.boot({0x4E56, 0xFFF8,   // link a6, #-8
                0x4E5E});          // unlk a6
        m.cpu.mutable_registers().a[6] = 0x12345678;
        const auto sp_before = m.cpu.registers().a[7];
        (void)m.cpu.step();
        check(m.cpu.registers().a[6] == sp_before - 4, "LINK points A6 at the saved frame");
        check(m.cpu.registers().a[7] == sp_before - 12, "LINK allocates the local space");
        (void)m.cpu.step();
        check(m.cpu.registers().a[6] == 0x12345678, "UNLK restores the old A6");
        check(m.cpu.registers().a[7] == sp_before, "UNLK restores the stack");
    }

    {   // PEA, SWAP and the two EXT widths.
        Machine m;
        m.boot({0x4878, 0x1234,   // pea ($1234).w
                0x4840,           // swap d0
                0x4881,           // ext.w d1
                0x48C1});         // ext.l d1
        m.cpu.mutable_registers().d[0] = 0x11112222;
        m.cpu.mutable_registers().d[1] = 0x000000F0;  // -16 as a byte
        const auto sp_before = m.cpu.registers().a[7];
        (void)m.cpu.step();
        check(m.cpu.registers().a[7] == sp_before - 4, "PEA pushes four bytes");
        check(m.peek16(m.cpu.registers().a[7] + 2) == 0x1234, "PEA pushes the address itself");
        (void)m.cpu.step();
        check(m.cpu.registers().d[0] == 0x22221111, "SWAP exchanges the halves of D0");
        (void)m.cpu.step();
        check(m.cpu.registers().d[1] == 0x0000FFF0, "EXT.W sign extends byte to word");
        (void)m.cpu.step();
        check(m.cpu.registers().d[1] == 0xFFFFFFF0, "EXT.L sign extends word to long");
    }

    {   // NEG, NOT and NEGX's clear-only Z.
        Machine m;
        m.boot({0x4480,   // neg.l d0
                0x4680,   // not.l d0
                0x4080}); // negx.l d0
        m.cpu.mutable_registers().d[0] = 5;
        (void)m.cpu.step();
        check(m.cpu.registers().d[0] == 0xFFFFFFFB, "NEG.L negates");
        check((m.cpu.registers().sr & M68kCpu::kStatusCarry) != 0, "NEG.L sets C for a non-zero operand");
        (void)m.cpu.step();
        check(m.cpu.registers().d[0] == 0x00000004, "NOT.L complements");
        m.cpu.mutable_registers().d[0] = 0;
        m.cpu.mutable_registers().sr |= M68kCpu::kStatusZero;
        m.cpu.mutable_registers().sr &= static_cast<std::uint16_t>(~M68kCpu::kStatusExtend);
        (void)m.cpu.step();
        check((m.cpu.registers().sr & M68kCpu::kStatusZero) != 0,
              "NEGX.L leaves Z set when the result is zero");
    }

    {   // TAS reads, flags, then sets the top bit.
        Machine m;
        m.boot({0x4AD0});  // tas (a0)
        m.cpu.mutable_registers().a[0] = 0x2000;
        m.memory[0x2000] = 0x01;
        (void)m.cpu.step();
        check(m.memory[0x2000] == 0x81, "TAS sets bit 7 of the operand");
        check((m.cpu.registers().sr & M68kCpu::kStatusZero) == 0, "TAS flags the value it read, not the value it wrote");
    }

    {   // TRAP takes its vector; ILLEGAL takes vector four.
        Machine m;
        m.boot({0x4E40});  // trap #0
        m.poke32(32 * 4, 0x00003000);
        (void)m.cpu.step();
        check(m.cpu.registers().pc == 0x3000, "TRAP #0 vectors through 32");

        Machine n;
        n.boot({0x4AFC});  // illegal
        n.poke32(4 * 4, 0x00004000);
        (void)n.cpu.step();
        check(n.cpu.registers().pc == 0x4000, "ILLEGAL vectors through 4");
    }

    {   // CHK passes in range and traps out of range.
        Machine m;
        m.boot({0x4181, 0x4181});  // chk d1, d0  (twice)
        m.poke32(6 * 4, 0x00003500);
        m.cpu.mutable_registers().d[0] = 5;
        m.cpu.mutable_registers().d[1] = 10;
        (void)m.cpu.step();
        check(m.cpu.registers().pc == 0x1002, "CHK falls through when the value is in range");
        m.cpu.mutable_registers().d[0] = 20;
        (void)m.cpu.step();
        check(m.cpu.registers().pc == 0x3500, "CHK traps when the value exceeds the bound");
    }

    {   // MOVE to and from SR, and MOVE to CCR.
        Machine m;
        m.boot({0x40C0,   // move sr, d0
                0x46C0,   // move d0, sr
                0x44C1}); // move d1, ccr
        (void)m.cpu.step();
        check((m.cpu.registers().d[0] & 0x2000) != 0, "MOVE SR,Dn reports supervisor mode");
        m.cpu.mutable_registers().d[0] = 0x2700;
        (void)m.cpu.step();
        check(m.cpu.registers().sr == 0x2700, "MOVE Dn,SR loads the whole status register");
        m.cpu.mutable_registers().d[1] = 0x00FF;
        (void)m.cpu.step();
        check((m.cpu.registers().sr & 0xFF00) == 0x2700, "MOVE Dn,CCR leaves the system half alone");
        check((m.cpu.registers().sr & 0x001F) == 0x001F, "MOVE Dn,CCR loads the condition codes");
    }

    {   // ADDX and SUBX carry X in and only ever clear Z.
        Machine m;
        m.boot({0xD181,   // addx.l d1, d0
                0x9181});  // subx.l d1, d0
        m.cpu.mutable_registers().d[0] = 1;
        m.cpu.mutable_registers().d[1] = 1;
        m.cpu.mutable_registers().sr |= M68kCpu::kStatusExtend;
        m.cpu.mutable_registers().sr |= M68kCpu::kStatusZero;
        (void)m.cpu.step();
        check(m.cpu.registers().d[0] == 3, "ADDX.L adds the extend bit as well");
        check((m.cpu.registers().sr & M68kCpu::kStatusZero) == 0, "ADDX.L clears Z on a non-zero result");
        m.cpu.mutable_registers().sr &= static_cast<std::uint16_t>(~M68kCpu::kStatusExtend);
        (void)m.cpu.step();
        check(m.cpu.registers().d[0] == 2, "SUBX.L subtracts without the extend bit");
    }

    {   // BCD arithmetic works in decimal, not binary.
        Machine m;
        m.boot({0xC101,   // abcd d1, d0
                0x8101});  // sbcd d1, d0
        m.cpu.mutable_registers().d[0] = 0x19;
        m.cpu.mutable_registers().d[1] = 0x01;
        m.cpu.mutable_registers().sr &= static_cast<std::uint16_t>(~M68kCpu::kStatusExtend);
        (void)m.cpu.step();
        check((m.cpu.registers().d[0] & 0xFF) == 0x20, "ABCD carries the low digit at nine, not fifteen");
        m.cpu.mutable_registers().sr &= static_cast<std::uint16_t>(~M68kCpu::kStatusExtend);
        (void)m.cpu.step();
        check((m.cpu.registers().d[0] & 0xFF) == 0x19, "SBCD borrows in decimal");
    }

    {   // MOVEP writes every other byte.
        Machine m;
        m.boot({0x0188, 0x0000});  // movep.w d0, (0, a0)
        m.cpu.mutable_registers().a[0] = 0x2000;
        m.cpu.mutable_registers().d[0] = 0x0000ABCD;
        (void)m.cpu.step();
        check(m.memory[0x2000] == 0xAB, "MOVEP.W puts the high byte at the base address");
        check(m.memory[0x2002] == 0xCD, "MOVEP.W skips a byte between halves");
        check(m.memory[0x2001] == 0x00, "MOVEP.W leaves the interleaved byte untouched");
    }

    {   // STOP halts until an interrupt arrives.
        Machine m;
        m.boot({0x4E72, 0x2000});  // stop #$2000
        (void)m.cpu.step();
        check(m.cpu.registers().sr == 0x2000, "STOP loads the status register it was given");
        const auto pc_after = m.cpu.registers().pc;
        (void)m.cpu.step();
        check(m.cpu.registers().pc == pc_after, "a stopped CPU does not advance");
        m.cpu.set_interrupt_level(4);
        (void)m.cpu.step();
        check(m.cpu.registers().pc != pc_after, "an interrupt wakes a stopped CPU");
    }

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
