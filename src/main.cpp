#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <string>
#include <string_view>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "mercury/core/module_status.hpp"
#include "mercury/gamegear/game_gear.hpp"

namespace {

std::string hex8(std::uint8_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return out.str();
}

std::string hex16(std::uint16_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << value;
    return out.str();
}

int parse_trace_steps(int argc, char* argv[]) {
    if (argc <= 2) {
        return 32;
    }

    try {
        return std::max(1, std::stoi(argv[2]));
    } catch (...) {
        return 32;
    }
}

bool try_export_visible_frame(mercury::gamegear::GameGear& game_gear,
                              const std::string_view file_name,
                              const std::string_view label) {
    std::vector<std::filesystem::path> candidate_paths;
    candidate_paths.push_back(std::filesystem::current_path() / file_name);
    try {
        candidate_paths.push_back(std::filesystem::temp_directory_path() / file_name);
    } catch (const std::exception&) {
    }

    for (const auto& candidate_path : candidate_paths) {
        if (game_gear.vdp().export_visible_frame_bmp(candidate_path)) {
            std::cout << label << ": " << candidate_path.string() << "\n";
            return true;
        }
    }

    std::cout << label << ": export failed in all candidate locations\n";
    return false;
}

using VdpExportFn = bool (mercury::gamegear::Vdp::*)(const std::filesystem::path&) const;

bool try_export_vdp_bmp(const mercury::gamegear::Vdp& vdp,
                        VdpExportFn export_fn,
                        const std::string_view file_name,
                        const std::string_view label) {
    std::vector<std::filesystem::path> candidate_paths;
    candidate_paths.push_back(std::filesystem::current_path() / file_name);
    try {
        candidate_paths.push_back(std::filesystem::temp_directory_path() / file_name);
    } catch (const std::exception&) {
    }

    for (const auto& candidate_path : candidate_paths) {
        if ((vdp.*export_fn)(candidate_path)) {
            std::cout << label << ": " << candidate_path.string() << "\n";
            return true;
        }
    }

    std::cout << label << ": export failed in all candidate locations\n";
    return false;
}

bool try_export_pixels_as_visible_frame(const std::vector<std::uint8_t>& pixels,
                                        const std::string_view file_name,
                                        const std::string_view label);

void export_visible_frame_layers(mercury::gamegear::GameGear& game_gear,
                                 const std::string_view base_file_name,
                                 const std::string_view base_label) {
    const auto base_name = std::string(base_file_name);
    const auto dot_index = base_name.find_last_of('.');
    const auto stem = dot_index == std::string::npos ? base_name : base_name.substr(0, dot_index);
    const auto extension = dot_index == std::string::npos ? std::string(".bmp") : base_name.substr(dot_index);

    const auto background_pixels = game_gear.vdp().visible_background_pixels(false);
    try_export_pixels_as_visible_frame(
        background_pixels,
        stem + "_background" + extension,
        std::string(base_label) + " background-only");

    const auto sprite_pixels = game_gear.vdp().visible_sprite_pixels(false);
    try_export_pixels_as_visible_frame(
        sprite_pixels,
        stem + "_sprites" + extension,
        std::string(base_label) + " sprites-only");
}

bool try_export_pixels_as_visible_frame(const std::vector<std::uint8_t>& pixels,
                                        const std::string_view file_name,
                                        const std::string_view label) {
    std::vector<std::filesystem::path> candidate_paths;
    candidate_paths.push_back(std::filesystem::current_path() / file_name);
    try {
        candidate_paths.push_back(std::filesystem::temp_directory_path() / file_name);
    } catch (const std::exception&) {
    }

    for (const auto& candidate_path : candidate_paths) {
        std::ofstream out(candidate_path, std::ios::binary);
        if (!out) {
            continue;
        }

        const auto image_width = mercury::gamegear::Vdp::kVisibleWidth;
        const auto image_height = mercury::gamegear::Vdp::kVisibleHeight;
        const std::uint32_t row_stride =
            static_cast<std::uint32_t>((image_width * 3 + 3) & ~std::size_t(3));
        const std::uint32_t pixel_data_size = row_stride * static_cast<std::uint32_t>(image_height);
        const std::uint32_t file_size = 14 + 40 + pixel_data_size;

        const auto write_u16 = [&out](std::uint16_t value) {
            out.put(static_cast<char>(value & 0xFF));
            out.put(static_cast<char>((value >> 8) & 0xFF));
        };
        const auto write_u32 = [&out](std::uint32_t value) {
            out.put(static_cast<char>(value & 0xFF));
            out.put(static_cast<char>((value >> 8) & 0xFF));
            out.put(static_cast<char>((value >> 16) & 0xFF));
            out.put(static_cast<char>((value >> 24) & 0xFF));
        };

        out.put('B');
        out.put('M');
        write_u32(file_size);
        write_u16(0);
        write_u16(0);
        write_u32(14 + 40);

        write_u32(40);
        write_u32(static_cast<std::uint32_t>(image_width));
        write_u32(static_cast<std::uint32_t>(image_height));
        write_u16(1);
        write_u16(24);
        write_u32(0);
        write_u32(pixel_data_size);
        write_u32(2835);
        write_u32(2835);
        write_u32(0);
        write_u32(0);

        std::vector<std::uint8_t> row(row_stride, 0x00);
        for (std::size_t y = image_height; y-- > 0;) {
            for (std::size_t x = 0; x < image_width; ++x) {
                const auto src = (y * image_width + x) * 3;
                const auto dst = x * 3;
                row[dst + 0] = pixels[src + 2];
                row[dst + 1] = pixels[src + 1];
                row[dst + 2] = pixels[src + 0];
            }
            out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        }

        if (out.good()) {
            std::cout << label << ": " << candidate_path.string() << "\n";
            return true;
        }
    }

    std::cout << label << ": export failed in all candidate locations\n";
    return false;
}

std::uint64_t frame_signature(const std::vector<std::uint8_t>& pixels) {
    constexpr std::uint64_t kOffsetBasis = 1469598103934665603ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;

    std::uint64_t hash = kOffsetBasis;
    for (const auto value : pixels) {
        hash ^= static_cast<std::uint64_t>(value);
        hash *= kPrime;
    }
    return hash;
}

bool try_export_wav(const std::vector<std::int16_t>& samples,
                    std::uint32_t sample_rate,
                    const std::string_view file_name,
                    const std::string_view label) {
    std::vector<std::filesystem::path> candidate_paths;
    candidate_paths.push_back(std::filesystem::current_path() / file_name);
    try {
        candidate_paths.push_back(std::filesystem::temp_directory_path() / file_name);
    } catch (const std::exception&) {
    }

    for (const auto& candidate_path : candidate_paths) {
        std::ofstream out(candidate_path, std::ios::binary);
        if (!out) {
            continue;
        }

        std::vector<std::int16_t> normalized_samples = samples;
        std::int16_t peak = 0;
        for (const auto sample : normalized_samples) {
            peak = std::max<std::int16_t>(peak, static_cast<std::int16_t>(std::abs(sample)));
        }
        if (peak > 0) {
            const double scale = 30000.0 / static_cast<double>(peak);
            for (auto& sample : normalized_samples) {
                sample = static_cast<std::int16_t>(std::clamp(
                    static_cast<int>(std::lround(static_cast<double>(sample) * scale)),
                    -32768,
                    32767));
            }
        }

        const std::uint32_t data_size =
            static_cast<std::uint32_t>(normalized_samples.size() * sizeof(std::int16_t));
        const std::uint32_t file_size = 36 + data_size;
        const std::uint16_t channel_count = 1;
        const std::uint16_t bits_per_sample = 16;
        const std::uint32_t byte_rate = sample_rate * channel_count * (bits_per_sample / 8);
        const std::uint16_t block_align = channel_count * (bits_per_sample / 8);

        const auto write_u16 = [&out](std::uint16_t value) {
            out.put(static_cast<char>(value & 0xFF));
            out.put(static_cast<char>((value >> 8) & 0xFF));
        };
        const auto write_u32 = [&out](std::uint32_t value) {
            out.put(static_cast<char>(value & 0xFF));
            out.put(static_cast<char>((value >> 8) & 0xFF));
            out.put(static_cast<char>((value >> 16) & 0xFF));
            out.put(static_cast<char>((value >> 24) & 0xFF));
        };

        out.write("RIFF", 4);
        write_u32(file_size);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        write_u32(16);
        write_u16(1);
        write_u16(channel_count);
        write_u32(sample_rate);
        write_u32(byte_rate);
        write_u16(block_align);
        write_u16(bits_per_sample);
        out.write("data", 4);
        write_u32(data_size);
        out.write(
            reinterpret_cast<const char*>(normalized_samples.data()),
            static_cast<std::streamsize>(data_size));

        if (out.good()) {
            std::cout << label << ": " << candidate_path.string()
                      << " (" << normalized_samples.size() << " samples, peak " << peak << ")\n";
            return true;
        }
    }

    std::cout << label << ": export failed in all candidate locations\n";
    return false;
}

bool try_export_text_lines(const std::vector<std::string>& lines,
                           const std::string_view file_name,
                           const std::string_view label) {
    std::vector<std::filesystem::path> candidate_paths;
    candidate_paths.push_back(std::filesystem::current_path() / file_name);
    try {
        candidate_paths.push_back(std::filesystem::temp_directory_path() / file_name);
    } catch (const std::exception&) {
    }

    for (const auto& candidate_path : candidate_paths) {
        std::ofstream out(candidate_path);
        if (!out) {
            continue;
        }
        for (const auto& line : lines) {
            out << line << '\n';
        }
        if (out.good()) {
            std::cout << label << ": " << candidate_path.string() << "\n";
            return true;
        }
    }

    std::cout << label << ": export failed in all candidate locations\n";
    return false;
}

bool wait_for_display_enabled(mercury::gamegear::GameGear& game_gear,
                              int max_steps,
                              const std::string_view stop_label) {
    if (game_gear.vdp().display_enabled()) {
        return true;
    }

    for (int step_index = 0; step_index < max_steps; ++step_index) {
        const auto step_result = game_gear.step();
        if (!step_result.executed || step_result.unimplemented) {
            std::cout << stop_label << " stopped at PC="
                      << hex16(step_result.pc_before)
                      << " because the opcode is not implemented yet.\n";
            return false;
        }
        if (game_gear.vdp().display_enabled()) {
            return true;
        }
    }

    std::cout << stop_label << " reached the step limit without enabling the display.\n";
    return false;
}

struct AudioCaptureRunResult {
    bool reached_target = false;
    std::string reason;
    std::uint16_t stop_pc = 0x0000;
};

struct StartupAudioCaptureOptions {
    struct ScriptedButtonWindow {
        mercury::gamegear::Button button = mercury::gamegear::Button::start;
        std::uint64_t start_frame = 0;
        std::uint64_t end_frame = 0;
    };

    std::string mode_name;
    std::string file_prefix;
    bool auto_press_start = false;
    std::uint64_t auto_start_frame = 430;
    std::vector<ScriptedButtonWindow> button_windows;
};

struct PsgWriteSiteStats {
    std::uint64_t writes = 0;
    std::uint64_t hits = 0;
    mercury::core::Z80StepResult example_step{};
};

struct RoutineEntryStats {
    std::uint64_t hits = 0;
    mercury::core::Z80StepResult example_step{};
    std::array<std::size_t, 3> visible_banks{};
};

struct MapperSwitchStats {
    std::uint64_t hits = 0;
    mercury::core::Z80StepResult example_step{};
    std::array<std::size_t, 3> before_banks{};
    std::array<std::size_t, 3> after_banks{};
    std::array<std::uint8_t, 4> before_registers{};
    std::array<std::uint8_t, 4> after_registers{};
};

struct BankedCodeEntryStats {
    std::uint64_t hits = 0;
    mercury::core::Z80StepResult caller_step{};
    std::uint16_t target_pc = 0x0000;
    std::array<std::size_t, 3> visible_banks{};
};

struct TargetedBank3EntryStats {
    std::uint64_t hits = 0;
    std::uint16_t target_pc = 0x0000;
    mercury::core::Z80StepResult caller_step{};
    std::uint8_t a_before = 0x00;
};

struct FixedPcHitStats {
    std::uint64_t hits = 0;
    std::uint16_t pc = 0x0000;
    mercury::core::Z80StepResult example_step{};
};

struct Bank3ObjectPathStats {
    std::uint64_t hits = 0;
    std::uint16_t pc = 0x0000;
    mercury::core::Z80StepResult example_step{};
    std::uint16_t ix = 0x0000;
    std::uint8_t ix26_before = 0x00;
    std::uint8_t ix27_before = 0x00;
    std::uint8_t ix26_after = 0x00;
    std::uint8_t ix27_after = 0x00;
    std::uint16_t dc08_before = 0x0000;
    std::uint16_t dc08_after = 0x0000;
};

struct WatchedRamStats {
    std::uint16_t address = 0x0000;
    std::uint8_t width = 1;
    std::uint16_t initial_value = 0x0000;
    std::uint16_t final_value = 0x0000;
    std::uint64_t change_count = 0;
    std::unordered_map<std::uint16_t, std::uint64_t> change_sites{};
};

struct RestartCommandStats {
    std::uint64_t hits = 0;
    mercury::core::Z80StepResult example_step{};
    std::uint8_t a_before = 0x00;
};

std::string summarize_top_psg_write_sites(const std::unordered_map<std::uint16_t, PsgWriteSiteStats>& write_sites,
                                          std::size_t limit = 8) {
    if (write_sites.empty()) {
        return "none";
    }

    std::vector<std::pair<std::uint16_t, PsgWriteSiteStats>> sorted_sites(write_sites.begin(), write_sites.end());
    std::sort(sorted_sites.begin(), sorted_sites.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.writes != rhs.second.writes) {
            return lhs.second.writes > rhs.second.writes;
        }
        if (lhs.second.hits != rhs.second.hits) {
            return lhs.second.hits > rhs.second.hits;
        }
        return lhs.first < rhs.first;
    });

    std::ostringstream out;
    const auto count = std::min(limit, sorted_sites.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            out << "; ";
        }
        const auto& [pc, stats] = sorted_sites[index];
        out << hex16(pc) << " op=";
        if (stats.example_step.prefix != 0x00) {
            out << hex8(stats.example_step.prefix) << " ";
        }
        out << hex8(stats.example_step.opcode)
            << " writes=" << stats.writes
            << " hits=" << stats.hits
            << " msg=" << stats.example_step.message;
    }
    return out.str();
}

std::string summarize_top_routine_entry_sites(const std::unordered_map<std::uint16_t, RoutineEntryStats>& entry_sites,
                                              std::size_t limit = 8) {
    if (entry_sites.empty()) {
        return "none";
    }

    std::vector<std::pair<std::uint16_t, RoutineEntryStats>> sorted_sites(entry_sites.begin(), entry_sites.end());
    std::sort(sorted_sites.begin(), sorted_sites.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.hits != rhs.second.hits) {
            return lhs.second.hits > rhs.second.hits;
        }
        return lhs.first < rhs.first;
    });

    std::ostringstream out;
    const auto count = std::min(limit, sorted_sites.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            out << "; ";
        }
        const auto& [pc, stats] = sorted_sites[index];
        out << hex16(pc)
            << " hits=" << stats.hits
            << " next=" << hex16(stats.example_step.pc_after)
            << " banks=[" << stats.visible_banks[0]
            << "," << stats.visible_banks[1]
            << "," << stats.visible_banks[2]
            << "]"
            << " msg=" << stats.example_step.message;
    }
    return out.str();
}

std::string summarize_top_mapper_switch_sites(const std::unordered_map<std::uint16_t, MapperSwitchStats>& switch_sites,
                                              std::size_t limit = 8) {
    if (switch_sites.empty()) {
        return "none";
    }

    std::vector<std::pair<std::uint16_t, MapperSwitchStats>> sorted_sites(switch_sites.begin(), switch_sites.end());
    std::sort(sorted_sites.begin(), sorted_sites.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.hits != rhs.second.hits) {
            return lhs.second.hits > rhs.second.hits;
        }
        return lhs.first < rhs.first;
    });

    std::ostringstream out;
    const auto count = std::min(limit, sorted_sites.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            out << "; ";
        }
        const auto& [pc, stats] = sorted_sites[index];
        out << hex16(pc)
            << " hits=" << stats.hits
            << " regs=["
            << hex8(stats.before_registers[0]) << "," << hex8(stats.before_registers[1]) << ","
            << hex8(stats.before_registers[2]) << "," << hex8(stats.before_registers[3]) << "]->["
            << hex8(stats.after_registers[0]) << "," << hex8(stats.after_registers[1]) << ","
            << hex8(stats.after_registers[2]) << "," << hex8(stats.after_registers[3]) << "]"
            << " banks=[" << stats.before_banks[0] << "," << stats.before_banks[1] << "," << stats.before_banks[2]
            << "]->[" << stats.after_banks[0] << "," << stats.after_banks[1] << "," << stats.after_banks[2] << "]"
            << " msg=" << stats.example_step.message;
    }
    return out.str();
}

std::string summarize_top_banked_code_entry_sites(
    const std::unordered_map<std::uint32_t, BankedCodeEntryStats>& entry_sites,
    std::size_t limit = 8) {
    if (entry_sites.empty()) {
        return "none";
    }

    std::vector<std::pair<std::uint32_t, BankedCodeEntryStats>> sorted_sites(entry_sites.begin(), entry_sites.end());
    std::sort(sorted_sites.begin(), sorted_sites.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.hits != rhs.second.hits) {
            return lhs.second.hits > rhs.second.hits;
        }
        return lhs.first < rhs.first;
    });

    std::ostringstream out;
    const auto count = std::min(limit, sorted_sites.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            out << "; ";
        }
        const auto& [key, stats] = sorted_sites[index];
        out << "bank" << stats.visible_banks[1]
            << ":" << hex16(stats.target_pc)
            << " hits=" << stats.hits
            << " caller=" << hex16(stats.caller_step.pc_before)
            << " next=" << hex16(stats.caller_step.pc_after)
            << " msg=" << stats.caller_step.message;
    }
    return out.str();
}

std::string summarize_targeted_bank3_entry_sites(
    const std::array<TargetedBank3EntryStats, 6>& stats_by_slot) {
    std::ostringstream out;
    bool first = true;
    for (const auto& stats : stats_by_slot) {
        if (stats.target_pc == 0x0000) {
            continue;
        }
        if (!first) {
            out << "; ";
        }
        first = false;
        out << hex16(stats.target_pc)
            << " hits=" << stats.hits
            << " caller=" << hex16(stats.caller_step.pc_before)
            << " A=" << hex8(stats.a_before)
            << " msg=" << stats.caller_step.message;
    }
    if (first) {
        return "none";
    }
    return out.str();
}

std::string summarize_fixed_pc_hits(const std::array<FixedPcHitStats, 6>& stats_by_slot) {
    std::ostringstream out;
    bool first = true;
    for (const auto& stats : stats_by_slot) {
        if (stats.pc == 0x0000) {
            continue;
        }
        if (!first) {
            out << "; ";
        }
        first = false;
        out << hex16(stats.pc)
            << " hits=" << stats.hits
            << " msg=" << stats.example_step.message;
    }
    if (first) {
        return "none";
    }
    return out.str();
}

std::string summarize_bank3_object_path_hits(const std::array<Bank3ObjectPathStats, 6>& stats_by_slot) {
    std::ostringstream out;
    bool first = true;
    for (const auto& stats : stats_by_slot) {
        if (stats.pc == 0x0000) {
            continue;
        }
        if (!first) {
            out << "; ";
        }
        first = false;
        out << hex16(stats.pc)
            << " hits=" << stats.hits
            << " ix=" << hex16(stats.ix)
            << " ix26/27=" << hex8(stats.ix26_before) << "," << hex8(stats.ix27_before)
            << "->" << hex8(stats.ix26_after) << "," << hex8(stats.ix27_after)
            << " dc08=" << hex16(stats.dc08_before) << "->" << hex16(stats.dc08_after)
            << " msg=" << stats.example_step.message;
    }
    if (first) {
        return "none";
    }
    return out.str();
}

template <std::size_t N>
std::string summarize_watched_ram(const std::array<WatchedRamStats, N>& watched) {
    std::ostringstream out;
    bool first = true;
    for (const auto& stats : watched) {
        if (stats.address == 0x0000) {
            continue;
        }
        if (!first) {
            out << "; ";
        }
        first = false;

        std::vector<std::pair<std::uint16_t, std::uint64_t>> sorted_sites(
            stats.change_sites.begin(),
            stats.change_sites.end());
        std::sort(sorted_sites.begin(), sorted_sites.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
            }
            return lhs.first < rhs.first;
        });

        out << hex16(stats.address)
            << "=" << (stats.width == 2 ? hex16(stats.final_value) : hex8(static_cast<std::uint8_t>(stats.final_value)))
            << " changes=" << stats.change_count
            << " topSites=[";
        const auto count = std::min<std::size_t>(3, sorted_sites.size());
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0) {
                out << "; ";
            }
            out << hex16(sorted_sites[index].first) << ":" << sorted_sites[index].second;
        }
        out << "]";
    }
    if (first) {
        return "none";
    }
    return out.str();
}

std::string summarize_restart_command_sites(
    const std::unordered_map<std::uint32_t, RestartCommandStats>& command_sites,
    std::size_t limit = 10) {
    if (command_sites.empty()) {
        return "none";
    }

    std::vector<std::pair<std::uint32_t, RestartCommandStats>> sorted_sites(
        command_sites.begin(),
        command_sites.end());
    std::sort(sorted_sites.begin(), sorted_sites.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.hits != rhs.second.hits) {
            return lhs.second.hits > rhs.second.hits;
        }
        return lhs.first < rhs.first;
    });

    std::ostringstream out;
    bool first = true;
    for (std::size_t index = 0; index < sorted_sites.size() && index < limit; ++index) {
        if (!first) {
            out << "; ";
        }
        first = false;

        const auto key = sorted_sites[index].first;
        const auto rst_opcode = static_cast<std::uint8_t>((key >> 24) & 0xFF);
        const auto a_before = static_cast<std::uint8_t>((key >> 16) & 0xFF);
        const auto pc = static_cast<std::uint16_t>(key & 0xFFFF);
        const auto& stats = sorted_sites[index].second;
        out << "RST" << hex8(rst_opcode)
            << "@" << hex16(pc)
            << " A=" << hex8(a_before)
            << " hits=" << stats.hits
            << " msg=" << stats.example_step.message;
    }

    return out.str();
}

AudioCaptureRunResult run_until_audio_samples(mercury::gamegear::GameGear& game_gear,
                                              std::size_t target_sample_count,
                                              int max_steps,
                                              const std::string_view stop_label) {
    for (int step_index = 0; step_index < max_steps; ++step_index) {
        if (game_gear.psg().sample_buffer().size() >= target_sample_count) {
            return {
                .reached_target = true,
                .reason = std::string(stop_label) + " reached the target sample count.",
                .stop_pc = game_gear.cpu().registers().pc,
            };
        }

        const auto step_result = game_gear.step();
        if (!step_result.executed || step_result.unimplemented) {
            std::ostringstream reason;
            reason << stop_label << " stopped at PC="
                   << hex16(step_result.pc_before)
                   << " with OP=";
            if (step_result.prefix != 0x00) {
                reason << hex8(step_result.prefix) << " ";
            }
            reason << hex8(step_result.opcode)
                   << " because the opcode is not implemented yet";
            if (!step_result.message.empty()) {
                reason << " (" << step_result.message << ")";
            }
            reason << ".";
            std::cout << reason.str() << "\n";
            return {
                .reached_target = false,
                .reason = reason.str(),
                .stop_pc = step_result.pc_before,
            };
        }
    }

    if (game_gear.psg().sample_buffer().size() >= target_sample_count) {
        return {
            .reached_target = true,
            .reason = std::string(stop_label) + " reached the target sample count at the step limit.",
            .stop_pc = game_gear.cpu().registers().pc,
        };
    }

    std::ostringstream reason;
    reason << stop_label << " reached the step limit before enough audio samples were produced.";
    std::cout << reason.str() << "\n";
    return {
        .reached_target = false,
        .reason = reason.str(),
        .stop_pc = game_gear.cpu().registers().pc,
    };
}

void export_frame_sequence(mercury::gamegear::GameGear& game_gear,
                           int frame_count,
                           const std::string_view file_prefix,
                           const std::string_view label_prefix) {
    constexpr int kStepsPerFrameProbeLimit = 500'000;

    for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
        if (!game_gear.vdp().display_enabled()) {
            std::cout << label_prefix << " stopped because display output is now off at frame "
                      << frame_index << ".\n";
            return;
        }

        std::ostringstream file_name;
        file_name << file_prefix << "_" << std::setw(2) << std::setfill('0') << frame_index << ".bmp";
        std::ostringstream label;
        label << label_prefix << " " << frame_index;
        try_export_visible_frame(game_gear, file_name.str(), label.str());

        const auto starting_frame = game_gear.vdp().frames_elapsed();
        int step_index = 0;
        for (; step_index < kStepsPerFrameProbeLimit; ++step_index) {
            const auto step_result = game_gear.step();
            if (!step_result.executed || step_result.unimplemented) {
                std::cout << "Frame sequence stopped at PC="
                          << hex16(step_result.pc_before)
                          << " because the opcode is not implemented yet.\n";
                return;
            }
            if (game_gear.vdp().frames_elapsed() != starting_frame) {
                break;
            }
        }

        if (step_index >= kStepsPerFrameProbeLimit) {
            std::cout << "Frame sequence stopped because a frame boundary was not reached in time.\n";
            return;
        }
    }
}

void export_startup_sequence_from_reset(const std::filesystem::path& rom_path) {
    constexpr int kDisplayProbeMaxSteps = 2'000'000;
    constexpr int kMaxFramesToObserve = 300;
    constexpr int kStableFrameThreshold = 12;

    mercury::gamegear::GameGear startup_capture;
    const auto load_result = startup_capture.load_rom(rom_path);
    if (!load_result.ok) {
        std::cout << "Startup sequence load failed: " << load_result.error << "\n";
        return;
    }

    if (!wait_for_display_enabled(
            startup_capture,
            kDisplayProbeMaxSteps,
            "Startup display-on probe")) {
        return;
    }

    std::cout << "Startup display-on CPU: " << startup_capture.cpu().describe() << "\n";
    std::cout << startup_capture.vdp().activity_summary() << "\n";
    try_export_visible_frame(
        startup_capture,
        "mercury_vdp_startup_display_on.bmp",
        "VDP startup display-on");

    std::vector<std::uint8_t> previous_pixels;
    std::uint64_t previous_signature = 0;
    int exported_keyframes = 0;
    int unchanged_frame_streak = 0;

    for (int frame_index = 0; frame_index < kMaxFramesToObserve; ++frame_index) {
        if (!startup_capture.vdp().display_enabled()) {
            std::cout << "Startup progression stopped because display output turned off at frame "
                      << frame_index << ".\n";
            break;
        }

        const auto pixels = startup_capture.vdp().visible_frame_pixels(false);
        const auto signature = frame_signature(pixels);
        const bool changed = frame_index == 0 || signature != previous_signature;

        if (changed) {
            std::ostringstream file_name;
            file_name << "mercury_vdp_startup_keyframe_" << std::setw(3) << std::setfill('0')
                      << exported_keyframes << ".bmp";
            std::ostringstream label;
            label << "VDP startup keyframe " << exported_keyframes
                  << " (frame " << frame_index << ")";
            try_export_pixels_as_visible_frame(pixels, file_name.str(), label.str());
            ++exported_keyframes;
            unchanged_frame_streak = 0;
            previous_signature = signature;
            previous_pixels = pixels;
        } else {
            ++unchanged_frame_streak;
        }

        if (unchanged_frame_streak >= kStableFrameThreshold) {
            std::cout << "Startup progression stabilized after frame " << frame_index
                      << " with " << exported_keyframes << " exported keyframes.\n";
            break;
        }

        const auto starting_frame = startup_capture.vdp().frames_elapsed();
        constexpr int kStepsPerFrameProbeLimit = 500'000;
        int step_index = 0;
        for (; step_index < kStepsPerFrameProbeLimit; ++step_index) {
            const auto step_result = startup_capture.step();
            if (!step_result.executed || step_result.unimplemented) {
                std::cout << "Startup progression stopped at PC="
                          << hex16(step_result.pc_before)
                          << " because the opcode is not implemented yet.\n";
                return;
            }
            if (startup_capture.vdp().frames_elapsed() != starting_frame) {
                break;
            }
        }

        if (step_index >= kStepsPerFrameProbeLimit) {
            std::cout << "Startup progression stopped because a frame boundary was not reached in time.\n";
            return;
        }

        if (frame_index == kMaxFramesToObserve - 1) {
            std::cout << "Startup progression observed " << kMaxFramesToObserve
                      << " frames and exported " << exported_keyframes << " keyframes.\n";
        }
    }
}

std::string export_startup_audio_from_reset(const std::filesystem::path& rom_path,
                                            const StartupAudioCaptureOptions& options) {
    constexpr int kAudioCaptureMaxSteps = 100'000'000;
    constexpr double kAudioSeconds = 20.0;
    constexpr int kMaxDisplayCycleExports = 8;
    constexpr int kLongRunFrameExportInterval = 60;
    constexpr int kMaxLongRunFrameExports = 24;

    mercury::gamegear::GameGear startup_audio_capture;
    const auto load_result = startup_audio_capture.load_rom(rom_path);
    if (!load_result.ok) {
        const auto message = options.mode_name + " audio load failed: " + load_result.error;
        std::cout << message << "\n";
        return message;
    }

    const auto target_sample_count = static_cast<std::size_t>(
        mercury::gamegear::Psg::kDefaultSampleRate * kAudioSeconds);
    AudioCaptureRunResult run_result{};
    bool previous_display_enabled = startup_audio_capture.vdp().display_enabled();
    std::uint8_t previous_vdp_reg1 = startup_audio_capture.vdp().register_value(1);
    bool auto_start_triggered = false;
    bool auto_start_active = false;
    std::uint64_t auto_start_release_frame = 0;
    int display_on_cycle_count = 0;
    std::uint64_t previous_frame_count = startup_audio_capture.vdp().frames_elapsed();
    int long_run_frame_exports = 0;
    std::unordered_map<std::uint16_t, PsgWriteSiteStats> psg_write_sites;
    std::unordered_map<std::uint16_t, PsgWriteSiteStats> vdp_data_write_sites;
    std::unordered_map<std::uint16_t, PsgWriteSiteStats> vdp_control_write_sites;
    std::unordered_map<std::uint16_t, int> focused_vdp_upload_log_counts;
    std::unordered_map<std::uint16_t, int> stage_handoff_log_counts;
    std::unordered_map<std::uint16_t, int> post_stage_wait_log_counts;
    std::unordered_map<std::uint16_t, RoutineEntryStats> chant_entry_sites;
    std::unordered_map<std::uint16_t, RoutineEntryStats> chant_launcher_entry_sites;
    std::unordered_map<std::uint16_t, RoutineEntryStats> title_init_entry_sites;
    std::unordered_map<std::uint16_t, MapperSwitchStats> mapper_switch_sites;
    std::unordered_map<std::uint32_t, BankedCodeEntryStats> banked_code_entry_sites;
    std::unordered_map<std::uint32_t, RestartCommandStats> restart_command_sites;
    std::array<TargetedBank3EntryStats, 6> targeted_bank3_entries{};
    std::array<FixedPcHitStats, 6> targeted_fixed_pc_hits{};
    std::array<Bank3ObjectPathStats, 6> bank3_object_path_hits{};
    std::array<WatchedRamStats, 19> watched_ram{{
        {.address = 0xD203, .width = 1},
        {.address = 0xD20F, .width = 1},
        {.address = 0xD210, .width = 1},
        {.address = 0xD211, .width = 2},
        {.address = 0xD213, .width = 2},
        {.address = 0xD215, .width = 2},
        {.address = 0xD217, .width = 2},
        {.address = 0xD21A, .width = 1},
        {.address = 0xD22C, .width = 2},
        {.address = 0xD236, .width = 2},
        {.address = 0xD238, .width = 1},
        {.address = 0xD240, .width = 1},
        {.address = 0xD241, .width = 1},
        {.address = 0xD242, .width = 1},
        {.address = 0xDC08, .width = 2},
        {.address = 0xDC0A, .width = 2},
        {.address = 0xDC48, .width = 2},
        {.address = 0xDC4C, .width = 2},
        {.address = 0xDC4E, .width = 1},
    }};
    mercury::core::Z80StepResult previous_step_result{};
    bool have_previous_step = false;
    bool previous_pc_in_chant_routine = false;
    bool previous_pc_in_chant_launcher_routine = false;
    bool previous_pc_in_title_init_routine = false;
    mercury::core::Z80StepResult current_chant_launcher_entry_step{};
    bool have_chant_launcher_entry_step = false;
    auto previous_mapper_registers = startup_audio_capture.cartridge().mapper_registers();
    auto previous_visible_banks = startup_audio_capture.cartridge().visible_banks();
    std::vector<std::string> startup_state_log;
    startup_state_log.reserve(1200);
    std::vector<std::string> startup_title_gate_log;
    startup_title_gate_log.reserve(1200);
    std::vector<std::string> startup_stage_handoff_log;
    startup_stage_handoff_log.reserve(1600);
    std::vector<std::string> startup_stack_restore_log;
    startup_stack_restore_log.reserve(256);
    std::vector<std::string> startup_stack_slot_log;
    startup_stack_slot_log.reserve(512);
    std::vector<std::string> startup_post_stage_script_log;
    startup_post_stage_script_log.reserve(2400);
    std::vector<std::string> startup_post_stage_object_log;
    startup_post_stage_object_log.reserve(512);
    std::vector<std::string> startup_post_stage_wait_log;
    startup_post_stage_wait_log.reserve(512);
    std::vector<std::string> startup_stage_branch_log;
    startup_stage_branch_log.reserve(256);
    std::vector<std::string> startup_bank3_object_trace_log;
    startup_bank3_object_trace_log.reserve(256);
    std::vector<std::string> startup_iy_change_log;
    startup_iy_change_log.reserve(512);
    std::vector<std::string> startup_iy_focus_log;
    startup_iy_focus_log.reserve(512);
    std::vector<std::string> startup_display_transition_log;
    startup_display_transition_log.reserve(256);
    std::vector<std::string> startup_late_display_transition_log;
    startup_late_display_transition_log.reserve(512);
    const auto read_watched_value = [&startup_audio_capture](const WatchedRamStats& stats) {
        const auto low = static_cast<std::uint16_t>(startup_audio_capture.bus().read8(stats.address));
        if (stats.width == 2) {
            const auto high = static_cast<std::uint16_t>(
                startup_audio_capture.bus().read8(static_cast<std::uint16_t>(stats.address + 1)));
            return static_cast<std::uint16_t>(low | (high << 8));
        }
        return low;
    };
    constexpr std::uint16_t kPostStageObjectBase = 0x2600;
    const auto read_post_stage_object_byte = [&startup_audio_capture](std::uint16_t offset) {
        return startup_audio_capture.bus().read8(
            static_cast<std::uint16_t>(kPostStageObjectBase + offset));
    };
    const auto read16_at = [&startup_audio_capture](std::uint16_t address) {
        const auto low = static_cast<std::uint16_t>(startup_audio_capture.bus().read8(address));
        const auto high = static_cast<std::uint16_t>(
            startup_audio_capture.bus().read8(static_cast<std::uint16_t>(address + 1)));
        return static_cast<std::uint16_t>(low | (high << 8));
    };
    const auto append_state_log = [&](const std::string_view tag,
                                      const mercury::core::Z80StepResult& current_step,
                                      const mercury::core::Z80Registers& registers_before) {
        if (startup_state_log.size() >= 1200) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        std::ostringstream line;
        const auto bc = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.b) << 8) | registers_before.c);
        const auto de = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.d) << 8) | registers_before.e);
        const auto hl = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.h) << 8) | registers_before.l);
          line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
               << " pc=" << hex16(current_step.pc_before)
               << " next=" << hex16(current_step.pc_after)
               << " a=" << hex8(registers_before.a)
             << " bc=" << hex16(bc)
             << " de=" << hex16(de)
             << " hl=" << hex16(hl)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " tag=" << tag
             << " msg=" << current_step.message
             << " d203=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[0])))
             << " d20f=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[1])))
             << " d210=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[2])))
             << " d211=" << hex16(read_watched_value(watched_ram[3]))
             << " d213=" << hex16(read_watched_value(watched_ram[4]))
             << " d215=" << hex16(read_watched_value(watched_ram[5]))
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " d22c=" << hex16(read_watched_value(watched_ram[8]))
             << " d236=" << hex16(read_watched_value(watched_ram[9]))
             << " d238=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[10])))
             << " d240=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[11])))
             << " d241=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[12])))
             << " d242=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[13])))
             << " vdpcode=" << hex8(startup_audio_capture.vdp().debug_code())
             << " vdpaddr=" << hex16(startup_audio_capture.vdp().debug_current_address())
               << " lastreg=#" << std::dec << static_cast<int>(startup_audio_capture.vdp().last_register_index())
               << "=" << hex8(startup_audio_capture.vdp().last_register_value())
               << " disp=" << (startup_audio_capture.vdp().display_enabled() ? "on" : "off");
          startup_state_log.push_back(line.str());
      };
    const auto append_title_gate_log = [&](const std::string_view tag,
                                           const mercury::core::Z80StepResult& current_step,
                                           const mercury::core::Z80Registers& registers_before) {
        if (startup_title_gate_log.size() >= 1200) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        const auto iy = registers_before.iy;
        const auto sp = registers_before.sp;
        const auto iy0 = startup_audio_capture.bus().read8(iy);
        const auto iy6 = startup_audio_capture.bus().read8(static_cast<std::uint16_t>(iy + 6));
        const auto iy7 = startup_audio_capture.bus().read8(static_cast<std::uint16_t>(iy + 7));
        const auto iy3 = startup_audio_capture.bus().read8(static_cast<std::uint16_t>(iy + 3));
        const auto iya = startup_audio_capture.bus().read8(static_cast<std::uint16_t>(iy + 0x0A));
        const auto d2dc = startup_audio_capture.bus().read8(0xD2DC);
        const auto stack_low = startup_audio_capture.bus().read8(sp);
        const auto stack_high =
            startup_audio_capture.bus().read8(static_cast<std::uint16_t>(sp + 1));
        const auto stack_word =
            static_cast<std::uint16_t>((stack_high << 8) | stack_low);
        const auto next_stack_low =
            startup_audio_capture.bus().read8(static_cast<std::uint16_t>(sp + 2));
        const auto next_stack_high =
            startup_audio_capture.bus().read8(static_cast<std::uint16_t>(sp + 3));
        const auto next_stack_word =
            static_cast<std::uint16_t>((next_stack_high << 8) | next_stack_low);
        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " a=" << hex8(registers_before.a)
             << " sp=" << hex16(sp)
             << " tos=" << hex16(stack_word)
             << " tos2=" << hex16(next_stack_word)
             << " iy=" << hex16(iy)
             << " iy0=" << hex8(iy0)
             << " iy6=" << hex8(iy6)
             << " iy7=" << hex8(iy7)
             << " iy3=" << hex8(iy3)
             << " iya=" << hex8(iya)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " tag=" << tag
             << " msg=" << current_step.message
             << " d203=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[0])))
             << " d20f=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[1])))
             << " d210=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[2])))
             << " d211=" << hex16(read_watched_value(watched_ram[3]))
             << " d213=" << hex16(read_watched_value(watched_ram[4]))
             << " d215=" << hex16(read_watched_value(watched_ram[5]))
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " d22c=" << hex16(read_watched_value(watched_ram[8]))
             << " d236=" << hex16(read_watched_value(watched_ram[9]))
             << " d238=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[10])))
             << " d240=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[11])))
             << " d241=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[12])))
             << " d242=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[13])))
               << " d2dc=" << hex8(d2dc)
               << " lastreg=#" << std::dec << static_cast<int>(startup_audio_capture.vdp().last_register_index())
               << "=" << hex8(startup_audio_capture.vdp().last_register_value())
               << " disp=" << (startup_audio_capture.vdp().display_enabled() ? "on" : "off");
          startup_title_gate_log.push_back(line.str());
      };
    const auto append_stage_handoff_log = [&](const std::string_view tag,
                                              const mercury::core::Z80StepResult& current_step,
                                              const mercury::core::Z80Registers& registers_before) {
        if (startup_stage_handoff_log.size() >= 1600) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        const auto iy = registers_before.iy;
        const auto sp = registers_before.sp;
        const auto iy0 = startup_audio_capture.bus().read8(iy);
        const auto iy3 = startup_audio_capture.bus().read8(static_cast<std::uint16_t>(iy + 3));
        const auto iya = startup_audio_capture.bus().read8(static_cast<std::uint16_t>(iy + 0x0A));
        const auto stack_low = startup_audio_capture.bus().read8(sp);
        const auto stack_high =
            startup_audio_capture.bus().read8(static_cast<std::uint16_t>(sp + 1));
        const auto stack_word =
            static_cast<std::uint16_t>((stack_high << 8) | stack_low);
        const auto bc = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.b) << 8) | registers_before.c);
        const auto de = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.d) << 8) | registers_before.e);
        const auto hl = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.h) << 8) | registers_before.l);
        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " a=" << hex8(registers_before.a)
             << " bc=" << hex16(bc)
             << " de=" << hex16(de)
             << " hl=" << hex16(hl)
             << " ix=" << hex16(registers_before.ix)
             << " sp=" << hex16(sp)
             << " tos=" << hex16(stack_word)
             << " iy=" << hex16(iy)
             << " iy0=" << hex8(iy0)
             << " iy3=" << hex8(iy3)
             << " iya=" << hex8(iya)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " tag=" << tag
             << " msg=" << current_step.message
             << " d20f=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[1])))
             << " d210=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[2])))
             << " d211=" << hex16(read_watched_value(watched_ram[3]))
             << " d213=" << hex16(read_watched_value(watched_ram[4]))
             << " d215=" << hex16(read_watched_value(watched_ram[5]))
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " d22c=" << hex16(read_watched_value(watched_ram[8]))
             << " d236=" << hex16(read_watched_value(watched_ram[9]))
             << " d238=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[10])))
             << " d240=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[11])))
             << " vdpcode=" << hex8(startup_audio_capture.vdp().debug_code())
             << " vdpaddr=" << hex16(startup_audio_capture.vdp().debug_current_address())
             << " lastreg=#" << std::dec << static_cast<int>(startup_audio_capture.vdp().last_register_index())
             << "=" << hex8(startup_audio_capture.vdp().last_register_value())
             << " disp=" << (startup_audio_capture.vdp().display_enabled() ? "on" : "off");
        startup_stage_handoff_log.push_back(line.str());
    };
    const auto append_stack_restore_log = [&](const std::string_view tag,
                                              const mercury::core::Z80StepResult& current_step,
                                              const mercury::core::Z80Registers& registers_before) {
        if (startup_stack_restore_log.size() >= 256) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        const auto sp = registers_before.sp;
        const auto tos = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(startup_audio_capture.bus().read8(sp)) |
            (static_cast<std::uint16_t>(
                 startup_audio_capture.bus().read8(static_cast<std::uint16_t>(sp + 1)))
             << 8));
        const auto tos2 = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(
                startup_audio_capture.bus().read8(static_cast<std::uint16_t>(sp + 2))) |
            (static_cast<std::uint16_t>(
                 startup_audio_capture.bus().read8(static_cast<std::uint16_t>(sp + 3)))
             << 8));
        const auto bc = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.b) << 8) | registers_before.c);
        const auto de = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.d) << 8) | registers_before.e);
        const auto hl = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.h) << 8) | registers_before.l);

        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " a=" << hex8(registers_before.a)
             << " bc=" << hex16(bc)
             << " de=" << hex16(de)
             << " hl=" << hex16(hl)
             << " ix=" << hex16(registers_before.ix)
             << " sp=" << hex16(sp)
             << " tos=" << hex16(tos)
             << " tos2=" << hex16(tos2)
             << " iy=" << hex16(registers_before.iy)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " tag=" << tag
             << " msg=" << current_step.message
             << " d20f=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[1])))
             << " d210=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[2])))
             << " d211=" << hex16(read_watched_value(watched_ram[3]))
             << " d213=" << hex16(read_watched_value(watched_ram[4]))
             << " d215=" << hex16(read_watched_value(watched_ram[5]))
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " disp=" << (startup_audio_capture.vdp().display_enabled() ? "on" : "off");
        startup_stack_restore_log.push_back(line.str());
    };
    const std::array<std::uint16_t, 6> watched_stack_addresses{
        {0xDC11, 0xDC12, 0xDC13, 0xDC14, 0xDC15, 0xDC16}};
    std::array<std::uint8_t, 6> previous_watched_stack_bytes{};
    for (std::size_t index = 0; index < watched_stack_addresses.size(); ++index) {
        previous_watched_stack_bytes[index] = startup_audio_capture.bus().read8(watched_stack_addresses[index]);
    }
    const auto append_stack_slot_log = [&](const mercury::core::Z80StepResult& current_step,
                                           const mercury::core::Z80Registers& registers_before,
                                           const mercury::core::Z80Registers& registers_after,
                                           const std::array<std::uint8_t, 6>& before_bytes,
                                           const std::array<std::uint8_t, 6>& after_bytes) {
        if (startup_stack_slot_log.size() >= 512) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " sp=" << hex16(registers_before.sp) << "->" << hex16(registers_after.sp)
             << " ix=" << hex16(registers_before.ix) << "->" << hex16(registers_after.ix)
             << " iy=" << hex16(registers_before.iy) << "->" << hex16(registers_after.iy)
             << " msg=" << current_step.message
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " dc11..16="
             << hex8(before_bytes[0]) << hex8(before_bytes[1]) << hex8(before_bytes[2])
             << hex8(before_bytes[3]) << hex8(before_bytes[4]) << hex8(before_bytes[5])
             << "->"
             << hex8(after_bytes[0]) << hex8(after_bytes[1]) << hex8(after_bytes[2])
             << hex8(after_bytes[3]) << hex8(after_bytes[4]) << hex8(after_bytes[5])
             << " tos=" << hex16(static_cast<std::uint16_t>(before_bytes[0] |
                                                            (static_cast<std::uint16_t>(before_bytes[1]) << 8)))
             << " tos2=" << hex16(static_cast<std::uint16_t>(before_bytes[2] |
                                                             (static_cast<std::uint16_t>(before_bytes[3]) << 8)))
             << " tos3=" << hex16(static_cast<std::uint16_t>(before_bytes[4] |
                                                             (static_cast<std::uint16_t>(before_bytes[5]) << 8)));
        startup_stack_slot_log.push_back(line.str());
    };
    const auto append_post_stage_script_log = [&](const std::string_view tag,
                                                  const mercury::core::Z80StepResult& current_step,
                                                  const mercury::core::Z80Registers& registers_before) {
        if (startup_post_stage_script_log.size() >= 2400) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        const auto bc = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.b) << 8) | registers_before.c);
        const auto de = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.d) << 8) | registers_before.e);
        const auto hl = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.h) << 8) | registers_before.l);
        const auto iy0 = startup_audio_capture.bus().read8(registers_before.iy);
        const auto iy5 = startup_audio_capture.bus().read8(
            static_cast<std::uint16_t>(registers_before.iy + 5));
        const auto iy6 = startup_audio_capture.bus().read8(
            static_cast<std::uint16_t>(registers_before.iy + 6));
        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " a=" << hex8(registers_before.a)
             << " bc=" << hex16(bc)
             << " de=" << hex16(de)
             << " hl=" << hex16(hl)
             << " sp=" << hex16(registers_before.sp)
             << " iy=" << hex16(registers_before.iy)
             << " iy0=" << hex8(iy0)
             << " iy5=" << hex8(iy5)
             << " iy6=" << hex8(iy6)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " tag=" << tag
             << " msg=" << current_step.message
             << " d20f=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[1])))
             << " d210=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[2])))
             << " d211=" << hex16(read_watched_value(watched_ram[3]))
             << " d213=" << hex16(read_watched_value(watched_ram[4]))
             << " d215=" << hex16(read_watched_value(watched_ram[5]))
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " d22c=" << hex16(read_watched_value(watched_ram[8]))
             << " d236=" << hex16(read_watched_value(watched_ram[9]))
             << " d238=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[10])))
             << " d240=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[11])))
             << " d241=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[12])))
             << " d242=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[13])))
             << " vdpcode=" << hex8(startup_audio_capture.vdp().debug_code())
             << " vdpaddr=" << hex16(startup_audio_capture.vdp().debug_current_address())
             << " lastreg=#" << std::dec
             << static_cast<int>(startup_audio_capture.vdp().last_register_index())
             << "=" << hex8(startup_audio_capture.vdp().last_register_value())
             << " disp=" << (startup_audio_capture.vdp().display_enabled() ? "on" : "off");
        startup_post_stage_script_log.push_back(line.str());
    };
    const auto append_post_stage_object_log = [&](const std::string_view tag,
                                                  const mercury::core::Z80StepResult& current_step,
                                                  const mercury::core::Z80Registers& registers_before,
                                                  const std::array<std::uint8_t, 3>& before_bytes,
                                                  const std::array<std::uint8_t, 3>& after_bytes) {
        if (startup_post_stage_object_log.size() >= 512) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " iy=" << hex16(registers_before.iy)
             << " a=" << hex8(registers_before.a)
             << " sp=" << hex16(registers_before.sp)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " tag=" << tag
             << " msg=" << current_step.message
             << " obj2600[0,5,6]="
             << hex8(before_bytes[0]) << "," << hex8(before_bytes[1]) << "," << hex8(before_bytes[2])
             << "->"
             << hex8(after_bytes[0]) << "," << hex8(after_bytes[1]) << "," << hex8(after_bytes[2])
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " d236=" << hex16(read_watched_value(watched_ram[9]))
             << " disp=" << (startup_audio_capture.vdp().display_enabled() ? "on" : "off");
        startup_post_stage_object_log.push_back(line.str());
    };
    const auto append_post_stage_wait_log = [&](const std::string_view tag,
                                                const mercury::core::Z80StepResult& current_step,
                                                const mercury::core::Z80Registers& registers_before,
                                                const mercury::core::Z80Registers& registers_after,
                                                const std::array<std::uint8_t, 4>& before_bytes,
                                                const std::array<std::uint8_t, 4>& after_bytes) {
        if (startup_post_stage_wait_log.size() >= 512) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " iy=" << hex16(registers_before.iy)
             << " iy_after=" << hex16(registers_after.iy)
             << " a=" << hex8(registers_before.a)
             << " f=" << hex8(registers_before.f)
             << " sp=" << hex16(registers_before.sp)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " tag=" << tag
             << " msg=" << current_step.message
             << " obj2600[0,3,5,6]="
             << hex8(before_bytes[0]) << "," << hex8(before_bytes[1]) << ","
             << hex8(before_bytes[2]) << "," << hex8(before_bytes[3])
             << "->"
             << hex8(after_bytes[0]) << "," << hex8(after_bytes[1]) << ","
             << hex8(after_bytes[2]) << "," << hex8(after_bytes[3])
             << " d20f=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[1])))
             << " d210=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[2])))
             << " d211=" << hex16(read_watched_value(watched_ram[3]))
             << " d213=" << hex16(read_watched_value(watched_ram[4]))
             << " d215=" << hex16(read_watched_value(watched_ram[5]))
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " d236=" << hex16(read_watched_value(watched_ram[9]))
             << " disp=" << (startup_audio_capture.vdp().display_enabled() ? "on" : "off");
        startup_post_stage_wait_log.push_back(line.str());
    };
    const auto append_bank3_object_trace_log = [&](const mercury::core::Z80StepResult& current_step,
                                                   const mercury::core::Z80Registers& registers_before) {
        if (startup_bank3_object_trace_log.size() >= 256) {
            return;
        }
        const auto banks = startup_audio_capture.cartridge().visible_banks();
        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " a=" << hex8(registers_before.a)
             << " bc=" << hex16(static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(registers_before.b) << 8) | registers_before.c))
             << " de=" << hex16(static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(registers_before.d) << 8) | registers_before.e))
             << " hl=" << hex16(static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(registers_before.h) << 8) | registers_before.l))
             << " ix=" << hex16(registers_before.ix)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " msg=" << current_step.message
             << " ix0d=" << hex8(startup_audio_capture.bus().read8(static_cast<std::uint16_t>(registers_before.ix + 0x0D)))
             << " ix22=" << hex8(startup_audio_capture.bus().read8(static_cast<std::uint16_t>(registers_before.ix + 0x22)))
             << " ix23=" << hex8(startup_audio_capture.bus().read8(static_cast<std::uint16_t>(registers_before.ix + 0x23)))
             << " ix24=" << hex8(startup_audio_capture.bus().read8(static_cast<std::uint16_t>(registers_before.ix + 0x24)))
             << " ix25=" << hex8(startup_audio_capture.bus().read8(static_cast<std::uint16_t>(registers_before.ix + 0x25)))
             << " ix26=" << hex8(startup_audio_capture.bus().read8(static_cast<std::uint16_t>(registers_before.ix + 0x26)))
             << " ix27=" << hex8(startup_audio_capture.bus().read8(static_cast<std::uint16_t>(registers_before.ix + 0x27)))
             << " ix28=" << hex8(startup_audio_capture.bus().read8(static_cast<std::uint16_t>(registers_before.ix + 0x28)))
             << " dc08=" << hex16(read16_at(0xDC08))
             << " dc0a=" << hex16(read16_at(0xDC0A));
        startup_bank3_object_trace_log.push_back(line.str());
    };
    const auto append_stage_branch_log = [&](const std::string_view tag,
                                             const mercury::core::Z80StepResult& current_step,
                                             const mercury::core::Z80Registers& registers_before) {
        if (startup_stage_branch_log.size() >= 256) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        const auto bc = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.b) << 8) | registers_before.c);
        const auto de = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.d) << 8) | registers_before.e);
        const auto hl = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.h) << 8) | registers_before.l);
        const auto carry = (registers_before.f & 0x01) != 0;
        const auto zero = (registers_before.f & 0x40) != 0;
        const auto sign = (registers_before.f & 0x80) != 0;
        const auto iy0 = startup_audio_capture.bus().read8(registers_before.iy);
        const auto iy3 = startup_audio_capture.bus().read8(
            static_cast<std::uint16_t>(registers_before.iy + 3));
        const auto iy5 = startup_audio_capture.bus().read8(
            static_cast<std::uint16_t>(registers_before.iy + 5));
        const auto iy6 = startup_audio_capture.bus().read8(
            static_cast<std::uint16_t>(registers_before.iy + 6));
        const auto ix26 = startup_audio_capture.bus().read8(
            static_cast<std::uint16_t>(registers_before.ix + 0x26));
        const auto ix27 = startup_audio_capture.bus().read8(
            static_cast<std::uint16_t>(registers_before.ix + 0x27));
        const auto ix28 = startup_audio_capture.bus().read8(
            static_cast<std::uint16_t>(registers_before.ix + 0x28));
        const auto dc08 =
            static_cast<std::uint16_t>(startup_audio_capture.bus().read8(0xDC08)) |
            (static_cast<std::uint16_t>(startup_audio_capture.bus().read8(0xDC09)) << 8);

        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " prefix=" << hex8(current_step.prefix)
             << " op=" << hex8(current_step.opcode)
             << " a=" << hex8(registers_before.a)
             << " f=" << hex8(registers_before.f)
             << " c=" << (carry ? 1 : 0)
             << " z=" << (zero ? 1 : 0)
             << " s=" << (sign ? 1 : 0)
             << " bc=" << hex16(bc)
             << " de=" << hex16(de)
             << " hl=" << hex16(hl)
             << " sp=" << hex16(registers_before.sp)
             << " ix=" << hex16(registers_before.ix)
             << " iy=" << hex16(registers_before.iy)
             << " iy0=" << hex8(iy0)
             << " iy3=" << hex8(iy3)
             << " iy5=" << hex8(iy5)
             << " iy6=" << hex8(iy6)
             << " ix26=" << hex8(ix26)
             << " ix27=" << hex8(ix27)
             << " ix28=" << hex8(ix28)
             << " dc08=" << hex16(dc08)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " tag=" << tag
             << " msg=" << current_step.message
             << " d20f=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[1])))
             << " d210=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[2])))
             << " d211=" << hex16(read_watched_value(watched_ram[3]))
             << " d213=" << hex16(read_watched_value(watched_ram[4]))
             << " d215=" << hex16(read_watched_value(watched_ram[5]))
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " disp=" << (startup_audio_capture.vdp().display_enabled() ? "on" : "off");
        startup_stage_branch_log.push_back(line.str());
    };
    const auto append_iy_log = [&](std::vector<std::string>& target_log,
                                   std::size_t max_entries,
                                   const std::string_view tag,
                                   const mercury::core::Z80StepResult& current_step,
                                   const mercury::core::Z80Registers& registers_before,
                                   const mercury::core::Z80Registers& registers_after) {
        if (target_log.size() >= max_entries) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        const auto sp_before = registers_before.sp;
        const auto sp_after = registers_after.sp;
        const auto stack_word_before = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(startup_audio_capture.bus().read8(sp_before)) |
            (static_cast<std::uint16_t>(
                 startup_audio_capture.bus().read8(static_cast<std::uint16_t>(sp_before + 1)))
             << 8));
        const auto stack_word_after = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(startup_audio_capture.bus().read8(sp_after)) |
            (static_cast<std::uint16_t>(
                 startup_audio_capture.bus().read8(static_cast<std::uint16_t>(sp_after + 1)))
             << 8));
        const auto bc_before = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.b) << 8) | registers_before.c);
        const auto de_before = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.d) << 8) | registers_before.e);
        const auto hl_before = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_before.h) << 8) | registers_before.l);
        const auto bc_after = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_after.b) << 8) | registers_after.c);
        const auto de_after = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_after.d) << 8) | registers_after.e);
        const auto hl_after = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_after.h) << 8) | registers_after.l);

        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " tag=" << tag
             << " msg=" << current_step.message
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " ix=" << hex16(registers_before.ix) << "->" << hex16(registers_after.ix)
             << " iy=" << hex16(registers_before.iy) << "->" << hex16(registers_after.iy)
             << " sp=" << hex16(sp_before) << "->" << hex16(sp_after)
             << " tos=" << hex16(stack_word_before) << "->" << hex16(stack_word_after)
             << " bc=" << hex16(bc_before) << "->" << hex16(bc_after)
             << " de=" << hex16(de_before) << "->" << hex16(de_after)
             << " hl=" << hex16(hl_before) << "->" << hex16(hl_after)
             << " d20f=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[1])))
             << " d210=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[2])))
             << " d211=" << hex16(read_watched_value(watched_ram[3]))
             << " d213=" << hex16(read_watched_value(watched_ram[4]))
             << " d215=" << hex16(read_watched_value(watched_ram[5]))
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " d22c=" << hex16(read_watched_value(watched_ram[8]))
             << " d236=" << hex16(read_watched_value(watched_ram[9]))
             << " d238=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[10])))
             << " d240=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[11])))
             << " d241=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[12])))
             << " d242=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[13])))
             << " disp=" << (startup_audio_capture.vdp().display_enabled() ? "on" : "off");
        target_log.push_back(line.str());
    };
    const auto append_display_transition_log = [&](
                                                  const std::string_view tag,
                                                  const mercury::core::Z80StepResult& current_step,
                                                  const mercury::core::Z80Registers& registers_before,
                                                  std::uint8_t reg1_before,
                                                  std::uint8_t reg1_after,
                                                  bool display_before,
                                                  bool display_after) {
        if (startup_display_transition_log.size() >= 256) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " a=" << hex8(registers_before.a)
             << " sp=" << hex16(registers_before.sp)
             << " iy=" << hex16(registers_before.iy)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " tag=" << tag
             << " msg=" << current_step.message
             << " reg1=" << hex8(reg1_before) << "->" << hex8(reg1_after)
             << " disp=" << (display_before ? "on" : "off")
             << "->" << (display_after ? "on" : "off")
             << " d20f=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[1])))
             << " d210=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[2])))
             << " d211=" << hex16(read_watched_value(watched_ram[3]))
             << " d213=" << hex16(read_watched_value(watched_ram[4]))
             << " d215=" << hex16(read_watched_value(watched_ram[5]))
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " d22c=" << hex16(read_watched_value(watched_ram[8]))
             << " d236=" << hex16(read_watched_value(watched_ram[9]))
             << " vdpcode=" << hex8(startup_audio_capture.vdp().debug_code())
             << " vdpaddr=" << hex16(startup_audio_capture.vdp().debug_current_address())
             << " lastreg=#" << std::dec
             << static_cast<int>(startup_audio_capture.vdp().last_register_index())
             << "=" << hex8(startup_audio_capture.vdp().last_register_value());
        startup_display_transition_log.push_back(line.str());
    };
    const auto append_late_display_transition_log = [&](
                                                         const std::string_view tag,
                                                         const mercury::core::Z80StepResult& current_step,
                                                         const mercury::core::Z80Registers& registers_before,
                                                         std::uint8_t reg1_before,
                                                         std::uint8_t reg1_after,
                                                         bool display_before,
                                                         bool display_after) {
        if (startup_late_display_transition_log.size() >= 512) {
            return;
        }

        const auto banks = startup_audio_capture.cartridge().visible_banks();
        std::ostringstream line;
        line << "frame=" << startup_audio_capture.vdp().frames_elapsed()
             << " pc=" << hex16(current_step.pc_before)
             << " next=" << hex16(current_step.pc_after)
             << " a=" << hex8(registers_before.a)
             << " bc=" << hex16(static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(registers_before.b) << 8) | registers_before.c))
             << " de=" << hex16(static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(registers_before.d) << 8) | registers_before.e))
             << " hl=" << hex16(static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(registers_before.h) << 8) | registers_before.l))
             << " sp=" << hex16(registers_before.sp)
             << " iy=" << hex16(registers_before.iy)
             << " banks=[" << banks[0] << "," << banks[1] << "," << banks[2] << "]"
             << " tag=" << tag
             << " msg=" << current_step.message
             << " reg1=" << hex8(reg1_before) << "->" << hex8(reg1_after)
             << " disp=" << (display_before ? "on" : "off")
             << "->" << (display_after ? "on" : "off")
             << " d20f=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[1])))
             << " d210=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[2])))
             << " d211=" << hex16(read_watched_value(watched_ram[3]))
             << " d213=" << hex16(read_watched_value(watched_ram[4]))
             << " d215=" << hex16(read_watched_value(watched_ram[5]))
             << " d217=" << hex16(read_watched_value(watched_ram[6]))
             << " d21a=" << hex8(static_cast<std::uint8_t>(read_watched_value(watched_ram[7])))
             << " d22c=" << hex16(read_watched_value(watched_ram[8]))
             << " d236=" << hex16(read_watched_value(watched_ram[9]))
             << " vdpcode=" << hex8(startup_audio_capture.vdp().debug_code())
             << " vdpaddr=" << hex16(startup_audio_capture.vdp().debug_current_address())
             << " lastreg=#" << std::dec
             << static_cast<int>(startup_audio_capture.vdp().last_register_index())
             << "=" << hex8(startup_audio_capture.vdp().last_register_value());
        startup_late_display_transition_log.push_back(line.str());
    };

    std::array<std::uint16_t, std::tuple_size_v<decltype(watched_ram)>> previous_watched_values{};
    for (std::size_t index = 0; index < watched_ram.size(); ++index) {
        previous_watched_values[index] = read_watched_value(watched_ram[index]);
        watched_ram[index].initial_value = previous_watched_values[index];
        watched_ram[index].final_value = previous_watched_values[index];
    }
    std::array<std::uint8_t, 3> previous_post_stage_object_bytes{
        read_post_stage_object_byte(0),
        read_post_stage_object_byte(5),
        read_post_stage_object_byte(6),
    };

    for (int step_index = 0; step_index < kAudioCaptureMaxSteps; ++step_index) {
        if (startup_audio_capture.psg().sample_buffer().size() >= target_sample_count) {
            run_result = {
                .reached_target = true,
                .reason = "Startup audio capture reached the target sample count.",
                .stop_pc = startup_audio_capture.cpu().registers().pc,
            };
            break;
        }

        const auto pre_step_frame = startup_audio_capture.vdp().frames_elapsed();
        if (options.auto_press_start &&
            !auto_start_triggered &&
            !auto_start_active &&
            pre_step_frame >= options.auto_start_frame &&
            startup_audio_capture.vdp().display_enabled()) {
            startup_audio_capture.input_state().set_button(mercury::gamegear::Button::start, true);
            auto_start_active = true;
            auto_start_release_frame = pre_step_frame + 2;
            append_state_log("auto-start-press", previous_step_result, startup_audio_capture.cpu().registers());
        } else if (auto_start_active && pre_step_frame >= auto_start_release_frame) {
            startup_audio_capture.input_state().set_button(mercury::gamegear::Button::start, false);
            auto_start_active = false;
            auto_start_triggered = true;
            append_state_log("auto-start-release", previous_step_result, startup_audio_capture.cpu().registers());
        }
        for (const auto& window : options.button_windows) {
            const bool pressed = pre_step_frame >= window.start_frame &&
                                 pre_step_frame < window.end_frame;
            startup_audio_capture.input_state().set_button(window.button, pressed);
        }

        const auto pre_step_registers = startup_audio_capture.cpu().registers();
        const auto write_count_before = startup_audio_capture.psg().write_count();
        const auto vdp_data_write_count_before = startup_audio_capture.vdp().data_write_count();
        const auto vdp_control_write_count_before = startup_audio_capture.vdp().control_write_count();
        const auto pre_step_ix26 = startup_audio_capture.bus().read8(static_cast<std::uint16_t>(pre_step_registers.ix + 0x26));
        const auto pre_step_ix27 = startup_audio_capture.bus().read8(static_cast<std::uint16_t>(pre_step_registers.ix + 0x27));
        const auto pre_step_dc08 = read16_at(0xDC08);
        const std::array<std::uint8_t, 4> pre_step_post_stage_wait_bytes{
            read_post_stage_object_byte(0),
            read_post_stage_object_byte(3),
            read_post_stage_object_byte(5),
            read_post_stage_object_byte(6),
        };
        const auto step_result = startup_audio_capture.step();
        const auto post_step_registers = startup_audio_capture.cpu().registers();
        const auto write_count_after = startup_audio_capture.psg().write_count();
        const auto vdp_data_write_count_after = startup_audio_capture.vdp().data_write_count();
        const auto vdp_control_write_count_after = startup_audio_capture.vdp().control_write_count();
        const auto post_step_ix26 = startup_audio_capture.bus().read8(static_cast<std::uint16_t>(pre_step_registers.ix + 0x26));
        const auto post_step_ix27 = startup_audio_capture.bus().read8(static_cast<std::uint16_t>(pre_step_registers.ix + 0x27));
        const auto post_step_dc08 = read16_at(0xDC08);
        switch (step_result.pc_before) {
            case 0x1CD7:
            case 0x1D62:
            case 0x1D8C:
            case 0x1DA6:
            case 0x1DB1:
            case 0x1DB9:
            case 0x1DDF:
            case 0x1E05:
            case 0x1E2B:
            case 0x1E7C:
            case 0x1E89:
            case 0x1EEF:
            case 0x1F02:
            case 0x2F5C:
            case 0x433B:
                append_state_log("focus", step_result, pre_step_registers);
                break;
            default:
                break;
        }
        const bool in_direct_title_gate =
            step_result.pc_before >= 0x4330 && step_result.pc_before <= 0x43B1;
        const auto frame_for_step = startup_audio_capture.vdp().frames_elapsed();
        const bool near_failure_window =
            (frame_for_step >= 420 && frame_for_step <= 470) ||
            (frame_for_step >= 880 && frame_for_step <= 930);
        const bool in_late_gate_breadcrumb =
            near_failure_window &&
            (step_result.pc_before == 0x016D ||
             step_result.pc_before == 0x0172 ||
             step_result.pc_before == 0x0178 ||
             step_result.pc_before == 0x017C ||
             step_result.pc_before == 0x0180 ||
             step_result.pc_before == 0x0185 ||
             step_result.pc_before == 0x019F ||
             step_result.pc_before == 0x01B4 ||
             step_result.pc_before == 0x01D7 ||
             step_result.pc_before == 0x0210 ||
             step_result.pc_before == 0x0327 ||
             step_result.pc_before == 0x032B ||
             step_result.pc_before == 0x032D ||
             step_result.pc_before == 0x1CD7 ||
             step_result.pc_before == 0x1E7C ||
             step_result.pc_before == 0x1E80 ||
             step_result.pc_before == 0x1E83 ||
             step_result.pc_before == 0x1EEF ||
             step_result.pc_before == 0x1F00 ||
             step_result.pc_before == 0x1F02 ||
             step_result.pc_before == 0x435A ||
             (step_result.pc_before == 0x032D &&
              (step_result.pc_after == 0x435A || step_result.pc_after == 0x1E83)));
        const bool in_stage_handoff_window = frame_for_step >= 520 && frame_for_step <= 620;
        const bool stage_handoff_pc =
            step_result.pc_before == 0x043B ||
            step_result.pc_before == 0x044E ||
            step_result.pc_before == 0x045E ||
            step_result.pc_before == 0x0490 ||
            step_result.pc_before == 0x04C9 ||
            step_result.pc_before == 0x04CF ||
            step_result.pc_before == 0x04D5 ||
            step_result.pc_before == 0x04DB ||
            step_result.pc_before == 0x0AAF ||
            step_result.pc_before == 0x0ADF ||
            step_result.pc_before == 0x0B12 ||
            step_result.pc_before == 0x0BFD ||
            step_result.pc_before == 0x0C16 ||
            step_result.pc_before == 0x0C21 ||
            step_result.pc_before == 0x0D04 ||
            step_result.pc_before == 0x0D1D ||
            step_result.pc_before == 0x0D22 ||
            step_result.pc_before == 0x0D2A ||
            step_result.pc_before == 0x0D4A ||
            step_result.pc_before == 0x0D4D ||
            step_result.pc_before == 0x0D60 ||
            step_result.pc_before == 0x0E23 ||
            step_result.pc_before == 0x1CD7;
        const bool in_post_stage_script_window = frame_for_step >= 568 && frame_for_step <= 590;
        const bool in_iy_transition_window = frame_for_step >= 552 && frame_for_step <= 570;
        const bool in_iy_focus_window = frame_for_step >= 557 && frame_for_step <= 569;
        const bool post_stage_script_pc =
            (step_result.pc_before >= 0x0BE0 && step_result.pc_before <= 0x0D80) ||
            (step_result.pc_before >= 0x0E23 && step_result.pc_before <= 0x0E79) ||
            step_result.pc_before == 0x00F9 ||
            step_result.pc_before == 0x016B;
        if (in_direct_title_gate || in_late_gate_breadcrumb) {
            append_title_gate_log("gate", step_result, pre_step_registers);
        }
        if (in_stage_handoff_window && stage_handoff_pc) {
            auto& count = stage_handoff_log_counts[step_result.pc_before];
            if (count < 24) {
                ++count;
                append_stage_handoff_log("stage-handoff", step_result, pre_step_registers);
            }
        }
        const bool in_stack_restore_window = frame_for_step >= 566 && frame_for_step <= 569;
        const bool stack_restore_pc =
            step_result.pc_before >= 0x00AA && step_result.pc_before <= 0x00E9;
        if (in_stack_restore_window && stack_restore_pc) {
            append_stack_restore_log("stack-restore", step_result, pre_step_registers);
        }
        if (in_post_stage_script_window && post_stage_script_pc) {
            append_post_stage_script_log("post-stage-script", step_result, pre_step_registers);
        }
        if (frame_for_step >= 520 && frame_for_step <= 620) {
            const std::array<std::uint8_t, 3> current_post_stage_object_bytes{
                read_post_stage_object_byte(0),
                read_post_stage_object_byte(5),
                read_post_stage_object_byte(6),
            };
            if (current_post_stage_object_bytes != previous_post_stage_object_bytes) {
                append_post_stage_object_log(
                    "post-stage-object-change",
                    step_result,
                    pre_step_registers,
                    previous_post_stage_object_bytes,
                    current_post_stage_object_bytes);
                previous_post_stage_object_bytes = current_post_stage_object_bytes;
            }
        }
        const bool in_post_stage_wait_window = frame_for_step >= 560 && frame_for_step <= 640;
        const bool post_stage_wait_pc =
            step_result.pc_before == 0x016D ||
            step_result.pc_before == 0x0171 ||
            step_result.pc_before == 0x0327 ||
            step_result.pc_before == 0x032B ||
            step_result.pc_before == 0x032D ||
            step_result.pc_before == 0x0C24 ||
            step_result.pc_before == 0x0C28 ||
            step_result.pc_before == 0x13C1 ||
            step_result.pc_before == 0x1D0A ||
            step_result.pc_before == 0x4335;
        if (in_post_stage_wait_window &&
            post_stage_wait_pc &&
            (pre_step_registers.iy == kPostStageObjectBase || post_step_registers.iy == kPostStageObjectBase)) {
            const std::array<std::uint8_t, 4> after_wait_bytes{
                read_post_stage_object_byte(0),
                read_post_stage_object_byte(3),
                read_post_stage_object_byte(5),
                read_post_stage_object_byte(6),
            };
            auto& count = post_stage_wait_log_counts[step_result.pc_before];
            ++count;
            const bool always_log_wake_path =
                step_result.pc_before == 0x016D ||
                step_result.pc_before == 0x0171 ||
                step_result.pc_before == 0x032D ||
                step_result.pc_before == 0x0C24 ||
                step_result.pc_before == 0x0C28;
            const bool bytes_changed = pre_step_post_stage_wait_bytes != after_wait_bytes;
            const bool sampled_wait_spin =
                (step_result.pc_before == 0x0327 || step_result.pc_before == 0x032B) &&
                (count <= 8 || (count % 32) == 0);
            if (always_log_wake_path || bytes_changed || sampled_wait_spin) {
                append_post_stage_wait_log(
                    "post-stage-wait",
                    step_result,
                    pre_step_registers,
                    post_step_registers,
                    pre_step_post_stage_wait_bytes,
                    after_wait_bytes);
            }
        }
        const bool in_stage_branch_window = frame_for_step >= 420 && frame_for_step <= 590;
        const bool stage_branch_pc =
            step_result.pc_before == 0x13B8 ||
            step_result.pc_before == 0x13BB ||
            step_result.pc_before == 0x13BF ||
            step_result.pc_before == 0x13D8 ||
            step_result.pc_before == 0x13DB ||
            step_result.pc_before == 0x42DA ||
            step_result.pc_before == 0x4312 ||
            step_result.pc_before == 0x434B ||
            step_result.pc_before == 0x43AA ||
            step_result.pc_before == 0x43AE ||
            step_result.pc_before == 0x43B1 ||
            step_result.pc_before == 0x43B2;
        if (in_stage_branch_window && stage_branch_pc) {
            append_stage_branch_log("stage-branch", step_result, pre_step_registers);
        }
        if (frame_for_step >= 520 &&
            frame_for_step <= 545 &&
            startup_audio_capture.cartridge().visible_banks()[1] == 3 &&
            pre_step_registers.ix == 0xDC26 &&
            step_result.pc_before >= 0x43DE &&
            step_result.pc_before <= 0x45FF) {
            append_bank3_object_trace_log(step_result, pre_step_registers);
        }
        const auto bc_before = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(pre_step_registers.b) << 8) | pre_step_registers.c);
        const auto bc_after = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(post_step_registers.b) << 8) | post_step_registers.c);
        const bool suspicious_sp =
            (pre_step_registers.sp >= 0xDBF0 && pre_step_registers.sp <= 0xDC40) ||
            (post_step_registers.sp >= 0xDBF0 && post_step_registers.sp <= 0xDC40);
        const bool suspicious_iy =
            pre_step_registers.iy != 0xD200 || post_step_registers.iy != 0xD200;
        const bool suspicious_bc =
            bc_before == 0x2600 || bc_after == 0x2600 ||
            (bc_before >= 0x25F0 && bc_before <= 0x2610) ||
            (bc_after >= 0x25F0 && bc_after <= 0x2610);
        const bool focused_pc =
            step_result.pc_before == 0x00CB ||
            step_result.pc_before == 0x00D0 ||
            step_result.pc_before == 0x00D7 ||
            step_result.pc_before == 0x00DE ||
            step_result.pc_before == 0x00E2 ||
            step_result.pc_before == 0x0AAF ||
            step_result.pc_before == 0x0AB7 ||
            step_result.pc_before == 0x0ADF ||
            step_result.pc_before == 0x0AE2 ||
            step_result.pc_before == 0x0AE9 ||
            step_result.pc_before == 0x0B12 ||
            step_result.pc_before == 0x0BE7 ||
            step_result.pc_before == 0x0C24 ||
            step_result.pc_before == 0x0D22 ||
            step_result.pc_before == 0x0D60 ||
            step_result.pc_before == 0x0E23 ||
            step_result.pc_before == 0x0E24 ||
            step_result.pc_before == 0x0E25 ||
            step_result.pc_before == 0x0E26 ||
            step_result.pc_before == 0x0327;

        if (pre_step_registers.iy != post_step_registers.iy) {
            append_iy_log(
                startup_iy_change_log, 512, "iy-change", step_result, pre_step_registers, post_step_registers);
        } else if ((in_iy_focus_window || in_iy_transition_window) &&
                   (focused_pc || step_result.prefix == 0xFD || suspicious_bc || suspicious_iy) &&
                   (suspicious_sp || suspicious_bc || suspicious_iy)) {
            append_iy_log(
                startup_iy_focus_log, 512, "iy-focus", step_result, pre_step_registers, post_step_registers);
        }
        if (frame_for_step >= 553 && frame_for_step <= 569) {
            std::array<std::uint8_t, 6> current_watched_stack_bytes{};
            bool changed = false;
            for (std::size_t index = 0; index < watched_stack_addresses.size(); ++index) {
                current_watched_stack_bytes[index] =
                    startup_audio_capture.bus().read8(watched_stack_addresses[index]);
                changed = changed || (current_watched_stack_bytes[index] != previous_watched_stack_bytes[index]);
            }
            if (changed) {
                append_stack_slot_log(
                    step_result,
                    pre_step_registers,
                    post_step_registers,
                    previous_watched_stack_bytes,
                    current_watched_stack_bytes);
                previous_watched_stack_bytes = current_watched_stack_bytes;
            }
        }
        if (step_result.executed &&
            !step_result.unimplemented &&
            (step_result.opcode == 0xDF || step_result.opcode == 0xE7 || step_result.opcode == 0xEF)) {
            const auto key =
                (static_cast<std::uint32_t>(step_result.opcode) << 24) |
                (static_cast<std::uint32_t>(pre_step_registers.a) << 16) |
                step_result.pc_before;
            auto& stats = restart_command_sites[key];
            ++stats.hits;
            if (stats.hits == 1) {
                stats.example_step = step_result;
                stats.a_before = pre_step_registers.a;
            }
        }
        if (write_count_after > write_count_before) {
            auto& site = psg_write_sites[step_result.pc_before];
            site.writes += (write_count_after - write_count_before);
            ++site.hits;
            if (site.hits == 1) {
                site.example_step = step_result;
            }
        }
        if (vdp_data_write_count_after > vdp_data_write_count_before) {
            auto& site = vdp_data_write_sites[step_result.pc_before];
            site.writes += (vdp_data_write_count_after - vdp_data_write_count_before);
            ++site.hits;
            if (site.hits == 1) {
                site.example_step = step_result;
            }

            const auto frame_for_upload = startup_audio_capture.vdp().frames_elapsed();
            const bool in_progression_window =
                frame_for_upload >= 500 && frame_for_upload <= 560;
            const bool focused_upload_pc =
                step_result.pc_before == 0x0357 ||
                step_result.pc_before == 0x036C ||
                step_result.pc_before == 0x0384 ||
                step_result.pc_before == 0x0490 ||
                step_result.pc_before == 0x04C9 ||
                step_result.pc_before == 0x04CF ||
                step_result.pc_before == 0x04D5 ||
                step_result.pc_before == 0x04DB ||
                step_result.pc_before == 0x05E6 ||
                step_result.pc_before == 0x05EA;
            if (focused_upload_pc && in_progression_window) {
                auto& count = focused_vdp_upload_log_counts[step_result.pc_before];
                if (count < 8) {
                    ++count;
                    append_state_log("vdp-upload", step_result, pre_step_registers);
                }
            }
        }
         if (vdp_control_write_count_after > vdp_control_write_count_before) {
              auto& site = vdp_control_write_sites[step_result.pc_before];
              site.writes += (vdp_control_write_count_after - vdp_control_write_count_before);
              ++site.hits;
              if (site.hits == 1) {
                  site.example_step = step_result;
              }
              if (startup_audio_capture.vdp().register_write_count() != 0 &&
                  startup_audio_capture.vdp().last_register_index() == 1 &&
                  frame_for_step >= 500) {
                  append_title_gate_log("vdp-reg1-write", step_result, pre_step_registers);
              }
          }

        const bool current_pc_in_chant_routine =
            step_result.pc_before >= 0x7C00 && step_result.pc_before < 0x7D20;
        if (current_pc_in_chant_routine && !previous_pc_in_chant_routine && have_previous_step) {
            auto& entry_site = chant_entry_sites[previous_step_result.pc_before];
            ++entry_site.hits;
            if (entry_site.hits == 1) {
                entry_site.example_step = previous_step_result;
                entry_site.visible_banks = startup_audio_capture.cartridge().visible_banks();
            }
        }

        const bool current_pc_in_chant_launcher_routine =
            step_result.pc_before >= 0x1D20 && step_result.pc_before < 0x1F10;
        if (current_pc_in_chant_launcher_routine &&
            !previous_pc_in_chant_launcher_routine &&
            have_previous_step) {
            current_chant_launcher_entry_step = previous_step_result;
            have_chant_launcher_entry_step = true;
        }
        if (step_result.pc_before == 0x1E2B && have_chant_launcher_entry_step) {
            auto& entry_site = chant_launcher_entry_sites[current_chant_launcher_entry_step.pc_before];
            ++entry_site.hits;
            if (entry_site.hits == 1) {
                entry_site.example_step = current_chant_launcher_entry_step;
                entry_site.visible_banks = startup_audio_capture.cartridge().visible_banks();
            }
        }

        const bool current_pc_in_title_init_routine =
            step_result.pc_before >= 0x1DE0 && step_result.pc_before < 0x1E80;
        if (current_pc_in_title_init_routine && !previous_pc_in_title_init_routine && have_previous_step) {
            auto& entry_site = title_init_entry_sites[previous_step_result.pc_before];
            ++entry_site.hits;
            if (entry_site.hits == 1) {
                entry_site.example_step = previous_step_result;
                entry_site.visible_banks = startup_audio_capture.cartridge().visible_banks();
            }
        }

        if (have_previous_step &&
            step_result.pc_before >= 0x4000 && step_result.pc_before < 0x8000 &&
            !(previous_step_result.pc_before >= 0x4000 && previous_step_result.pc_before < 0x8000)) {
            const auto visible_banks = startup_audio_capture.cartridge().visible_banks();
            const auto key = (static_cast<std::uint32_t>(visible_banks[1]) << 16) | step_result.pc_before;
            auto& entry_site = banked_code_entry_sites[key];
            ++entry_site.hits;
            if (entry_site.hits == 1) {
                entry_site.caller_step = previous_step_result;
                entry_site.target_pc = step_result.pc_before;
                entry_site.visible_banks = visible_banks;
            }
        }

        if (step_result.executed &&
            !step_result.unimplemented &&
            step_result.message == "CALL nn" &&
            step_result.pc_after >= 0x4000 && step_result.pc_after < 0x8000 &&
            startup_audio_capture.cartridge().visible_banks()[1] == 3) {
            int slot = -1;
            switch (step_result.pc_after) {
                case 0x4000:
                    slot = 0;
                    break;
                case 0x4006:
                    slot = 1;
                    break;
                case 0x4009:
                    slot = 2;
                    break;
                case 0x400C:
                    slot = 3;
                    break;
                case 0x4012:
                    slot = 4;
                    break;
                case 0x4015:
                    slot = 5;
                    break;
                default:
                    break;
            }
            if (slot >= 0) {
                auto& stats = targeted_bank3_entries[static_cast<std::size_t>(slot)];
                ++stats.hits;
                if (stats.target_pc == 0x0000) {
                    stats.target_pc = step_result.pc_after;
                    stats.caller_step = step_result;
                    stats.a_before = pre_step_registers.a;
                }
            }
        }

        {
            int slot = -1;
            switch (step_result.pc_before) {
                case 0x00CB:
                    slot = 0;
                    break;
                case 0x02ED:
                    slot = 1;
                    break;
                case 0x02FE:
                    slot = 2;
                    break;
                case 0x0311:
                    slot = 3;
                    break;
                case 0x15FB:
                    slot = 4;
                    break;
                case 0x1855:
                    slot = 5;
                    break;
                default:
                    break;
            }
            if (slot >= 0) {
                auto& stats = targeted_fixed_pc_hits[static_cast<std::size_t>(slot)];
                ++stats.hits;
                if (stats.pc == 0x0000) {
                    stats.pc = step_result.pc_before;
                    stats.example_step = step_result;
                }
            }
        }

        {
            int slot = -1;
            switch (step_result.pc_before) {
                case 0x43AA:
                    slot = 0;
                    break;
                case 0x43AE:
                    slot = 1;
                    break;
                case 0x43B1:
                    slot = 2;
                    break;
                case 0x43B2:
                    slot = 3;
                    break;
                case 0x45AF:
                    slot = 4;
                    break;
                case 0x45B7:
                    slot = 5;
                    break;
                default:
                    break;
            }
            if (slot >= 0) {
                auto& stats = bank3_object_path_hits[static_cast<std::size_t>(slot)];
                ++stats.hits;
                if (stats.pc == 0x0000) {
                    stats.pc = step_result.pc_before;
                    stats.example_step = step_result;
                    stats.ix = pre_step_registers.ix;
                    stats.ix26_before = pre_step_ix26;
                    stats.ix27_before = pre_step_ix27;
                    stats.ix26_after = post_step_ix26;
                    stats.ix27_after = post_step_ix27;
                    stats.dc08_before = pre_step_dc08;
                    stats.dc08_after = post_step_dc08;
                }
            }
        }

        const auto current_mapper_registers = startup_audio_capture.cartridge().mapper_registers();
        const auto current_visible_banks = startup_audio_capture.cartridge().visible_banks();
        for (std::size_t index = 0; index < watched_ram.size(); ++index) {
            const auto current_value = read_watched_value(watched_ram[index]);
            watched_ram[index].final_value = current_value;
            if (current_value != previous_watched_values[index]) {
                ++watched_ram[index].change_count;
                ++watched_ram[index].change_sites[step_result.pc_before];
                if (index == 4 ||
                    index == 5 ||
                    (index == 3 && step_result.pc_before == 0x1EEA) ||
                    ((index == 0 || index == 1 || index == 2 || index == 7 || index == 8) &&
                     step_result.pc_before >= 0x1D20 && step_result.pc_before < 0x1F20)) {
                    append_state_log("ram-change", step_result, pre_step_registers);
                }
                if ((index == 1 || index == 2 || index == 3 || index == 4 || index == 5) &&
                    step_result.pc_before >= 0x4330 && step_result.pc_before <= 0x43AF) {
                    append_title_gate_log("gate-ram-change", step_result, pre_step_registers);
                }
                if ((index == 1 || index == 2 || index == 3 || index == 4 || index == 5) &&
                    step_result.pc_before == 0x02B4 &&
                    startup_audio_capture.vdp().frames_elapsed() >= 400) {
                    append_title_gate_log("gate-reset-ram-change", step_result, pre_step_registers);
                }
                if (in_stage_handoff_window &&
                    (index == 1 || index == 3 || index == 4 || index == 5 || index == 7 || index == 8)) {
                    append_stage_handoff_log("stage-ram-change", step_result, pre_step_registers);
                }
                if (in_post_stage_script_window &&
                    (index == 1 || index == 2 || index == 3 || index == 4 || index == 5 ||
                     index == 6 || index == 7 || index == 9 || index == 10 || index == 11)) {
                    append_post_stage_script_log("post-stage-ram-change", step_result, pre_step_registers);
                }
                previous_watched_values[index] = current_value;
            }
        }
        if (current_mapper_registers != previous_mapper_registers ||
            current_visible_banks != previous_visible_banks) {
            auto& switch_site = mapper_switch_sites[step_result.pc_before];
            ++switch_site.hits;
            if (switch_site.hits == 1) {
                switch_site.example_step = step_result;
                switch_site.before_registers = previous_mapper_registers;
                switch_site.after_registers = current_mapper_registers;
                switch_site.before_banks = previous_visible_banks;
                switch_site.after_banks = current_visible_banks;
            }
        }

        if (!step_result.executed || step_result.unimplemented) {
            std::ostringstream reason;
            reason << "Startup audio capture stopped at PC="
                   << hex16(step_result.pc_before)
                   << " with OP=";
            if (step_result.prefix != 0x00) {
                reason << hex8(step_result.prefix) << " ";
            }
            reason << hex8(step_result.opcode)
                   << " because the opcode is not implemented yet";
            if (!step_result.message.empty()) {
                reason << " (" << step_result.message << ")";
            }
            reason << ".";
            std::cout << reason.str() << "\n";
            run_result = {
                .reached_target = false,
                .reason = reason.str(),
                .stop_pc = step_result.pc_before,
            };
            break;
        }

        const auto current_vdp_reg1 = startup_audio_capture.vdp().register_value(1);
        const bool display_enabled = startup_audio_capture.vdp().display_enabled();
        if (current_vdp_reg1 != previous_vdp_reg1 || display_enabled != previous_display_enabled) {
            append_display_transition_log(
                "display-transition",
                step_result,
                pre_step_registers,
                previous_vdp_reg1,
                current_vdp_reg1,
                previous_display_enabled,
                display_enabled);
            if (frame_for_step >= 520) {
                append_late_display_transition_log(
                    "late-display-transition",
                    step_result,
                    pre_step_registers,
                    previous_vdp_reg1,
                    current_vdp_reg1,
                    previous_display_enabled,
                    display_enabled);
            }
        }
        if (display_enabled && !previous_display_enabled) {
            std::ostringstream file_name;
            file_name << "mercury_vdp_" << options.file_prefix << "_audio_cycle_"
                      << std::setw(2) << std::setfill('0')
                      << display_on_cycle_count << ".bmp";
            std::ostringstream label;
            label << "VDP " << options.mode_name << " audio cycle " << display_on_cycle_count;
            if (display_on_cycle_count < kMaxDisplayCycleExports) {
                try_export_visible_frame(startup_audio_capture, file_name.str(), label.str());
            }
            ++display_on_cycle_count;
        }

        const auto current_frame_count = startup_audio_capture.vdp().frames_elapsed();
        if (current_frame_count != previous_frame_count &&
            (current_frame_count % kLongRunFrameExportInterval) == 0 &&
            long_run_frame_exports < kMaxLongRunFrameExports) {
            std::ostringstream file_name;
            file_name << "mercury_vdp_" << options.file_prefix << "_longrun_frame_"
                      << std::setw(3) << std::setfill('0')
                      << current_frame_count << ".bmp";
            std::ostringstream label;
            label << "VDP " << options.mode_name << " longrun frame " << current_frame_count;
            if (display_enabled) {
                try_export_visible_frame(startup_audio_capture, file_name.str(), label.str());
            }
            if (current_frame_count >= 480) {
                export_visible_frame_layers(startup_audio_capture, file_name.str(), label.str());
            }
            if (current_frame_count == 900) {
                const auto tilesheet_file_name =
                    "mercury_vdp_" + options.file_prefix + "_longrun_frame_900_tilesheet.bmp";
                const auto background_map_file_name =
                    "mercury_vdp_" + options.file_prefix + "_longrun_frame_900_background_map_full.bmp";
                try_export_vdp_bmp(
                    startup_audio_capture.vdp(),
                    &mercury::gamegear::Vdp::export_tilesheet_bmp,
                    tilesheet_file_name,
                    "VDP " + options.mode_name + " longrun frame 900 tilesheet");
                try_export_vdp_bmp(
                    startup_audio_capture.vdp(),
                    &mercury::gamegear::Vdp::export_background_map_bmp,
                    background_map_file_name,
                    "VDP " + options.mode_name + " longrun frame 900 background map");
            }
            if (current_frame_count == 540) {
                const auto background_map_file_name =
                    "mercury_vdp_" + options.file_prefix + "_longrun_frame_540_background_map_full.bmp";
                try_export_vdp_bmp(
                    startup_audio_capture.vdp(),
                    &mercury::gamegear::Vdp::export_background_map_bmp,
                    background_map_file_name,
                    "VDP " + options.mode_name + " longrun frame 540 background map");
                std::cout << "VDP " << options.mode_name << " longrun frame 540 regs: "
                          << startup_audio_capture.vdp().register_summary() << "\n";
            }
            if (current_frame_count == 600) {
                const auto background_map_file_name =
                    "mercury_vdp_" + options.file_prefix + "_longrun_frame_600_background_map_full.bmp";
                try_export_vdp_bmp(
                    startup_audio_capture.vdp(),
                    &mercury::gamegear::Vdp::export_background_map_bmp,
                    background_map_file_name,
                    "VDP " + options.mode_name + " longrun frame 600 background map");
                std::cout << "VDP " << options.mode_name << " longrun frame 600 regs: "
                          << startup_audio_capture.vdp().register_summary() << "\n";
            }
            if (current_frame_count == 720) {
                const auto background_map_file_name =
                    "mercury_vdp_" + options.file_prefix + "_longrun_frame_720_background_map_full.bmp";
                try_export_vdp_bmp(
                    startup_audio_capture.vdp(),
                    &mercury::gamegear::Vdp::export_background_map_bmp,
                    background_map_file_name,
                    "VDP " + options.mode_name + " longrun frame 720 background map");
                std::cout << "VDP " << options.mode_name << " longrun frame 720 regs: "
                          << startup_audio_capture.vdp().register_summary() << "\n";
            }
            ++long_run_frame_exports;
        }

        previous_display_enabled = display_enabled;
        previous_vdp_reg1 = current_vdp_reg1;
        previous_frame_count = current_frame_count;
        previous_step_result = step_result;
        have_previous_step = true;
        previous_pc_in_chant_routine = current_pc_in_chant_routine;
        previous_pc_in_chant_launcher_routine = current_pc_in_chant_launcher_routine;
        previous_pc_in_title_init_routine = current_pc_in_title_init_routine;
        previous_mapper_registers = current_mapper_registers;
        previous_visible_banks = current_visible_banks;
    }

    if (run_result.reason.empty()) {
        if (startup_audio_capture.psg().sample_buffer().size() >= target_sample_count) {
            run_result = {
                .reached_target = true,
                .reason = "Startup audio capture reached the target sample count at the step limit.",
                .stop_pc = startup_audio_capture.cpu().registers().pc,
            };
        } else {
            std::ostringstream reason;
            reason << "Startup audio capture reached the step limit before enough audio samples were produced.";
            std::cout << reason.str() << "\n";
            run_result = {
                .reached_target = false,
                .reason = reason.str(),
                .stop_pc = startup_audio_capture.cpu().registers().pc,
            };
        }
    }

    const bool export_debug_logs =
        !run_result.reached_target ||
        (options.file_prefix == "progression" && !startup_stage_branch_log.empty());

    if (export_debug_logs) {
        const auto state_log_file_name = "mercury_" + options.file_prefix + "_state_log.txt";
        const auto title_gate_log_file_name = "mercury_" + options.file_prefix + "_title_gate_log.txt";
        const auto stage_handoff_log_file_name =
            "mercury_" + options.file_prefix + "_stage_handoff_log.txt";
        const auto post_stage_script_log_file_name =
            "mercury_" + options.file_prefix + "_post_stage_script_log.txt";
        const auto post_stage_object_log_file_name =
            "mercury_" + options.file_prefix + "_post_stage_object_log.txt";
        const auto post_stage_wait_log_file_name =
            "mercury_" + options.file_prefix + "_post_stage_wait_log.txt";
        const auto stage_branch_log_file_name =
            "mercury_" + options.file_prefix + "_stage_branch_log.txt";
        const auto iy_change_log_file_name =
            "mercury_" + options.file_prefix + "_iy_change_log.txt";
        const auto iy_focus_log_file_name =
            "mercury_" + options.file_prefix + "_iy_focus_log.txt";
        const auto stack_restore_log_file_name =
            "mercury_" + options.file_prefix + "_stack_restore_log.txt";
        const auto stack_slot_log_file_name =
            "mercury_" + options.file_prefix + "_stack_slot_log.txt";
        const auto bank3_object_trace_log_file_name =
            "mercury_" + options.file_prefix + "_bank3_object_trace_log.txt";
        const auto display_transition_log_file_name =
            "mercury_" + options.file_prefix + "_display_transition_log.txt";
        const auto late_display_transition_log_file_name =
            "mercury_" + options.file_prefix + "_late_display_transition_log.txt";
        try_export_text_lines(
            startup_state_log,
            state_log_file_name,
            options.mode_name + " state log");
        try_export_text_lines(
            startup_title_gate_log,
            title_gate_log_file_name,
            options.mode_name + " title gate log");
        try_export_text_lines(
            startup_stage_handoff_log,
            stage_handoff_log_file_name,
            options.mode_name + " stage handoff log");
        try_export_text_lines(
            startup_post_stage_script_log,
            post_stage_script_log_file_name,
            options.mode_name + " post-stage script log");
        try_export_text_lines(
            startup_post_stage_object_log,
            post_stage_object_log_file_name,
            options.mode_name + " post-stage object log");
        try_export_text_lines(
            startup_post_stage_wait_log,
            post_stage_wait_log_file_name,
            options.mode_name + " post-stage wait log");
        try_export_text_lines(
            startup_stage_branch_log,
            stage_branch_log_file_name,
            options.mode_name + " stage branch log");
        try_export_text_lines(
            startup_iy_change_log,
            iy_change_log_file_name,
            options.mode_name + " IY change log");
        try_export_text_lines(
            startup_iy_focus_log,
            iy_focus_log_file_name,
            options.mode_name + " IY focus log");
        try_export_text_lines(
            startup_stack_restore_log,
            stack_restore_log_file_name,
            options.mode_name + " stack restore log");
        try_export_text_lines(
            startup_stack_slot_log,
            stack_slot_log_file_name,
            options.mode_name + " stack slot log");
        try_export_text_lines(
            startup_bank3_object_trace_log,
            bank3_object_trace_log_file_name,
            options.mode_name + " bank3 object trace log");
        try_export_text_lines(
            startup_display_transition_log,
            display_transition_log_file_name,
            options.mode_name + " display transition log");
        try_export_text_lines(
            startup_late_display_transition_log,
            late_display_transition_log_file_name,
            options.mode_name + " late display transition log");
        const auto& partial_samples = startup_audio_capture.psg().sample_buffer();
        if (partial_samples.empty()) {
            const std::string message =
                "PSG " + options.mode_name + " audio: no samples were captured. " + run_result.reason;
            std::cout << message << "\n";
            return message;
        }

        std::ostringstream message;
        message << "PSG " << options.mode_name << " audio: captured only " << partial_samples.size()
                << " samples before stopping early. "
                << run_result.reason << " "
                << "display-on cycles=" << display_on_cycle_count << ", "
                << "longrun frame exports=" << long_run_frame_exports << ", "
                << "top PSG write sites=[" << summarize_top_psg_write_sites(psg_write_sites) << "], "
                << "top VDP data write sites=[" << summarize_top_psg_write_sites(vdp_data_write_sites) << "], "
                << "top VDP control write sites=[" << summarize_top_psg_write_sites(vdp_control_write_sites) << "], "
                << "chant entry sites=[" << summarize_top_routine_entry_sites(chant_entry_sites) << "], "
                << "chant launcher entry sites=[" << summarize_top_routine_entry_sites(chant_launcher_entry_sites) << "], "
                << "title init entry sites=[" << summarize_top_routine_entry_sites(title_init_entry_sites) << "], "
                << "mapper switch sites=[" << summarize_top_mapper_switch_sites(mapper_switch_sites) << "], "
                << "banked code entry sites=[" << summarize_top_banked_code_entry_sites(banked_code_entry_sites) << "], "
                << "restart command sites=[" << summarize_restart_command_sites(restart_command_sites) << "], "
                << "targeted bank3 entries=[" << summarize_targeted_bank3_entry_sites(targeted_bank3_entries) << "], "
                << "targeted fixed PCs=[" << summarize_fixed_pc_hits(targeted_fixed_pc_hits) << "], "
                << "bank3 object path=[" << summarize_bank3_object_path_hits(bank3_object_path_hits) << "], "
                << "watched RAM=[" << summarize_watched_ram(watched_ram) << "], "
                << "startup VDP={" << startup_audio_capture.vdp().activity_summary() << "}, "
                << startup_audio_capture.psg().activity_summary();
        std::cout << message.str() << "\n";
        const auto partial_audio_file_name = "mercury_psg_" + options.file_prefix + "_partial.wav";
        const bool exported = try_export_wav(
            partial_samples,
            mercury::gamegear::Psg::kDefaultSampleRate,
            partial_audio_file_name,
            "PSG " + options.mode_name + " partial audio");
        if (exported) {
            message << " Exported " << partial_audio_file_name << ".";
        } else {
            message << " Partial WAV export failed.";
        }
        return message.str();
    }

    const auto& full_buffer = startup_audio_capture.psg().sample_buffer();
    const auto trimmed_sample_count = std::min<std::size_t>(target_sample_count, full_buffer.size());
    std::vector<std::int16_t> trimmed_samples(
        full_buffer.begin(),
        full_buffer.begin() + static_cast<std::ptrdiff_t>(trimmed_sample_count));
    const auto full_audio_file_name = "mercury_psg_" + options.file_prefix + "_20s.wav";
    const bool exported = try_export_wav(
        trimmed_samples,
        mercury::gamegear::Psg::kDefaultSampleRate,
        full_audio_file_name,
        "PSG " + options.mode_name + " audio");
    const auto state_log_file_name = "mercury_" + options.file_prefix + "_state_log.txt";
    const auto title_gate_log_file_name = "mercury_" + options.file_prefix + "_title_gate_log.txt";
    const auto stage_handoff_log_file_name =
        "mercury_" + options.file_prefix + "_stage_handoff_log.txt";
    const auto post_stage_script_log_file_name =
        "mercury_" + options.file_prefix + "_post_stage_script_log.txt";
    const auto post_stage_object_log_file_name =
        "mercury_" + options.file_prefix + "_post_stage_object_log.txt";
    const auto post_stage_wait_log_file_name =
        "mercury_" + options.file_prefix + "_post_stage_wait_log.txt";
    const auto stage_branch_log_file_name =
        "mercury_" + options.file_prefix + "_stage_branch_log.txt";
    const auto iy_change_log_file_name =
        "mercury_" + options.file_prefix + "_iy_change_log.txt";
    const auto iy_focus_log_file_name =
        "mercury_" + options.file_prefix + "_iy_focus_log.txt";
    const auto stack_restore_log_file_name =
        "mercury_" + options.file_prefix + "_stack_restore_log.txt";
    const auto stack_slot_log_file_name =
        "mercury_" + options.file_prefix + "_stack_slot_log.txt";
    const auto bank3_object_trace_log_file_name =
        "mercury_" + options.file_prefix + "_bank3_object_trace_log.txt";
    const auto display_transition_log_file_name =
        "mercury_" + options.file_prefix + "_display_transition_log.txt";
    const auto late_display_transition_log_file_name =
        "mercury_" + options.file_prefix + "_late_display_transition_log.txt";
    try_export_text_lines(
        startup_state_log,
        state_log_file_name,
        options.mode_name + " state log");
    try_export_text_lines(
        startup_title_gate_log,
        title_gate_log_file_name,
        options.mode_name + " title gate log");
    try_export_text_lines(
        startup_stage_handoff_log,
        stage_handoff_log_file_name,
        options.mode_name + " stage handoff log");
    try_export_text_lines(
        startup_post_stage_script_log,
        post_stage_script_log_file_name,
        options.mode_name + " post-stage script log");
    try_export_text_lines(
        startup_post_stage_object_log,
        post_stage_object_log_file_name,
        options.mode_name + " post-stage object log");
    try_export_text_lines(
        startup_post_stage_wait_log,
        post_stage_wait_log_file_name,
        options.mode_name + " post-stage wait log");
    try_export_text_lines(
        startup_stage_branch_log,
        stage_branch_log_file_name,
        options.mode_name + " stage branch log");
    try_export_text_lines(
        startup_iy_change_log,
        iy_change_log_file_name,
        options.mode_name + " IY change log");
    try_export_text_lines(
        startup_iy_focus_log,
        iy_focus_log_file_name,
        options.mode_name + " IY focus log");
    try_export_text_lines(
        startup_stack_restore_log,
        stack_restore_log_file_name,
        options.mode_name + " stack restore log");
    try_export_text_lines(
        startup_stack_slot_log,
        stack_slot_log_file_name,
        options.mode_name + " stack slot log");
    try_export_text_lines(
        startup_bank3_object_trace_log,
        bank3_object_trace_log_file_name,
        options.mode_name + " bank3 object trace log");
    try_export_text_lines(
        startup_display_transition_log,
        display_transition_log_file_name,
        options.mode_name + " display transition log");
    try_export_text_lines(
        startup_late_display_transition_log,
        late_display_transition_log_file_name,
        options.mode_name + " late display transition log");
    const auto tilesheet_file_name = "mercury_vdp_" + options.file_prefix + "_20s_tilesheet.bmp";
    const auto background_map_file_name = "mercury_vdp_" + options.file_prefix + "_20s_background_map.bmp";
    const auto visible_frame_file_name = "mercury_vdp_" + options.file_prefix + "_20s_visible_frame.bmp";
    try_export_vdp_bmp(
        startup_audio_capture.vdp(),
        &mercury::gamegear::Vdp::export_tilesheet_bmp,
        tilesheet_file_name,
        "VDP " + options.mode_name + " 20s tilesheet");
    try_export_vdp_bmp(
        startup_audio_capture.vdp(),
        &mercury::gamegear::Vdp::export_background_map_bmp,
        background_map_file_name,
        "VDP " + options.mode_name + " 20s background map");
    try_export_vdp_bmp(
        startup_audio_capture.vdp(),
        &mercury::gamegear::Vdp::export_visible_frame_bmp,
        visible_frame_file_name,
        "VDP " + options.mode_name + " 20s visible frame");
    std::ostringstream message;
    message << "PSG " << options.mode_name << " audio summary: captured " << trimmed_sample_count
             << " samples. " << run_result.reason << " "
             << "display-on cycles=" << display_on_cycle_count << ", "
             << "longrun frame exports=" << long_run_frame_exports << ", "
             << "top PSG write sites=[" << summarize_top_psg_write_sites(psg_write_sites) << "], "
             << "top VDP data write sites=[" << summarize_top_psg_write_sites(vdp_data_write_sites) << "], "
             << "top VDP control write sites=[" << summarize_top_psg_write_sites(vdp_control_write_sites) << "], "
             << "chant entry sites=[" << summarize_top_routine_entry_sites(chant_entry_sites) << "], "
             << "chant launcher entry sites=[" << summarize_top_routine_entry_sites(chant_launcher_entry_sites) << "], "
             << "title init entry sites=[" << summarize_top_routine_entry_sites(title_init_entry_sites) << "], "
             << "mapper switch sites=[" << summarize_top_mapper_switch_sites(mapper_switch_sites) << "], "
             << "banked code entry sites=[" << summarize_top_banked_code_entry_sites(banked_code_entry_sites) << "], "
             << "restart command sites=[" << summarize_restart_command_sites(restart_command_sites) << "], "
             << "targeted bank3 entries=[" << summarize_targeted_bank3_entry_sites(targeted_bank3_entries) << "], "
             << "targeted fixed PCs=[" << summarize_fixed_pc_hits(targeted_fixed_pc_hits) << "], "
             << "bank3 object path=[" << summarize_bank3_object_path_hits(bank3_object_path_hits) << "], "
             << "watched RAM=[" << summarize_watched_ram(watched_ram) << "], "
             << "startup VDP={" << startup_audio_capture.vdp().activity_summary() << "}, "
             << startup_audio_capture.psg().activity_summary();
    if (exported) {
        message << " and exported " << full_audio_file_name << ".";
    } else {
        message << " but WAV export failed.";
    }
    return message.str();
}

void print_trace(mercury::gamegear::GameGear& game_gear, int step_count) {
    std::cout << "\n";
    std::cout << "CPU trace (" << step_count << " steps max)\n";

    std::vector<mercury::core::Z80StepResult> steps;
    steps.reserve(static_cast<std::size_t>(step_count));
    int stop_index = -1;

    for (int index = 0; index < step_count; ++index) {
        const auto step_result = game_gear.step();
        steps.push_back(step_result);

        if (step_result.unimplemented || !step_result.executed) {
            stop_index = index;
            break;
        }
    }

    const auto same_step = [](const mercury::core::Z80StepResult& lhs, const mercury::core::Z80StepResult& rhs) {
        return lhs.pc_before == rhs.pc_before &&
               lhs.opcode == rhs.opcode &&
               lhs.prefix == rhs.prefix &&
               lhs.length == rhs.length &&
               lhs.cycles == rhs.cycles &&
               lhs.message == rhs.message &&
               lhs.unimplemented == rhs.unimplemented &&
               lhs.executed == rhs.executed;
    };

    const auto print_step = [](int index, const mercury::core::Z80StepResult& step_result) {
        const auto prefix_text = step_result.prefix != 0x00 ? hex8(step_result.prefix) + " " : "";
        std::cout << std::dec << std::setw(3) << std::setfill(' ') << index
                  << "  PC=" << hex16(step_result.pc_before)
                  << "  OP=" << prefix_text << hex8(step_result.opcode)
                  << "  LEN=" << std::dec << static_cast<int>(step_result.length)
                  << "  CYC=" << step_result.cycles
                  << "  " << step_result.message
                  << "\n";
    };

    for (std::size_t index = 0; index < steps.size();) {
        if (index + 3 < steps.size() &&
            same_step(steps[index], steps[index + 2]) &&
            same_step(steps[index + 1], steps[index + 3]) &&
            !same_step(steps[index], steps[index + 1])) {
            print_step(static_cast<int>(index), steps[index]);
            print_step(static_cast<int>(index + 1), steps[index + 1]);

            std::size_t pair_count = 2;
            std::size_t cursor = index + 4;
            while (cursor + 1 < steps.size() &&
                   same_step(steps[index], steps[cursor]) &&
                   same_step(steps[index + 1], steps[cursor + 1])) {
                ++pair_count;
                cursor += 2;
            }

            if (pair_count > 1) {
                std::cout << "    ... previous two-step pattern repeated " << (pair_count - 1) << " more times\n";
            }
            index += pair_count * 2;
            continue;
        }

        std::size_t repeat_count = 1;
        while (index + repeat_count < steps.size() && same_step(steps[index], steps[index + repeat_count])) {
            ++repeat_count;
        }

        print_step(static_cast<int>(index), steps[index]);
        if (repeat_count > 1) {
            std::cout << "    ... previous step repeated " << (repeat_count - 1) << " more times\n";
        }
        index += repeat_count;
    }

    if (stop_index >= 0) {
        std::cout << "Trace stopped at PC=" << hex16(steps[static_cast<std::size_t>(stop_index)].pc_before)
                  << " because the opcode is not implemented yet.\n";
    }

    std::cout << "Final CPU: " << game_gear.cpu().describe() << "\n";
    std::cout << game_gear.vdp().activity_summary() << "\n";
    std::cout << game_gear.psg().activity_summary() << "\n";
    try_export_wav(
        game_gear.psg().sample_buffer(),
        mercury::gamegear::Psg::kDefaultSampleRate,
        "mercury_psg_trace.wav",
        "PSG trace audio");

    std::vector<std::filesystem::path> candidate_paths;
    candidate_paths.push_back(std::filesystem::current_path() / "mercury_vdp_tilesheet.bmp");

    try {
        candidate_paths.push_back(std::filesystem::temp_directory_path() / "mercury_vdp_tilesheet.bmp");
    } catch (const std::exception&) {
    }

    bool exported = false;
    std::filesystem::path exported_path;
    for (const auto& candidate_path : candidate_paths) {
        if (game_gear.vdp().export_tilesheet_bmp(candidate_path)) {
            exported = true;
            exported_path = candidate_path;
            break;
        }
    }

    if (exported) {
        std::cout << "VDP tilesheet: " << exported_path.string() << "\n";
    } else {
        std::cout << "VDP tilesheet: export failed in all candidate locations\n";
    }

    candidate_paths.clear();
    candidate_paths.push_back(std::filesystem::current_path() / "mercury_vdp_background_map.bmp");
    try {
        candidate_paths.push_back(std::filesystem::temp_directory_path() / "mercury_vdp_background_map.bmp");
    } catch (const std::exception&) {
    }

    exported = false;
    exported_path.clear();
    for (const auto& candidate_path : candidate_paths) {
        if (game_gear.vdp().export_background_map_bmp(candidate_path)) {
            exported = true;
            exported_path = candidate_path;
            break;
        }
    }

    if (exported) {
        std::cout << "VDP background map: " << exported_path.string() << "\n";
    } else {
        std::cout << "VDP background map: export failed in all candidate locations\n";
    }

    candidate_paths.clear();
    candidate_paths.push_back(std::filesystem::current_path() / "mercury_vdp_visible_frame.bmp");
    try {
        candidate_paths.push_back(std::filesystem::temp_directory_path() / "mercury_vdp_visible_frame.bmp");
    } catch (const std::exception&) {
    }

    exported = false;
    exported_path.clear();
    for (const auto& candidate_path : candidate_paths) {
        if (game_gear.vdp().export_visible_frame_bmp(candidate_path)) {
            exported = true;
            exported_path = candidate_path;
            break;
        }
    }

    if (exported) {
        std::cout << "VDP visible frame: " << exported_path.string() << "\n";
    } else {
        std::cout << "VDP visible frame: export failed in all candidate locations\n";
    }

    candidate_paths.clear();
    candidate_paths.push_back(std::filesystem::current_path() / "mercury_vdp_visible_frame_debug.bmp");
    try {
        candidate_paths.push_back(std::filesystem::temp_directory_path() / "mercury_vdp_visible_frame_debug.bmp");
    } catch (const std::exception&) {
    }

    exported = false;
    exported_path.clear();
    for (const auto& candidate_path : candidate_paths) {
        if (game_gear.vdp().export_visible_frame_debug_bmp(candidate_path)) {
            exported = true;
            exported_path = candidate_path;
            break;
        }
    }

    if (exported) {
        std::cout << "VDP visible frame debug: " << exported_path.string() << "\n";
    } else {
        std::cout << "VDP visible frame debug: export failed in all candidate locations\n";
    }

    if (!game_gear.vdp().display_enabled()) {
        constexpr int kDisplayProbeMaxSteps = 2'000'000;
        if (wait_for_display_enabled(
                game_gear,
                kDisplayProbeMaxSteps,
                "Display-on probe")) {
            std::cout << "Display enabled after additional CPU steps.\n";
            std::cout << "Display-on CPU: " << game_gear.cpu().describe() << "\n";
            std::cout << game_gear.vdp().activity_summary() << "\n";
            try_export_visible_frame(
                game_gear,
                "mercury_vdp_visible_frame_display_on.bmp",
                "VDP visible frame display-on");
            export_frame_sequence(
                game_gear,
                6,
                "mercury_vdp_visible_frame_sequence",
                "VDP visible frame sequence");
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::cout << "Mercury build marker: 2026-05-14-indexed-alu-audio\n";
    mercury::gamegear::GameGear game_gear;
    bool rom_loaded = false;
    int trace_steps = 1;
    std::filesystem::path rom_path;
    std::string startup_audio_summary;
    std::string progression_audio_summary;

    if (argc > 1) {
        rom_path = argv[1];
        const auto rom_path_view = std::string_view(argv[1]);
        const auto load_result = game_gear.load_rom(rom_path);

        if (!load_result.ok) {
            std::cerr << "ROM load failed: " << load_result.error << "\n";
            return 1;
        }

        std::cout << "Loaded ROM: " << rom_path_view << "\n\n";
        rom_loaded = true;
        trace_steps = parse_trace_steps(argc, argv);
    }

    std::cout << "Mercury bootstrap\n";
    std::cout << "=================\n";
    std::cout << game_gear.describe() << "\n\n";

    std::cout << "Module status\n";
    for (const auto& module : game_gear.module_statuses()) {
        std::cout << "- " << module.name << ": "
                  << mercury::core::to_string(module.state)
                  << " (" << module.note << ")\n";
    }

    const auto step_result = game_gear.step();
    std::cout << "\n";
    std::cout << "CPU step\n";
    std::cout << "- executed: " << step_result.executed << "\n";
    std::cout << "- unimplemented: " << step_result.unimplemented << "\n";
    std::cout << "- cycles: " << step_result.cycles << "\n";
    std::cout << "- pc before: " << hex16(step_result.pc_before) << "\n";
    std::cout << "- pc after: " << hex16(step_result.pc_after) << "\n";
    std::cout << "- message: " << step_result.message << "\n";
    std::cout << "- cpu: " << game_gear.cpu().describe() << "\n";

    if (rom_loaded) {
        game_gear.reset();
        export_startup_sequence_from_reset(rom_path);
        StartupAudioCaptureOptions startup_options;
        startup_options.mode_name = "startup";
        startup_options.file_prefix = "startup";
        startup_audio_summary = export_startup_audio_from_reset(rom_path, startup_options);

        StartupAudioCaptureOptions progression_options;
        progression_options.mode_name = "progression";
        progression_options.file_prefix = "progression";
        progression_options.auto_press_start = true;
        progression_options.auto_start_frame = 430;
        progression_audio_summary = export_startup_audio_from_reset(rom_path, progression_options);
        print_trace(game_gear, trace_steps);
        if (!startup_audio_summary.empty()) {
            std::cout << startup_audio_summary << "\n";
        }
        if (!progression_audio_summary.empty()) {
            std::cout << progression_audio_summary << "\n";
        }
    }

    return 0;
}
