#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mercury::core {

struct Z80BusCallbacks {
    std::function<std::uint8_t(std::uint16_t)> read8;
    std::function<void(std::uint16_t, std::uint8_t)> write8;
    std::function<std::uint8_t(std::uint8_t)> in;
    std::function<void(std::uint8_t, std::uint8_t)> out;
    std::function<std::uint32_t(std::uint8_t, bool)> io_wait_cycles;
    std::function<void(std::uint32_t)> advance_io_timing;
    // How many of an I/O instruction's own T-states elapse before it actually
    // samples the bus. Devices are advanced by this much ahead of the read so
    // a port read can observe a flag that changes part-way through the
    // instruction; the remainder is advanced afterwards, so the instruction's
    // total length is unchanged.
    std::function<std::uint32_t()> io_read_phase_cycles;
};

enum class Z80InterruptMode {
    mode0,
    mode1,
    mode2,
};

struct Z80Registers {
    std::uint8_t a = 0x00;
    std::uint8_t f = 0x00;
    std::uint8_t b = 0x00;
    std::uint8_t c = 0x00;
    std::uint8_t d = 0x00;
    std::uint8_t e = 0x00;
    std::uint8_t h = 0x00;
    std::uint8_t l = 0x00;
    std::uint8_t i = 0x00;
    std::uint8_t r = 0x00;
    std::uint16_t ix = 0x0000;
    std::uint16_t iy = 0x0000;
    std::uint16_t sp = 0xFFFF;
    std::uint16_t pc = 0x0000;
};

struct Z80StepResult {
    bool executed = false;
    bool unimplemented = false;
    std::uint8_t opcode = 0x00;
    std::uint8_t prefix = 0x00;
    std::uint8_t length = 0;
    std::uint32_t cycles = 0;
    std::uint16_t pc_before = 0x0000;
    std::uint16_t pc_after = 0x0000;
    std::string message;
};

class Z80Cpu {
public:
    void attach_bus(Z80BusCallbacks callbacks);
    void reset() noexcept;
    // Z80 RESET does not define SP - it keeps whatever the hardware left
    // there - so the host system decides the power-on value.
    void set_stack_pointer(std::uint16_t value) noexcept;

    [[nodiscard]] Z80StepResult step() noexcept;
    void request_maskable_interrupt() noexcept;
    void set_maskable_interrupt_line(bool active) noexcept;
    void request_non_maskable_interrupt() noexcept;
    [[nodiscard]] std::uint32_t take_pending_wait_cycles() noexcept;
    [[nodiscard]] std::uint32_t take_applied_io_wait_cycles() noexcept;

    [[nodiscard]] const Z80Registers& registers() const noexcept;
    [[nodiscard]] Z80InterruptMode interrupt_mode() const noexcept;
    [[nodiscard]] bool interrupts_enabled() const noexcept;
    [[nodiscard]] bool halted() const noexcept;
    [[nodiscard]] std::string describe() const;

private:
    static constexpr std::uint8_t kFlagCarry = 0x01;
    static constexpr std::uint8_t kFlagSubtract = 0x02;
    static constexpr std::uint8_t kFlagParityOverflow = 0x04;
    static constexpr std::uint8_t kFlagHalfCarry = 0x10;
    static constexpr std::uint8_t kFlagZero = 0x40;
    static constexpr std::uint8_t kFlagSign = 0x80;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::uint8_t read8(std::uint16_t address) noexcept;
    void write8(std::uint16_t address, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t in(std::uint8_t port) noexcept;
    void out(std::uint8_t port, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t fetch8() noexcept;
    [[nodiscard]] std::uint16_t fetch16() noexcept;
    [[nodiscard]] std::uint16_t* select_index_register(std::uint8_t prefix) noexcept;
    [[nodiscard]] std::uint16_t indexed_address(std::uint8_t prefix, std::int8_t displacement) noexcept;
    [[nodiscard]] std::uint16_t hl() const noexcept;
    void set_hl(std::uint16_t value) noexcept;
    [[nodiscard]] std::uint16_t bc() const noexcept;
    void set_bc(std::uint16_t value) noexcept;
    [[nodiscard]] std::uint16_t de() const noexcept;
    void set_de(std::uint16_t value) noexcept;
    [[nodiscard]] std::uint16_t af() const noexcept;
    void set_af(std::uint16_t value) noexcept;
    [[nodiscard]] std::uint8_t read_alu_operand(std::uint8_t selector) noexcept;
    [[nodiscard]] bool zero_flag() const noexcept;
    [[nodiscard]] bool carry_flag() const noexcept;
    [[nodiscard]] bool parity_overflow_flag() const noexcept;
    [[nodiscard]] bool sign_flag() const noexcept;
    [[nodiscard]] bool condition_holds(std::uint8_t condition_code) const noexcept;
    void set_compare_flags(std::uint8_t lhs, std::uint8_t rhs) noexcept;
    void set_or_flags(std::uint8_t value) noexcept;
    void set_add_flags(std::uint8_t lhs, std::uint8_t rhs, std::uint8_t carry_in, std::uint8_t result) noexcept;
    void set_subtract_flags(std::uint8_t lhs, std::uint8_t rhs, std::uint8_t carry_in, std::uint8_t result) noexcept;
    void set_logic_flags(std::uint8_t value, bool set_half_carry) noexcept;
    void push16(std::uint16_t value) noexcept;
    [[nodiscard]] std::uint16_t pop16() noexcept;
    void finish_step() noexcept;
    [[nodiscard]] Z80StepResult service_interrupts() noexcept;
    [[nodiscard]] Z80StepResult step_cb_prefixed() noexcept;
    [[nodiscard]] Z80StepResult step_ed_prefixed() noexcept;
    [[nodiscard]] Z80StepResult step_indexed_prefixed(std::uint8_t prefix, std::uint16_t pc_before) noexcept;

  public:
    // See the note in M68kCpu: callbacks_ is wiring, not state.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(registers_);
        ar(shadow_registers_);
        ar(interrupt_mode_);
        ar(iff1_);
        ar(iff2_);
        ar(halted_);
        ar(maskable_interrupt_pending_);
        ar(maskable_interrupt_line_);
        ar(non_maskable_interrupt_pending_);
        ar(ei_delay_);
        ar(pending_wait_cycles_);
        ar(applied_io_wait_cycles_);
        if constexpr (Archive::loading()) {
            // A loaded state can come from someone else. Wait states are a
            // handful of cycles; billions would be charged to one instruction
            // and stall the whole machine. EI's delay is one instruction.
            if (pending_wait_cycles_ > 1000) pending_wait_cycles_ = 0;
            if (applied_io_wait_cycles_ > 1000) applied_io_wait_cycles_ = 0;
            if (ei_delay_ > 2) ei_delay_ = 0;
        }
    }

  private:
    Z80BusCallbacks callbacks_{};
    Z80Registers registers_{};
    Z80Registers shadow_registers_{};
    Z80InterruptMode interrupt_mode_ = Z80InterruptMode::mode0;
    bool iff1_ = false;
    bool iff2_ = false;
    bool halted_ = false;
    bool maskable_interrupt_pending_ = false;
    bool maskable_interrupt_line_ = false;
    bool non_maskable_interrupt_pending_ = false;
    std::uint8_t ei_delay_ = 0;
    std::uint32_t pending_wait_cycles_ = 0;
    std::uint32_t applied_io_wait_cycles_ = 0;
};

}  // namespace mercury::core
