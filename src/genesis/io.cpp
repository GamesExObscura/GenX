#include "mercury/genesis/io.hpp"

#include <sstream>

namespace mercury::genesis {

namespace {

constexpr std::size_t index_of(Button button) noexcept {
    return static_cast<std::size_t>(button);
}

}  // namespace

void ControllerPort::reset() noexcept {
    pressed_.fill(false);
    data_ = 0x00;
    control_ = 0x00;
    th_high_ = true;
    th_edges_ = 0;
    idle_frames_ = 0;
    piano_.reset();
}

void ControllerPort::set_button(Button button, bool pressed) noexcept {
    if (button == Button::count) {
        return;
    }
    pressed_[index_of(button)] = pressed;
}

bool ControllerPort::is_pressed(Button button) const noexcept {
    if (button == Button::count) {
        return false;
    }
    return pressed_[index_of(button)];
}

void ControllerPort::write_control(std::uint8_t value) noexcept {
    control_ = value;
    // Changing the direction bit changes what the pad sees on TH just as much
    // as changing the level does. A game that writes the data register FIRST
    // and only then makes TH an output was leaving the pin stuck high here,
    // because the level was only ever recomputed on a data write.
    ++control_writes_;
    update_th();
}

void ControllerPort::write_data(std::uint8_t value) noexcept {
    data_ = value;
    // The keyboard is bit-banged, so every single write to the data register
    // is a clock or data edge it has to see. A pad only cares about TH, which
    // is why nothing else here needs the write.
    if (kind_ == Kind::miracle_piano) {
        piano_.write_lines(data_, control_);
    }
    update_th();
}

// TH only means anything when the pin is configured as an output. A game that
// leaves it as an input sees a pad stuck in its TH-high bank.
void ControllerPort::update_th() noexcept {
    const bool th_is_output = (control_ & 0x40) != 0;
    const bool new_th = th_is_output ? ((data_ & 0x40) != 0) : true;
    if (new_th != th_high_) {
        // COUNT ON THE RISING EDGE, AND STOP AT FOUR.
        //
        // This counted the FALLING edge and wrapped modulo four, which put the
        // six-button pad's identifier one read early and then let the sequence
        // repeat for ever. The protocol is: the counter advances when TH goes
        // 0 -> 1, and the reads that matter are the TH-LOW ones - the third of
        // them (counter 2) returns zeros in the low nibble, which is the "I am
        // a six-button pad" answer, and the fourth (counter 3) returns ones.
        // Counting the other edge shifted both one read earlier, so a game
        // comparing successive reads saw a six-button pad answer where
        // hardware gives it plain directions.
        //
        // It also SATURATES at four rather than wrapping: after the sequence
        // the pad goes back to answering normally until the counter is reset,
        // and a wrap walked back into the identifier phases instead.
        // Matches ares's FightingPad, which increments on `!select && bit(6)`
        // and holds at 4, and the published protocol.
        if (new_th && th_edges_ < 4) {
            ++th_edges_;
        }
        th_high_ = new_th;
        idle_frames_ = 0;
    }
}

void ControllerPort::tick_timeout() noexcept {
    // ~1.5ms is under a tenth of a frame, so any frame in which the pad was not
    // strobed is long enough to reset the sequence.
    if (++idle_frames_ >= 1) {
        th_edges_ = 0;
    }
}

std::uint8_t ControllerPort::bank_value(std::uint32_t phase, bool th_high) const noexcept {
    // Buttons are active LOW; a set bit means released.
    const auto bit = [&](Button button, std::uint8_t position) -> std::uint8_t {
        return is_pressed(button) ? 0 : static_cast<std::uint8_t>(1u << position);
    };

    if (th_high) {
        if (kind_ == Kind::pad6 && phase == 3) {
            // Fourth high phase presents the extra buttons in place of the
            // direction pad: ?1CBMXYZ.
            std::uint8_t value = 0x40;
            value |= bit(Button::z, 0);
            value |= bit(Button::y, 1);
            value |= bit(Button::x, 2);
            value |= bit(Button::mode, 3);
            value |= bit(Button::b, 4);
            value |= bit(Button::c, 5);
            return value;
        }
        // ?1CBRLDU
        std::uint8_t value = 0x40;
        value |= bit(Button::up, 0);
        value |= bit(Button::down, 1);
        value |= bit(Button::left, 2);
        value |= bit(Button::right, 3);
        value |= bit(Button::b, 4);
        value |= bit(Button::c, 5);
        return value;
    }

    // TH low. Left and right read as zero here, which is how a game tells a
    // pad from an empty port.
    std::uint8_t value = 0x00;
    value |= bit(Button::a, 4);
    value |= bit(Button::start, 5);
    if (kind_ == Kind::pad6 && phase == 2) {
        // Third low phase drops up/down too - the signature that says "this is
        // a six-button pad, keep strobing".
        return value;
    }
    if (kind_ == Kind::pad6 && phase == 3) {
        // Fourth low phase returns all ones in the low nibble.
        return static_cast<std::uint8_t>(value | 0x0F);
    }
    value |= bit(Button::up, 0);
    value |= bit(Button::down, 1);
    return value;
}

// The Menacer is not a pad and does not use the TH multiplexing: it reports its
// four buttons on the low nibble in one fixed arrangement, with TH always high.
// The order is not the order a pad uses - trigger sits where a pad's B would be
// - which is taken from Genesis Plus GX's menacer_read, the only implementation
// available to copy from. Note the buttons are reported ACTIVE HIGH here, which
// is the opposite of a pad; that is what the reference does.
std::uint8_t ControllerPort::menacer_value() const noexcept {
    std::uint8_t value = 0x40;  // TH reads high
    if (is_pressed(Button::b)) value |= 0x01;
    if (is_pressed(Button::a)) value |= 0x02;  // the trigger
    if (is_pressed(Button::c)) value |= 0x04;
    if (is_pressed(Button::start)) value |= 0x08;
    return value;
}

// The Konami Justifier, which is a different device with a different protocol
// from the Menacer despite doing the same job. It is addressed rather than
// simply read: TH selects whether a gun is talking at all, and TR picks which
// of the two daisy-chained guns answers.
//
// Only bits the game has configured as OUTPUTS count. Genesis Plus GX clears
// the rest before decoding and notes that this is what fixes Lethal Enforcers
// II, so a game leaving TH as an input reads as "gun selected" - the opposite
// of the floating-high a pad would see. Copied deliberately rather than
// reasoned from the pin.
std::uint8_t ControllerPort::justifier_value() const noexcept {
    const auto driven = static_cast<std::uint8_t>(data_ & control_);
    // TH high: nothing selected. The console uses this to detect the device.
    if ((driven & 0x40) != 0) {
        return 0x30;
    }
    // TR picks the gun. Both are emulated: the second one daisy-chains off the
    // first and is how Lethal Enforcers is played two-handed.
    const bool second = (driven & 0x20) != 0;
    const bool trigger = second ? gun2_trigger_ : is_pressed(Button::a);
    const bool start = second ? gun2_start_ : is_pressed(Button::start);
    // ACTIVE LOW, so a released button is a SET bit. An unused second gun has to
    // read 0x73 and not 0x70 - the latter reports its trigger and Start held
    // down for ever, which is a stuck-fire player two.
    std::uint8_t value = 0x70;  // TL, TR and TH read high
    if (!trigger) value |= 0x01;
    if (!start) value |= 0x02;
    return value;
}

std::uint8_t ControllerPort::read_data() const noexcept {
    if (kind_ == Kind::none) {
        // An empty port floats high on every input pin.
        return static_cast<std::uint8_t>((data_ & control_) | (~control_ & 0x7F));
    }
    if (kind_ == Kind::miracle_piano) {
        ++reads_;
        const auto value = const_cast<MiraclePiano&>(piano_).read_lines(control_);
        return static_cast<std::uint8_t>((data_ & control_) | (value & ~control_));
    }
    if (kind_ == Kind::menacer || kind_ == Kind::justifier) {
        ++reads_;
        const auto value = kind_ == Kind::menacer ? menacer_value() : justifier_value();
        const auto result = static_cast<std::uint8_t>((data_ & control_) | (value & ~control_));
        // Logged like a pad's, so "the game is reading the gun but not acting
        // on it" can be told apart from "the game is not seeing the button".
        ++read_log_[th_high_ ? 0 : 1][result];
        return result;
    }
    // The counter saturates at four, so it is the phase directly - no modulo.
    // Phase four answers like phase zero, which is what the mapping below does
    // by falling through.
    const std::uint32_t phase = (kind_ == Kind::pad6) ? th_edges_ : 0;
    const auto value = bank_value(phase, th_high_);
    // Pins configured as outputs read back what the CPU last wrote to them.
    const auto result = static_cast<std::uint8_t>((data_ & control_) | (value & ~control_));
    // What the game actually receives, split by which bank it was reading. A
    // pad fault shows up here as a byte the hardware could never produce.
    ++read_log_[th_high_ ? 0 : 1][result];
    ++reads_;
    return result;
}

std::string ControllerPort::describe() const {
    std::ostringstream out;
    out << (kind_ == Kind::none        ? "empty"
            : kind_ == Kind::pad6      ? "6-button pad"
            : kind_ == Kind::menacer   ? "Menacer"
            : kind_ == Kind::justifier ? "Justifier"
                                       : "3-button pad");
    if (kind_ != Kind::none) {
        out << " [";
        const char* names[] = {"U", "D", "L", "R", "A", "B", "C", "St", "X", "Y", "Z", "M"};
        bool any = false;
        for (std::size_t i = 0; i < pressed_.size(); ++i) {
            if (pressed_[i]) {
                if (any) out << ' ';
                out << names[i];
                any = true;
            }
        }
        if (!any) out << "idle";
        out << "]";
    }
    return out.str();
}

}  // namespace mercury::genesis


