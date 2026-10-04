#include "mercury/core/module_status.hpp"

namespace mercury::core {

std::string_view to_string(ModuleState state) {
    switch (state) {
        case ModuleState::planned:
            return "planned";
        case ModuleState::scaffolded:
            return "scaffolded";
        case ModuleState::in_progress:
            return "in-progress";
        case ModuleState::validated:
            return "validated";
    }

    return "unknown";
}

}  // namespace mercury::core
