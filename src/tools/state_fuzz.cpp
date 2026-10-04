// Hostile save-state test.
//
// A save state is not always GenX's own: the netplay host sends the joiner its
// whole machine, and state files get shared. Everything a state restores is
// therefore input from someone else, and a field the emulator later uses as an
// index - a FIFO position, a channel number, a bank, a counter - is a way to
// make it read or write memory it does not own, unless the emulator masks or
// checks it.
//
// This takes a real state from a real game, maps where every field of it sits
// (by running the machine's own visit_state through an archive that only
// counts), and then for each small field in turn writes hostile values into a
// copy of the state - all ones, the top bit, a large number, random bits -
// loads it and runs a few frames. Built with AddressSanitizer
// (MERCURY_ASAN=ON) any read or write out of bounds is reported, with the
// field that was being tested printed just before it. Hangs and hardware
// exceptions are reported too.
//
// Bulk memories - RAM, VRAM - are the software's own to fill with anything,
// so the emulator already has to cope with any contents there; they are only
// spot-checked.
//
// Usage: state_fuzz <image> [--save-at N] [--frames K] [--start F] [--seed S]
//        (Game Gear and Master System images run on the Game Gear machine.)

#include "mercury/gamegear/game_gear.hpp"
#include "mercury/genesis/genesis.hpp"
#include "mercury/segacd/bios.hpp"

#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#pragma comment(lib, "dbghelp.lib")

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Field {
    std::size_t offset;
    std::size_t size;
    bool bulk;  // a spot inside a large memory rather than a field of its own
    void* site; // the ar(...) call that visits it, for --where
};

// An archive that writes nothing and remembers where each field would go -
// and which ar(...) call in which visit_state put it there, so a finding can
// be named by source line (--where) instead of worked out from offsets.
class FieldMap {
  public:
    template <typename T>
    __declspec(noinline) void operator()(const T&) {
        site_ = _ReturnAddress();
        add_chunked(sizeof(T));
    }
    template <typename T, std::size_t N>
    __declspec(noinline) void operator()(const std::array<T, N>&) {
        site_ = _ReturnAddress();
        const std::size_t bytes = sizeof(T) * N;
        if (bytes <= kChunkLimit) {
            for (std::size_t i = 0; i < N; ++i) add_chunked(sizeof(T));
        } else {
            add_bulk(bytes);
        }
    }
    template <typename T>
    __declspec(noinline) void operator()(const std::vector<T>& v) {
        site_ = _ReturnAddress();
        at_ += 8;  // the length, which must match the machine's or the load is refused
        add_bulk(sizeof(T) * v.size());
    }
    template <typename T>
    __declspec(noinline) void variable(const std::vector<T>& v, std::size_t) {
        site_ = _ReturnAddress();
        fields_.push_back({at_, 8, false, site_});  // a queue's length is fair game
        at_ += 8;
        add_bulk(sizeof(T) * v.size());
    }
    [[nodiscard]] static constexpr bool loading() noexcept { return false; }

    [[nodiscard]] std::size_t size() const noexcept { return at_; }
    [[nodiscard]] const std::vector<Field>& fields() const noexcept { return fields_; }

  private:
    // Fixed arrays up to this size are internal tables (channel and operator
    // structs, caches, FIFOs) and every word of them is tested. The real
    // memories are vectors and only spot-checked.
    static constexpr std::size_t kChunkLimit = 16384;
    void add_chunked(std::size_t bytes) {
        // Scalars as themselves; a larger plain struct as 4-byte pieces, since
        // it may hold indices of its own.
        if (bytes <= 8) {
            fields_.push_back({at_, bytes, false, site_});
            at_ += bytes;
            return;
        }
        while (bytes > 0) {
            const std::size_t piece = bytes >= 4 ? 4 : bytes;
            fields_.push_back({at_, piece, false, site_});
            at_ += piece;
            bytes -= piece;
        }
    }
    void add_bulk(std::size_t bytes) {
        // A handful of spots, evenly spread, so a large INTERNAL table (a
        // cache, a FIFO) that happens to be big is not skipped entirely.
        if (bytes >= 4) {
            for (std::size_t k = 0; k < 8; ++k) {
                const std::size_t at = ((bytes - 4) * k / 7) & ~std::size_t{3};
                fields_.push_back({at_ + at, 4, true, site_});
            }
        }
        at_ += bytes;
    }
    void* site_ = nullptr;
    std::size_t at_ = 0;
    std::vector<Field> fields_;
};

bool is_game_gear(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".gg" || ext == ".sms" || ext == ".sg";
}

mercury::genesis::Genesis::LoadResult load_genesis(mercury::genesis::Genesis& machine,
                                                   const std::filesystem::path& image) {
    if (!mercury::segacd::Disc::is_disc_image(image)) {
        return machine.load_rom(image);
    }
    mercury::segacd::Disc peek;
    std::string error;
    mercury::genesis::Genesis::LoadResult failed;
    if (!peek.open(image, error)) {
        failed.message = error;
        return failed;
    }
    std::filesystem::path configured;
    std::vector<std::uint8_t> bios;
    if (const char* e = std::getenv("MERCURY_SEGACD_BIOS"); e != nullptr && *e != '\0') {
        configured = e;
        std::error_code ignored;
        if (std::filesystem::is_regular_file(configured, ignored)) {
            std::ifstream in(configured, std::ios::binary);
            bios.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    if (bios.empty() && !mercury::segacd::load_bios_for(peek.region(), configured, image, bios, error)) {
        failed.message = error;
        return failed;
    }
    return machine.load_cd(image, bios);
}

// What the fuzzer needs from either machine.
struct Target {
    virtual ~Target() = default;
    virtual std::vector<std::uint8_t> save() = 0;
    virtual bool load(const std::vector<std::uint8_t>& blob, std::string& error) = 0;
    virtual void frame() = 0;
    virtual FieldMap map() = 0;
};

struct GenesisTarget : Target {
    std::unique_ptr<mercury::genesis::Genesis> machine = std::make_unique<mercury::genesis::Genesis>();
    std::vector<std::uint8_t> save() override { return machine->save_state_blob(); }
    bool load(const std::vector<std::uint8_t>& blob, std::string& error) override {
        return machine->load_state_blob(blob, error);
    }
    void frame() override { (void)machine->run_frame(); }
    FieldMap map() override {
        FieldMap m;
        machine->visit_state(m, mercury::genesis::Genesis::kStateVersion, machine->state_media());
        return m;
    }
};

struct GameGearTarget : Target {
    std::unique_ptr<mercury::gamegear::GameGear> machine = std::make_unique<mercury::gamegear::GameGear>();
    std::vector<std::uint8_t> save() override { return machine->save_state_blob(); }
    bool load(const std::vector<std::uint8_t>& blob, std::string& error) override {
        return machine->load_state_blob(blob, error);
    }
    void frame() override {
        const auto start = machine->vdp().frames_elapsed();
        for (int i = 0; i < 2'000'000 && machine->vdp().frames_elapsed() == start; ++i) {
            (void)machine->step();
        }
    }
    FieldMap map() override {
        FieldMap m;
        machine->visit_state(m);
        return m;
    }
};

std::atomic<std::int64_t> g_heartbeat{0};
std::atomic<bool> g_done{false};
char g_current[160] = "";

// Frames, inside SEH so a hardware exception (divide by zero, a wild pointer
// ASan did not instrument) is reported and the run goes on. No objects with
// destructors may live in a function that uses __try.
int run_frames_guarded(Target* target, int frames) {
    __try {
        for (int i = 0; i < frames; ++i) target->frame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<int>(GetExceptionCode());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: state_fuzz <image> [--save-at N] [--frames K] [--start F] [--seed S]\n");
        return 2;
    }
    const std::filesystem::path image = argv[1];
    int save_at = 600;
    int frames = 3;
    std::size_t start = 0;
    std::size_t end = static_cast<std::size_t>(-1);
    bool count_only = false;
    std::vector<std::size_t> where;
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--count") { count_only = true; continue; }
        if (i + 1 >= argc) break;
        if (a == "--where") {
            // Field numbers, comma separated: print which ar(...) line visits each.
            std::string list = argv[++i];
            std::size_t at = 0;
            while (at < list.size()) {
                const auto comma = list.find(',', at);
                where.push_back(static_cast<std::size_t>(std::atoll(list.substr(at, comma - at).c_str())));
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
            continue;
        }
        if (a == "--save-at") save_at = std::atoi(argv[++i]);
        else if (a == "--frames") frames = std::atoi(argv[++i]);
        else if (a == "--start") start = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (a == "--end") end = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (a == "--seed") seed = std::strtoull(argv[++i], nullptr, 0);
    }

    std::unique_ptr<Target> target;
    if (is_game_gear(image)) {
        auto gg = std::make_unique<GameGearTarget>();
        const auto load = gg->machine->load_rom(image);
        if (!load.ok) { std::fprintf(stderr, "load failed: %s\n", load.error.c_str()); return 1; }
        gg->machine->reset();
        target = std::move(gg);
    } else {
        auto md = std::make_unique<GenesisTarget>();
        const auto load = load_genesis(*md->machine, image);
        if (!load.loaded) { std::fprintf(stderr, "load failed: %s\n", load.message.c_str()); return 1; }
        target = std::move(md);
    }
    for (int i = 0; i < save_at; ++i) target->frame();
    const auto clean = target->save();
    const auto map = target->map();
    const std::size_t header = clean.size() - map.size();
    std::fprintf(stderr, "state %zu bytes, header %zu, %zu fields to test (from %zu)\n", clean.size(),
                 header, map.fields().size(), start);
    if (count_only) {
        std::printf("%zu\n", map.fields().size());
        return 0;
    }
    if (!where.empty()) {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(process, nullptr, TRUE);
        for (const auto f : where) {
            if (f >= map.fields().size()) continue;
            const auto& field = map.fields()[f];
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD displacement = 0;
            // The return address is just past the call; step back one byte
            // so the line is the call's own.
            const auto address = reinterpret_cast<DWORD64>(field.site) - 1;
            char symbol_space[sizeof(SYMBOL_INFO) + 256] = {};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_space);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 sym_disp = 0;
            const bool have_sym = SymFromAddr(process, address, &sym_disp, symbol) != FALSE;
            // visit_state bodies inline into one another, so walk the inline
            // chain: the innermost frame is the ar(...) line itself.
            const DWORD inline_frames = SymAddrIncludeInlineTrace(process, address);
            DWORD context = 0, frame_index = 0;
            if (inline_frames > 0 &&
                SymQueryInlineTrace(process, address, 0, address, address, &context, &frame_index)) {
                std::printf("field %zu offset %zu size %zu%s:", f, field.offset, field.size, field.bulk ? " [bulk]" : "");
                for (DWORD k = 0; k < inline_frames; ++k) {
                    IMAGEHLP_LINE64 inl{};
                    inl.SizeOfStruct = sizeof(inl);
                    DWORD inl_disp = 0;
                    if (SymGetLineFromInlineContext(process, address, context + k, 0, &inl_disp, &inl)) {
                        const char* name = std::strrchr(inl.FileName, '\\');
                        std::printf(" %s:%lu", name != nullptr ? name + 1 : inl.FileName, inl.LineNumber);
                    }
                }
                std::printf("\n");
            } else if (SymGetLineFromAddr64(process, address, &displacement, &line)) {
                std::printf("field %zu offset %zu size %zu%s: %s:%lu (%s)\n", f, field.offset, field.size,
                            field.bulk ? " [bulk]" : "", line.FileName, line.LineNumber,
                            have_sym ? symbol->Name : "?");
            } else {
                std::printf("field %zu offset %zu size %zu: no line info (%s)\n", f, field.offset, field.size,
                            have_sym ? symbol->Name : "?");
            }
        }
        SymCleanup(process);
        return 0;
    }
    if (end > map.fields().size()) end = map.fields().size();

    // A watchdog: one mutation that runs for 20 seconds is a hang - report it
    // and stop, since the stuck thread cannot be taken back.
    std::thread watchdog([] {
        std::int64_t last = -1;
        int still = 0;
        while (!g_done) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const auto now = g_heartbeat.load();
            still = (now == last) ? still + 1 : 0;
            last = now;
            if (still >= 20) {
                std::fprintf(stderr, "HANG while testing %s\n", g_current);
                std::fflush(stderr);
                std::_Exit(3);
            }
        }
    });

    std::uint64_t rng = seed;
    const auto next = [&rng]() {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        return rng;
    };
    std::size_t refused = 0, ran = 0, faults = 0;
    std::string error;
    for (std::size_t f = start; f < end; ++f) {
        const auto& field = map.fields()[f];
        const std::size_t at = header + field.offset;
        const std::size_t size = field.size;
        const std::uint64_t mask = size >= 8 ? ~0ull : ((1ull << (size * 8)) - 1);
        const std::uint64_t values[] = {
            ~0ull & mask,
            (1ull << (size * 8 - 1)) & mask,
            (size >= 4 ? 0x00100000ull : (size >= 2 ? 0x1000ull : 0x7Full)) & mask,
            next() & mask,
            next() & mask,
        };
        for (const auto value : values) {
            auto blob = clean;
            std::uint64_t original = 0;
            std::memcpy(&original, blob.data() + at, size);
            if (original == value) continue;
            std::memcpy(blob.data() + at, &value, size);
            std::snprintf(g_current, sizeof(g_current), "field %zu offset %zu size %zu%s value %llX (was %llX)",
                          f, field.offset, size, field.bulk ? " [bulk]" : "",
                          static_cast<unsigned long long>(value), static_cast<unsigned long long>(original));
            std::fprintf(stderr, "T %s\n", g_current);
            std::fflush(stderr);
            if (!target->load(blob, error)) {
                ++refused;
            } else {
                ++ran;
                const int code = run_frames_guarded(target.get(), frames);
                if (code != 0) {
                    ++faults;
                    std::fprintf(stderr, "FAULT 0x%08X while testing %s\n", static_cast<unsigned>(code), g_current);
                    std::fflush(stderr);
                }
            }
            ++g_heartbeat;
            // Back to a clean machine for the next one.
            if (!target->load(clean, error)) {
                std::fprintf(stderr, "could not restore the clean state: %s\n", error.c_str());
                g_done = true;
                watchdog.join();
                return 1;
            }
        }
    }
    g_done = true;
    watchdog.join();
    std::fprintf(stderr, "DONE: %zu loaded and ran, %zu refused by the loader, %zu hardware faults\n", ran,
                 refused, faults);
    return 0;
}
