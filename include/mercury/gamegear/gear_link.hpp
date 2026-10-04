#pragma once

#include <cstddef>
#include <cstdint>

namespace mercury::gamegear {

// The Gear-to-Gear cable.
//
// Seven signal pins (PC0-PC6) run between the two consoles, and every one of
// them is open drain: a console can pull a pin low or let it float, and a
// resistor pulls it high when nobody is pulling. So a pin reads high only when
// NEITHER end is pulling it down, which is what lets both consoles share a wire
// without one of them driving current into the other.
//
// The cable is crossed over. Each console's transmit pin lands on the other
// one's receive pin, and the same swap happens for the two parallel handshake
// lines - otherwise both ends would be shouting into the same wire and neither
// would hear anything. Pin 6, the interrupt line, is common to both.
//
//   local bit : 0  1  2  3  4(TX)  5(RX)  6(/NINT)
//   arrives as: 2  3  0  1  5      4      6
//
// Verified against Gearsystem, the one emulator with a working implementation.
class GearLinkCable {
public:
    static constexpr int kSideCount = 2;

    // One console's contribution to the wire: which pins it is currently
    // pulling, and the level it is presenting on each.
    struct WireState {
        std::uint8_t drive_mask = 0x00;
        std::uint8_t levels = 0x7F;
    };

    void reset() noexcept {
        for (auto& side : sides_) {
            side = WireState{};
        }
        for (auto& revision : revisions_) {
            ++revision;
        }
    }

    void set_connected(bool connected) noexcept {
        connected_ = connected;
        for (auto& revision : revisions_) {
            ++revision;
        }
    }
    [[nodiscard]] bool connected() const noexcept { return connected_; }

    void publish(int side, const WireState& state) noexcept {
        if (side < 0 || side >= kSideCount) {
            return;
        }
        auto& stored = sides_[static_cast<std::size_t>(side)];
        const std::uint8_t drive_mask = state.drive_mask & 0x7F;
        const std::uint8_t levels = state.levels & 0x7F;
        if (stored.drive_mask == drive_mask && stored.levels == levels) {
            return;
        }
        stored.drive_mask = drive_mask;
        stored.levels = levels;
        ++revisions_[static_cast<std::size_t>(side)];
    }

    // Bumped whenever this side's pins move. The other console checks it once
    // per instruction, so it has to be cheaper than re-deriving the wire.
    [[nodiscard]] std::uint32_t revision(int side) const noexcept {
        if (side < 0 || side >= kSideCount) {
            return 0;
        }
        return revisions_[static_cast<std::size_t>(side)];
    }

    // What the OTHER console is putting on this console's pins, already
    // translated through the crossover. A disconnected cable presents nothing,
    // which leaves every pin pulled high - exactly what an unplugged Game Gear
    // sees.
    [[nodiscard]] WireState remote_for(int side) const noexcept {
        WireState state;
        if (!connected_ || side < 0 || side >= kSideCount) {
            return state;
        }
        const auto& other = sides_[static_cast<std::size_t>(side ^ 1)];
        state.drive_mask = swap_pins(other.drive_mask);
        state.levels = swap_pins(other.levels);
        return state;
    }

    // Both ends pulling the same pin to different levels. Harmless on real
    // hardware (open drain, low always wins) but worth being able to see: it
    // means the two sides disagree about who is talking.
    [[nodiscard]] std::uint8_t contention_mask() const noexcept {
        if (!connected_) {
            return 0;
        }
        const auto remote = remote_for(0);
        const std::uint8_t both = static_cast<std::uint8_t>(sides_[0].drive_mask & remote.drive_mask);
        const std::uint8_t differ = static_cast<std::uint8_t>(sides_[0].levels ^ remote.levels);
        return static_cast<std::uint8_t>(both & differ & 0x7F);
    }

private:
    static constexpr std::uint8_t swap_pins(std::uint8_t value) noexcept {
        constexpr int kMap[7] = {2, 3, 0, 1, 5, 4, 6};
        std::uint8_t mapped = 0;
        for (int bit = 0; bit < 7; ++bit) {
            if ((value & (1u << bit)) != 0) {
                mapped |= static_cast<std::uint8_t>(1u << kMap[bit]);
            }
        }
        return mapped;
    }

    WireState sides_[kSideCount]{};
    // Starts at 1, never returns to 0: a console tracking it resets its own
    // copy to 0, so the first look after plugging in always counts as a change.
    std::uint32_t revisions_[kSideCount]{1, 1};
    bool connected_ = false;
};

}  // namespace mercury::gamegear
