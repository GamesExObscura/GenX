// z80_zex: run Frank Cringle's Z80 instruction exerciser (zexdoc/zexall)
// against the Mercury Z80 core in a minimal CP/M environment.
//
// No Game Gear hardware is involved: the CPU sees a flat 64 KB RAM. The
// exerciser self-checks every instruction against known-good CRCs and prints
// "OK" or "ERROR" per instruction group, so this gives a definitive verdict
// on CPU correctness instead of inference from game behaviour.
//
// CP/M shim:
//   0000: JP 0008          (reset vector -> our startup)
//   0005: IN A,(FF) / RET  (BDOS entry; the IN is intercepted by the harness)
//   0008: LD SP,F000 / JP 0100
//   0100: the .COM image
//
// The Z80 core exposes no register setters, so control flow is steered by
// planting real instructions in memory rather than poking PC/SP directly.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "mercury/core/z80_cpu.hpp"

namespace {

std::string hex8(std::uint8_t value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02X", value);
    return buffer;
}

std::string hex16(std::uint16_t value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%04X", value);
    return buffer;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: z80_zex <zexdoc.com|zexall.com> [--max-steps N]\n");
        return 2;
    }

    std::uint64_t max_steps = 40'000'000'000ull;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--max-steps" && i + 1 < argc) {
            max_steps = std::strtoull(argv[++i], nullptr, 10);
        }
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    const std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    if (image.empty()) {
        std::fprintf(stderr, "empty image\n");
        return 1;
    }
    std::printf("loaded %s (%zu bytes)\n\n", argv[1], image.size());

    std::vector<std::uint8_t> memory(65536, 0x00);
    for (std::size_t i = 0; i < image.size() && (0x0100 + i) < memory.size(); ++i) {
        memory[0x0100 + i] = image[i];
    }

    // Reset vector -> startup stub.
    memory[0x0000] = 0xC3;  // JP 0008
    memory[0x0001] = 0x08;
    memory[0x0002] = 0x00;
    // BDOS entry.
    memory[0x0005] = 0xDB;  // IN A,(FF)
    memory[0x0006] = 0xFF;
    memory[0x0007] = 0xC9;  // RET
    // Startup stub.
    memory[0x0008] = 0x31;  // LD SP,F000
    memory[0x0009] = 0x00;
    memory[0x000A] = 0xF0;
    memory[0x000B] = 0xC3;  // JP 0100
    memory[0x000C] = 0x00;
    memory[0x000D] = 0x01;

    mercury::core::Z80Cpu cpu;
    bool finished = false;

    mercury::core::Z80BusCallbacks callbacks;
    callbacks.read8 = [&memory](std::uint16_t address) { return memory[address]; };
    callbacks.write8 = [&memory](std::uint16_t address, std::uint8_t value) {
        memory[address] = value;
    };
    callbacks.out = [](std::uint8_t, std::uint8_t) {};
    callbacks.in = [&](std::uint8_t port) -> std::uint8_t {
        if (port != 0xFF) {
            return 0xFF;
        }
        const auto& reg = cpu.registers();
        if (reg.c == 2) {
            std::putchar(static_cast<int>(reg.e));
            std::fflush(stdout);
        } else if (reg.c == 9) {
            auto address = static_cast<std::uint16_t>((reg.d << 8) | reg.e);
            for (int guard = 0; guard < 65536; ++guard) {
                const auto ch = memory[address];
                if (ch == '$') {
                    break;
                }
                std::putchar(static_cast<int>(ch));
                address = static_cast<std::uint16_t>(address + 1);
            }
            std::fflush(stdout);
        } else if (reg.c == 0) {
            finished = true;
        }
        return 0x00;
    };
    callbacks.io_wait_cycles = [](std::uint8_t, bool) { return std::uint32_t{0}; };
    callbacks.advance_io_timing = [](std::uint32_t) {};

    cpu.attach_bus(callbacks);
    cpu.reset();

    std::uint64_t steps = 0;
    while (!finished && steps < max_steps) {
        const auto result = cpu.step();
        ++steps;

        if (!result.executed || result.unimplemented) {
            std::printf("\n\n*** CPU STOPPED: PC=%s prefix=%s op=%s : %s\n",
                        hex16(result.pc_before).c_str(),
                        hex8(result.prefix).c_str(),
                        hex8(result.opcode).c_str(),
                        result.message.c_str());
            std::printf("*** after %llu steps\n", static_cast<unsigned long long>(steps));
            return 3;
        }

        // CP/M warm boot: program jumped back to 0000 to exit.
        if (steps > 64 && result.pc_after == 0x0000) {
            finished = true;
        }
    }

    std::printf("\n\nfinished after %llu steps%s\n",
                static_cast<unsigned long long>(steps),
                finished ? "" : " (step cap reached)");
    return 0;
}
