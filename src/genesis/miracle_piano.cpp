#include "mercury/genesis/miracle_piano.hpp"

namespace mercury::genesis {

namespace {

// Which pin does what, on a Mega Drive controller port.
constexpr std::uint8_t kTh = 0x40;  // console -> keyboard: data, and the poll strobe
constexpr std::uint8_t kTr = 0x20;  // console -> keyboard: the bit clock
// The keyboard answers on TL. It is the only spare pin the console leaves as
// an input once it has claimed TH and TR, and the game reads the whole port
// immediately after each strobe.
constexpr std::uint8_t kTl = 0x10;

// Sysex framing. Every message the keyboard cares about is addressed to this
// manufacturer and device, which is what makes it safe to answer at all: the
// game is talking to a specific instrument, not broadcasting.
constexpr std::uint8_t kSysexStart = 0xF0;
constexpr std::uint8_t kSysexEnd = 0xF7;
constexpr std::uint8_t kSystemReset = 0xFF;
constexpr std::uint8_t kSysexHeader[5] = {0xF0, 0x00, 0x00, 0x42, 0x01};

}  // namespace

void MiraclePiano::reset() noexcept {
    queue_head_ = 0;
    queue_size_ = 0;
    previous_clock_ = false;
    previous_strobe_ = false;
    capture_byte_ = 0;
    capture_bits_ = 0;
    reply_active_ = false;
    reply_byte_ = 0;
    reply_bit_ = 0;
    clock_during_strobe_ = false;
    in_poll_ = false;
    read_since_clock_ = false;
    announcing_ = false;
    silence_pending_ = false;
    running_status_ = 0;
    midi_arg_count_ = 0;
    sound_head_ = 0;
    sound_size_ = 0;
    program_.fill(0);
    identity_pending_ = true;
    reset_parser();
    activity_ = Activity{};
}

// --- what the player is doing ------------------------------------------

void MiraclePiano::set_key(int key, bool pressed, std::uint8_t velocity) noexcept {
    if (key < 0 || key >= kKeyCount || key_[static_cast<std::size_t>(key)] == pressed) {
        return;
    }
    key_[static_cast<std::size_t>(key)] = pressed;
    velocity_[static_cast<std::size_t>(key)] = velocity != 0 ? velocity : 0x64;
    queue_note_event(static_cast<std::uint8_t>(kBaseNote + key),
                     velocity_[static_cast<std::size_t>(key)], pressed);
}

void MiraclePiano::set_control(Control control, bool pressed) noexcept {
    const auto index = static_cast<std::size_t>(control);
    if (index >= control_pressed_.size() || control_pressed_[index] == pressed) {
        return;
    }
    control_pressed_[index] = pressed;
    // The voice buttons report as notes above the top of the keyboard, which
    // is how the instrument distinguishes them from anything playable.
    queue_note_event(static_cast<std::uint8_t>(kBaseButtonNote + index), 0x64, pressed);
}

void MiraclePiano::set_sustain(bool pressed) noexcept {
    if (sustain_ == pressed) {
        return;
    }
    sustain_ = pressed;
    queue_sustain_event(pressed);
}

bool MiraclePiano::key_pressed(int key) const noexcept {
    return key >= 0 && key < kKeyCount && key_[static_cast<std::size_t>(key)];
}

// --- the outgoing queue --------------------------------------------------

void MiraclePiano::queue_byte(std::uint8_t value) noexcept {
    if (queue_size_ >= queue_.size()) {
        return;
    }
    queue_[(queue_head_ + queue_size_) % queue_.size()] = value;
    ++queue_size_;
}

void MiraclePiano::queue_bytes(const std::uint8_t* values, std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        queue_byte(values[index]);
    }
}

void MiraclePiano::queue_version_response() noexcept {
    static const std::uint8_t kResponse[] = {0xF0, 0x00, 0x00, 0x42, 0x01,
                                             0x05, 0x01, 0x0A, 0xF7};
    queue_bytes(kResponse, sizeof(kResponse));
}

void MiraclePiano::queue_memory_test_response() noexcept {
    static const std::uint8_t kResponse[] = {0xF0, 0x00, 0x00, 0x42, 0x01, 0x0B, 0x01, 0xF7};
    queue_bytes(kResponse, sizeof(kResponse));
}

void MiraclePiano::queue_note_event(std::uint8_t note, std::uint8_t velocity,
                                    bool pressed) noexcept {
    // Note-on for both, with velocity zero meaning release. That is what the
    // instrument itself sends, and the games are written against it.
    queue_byte(0x90);
    queue_byte(note);
    queue_byte(pressed ? velocity : 0x00);
}

void MiraclePiano::queue_sustain_event(bool pressed) noexcept {
    queue_byte(0xB0);
    queue_byte(0x40);
    queue_byte(pressed ? 0x7F : 0x00);
}

void MiraclePiano::flush() noexcept {
    queue_head_ = 0;
    queue_size_ = 0;
    reply_active_ = false;
    reply_bit_ = 0;
    announcing_ = false;
    identity_pending_ = true;
    // Anything sounding stops, and the byte being assembled is abandoned:
    // a reset is a clean line, so half a byte from before it is not part of
    // anything that follows.
    silence_pending_ = true;
    capture_byte_ = 0;
    capture_bits_ = 0;
    running_status_ = 0;
    midi_arg_count_ = 0;
    reset_parser();
}

// --- what the console sends ----------------------------------------------

void MiraclePiano::reset_parser() noexcept {
    sysex_active_ = false;
    sysex_length_ = 0;
    sysex_handled_this_message_ = false;
}

void MiraclePiano::handle_sysex() noexcept {
    if (sysex_length_ < 6) {
        return;
    }
    // Act once per message, not once per byte after the sixth. The command
    // byte arrives at length six and the message usually runs to seven or
    // eight, so without this a single request is handled two or three times
    // and any reply is queued that many times over.
    if (sysex_handled_this_message_) {
        return;
    }
    for (std::size_t index = 0; index < sizeof(kSysexHeader); ++index) {
        if (sysex_[index] != kSysexHeader[index]) {
            return;
        }
    }
    ++activity_.sysex_handled;
    // Record the vocabulary before acting on it. Only three of these
    // commands are answered, here or in either reference emulator, and a
    // command the game keeps asking and never gets an answer to is the
    // obvious place to look when it behaves as though the instrument were
    // misconfigured.
    sysex_handled_this_message_ = true;
    if (sysex_[5] < activity_.sysex_commands.size()) {
        ++activity_.sysex_commands[sysex_[5]];
    }
    if (activity_.sysex_sample_count < activity_.sysex_samples.size()) {
        const auto slot = activity_.sysex_sample_count++;
        const auto length = (std::min)(sysex_length_, activity_.sysex_samples[slot].size());
        for (std::size_t i = 0; i < length; ++i) {
            activity_.sysex_samples[slot][i] = sysex_[i];
        }
        activity_.sysex_sample_length[slot] = static_cast<std::uint8_t>(length);
    }
    switch (sysex_[5]) {
        case 0x04:  // who are you?
            queue_version_response();
            reset_parser();
            return;
        case 0x07:  // forget everything and start again
            flush();
            return;
        case 0x0B:  // self test
            queue_memory_test_response();
            reset_parser();
            return;
        // Command 0x02 is deliberately NOT answered.
        //
        // It is the only one this game actually sends - eighteen times over
        // 2,400 frames, roughly every two seconds - and neither reference
        // emulator answers it either. A reply was tried, shaped on the one
        // known request-and-answer pair in this protocol (04 is answered by
        // 05, so an even command asks and the next odd one answers), echoing
        // the parameter with a full-scale value on the theory that the game
        // was asking how the instrument is set. It did not change the game's
        // behaviour, so it was invented, not discovered, and inventing
        // protocol is worse than admitting the gap. Settling this needs the
        // game's own code for this message rather than more guesses at the
        // wire.
        default:
            break;
    }
    if (sysex_length_ >= sysex_.size()) {
        reset_parser();
    }
}

void MiraclePiano::push_sound_event(const SoundEvent& event) noexcept {
    if (sound_size_ >= sound_.size()) {
        // Drop the oldest rather than the newest: a note-off that never
        // arrives leaves a key sounding for ever.
        sound_head_ = (sound_head_ + 1) % sound_.size();
        --sound_size_;
    }
    sound_[(sound_head_ + sound_size_) % sound_.size()] = event;
    ++sound_size_;
}

void MiraclePiano::process_incoming_byte(std::uint8_t value) noexcept {
    ++activity_.bytes_from_console;
    activity_.last_from_console = value;
    if (activity_.first_count < activity_.first_from_console.size()) {
        activity_.first_from_console[activity_.first_count++] = value;
    }

    if (value == kSystemReset) {
        flush();
        return;
    }
    if (value == kSysexEnd) {
        reset_parser();
        return;
    }
    if (value == kSysexStart) {
        sysex_active_ = true;
        sysex_length_ = 0;
    }

    if (sysex_active_) {
        if (sysex_length_ >= sysex_.size()) {
            reset_parser();
            return;
        }
        sysex_[sysex_length_++] = value;
        handle_sysex();
        return;
    }

    // Ordinary MIDI, which is the game telling the instrument what to sound.
    // Running status: a data byte with no status in front of it belongs to
    // whatever status came last.
    if ((value & 0x80) != 0) {
        running_status_ = value;
        midi_arg_count_ = 0;
        return;
    }
    if (running_status_ == 0) {
        return;
    }
    midi_args_[static_cast<std::size_t>(midi_arg_count_)] = value;
    ++midi_arg_count_;

    const std::uint8_t kind = running_status_ & 0xF0;
    const std::uint8_t channel = running_status_ & 0x0F;

    if (kind == 0xC0) {  // program change: one argument
        program_[channel] = midi_args_[0];
        midi_arg_count_ = 0;
        return;
    }
    if (midi_arg_count_ < 2) {
        return;
    }
    midi_arg_count_ = 0;

    if (kind == 0x90) {
        push_sound_event(SoundEvent{channel, midi_args_[0], midi_args_[1], program_[channel]});
    } else if (kind == 0x80) {
        push_sound_event(SoundEvent{channel, midi_args_[0], 0, program_[channel]});
    } else if (kind == 0xB0) {
        // Control change. These were parsed and then dropped, which is what
        // left a note hanging when the title music was interrupted: leaving
        // a screen, the game does not send a note-off for every note it has
        // started, it sends ONE of these and expects the instrument to fall
        // silent. Ignoring them meant whatever was sounding kept sounding.
        switch (midi_args_[0]) {
            case 0x78:  // all sound off
            case 0x79:  // reset all controllers
            case 0x7B:  // all notes off
                silence_pending_ = true;
                break;
            default:
                break;
        }
    }
}

std::size_t MiraclePiano::take_sound_events(SoundEvent* out, std::size_t capacity) noexcept {
    std::size_t taken = 0;
    while (taken < capacity && sound_size_ > 0) {
        out[taken++] = sound_[sound_head_];
        sound_head_ = (sound_head_ + 1) % sound_.size();
        --sound_size_;
    }
    return taken;
}

// --- the wire ------------------------------------------------------------

void MiraclePiano::write_lines(std::uint8_t data, std::uint8_t control) noexcept {
    // Only pins the console has configured as outputs are being driven; the
    // rest are floating and mean nothing.
    const std::uint8_t driven = static_cast<std::uint8_t>(data & control);
    const bool clock = (driven & kTr) != 0;
    const bool strobe = (driven & kTh) != 0;

    // TR clocks BOTH directions, and which one depends on whether a reply is
    // in progress. Read off the wire: during a reply the console pulses TR
    // between reads to step to the next bit, and at every other time a TR
    // pulse is it shifting a bit of its own out on TH.
    if (clock && !previous_clock_) {
        clock_during_strobe_ = true;
        // A clock belongs to the answer only if it is inside a poll AND a
        // read came immediately before it. That pairing is what the console
        // actually does when collecting a reply - read, clock, read, clock -
        // and it is the only thing that separates those clocks from the
        // data bits the console shifts out, because a zero data bit puts
        // exactly the same levels on the wire.
        //
        // Both looser rules failed against the cartridge. Capturing every
        // clock turned each idle poll's trailing pulse into a data bit, so
        // eight idle polls manufactured a 00 byte and thirty of them showed
        // up in one run before a misplaced clock shifted the rest into
        // noise. Swallowing a flat eight clocks after every poll went the
        // other way and ate real data from the second byte onward.
        const bool answering = in_poll_ && read_since_clock_;
        read_since_clock_ = false;
        if (answering) {
            if (announcing_) {
                // The console has taken the "I have something" flag. The
                // eight data bits start now.
                announcing_ = false;
                reply_active_ = true;
                reply_bit_ = 0;
            } else if (reply_active_) {
                if (++reply_bit_ >= 8) {
                    reply_active_ = false;
                    reply_bit_ = 0;
                    in_poll_ = false;
                    ++activity_.bytes_to_console;
                    activity_.last_to_console = reply_byte_;
                    queue_head_ = (queue_head_ + 1) % queue_.size();
                    if (queue_size_ != 0) {
                        --queue_size_;
                    }
                }
            }
            // Nothing to say: the clock is still swallowed, and the
            // transaction stays open. Closing it after one clock was wrong.
            // The console does not always stop at one read, and every
            // further clock then fell through to the capture below and
            // became a zero bit - six thousand phantom bytes in a
            // nine-hundred-frame run, against under three hundred real
            // ones. Only the console transmitting ends a transaction.
        } else {
            // The console is shifting out a byte of its own, which means it
            // is not collecting an answer. THIS is the unambiguous end of a
            // reply - not the rising edge of the data line, which also
            // begins every poll and so cancelled each answer a moment after
            // it was offered.
            in_poll_ = false;
            announcing_ = false;
            reply_active_ = false;
            reply_bit_ = 0;
            capture_byte_ = static_cast<std::uint8_t>((capture_byte_ << 1) | (strobe ? 1u : 0u));
            if (++capture_bits_ >= 8) {
                process_incoming_byte(capture_byte_);
                capture_byte_ = 0;
                capture_bits_ = 0;
            }
        }
    }

    if (strobe && !previous_strobe_) {
        clock_during_strobe_ = false;
        if (cancel_on_strobe_) {
            in_poll_ = false;
            announcing_ = false;
            reply_active_ = false;
            reply_bit_ = 0;
        }
    }

    // The poll is a TH pulse with NO clock inside it. That distinction is the
    // whole trick: sending a one-bit ALSO raises and lowers TH, so an edge
    // alone cannot tell a question from a data bit. Measured on the real
    // cartridge, treating every TH fall as a poll produced 750 of them in
    // four hundred frames against roughly 59 genuine ones, and the invented
    // ones ate the reply.
    // A data-line pulse WITH a clock inside it was the console shifting out
    // a one. That is the other way to know it has stopped collecting an
    // answer, and it is needed as well as the capture branch below: if a
    // stray read makes a data clock look like an answer step, this catches
    // it on the falling edge and puts the reply back where it belongs.
    if (previous_strobe_ && !strobe && clock_during_strobe_) {
        in_poll_ = false;
        announcing_ = false;
        reply_active_ = false;
        reply_bit_ = 0;
    }

    if (previous_strobe_ && !strobe && !clock_during_strobe_) {
        ++activity_.polls;
        in_poll_ = true;
        read_since_clock_ = false;
        if (!reply_active_ && !announcing_) {
            if (queue_size_ == 0 && identity_pending_) {
                // Announce the instrument once, so a game that never asks
                // still discovers it is plugged in.
                queue_version_response();
                identity_pending_ = false;
            }
            if (queue_size_ != 0) {
                // Raise the flag rather than the first data bit. The
                // console reads this to decide whether to collect a byte at
                // all, which is why an idle poll is one read long and a
                // real answer is nine.
                announcing_ = true;
                reply_byte_ = queue_[queue_head_];
                reply_bit_ = 0;
            }
        }
    }

    previous_clock_ = clock;
    previous_strobe_ = strobe;
}

std::uint8_t MiraclePiano::read_lines(std::uint8_t control) noexcept {
    // Present the current bit. Advancing is the clock's job, not the read's,
    // so the console can look at the same bit twice without losing it.
    // Most significant first and inverted on the wire: an undriven line reads
    // high, so an un-inverted zero byte would be indistinguishable from an
    // empty socket.
    // While announcing, the line simply says "yes, a byte is waiting". Once
    // the console has acknowledged that with a clock, the same line carries
    // the byte, most significant bit first and inverted.
    const bool line =
        announcing_ || (reply_active_ && ((~reply_byte_ >> (7 - reply_bit_)) & 1) != 0);
    // Pairs this read with the clock that follows it, which is how an
    // answer's clocks are told apart from data bits.
    read_since_clock_ = true;

    // Every pin the console is not driving floats high; the keyboard pulls
    // the reply pins down to signal.
    std::uint8_t value = static_cast<std::uint8_t>(~control & 0x7F);
    const bool assert_low = reply_inverted_ ? line : !line;
    if (assert_low) {
        value &= static_cast<std::uint8_t>(~(reply_mask_ & ~control));
    } else {
        value |= static_cast<std::uint8_t>(reply_mask_ & ~control);
    }
    return value;
}

}  // namespace mercury::genesis
