// Does the Miracle Piano actually hold a conversation?
//
// The wire was measured off the real cartridge: TR clocks, TH carries the
// data, MSB first, and the keyboard answers on D0. These drive the device the
// way the console does and check that a byte sent arrives, that a question
// gets the right answer, and that pressing a key produces a note the game
// could read.

#include "mercury/genesis/miracle_piano.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using mercury::genesis::MiraclePiano;

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

// The console's side of the cable. Port 2's control register is 0x60, so TH
// and TR are outputs and everything else is an input.
constexpr std::uint8_t kControl = 0x60;
constexpr std::uint8_t kTh = 0x40;
constexpr std::uint8_t kTr = 0x20;
constexpr std::uint8_t kD0 = 0x01;

// Clock one byte out to the keyboard, most significant bit first: set TH to
// the bit, pulse TR.
void send_byte(MiraclePiano& piano, std::uint8_t value) {
    for (int bit = 7; bit >= 0; --bit) {
        const std::uint8_t data = ((value >> bit) & 1) != 0 ? kTh : 0x00;
        piano.write_lines(data, kControl);
        piano.write_lines(static_cast<std::uint8_t>(data | kTr), kControl);
        piano.write_lines(data, kControl);
    }
    piano.write_lines(0x00, kControl);
}

void send_bytes(MiraclePiano& piano, const std::vector<std::uint8_t>& values) {
    for (const auto value : values) {
        send_byte(piano, value);
    }
}

// The poll: a TH pulse with NO clock inside it. That is what makes it a
// question rather than a data bit - sending a one-bit raises and lowers TH
// as well, and only a data bit has a TR pulse between the two edges.
void poll(MiraclePiano& piano) {
    piano.write_lines(kTh, kControl);
    piano.write_lines(0x00, kControl);
}

// Read one byte back the way the console does.
//
// Ask, then read the FLAG. The keyboard raises the line to say a byte is
// waiting; if it does not, the console stops there. That is why the real
// cartridge shows one read per idle poll and nine when it is collecting -
// a flag and then eight bits. Reading eight times regardless, which this
// harness used to do, pushed seven phantom clocks into the keyboard and
// corrupted everything that followed.
// The line is OPEN DRAIN: nothing driving it reads high, and the keyboard
// signals by pulling it low. So a low read is the flag, and a low data bit
// is a one after inversion.
bool receive_byte(MiraclePiano& piano, std::uint8_t& out) {
    poll(piano);
    const bool waiting = (piano.read_lines(kControl) & kD0) == 0;
    piano.write_lines(kTr, kControl);
    piano.write_lines(0x00, kControl);
    if (!waiting) {
        return false;
    }
    std::uint8_t value = 0;
    for (int bit = 0; bit < 8; ++bit) {
        const bool pulled_low = (piano.read_lines(kControl) & kD0) == 0;
        // Inverted on the wire, most significant first: pulled low is a
        // zero bit, released is a one.
        value = static_cast<std::uint8_t>((value << 1) | (pulled_low ? 0u : 1u));
        piano.write_lines(kTr, kControl);
        piano.write_lines(0x00, kControl);
    }
    out = value;
    return true;
}

std::vector<std::uint8_t> drain(MiraclePiano& piano, int limit = 32) {
    std::vector<std::uint8_t> got;
    std::uint8_t byte = 0;
    while (static_cast<int>(got.size()) < limit && receive_byte(piano, byte)) {
        got.push_back(byte);
    }
    return got;
}

void test_console_bytes_arrive() {
    MiraclePiano piano;
    piano.reset();
    // The exact opening the real cartridge sends, decoded off the wire.
    send_bytes(piano, {0xC1, 0x64, 0xFF});
    const auto& activity = piano.activity();
    check(activity.bytes_from_console >= 3, "the keyboard decoded the bytes the console sent");
    check_equal(activity.last_from_console, 0xFF, "the last byte in");
}

void test_identity_is_announced_once() {
    MiraclePiano piano;
    piano.reset();
    // Nothing asked, but the keyboard announces itself on the first poll so a
    // game that only listens still finds it.
    const auto reply = drain(piano);
    const std::vector<std::uint8_t> expected{0xF0, 0x00, 0x00, 0x42, 0x01, 0x05, 0x01, 0x0A, 0xF7};
    check(reply == expected, "the keyboard identifies itself unprompted");

    // ...and does not keep repeating it.
    const auto again = drain(piano);
    check(again.empty(), "the identity is sent once, not on every poll");
}

void test_version_request_is_answered() {
    MiraclePiano piano;
    piano.reset();
    (void)drain(piano);  // clear the unprompted identity
    send_bytes(piano, {0xF0, 0x00, 0x00, 0x42, 0x01, 0x04, 0xF7});
    const auto reply = drain(piano);
    const std::vector<std::uint8_t> expected{0xF0, 0x00, 0x00, 0x42, 0x01, 0x05, 0x01, 0x0A, 0xF7};
    check(reply == expected, "a version request gets the version back");
}

void test_memory_test_is_answered() {
    MiraclePiano piano;
    piano.reset();
    (void)drain(piano);
    send_bytes(piano, {0xF0, 0x00, 0x00, 0x42, 0x01, 0x0B, 0xF7});
    const auto reply = drain(piano);
    const std::vector<std::uint8_t> expected{0xF0, 0x00, 0x00, 0x42, 0x01, 0x0B, 0x01, 0xF7};
    check(reply == expected, "the self test reports a pass");
}

void test_reset_clears_the_queue() {
    MiraclePiano piano;
    piano.reset();
    piano.set_key(0, true);
    // A system reset throws away anything waiting, so the console never gets
    // a note that belongs to before the reset.
    send_bytes(piano, {0xFF});
    const auto reply = drain(piano);
    check(!reply.empty() && reply[0] == 0xF0,
          "after a reset the keyboard starts again by identifying itself");
}

void test_a_pressed_key_reaches_the_console() {
    MiraclePiano piano;
    piano.reset();
    (void)drain(piano);

    piano.set_key(0, true, 0x50);
    const auto pressed = drain(piano);
    const std::vector<std::uint8_t> expected_on{0x90, MiraclePiano::kBaseNote, 0x50};
    check(pressed == expected_on, "pressing the lowest key sends note-on with its velocity");

    piano.set_key(0, false);
    const auto released = drain(piano);
    const std::vector<std::uint8_t> expected_off{0x90, MiraclePiano::kBaseNote, 0x00};
    check(released == expected_off, "releasing it sends the same note at velocity zero");
}

void test_every_key_maps_to_its_own_note() {
    MiraclePiano piano;
    piano.reset();
    (void)drain(piano);
    for (int key = 0; key < MiraclePiano::kKeyCount; ++key) {
        piano.set_key(key, true);
        const auto reply = drain(piano);
        if (reply.size() != 3 || reply[1] != MiraclePiano::kBaseNote + key) {
            ++g_failures;
            std::printf("FAIL: key %d did not send note 0x%02X\n", key,
                        MiraclePiano::kBaseNote + key);
            return;
        }
        piano.set_key(key, false);
        (void)drain(piano);
    }
}

void test_sustain_pedal() {
    MiraclePiano piano;
    piano.reset();
    (void)drain(piano);
    piano.set_sustain(true);
    const auto down = drain(piano);
    const std::vector<std::uint8_t> expected{0xB0, 0x40, 0x7F};
    check(down == expected, "the sustain pedal sends controller 64 at full");
}

void test_the_game_can_drive_the_instrument() {
    MiraclePiano piano;
    piano.reset();
    // The game plays notes THROUGH the keyboard, which is where the sound in
    // a lesson comes from. Those have to come back out as events something
    // can synthesise.
    send_bytes(piano, {0xC0, 0x05});        // program change on channel 0
    send_bytes(piano, {0x90, 0x40, 0x7F});  // note on
    send_bytes(piano, {0x90, 0x40, 0x00});  // and off again

    MiraclePiano::SoundEvent events[8];
    const auto count = piano.take_sound_events(events, 8);
    check(count == 2, "the two note events came through");
    if (count >= 2) {
        check_equal(events[0].note, 0x40, "the note the game asked for");
        check_equal(events[0].velocity, 0x7F, "at full velocity");
        check_equal(events[0].program, 0x05, "with the program it selected");
        check_equal(events[1].velocity, 0x00, "and then released");
    }
}

void test_running_status() {
    MiraclePiano piano;
    piano.reset();
    // MIDI lets a sender leave the status byte off when it has not changed.
    // A parser that ignores that drops most of what the game sends.
    send_bytes(piano, {0x90, 0x3C, 0x64});
    send_bytes(piano, {0x3E, 0x64});
    send_bytes(piano, {0x40, 0x64});

    MiraclePiano::SoundEvent events[8];
    const auto count = piano.take_sound_events(events, 8);
    check(count == 3, "all three notes were parsed under running status");
    if (count >= 3) {
        check_equal(events[1].note, 0x3E, "the second note");
        check_equal(events[2].note, 0x40, "the third note");
    }
}

}  // namespace

int main() {
    test_console_bytes_arrive();
    test_identity_is_announced_once();
    test_version_request_is_answered();
    test_memory_test_is_answered();
    test_reset_clears_the_queue();
    test_a_pressed_key_reaches_the_console();
    test_every_key_maps_to_its_own_note();
    test_sustain_pedal();
    test_the_game_can_drive_the_instrument();
    test_running_status();

    if (g_failures == 0) {
        std::printf("miracle piano: all checks passed\n");
        return 0;
    }
    std::printf("miracle piano: %d check(s) failed\n", g_failures);
    return 1;
}
