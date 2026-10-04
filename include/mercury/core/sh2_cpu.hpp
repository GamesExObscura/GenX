#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace mercury::core {

// Hitachi SH-2, the 32X's processor. There are two of them on the adapter,
// master and slave, sharing one bus.
//
// Shape deliberately mirrors Z80Cpu and M68kCpu so the same harness pattern
// works: a bus of std::function callbacks, a step() that reports what it did
// and how long it took, and no knowledge of any particular machine.
//
// What drives the design, and what makes this different from the other two:
//
//   * Every instruction is exactly 16 bits. There is no prefix byte and no
//     variable length, so the decoder is a switch on the top nibble and then
//     on the bottom, and nothing has to be fetched twice.
//   * 32-bit registers, 32-bit addresses, big-endian - the same byte order as
//     the 68000 next to it.
//   * DELAYED BRANCHES. The instruction AFTER a branch executes before the
//     branch takes effect. This is the single biggest source of wrong
//     behaviour in an SH-2 core: a naive interpreter that jumps immediately
//     runs the delay slot instruction never, or twice. Modelled explicitly
//     with a pending-branch target rather than by recursion, because an
//     instruction in a delay slot may not itself be a branch and the core has
//     to be able to say so.
//   * Accesses must be aligned. A word read at an odd address and a longword
//     read at a non-multiple of four are address errors on hardware.
struct Sh2BusCallbacks {
    std::function<std::uint8_t(std::uint32_t)> read8;
    std::function<std::uint16_t(std::uint32_t)> read16;
    std::function<std::uint32_t(std::uint32_t)> read32;
    std::function<void(std::uint32_t, std::uint8_t)> write8;
    std::function<void(std::uint32_t, std::uint16_t)> write16;
    std::function<void(std::uint32_t, std::uint32_t)> write32;
};

struct Sh2Registers {
    // R15 is the stack pointer by convention; the hardware only treats it
    // specially in the exception sequence.
    std::array<std::uint32_t, 16> r{};
    std::uint32_t pc = 0;
    // Procedure Register: where BSR/JSR put the return address.
    std::uint32_t pr = 0;
    // Global Base Register, the base for the @(disp,GBR) addressing modes.
    std::uint32_t gbr = 0;
    // Vector Base Register: exception vectors are read relative to this.
    std::uint32_t vbr = 0;
    // Multiply-accumulate result, high and low.
    std::uint32_t mach = 0;
    std::uint32_t macl = 0;
    // Status register. Bit 0 T (the condition flag every compare writes and
    // every conditional branch reads), bit 1 S (saturation for MAC), bits 4-7
    // the interrupt mask, bit 8 Q and bit 9 M (the division state bits).
    std::uint32_t sr = 0x000000F0;
};

struct Sh2StepResult {
    bool executed = false;
    bool unimplemented = false;
    std::uint16_t opcode = 0x0000;
    std::uint32_t cycles = 0;
    std::uint32_t pc_before = 0;
    std::uint32_t pc_after = 0;
    // True when this step was the delay slot of a branch. Worth reporting:
    // "the delay slot ran" is the thing that is wrong in a broken SH-2 core,
    // and a probe cannot see it from the outside.
    bool in_delay_slot = false;
    // Always a string literal. It was a std::string, and building and
    // destroying one on every step cost the 32X a tenth of its host time -
    // the step is the hottest function in the emulator and almost never has
    // anything to say. The unimplemented opcode itself is in `opcode`.
    std::string_view message;
};

// The fast way in, for a machine whose bus is one object. Sh2BusCallbacks
// stays for harnesses that want to wire up lambdas; a std::function costs an
// emptiness check and a second indirect call on every access, and an SH-2
// does an access on every instruction just to fetch it.
class Sh2Bus {
  public:
    virtual std::uint8_t read8(std::uint32_t address) = 0;
    virtual std::uint16_t read16(std::uint32_t address) = 0;
    virtual std::uint32_t read32(std::uint32_t address) = 0;
    virtual void write8(std::uint32_t address, std::uint8_t value) = 0;
    virtual void write16(std::uint32_t address, std::uint16_t value) = 0;
    virtual void write32(std::uint32_t address, std::uint32_t value) = 0;

    // Where instruction fetches may skip the bus. If `address` sits in plain
    // memory that reads with no side effects, return a pointer to the first
    // byte of a window around it and give the window's first address and its
    // size in bytes (even). Every word in the window must read back exactly
    // as read16 would give it, because the processor will read it from here
    // instead. nullptr - the default - sends fetches through read16.
    virtual const std::uint8_t* fetch_window(std::uint32_t /*address*/, std::uint32_t& /*base*/,
                                             std::uint32_t& /*size*/) {
        return nullptr;
    }

  protected:
    ~Sh2Bus() = default;
};

class Sh2Cpu {
  public:
    // Status register bits, public because the machine sets the interrupt
    // mask and reads T.
    static constexpr std::uint32_t kFlagT = 0x00000001;
    static constexpr std::uint32_t kFlagS = 0x00000002;
    static constexpr std::uint32_t kFlagQ = 0x00000100;
    static constexpr std::uint32_t kFlagM = 0x00000200;
    static constexpr std::uint32_t kInterruptMask = 0x000000F0;

    void attach(Sh2BusCallbacks bus) {
        bus_ = std::move(bus);
        direct_ = nullptr;
        invalidate_fetch_window();
    }
    // Attach a bus object directly. Takes precedence over the callbacks.
    void attach(Sh2Bus* bus) noexcept {
        direct_ = bus;
        invalidate_fetch_window();
    }
    // Forget the fetch window, so the next fetch asks the bus again. For
    // when the memory behind it may have moved - a cartridge swapped, say.
    void invalidate_fetch_window() noexcept {
        fetch_data_ = nullptr;
        fetch_base_ = 0;
        fetch_size_ = 0;
    }

    // The reset sequence reads the first two longwords of the vector table:
    // entry 0 is the starting PC and entry 1 the starting stack pointer. That
    // is why a 32X cartridge has an SH-2 vector table in its header.
    void reset() noexcept;
    // For a harness that wants to place the core somewhere directly.
    void set_pc(std::uint32_t pc) noexcept { registers_.pc = pc; }

    [[nodiscard]] Sh2Registers& registers() noexcept { return registers_; }
    [[nodiscard]] const Sh2Registers& registers() const noexcept { return registers_; }

    [[nodiscard]] bool t_flag() const noexcept { return (registers_.sr & kFlagT) != 0; }
    void set_t_flag(bool on) noexcept {
        registers_.sr = on ? (registers_.sr | kFlagT) : (registers_.sr & ~kFlagT);
    }

    // One instruction, including its delay slot handling. Returns what it did.
    Sh2StepResult step() noexcept;

    // Raise an interrupt at `level` through vector `vector`. Taken only when
    // the level is above the mask in SR, which is what makes the 32X's
    // interrupt priorities work.
    void request_interrupt(int level, int vector) noexcept;
    // Present the level an interrupt LINE is at right now, replacing whatever
    // was requested before. Level 0 withdraws it.
    //
    // This is what a machine with level-triggered sources has to call, and it
    // is different from request_interrupt in the one way that matters: a
    // request that is acknowledged before it is taken must disappear. A latch
    // that only ever rises keeps the stale request, and the processor takes
    // it the moment its mask drops - a second handler run with no event
    // behind it.
    // Inline: the machine calls this before every instruction.
    void set_interrupt_level(int level, int vector) noexcept {
        pending_level_ = level > 0 ? level : 0;
        pending_vector_ = level > 0 ? vector : 0;
    }
    [[nodiscard]] bool interrupt_pending() const noexcept { return pending_level_ > 0; }
    // How many interrupts this core has actually accepted. Requesting one and
    // taking one are different things - the mask in SR decides - and when a
    // 32X game sits waiting on a handler that never runs, this is the number
    // that says so.
    [[nodiscard]] std::uint64_t interrupts_taken() const noexcept { return interrupts_taken_; }

    // Held in reset by the Mega Drive side through the adapter control
    // register. A halted core consumes cycles and does nothing, which is what
    // the slave SH-2 does for most of a game's startup.
    void set_halted(bool halted) noexcept { halted_ = halted; }
    [[nodiscard]] bool halted() const noexcept { return halted_; }

    [[nodiscard]] std::string describe() const;

    // Everything that decides what the next instruction does. The bus and the
    // fetch window are wiring, not state; the window is dropped on a load so
    // the next fetch asks the bus again rather than trusting a pointer taken
    // before the memory behind it was replaced. interrupts_taken_ is a
    // diagnostic count and stays out, like the other cores' counters.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(registers_);
        ar(delay_pending_);
        ar(delay_target_);
        ar(pending_level_);
        ar(pending_vector_);
        ar(halted_);
        if constexpr (Archive::loading()) {
            invalidate_fetch_window();
        }
    }

  private:
    [[nodiscard]] std::uint8_t read8(std::uint32_t address) const;
    [[nodiscard]] std::uint16_t read16(std::uint32_t address) const;
    // An instruction fetch: from the fetch window when the address is in
    // it, through read16 otherwise.
    [[nodiscard]] std::uint16_t fetch(std::uint32_t address) noexcept {
        // Aligned the way the bus aligns a word read, so an odd PC reads
        // what read16 would have.
        const std::uint32_t into = (address & ~1u) - fetch_base_;
        if (into < fetch_size_) {
            return static_cast<std::uint16_t>((fetch_data_[into] << 8) | fetch_data_[into + 1]);
        }
        return fetch_slow(address);
    }
    [[nodiscard]] std::uint16_t fetch_slow(std::uint32_t address) noexcept;
    [[nodiscard]] std::uint32_t read32(std::uint32_t address) const;
    void write8(std::uint32_t address, std::uint8_t value) const;
    void write16(std::uint32_t address, std::uint16_t value) const;
    void write32(std::uint32_t address, std::uint32_t value) const;

    // Decode and run one opcode. Split out so the delay slot can run one
    // without re-entering the branch bookkeeping.
    void execute(std::uint16_t opcode, Sh2StepResult& result) noexcept;
    // Queue a branch to `target`, to be taken after the next instruction.
    void branch_after_delay(std::uint32_t target) noexcept;
    void take_interrupt(Sh2StepResult& result) noexcept;

    Sh2BusCallbacks bus_{};
    Sh2Bus* direct_ = nullptr;
    // The fetch window: fetch_size_ bytes of plain memory starting at
    // address fetch_base_. Empty until the bus offers one.
    const std::uint8_t* fetch_data_ = nullptr;
    std::uint32_t fetch_base_ = 0;
    std::uint32_t fetch_size_ = 0;
    Sh2Registers registers_{};

    // The delayed branch. `delay_pending_` means the NEXT instruction is a
    // delay slot and `delay_target_` is where to go afterwards.
    bool delay_pending_ = false;
    std::uint32_t delay_target_ = 0;

    int pending_level_ = 0;
    int pending_vector_ = 0;
    std::uint64_t interrupts_taken_ = 0;
    bool halted_ = false;
};

}  // namespace mercury::core
