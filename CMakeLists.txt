cmake_minimum_required(VERSION 3.20)

project(Mercury LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MERCURY_ASAN=ON: a separate build with AddressSanitizer, for state_fuzz and
# anything else that needs every out-of-bounds access reported. Never the build
# that ships.
option(MERCURY_ASAN "Build with AddressSanitizer (testing only)" OFF)
if(MERCURY_ASAN AND MSVC)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/DEBUG /INCREMENTAL:NO)
endif()

enable_testing()

add_library(mercury
    src/core/module_status.cpp
    src/core/rom_image.cpp
    src/core/zip_archive.cpp
    src/core/z80_cpu.cpp
    # Shared between Game Gear and Mega Drive: the Mega Drive's sound CPU is a
    # Z80 and its PSG is the same SN76489, so core/ stays system-agnostic and
    # each machine lives in its own directory on top.
    src/core/m68k_cpu.cpp
    src/core/sh2_cpu.cpp
    src/core/game_genie.cpp
    src/core/chd.cpp
    src/core/lzma.cpp
    src/core/flac.cpp
    src/thirtytwox/adapter.cpp
    src/thirtytwox/bus.cpp
    src/thirtytwox/pwm.cpp
    src/thirtytwox/sh2_peripherals.cpp
    src/thirtytwox/thirtytwox.cpp
    src/thirtytwox/vdp.cpp
    # The Sega CD: a second 68000 and its peripherals under the Mega Drive.
    src/segacd/bios.cpp
    src/segacd/disc.cpp
    src/segacd/segacd.cpp
    src/segacd/segacd_cdc.cpp
    src/segacd/segacd_cdd.cpp
    src/segacd/segacd_gfx.cpp
    src/segacd/segacd_pcm.cpp
    src/genesis/cartridge.cpp
    src/genesis/eeprom.cpp
    src/genesis/game_quirks.cpp
    src/genesis/genesis.cpp
    src/genesis/io.cpp
    src/genesis/miracle_piano.cpp
    src/genesis/svp.cpp
    src/genesis/vdp.cpp
    src/genesis/ym2612.cpp
    src/gamegear/bus.cpp
    src/gamegear/cartridge.cpp
    src/gamegear/game_gear.cpp
    src/gamegear/gear_link_session.cpp
    src/gamegear/input_state.cpp
    src/gamegear/mapper_state.cpp
    src/gamegear/memory_map.cpp
    src/gamegear/psg.cpp
    src/gamegear/serial_eeprom.cpp
    src/gamegear/system_control.cpp
    src/gamegear/vdp.cpp
    # Netplay is deliberately system-agnostic: it moves a button mask and a blob
    # of machine state, so a core that does not exist yet gets it for free.
    src/net/netplay.cpp
    src/net/genesis_seats.cpp
)

target_include_directories(mercury PUBLIC include)

target_compile_features(mercury PUBLIC cxx_std_20)

add_executable(mercury_bootstrap
    src/main.cpp
)

target_link_libraries(mercury_bootstrap PRIVATE mercury)

# The player is called genx - the shell is GenX-branded. The library and the
# probe tools keep the mercury name; only the thing a person double-clicks was
# renamed.
add_executable(genx WIN32
    src/runtime_win32.cpp
    src/runtime_win32.rc
)

# gdiplus decodes the embedded logo PNG (it keeps the alpha channel, which a
# build-time conversion to a bitmap would lose); dwmapi darkens the title bar;
# comdlg32 is the Open ROM dialog; winhttp asks GitHub for the newest release.
target_link_libraries(genx PRIVATE mercury gdiplus dwmapi comdlg32 msimg32 ole32 shell32 comctl32
                                   xinput dinput8 dxguid d2d1 winhttp)

# The version this build calls itself, which the update check compares with
# the newest release on GitHub. Bump it with each release. It can be overridden
# when configuring (-DGENX_VERSION=0.9.0) to test the "new version" message.
if(NOT GENX_VERSION)
    set(GENX_VERSION 1.1.1)
endif()
# The three numbers also go into the exe's file properties (VERSIONINFO in
# runtime_win32.rc), which is where Windows and front ends read a version
# without running the program.
if(NOT GENX_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "GENX_VERSION must look like 1.2.3, not \"${GENX_VERSION}\"")
endif()
target_compile_definitions(genx PRIVATE GENX_VERSION="${GENX_VERSION}"
                                        GENX_VERSION_MAJOR=${CMAKE_MATCH_1}
                                        GENX_VERSION_MINOR=${CMAKE_MATCH_2}
                                        GENX_VERSION_PATCH=${CMAKE_MATCH_3})

# The controller pictures in res/ are optional. A tree without them - the
# published source leaves out pictures whose origin is not known - still
# builds, and the Input tab draws a labelled outline instead. Each picture found
# defines GENX_HAVE_<NAME>_PNG for the resource script, so dropping a file into
# res/ and re-running CMake is all it takes to put it back.
foreach(art IN ITEMS pad_3button.png pad_6button.png pad_justifier.png pad_menacer.png
                     pad_piano.png)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/${art}")
        string(TOUPPER "GENX_HAVE_${art}" art_define)
        string(REPLACE "." "_" art_define "${art_define}")
        target_compile_definitions(genx PRIVATE ${art_define})
    endif()
endforeach()

add_executable(z80_cpu_smoke_tests
    src/tests/z80_cpu_smoke_tests.cpp
)

target_link_libraries(z80_cpu_smoke_tests PRIVATE mercury)

add_test(NAME z80_cpu_smoke_tests COMMAND z80_cpu_smoke_tests)

# The library sweep's probe. It was being compiled by hand and left in the build
# directory under its own name, which meant a sweep could quietly measure a
# binary older than the change it was supposed to be testing. Built here so it
# cannot fall behind the library again.
add_executable(genesis_probe
    src/tools/genesis_probe.cpp
    # MERCURY_HOST_PROFILE=1: where the emulator's own time goes.
    src/tools/host_profiler.cpp
)

target_link_libraries(genesis_probe PRIVATE mercury)

add_executable(gg_probe
    src/tools/gg_probe.cpp
)

target_link_libraries(gg_probe PRIVATE mercury)

add_executable(lightgun_check
    src/tools/lightgun_check.cpp
)

target_link_libraries(lightgun_check PRIVATE mercury)

add_executable(gearlink_check
    src/tools/gearlink_check.cpp
)

target_link_libraries(gearlink_check PRIVATE mercury)

add_executable(io_port_check
    src/tools/io_port_check.cpp
)

target_link_libraries(io_port_check PRIVATE mercury)

add_executable(netplay_check
    src/tools/netplay_check.cpp
)

target_link_libraries(netplay_check PRIVATE mercury)

add_executable(determinism_check
    src/tools/determinism_check.cpp
)

target_link_libraries(determinism_check PRIVATE mercury)

add_executable(gun_calibrate
    src/tools/gun_calibrate.cpp
)

target_link_libraries(gun_calibrate PRIVATE mercury)

add_executable(crosshair_preview
    src/tools/crosshair_preview.cpp
)

target_link_libraries(crosshair_preview PRIVATE mercury)

add_executable(dac_rate_check
    src/tools/dac_rate_check.cpp
)

target_link_libraries(dac_rate_check PRIVATE mercury)

# No mercury dependency: this measures the Windows drawing path alone, with no
# emulator underneath, which is the only way to get a number that is not
# entangled with how fast a particular game runs.
add_executable(smooth_bench
    src/tools/smooth_bench.cpp
)

target_link_libraries(smooth_bench PRIVATE gdiplus d2d1)

add_executable(battery_check
    src/tools/battery_check.cpp
)

target_link_libraries(battery_check PRIVATE mercury)

add_executable(gg_state_check
    src/tools/gg_state_check.cpp
)

target_link_libraries(gg_state_check PRIVATE mercury)

# Feeds hostile save states to the machine; see the file. Meant to be built
# with MERCURY_ASAN=ON.
add_executable(state_fuzz
    src/tools/state_fuzz.cpp
)

target_link_libraries(state_fuzz PRIVATE mercury)

add_executable(zip_archive_tests
    src/tests/zip_archive_tests.cpp
)

target_link_libraries(zip_archive_tests PRIVATE mercury)

add_executable(psg_tests
    src/tests/psg_tests.cpp
)

target_link_libraries(psg_tests PRIVATE mercury)

add_test(NAME psg_tests COMMAND psg_tests)

add_executable(gg_eeprom_tests
    src/tests/gg_eeprom_tests.cpp
)

target_link_libraries(gg_eeprom_tests PRIVATE mercury)

add_test(NAME gg_eeprom_tests COMMAND gg_eeprom_tests)

add_executable(miracle_piano_tests
    src/tests/miracle_piano_tests.cpp
)

target_link_libraries(miracle_piano_tests PRIVATE mercury)

add_test(NAME miracle_piano_tests COMMAND miracle_piano_tests)

add_executable(gear_link_tests
    src/tests/gear_link_tests.cpp
)

target_link_libraries(gear_link_tests PRIVATE mercury)

add_test(NAME gear_link_tests COMMAND gear_link_tests)

add_executable(sh2_tests
    src/tests/sh2_tests.cpp
)

target_link_libraries(sh2_tests PRIVATE mercury)

add_test(NAME sh2_tests COMMAND sh2_tests)

add_executable(segacd_tests
    src/tests/segacd_tests.cpp
)

target_link_libraries(segacd_tests PRIVATE mercury)

add_test(NAME segacd_tests COMMAND segacd_tests)

add_executable(thirtytwox_tests
    src/tests/thirtytwox_tests.cpp
)

target_link_libraries(thirtytwox_tests PRIVATE mercury)

add_test(NAME thirtytwox_tests COMMAND thirtytwox_tests)

add_executable(thirtytwox_vdp_tests
    src/tests/thirtytwox_vdp_tests.cpp
)

target_link_libraries(thirtytwox_vdp_tests PRIVATE mercury)

add_test(NAME thirtytwox_vdp_tests COMMAND thirtytwox_vdp_tests)

add_executable(justifier_tests
    src/tests/justifier_tests.cpp
)

target_link_libraries(justifier_tests PRIVATE mercury)

add_test(NAME justifier_tests COMMAND justifier_tests)

add_executable(chd_probe
    src/tools/chd_probe.cpp
)

target_link_libraries(chd_probe PRIVATE mercury)

add_executable(game_genie_tests
    src/tests/game_genie_tests.cpp
)

target_link_libraries(game_genie_tests PRIVATE mercury)

add_test(NAME game_genie_tests COMMAND game_genie_tests)

add_executable(ym2612_ssg_tests
    src/tests/ym2612_ssg_tests.cpp
)

target_link_libraries(ym2612_ssg_tests PRIVATE mercury)

add_test(NAME ym2612_ssg_tests COMMAND ym2612_ssg_tests)

add_executable(m68k_cpu_smoke_tests
    src/tests/m68k_cpu_smoke_tests.cpp
)

target_link_libraries(m68k_cpu_smoke_tests PRIVATE mercury)

add_test(NAME m68k_cpu_smoke_tests COMMAND m68k_cpu_smoke_tests)

