// z80_coverage: brute-force check of which Z80 opcodes the core implements.
//
// For every opcode in the base, CB, ED, DD, FD, DDCB and FDCB spaces, plant
// the encoding at address 0x0000 (where reset() leaves PC) and single-step
// once, reporting any that come back flagged unimplemented. Far faster than
// discovering gaps one long zexdoc run at a time.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "mercury/core/z80_cpu.hpp"

namespace {

struct Space {
    const char* name;
    int prefix1;  // -1 = none
    int prefix2;  // -1 = none (for DDCB/FDCB, displacement sits between)
};

}  // namespace

int main() {
    std::vector<std::uint8_t> memory(65536, 0x00);

    mercury::core::Z80Cpu cpu;
    mercury::core::Z80BusCallbacks callbacks;
    callbacks.read8 = [&memory](std::uint16_t address) { return memory[address]; };
    callbacks.write8 = [&memory](std::uint16_t address, std::uint8_t value) { memory[address] = value; };
    callbacks.in = [](std::uint8_t) { return std::uint8_t{0xFF}; };
    callbacks.out = [](std::uint8_t, std::uint8_t) {};
    callbacks.io_wait_cycles = [](std::uint8_t, bool) { return std::uint32_t{0}; };
    callbacks.advance_io_timing = [](std::uint32_t) {};
    cpu.attach_bus(callbacks);

    const Space spaces[] = {
        {"base", -1, -1},
        {"CB",   0xCB, -1},
        {"ED",   0xED, -1},
        {"DD",   0xDD, -1},
        {"FD",   0xFD, -1},
        {"DDCB", 0xDD, 0xCB},
        {"FDCB", 0xFD, 0xCB},
    };

    int total_missing = 0;

    // Opcodes that genuinely have an IX/IY form; a DD/FD prefix in front of
    // any of these must NOT fall back to the unprefixed instruction.
    auto has_indexed_form = [](int op) {
        if (op == 0x09 || op == 0x19 || op == 0x29 || op == 0x39) return true;   // ADD IX,rr
        if (op == 0x21 || op == 0x22 || op == 0x2A) return true;                 // LD IX,nn / (nn)
        if (op == 0x23 || op == 0x2B) return true;                               // INC/DEC IX
        if (op == 0x24 || op == 0x25 || op == 0x26) return true;                 // IXH ops
        if (op == 0x2C || op == 0x2D || op == 0x2E) return true;                 // IXL ops
        if (op == 0x34 || op == 0x35 || op == 0x36) return true;                 // (IX+d) inc/dec/ld
        if (op == 0xCB) return true;                                             // DDCB
        if (op == 0xE1 || op == 0xE3 || op == 0xE5 || op == 0xE9 || op == 0xF9) return true;
        // LD r,r' and ALU forms touching H/L (which become IXH/IXL) or (HL)->(IX+d)
        if (op >= 0x40 && op <= 0x7F && op != 0x76) {
            const int dst = (op >> 3) & 0x07;
            const int src = op & 0x07;
            return dst == 4 || dst == 5 || dst == 6 || src == 4 || src == 5 || src == 6;
        }
        if (op >= 0x80 && op <= 0xBF) {
            const int src = op & 0x07;
            return src == 4 || src == 5 || src == 6;
        }
        return false;
    };

    for (const auto& space : spaces) {
        std::vector<int> missing;
        std::vector<int> ignored;

        for (int op = 0; op <= 0xFF; ++op) {
            for (auto& byte : memory) {
                byte = 0x00;
            }

            std::size_t at = 0;
            if (space.prefix1 >= 0) {
                memory[at++] = static_cast<std::uint8_t>(space.prefix1);
            }
            if (space.prefix2 >= 0) {
                memory[at++] = static_cast<std::uint8_t>(space.prefix2);
                memory[at++] = 0x00;  // displacement
            }
            memory[at] = static_cast<std::uint8_t>(op);

            cpu.reset();
            const auto result = cpu.step();
            if (!result.executed || result.unimplemented) {
                missing.push_back(op);
            } else if (result.message.find("prefix ignored") != std::string::npos) {
                // For DD/FD this is correct hardware behaviour only when the
                // opcode genuinely has no IX/IY form. Record them so the list
                // can be checked against the ones that should be indexed.
                ignored.push_back(op);
            }
        }

        std::printf("%-5s : %3zu/256 implemented", space.name, static_cast<std::size_t>(256 - missing.size()));
        if (missing.empty()) {
            std::printf("   (complete)\n");
        } else {
            std::printf("   MISSING %zu:", missing.size());
            int shown = 0;
            for (const int op : missing) {
                if (shown++ == 24) {
                    std::printf(" ...");
                    break;
                }
                std::printf(" %02X", op);
            }
            std::printf("\n");
            total_missing += static_cast<int>(missing.size());
        }

        if (std::string(space.name) == "DD" || std::string(space.name) == "FD") {
            std::vector<int> wrongly_ignored;
            for (const int op : ignored) {
                if (has_indexed_form(op)) {
                    wrongly_ignored.push_back(op);
                }
            }
            std::printf("        prefix-ignored fallbacks: %zu total, %zu of them WRONG",
                        ignored.size(), wrongly_ignored.size());
            if (!wrongly_ignored.empty()) {
                std::printf(" ->");
                for (const int op : wrongly_ignored) {
                    std::printf(" %02X", op);
                }
                total_missing += static_cast<int>(wrongly_ignored.size());
            }
            std::printf("\n");
        }
    }

    std::printf("\ntotal unimplemented encodings: %d\n", total_missing);
    return 0;
}
