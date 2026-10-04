#include "mercury/gamegear/gear_link_session.hpp"

#include <cstdlib>

namespace mercury::gamegear {

namespace {

// A frame is about 60,000 Z80 cycles, so a couple of hundred thousand
// instructions is far more than a healthy machine needs and still bounded if
// one of them stops producing frames.
constexpr int kInstructionCeiling = 4'000'000;

// Past this the two clocks are not merely interleaved badly, something reset a
// machine or loaded a state into one side. Start counting again rather than
// letting one console sit frozen while the other catches up.
constexpr std::int64_t kRebaseThreshold = 1'000'000;

}  // namespace

// The default lives in the header (`startup_offset_ = 3'001`).
//
// Two Game Gears are never switched on in the same instant. Emulating them
// from a shared reset makes them agree about everything for ever, and a game
// that decides who is player one by racing cannot decide it from a perfect
// tie: Mortal Kombat's two consoles each announce themselves, each see the
// other announce at the same moment, and both sit on WAIT for ever. Measured
// directly - every read and write mirrored one for one, indefinitely.
//
// Any offset at all settles the race; the default is about a twentieth of a
// frame, far smaller than the gap between two people pressing two power
// switches and large enough that no polling loop can straddle it.
void GearLinkSession::set_startup_offset(std::int64_t cycles) noexcept {
    startup_offset_ = cycles;
    if (connected_) {
        rebase();
    }
}

void GearLinkSession::connect(GameGear& first, GameGear& second) noexcept {
    machines_[0] = &first;
    machines_[1] = &second;
    cable_.reset();
    cable_.set_connected(true);
    // Side 0 and side 1 are opposite ends of the crossover. Both consoles on
    // the same side would each hear only themselves.
    first.mutable_system_control().attach_link(&cable_, 0);
    second.mutable_system_control().attach_link(&cable_, 1);
    connected_ = true;
    rebase();
}

void GearLinkSession::disconnect() noexcept {
    for (auto* machine : machines_) {
        if (machine != nullptr) {
            machine->mutable_system_control().detach_link();
        }
    }
    machines_[0] = nullptr;
    machines_[1] = nullptr;
    cable_.set_connected(false);
    cable_.reset();
    connected_ = false;
}

std::int64_t GearLinkSession::clock(int index) const noexcept {
    const auto* machine = machines_[index];
    if (machine == nullptr) {
        return 0;
    }
    return static_cast<std::int64_t>(machine->system_control().serial_cycle()) - base_[index];
}

void GearLinkSession::rebase() noexcept {
    for (int index = 0; index < kMachineCount; ++index) {
        const auto* machine = machines_[index];
        base_[index] =
            machine == nullptr ? 0 : static_cast<std::int64_t>(machine->system_control().serial_cycle());
    }
    // Console one is treated as having been switched on first. Done by moving
    // its partner's baseline rather than by running one machine outside the
    // interleave, so the pair stays deterministic - and applied here rather
    // than in connect() so that a rebase after a state load does not quietly
    // put the two consoles back in step.
    base_[1] -= startup_offset_;
}

std::int64_t GearLinkSession::skew() const noexcept {
    return clock(0) - clock(1);
}

namespace {
constexpr std::uint8_t kPairTag[4] = {'G', 'L', 'N', 'K'};
}  // namespace

bool GearLinkSession::is_pair_state(const std::vector<std::uint8_t>& blob) noexcept {
    return blob.size() >= 4 && blob[0] == kPairTag[0] && blob[1] == kPairTag[1] &&
           blob[2] == kPairTag[2] && blob[3] == kPairTag[3];
}

std::vector<std::uint8_t> GearLinkSession::save_pair_state() const {
    std::vector<std::uint8_t> blob(kPairTag, kPairTag + 4);
    const auto put = [&blob](std::uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            blob.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        }
    };
    for (const auto* machine : machines_) {
        const auto one = machine != nullptr ? machine->save_state_blob() : std::vector<std::uint8_t>{};
        put(one.size(), 4);
        blob.insert(blob.end(), one.begin(), one.end());
    }
    put(static_cast<std::uint64_t>(base_[0]), 8);
    put(static_cast<std::uint64_t>(base_[1]), 8);
    return blob;
}

bool GearLinkSession::load_pair_state(const std::vector<std::uint8_t>& blob, std::string& error) {
    if (!is_pair_state(blob)) {
        error = "not a Gear Link pair";
        return false;
    }
    if (machines_[0] == nullptr || machines_[1] == nullptr) {
        error = "no Gear Link is open to load a pair into";
        return false;
    }
    std::size_t at = 4;
    const auto take = [&blob, &at](int bytes, std::uint64_t& value) {
        if (at + static_cast<std::size_t>(bytes) > blob.size()) {
            return false;
        }
        value = 0;
        for (int i = 0; i < bytes; ++i) {
            value |= static_cast<std::uint64_t>(blob[at + static_cast<std::size_t>(i)]) << (i * 8);
        }
        at += static_cast<std::size_t>(bytes);
        return true;
    };
    std::vector<std::uint8_t> parts[kMachineCount];
    for (auto& part : parts) {
        std::uint64_t length = 0;
        if (!take(4, length) || at + length > blob.size()) {
            error = "the Gear Link pair is cut short";
            return false;
        }
        part.assign(blob.begin() + static_cast<std::ptrdiff_t>(at),
                    blob.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += static_cast<std::size_t>(length);
    }
    std::uint64_t first_base = 0;
    std::uint64_t second_base = 0;
    if (!take(8, first_base) || !take(8, second_base)) {
        error = "the Gear Link pair is cut short";
        return false;
    }
    // Both consoles before the session, so the republish below reads the
    // pins the consoles now hold rather than the ones they held before.
    if (!machines_[0]->load_state_blob(parts[0], error) ||
        !machines_[1]->load_state_blob(parts[1], error)) {
        return false;
    }
    restore_after_load(static_cast<std::int64_t>(first_base),
                       static_cast<std::int64_t>(second_base));
    return true;
}

void GearLinkSession::restore_after_load(std::int64_t first_baseline,
                                         std::int64_t second_baseline) noexcept {
    base_[0] = first_baseline;
    base_[1] = second_baseline;
    // Both ends of the cable from the restored consoles, not from whatever
    // this machine's cable held a moment ago.
    for (auto* machine : machines_) {
        if (machine != nullptr) {
            machine->mutable_system_control().republish_link();
        }
    }
}

void GearLinkSession::step_behind() noexcept {
    if (!connected_) {
        return;
    }
    const auto difference = skew();
    if (difference > kRebaseThreshold || difference < -kRebaseThreshold) {
        rebase();
        if (step_observer_ != nullptr) {
            step_observer_(step_observer_user_, 0);
        }
        machines_[0]->step();
        return;
    }
    // Ties go to machine 0, arbitrarily but consistently: the pair has to be
    // deterministic or a linked session would play differently every run.
    const int index = difference <= 0 ? 0 : 1;
    if (step_observer_ != nullptr) {
        step_observer_(step_observer_user_, index);
    }
    machines_[index]->step();
}

bool GearLinkSession::run_frame() noexcept {
    if (!connected_) {
        return false;
    }

    const auto first_entry = machines_[0]->vdp().frames_elapsed();
    const auto second_entry = machines_[1]->vdp().frames_elapsed();

    for (int guard = 0; guard < kInstructionCeiling; ++guard) {
        const bool first_done = machines_[0]->vdp().frames_elapsed() != first_entry;
        const bool second_done = machines_[1]->vdp().frames_elapsed() != second_entry;
        if (first_done && second_done) {
            return true;
        }
        // Always the one that is behind, including after it has finished its
        // frame. Freezing a console at its frame boundary while the other ran
        // on looked harmless - they are the same hardware at the same clock, so
        // how far apart could they get? - but the two consoles do not start in
        // the same place, and the gap is the whole head start. Mortal Kombat
        // spins 255 times waiting for an answer; with its partner held still
        // for that entire window the answer cannot come, and the game says
        // LINK ERROR. A console that reaches the end of its frame first simply
        // carries on into the next one, which is what hardware does.
        step_behind();
    }
    return false;
}

}  // namespace mercury::gamegear
