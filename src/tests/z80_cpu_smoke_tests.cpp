#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "mercury/core/z80_cpu.hpp"

namespace {

struct RamBus {
    std::array<std::uint8_t, 65536> memory{};
    std::array<std::uint8_t, 256> ports_in{};
    std::array<std::uint8_t, 256> ports_out{};

    [[nodiscard]] mercury::core::Z80BusCallbacks callbacks() {
        mercury::core::Z80BusCallbacks bus;
        bus.read8 = [this](std::uint16_t address) { return memory[address]; };
        bus.write8 = [this](std::uint16_t address, std::uint8_t value) { memory[address] = value; };
        bus.in = [this](std::uint8_t port) { return ports_in[port]; };
        bus.out = [this](std::uint8_t port, std::uint8_t value) { ports_out[port] = value; };
        return bus;
    }
};

std::uint16_t bc(const mercury::core::Z80Registers& regs) {
    return static_cast<std::uint16_t>((regs.b << 8) | regs.c);
}

std::uint16_t de(const mercury::core::Z80Registers& regs) {
    return static_cast<std::uint16_t>((regs.d << 8) | regs.e);
}

std::uint16_t hl(const mercury::core::Z80Registers& regs) {
    return static_cast<std::uint16_t>((regs.h << 8) | regs.l);
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_rrca() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    bus.memory[0x0000] = 0x3E;
    bus.memory[0x0001] = 0x81;
    bus.memory[0x0002] = 0x0F;

    cpu.step();
    cpu.step();

    const auto& regs = cpu.registers();
    expect(regs.a == 0xC0, "RRCA should rotate A right circular.");
    expect((regs.f & 0x01) != 0, "RRCA should copy bit 0 into carry.");
}

void test_rotate_a_preserves_szpv() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 10> program{
        0x3E, 0x00,  // LD A,00h
        0xB7,        // OR A (set Z and parity)
        0x3E, 0x81,  // LD A,81h
        0x07,        // RLCA
        0x0F,        // RRCA
        0x37,        // SCF
        0x17,        // RLA
        0x1F,        // RRA
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    for (std::size_t i = 0; i < program.size(); ++i) {
        cpu.step();
    }

    const auto& regs = cpu.registers();
    expect((regs.f & 0x40) != 0, "Accumulator rotates should preserve zero.");
    expect((regs.f & 0x04) != 0, "Accumulator rotates should preserve parity/overflow.");
}

void test_ld_a_from_bc() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    bus.memory[0x0000] = 0x01;
    bus.memory[0x0001] = 0x34;
    bus.memory[0x0002] = 0x12;
    bus.memory[0x0003] = 0x0A;
    bus.memory[0x1234] = 0x5A;

    cpu.step();
    cpu.step();

    expect(cpu.registers().a == 0x5A, "LD A,(BC) should read from the BC address.");
}

void test_exx() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 22> program{
        0x01, 0x11, 0x11,  // LD BC,1111h
        0x11, 0x22, 0x22,  // LD DE,2222h
        0x21, 0x33, 0x33,  // LD HL,3333h
        0xD9,              // EXX
        0x01, 0xAA, 0xAA,  // LD BC,AAAAh
        0x11, 0xBB, 0xBB,  // LD DE,BBBBh
        0x21, 0xCC, 0xCC,  // LD HL,CCCCh
        0xD9,              // EXX
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    for (std::size_t i = 0; i < 9; ++i) {
        cpu.step();
    }

    const auto& regs = cpu.registers();
    expect(bc(regs) == 0x1111, "EXX should restore the primary BC pair.");
    expect(de(regs) == 0x2222, "EXX should restore the primary DE pair.");
    expect(hl(regs) == 0x3333, "EXX should restore the primary HL pair.");
}

void test_srl_d() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    bus.memory[0x0000] = 0x16;
    bus.memory[0x0001] = 0x85;
    bus.memory[0x0002] = 0xCB;
    bus.memory[0x0003] = 0x3A;

    cpu.step();
    cpu.step();

    const auto& regs = cpu.registers();
    expect(regs.d == 0x42, "CB 3A should perform SRL D.");
    expect((regs.f & 0x01) != 0, "SRL D should move bit 0 into carry.");
}

void test_inc_iy_displacement() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 7> program{
        0xFD, 0x21, 0x00, 0x20,  // LD IY,2000h
        0xFD, 0x34, 0x02,        // INC (IY+2)
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }
    bus.memory[0x2002] = 0x7F;

    cpu.step();
    cpu.step();

    const auto& regs = cpu.registers();
    expect(bus.memory[0x2002] == 0x80, "INC (IY+d) should update indexed memory.");
    expect((regs.f & 0x80) != 0, "INC (IY+d) should set sign when result is negative.");
    expect((regs.f & 0x04) != 0, "INC (IY+d) should set overflow when 7Fh becomes 80h.");
}

void test_adc_a_ix_displacement() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 12> program{
        0x3E, 0x01,              // LD A,01h
        0x0F,                    // RRCA (set carry)
        0x3E, 0x20,              // LD A,20h
        0xDD, 0x21, 0x00, 0x20,  // LD IX,2000h
        0xDD, 0x8E, 0x02,        // ADC A,(IX+2)
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }
    bus.memory[0x2002] = 0x11;

    for (std::size_t i = 0; i < 5; ++i) {
        cpu.step();
    }

    const auto& regs = cpu.registers();
    expect(regs.a == 0x32, "ADC A,(IX+d) should add indexed memory and carry.");
    expect((regs.f & 0x01) == 0, "ADC A,(IX+d) should leave carry clear when no overflow occurs.");
}

void test_set_0_iy_displacement() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 8> program{
        0xFD, 0x21, 0x00, 0x20,  // LD IY,2000h
        0xFD, 0xCB, 0x00, 0xC6,  // SET 0,(IY+0)
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }
    bus.memory[0x2000] = 0x08;

    cpu.step();
    const auto result = cpu.step();

    expect(bus.memory[0x2000] == 0x09, "SET 0,(IY+d) should update indexed memory.");
    expect(result.prefix == 0xFD, "SET 0,(IY+d) should report the IY prefix.");
    expect(result.opcode == 0xC6, "SET 0,(IY+d) should expose the indexed CB opcode byte.");
    expect(result.length == 4, "SET 0,(IY+d) should consume four bytes.");
}

void test_dec_de() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 4> program{
        0x11, 0x00, 0x20,  // LD DE,2000h
        0x1B,              // DEC DE
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    cpu.step();
    cpu.step();

    expect(de(cpu.registers()) == 0x1FFF, "DEC DE should decrement the DE register pair.");
}

void test_cpl() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 4> program{
        0x3E, 0x55,  // LD A,55h
        0x37,        // SCF
        0x2F,        // CPL
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    cpu.step();
    cpu.step();
    cpu.step();

    const auto& regs = cpu.registers();
    expect(regs.a == 0xAA, "CPL should invert all bits in A.");
    expect((regs.f & 0x01) != 0, "CPL should preserve carry.");
    expect((regs.f & 0x10) != 0, "CPL should set half-carry.");
    expect((regs.f & 0x02) != 0, "CPL should set subtract.");
}

void test_ccf() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 3> program{
        0x37,  // SCF
        0x3F,  // CCF
        0x3F,  // CCF
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    cpu.step();
    cpu.step();

    auto regs = cpu.registers();
    expect((regs.f & 0x01) == 0, "CCF should clear carry when the old carry was set.");
    expect((regs.f & 0x10) != 0, "CCF should copy the old carry into half-carry.");
    expect((regs.f & 0x02) == 0, "CCF should clear subtract.");

    cpu.step();
    regs = cpu.registers();
    expect((regs.f & 0x01) != 0, "CCF should set carry when the old carry was clear.");
    expect((regs.f & 0x10) == 0, "CCF should clear half-carry when the old carry was clear.");
}

void test_ldi() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 12> program{
        0x21, 0x00, 0x20,  // LD HL,2000h
        0x11, 0x00, 0x30,  // LD DE,3000h
        0x01, 0x02, 0x00,  // LD BC,0002h
        0xED, 0xA0,        // LDI
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }
    bus.memory[0x2000] = 0x5A;

    for (std::size_t i = 0; i < 4; ++i) {
        cpu.step();
    }

    const auto& regs = cpu.registers();
    expect(bus.memory[0x3000] == 0x5A, "LDI should copy the byte from HL to DE.");
    expect(hl(regs) == 0x2001, "LDI should increment HL.");
    expect(de(regs) == 0x3001, "LDI should increment DE.");
    expect(bc(regs) == 0x0001, "LDI should decrement BC.");
    expect((regs.f & 0x04) != 0, "LDI should set PV when BC remains nonzero.");
}

void test_ldi_preserves_zero_and_carry() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 18> program{
        0x3E, 0x01,        // LD A,01h
        0x0F,              // RRCA (set carry)
        0x3E, 0x00,        // LD A,00h
        0xB7,              // OR A (set zero, preserve carry clear)
        0x37,              // SCF
        0x21, 0x00, 0x20,  // LD HL,2000h
        0x11, 0x00, 0x30,  // LD DE,3000h
        0x01, 0x01, 0x00,  // LD BC,0001h
        0xED, 0xA0,        // LDI
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }
    bus.memory[0x2000] = 0x5A;

    for (std::size_t i = 0; i < 9; ++i) {
        cpu.step();
    }

    const auto& regs = cpu.registers();
    expect((regs.f & 0x40) != 0, "LDI should preserve zero.");
    expect((regs.f & 0x01) != 0, "LDI should preserve carry.");
    expect((regs.f & 0x04) == 0,
           "LDI should clear PV when BC reaches zero (F=" + std::to_string(regs.f) +
               ", BC=" + std::to_string(bc(regs)) + ").");
}

void test_ldir() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 11> program{
        0x21, 0x00, 0x20,  // LD HL,2000h
        0x11, 0x00, 0x30,  // LD DE,3000h
        0x01, 0x02, 0x00,  // LD BC,0002h
        0xED, 0xB0,        // LDIR
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }
    bus.memory[0x2000] = 0x12;
    bus.memory[0x2001] = 0x34;

    for (std::size_t i = 0; i < 5; ++i) {
        cpu.step();
    }

    const auto& regs = cpu.registers();
    expect(bus.memory[0x3000] == 0x12, "LDIR should copy the first byte.");
    expect(bus.memory[0x3001] == 0x34, "LDIR should copy the second byte.");
    expect(hl(regs) == 0x2002, "LDIR should increment HL across the whole transfer.");
    expect(de(regs) == 0x3002, "LDIR should increment DE across the whole transfer.");
    expect(bc(regs) == 0x0000, "LDIR should stop with BC at zero.");
    expect((regs.f & 0x04) == 0, "LDIR should clear PV when the repeat finishes.");
}

void test_ld_bc_indirect_from_a() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 6> program{
        0x01, 0x34, 0x12,  // LD BC,1234h
        0x3E, 0x5A,        // LD A,5Ah
        0x02,              // LD (BC),A
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    cpu.step();
    cpu.step();
    cpu.step();

    expect(bus.memory[0x1234] == 0x5A, "LD (BC),A should store A at the BC address.");
}

void test_dec_hl() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 4> program{
        0x21, 0x00, 0x20,  // LD HL,2000h
        0x2B,              // DEC HL
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    cpu.step();
    cpu.step();

    expect(hl(cpu.registers()) == 0x1FFF, "DEC HL should decrement the HL register pair.");
}

void test_daa_after_add() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 5> program{
        0x3E, 0x09,  // LD A,09h
        0xC6, 0x09,  // ADD A,09h
        0x27,        // DAA
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    cpu.step();
    cpu.step();
    cpu.step();

    const auto& regs = cpu.registers();
    expect(regs.a == 0x18, "DAA should convert 09h + 09h into packed BCD 18h.");
    expect((regs.f & 0x01) == 0, "DAA should leave carry clear for 18h.");
}

void test_sbc_hl_bc() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 9> program{
        0x21, 0x00, 0x10,  // LD HL,1000h
        0x01, 0x01, 0x00,  // LD BC,0001h
        0xAF,              // XOR A (clear carry)
        0xED, 0x42,        // SBC HL,BC
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    for (std::size_t i = 0; i < 4; ++i) {
        cpu.step();
    }

    const auto& regs = cpu.registers();
    expect(hl(regs) == 0x0FFF, "SBC HL,BC should subtract the 16-bit operand from HL.");
    expect((regs.f & 0x02) != 0, "SBC HL,BC should set subtract.");
    expect((regs.f & 0x10) != 0, "SBC HL,BC should set half-carry when bit 11 borrows.");
    expect((regs.f & 0x01) == 0, "SBC HL,BC should leave carry clear when no full borrow occurs.");
}

void test_ld_bc_indirect_roundtrip() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 15> program{
        0x01, 0x34, 0x12,        // LD BC,1234h
        0xED, 0x43, 0x00, 0x20,  // LD (2000h),BC
        0x01, 0x00, 0x00,        // LD BC,0000h
        0xED, 0x4B, 0x00, 0x20,  // LD BC,(2000h)
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    for (std::size_t i = 0; i < 4; ++i) {
        cpu.step();
    }

    const auto& regs = cpu.registers();
    expect(bus.memory[0x2000] == 0x34, "LD (nn),BC should write the low byte first.");
    expect(bus.memory[0x2001] == 0x12, "LD (nn),BC should write the high byte second.");
    expect(bc(regs) == 0x1234, "LD BC,(nn) should restore the stored value.");
}

void test_srl_d_rr_e_shift_pair() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 8> program{
        0x16, 0x12,  // LD D,12h
        0x1E, 0x34,  // LD E,34h
        0xCB, 0x3A,  // SRL D
        0xCB, 0x1B,  // RR E
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }

    for (std::size_t i = 0; i < 4; ++i) {
        cpu.step();
    }

    const auto& regs = cpu.registers();
    expect(regs.d == 0x09, "SRL D should shift the high byte right.");
    expect(regs.e == 0x1A, "RR E should pull in the carry from SRL D.");
    expect((regs.f & 0x01) == 0, "RR E should leave carry clear when bit 0 of E was zero.");
}

void test_bit_7_iy_displacement() {
    RamBus bus;
    mercury::core::Z80Cpu cpu;
    cpu.attach_bus(bus.callbacks());
    cpu.reset();

    const std::array<std::uint8_t, 8> program{
        0xFD, 0x21, 0x00, 0x20,  // LD IY,2000h
        0xFD, 0xCB, 0x03, 0x7E,  // BIT 7,(IY+3)
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        bus.memory[i] = program[i];
    }
    bus.memory[0x2003] = 0x80;

    cpu.step();
    const auto result = cpu.step();

    const auto& regs = cpu.registers();
    expect(result.prefix == 0xFD, "BIT 7,(IY+d) should report the IY prefix.");
    expect(result.opcode == 0x7E, "BIT 7,(IY+d) should expose the CB opcode byte.");
    expect(result.length == 4, "BIT 7,(IY+d) should consume four bytes.");
    expect(result.pc_before == 0x0004, "BIT 7,(IY+d) should start at the FD prefix address.");
    expect(result.pc_after == 0x0008, "BIT 7,(IY+d) should advance past FD CB d op.");
    expect((regs.f & 0x40) == 0, "BIT 7,(IY+d) should clear zero when bit 7 is set.");
    expect((regs.f & 0x80) != 0, "BIT 7,(IY+d) should set sign when bit 7 is set.");
    expect((regs.f & 0x10) != 0, "BIT 7,(IY+d) should set half-carry.");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"rrca", test_rrca},
        {"rotate_a_preserves_szpv", test_rotate_a_preserves_szpv},
        {"ld_a_from_bc", test_ld_a_from_bc},
        {"exx", test_exx},
        {"srl_d", test_srl_d},
        {"inc_iy_displacement", test_inc_iy_displacement},
        {"adc_a_ix_displacement", test_adc_a_ix_displacement},
        {"set_0_iy_displacement", test_set_0_iy_displacement},
        {"dec_de", test_dec_de},
        {"cpl", test_cpl},
        {"ccf", test_ccf},
        {"ldi", test_ldi},
        {"ldi_preserves_zero_and_carry", test_ldi_preserves_zero_and_carry},
        {"ldir", test_ldir},
        {"ld_bc_indirect_from_a", test_ld_bc_indirect_from_a},
        {"dec_hl", test_dec_hl},
        {"daa_after_add", test_daa_after_add},
        {"sbc_hl_bc", test_sbc_hl_bc},
        {"ld_bc_indirect_roundtrip", test_ld_bc_indirect_roundtrip},
        {"srl_d_rr_e_shift_pair", test_srl_d_rr_e_shift_pair},
        {"bit_7_iy_displacement", test_bit_7_iy_displacement},
    };

    int failures = 0;

    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << name << ": " << error.what() << "\n";
        }
    }

    return failures == 0 ? 0 : 1;
}
