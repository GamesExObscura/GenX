#include "mercury/gamegear/input_state.hpp"

#include <sstream>

namespace mercury::gamegear {

namespace {

constexpr std::uint8_t active_low(bool pressed, std::uint8_t mask) noexcept {
    return pressed ? 0x00 : mask;
}

}  // namespace

void InputState::reset() noexcept {
    up_ = false;
    down_ = false;
    left_ = false;
    right_ = false;
    button1_ = false;
    button2_ = false;
    start_ = false;
    ext_up_ = false;
    ext_down_ = false;
    ext_left_ = false;
    ext_right_ = false;
    ext_button1_ = false;
    ext_button2_ = false;
    ext_th_ = false;
}

void InputState::set_button(Button button, bool pressed) noexcept {
    switch (button) {
        case Button::up:
            up_ = pressed;
            break;
        case Button::down:
            down_ = pressed;
            break;
        case Button::left:
            left_ = pressed;
            break;
        case Button::right:
            right_ = pressed;
            break;
        case Button::button1:
            button1_ = pressed;
            break;
        case Button::button2:
            button2_ = pressed;
            break;
        case Button::start:
            start_ = pressed;
            break;
        case Button::ext_up:
            ext_up_ = pressed;
            break;
        case Button::ext_down:
            ext_down_ = pressed;
            break;
        case Button::ext_left:
            ext_left_ = pressed;
            break;
        case Button::ext_right:
            ext_right_ = pressed;
            break;
        case Button::ext_button1:
            ext_button1_ = pressed;
            break;
        case Button::ext_button2:
            ext_button2_ = pressed;
            break;
        case Button::ext_th:
            ext_th_ = pressed;
            break;
    }
}

bool InputState::is_pressed(Button button) const noexcept {
    switch (button) {
        case Button::up:
            return up_;
        case Button::down:
            return down_;
        case Button::left:
            return left_;
        case Button::right:
            return right_;
        case Button::button1:
            return button1_;
        case Button::button2:
            return button2_;
        case Button::start:
            return start_;
        case Button::ext_up:
            return ext_up_;
        case Button::ext_down:
            return ext_down_;
        case Button::ext_left:
            return ext_left_;
        case Button::ext_right:
            return ext_right_;
        case Button::ext_button1:
            return ext_button1_;
        case Button::ext_button2:
            return ext_button2_;
        case Button::ext_th:
            return ext_th_;
    }

    return false;
}

std::uint8_t InputState::read_port_dc() const noexcept {
    std::uint8_t value = 0x00;
    value |= active_low(up_, 0x01);
    value |= active_low(down_, 0x02);
    value |= active_low(left_, 0x04);
    value |= active_low(right_, 0x08);
    value |= active_low(button1_, 0x10);
    value |= active_low(button2_, 0x20);
    value |= active_low(ext_up_, 0x40);
    value |= active_low(ext_down_, 0x80);
    return value;
}

std::uint8_t InputState::read_port_dd() const noexcept {
    std::uint8_t value = 0x70;
    value |= active_low(ext_left_, 0x01);
    value |= active_low(ext_right_, 0x02);
    value |= active_low(ext_button1_, 0x04);
    value |= active_low(ext_button2_, 0x08);
    value |= active_low(ext_th_, 0x80);
    return value;
}

std::string InputState::describe() const {
    std::ostringstream out;
    out << "Input: U=" << up_
        << " D=" << down_
        << " L=" << left_
        << " R=" << right_
        << " B1=" << button1_
        << " B2=" << button2_
        << " ST=" << start_;
    return out.str();
}

}  // namespace mercury::gamegear
