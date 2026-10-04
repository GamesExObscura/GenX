#include "mercury/gamegear/memory_map.hpp"

#include <iomanip>
#include <sstream>

namespace mercury::gamegear {

std::string MemoryMap::describe() const {
    std::ostringstream out;
    out << "Memory map: cart[0000-3FFF, 4000-7FFF, 8000-BFFF], "
        << "RAM[" << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << kWorkRamStart
        << "-" << std::setw(4) << kWorkRamEnd << "]";
    return out.str();
}

}  // namespace mercury::gamegear
