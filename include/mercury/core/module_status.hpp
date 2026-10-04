#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mercury::core {

enum class ModuleState {
    planned,
    scaffolded,
    in_progress,
    validated,
};

struct ModuleStatus {
    std::string name;
    ModuleState state;
    std::string note;
};

std::string_view to_string(ModuleState state);

using ModuleStatuses = std::vector<ModuleStatus>;

}  // namespace mercury::core
