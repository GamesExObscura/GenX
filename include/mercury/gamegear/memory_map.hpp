#pragma once

#include <cstdint>
#include <string>

namespace mercury::gamegear {

class MemoryMap {
public:
    static constexpr std::uint16_t kCartridgeArea0Start = 0x0000;
    static constexpr std::uint16_t kCartridgeArea1Start = 0x4000;
    static constexpr std::uint16_t kCartridgeArea2Start = 0x8000;
    static constexpr std::uint16_t kWorkRamStart = 0xC000;
    static constexpr std::uint16_t kWorkRamEnd = 0xDFFF;

    [[nodiscard]] std::string describe() const;
};

}  // namespace mercury::gamegear
