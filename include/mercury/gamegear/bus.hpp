#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "mercury/core/game_genie.hpp"
#include "mercury/core/z80_cpu.hpp"

namespace mercury::gamegear {

class Cartridge;
class Psg;
class SystemControl;
class Vdp;

class GameGearBus {
public:
    static constexpr std::size_t kWorkRamSize = 8 * 1024;

    void attach(Cartridge& cartridge, SystemControl& system_control, Vdp& vdp, Psg& psg) noexcept;
    void reset() noexcept;

    [[nodiscard]] std::uint8_t read8(std::uint16_t address) const noexcept;
    void write8(std::uint16_t address, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t in(std::uint8_t port) noexcept;
    void out(std::uint8_t port, std::uint8_t value) noexcept;
    [[nodiscard]] mercury::core::Z80BusCallbacks make_z80_callbacks() noexcept;
    [[nodiscard]] std::string describe() const;

    // Game Genie codes in force, or none.
    //
    // A Master System or Game Gear code names a PROCESSOR address, not a ROM
    // offset, so it cannot be patched into the image the way a Mega Drive one
    // is: a bank switch would move the game out from under it. The device
    // watches the bus instead, and so does this. Owned by the caller, and
    // null while there are no codes - which is the test the read path makes
    // before anything else.
    void attach_cheats(const mercury::core::CheatMap* cheats) noexcept { cheats_ = cheats; }

private:
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::uint8_t read_work_ram(std::uint16_t address) const noexcept;
    void write_work_ram(std::uint16_t address, std::uint8_t value) noexcept;
    [[nodiscard]] std::uint32_t io_wait_cycles(std::uint8_t port, bool is_write) const noexcept;

    Cartridge* cartridge_ = nullptr;
    SystemControl* system_control_ = nullptr;
    Vdp* vdp_ = nullptr;
    Psg* psg_ = nullptr;
    const mercury::core::CheatMap* cheats_ = nullptr;
  public:
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(work_ram_);
    }

  private:
    std::array<std::uint8_t, kWorkRamSize> work_ram_{};
};

}  // namespace mercury::gamegear
