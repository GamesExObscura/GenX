#pragma once

#include <filesystem>
#include <string>

#include "mercury/core/module_status.hpp"
#include "mercury/core/rom_image.hpp"
#include "mercury/core/z80_cpu.hpp"
#include "mercury/gamegear/bus.hpp"
#include "mercury/gamegear/cartridge.hpp"
#include "mercury/gamegear/input_state.hpp"
#include "mercury/gamegear/memory_map.hpp"
#include "mercury/gamegear/psg.hpp"
#include "mercury/gamegear/system_control.hpp"
#include "mercury/gamegear/vdp.hpp"

namespace mercury::gamegear {

class GameGear {
public:
    GameGear();

    [[nodiscard]] mercury::core::RomLoadResult load_rom(const std::filesystem::path& path);
    void reset() noexcept;
    [[nodiscard]] mercury::core::Z80StepResult step() noexcept;

    [[nodiscard]] const GameGearBus& bus() const noexcept;
    // Game Genie codes, watched on the bus - see GameGearBus::attach_cheats.
    // The list belongs to the host, which is where codes are typed and kept.
    void attach_cheats(const mercury::core::CheatMap* cheats) noexcept {
        bus_.attach_cheats(cheats);
    }
    [[nodiscard]] const Cartridge& cartridge() const noexcept;
    // Non-const access, for the host to hand a battery file back to the cart.
    [[nodiscard]] Cartridge& mutable_cartridge() noexcept { return cartridge_; }
    [[nodiscard]] mercury::core::Z80Cpu& cpu() noexcept;
    [[nodiscard]] const mercury::core::Z80Cpu& cpu() const noexcept;
    [[nodiscard]] InputState& input_state() noexcept;
    [[nodiscard]] const InputState& input_state() const noexcept;
    [[nodiscard]] const MemoryMap& memory_map() const noexcept;
    [[nodiscard]] Psg& psg() noexcept;
    [[nodiscard]] const Psg& psg() const noexcept;
    [[nodiscard]] const SystemControl& system_control() const noexcept;
    // Non-const access, for the host to plug a Gear-to-Gear cable in.
    [[nodiscard]] SystemControl& mutable_system_control() noexcept { return system_control_; }
    [[nodiscard]] const Vdp& vdp() const noexcept;

    // Whether this cartridge needs the link port's transmit holding register.
    // See the definition: exactly one game does, and exactly one game breaks
    // if it is on for everybody.
    [[nodiscard]] static bool wants_tx_holding_register(
        const std::vector<std::uint8_t>& rom) noexcept;
    [[nodiscard]] mercury::core::Z80BusCallbacks make_z80_callbacks() noexcept;

    [[nodiscard]] mercury::core::ModuleStatuses module_statuses() const;
    [[nodiscard]] std::string describe() const;

private:
    void initialize_runtime() noexcept;
    void synchronize_interrupts() noexcept;

  public:
    // 2: the PSG moved from phase accumulators to hardware counters, changing the
    // shape of its channel structs.
    // 3: the Gear-to-Gear serial port became real hardware, so system control now
    // carries a transmit/receive state machine.
    // 4: the baseball cartridges' serial EEPROM, appended at the very end so a
    // version 3 file is laid out exactly as before and still loads.
    static constexpr std::uint32_t kStateVersion = 4;

    template <typename Archive>
    void visit_state(Archive& ar, std::uint32_t version = kStateVersion) {
        cpu_.visit_state(ar);
        vdp_.visit_state(ar);
        psg_.visit_state(ar);
        bus_.visit_state(ar);
        cartridge_.visit_state(ar);
        system_control_.visit_state(ar);
        ar(pause_button_was_pressed_);
        if (version >= 4) {
            cartridge_.visit_eeprom_state(ar);
        } else if constexpr (Archive::loading()) {
            cartridge_.reset_eeprom_logic();
        }
    }

    // Same contract as the Mega Drive side: a header identifies the cartridge
    // so a state can be refused rather than loaded into the wrong game.
    [[nodiscard]] std::vector<std::uint8_t> save_state_blob() const;
    [[nodiscard]] bool load_state_blob(const std::vector<std::uint8_t>& blob, std::string& error);

  private:
    GameGearBus bus_;
    Cartridge cartridge_;
    mercury::core::Z80Cpu cpu_;
    MemoryMap memory_map_;
    Psg psg_;
    SystemControl system_control_;
    Vdp vdp_;
    // Previous state of the SMS pause line, so the NMI fires on the press
    // rather than continuously while the button is held.
    bool pause_button_was_pressed_ = false;
};

}  // namespace mercury::gamegear
