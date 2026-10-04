#pragma once

// Save states.
//
// Every component describes its state ONCE, in a templated visit_state(), and
// the same description drives both saving and loading. Two parallel lists - one
// that writes and one that reads - is how a save state ends up restoring
// fifteen of a component's sixteen fields, and the sixteenth then shows up as a
// game that runs slightly wrong after a load with nothing to point at.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace mercury::core {

class StateWriter {
  public:
    explicit StateWriter(std::vector<std::uint8_t>& out) noexcept : out_(&out) {}

    template <typename T>
    void operator()(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "state fields must be trivially copyable");
        raw(&value, sizeof(T));
    }

    template <typename T, std::size_t N>
    void operator()(const std::array<T, N>& value) {
        static_assert(std::is_trivially_copyable_v<T>, "state arrays must hold plain data");
        raw(value.data(), sizeof(T) * N);
    }

    template <typename T>
    void operator()(const std::vector<T>& value) {
        static_assert(std::is_trivially_copyable_v<T>, "state vectors must hold plain data");
        const auto count = static_cast<std::uint64_t>(value.size());
        raw(&count, sizeof(count));
        if (count != 0) {
            raw(value.data(), sizeof(T) * value.size());
        }
    }

    // A vector whose length genuinely varies - a queue. Written exactly as
    // above; see StateReader::variable for why the reader treats it apart.
    template <typename T>
    void variable(const std::vector<T>& value, std::size_t /*max*/) {
        (*this)(value);
    }

    [[nodiscard]] static constexpr bool loading() noexcept { return false; }

  private:
    void raw(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        out_->insert(out_->end(), bytes, bytes + size);
    }
    std::vector<std::uint8_t>* out_;
};

class StateReader {
  public:
    StateReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    template <typename T>
    void operator()(T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "state fields must be trivially copyable");
        raw(&value, sizeof(T));
    }

    template <typename T, std::size_t N>
    void operator()(std::array<T, N>& value) {
        static_assert(std::is_trivially_copyable_v<T>, "state arrays must hold plain data");
        raw(value.data(), sizeof(T) * N);
    }

    // A memory: VRAM, work RAM, cartridge RAM, a frame buffer. Its size is the
    // MACHINE's, never the file's - a state whose length disagrees is refused,
    // not obeyed. A state can arrive from someone else (the netplay host sends
    // the joiner its whole machine, and states are shared as files), and
    // resizing to the file's number would let a crafted one shrink, say, VRAM
    // to one byte while everything that indexes VRAM still assumes 64KB - a
    // write past the end of the buffer on the very next frame.
    template <typename T>
    void operator()(std::vector<T>& value) {
        static_assert(std::is_trivially_copyable_v<T>, "state vectors must hold plain data");
        std::uint64_t count = 0;
        raw(&count, sizeof(count));
        if (!ok_) {
            return;
        }
        if (count != value.size()) {
            ok_ = false;
            return;
        }
        if (count != 0) {
            raw(value.data(), sizeof(T) * value.size());
        }
    }

    // A queue, whose length is part of the state: taken from the file, but
    // never past `max` (what the machine itself could ever have queued, with
    // room) and never past what the file actually holds. Code that walks a
    // queue goes by its size(), so any length up to the cap is safe.
    template <typename T>
    void variable(std::vector<T>& value, std::size_t max) {
        static_assert(std::is_trivially_copyable_v<T>, "state vectors must hold plain data");
        std::uint64_t count = 0;
        raw(&count, sizeof(count));
        if (!ok_) {
            return;
        }
        if (count > max || count > (size_ - at_) / sizeof(T)) {
            ok_ = false;
            return;
        }
        value.resize(static_cast<std::size_t>(count));
        if (count != 0) {
            raw(value.data(), sizeof(T) * value.size());
        }
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t consumed() const noexcept { return at_; }
    [[nodiscard]] static constexpr bool loading() noexcept { return true; }

  private:
    void raw(void* destination, std::size_t size) {
        if (!ok_ || at_ + size > size_) {
            ok_ = false;
            return;
        }
        std::memcpy(destination, data_ + at_, size);
        at_ += size;
    }
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

}  // namespace mercury::core
