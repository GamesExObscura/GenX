#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mercury/gamegear/game_gear.hpp"
#include "mercury/gamegear/gear_link.hpp"

namespace mercury::gamegear {

// Two Game Gears joined by a link cable.
//
// The hard part of a link cable is not the wire, it is time. On real hardware
// both consoles run at once and a bit put on the wire is seen by the other
// console the instant it changes. Two emulated consoles take turns, so if one
// is allowed to run a whole frame ahead its transmissions arrive in the other
// one's past.
//
// This runs them turn about, always stepping whichever console is behind. That
// keeps them within a single instruction of each other - twenty-odd cycles
// against 746 cycles per serial bit, under three percent of a bit period - and
// the receiver samples the middle of each bit, so no realistic disagreement
// about timing can flip one.
//
// Deliberately non-owning: the windows own their machines and this only says
// when each of them may run.
class GearLinkSession {
public:
    static constexpr int kMachineCount = 2;

    void connect(GameGear& first, GameGear& second) noexcept;
    void disconnect() noexcept;
    [[nodiscard]] bool connected() const noexcept { return connected_; }

    // How far ahead console one starts, in Z80 cycles. Two real Game Gears are
    // never switched on in the same instant, and a game that picks a leader by
    // racing cannot pick one from a perfect tie. Settable so a probe can put
    // the pair back into exact lockstep and find out whether a fault is caused
    // by the offset or merely uncovered by it.
    void set_startup_offset(std::int64_t cycles) noexcept;
    [[nodiscard]] std::int64_t startup_offset() const noexcept { return startup_offset_; }

    // Told which console is about to execute an instruction, just before it
    // does. For a probe that wants to rank executed addresses across a linked
    // pair: the interleaving rule lives here, so a tool driving the pair by
    // hand would have to duplicate it and could get it subtly wrong.
    using StepObserver = void (*)(void* user, int machine_index);
    void set_step_observer(StepObserver observer, void* user) noexcept {
        step_observer_ = observer;
        step_observer_user_ = user;
    }

    [[nodiscard]] GearLinkCable& cable() noexcept { return cable_; }
    [[nodiscard]] const GearLinkCable& cable() const noexcept { return cable_; }

    // Run both consoles forward until each has finished a frame. Returns false
    // if a machine failed to complete one inside the instruction ceiling,
    // which means it is wedged rather than slow.
    bool run_frame() noexcept;

    // One instruction on whichever console is behind. The building block
    // run_frame uses, exposed for tests that want finer control.
    void step_behind() noexcept;

    // How far apart the two clocks are right now, in Z80 cycles. Should stay
    // inside the length of one instruction; a large number means something
    // stepped a machine outside this session.
    [[nodiscard]] std::int64_t skew() const noexcept;

    // WHAT A PAIR HAS THAT NEITHER CONSOLE DOES. Each console's clock is
    // measured from a baseline this session holds, and those two numbers
    // decide which console steps next - so two machines holding the same two
    // consoles but different baselines interleave differently and part. A
    // pair sent across a network has to carry them, and put both consoles'
    // pins back on the cable, which is not in either console's state either.
    [[nodiscard]] std::int64_t baseline(int index) const noexcept {
        return index >= 0 && index < kMachineCount ? base_[index] : 0;
    }
    void restore_after_load(std::int64_t first_baseline, std::int64_t second_baseline) noexcept;

    // THE WHOLE PAIR AS ONE BLOB - both consoles and the two baselines - in
    // the form netplay sends to start a linked session. Here rather than in
    // the shell so the tool that tests it over a real socket runs the same
    // code the shell does, not a copy of it.
    //
    // Layout: "GLNK", then each console as a 32-bit little-endian length and
    // its saved state, then the two baselines as 64-bit little-endian.
    [[nodiscard]] std::vector<std::uint8_t> save_pair_state() const;
    [[nodiscard]] bool load_pair_state(const std::vector<std::uint8_t>& blob, std::string& error);
    [[nodiscard]] static bool is_pair_state(const std::vector<std::uint8_t>& blob) noexcept;

private:
    [[nodiscard]] std::int64_t clock(int index) const noexcept;
    void rebase() noexcept;

    GearLinkCable cable_{};
    GameGear* machines_[kMachineCount]{nullptr, nullptr};
    std::int64_t base_[kMachineCount]{0, 0};
    std::int64_t startup_offset_ = 3'001;
    StepObserver step_observer_ = nullptr;
    void* step_observer_user_ = nullptr;
    bool connected_ = false;
};

}  // namespace mercury::gamegear
