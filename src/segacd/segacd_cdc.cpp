// The CD data controller, a Sanyo LC8951.
//
// It sits between the drive and the rest of the machine. Every block the drive
// reads passes through its decoder, which checks the header and - if asked -
// writes the block into a 16KB buffer. From there the sub CPU has it moved:
// by DMA into program RAM, Word RAM or PCM wave RAM, or a word at a time
// through a data register either processor can read.
//
// The sub CPU drives it through two gate-array registers: $05 selects one of
// the chip's internal registers and $07 reads or writes it, the address moving
// on by one after each access. The register rules and the interrupt edges are
// Genesis Plus GX's, several of them verified against Krikzz's mcd-verificator
// test cartridge on real hardware.

#include <algorithm>

#include "mercury/segacd/segacd.hpp"

namespace mercury::segacd {

namespace {

// IFSTAT: the chip's status bits are ACTIVE LOW.
constexpr std::uint8_t kDtei = 0x40;   // data transfer end interrupt
constexpr std::uint8_t kDeci = 0x20;   // decoder interrupt
constexpr std::uint8_t kDtbsy = 0x08;  // transfer busy
constexpr std::uint8_t kDten = 0x02;   // transfer enabled

// IFCTRL
constexpr std::uint8_t kDteien = 0x40;
constexpr std::uint8_t kDecien = 0x20;
constexpr std::uint8_t kDouten = 0x02;

// CTRL0 / CTRL1
constexpr std::uint8_t kDecen = 0x80;
constexpr std::uint8_t kAutorq = 0x10;
constexpr std::uint8_t kWrrq = 0x04;
constexpr std::uint8_t kModrq = 0x08;
constexpr std::uint8_t kFormrq = 0x04;
constexpr std::uint8_t kShdren = 0x01;

// A DMA moves one byte every four sub-CPU cycles at best (MiSTer's logic
// analyser captures, via Genesis Plus GX). Memory contention on the far side
// is not modelled.
constexpr std::uint32_t kCyclesPerByte = 4;
// How long the decoder holds its interrupt after a block: 267,000 of the CD
// unit's 50MHz clocks, which is a quarter of that in sub-CPU cycles.
constexpr std::uint64_t kDecoderWindow = 267'000 / 4;

}  // namespace

void SegaCd::cdc_reset() noexcept {
    set_lo(0x02, 0x00);  // register address
    cdc_.ifstat = 0xFF;
    cdc_.ifctrl = 0x00;
    cdc_.ctrl = {0, 0};
    cdc_.stat = {0, 0, 0, 0x80};
    cdc_.head[0] = {0, 0, 0, 0x01};
    cdc_.head[1] = {0, 0, 0, 0x00};
    cdc_.dma = 0;
    cdc_.halted_dma = 0;
    cdc_.dma_clock = 0;
    cdc_.decoder_until = 0;
    cdc_.irq = 0;
    if ((pending_ & (1u << 5)) != 0) {
        pending_ &= static_cast<std::uint8_t>(~(1u << 5));
        update_sub_interrupt();
    }
}

// /INT is the OR of the enabled, pending sources, and level 5 fires on its
// FALLING edge only - a second source arriving while the first still holds
// the line produces no new interrupt.
void SegaCd::cdc_raise_if_edge(std::uint8_t previous_irq) noexcept {
    if (cdc_.irq != 0 && previous_irq == 0 && (lo(0x19) & 0x20) != 0) {
        raise(5);
    }
}

void SegaCd::cdc_update_decoder_window() noexcept {
    // The decoder's interrupt only stands for a while after each block.
    if (now_ > cdc_.decoder_until) {
        cdc_.ifstat |= kDeci;
        cdc_.irq &= static_cast<std::uint8_t>(~kDeci);
    }
}

void SegaCd::cdc_finish_transfer() noexcept {
    cdc_.dbc = 0xFFFF;
    cdc_.ifstat |= kDtbsy | kDten;
    cdc_.ifstat &= static_cast<std::uint8_t>(~kDtei);
    if ((cdc_.ifctrl & kDteien) != 0) {
        if ((cdc_.irq & kDeci) != 0 && cdc_.dma_clock > cdc_.decoder_until) {
            cdc_.ifstat |= kDeci;
            cdc_.irq &= static_cast<std::uint8_t>(~kDeci);
        }
        if (cdc_.irq == 0 && (lo(0x19) & 0x20) != 0) {
            raise(5);
        }
        cdc_.irq |= kDtei;
    }
}

// Arm whatever the $04 destination bits name. A no-op while no transfer has
// been started (DTEN still high).
void SegaCd::cdc_start_transfer() noexcept {
    if ((cdc_.ifstat & kDten) != 0) {
        return;
    }
    cdc_.dma = 0;
    cdc_.halted_dma = 0;
    const std::uint8_t destination = hi(0x02) & 0x07;
    switch (destination) {
        case 2:    // main CPU reads it
        case 3: {  // sub CPU reads it
            // The first word is fetched into the data register at once.
            const std::uint32_t at = cdc_.dac & 0x3FFE;
            regs_[0x04] = static_cast<std::uint16_t>((cdc_.ram[at] << 8) | cdc_.ram[at + 1]);
            set_hi(0x02, static_cast<std::uint8_t>(hi(0x02) | 0x40));  // DSR
            cdc_.dac = static_cast<std::uint16_t>(cdc_.dac + 2);
            cdc_.dbc = static_cast<std::uint16_t>(cdc_.dbc - 2);
            if (static_cast<std::int16_t>(cdc_.dbc) < 0) {
                cdc_finish_transfer();
                set_hi(0x02, static_cast<std::uint8_t>(hi(0x02) | 0x80));  // EDT
            }
            break;
        }
        case 4:  // PCM wave RAM
        case 5:  // program RAM
        case 7:  // Word RAM
            cdc_.dma = destination;
            break;
        default:
            break;  // not a valid destination
    }
}

std::uint16_t SegaCd::cdc_host_read(bool main_side) noexcept {
    const std::uint16_t data = regs_[0x04];
    // Only the processor the transfer was set up for moves it along.
    const std::uint8_t wanted = main_side ? 0x42 : 0x43;
    if ((hi(0x02) & 0x47) != wanted) {
        return data;
    }
    if ((hi(0x02) & 0x80) != 0) {
        // Finished: the last word has been read, so the data is no longer ready.
        set_hi(0x02, static_cast<std::uint8_t>(hi(0x02) & ~0x40));
        return data;
    }
    const std::uint32_t at = cdc_.dac & 0x3FFE;
    regs_[0x04] = static_cast<std::uint16_t>((cdc_.ram[at] << 8) | cdc_.ram[at + 1]);
    cdc_.dac = static_cast<std::uint16_t>(cdc_.dac + 2);
    cdc_.dbc = static_cast<std::uint16_t>(cdc_.dbc - 2);
    if (static_cast<std::int16_t>(cdc_.dbc) < 0) {
        cdc_finish_transfer();
        set_hi(0x02, static_cast<std::uint8_t>(hi(0x02) | 0x80));  // EDT
    }
    return data;
}

void SegaCd::cdc_run_dma(std::uint64_t now) noexcept {
    // A destination the other processor currently holds makes the DMA wait:
    // program RAM while the main CPU has the sub's bus, and 2M Word RAM while
    // it belongs to the main CPU. The clock is held so no burst builds up.
    const bool blocked = (cdc_.dma == 5 && (lo(0x00) & 0x02) != 0) ||
                         (cdc_.dma == 7 && !one_meg_mode() && main_owns_word_ram());
    if (blocked || now <= cdc_.dma_clock) {
        if (blocked) cdc_.dma_clock = now;
        return;
    }
    std::uint64_t bytes = (now - cdc_.dma_clock + kCyclesPerByte - 1) / kCyclesPerByte;
    bytes = (bytes / 8) * 8;  // always in blocks of eight bytes
    bool finished = false;
    if (static_cast<std::uint64_t>(cdc_.dbc) < bytes) {
        bytes = static_cast<std::uint64_t>(cdc_.dbc) + 1;
        finished = true;
    }
    if (bytes == 0) {
        return;
    }

    const auto count = static_cast<std::uint32_t>(bytes);
    if (cdc_.dma == 4) {
        // PCM wave RAM, a byte at a time into the selected 4KB bank.
        std::uint32_t source = cdc_.dac & 0x3FFF;
        std::uint32_t target = (static_cast<std::uint32_t>(regs_[0x05]) << 2) & 0xFFF;
        regs_[0x05] = static_cast<std::uint16_t>(regs_[0x05] + (count >> 2));
        cdc_.dac = static_cast<std::uint16_t>(cdc_.dac + count);
        for (std::uint32_t i = 0; i < count; ++i) {
            pcm_ram_[pcm_bank_ + target] = cdc_.ram[source];
            source = (source + 1) & 0x3FFF;
            target = (target + 1) & 0xFFF;
        }
    } else {
        // Program RAM and Word RAM take words. The address register counts
        // in eight-byte units.
        const std::uint32_t words = count >> 1;
        std::uint32_t source = cdc_.dac & 0x3FFE;
        std::uint32_t target = static_cast<std::uint32_t>(regs_[0x05]) << 3;
        regs_[0x05] = static_cast<std::uint16_t>(regs_[0x05] + (words >> 2));
        cdc_.dac = static_cast<std::uint16_t>(cdc_.dac + (words << 1));
        for (std::uint32_t i = 0; i < words; ++i) {
            const std::uint8_t high = cdc_.ram[source];
            const std::uint8_t low = cdc_.ram[source + 1];
            if (cdc_.dma == 5) {
                const std::uint32_t o = target & 0x7FFFE;
                prg_ram_[o] = high;
                prg_ram_[o + 1] = low;
            } else if (one_meg_mode()) {
                const std::uint32_t o = target & 0x1FFFE;
                word_ram_[bank_offset(sub_bank(), o)] = high;
                word_ram_[bank_offset(sub_bank(), o + 1)] = low;
            } else {
                const std::uint32_t o = target & 0x3FFFE;
                word_ram_[o] = high;
                word_ram_[o + 1] = low;
            }
            source = (source + 2) & 0x3FFE;
            target += 2;
        }
    }
    cdc_.dma_clock += bytes * kCyclesPerByte;
    if (finished) {
        cdc_finish_transfer();
        // DSR clear, EDT set.
        set_hi(0x02, static_cast<std::uint8_t>((hi(0x02) & 0x07) | 0x80));
        cdc_.dma = 0;
        cdc_.halted_dma = 0;
    } else {
        cdc_.dbc = static_cast<std::uint16_t>(cdc_.dbc - count);
    }
}

void SegaCd::cdc_decode_block(std::uint32_t header) noexcept {
    if ((cdc_.ctrl[0] & kDecen) == 0) {
        return;
    }
    ++sectors_decoded_;
    cdc_.head[0] = {static_cast<std::uint8_t>(header >> 24), static_cast<std::uint8_t>(header >> 16),
                    static_cast<std::uint8_t>(header >> 8), static_cast<std::uint8_t>(header)};
    cdc_.stat[3] = 0x00;  // !VALST: a block is ready
    cdc_.ifstat &= static_cast<std::uint8_t>(~kDeci);
    cdc_.decoder_until = now_ + kDecoderWindow;
    if ((cdc_.ifctrl & kDecien) != 0) {
        // Only DTEI can already be holding the line: DECI clears itself
        // between blocks.
        if ((cdc_.irq & kDtei) == 0 && (lo(0x19) & 0x20) != 0) {
            raise(5);
        }
        cdc_.irq |= kDeci;
    }

    if ((cdc_.ctrl[0] & kWrrq) == 0) {
        return;  // decode only, no buffer write
    }
    cdc_.pt = static_cast<std::uint16_t>(cdc_.pt + 2352);
    cdc_.wa = static_cast<std::uint16_t>(cdc_.wa + 2352);
    std::uint32_t offset = cdc_.pt & 0x3FFF;
    for (int i = 0; i < 4; ++i) {
        cdc_.ram[offset + static_cast<std::uint32_t>(i)] = cdc_.head[0][static_cast<std::size_t>(i)];
    }
    offset += 4;
    if (cdc_.head[0][3] == 0x01) {
        (void)disc_.read_data(cdd_lba_, cdc_.ram.data() + offset);
        offset += 2048;
    } else {
        std::uint8_t sub[4] = {};
        if ((cdc_.ctrl[1] & kModrq) != 0) {
            // Mode 2 decoding: the sub-header twice, then the data.
            (void)disc_.read_data(cdd_lba_, cdc_.ram.data() + offset + 8, sub);
            for (int copy = 0; copy < 2; ++copy) {
                for (int i = 0; i < 4; ++i) {
                    cdc_.ram[offset + static_cast<std::uint32_t>(copy * 4 + i)] = sub[i];
                }
            }
            offset += 2336;
        } else {
            // Without Mode 2 decoding the sub-header is NOT written (Wonder
            // Library needs that).
            (void)disc_.read_data(cdd_lba_, cdc_.ram.data() + offset, sub);
            offset += 2328;
        }
        cdc_.head[1] = {sub[0], sub[1], sub[2], sub[3]};
        if ((cdc_.ctrl[0] & kAutorq) != 0) {
            cdc_.stat[2] = static_cast<std::uint8_t>((cdc_.ctrl[1] & kModrq) | ((cdc_.head[1][2] & 0x20) >> 3));
        }
    }
    // A block that ran past the end of the buffer wraps to the start.
    if (offset > 0x4000) {
        std::copy(cdc_.ram.begin() + 0x4000, cdc_.ram.begin() + offset, cdc_.ram.begin());
    }
}

void SegaCd::cdc_register_write(std::uint8_t value) noexcept {
    const std::uint8_t reg = lo(0x02) & 0x0F;
    switch (reg) {
        case 0x01: {  // IFCTRL
            std::uint8_t previous = cdc_.irq;
            if (now_ > cdc_.decoder_until) {
                cdc_.ifstat |= kDeci;
                previous &= static_cast<std::uint8_t>(~kDeci);
            }
            cdc_.irq = static_cast<std::uint8_t>(~cdc_.ifstat & value & (kDteien | kDecien));
            cdc_raise_if_edge(previous);
            if ((value & kDouten) == 0) {
                // Output disabled aborts any transfer.
                cdc_.ifstat |= kDtbsy | kDten;
                cdc_.dma = 0;
                cdc_.halted_dma = 0;
            }
            cdc_.ifctrl = value;
            break;
        }
        case 0x02: cdc_.dbc = static_cast<std::uint16_t>((cdc_.dbc & 0xFF00) | value); break;
        case 0x03: cdc_.dbc = static_cast<std::uint16_t>((cdc_.dbc & 0x00FF) | ((value & 0x0F) << 8)); break;
        case 0x04: cdc_.dac = static_cast<std::uint16_t>((cdc_.dac & 0xFF00) | value); break;
        case 0x05: cdc_.dac = static_cast<std::uint16_t>((cdc_.dac & 0x00FF) | (value << 8)); break;
        case 0x06:  // DTRG: start a transfer, if output is enabled
            if ((cdc_.ifctrl & kDouten) != 0) {
                cdc_.ifstat &= static_cast<std::uint8_t>(~(kDtbsy | kDten));
                set_hi(0x02, static_cast<std::uint8_t>(hi(0x02) & 0x07));  // EDT and DSR clear
                cdc_start_transfer();
                cdc_.dma_clock = now_;
            }
            break;
        case 0x07:  // DTACK: acknowledge the end-of-transfer interrupt
            cdc_.ifstat |= kDtei;
            cdc_.irq &= static_cast<std::uint8_t>(~kDtei);
            break;
        case 0x08: cdc_.wa = static_cast<std::uint16_t>((cdc_.wa & 0xFF00) | value); break;
        case 0x09: cdc_.wa = static_cast<std::uint16_t>((cdc_.wa & 0x00FF) | (value << 8)); break;
        case 0x0A:  // CTRL0
            cdc_.stat[0] = static_cast<std::uint8_t>(value & kDecen);  // CRCOK follows DECEN
            if ((value & kDecen) == 0) {
                cdc_.ifstat |= kDeci;
                cdc_.irq &= static_cast<std::uint8_t>(~kDeci);
            }
            if ((value & kAutorq) != 0) {
                cdc_.stat[2] = static_cast<std::uint8_t>((cdc_.ctrl[1] & kModrq) | ((cdc_.head[1][2] & 0x20) >> 3));
            } else {
                cdc_.stat[2] = static_cast<std::uint8_t>(cdc_.ctrl[1] & (kModrq | kFormrq));
            }
            cdc_.ctrl[0] = value;
            break;
        case 0x0B:  // CTRL1
            if ((cdc_.ctrl[0] & kAutorq) != 0) {
                cdc_.stat[2] = static_cast<std::uint8_t>((value & kModrq) | ((cdc_.head[1][2] & 0x20) >> 3));
            } else {
                cdc_.stat[2] = static_cast<std::uint8_t>(value & (kModrq | kFormrq));
            }
            cdc_.ctrl[1] = value;
            break;
        case 0x0C: cdc_.pt = static_cast<std::uint16_t>((cdc_.pt & 0xFF00) | value); break;
        case 0x0D: cdc_.pt = static_cast<std::uint16_t>((cdc_.pt & 0x00FF) | (value << 8)); break;
        case 0x0F: cdc_reset(); break;
        default: break;
    }
    // The address moves on after every access except to register 0.
    if (reg != 0) {
        set_lo(0x02, static_cast<std::uint8_t>((reg + 1) & 0x0F));
    }
}

std::uint8_t SegaCd::cdc_register_read() noexcept {
    const std::uint8_t reg = lo(0x02) & 0x0F;
    std::uint8_t data = 0xFF;
    const std::size_t which = (cdc_.ctrl[1] & kShdren) != 0 ? 1 : 0;
    switch (reg) {
        case 0x01:
            cdc_update_decoder_window();
            data = cdc_.ifstat;
            break;
        case 0x02: data = static_cast<std::uint8_t>(cdc_.dbc); break;
        case 0x03: data = static_cast<std::uint8_t>(cdc_.dbc >> 8); break;
        case 0x04: data = cdc_.head[which][0]; break;
        case 0x05: data = cdc_.head[which][1]; break;
        case 0x06: data = cdc_.head[which][2]; break;
        case 0x07: data = cdc_.head[which][3]; break;
        case 0x08: data = static_cast<std::uint8_t>(cdc_.pt); break;
        case 0x09: data = static_cast<std::uint8_t>(cdc_.pt >> 8); break;
        case 0x0A: data = static_cast<std::uint8_t>(cdc_.wa); break;
        case 0x0B: data = static_cast<std::uint8_t>(cdc_.wa >> 8); break;
        case 0x0C: data = cdc_.stat[0]; break;
        case 0x0D: data = 0x00; break;  // STAT1
        case 0x0E: data = cdc_.stat[2]; break;
        case 0x0F:
            data = cdc_.stat[3];
            // Reading STAT3 retires the decoder interrupt.
            cdc_.stat[3] = 0x80;
            cdc_.ifstat |= kDeci;
            cdc_.irq &= static_cast<std::uint8_t>(~kDeci);
            break;
        default:
            break;
    }
    if (reg != 0) {
        set_lo(0x02, static_cast<std::uint8_t>((reg + 1) & 0x0F));
    }
    return data;
}

}  // namespace mercury::segacd
