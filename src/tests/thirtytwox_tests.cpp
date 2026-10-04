// The 32X adapter's shared registers.
//
// The test that matters most is the first one: with the adapter permanently
// attached, a Mega Drive cartridge that never touches $A15100 must see
// nothing at all. 694 Mega Drive games currently work and the 32X is not
// allowed to move any of them.

#include "mercury/genesis/genesis.hpp"
#include "mercury/thirtytwox/adapter.hpp"
#include "mercury/thirtytwox/bus.hpp"
#include "mercury/thirtytwox/pwm.hpp"
#include "mercury/thirtytwox/sh2_peripherals.hpp"
#include "mercury/thirtytwox/thirtytwox.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void check_eq(std::uint32_t got, std::uint32_t want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s: got %04X want %04X\n", what.c_str(), got, want);
    }
}

void test_inert_until_enabled() {
    std::printf("attached but inert: ADEN is clear out of reset\n");
    mercury::thirtytwox::Adapter adapter;
    adapter.reset();
    check(!adapter.enabled(), "ADEN starts clear, so the 68000 map is unchanged");
    check(!adapter.sh2_running(), "and with no adapter the SH-2s do not run");
    // The power-on value, taken from PicoDrive's register map where it is
    // marked verified against hardware. nRES is high from the start: the
    // SH-2s are not being held down, they simply have nothing to run against.
    check_eq(adapter.read16(mercury::thirtytwox::Adapter::kControl), 0x0082,
             "the control register reads REN and nRES set, ADEN clear");
    check(!adapter.sh2_in_reset(), "nRES is already released at power-on");
    check(!adapter.sh2_owns_vdp(), "the Mega Drive owns the 32X VDP at first");
}

void test_only_the_register_block_is_decoded() {
    std::printf("the adapter claims its registers and nothing else\n");
    using mercury::thirtytwox::Adapter;
    check(Adapter::decodes(0xA15100), "the control register is ours");
    check(Adapter::decodes(0xA1512E), "the last communication word is ours");
    check(!Adapter::decodes(0xA150FE), "just below the block is not ours");
    check(Adapter::decodes(0xA15130), "the PWM block is ours too");
    check(Adapter::decodes(0xA1513E), "up to its last register");
    // The window is 128 bytes and only five address bits pick a register, so
    // the upper half is the lower half again. $A15140 is the control
    // register a second time, not open bus.
    check(Adapter::decodes(0xA15140), "and the block mirrors through $A1517F");
    check(Adapter::decodes(0xA1517E), "right to the end of the window");
    check(!Adapter::decodes(0xA15180), "but the video registers are a different window");
    // The ones that matter: ordinary Mega Drive territory.
    check(!Adapter::decodes(0x000000), "cartridge ROM is not ours");
    check(!Adapter::decodes(0xFF0000), "work RAM is not ours");
    check(!Adapter::decodes(0xC00000), "the Mega Drive VDP is not ours");
    check(!Adapter::decodes(0xA10003), "the controller ports are not ours");
    check(!Adapter::decodes(0x880000), "the 32X ROM window is mapped by the cartridge, not here");
}

void test_enabling_the_adapter() {
    std::printf("setting ADEN, and holding and releasing the SH-2s\n");
    using Adapter = mercury::thirtytwox::Adapter;
    Adapter adapter;
    adapter.reset();
    adapter.write16(Adapter::kControl, Adapter::kAden | Adapter::kNotRes);
    check(adapter.enabled(), "ADEN took");
    check(adapter.sh2_running(), "ADEN with nRES already high is enough to start them");

    adapter.write16(Adapter::kControl, Adapter::kAden);
    check(adapter.sh2_in_reset(), "dropping nRES holds them again");
    check(!adapter.sh2_running(), "and they are not running while held");

    // REN is read-only. A game that writes zeros to the whole register still
    // reads it back, which is the property the old guessed bit map broke.
    adapter.write16(Adapter::kControl, 0x0000);
    check_eq(adapter.read16(Adapter::kControl) & Adapter::kRen, Adapter::kRen,
             "REN survives a write of zero");
    check(!adapter.enabled(), "but ADEN can be cleared");
    // Clearing ADEN forces nRES back high, so the register lands on exactly
    // its power-on value. A game that switches the adapter off cannot leave
    // the SH-2s in a half-held state.
    check_eq(adapter.read16(Adapter::kControl), Adapter::kPowerOnControl,
             "switching the adapter off returns the register to power-on");
    check(!adapter.sh2_running(), "and nothing runs with ADEN clear");
}

void test_communication_port_is_one_storage_two_windows() {
    std::printf("the communication port is the same eight words from both sides\n");
    mercury::thirtytwox::Adapter adapter;
    adapter.reset();
    adapter.write16(0xA15120, 0x1234);
    adapter.write16(0xA1512E, 0xBEEF);
    check_eq(adapter.comm_read(0), 0x1234, "word 0 seen from the SH-2 side");
    check_eq(adapter.comm_read(7), 0xBEEF, "word 7 seen from the SH-2 side");

    adapter.comm_write(3, 0xCAFE);
    check_eq(adapter.read16(0xA15126), 0xCAFE, "an SH-2 write is seen by the 68000");
}

void test_byte_access_picks_a_half() {
    std::printf("byte access on a 16-bit bus picks a half of the word\n");
    mercury::thirtytwox::Adapter adapter;
    adapter.reset();
    adapter.write16(0xA15120, 0x1234);
    check_eq(adapter.read8(0xA15120), 0x12, "the even byte is the high half");
    check_eq(adapter.read8(0xA15121), 0x34, "the odd byte is the low half");

    adapter.write8(0xA15121, 0x99);
    check_eq(adapter.read16(0xA15120), 0x1299, "writing the odd byte leaves the high half alone");
    adapter.write8(0xA15120, 0x77);
    check_eq(adapter.read16(0xA15120), 0x7799, "writing the even byte leaves the low half alone");
}

void test_interrupts_are_levels() {
    std::printf("an interrupt to an SH-2 is a level, held until acknowledged\n");
    using Adapter = mercury::thirtytwox::Adapter;
    Adapter adapter;
    adapter.reset();

    // Nothing is enabled out of reset, so the command interrupt does not
    // stand however hard the 68000 asks.
    adapter.write16(Adapter::kInterruptControl, Adapter::kIntm);
    check_eq(static_cast<std::uint32_t>(adapter.pending_sh2_level(0)), 0,
             "masked off, so nothing reaches the master");

    // The mask is per processor, and it is set by a byte write to the odd
    // half of the SH-2's own control register.
    adapter.sh2_write8(0x01, Adapter::kSh2IntCmd, 0);
    check_eq(static_cast<std::uint32_t>(adapter.pending_sh2_level(0)), 8,
             "with the command interrupt enabled it stands at level 8");
    check_eq(static_cast<std::uint32_t>(Adapter::vector_for_level(8)), 68,
             "and auto-vectors through vector 68");
    check_eq(static_cast<std::uint32_t>(adapter.pending_sh2_level(1)), 0,
             "the slave was not asked and has no mask anyway");
    // Standing, not consumed: asking again gives the same answer.
    check_eq(static_cast<std::uint32_t>(adapter.pending_sh2_level(0)), 8,
             "and it is still there, because nothing acknowledged it");

    // The master acknowledges through its own clear register at $401A.
    adapter.sh2_write16(0x1A, 0, 0);
    check_eq(static_cast<std::uint32_t>(adapter.pending_sh2_level(0)), 0,
             "acknowledging clears it");

    // Vertical interrupt: raised in both, but only where enabled.
    adapter.sh2_write8(0x01, Adapter::kSh2IntVint, 1);
    adapter.raise_sh2_interrupt(Adapter::kIrqVint);
    check_eq(static_cast<std::uint32_t>(adapter.pending_sh2_level(1)), 12,
             "the slave takes the vertical interrupt at level 12");
    check_eq(static_cast<std::uint32_t>(adapter.pending_sh2_level(0)), 0,
             "the master has it masked off and sees nothing");
    adapter.sh2_write16(0x16, 0, 1);
    check_eq(static_cast<std::uint32_t>(adapter.pending_sh2_level(1)), 0,
             "and the slave's acknowledgement clears only its own");

    // Reset is not maskable, and outranks everything else.
    adapter.raise_sh2_interrupt(Adapter::kIrqVres);
    check_eq(static_cast<std::uint32_t>(adapter.pending_sh2_level(0)), 14,
             "reset gets through a mask that blocks everything else");
}

void test_rom_bank_selection() {
    std::printf("the ROM bank register picks one of four\n");
    mercury::thirtytwox::Adapter adapter;
    adapter.reset();
    check_eq(static_cast<std::uint32_t>(adapter.rom_bank()), 0, "bank 0 out of reset");
    adapter.write16(mercury::thirtytwox::Adapter::kBank, 0x0003);
    check_eq(static_cast<std::uint32_t>(adapter.rom_bank()), 3, "bank 3 selected");
    adapter.write16(mercury::thirtytwox::Adapter::kBank, 0xFFFF);
    check_eq(static_cast<std::uint32_t>(adapter.rom_bank()), 3, "only two bits are the bank");
}

void test_vdp_ownership() {
    std::printf("FM says which side may touch the 32X VDP\n");
    mercury::thirtytwox::Adapter adapter;
    adapter.reset();
    check(!adapter.sh2_owns_vdp(), "the Mega Drive has it first");
    adapter.write16(mercury::thirtytwox::Adapter::kControl,
                    mercury::thirtytwox::Adapter::kFm);
    check(adapter.sh2_owns_vdp(), "setting FM hands it to the SH-2s");
}

void test_cached_and_cache_through_are_one_memory() {
    std::printf("the two views of the bus are the same storage\n");
    using mercury::thirtytwox::Bus;
    Bus bus;
    bus.reset();
    // SDRAM, written through the cached view and read back through the
    // cache-through one. Software uses both on purpose and they must agree.
    bus.write32(0x06000010, 0xDEADBEEF);
    check_eq(bus.read32(0x26000010), 0xDEADBEEF, "cached write, cache-through read");
    bus.write16(0x26000020, 0x1234);
    check_eq(bus.read16(0x06000020), 0x1234, "cache-through write, cached read");
    check_eq(Bus::normalise(0x26000000), 0x06000000, "normalise folds the two views");
}

void test_sdram_is_big_endian() {
    std::printf("SDRAM stores big-endian, same as the 68000 beside it\n");
    mercury::thirtytwox::Bus bus;
    bus.reset();
    bus.write32(0x06000000, 0x11223344);
    check_eq(bus.read8(0x06000000), 0x11, "the first byte is the most significant");
    check_eq(bus.read8(0x06000003), 0x44, "the last byte is the least significant");
    check_eq(bus.read16(0x06000002), 0x3344, "a word from the middle");
}

void test_cartridge_is_read_only() {
    std::printf("the cartridge window reads the ROM and ignores writes\n");
    std::vector<std::uint8_t> rom(0x1000, 0);
    rom[0] = 0xAB;
    rom[1] = 0xCD;
    mercury::thirtytwox::Bus bus;
    bus.reset();
    bus.attach_rom(&rom);
    check_eq(bus.read16(0x22000000), 0xABCD, "the cartridge is visible to the SH-2s");
    bus.write16(0x22000000, 0x0000);
    check_eq(bus.read16(0x22000000), 0xABCD, "and cannot be written");
    check_eq(bus.read8(0x22000FFF + 1), 0xFF, "past the end of a small ROM reads open bus");
}

void test_frame_buffer_overwrite_image_skips_zero() {
    std::printf("the overwrite image treats a zero byte as transparent\n");
    mercury::thirtytwox::Bus bus;
    bus.reset();
    // Ordinary window: a BYTE write of zero does not go through either - the
    // hardware swallows 8-bit zero writes on both sides of the frame buffer
    // (ares: "8-bit 0x00 writes do not go through"). This asserted the
    // opposite once, and that let Spider-Man's sprite blitter punch holes of
    // palette entry 0 around every sprite.
    bus.write8(0x24000000, 0x77);
    bus.write8(0x24000000, 0x00);
    check_eq(bus.read8(0x24000000), 0x77, "a byte write of zero to the normal window is dropped");
    // A WORD write of zero still clears - that is how a frame buffer is wiped.
    bus.write16(0x24000000, 0x0000);
    check_eq(bus.read8(0x24000000), 0x00, "but a word write of zero clears it");

    bus.write8(0x24000001, 0x55);
    bus.write8(0x24020001, 0x00);  // overwrite image, same byte
    check_eq(bus.read8(0x24000001), 0x55, "the overwrite image leaves a zero alone");
    bus.write8(0x24020001, 0x99);
    check_eq(bus.read8(0x24000001), 0x99, "and writes a non-zero normally");
}

void test_overwrite_image_tests_each_byte_of_a_word() {
    std::printf("a word write to the overwrite image judges its halves separately\n");
    mercury::thirtytwox::Bus bus;
    bus.reset();
    bus.write16(0x24000010, 0xAABB);
    bus.write16(0x24020010, 0x00CC);  // high byte transparent, low byte opaque
    check_eq(bus.read8(0x24000010), 0xAA, "the transparent half was kept");
    check_eq(bus.read8(0x24000011), 0xCC, "the opaque half was written");
}

void test_the_drawn_bank_is_not_the_shown_one() {
    std::printf("the SH-2s draw into whichever bank is not being displayed\n");
    mercury::thirtytwox::Bus bus;
    bus.reset();
    bus.set_displayed_bank(0);
    bus.write8(0x24000000, 0x11);
    check_eq(bus.frame_buffer(1)[0], 0x11, "bank 0 shown, so bank 1 is drawn into");
    check_eq(bus.frame_buffer(0)[0], 0x00, "the shown bank was left alone");

    bus.set_displayed_bank(1);
    bus.write8(0x24000000, 0x22);
    check_eq(bus.frame_buffer(0)[0], 0x22, "after the swap the other bank is drawn into");
}

void test_palette_is_shared_and_word_sized() {
    std::printf("the palette is 256 words, reachable a byte or a word at a time\n");
    mercury::thirtytwox::Bus bus;
    bus.reset();
    bus.write16(0x20004200, 0x7FFF);
    check_eq(bus.palette()[0], 0x7FFF, "entry 0 written as a word");
    bus.write16(0x20004200 + 255 * 2, 0x1234);
    check_eq(bus.palette()[255], 0x1234, "the last entry");
    check_eq(bus.read16(0x20004200 + 255 * 2), 0x1234, "and reads back");
    bus.write8(0x20004201, 0xEE);
    check_eq(bus.palette()[0], 0x7FEE, "a byte write touches only its half");
}

void test_system_registers_are_visible_from_the_sh2_side() {
    std::printf("the communication port is the same words from the SH-2 side\n");
    mercury::thirtytwox::Adapter adapter;
    mercury::thirtytwox::Bus bus;
    adapter.reset();
    bus.reset();
    bus.attach_adapter(&adapter);
    // The 68000 writes at $A15120; the SH-2 sees it at 0x20004020.
    adapter.write16(0xA15120, 0x5AA5);
    check_eq(bus.read16(0x20004020), 0x5AA5, "the 68000's write is visible to the SH-2s");
    bus.write16(0x20004022, 0x1357);
    check_eq(adapter.read16(0xA15122), 0x1357, "and the other way round");
}

// A cartridge image whose SH-2 vector table points at a tiny program in the
// cartridge window: MOV #0x5A,R1 then a self-loop. Enough to prove the
// processors start where the table says and run real code.
std::vector<std::uint8_t> make_test_cartridge() {
    std::vector<std::uint8_t> rom(0x1000, 0);
    const auto poke32 = [&rom](std::size_t at, std::uint32_t value) {
        rom[at] = static_cast<std::uint8_t>(value >> 24);
        rom[at + 1] = static_cast<std::uint8_t>(value >> 16);
        rom[at + 2] = static_cast<std::uint8_t>(value >> 8);
        rom[at + 3] = static_cast<std::uint8_t>(value);
    };
    const auto poke16 = [&rom](std::size_t at, std::uint16_t value) {
        rom[at] = static_cast<std::uint8_t>(value >> 8);
        rom[at + 1] = static_cast<std::uint8_t>(value);
    };
    // The SH-2 reads its vector table from address 0, which on this bus is the
    // boot ROM - so for the test the program is entered by pointing the
    // processors directly. What this cartridge provides is the code.
    poke16(0x0100, 0xE15A);  // MOV #0x5A,R1
    poke16(0x0102, 0xAFFF);  // BRA -2 (self loop)
    poke16(0x0104, 0x0009);  // NOP in the delay slot
    poke32(0x0000, 0x22000100);
    poke32(0x0004, 0x0603F000);
    return rom;
}

void test_the_adapter_is_free_when_a_mega_drive_game_runs() {
    std::printf("an inert adapter costs nothing and does nothing\n");
    mercury::thirtytwox::ThirtyTwoX unit;
    unit.reset();
    check(!unit.active(), "not active out of reset");
    unit.run(100000);
    check_eq(static_cast<std::uint32_t>(unit.master_instructions()), 0,
             "the master did not execute anything");
    check_eq(static_cast<std::uint32_t>(unit.slave_instructions()), 0,
             "nor the slave");
}

void test_enabling_then_releasing_starts_both() {
    std::printf("ADEN then RES starts both processors\n");
    auto rom = make_test_cartridge();
    mercury::thirtytwox::ThirtyTwoX unit;
    unit.reset();
    unit.attach_rom(&rom);

    unit.adapter().write16(mercury::thirtytwox::Adapter::kControl,
                           mercury::thirtytwox::Adapter::kAden);
    unit.run(1000);
    check_eq(static_cast<std::uint32_t>(unit.master_instructions()), 0,
             "ADEN alone does not start them");

    unit.adapter().write16(mercury::thirtytwox::Adapter::kControl,
                           mercury::thirtytwox::Adapter::kAden |
                               mercury::thirtytwox::Adapter::kNotRes);
    check(unit.active(), "now active");

    // Releasing RES makes both processors read their vector table, and that
    // happens on the first run() rather than on the write - the cartridge has
    // to be mapped first. So the step that performs the reset comes before
    // anything can place them.
    //
    // NOT YET REAL: the vector table lives in the 32X boot ROM, which GenX
    // does not have, so it reads as zero and both processors start at address
    // 0. A cartridge will need either a BIOS image or its header's SH-2 entry
    // points decoded. Until then a harness places them by hand, and this test
    // asserts the gap so it cannot be forgotten.
    unit.run(1);
    check(unit.unimplemented_opcodes() > 0,
          "with no boot ROM both start at 0 and execute empty space - THIS IS THE GAP");

    unit.master().set_pc(0x22000100);
    unit.slave().set_pc(0x22000100);
    unit.run(50);
    check(unit.master_instructions() > 0, "the master is running");
    check(unit.slave_instructions() > 0, "the slave is running");
    check_eq(unit.master().registers().r[1], 0x5A, "the master ran code from the cartridge");
    check_eq(unit.slave().registers().r[1], 0x5A, "and so did the slave");
}

// A cartridge with a real 32X startup header: SH-2 code sitting in the
// cartridge, a request to copy it into 32X RAM, and an entry point for each
// processor. This is what the boot ROM reads, and what lets GenX start a
// cartridge without one.
std::vector<std::uint8_t> make_booting_cartridge() {
    std::vector<std::uint8_t> rom(0x2000, 0);
    const auto poke32 = [&rom](std::size_t at, std::uint32_t value) {
        rom[at] = static_cast<std::uint8_t>(value >> 24);
        rom[at + 1] = static_cast<std::uint8_t>(value >> 16);
        rom[at + 2] = static_cast<std::uint8_t>(value >> 8);
        rom[at + 3] = static_cast<std::uint8_t>(value);
    };
    const auto poke16 = [&rom](std::size_t at, std::uint16_t value) {
        rom[at] = static_cast<std::uint8_t>(value >> 8);
        rom[at + 1] = static_cast<std::uint8_t>(value);
    };

    // The program, at 0x1000 in the cartridge. The master writes 0x21 into
    // R1, the slave lands six bytes later and writes 0x32, so the two can be
    // told apart. Both then sit in a self-loop.
    //
    // Values under 0x80 on purpose: MOV #imm sign-extends, so 0xA1 would
    // arrive as 0xFFFFFFA1 and this test would be about sign extension
    // instead of about booting. That has its own test in the SH-2 suite.
    poke16(0x1000, 0xE121);  // MOV #0x21,R1
    poke16(0x1002, 0xAFFE);  // BRA to itself
    poke16(0x1004, 0x0009);  // NOP in the delay slot
    poke16(0x1006, 0xE232);  // MOV #0x32,R2   <- the slave starts here
    poke16(0x1008, 0xAFFE);  // BRA to itself
    poke16(0x100A, 0x0009);  // NOP

    // What a 32X cartridge declares itself to be. Checked before anything
    // else, because a Mega Drive cartridge could match the entry points by
    // accident but will not claim to be a 32X.
    const char* name = "SEGA 32X        ";
    for (std::size_t i = 0; i < 16; ++i) {
        rom[0x100 + i] = static_cast<std::uint8_t>(name[i]);
    }

    // The header. Copy 0x100 bytes from 0x1000 in the cartridge to the start
    // of 32X RAM, master entry there, slave six bytes in. The destination is
    // an OFFSET into that RAM - the real test suite declares 0 and means the
    // beginning of SDRAM.
    poke32(0x3D4, 0x00001000);
    poke32(0x3D8, 0x00000000);
    poke32(0x3DC, 0x00000100);
    poke32(0x3E0, 0x06000000);
    poke32(0x3E4, 0x06000006);
    return rom;
}

void test_a_cartridge_boots_with_no_bios() {
    std::printf("BIOS AGNOSTIC: the header alone starts both processors\n");
    auto rom = make_booting_cartridge();
    mercury::thirtytwox::ThirtyTwoX unit;
    unit.reset();
    unit.attach_rom(&rom);

    check(unit.startup().valid, "the 32X startup header was recognised");
    check_eq(unit.startup().destination, 0x06000000,
             "a destination of 0 means the start of 32X RAM");
    check_eq(unit.startup().master_entry, 0x06000000, "master entry read from the header");
    check_eq(unit.startup().slave_entry, 0x06000006, "slave entry read from the header");

    unit.adapter().write16(mercury::thirtytwox::Adapter::kControl,
                           mercury::thirtytwox::Adapter::kAden |
                               mercury::thirtytwox::Adapter::kNotRes);

    // THE STARTUP DELAY. Neither processor runs straight away, and neither
    // marker is published, because the 68000's own startup is still clearing
    // memory - including these communication ports. Publishing early loses
    // the marker for good and hangs the game.
    unit.run(200);
    check_eq(unit.bus().read16(0x06000000), 0xE121, "the program is copied into RAM at once");
    check_eq(unit.adapter().comm_read(0), 0x0000, "but no marker is published yet");
    check_eq(unit.master().registers().r[1], 0x00, "and the master has not run");

    // Past the delay the handshake completes: the master publishes "M_OK",
    // the slave sees it and answers "S_OK", and both run their own code.
    unit.run(400'000);
    check_eq(unit.adapter().comm_read(0), 0x4D5F, "the master published M_ ...");
    check_eq(unit.adapter().comm_read(1), 0x4F4B, "... OK into port 0");
    check_eq(unit.adapter().comm_read(2), 0x535F, "the slave answered S_ ...");
    check_eq(unit.adapter().comm_read(3), 0x4F4B, "... OK into port 2");
    check_eq(unit.master().registers().r[1], 0x21, "the master ran its own entry point");
    check_eq(unit.slave().registers().r[2], 0x32, "the slave ran its own entry point");
    check_eq(unit.master().registers().r[2], 0x00, "the master did not run the slave's code");
    check_eq(static_cast<std::uint32_t>(unit.unimplemented_opcodes()), 0,
             "nothing unimplemented was executed");
}

void test_a_mega_drive_cartridge_has_no_startup_header() {
    std::printf("a plain Mega Drive cartridge is not mistaken for a 32X one\n");
    // Zeroed where the 32X header would be, which is what an ordinary
    // cartridge looks like there.
    std::vector<std::uint8_t> rom(0x2000, 0);
    mercury::thirtytwox::ThirtyTwoX unit;
    unit.reset();
    unit.attach_rom(&rom);
    check(!unit.startup().valid, "no entry points means no 32X startup");
}

void test_both_processors_share_one_memory() {
    std::printf("the two processors are on ONE bus\n");
    mercury::thirtytwox::ThirtyTwoX unit;
    unit.reset();
    unit.bus().write32(0x06001000, 0x0BADF00D);
    // Whatever one writes, the other reads - there is only one SDRAM.
    check_eq(unit.bus().read32(0x26001000), 0x0BADF00D, "one storage, both views");
}

// --- as the 68000 sees it --------------------------------------------------
//
// Everything above tests the 32X on its own. These go through the Mega Drive,
// which is the path a real cartridge takes and the one that could disturb the
// 693 games that already work.

void test_the_68000_reaches_the_adapter_registers() {
    std::printf("the 68000 can see and set the adapter registers\n");
    mercury::genesis::Genesis machine;
    // Detection: readable with the adapter still inert, which is how a
    // cartridge finds out there is one.
    machine.write16(0xA15120, 0xC0DE);
    check_eq(machine.read16(0xA15120), 0xC0DE, "the communication port answers the 68000");
    check(!machine.thirtytwox().adapter().enabled(), "and the adapter is still off");

    machine.write16(0xA15100, mercury::thirtytwox::Adapter::kAden);
    check(machine.thirtytwox().adapter().enabled(), "the 68000 switched the adapter on");
}

void test_the_rom_windows_appear_only_when_enabled() {
    std::printf("THE PROPERTY THAT PROTECTS EVERY MEGA DRIVE GAME\n");
    mercury::genesis::Genesis machine;
    // While the adapter is inert there is no 32X ROM window at all, so a Mega
    // Drive game reading up there gets whatever it got before.
    check(machine.thirtytwox_rom_offset(0x880000) < 0, "no fixed window while inert");
    check(machine.thirtytwox_rom_offset(0x900000) < 0, "no banked window while inert");
    check(machine.thirtytwox_rom_offset(0x000000) < 0, "and cartridge ROM is never one");
    check(machine.thirtytwox_rom_offset(0xFF0000) < 0, "nor work RAM");

    machine.write16(0xA15100, mercury::thirtytwox::Adapter::kAden);
    check_eq(static_cast<std::uint32_t>(machine.thirtytwox_rom_offset(0x880000)), 0,
             "the fixed window starts at the beginning of the cartridge");
    check_eq(static_cast<std::uint32_t>(machine.thirtytwox_rom_offset(0x880010)), 0x10,
             "and tracks the address");
    check_eq(static_cast<std::uint32_t>(machine.thirtytwox_rom_offset(0x900000)), 0,
             "the banked window starts at bank 0");

    machine.write16(0xA15104, 2);  // bank 2
    check_eq(static_cast<std::uint32_t>(machine.thirtytwox_rom_offset(0x900000)), 0x200000,
             "and follows the bank register");
    check_eq(static_cast<std::uint32_t>(machine.thirtytwox_rom_offset(0x880000)), 0,
             "while the fixed window ignores the bank");
}

void test_a_reset_puts_the_adapter_back() {
    std::printf("a reset returns the adapter to inert\n");
    mercury::genesis::Genesis machine;
    machine.write16(0xA15100, mercury::thirtytwox::Adapter::kAden |
                                  mercury::thirtytwox::Adapter::kNotRes);
    check(machine.thirtytwox().active(), "on and running");
    machine.reset();
    check(!machine.thirtytwox().adapter().enabled(), "reset turned it off again");
    // Back to the power-on value, which has nRES high. "Not running" here is
    // ADEN being clear, not the processors being held: those are two
    // different things and the register distinguishes them.
    check(!machine.thirtytwox().active(), "and stopped both processors");
    check_eq(machine.thirtytwox().adapter().control(),
             mercury::thirtytwox::Adapter::kPowerOnControl,
             "with the control register exactly as it powers on");
}

void test_the_cache_purge_area_is_not_memory() {
    std::printf("a write to the cache's purge area leaves the memory it names alone\n");
    mercury::thirtytwox::Bus bus;
    bus.reset();
    bus.write32(0x06004F10, 0x12345678);
    // What Knuckles' Chaotix does to its command buffer just before running it.
    bus.write32(0x46004F10, 0x00000000);
    check_eq(bus.read32(0x06004F10), 0x12345678, "a longword purge does not reach SDRAM");
    bus.write32(0x06004F14, 0xCAFEF00D);
    bus.write16(0x46004F14, 0x0000);
    bus.write8(0x46004F17, 0x00);
    bus.write32(0x66004F14, 0x00000000);
    check_eq(bus.read32(0x06004F14), 0xCAFEF00D,
             "nor does a word, a byte, or a write to the address array");
}

void test_a_word_read_of_the_fifo_takes_one_word() {
    std::printf("the 68000 -> SH-2 FIFO gives up exactly one word per word read\n");
    using mercury::thirtytwox::Adapter;
    Adapter adapter;
    mercury::thirtytwox::Bus bus;
    adapter.reset();
    bus.reset();
    bus.attach_adapter(&adapter);
    adapter.write16(Adapter::kDreqControl, Adapter::k68s);
    adapter.write16(0xA15110, 4);  // the length, in words
    adapter.write16(Adapter::kDreqFifo, 0x0705);
    adapter.write16(Adapter::kDreqFifo, 0x0200);
    check(adapter.dreq_line(), "a word in the FIFO raises the request line");
    check_eq(bus.read16(0x20004012), 0x0705, "the first word comes out whole");
    check_eq(bus.read16(0x20004012), 0x0200, "and so does the second, not half of each");
    check(!adapter.dreq_line(), "an empty FIFO drops the line");
    check((bus.read16(0x20004006) & 0x4000) != 0, "and reports itself empty to the SH-2s");
    adapter.write16(Adapter::kDreqFifo, 0x1111);
    adapter.write16(Adapter::kDreqControl, 0x0000);
    check(!adapter.dreq_line(), "closing the request flushes what was not taken");
}

void test_a_word_write_to_the_fill_register_runs_one_fill() {
    std::printf("a word write to the auto-fill data register runs one fill, from the right place\n");
    mercury::thirtytwox::Bus bus;
    mercury::thirtytwox::Vdp vdp;
    bus.reset();
    vdp.reset();
    bus.attach_vdp(&vdp);
    vdp.attach_bus(&bus);
    // $4104 is the length (words less one) and $4106 the start, in that
    // order - the order Knuckles' Chaotix writes them.
    bus.write16(0x20004104, 0x0003);
    bus.write16(0x20004106, 0x0100);
    bus.write16(0x20004108, 0x2424);
    const auto& bank = bus.frame_buffer(bus.displayed_bank() ^ 1);
    const auto word_at = [&bank](std::size_t word) {
        return static_cast<std::uint32_t>((bank[word * 2] << 8) | bank[word * 2 + 1]);
    };
    check_eq(word_at(0x100), 0x2424, "the first word filled is the whole value, not half of it");
    check_eq(word_at(0x103), 0x2424, "the fill covers length plus one words");
    check_eq(word_at(0x104), 0x0000, "and stops there");
    check_eq(word_at(0x000), 0x0000, "and does not reach the line table");
}

void test_the_watchdog_is_written_through_a_select_byte() {
    std::printf("the watchdog's two registers sit behind one address pair\n");
    mercury::thirtytwox::Sh2Peripherals chip;
    chip.reset();
    // $5A selects the counter, so the value lands there and reads back at
    // $FE81. Star Wars Arcade writes exactly this word.
    chip.write8(0xFFFFFE80, 0x5A);
    chip.write8(0xFFFFFE81, 0xFF);
    check_eq(chip.read8(0xFFFFFE81), 0xFF, "the counter takes the value after a $5A select");
    // $A5 selects the control register instead.
    chip.write8(0xFFFFFE80, 0xA5);
    chip.write8(0xFFFFFE81, 0x38);
    check_eq(chip.read8(0xFFFFFE80) & 0x7F, 0x38,
             "the control register takes the value after an $A5 select");
    check_eq(chip.read8(0xFFFFFE81), 0xFF, "and the counter is untouched by it");
}

void test_the_watchdog_interrupts_on_overflow() {
    std::printf("the watchdog raises an interrupt when its counter overflows\n");
    mercury::thirtytwox::Sh2Peripherals chip;
    chip.reset();
    // Priority 6 for the watchdog (IPRA's low byte, high nibble) and vector
    // $4A (VCRWDT's high byte).
    chip.write8(0xFFFFFEE3, 0x60);
    chip.write8(0xFFFFFEE4, 0x4A);
    check_eq(chip.on_chip_interrupt_level(), 0, "nothing is asked for while it is stopped");
    // Counter near the top, then started: TME set, interval-timer mode,
    // slowest-but-one clock.
    chip.write8(0xFFFFFE80, 0x5A);
    chip.write8(0xFFFFFE81, 0xFE);
    chip.write8(0xFFFFFE80, 0xA5);
    chip.write8(0xFFFFFE81, 0x20);  // TME, WT/IT clear, clock/2
    chip.tick(8);                    // four counts at clock/2: $FE -> overflow
    check_eq(chip.on_chip_interrupt_level(), 6, "an overflow asks at the watchdog's priority");
    check_eq(chip.on_chip_interrupt_vector(), 0x4A, "and through the watchdog's own vector");
    // The flag - and so the request - is only cleared by software writing a
    // zero over it. That is what makes this a level and not a pulse.
    chip.write8(0xFFFFFE80, 0xA5);
    chip.write8(0xFFFFFE81, 0x20);
    check_eq(chip.on_chip_interrupt_level(), 0, "clearing the overflow flag drops the request");
}

void test_watchdog_mode_does_not_interrupt() {
    std::printf("in watchdog mode an overflow does not interrupt\n");
    mercury::thirtytwox::Sh2Peripherals chip;
    chip.reset();
    chip.write8(0xFFFFFEE3, 0x60);
    chip.write8(0xFFFFFE80, 0x5A);
    chip.write8(0xFFFFFE81, 0xFF);
    chip.write8(0xFFFFFE80, 0xA5);
    chip.write8(0xFFFFFE81, 0x60);  // TME and WT/IT both set: watchdog mode
    chip.tick(8);
    check_eq(chip.on_chip_interrupt_level(), 0,
             "watchdog mode resets the machine instead of interrupting, so it asks for nothing");
}

// --- PWM audio -------------------------------------------------------------

// Feed one width repeatedly and run for a while. Returns the output.
std::vector<std::int16_t> run_pwm(mercury::thirtytwox::Pwm& pwm, std::uint16_t width,
                                  std::uint32_t cycles) {
    // Top the queue up every period so the FIFO never runs dry - that is what
    // a game's PWM interrupt handler is for.
    const std::uint32_t step = 64;
    for (std::uint32_t at = 0; at < cycles; at += step) {
        pwm.write16(mercury::thirtytwox::Pwm::kMono, width);
        (void)pwm.tick(step);
    }
    return pwm.sample_buffer();
}

void test_pwm_generates_audio_at_the_host_rate() {
    std::printf("the PWM emits samples at the host rate, not at its own\n");
    mercury::thirtytwox::Pwm pwm;
    pwm.reset();
    // A period of 1047 is what several cartridges actually use: a 22kHz
    // sample rate out of the 32X's 23.01MHz clock.
    pwm.write16(mercury::thirtytwox::Pwm::kCycle, 1047);
    pwm.write16(mercury::thirtytwox::Pwm::kControl, 0x0005);  // both channels on

    const auto produced = run_pwm(pwm, 523, mercury::thirtytwox::Pwm::kClockHz / 10);
    // A tenth of a second of 32X time is a tenth of a second of audio.
    const auto expected = mercury::thirtytwox::Pwm::kOutputRate / 10;
    const auto difference = produced.size() > expected ? produced.size() - expected
                                                       : expected - produced.size();
    check(difference < 64, "a tenth of a second of cycles makes a tenth of a second of samples");
}

void test_pwm_silence_is_half_the_period() {
    std::printf("a width of half the period is silence, and a steady width carries no DC\n");
    mercury::thirtytwox::Pwm pwm;
    pwm.reset();
    pwm.write16(mercury::thirtytwox::Pwm::kCycle, 1047);
    pwm.write16(mercury::thirtytwox::Pwm::kControl, 0x0005);
    const auto produced = run_pwm(pwm, 523, mercury::thirtytwox::Pwm::kClockHz / 4);
    check(!produced.empty(), "silence still produces samples");
    // The last of them, once the DC blocker has settled.
    check(produced.back() > -64 && produced.back() < 64, "a steady half-period width is silent");
}

// THE ONE THAT MATTERS. Games do not agree on where their silence sits: After
// Burner straddles the halfway mark, Star Wars Arcade and Doom sit near it but
// not on it, and a width is an absolute level rather than a signed sample. The
// output is AC coupled on the hardware, so ANY steady width must settle to
// silence - otherwise a game riding an offset is quiet, clipped, and starts
// with a thump.
void test_pwm_blocks_dc_whatever_a_game_calls_silence() {
    std::printf("any steady width settles to silence, wherever the game puts it\n");
    for (const std::uint16_t width : {std::uint16_t{0}, std::uint16_t{127}, std::uint16_t{508},
                                      std::uint16_t{1046}}) {
        mercury::thirtytwox::Pwm pwm;
        pwm.reset();
        pwm.write16(mercury::thirtytwox::Pwm::kCycle, 1047);
        pwm.write16(mercury::thirtytwox::Pwm::kControl, 0x0005);
        const auto produced = run_pwm(pwm, width, mercury::thirtytwox::Pwm::kClockHz / 2);
        check(!produced.empty(), "a steady width still clocks samples out");
        const int settled = produced.empty() ? 0 : produced.back();
        check(settled > -256 && settled < 256,
              "width " + std::to_string(width) + " settles to silence, not to an offset");
    }
}

void test_pwm_carries_a_changing_signal() {
    std::printf("a changing width is a changing signal\n");
    mercury::thirtytwox::Pwm pwm;
    pwm.reset();
    pwm.write16(mercury::thirtytwox::Pwm::kCycle, 1047);
    pwm.write16(mercury::thirtytwox::Pwm::kControl, 0x0005);
    // A square wave around the centre, slow enough to survive the DC blocker.
    bool high = true;
    for (int block = 0; block < 200; ++block) {
        for (int at = 0; at < 8; ++at) {
            pwm.write16(mercury::thirtytwox::Pwm::kMono, high ? 900 : 150);
            (void)pwm.tick(1047);
        }
        high = !high;
    }
    std::int32_t loudest = 0;
    for (const auto sample : pwm.sample_buffer()) {
        const std::int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > loudest) loudest = magnitude;
    }
    check(loudest > 8000, "a square wave across the centre is loud, not silent");
}

void test_pwm_outputs_off_are_silent() {
    std::printf("output mode zero is silent even while the queue keeps draining\n");
    mercury::thirtytwox::Pwm pwm;
    pwm.reset();
    pwm.write16(mercury::thirtytwox::Pwm::kCycle, 1047);
    // Control written with the timer set but both output modes zero.
    pwm.write16(mercury::thirtytwox::Pwm::kControl, 0x0100);
    const auto before = pwm.samples();
    for (int block = 0; block < 200; ++block) {
        pwm.write16(mercury::thirtytwox::Pwm::kMono, block % 2 == 0 ? 900 : 150);
        (void)pwm.tick(1047);
    }
    std::int32_t loudest = 0;
    for (const auto sample : pwm.sample_buffer()) {
        const std::int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > loudest) loudest = magnitude;
    }
    check(loudest == 0, "nothing is audible with the outputs off");
    // And the queue still drained - that is the rule Virtua Racing Deluxe
    // hangs on, and silence must not have quietly reintroduced the hang.
    check(pwm.samples() > before, "the FIFOs keep draining with the outputs off");
}

// --- firmware folders ------------------------------------------------------

// The three images are told apart by SIZE, so a folder is "complete" purely on
// three file lengths. Written with files of the right sizes and no other
// content, because that is all the rule looks at - and the settings dialog
// reports what this returns, so a disagreement here is a dialog that lies.
void test_a_bios_folder_is_recognised_by_file_size() {
    std::printf("firmware images are found by size, not by name\n");
    namespace fs = std::filesystem;
    std::error_code ignored;
    const auto root = fs::temp_directory_path(ignored) / "genx_bios_scan_test";
    fs::remove_all(root, ignored);
    fs::create_directories(root, ignored);

    const auto write = [](const fs::path& at, std::size_t size) {
        std::ofstream out(at, std::ios::binary);
        const std::vector<char> zeros(size, '\0');
        out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    };

    auto scan = mercury::thirtytwox::ThirtyTwoX::scan_bios_folder(root);
    check(!scan.complete(), "an empty folder holds no firmware");

    // Deliberately unhelpful names: the rule must not be reading them.
    write(root / "a.bin", 0x100);
    scan = mercury::thirtytwox::ThirtyTwoX::scan_bios_folder(root);
    check(scan.genesis_page, "256 bytes is the Mega Drive boot page whatever it is called");
    check(!scan.complete(), "one image out of three is not a complete folder");

    write(root / "b.bin", 0x800);
    write(root / "c.bin", 0x400);
    // A file of some other size must not count as anything.
    write(root / "readme.txt", 0x123);
    scan = mercury::thirtytwox::ThirtyTwoX::scan_bios_folder(root);
    check(scan.master_rom, "2048 bytes is the master's boot ROM");
    check(scan.slave_rom, "1024 bytes is the slave's boot ROM");
    check(scan.complete(), "all three present is a complete folder");

    check(!mercury::thirtytwox::ThirtyTwoX::scan_bios_folder(root / "nope").complete(),
          "a folder that does not exist holds nothing, and does not throw");
    check(!mercury::thirtytwox::ThirtyTwoX::scan_bios_folder({}).complete(),
          "an empty path holds nothing, and does not throw");

    fs::remove_all(root, ignored);
}

// The configured folder is searched FIRST. Without that, someone whose library
// is not laid out as <root>/Games/<title> beside <root>/BIOS could point the
// setting anywhere and nothing would happen.
void test_the_configured_folder_is_searched_first() {
    std::printf("a configured firmware folder wins over the walk up from the cartridge\n");
    namespace fs = std::filesystem;
    std::error_code ignored;
    const auto root = fs::temp_directory_path(ignored) / "genx_bios_pick_test";
    fs::remove_all(root, ignored);
    const auto elsewhere = root / "somewhere else";
    const auto beside = root / "games" / "BIOS";
    fs::create_directories(elsewhere, ignored);
    fs::create_directories(beside, ignored);

    const auto write = [](const fs::path& at, std::size_t size, char fill) {
        std::ofstream out(at, std::ios::binary);
        const std::vector<char> bytes(size, fill);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    // Both folders are complete, and they differ in content, so which one was
    // taken is visible rather than merely asserted.
    for (const auto& folder : {elsewhere, beside}) {
        const char fill = folder == elsewhere ? '\x11' : '\x22';
        write(folder / "page", 0x100, fill);
        write(folder / "master", 0x800, fill);
        write(folder / "slave", 0x400, fill);
    }

    const auto cartridge = root / "games" / "title.32x";
    {
        std::ofstream out(cartridge, std::ios::binary);
        out.put('\0');
    }

    mercury::thirtytwox::ThirtyTwoX adapter;
    check(adapter.bios_folder().empty(), "no folder is configured by default");
    check(adapter.attach_bios(cartridge), "with nothing configured the walk up finds the BIOS folder");
    check(adapter.bios_loaded(), "and reports firmware loaded");

    adapter.set_bios_folder(elsewhere);
    check(adapter.attach_bios(cartridge), "a configured folder is used");
    check_eq(adapter.bus().boot_rom_byte(0, 0), 0x11u,
             "the master boot ROM came from the CONFIGURED folder, not the one beside the game");

    // A configured folder that holds nothing must not stop the fallbacks: the
    // setting is a hint, not a promise, and a stale path should not turn a
    // working setup into a BIOS-less one.
    adapter.set_bios_folder(root / "empty");
    check(adapter.attach_bios(cartridge),
          "a configured folder with no firmware falls back to the walk up");
    check_eq(adapter.bus().boot_rom_byte(0, 0), 0x22u,
             "and the fallback really is the folder beside the game");

    fs::remove_all(root, ignored);
}

}  // namespace

int main() {
    std::printf("32X adapter tests\n=================\n\n");
    test_inert_until_enabled();
    test_only_the_register_block_is_decoded();
    test_enabling_the_adapter();
    test_communication_port_is_one_storage_two_windows();
    test_byte_access_picks_a_half();
    test_interrupts_are_levels();
    test_rom_bank_selection();
    test_vdp_ownership();
    test_cached_and_cache_through_are_one_memory();
    test_sdram_is_big_endian();
    test_cartridge_is_read_only();
    test_frame_buffer_overwrite_image_skips_zero();
    test_overwrite_image_tests_each_byte_of_a_word();
    test_the_drawn_bank_is_not_the_shown_one();
    test_palette_is_shared_and_word_sized();
    test_system_registers_are_visible_from_the_sh2_side();
    test_the_adapter_is_free_when_a_mega_drive_game_runs();
    test_enabling_then_releasing_starts_both();
    test_a_cartridge_boots_with_no_bios();
    test_a_mega_drive_cartridge_has_no_startup_header();
    test_both_processors_share_one_memory();
    test_the_68000_reaches_the_adapter_registers();
    test_the_rom_windows_appear_only_when_enabled();
    test_a_reset_puts_the_adapter_back();
    test_the_cache_purge_area_is_not_memory();
    test_a_word_read_of_the_fifo_takes_one_word();
    test_a_word_write_to_the_fill_register_runs_one_fill();
    test_the_watchdog_is_written_through_a_select_byte();
    test_the_watchdog_interrupts_on_overflow();
    test_watchdog_mode_does_not_interrupt();
    test_pwm_generates_audio_at_the_host_rate();
    test_pwm_silence_is_half_the_period();
    test_pwm_blocks_dc_whatever_a_game_calls_silence();
    test_pwm_carries_a_changing_signal();
    test_pwm_outputs_off_are_silent();
    test_a_bios_folder_is_recognised_by_file_size();
    test_the_configured_folder_is_searched_first();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
