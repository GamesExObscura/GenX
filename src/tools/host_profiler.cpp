#include "host_profiler.hpp"

#ifdef _WIN32

#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace mercury::tools {

struct HostProfiler::State {
    HANDLE target = nullptr;
    std::atomic<bool> running{false};
    std::thread sampler;
    std::unordered_map<std::uint64_t, std::uint64_t> hits;
    std::uint64_t samples = 0;
};

void HostProfiler::start() {
    delete state_;
    state_ = new State();
    // A real handle to this thread, which the sampler can suspend.
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &state_->target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    state_->running = true;
    timeBeginPeriod(1);
    State* s = state_;
    s->sampler = std::thread([s]() {
        while (s->running.load()) {
            Sleep(1);
            if (SuspendThread(s->target) == static_cast<DWORD>(-1)) {
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(s->target, &context)) {
                ++s->hits[context.Rip];
                ++s->samples;
            }
            ResumeThread(s->target);
        }
    });
}

void HostProfiler::stop_and_report(int rows) {
    if (state_ == nullptr) {
        return;
    }
    state_->running = false;
    if (state_->sampler.joinable()) {
        state_->sampler.join();
    }
    timeEndPeriod(1);

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);

    // Fold addresses into the functions that contain them.
    std::map<std::string, std::uint64_t> by_function;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (const auto& [address, count] : state_->hits) {
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        std::memset(buffer, 0, sizeof(buffer));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        std::string name;
        if (SymFromAddr(process, address, &displacement, symbol)) {
            name = symbol->Name;
        } else {
            char text[32];
            std::snprintf(text, sizeof(text), "?? %llx", static_cast<unsigned long long>(address & ~0xFFFull));
            name = text;
        }
        by_function[name] += count;
    }
    std::vector<std::pair<std::string, std::uint64_t>> sorted(by_function.begin(), by_function.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::printf("host profile: %llu samples\n", static_cast<unsigned long long>(state_->samples));
    const double total = state_->samples == 0 ? 1.0 : static_cast<double>(state_->samples);
    for (int i = 0; i < rows && i < static_cast<int>(sorted.size()); ++i) {
        std::printf("  %5.1f%%  %s\n", 100.0 * static_cast<double>(sorted[static_cast<std::size_t>(i)].second) / total,
                    sorted[static_cast<std::size_t>(i)].first.c_str());
    }
    SymCleanup(process);
    CloseHandle(state_->target);
    delete state_;
    state_ = nullptr;
}

HostProfiler::~HostProfiler() {
    if (state_ != nullptr) {
        state_->running = false;
        if (state_->sampler.joinable()) state_->sampler.join();
        CloseHandle(state_->target);
        delete state_;
    }
}

}  // namespace mercury::tools

#else

namespace mercury::tools {
void HostProfiler::start() {}
void HostProfiler::stop_and_report(int) {}
HostProfiler::~HostProfiler() = default;
}  // namespace mercury::tools

#endif
