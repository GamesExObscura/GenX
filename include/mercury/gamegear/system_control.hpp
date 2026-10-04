#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "mercury/gamegear/gear_link.hpp"
#include "mercury/gamegear/input_state.hpp"

namespace mercury::gamegear {

class SystemControl {
public:
    static constexpr std::uint8_t kStartRegionPort = 0x00;
    static constexpr std::uint8_t kStereoPort = 0x06;
    static constexpr std::uint8_t kInputPort0 = 0xC0;
    static constexpr std::uint8_t kInputPort1 = 0xC1;

    void reset() noexcept;

    // The bus read. Port $04 has side effects on real hardware - reading the
    // received byte is what clears the ready flag and the serial interrupt -
    // so this cannot be const.
    [[nodiscard]] std::uint8_t read_port(std::uint8_t port) noexcept;
    // Same decode with the side effects left out, for probes and debuggers
    // that want to look at a port without the machine noticing.
    [[nodiscard]] std::uint8_t peek_port(std::uint8_t port) const noexcept;

    void write_port(std::uint8_t port, std::uint8_t value) noexcept;
    [[nodiscard]] InputState& input_state() noexcept;
    [[nodiscard]] const InputState& input_state() const noexcept;
    void set_start_pressed(bool pressed) noexcept;
    void set_input_ports(std::uint8_t port0, std::uint8_t port1) noexcept;

    [[nodiscard]] std::string describe() const;

    // Every access to a link port, handed to a probe as it happens. Counters
    // say a game talks to the cable; only the sequence says what it is trying
    // to do, and a bit-banged protocol is invisible in aggregate.
    //
    // A raw function pointer rather than std::function: this sits on the port
    // decode path of every game, and the null check has to cost nothing.
    using PortTrace = void (*)(void* user, bool write, std::uint8_t port, std::uint8_t value);
    void set_port_trace(PortTrace trace, void* user) noexcept {
        port_trace_ = trace;
        port_trace_user_ = user;
    }

    // Hold a write to $03 that lands while a byte is going out, instead of
    // dropping it. Off by default and turned on per cartridge - see
    // GameGear::wants_tx_holding_register for why it cannot simply be on.
    void set_tx_holding_register(bool enabled) noexcept { tx_holding_register_ = enabled; }
    [[nodiscard]] bool tx_holding_register() const noexcept { return tx_holding_register_; }

    // ---- Gear-to-Gear link ----------------------------------------------
    //
    // Plug this console into one end of a cable. `side` is 0 or 1 and only
    // decides which end of the crossover it sits on; the two consoles must not
    // share a side or each would be listening to its own transmit pin.
    void attach_link(GearLinkCable* cable, int side) noexcept;
    void detach_link() noexcept;
    // Put this console's pins back on the cable after a state load, WITHOUT
    // resetting the port the way attaching does. The cable is not part of
    // either console's saved state, so a pair restored from another machine
    // otherwise carries on reading whatever this machine's cable last held.
    void republish_link() noexcept;
    [[nodiscard]] bool link_attached() const noexcept { return link_ != nullptr; }

    // Run the serial hardware forward. Called from the machine step with the
    // cycles the CPU just spent, so a transmission takes the same wall-clock
    // time it takes on hardware and a game polling the busy bit sees it clear
    // when it should.
    void advance_serial(std::uint32_t cycles) noexcept {
        // Nothing plugged in and nothing in flight: there is no state machine
        // to run, and this is on the path of every instruction of every Game
        // Gear game whether it has a link mode or not.
        // ...and nothing to tell the game about the empty socket, which only
        // matters once it has actually switched the port on.
        if (link_ == nullptr && !tx_.shifting && rx_.state == RxState::idle && !link_running()) {
            serial_cycle_ += cycles;
            return;
        }
        advance_serial_slow(cycles);
    }

    // Set by a completed reception (or a parallel handshake) when the game has
    // enabled the link interrupt. Consumed by the machine, which turns it into
    // a Z80 NMI.
    [[nodiscard]] bool take_link_nmi() noexcept {
        const bool pending = nmi_edge_;
        nmi_edge_ = false;
        if (pending) {
            ++serial_.link_nmis;
        }
        return pending;
    }

    [[nodiscard]] std::uint64_t serial_cycle() const noexcept { return serial_cycle_; }
    [[nodiscard]] bool serial_tx_busy() const noexcept { return tx_.busy; }
    [[nodiscard]] bool serial_rx_ready() const noexcept { return rx_.ready; }
    [[nodiscard]] bool serial_frame_error() const noexcept { return rx_.frame_error; }
    [[nodiscard]] std::uint8_t serial_status() const noexcept;
    [[nodiscard]] std::uint8_t resolved_pins() const noexcept;

private:
    static constexpr std::uint8_t kPort00LowBitsMask = 0x1F;

    // Z80 cycles per serial bit, indexed by port $05 bits 7-6. The half-bit
    // figure is where the receiver samples: the middle of the bit, so a small
    // disagreement about when the edge happened cannot flip a bit.
    static constexpr std::uint32_t kBitCycles[4] = {746, 1492, 2983, 11932};
    static constexpr std::uint32_t kHalfBitCycles[4] = {373, 746, 1492, 5966};

    // Port $05, the serial control register.
    static constexpr std::uint8_t kSerialNmiEnable = 0x08;
    static constexpr std::uint8_t kSerialTxEnable = 0x10;
    static constexpr std::uint8_t kSerialRxEnable = 0x20;
    static constexpr std::uint8_t kSerialStatusMask = 0x07;  // read-only low bits

    // Which pin of the seven carries what.
    static constexpr std::uint8_t kTxPinMask = 0x10;
    static constexpr int kRxPin = 5;
    static constexpr int kInterruptPin = 6;

    enum class RxState : std::uint8_t { idle, confirm_start, data, stop };

    struct SerialTx {
        std::uint64_t next_cycle = 0;
        std::uint32_t bit_cycles = kBitCycles[0];
        std::uint8_t data = 0;
        std::uint8_t phase = 0;
        bool busy = false;
        bool line = true;  // idle high
        // Whether a frame is still going out on the WIRE, which is not the
        // same question as whether the register will accept a byte.
        //
        // `busy` - status bit 0 - clears once the last data bit is out and the
        // line has gone high for the stop bit, because the stop bit is a guard
        // period on the wire rather than work the transmitter needs more data
        // for. `shifting` stays true until that guard period ends, so a byte
        // handed over during it waits its turn instead of cutting the stop bit
        // short and handing the receiver a framing error.
        //
        // The window is one bit period in ten. That is the whole difference
        // between Pac-Man's link working and not: it writes $03 without
        // checking bit 0, and one refused write puts its index/value protocol
        // permanently a byte out of step. A FULL holding register was tried
        // first and is wrong - it fixes Pac-Man and stops R.B.I. Baseball '94
        // reaching its title screen.
        bool shifting = false;
        std::uint8_t pending = 0;
        bool has_pending = false;
    };

    struct SerialRx {
        std::uint64_t next_cycle = 0;
        std::uint32_t bit_cycles = kBitCycles[0];
        std::uint32_t half_bit_cycles = kHalfBitCycles[0];
        std::uint8_t shift = 0;
        std::uint8_t bit = 0;
        RxState state = RxState::idle;
        bool ready = false;
        bool frame_error = false;
    };

    struct SerialNmi {
        bool parallel_latch = false;
        bool serial_latch = false;
        bool asserted = false;
        bool parallel_armed = false;
        std::uint8_t parallel_arm_delay = 0;
        bool previous_interrupt_pin = true;
        // The "nothing on the other end" signal. Status bit 2 is not only a
        // framing error: it also means the console at the far end is switched
        // off or unplugged, and the hardware keeps announcing that rather than
        // saying it once. Games are expected to notice, turn the link off, and
        // only switch it on again while they are looking for a partner.
        std::uint64_t next_absent_cycle = 0;
    };

    void advance_serial_slow(std::uint32_t cycles) noexcept;
    void reset_link_hardware() noexcept;
    void refresh_local_wire(bool detect_rx_edge = true) noexcept;
    [[nodiscard]] GearLinkCable::WireState compute_local_wire() const noexcept;
    void sample_remote_wire(bool detect_edges) noexcept;
    [[nodiscard]] bool resolve_pin(int bit) const noexcept;
    void handle_rx_edge(bool old_level, bool new_level) noexcept;
    void handle_interrupt_pin_edge(bool old_level, bool new_level) noexcept;
    void update_link_nmi() noexcept;
    void start_tx(std::uint8_t value) noexcept;
    void abort_tx() noexcept;
    void abort_rx() noexcept;
    void process_tx_boundary() noexcept;
    void process_rx_sample() noexcept;
    // Is there a console at the other end of the cable? Both enables have to
    // be set for the port to be running at all, so a game that has not turned
    // the link on is not told anything either way.
    [[nodiscard]] bool link_running() const noexcept {
        return (serial_control_ & (kSerialTxEnable | kSerialRxEnable)) ==
               (kSerialTxEnable | kSerialRxEnable);
    }
    [[nodiscard]] bool partner_absent() const noexcept {
        return link_ == nullptr || !link_->connected();
    }
    void service_absent_partner() noexcept;

  public:
    // input_ is left out: which buttons are down comes from whoever is holding
    // the console, not from a file. The cable pointer is left out too - it is
    // whether a second window happens to be open right now, which a state file
    // has no business deciding.
    template <typename Archive>
    void visit_state(Archive& ar) {
        ar(ext_data_);
        ar(ext_direction_);
        ar(serial_tx_data_);
        ar(serial_rx_data_);
        ar(serial_control_);
        ar(stereo_control_);
        ar(overseas_mode_);
        ar(pal_mode_);
        ar(serial_cycle_);
        ar(tx_);
        ar(rx_);
        ar(nmi_);
        ar(local_wire_);
        ar(remote_wire_);
        if (Archive::loading()) {
            nmi_edge_ = false;
            sanitize_loaded();
        }
    }
    // A loaded state can come from someone else - over netplay with a Gear
    // Link, or a shared file. Serial timings and scheduled bit times the
    // hardware could never hold are brought back into range, so none of them
    // can drive the catch-up loops for billions of iterations.
    void sanitize_loaded() noexcept;

  private:
    std::uint8_t ext_data_ = 0x7F;
    std::uint8_t ext_direction_ = 0xFF;
    std::uint8_t serial_tx_data_ = 0x00;
    std::uint8_t serial_rx_data_ = 0xFF;
    std::uint8_t serial_control_ = 0x00;

    PortTrace port_trace_ = nullptr;
    void* port_trace_user_ = nullptr;
    // Deliberately NOT cleared by reset(): it describes the cartridge in the
    // slot, not the machine's state, and reset() runs during load_rom.
    bool tx_holding_register_ = false;

  public:
    // Does this game touch the link cable at all, and when?
    //
    // Worth counting before emulating anything: the Gear-to-Gear port used to
    // be a stub here, so the first question was whether a game even reaches
    // for it. Still useful with the hardware in place - the transmit and
    // receive counts are what say whether a link session is actually moving
    // bytes, rather than two consoles politely ignoring each other.
    struct SerialActivity {
        std::uint64_t tx_writes = 0;     // port $03, a byte handed to the cable
        std::uint64_t rx_reads = 0;      // port $04, looking for a byte back
        std::uint64_t control_writes = 0;  // port $05, setting up the link
        std::uint64_t control_reads = 0;   // port $05, polling the status bits
        std::uint64_t bytes_sent = 0;      // frames that finished on the wire
        std::uint64_t bytes_received = 0;  // frames that arrived intact
        std::uint64_t frame_errors = 0;    // arrived with a broken stop bit
        // The other half of the port. A game can use the seven pins as plain
        // wires without going near the UART, so a link game that never touches
        // $03/$04/$05 is not necessarily a game that ignores the cable.
        std::uint64_t parallel_writes = 0;  // port $01, driving the pins
        std::uint64_t direction_writes = 0;  // port $02, choosing which are outputs
        std::uint64_t parallel_reads = 0;    // port $01, reading the wire
        std::uint64_t link_nmis = 0;         // interrupts the cable caused
        // Writes to $03 thrown away because a transmission was still running.
        // A game that checks the busy bit first can never do this; Pac-Man
        // does not check, and a single lost byte puts its index/value protocol
        // permanently one byte out of phase.
        std::uint64_t tx_dropped = 0;
        // ...and how far through the byte on the wire each refusal happened,
        // indexed by bit: 0 is the start bit, 1-8 the data bits, 9 the stop
        // bit. Whether the games that need a write accepted and the games that
        // need it refused can be told apart by timing at all is a question
        // only this can answer.
        std::uint64_t tx_dropped_at[10]{};
        std::uint8_t last_parallel = 0;
        std::uint8_t last_direction = 0;
        std::uint8_t last_control = 0;
        std::uint8_t last_tx = 0;
        std::uint8_t last_rx = 0;
    };
    [[nodiscard]] const SerialActivity& serial_activity() const noexcept { return serial_; }
    void clear_serial_activity() noexcept { serial_ = SerialActivity{}; }

  private:
    mutable SerialActivity serial_{};
    std::uint8_t stereo_control_ = 0xFF;
    bool overseas_mode_ = true;
    bool pal_mode_ = false;
    InputState input_{};

    GearLinkCable* link_ = nullptr;
    int link_side_ = 0;
    std::uint64_t serial_cycle_ = 0;
    SerialTx tx_{};
    SerialRx rx_{};
    SerialNmi nmi_{};
    GearLinkCable::WireState local_wire_{};
    GearLinkCable::WireState remote_wire_{};
    std::uint32_t remote_revision_ = 0;
    bool nmi_edge_ = false;
};

}  // namespace mercury::gamegear
