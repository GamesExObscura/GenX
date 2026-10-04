// The SH-2 core, instruction by instruction.
//
// Written alongside the core rather than after it, because an interpreter
// that is 95% right boots nothing and gives no clue which 5% is wrong. Each
// test assembles a couple of opcodes into a small memory, runs them, and
// checks the registers - so a failure names the instruction.
//
// The delay slot tests are the ones that matter most. Every other mistake
// shows up as a wrong number; a delay slot mistake shows up as a game that
// runs off into nothing, days later.

#include "mercury/core/sh2_cpu.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void check_eq(std::uint32_t got, std::uint32_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s: got %08X want %08X\n", what.c_str(), got, want);
    }
}

// A little machine: 64KB of memory, big-endian, with the SH-2 on it.
struct Harness {
    std::vector<std::uint8_t> memory;
    mercury::core::Sh2Cpu cpu;

    Harness() : memory(0x10000, 0) {
        mercury::core::Sh2BusCallbacks bus;
        bus.read8 = [this](std::uint32_t a) { return memory[a & 0xFFFF]; };
        bus.read16 = [this](std::uint32_t a) {
            return static_cast<std::uint16_t>((memory[a & 0xFFFF] << 8) |
                                              memory[(a + 1) & 0xFFFF]);
        };
        bus.read32 = [this](std::uint32_t a) {
            return (static_cast<std::uint32_t>(memory[a & 0xFFFF]) << 24) |
                   (static_cast<std::uint32_t>(memory[(a + 1) & 0xFFFF]) << 16) |
                   (static_cast<std::uint32_t>(memory[(a + 2) & 0xFFFF]) << 8) |
                   static_cast<std::uint32_t>(memory[(a + 3) & 0xFFFF]);
        };
        bus.write8 = [this](std::uint32_t a, std::uint8_t v) { memory[a & 0xFFFF] = v; };
        bus.write16 = [this](std::uint32_t a, std::uint16_t v) {
            memory[a & 0xFFFF] = static_cast<std::uint8_t>(v >> 8);
            memory[(a + 1) & 0xFFFF] = static_cast<std::uint8_t>(v);
        };
        bus.write32 = [this](std::uint32_t a, std::uint32_t v) {
            memory[a & 0xFFFF] = static_cast<std::uint8_t>(v >> 24);
            memory[(a + 1) & 0xFFFF] = static_cast<std::uint8_t>(v >> 16);
            memory[(a + 2) & 0xFFFF] = static_cast<std::uint8_t>(v >> 8);
            memory[(a + 3) & 0xFFFF] = static_cast<std::uint8_t>(v);
        };
        cpu.attach(bus);
    }

    void poke16(std::uint32_t address, std::uint16_t value) {
        memory[address & 0xFFFF] = static_cast<std::uint8_t>(value >> 8);
        memory[(address + 1) & 0xFFFF] = static_cast<std::uint8_t>(value);
    }
    void poke32(std::uint32_t address, std::uint32_t value) {
        memory[address & 0xFFFF] = static_cast<std::uint8_t>(value >> 24);
        memory[(address + 1) & 0xFFFF] = static_cast<std::uint8_t>(value >> 16);
        memory[(address + 2) & 0xFFFF] = static_cast<std::uint8_t>(value >> 8);
        memory[(address + 3) & 0xFFFF] = static_cast<std::uint8_t>(value);
    }
    [[nodiscard]] std::uint32_t peek32(std::uint32_t address) const {
        return (static_cast<std::uint32_t>(memory[address & 0xFFFF]) << 24) |
               (static_cast<std::uint32_t>(memory[(address + 1) & 0xFFFF]) << 16) |
               (static_cast<std::uint32_t>(memory[(address + 2) & 0xFFFF]) << 8) |
               static_cast<std::uint32_t>(memory[(address + 3) & 0xFFFF]);
    }

    // Load a run of opcodes at 0x1000 and start there.
    void program(std::initializer_list<std::uint16_t> opcodes) {
        std::uint32_t at = 0x1000;
        for (const auto op : opcodes) {
            poke16(at, op);
            at += 2;
        }
        cpu.set_pc(0x1000);
    }
    mercury::core::Sh2StepResult run(int steps) {
        mercury::core::Sh2StepResult last;
        for (int i = 0; i < steps; ++i) {
            last = cpu.step();
            if (last.unimplemented) {
                std::printf("  FAIL  unimplemented opcode %04X at %08X\n", last.opcode,
                            last.pc_before);
                ++g_failures;
                break;
            }
        }
        return last;
    }
};

void test_reset_reads_the_vector_table() {
    std::printf("reset takes PC and SP from the vector table\n");
    Harness h;
    h.poke32(0x0000, 0x00002000);
    h.poke32(0x0004, 0x0000FF00);
    h.cpu.reset();
    check_eq(h.cpu.registers().pc, 0x00002000, "PC from vector 0");
    check_eq(h.cpu.registers().r[15], 0x0000FF00, "SP from vector 1");
}

void test_immediate_and_moves() {
    std::printf("MOV immediate, register to register, and sign extension\n");
    Harness h;
    // MOV #0x42,R1 ; MOV #-1,R2 ; MOV R1,R3
    h.program({0xE142, 0xE2FF, 0x6313});
    h.run(3);
    check_eq(h.cpu.registers().r[1], 0x00000042, "MOV #imm,Rn");
    check_eq(h.cpu.registers().r[2], 0xFFFFFFFF, "MOV #imm sign extends");
    check_eq(h.cpu.registers().r[3], 0x00000042, "MOV Rm,Rn");
}

void test_add_and_carry() {
    std::printf("ADD, ADDC and the T flag\n");
    Harness h;
    // MOV #1,R1 ; ADD #-2,R1  -> 0xFFFFFFFF
    h.program({0xE101, 0x71FE});
    h.run(2);
    check_eq(h.cpu.registers().r[1], 0xFFFFFFFF, "ADD #imm is signed");

    Harness c;
    // R1 = 0xFFFFFFFF, R2 = 1, ADDC should carry
    c.program({0xE1FF, 0xE201, 0x312E});
    c.cpu.registers().r[1] = 0xFFFFFFFF;
    c.cpu.registers().r[2] = 0x00000001;
    c.cpu.set_pc(0x1004);
    c.run(1);
    check_eq(c.cpu.registers().r[1], 0x00000000, "ADDC wraps");
    check(c.cpu.t_flag(), "ADDC sets T on carry out");
}

void test_compares() {
    std::printf("the CMP family writes T\n");
    Harness h;
    h.cpu.registers().r[1] = 5;
    h.cpu.registers().r[2] = 5;
    h.program({0x3120});  // CMP/EQ R2,R1
    h.run(1);
    check(h.cpu.t_flag(), "CMP/EQ equal sets T");

    Harness g;
    g.cpu.registers().r[1] = 0xFFFFFFFF;  // -1
    g.cpu.registers().r[2] = 0x00000001;
    g.program({0x3127});  // CMP/GT R2,R1 - signed, -1 > 1 is false
    g.run(1);
    check(!g.cpu.t_flag(), "CMP/GT is signed");

    Harness u;
    u.cpu.registers().r[1] = 0xFFFFFFFF;
    u.cpu.registers().r[2] = 0x00000001;
    u.program({0x3126});  // CMP/HI R2,R1 - unsigned, 0xFFFFFFFF > 1 is true
    u.run(1);
    check(u.cpu.t_flag(), "CMP/HI is unsigned");
}

void test_memory_access_sizes() {
    std::printf("byte, word and longword access, with sign extension\n");
    Harness h;
    h.memory[0x2000] = 0x80;
    h.cpu.registers().r[1] = 0x2000;
    h.program({0x6210});  // MOV.B @R1,R2
    h.run(1);
    check_eq(h.cpu.registers().r[2], 0xFFFFFF80, "MOV.B sign extends");

    Harness w;
    w.poke16(0x2000, 0x8001);
    w.cpu.registers().r[1] = 0x2000;
    w.program({0x6211});  // MOV.W @R1,R2
    w.run(1);
    check_eq(w.cpu.registers().r[2], 0xFFFF8001, "MOV.W sign extends");

    Harness l;
    l.poke32(0x2000, 0x12345678);
    l.cpu.registers().r[1] = 0x2000;
    l.program({0x6212});  // MOV.L @R1,R2
    l.run(1);
    check_eq(l.cpu.registers().r[2], 0x12345678, "MOV.L reads big-endian");
}

void test_post_increment_and_pre_decrement() {
    std::printf("@Rm+ and @-Rn adjust by the access size\n");
    Harness h;
    h.poke32(0x2000, 0xAABBCCDD);
    h.cpu.registers().r[1] = 0x2000;
    h.program({0x6216});  // MOV.L @R1+,R2
    h.run(1);
    check_eq(h.cpu.registers().r[2], 0xAABBCCDD, "MOV.L @Rm+ reads");
    check_eq(h.cpu.registers().r[1], 0x2004, "MOV.L @Rm+ advances by 4");

    Harness p;
    p.cpu.registers().r[1] = 0x2004;
    p.cpu.registers().r[2] = 0x11223344;
    p.program({0x2126});  // MOV.L R2,@-R1
    p.run(1);
    check_eq(p.cpu.registers().r[1], 0x2000, "MOV.L @-Rn pre-decrements");
    check_eq(p.peek32(0x2000), 0x11223344, "MOV.L @-Rn stores");
}

void test_pc_relative_loads() {
    std::printf("PC-relative loads reach past the delay slot\n");
    Harness h;
    // MOV.L @(1,PC),R1 at 0x1000 reads from (0x1000+4 & ~3) + 4 = 0x1008
    h.program({0xD101, 0x0009});
    h.poke32(0x1008, 0xCAFEBABE);
    h.run(1);
    check_eq(h.cpu.registers().r[1], 0xCAFEBABE, "MOV.L @(disp,PC) address");
}

void test_branch_delay_slot_runs() {
    std::printf("THE DELAY SLOT: the instruction after a branch runs first\n");
    Harness h;
    // BRA +2 (to 0x1008) ; MOV #1,R1 (delay slot, MUST run) ;
    // MOV #2,R2 (skipped) ; MOV #3,R3 (landed on)
    h.program({0xA001, 0xE101, 0xE202, 0xE303});
    h.run(3);
    check_eq(h.cpu.registers().r[1], 1, "the delay slot instruction ran");
    check_eq(h.cpu.registers().r[2], 0, "the instruction after the delay slot was skipped");
    check_eq(h.cpu.registers().r[3], 3, "the branch landed in the right place");
}

void test_bsr_and_rts() {
    std::printf("BSR stores the return address past the delay slot, RTS uses it\n");
    Harness h;
    // A branch displacement counts from PC+4, not from the branch, so BSR with
    // disp 2 lands at 0x1000 + 4 + 4 = 0x1008. Getting this wrong in the test
    // rather than in the core was the first thing this suite caught.
    //
    // 1000 BSR +2   -> 0x1008
    // 1002 NOP          (delay slot)
    // 1004 MOV #5,R5    (returned to)
    // 1006 NOP
    // 1008 RTS
    // 100A MOV #7,R7    (delay slot of RTS, runs)
    h.program({0xB002, 0x0009, 0xE505, 0x0009, 0x000B, 0xE707});
    h.run(2);
    check_eq(h.cpu.registers().pr, 0x1004, "PR points past the delay slot");
    h.run(2);
    check_eq(h.cpu.registers().r[7], 7, "RTS ran its delay slot");
    check_eq(h.cpu.registers().pc, 0x1004, "RTS returned to PR");
}

void test_conditional_branches() {
    std::printf("BT and BF, with and without delay slots\n");
    Harness h;
    // SETT ; BT +0 (to 0x1002 + 4 = 0x1006) ; MOV #1,R1 (skipped) ; MOV #2,R2
    h.program({0x0018, 0x8900, 0xE101, 0xE202});
    h.run(3);
    check_eq(h.cpu.registers().r[1], 0, "BT taken skips the next instruction");
    check_eq(h.cpu.registers().r[2], 2, "BT landed correctly");

    Harness s;
    // SETT ; BT/S +1 ; MOV #1,R1 (delay slot, RUNS) ; MOV #2,R2 (skipped) ; MOV #3,R3
    s.program({0x0018, 0x8D01, 0xE101, 0xE202, 0xE303});
    s.run(4);
    check_eq(s.cpu.registers().r[1], 1, "BT/S runs its delay slot");
    check_eq(s.cpu.registers().r[2], 0, "BT/S skipped the right instruction");
    check_eq(s.cpu.registers().r[3], 3, "BT/S landed correctly");
}

void test_shifts_and_rotates() {
    std::printf("shifts and rotates, and what they do to T\n");
    Harness h;
    h.cpu.registers().r[1] = 0x80000000;
    h.program({0x4100, 0x4101});  // SHLL R1 ; SHLR R1
    h.run(1);
    check_eq(h.cpu.registers().r[1], 0x00000000, "SHLL shifts out the top");
    check(h.cpu.t_flag(), "SHLL puts the old top bit in T");

    Harness a;
    a.cpu.registers().r[1] = 0x80000000;
    a.program({0x4121});  // SHAR R1
    a.run(1);
    check_eq(a.cpu.registers().r[1], 0xC0000000, "SHAR keeps the sign");

    Harness c;
    c.cpu.registers().r[1] = 0x00000001;
    c.cpu.set_t_flag(true);
    c.program({0x4124});  // ROTCL R1
    c.run(1);
    check_eq(c.cpu.registers().r[1], 0x00000003, "ROTCL brings T in at the bottom");
    check(!c.cpu.t_flag(), "ROTCL puts the old top bit in T");

    Harness m;
    m.cpu.registers().r[1] = 0x00000001;
    m.program({0x4108, 0x4118, 0x4128});  // SHLL2, SHLL8, SHLL16
    m.run(3);
    check_eq(m.cpu.registers().r[1], 0x04000000, "SHLL2/8/16 compose");
}

void test_logic_and_bit_ops() {
    std::printf("AND, OR, XOR, NOT, TST\n");
    Harness h;
    h.cpu.registers().r[1] = 0xF0F0F0F0;
    h.cpu.registers().r[2] = 0x0FF00FF0;
    h.program({0x2129, 0x612A});  // AND R2,R1 ; NOT R2,R1  (second overwrites)
    h.run(1);
    check_eq(h.cpu.registers().r[1], 0x00F000F0, "AND");

    Harness t;
    t.cpu.registers().r[1] = 0x0000FF00;
    t.cpu.registers().r[2] = 0x000000FF;
    t.program({0x2128});  // TST R2,R1
    t.run(1);
    check(t.cpu.t_flag(), "TST sets T when there are no common bits");
}

void test_extend_and_swap() {
    std::printf("EXTS, EXTU, SWAP, XTRCT\n");
    Harness h;
    h.cpu.registers().r[2] = 0x000000FF;
    h.program({0x612E});  // EXTS.B R2,R1
    h.run(1);
    check_eq(h.cpu.registers().r[1], 0xFFFFFFFF, "EXTS.B");

    Harness u;
    u.cpu.registers().r[2] = 0xFFFFFFFF;
    u.program({0x612C});  // EXTU.B R2,R1
    u.run(1);
    check_eq(u.cpu.registers().r[1], 0x000000FF, "EXTU.B");

    Harness s;
    s.cpu.registers().r[2] = 0x12345678;
    s.program({0x6129});  // SWAP.W R2,R1
    s.run(1);
    check_eq(s.cpu.registers().r[1], 0x56781234, "SWAP.W");

    Harness b;
    b.cpu.registers().r[2] = 0x12345678;
    b.program({0x6128});  // SWAP.B R2,R1
    b.run(1);
    check_eq(b.cpu.registers().r[1], 0x12347856, "SWAP.B swaps only the low two");

    Harness x;
    x.cpu.registers().r[1] = 0x12345678;
    x.cpu.registers().r[2] = 0x9ABCDEF0;
    x.program({0x212D});  // XTRCT R2,R1
    x.run(1);
    check_eq(x.cpu.registers().r[1], 0xDEF01234, "XTRCT takes the middle");
}

void test_multiplies() {
    std::printf("the multiply family and MAC\n");
    Harness h;
    h.cpu.registers().r[1] = 0x00010000;
    h.cpu.registers().r[2] = 0x00000010;
    h.program({0x0127});  // MUL.L R2,R1
    h.run(1);
    check_eq(h.cpu.registers().macl, 0x00100000, "MUL.L low 32 bits");

    Harness d;
    d.cpu.registers().r[1] = 0xFFFFFFFF;  // -1
    d.cpu.registers().r[2] = 0x00000002;
    d.program({0x312D});  // DMULS.L R2,R1
    d.run(1);
    check_eq(d.cpu.registers().mach, 0xFFFFFFFF, "DMULS.L high");
    check_eq(d.cpu.registers().macl, 0xFFFFFFFE, "DMULS.L low");

    Harness u;
    u.cpu.registers().r[1] = 0xFFFFFFFF;
    u.cpu.registers().r[2] = 0x00000002;
    u.program({0x3125});  // DMULU.L R2,R1
    u.run(1);
    check_eq(u.cpu.registers().mach, 0x00000001, "DMULU.L high is unsigned");
    check_eq(u.cpu.registers().macl, 0xFFFFFFFE, "DMULU.L low");
}

void test_dt_loop() {
    std::printf("DT, the loop instruction\n");
    Harness h;
    h.cpu.registers().r[1] = 2;
    h.program({0x4110, 0x4110});  // DT R1 ; DT R1
    h.run(1);
    check_eq(h.cpu.registers().r[1], 1, "DT decrements");
    check(!h.cpu.t_flag(), "DT clears T while non-zero");
    h.run(1);
    check(h.cpu.t_flag(), "DT sets T at zero");
}

void test_jsr_and_jmp() {
    std::printf("JSR and JMP with their delay slots\n");
    Harness h;
    h.cpu.registers().r[1] = 0x1100;
    h.poke16(0x1100, 0xE909);  // MOV #9,R9 at the target
    // JSR @R1 ; MOV #4,R4 (delay slot)
    h.program({0x410B, 0xE404});
    h.run(2);
    check_eq(h.cpu.registers().r[4], 4, "JSR ran its delay slot");
    check_eq(h.cpu.registers().pc, 0x1100, "JSR jumped");
    check_eq(h.cpu.registers().pr, 0x1004, "JSR stored the return address");
}

void test_gbr_addressing() {
    std::printf("the GBR-relative modes\n");
    Harness h;
    h.cpu.registers().gbr = 0x3000;
    h.cpu.registers().r[0] = 0x11223344;
    h.program({0xC202});  // MOV.L R0,@(2,GBR) -> 0x3008
    h.run(1);
    check_eq(h.peek32(0x3008), 0x11223344, "MOV.L R0,@(disp,GBR)");

    Harness r;
    r.cpu.registers().gbr = 0x3000;
    r.poke32(0x3008, 0x55667788);
    r.program({0xC602});  // MOV.L @(2,GBR),R0
    r.run(1);
    check_eq(r.cpu.registers().r[0], 0x55667788, "MOV.L @(disp,GBR),R0");
}

void test_tas_is_atomic_read_modify_write() {
    std::printf("TAS.B sets the top bit and reports whether it was zero\n");
    Harness h;
    h.memory[0x2000] = 0x00;
    h.cpu.registers().r[1] = 0x2000;
    h.program({0x411B});  // TAS.B @R1
    h.run(1);
    check(h.cpu.t_flag(), "TAS.B sets T when the byte was zero");
    check_eq(h.memory[0x2000], 0x80, "TAS.B sets bit 7");
}

void test_interrupt_uses_the_vector_table() {
    std::printf("an interrupt stacks SR and PC and vectors through VBR\n");
    Harness h;
    h.cpu.registers().r[15] = 0x4000;
    h.cpu.registers().vbr = 0x0100;
    h.cpu.registers().sr = 0x00000000;  // mask 0, so level 1 is taken
    h.poke32(0x0100 + 0x40 * 4, 0x00005000);
    h.program({0x0009});  // NOP, which should not run
    h.cpu.request_interrupt(1, 0x40);
    h.run(1);
    check_eq(h.cpu.registers().pc, 0x00005000, "vectored through VBR");
    check_eq(h.cpu.registers().r[15], 0x3FF8, "two longwords pushed");
    check_eq(h.peek32(0x3FF8), 0x1000, "the interrupted PC was stacked");
    check_eq((h.cpu.registers().sr >> 4) & 0x0F, 1, "the mask was raised to the level");
}

void test_interrupt_waits_for_the_delay_slot() {
    std::printf("an interrupt never lands between a branch and its delay slot\n");
    Harness h;
    h.cpu.registers().r[15] = 0x4000;
    h.cpu.registers().vbr = 0x0100;
    h.cpu.registers().sr = 0x00000000;
    h.poke32(0x0100 + 0x40 * 4, 0x00005000);
    // BRA +2 ; MOV #1,R1 (delay slot)
    h.program({0xA001, 0xE101});
    h.run(1);                       // the branch
    h.cpu.request_interrupt(1, 0x40);
    h.cpu.step();                   // must be the delay slot, NOT the interrupt
    check_eq(h.cpu.registers().r[1], 1, "the delay slot ran before the interrupt");
}

}  // namespace

// Which of the 65,536 possible opcodes the decoder refuses.
//
// Not a pass/fail check: plenty of the encoding space really is undefined on
// an SH-2, and a game executing one of those has already gone wrong. This is
// a report, so that "the game hit an unimplemented opcode" can be told apart
// from "the game ran off into data" without guessing - the difference matters,
// because the first is a gap here and the second is a gap somewhere else.
void report_opcode_coverage() {
    std::printf("\nopcode coverage\n");
    std::array<int, 16> refused_by_family{};
    std::array<int, 16> total_by_family{};
    int refused = 0;
    for (std::uint32_t opcode = 0; opcode <= 0xFFFF; ++opcode) {
        Harness harness;
        // A register set that keeps addresses inside the harness memory, so a
        // load or store decodes and runs rather than faulting on the bus.
        for (int reg = 0; reg < 16; ++reg) {
            harness.cpu.registers().r[reg] = 0x2000 + static_cast<std::uint32_t>(reg) * 4;
        }
        harness.program({static_cast<std::uint16_t>(opcode)});
        const auto result = harness.cpu.step();
        const auto family = static_cast<std::size_t>(opcode >> 12);
        ++total_by_family[family];
        if (result.unimplemented) {
            ++refused_by_family[family];
            ++refused;
        }
    }
    for (std::size_t family = 0; family < refused_by_family.size(); ++family) {
        if (refused_by_family[family] != 0) {
            std::printf("  family %zX: %d of %d refused\n", family, refused_by_family[family],
                        total_by_family[family]);
        }
    }
    std::printf("  %d of 65536 opcodes refused\n", refused);
}

void test_pc_relative_loads_in_a_delay_slot_count_from_the_target() {
    std::printf("PC-relative loads in a delay slot count from the branch target\n");
    // BRA to 0x1010, and in its delay slot a load that counts from PC. Counted
    // from the slot the address would be 0x1008; counted from the target, as
    // the hardware does, it is 0x1014. A decoy sits at the wrong one.
    Harness w;
    w.program({0xA006, 0x9101});  // BRA +6 ; MOV.W @(2,PC),R1
    w.poke16(0x1008, 0x1111);
    w.poke16(0x1014, 0x2222);
    w.run(2);
    check_eq(w.cpu.registers().r[1], 0x00002222, "MOV.W @(disp,PC) in a slot reads past the target");
    check_eq(w.cpu.registers().pc, 0x00001010, "and the branch is still taken");

    Harness l;
    l.program({0xA006, 0xD201});  // BRA +6 ; MOV.L @(4,PC),R2
    l.poke32(0x1008, 0x11111111);
    l.poke32(0x1014, 0xCAFEF00D);
    l.run(2);
    check_eq(l.cpu.registers().r[2], 0xCAFEF00D, "MOV.L @(disp,PC) in a slot reads past the target");

    Harness a;
    a.program({0xA006, 0xC701});  // BRA +6 ; MOVA @(4,PC),R0
    a.run(2);
    check_eq(a.cpu.registers().r[0], 0x00001014, "MOVA in a slot is an address past the target");
}

// A REAL DIVISION, not a check of one step's flags. DIV1's Q/M/T interplay is
// the easiest thing in the instruction set to get subtly wrong, and a wrong
// step shows up only as a wrong ANSWER thirty-two steps later - which is
// exactly how it hid here: half the steps inverted Q, every quotient came out
// as alternating bits, and the tell-tale $5555/$AAAA only appeared deep inside
// a game. So divide actual numbers and check the quotient and remainder.
void divide_unsigned(Harness& h, std::uint32_t dividend, std::uint32_t divisor) {
    // The Hitachi manual's unsigned 64/32 sequence, with the dividend's high
    // half zero: R1:R2 over R0, quotient in R2 and remainder in R1.
    h.cpu.registers().r[0] = divisor;
    h.cpu.registers().r[1] = 0;
    h.cpu.registers().r[2] = dividend;
    // Written straight into memory: the harness's program() takes a fixed
    // list, and this one is sixty-six instructions long.
    std::uint32_t at = 0x1000;
    h.poke16(at, 0x0019);  // DIV0U
    at += 2;
    for (int step = 0; step < 32; ++step) {
        h.poke16(at, 0x4224);  // ROTCL R2
        at += 2;
        h.poke16(at, 0x3104);  // DIV1 R0,R1
        at += 2;
    }
    h.poke16(at, 0x4224);  // ROTCL R2 - the last quotient bit
    h.cpu.set_pc(0x1000);
    h.run(66);
}

void test_div1_divides() {
    std::printf("DIV1 divides - the whole 32-step sequence, not one step\n");
    // Quotients only. The manual's sequence leaves the remainder one
    // subtraction past the end - software adds the divisor back afterwards if
    // it wants it - so checking R1 here would be testing the test.
    {
        Harness h;
        divide_unsigned(h, 1000, 5);
        check_eq(h.cpu.registers().r[2], 200, "1000 / 5 = 200");
    }
    {
        Harness h;
        divide_unsigned(h, 0x0000A000, 0x00000100);
        check_eq(h.cpu.registers().r[2], 0xA0, "0xA000 / 0x100 = 0xA0");
    }
    {
        Harness h;
        divide_unsigned(h, 1234567, 1000);
        check_eq(h.cpu.registers().r[2], 1234, "1234567 / 1000 = 1234");
    }
    {
        // A divisor with its top bit set: M is then set for the whole run,
        // which is the half of the Q logic that was inverted.
        Harness h;
        divide_unsigned(h, 0xFFFFFFFF, 0x80000001);
        check_eq(h.cpu.registers().r[2], 1, "0xFFFFFFFF / 0x80000001 = 1");
        check_eq(h.cpu.registers().r[1], 0x7FFFFFFE, "and the remainder");
    }
}

int main() {
    std::printf("SH-2 core tests\n===============\n\n");
    test_reset_reads_the_vector_table();
    test_immediate_and_moves();
    test_add_and_carry();
    test_compares();
    test_memory_access_sizes();
    test_post_increment_and_pre_decrement();
    test_pc_relative_loads();
    test_branch_delay_slot_runs();
    test_bsr_and_rts();
    test_conditional_branches();
    test_shifts_and_rotates();
    test_logic_and_bit_ops();
    test_extend_and_swap();
    test_multiplies();
    test_dt_loop();
    test_jsr_and_jmp();
    test_gbr_addressing();
    test_tas_is_atomic_read_modify_write();
    test_interrupt_uses_the_vector_table();
    test_interrupt_waits_for_the_delay_slot();
    test_pc_relative_loads_in_a_delay_slot_count_from_the_target();
    test_div1_divides();
    report_opcode_coverage();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
