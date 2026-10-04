#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace mercury::core {

// Motorola 68000, the Mega Drive's main CPU.
//
// Shape deliberately mirrors Z80Cpu so the same harness pattern works: a bus of
// std::function callbacks, a step() that reports what it did and how long it
// took, and no knowledge of any particular machine.
//
// Differences that drive the design:
//   * 16-bit data bus. read16/write16 are primitive; byte access is derived and
//     picks a half of the addressed word, so the bus callbacks stay honest
//     about what the hardware actually does.
//   * Big-endian. Instruction words and operands are MSB-first.
//   * 24-bit address bus. Addresses are masked to 0x00FFFFFF on every access;
//     software relies on that wrap.
//   * Two stack pointers. A7 is USP in user mode and SSP in supervisor mode;
//     switching the S bit swaps which one A7 refers to.
struct M68kBusCallbacks {
    std::function<std::uint16_t(std::uint32_t)> read16;
    std::function<void(std::uint32_t, std::uint16_t)> write16;
    // Optional. When absent, byte access is synthesised from read16/write16,
    // which is what a 16-bit bus does anyway.
    std::function<std::uint8_t(std::uint32_t)> read8;
    std::function<void(std::uint32_t, std::uint8_t)> write8;
};

struct M68kRegisters {
    std::array<std::uint32_t, 8> d{};
    // a[7] is the *active* stack pointer. usp/ssp hold the inactive one.
    std::array<std::uint32_t, 8> a{};
    std::uint32_t usp = 0;
    std::uint32_t ssp = 0;
    std::uint32_t pc = 0;
    // T-S--III---XNZVC. Reset leaves supervisor set and interrupts masked.
    std::uint16_t sr = 0x2700;
};

struct M68kStepResult {
    bool executed = false;
    bool unimplemented = false;
    std::uint16_t opcode = 0x0000;
    std::uint32_t cycles = 0;
    std::uint32_t pc_before = 0;
    std::uint32_t pc_after = 0;
    // Non-zero when this step was an interrupt acknowledge rather than an
    // instruction. The machine needs it to clear the right device flag: the VDP
    // holds IPL until the CPU actually takes the vector.
    int interrupt_serviced = 0;
    std::string message;
};

// Vector numbers the core raises itself.
enum class M68kException : std::uint8_t {
    bus_error = 2,
    address_error = 3,
    illegal_instruction = 4,
    divide_by_zero = 5,
    chk = 6,
    trapv = 7,
    privilege_violation = 8,
    trace = 9,
    line_a = 10,
    line_f = 11,
};

class M68kCpu {
public:
    void attach_bus(M68kBusCallbacks callbacks);
    // Reset reads SSP from vector 0 and PC from vector 1, as the hardware does.
    void reset() noexcept;

    [[nodiscard]] M68kStepResult step() noexcept;

    // Charge the real effective-address calculation time. Off by default; the
    // per-game quirk table turns it on for the few games that need the slower
    // CPU. See the note on ea_cycles() before making this a global default -
    // it gains one game and loses two.
    static void set_charge_ea_time(bool on) noexcept;

    // Which exception vectors have been taken, and the PC that caused the most
    // recent one. A game that runs for a while and then jumps somewhere absurd
    // has almost always taken an exception into an unused vector, and the whole
    // question is WHICH - guessing from the destination address cannot tell you.
    [[nodiscard]] const std::array<std::uint64_t, 64>& exception_tally() const noexcept {
        return exception_tally_;
    }
    [[nodiscard]] std::uint32_t last_exception_pc() const noexcept { return last_exception_pc_; }
    [[nodiscard]] std::uint8_t last_exception_vector() const noexcept {
        return last_exception_vector_;
    }

    // Levels 1-7; 7 is non-maskable. The host holds the line until the device
    // is acknowledged, matching how the VDP drives IPL.
    void set_interrupt_level(int level) noexcept;
    [[nodiscard]] int interrupt_level() const noexcept { return pending_interrupt_level_; }

    [[nodiscard]] const M68kRegisters& registers() const noexcept { return registers_; }
    [[nodiscard]] M68kRegisters& mutable_registers() noexcept { return registers_; }
    [[nodiscard]] bool halted() const noexcept { return halted_; }
    [[nodiscard]] bool supervisor() const noexcept { return (registers_.sr & kStatusSupervisor) != 0; }
    [[nodiscard]] std::string describe() const;

    // Status register bits.
    static constexpr std::uint16_t kStatusCarry = 0x0001;
    static constexpr std::uint16_t kStatusOverflow = 0x0002;
    static constexpr std::uint16_t kStatusZero = 0x0004;
    static constexpr std::uint16_t kStatusNegative = 0x0008;
    static constexpr std::uint16_t kStatusExtend = 0x0010;
    static constexpr std::uint16_t kStatusInterruptMask = 0x0700;
    static constexpr std::uint16_t kStatusSupervisor = 0x2000;
    static constexpr std::uint16_t kStatusTrace = 0x8000;
    // Bits that physically exist in SR. Everything else reads back zero, so any
    // write to SR has to be masked with this.
    static constexpr std::uint16_t kStatusWritableMask = 0xA71F;

private:
    // Operand widths, kept as the encoded size field where possible.
    enum class Size { byte, word, longword };

    [[nodiscard]] bool ready() const noexcept;

    // --- bus ---
    [[nodiscard]] std::uint8_t read8(std::uint32_t address) noexcept;
    [[nodiscard]] std::uint16_t read16(std::uint32_t address) noexcept;
    [[nodiscard]] std::uint32_t read32(std::uint32_t address) noexcept;
    void write8(std::uint32_t address, std::uint8_t value) noexcept;
    void write16(std::uint32_t address, std::uint16_t value) noexcept;
    void write32(std::uint32_t address, std::uint32_t value) noexcept;
    [[nodiscard]] std::uint16_t fetch16() noexcept;
    [[nodiscard]] std::uint32_t fetch32() noexcept;

    // --- effective address ---
    // Resolving an EA can consume extension words, so it must happen exactly
    // once per operand and in the order the hardware fetches them.
    struct Operand {
        bool is_register = false;
        std::uint32_t* reg = nullptr;   // when is_register
        std::uint32_t address = 0;      // when memory
        bool is_address_register = false;
        // Set for a LONG write through -(An). The 68000 does not decrement by
        // four and then store: it decrements two, stores the LOW word, then
        // decrements two more and stores the HIGH word. The difference is only
        // visible when the address is odd - the first store faults and An is
        // left two back, not four - but games that unwind stacks care.
        std::uint32_t* split_predecrement = nullptr;
    };
    // writing matters for the pre/post-increment modes: when the resulting
    // address is odd and the access is a word or long, a WRITE never performs
    // the register update (the bus cycle is abandoned first), while a READ
    // does. Getting this backwards moves address registers by 2 either way.
    [[nodiscard]] Operand resolve(std::uint8_t mode, std::uint8_t reg, Size size, bool& fault,
                                  bool writing = false) noexcept;
    [[nodiscard]] std::uint32_t read_operand(const Operand& operand, Size size) noexcept;
    void write_operand(const Operand& operand, Size size, std::uint32_t value) noexcept;

    // --- flags ---
    void set_logic_flags(std::uint32_t result, Size size) noexcept;
    // Arithmetic sets all five flags including X, and V/C differ between add
    // and subtract, so they get their own helpers rather than a shared one with
    // a "subtract" bool - that shape is where sign errors hide.
    void set_add_flags(std::uint32_t a, std::uint32_t b, std::uint32_t result, Size size) noexcept;
    void set_sub_flags(std::uint32_t a, std::uint32_t b, std::uint32_t result, Size size) noexcept;
    // CMP is a subtract that leaves X alone.
    void set_compare_flags(std::uint32_t a, std::uint32_t b, std::uint32_t result, Size size) noexcept;
    void set_flag(std::uint16_t mask, bool on) noexcept;
    [[nodiscard]] bool flag(std::uint16_t mask) const noexcept { return (registers_.sr & mask) != 0; }
    [[nodiscard]] bool condition(std::uint8_t code) const noexcept;

    // --- exceptions ---
    void raise(M68kException vector) noexcept;
    void enter_exception(std::uint8_t vector, std::uint32_t stacked_pc) noexcept;
    void set_supervisor(bool on) noexcept;
    void service_interrupt(int level) noexcept;

    static constexpr std::uint32_t kAddressMask = 0x00FFFFFF;
    // Instruction timing. Base cost plus effective-address time; see the table
    // in the .cpp. Cycle counts are not cosmetic - they set the ratio between
    // CPU work and video position, which is what raster effects depend on.
    [[nodiscard]] static std::uint32_t ea_cycles(std::uint8_t mode, std::uint8_t reg, Size size) noexcept;
    [[nodiscard]] static std::uint32_t jump_ea_cycles(std::uint8_t mode, std::uint8_t reg) noexcept;
    [[nodiscard]] static std::uint32_t control_ea_cycles(std::uint8_t mode, std::uint8_t reg) noexcept;
    [[nodiscard]] static std::uint32_t move_dest_cycles(std::uint8_t mode, std::uint8_t reg, Size size) noexcept;
    [[nodiscard]] static bool ea_is_immediate_or_register(std::uint8_t mode, std::uint8_t reg) noexcept;

    [[nodiscard]] static std::uint32_t mask_of(Size size) noexcept;
    [[nodiscard]] static std::uint32_t sign_bit_of(Size size) noexcept;
    [[nodiscard]] static std::uint32_t sign_extend(std::uint32_t value, Size size) noexcept;

    // A word or long access to an odd address is an ADDRESS ERROR on the
    // 68000, not an unaligned access. It aborts the instruction part-way and
    // takes vector 3 with a seven-word group-0 frame. Detection has to happen
    // inside the bus helpers, so it is recorded here and turned into the
    // exception once the instruction body unwinds.
    void note_address_error(std::uint32_t address, bool write, bool program = false) noexcept;
    void take_address_error() noexcept;
    // Every branch, jump and return goes through this: landing on an odd
    // address faults as part of the transferring instruction, not on the next
    // fetch, and the frame has to record the odd target.
    void set_program_counter(std::uint32_t value) noexcept;

  public:
    // Everything a save state has to carry, listed once. bus_ is deliberately
    // absent: it is wiring set up by whoever owns this CPU, not state, and
    // restoring it from a file would point the callbacks at a dead object.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(registers_);
        ar(halted_);
        ar(stopped_);
        ar(pending_interrupt_level_);
        ar(cycles_this_step_);
        ar(ea_setup_cycles_);
        ar(last_exception_pc_);
        ar(last_exception_vector_);
        ar(address_error_);
        ar(address_error_address_);
        ar(address_error_write_);
        ar(address_error_program_);
        ar(address_error_pc_valid_);
        ar(address_error_pc_);
        ar(current_opcode_);
        ar(instruction_pc_);
    }

  private:
    M68kBusCallbacks bus_{};
    M68kRegisters registers_{};
    bool halted_ = false;
    bool stopped_ = false;
    int pending_interrupt_level_ = 0;
    std::uint32_t cycles_this_step_ = 0;
    // Address-calculation cycles already spent when an access faults.
    std::uint32_t ea_setup_cycles_ = 0;
    std::array<std::uint64_t, 64> exception_tally_{};
    std::uint32_t last_exception_pc_ = 0;
    std::uint8_t last_exception_vector_ = 0;
    bool address_error_ = false;
    std::uint32_t address_error_address_ = 0;
    bool address_error_write_ = false;
    bool address_error_program_ = false;
    // Frame PC differs by fault kind: a data fault records (pc - 2), a jump to
    // an odd target records (target - 4). Captured when the fault is noted.
    bool address_error_pc_valid_ = false;
    std::uint32_t address_error_pc_ = 0;
    std::uint16_t current_opcode_ = 0;
    std::uint32_t instruction_pc_ = 0;
};

}  // namespace mercury::core
