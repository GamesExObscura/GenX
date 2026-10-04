// Mega Drive probe. Loads a ROM, runs frames, dumps what the machine did.
//
// Also carries a self-test that builds a small 68000 program in memory, writes
// it out as a cartridge image and runs it. That gives an end-to-end check of
// cartridge -> CPU -> bus -> VDP -> framebuffer without needing a commercial
// ROM, which matters because the alternative is theorising about a black
// screen.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "mercury/genesis/genesis.hpp"
#include "mercury/segacd/bios.hpp"
#include "host_profiler.hpp"

namespace {

// A cartridge, or a Sega CD disc image. Discs are told apart by extension -
// .cue, .iso, .chd - or, for a .bin, by being a raw or cooked CD sector rather
// than a cartridge. "segacd" alone boots the BIOS with no disc.
//
// The BIOS comes from MERCURY_SEGACD_BIOS (a folder, or a single image) or a
// folder called BIOS beside the disc, chosen by the disc's region.
bool is_disc_image(const std::filesystem::path& path) {
    return mercury::segacd::Disc::is_disc_image(path);
}

mercury::genesis::Genesis::LoadResult load_image(mercury::genesis::Genesis& machine,
                                                 const std::filesystem::path& rom) {
    const bool bios_only = rom == "segacd";
    if (!bios_only && !is_disc_image(rom)) {
        return machine.load_rom(rom);
    }
    // Region first, so the right BIOS can be picked for it.
    char region = 0;
    if (!bios_only) {
        mercury::segacd::Disc peek;
        std::string error;
        if (!peek.open(rom, error)) {
            mercury::genesis::Genesis::LoadResult failed;
            failed.message = error;
            return failed;
        }
        region = peek.region();
    }
    std::filesystem::path configured;
    std::vector<std::uint8_t> bios;
    std::string error;
    if (const char* e = std::getenv("MERCURY_SEGACD_BIOS"); e != nullptr && *e != '\0') {
        configured = e;
        std::error_code ignored;
        if (std::filesystem::is_regular_file(configured, ignored)) {
            std::ifstream in(configured, std::ios::binary);
            bios.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    if (bios.empty() && !mercury::segacd::load_bios_for(region, configured,
                                                       bios_only ? std::filesystem::path() : rom,
                                                       bios, error)) {
        mercury::genesis::Genesis::LoadResult failed;
        failed.message = error;
        return failed;
    }
    return machine.load_cd(bios_only ? std::filesystem::path() : rom, bios);
}

void write_bmp24(const std::filesystem::path& path, const std::vector<std::uint8_t>& rgb,
                 std::uint32_t width, std::uint32_t height, std::uint32_t stride_pixels) {
    const std::uint32_t row_stride = (width * 3 + 3) & ~std::uint32_t{3};
    const std::uint32_t image_bytes = row_stride * height;
    const std::uint32_t file_size = 54 + image_bytes;

    std::vector<std::uint8_t> header(54, 0);
    header[0] = 'B'; header[1] = 'M';
    std::memcpy(header.data() + 2, &file_size, 4);
    const std::uint32_t offset = 54;
    std::memcpy(header.data() + 10, &offset, 4);
    const std::uint32_t dib = 40;
    std::memcpy(header.data() + 14, &dib, 4);
    std::memcpy(header.data() + 18, &width, 4);
    std::memcpy(header.data() + 22, &height, 4);
    const std::uint16_t planes = 1;
    std::memcpy(header.data() + 26, &planes, 2);
    const std::uint16_t bpp = 24;
    std::memcpy(header.data() + 28, &bpp, 2);
    std::memcpy(header.data() + 34, &image_bytes, 4);

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header.data()), 54);
    std::vector<std::uint8_t> row(row_stride, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto source_y = height - 1 - y;  // BMP rows run bottom-up
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto src = (static_cast<std::size_t>(source_y) * stride_pixels + x) * 3;
            row[x * 3 + 0] = rgb[src + 2];  // BMP stores BGR
            row[x * 3 + 1] = rgb[src + 1];
            row[x * 3 + 2] = rgb[src + 0];
        }
        out.write(reinterpret_cast<const char*>(row.data()), row_stride);
    }
}

// --- self-test ROM ---------------------------------------------------------

class RomBuilder {
public:
    RomBuilder() : bytes_(0x8000, 0) {}

    void word(std::uint32_t at, std::uint16_t value) {
        bytes_[at] = static_cast<std::uint8_t>(value >> 8);
        bytes_[at + 1] = static_cast<std::uint8_t>(value & 0xFF);
    }
    void longword(std::uint32_t at, std::uint32_t value) {
        word(at, static_cast<std::uint16_t>(value >> 16));
        word(at + 2, static_cast<std::uint16_t>(value & 0xFFFF));
    }
    void emit(std::uint16_t value) { word(pc_, value); pc_ += 2; }
    void emit32(std::uint32_t value) { longword(pc_, value); pc_ += 4; }
    void text(std::uint32_t at, const char* value, std::size_t length) {
        for (std::size_t i = 0; i < length; ++i) {
            bytes_[at + i] = value[i] ? static_cast<std::uint8_t>(value[i]) : ' ';
        }
    }

    // move.w #value, 4(a0)  - the VDP control port
    void vdp_control(std::uint16_t value) { emit(0x317C); emit(value); emit(0x0004); }
    // move.w #value, (a0)   - the VDP data port
    void vdp_data(std::uint16_t value) { emit(0x30BC); emit(value); }
    // move.w #value, (address).l
    void store_word(std::uint32_t address, std::uint16_t value) {
        emit(0x33FC); emit(value); emit32(address);
    }
    // lea address, a1
    void lea_a1(std::uint32_t address) { emit(0x43F9); emit32(address); }
    void move_w_postinc_a1(std::uint16_t value) { emit(0x32FC); emit(value); }
    void move_b_postinc_a1(std::uint8_t value) { emit(0x12FC); emit(value); }
    void vdp_register(std::uint8_t index, std::uint8_t value) {
        vdp_control(static_cast<std::uint16_t>(0x8000 | (index << 8) | value));
    }
    void vdp_address(std::uint8_t code, std::uint16_t address) {
        vdp_control(static_cast<std::uint16_t>(((code & 0x03) << 14) | (address & 0x3FFF)));
        vdp_control(static_cast<std::uint16_t>(((code >> 2) << 4) | ((address >> 14) & 0x03)));
    }

    void set_pc(std::uint32_t pc) { pc_ = pc; }
    [[nodiscard]] std::uint32_t pc() const { return pc_; }
    [[nodiscard]] std::vector<std::uint8_t>& bytes() { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint32_t pc_ = 0x200;
};

std::vector<std::uint8_t> build_selftest_rom() {
    RomBuilder rom;
    rom.longword(0x000, 0x00FFFE00);  // initial SSP, in work RAM
    rom.longword(0x004, 0x00000200);  // initial PC
    // Every other vector points at the entry too, so a stray exception lands
    // somewhere legible rather than running off into unmapped space.
    for (std::uint32_t vector = 2; vector < 64; ++vector) {
        rom.longword(vector * 4, 0x00000200);
    }
    rom.text(0x100, "SEGA MEGA DRIVE ", 16);
    rom.text(0x110, "(C)MERCURY 2026 ", 16);
    rom.text(0x120, "MERCURY SELF TEST", 17);
    rom.text(0x150, "MERCURY SELF TEST", 17);
    rom.text(0x180, "GM 00000000-00", 14);
    rom.text(0x1F0, "JUE", 3);
    rom.longword(0x1A0, 0x00000000);  // ROM start
    rom.longword(0x1A4, 0x00007FFF);  // ROM end

    // Vertical interrupt vector, number 30.
    rom.longword(0x78, 0x00000800);

    rom.set_pc(0x200);
    // move #$2000, sr - supervisor, interrupt mask zero. Reset leaves the mask
    // at seven, which would block the VDP's level six entirely.
    rom.emit(0x46FC);
    rom.emit(0x2000);
    // lea $00C00000, a0 - a0 addresses the VDP for the rest of the program
    rom.emit(0x41F9);
    rom.emit32(0x00C00000);

    rom.vdp_register(0x00, 0x04);  // mode 5, no H interrupt
    rom.vdp_register(0x01, 0x74);  // display on, V interrupt on, DMA enabled
    rom.vdp_register(0x02, 0x30);  // plane A name table at $C000
    rom.vdp_register(0x03, 0x00);  // window at $0000
    rom.vdp_register(0x04, 0x07);  // plane B name table at $E000
    rom.vdp_register(0x05, 0x78);  // sprite table at $F000
    rom.vdp_register(0x07, 0x00);  // backdrop is colour 0
    rom.vdp_register(0x0A, 0xFF);
    rom.vdp_register(0x0B, 0x00);  // full-screen scroll, no external interrupt
    rom.vdp_register(0x0C, 0x81);  // H40
    rom.vdp_register(0x0D, 0x34);  // h-scroll table at $D000
    rom.vdp_register(0x0F, 0x02);  // auto-increment two
    rom.vdp_register(0x10, 0x01);  // planes 64x32 cells
    rom.vdp_register(0x11, 0x00);
    rom.vdp_register(0x12, 0x00);

    // Palette: black, red, green, blue.
    rom.vdp_address(0x03, 0x0000);  // CRAM write from entry zero
    rom.vdp_data(0x0000);
    rom.vdp_data(0x000E);
    rom.vdp_data(0x00E0);
    rom.vdp_data(0x0E00);

    // Tile 1: solid colour index 1. Tile 2: solid index 2. Tile 3: index 3.
    rom.vdp_address(0x01, 0x0020);  // VRAM write at tile 1
    for (int i = 0; i < 16; ++i) rom.vdp_data(0x1111);
    for (int i = 0; i < 16; ++i) rom.vdp_data(0x2222);
    for (int i = 0; i < 16; ++i) rom.vdp_data(0x3333);

    // Build a fourth tile in work RAM and move it into VRAM by DMA, which is
    // how every real game gets its graphics across.
    rom.lea_a1(0x00FF0000);
    for (int i = 0; i < 16; ++i) rom.move_w_postinc_a1(0x1111);
    rom.vdp_register(0x13, 0x10);  // 16 words
    rom.vdp_register(0x14, 0x00);
    rom.vdp_register(0x15, 0x00);  // source $FF0000, stored as a word address
    rom.vdp_register(0x16, 0x80);
    rom.vdp_register(0x17, 0x7F);  // top bits zero selects 68000 -> VDP
    rom.vdp_address(0x21, 0x0080);  // VRAM write with CD5: tile 4

    // Plane A: three cells across the top-left.
    rom.vdp_address(0x01, 0xC000);
    rom.vdp_data(0x0001);
    rom.vdp_data(0x0002);
    rom.vdp_data(0x0003);
    // Cell eight gets the DMA'd tile, so a red pixel there proves the transfer.
    rom.vdp_address(0x01, 0xC010);
    rom.vdp_data(0x0004);

    // Plane B: tile 2 under plane A's cell zero, so plane A winning there is a
    // real precedence result rather than an empty-B result, and tile 2 again at
    // cell four where plane A is empty so B is visible on its own.
    rom.vdp_address(0x01, 0xE000);
    rom.vdp_data(0x0002);
    rom.vdp_data(0x0000);
    rom.vdp_data(0x0000);
    rom.vdp_data(0x0000);
    rom.vdp_data(0x0002);

    // Sprite 0: 1x1 cell of tile 3 at screen (64, 64). Sprite coordinates carry
    // a +128 bias.
    rom.vdp_address(0x01, 0xF000);
    rom.vdp_data(0x00C0);  // y = 64 + 128
    rom.vdp_data(0x0000);  // 1x1, link 0
    rom.vdp_data(0x0003);  // tile 3, palette 0, low priority
    rom.vdp_data(0x00C0);  // x = 64 + 128

    // Bring the Z80 up: take its bus, write a three-instruction program into
    // its RAM, then release reset and the bus so it runs.
    rom.store_word(0xA11100, 0x0100);  // request the Z80 bus
    rom.lea_a1(0x00A00000);
    rom.move_b_postinc_a1(0x3E);  // ld a, $5A
    rom.move_b_postinc_a1(0x5A);
    rom.move_b_postinc_a1(0x32);  // ld ($1000), a
    rom.move_b_postinc_a1(0x00);
    rom.move_b_postinc_a1(0x10);
    rom.move_b_postinc_a1(0x76);  // halt
    rom.store_word(0xA11200, 0x0100);  // release reset
    rom.store_word(0xA11100, 0x0000);  // release the bus

    rom.store_word(0x00FF1002, 0x0001);  // "setup ran to completion" marker
    rom.emit(0x60FE);  // bra.s * - park here so the frame loop can run

    // Vertical interrupt handler: bump a counter and return.
    rom.set_pc(0x800);  // clear of the setup code, which runs past 0x400
    rom.emit(0x5279);              // addq.w #1, (xxx).l
    rom.emit32(0x00FF1000);
    rom.emit(0x4E73);              // rte

    // Checksum over everything from $200, which is what the header claims.
    std::uint16_t sum = 0;
    auto& bytes = rom.bytes();
    for (std::size_t offset = 0x200; offset + 1 < bytes.size(); offset += 2) {
        sum = static_cast<std::uint16_t>(sum + ((bytes[offset] << 8) | bytes[offset + 1]));
    }
    rom.word(0x18E, sum);
    return bytes;
}

struct PixelCheck {
    const char* name;
    std::uint32_t x;
    std::uint32_t y;
    std::uint8_t r, g, b;
};

// A single FM operator at full volume, checked for level. This is the cheap
// guard against the failure that is hardest to hear your way out of: the whole
// chip working but at the wrong gain, which in FM shows up as distortion
// rather than as a volume problem. One operator with no modulation should land
// near a quarter of full scale after the six-channel mix headroom is applied -
// silence and clipping are both wrong, and both have specific causes.
// Plays algorithm 7 - all four operators in parallel, no modulation - with
// `voices` of them audible, and reports the peak level.
std::int32_t measure_fm_peak(int voices) {
    mercury::genesis::Ym2612 fm;
    fm.reset();
    const auto poke = [&](std::uint8_t reg, std::uint8_t value) {
        fm.write(0, reg);
        fm.write(1, value);
    };

    for (int slot = 0; slot < 4; ++slot) {
        const auto base = static_cast<std::uint8_t>(slot * 4);
        poke(static_cast<std::uint8_t>(0x30 + base), 0x01);  // detune 0, multiple 1
        poke(static_cast<std::uint8_t>(0x40 + base), slot < voices ? 0x00 : 0x7F);
        poke(static_cast<std::uint8_t>(0x50 + base), 0x1F);  // instant attack
        poke(static_cast<std::uint8_t>(0x60 + base), 0x00);  // no decay
        poke(static_cast<std::uint8_t>(0x70 + base), 0x00);  // no sustain decay
        poke(static_cast<std::uint8_t>(0x80 + base), 0x0F);  // full sustain, fast release
    }
    poke(0xA4, 0x21);  // block 4, frequency number high
    poke(0xA0, 0x00);
    poke(0xB0, 0x07);  // algorithm 7: all four operators in parallel
    poke(0xB4, 0xC0);  // both speakers
    poke(0x28, 0xF0);  // key on all four operators of channel 1

    // A twentieth of a second is plenty for a note with an instant attack.
    fm.advance_cycles(static_cast<std::uint32_t>(mercury::genesis::Genesis::kM68kClockHz / 20));

    std::int32_t peak = 0;
    for (auto sample : fm.sample_buffer()) {
        peak = std::max<std::int32_t>(peak, std::abs(static_cast<int>(sample)));
    }
    return peak;
}

int run_fm_check() {
    const auto one = measure_fm_peak(1);
    const auto four = measure_fm_peak(4);

    std::printf("\nFM gain check\n");
    std::printf("  one operator     : peak %d\n", one);
    std::printf("  four operators   : peak %d\n", four);

    int failures = 0;
    const auto report = [&](const char* what, bool ok) {
        if (!ok) ++failures;
        std::printf("  %-40s %s\n", what, ok ? "PASS" : "FAIL");
    };

    report("one operator is audible", one > 1000);
    report("one operator has headroom", one < 8000);
    // The real check: four carriers must come out about four times one carrier.
    // If the channel limiter is set too low they all land on the same number
    // instead, which is hard clipping - and it is the bright additive patches
    // that hit it, so it is heard as distortion on leads rather than as
    // distortion everywhere.
    const bool scales = four > one * 7 / 2 && four < one * 9 / 2;
    report("four carriers sum to four times one", scales);
    report("four carriers stay inside the sample range", four < 30000);
    return failures;
}

int run_selftest(const std::filesystem::path& scratch) {
    const auto image = build_selftest_rom();
    const auto rom_path = scratch / "mercury_selftest.bin";
    {
        std::ofstream out(rom_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
    }

    mercury::genesis::Genesis machine;
    const auto load = load_image(machine, rom_path);
    if (!load.loaded) {
        std::cout << "FAIL: cartridge did not load - " << load.message << "\n";
        return 1;
    }
    std::cout << load.message << "\n\n";

    for (int frame = 0; frame < 4; ++frame) {
        machine.run_frame();
    }

    const auto& vdp = machine.vdp();
    const auto& pixels = vdp.frame_pixels();
    const auto stride = mercury::genesis::Vdp::kMaxWidth;

    const PixelCheck checks[] = {
        // Plane A cells zero, one and two, in the three palette colours. Cell
        // zero also has plane B tile 2 underneath it, so red there proves the
        // priority order rather than an empty plane B.
        {"plane A wins over plane B", 4, 4, 255, 0, 0},
        {"plane A cell 1 is green", 12, 4, 0, 255, 0},
        {"plane A cell 2 is blue", 20, 4, 0, 0, 255},
        // Cell four of plane A is empty, so plane B's tile 2 shows through.
        {"plane B shows under empty A", 36, 4, 0, 255, 0},
        // Nothing on either plane out here: the backdrop.
        {"backdrop where both planes are empty", 200, 100, 0, 0, 0},
        // Cell eight uses a tile that only exists because DMA put it there.
        {"DMA'd tile renders", 68, 4, 255, 0, 0},
        // The sprite.
        {"sprite is blue at (64,64)", 68, 68, 0, 0, 255},
        {"no sprite just left of it", 60, 68, 0, 0, 0},
    };

    int failures = 0;
    for (const auto& check : checks) {
        const auto index = (static_cast<std::size_t>(check.y) * stride + check.x) * 3;
        const auto r = pixels[index + 0];
        const auto g = pixels[index + 1];
        const auto b = pixels[index + 2];
        const bool ok = r == check.r && g == check.g && b == check.b;
        if (!ok) ++failures;
        std::printf("  %-40s %s  got %3u,%3u,%3u  want %3u,%3u,%3u\n", check.name,
                    ok ? "PASS" : "FAIL", r, g, b, check.r, check.g, check.b);
    }

    // State checks that are not pixels: the interrupt actually reached the
    // handler, setup ran to the end, and the Z80 executed its own program.
    struct StateCheck {
        const char* name;
        std::uint32_t value;
        std::uint32_t want_at_least;
    };
    const StateCheck states[] = {
        {"setup ran to completion", machine.read8(0x00FF1003), 1},
        {"vertical interrupt reached its handler", machine.read8(0x00FF1001), 3},
        {"Z80 ran its own program", machine.read8(0x00A01000), 0x5A},
        {"DMA transfers performed", static_cast<std::uint32_t>(vdp.dma_count()), 1},
    };
    for (const auto& state : states) {
        const bool ok = state.value >= state.want_at_least;
        if (!ok) ++failures;
        std::printf("  %-40s %s  got %u  want >= %u\n", state.name, ok ? "PASS" : "FAIL",
                    state.value, state.want_at_least);
    }

    std::printf("\n  frames rendered : %u\n", vdp.frames_elapsed());
    std::printf("  register writes : %llu\n", static_cast<unsigned long long>(vdp.register_write_count()));
    std::printf("  data writes     : %llu\n", static_cast<unsigned long long>(vdp.data_write_count()));
    std::printf("  68000 PC        : %06X\n", machine.m68k().registers().pc);
    std::printf("  display         : %s %ux%u\n", vdp.display_enabled() ? "on" : "off",
                vdp.screen_width(), vdp.screen_height());

    const auto bmp = scratch / "mercury_selftest.bmp";
    write_bmp24(bmp, pixels, vdp.screen_width(), vdp.screen_height(), stride);
    std::cout << "\n  frame written to " << bmp.string() << "\n";

    // Sample-rate agreement. The two sound chips are clocked from different
    // divisions of the master clock, and if either division loses its remainder
    // the chip falls behind and the tail of every frame ends up with only the
    // other one in it - which is a 60Hz gate on that chip's voices, heard as
    // distortion on whatever it is playing rather than as a pitch error.
    {
        mercury::genesis::Genesis timing;
        load_image(timing, rom_path);
        constexpr int kFrames = 60;
        for (int i = 0; i < kFrames; ++i) {
            timing.run_frame();
        }
        const double psg_per_frame =
            static_cast<double>(timing.psg().sample_buffer().size()) / kFrames;
        // The FM emits stereo pairs at its own rate; converted to the output
        // rate it has to line up with what the PSG produced.
        const double fm_native_per_frame =
            static_cast<double>(timing.ym2612().sample_buffer().size() / 2) / kFrames;
        const double fm_resampled_per_frame =
            fm_native_per_frame /
            (mercury::genesis::Ym2612::kSampleRateHz /
             static_cast<double>(mercury::gamegear::Psg::kDefaultSampleRate));

        std::printf("\nSample rate agreement\n");
        std::printf("  psg per frame    : %.1f\n", psg_per_frame);
        std::printf("  fm  per frame    : %.1f (%.1f after resampling)\n",
                    fm_native_per_frame, fm_resampled_per_frame);

        const double drift = std::abs(psg_per_frame - fm_resampled_per_frame);
        const bool aligned = drift < 8.0;
        if (!aligned) ++failures;
        std::printf("  %-40s %s  (drift %.1f samples/frame)\n",
                    "both chips fill the same frame", aligned ? "PASS" : "FAIL", drift);
    }

    failures += run_fm_check();

    std::cout << (failures == 0 ? "\nSELF TEST PASSED\n" : "\nSELF TEST FAILED\n");
    return failures == 0 ? 0 : 1;
}

}  // namespace

// One line of key=value pairs per ROM, for the library sweep. Kept separate
// from the human-readable path so the parser never has to cope with prose.
// Set by --piano: plug a Miracle Piano keyboard into port 2 before the run.
// The only way to tell whether the game accepts the instrument is to look at
// the screen it draws, and this is the tool that can take that picture.
bool g_attach_piano = false;

int run_summary(const std::filesystem::path& rom, int frames, const std::filesystem::path& shot,
                bool auto_input) {
    mercury::genesis::Genesis machine;
    const auto load = load_image(machine, rom);
    if (!load.loaded) {
        std::printf("status=LOAD_FAIL\n");
        return 1;
    }
    if (g_attach_piano) {
        machine.port2().set_kind(mercury::genesis::ControllerPort::Kind::miracle_piano);
    }

    // Sample the PC periodically. A game that is running visits many different
    // addresses; one that has wedged sits on a handful, and that distinction is
    // what separates "boots slowly" from "crashed".
    // Judging a game on the single frame it happens to stop on is unreliable:
    // the display is legitimately off during transitions, and a title screen
    // can be mid-fade. Track the BEST frame over the second half of the run
    // instead, and count how often the display was on at all.
    std::vector<std::uint32_t> pc_samples;
    double best_coverage = 0.0;
    std::size_t best_colors = 0;
    int display_on_frames = 0;
    const auto measure_frame = [&](double& coverage_out, std::size_t& colors_out) {
        const auto& v = machine.vdp();
        const auto w = v.screen_width();
        const auto h = v.screen_height();
        const auto& px = v.frame_pixels();
        std::size_t non_black = 0;
        std::vector<std::uint32_t> seen;
        seen.reserve(static_cast<std::size_t>(w) * h);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto i = (static_cast<std::size_t>(y) * mercury::genesis::Vdp::kMaxWidth + x) * 3;
                const std::uint32_t rgb = (static_cast<std::uint32_t>(px[i]) << 16) |
                                          (static_cast<std::uint32_t>(px[i + 1]) << 8) | px[i + 2];
                if (rgb != 0) ++non_black;
                seen.push_back(rgb);
            }
        }
        std::sort(seen.begin(), seen.end());
        colors_out = static_cast<std::size_t>(std::unique(seen.begin(), seen.end()) - seen.begin());
        coverage_out = 100.0 * static_cast<double>(non_black) /
                       static_cast<double>(static_cast<std::size_t>(w) * h);
    };

    // Tap Start and C on a slow cycle. A great many games sit on a title, a
    // save-initialisation notice or a story screen until someone presses
    // something, and a sweep that never presses anything scores those as
    // failures - they are the single largest source of false alarms here.
    const auto drive_input = [&](int frame) {
        using mercury::genesis::Button;
        const int phase = frame % 120;
        machine.port1().set_button(Button::start, phase >= 20 && phase < 28);
        machine.port1().set_button(Button::c, phase >= 60 && phase < 68);
        machine.port1().set_button(Button::a, phase >= 90 && phase < 98);
    };

    // MERCURY_RAM_DUMP=<hex work-RAM address> prints 64 words from there at the
    // end of the run. Virtua Racing's self-test stores its per-test results
    // from 0xFF311A, so this turns "DRAM R/W: ERROR" into which sub-test failed.
    const long ram_dump = [] {
        const char* e = std::getenv("MERCURY_RAM_DUMP");
        return e == nullptr ? -1L : std::strtol(e, nullptr, 16);
    }();

    // Virtua Racing has a built-in SVP self-test, reached by holding every
    // button on pad 1 through boot. It checks DRAM read/write, both
    // cell-arrange windows, DSP ROM reads, IRAM, the DRAM overwrite flag and
    // the pointer registers, and reports pass/fail for each - which makes the
    // cartridge its own oracle instead of making me infer correctness from the
    // shape of a polygon. MERCURY_HOLD_ALL=1 turns it on.
    const bool hold_all = [] {
        const char* e = std::getenv("MERCURY_HOLD_ALL");
        return e != nullptr && e[0] != '0';
    }();

    // MERCURY_HOLD=<letters> holds exactly those buttons for the whole run:
    // u d l r a b c s. Pressing ONE button and watching what the game thinks it
    // received is the only way to catch a pad read that turns a single press
    // into a combination - which is how Awesome Possum was failing.
    const std::string hold_list = [] {
        const char* e = std::getenv("MERCURY_HOLD");
        return e == nullptr ? std::string{} : std::string{e};
    }();
    // MERCURY_HOLD_FROM=<frame> delays the hold, so a button can be pressed
    // after the game is already running instead of through its boot - holding
    // one at power-on often selects a deliberate mode and proves nothing.
    // MERCURY_PAD6=1 plugs a six-button pad into port 1. Games older than that
    // pad can misread its extra strobe phases - which is why the real one has a
    // MODE button to make it behave as three-button.
    if (const char* e = std::getenv("MERCURY_PAD6"); e != nullptr && e[0] != '0') {
        machine.port1().set_kind(mercury::genesis::ControllerPort::Kind::pad6);
    }
    const int hold_from = [] {
        const char* e = std::getenv("MERCURY_HOLD_FROM");
        return e == nullptr ? 0 : std::atoi(e);
    }();
    const auto apply_hold_list = [&]() {
        using mercury::genesis::Button;
        for (const char ch : hold_list) {
            switch (ch) {
                case 'u': machine.port1().set_button(Button::up, true); break;
                case 'd': machine.port1().set_button(Button::down, true); break;
                case 'l': machine.port1().set_button(Button::left, true); break;
                case 'r': machine.port1().set_button(Button::right, true); break;
                case 'a': machine.port1().set_button(Button::a, true); break;
                case 'b': machine.port1().set_button(Button::b, true); break;
                case 'c': machine.port1().set_button(Button::c, true); break;
                case 's': machine.port1().set_button(Button::start, true); break;
                default: break;
            }
        }
    };

    // MERCURY_INPUT_SCRIPT="frame:letters,frame:letters,..." holds those buttons
    // from each frame until the next entry. Menus that need a direction and then
    // a confirm cannot be driven by a single fixed hold, and tapping blindly
    // just cycles past the item you wanted.
    std::vector<std::pair<int, std::string>> script;
    if (const char* e = std::getenv("MERCURY_INPUT_SCRIPT")) {
        std::string text(e);
        std::size_t pos = 0;
        while (pos < text.size()) {
            const auto comma = text.find(',', pos);
            const auto piece = text.substr(pos, comma == std::string::npos ? comma : comma - pos);
            const auto colon = piece.find(':');
            if (colon != std::string::npos) {
                script.emplace_back(std::atoi(piece.substr(0, colon).c_str()),
                                    piece.substr(colon + 1));
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        std::sort(script.begin(), script.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::printf("input script: %zu steps\n", script.size());
    }
    const auto apply_letters = [&](const std::string& letters) {
        using mercury::genesis::Button;
        for (const char ch : letters) {
            switch (ch) {
                case 'u': machine.port1().set_button(Button::up, true); break;
                case 'd': machine.port1().set_button(Button::down, true); break;
                case 'l': machine.port1().set_button(Button::left, true); break;
                case 'r': machine.port1().set_button(Button::right, true); break;
                case 'a': machine.port1().set_button(Button::a, true); break;
                case 'b': machine.port1().set_button(Button::b, true); break;
                case 'c': machine.port1().set_button(Button::c, true); break;
                case 's': machine.port1().set_button(Button::start, true); break;
                // Six-button pad only. A 32X cartridge is far likelier to want
                // these than a Mega Drive one - the 32X shipped into a
                // six-button world - and a menu bound to one of them cannot
                // be driven at all from a three-button pad.
                case 'x': machine.port1().set_button(Button::x, true); break;
                case 'y': machine.port1().set_button(Button::y, true); break;
                case 'z': machine.port1().set_button(Button::z, true); break;
                case 'm': machine.port1().set_button(Button::mode, true); break;
                default: break;
            }
        }
    };

    const bool no_reset = [] {
        const char* e = std::getenv("MERCURY_NO_RESET");
        return e != nullptr && *e != '0';
    }();
    const int watch_vector = [] {
        const char* e = std::getenv("MERCURY_EXC_WATCH");
        return e == nullptr ? -1 : std::atoi(e);
    }();
    bool watch_reported = false;
    // MERCURY_INPUT_FROM=<frame> holds off the button masher until then, so a
    // cartridge gets a clean run at its own startup before anything is
    // pressed. The 32X startup is a conversation between the two sides and is
    // worth leaving alone while it happens.
    const int input_from = [] {
        const char* e = std::getenv("MERCURY_INPUT_FROM");
        return e == nullptr ? 0 : std::atoi(e);
    }();

    // MERCURY_PAD6=1 puts a six-button pad in port 1.
    if (const char* e = std::getenv("MERCURY_PAD6"); e != nullptr && *e != '0') {
        machine.port1().set_kind(mercury::genesis::ControllerPort::Kind::pad6);
        std::printf("port 1: six-button pad\n");
    }

    // WHERE IS THE SH-2 ACTUALLY SPENDING ITS TIME?
    //
    // A stuck 32X cartridge reports hundreds of millions of instructions and
    // no picture, and neither number says which loop it is in. These two are
    // how to find out, and both are off unless asked for because either one
    // costs something on every instruction.
    //
    // MERCURY_SH2_PROFILE=1     count instructions by PC, report the hottest.
    // MERCURY_SH2_TRACE=<file>  write every instruction to a file, with
    //                           MERCURY_SH2_TRACE_SKIP to start late and
    //                           MERCURY_SH2_TRACE_LIMIT to bound the size.
    // MERCURY_SH2_TRACE_WHICH   m, s or ms; both by default.
    std::array<std::unordered_map<std::uint32_t, std::uint64_t>, 2> sh2_pc_counts;
    const bool sh2_profile = [] {
        const char* e = std::getenv("MERCURY_SH2_PROFILE");
        return e != nullptr && *e != '0';
    }();
    std::FILE* sh2_trace_file = nullptr;
    if (const char* path = std::getenv("MERCURY_SH2_TRACE"); path != nullptr && *path != '\0') {
        sh2_trace_file = std::fopen(path, "wb");
        if (sh2_trace_file == nullptr) {
            std::printf("sh2 trace: cannot open %s\n", path);
        }
    }
    const std::uint64_t trace_skip = [] {
        const char* e = std::getenv("MERCURY_SH2_TRACE_SKIP");
        return e == nullptr ? 0ull : std::strtoull(e, nullptr, 10);
    }();
    const std::uint64_t trace_limit = [] {
        const char* e = std::getenv("MERCURY_SH2_TRACE_LIMIT");
        return e == nullptr ? 2000000ull : std::strtoull(e, nullptr, 10);
    }();
    const std::string trace_which = [] {
        const char* e = std::getenv("MERCURY_SH2_TRACE_WHICH");
        return std::string(e == nullptr ? "ms" : e);
    }();
    const bool trace_master = trace_which.find('m') != std::string::npos;
    const bool trace_slave = trace_which.find('s') != std::string::npos;
    // MERCURY_SH2_TRACE_FROM_PC=<hex> holds the trace back until a traced
    // processor first executes that address, so a trace can start at the
    // cartridge's entry point instead of inside firmware startup that no
    // other emulator runs the same way.
    const std::uint32_t trace_from_pc = [] {
        const char* e = std::getenv("MERCURY_SH2_TRACE_FROM_PC");
        return e == nullptr ? 0u : static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
    }();
    bool trace_started = trace_from_pc == 0;
    // MERCURY_SH2_TRACE_FIRST=1 writes each address only the first time it
    // runs, so the trace is the order in which code is first reached. That
    // covers a whole boot in a small file and ignores how long any wait loop
    // spins, which is what makes it comparable with another emulator's.
    const bool trace_first = [] {
        const char* e = std::getenv("MERCURY_SH2_TRACE_FIRST");
        return e != nullptr && *e != '0';
    }();
    // MERCURY_SH2_TRACE_ONLY=<hex>[,<hex>...] narrows the trace to a handful of
    // addresses and writes the registers at every hit. A first-visit trace says
    // which code runs; this says which VALUES it sees, which is what is left to
    // compare once two emulators reach the same code and only one of them
    // draws. ares takes the same switch under the name ARES_TRACE_ONLY.
    const std::vector<std::uint32_t> trace_only = [] {
        std::vector<std::uint32_t> list;
        const char* e = std::getenv("MERCURY_SH2_TRACE_ONLY");
        if (e == nullptr) return list;
        std::string text(e);
        std::size_t at = 0;
        while (at < text.size()) {
            const std::size_t comma = text.find(',', at);
            const std::string piece =
                text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (!piece.empty()) {
                list.push_back(static_cast<std::uint32_t>(std::strtoul(piece.c_str(), nullptr, 16)));
            }
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        return list;
    }();
    std::array<std::unordered_map<std::uint32_t, std::uint8_t>, 2> first_seen;
    std::uint64_t traced = 0;
    std::uint64_t seen = 0;
    // MERCURY_SH2_WATCH=<hex> names one SDRAM word and prints the instruction
    // that changes it. "Who writes this?" is otherwise unanswerable here: the
    // bus does not know the PC, and a full trace of a booting game is hundreds
    // of millions of lines. Comparing against another emulator turns a missing
    // picture into a single word that one of them sets and the other does not,
    // and this is what names the code that sets it.
    const std::uint32_t watch_address = [] {
        const char* e = std::getenv("MERCURY_SH2_WATCH");
        return e == nullptr ? 0u : static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
    }();
    std::uint16_t watch_value = 0;
    bool watch_primed = false;
    std::uint32_t watch_lines = 0;
    // MERCURY_SH2_TRACE_UNTIL=<hex>: keep the trace in a ring and write it out
    // when a processor first reaches that address. See the dump site below.
    const std::uint32_t trace_until = [] {
        const char* e = std::getenv("MERCURY_SH2_TRACE_UNTIL");
        return e == nullptr ? 0u : static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
    }();
    const std::uint64_t irq_log_limit = [] {
        const char* e = std::getenv("MERCURY_SH2_IRQ_LOG");
        return e == nullptr ? 0ull : std::strtoull(e, nullptr, 10);
    }();
    std::array<std::uint64_t, 2> irq_logged{};
    std::vector<std::string> trace_ring;
    std::size_t ring_at = 0;
    if (trace_until != 0 && sh2_trace_file != nullptr) {
        // Bounded: the ring is the line limit, and a line is about 250 bytes.
        trace_ring.resize(trace_limit > 100000 ? 100000 : static_cast<std::size_t>(trace_limit));
    }
    if (sh2_profile || sh2_trace_file != nullptr || watch_address != 0 || irq_log_limit != 0) {
        machine.thirtytwox().set_sh2_trace(
            [&](int which, const mercury::core::Sh2StepResult& step,
                const mercury::core::Sh2Registers& regs) {
                if (!step.executed) return;
                if (sh2_profile) ++sh2_pc_counts[which == 0 ? 0 : 1][step.pc_before];
                // MERCURY_SH2_IRQ_LOG=<n>: the first n interrupts each
                // processor takes, with where it was, which vector answered and
                // the stack pointer AFTER the frame is pushed. An interrupt
                // that never returns shows up here as a handler address that
                // is not a handler, and a leaked frame shows as a stack
                // pointer that never comes back up.
                if (irq_log_limit != 0 && step.message == "interrupt") {
                    auto& seen_irqs = irq_logged[which == 0 ? 0 : 1];
                    if (seen_irqs < irq_log_limit) {
                        ++seen_irqs;
                        std::printf("[irq] %c #%llu at %08X -> handler %08X sp %08X sr %08X\n",
                                    which == 0 ? 'M' : 'S',
                                    static_cast<unsigned long long>(seen_irqs), step.pc_before,
                                    step.pc_after, regs.r[15], regs.sr);
                    }
                }
                if (watch_address != 0) {
                    // A palette entry is watched the same way as an SDRAM word:
                    // $4200-$43FF, with or without the $20000000 the SH-2s use
                    // to reach it. The palette is not memory on this bus, so it
                    // has to be read from its own array.
                    const bool palette_watch = (watch_address & 0x0000FE00u) == 0x00004200u;
                    // $4020-$402F: a communication port, watched the same way.
                    // "Which instruction put THAT in the port?" is the question
                    // a two-sided handshake deadlock comes down to, and the
                    // adapter cannot answer it - it never sees a PC.
                    const bool comm_watch = (watch_address & 0x0000FFF0u) == 0x00004020u;
                    std::uint16_t current = 0;
                    if (comm_watch) {
                        current = machine.thirtytwox().adapter().comm_read(
                            static_cast<int>((watch_address >> 1) & 7));
                    } else if (palette_watch) {
                        const auto& palette = machine.thirtytwox().bus().palette();
                        current = palette[(watch_address >> 1) & (palette.size() - 1)];
                    } else {
                        const auto& sdram = machine.thirtytwox().bus().sdram();
                        const std::size_t at = watch_address & (sdram.size() - 2);
                        current = static_cast<std::uint16_t>(
                            (static_cast<std::uint16_t>(sdram[at]) << 8) | sdram[at + 1]);
                    }
                    if (!watch_primed) {
                        watch_value = current;
                        watch_primed = true;
                    } else if (current != watch_value) {
                        if (watch_lines < 200) {
                            ++watch_lines;
                            std::printf("[watch] %08X: %04X -> %04X by %c %08X op %04X "
                                        "r0=%08X r1=%08X r2=%08X r13=%08X pr=%08X\n",
                                        watch_address, watch_value, current,
                                        which == 0 ? 'M' : 'S', step.pc_before, step.opcode,
                                        regs.r[0], regs.r[1], regs.r[2], regs.r[13], regs.pr);
                        }
                        watch_value = current;
                    }
                }
                if (sh2_trace_file == nullptr) return;
                if (which == 0 ? !trace_master : !trace_slave) return;
                if (!trace_started) {
                    if (step.pc_before != trace_from_pc) return;
                    trace_started = true;
                }
                if (trace_first &&
                    !first_seen[which == 0 ? 0 : 1].emplace(step.pc_before, 1).second) {
                    return;
                }
                if (!trace_only.empty() &&
                    std::find(trace_only.begin(), trace_only.end(), step.pc_before) ==
                        trace_only.end()) {
                    return;
                }
                if (++seen <= trace_skip) return;
                if (traced >= trace_limit) return;
                // Every general register. A value that goes wrong in r6..r13
                // is otherwise invisible - After Burner's corrupt palette mask
                // arrived through r6.
                char line[512];
                std::snprintf(line, sizeof(line),
                              "%c %08X %04X sr=%08X pr=%08X gbr=%08X "
                              "r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X "
                              "r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X "
                              "r12=%08X r13=%08X r14=%08X r15=%08X mach=%08X macl=%08X%s\n",
                              which == 0 ? 'M' : 'S', step.pc_before, step.opcode, regs.sr,
                              regs.pr, regs.gbr, regs.r[0], regs.r[1], regs.r[2], regs.r[3],
                              regs.r[4], regs.r[5], regs.r[6], regs.r[7], regs.r[8], regs.r[9],
                              regs.r[10], regs.r[11], regs.r[12], regs.r[13], regs.r[14],
                              regs.r[15], regs.mach, regs.macl,
                              step.in_delay_slot ? " ds" : "");
                // A CRASH IS READ BACKWARDS. MERCURY_SH2_TRACE_UNTIL keeps the
                // last MERCURY_SH2_TRACE_LIMIT lines in a ring and writes them
                // out the moment a processor reaches that address, which is the
                // only way to see the instructions BEFORE a runaway: a full
                // trace of the approach is hundreds of millions of lines and
                // the place to start it is exactly what is not known yet.
                if (!trace_ring.empty()) {
                    trace_ring[ring_at % trace_ring.size()] = line;
                    ++ring_at;
                    if (step.pc_before != trace_until) return;
                    const std::size_t held = ring_at < trace_ring.size() ? ring_at
                                                                         : trace_ring.size();
                    const std::size_t start = ring_at - held;
                    for (std::size_t i = 0; i < held; ++i) {
                        std::fputs(trace_ring[(start + i) % trace_ring.size()].c_str(),
                                   sh2_trace_file);
                    }
                    traced = trace_limit;  // one dump, at the first arrival
                    std::printf("sh2 trace: %zu lines written up to %c reaching %08X\n", held,
                                which == 0 ? 'M' : 'S', trace_until);
                    return;
                }
                ++traced;
                std::fputs(line, sh2_trace_file);
            });
        std::printf("sh2 diagnostics: profile=%s trace=%s\n", sh2_profile ? "on" : "off",
                    sh2_trace_file != nullptr ? "on" : "off");
    }

    // THE SAME TRACE, FOR THE 68000. Two 32X cartridges came down to what the
    // Mega Drive side was doing in a handshake, and there was no way to see it
    // from here: the SH-2s had a per-instruction sink and the 68000 had none.
    // Switches mirror the SH-2 ones so the two can be driven together.
    std::FILE* m68k_trace_file = nullptr;
    if (const char* path = std::getenv("MERCURY_M68K_TRACE"); path != nullptr && *path != '\0') {
        m68k_trace_file = std::fopen(path, "wb");
        if (m68k_trace_file == nullptr) std::printf("m68k trace: cannot open %s\n", path);
    }
    const bool m68k_trace_first = [] {
        const char* e = std::getenv("MERCURY_M68K_TRACE_FIRST");
        return e != nullptr && *e != '0';
    }();
    const std::uint32_t m68k_trace_from_pc = [] {
        const char* e = std::getenv("MERCURY_M68K_TRACE_FROM_PC");
        return e == nullptr ? 0u : static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
    }();
    const std::uint64_t m68k_trace_limit = [] {
        const char* e = std::getenv("MERCURY_M68K_TRACE_LIMIT");
        return e == nullptr ? 500000ull : std::strtoull(e, nullptr, 10);
    }();
    const std::vector<std::uint32_t> m68k_trace_only = [] {
        std::vector<std::uint32_t> list;
        const char* e = std::getenv("MERCURY_M68K_TRACE_ONLY");
        if (e == nullptr) return list;
        std::string text(e);
        std::size_t at = 0;
        while (at < text.size()) {
            const std::size_t comma = text.find(',', at);
            const std::string piece =
                text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (!piece.empty()) {
                list.push_back(static_cast<std::uint32_t>(std::strtoul(piece.c_str(), nullptr, 16)));
            }
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        return list;
    }();
    // MERCURY_M68K_TRACE_FRAMES=a-b traces only those frames (0-based, as
    // --dump-at counts them) and prefixes each line with the raster position,
    // "L<line>+<cycles into line>", so a per-line routine can be lined up
    // against the VDP's own timing.
    long m68k_trace_from_frame = -1;
    long m68k_trace_to_frame = -1;
    if (const char* e = std::getenv("MERCURY_M68K_TRACE_FRAMES"); e != nullptr && *e != '\0') {
        char* end = nullptr;
        m68k_trace_from_frame = std::strtol(e, &end, 10);
        m68k_trace_to_frame = (end != nullptr && *end == '-') ? std::strtol(end + 1, nullptr, 10)
                                                              : m68k_trace_from_frame;
    }
    long m68k_trace_frame = 0;
    std::unordered_map<std::uint32_t, std::uint8_t> m68k_first_seen;
    std::uint64_t m68k_traced = 0;
    bool m68k_started = m68k_trace_from_pc == 0;
    if (m68k_trace_file != nullptr) {
        machine.set_m68k_trace([&](const mercury::core::M68kStepResult& step,
                                   const mercury::core::M68kRegisters& regs) {
            if (!step.executed) return;
            if (m68k_trace_from_frame >= 0) {
                if (m68k_trace_frame < m68k_trace_from_frame || m68k_trace_frame > m68k_trace_to_frame) return;
                std::fprintf(m68k_trace_file, "L%u+%u ", machine.vdp().scanline(),
                             machine.vdp().cycles_into_line());
            }
            if (!m68k_started) {
                if (step.pc_before != m68k_trace_from_pc) return;
                m68k_started = true;
            }
            if (m68k_trace_first && !m68k_first_seen.emplace(step.pc_before, 1).second) return;
            if (!m68k_trace_only.empty() &&
                std::find(m68k_trace_only.begin(), m68k_trace_only.end(), step.pc_before) ==
                    m68k_trace_only.end()) {
                return;
            }
            if (m68k_traced >= m68k_trace_limit) return;
            ++m68k_traced;
            std::fprintf(m68k_trace_file,
                         "%06X %04X sr=%04X d0=%08X d1=%08X d2=%08X d3=%08X "
                         "a0=%08X a1=%08X a2=%08X a3=%08X a7=%08X\n",
                         step.pc_before, step.opcode, regs.sr, regs.d[0], regs.d[1], regs.d[2],
                         regs.d[3], regs.a[0], regs.a[1], regs.a[2], regs.a[3], regs.a[7]);
        });
        std::printf("m68k trace: on\n");
    }

    int cd_audio_peak = 0;
    // See the write below. Opened with a placeholder header the length is
    // patched into afterwards, exactly as the runtime's own capture does.
    std::FILE* pwm_wav = nullptr;
    std::size_t pwm_wav_samples = 0;
    if (const char* path = std::getenv("MERCURY_PWM_WAV"); path != nullptr && *path != '\0') {
        pwm_wav = std::fopen(path, "wb");
        if (pwm_wav == nullptr) {
            std::printf("pwm wav: cannot open %s\n", path);
        } else {
            const unsigned char header[44] = {};
            std::fwrite(header, 1, sizeof(header), pwm_wav);
        }
    }

    std::vector<int> frame_hash_at;
    if (const char* e = std::getenv("MERCURY_FRAME_HASH_AT"); e != nullptr && *e != '\0') {
        std::string text(e);
        std::size_t at = 0;
        while (at < text.size()) {
            const std::size_t comma = text.find(',', at);
            const std::string piece = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (!piece.empty()) frame_hash_at.push_back(std::atoi(piece.c_str()));
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
    }
    for (int frame = 0; frame < frames; ++frame) {
        m68k_trace_frame = frame;
        if (!script.empty()) {
            const std::string* active = nullptr;
            for (const auto& step : script) {
                if (frame >= step.first) active = &step.second; else break;
            }
            if (active != nullptr) apply_letters(*active);
        }
        if (hold_all) {
            using mercury::genesis::Button;
            for (const auto button : {Button::up, Button::down, Button::left, Button::right,
                                      Button::a, Button::b, Button::c, Button::start}) {
                machine.port1().set_button(button, true);
            }
        }
        if (auto_input && frame >= input_from) drive_input(frame);
        if (!hold_list.empty() && frame >= hold_from) { apply_hold_list(); }
        // Some games finish their first-run setup and then wait on the console's
        // RESET button rather than any pad button - HardBall III prints exactly
        // that instruction. A sweep that never presses it scores them as
        // failures. Once only, and early: coverage is sampled over the back half
        // of the run, so a reset any later would be measuring a game that is
        // still booting rather than one that is broken.
        // MERCURY_NO_RESET=1 keeps the console out of it.
        //
        // A soft reset restarts the 32X from scratch while the 68000's work
        // RAM keeps its old contents, and a 32X cartridge does not survive
        // that here: T-Mek draws 63,677 pixels through a clean run and
        // exactly zero through one with a reset in it. Until that is fixed,
        // a 32X sweep that resets is measuring the reset and not the game.
        if (auto_input && frame == frames / 8 && !no_reset) {
            machine.soft_reset();
        }
        machine.run_frame();
        // MERCURY_EXC_WATCH=<vector> reports the first time that exception is
        // taken: which frame, what the processor was doing, and what is
        // actually sitting at the handler it jumped to. "The interrupt fired
        // once and everything afterwards is wreckage" is only half an answer
        // without knowing whether the handler existed yet.
        if (watch_vector >= 0 && !watch_reported) {
            const auto& tally = machine.m68k().exception_tally();
            if (tally[static_cast<std::size_t>(watch_vector)] != 0) {
                watch_reported = true;
                const auto& ram = machine.work_ram();
                // The trampoline is `jmp <long>`, so the destination is the
                // two words after the opcode.
                const std::uint32_t slot =
                    0x880200u + static_cast<std::uint32_t>(watch_vector - 1) * 6u + 2u;
                const std::uint32_t target =
                    (static_cast<std::uint32_t>(machine.read16(slot)) << 16) |
                    machine.read16(slot + 2);
                std::printf("exception %d first taken at frame %d, stacked PC %06X\n",
                            watch_vector, frame, machine.m68k().last_exception_pc());
                std::printf("  trampoline jumps to %06X, and work RAM there holds:", target);
                for (int at = 0; at < 12; ++at) {
                    std::printf(" %02X", ram[(target + static_cast<std::uint32_t>(at)) & 0xFFFF]);
                }
                std::printf("\n");
            }
        }
        // MERCURY_PWM_WAV=<path> writes the 32X's PWM output ON ITS OWN, before
        // it is mixed with the Mega Drive's chips. "The game is quiet" and "the
        // game is silent" are different faults with the same summary line, and
        // a peak or an RMS cannot tell a short burst of real audio from a
        // constant hum - the waveform can.
        if (pwm_wav != nullptr) {
            auto& pwm = machine.thirtytwox().adapter().pwm();
            const auto& samples = pwm.sample_buffer();
            if (!samples.empty()) {
                std::fwrite(samples.data(), sizeof(std::int16_t), samples.size(), pwm_wav);
                pwm_wav_samples += samples.size();
                pwm.clear_sample_buffer();
            }
        }
        // The Sega CD's PCM and CD audio, measured and drained each frame the
        // way the host drains it, so a long run does not pile it up.
        if (machine.cd_mode()) {
            auto& cd = machine.segacd();
            for (const auto sample : cd.audio()) {
                cd_audio_peak = std::max(cd_audio_peak, std::abs(static_cast<int>(sample)));
            }
            cd.clear_audio();
        }
        // MERCURY_FRAME_HASH_AT=a,b,c prints "framehash <frame> <hash>" of the
        // Mega Drive picture at those frames (0-based). Two runs of the same
        // game that differ in one rendering rule can then be compared without
        // keeping a picture of every frame of every game: same hash, same
        // picture.
        if (std::find(frame_hash_at.begin(), frame_hash_at.end(), frame) != frame_hash_at.end()) {
            const auto& v = machine.vdp();
            const auto& px = v.frame_pixels();
            std::uint64_t hash = 1469598103934665603ull;
            for (std::uint32_t y = 0; y < v.screen_height(); ++y) {
                const auto row = static_cast<std::size_t>(y) * mercury::genesis::Vdp::kMaxWidth * 3;
                for (std::size_t i = 0; i < static_cast<std::size_t>(v.screen_width()) * 3; ++i) {
                    hash = (hash ^ px[row + i]) * 1099511628211ull;
                }
            }
            std::printf("framehash %d %016llX\n", frame, static_cast<unsigned long long>(hash));
        }
        pc_samples.push_back(machine.m68k().registers().pc);
        if (machine.vdp().display_enabled()) ++display_on_frames;
        // Sampling every eighth frame in the back half keeps the cost sane.
        if (frame >= frames / 2 && (frame % 8) == 0) {
            double coverage = 0.0;
            std::size_t colors = 0;
            measure_frame(coverage, colors);
            if (coverage > best_coverage) best_coverage = coverage;
            if (colors > best_colors) best_colors = colors;
        }
    }
    if (pwm_wav != nullptr) {
        // Patch the two lengths now that they are known.
        const auto rate = static_cast<std::uint32_t>(mercury::thirtytwox::Pwm::kOutputRate);
        const auto data_bytes = static_cast<std::uint32_t>(pwm_wav_samples * 2);
        unsigned char header[44] = {};
        const auto put32 = [&header](int at, std::uint32_t value) {
            header[at] = static_cast<unsigned char>(value & 0xFF);
            header[at + 1] = static_cast<unsigned char>((value >> 8) & 0xFF);
            header[at + 2] = static_cast<unsigned char>((value >> 16) & 0xFF);
            header[at + 3] = static_cast<unsigned char>((value >> 24) & 0xFF);
        };
        const auto put16 = [&header](int at, std::uint16_t value) {
            header[at] = static_cast<unsigned char>(value & 0xFF);
            header[at + 1] = static_cast<unsigned char>((value >> 8) & 0xFF);
        };
        std::memcpy(header, "RIFF", 4);
        put32(4, 36 + data_bytes);
        std::memcpy(header + 8, "WAVEfmt ", 8);
        put32(16, 16);
        put16(20, 1);
        put16(22, 1);
        put32(24, rate);
        put32(28, rate * 2);
        put16(32, 2);
        put16(34, 16);
        std::memcpy(header + 36, "data", 4);
        put32(40, data_bytes);
        std::fseek(pwm_wav, 0, SEEK_SET);
        std::fwrite(header, 1, sizeof(header), pwm_wav);
        std::fclose(pwm_wav);
        std::printf("pwm wav: %zu samples\n", pwm_wav_samples);
    }

    std::sort(pc_samples.begin(), pc_samples.end());
    const auto distinct_pcs =
        static_cast<std::size_t>(std::unique(pc_samples.begin(), pc_samples.end()) - pc_samples.begin());

    const auto& vdp = machine.vdp();
    const auto width = vdp.screen_width();
    const auto height = vdp.screen_height();
    const auto& pixels = vdp.frame_pixels();
    constexpr std::size_t stride = mercury::genesis::Vdp::kMaxWidth;

    std::size_t non_black = 0;
    std::vector<std::uint32_t> colors;
    colors.reserve(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto i = (static_cast<std::size_t>(y) * stride + x) * 3;
            const std::uint32_t rgb =
                (static_cast<std::uint32_t>(pixels[i]) << 16) |
                (static_cast<std::uint32_t>(pixels[i + 1]) << 8) | pixels[i + 2];
            if (rgb != 0) ++non_black;
            colors.push_back(rgb);
        }
    }
    std::sort(colors.begin(), colors.end());
    const auto distinct_colors =
        static_cast<std::size_t>(std::unique(colors.begin(), colors.end()) - colors.begin());
    const double coverage = 100.0 * static_cast<double>(non_black) /
                            static_cast<double>(static_cast<std::size_t>(width) * height);

    std::int32_t fm_peak = 0;
    for (auto s : machine.ym2612().sample_buffer()) {
        fm_peak = std::max<std::int32_t>(fm_peak, std::abs(static_cast<int>(s)));
    }
    std::int32_t psg_peak = 0;
    for (auto s : machine.psg().sample_buffer()) {
        psg_peak = std::max<std::int32_t>(psg_peak, std::abs(static_cast<int>(s)));
    }

    // Classify on the best frame seen, not the last one.
    // Real rendered content outranks the display-enable bit. Games routinely
    // switch the display off during vblank to buy VRAM bandwidth, and sampling
    // that bit once per frame lands exactly there - Lemmings 2 draws a full
    // scene while reporting the display off in 17 frames out of 1800.
    const char* status = "BOOTS_OK";
    if (machine.m68k_unimplemented_count() > 1000) status = "BAD_OPCODE";
    else if (distinct_pcs <= 2) status = "CPU_WEDGED";
    else if (best_coverage >= 0.5 && best_colors > 2) status = "BOOTS_OK";
    else if (display_on_frames < frames / 20) status = "DISPLAY_OFF";
    else if (best_coverage < 0.5) status = "BLACK_SCREEN";
    else status = "FLAT_SCREEN";

    // The 32X, for a sweep of 32X cartridges. Four things separate the ways a
    // 32X game can fail, and the summary line has to carry all four: whether
    // the adapter came on at all, whether the processors are still executing,
    // what video mode they asked for, and how much of the layer is actually
    // opaque. A game can boot the Mega Drive side perfectly and still show
    // nothing because only the 32X draws.
    const auto& unit = machine.thirtytwox();
    int layer_pixels = 0;
    if (unit.active() && unit.vdp().produces_picture()) {
        std::array<mercury::thirtytwox::Vdp::Pixel, mercury::thirtytwox::Vdp::kWidth> row{};
        for (int y = 0; y < unit.vdp().height(); ++y) {
            unit.vdp().render_line(y, row.data());
            for (const auto& pixel : row) layer_pixels += pixel.opaque ? 1 : 0;
        }
    }
    static const char* kModeNames[] = {"blank", "packed", "direct", "runlen"};
    // Which adapter register the SH-2s come back to most, and how hard. For a
    // sweep this is the triage column: a library full of games stuck on the
    // same register is one bug, not thirty.
    std::uint32_t busiest_word = 0;
    std::uint32_t busiest_reads = 0;
    {
        const auto& tally = unit.bus().register_reads();
        for (std::size_t word = 0; word < tally.size(); ++word) {
            if (tally[word] > busiest_reads) {
                busiest_reads = tally[word];
                busiest_word = static_cast<std::uint32_t>(word);
            }
        }
    }

    std::printf("status=%s frames=%u pcs=%zu coverage=%.1f colors=%zu dispon=%d "
                "w=%u h=%u m68kbad=%llu z80bad=%llu z80insn=%llu z80int=%llu "
                "fmpeak=%d psgpeak=%d pwmpeak=%d dma=%llu "
                "x32=%s x32mode=%s x32insn=%llu x32bad=%llu x32px=%d "
                "x32poll=%04X x32polls=%u "
                "scd=%s scdinsn=%llu scdbad=%llu cdcmds=%llu sectors=%llu cdpeak=%d\n",
                status, vdp.frames_elapsed(), distinct_pcs, best_coverage, best_colors,
                display_on_frames, width, height,
                static_cast<unsigned long long>(machine.m68k_unimplemented_count()),
                static_cast<unsigned long long>(machine.z80_unimplemented_count()),
                static_cast<unsigned long long>(machine.z80_instructions()),
                static_cast<unsigned long long>(machine.z80_interrupts()),
                fm_peak, psg_peak,
                // The 32X's own audio. A 32X title with fmpeak and psgpeak
                // both zero is not necessarily silent - it may be doing all of
                // its sound here, which is exactly Star Wars Arcade.
                static_cast<int>(unit.adapter().pwm().peak()),
                static_cast<unsigned long long>(vdp.dma_count()),
                unit.active() ? "on" : "off",
                kModeNames[static_cast<int>(unit.vdp().mode())],
                static_cast<unsigned long long>(unit.master_instructions()),
                static_cast<unsigned long long>(unit.unimplemented_opcodes()), layer_pixels,
                0x4000 + busiest_word * 2, busiest_reads,
                // The Sega CD: the sub CPU's work, the drive traffic and the
                // sectors read, and the level of PCM plus CD audio.
                machine.cd_mode() ? "on" : "off",
                static_cast<unsigned long long>(machine.segacd().sub_instructions()),
                static_cast<unsigned long long>(machine.segacd().sub_unimplemented()),
                static_cast<unsigned long long>(machine.segacd().cdd_commands()),
                static_cast<unsigned long long>(machine.segacd().sectors_decoded()),
                cd_audio_peak);
    // MERCURY_MEM_DUMP_DIR=<dir>: the 68000's work RAM as wram.bin and, for
    // a Sega CD, the sub CPU's program RAM and Word RAM as prg.bin / word.bin,
    // at the end of the run. For searching a game's own code for who writes
    // the register another processor is waiting on.
    if (const char* dir = std::getenv("MERCURY_MEM_DUMP_DIR"); dir != nullptr && *dir != '\0') {
        const std::filesystem::path out(dir);
        std::filesystem::create_directories(out);
        std::vector<std::uint8_t> wram(0x10000);
        for (std::uint32_t i = 0; i < 0x10000; i += 2) {
            const std::uint16_t w = machine.read16(0xFF0000 + i);
            wram[i] = static_cast<std::uint8_t>(w >> 8);
            wram[i + 1] = static_cast<std::uint8_t>(w);
        }
        std::ofstream(out / "wram.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(wram.data()), static_cast<std::streamsize>(wram.size()));
        if (machine.cd_mode()) {
            const auto& prg = machine.segacd().prg_ram();
            std::ofstream(out / "prg.bin", std::ios::binary)
                .write(reinterpret_cast<const char*>(prg.data()), static_cast<std::streamsize>(prg.size()));
            const auto& word = machine.segacd().word_ram();
            std::ofstream(out / "word.bin", std::ios::binary)
                .write(reinterpret_cast<const char*>(word.data()), static_cast<std::streamsize>(word.size()));
        }
    }
    if (machine.cd_mode()) {
        std::printf("%s\n", machine.segacd().describe().c_str());
        if (const auto& counts = machine.segacd().sub_pc_counts(); !counts.empty()) {
            std::vector<std::pair<std::uint64_t, std::uint32_t>> hot;
            std::uint64_t total = 0;
            for (const auto& [pc, n] : counts) { hot.push_back({n, pc}); total += n; }
            std::sort(hot.rbegin(), hot.rend());
            std::printf("sub CPU hottest PCs (%zu distinct):\n", counts.size());
            for (std::size_t i = 0; i < hot.size() && i < 24; ++i) {
                std::printf("   %06X  x%-10llu %5.1f%%\n", hot[i].second,
                            static_cast<unsigned long long>(hot[i].first),
                            100.0 * static_cast<double>(hot[i].first) / static_cast<double>(total));
            }
        }
        // MERCURY_SCD_PRG_DUMP=<hex>: 128 bytes of the sub CPU's program RAM
        // from there, for reading the loop a sub program is sitting in.
        if (const char* e = std::getenv("MERCURY_SCD_PRG_DUMP"); e != nullptr) {
            const auto at = static_cast<std::size_t>(std::strtoul(e, nullptr, 16)) & ~std::size_t{1};
            const auto& prg = machine.segacd().prg_ram();
            for (std::size_t row = 0; row < 8; ++row) {
                std::printf("  prg %06zX:", at + row * 16);
                for (std::size_t w = 0; w < 8; ++w) {
                    const std::size_t a = (at + row * 16 + w * 2) % prg.size();
                    std::printf(" %02X%02X", prg[a], prg[a + 1]);
                }
                std::printf("\n");
            }
        }
    }

    if (sh2_trace_file != nullptr) {
        std::fclose(sh2_trace_file);
        sh2_trace_file = nullptr;
        std::printf("sh2 trace: %llu instructions written\n",
                    static_cast<unsigned long long>(traced));
    }
    if (sh2_profile) {
        // A wedged processor lives on a handful of addresses. The percentages
        // are what say whether this is a spin-wait or a game doing work.
        //
        // MERCURY_SH2_PROFILE_TOP widens the list. Sixteen names the spin loop,
        // which is all that is wanted for a game that is stuck; lining this up
        // against another emulator's profile needs enough depth to reach the
        // drawing code, which is spread thin over an unrolled loop.
        const std::size_t profile_top = [] {
            const char* e = std::getenv("MERCURY_SH2_PROFILE_TOP");
            const long n = e == nullptr ? 16 : std::strtol(e, nullptr, 10);
            return n > 0 ? static_cast<std::size_t>(n) : std::size_t{16};
        }();
        for (int which = 0; which < 2; ++which) {
            std::vector<std::pair<std::uint64_t, std::uint32_t>> hot;
            std::uint64_t total = 0;
            for (const auto& [pc, count] : sh2_pc_counts[which]) {
                hot.push_back({count, pc});
                total += count;
            }
            std::sort(hot.rbegin(), hot.rend());
            std::printf("sh2 %s: %zu distinct PCs, %llu instructions; hottest:\n",
                        which == 0 ? "master" : "slave", sh2_pc_counts[which].size(),
                        static_cast<unsigned long long>(total));
            for (std::size_t i = 0; i < hot.size() && i < profile_top; ++i) {
                std::printf("   %08X  x%-12llu %5.1f%%\n", hot[i].second,
                            static_cast<unsigned long long>(hot[i].first),
                            total ? 100.0 * static_cast<double>(hot[i].first) /
                                        static_cast<double>(total)
                                  : 0.0);
            }
        }
    }

    if (machine.svp().present()) {
        std::printf("svp: insn=%llu unknown=%llu halted=%d\n",
                    static_cast<unsigned long long>(machine.svp().instructions()),
                    static_cast<unsigned long long>(machine.svp().unknown_opcodes()),
                    machine.svp().halted() ? 1 : 0);
        std::printf("svp: DRAM writes per 4K words:\n ");
        for (std::size_t i = 0; i < machine.svp().dram_write_buckets().size(); ++i) {
            std::printf(" %X:%llu", static_cast<unsigned>(i),
                        static_cast<unsigned long long>(machine.svp().dram_write_buckets()[i]));
        }
        std::printf("\nsvp: NON-ZERO DRAM words per 4K region:\n ");
        for (std::uint32_t i = 0; i < 16; ++i) {
            std::printf(" %X:%zu", i, machine.svp().dram_nonzero_in(i * 0x1000, 0x1000));
        }
        std::printf("\n");
        std::printf("svp: wait cycles skipped = %llu\n", (unsigned long long)machine.svp().wait_cycles_skipped());
        std::printf("svp: hottest DSP PCs\n"); { std::vector<std::pair<std::uint32_t,std::uint32_t>> hp; const auto& h = machine.svp().pc_histogram(); for (std::size_t i = 0; i < h.size(); ++i) if (h[i]) hp.push_back({h[i], (std::uint32_t)i}); std::sort(hp.rbegin(), hp.rend()); std::uint64_t tot = 0; for (auto& e : hp) tot += e.first; for (std::size_t i = 0; i < hp.size() && i < 12; ++i) std::printf("   PC %04X  x%-10u %5.1f%%\n", hp[i].second, hp[i].first, tot ? 100.0*hp[i].first/(double)tot : 0.0); }
        std::printf("svp: render cost = %llu DSP instructions avg over %llu samples (one display frame = 209600)\n", (unsigned long long)machine.svp().render_instructions_avg(), (unsigned long long)machine.svp().render_samples());
        std::printf("svp: DSP writes to completion cell 30FE02 = %llu\n", (unsigned long long)machine.svp().dsp_completion_writes());
        std::printf("svp: 32-bit ALU ops (OP a,p / OP a,a) = %llu\n", (unsigned long long)machine.svp().alu32_ops());
        std::printf("svp: CELL-WINDOW 68k reads = %llu (non-zero %llu)\n", (unsigned long long)machine.svp().cell_reads(), (unsigned long long)machine.svp().cell_nonzero_reads());
        std::printf("svp: IROM(FC00+) data reads=%llu fetches=%llu | L/OV conds=%llu | PMC non-blind=%llu\n", (unsigned long long)machine.svp().irom_reads(), (unsigned long long)machine.svp().irom_fetches(), (unsigned long long)machine.svp().carry_overflow_conds(), (unsigned long long)machine.svp().pmc_non_blind());
        std::printf("svp: PM modes the reference would REFUSE = %llu:", (unsigned long long)machine.svp().unhandled_modes()); for (const auto& m : machine.svp().unhandled_mode_words()) { if (m.second) std::printf(" %04X x%llu", m.first, (unsigned long long)m.second); } std::printf("\n");
        if (ram_dump >= 0) {
            std::printf("work RAM at %06lX:\n", ram_dump);
            for (int r = 0; r < 8; ++r) {
                std::printf("  %06lX:", ram_dump + r * 16);
                for (int w = 0; w < 8; ++w) {
                    const std::uint32_t a = static_cast<std::uint32_t>(ram_dump) + r * 16 + w * 2;
                    std::printf(" %04X", machine.read16(a));
                }
                std::printf("\n");
            }
        }
        std::printf("svp: cell-arrange DECREMENT steps = %llu\n", (unsigned long long)machine.svp().cell_decrement_steps());
        std::printf("svp: ST saturation bit set=%llu, MACS bit set=%llu, mld/mpya b-bit set=%llu\n", (unsigned long long)machine.svp().st_saturate_set(), (unsigned long long)machine.svp().st_macs_set(), (unsigned long long)machine.svp().mac_b_bit_set());
        std::printf("svp: SHARED BLIND SPOTS - unimplemented accumulator ops=%llu (ror=%llu rol=%llu inc=%llu dec=%llu), ((ri)) with a modifier=%llu\n", (unsigned long long)machine.svp().acc_op_unimplemented(), (unsigned long long)machine.svp().acc_op_kinds()[0], (unsigned long long)machine.svp().acc_op_kinds()[1], (unsigned long long)machine.svp().acc_op_kinds()[4], (unsigned long long)machine.svp().acc_op_kinds()[5], (unsigned long long)machine.svp().ptr2_mod_used());
        std::printf("svp: transfers started=%llu; buffer CHANGED since the DSP said done: A=%llu B=%llu\n", (unsigned long long)machine.svp().transfer_starts(), (unsigned long long)machine.svp().buffer_changed_a(), (unsigned long long)machine.svp().buffer_changed_b());
        std::printf("svp: 68000 zeroed a word the DSP had drawn: total=%llu, in the LIVE buffer=%llu\n", (unsigned long long)machine.svp().erased_words(), (unsigned long long)machine.svp().erased_words_live());
        std::printf("svp: 68000 buffer writes INTO THE BUFFER THE DSP IS DRAWING=%llu, into the other one=%llu\n", (unsigned long long)machine.svp().clear_collisions(), (unsigned long long)machine.svp().clear_clean());
        std::printf("svp: 68000 DRAM writes with DSP HALTED=%llu RUNNING=%llu\n", (unsigned long long)machine.svp().writes_68k_halted(), (unsigned long long)machine.svp().writes_68k_running());
        std::printf("svp: 68000 WRITES into DSP DRAM per 4K:\n "); for (std::size_t i = 0; i < 16; ++i) std::printf(" %X:%llu", (unsigned)i, (unsigned long long)machine.svp().writes_68k()[i]); std::printf("\n");
        std::printf("svp: DRAM words the 68000 took with the DSP HALTED=%llu, RUNNING=%llu\n", (unsigned long long)machine.svp().dram_reads_while_halted(), (unsigned long long)machine.svp().dram_reads_while_running());
        std::printf("svp: A15006 halt writes=%llu:", (unsigned long long)machine.svp().halt_writes()); for (const auto& hv : machine.svp().halt_values()) { if (hv.second) std::printf(" %04X x%llu", hv.first, (unsigned long long)hv.second); } std::printf("\n");
        { std::vector<std::pair<std::uint32_t,std::uint32_t>> wv; const auto& wc = machine.svp().cell_word_counts(); for (std::size_t i = 0; i < wc.size(); ++i) if (wc[i]) wv.push_back({wc[i], (std::uint32_t)i}); std::sort(wv.rbegin(), wv.rend()); std::printf("svp: top cell words (%zu distinct):\n", wv.size()); for (std::size_t i = 0; i < wv.size() && i < 16; ++i) std::printf("   %04X x%u\n", wv[i].second, wv[i].first); }
        std::printf("watched work-RAM word written %llu times, last from PC %06X\n",
                    (unsigned long long)machine.watch_write_count(), machine.watch_write_pc());
        std::printf("svp: RPL pointer steps=%llu (on writes %llu)\nsvp: colour indices written:\n ", (unsigned long long)machine.svp().rpl_pointer_steps(), (unsigned long long)machine.svp().rpl_write_steps()); for (std::size_t i = 0; i < 16; ++i) std::printf(" %X:%llu", (unsigned)i, (unsigned long long)machine.svp().pixel_histogram()[i]); std::printf("\n");
        std::printf("svp: 68k DRAM reads returning NON-ZERO = %llu\n", (unsigned long long)machine.svp().dram_nonzero_reads());
        std::printf("svp: CELL-mode writes=%llu  non-zero per 4K:\n ", (unsigned long long)machine.svp().cell_writes()); for (std::size_t i = 0; i < 16; ++i) std::printf(" %X:%llu", (unsigned)i, (unsigned long long)machine.svp().cell_nonzero_writes()[i]); std::printf("\n");
        std::printf("svp: PMC latch ADDRESS per 4K (what the DSP asks for):\n "); for (std::size_t i = 0; i < 16; ++i) std::printf(" %X:%llu", (unsigned)i, (unsigned long long)machine.svp().pm_latch_buckets()[i]); std::printf("\n");
        std::printf("svp: DRAM write MODE words used:\n"); for (const auto& m : machine.svp().dram_write_modes()) { if (m.second) std::printf("   mode %04X  x%llu  (region %02X, inc-bits %u, ovr %u, cell %u)\n", m.first, (unsigned long long)m.second, m.first & 0x1F, (m.first >> 11) & 7, (m.first >> 10) & 1, (m.first >> 14) & 1); }
        std::printf("svp: 68k DRAM READS per 4K:\n "); for (std::size_t i = 0; i < 16; ++i) std::printf(" %X:%llu", (unsigned)i, (unsigned long long)machine.svp().dram_read_buckets()[i]); std::printf("\n");
        std::printf("svp: NON-ZERO DRAM writes per 4K:\n "); for (std::size_t i = 0; i < 16; ++i) std::printf(" %X:%llu", (unsigned)i, (unsigned long long)machine.svp().dram_nonzero_writes()[i]); std::printf("\nsvp: non-zero writes using overwrite mode=%llu\n", (unsigned long long)machine.svp().dram_overwrite_writes());
        std::printf("svp: PM4 reads by DSP=%llu (non-zero %llu)\n", (unsigned long long)machine.svp().pm4_reads_dsp(), (unsigned long long)machine.svp().pm4_nonzero_reads());
        std::printf("svp: 68k cmd writes 30FE06=%llu 30FE08=%llu\n", (unsigned long long)machine.svp().cmd_30fe06_writes(), (unsigned long long)machine.svp().cmd_30fe08_writes());
        std::printf("svp: handshake 30FE02=%04X 30FE04=%04X 30FE06=%04X 30FE08=%04X\n", machine.svp().dram_word(0x7F01), machine.svp().dram_word(0x7F02), machine.svp().dram_word(0x7F03), machine.svp().dram_word(0x7F04));
        std::printf("svp: XSTwr(68k)=%llu XSTrd(dsp)=%llu PM0rd(dsp)=%llu lastXST=%04X\n", (unsigned long long)machine.svp().xst_writes_68k(), (unsigned long long)machine.svp().xst_reads_dsp(), (unsigned long long)machine.svp().pm0_reads_dsp(), machine.svp().last_xst());
        std::printf("svp: dramwr=%llu iramwr=%llu dramnonzero=%zu\n",
                    static_cast<unsigned long long>(machine.svp().dram_writes()),
                    static_cast<unsigned long long>(machine.svp().iram_writes()),
                    machine.svp().dram_nonzero());
std::printf("svp: first bad fetch at PC %04X, last at %04X\n", machine.svp().first_unknown_pc(), machine.svp().last_unknown_pc());
        for (const auto& [opcode, count] : machine.svp().unknown_samples()) {
            if (count != 0) {
                std::printf("  svp opcode %04X x%llu\n", opcode,
                            static_cast<unsigned long long>(count));
            }
        }
    }

    if (!shot.empty()) {
        write_bmp24(shot, pixels, width, height, stride);
    }

    for (const auto& [opcode, count] : machine.m68k_unimplemented_opcodes()) {
        std::printf("opcode=%04X count=%llu\n", opcode, static_cast<unsigned long long>(count));
    }
    return 0;
}

// For a game that wedges: what is it reading, and where is it looping.
// auto_input matters here as much as in the summary: a game that only touches
// the hardware we are hunting AFTER the title screen shows nothing without it.
int run_trace(const std::filesystem::path& rom, int frames, int trace_from, bool auto_input) {
    mercury::genesis::Genesis machine;
    const auto load = load_image(machine, rom);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }
    // Let it boot normally first, then tally only the steady state - otherwise
    // the one-time setup traffic outweighs the loop we care about.
    // trace_from lets the tally start at frame zero for a game that dies during
    // setup - the steady state of a halted game says nothing.
    const auto drive_input = [&](int frame) {
        using mercury::genesis::Button;
        const int phase = frame % 120;
        machine.port1().set_button(Button::start, phase >= 20 && phase < 28);
        machine.port1().set_button(Button::c, phase >= 60 && phase < 68);
        machine.port1().set_button(Button::a, phase >= 90 && phase < 98);
    };
    // MERCURY_HOLD works here too, so the pad log below can be read while a
    // specific button is held down.
    if (const char* e = std::getenv("MERCURY_HOLD_FROM"); e != nullptr) { (void)e; }
    const std::string trace_hold = [] {
        const char* e = std::getenv("MERCURY_HOLD");
        return e == nullptr ? std::string{} : std::string{e};
    }();
    const auto hold_now = [&]() {
        using mercury::genesis::Button;
        for (const char ch : trace_hold) {
            switch (ch) {
                case 'u': machine.port1().set_button(Button::up, true); break;
                case 'd': machine.port1().set_button(Button::down, true); break;
                case 'l': machine.port1().set_button(Button::left, true); break;
                case 'r': machine.port1().set_button(Button::right, true); break;
                case 'a': machine.port1().set_button(Button::a, true); break;
                case 'b': machine.port1().set_button(Button::b, true); break;
                case 'c': machine.port1().set_button(Button::c, true); break;
                case 's': machine.port1().set_button(Button::start, true); break;
                default: break;
            }
        }
    };
    for (int i = 0; i < trace_from; ++i) {
        if (auto_input) drive_input(i);
        if (!trace_hold.empty()) hold_now();
        machine.run_frame();
    }
    machine.set_trace_reads(true);
    for (int i = trace_from; i < frames; ++i) {
        if (auto_input) drive_input(i);
        if (!trace_hold.empty()) hold_now();
        machine.run_frame();
    }

    const auto describe_port = [](std::uint32_t address) -> const char* {
        if (address >= 0xC00000 && address <= 0xC00003) return "VDP data";
        if (address >= 0xC00004 && address <= 0xC00007) return "VDP status/control";
        if (address >= 0xC00008 && address <= 0xC0000F) return "VDP HV counter";
        if (address == 0xA10001) return "version register";
        if (address >= 0xA10002 && address <= 0xA10007) return "controller data";
        if (address >= 0xA10008 && address <= 0xA1000D) return "controller control";
        if (address >= 0xA11100 && address <= 0xA11101) return "Z80 bus request";
        if (address >= 0xA11200 && address <= 0xA11201) return "Z80 reset";
        if (address >= 0xA14000 && address <= 0xA14003) return "TMSS";
        if (address >= 0xA00000 && address <= 0xA0FFFF) return "Z80 space";
        return "";
    };

    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads;
    for (const auto& [address, count] : machine.read_tally()) reads.push_back({count, address});
    std::sort(reads.rbegin(), reads.rend());
    std::printf("\nHardware reads in the steady state\n");
    for (std::size_t i = 0; i < reads.size() && i < 12; ++i) {
        std::printf("  %06X  x%-10llu %s\n", reads[i].second,
                    static_cast<unsigned long long>(reads[i].first),
                    describe_port(reads[i].second));
    }
    if (reads.empty()) std::printf("  (none - the CPU is not touching hardware at all)\n");

    // A DMA out of a big buffer shows up as thousands of addresses read once or
    // twice each, which never reach a top-N list. Count them by range instead.
    std::uint64_t dram_reads = 0, dram_addrs = 0, cell_reads = 0, cell_addrs = 0;
    for (const auto& [address, count] : machine.read_tally()) {
        if (address >= 0x300000 && address <= 0x31FFFF) { dram_reads += count; ++dram_addrs; }
        else if (address >= 0x390000 && address <= 0x3AFFFF) { cell_reads += count; ++cell_addrs; }
    }
    std::printf("SVP windows: DRAM %llu reads over %llu addresses, "
                "cell-arrange %llu reads over %llu addresses\n",
                static_cast<unsigned long long>(dram_reads),
                static_cast<unsigned long long>(dram_addrs),
                static_cast<unsigned long long>(cell_reads),
                static_cast<unsigned long long>(cell_addrs));

    std::vector<std::pair<std::uint64_t, std::uint32_t>> pcs;
    for (const auto& [pc, count] : machine.pc_tally()) pcs.push_back({count, pc});
    std::sort(pcs.rbegin(), pcs.rend());
    std::printf("\nHottest PCs\n");
    for (std::size_t i = 0; i < pcs.size() && i < 10; ++i) {
        std::printf("  %06X  x%llu\n", pcs[i].second,
                    static_cast<unsigned long long>(pcs[i].first));
    }
    {
        static const char* const kVectorNames[] = {
            "reset SSP", "reset PC", "bus error", "address error", "illegal instruction",
            "divide by zero", "CHK", "TRAPV", "privilege violation", "trace",
            "line-A", "line-F"};
        constexpr std::size_t kNamed = sizeof(kVectorNames) / sizeof(*kVectorNames);
        const auto& tally = machine.m68k().exception_tally();
        std::printf("\n68000 exceptions taken\n");
        bool any = false;
        for (std::size_t v = 0; v < tally.size(); ++v) {
            if (tally[v] == 0) continue;
            any = true;
            const char* name = v < kNamed ? kVectorNames[v]
                             : (v >= 24 && v <= 31) ? "interrupt autovector"
                             : (v >= 32 && v <= 47) ? "TRAP" : "?";
            std::printf("  vector %2zu  x%-10llu %s\n", v,
                        static_cast<unsigned long long>(tally[v]), name);
        }
        if (!any) std::printf("  (none)\n");
        std::printf("  last: vector %u stacking PC %06X\n",
                    machine.m68k().last_exception_vector(),
                    machine.m68k().last_exception_pc());
    }

    std::printf("\nline-counter ticks=%llu  avg reload=%.2f\n", static_cast<unsigned long long>(machine.vdp().line_counter_ticks()), machine.vdp().line_counter_ticks() ? static_cast<double>(machine.vdp().line_counter_reload_sum()) / static_cast<double>(machine.vdp().line_counter_ticks()) : 0.0);
    std::printf("interrupt events (frame/line):\n"); for (const auto& e : machine.vdp().int_events()) { std::printf("   f%u l%u  %s\n", e.frame, e.line, e.what); }
    std::printf("reg1 writes (frame/line/value/pendingBefore):\n"); for (const auto& w : machine.vdp().reg1_log()) { std::printf("   f%u l%u = %02X  Vint=%s pendingBefore=%d\n", w.frame, w.line, w.value, (w.value & 0x20) ? "ON " : "off", w.pending_before ? 1 : 0); }
    std::printf("H-int raise scanlines:"); for (std::size_t i = 0; i < 256; ++i) { if (machine.vdp().hint_raise_lines()[i]) std::printf(" %zu(x%u)", i, machine.vdp().hint_raise_lines()[i]); } std::printf("\n");
    std::printf("H-int requests LOST (overwritten before service): %llu\n  lost on lines:", (unsigned long long)machine.vdp().hint_lost_count()); for (std::size_t i = 0; i < 256; ++i) { if (machine.vdp().hint_lost_lines()[i]) std::printf(" %zu(x%u)", i, machine.vdp().hint_lost_lines()[i]); } std::printf("\n");
    std::printf("DMA stall cycles total=%llu  (a frame is ~127800)\n", (unsigned long long)machine.vdp().total_dma_cycles());
    std::printf("H-int requests RAISED by the VDP: %llu\n", static_cast<unsigned long long>(machine.vdp().hint_raise_count()));
    std::printf("68000 interrupts: hint=%llu vint=%llu\n",
                static_cast<unsigned long long>(machine.m68k_hint_count()),
                static_cast<unsigned long long>(machine.m68k_vint_count()));
    std::printf("battery window written: %s\n", machine.battery_window_written() ? "yes" : "no");
    {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> drops;
        for (const auto& [address, count] : machine.dropped_rom_writes()) {
            drops.push_back({count, address});
        }
        std::sort(drops.rbegin(), drops.rend());
        std::printf("writes into cartridge space that went nowhere: %zu addresses\n", drops.size());
        for (std::size_t i = 0; i < drops.size() && i < 8; ++i) {
            std::printf("  %06X  x%llu\n", drops[i].second,
                        static_cast<unsigned long long>(drops[i].first));
        }
    }
    {
        const auto& zram = machine.z80_ram();
        std::size_t nonzero = 0;
        for (const auto byte : zram) {
            if (byte != 0) ++nonzero;
        }
        std::printf("Z80 RAM: %zu of %zu bytes non-zero%s\n  first 32:", nonzero, zram.size(),
                    nonzero == 0 ? "  <-- NO DRIVER UPLOADED (Z80 is running NOPs)" : "");
        for (std::size_t i = 0; i < 32; ++i) std::printf(" %02X", zram[i]);
        std::printf("\n");
    }
    {
            const auto& p1 = machine.port1();
            std::printf("pad1: %llu reads, %llu control writes\n",
                        (unsigned long long)p1.reads(), (unsigned long long)p1.control_writes());
            for (int bank = 0; bank < 2; ++bank) {
                std::printf("  TH %s bank:", bank == 0 ? "HIGH" : "LOW ");
                for (int v = 0; v < 256; ++v) {
                    if (p1.read_log(bank)[v] != 0) {
                        std::printf(" %02X:%llu", v, (unsigned long long)p1.read_log(bank)[v]);
                    }
                }
                std::printf("\n");
            }
        }
        {
        const auto& hv = machine.vdp().hv_v_histogram();
        std::vector<std::pair<std::uint64_t,int>> top;
        for (int i = 0; i < 256; ++i) if (hv[i]) top.push_back({hv[i], i});
        std::sort(top.rbegin(), top.rend());
        std::printf("HV-counter V values most read:");
        for (std::size_t i = 0; i < top.size() && i < 10; ++i)
            std::printf("  V=%02X x%llu", top[i].second, (unsigned long long)top[i].first);
        std::printf("\n  distinct V values seen: %zu\n", top.size());
    }
    {
        std::printf("reg1 writes (frame/line = value):\n ");
        for (const auto& e : machine.vdp().reg1_log()) {
            std::printf("  f%u l%u=%02X", e.frame, e.line, e.value);
        }
        std::printf("\n");
    }
    if (const char* e = std::getenv("MERCURY_RAM_DUMP"); e != nullptr) {
        const auto at = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
        std::printf("work RAM at %06X:\n", at);
        for (int r = 0; r < 10; ++r) {
            std::printf("  %06X:", at + r * 16);
            for (int w = 0; w < 8; ++w) {
                std::printf(" %04X", machine.read16(at + r * 16 + w * 2));
            }
            std::printf("\n");
        }
    }
    std::printf("watched write seen %llu times, last from PC %06X\n",
                (unsigned long long)machine.watch_write_count(), machine.watch_write_pc());
    {
        const auto& bl = machine.vdp().blank_start_lines();
        std::uint64_t legal = 0, illegal = 0;
        std::printf("display blanked starting on line:");
        for (std::size_t i = 0; i < bl.size(); ++i) {
            if (!bl[i]) continue;
            if (bl[i] > 20) std::printf("  %zu x%llu", i, (unsigned long long)bl[i]);
            if (i < 80 || i >= 246) legal += bl[i]; else illegal += bl[i];
        }
        std::printf("\n  inside the safe window: %llu   OUTSIDE it: %llu\n",
                    (unsigned long long)legal, (unsigned long long)illegal);
    }
    {
        std::printf("writers of the watched address:");
        for (const auto& e : machine.watch_writer_pcs()) {
            if (e.second) std::printf("  PC %06X x%llu", e.first, (unsigned long long)e.second);
        }
        std::printf("\n");
    }
    {
        std::printf("watched reg1 write, lines (>=20):");
        const auto& wl = machine.watch_lines();
        for (std::size_t i = 0; i < wl.size(); ++i) {
            if (wl[i] >= 20) std::printf("  %zu x%llu", i, (unsigned long long)wl[i]);
        }
        std::printf("\n  writers:");
        for (const auto& e : machine.ctrl_writer_pcs()) {
            if (e.second) std::printf("  PC %06X x%llu", e.first, (unsigned long long)e.second);
        }
        std::printf("\n");
    }
    if (!machine.watch_stack().empty()) {
        std::printf("stack at the first watched write (line %u):", machine.watch_stack_line());
        std::uint32_t off = 0;
        for (auto v : machine.watch_stack()) {
            std::printf("  +%u=%06X", off, v & 0xFFFFFF);
            off += 2;
        }
        std::printf("\n");
    }
    {
        const auto& hv = machine.vdp().hv_v_histogram();
        std::printf("V values the game actually saw, 0xC8..0xD8:");
        for (std::size_t v = 0xC8; v <= 0xD8; ++v) {
            std::printf("  %02zX=%llu", v, (unsigned long long)hv[v]);
        }
        std::uint64_t total = 0;
        for (auto n : hv) total += n;
        std::printf("\n  0xCF seen %llu times out of %llu reads\n",
                    (unsigned long long)hv[0xCF], (unsigned long long)total);
    }
    {
        const auto& rl = machine.vdp().hint_raise_lines();
        std::printf("H-ints raised on lines (>=20 times):");
        for (std::size_t i = 0; i < rl.size(); ++i) {
            if (rl[i] >= 20) std::printf("  %zu x%llu", i, (unsigned long long)rl[i]);
        }
        std::printf("\n");
    }
    {
        const auto& vr = machine.vdp().vram();
        const auto base = static_cast<std::uint32_t>(machine.vdp().plane_a_base_addr());
        const auto stride = static_cast<std::uint32_t>(machine.vdp().plane_a_row_cells()) * 2u;
        const auto bbase = static_cast<std::uint32_t>(machine.vdp().plane_b_base_addr());
        std::printf("vscrollA=%u vscrollB=%u  plane height=%u px  -> A wraps at line %d, B wraps at line %d\n",
                    machine.vdp().vsram_entry(0), machine.vdp().vsram_entry(1),
                    machine.vdp().plane_h_px(),
                    (int)machine.vdp().plane_h_px() - (int)machine.vdp().vsram_entry(0),
                    (int)machine.vdp().plane_h_px() - (int)machine.vdp().vsram_entry(1));
        for (std::uint32_t tile : {0x0D3u, 0x001u, 0x0E1u}) {
            const std::uint32_t ta = tile * 32u;
            std::uint32_t nonzero = 0;
            for (std::uint32_t i = 0; i < 32; ++i) {
                if (vr[(ta + i) & 0xFFFF] != 0) ++nonzero;
            }
            std::printf("filler tile pixels: tile %03X at VRAM %04X, %u of 32 bytes non-zero:",
                        tile, ta, nonzero);
            for (std::uint32_t i = 0; i < 32; ++i) std::printf(" %02X", vr[(ta + i) & 0xFFFF]);
            std::printf("\n");
        }
        if (machine.vdp().vsram_log_count()) {
            {
            const auto& rv = machine.vdp().reg10_values();
            std::printf("every reg10 value written:");
            for (std::size_t i = 0; i < rv.size(); ++i) {
                if (rv[i]) std::printf("  %02zX(%zu lines) x%llu", i, i + 1, (unsigned long long)rv[i]);
            }
            std::printf("\n");
        }
        std::printf("raw reg dump: reg16(plane size)=%02X reg11(scroll mode)=%02X reg13(hscroll base)=%02X\n",
                    machine.vdp().reg(0x10), machine.vdp().reg(0x0B), machine.vdp().reg(0x0D));
        std::printf("VSRAM writes: %llu total, last %zu:\n",
                        (unsigned long long)machine.vdp().vsram_log_count(),
                        machine.vdp().vsram_log().size());
            const auto& lg = machine.vdp().vsram_log();
            const auto pos = machine.vdp().vsram_log_pos();
            for (std::size_t i = 0; i < lg.size(); ++i) {
                const auto& e = lg[(pos + i) % lg.size()];
                if (!e[0] && !e[3]) continue;
                std::printf("    frame %5u line %3u  plane %c = %4u  %s\n", e[0], e[1],
                            e[2] ? 66 : 65, e[3], e[4] ? "DMA" : "port");
            }
        }
        if (machine.vdp().vram_watch_count()) {
            std::printf("VRAM watch window: %llu writes, last %zu in order:\n",
                        (unsigned long long)machine.vdp().vram_watch_count(),
                        machine.vdp().vram_watch_log().size());
            const auto& lg = machine.vdp().vram_watch_log();
            const auto pos = machine.vdp().vram_watch_pos();
            for (std::size_t i = 0; i < lg.size(); ++i) {
                const auto& e = lg[(pos + i) % lg.size()];
                if (e[1] == 0 && e[2] == 0 && e[0] == 0) continue;
                std::printf("    frame %5u line %3u  %04X = %04X  %s\n", e[0], e[1], e[2], e[3],
                            e[4] ? "DMA" : "port");
            }
        }
        std::printf("column profile at cell col 6, tile per cell row (A | B):\n");
        for (std::uint32_t r = 0; r < 64; ++r) {
            const std::uint32_t ao = (base + r * stride + 12u) & 0xFFFF;
            const std::uint32_t bo = (bbase + r * stride + 12u) & 0xFFFF;
            if (r % 8 == 0) std::printf("\n  row %2u: ", r);
            std::printf(" %04X/%04X", (vr[ao] << 8) | vr[ao + 1], (vr[bo] << 8) | vr[bo + 1]);
        }
        std::printf("\n");
        std::printf("plane B nametable rows (base %04X):\n", bbase);
        for (std::uint32_t r : {0u, 1u, 26u, 27u, 62u, 63u}) {
            std::printf("  B cell row %2u (pixel %3u):", r, r * 8u);
            for (std::uint32_t c = 0; c < 12; ++c) {
                const std::uint32_t off = (bbase + r * stride + c * 2u) & 0xFFFF;
                std::printf(" %04X", (vr[off] << 8) | vr[off + 1]);
            }
            std::printf("\n");
        }
        std::printf("plane A nametable rows (base %04X stride %u), first 14 cells:\n", base, stride);
        for (std::uint32_t r : {0u, 1u, 7u, 25u, 26u, 27u, 62u, 63u}) {
            std::printf("  cell row %2u (pixel %3u):", r, r * 8u);
            for (std::uint32_t c = 0; c < 14; ++c) {
                const std::uint32_t off = (base + r * stride + c * 2u) & 0xFFFF;
                std::printf(" %04X", (vr[off] << 8) | vr[off + 1]);
            }
            std::printf("\n");
        }
    }
    std::printf("code polling the HV counter:");
    for (const auto& e : machine.hv_reader_pcs()) {
        if (e.second) std::printf("  PC %06X x%llu", e.first, (unsigned long long)e.second);
    }
    std::printf("\n");
    std::printf("VDP port reads: status=%llu  hv-counter=%llu  data=%llu\n",
                (unsigned long long)machine.vdp_status_reads(),
                (unsigned long long)machine.vdp_hv_reads(),
                (unsigned long long)machine.vdp_data_reads());
    std::printf("\ndisplay=%s  frames=%u  dma=%llu\n",
                machine.vdp().display_enabled() ? "on" : "off",
                machine.vdp().frames_elapsed(),
                static_cast<unsigned long long>(machine.vdp().dma_count()));
    const auto& regs = machine.vdp().registers();
    std::printf("VDP regs: ");
    for (std::size_t i = 0; i < regs.size(); ++i) std::printf("%02X ", regs[i]);
    std::printf("\n");
    return 0;
}

// Watch one work-RAM longword and report every instruction that changes it.
// "The pointer is zero" is only half an answer; the other half is who zeroed
// it, and that is invisible from the crash site.
int run_watch(const std::filesystem::path& rom, std::uint32_t address, int frames) {
    mercury::genesis::Genesis machine;
    const auto load = load_image(machine, rom);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }
    const std::uint32_t offset = address & 0xFFFF;
    const auto& wram = machine.work_ram();
    const auto peek = [&]() -> std::uint32_t {
        return (static_cast<std::uint32_t>(wram[offset]) << 24) |
               (static_cast<std::uint32_t>(wram[(offset + 1) & 0xFFFF]) << 16) |
               (static_cast<std::uint32_t>(wram[(offset + 2) & 0xFFFF]) << 8) |
               static_cast<std::uint32_t>(wram[(offset + 3) & 0xFFFF]);
    };
    // Same all-buttons hold as the summary mode, so this can watch what the
    // cartridge's own self-test does rather than what the attract loop does.
    if (const char* e = std::getenv("MERCURY_HOLD_ALL"); e != nullptr && e[0] != '0') {
        using mercury::genesis::Button;
        for (const auto button : {Button::up, Button::down, Button::left, Button::right,
                                  Button::a, Button::b, Button::c, Button::start}) {
            machine.port1().set_button(button, true);
        }
    }
    std::uint32_t previous = peek();
    std::printf("watching %06X (initially %08X)\n", address, previous);
    const std::uint64_t budget = static_cast<std::uint64_t>(frames) * 200000ull;
    int reported = 0;
    for (std::uint64_t i = 0; i < budget && reported < 40; ++i) {
        const auto pc = machine.m68k().registers().pc;
        (void)machine.step();
        const auto now = peek();
        if (now != previous) {
            std::printf("  insn %9llu  PC %06X  %08X -> %08X\n",
                        static_cast<unsigned long long>(i), pc, previous, now);
            previous = now;
            ++reported;
        }
    }
    std::printf("done (%d changes reported)\n", reported);
    return 0;
}

// Run until the PC first reaches a given address and dump the registers. When a
// game jumps to its own crash handler, the question is what the failing test
// was looking at - and that lives in the address registers, not the PC.
// skip lets a LATER visit be caught. For a game stuck re-running its own setup,
// the first visit is just the normal boot and says nothing; the interesting
// question is who sends it back there the second and twentieth time.
int run_break(const std::filesystem::path& rom, std::uint32_t target, int frames, int skip) {
    mercury::genesis::Genesis machine;
    const auto load = load_image(machine, rom);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }
    if (const char* e = std::getenv("MERCURY_HOLD_ALL"); e != nullptr && e[0] != '0') {
        using mercury::genesis::Button;
        for (const auto button : {Button::up, Button::down, Button::left, Button::right,
                                  Button::a, Button::b, Button::c, Button::start}) {
            machine.port1().set_button(button, true);
        }
    }
    // The registers say what the failing test looked at; the PCs that led here
    // say which code did the looking. Without the trail you are guessing about
    // the caller, which is the whole question for a wild jump.
    constexpr std::size_t kTrail = 24;
    std::array<std::uint32_t, kTrail> trail{};
    std::size_t trail_pos = 0;
    const std::uint64_t budget = static_cast<std::uint64_t>(frames) * 200000ull;
    for (std::uint64_t i = 0; i < budget; ++i) {
        if (machine.m68k().registers().pc == target && skip-- <= 0) {
            const auto& r = machine.m68k().registers();
            std::printf("reached PC=%06X after %llu instructions\n\n", target,
                        static_cast<unsigned long long>(i));
            std::printf("  PCs leading here (oldest first):\n   ");
            for (std::size_t n = 0; n < kTrail; ++n) {
                const auto pc = trail[(trail_pos + n) % kTrail];
                std::printf(" %06X", pc);
                if ((n % 8) == 7) std::printf("\n   ");
            }
            std::printf("\n\n");
            for (int n = 0; n < 8; ++n) std::printf("  d%d = %08X\n", n, r.d[n]);
            for (int n = 0; n < 8; ++n) std::printf("  a%d = %08X\n", n, r.a[n]);
            std::printf("  sr = %04X\n", r.sr);
            {
                const auto& vr = machine.vdp().registers();
                std::printf("\n  VDP regs:");
                for (std::size_t i = 0; i < vr.size(); ++i) std::printf(" %02X", vr[i]);
                std::printf("\n  reg1=%02X -> display %s, V-int %s\n", vr[1],
                            (vr[1] & 0x40) ? "ON" : "off", (vr[1] & 0x20) ? "ENABLED" : "disabled");
            }
            const auto& wram = machine.work_ram();
            // Games that copy hot routines into RAM run them from the bottom of
            // it; without this the disassembly of a RAM-resident loop is
            // guesswork.
            std::printf("\n  work RAM FF0000-FF003F:\n");
            for (std::uint32_t row = 0x0000; row < 0x0040; row += 16) {
                std::printf("   FF%04X:", row);
                for (std::uint32_t c = 0; c < 16; c += 2) {
                    std::printf(" %02X%02X", wram[row + c], wram[row + c + 1]);
                }
                std::printf("\n");
            }
            // Work RAM around the handler tables most games keep near the top.
            std::printf("\n  work RAM FFE000-FFE07F:\n");
            for (std::uint32_t row = 0xE000; row < 0xE080; row += 16) {
                std::printf("   FF%04X:", row);
                for (std::uint32_t c = 0; c < 16; c += 2) {
                    std::printf(" %02X%02X", wram[row + c], wram[row + c + 1]);
                }
                std::printf("\n");
            }
            return 0;
        }
        trail[trail_pos] = machine.m68k().registers().pc;
        trail_pos = (trail_pos + 1) % kTrail;
        (void)machine.step();
    }
    std::printf("PC=%06X never reached\n", target);
    return 1;
}

// Dump what the VDP is actually being told to draw. A corrupt picture is either
// a corrupt name table (the game wrote the wrong thing, or a transfer went to
// the wrong place) or correct entries drawn wrongly - and those need completely
// different fixes, so the first job is to tell them apart.
int run_planes(const std::filesystem::path& rom, int frames, bool auto_input) {
    mercury::genesis::Genesis machine;
    const auto load = load_image(machine, rom);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }
    for (int i = 0; i < frames; ++i) {
        if (auto_input) {
            using mercury::genesis::Button;
            const int phase = i % 120;
            machine.port1().set_button(Button::start, phase >= 20 && phase < 28);
            machine.port1().set_button(Button::c, phase >= 60 && phase < 68);
            machine.port1().set_button(Button::a, phase >= 90 && phase < 98);
        }
        // MERCURY_HOLD / MERCURY_HOLD_FROM, matching run_summary exactly. This
        // mode explains a screenshot, so it has to be in the SAME game state as
        // the screenshot - analysing a different one has already produced two
        // confident and wrong conclusions today.
        static const int planes_hold_from = [] {
            const char* e = std::getenv("MERCURY_HOLD_FROM");
            return e == nullptr ? 0 : std::atoi(e);
        }();
        if (const char* hb = std::getenv("MERCURY_HOLD"); hb != nullptr && i >= planes_hold_from) {
            using mercury::genesis::Button;
            for (const char* q = hb; *q != '\0'; ++q) {
                switch (*q) {
                    case 'u': machine.port1().set_button(Button::up, true); break;
                    case 'd': machine.port1().set_button(Button::down, true); break;
                    case 'l': machine.port1().set_button(Button::left, true); break;
                    case 'r': machine.port1().set_button(Button::right, true); break;
                    case 'a': machine.port1().set_button(Button::a, true); break;
                    case 'b': machine.port1().set_button(Button::b, true); break;
                    case 'c': machine.port1().set_button(Button::c, true); break;
                    case 's': machine.port1().set_button(Button::start, true); break;
                    default: break;
                }
            }
        }
        // The same soft reset run_summary performs, at the same point. Without
        // it this mode ends up in a different place in the game than the
        // screenshot it is supposed to explain.
        if (auto_input && i == frames / 8) {
            machine.soft_reset();
        }
        machine.run_frame();
    }

    const auto& vdp = machine.vdp();
    const auto& regs = vdp.registers();
    const auto& vram = vdp.vram();
    const auto read16v = [&](std::uint32_t a) -> std::uint16_t {
        return static_cast<std::uint16_t>((vram[a & 0xFFFE] << 8) | vram[(a & 0xFFFE) + 1]);
    };

    const bool h40 = (regs[0x0C] & 0x01) != 0;
    const std::uint32_t plane_a = static_cast<std::uint32_t>(regs[0x02] & 0x38) << 10;
    const std::uint32_t plane_b = static_cast<std::uint32_t>(regs[0x04] & 0x07) << 13;
    const std::uint32_t window = static_cast<std::uint32_t>(regs[0x03] & (h40 ? 0x3E : 0x3F)) << 10;
    const std::uint32_t sprites = static_cast<std::uint32_t>(regs[0x05] & 0x7F) << 9;
    static const int kSizes[4] = {32, 64, 32, 128};
    const int pw = kSizes[regs[0x10] & 3];
    const int ph = kSizes[(regs[0x10] >> 4) & 3];

    std::printf("mode      : %s  planes %dx%d cells\n", h40 ? "H40 (320)" : "H32 (256)", pw, ph);
    std::printf("bases     : A=%04X B=%04X window=%04X sprites=%04X hscroll=%04X\n",
                plane_a, plane_b, window, sprites,
                static_cast<std::uint32_t>(regs[0x0D] & 0x3F) << 10);
    std::printf("scroll    : hscr=%d vscr=%d   window H=%02X V=%02X\n",
                regs[0x0B] & 3, (regs[0x0B] >> 2) & 1, regs[0x11], regs[0x12]);
    std::printf("backdrop  : reg7=%02X (palette %d colour %d)   shadow/hilite=%s\n",
                regs[0x07], (regs[0x07] >> 4) & 3, regs[0x07] & 0x0F,
                (regs[0x0C] & 0x08) ? "ON" : "off");

    // A screen that renders as one flat colour is nearly always the palette,
    // not the tilemaps, so show it.
    const auto& cram = vdp.cram();
    std::size_t distinct_cram = 0;
    {
        std::vector<std::uint16_t> seen(cram.begin(), cram.end());
        std::sort(seen.begin(), seen.end());
        distinct_cram = static_cast<std::size_t>(std::unique(seen.begin(), seen.end()) - seen.begin());
    }
    std::printf("CRAM      : %zu distinct entries of %zu\n", distinct_cram, cram.size());
    std::printf("VDP writes: vram=%llu cram=%llu vsram=%llu invalid=%llu (total %llu)\n",
                static_cast<unsigned long long>(vdp.vram_write_count()),
                static_cast<unsigned long long>(vdp.cram_write_count()),
                static_cast<unsigned long long>(vdp.vsram_write_count()),
                static_cast<unsigned long long>(vdp.invalid_write_count()),
                static_cast<unsigned long long>(vdp.data_write_count()));
    for (int line = 0; line < 4; ++line) {
        std::printf("  pal %d: ", line);
        for (int i = 0; i < 16; ++i) std::printf("%03X ", cram[line * 16 + i] & 0x0EEE);
        std::printf("\n");
    }

    // "Writes plenty but the screen is empty" is answered by WHERE they land.
    {
        const auto s = vdp.last_d_source();
        const auto d = vdp.last_d_dest();
        const auto n = vdp.last_d_length();
        std::printf("last region-D DMA: VRAM %04X <- %06X x%u words, auto-increment %u\n", d, s,
                    n, vdp.last_d_inc());
        std::size_t bad = 0;
        for (std::uint32_t w = 0; w < n; ++w) {
            const auto in_vram = read16v(d + w * 2);
            const auto at_source = machine.read16(s + w * 2);
            if (in_vram != at_source) {
                if (bad < 8) {
                    std::printf("  word %u: VRAM %04X  source %04X\n", w, in_vram, at_source);
                }
                ++bad;
            }
        }
        std::printf("  %zu of %u words differ from the source\n", bad, n);
    }
    {
        const auto& lg = vdp.d_port_log();
        const auto pos = vdp.d_port_log_pos();
        std::printf("last %zu data-port writes into region D (total %llu):\n", lg.size(),
                    (unsigned long long)pos);
        const std::size_t start = pos > lg.size() ? pos - lg.size() : 0;
        for (std::size_t k = start; k < pos; ++k) {
            const auto& e = lg[k % lg.size()];
            std::printf("  inc%02X %04X=%04X", (e.first >> 16) & 0xFF, e.first & 0xFFFF, e.second);
            if (((k - start) % 5) == 4) { std::printf("\n"); }
        }
        std::printf("\n");
    }
    {
        const auto& cl = vdp.ctrl_log();
        const auto pos = vdp.ctrl_log_pos();
        std::printf("needle command written %llu times, last from PC %06X\n",
                (unsigned long long)machine.needle_cmd_count(), machine.needle_cmd_pc());
    std::printf("last commands into the needle region (first word -> address):\n");
        const std::size_t start = pos > cl.size() ? pos - cl.size() : 0;
        for (std::size_t k = start; k < pos; ++k) {
            const auto& e = cl[k % cl.size()];
            std::printf("  %04X(@%04X)", e.first, e.second);
            if (((k - start) % 6) == 5) { std::printf("\n"); }
        }
        std::printf("\n");
    }
    std::printf("DMA commands skipped because the enable bit was clear: %llu\n",
                (unsigned long long)vdp.dma_skipped_disabled());
    {
        const auto& dl = vdp.d_dma_log();
        const auto pos = vdp.d_dma_log_pos();
        std::printf("last region-D DMAs (dest <- source x len):\n");
        const std::size_t st = pos > dl.size() ? pos - dl.size() : 0;
        for (std::size_t k = st; k < pos; ++k) {
            const auto& e = dl[k % dl.size()];
            std::printf("  %04X <- %06X x%-5u", e.dest, e.src, e.len);
            if (((k - st) % 3) == 2) { std::printf("\n"); }
        }
        std::printf("\n");
    }
    std::printf("sources of DMA into region D:");
    for (std::size_t i = 0; i < 256; ++i) {
        if (vdp.dma_d_sources()[i] != 0) {
            std::printf(" %02X0000:%llu", static_cast<unsigned>(i),
                        static_cast<unsigned long long>(vdp.dma_d_sources()[i]));
        }
    }
    std::printf("\n");
    std::printf("DMA words landing per 4KB region:\n ");
    for (std::size_t i = 0; i < vdp.dma_dest_buckets().size(); ++i) {
        std::printf(" %X:%llu", static_cast<unsigned>(i),
                    static_cast<unsigned long long>(vdp.dma_dest_buckets()[i]));
    }
    std::printf("\n");
    std::printf("VRAM writes per 4KB region:\n ");
    for (std::size_t i = 0; i < vdp.vram_write_buckets().size(); ++i) {
        std::printf(" %X:%llu", static_cast<unsigned>(i),
                    static_cast<unsigned long long>(vdp.vram_write_buckets()[i]));
    }
    std::printf("\n");

    // The top-left corner of each plane, which is what the eye checks first.
    for (int plane = 0; plane < 2; ++plane) {
        const auto base = plane == 0 ? plane_a : plane_b;
        std::printf("\n%s name table at %04X, first 3 rows:\n", plane == 0 ? "Plane A" : "Plane B", base);
        for (int row = 0; row < 3; ++row) {
            std::printf("  ");
            for (int col = 0; col < 16; ++col) {
                std::printf("%04X ", read16v(base + ((row * pw + col) * 2)));
            }
            std::printf("\n");
        }
    }

    // How many distinct tiles each plane references, and how many of those have
    // any non-zero pattern data. A plane pointing at empty tiles draws nothing;
    // a plane referencing hundreds of scattered indices is reading noise.
    for (int plane = 0; plane < 2; ++plane) {
        const auto base = plane == 0 ? plane_a : plane_b;
        std::vector<std::uint16_t> tiles;
        for (int row = 0; row < 28; ++row) {
            for (int col = 0; col < (h40 ? 40 : 32); ++col) {
                tiles.push_back(read16v(base + ((row * pw + col) * 2)) & 0x07FF);
            }
        }
        std::sort(tiles.begin(), tiles.end());
        const auto distinct = static_cast<std::size_t>(
            std::unique(tiles.begin(), tiles.end()) - tiles.begin());
        std::size_t blank = 0;
        for (std::size_t i = 0; i < distinct; ++i) {
            bool any = false;
            for (int b = 0; b < 32; ++b) {
                if (vram[(tiles[i] * 32 + b) & 0xFFFF] != 0) { any = true; break; }
            }
            if (!any) ++blank;
        }
        std::printf("\n%s: %zu distinct tiles on screen, %zu of them blank, highest index %u\n",
                    plane == 0 ? "Plane A" : "Plane B", distinct, blank,
                    distinct ? tiles[distinct - 1] : 0);
        // WHICH tiles are blank matters more than how many. A contiguous run
        // says a transfer is short or misaddressed; a scatter says the
        // renderer simply left them empty.
        if (blank != 0) {
            std::printf("  blank runs:");
            std::size_t printed = 0;
            for (std::size_t i = 0; i < distinct && printed < 24;) {
                bool empty = true;
                for (int b = 0; b < 32; ++b) {
                    if (vram[(tiles[i] * 32 + b) & 0xFFFF] != 0) { empty = false; break; }
                }
                if (!empty) { ++i; continue; }
                const auto first = tiles[i];
                auto last = first;
                std::size_t j = i + 1;
                for (; j < distinct; ++j) {
                    bool e2 = true;
                    for (int b = 0; b < 32; ++b) {
                        if (vram[(tiles[j] * 32 + b) & 0xFFFF] != 0) { e2 = false; break; }
                    }
                    if (!e2) break;
                    last = tiles[j];
                }
                std::printf(" %u-%u", first, last);
                ++printed;
                i = j;
            }
            std::printf("\n");
        }
    }

    // The sprite attribute table, walked through the link field exactly as the
    // VDP does. A thin diagonal drawn as a chain of small sprites shows up here
    // as its positions - which separates "the game placed them with gaps" from
    // "the tiles themselves are broken".
    {
        std::printf("\nSprite list (link order): idx  y    x    size   tile  pal pri\n");
        std::uint32_t entry = 0;
        for (int n = 0; n < 80; ++n) {
            const auto base = sprites + entry * 8;
            const auto y = static_cast<int>(read16v(base) & 0x03FF) - 128;
            const auto size = vram[(base + 2) & 0xFFFF];
            const auto link = static_cast<std::uint32_t>(vram[(base + 3) & 0xFFFF] & 0x7F);
            const auto attr = read16v(base + 4);
            const auto x = static_cast<int>(read16v(base + 6) & 0x01FF) - 128;
            std::printf("  %3d: %4d %4d  %ux%u  %04X   %u   %u\n", entry, y, x,
                        ((size >> 2) & 3) + 1, (size & 3) + 1, attr & 0x07FF,
                        (attr >> 13) & 3, (attr >> 15) & 1);
            if (link == 0) { break; }
            entry = link;
        }
    }

    // The sprite table is built in work RAM and DMAd into VRAM. Comparing the
    // two IN THE SAME RUN says whether the transfer is faithful - doing it
    // across two runs proves nothing, because the game will be in a different
    // state. MERCURY_SPRITE_SRC=<hex work-RAM address of the source list>.
    if (const char* src = std::getenv("MERCURY_SPRITE_SRC"); src != nullptr) {
        const auto source = static_cast<std::uint32_t>(std::strtol(src, nullptr, 16));
        std::printf("\nSprite table: VRAM %04X vs source %06X\n", sprites, source);
        std::size_t bad = 0;
        for (std::uint32_t e = 0; e < 64; ++e) {
            for (int w = 0; w < 4; ++w) {
                const auto in_vram = read16v(sprites + e * 8 + w * 2);
                const auto in_ram = machine.read16(source + e * 8 + w * 2);
                if (in_vram != in_ram) {
                    if (bad < 12) {
                        std::printf("  entry %2u word %d: VRAM %04X  source %04X\n", e, w, in_vram,
                                    in_ram);
                    }
                    ++bad;
                }
            }
        }
        std::printf("  %zu of 256 words differ\n", bad);
    }

    // Which sprites use a given colour index? MERCURY_FIND_INDEX=<0-15>.
    // Hunting a coloured artefact by eye across 80 sprites is hopeless; asking
    // which ones can even produce that colour takes one pass.
    if (const char* want = std::getenv("MERCURY_FIND_INDEX"); want != nullptr) {
        const auto target = static_cast<std::uint8_t>(std::atoi(want) & 0x0F);
        std::printf("\nSprites containing colour index %X:\n", target);
        std::uint32_t entry = 0;
        for (int n = 0; n < 80; ++n) {
            const auto base = sprites + entry * 8;
            const auto size = vram[(base + 2) & 0xFFFF];
            const auto link = static_cast<std::uint32_t>(vram[(base + 3) & 0xFFFF] & 0x7F);
            const auto attr = read16v(base + 4);
            const std::uint32_t cells = (((size >> 2) & 3) + 1) * ((size & 3) + 1);
            const std::uint32_t tile0 = attr & 0x07FF;
            std::uint32_t hits = 0;
            for (std::uint32_t b = 0; b < cells * 32; ++b) {
                const auto byte = vram[(tile0 * 32 + b) & 0xFFFF];
                if ((byte >> 4) == target || (byte & 0x0F) == target) { ++hits; }
            }
            if (hits != 0) {
                std::printf("  sprite %3u  y=%4d x=%4d  pal=%u  tile=%04X  %u bytes hit\n", entry,
                            static_cast<int>(read16v(base) & 0x3FF) - 128,
                            static_cast<int>(read16v(base + 6) & 0x1FF) - 128,
                            (attr >> 13) & 3, tile0, hits);
            }
            if (link == 0) { break; }
            entry = link;
        }
    }

    // Render one sprite from VRAM as ASCII, assembled the way the VDP does
    // (tiles run DOWN each column, then right). MERCURY_SPRITE=<link index>.
    // A thin diagonal that looks dashed on screen is either dashed in these
    // tiles - so whoever generates them is at fault - or it is not, and the
    // compositor is dropping it.
    if (const char* want = std::getenv("MERCURY_SPRITE"); want != nullptr) {
        const std::uint32_t target = static_cast<std::uint32_t>(std::atoi(want));
        const auto base = sprites + target * 8;
        const auto size = vram[(base + 2) & 0xFFFF];
        const auto attr = read16v(base + 4);
        const std::uint32_t cw = ((size >> 2) & 3) + 1;
        const std::uint32_t chh = (size & 3) + 1;
        const std::uint32_t tile0 = attr & 0x07FF;
        std::printf("\nSprite %u: %ux%u cells, base tile %04X\n", target, cw, chh, tile0);
        for (std::uint32_t py = 0; py < chh * 8; ++py) {
            std::printf("  ");
            for (std::uint32_t px = 0; px < cw * 8; ++px) {
                const std::uint32_t ti = tile0 + (px / 8) * chh + (py / 8);
                const auto byte = vram[(ti * 32 + (py % 8) * 4 + (px % 8) / 2) & 0xFFFF];
                const auto idx = static_cast<std::uint8_t>((px & 1) ? (byte & 0x0F) : (byte >> 4));
                std::printf("%c", idx == 0 ? '.' : "0123456789ABCDEF"[idx]);
            }
            std::printf("\n");
        }
    }

    // One row of Plane A drawn as ASCII, straight out of VRAM through the real
    // name table. This separates two things a screenshot cannot: whether a tile
    // holds the wrong PICTURE (the renderer is at fault) or the right picture in
    // the wrong PLACE (the name table or the compositor is). The row is chosen
    // to land on the ranking screen's title text, which is unambiguous - unlike
    // polygons, a letter is either an I or it isn't.
    {
        const int text_rows[] = {30, 31, 34, 35};
        for (const int prow : text_rows) {
            std::printf("\nPlane A row %d, cells 0-15, name table entries:\n ", prow);
            for (int col = 0; col < 16; ++col) {
                std::printf(" %04X", read16v(plane_a + ((prow * pw + col) * 2)));
            }
            std::printf("\n");
            for (int line = 0; line < 8; ++line) {
                std::printf("  ");
                for (int col = 0; col < 16; ++col) {
                    const std::uint16_t entry = read16v(plane_a + ((prow * pw + col) * 2));
                    const std::uint32_t tile = entry & 0x07FF;
                    const std::uint32_t at = tile * 32 + static_cast<std::uint32_t>(line) * 4;
                    for (int b = 0; b < 4; ++b) {
                        const std::uint8_t byte = vram[(at + b) & 0xFFFF];
                        for (int nib = 0; nib < 2; ++nib) {
                            const std::uint8_t index =
                                nib == 0 ? static_cast<std::uint8_t>(byte >> 4)
                                         : static_cast<std::uint8_t>(byte & 0x0F);
                            std::printf("%c", index == 0 ? '.' : "0123456789ABCDEF"[index]);
                        }
                    }
                }
                std::printf("\n");
            }
        }
    }

    // The DSP's own output, drawn straight out of its DRAM without going
    // through the transfer or the VDP at all. If the tearing is visible here it
    // belongs to the renderer; if this is clean and the screen is not, it
    // belongs to the transfer. Every other test leaves that ambiguous.
    {
        const std::uint32_t kTiles = 768;
        const std::uint32_t kCols = 32;
        const std::uint32_t w = kCols * 8;
        const std::uint32_t h = (kTiles / kCols) * 8;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 3, 0);
        const auto& svp = machine.svp();
        // The name table gives each screen cell its own tile and steps by TWO
        // (row 0 is 0001 0003 0005 ...), so a naive consecutive layout
        // interleaves the buffer with tiles nothing displays.
        for (std::uint32_t cell = 0; cell < kTiles; ++cell) {
            const std::uint32_t col = cell % kCols;
            const std::uint32_t scrrow = cell / kCols;
            const std::uint32_t tile = 2 * col + 64 * scrrow;
            const std::uint32_t ox = col * 8;
            const std::uint32_t oy = scrrow * 8;
            for (std::uint32_t row = 0; row < 8; ++row) {
                for (std::uint32_t half = 0; half < 2; ++half) {
                    // DRAM word 1 is the first word of the first tile: the
                    // transfer starts at 0x300002.
                    const std::uint16_t word = svp.dram_word(1 + tile * 16 + row * 2 + half);
                    for (std::uint32_t p = 0; p < 4; ++p) {
                        const std::uint8_t index =
                            static_cast<std::uint8_t>((word >> (12 - p * 4)) & 0x0F);
                        const std::uint16_t colour = cram[index];
                        const auto expand = [](std::uint16_t v) {
                            return static_cast<std::uint8_t>((v & 7) * 36);
                        };
                        const std::size_t at =
                            (static_cast<std::size_t>(oy + row) * w + ox + half * 4 + p) * 3;
                        pixels[at + 0] = expand(static_cast<std::uint16_t>(colour >> 1));
                        pixels[at + 1] = expand(static_cast<std::uint16_t>(colour >> 5));
                        pixels[at + 2] = expand(static_cast<std::uint16_t>(colour >> 9));
                    }
                }
            }
        }
        if (FILE* f = std::fopen("svp_dram.ppm", "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", w, h);
            std::fwrite(pixels.data(), 1, pixels.size(), f);
            std::fclose(f);
            std::printf("\nwrote svp_dram.ppm (%ux%u) - the DSP's buffer, pre-transfer\n", w, h);
        }
    }

    // The pixels themselves. A tile that renders as a checkerboard says the
    // pattern is nibble-level, which only one write path can produce; a tile
    // that renders as 4-pixel blocks says it is word-level. Guessing between
    // those two from a screenshot is how time gets wasted.
    for (const std::uint16_t tile : {1, 3, 5, 129, 131}) {
        std::printf("tile %4u:", tile);
        for (int w = 0; w < 16; ++w) {
            std::printf(" %04X", read16v(static_cast<std::uint32_t>(tile) * 32 + w * 2));
        }
        std::printf("\n");
    }

    // Raster effects are invisible in a whole-frame view. Collapsing the
    // per-line record into runs shows where a game changed scroll or switched
    // the display off, which is what turns "there is a black bar" into a line
    // number that can be checked against the game's own code.
    std::printf("\nPer-line state, as runs\n");
    const auto& lines = vdp.line_records();
    std::uint32_t run_start = 0;
    for (std::uint32_t y = 1; y <= vdp.screen_height(); ++y) {
        const bool last = (y == vdp.screen_height());
        const auto& a = lines[run_start];
        const bool changed = !last && (lines[y].display_on != a.display_on ||
                                       lines[y].hscroll_a != a.hscroll_a ||
                                       lines[y].vscroll_a != a.vscroll_a);
        if (last || changed) {
            std::printf("  lines %3u-%3u  display=%s  hscrollA=%4u vscrollA=%4u reg1=%02X\n",
                        run_start, y - 1, a.display_on ? "ON " : "off",
                        a.hscroll_a, a.vscroll_a, a.reg1);
            run_start = y;
        }
    }

    std::printf("\nFirst DMA transfers (destination <- source, length in words)\n");
    const auto& log = vdp.dma_log();
    for (std::size_t i = 0; i < log.size() && i < 20; ++i) {
        const char* space = (log[i].code & 0x0F) == 0x01 ? "VRAM"
                          : (log[i].code & 0x0F) == 0x03 ? "CRAM"
                          : (log[i].code & 0x0F) == 0x05 ? "VSRAM" : "?";
        std::printf("  %-5s %04X <- %06X  x%u\n", space, log[i].destination,
                    log[i].source, log[i].length);
    }
    return 0;
}

// Run to a PC, then print the instruction stream from there. For an interrupt
// handler that runs but does not do its job, the branch it takes is the answer
// and a hot-PC tally cannot show ordering.
int run_follow(const std::filesystem::path& rom, std::uint32_t target, int steps, int frames) {
    mercury::genesis::Genesis machine;
    const auto load = load_image(machine, rom);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }
    const std::uint64_t budget = static_cast<std::uint64_t>(frames) * 200000ull;
    bool armed = false;
    for (std::uint64_t i = 0; i < budget; ++i) {
        if (!armed && machine.m68k().registers().pc == target) {
            armed = true;
        }
        if (armed) {
            const auto& r = machine.m68k().registers();
            const auto pc = r.pc;
            const auto result = machine.step();
            std::printf("%06X  op=%04X  d0=%08X d1=%08X a0=%08X a1=%08X sr=%04X\n",
                        pc, result.opcode, r.d[0], r.d[1], r.a[0], r.a[1], r.sr);
            if (--steps <= 0) return 0;
        } else {
            (void)machine.step();
        }
    }
    std::printf("PC=%06X never reached\n", target);
    return 1;
}

// Everything that decides what one frame shows, written next to the bitmap of
// that frame. A stray or duplicated graphic is in exactly one of three places -
// the sprite list, a plane nametable, or the scroll values - and reading all
// three at once says which, instead of inferring it from pixels.
void dump_screen_state(const mercury::genesis::Vdp& vdp, const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    const auto& vram = vdp.vram();
    const auto read16 = [&](std::uint32_t a) {
        return static_cast<std::uint32_t>((vram[a & 0xFFFF] << 8) | vram[(a + 1) & 0xFFFF]);
    };
    char buf[256];
    const auto line = [&](const char* fmt, auto... args) {
        std::snprintf(buf, sizeof(buf), fmt, args...);
        out << buf << "\n";
    };

    line("screen %ux%u  H%s  display=%s", vdp.screen_width(), vdp.screen_height(),
         vdp.wide_mode() ? "40" : "32", vdp.display_enabled() ? "on" : "OFF");
    out << "registers:";
    for (std::size_t i = 0; i < 24; ++i) {
        std::snprintf(buf, sizeof(buf), " %02zu=%02X", i, vdp.reg(i));
        out << buf;
    }
    out << "\n";

    const int m3 = vdp.reg(0x0B);
    static const char* kHMode[4] = {"whole screen", "prohibited(first cell row)", "per cell row",
                                    "per line"};
    line("mode3=%02X  hscroll=%s  vscroll=%s", m3, kHMode[m3 & 3],
         (m3 & 4) ? "per 2-cell column" : "whole screen");
    line("planes: A=%04X B=%04X window=%04X  size %ux%u cells", vdp.plane_a_base_addr(),
         vdp.plane_b_base_addr(), (vdp.reg(0x03) & 0x3E) << 10, vdp.plane_a_row_cells(),
         vdp.plane_h_px() / 8);
    const int r17 = vdp.reg(0x11);
    const int r18 = vdp.reg(0x12);
    line("window: H from_%s cell %d   V from_%s line %d",
         (r17 & 0x80) ? "right" : "left", (r17 & 0x1F) * 2,
         (r18 & 0x80) ? "bottom" : "top", (r18 & 0x1F) * 8);

    out << "vsram:";
    for (std::size_t i = 0; i < 40; ++i) {
        std::snprintf(buf, sizeof(buf), " %s%03X", (i % 2) ? "" : "|", vdp.vsram_entry(i));
        out << buf;
    }
    out << "   (pairs are A,B per 2-cell column)\n";

    // The horizontal scroll table, collapsed to the lines where it changes: a
    // raster split shows up as a change partway down the screen, and a scroll
    // applied a few lines late is exactly what duplicates a band of graphics.
    const std::uint32_t hbase = static_cast<std::uint32_t>(vdp.reg(0x0D) & 0x3F) << 10;
    out << "hscroll changes (line: A,B):";
    int prev_a = -1;
    int prev_b = -1;
    for (std::uint32_t l = 0; l < vdp.screen_height(); ++l) {
        std::uint32_t off = 0;
        switch (m3 & 3) {
            case 0: off = 0; break;
            case 1: off = (l & 7) * 4; break;
            case 2: off = (l & ~7u) * 4; break;
            default: off = l * 4; break;
        }
        const int a = static_cast<int>(read16(hbase + off) & 0x3FF);
        const int b = static_cast<int>(read16(hbase + off + 2) & 0x3FF);
        if (a != prev_a || b != prev_b) {
            std::snprintf(buf, sizeof(buf), " %u:%03X,%03X", l, a, b);
            out << buf;
            prev_a = a;
            prev_b = b;
        }
    }
    out << "\n";

    // The sprite list walked the way the VDP walks it - through the link field.
    // A sprite the game meant to hide sits at an off-screen Y or X here; one
    // that is visible when it should not be is the bug.
    const std::uint32_t sat = static_cast<std::uint32_t>(vdp.reg(0x05) & 0x7F) << 9;
    const std::uint32_t capacity = vdp.wide_mode() ? 80u : 64u;
    line("sprite table @%04X, capacity %u, walked by link:", sat, capacity);
    line("  slot   x    y   size  tile  pal pri flip link");
    std::uint32_t idx = 0;
    for (std::uint32_t visited = 0; visited < capacity; ++visited) {
        const auto base = sat + idx * 8;
        const int y = static_cast<int>(read16(base) & 0x3FF) - 128;
        const auto size = vram[(base + 2) & 0xFFFF];
        const auto link = static_cast<std::uint32_t>(vram[(base + 3) & 0xFFFF] & 0x7F);
        const auto attr = read16(base + 4);
        const int x = static_cast<int>(read16(base + 6) & 0x1FF) - 128;
        line("  %4u %4d %4d  %ux%u  %04X   %u   %u   %c%c  %3u", idx, x, y,
             ((size >> 2) & 3) + 1, (size & 3) + 1, static_cast<unsigned>(attr & 0x7FF),
             static_cast<unsigned>((attr >> 13) & 3), (attr & 0x8000) ? 1u : 0u,
             (attr & 0x0800) ? 'H' : '-', (attr & 0x1000) ? 'V' : '-', link);
        if (link == 0 || link >= capacity) break;
        idx = link;
    }

    if (vdp.raster_log_count() != 0) {
        static const char* kKind[5] = {"reg10 <-", "H-int raised, reload", "H-int LOST, reload",
                                       "V-int TAKEN, reg10 =", "reg18 (window V) <-"};
        const auto& lg = vdp.raster_log();
        const auto pos = vdp.raster_log_pos();
        line("\nraster events: %llu total, last %zu:", (unsigned long long)vdp.raster_log_count(),
             lg.size());
        for (std::size_t i = 0; i < lg.size(); ++i) {
            const auto& e = lg[(pos + i) % lg.size()];
            if (e[0] == 0 && e[1] == 0 && e[3] == 0) continue;
            if (e[2] == 5) {
                line("  frame %5u line %3u  reg%-2u <- %02X", e[0], e[1], e[3] >> 8, e[3] & 0xFF);
            } else {
                line("  frame %5u line %3u  %s %u", e[0], e[1], kKind[e[2] < 5 ? e[2] : 0], e[3]);
            }
        }
    }

    // MERCURY_VSRAM_LOG=1 fills this. A plane whose scroll changes partway down
    // the screen puts its content on the wrong lines if the write lands a line
    // or two off, and the SCANLINE column is the whole measurement: the value
    // alone looks correct in both cases.
    if (vdp.vsram_log_count() != 0) {
        const auto& lg = vdp.vsram_log();
        const auto pos = vdp.vsram_log_pos();
        line("\nVSRAM writes: %llu total, last %zu:", (unsigned long long)vdp.vsram_log_count(),
             lg.size());
        for (std::size_t i = 0; i < lg.size(); ++i) {
            const auto& e = lg[(pos + i) % lg.size()];
            if (e[0] == 0 && e[1] == 0 && e[3] == 0) continue;
            line("  frame %5u line %3u  plane %c = %4u  %s", e[0], e[1], e[2] ? 'B' : 'A', e[3],
                 e[4] ? "DMA" : "port");
        }
    }

    // MERCURY_VRAM_WATCH=<addr>[,<size>] fills this: every write into that
    // window with the frame AND scanline it landed on. Art that is complete at
    // the end of the frame but was written after the raster passed its rows
    // renders half-drawn, and only the scanline separates that from art that
    // was never written at all.
    if (vdp.vram_watch_count() != 0) {
        const auto& lg = vdp.vram_watch_log();
        const auto pos = vdp.vram_watch_pos();
        line("\nVRAM watch: %llu writes, last %zu:", (unsigned long long)vdp.vram_watch_count(),
             lg.size());
        for (std::size_t i = 0; i < lg.size(); ++i) {
            const auto& e = lg[(pos + i) % lg.size()];
            if (e[0] == 0 && e[2] == 0 && e[3] == 0) continue;
            line("  frame %5u line %3u  %04X = %04X  %s", e[0], e[1], e[2], e[3],
                 e[4] ? "DMA" : "port");
        }
    }

    // The last transfers into VRAM. Art that is missing rows was either never
    // sent or sent short, and the length here says which.
    {
        const auto& dl = vdp.dma_log();
        line("\nlast DMA transfers (%zu logged):", dl.size());
        const std::size_t first = dl.size() > 24 ? dl.size() - 24 : 0;
        for (std::size_t i = first; i < dl.size(); ++i) {
            line("  %c dest=%05X src=%06X len=%u words  code=%02X", dl[i].kind, dl[i].destination,
                 dl[i].source, dl[i].length, dl[i].code);
        }
    }

    out << "\ncram (4 palettes of 16):\n";
    for (std::size_t p = 0; p < 4; ++p) {
        std::snprintf(buf, sizeof(buf), "  pal%zu:", p);
        out << buf;
        for (std::size_t i = 0; i < 16; ++i) {
            std::snprintf(buf, sizeof(buf), " %03X", vdp.cram()[p * 16 + i] & 0xEEE);
            out << buf;
        }
        out << "\n";
    }

    // Both nametables for the visible rows, as the full 16-bit entry: the
    // priority bit and the palette matter as much as the tile, because a
    // high-priority cell on the other plane covering low-priority art looks
    // exactly like art that was never written.
    const std::uint32_t width_cells = vdp.plane_a_row_cells();
    const std::uint32_t height_cells = vdp.plane_h_px() / 8;
    const std::uint32_t show_cells = std::min<std::uint32_t>(width_cells, 42);
    // The window is included: it REPLACES plane A wherever it covers, so a blank
    // window nametable over a plane that holds the picture blanks the picture,
    // and reading plane A alone makes that look like the picture is fine.
    for (int pass = 0; pass < 3; ++pass) {
        const std::uint32_t nbase = pass == 0   ? vdp.plane_a_base_addr()
                                    : pass == 1 ? vdp.plane_b_base_addr()
                                                : ((vdp.reg(0x03) & (vdp.wide_mode() ? 0x3E : 0x3F)) << 10);
        // The window never scrolls and always uses a full-width row: 64 cells in
        // H40, 32 in H32, whatever the plane size register says.
        const std::uint32_t row_cells = pass == 2 ? (vdp.wide_mode() ? 64u : 32u) : width_cells;
        const char* name = pass == 0 ? "A" : (pass == 1 ? "B" : "window");
        line("\nplane %s nametable @%04X (pri/pal/flip/tile, first %u of %u columns):", name, nbase,
             std::min(show_cells, row_cells), row_cells);
        for (std::uint32_t r = 0; r < height_cells && r < 32; ++r) {
            std::snprintf(buf, sizeof(buf), "  r%02u:", r);
            out << buf;
            for (std::uint32_t c = 0; c < show_cells && c < row_cells; ++c) {
                std::snprintf(buf, sizeof(buf), " %04X", read16(nbase + (r * row_cells + c) * 2));
                out << buf;
            }
            out << "\n";
        }
    }

    // MERCURY_TILE_ART=<hex tile>,<count>: the raw 4bpp pixels of a run of
    // tiles, one text row per pixel row. Whether a glyph is missing its top
    // rows in VRAM or only on screen is the whole question when text renders
    // half-drawn, and this answers it without a second render path.
    // MERCURY_TILE_STRIP=<hex first tile>,<cols>,<rows>: the same tiles laid out
    // as the picture they form, so half-drawn text is legible as text. The
    // menus store their pop-ups column-major - tile = first + col*rows + row -
    // which is unreadable one tile at a time.
    if (const char* e = std::getenv("MERCURY_TILE_STRIP")) {
        const auto first = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
        std::uint32_t cols = 1;
        std::uint32_t rows = 1;
        if (const char* c1 = std::strchr(e, ',')) {
            cols = static_cast<std::uint32_t>(std::strtoul(c1 + 1, nullptr, 10));
            if (const char* c2 = std::strchr(c1 + 1, ',')) {
                rows = static_cast<std::uint32_t>(std::strtoul(c2 + 1, nullptr, 10));
            }
        }
        line("\ntile strip from %03X, %u cols x %u rows (column-major):", first, cols, rows);
        for (std::uint32_t py = 0; py < rows * 8; ++py) {
            std::snprintf(buf, sizeof(buf), "  %3u|", py);
            out << buf;
            for (std::uint32_t px = 0; px < cols * 8; ++px) {
                const auto t = first + (px / 8) * rows + (py / 8);
                const auto byte = vram[(t * 32 + (py % 8) * 4 + (px % 8) / 2) & 0xFFFF];
                const auto idx = (px & 1) ? (byte & 0x0F) : (byte >> 4);
                out << (idx == 0 ? '.' : static_cast<char>(idx < 10 ? '0' + idx : 'a' + idx - 10));
            }
            out << "|\n";
        }
    }

    if (const char* e = std::getenv("MERCURY_TILE_ART")) {
        const auto first = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
        const char* comma = std::strchr(e, ',');
        const auto count = comma ? static_cast<std::uint32_t>(std::strtoul(comma + 1, nullptr, 10)) : 1u;
        line("\ntile art from %03X, %u tiles (0 = transparent):", first, count);
        for (std::uint32_t t = first; t < first + count; ++t) {
            line("  tile %03X:", t);
            for (std::uint32_t row = 0; row < 8; ++row) {
                out << "    ";
                for (std::uint32_t col = 0; col < 8; ++col) {
                    const auto byte = vram[(t * 32 + row * 4 + col / 2) & 0xFFFF];
                    const auto idx = (col & 1) ? (byte & 0x0F) : (byte >> 4);
                    out << (idx == 0 ? '.' : static_cast<char>(idx < 10 ? '0' + idx : 'a' + idx - 10));
                }
                out << "\n";
            }
        }
    }
    out.flush();
}

// Proves a save state is COMPLETE, not merely that it writes a file.
//
// Run to a frame, save, run on and fingerprint the machine; then load the state
// back and run the same distance again. If anything the state failed to carry
// still influences the future, the two fingerprints differ. A state that is
// missing a field usually still loads and still looks right for a second or
// two, which is exactly why "it loaded fine" is not evidence.
int run_state_check(const std::filesystem::path& rom, int save_at, int distance,
                    bool auto_input) {
    const auto fingerprint = [](mercury::genesis::Genesis& machine) {
        std::uint64_t hash = 1469598103934665603ull;
        const auto mix = [&hash](const std::uint8_t* data, std::size_t size) {
            for (std::size_t i = 0; i < size; ++i) {
                hash ^= data[i];
                hash *= 1099511628211ull;
            }
        };
        const auto& vdp = machine.vdp();
        mix(vdp.frame_pixels().data(), vdp.frame_pixels().size());
        mix(vdp.vram().data(), vdp.vram().size());
        mix(machine.work_ram().data(), machine.work_ram().size());
        const auto& regs = machine.m68k().registers();
        mix(reinterpret_cast<const std::uint8_t*>(&regs), sizeof(regs));
        // Everything the state itself holds - the 32X's and the Sega CD's
        // memories and processors included - as it stands after the run. A
        // field the state leaves out shows up here as a difference in the
        // fields it does carry, which is the whole point of running on.
        const auto state = machine.save_state_blob();
        mix(state.data(), state.size());
        return hash;
    };
    const auto drive = [&](mercury::genesis::Genesis& machine, int frame) {
        if (!auto_input) return;
        using mercury::genesis::Button;
        const int phase = frame % 120;
        machine.port1().set_button(Button::start, phase >= 20 && phase < 28);
        machine.port1().set_button(Button::c, phase >= 60 && phase < 68);
        machine.port1().set_button(Button::a, phase >= 90 && phase < 98);
    };

    // On the heap, all three: a Genesis carries the SVP's DRAM inline, and
    // three of them on a 1MB stack is a stack overflow before main's first
    // printf.
    auto machine_owner = std::make_unique<mercury::genesis::Genesis>();
    auto& machine = *machine_owner;
    const auto load = load_image(machine, rom);
    if (!load.loaded) {
        std::printf("load failed: %s\n", load.message.c_str());
        return 1;
    }
    for (int frame = 0; frame < save_at; ++frame) {
        drive(machine, frame);
        machine.run_frame();
    }
    const auto blob = machine.save_state_blob();
    std::printf("saved at frame %d: %zu bytes\n", save_at, blob.size());

    for (int frame = save_at; frame < save_at + distance; ++frame) {
        drive(machine, frame);
        machine.run_frame();
    }
    const auto expected = fingerprint(machine);

    std::string error;
    if (!machine.load_state_blob(blob, error)) {
        std::printf("FAIL: load rejected the state: %s\n", error.c_str());
        return 1;
    }
    for (int frame = save_at; frame < save_at + distance; ++frame) {
        drive(machine, frame);
        machine.run_frame();
    }
    const auto got = fingerprint(machine);

    if (got != expected) {
        std::printf("FAIL: diverged after reload (%016llX vs %016llX) - the state is missing "
                    "something that still affects the machine\n",
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(expected));
        return 1;
    }
    std::printf("PASS: identical %d frames after reload (%016llX)\n", distance,
                static_cast<unsigned long long>(got));

    // THE NETPLAY CASE: the state loaded into a DIFFERENT machine - the same
    // game loaded fresh and run to somewhere else first - must then play out
    // exactly as the original did. Reloading into the machine that took the
    // state cannot catch a field the state misses but the two runs happen to
    // share; a second machine can.
    {
        auto joiner_owner = std::make_unique<mercury::genesis::Genesis>();
        auto& joiner = *joiner_owner;
        if (!load_image(joiner, rom).loaded) {
            std::printf("FAIL: second machine would not load the image\n");
            return 1;
        }
        const int elsewhere = save_at / 3 + 37;
        for (int frame = 0; frame < elsewhere; ++frame) {
            drive(joiner, frame + 11);
            joiner.run_frame();
        }
        if (!joiner.load_state_blob(blob, error)) {
            std::printf("FAIL: second machine rejected the state: %s\n", error.c_str());
            return 1;
        }
        for (int frame = save_at; frame < save_at + distance; ++frame) {
            drive(joiner, frame);
            joiner.run_frame();
        }
        const auto joined = fingerprint(joiner);
        if (joined != expected) {
            std::printf("FAIL: a second machine diverged after loading the state (%016llX vs "
                        "%016llX) - netplay would desynchronise\n",
                        static_cast<unsigned long long>(joined),
                        static_cast<unsigned long long>(expected));
            return 1;
        }
        std::printf("PASS: a second machine loaded it and matched %d frames on\n", distance);
    }

    // AN OLDER FILE. Version 4 is exactly version 6 without the media header
    // (one byte and three 64-bit ids, after the cartridge id at offset 14),
    // without the blocks it describes, and without version 6's raster samples
    // at the very end (a bool and two 16-bit words). A plain cartridge's
    // version 4 state must still load, so nobody's saves stop working with the
    // new build; one for a machine with a 32X, a Sega CD or an SVP must be
    // refused, because it never held that hardware.
    {
        std::vector<std::uint8_t> old = blob;
        old[4] = 4;
        old[5] = old[6] = old[7] = 0;
        old.resize(old.size() - (1 + 2 * 2));
        old.erase(old.begin() + 14, old.begin() + 14 + 1 + 3 * 8);
        auto old_owner = std::make_unique<mercury::genesis::Genesis>();
        auto& reader = *old_owner;
        const bool plain = machine.state_media() == 0;
        const bool loaded = load_image(reader, rom).loaded && reader.load_state_blob(old, error);
        if (loaded != plain) {
            std::printf("FAIL: a version 4 state was %s\n", plain ? "refused" : "accepted");
            return 1;
        }
        std::printf("PASS: a version 4 state is %s\n",
                    plain ? "still accepted" : ("refused (" + error + ")").c_str());
    }

    // A state must also be refused by a different cartridge rather than loaded
    // into it, so check the guard actually bites.
    auto other_owner = std::make_unique<mercury::genesis::Genesis>();
    auto& other = *other_owner;
    std::vector<std::uint8_t> corrupt = blob;
    corrupt[8] ^= 0xFF;  // the stored checksum
    if (load_image(other, rom).loaded && other.load_state_blob(corrupt, error)) {
        std::printf("FAIL: a state with the wrong cartridge id was accepted\n");
        return 1;
    }
    std::printf("PASS: mismatched state refused (%s)\n", error.c_str());
    return 0;
}

int main(int argc, char** argv) {
    std::filesystem::path scratch = std::filesystem::current_path();
    std::filesystem::path rom;
    int frames = 60;
    int dump_from = -1;
    std::vector<int> dump_at;
    bool selftest = false;
    bool summary = false;
    bool auto_input = false;
    bool state_check = false;
    int state_at = 600;
    int state_distance = 180;
    bool trace = false;
    int trace_from = -1;
    long break_pc = -1;
    long watch_addr = -1;
    int break_skip = 0;
    bool planes = false;
    long follow_pc = -1;
    int follow_steps = 40;
    std::filesystem::path shot;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--selftest") {
            selftest = true;
        } else if (arg == "--piano") {
            g_attach_piano = true;
        } else if (arg == "--shot" && i + 1 < argc) {
            shot = argv[++i];
        } else if (arg == "--follow" && i + 1 < argc) {
            follow_pc = std::strtol(argv[++i], nullptr, 16);
        } else if (arg == "--steps" && i + 1 < argc) {
            follow_steps = std::atoi(argv[++i]);
        } else if (arg == "--planes") {
            planes = true;
        } else if (arg == "--break" && i + 1 < argc) {
            break_pc = std::strtol(argv[++i], nullptr, 16);
        } else if (arg == "--break-skip" && i + 1 < argc) {
            break_skip = std::atoi(argv[++i]);
        } else if (arg == "--watch" && i + 1 < argc) {
            watch_addr = std::strtol(argv[++i], nullptr, 16);
        } else if (arg == "--trace-from" && i + 1 < argc) {
            trace_from = std::atoi(argv[++i]);
        } else if (arg == "--trace") {
            trace = true;
        } else if (arg == "--auto-input") {
            auto_input = true;
        } else if (arg == "--state-check") {
            state_check = true;
        } else if (arg == "--state-at" && i + 1 < argc) {
            state_at = std::atoi(argv[++i]);
        } else if (arg == "--state-distance" && i + 1 < argc) {
            state_distance = std::atoi(argv[++i]);
        } else if (arg == "--summary") {
            summary = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (arg == "--dump-from" && i + 1 < argc) {
            dump_from = std::atoi(argv[++i]);
        } else if (arg == "--dump-at" && i + 1 < argc) {
            // Comma-separated frame numbers. --dump-from writes every frame from
            // there on, which is thousands of bitmaps when the screen of
            // interest is deep inside a menu tree.
            std::string list = argv[++i];
            std::size_t pos = 0;
            while (pos <= list.size()) {
                const auto comma = list.find(',', pos);
                const auto piece = list.substr(pos, comma == std::string::npos ? comma : comma - pos);
                if (!piece.empty()) dump_at.push_back(std::atoi(piece.c_str()));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        } else if (arg == "--out" && i + 1 < argc) {
            scratch = argv[++i];
        } else {
            rom = arg;
        }
    }

    if (selftest) {
        return run_selftest(scratch);
    }
    if (state_check && !rom.empty()) {
        return run_state_check(rom, state_at, state_distance, auto_input);
    }
    if (follow_pc >= 0 && !rom.empty()) {
        return run_follow(rom, static_cast<std::uint32_t>(follow_pc), follow_steps, frames);
    }
    if (planes && !rom.empty()) {
        return run_planes(rom, frames, auto_input);
    }
    if (watch_addr >= 0 && !rom.empty()) {
        return run_watch(rom, static_cast<std::uint32_t>(watch_addr), frames);
    }
    if (break_pc >= 0 && !rom.empty()) {
        return run_break(rom, static_cast<std::uint32_t>(break_pc), frames, break_skip);
    }
    if (trace && !rom.empty()) {
        return run_trace(rom, frames, trace_from < 0 ? frames / 2 : trace_from, auto_input);
    }
    if (summary && !rom.empty()) {
        // MERCURY_HOST_PROFILE=N samples this thread and prints the N busiest
        // host functions at the end (RelWithDebInfo build for names).
        const char* profile = std::getenv("MERCURY_HOST_PROFILE");
        mercury::tools::HostProfiler profiler;
        if (profile != nullptr) profiler.start();
        const int result = run_summary(rom, frames, shot, auto_input);
        if (profile != nullptr) profiler.stop_and_report(std::max(10, std::atoi(profile)));
        return result;
    }
    if (rom.empty()) {
        std::cout << "usage: genesis_probe <rom> [--frames N] [--dump-from N] [--out DIR]\n"
                     "       genesis_probe --selftest [--out DIR]\n";
        return 2;
    }

    mercury::genesis::Genesis machine;
    const auto load = load_image(machine, rom);
    if (!load.loaded) {
        std::cout << "load failed: " << load.message << "\n";
        return 1;
    }
    std::cout << load.message << "\n\n";

    // MERCURY_PAD6=1 here too. It used to be set in the summary path only, so
    // the verbose report - the one that prints the port and the register dumps
    // - always showed a three-button pad however the run was configured, which
    // is exactly the report someone reads to check the setting took.
    if (const char* e = std::getenv("MERCURY_PAD6"); e != nullptr && *e != '0') {
        machine.port1().set_kind(mercury::genesis::ControllerPort::Kind::pad6);
        std::printf("port 1: six-button pad\n");
    }
    // Same MERCURY_INPUT_SCRIPT as the summary mode: this is the loop that dumps
    // frames, so it is the one that has to be able to walk a menu.
    std::vector<std::pair<int, std::string>> dump_script;
    if (const char* e = std::getenv("MERCURY_INPUT_SCRIPT")) {
        std::string text(e);
        std::size_t pos = 0;
        while (pos < text.size()) {
            const auto comma = text.find(',', pos);
            const auto piece = text.substr(pos, comma == std::string::npos ? comma : comma - pos);
            const auto colon = piece.find(':');
            if (colon != std::string::npos) {
                dump_script.emplace_back(std::atoi(piece.substr(0, colon).c_str()),
                                         piece.substr(colon + 1));
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        std::sort(dump_script.begin(), dump_script.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::printf("input script: %zu steps\n", dump_script.size());
    }

    // The summary mode's MERCURY_M68K_TRACE, for this loop too, but only with
    // MERCURY_M68K_TRACE_FRAMES: this is the loop that dumps pictures, so it is
    // the one that can put a trace next to the frame it explains. Each line
    // carries the raster position, "L<line>+<cycles into line>".
    std::FILE* dump_trace_file = nullptr;
    long dump_trace_from = -1;
    long dump_trace_to = -1;
    long dump_trace_frame = 0;
    if (const char* path = std::getenv("MERCURY_M68K_TRACE"); path != nullptr && *path != '\0') {
        if (const char* e = std::getenv("MERCURY_M68K_TRACE_FRAMES"); e != nullptr && *e != '\0') {
            char* end = nullptr;
            dump_trace_from = std::strtol(e, &end, 10);
            dump_trace_to = (end != nullptr && *end == '-') ? std::strtol(end + 1, nullptr, 10) : dump_trace_from;
            dump_trace_file = std::fopen(path, "wb");
        }
    }
    if (dump_trace_file != nullptr) {
        machine.set_m68k_trace([&](const mercury::core::M68kStepResult& step,
                                   const mercury::core::M68kRegisters& regs) {
            if (!step.executed || dump_trace_frame < dump_trace_from || dump_trace_frame > dump_trace_to) return;
            std::fprintf(dump_trace_file,
                         "L%u+%u %06X %04X sr=%04X d0=%08X d1=%08X d2=%08X a0=%08X a1=%08X a6=%08X a7=%08X\n",
                         machine.vdp().scanline(), machine.vdp().cycles_into_line(), step.pc_before, step.opcode,
                         regs.sr, regs.d[0], regs.d[1], regs.d[2], regs.a[0], regs.a[1], regs.a[6], regs.a[7]);
        });
        std::printf("m68k trace: frames %ld-%ld\n", dump_trace_from, dump_trace_to);
    }

    for (int frame = 0; frame < frames; ++frame) {
        dump_trace_frame = frame;
        // The same periodic taps run_summary uses. Menus that need a button to
        // advance sit forever without it, and an explicit script cannot express
        // "tap start every two seconds" without hundreds of entries.
        if (auto_input) {
            using mercury::genesis::Button;
            const int phase = frame % 120;
            machine.port1().set_button(Button::start, phase >= 20 && phase < 28);
            machine.port1().set_button(Button::c, phase >= 60 && phase < 68);
            machine.port1().set_button(Button::a, phase >= 90 && phase < 98);
        }
        if (!dump_script.empty()) {
            using mercury::genesis::Button;
            const std::string* active = nullptr;
            for (const auto& step : dump_script) {
                if (frame >= step.first) active = &step.second; else break;
            }
            for (const auto b : {Button::up, Button::down, Button::left, Button::right,
                                 Button::a, Button::b, Button::c, Button::start}) {
                machine.port1().set_button(b, false);
            }
            if (active != nullptr) {
                for (const char ch : *active) {
                    switch (ch) {
                        case 'u': machine.port1().set_button(Button::up, true); break;
                        case 'd': machine.port1().set_button(Button::down, true); break;
                        case 'l': machine.port1().set_button(Button::left, true); break;
                        case 'r': machine.port1().set_button(Button::right, true); break;
                        case 'a': machine.port1().set_button(Button::a, true); break;
                        case 'b': machine.port1().set_button(Button::b, true); break;
                        case 'c': machine.port1().set_button(Button::c, true); break;
                        case 's': machine.port1().set_button(Button::start, true); break;
                        default: break;
                    }
                }
            }
        }
        const auto cycles = machine.run_frame();
        // MERCURY_CTRL_WATCH names a control-port word; these are the addresses
        // that wrote it. A register whose value alternates was written by code
        // that chose between two values, and the branch is at one of these.
        if (frame == frames - 1 && (machine.watch_write_pc() != 0 ||
                                    machine.watch_writer_pcs()[0].second != 0)) {
            std::printf("ctrl-watch writers (pc x count):");
            for (const auto& e : machine.ctrl_writer_pcs()) {
                if (e.second != 0) std::printf("  %06X x%llu", e.first, (unsigned long long)e.second);
            }
            std::printf("\n");
            // The writers above are leaf routines called from a table; the
            // return addresses on the stack are what identify the caller that
            // chose between them.
            if (machine.watch_writer_pcs()[0].second != 0) {
                std::printf("wram-watch writers (pc x count):");
                for (const auto& e : machine.watch_writer_pcs()) {
                    if (e.second != 0) {
                        std::printf("  %06X x%llu", e.first, (unsigned long long)e.second);
                    }
                }
                std::printf("\n");
            }
            if (!machine.watch_stack().empty()) {
                std::printf("stack at the watched write (line %u):", machine.watch_stack_line());
                for (const auto v : machine.watch_stack()) std::printf(" %06X", v & 0xFFFFFF);
                std::printf("\n");
            }
        }
        const bool wanted = (dump_from >= 0 && frame >= dump_from) ||
                            std::find(dump_at.begin(), dump_at.end(), frame) != dump_at.end();
        if (wanted) {
            const auto& vdp = machine.vdp();
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%04d.bmp", frame);
            write_bmp24(scratch / name, vdp.frame_pixels(), vdp.screen_width(), vdp.screen_height(),
                        mercury::genesis::Vdp::kMaxWidth);
            std::snprintf(name, sizeof(name), "frame_%04d.txt", frame);
            dump_screen_state(vdp, scratch / name);
        }
        if (frame % 30 == 0 || frame == frames - 1) {
            std::printf("frame %4d  %7u cycles  pc=%06X  line=%3u\n", frame, cycles,
                        machine.m68k().registers().pc, machine.vdp().scanline());
        }
    }

    std::cout << "\n" << machine.describe() << "\n\n" << machine.ym2612().describe() << "\n";

    // Audio health. A silent game is nearly always one of: the Z80 never ran,
    // the Z80 wedged on an opcode, the driver never got its vblank interrupt,
    // or the chips ran but produced silence. Each has its own line here so the
    // answer does not have to be guessed at.
    const auto& fm = machine.ym2612().sample_buffer();
    const auto& psg = machine.psg().sample_buffer();
    std::int32_t fm_peak = 0;
    for (auto sample : fm) fm_peak = std::max<std::int32_t>(fm_peak, std::abs(static_cast<int>(sample)));
    std::int32_t psg_peak = 0;
    for (auto sample : psg) psg_peak = std::max<std::int32_t>(psg_peak, std::abs(static_cast<int>(sample)));

    // The 32X, when a cartridge has woken it. Silent for a Mega Drive game,
    // which is the overwhelmingly common case and should cost nothing to
    // read past.
    {
        const auto& unit = machine.thirtytwox();
        const auto& startup = unit.startup();
        if (startup.valid || unit.adapter().enabled()) {
            std::printf("\n32X\n");
            std::printf("  %s\n", unit.describe().c_str());
            std::printf("  firmware        : %s\n",
                        unit.bios_loaded() ? "the real BIOS files" : "GenX's own startup code");
            std::printf("  header          : copy %u bytes from %06X to %08X\n", startup.size,
                        startup.source, startup.destination);
            std::printf("  entry points    : master %08X  slave %08X\n", startup.master_entry,
                        startup.slave_entry);
            std::printf("  master PC       : %08X\n", unit.master().registers().pc);
            std::printf("  slave PC        : %08X\n", unit.slave().registers().pc);
            // Where a stalled processor is actually pointing. An address
            // computed through the global base register means nothing without
            // knowing what that register holds, and guessing it wrong sends
            // the whole diagnosis somewhere else.
            for (int which = 0; which < 2; ++which) {
                const auto& r = which == 0 ? unit.master().registers() : unit.slave().registers();
                std::printf("  %s regs      : GBR %08X VBR %08X SP %08X SR %03X\n",
                            which == 0 ? "master" : "slave ", r.gbr, r.vbr, r.r[15], r.sr);
                std::printf("      R0-R7   : %08X %08X %08X %08X %08X %08X %08X %08X\n", r.r[0],
                            r.r[1], r.r[2], r.r[3], r.r[4], r.r[5], r.r[6], r.r[7]);
            }
            std::printf("  %s\n", unit.vdp_description().c_str());
            // Is anything actually being drawn? Two banks of frame buffer and
            // a palette; all three empty means the SH-2s ran but produced no
            // picture, which is a completely different fault from not running.
            for (int bank = 0; bank < 2; ++bank) {
                const auto& fb = unit.bus().frame_buffer(bank);
                std::size_t written = 0;
                for (auto byte : fb) written += byte != 0 ? 1 : 0;
                std::printf("  frame buffer %d  : %zu of %zu bytes written%s\n", bank, written,
                            fb.size(), bank == unit.bus().displayed_bank() ? " (displayed)" : "");
            }
            const auto& pal = unit.bus().palette();
            std::size_t colours = 0;
            for (auto entry : pal) colours += entry != 0 ? 1 : 0;
            std::printf("  palette         : %zu of %zu entries set\n", colours, pal.size());
            // What the layer would actually contribute, and whether the Mega
            // Drive is leaving room for it. A layer full of opaque pixels
            // that still shows nothing is a compositing fault; an empty layer
            // is a rendering one.
            std::array<mercury::thirtytwox::Vdp::Pixel, mercury::thirtytwox::Vdp::kWidth> line{};
            std::size_t opaque = 0;
            std::size_t in_front = 0;
            // Both banks, not just the one showing. "The layer is empty" and
            // "the wrong bank is on screen" look identical from the outside
            // and need completely different fixes.
            auto& bus = machine.thirtytwox().bus();
            const int showing = bus.displayed_bank();
            // MERCURY_32X_BANKS=<dir> also writes each bank as the layer renders
            // it, MAGENTA where a pixel is transparent, so "a black pixel" and
            // "nothing here" can be told apart at a glance.
            const char* bank_dir = std::getenv("MERCURY_32X_BANKS");
            const auto layer_width = static_cast<std::uint32_t>(mercury::thirtytwox::Vdp::kWidth);
            const auto layer_height = static_cast<std::uint32_t>(unit.vdp().height());
            for (int bank = 0; bank < 2; ++bank) {
                bus.set_displayed_bank(bank);
                std::size_t bank_opaque = 0;
                std::vector<std::uint8_t> rgb;
                if (bank_dir != nullptr) {
                    rgb.assign(static_cast<std::size_t>(layer_width) * layer_height * 3, 0);
                }
                for (int y = 0; y < unit.vdp().height(); ++y) {
                    unit.vdp().render_line(y, line.data());
                    for (std::size_t x = 0; x < line.size(); ++x) {
                        const auto& pixel = line[x];
                        bank_opaque += pixel.opaque ? 1 : 0;
                        if (bank_dir != nullptr) {
                            const std::size_t at = (static_cast<std::size_t>(y) * layer_width + x) * 3;
                            rgb[at] = pixel.opaque ? pixel.r : 255;
                            rgb[at + 1] = pixel.opaque ? pixel.g : 0;
                            rgb[at + 2] = pixel.opaque ? pixel.b : 255;
                        }
                    }
                }
                std::printf("  bank %d renders  : %zu opaque pixels\n", bank, bank_opaque);
                // The line table is the first thing to check when a bank holds
                // pixels but renders nothing: every line reads through it.
                const auto& fb = bus.frame_buffer(bank);
                std::printf("  bank %d lines    : 0=%04X 1=%04X 100=%04X 200=%04X\n", bank,
                            (fb[0] << 8) | fb[1], (fb[2] << 8) | fb[3], (fb[200] << 8) | fb[201],
                            (fb[400] << 8) | fb[401]);
                if (bank_dir != nullptr) {
                    write_bmp24(std::filesystem::path(bank_dir) /
                                    ("bank" + std::to_string(bank) + ".bmp"),
                                rgb, layer_width, layer_height, layer_width);
                }
            }
            std::printf("  zero byte writes: %llu to the plain frame buffer region\n",
                        static_cast<unsigned long long>(bus.zero_byte_frame_writes()));
            // IS IT THE PALETTE OR THE PIXELS? A layer of "opaque" pixels that
            // all come out one colour is either a frame buffer holding one
            // index or a palette holding one colour, and the two are opposite
            // bugs. Both are counted here rather than inferred from a picture.
            {
                const auto& palette = bus.palette();
                std::size_t palette_used = 0;
                std::array<bool, 65536> palette_seen{};
                std::size_t palette_distinct = 0;
                for (const auto entry : palette) {
                    if (entry != 0) ++palette_used;
                    if (!palette_seen[entry]) {
                        palette_seen[entry] = true;
                        ++palette_distinct;
                    }
                }
                const auto& fb = bus.frame_buffer(bus.displayed_bank());
                std::array<bool, 256> byte_seen{};
                std::size_t byte_distinct = 0;
                // Past the 256-word line table, which is not picture data.
                for (std::size_t at = 512; at < fb.size(); ++at) {
                    if (!byte_seen[fb[at]]) {
                        byte_seen[fb[at]] = true;
                        ++byte_distinct;
                    }
                }
                std::printf("  palette         : %zu of %zu entries non-zero, %zu distinct "
                            "colours\n",
                            palette_used, palette.size(), palette_distinct);
                // Entry 0 on its own, and the screen priority it is judged
                // against. Index 0 is drawn like any other index, so whether
                // an empty 32X background hides the Mega Drive or leaves it
                // alone comes down to exactly these two values.
                std::printf("  palette entry 0 : %04X (through bit %s), screen priority %s\n",
                            palette[0], (palette[0] & 0x8000) != 0 ? "SET" : "clear",
                            unit.vdp().screen_priority() ? "32X in front" : "Mega Drive in front");
                {
                    // How many frame-buffer pixels are index 0 on the lines
                    // the picture actually uses.
                    std::size_t zero = 0;
                    std::size_t total = 0;
                    for (int y = 0; y < 224; ++y) {
                        const std::size_t entry = static_cast<std::size_t>(y) * 2;
                        const std::size_t base =
                            static_cast<std::size_t>((fb[entry] << 8) | fb[entry + 1]) * 2;
                        for (int x = 0; x < 320 && base + x < fb.size(); ++x) {
                            ++total;
                            if (fb[base + x] == 0) ++zero;
                        }
                    }
                    std::printf("  index-0 pixels  : %zu of %zu on screen\n", zero, total);
                }
                // The values themselves, when there are few enough to name. A
                // palette of one repeated word is a write path that lost the
                // address or the data, and which word it is says which.
                if (palette_distinct <= 6) {
                    std::printf("  palette values  :");
                    for (std::size_t value = 0; value < palette_seen.size(); ++value) {
                        if (!palette_seen[value]) continue;
                        std::size_t count = 0;
                        for (const auto entry : palette) {
                            if (entry == value) ++count;
                        }
                        std::printf(" %04zX x%zu", value, count);
                    }
                    std::printf("\n");
                }
                std::printf("  frame buffer    : %zu distinct byte values in the shown bank\n",
                            byte_distinct);
            }
            bus.set_displayed_bank(showing);
            for (int y = 0; y < unit.vdp().height(); ++y) {
                unit.vdp().render_line(y, line.data());
                for (const auto& pixel : line) {
                    opaque += pixel.opaque ? 1 : 0;
                    in_front += (pixel.opaque && pixel.in_front) ? 1 : 0;
                }
            }
            const auto& backdrop = machine.vdp().frame_backdrop();
            std::size_t empty = 0;
            for (auto byte : backdrop) empty += byte != 0 ? 1 : 0;
            std::printf("  layer pixels    : %zu opaque, %zu of them in front\n", opaque, in_front);
            if (unit.unimplemented_opcodes() != 0) {
                std::printf("  first bad opcode: %04X at PC %08X on the %s, %llu since\n",
                            unit.first_unimplemented(), unit.first_unimplemented_pc(),
                            unit.first_unimplemented_was_slave() ? "slave" : "master",
                            static_cast<unsigned long long>(unit.unimplemented_opcodes()));
            }
            std::printf("  interrupts      : master %llu, slave %llu (masks %02X / %02X)\n",
                        static_cast<unsigned long long>(unit.master_interrupts()),
                        static_cast<unsigned long long>(unit.slave_interrupts()),
                        unit.adapter().sh2_interrupt_mask(0), unit.adapter().sh2_interrupt_mask(1));
            std::printf("  %s\n", unit.adapter().pwm().describe().c_str());
            std::printf("  divider/dma     : master %llu divides, %llu transfers (%llu bytes); "
                        "slave %llu / %llu (%llu)\n",
                        static_cast<unsigned long long>(unit.master_peripherals().divides()),
                        static_cast<unsigned long long>(unit.master_peripherals().dma_transfers()),
                        static_cast<unsigned long long>(unit.master_peripherals().dma_bytes()),
                        static_cast<unsigned long long>(unit.slave_peripherals().divides()),
                        static_cast<unsigned long long>(unit.slave_peripherals().dma_transfers()),
                        static_cast<unsigned long long>(unit.slave_peripherals().dma_bytes()));
            std::printf("  master %s\n", unit.master_peripherals().describe().c_str());
            std::printf("  slave  %s\n", unit.slave_peripherals().describe().c_str());
            // What is actually in the communication ports. Both sides poll
            // these, so when a game hangs the contents say which side is
            // waiting and for what.
            std::printf("  comm ports      :");
            for (int port = 0; port < 8; ++port) {
                std::printf(" %04X", unit.adapter().comm_read(port));
            }
            std::printf("\n");
            // Which adapter register the SH-2s keep coming back to. The
            // communication ports are the usual answer for a game that runs
            // but draws nothing: it is waiting on the 68000.
            {
                const auto& tally = unit.bus().register_reads();
                std::uint32_t busiest = 0;
                for (std::size_t word = 0; word < tally.size(); ++word) {
                    if (tally[word] > tally[busiest]) busiest = static_cast<std::uint32_t>(word);
                }
                if (tally[busiest] != 0) {
                    std::printf("  sh2 polls       :");
                    for (std::size_t word = 0; word < tally.size(); ++word) {
                        if (tally[word] * 20 >= tally[busiest]) {
                            std::printf(" $40%02zX=%u", word * 2, tally[word]);
                        }
                    }
                    std::printf("\n");
                }
            }
            // Which SH-2 on-chip register the game is sitting on, if any.
            // None of that block is modelled, so a poll of one never changes
            // and the game waits for ever.
            if (unit.bus().on_chip_read_count() != 0) {
                auto tally = unit.bus().on_chip_reads();  // a copy, so it can be drained
                std::printf("  on-chip polls   : %llu reads of $FFFFFE00+, busiest:\n",
                            static_cast<unsigned long long>(unit.bus().on_chip_read_count()));
                for (int shown = 0; shown < 4; ++shown) {
                    std::size_t best = 0;
                    for (std::size_t at = 1; at < tally.size(); ++at) {
                        if (tally[at] > tally[best]) best = at;
                    }
                    if (tally[best] == 0) break;
                    std::printf("      $FFFFFE%02zX  %u\n", best, tally[best]);
                    tally[best] = 0;
                }
            }
            std::printf("  mega drive      : %zu of %zu pixels left as backdrop\n", empty,
                        backdrop.size());
        }
    }

    // MERCURY_RAM_DUMP works for ANY cartridge, not just an SVP one. The
    // original print sat inside the SVP report, so asking for a work-RAM dump
    // on a 32X title silently produced nothing: Star Wars Arcade's 68000 runs
    // its wait loop from work RAM at $FF3AD4, and reading that loop is the only
    // way to see which side of the handshake it is sitting on.
    const long work_ram_dump = [] {
        const char* e = std::getenv("MERCURY_RAM_DUMP");
        return e == nullptr ? -1L : std::strtol(e, nullptr, 16);
    }();
    if (work_ram_dump >= 0 && !machine.svp().present()) {
        std::printf("work RAM at %06lX:\n", work_ram_dump);
        for (int r = 0; r < 16; ++r) {
            std::printf("  %06lX:", work_ram_dump + r * 16);
            for (int w = 0; w < 8; ++w) {
                const std::uint32_t a =
                    static_cast<std::uint32_t>(work_ram_dump) + r * 16 + w * 2;
                std::printf(" %04X", machine.read16(a));
            }
            std::printf("\n");
        }
    }

    // MERCURY_VDP_DUMP=<dir> writes the Mega Drive VDP's memories and registers
    // as it stands at the end of the run: vram.bin, cram.bin, vsram.txt and
    // regs.txt. Two runs a few frames apart, diffed, answer "what did the game
    // change when the picture changed?" - which a screenshot cannot, and which
    // the traffic counters only answer as a number.
    if (const char* dir = std::getenv("MERCURY_VDP_DUMP"); dir != nullptr && *dir != '\0') {
        const std::filesystem::path out_dir(dir);
        std::error_code ignored;
        std::filesystem::create_directories(out_dir, ignored);
        const auto& vdp_state = machine.vdp();
        if (std::FILE* f = std::fopen((out_dir / "vram.bin").string().c_str(), "wb")) {
            std::fwrite(vdp_state.vram().data(), 1, vdp_state.vram().size(), f);
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen((out_dir / "cram.bin").string().c_str(), "wb")) {
            for (const auto colour : vdp_state.cram()) {
                const unsigned char pair[2] = {static_cast<unsigned char>(colour >> 8),
                                               static_cast<unsigned char>(colour & 0xFF)};
                std::fwrite(pair, 1, 2, f);
            }
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen((out_dir / "vsram.txt").string().c_str(), "w")) {
            for (std::size_t i = 0; i < 40; ++i) {
                std::fprintf(f, "%zu %04X\n", i, vdp_state.vsram_entry(i));
            }
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen((out_dir / "regs.txt").string().c_str(), "w")) {
            const auto& regs = vdp_state.registers();
            for (std::size_t i = 0; i < regs.size(); ++i) {
                std::fprintf(f, "R%02zu %02X\n", i, regs[i]);
            }
            std::fclose(f);
        }
        std::printf("vdp dump: written to %s\n", out_dir.string().c_str());
    }

    std::printf("\nAudio\n");
    std::printf("  z80 state       : %s\n", machine.z80_running() ? "running" : "held (reset or bus taken)");
    std::printf("  z80 instructions: %llu\n", static_cast<unsigned long long>(machine.z80_instructions()));
    std::printf("  z80 unimplement.: %llu (last at PC=%04X)\n",
                static_cast<unsigned long long>(machine.z80_unimplemented_count()),
                machine.z80_last_bad_pc());
    std::printf("  z80 interrupts  : %llu\n", static_cast<unsigned long long>(machine.z80_interrupts()));
    std::printf("  ym2612 samples  : %zu (peak %d)\n", fm.size(), fm_peak);
    std::printf("  psg samples     : %zu (peak %d)\n", psg.size(), psg_peak);
    return 0;
}
























































































