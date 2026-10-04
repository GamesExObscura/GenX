#pragma once

#include <cstdint>
#include <string>

namespace mercury::gamegear {

enum class Button {
    up,
    down,
    left,
    right,
    button1,
    button2,
    start,
    ext_up,
    ext_down,
    ext_left,
    ext_right,
    ext_button1,
    ext_button2,
    ext_th,
};

class InputState {
public:
    void reset() noexcept;
    void set_button(Button button, bool pressed) noexcept;

    [[nodiscard]] bool is_pressed(Button button) const noexcept;
    [[nodiscard]] std::uint8_t read_port_dc() const noexcept;
    [[nodiscard]] std::uint8_t read_port_dd() const noexcept;
    [[nodiscard]] std::string describe() const;

private:
    bool up_ = false;
    bool down_ = false;
    bool left_ = false;
    bool right_ = false;
    bool button1_ = false;
    bool button2_ = false;
    bool start_ = false;
    bool ext_up_ = false;
    bool ext_down_ = false;
    bool ext_left_ = false;
    bool ext_right_ = false;
    bool ext_button1_ = false;
    bool ext_button2_ = false;
    bool ext_th_ = false;
};

}  // namespace mercury::gamegear
