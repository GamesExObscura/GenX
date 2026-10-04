#include "mercury/gamegear/system_control.hpp"

#include <iomanip>
#include <sstream>

namespace mercury::gamegear {

namespace {
// Whether an empty socket also raises the interrupt, over and over, as well as
// showing in status bit 2. The documented behaviour says it does; how OFTEN it
// does is not documented anywhere, and a rate has to be picked. Measured over
// the 233-cartridge library, picking one moves 42 games onto different
// execution paths - not because the bit is wrong but because the invented rate
// hammers their interrupt handlers. Status bit 2 alone tells a game everything
// it needs to know without that, so the nagging stays off until there is
// evidence for a real period.
constexpr bool kNagAboutAbsentPartner = false;
}  // namespace

void SystemControl::reset() noexcept {
    ext_data_ = 0x7F;
    ext_direction_ = 0xFF;
    serial_tx_data_ = 0x00;
    serial_rx_data_ = 0xFF;
    serial_control_ = 0x00;
    stereo_control_ = 0xFF;
    overseas_mode_ = true;
    pal_mode_ = false;
    input_.reset();
    reset_link_hardware();
}

std::uint8_t SystemControl::peek_port(std::uint8_t port) const noexcept {
    switch (port) {
        case kStartRegionPort: {
            std::uint8_t value = kPort00LowBitsMask;
            if (!pal_mode_) {
                value &= static_cast<std::uint8_t>(~0x20);
            } else {
                value |= 0x20;
            }
            if (overseas_mode_) {
                value |= 0x40;
            } else {
                value &= static_cast<std::uint8_t>(~0x40);
            }
            if (!input_.is_pressed(Button::start)) {
                value |= 0x80;
            } else {
                value &= static_cast<std::uint8_t>(~0x80);
            }
            return value;
        }
        // The parallel data port reads the WIRE, not what was last written to
        // it: a pin configured as an input reads whatever the other console is
        // doing with it, and a pin driven high still reads low if the other end
        // is pulling it down. Bit 7 is not a signal pin, so it reads back.
        case 0x01:
            return static_cast<std::uint8_t>((ext_data_ & 0x80) | resolved_pins());
        case 0x02:
            return ext_direction_;
        case 0x03:
            return serial_tx_data_;
        case 0x04:
            return serial_rx_data_;
        case 0x05:
            return serial_status();
        case kStereoPort:
            return stereo_control_;
        case kInputPort0:
            return input_.read_port_dc();
        case kInputPort1:
            return input_.read_port_dd();
        default:
            return 0xFF;
    }
}

std::uint8_t SystemControl::read_port(std::uint8_t port) noexcept {
    if (port_trace_ != nullptr && port <= 0x05) {
        port_trace_(port_trace_user_, false, port, peek_port(port));
    }
    switch (port) {
        case 0x01:
            ++serial_.parallel_reads;
            return peek_port(port);
        case 0x04: {
            ++serial_.rx_reads;
            const std::uint8_t value = serial_rx_data_;
            // Taking the byte is what acknowledges it. A game that never reads
            // $04 keeps the ready flag set and the interrupt asserted, which is
            // exactly what hardware does.
            rx_.ready = false;
            rx_.frame_error = false;
            nmi_.serial_latch = false;
            update_link_nmi();
            return value;
        }
        case 0x05:
            ++serial_.control_reads;
            return serial_status();
        default:
            return peek_port(port);
    }
}

void SystemControl::write_port(std::uint8_t port, std::uint8_t value) noexcept {
    if (port_trace_ != nullptr && port <= 0x05) {
        port_trace_(port_trace_user_, true, port, value);
    }
    switch (port) {
        case 0x01:
            // Kept whole, including bit 7. Only the low seven are signal pins,
            // but bit 7 reads back what was written, so masking it away here
            // would make it permanently zero.
            ext_data_ = value;
            ++serial_.parallel_writes;
            serial_.last_parallel = value;
            refresh_local_wire();
            break;
        case 0x02: {
            ext_direction_ = value;
            ++serial_.direction_writes;
            serial_.last_direction = value;
            // Bit 7 is the parallel handshake interrupt. Setting it disables
            // and clears; clearing it arms, but not immediately - the hardware
            // takes a couple of instructions, and arming instantly would fire
            // on the console's own settling edge.
            if ((value & 0x80) != 0) {
                nmi_.parallel_latch = false;
                nmi_.parallel_armed = false;
                nmi_.parallel_arm_delay = 0;
                update_link_nmi();
            } else {
                nmi_.parallel_armed = false;
                nmi_.parallel_arm_delay = 2;
            }
            refresh_local_wire();
            break;
        }
        case 0x03:
            serial_tx_data_ = value;
            ++serial_.tx_writes;
            serial_.last_tx = value;
            start_tx(value);
            break;
        case 0x05: {
            ++serial_.control_writes;
            serial_.last_control = value;
            const std::uint8_t previous = serial_control_;
            // The low three bits are status lines coming back from the
            // hardware; a write cannot set them.
            serial_control_ = static_cast<std::uint8_t>(value & ~kSerialStatusMask);

            if ((previous & kSerialNmiEnable) != 0 && (serial_control_ & kSerialNmiEnable) == 0) {
                nmi_.serial_latch = false;
                update_link_nmi();
            }
            if ((previous & kSerialTxEnable) != 0 && (serial_control_ & kSerialTxEnable) == 0) {
                abort_tx();
            }
            if ((previous & kSerialRxEnable) != 0 && (serial_control_ & kSerialRxEnable) == 0) {
                abort_rx();
                rx_.ready = false;
                rx_.frame_error = false;
                nmi_.serial_latch = false;
                update_link_nmi();
            }

            // Turning the receiver ON must not itself count as a start bit:
            // the line was already low before the receiver was listening.
            const bool receiver_just_enabled =
                (previous & kSerialRxEnable) == 0 && (serial_control_ & kSerialRxEnable) != 0;
            refresh_local_wire(!receiver_just_enabled);
            break;
        }
        case kStereoPort:
            stereo_control_ = value;
            break;
        default:
            break;
    }
}

// ---- Gear-to-Gear hardware ---------------------------------------------

void SystemControl::attach_link(GearLinkCable* cable, int side) noexcept {
    link_ = cable;
    link_side_ = side;
    reset_link_hardware();
    refresh_local_wire(false);
    sample_remote_wire(false);
}

void SystemControl::detach_link() noexcept {
    link_ = nullptr;
    reset_link_hardware();
}

void SystemControl::republish_link() noexcept {
    if (link_ == nullptr) {
        return;
    }
    link_->publish(link_side_, local_wire_);
    // Look again at the far end on the next instruction. The comparison is
    // against remote_wire_, which WAS restored, so a far end that matches
    // what was saved produces no edge - this only makes sure a far end that
    // differs is noticed.
    remote_revision_ = 0;
}

void SystemControl::sanitize_loaded() noexcept {
    const auto one_of = [](std::uint32_t value, const std::uint32_t (&allowed)[4]) {
        for (const auto a : allowed) {
            if (value == a) return value;
        }
        return allowed[0];
    };
    tx_.bit_cycles = one_of(tx_.bit_cycles, kBitCycles);
    rx_.bit_cycles = one_of(rx_.bit_cycles, kBitCycles);
    rx_.half_bit_cycles = one_of(rx_.half_bit_cycles, kHalfBitCycles);
    if (tx_.phase > 10) tx_.phase = 10;
    if (rx_.bit > 7) rx_.bit = 7;
    if (static_cast<std::uint8_t>(rx_.state) > static_cast<std::uint8_t>(RxState::stop)) {
        rx_.state = RxState::idle;
    }
    if (nmi_.parallel_arm_delay > 2) nmi_.parallel_arm_delay = 2;
    // Every scheduled time is within a few frames of now - the slowest baud's
    // whole frame is 120,000 cycles. Far outside that, the catch-up loops
    // that step from one event to the next would run for billions of turns.
    constexpr std::uint64_t kWindow = 1'000'000;
    const auto near_now = [this](std::uint64_t at) {
        const std::uint64_t distance = at > serial_cycle_ ? at - serial_cycle_ : serial_cycle_ - at;
        return distance <= kWindow;
    };
    if (tx_.next_cycle != 0 && !near_now(tx_.next_cycle)) tx_.next_cycle = serial_cycle_ + tx_.bit_cycles;
    if (rx_.next_cycle != 0 && !near_now(rx_.next_cycle)) rx_.next_cycle = serial_cycle_ + rx_.bit_cycles;
    if (nmi_.next_absent_cycle != 0 && !near_now(nmi_.next_absent_cycle)) nmi_.next_absent_cycle = 0;
}

void SystemControl::reset_link_hardware() noexcept {
    tx_ = SerialTx{};
    rx_ = SerialRx{};
    nmi_ = SerialNmi{};
    nmi_edge_ = false;
    nmi_.next_absent_cycle = 0;
    remote_wire_ = GearLinkCable::WireState{};
    remote_revision_ = 0;
    local_wire_ = compute_local_wire();
    if (link_ != nullptr) {
        link_->publish(link_side_, local_wire_);
    }
    nmi_.previous_interrupt_pin = resolve_pin(kInterruptPin);
}

GearLinkCable::WireState SystemControl::compute_local_wire() const noexcept {
    GearLinkCable::WireState state;
    state.levels = static_cast<std::uint8_t>(ext_data_ & 0x7F);
    state.drive_mask = 0;

    // Port $02 is a direction register with 0 meaning output, so a zero bit is
    // a pin this console is driving.
    for (int bit = 0; bit < 7; ++bit) {
        if ((ext_direction_ & (1u << bit)) == 0) {
            state.drive_mask |= static_cast<std::uint8_t>(1u << bit);
        }
    }

    // With the transmitter enabled the UART owns its pin regardless of what the
    // direction register says.
    if ((serial_control_ & kSerialTxEnable) != 0) {
        state.drive_mask |= kTxPinMask;
        if (tx_.line) {
            state.levels |= kTxPinMask;
        } else {
            state.levels &= static_cast<std::uint8_t>(~kTxPinMask);
        }
    }

    // ...and with the receiver enabled it lets go of the receive pin, so the
    // other console can pull it.
    if ((serial_control_ & kSerialRxEnable) != 0) {
        state.drive_mask &= static_cast<std::uint8_t>(~(1u << kRxPin));
    }

    state.drive_mask &= 0x7F;
    state.levels &= 0x7F;
    return state;
}

void SystemControl::refresh_local_wire(bool detect_rx_edge) noexcept {
    const bool old_rx = resolve_pin(kRxPin);
    const bool old_interrupt = resolve_pin(kInterruptPin);

    local_wire_ = compute_local_wire();
    if (link_ != nullptr) {
        link_->publish(link_side_, local_wire_);
    }

    const bool new_rx = resolve_pin(kRxPin);
    const bool new_interrupt = resolve_pin(kInterruptPin);

    if (detect_rx_edge && old_rx != new_rx) {
        handle_rx_edge(old_rx, new_rx);
    }
    if (old_interrupt != new_interrupt) {
        handle_interrupt_pin_edge(old_interrupt, new_interrupt);
    } else {
        nmi_.previous_interrupt_pin = new_interrupt;
    }
}

void SystemControl::sample_remote_wire(bool detect_edges) noexcept {
    if (link_ == nullptr) {
        return;
    }
    // This runs once per instruction, so it has to be nearly free when nothing
    // moved - which is almost always.
    const auto revision = link_->revision(link_side_ ^ 1);
    if (revision == remote_revision_) {
        return;
    }
    remote_revision_ = revision;

    const auto sampled = link_->remote_for(link_side_);
    if (sampled.drive_mask == remote_wire_.drive_mask && sampled.levels == remote_wire_.levels) {
        return;
    }

    const bool old_rx = resolve_pin(kRxPin);
    const bool old_interrupt = resolve_pin(kInterruptPin);

    remote_wire_ = sampled;

    const bool new_rx = resolve_pin(kRxPin);
    const bool new_interrupt = resolve_pin(kInterruptPin);

    if (detect_edges && old_rx != new_rx) {
        handle_rx_edge(old_rx, new_rx);
    }
    if (detect_edges && old_interrupt != new_interrupt) {
        handle_interrupt_pin_edge(old_interrupt, new_interrupt);
    } else {
        nmi_.previous_interrupt_pin = new_interrupt;
    }
}

bool SystemControl::resolve_pin(int bit) const noexcept {
    const auto mask = static_cast<std::uint8_t>(1u << bit);
    const bool local_drives = (local_wire_.drive_mask & mask) != 0;
    const bool remote_drives = (remote_wire_.drive_mask & mask) != 0;
    const bool local_level = (local_wire_.levels & mask) != 0;
    const bool remote_level = (remote_wire_.levels & mask) != 0;

    // Open drain: whoever pulls low wins, and an undriven pin floats high.
    if (local_drives && remote_drives) {
        return local_level && remote_level;
    }
    if (local_drives) {
        return local_level;
    }
    if (remote_drives) {
        return remote_level;
    }
    return true;
}

std::uint8_t SystemControl::resolved_pins() const noexcept {
    std::uint8_t pins = 0;
    for (int bit = 0; bit < 7; ++bit) {
        if (resolve_pin(bit)) {
            pins |= static_cast<std::uint8_t>(1u << bit);
        }
    }
    return pins;
}

std::uint8_t SystemControl::serial_status() const noexcept {
    std::uint8_t value = static_cast<std::uint8_t>(serial_control_ & ~kSerialStatusMask);
    // Bit 0 reports the TRANSMISSION, not whether a write would be accepted.
    //
    // Both halves of that were measured. R.B.I. Baseball '94 watches this bit
    // to know a byte went out: report the holding register instead and the bit
    // is clear almost always, the game sends eight bytes and never reaches its
    // title screen. Pac-Man never reads the bit at all and just needs its
    // write taken, which the holding register does independently. So the bit
    // follows the shifter and the holding register accepts the byte anyway.
    value |= tx_.busy ? 0x01 : 0x00;
    value |= rx_.ready ? 0x02 : 0x00;
    // Bit 2 is not a framing-error flag, whatever the name suggests. It means
    // "the console at the other end is not powered on, or something went
    // wrong" - the two are one signal on this hardware, and a game looking for
    // a partner is reading it to find out whether anybody answered.
    const bool nobody_there = link_running() && partner_absent();
    value |= (rx_.frame_error || nobody_there) ? 0x04 : 0x00;
    return value;
}

void SystemControl::service_absent_partner() noexcept {
    if (!link_running() || !partner_absent()) {
        nmi_.next_absent_cycle = 0;
        return;
    }
    // Announced over and over rather than once. A game that enables the port
    // with the interrupt on and finds nothing there is meant to be nagged
    // until it turns the port off again.
    const auto baud = static_cast<std::size_t>((serial_control_ >> 6) & 0x03);
    const std::uint64_t period = static_cast<std::uint64_t>(kBitCycles[baud]) * 10;
    if (nmi_.next_absent_cycle == 0) {
        nmi_.next_absent_cycle = serial_cycle_ + period;
        return;
    }
    while (serial_cycle_ >= nmi_.next_absent_cycle) {
        nmi_.next_absent_cycle += period;
        if (kNagAboutAbsentPartner && (serial_control_ & kSerialNmiEnable) != 0) {
            // Forced back to un-asserted first so this counts as a fresh edge
            // even if the game has not read $04 since the last one. The point
            // of this signal is that it comes back.
            nmi_.asserted = false;
            nmi_.serial_latch = true;
            update_link_nmi();
        }
    }
}

void SystemControl::handle_rx_edge(bool old_level, bool new_level) noexcept {
    if ((serial_control_ & kSerialRxEnable) == 0) {
        return;
    }
    // A falling edge on an idle line is a start bit. Half a bit later the
    // receiver looks again: if the line came back up it was a glitch, and if it
    // is still low a frame has begun.
    if (old_level && !new_level && rx_.state == RxState::idle) {
        const auto baud = static_cast<std::size_t>((serial_control_ >> 6) & 0x03);
        rx_.bit_cycles = kBitCycles[baud];
        rx_.half_bit_cycles = kHalfBitCycles[baud];
        rx_.state = RxState::confirm_start;
        rx_.next_cycle = serial_cycle_ + rx_.half_bit_cycles;
    }
}

void SystemControl::handle_interrupt_pin_edge(bool old_level, bool new_level) noexcept {
    nmi_.previous_interrupt_pin = new_level;
    const bool configured_as_input = (ext_direction_ & 0x40) != 0;
    if (old_level && !new_level && configured_as_input && nmi_.parallel_armed) {
        nmi_.parallel_latch = true;
        update_link_nmi();
    }
}

void SystemControl::update_link_nmi() noexcept {
    const bool asserted = nmi_.parallel_latch || nmi_.serial_latch;
    // Edge triggered: the machine gets one NMI per assertion, not one per
    // instruction for as long as the latch stays set.
    if (asserted && !nmi_.asserted) {
        nmi_edge_ = true;
    }
    nmi_.asserted = asserted;
}

void SystemControl::start_tx(std::uint8_t value) noexcept {
    if ((serial_control_ & kSerialTxEnable) == 0) {
        return;
    }
    if (tx_.busy) {
        // Held only for the cartridges that need it, dropped otherwise. Which
        // is right is not settled - see GameGear::wants_tx_holding_register -
        // and the default matches Gearsystem, whose StartGearToGearTx also
        // returns early on busy.
        if (tx_holding_register_) {
            if (tx_.has_pending) {
                ++serial_.tx_dropped;
                if (tx_.phase < 10) {
                    ++serial_.tx_dropped_at[tx_.phase];
                }
            }
            tx_.pending = value;
            tx_.has_pending = true;
            return;
        }
        ++serial_.tx_dropped;
        if (tx_.phase < 10) {
            ++serial_.tx_dropped_at[tx_.phase];
        }
        return;
    }
    if (tx_.shifting) {
        // Accepted, but the previous frame's stop bit is still on the wire.
        // Held for that one bit period rather than truncating a frame the
        // receiver is in the middle of sampling.
        tx_.pending = value;
        tx_.has_pending = true;
        return;
    }
    const auto baud = static_cast<std::size_t>((serial_control_ >> 6) & 0x03);
    tx_.busy = true;
    tx_.shifting = true;
    tx_.data = value;
    tx_.phase = 0;
    tx_.bit_cycles = kBitCycles[baud];
    tx_.line = false;  // start bit
    tx_.next_cycle = serial_cycle_ + tx_.bit_cycles;
    refresh_local_wire();
}

void SystemControl::abort_tx() noexcept {
    tx_.busy = false;
    tx_.shifting = false;
    tx_.line = true;
    tx_.phase = 0;
    tx_.next_cycle = serial_cycle_;
    // Turning the transmitter off throws away what was held as well as what
    // was on the wire.
    tx_.has_pending = false;
    tx_.pending = 0;
}

void SystemControl::abort_rx() noexcept {
    rx_.state = RxState::idle;
    rx_.shift = 0;
    rx_.bit = 0;
    rx_.next_cycle = 0;
}

void SystemControl::process_tx_boundary() noexcept {
    if (tx_.phase < 8) {
        tx_.line = (tx_.data & (1u << tx_.phase)) != 0;
        ++tx_.phase;
        tx_.next_cycle += tx_.bit_cycles;
    } else if (tx_.phase == 8) {
        tx_.line = true;  // stop bit
        tx_.phase = 9;
        tx_.next_cycle += tx_.bit_cycles;
        // Free for the next byte as soon as the last data bit is out and the
        // line has gone high. The stop bit is a guard period on the wire, not
        // work the transmitter still has to be handed data for, and holding
        // "busy" across it refuses a write that hardware would have taken.
        // That one refused write per exchange is what put Pac-Man's
        // index/value protocol permanently a byte out of step.
        tx_.busy = false;
        ++serial_.bytes_sent;
    } else {
        tx_.line = true;
        tx_.phase = 10;
        tx_.shifting = false;
        if (tx_.has_pending) {
            const std::uint8_t next = tx_.pending;
            tx_.has_pending = false;
            start_tx(next);
            return;
        }
    }
    refresh_local_wire();
}

void SystemControl::process_rx_sample() noexcept {
    const bool line = resolve_pin(kRxPin);

    switch (rx_.state) {
        case RxState::confirm_start:
            if (line) {
                abort_rx();  // it was noise, not a frame
            } else {
                rx_.state = RxState::data;
                rx_.bit = 0;
                rx_.shift = 0;
                rx_.next_cycle += rx_.bit_cycles;
            }
            break;
        case RxState::data:
            if (line) {
                rx_.shift |= static_cast<std::uint8_t>(1u << rx_.bit);
            }
            if (rx_.bit < 7) {
                ++rx_.bit;
            } else {
                rx_.state = RxState::stop;
            }
            rx_.next_cycle += rx_.bit_cycles;
            break;
        case RxState::stop:
            if (line) {
                serial_rx_data_ = rx_.shift;
                rx_.ready = true;
                rx_.frame_error = false;
                ++serial_.bytes_received;
                serial_.last_rx = rx_.shift;
            } else {
                // The line should have returned high for the stop bit. It did
                // not, so the two consoles disagree about the baud rate or the
                // frame was cut short.
                rx_.frame_error = true;
                ++serial_.frame_errors;
            }
            if ((serial_control_ & kSerialNmiEnable) != 0) {
                nmi_.serial_latch = true;
                update_link_nmi();
            }
            rx_.state = RxState::idle;
            rx_.next_cycle = 0;
            break;
        case RxState::idle:
        default:
            break;
    }
}

void SystemControl::advance_serial_slow(std::uint32_t cycles) noexcept {
    const std::uint64_t target = serial_cycle_ + cycles;

    // The other console runs in the same process, one instruction at a time,
    // so its wire state is at most one instruction stale by the time we look.
    // At 746 cycles per bit that is under three percent of a bit period, and
    // the receiver samples the middle of the bit - the edge cannot land on the
    // wrong side of a sample point.
    sample_remote_wire(true);

    // Deliver the pending arm for the parallel interrupt. Counted in
    // instructions rather than cycles because that is what the delay is for:
    // giving the program a moment to finish setting the port up.
    if (nmi_.parallel_arm_delay > 0) {
        --nmi_.parallel_arm_delay;
        if (nmi_.parallel_arm_delay == 0 && (ext_direction_ & 0x80) == 0) {
            nmi_.parallel_armed = true;
        }
    }

    while (true) {
        std::uint64_t next = 0;
        bool has_work = false;

        if (rx_.state != RxState::idle) {
            next = rx_.next_cycle;
            has_work = true;
        }
        if (tx_.shifting && (!has_work || tx_.next_cycle < next)) {
            next = tx_.next_cycle;
            has_work = true;
        }
        if (!has_work || next > target) {
            break;
        }

        serial_cycle_ = next;

        if (rx_.state != RxState::idle && rx_.next_cycle == next) {
            process_rx_sample();
        }
        if (tx_.shifting && tx_.next_cycle == next) {
            process_tx_boundary();
        }
    }

    serial_cycle_ = target;
    service_absent_partner();
}

// ---- Plain system control ----------------------------------------------

InputState& SystemControl::input_state() noexcept {
    return input_;
}

const InputState& SystemControl::input_state() const noexcept {
    return input_;
}

void SystemControl::set_start_pressed(bool pressed) noexcept {
    input_.set_button(Button::start, pressed);
}

void SystemControl::set_input_ports(std::uint8_t port0, std::uint8_t port1) noexcept {
    input_.set_button(Button::up, (port0 & 0x01) == 0);
    input_.set_button(Button::down, (port0 & 0x02) == 0);
    input_.set_button(Button::left, (port0 & 0x04) == 0);
    input_.set_button(Button::right, (port0 & 0x08) == 0);
    input_.set_button(Button::button1, (port0 & 0x10) == 0);
    input_.set_button(Button::button2, (port0 & 0x20) == 0);
    input_.set_button(Button::ext_up, (port0 & 0x40) == 0);
    input_.set_button(Button::ext_down, (port0 & 0x80) == 0);
    input_.set_button(Button::ext_left, (port1 & 0x01) == 0);
    input_.set_button(Button::ext_right, (port1 & 0x02) == 0);
    input_.set_button(Button::ext_button1, (port1 & 0x04) == 0);
    input_.set_button(Button::ext_button2, (port1 & 0x08) == 0);
    input_.set_button(Button::ext_th, (port1 & 0x80) == 0);
}

std::string SystemControl::describe() const {
    std::ostringstream out;
    out << "System control: Start/region @ "
        << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(kStartRegionPort)
        << ", stereo @ " << std::setw(2) << static_cast<int>(kStereoPort)
        << ", inputs @ " << std::setw(2) << static_cast<int>(kInputPort0)
        << "/" << std::setw(2) << static_cast<int>(kInputPort1)
        << ", stereo mask=" << std::setw(2) << static_cast<int>(stereo_control_)
        << ", link " << (link_ != nullptr ? "attached" : "unplugged")
        << ", " << input_.describe();
    return out.str();
}

}  // namespace mercury::gamegear
