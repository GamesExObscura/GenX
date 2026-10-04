#pragma once

#include <array>
#include <cstdint>

namespace mercury::genesis {

// The Miracle Piano Teaching System keyboard, on a Mega Drive controller port.
//
// The keyboard is a MIDI instrument with a console cable. The game teaches you
// to play; the keyboard tells it which keys you pressed and how hard, and the
// game sends back the notes it wants sounded. Without the physical instrument
// the game cannot be used at all, which is why emulators grow an on-screen one.
//
// **It does not use the Mega Drive's serial hardware.** Measured on the real
// cartridge: the nine UART registers are never touched. The game writes the
// port-2 control register with 0x60, taking TH (bit 6) and TR (bit 5) as
// outputs and leaving the rest as inputs, and then bit-bangs.
//
//   TR rising edge  -> clocks one bit, MSB first
//   TH              -> that bit's value, held either side of the pulse
//
// The bytes that come out are ordinary MIDI. Decoded from a real run:
//
//   C1 64                     program change
//   FF                        system reset
//   F0 00 00 42 01 07 F7      sysex reset
//   B0 7A 7F                  control change, local control
//
// which is byte-for-byte the same conversation the SNES and NES versions have.
// That is what makes the two reference emulators useful here: the wire below
// is different on every machine, but everything above it is one protocol.
class MiraclePiano {
public:
    static constexpr int kKeyCount = 49;
    static constexpr int kButtonCount = 8;
    // The lowest key on the keyboard, as a MIDI note number.
    static constexpr std::uint8_t kBaseNote = 0x24;
    // The voice buttons report as notes above the top of the keyboard. This
    // is NOT simply the note after the last key: both reference emulators
    // put them at 0x58, three higher than the keys end, and a game matching
    // on the exact number would ignore anything else.
    static constexpr std::uint8_t kBaseButtonNote = 0x58;

    // The six voice buttons along the top, then the two that transpose.
    enum class Control : int {
        piano,
        harpsichord,
        organ,
        vibraphone,
        electric_piano,
        synthesizer,
        up,
        down,
    };

    void reset() noexcept;

    // --- what the player is doing ----------------------------------------
    void set_key(int key, bool pressed, std::uint8_t velocity = 0x64) noexcept;
    void set_control(Control control, bool pressed) noexcept;
    void set_sustain(bool pressed) noexcept;
    [[nodiscard]] bool key_pressed(int key) const noexcept;

    // --- the wire ---------------------------------------------------------
    // Called whenever the console writes the port's data register. Only the
    // pins it has configured as outputs mean anything, which is why the
    // control register comes in too.
    void write_lines(std::uint8_t data, std::uint8_t control) noexcept;
    // What the console reads back on the pins it is NOT driving.
    [[nodiscard]] std::uint8_t read_lines(std::uint8_t control) noexcept;

    // --- what the game is asking the keyboard to sound --------------------
    //
    // The game drives the instrument as well as listening to it, so the notes
    // it sends are the ones the on-screen keyboard has to play. Exposed as a
    // simple note-on/note-off stream for whatever is making the sound.
    struct SoundEvent {
        std::uint8_t channel = 0;
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;  // zero means release
        std::uint8_t program = 0;
    };
    // Drains up to `capacity` pending events, returning how many were written.
    [[nodiscard]] std::size_t take_sound_events(SoundEvent* out, std::size_t capacity) noexcept;

    // The console told the instrument to shut up: a MIDI system reset, or the
    // sysex that flushes it. Whatever was sounding stops.
    //
    // A safety net as much as a feature. If the byte stream ever loses its
    // framing, the garbage can decode as a note-on whose note-off never
    // comes, and the note then sounds for ever - which is exactly what
    // happened once. A reset is the one moment the game guarantees silence,
    // so honouring it means a stuck note can always be cleared.
    [[nodiscard]] bool take_silence_request() noexcept {
        const bool pending = silence_pending_;
        silence_pending_ = false;
        return pending;
    }

    // Diagnostics. Whether the game has recognised the keyboard is the single
    // most useful thing to know while this is being built.
    struct Activity {
        std::uint64_t bytes_from_console = 0;
        std::uint64_t bytes_to_console = 0;
        std::uint64_t polls = 0;
        std::uint64_t sysex_handled = 0;
        std::uint8_t last_from_console = 0;
        std::uint8_t last_to_console = 0;
        // The first bytes the device believed it received. Clean MIDI here
        // and garbage out of the parser means the parser is wrong; garbage
        // here means the bits are being lost on the wire.
        std::array<std::uint8_t, 64> first_from_console{};
        std::size_t first_count = 0;
        // Which sysex commands the game has sent, and how often. The
        // vocabulary is the thing worth knowing: a command that arrives
        // repeatedly and gets no answer is a question the instrument is
        // failing to answer, and the reference emulators only ever handle
        // three of them.
        std::array<std::uint64_t, 128> sysex_commands{};
        // The full body of the first few, since a command's ARGUMENT is
        // often the whole point.
        std::array<std::array<std::uint8_t, 16>, 8> sysex_samples{};
        std::array<std::uint8_t, 8> sysex_sample_length{};
        std::size_t sysex_sample_count = 0;
    };
    [[nodiscard]] const Activity& activity() const noexcept { return activity_; }

    // Which input pin carries the reply, and which way round.
    //
    // The outbound half of this link is measured and certain. The return path
    // is not: the console leaves TL and the four data pins as inputs, and
    // nothing observed so far says which of them the keyboard answers on.
    // Rather than hard-code a guess, this is settable so the question can be
    // put to the game itself - it is the only oracle there is.
    void set_reply_mask(std::uint8_t mask) noexcept { reply_mask_ = mask & 0x1F; }
    void set_reply_inverted(bool inverted) noexcept { reply_inverted_ = inverted; }
    // Cancel a reply the moment the console raises the data line.
    //
    // Two readings of the wire are possible and they cannot both be right.
    // Cancelling gives a perfectly clean inbound stream - real music, no
    // stuck notes - but no answer ever survives long enough to be
    // collected. Not cancelling lets the keyboard answer but lets a reply
    // eat the console's own data clocks, which corrupts the stream. Left
    // switchable so the two can be compared against the cartridge rather
    // than argued about.
    void set_cancel_on_strobe(bool cancel) noexcept { cancel_on_strobe_ = cancel; }

private:
    void queue_byte(std::uint8_t value) noexcept;
    void queue_bytes(const std::uint8_t* values, std::size_t count) noexcept;
    void queue_version_response() noexcept;
    void queue_memory_test_response() noexcept;
    void queue_note_event(std::uint8_t note, std::uint8_t velocity, bool pressed) noexcept;
    void queue_sustain_event(bool pressed) noexcept;
    void process_incoming_byte(std::uint8_t value) noexcept;
    void handle_sysex() noexcept;
    void reset_parser() noexcept;
    void flush() noexcept;
    void push_sound_event(const SoundEvent& event) noexcept;

    // Outgoing to the console: a ring of MIDI bytes waiting to be clocked out
    // a bit at a time.
    std::array<std::uint8_t, 512> queue_{};
    std::size_t queue_head_ = 0;
    std::size_t queue_size_ = 0;

    // Incoming from the console, assembled bit by bit off the TR clock.
    bool previous_clock_ = false;
    bool previous_strobe_ = false;
    std::uint8_t capture_byte_ = 0;
    int capture_bits_ = 0;

    // The byte being handed back, one bit per poll.
    bool reply_active_ = false;
    std::uint8_t reply_byte_ = 0;
    int reply_bit_ = 0;
    // Whether a clock pulse happened inside the current TH pulse. That is
    // what separates a poll from a data bit: both raise and lower TH, and
    // only a data bit has a clock inside it.
    bool clock_during_strobe_ = false;
    // Whether a poll is still being answered, and whether the console has
    // read since the last clock. A clock counts towards the answer only
    // when both are true.
    bool in_poll_ = false;
    bool read_since_clock_ = false;
    // Holding the line up to say a byte is waiting, before any of it has
    // been sent.
    bool announcing_ = false;
    bool cancel_on_strobe_ = false;
    bool silence_pending_ = false;
    // D0. Measured, not assumed: with the reply put on each candidate pin in
    // turn, this is the ONLY one the game reacts to - port-2 reads jump from
    // 358 to 586 over the same run while writes fall, which is the game
    // switching from transmitting into silence to actually listening. TL and
    // the other three data pins changed nothing whatsoever.
    std::uint8_t reply_mask_ = 0x01;
    // Inverted, because the line is open drain: nothing driving it reads
    // HIGH through the pull-up, and the keyboard signals by pulling it
    // LOW. Getting this backwards did not simply mute the keyboard - it
    // corrupted the console's own byte stream, because the game reacted to
    // a line it thought was permanently asserted. With it the right way
    // round the inbound stream stays clean MIDI (294 bytes and twelve sysex
    // messages over nine hundred frames) AND the keyboard answers; with it
    // wrong, the same run captured 302 bytes of which most were noise and
    // only three sysex survived.
    bool reply_inverted_ = true;

    // Sysex reassembly.
    bool sysex_active_ = false;
    bool sysex_handled_this_message_ = false;
    std::array<std::uint8_t, 16> sysex_{};
    std::size_t sysex_length_ = 0;
    // The keyboard announces itself once when the console starts listening;
    // without that the game never learns it is there.
    bool identity_pending_ = true;

    // MIDI running status and per-channel program, for turning the game's
    // stream back into note events.
    std::uint8_t running_status_ = 0;
    std::array<std::uint8_t, 16> program_{};
    std::array<std::uint8_t, 2> midi_args_{};
    int midi_arg_count_ = 0;

    std::array<SoundEvent, 64> sound_{};
    std::size_t sound_head_ = 0;
    std::size_t sound_size_ = 0;

    std::array<bool, kKeyCount> key_{};
    std::array<std::uint8_t, kKeyCount> velocity_{};
    std::array<bool, kButtonCount> control_pressed_{};
    bool sustain_ = false;

    Activity activity_{};
};

}  // namespace mercury::genesis
