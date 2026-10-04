#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "mercury/core/sh2_cpu.hpp"
#include "mercury/thirtytwox/adapter.hpp"
#include "mercury/thirtytwox/bus.hpp"
#include "mercury/thirtytwox/sh2_peripherals.hpp"
#include "mercury/thirtytwox/vdp.hpp"

namespace mercury::thirtytwox {

// The 32X as one thing: two SH-2s, the bus they share, and the registers the
// Mega Drive talks to them through.
//
// ALWAYS ATTACHED, by the user's decision and by how people used the real
// hardware. That is safe because the adapter starts inert - see Adapter - and
// because both processors start held in reset. Until a cartridge sets ADEN and
// then releases RES, this object consumes a little memory and does nothing
// observable. A Mega Drive game cannot tell it is there.
//
// The master runs the game; the slave is usually left in reset for most of
// startup and woken later for whatever the game wants to parallelise.
class ThirtyTwoX {
  public:
    ThirtyTwoX();

    void reset() noexcept;
    void attach_rom(const std::vector<std::uint8_t>* rom) noexcept;

    // Look for the adapter's three firmware images near the cartridge and use
    // them if they are there.
    //
    // GenX stays BIOS AGNOSTIC by default and that is not changing: without
    // these it synthesises the startup from the cartridge header, which is
    // how the whole library runs today. But the synthesised version is an
    // informed reconstruction of what the firmware does, and every startup
    // fault found so far has been in exactly that reconstruction. With the
    // real images the same cartridge can be run both ways, and any
    // difference is a bug here rather than a question about the hardware.
    //
    // The three are told apart by SIZE rather than by name, because nobody
    // agrees on the names: 256 bytes is the Mega Drive page, 2048 the
    // master's boot ROM and 1024 the slave's. Returns true if all three were
    // found and loaded.
    bool attach_bios(const std::filesystem::path& rom_path);
    [[nodiscard]] bool bios_loaded() const noexcept { return bios_loaded_; }

    // Where the user says the firmware is. Searched BEFORE the environment
    // override and before the walk up from the cartridge, because it is the
    // one place a person has actually pointed at. Empty - the default - leaves
    // the old behaviour exactly as it was.
    //
    // It exists because the walk-up only finds firmware laid out as
    // <root>/Games/<title>/<rom> beside <root>/BIOS. That is one library
    // layout out of many, and a person whose firmware lives somewhere else had
    // no way to say so.
    void set_bios_folder(std::filesystem::path folder) { bios_folder_ = std::move(folder); }
    [[nodiscard]] const std::filesystem::path& bios_folder() const noexcept {
        return bios_folder_;
    }

    // What a folder holds, without loading anything or touching the machine -
    // so a settings dialog can tell someone their folder is right before they
    // start a game. Reports the three images by size, the way attach_bios
    // finds them.
    struct BiosScan {
        bool genesis_page = false;  // 256 bytes
        bool master_rom = false;    // 2048 bytes
        bool slave_rom = false;     // 1024 bytes
        [[nodiscard]] bool complete() const noexcept {
            return genesis_page && master_rom && slave_rom;
        }
    };
    [[nodiscard]] static BiosScan scan_bios_folder(const std::filesystem::path& folder);

    // Run both processors for roughly `cycles` of SH-2 time. Returns the
    // number actually consumed, which can overshoot by the length of one
    // instruction because instructions are not interruptible.
    std::uint32_t run(std::uint32_t cycles) noexcept;

    // The frame boundary, called by the machine around the two run() calls
    // that make up a frame. Entering blank swaps the frame buffer banks if a
    // swap was asked for, and raises the vertical interrupt in both
    // processors - which is what a 32X game's main loop waits on.
    void start_vblank() noexcept;
    void end_vblank() noexcept;

    // One Mega Drive scanline has gone by. The 32X counts these down from the
    // value at $4004 and raises its horizontal interrupt when the count runs
    // out, which is how a 32X game splits the screen. Nothing raised it
    // before, so the count register was writable and readable and did
    // nothing; Virtua Fighter polls it 830,000 times in 400 frames.
    //
    // `active` is false during vertical blank, where the interrupt only
    // fires if the game has asked for it through HEN.
    void scanline(bool active) noexcept;

    [[nodiscard]] Adapter& adapter() noexcept { return adapter_; }
    [[nodiscard]] const Adapter& adapter() const noexcept { return adapter_; }
    [[nodiscard]] Bus& bus() noexcept { return bus_; }
    [[nodiscard]] const Bus& bus() const noexcept { return bus_; }
    [[nodiscard]] mercury::core::Sh2Cpu& master() noexcept { return master_; }
    [[nodiscard]] mercury::core::Sh2Cpu& slave() noexcept { return slave_; }
    [[nodiscard]] const mercury::core::Sh2Cpu& master() const noexcept { return master_; }
    [[nodiscard]] const mercury::core::Sh2Cpu& slave() const noexcept { return slave_; }
    [[nodiscard]] const Sh2Peripherals& master_peripherals() const noexcept {
        return peripherals_[0];
    }
    [[nodiscard]] const Sh2Peripherals& slave_peripherals() const noexcept {
        return peripherals_[1];
    }
    [[nodiscard]] Vdp& vdp() noexcept { return vdp_; }
    [[nodiscard]] const Vdp& vdp() const noexcept { return vdp_; }
    [[nodiscard]] std::string vdp_description() const { return vdp_.describe(); }

    // Has a cartridge actually switched the adapter on? The renderer asks
    // this before compositing anything, so a plain Mega Drive game never
    // pays for a layer it does not have.
    [[nodiscard]] bool active() const noexcept { return adapter_.sh2_running(); }

    // How many instructions each processor has retired. Not for emulation -
    // for answering "is the slave doing anything at all", which is the first
    // question when a 32X game half works.
    [[nodiscard]] std::uint64_t master_instructions() const noexcept { return master_steps_; }
    [[nodiscard]] std::uint64_t slave_instructions() const noexcept { return slave_steps_; }
    [[nodiscard]] std::uint64_t unimplemented_opcodes() const noexcept { return unimplemented_; }
    // How many 32X interrupts each processor has actually accepted. Zero here
    // while a game sits polling a communication port is the whole answer:
    // the handler that would have written that port never ran.
    [[nodiscard]] std::uint64_t master_interrupts() const noexcept {
        return master_.interrupts_taken();
    }
    [[nodiscard]] std::uint64_t slave_interrupts() const noexcept {
        return slave_.interrupts_taken();
    }
    [[nodiscard]] std::uint16_t last_unimplemented() const noexcept { return last_unimplemented_; }
    // The FIRST one, and where it happened. Once a processor has run off into
    // data it decodes millions of nonsense opcodes, and the last of those
    // says nothing about what went wrong - several cartridges here report
    // 68000 instructions, because the SH-2 has ended up fetching from the
    // cartridge's Mega Drive code. The first one is the one to chase.
    [[nodiscard]] std::uint16_t first_unimplemented() const noexcept {
        return first_unimplemented_;
    }
    [[nodiscard]] std::uint32_t first_unimplemented_pc() const noexcept {
        return first_unimplemented_pc_;
    }
    [[nodiscard]] bool first_unimplemented_was_slave() const noexcept {
        return first_unimplemented_slave_;
    }

    // WATCH EVERY SH-2 INSTRUCTION.
    //
    // Called after each step with which processor ran it, what it did, and the
    // registers as they now stand. Nothing is attached by default and the call
    // is skipped entirely when nothing is: a game runs a few hundred million
    // instructions in a diagnostic pass, so this has to cost nothing when it
    // is not wanted.
    //
    // It exists so a run here can be lined up against another emulator's
    // trace of the same cartridge. The two machines start differently - GenX
    // synthesises the startup a real adapter's firmware would do - so the
    // early instructions will not correspond. The PCs of the CARTRIDGE's own
    // code will, because that is the same ROM either way, and comparing those
    // is the point.
    using Sh2TraceSink = std::function<void(int which, const core::Sh2StepResult& step,
                                            const core::Sh2Registers& registers)>;
    void set_sh2_trace(Sh2TraceSink sink) noexcept { sh2_trace_ = std::move(sink); }
    [[nodiscard]] bool sh2_trace_attached() const noexcept { return static_cast<bool>(sh2_trace_); }

    // What the cartridge header asked for at startup, for the diagnostic log.
    // A 32X cartridge that declares nothing here is one GenX cannot start,
    // and that is worth being able to see.
    struct Startup {
        bool valid = false;
        std::uint32_t source = 0;       // where the SH-2 code sits in the cartridge
        std::uint32_t destination = 0;  // where to copy it in 32X RAM
        std::uint32_t size = 0;
        std::uint32_t master_entry = 0;
        std::uint32_t slave_entry = 0;
        // Each processor's exception vector table. Skipping these was what
        // sent the master off the rails: with VBR left at zero every vector
        // read came out of an empty boot ROM page and the first exception
        // jumped to nowhere.
        std::uint32_t master_vbr = 0;
        std::uint32_t slave_vbr = 0;
    };
    [[nodiscard]] const Startup& startup() const noexcept { return startup_; }

    // The first 256 bytes the 68000 sees while the adapter is on.
    //
    // On hardware, setting ADEN swaps the bottom of the 68000's map away from
    // the cartridge and onto the 32X's own 68000 ROM. That page is almost
    // entirely an exception vector table, and every vector in it points into
    // the CARTRIDGE at $880200 upward - six bytes per entry, which is exactly
    // one jump instruction. So a 32X game's real 68000 program starts at
    // $880200, not at the Mega Drive reset vector.
    //
    // Without this the 68000 boots from the cartridge's own Mega Drive vector
    // table, which on a 32X cartridge leads to the "this needs a 32X" stub -
    // measured on the 240p Test Suite, where the 68000 took that path, and
    // the SH-2s were left running with nobody driving them.
    //
    // Returns false for anything outside the page or when the adapter is off,
    // and then the caller reads the cartridge as usual.
    [[nodiscard]] bool m68k_boot_overlay(std::uint32_t address, std::uint16_t& out) const noexcept;

    // ONE SLOT OF THE BOOT PAGE IS WRITABLE: the level 4 vector at $000070,
    // which on a Mega Drive is the horizontal interrupt.
    //
    // That is why the real firmware leaves it zero while pointing every other
    // vector at a trampoline. It is not an omission, it is storage: a 32X
    // game installs its own horizontal interrupt handler by writing the
    // address here at runtime. With the page read-only the write vanished and
    // the vector stayed zero, so any game that took a horizontal interrupt
    // went to address 0 and ran through the page as code.
    //
    // Writes anywhere else in the page are dropped, which is what the
    // hardware does with them.
    static constexpr std::uint32_t kWritableVector = 0x000070;
    [[nodiscard]] bool m68k_boot_overlay_write(std::uint32_t address, std::uint8_t value) noexcept;

    // THE MEGA DRIVE'S OWN WINDOWS ONTO THE 32X VIDEO LAYER.
    //
    // The video registers appear again at $A15180 and the palette at
    // $A15200, and these are not a convenience: a cartridge's Mega Drive code
    // polls the frame buffer control register here to pace itself against the
    // 32X's blanking. Without them the 240p Test Suite's 68000 read $A1518A
    // 116,232 times in twenty frames waiting for a bit that could never
    // arrive, which is what stopped every cartridge dead once they were left
    // to switch the adapter on themselves.
    //
    // Reading is always allowed. WRITING IS NOT: the Mega Drive may only
    // write while it owns the video chip, which is what the FM bit says.
    // 128 bytes, and it mirrors the same way the system block does: the
    // window is $A15180-$A151FF and only the low address bits pick a
    // register. Decoding just the first sixteen bytes left the rest reading
    // as open bus.
    static constexpr std::uint32_t kVideoFirst = 0xA15180;
    static constexpr std::uint32_t kVideoLast = 0xA151FF;
    static constexpr std::uint32_t kPaletteFirst = 0xA15200;
    static constexpr std::uint32_t kPaletteLast = 0xA153FF;

    [[nodiscard]] static bool decodes_video(std::uint32_t address) noexcept {
        return (address >= kVideoFirst && address <= kVideoLast) ||
               (address >= kPaletteFirst && address <= kPaletteLast);
    }
    // THE FRAME BUFFER, AS THE MEGA DRIVE SEES IT.
    //
    // $840000 is the same memory the processors reach at $04000000, and
    // $860000 is its overwrite image, exactly parallel to $04020000. A 32X
    // cartridge's Mega Drive startup writes here, so without the window that
    // startup goes nowhere and the SH-2s sit waiting for a partner that never
    // answers - all seven cartridges that regressed on the accurate startup
    // path were spinning on communication port 0 for want of it.
    //
    // Only while the adapter is on, like the ROM windows. Nothing in an
    // ordinary Mega Drive cartridge lives up here, but the gate costs nothing
    // and keeps the promise that an inert adapter is invisible.
    static constexpr std::uint32_t kFrameBufferFirst = 0x840000;
    static constexpr std::uint32_t kFrameBufferLast = 0x87FFFF;

    // Whether the Mega Drive may touch the frame buffer at this instant.
    [[nodiscard]] bool frame_buffer_reachable() const noexcept;
    [[nodiscard]] bool decodes_frame_buffer(std::uint32_t address) const noexcept {
        return adapter_.enabled() && address >= kFrameBufferFirst &&
               address <= kFrameBufferLast;
    }
    [[nodiscard]] std::uint16_t frame_buffer_read16(std::uint32_t address) const noexcept;
    void frame_buffer_write16(std::uint32_t address, std::uint16_t value) noexcept;
    [[nodiscard]] std::uint8_t frame_buffer_read8(std::uint32_t address) const noexcept;
    void frame_buffer_write8(std::uint32_t address, std::uint8_t value) noexcept;

    [[nodiscard]] std::uint16_t video_read16(std::uint32_t address) const noexcept;
    void video_write16(std::uint32_t address, std::uint16_t value) noexcept;
    [[nodiscard]] std::uint8_t video_read8(std::uint32_t address) const noexcept;
    void video_write8(std::uint32_t address, std::uint8_t value) noexcept;

    [[nodiscard]] std::string describe() const;

    // SAVE STATES. Everything the two processors and the adapter hold, in one
    // pass that serves both directions. Left out on purpose: the cartridge
    // and the startup block read from its header (the machine's cartridge
    // check covers those), the firmware images (see firmware_id), and the
    // instruction and interrupt counters, which are diagnostics.
    template <typename Archive>
    void visit_state(Archive& ar) {
        adapter_.visit_state(ar);
        bus_.visit_state(ar);
        vdp_.visit_state(ar);
        peripherals_[0].visit_state(ar);
        peripherals_[1].visit_state(ar);
        master_.visit_state(ar);
        slave_.visit_state(ar);
        ar(released_);
        ar(master_delay_);
        ar(slave_waiting_);
        ar(hint_countdown_);
        // The whole 256-byte page, although only the vector at $70 can
        // change: a game installs its horizontal interrupt handler there.
        ar(m68k_boot_page_);
    }

    // Which firmware the processors are running, as a number: 0 for the
    // synthesised startup, otherwise a hash of both boot ROMs. A state taken
    // with one and loaded with the other would hand the processors vectors
    // and code they never had.
    [[nodiscard]] std::uint64_t firmware_id() const noexcept;

  private:
    void wire_bus(mercury::core::Sh2Cpu& cpu) noexcept;
    // Do what the 32X boot ROM does, from the cartridge header rather than
    // from a BIOS image. GenX is deliberately BIOS agnostic: the header
    // carries the source, destination, size and both entry points, so the
    // startup sequence is a documented memory copy and two jumps, not an
    // approximation of something unknown.
    void run_boot_stub() noexcept;

    Adapter adapter_{};
    Bus bus_{};
    // Both processors' way onto bus_: one virtual call per access instead of
    // a std::function's check and double indirection. See wire_bus.
    struct SharedBus final : mercury::core::Sh2Bus {
        Bus* bus = nullptr;
        std::uint8_t read8(std::uint32_t a) override { return bus->read8(a); }
        std::uint16_t read16(std::uint32_t a) override { return bus->read16(a); }
        std::uint32_t read32(std::uint32_t a) override { return bus->read32(a); }
        void write8(std::uint32_t a, std::uint8_t v) override { bus->write8(a, v); }
        void write16(std::uint32_t a, std::uint16_t v) override { bus->write16(a, v); }
        void write32(std::uint32_t a, std::uint32_t v) override { bus->write32(a, v); }
        const std::uint8_t* fetch_window(std::uint32_t a, std::uint32_t& base,
                                         std::uint32_t& size) override {
            return bus->fetch_window(a, base, size);
        }
    };
    SharedBus shared_bus_{};
    Vdp vdp_{};
    // One block per processor, master first. Not shared, unlike everything
    // else the two of them can see.
    std::array<Sh2Peripherals, 2> peripherals_{};
    mercury::core::Sh2Cpu master_{};
    mercury::core::Sh2Cpu slave_{};
    // Both come out of reset together when RES is released, and each needs
    // its vector table read at that moment rather than at construction.
    bool released_ = false;

    // THE STARTUP RACE, and it is not a detail.
    //
    // The real boot ROM does not start the master immediately. It spins for
    // roughly 0x10000 iterations first, so that the 68000's own startup has
    // finished clearing work RAM and the communication ports before the
    // master publishes "M_OK" into port 0 and jumps to the game.
    //
    // Publishing the marker at once instead loses it: the 68000's startup
    // clears the port afterwards, the marker never comes back because it is
    // the boot ROM that writes it and not the game, and the 68000 waits for
    // ever. Measured on Star Wars Arcade, whose 68000 read port 0 ninety-six
    // thousand times in a three-hundred-frame run and never moved.
    //
    // The slave has its own half of the handshake: it waits to see "M_OK",
    // then publishes "S_OK" into port 2 and jumps to its own entry point.
    std::uint32_t master_delay_ = 0;
    bool slave_waiting_ = false;
    // Scanlines left before the next horizontal interrupt.
    int hint_countdown_ = 0;

    // Build the 68000 boot page once, when a cartridge is attached.
    void build_m68k_boot_page() noexcept;
    std::array<std::uint8_t, 0x100> m68k_boot_page_{};
    // True once real firmware is in place. The synthesised startup then steps
    // aside entirely: the processors reset out of their own boot ROMs and the
    // firmware does the copy, the registers and the handshake itself.
    bool bios_loaded_ = false;
    // The folder a person pointed at, if any. See set_bios_folder.
    std::filesystem::path bios_folder_{};

    Startup startup_{};
    const std::vector<std::uint8_t>* rom_ = nullptr;

    std::uint64_t master_steps_ = 0;
    std::uint64_t slave_steps_ = 0;
    std::uint64_t unimplemented_ = 0;
    std::uint16_t last_unimplemented_ = 0;
    std::uint16_t first_unimplemented_ = 0;
    std::uint32_t first_unimplemented_pc_ = 0;
    bool first_unimplemented_slave_ = false;
    Sh2TraceSink sh2_trace_{};
};

}  // namespace mercury::thirtytwox
