#pragma once

// A sampling profiler for the emulator itself - where the HOST's time goes,
// not the emulated machine's.
//
// A background thread wakes about a thousand times a second, briefly suspends
// the thread being measured, reads its instruction pointer, and lets it go.
// At the end the addresses are resolved to function names through the PDB
// (build the RelWithDebInfo configuration, or there are no names to find) and
// the busiest functions are printed with their share of the samples.
//
// "The 32X runs at 44fps" says nothing about why; this says which function
// is spending the frame. Windows-only, and off unless asked for.

#include <cstdint>
#include <string>

namespace mercury::tools {

class HostProfiler {
  public:
    // Start sampling the calling thread.
    void start();
    // Stop, resolve, and print the top `rows` functions.
    void stop_and_report(int rows);
    ~HostProfiler();

  private:
    struct State;
    State* state_ = nullptr;
};

}  // namespace mercury::tools
