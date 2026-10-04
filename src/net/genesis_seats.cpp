#include "mercury/net/genesis_seats.hpp"

namespace mercury::net {

namespace {

using Kind = genesis::ControllerPort::Kind;
using genesis::Button;

constexpr int kPadButtons = static_cast<int>(Button::count);

bool is_gun(Kind kind) noexcept { return kind == Kind::justifier || kind == Kind::menacer; }

bool held(const PlayerInput& input, int bit) noexcept {
    return (input.buttons & (1u << bit)) != 0;
}

}  // namespace

void apply_genesis_seats(genesis::Genesis& machine, const std::array<Kind, 2>& seats,
                         const std::array<PlayerInput, 2>& inputs) noexcept {
    const bool two_guns = seats[0] == Kind::justifier && seats[1] == Kind::justifier;
    std::array<std::array<bool, kPadButtons>, 2> pins{};

    for (int seat = 0; seat < 2; ++seat) {
        const auto& input = inputs[static_cast<std::size_t>(seat)];
        const bool trigger = held(input, PlayerInput::kTriggerBit) ||
                             held(input, PlayerInput::kReloadBit);
        if (two_guns && seat == 1) {
            // The second Justifier: same port, its own registers.
            auto& port = machine.port2();
            port.set_gun2_aim(input.gun_x, input.gun_y, input.on_screen);
            port.set_gun2_trigger(trigger || held(input, static_cast<int>(Button::a)));
            port.set_gun2_start(held(input, static_cast<int>(Button::start)));
            continue;
        }
        // port_for_player's rule: a gun is in port 2, and so is seat 2.
        const int port = (is_gun(seats[static_cast<std::size_t>(seat)]) || seat == 1) ? 1 : 0;
        auto& lines = pins[static_cast<std::size_t>(port)];
        for (int bit = 0; bit < kPadButtons; ++bit) {
            lines[static_cast<std::size_t>(bit)] =
                lines[static_cast<std::size_t>(bit)] || held(input, bit);
        }
        if (is_gun(seats[static_cast<std::size_t>(seat)])) {
            lines[static_cast<std::size_t>(Button::a)] =
                lines[static_cast<std::size_t>(Button::a)] || trigger;
            (port == 0 ? machine.port1() : machine.port2())
                .set_aim(input.gun_x, input.gun_y, input.on_screen);
        }
    }

    for (int port = 0; port < 2; ++port) {
        auto& target = port == 0 ? machine.port1() : machine.port2();
        for (int bit = 0; bit < kPadButtons; ++bit) {
            target.set_button(static_cast<Button>(bit),
                              pins[static_cast<std::size_t>(port)][static_cast<std::size_t>(bit)]);
        }
    }
}

}  // namespace mercury::net
