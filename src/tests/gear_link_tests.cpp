// Does the Gear-to-Gear cable actually carry a byte?
//
// These drive two SystemControl objects directly, with no CPU and no ROM, so a
// failure points at the serial hardware and nothing else. The register map and
// the baud table come from Gearsystem, the one emulator with a working
// implementation; these tests are what say GenX reproduces it.

#include "mercury/gamegear/gear_link.hpp"
#include "mercury/gamegear/system_control.hpp"

#include <cstdio>
#include <string>

namespace {

using mercury::gamegear::GearLinkCable;
using mercury::gamegear::SystemControl;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void check_equal(unsigned actual, unsigned expected, const std::string& what) {
    if (actual != expected) {
        ++g_failures;
        std::printf("FAIL: %s (got 0x%02X, expected 0x%02X)\n", what.c_str(), actual, expected);
    }
}

// Two consoles plugged into one cable, each on its own end of the crossover.
struct LinkedPair {
    GearLinkCable cable;
    SystemControl left;
    SystemControl right;

    LinkedPair() {
        left.reset();
        right.reset();
        cable.set_connected(true);
        left.attach_link(&cable, 0);
        right.attach_link(&cable, 1);
    }

    // Run both forward together in small slices. Anything coarser and one
    // console would miss the moment the other moved a pin.
    void advance(std::uint32_t cycles, std::uint32_t slice = 8) {
        for (std::uint32_t done = 0; done < cycles; done += slice) {
            const std::uint32_t step = (cycles - done) < slice ? (cycles - done) : slice;
            left.advance_serial(step);
            right.advance_serial(step);
        }
    }
};

// Port $05 bit 4 enables the transmitter, bit 5 the receiver, bits 7-6 pick
// the baud rate.
constexpr std::uint8_t kTxEnable = 0x10;
constexpr std::uint8_t kRxEnable = 0x20;
constexpr std::uint8_t kNmiEnable = 0x08;

// One frame is a start bit, eight data bits and a stop bit, so ten bit
// periods; a little slack on top covers the half-bit the receiver waits before
// confirming the start.
constexpr std::uint32_t kBitCycles = 746;
constexpr std::uint32_t kFrameCycles = kBitCycles * 12;

void test_byte_crosses_the_cable() {
    LinkedPair pair;
    pair.left.write_port(0x05, kTxEnable | kRxEnable);
    pair.right.write_port(0x05, kTxEnable | kRxEnable);

    check(!pair.right.serial_rx_ready(), "receiver starts with nothing waiting");

    pair.left.write_port(0x03, 0xA5);
    check(pair.left.serial_tx_busy(), "writing the transmit port starts a transmission");

    pair.advance(kFrameCycles);

    check(!pair.left.serial_tx_busy(), "the transmitter finishes inside one frame time");
    check(pair.right.serial_rx_ready(), "the other console has a byte waiting");
    check(!pair.right.serial_frame_error(), "the frame arrived intact");
    check_equal(pair.right.read_port(0x04), 0xA5, "the byte that arrived");
    check(!pair.right.serial_rx_ready(), "reading the byte clears the ready flag");
}

void test_both_directions_at_once() {
    LinkedPair pair;
    pair.left.write_port(0x05, kTxEnable | kRxEnable);
    pair.right.write_port(0x05, kTxEnable | kRxEnable);

    // The crossover is what makes this work: each console's transmit pin is
    // the other's receive pin, so the two frames do not collide.
    pair.left.write_port(0x03, 0x3C);
    pair.right.write_port(0x03, 0xC3);
    pair.advance(kFrameCycles);

    check_equal(pair.right.read_port(0x04), 0x3C, "byte received on the right");
    check_equal(pair.left.read_port(0x04), 0xC3, "byte received on the left");
}

void test_status_bits_report_progress() {
    LinkedPair pair;
    pair.left.write_port(0x05, kTxEnable | kRxEnable);
    pair.right.write_port(0x05, kTxEnable | kRxEnable);

    pair.left.write_port(0x03, 0x5A);
    check((pair.left.read_port(0x05) & 0x01) != 0, "busy bit set while transmitting");
    pair.advance(kBitCycles * 4);
    check((pair.left.read_port(0x05) & 0x01) != 0, "still busy part-way through the frame");
    pair.advance(kFrameCycles);
    check((pair.left.read_port(0x05) & 0x01) == 0, "busy bit clears when the frame ends");
    check((pair.right.read_port(0x05) & 0x02) != 0, "ready bit set on the receiving side");

    // A write to $05 must not be able to set the three status bits.
    pair.right.write_port(0x05, 0xFF);
    check_equal(pair.right.peek_port(0x05) & 0xF8, 0xF8, "control bits take the write");
}

void test_disabled_receiver_hears_nothing() {
    LinkedPair pair;
    pair.left.write_port(0x05, kTxEnable);
    pair.right.write_port(0x05, kTxEnable);  // receiver off

    pair.left.write_port(0x03, 0x77);
    pair.advance(kFrameCycles);

    check(!pair.right.serial_rx_ready(), "a receiver that is switched off collects nothing");
}

void test_disabled_transmitter_sends_nothing() {
    LinkedPair pair;
    pair.left.write_port(0x05, kRxEnable);   // transmitter off
    pair.right.write_port(0x05, kTxEnable | kRxEnable);

    pair.left.write_port(0x03, 0x11);
    check(!pair.left.serial_tx_busy(), "writing the data port does nothing with the transmitter off");
    pair.advance(kFrameCycles);
    check(!pair.right.serial_rx_ready(), "and nothing reaches the other console");
}

void test_mismatched_baud_is_a_frame_error() {
    LinkedPair pair;
    // Left transmits at the fastest rate, right listens at the slowest. The
    // receiver samples long after the frame is over and finds the line in the
    // wrong state for a stop bit.
    pair.left.write_port(0x05, kTxEnable | kRxEnable);
    pair.right.write_port(0x05, static_cast<std::uint8_t>(0xC0 | kTxEnable | kRxEnable));

    pair.left.write_port(0x03, 0xFF);
    pair.advance(11932u * 12u);

    check(pair.right.serial_frame_error() || !pair.right.serial_rx_ready(),
          "a baud mismatch does not silently deliver a wrong byte");
}

void test_unplugged_cable_is_quiet() {
    GearLinkCable cable;
    SystemControl alone;
    alone.reset();
    cable.set_connected(false);
    alone.attach_link(&cable, 0);

    alone.write_port(0x05, kTxEnable | kRxEnable);
    alone.write_port(0x03, 0x42);
    check(alone.serial_tx_busy(), "transmitting into an unplugged cable still takes time");
    for (int slice = 0; slice < 200; ++slice) {
        alone.advance_serial(64);
    }
    check(!alone.serial_tx_busy(), "and still finishes");
    check(!alone.serial_rx_ready(), "with nothing coming back");

    // Every pin floats high with nobody pulling it down.
    check_equal(alone.peek_port(0x01) & 0x7F, 0x7F, "an unplugged cable reads all ones");
}

void test_link_interrupt_fires_once_per_byte() {
    LinkedPair pair;
    pair.left.write_port(0x05, kTxEnable | kRxEnable);
    pair.right.write_port(0x05, kTxEnable | kRxEnable | kNmiEnable);

    check(!pair.right.take_link_nmi(), "no interrupt before anything arrives");

    pair.left.write_port(0x03, 0x9E);
    pair.advance(kFrameCycles);

    check(pair.right.take_link_nmi(), "a completed reception raises the link interrupt");
    check(!pair.right.take_link_nmi(), "and raises it once, not every instruction");
    check_equal(pair.right.read_port(0x04), 0x9E, "the byte behind the interrupt");
}

void test_aborting_mid_frame() {
    LinkedPair pair;
    pair.left.write_port(0x05, kTxEnable | kRxEnable);
    pair.right.write_port(0x05, kTxEnable | kRxEnable);

    pair.left.write_port(0x03, 0x33);
    pair.advance(kBitCycles * 3);
    // Clearing the transmit enable pulls the plug on a frame in flight.
    pair.left.write_port(0x05, kRxEnable);
    check(!pair.left.serial_tx_busy(), "clearing transmit enable aborts the frame");
    check((pair.left.read_port(0x05) & 0x01) == 0, "and the busy bit goes with it");

    pair.advance(kFrameCycles);
    // The receiver is not told about the abort and cannot infer it: an idle
    // line is high, and high is a one, so the rest of the frame arrives as
    // ones and the stop bit looks perfectly good. What it must NOT do is hand
    // over the byte that was being sent.
    if (pair.right.serial_rx_ready()) {
        check(pair.right.read_port(0x04) != 0x33, "an aborted frame does not arrive as the real byte");
    }

    // The line has to be left idle, or the next transmission's start bit would
    // never be seen as an edge.
    pair.left.write_port(0x05, kTxEnable | kRxEnable);
    pair.left.write_port(0x03, 0x33);
    pair.advance(kFrameCycles);
    check(pair.right.serial_rx_ready(), "the link recovers and the retry arrives");
    check_equal(pair.right.read_port(0x04), 0x33, "the retried byte");
}

void test_all_byte_values_survive_the_round_trip() {
    for (int value = 0; value < 256; ++value) {
        LinkedPair pair;
        pair.left.write_port(0x05, kTxEnable | kRxEnable);
        pair.right.write_port(0x05, kTxEnable | kRxEnable);
        pair.left.write_port(0x03, static_cast<std::uint8_t>(value));
        pair.advance(kFrameCycles);
        if (!pair.right.serial_rx_ready() ||
            pair.right.read_port(0x04) != static_cast<std::uint8_t>(value)) {
            ++g_failures;
            std::printf("FAIL: byte 0x%02X did not survive the cable\n", value);
            return;
        }
    }
}

}  // namespace

int main() {
    test_byte_crosses_the_cable();
    test_both_directions_at_once();
    test_status_bits_report_progress();
    test_disabled_receiver_hears_nothing();
    test_disabled_transmitter_sends_nothing();
    test_mismatched_baud_is_a_frame_error();
    test_unplugged_cable_is_quiet();
    test_link_interrupt_fires_once_per_byte();
    test_aborting_mid_frame();
    test_all_byte_values_survive_the_round_trip();

    if (g_failures == 0) {
        std::printf("gear link: all checks passed\n");
        return 0;
    }
    std::printf("gear link: %d check(s) failed\n", g_failures);
    return 1;
}
