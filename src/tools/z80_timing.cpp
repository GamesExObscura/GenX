// z80_timing: check the core's reported T-state counts against documented Z80
// timings, and measure interrupt acceptance latency.
//
// zexdoc proves the ALU and flags are right but says nothing about how long
// anything takes. A systematic cycle error is invisible to every test we have
// and yet decides whether a raster-heavy game keeps up with its own line
// interrupts - Rise of the Robots asks for 96 splits a frame and we service
// only ~55, which is a timing question, not a video one.
//
// Each case plants optional setup bytes (stepped and discarded) followed by the
// instruction under test, then compares that instruction's reported cycles with
// the reference value.

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "mercury/core/z80_cpu.hpp"

namespace {

struct Case {
    const char* name;
    std::vector<std::uint8_t> setup;  // stepped first, cycles ignored
    std::vector<std::uint8_t> code;   // the instruction under test
    std::uint32_t expected;
};

// Flag-setting helpers for the conditional forms.
// XOR A leaves Z=1, C=0. OR A after LD A,1 leaves Z=0. SCF sets C=1.
const std::vector<std::uint8_t> kZeroSet   = {0xAF};              // XOR A  -> Z=1
const std::vector<std::uint8_t> kZeroClear = {0x3E, 0x01, 0xB7};  // LD A,1 / OR A -> Z=0
const std::vector<std::uint8_t> kCarrySet  = {0x37};              // SCF -> C=1
const std::vector<std::uint8_t> kCarryClr  = {0x37, 0x3F};        // SCF / CCF -> C=0

std::vector<Case> build_cases() {
    std::vector<Case> c;
    const std::vector<std::uint8_t> none;

    auto add = [&c](const char* name,
                    std::vector<std::uint8_t> setup,
                    std::vector<std::uint8_t> code,
                    std::uint32_t expected) {
        c.push_back(Case{name, std::move(setup), std::move(code), expected});
    };

    // --- the instructions Rise of the Robots' line-interrupt handler uses ---
    add("PUSH AF",        none, {0xF5}, 11);
    add("POP AF",         none, {0xF1}, 10);
    add("PUSH HL",        none, {0xE5}, 11);
    add("POP HL",         none, {0xE1}, 10);
    add("IN A,(n)",       none, {0xDB, 0xBF}, 11);
    add("OUT (n),A",      none, {0xD3, 0xBF}, 11);
    add("RLA",            none, {0x17}, 4);
    add("LD HL,(nn)",     none, {0x2A, 0x00, 0x40}, 16);
    add("LD (nn),HL",     none, {0x22, 0x00, 0x40}, 16);
    add("LD A,(nn)",      none, {0x3A, 0x00, 0x40}, 13);
    add("LD (nn),A",      none, {0x32, 0x00, 0x40}, 13);
    add("INC HL",         none, {0x23}, 6);
    add("LD A,(HL)",      none, {0x7E}, 7);
    add("LD H,(HL)",      none, {0x66}, 7);
    add("LD L,A",         none, {0x6F}, 4);
    add("JP (HL)",        none, {0xE9}, 4);
    add("LD A,n",         none, {0x3E, 0x12}, 7);
    add("EI",             none, {0xFB}, 4);
    add("DI",             none, {0xF3}, 4);
    add("RET",            none, {0xC9}, 10);

    // --- 8-bit loads / ALU ---
    add("NOP",            none, {0x00}, 4);
    add("LD B,C",         none, {0x41}, 4);
    add("LD B,(HL)",      none, {0x46}, 7);
    add("LD (HL),B",      none, {0x70}, 7);
    add("LD (HL),n",      none, {0x36, 0x55}, 10);
    add("ADD A,B",        none, {0x80}, 4);
    add("ADD A,(HL)",     none, {0x86}, 7);
    add("ADD A,n",        none, {0xC6, 0x01}, 7);
    add("INC B",          none, {0x04}, 4);
    add("INC (HL)",       none, {0x34}, 11);
    add("DEC (HL)",       none, {0x35}, 11);
    add("CP n",           none, {0xFE, 0x01}, 7);
    add("DAA",            none, {0x27}, 4);
    add("CPL",            none, {0x2F}, 4);
    add("SCF",            none, {0x37}, 4);

    // --- 16-bit ---
    add("LD BC,nn",       none, {0x01, 0x34, 0x12}, 10);
    add("INC BC",         none, {0x03}, 6);
    add("DEC BC",         none, {0x0B}, 6);
    add("ADD HL,BC",      none, {0x09}, 11);
    add("LD SP,HL",       none, {0xF9}, 6);
    add("EX DE,HL",       none, {0xEB}, 4);
    add("EX (SP),HL",     none, {0xE3}, 19);
    add("EX AF,AF'",      none, {0x08}, 4);
    add("EXX",            none, {0xD9}, 4);

    // --- jumps, calls, returns: both directions ---
    add("JP nn",          none, {0xC3, 0x00, 0x02}, 10);
    add("JP NZ,nn taken", kZeroClear, {0xC2, 0x00, 0x02}, 10);
    add("JP NZ,nn not",   kZeroSet,   {0xC2, 0x00, 0x02}, 10);
    add("JR d",           none, {0x18, 0x00}, 12);
    add("JR NZ,d taken",  kZeroClear, {0x20, 0x00}, 12);
    add("JR NZ,d not",    kZeroSet,   {0x20, 0x00}, 7);
    add("JR Z,d taken",   kZeroSet,   {0x28, 0x00}, 12);
    add("JR Z,d not",     kZeroClear, {0x28, 0x00}, 7);
    add("JR C,d taken",   kCarrySet,  {0x38, 0x00}, 12);
    add("JR C,d not",     kCarryClr,  {0x38, 0x00}, 7);
    add("DJNZ taken",     {0x06, 0x05}, {0x10, 0x00}, 13);
    add("DJNZ not",       {0x06, 0x01}, {0x10, 0x00}, 8);
    add("CALL nn",        none, {0xCD, 0x00, 0x02}, 17);
    add("CALL NZ taken",  kZeroClear, {0xC4, 0x00, 0x02}, 17);
    add("CALL NZ not",    kZeroSet,   {0xC4, 0x00, 0x02}, 10);
    add("RET NZ taken",   kZeroClear, {0xC0}, 11);
    add("RET NZ not",     kZeroSet,   {0xC0}, 5);
    add("RST 00",         none, {0xC7}, 11);

    // --- CB space ---
    add("CB RLC B",       none, {0xCB, 0x00}, 8);
    add("CB RLC (HL)",    none, {0xCB, 0x06}, 15);
    add("CB BIT 0,B",     none, {0xCB, 0x40}, 8);
    add("CB BIT 0,(HL)",  none, {0xCB, 0x46}, 12);
    add("CB SET 0,(HL)",  none, {0xCB, 0xC6}, 15);
    add("CB RES 0,B",     none, {0xCB, 0x80}, 8);

    // --- ED space ---
    add("ED IN B,(C)",    none, {0xED, 0x40}, 12);
    add("ED OUT (C),B",   none, {0xED, 0x41}, 12);
    add("ED SBC HL,BC",   none, {0xED, 0x42}, 15);
    add("ED ADC HL,BC",   none, {0xED, 0x4A}, 15);
    add("ED LD (nn),BC",  none, {0xED, 0x43, 0x00, 0x40}, 20);
    add("ED LD BC,(nn)",  none, {0xED, 0x4B, 0x00, 0x40}, 20);
    add("ED NEG",         none, {0xED, 0x44}, 8);
    add("ED RETN",        none, {0xED, 0x45}, 14);
    add("ED RETI",        none, {0xED, 0x4D}, 14);
    add("ED IM 1",        none, {0xED, 0x56}, 8);
    add("ED LD I,A",      none, {0xED, 0x47}, 9);
    add("ED LD A,I",      none, {0xED, 0x57}, 9);
    add("ED RRD",         none, {0xED, 0x67}, 18);
    add("ED RLD",         none, {0xED, 0x6F}, 18);
    add("ED LDI",         none, {0xED, 0xA0}, 16);
    add("ED CPI",         none, {0xED, 0xA1}, 16);
    add("ED INI",         none, {0xED, 0xA2}, 16);
    add("ED OUTI",        none, {0xED, 0xA3}, 16);
    // LDIR with BC=1 finishes (16); with BC=2 it repeats (21).
    add("ED LDIR done",   {0x01, 0x01, 0x00}, {0xED, 0xB0}, 16);
    add("ED LDIR repeat", {0x01, 0x02, 0x00}, {0xED, 0xB0}, 21);
    add("ED CPIR repeat", {0x01, 0x02, 0x00, 0x3E, 0xFF}, {0xED, 0xB1}, 21);

    // --- DD/FD space ---
    add("DD ADD IX,BC",   none, {0xDD, 0x09}, 15);
    add("DD LD IX,nn",    none, {0xDD, 0x21, 0x34, 0x12}, 14);
    add("DD LD (nn),IX",  none, {0xDD, 0x22, 0x00, 0x40}, 20);
    add("DD LD IX,(nn)",  none, {0xDD, 0x2A, 0x00, 0x40}, 20);
    add("DD INC IX",      none, {0xDD, 0x23}, 10);
    add("DD INC (IX+d)",  none, {0xDD, 0x34, 0x01}, 23);
    add("DD LD (IX+d),n", none, {0xDD, 0x36, 0x01, 0x55}, 19);
    add("DD LD B,(IX+d)", none, {0xDD, 0x46, 0x01}, 19);
    add("DD LD (IX+d),B", none, {0xDD, 0x70, 0x01}, 19);
    add("DD ADD A,(IX+d)",none, {0xDD, 0x86, 0x01}, 19);
    add("DD PUSH IX",     none, {0xDD, 0xE5}, 15);
    add("DD POP IX",      none, {0xDD, 0xE1}, 14);
    add("DD EX (SP),IX",  none, {0xDD, 0xE3}, 23);
    add("DD JP (IX)",     none, {0xDD, 0xE9}, 8);
    add("DD LD SP,IX",    none, {0xDD, 0xF9}, 10);
    add("DD INC IXH",     none, {0xDD, 0x24}, 8);
    add("DD LD IXH,n",    none, {0xDD, 0x26, 0x12}, 11);

    // --- DDCB space ---
    add("DDCB RLC (IX+d)", none, {0xDD, 0xCB, 0x01, 0x06}, 23);
    add("DDCB BIT 0,(IX+d)", none, {0xDD, 0xCB, 0x01, 0x46}, 20);
    add("DDCB SET 0,(IX+d)", none, {0xDD, 0xCB, 0x01, 0xC6}, 23);

    return c;
}

}  // namespace

int main() {
    std::vector<std::uint8_t> memory(65536, 0x00);

    mercury::core::Z80Cpu cpu;
    mercury::core::Z80BusCallbacks callbacks;
    callbacks.read8 = [&memory](std::uint16_t address) { return memory[address]; };
    callbacks.write8 = [&memory](std::uint16_t address, std::uint8_t value) { memory[address] = value; };
    callbacks.in = [](std::uint8_t) { return std::uint8_t{0xFF}; };
    callbacks.out = [](std::uint8_t, std::uint8_t) {};
    callbacks.io_wait_cycles = [](std::uint8_t, bool) { return std::uint32_t{0}; };
    callbacks.advance_io_timing = [](std::uint32_t) {};
    cpu.attach_bus(callbacks);

    const auto cases = build_cases();
    int failures = 0;

    std::printf("-- instruction timing --\n");
    for (const auto& test : cases) {
        std::fill(memory.begin(), memory.end(), std::uint8_t{0x00});
        cpu.reset();
        // Give the stack somewhere sane to live so PUSH/POP/CALL don't scribble
        // over the code under test.
        cpu.set_stack_pointer(0xF000);

        std::uint16_t at = 0x0000;
        for (auto byte : test.setup) {
            memory[at++] = byte;
        }
        const std::uint16_t test_at = at;
        for (auto byte : test.code) {
            memory[at++] = byte;
        }

        // Run the setup instructions off the clock.
        while (cpu.registers().pc < test_at) {
            auto ignored = cpu.step();
            ignored.cycles += cpu.take_pending_wait_cycles();
            if (!ignored.executed || ignored.unimplemented) {
                break;
            }
        }

        auto result = cpu.step();
        const auto cycles = result.cycles + cpu.take_pending_wait_cycles();

        if (cycles != test.expected) {
            ++failures;
            std::printf("  MISMATCH %-22s got %2u, expected %2u\n",
                        test.name, cycles, test.expected);
        }
    }
    std::printf("  %d/%zu instructions match documented timing\n",
                static_cast<int>(cases.size()) - failures, cases.size());

    // --- interrupt acceptance latency ---
    std::printf("\n-- interrupt acceptance --\n");
    struct IntCase {
        const char* name;
        std::uint8_t im_opcode;  // ED xx to select the mode
        std::uint32_t expected;
    };
    const IntCase int_cases[] = {
        {"IM 1 maskable", 0x56, 13},
        {"IM 2 maskable", 0x5E, 19},
    };

    for (const auto& ic : int_cases) {
        std::fill(memory.begin(), memory.end(), std::uint8_t{0x00});
        cpu.reset();
        cpu.set_stack_pointer(0xF000);
        memory[0] = 0xED;
        memory[1] = ic.im_opcode;  // IM n
        memory[2] = 0xFB;          // EI
        memory[3] = 0x00;          // NOP - EI delay covers this one
        memory[4] = 0x00;          // NOP - interrupt lands here

        auto r = cpu.step();  // IM n
        r = cpu.step();       // EI
        cpu.set_maskable_interrupt_line(true);
        r = cpu.step();       // NOP, must NOT be interrupted (EI delay)
        if (r.message.rfind("INT accepted", 0) == 0) {
            std::printf("  FAIL %-16s interrupt accepted during the EI delay slot\n", ic.name);
            ++failures;
            continue;
        }
        r = cpu.step();       // acceptance should happen here
        const auto cycles = r.cycles + cpu.take_pending_wait_cycles();
        if (r.message.rfind("INT accepted", 0) != 0) {
            std::printf("  FAIL %-16s no interrupt accepted after EI delay\n", ic.name);
            ++failures;
        } else if (cycles != ic.expected) {
            std::printf("  MISMATCH %-16s got %u, expected %u\n", ic.name, cycles, ic.expected);
            ++failures;
        } else {
            std::printf("  OK   %-16s %u cycles\n", ic.name, cycles);
        }
    }

    // NMI is 11 T-states and ignores IFF1.
    {
        std::fill(memory.begin(), memory.end(), std::uint8_t{0x00});
        cpu.reset();
        cpu.set_stack_pointer(0xF000);
        cpu.request_non_maskable_interrupt();
        auto r = cpu.step();
        const auto cycles = r.cycles + cpu.take_pending_wait_cycles();
        if (r.message.rfind("NMI accepted", 0) != 0) {
            std::printf("  FAIL NMI          not accepted\n");
            ++failures;
        } else if (cycles != 11) {
            std::printf("  MISMATCH NMI      got %u, expected 11\n", cycles);
            ++failures;
        } else {
            std::printf("  OK   NMI              %u cycles\n", cycles);
        }
    }

    std::printf("\ntotal timing failures: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
