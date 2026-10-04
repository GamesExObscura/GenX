// m68k_tests: run the SingleStepTests/680x0 JSON suite against our 68000 core.
//
// Each test gives a complete initial machine state, a complete expected final
// state, a cycle count and the bus transactions. This is a far better oracle
// than zexdoc was for the Z80: it checks every register, the flags, and memory,
// per instruction, in bulk.
//
// Two conventions in the data matter:
//   * a7 is not stored. It IS usp or ssp depending on the S bit in sr.
//   * "prefetch" is the 68000's two-word queue: prefetch[0] is the word at pc
//     and prefetch[1] the word at pc+2. Our core fetches on demand rather than
//     modelling the queue, so the harness plants those two words in memory and
//     compares everything except the queue itself. That still validates decode,
//     addressing, flags and memory effects; queue state and exact bus order are
//     left for when the core models prefetch.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "mercury/core/m68k_cpu.hpp"

namespace {

using mercury::core::M68kCpu;
using mercury::core::M68kBusCallbacks;

// ---- minimal JSON reader ---------------------------------------------------
// Only what this file format uses: objects, arrays, strings, integers.
struct Json {
    enum class Kind { null, number, string, array, object } kind = Kind::null;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    [[nodiscard]] const Json* find(const std::string& key) const {
        for (const auto& field : fields) {
            if (field.first == key) return &field.second;
        }
        return nullptr;
    }
    [[nodiscard]] std::uint32_t as_u32() const { return static_cast<std::uint32_t>(number); }
};

struct Parser {
    const char* p;
    const char* end;

    void skip() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) ++p; }

    Json parse() {
        skip();
        if (p >= end) return {};
        if (*p == '{') return parse_object();
        if (*p == '[') return parse_array();
        if (*p == '"') return parse_string();
        return parse_number();
    }

    Json parse_object() {
        Json node; node.kind = Json::Kind::object;
        ++p;  // {
        for (;;) {
            skip();
            if (p >= end || *p == '}') { if (p < end) ++p; break; }
            auto key = parse_string();
            skip();
            if (p < end && *p == ':') ++p;
            node.fields.emplace_back(key.text, parse());
        }
        return node;
    }

    Json parse_array() {
        Json node; node.kind = Json::Kind::array;
        ++p;  // [
        for (;;) {
            skip();
            if (p >= end || *p == ']') { if (p < end) ++p; break; }
            node.items.push_back(parse());
        }
        return node;
    }

    Json parse_string() {
        Json node; node.kind = Json::Kind::string;
        ++p;  // "
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) ++p;
            node.text.push_back(*p++);
        }
        if (p < end) ++p;
        return node;
    }

    Json parse_number() {
        Json node; node.kind = Json::Kind::number;
        const char* start = p;
        while (p < end && (*p == '-' || *p == '+' || *p == '.' || (*p >= '0' && *p <= '9') ||
                           *p == 'e' || *p == 'E')) ++p;
        node.number = std::strtod(std::string(start, p).c_str(), nullptr);
        if (p == start) ++p;  // skip a token we do not understand (true/false/null)
        return node;
    }
};

// ---- machine ---------------------------------------------------------------
std::vector<std::uint8_t> g_memory;

void apply_state(M68kCpu& cpu, const Json& state) {
    auto& regs = cpu.mutable_registers();
    for (int i = 0; i < 8; ++i) {
        char key[4]; std::snprintf(key, sizeof(key), "d%d", i);
        if (const auto* v = state.find(key)) regs.d[i] = v->as_u32();
    }
    for (int i = 0; i < 7; ++i) {
        char key[4]; std::snprintf(key, sizeof(key), "a%d", i);
        if (const auto* v = state.find(key)) regs.a[i] = v->as_u32();
    }
    const auto* sr = state.find("sr");
    const auto* usp = state.find("usp");
    const auto* ssp = state.find("ssp");
    const auto* pc = state.find("pc");
    if (sr) regs.sr = static_cast<std::uint16_t>(sr->as_u32());
    if (usp) regs.usp = usp->as_u32();
    if (ssp) regs.ssp = ssp->as_u32();
    // a7 aliases whichever stack the S bit selects.
    const bool supervisor = (regs.sr & M68kCpu::kStatusSupervisor) != 0;
    regs.a[7] = supervisor ? regs.ssp : regs.usp;
    if (pc) regs.pc = pc->as_u32();

    if (const auto* ram = state.find("ram")) {
        for (const auto& pair : ram->items) {
            if (pair.items.size() >= 2) {
                const auto address = pair.items[0].as_u32() & 0x00FFFFFF;
                g_memory[address] = static_cast<std::uint8_t>(pair.items[1].as_u32());
            }
        }
    }
    // Plant the prefetch queue as the words at pc and pc+2, which is what it
    // represents.
    if (const auto* prefetch = state.find("prefetch")) {
        if (prefetch->items.size() >= 2 && pc) {
            const auto at = pc->as_u32() & 0x00FFFFFF;
            const auto w0 = static_cast<std::uint16_t>(prefetch->items[0].as_u32());
            const auto w1 = static_cast<std::uint16_t>(prefetch->items[1].as_u32());
            g_memory[at] = static_cast<std::uint8_t>(w0 >> 8);
            g_memory[(at + 1) & 0xFFFFFF] = static_cast<std::uint8_t>(w0 & 0xFF);
            g_memory[(at + 2) & 0xFFFFFF] = static_cast<std::uint8_t>(w1 >> 8);
            g_memory[(at + 3) & 0xFFFFFF] = static_cast<std::uint8_t>(w1 & 0xFF);
        }
    }
}

struct Failure {
    std::string test;
    std::string detail;
};

bool compare_state(const M68kCpu& cpu, const Json& expected, std::string& detail) {
    const auto& regs = cpu.registers();
    char buffer[160];
    for (int i = 0; i < 8; ++i) {
        char key[4]; std::snprintf(key, sizeof(key), "d%d", i);
        if (const auto* v = expected.find(key)) {
            if (regs.d[i] != v->as_u32()) {
                std::snprintf(buffer, sizeof(buffer), "d%d = %08X, expected %08X", i, regs.d[i], v->as_u32());
                detail = buffer; return false;
            }
        }
    }
    for (int i = 0; i < 7; ++i) {
        char key[4]; std::snprintf(key, sizeof(key), "a%d", i);
        if (const auto* v = expected.find(key)) {
            if (regs.a[i] != v->as_u32()) {
                std::snprintf(buffer, sizeof(buffer), "a%d = %08X, expected %08X", i, regs.a[i], v->as_u32());
                detail = buffer; return false;
            }
        }
    }
    // a7 is not stored directly, so check it through whichever stack pointer
    // the S bit selects, plus the inactive one. A wrong SP otherwise only shows
    // up indirectly as a RAM mismatch, which is far harder to read.
    {
        const bool supervisor = (regs.sr & M68kCpu::kStatusSupervisor) != 0;
        const auto* active = expected.find(supervisor ? "ssp" : "usp");
        if (active != nullptr && regs.a[7] != active->as_u32()) {
            std::snprintf(buffer, sizeof(buffer), "a7 (%s) = %08X, expected %08X",
                          supervisor ? "ssp" : "usp", regs.a[7], active->as_u32());
            detail = buffer; return false;
        }
        const auto* inactive = expected.find(supervisor ? "usp" : "ssp");
        const auto held = supervisor ? regs.usp : regs.ssp;
        if (inactive != nullptr && held != inactive->as_u32()) {
            std::snprintf(buffer, sizeof(buffer), "%s = %08X, expected %08X",
                          supervisor ? "usp" : "ssp", held, inactive->as_u32());
            detail = buffer; return false;
        }
    }
    if (const auto* v = expected.find("sr")) {
        if (regs.sr != static_cast<std::uint16_t>(v->as_u32())) {
            std::snprintf(buffer, sizeof(buffer), "sr = %04X, expected %04X", regs.sr, v->as_u32());
            detail = buffer; return false;
        }
    }
    if (const auto* v = expected.find("pc")) {
        if (regs.pc != v->as_u32()) {
            std::snprintf(buffer, sizeof(buffer), "pc = %06X, expected %06X", regs.pc, v->as_u32());
            detail = buffer; return false;
        }
    }
    if (const auto* ram = expected.find("ram")) {
        for (const auto& pair : ram->items) {
            if (pair.items.size() < 2) continue;
            const auto address = pair.items[0].as_u32() & 0x00FFFFFF;
            const auto want = static_cast<std::uint8_t>(pair.items[1].as_u32());
            if (g_memory[address] != want) {
                std::snprintf(buffer, sizeof(buffer), "ram[%06X] = %02X, expected %02X",
                              address, g_memory[address], want);
                detail = buffer; return false;
            }
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: m68k_tests <file.json> [more.json ...] [--verbose N]\n");
        return 2;
    }

    g_memory.assign(0x1000000, 0);

    M68kCpu cpu;
    M68kBusCallbacks bus;
    bus.read16 = [](std::uint32_t a) -> std::uint16_t {
        a &= 0xFFFFFF;
        return static_cast<std::uint16_t>((g_memory[a] << 8) | g_memory[(a + 1) & 0xFFFFFF]);
    };
    bus.write16 = [](std::uint32_t a, std::uint16_t v) {
        a &= 0xFFFFFF;
        g_memory[a] = static_cast<std::uint8_t>(v >> 8);
        g_memory[(a + 1) & 0xFFFFFF] = static_cast<std::uint8_t>(v & 0xFF);
    };
    bus.read8 = [](std::uint32_t a) -> std::uint8_t { return g_memory[a & 0xFFFFFF]; };
    bus.write8 = [](std::uint32_t a, std::uint8_t v) { g_memory[a & 0xFFFFFF] = v; };
    cpu.attach_bus(bus);

    int verbose = 3;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0 && i + 1 < argc) { verbose = std::atoi(argv[++i]); }
        else files.emplace_back(argv[i]);
    }

    std::uint64_t total = 0, passed = 0, unimplemented = 0, cycle_mismatch = 0;
    std::vector<std::string> cycle_examples;
    bool dump_first = true;
    std::vector<Failure> failures;

    for (const auto& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) { std::printf("cannot open %s\n", file.c_str()); continue; }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Parser parser{text.data(), text.data() + text.size()};
        const auto root = parser.parse();

        std::uint64_t file_total = 0, file_pass = 0, file_unimpl = 0;
        for (const auto& test : root.items) {
            const auto* initial = test.find("initial");
            const auto* final_state = test.find("final");
            if (initial == nullptr || final_state == nullptr) continue;
            ++total; ++file_total;

            cpu.reset();
            apply_state(cpu, *initial);
            const auto result = cpu.step();

            if (result.unimplemented) {
                ++unimplemented; ++file_unimpl;
                continue;
            }

            std::string detail;
            if (compare_state(cpu, *final_state, detail)) {
                ++passed; ++file_pass;
                if (const auto* length = test.find("length")) {
                    if (result.cycles != length->as_u32()) {
                        ++cycle_mismatch;
                        // Keep a handful of examples with the name, which
                        // carries the opcode and addressing mode. A bare count
                        // says timing is wrong; these say which mode is wrong.
                        if (cycle_examples.size() < 12) {
                            const auto* name = test.find("name");
                            cycle_examples.push_back(
                                (name ? name->text : std::string("?")) + "  got " +
                                std::to_string(result.cycles) + ", expected " +
                                std::to_string(length->as_u32()));
                        }
                    }
                }
            } else if (failures.size() < 40) {
                const auto* name = test.find("name");
                failures.push_back({name ? name->text : "?", detail});
                if (dump_first && failures.size() == 1) {
                    dump_first = false;
                    std::printf("\n--- full state for %s ---\n", name ? name->text.c_str() : "?");
                    const char* keys[] = {"d0","d1","d2","d3","d4","d5","d6","d7",
                                          "a0","a1","a2","a3","a4","a5","a6",
                                          "usp","ssp","sr","pc"};
                    std::printf("initial:");
                    for (const auto* k : keys) {
                        if (const auto* v = initial->find(k)) std::printf(" %s=%08X", k, v->as_u32());
                    }
                    std::printf("\n");
                    if (const auto* pf = initial->find("prefetch")) {
                        std::printf("prefetch:");
                        for (const auto& w : pf->items) std::printf(" %04X", w.as_u32());
                        std::printf("\n");
                    }
                    if (const auto* ram = initial->find("ram")) {
                        std::printf("initial ram:");
                        for (const auto& pair : ram->items) {
                            if (pair.items.size() >= 2)
                                std::printf(" [%06X]=%02X", pair.items[0].as_u32(), pair.items[1].as_u32());
                        }
                        std::printf("\n");
                    }
                    std::printf("final  :");
                    for (const auto* k : keys) {
                        if (const auto* v = final_state->find(k)) std::printf(" %s=%08X", k, v->as_u32());
                    }
                    std::printf("\nours   : pc=%08X sr=%04X", cpu.registers().pc, cpu.registers().sr);
                    for (int i = 0; i < 8; ++i) std::printf(" d%d=%08X", i, cpu.registers().d[i]);
                    for (int i = 0; i < 8; ++i) std::printf(" a%d=%08X", i, cpu.registers().a[i]);
                    std::printf("\n\n");
                }
            }
        }
        std::printf("  %-24s %6llu tests  %6llu pass  %6llu unimplemented\n",
                    std::filesystem::path(file).filename().string().c_str(),
                    static_cast<unsigned long long>(file_total),
                    static_cast<unsigned long long>(file_pass),
                    static_cast<unsigned long long>(file_unimpl));
    }

    std::printf("\ntotal %llu | pass %llu | fail %llu | unimplemented %llu\n",
                static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(passed),
                static_cast<unsigned long long>(total - passed - unimplemented),
                static_cast<unsigned long long>(unimplemented));
    std::printf("cycle-count mismatches among passing tests: %llu\n",
                static_cast<unsigned long long>(cycle_mismatch));
    for (const auto& example : cycle_examples) {
        std::printf("  %s\n", example.c_str());
    }

    if (!failures.empty()) {
        std::printf("\nfirst failures:\n");
        for (int i = 0; i < verbose && i < static_cast<int>(failures.size()); ++i) {
            std::printf("  %-28s %s\n", failures[i].test.c_str(), failures[i].detail.c_str());
        }
    }
    return failures.empty() ? 0 : 1;
}
