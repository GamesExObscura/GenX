#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <memory>
#include <windowsx.h>
#include <fstream>
#include <iomanip>
#include <mmsystem.h>
#include <shellapi.h>
#include <commdlg.h>
// The graphics card's path for pixel smoothing. GDI+ scaling is a processor
// job and costs 85ms a frame at 1080p; this costs 0.14ms - see smooth_bench.
#include <d2d1.h>
#include <dwmapi.h>
#include <shlobj.h>
// The update check: one HTTPS request to GitHub, with no library to ship.
#include <winhttp.h>
// DBT_DEVNODES_CHANGED, for noticing a controller plugged in while a game runs.
#include <dbt.h>
#include <xinput.h>
// Version 8 is the last one there is; the macro has to be set before the header.
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
// min/max are needed by GDI+ headers but NOMINMAX above removes the macros, so
// pull in the std versions under those names for the duration of the include.
namespace Gdiplus {
using std::min;
using std::max;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <array>
#include <chrono>
#include <commctrl.h>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <algorithm>
#include <cstdio>

#include "mercury/core/crosshair.hpp"
#include "mercury/core/game_genie.hpp"
#include "mercury/gamegear/game_gear.hpp"
#include "mercury/gamegear/gear_link_session.hpp"
#include "mercury/genesis/genesis.hpp"
#include "mercury/net/genesis_seats.hpp"
#include "mercury/net/netplay.hpp"
#include "mercury/segacd/bios.hpp"
#include "runtime_resources.hpp"

#pragma comment(lib, "winmm.lib")

namespace {

using mercury::gamegear::Button;
using mercury::gamegear::GameGear;
using mercury::gamegear::Vdp;

constexpr wchar_t kWindowClassName[] = L"MercuryRuntimeWindow";
// The Mega Drive's picture is already 320 wide, so the same multiplier that
// suits a 160-pixel Game Gear screen would overflow most displays.
constexpr int kWindowScale = 4;
constexpr int kGenesisWindowScale = 2;
constexpr int kFrameBudgetSteps = 400000;
constexpr std::size_t kAudioBufferCount = 4;
constexpr std::size_t kAudioChunkSamples = 512;
constexpr int kRuntimeAudioGain = 4;

// The idle window with no cartridge in it: dark grey by default, white in the
// light theme, with the logo centred on top. Sizes are in pixels at 100% DPI.
constexpr int kIdleWindowWidth = 720;
constexpr int kIdleWindowHeight = 540;
// How much of the shorter client dimension the logo is allowed to take. Any
// larger and it crowds the menu bar on a small window.
constexpr double kLogoFillFraction = 0.62;

constexpr std::size_t kRecentSlots = 10;

// How close to the top of a fullscreen display the pointer has to come before
// the menu bar is revealed, and how long the pointer stays visible after it
// stops moving.
constexpr int kFullscreenMenuReveal = 4;
constexpr int kCursorIdleFrames = 120;

struct Theme {
    COLORREF background;
    COLORREF text;
};

constexpr Theme kDarkTheme{RGB(45, 45, 48), RGB(210, 210, 214)};
constexpr Theme kLightTheme{RGB(255, 255, 255), RGB(60, 60, 66)};

struct AudioBuffer {
    WAVEHDR header{};
    std::vector<std::int16_t> samples;
    bool prepared = false;
    bool submitted = false;
};

// Which machine the ROM on the command line asked for. Chosen from the file
// extension, then from the Mega Drive header - a .bin could be either, and the
// "SEGA" signature at $100 settles it.
enum class Machine { game_gear, genesis };

// WHICH CONSOLE OF A GEAR LINK PAIR IS ON SCREEN.
//
// Two windows side by side is the right default - two people at one desk,
// each watching their own - but a lot of link games give BOTH players the
// same picture, and then the second window is the same thing again at half
// the size. Both Mortal Kombats are like that, and so is Batter Up.
//
// Over a network it is not a preference at all: your opponent's console runs
// on your machine too, and there is nothing to gain from watching it. The
// link switches to your own side as soon as the session knows which that is.
enum class LinkView { both, first, second };

// One Game Genie code in the library, and which games it is for.
//
// The list of games is the point of it: the same code can be right for
// several games - a series often shares code - and a code typed with nothing
// loaded has to go somewhere. An EMPTY list means "every game", which is
// both the sensible default and what makes a code entered before a game is
// opened still work once one is.
struct LibraryCheat {
    mercury::core::Cheat cheat;
    std::vector<std::string> games;
};

// How the picture is shaped inside whatever area it has. "stretch" fills the
// area exactly, which with a window sized to a whole multiple of the source is
// the same square-pixel output the runtime has always produced - so it stays
// the default and nothing changes unless a shape is chosen.
enum class Aspect { stretch, four_three, sixteen_nine, custom };

// --- input model -----------------------------------------------------------

// Deliberately the same order and the same values as mercury::genesis::Button,
// so a binding index casts straight across with no lookup table to fall out of
// step. The Input tab lists them in a friendlier order of its own.
enum class PadButton : int {
    up, down, left, right, a, b, c, start, x, y, z, mode,
    // Past the end of the pad, and NOT castable to a mercury Button - these two
    // have no pin of their own. The trigger ends up on the port's A line and
    // the reload is a trigger pull with the gun pointed away, so both are
    // translated in apply_pad_button rather than passed straight through.
    //
    // They get their own slots instead of reusing A, so that switching a player
    // between a pad and a gun does not silently rebind one from the other, and
    // so the gun can default to the mouse while the pad defaults to the
    // keyboard.
    gun_trigger, gun_reload,
    count
};

constexpr int kPadButtonCount = static_cast<int>(PadButton::count);

// The order the Input tab reads them out in - directions, then the face rows,
// then Start and Mode - which is not the order the hardware numbers them. The
// gun rows come first because on a gun they are the only ones that matter, and
// on a pad they are filtered out by kind_has_button and cost nothing.
constexpr PadButton kPadButtonDisplayOrder[kPadButtonCount] = {
    PadButton::gun_trigger, PadButton::gun_reload,
    PadButton::up, PadButton::down, PadButton::left, PadButton::right,
    PadButton::a,  PadButton::b,    PadButton::c,
    PadButton::x,  PadButton::y,    PadButton::z,
    PadButton::start, PadButton::mode,
};

const wchar_t* pad_button_name(PadButton button) {
    switch (button) {
        case PadButton::up: return L"Up";
        case PadButton::down: return L"Down";
        case PadButton::left: return L"Left";
        case PadButton::right: return L"Right";
        case PadButton::a: return L"A";
        case PadButton::b: return L"B";
        case PadButton::c: return L"C";
        case PadButton::start: return L"Start";
        case PadButton::x: return L"X";
        case PadButton::y: return L"Y";
        case PadButton::z: return L"Z";
        case PadButton::mode: return L"Mode";
        case PadButton::gun_trigger: return L"Trigger";
        case PadButton::gun_reload: return L"Reload";
        default: return L"?";
    }
}

// What is plugged into a port. A port holds one of these, never two, so the
// guns share the list with the pads rather than sitting in a parallel setting.
// The Master System's Light Phaser used to hold the third slot and has been
// replaced by the Konami Justifier, which is the gun Lethal Enforcers, Lethal
// Enforcers II and Snatcher use. The Phaser was never readable by a Mega Drive
// game at all, so it was a choice that could not do anything; the Justifier is
// the gun that was actually missing. Menacer keeps its number so an existing
// config still loads as a Menacer.
// miracle_piano is appended last so every kind already written into a
// settings file keeps its number.
enum class PadKind { three_button, six_button, justifier, menacer, miracle_piano };
// Where a player's input comes from, named after the API that reads it rather
// than after the cable. USB and Bluetooth were the wrong distinction: neither
// API can tell them apart, because the transport is invisible to both. What
// actually differs is which driver model a pad speaks - XInput for anything
// Xbox-compatible, DirectInput for everything older or stranger.
//
// Mouse is not a choice at all: it is what a light gun is, so choosing one
// forces it and the dropdown greys out.
enum class InputDevice { keyboard, xinput, dinput, mouse };

bool kind_is_light_gun(PadKind kind) {
    return kind == PadKind::justifier || kind == PadKind::menacer;
}

// Whether to replace the mouse pointer with a drawn crosshair. Only the
// Justifier gets one. Every Menacer game draws a crosshair of its own from the
// position it reads, and a second one on top is worse than useless: the two do
// not sit in the same place unless the emulation is perfect, so the drawn one
// becomes a thing to distrust rather than something to aim with. The Justifier
// games - Lethal Enforcers I and II, Snatcher - draw nothing, which is exactly
// why they need one.
bool kind_draws_crosshair(PadKind kind) { return kind == PadKind::justifier; }

// The other half of that rule: a gun whose GAME draws the sight wants no
// pointer at all. An arrow sitting next to the game's own crosshair is just a
// second thing on screen that is not the sight, so the Menacer hides it
// outright rather than waiting for the idle timer the hide-cursor setting uses.
bool kind_hides_pointer(PadKind kind) { return kind == PadKind::menacer; }

// A six-button pad exposes X, Y, Z and Mode; a three-button one does not, and
// the Input tab greys those rows out rather than accepting a binding that can
// never fire.
bool kind_has_six_buttons(PadKind kind) { return kind == PadKind::six_button; }

// Which buttons a peripheral actually has, and therefore which rows the Input
// tab offers. On both guns the aim and the trigger are the mouse; what differs
// is what else is on the body. The Menacer carries three more buttons - B, C
// and Start - that a game can read. The Justifier carries one, Start, which is
// how Lethal Enforcers is started and paused without putting the gun down.
bool kind_has_button(PadKind kind, PadButton button) {
    // Every gun has a trigger and a reload, and both are bindable. They used to
    // be hard-wired to the left and right mouse buttons, which a Wii remote in
    // a gun holder gets backwards: the trigger under the shell is B, and a
    // Dolphin Bar sends that as the RIGHT click.
    if (kind_is_light_gun(kind)) {
        if (button == PadButton::gun_trigger || button == PadButton::gun_reload) {
            return true;
        }
    } else if (button == PadButton::gun_trigger || button == PadButton::gun_reload) {
        return false;
    }
    if (kind == PadKind::justifier) {
        return button == PadButton::start;
    }
    if (kind == PadKind::menacer) {
        return button == PadButton::b || button == PadButton::c || button == PadButton::start;
    }
    // The keyboard has no bindable buttons at all: it is played with a
    // pointer or a fingertip in its own window, and 49 keys are not something
    // anyone would sit and bind one at a time.
    if (kind == PadKind::miracle_piano) {
        return false;
    }
    if (kind_has_six_buttons(kind)) {
        return true;
    }
    return button != PadButton::x && button != PadButton::y && button != PadButton::z &&
           button != PadButton::mode;
}

// --- gamepads --------------------------------------------------------------
// XInput only, deliberately. Practically every pad sold in the last fifteen
// years presents as an Xbox-compatible device, and XInput cannot see the
// difference between one plugged in and one paired over Bluetooth - the
// transport is invisible to the API. That is why "USB controller" and
// "Bluetooth controller" behave identically here: they are the same devices as
// far as Windows is concerned, and pretending otherwise would be a lie in a
// dropdown.
//
// A binding is one of these codes. The low sixteen bits are XInput's own button
// mask so they pass straight through; the rest are the analogue inputs given
// pseudo-button codes so that everything the pad can do is bindable as a single
// integer.
using PadInput = std::uint32_t;
constexpr PadInput kPadInputNone = 0;
constexpr PadInput kPadInputStickUp = 1u << 16;
constexpr PadInput kPadInputStickDown = 1u << 17;
constexpr PadInput kPadInputStickLeft = 1u << 18;
constexpr PadInput kPadInputStickRight = 1u << 19;
constexpr PadInput kPadInputLeftTrigger = 1u << 20;
constexpr PadInput kPadInputRightTrigger = 1u << 21;

// DirectInput has far more inputs than fit in a mask, so its codes are plain
// numbers in their own ranges rather than bits. They never mix with the XInput
// codes above: a player's DirectInput bindings live in a separate array, so
// switching driver models does not reinterpret one as the other.
constexpr PadInput kDiButtonBase = 1;      // + button index, 0-127
constexpr PadInput kDiPovBase = 200;       // + 0 up, 1 right, 2 down, 3 left
constexpr PadInput kDiAxisBase = 300;      // + axis * 2, +1 for the positive end
constexpr int kDiAxisCount = 6;            // X, Y, Z, Rx, Ry, Rz

struct PlayerInput {
    PadKind kind = PadKind::three_button;
    InputDevice device = InputDevice::keyboard;
    // Virtual-key code per button, indexed by PadButton. 0 means unbound.
    std::array<UINT, kPadButtonCount> keys{};
    // One binding set per driver model, kept alongside the keyboard one rather
    // than replacing it: switching the device dropdown back and forth must not
    // throw away the bindings for the others.
    std::array<PadInput, kPadButtonCount> pad{};
    std::array<PadInput, kPadButtonCount> dpad{};
    // Which device slot this player reads, 0-3, in whichever model is selected.
    int pad_slot = 0;
};

bool device_is_gamepad(InputDevice device) {
    return device == InputDevice::xinput || device == InputDevice::dinput;
}

// --- hotkeys ---------------------------------------------------------------

// Unscoped on purpose: these index an array, and a scoped enum would need a
// cast at every use for nothing gained.
enum HotkeyAction {
    kHkOpenGenesis, kHkQuit,
    kHkPause, kHkSoftReset, kHkHardReset, kHkPowerOff,
    kHkFullscreen, kHkCrtFilter,
    kHkTurbo, kHkFastForward, kHkRewind,
    kHkDumpVideo,
    kHkSaveStateFirst,
    kHkLoadStateFirst = kHkSaveStateFirst + 10,
    kHkStateEnd = kHkLoadStateFirst + 10,
    // Appended AFTER the ten save and ten load slots, so those keep the numbers
    // they are stored under and an existing settings file still reads. Adding
    // them in the obvious place, next to the other Open action, would have
    // shifted twenty bindings by three.
    kHkOpenGameGear = kHkStateEnd,
    kHkOpen32X,
    kHkOpenCd,
    kHkDumpDiagnostics,
    // Appended for the same reason as the block above: a new action goes on
    // the end so every binding already in a settings file keeps its index.
    kHkMiraclePiano,
    kHkCount
};

// A chord. Zero key means unbound, which is a real setting - a hotkey the user
// has cleared should stay cleared rather than quietly reverting to its default.
struct Hotkey {
    UINT key = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

bool hotkey_bound(const Hotkey& binding) { return binding.key != 0; }

bool hotkey_same(const Hotkey& a, const Hotkey& b) {
    return a.key == b.key && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt;
}

// Held rather than tapped: these do something for as long as the key is down,
// so the dispatcher must not swallow the key-up.
bool hotkey_is_held(int action) {
    return action == kHkTurbo || action == kHkFastForward || action == kHkRewind;
}

std::wstring hotkey_action_name(int action) {
    // kHkStateEnd, not kHkCount: the actions added after the state slots would
    // otherwise all be named "Load state 11" and up.
    if (action >= kHkLoadStateFirst && action < kHkStateEnd) {
        return L"Load state " + std::to_wstring(action - kHkLoadStateFirst + 1);
    }
    if (action >= kHkSaveStateFirst && action < kHkLoadStateFirst) {
        return L"Save state " + std::to_wstring(action - kHkSaveStateFirst + 1);
    }
    switch (action) {
        case kHkOpenGenesis: return L"Open Genesis ROM";
        case kHkOpenGameGear: return L"Open Game Gear ROM";
        case kHkOpen32X: return L"Open 32X ROM";
        case kHkOpenCd: return L"Open Sega CD image";
        case kHkDumpDiagnostics: return L"Write diagnostic log";
        case kHkQuit: return L"Quit GenX";
        case kHkPause: return L"Play / pause";
        case kHkSoftReset: return L"Soft reset";
        case kHkHardReset: return L"Hard reset";
        case kHkPowerOff: return L"Power off";
        case kHkFullscreen: return L"Toggle fullscreen";
        case kHkCrtFilter: return L"Toggle CRT filter";
        case kHkTurbo: return L"Turbo (auto-fire, hold)";
        case kHkFastForward: return L"Fast forward (hold)";
        case kHkRewind: return L"Rewind (hold)";
        case kHkDumpVideo: return L"Dump video state";
        case kHkMiraclePiano: return L"Miracle Piano keyboard";
        default: return L"?";
    }
}

void set_default_hotkeys(std::array<Hotkey, kHkCount>& keys) {
    keys = {};
    // One per system, named after the system rather than after "open", since
    // there is no longer a single Open ROM to bind.
    keys[kHkOpenGenesis] = Hotkey{'G', true, false, false};
    keys[kHkOpenGameGear] = Hotkey{'M', true, false, false};
    keys[kHkOpen32X] = Hotkey{'X', true, false, false};
    keys[kHkOpenCd] = Hotkey{'C', true, false, false};
    keys[kHkDumpDiagnostics] = Hotkey{'L', true, false, false};
    // Plain M, as asked. Ctrl+M is Open Game Gear, so the two do not collide.
    keys[kHkMiraclePiano] = Hotkey{'M', false, false, false};
    keys[kHkQuit] = Hotkey{VK_ESCAPE, false, false, false};
    keys[kHkPause] = Hotkey{'P', true, false, false};
    keys[kHkSoftReset] = Hotkey{'R', true, false, false};
    keys[kHkHardReset] = Hotkey{'R', true, true, false};
    keys[kHkPowerOff] = Hotkey{'W', true, false, false};
    keys[kHkFullscreen] = Hotkey{VK_RETURN, false, false, true};
    keys[kHkCrtFilter] = Hotkey{'T', true, false, false};
    keys[kHkTurbo] = Hotkey{VK_OEM_2, false, false, false};       // '/'
    keys[kHkFastForward] = Hotkey{VK_OEM_PERIOD, false, false, false};
    keys[kHkRewind] = Hotkey{VK_OEM_COMMA, false, false, false};
    keys[kHkDumpVideo] = Hotkey{VK_F12, false, false, false};
    for (int slot = 0; slot < 10; ++slot) {
        // 1-9 then 0, matching how the menus number the slots.
        const UINT digit = slot == 9 ? '0' : static_cast<UINT>('1' + slot);
        keys[kHkSaveStateFirst + slot] = Hotkey{digit, false, true, false};
        keys[kHkLoadStateFirst + slot] = Hotkey{digit, false, false, false};
    }
}

// Windows knows the name of every key in the user's own layout and language;
// spelling them out here would be a table that is wrong on half the world's
// keyboards. The extended-key bit matters - without it the arrow keys come back
// named as their numeric-keypad twins.
std::wstring key_display_name(UINT key) {
    if (key == 0) {
        return L"(none)";
    }
    // Mouse buttons live in the same virtual-key space as the keyboard, which
    // is what lets a gun's trigger be bound with the ordinary key machinery.
    // Windows has no names for them, so they are spelled out here.
    switch (key) {
        case VK_LBUTTON: return L"Left mouse";
        case VK_RBUTTON: return L"Right mouse";
        case VK_MBUTTON: return L"Middle mouse";
        case VK_XBUTTON1: return L"Mouse 4";
        case VK_XBUTTON2: return L"Mouse 5";
        default: break;
    }
    UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    switch (key) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            scan |= 0x100;
            break;
        default:
            break;
    }
    wchar_t name[64] = L"";
    if (GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) {
        return name;
    }
    return L"Key " + std::to_wstring(key);
}

std::wstring hotkey_display_name(const Hotkey& binding) {
    if (!hotkey_bound(binding)) {
        return L"(none)";
    }
    std::wstring text;
    if (binding.ctrl) text += L"Ctrl + ";
    if (binding.shift) text += L"Shift + ";
    if (binding.alt) text += L"Alt + ";
    text += key_display_name(binding.key);
    return text;
}

// Which folder a machine's cartridge saves go in. 32X and Sega CD are stored
// but unused: neither machine exists yet, and the setting was asked for so the
// paths are in place when they do.
enum SaveFolder { kSaveGenesis, kSaveGameGear, kSave32X, kSaveCd, kSaveFolderCount };

struct RuntimeState {
    // Neither copyable nor movable, and the compiler should say so. Genesis
    // wires [this] lambdas into the 68000 and Z80 bus structs and into the VDP's
    // memory reader in its constructor; moving a RuntimeState carries those
    // lambdas across with the OLD this still captured, and every bus access
    // afterwards runs against a destroyed object. That was written once - a
    // "load into a fresh state and move it in" cartridge loader - and the
    // symptom was simply that no game would load. Loading happens IN PLACE.
    RuntimeState() = default;
    RuntimeState(const RuntimeState&) = delete;
    RuntimeState& operator=(const RuntimeState&) = delete;
    RuntimeState(RuntimeState&&) = delete;
    RuntimeState& operator=(RuntimeState&&) = delete;

    // Which preferences file this window reads and writes. The Gear Link
    // second console gets its own, so a controller bound to it there does not
    // land on top of the first window's bindings.
    std::wstring settings_name = L"settings.txt";
    // True for the right-hand console of a Gear Link pair. It has no library
    // of its own to browse and does not own the pair - closing it is the left
    // window's job - so a few menu items are hidden and a few paths skipped.
    bool link_secondary = false;

    Machine machine = Machine::game_gear;
    GameGear game_gear;
    mercury::genesis::Genesis genesis;
    std::filesystem::path rom_path;
    // The runtime now starts with no cartridge and waits at the logo screen, so
    // every emulation path has to ask whether one is loaded first.
    bool rom_loaded = false;
    bool paused = false;
    bool dark_theme = true;
    bool hide_cursor = false;
    // Ask GitHub for the newest release at startup. The answer arrives on the
    // window as a message; the flag stops a second request starting while one
    // is still out.
    bool check_for_updates = true;
    bool update_check_running = false;
    int view_scale = 0;  // 0 = the per-machine default
    Aspect aspect = Aspect::stretch;
    int custom_aspect_width = 16;
    int custom_aspect_height = 10;
    bool fullscreen = false;
    bool start_maximized = false;
    // The windowed position and style, kept so leaving fullscreen puts the
    // window back exactly where it was rather than at a default size.
    WINDOWPLACEMENT windowed_placement{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowed_style = 0;
    // Set while Windows is running a menu or a window drag, both of which run
    // their own message loop and would otherwise stall emulation until the user
    // let go. A timer drives frames for the duration.
    bool in_modal_loop = false;
    bool stepping_frame = false;
    // Shared with the modal-loop timer so both pace against the same clock.
    std::chrono::steady_clock::time_point next_frame_deadline{};
    HMENU menu = nullptr;
    HMENU recent_menu = nullptr;
    HMENU load_state_menu = nullptr;
    HMENU save_state_menu = nullptr;
    HMENU delete_state_menu = nullptr;
    // Shown in the title bar for a moment after a save or a load, so the action
    // is confirmed without a dialog to dismiss.
    std::wstring status_message;
    int status_frames_remaining = 0;
    // Most recent first, at most kRecentSlots, no duplicates. Persisted so the
    // list survives a restart - a recent list that empties every launch is
    // worse than none.
    std::vector<std::filesystem::path> recent;
    HINSTANCE instance = nullptr;
    // The logo, decoded once from the embedded PNG into a premultiplied BGRA
    // DIB that AlphaBlend can use directly.
    HBITMAP logo_bitmap = nullptr;
    int logo_width = 0;
    int logo_height = 0;
    // Counts down while the pointer sits still; the cursor stays visible until
    // it reaches zero, so moving the mouse during a fullscreen game brings the
    // pointer back rather than leaving the player without one.
    int cursor_idle_frames = 0;
    int battery_flush_countdown = 0;
    bool crt_filter = false;
    // Bilinear rather than nearest-neighbour when the picture is scaled up.
    // Off by default: the hardware's pixels are square and hard edged, and
    // smoothing them is a preference, not a correction.
    bool smooth_scaling = false;
    // Scratch surface for the smooth path. GDI+ interpolating straight onto a
    // screen DC runs at single-figure frame rates; the same call into a memory
    // bitmap followed by a plain blit is an order of magnitude faster. Kept
    // between frames because allocating a screen-sized bitmap sixty times a
    // second would give back everything that buys.
    HDC smooth_dc = nullptr;
    HBITMAP smooth_bitmap = nullptr;
    HBITMAP smooth_previous = nullptr;
    int smooth_width = 0;
    int smooth_height = 0;
    // The whole client area, off screen, for the GDI path while the CRT filter
    // is on: the picture, its scanlines and any crosshairs are put together
    // here and reach the window in ONE copy. See WM_PAINT.
    HDC compose_dc = nullptr;
    HBITMAP compose_bitmap = nullptr;
    HBITMAP compose_previous = nullptr;
    int compose_width = 0;
    int compose_height = 0;

    // ...and the fast path, which is the graphics card. Measured with
    // smooth_bench on this machine: scaling a Mega Drive frame to a 1080p
    // window costs 85ms through GDI+ and 0.14ms through Direct2D, and the
    // Direct2D figure does not grow with the window. 85ms is twelve frames a
    // second, which is exactly the stutter that was reported. The GDI+ surface
    // above stays as the fallback for a machine where Direct2D will not start.
    ID2D1HwndRenderTarget* d2d_target = nullptr;
    ID2D1Bitmap* d2d_picture = nullptr;
    ID2D1BitmapBrush* d2d_scanlines = nullptr;
    unsigned d2d_target_width = 0;
    unsigned d2d_target_height = 0;
    unsigned d2d_picture_width = 0;
    unsigned d2d_picture_height = 0;
    // How the live target was created: waiting for the vertical blank, or not.
    // The choice is baked in at creation, so it is remembered here to know when
    // the target has to be rebuilt. See present_smooth_d2d.
    bool d2d_present_immediately = false;
    // Frames actually presented per second, measured over a rolling second so
    // it answers "is this running at full speed" rather than "how fast could
    // it run". Counted at the point the picture goes up, so a frame the
    // emulator computed but never showed is not counted.
    // What happened to the last few keys pressed.
    //
    // A dead keyboard has several possible causes that look identical from the
    // outside - the window not having focus, a hotkey swallowing the key, no
    // cartridge, a binding filtered out by the controller kind, or no binding
    // at all - and the fault has never been reproduced on demand. So the
    // window procedure writes down what it did with each key, and the
    // diagnostic log prints it. One Ctrl+L after it happens should name the
    // cause instead of costing another round of guessing.
    struct KeyEvent {
        UINT key = 0;
        const char* outcome = nullptr;
        int delivered = 0;
    };
    std::array<KeyEvent, 16> key_history{};
    std::size_t key_history_next = 0;
    std::uint64_t keys_seen = 0;

    bool show_fps = false;
    int fps_frames = 0;
    double fps_value = 0.0;
    std::chrono::steady_clock::time_point fps_window_start{};
    bool fullscreen_menu_shown = false;
    // One physical mouse, claimed by one gun.
    //
    // Windows merges every mouse into a SINGLE system pointer, so two Wii
    // remotes on a Dolphin Bar are indistinguishable through WM_MOUSEMOVE -
    // both move the same arrow and there is no field that says which one did.
    // Raw input is the only API that reports the device, which is why two-gun
    // play needs a second input path rather than a second copy of the first.
    struct GunPointer {
        HANDLE device = nullptr;  // the physical mouse this gun has claimed
        int x = 0;                // client pixels, ours to track
        int y = 0;
        bool seen = false;
        // Raw button state, not meaning. Which one fires and which reloads is
        // whatever the player bound on the Input tab, resolved when this is
        // applied - so a Wii remote whose trigger reports as a right click
        // works the same as a mouse whose trigger is the left.
        bool left = false;
        bool right = false;
        bool middle = false;
        bool off_screen_shot = false;
        // What was last pushed into the machine, so the buttons are written on
        // CHANGE rather than every frame. Writing them every frame would hold
        // a keyboard-bound Start permanently released - the raw device's steady
        // "not pressed" would overwrite the key press before the machine ran.
        bool trigger_applied = false;
        bool start_applied = false;
    };
    std::array<GunPointer, 2> gun_pointers{};
    bool raw_input_ready = false;
    // Reloading, in every light gun game that has it, is a shot fired while the
    // gun points AWAY from the screen. Held as state rather than applied once,
    // because the aim is recomputed from the pointer every single frame and a
    // one-shot flag was being overwritten before the machine ever ran.
    bool off_screen_held = false;

    // --- netplay ---
    mercury::net::Session netplay;
    std::wstring netplay_nickname = L"player";
    std::wstring netplay_address = L"";
    int netplay_port = 47215;
    int netplay_delay = 4;
    // Counts the frames THIS session has run, which is what the two ends agree
    // on. Not the VDP's frame counter, which was already running before anyone
    // connected.
    std::uint32_t netplay_frame = 0;
    bool netplay_stalled = false;
    // The session runs on its own thread, so its progress only reaches the
    // window when something pushes it there. Without this the title sits on
    // whatever it said the moment the session started - "connecting..." for a
    // session that connected ten seconds ago, which reads as a hang.
    std::wstring netplay_label;
    // What THIS player is physically holding, kept apart from the ports.
    //
    // Reading it back out of port 1 does not work and was the first attempt:
    // netplay overwrites both ports every frame with the agreed, delayed
    // inputs, so the next frame's "local input" was actually the value netplay
    // had just written. Every press then fed back into itself and stuck on.
    std::uint16_t netplay_local_mask = 0;
    // What is plugged into the two seats for this session - the HOST's choice,
    // at both ends - and whether it has been plugged in yet. Each computer's
    // own Input tab is put back when the session ends. See netplay_step.
    bool netplay_seats_applied = false;
    std::array<PadKind, 2> netplay_seat_kinds{};
    // Both seats' inputs as last applied, which is where the OTHER player's
    // gun is aimed: their sight is drawn from this.
    std::array<mercury::net::PlayerInput, 2> netplay_seat_inputs{};
    // The mouse's buttons as Windows reports them - logical, so a left-handed
    // mouse fires with the button its owner calls "left". Tracked always,
    // because the two-gun path stops these messages reaching the bindings.
    bool mouse_left_down = false;
    bool mouse_right_down = false;
    bool mouse_middle_down = false;
    // Two players' worth of bindings and what each is holding. Player 2 starts
    // unbound rather than sharing the keyboard with player 1 by default - a
    // silent collision on the same keys is worse than nothing happening.
    std::array<PlayerInput, 2> players{};
    // The saved profile each player last picked (or saved), shown selected in
    // the Input page the next time it opens. Empty means "(current)". A label
    // only: the bindings themselves are saved in full either way.
    std::array<std::wstring, 2> profile_names{};
    // Two people, one Game Gear.
    //
    // A Game Gear has no controller port: the pad is moulded into the machine.
    // So a two-player game on ONE console has no choice but to split the
    // buttons that are there, which is exactly what Micro Machines 1 and 2 do -
    // one player steers on the d-pad while the other drives with buttons 1
    // and 2.
    //
    // Reproducing that with two pads means both feed the same console and are
    // COMBINED rather than overwriting each other. There is one physical
    // button 1, so either player pressing it presses it, and player one
    // releasing must not cancel player two's press. Held per player for that
    // reason; the machine sees the OR.
    std::array<std::array<bool, kPadButtonCount>, 2> shared_pad_held{};

    // What each player's pad was holding last frame, so only changes are sent
    // to the machine, and whether that pad is actually plugged in.
    std::array<PadInput, 2> pad_previous{};
    std::array<DIJOYSTATE2, 2> dinput_previous{};
    std::array<bool, 2> pad_connected{};
    // Following a pad that moves. See find_moved_pad.
    std::array<std::wstring, 2> dinput_pad_name{};
    std::array<int, 2> pad_search_countdown{};
    std::array<Hotkey, kHkCount> hotkeys{};
    // 0-100, applied on top of the per-machine gain.
    int volume = 100;
    bool muted = false;
    // This console is one half of a Gear Link that is showing only the other
    // half. It keeps running - the cable needs both - but its window is
    // hidden and it makes no sound. Not persisted: it belongs to a link, not
    // to the machine. See apply_gear_link_view.
    bool hidden_by_view = false;
    // The console's output-stage low-pass. On by default because that is what
    // the hardware does; a toggle because the difference is large enough to
    // want to hear both ways.
    bool console_filter = true;
    double lowpass_state = 0.0;
    // Diagnostic per-source mutes from the Tools menu. Not persisted: leaving
    // a chip muted across a restart would look like a bug in the emulator.
    bool mute_fm = false;
    bool mute_psg = false;
    bool mute_dac = false;
    // The 32X's PWM. Worth its own switch for the same reason as the others:
    // on a 32X cartridge it can be the whole soundtrack, so "is this coming
    // from the adapter or from the Mega Drive?" is the first question.
    bool mute_pwm = false;
    // The Sega CD's PCM chip and disc audio together.
    bool mute_cd = false;
    // WAV capture of the mixed output. "It sounds stretched" is not something
    // anyone can act on; the same sound as a waveform can be measured against a
    // recording from real hardware or another emulator.
    std::ofstream recording;
    std::uint32_t recorded_samples = 0;
    std::filesystem::path recording_path;
    // Register-write log, drained to a text file once a frame. Paired with a
    // WAV of the same take it says whether a wrong sound was wrongly asked for
    // or wrongly played.
    std::ofstream fm_log;
    std::filesystem::path fm_log_path;
    // The DAC log is a filtered view of the SAME chip log the FM one drains, so
    // it costs no extra machinery in the chip: the DAC is register 0x2A of
    // bank 0 and nothing else.
    std::ofstream dac_log;
    std::filesystem::path dac_log_path;
    std::ofstream psg_log;
    std::filesystem::path psg_log_path;
    // One per system, indexed by the same SaveFolder enum the save paths use,
    // so Open ROM can start in the right library rather than in whichever one
    // was browsed last.
    std::array<std::filesystem::path, kSaveFolderCount> games_folders{};
    std::array<std::filesystem::path, kSaveFolderCount> save_folders{};
    // Where the 32X firmware images are, if the user has said. Optional: empty
    // leaves the adapter looking for a BIOS folder beside the cartridge, and
    // failing that booting on its synthesised startup. Not part of the two
    // arrays above because it is not per-system storage - it is one folder for
    // one adapter, and Sega CD will want its own rather than an index here.
    std::filesystem::path bios_folder_32x{};
    // Where the Sega CD BIOS images are. NOT optional the way the 32X's is:
    // a Sega CD cannot start without one. Empty still works if there is a
    // folder called BIOS near the disc; otherwise opening a disc says so.
    std::filesystem::path bios_folder_segacd{};
    // The whole Game Genie library and the master switch. ONE list for every
    // game, each code carrying which games it is for - see
    // cheat_library_path. It is loaded at startup and saved on every change,
    // including with no game open.
    std::vector<LibraryCheat> cheats;
    bool cheats_on = true;
    // What the Game Gear's bus watches. Rebuilt from `cheats` whenever the
    // list or the switch changes; a Mega Drive code is not in here, because
    // it is written straight into the cartridge image instead.
    mercury::core::CheatMap cheat_map;
    // Said once per game, not once per frame - see step_until_next_frame_genesis.
    bool warned_no_32x_bios = false;
    // Held-hotkey state, cleared on focus loss along with the pad buttons.
    bool turbo_held = false;
    bool fast_forward_held = false;
    bool rewind_held = false;
    // Flips every frame; turbo presses the face buttons on the true half.
    bool turbo_phase = false;
    // Oldest first. Cleared whenever the machine is replaced under it - a
    // snapshot from another cartridge would be refused on load, but one from
    // before a reset would silently undo the reset.
    std::deque<std::vector<std::uint8_t>> rewind_history;
    int rewind_countdown = 0;
    std::vector<std::uint8_t> rgb_frame;
    std::vector<std::uint32_t> bgra_frame;
    std::string status_text = "Booting";
    bool running = true;
    bool faulted = false;
    std::uint32_t last_presented_frame = 0;
    bool has_visible_frame = false;
    std::uint32_t start_pulse_frames_remaining = 0;
    // Last size the window was sized for, so a mid-run display-height change
    // can be noticed and acted on.
    std::size_t presented_width = 0;
    std::size_t presented_height = 0;
    bool resize_pending = false;
    HWND window = nullptr;
    HWAVEOUT wave_out = nullptr;
    std::array<AudioBuffer, kAudioBufferCount> audio_buffers{};
    std::vector<std::int16_t> pending_audio;
    // The Game Gear's PSG output is quiet enough to want a boost; the Mega
    // Drive's FM chip is not, so the gain is per machine rather than a constant.
    int audio_gain = kRuntimeAudioGain;
    // Carried between frames so resampling the YM2612's ~53kHz output down to
    // the 44.1kHz device rate does not drift or click at the frame boundary.
    double ym_resample_phase = 0.0;
    // Last mono sample of the previous batch, so interpolation is continuous
    // across frame boundaries rather than snapping back to zero.
    int ym_previous_mono = 0;
};

constexpr auto kFrameDuration = std::chrono::duration<double>(
    (static_cast<double>(Vdp::kCpuCyclesPerScanline) *
     static_cast<double>(Vdp::kScanlinesPerFrame)) /
    mercury::gamegear::Psg::kBaseClockHz);

int window_scale(const RuntimeState& state) {
    return state.machine == Machine::genesis ? kGenesisWindowScale : kWindowScale;
}

// Kept here rather than reaching into the device header from the window code,
// so the layout maths has a plain number to work with.
constexpr int MiraclePianoKeyCount = mercury::genesis::MiraclePiano::kKeyCount;

void toggle_piano_window(RuntimeState& state);
void close_piano_window();
[[nodiscard]] bool piano_window_open();
[[nodiscard]] HWND piano_window();
// What a player's controller choice would be with the piano unplugged. The
// piano takes player two's slot while its window is open, and that takeover
// must never reach the settings file - see save_settings.
struct SavedInputChoice {
    PadKind kind = PadKind::three_button;
    InputDevice device = InputDevice::keyboard;
};
[[nodiscard]] SavedInputChoice piano_saved_input(const RuntimeState& state, int player);
[[nodiscard]] bool piano_owner_is(const RuntimeState* state);
// Adds the keyboard's own voice on top of the console's output. Declared here
// because the Mega Drive mixer calls it and is defined long before the
// synthesiser is.
void mix_piano_audio(RuntimeState& state, std::vector<std::int16_t>& samples);

// Gear Link puts a second Game Gear in a second window and joins the two with
// an emulated cable. Declared up here because window sizing and cartridge
// loading both have to ask about it; defined near the bottom, where the window
// and audio helpers it needs already exist.
bool gear_link_active();
// Which console of the pair is on screen - see apply_gear_link_view. Declared
// here because the menu has to tick the right one long before the link code.
LinkView gear_link_view();
// Netplay has just started on a linked pair: show this player's own console
// as soon as the session can say which one that is.
void gear_link_follow_netplay();
// The console at the other window of a linked pair, or nothing. Declared here
// because netplay has to carry both consoles and is defined long before.
RuntimeState* link_partner(RuntimeState& state);
// The whole linked pair as netplay sends it, and loading one - see
// GearLinkSession::save_pair_state. Declared here for the same reason.
std::vector<std::uint8_t> gear_link_save_pair();
bool gear_link_load_pair(const std::vector<std::uint8_t>& blob, std::string& error);
// The OTHER console's window, or nothing when there is no pair. Declared here
// because the diagnostic log has to name where the keys went and is defined
// long before the Gear Link.
[[nodiscard]] HWND gear_link_partner_window(const RuntimeState& state);
void gear_link_mirror_rom(RuntimeState& source, const std::filesystem::path& path);
void gear_link_mirror_unload(RuntimeState& source);

// --- XInput ----------------------------------------------------------------

// Everything the pad is holding, as one mask. The sticks and triggers are
// folded in as pseudo-buttons so a binding is a single integer whatever it is
// bound to.
PadInput read_gamepad(int slot, bool& connected) {
    XINPUT_STATE state{};
    if (XInputGetState(static_cast<DWORD>(slot), &state) != ERROR_SUCCESS) {
        connected = false;
        return kPadInputNone;
    }
    connected = true;
    PadInput mask = state.Gamepad.wButtons;

    // The left stick doubles as a d-pad. The deadzone is XInput's own
    // recommended figure - without one, a stick that rests slightly off centre
    // holds a direction down for ever.
    constexpr SHORT kDeadzone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    if (state.Gamepad.sThumbLY > kDeadzone) mask |= kPadInputStickUp;
    if (state.Gamepad.sThumbLY < -kDeadzone) mask |= kPadInputStickDown;
    if (state.Gamepad.sThumbLX < -kDeadzone) mask |= kPadInputStickLeft;
    if (state.Gamepad.sThumbLX > kDeadzone) mask |= kPadInputStickRight;
    if (state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        mask |= kPadInputLeftTrigger;
    }
    if (state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        mask |= kPadInputRightTrigger;
    }
    return mask;
}

// --- DirectInput -----------------------------------------------------------
// Everything that is not Xbox-compatible: older pads, arcade sticks, flight
// gear. Devices are enumerated once and then polled by slot, the same shape as
// the XInput side, so the rest of the runtime does not care which model a
// player is on.
struct DirectInputDevices {
    IDirectInput8W* api = nullptr;
    std::array<IDirectInputDevice8W*, 4> device{};
    std::array<std::wstring, 4> name{};
    int count = 0;
    bool tried = false;
    // Kept so a rescan can rebuild the device list without the caller having
    // to carry the module handle around.
    HINSTANCE instance = nullptr;
    // The window the cooperative level is bound to. This MUST be the main
    // window and nothing else. It used to be GetActiveWindow(), which is the
    // active window OF THE CALLING THREAD AT THAT MOMENT: null during startup
    // because the enumeration runs before the window exists, and the settings
    // dialog if a device change arrives while the dialog is open. Bound to a
    // window that is then destroyed, every later Acquire fails and DirectInput
    // goes silent for the rest of the session with nothing said.
    HWND owner = nullptr;
    // Why DirectInput is quiet, for the diagnostic log. Silence has several
    // causes - no devices, a failed acquire, a binding to nothing - and they
    // are indistinguishable from the outside.
    int acquire_failures = 0;
    int cooperative_failures = 0;
};

DirectInputDevices& dinput_devices() {
    static DirectInputDevices instance;
    return instance;
}

BOOL CALLBACK dinput_enum_callback(LPCDIDEVICEINSTANCEW instance, void* context) {
    auto& devices = *static_cast<DirectInputDevices*>(context);
    if (devices.count >= static_cast<int>(devices.device.size())) {
        return DIENUM_STOP;
    }
    IDirectInputDevice8W* device = nullptr;
    if (FAILED(devices.api->CreateDevice(instance->guidInstance, &device, nullptr)) ||
        device == nullptr) {
        return DIENUM_CONTINUE;
    }
    // c_dfDIJoystick2 is the superset format: buttons, both hats and every axis,
    // so one layout serves every device shape without asking what it is.
    if (FAILED(device->SetDataFormat(&c_dfDIJoystick2))) {
        device->Release();
        return DIENUM_CONTINUE;
    }
    // Background and non-exclusive: the pad keeps working when the window is not
    // focused, and nothing else on the system is locked out of it.
    if (FAILED(device->SetCooperativeLevel(devices.owner,
                                           DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        ++devices.cooperative_failures;
    }
    if (FAILED(device->Acquire())) {
        // Not fatal and not worth dropping the device over: read_dinput
        // re-acquires on every poll, so a pad that is busy right now starts
        // answering as soon as it is free. Counted so the log can say so.
        ++devices.acquire_failures;
    }
    devices.name[devices.count] = instance->tszProductName;
    devices.device[devices.count] = device;
    ++devices.count;
    return DIENUM_CONTINUE;
}

void open_dinput(HINSTANCE instance) {
    auto& devices = dinput_devices();
    if (devices.tried) {
        return;
    }
    devices.tried = true;
    devices.instance = instance;
    if (FAILED(DirectInput8Create(instance, DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(&devices.api), nullptr)) ||
        devices.api == nullptr) {
        return;
    }
    devices.api->EnumDevices(DI8DEVCLASS_GAMECTRL, dinput_enum_callback, &devices,
                             DIEDFL_ATTACHEDONLY);
}

// Hand DirectInput the window it should belong to, once that window exists.
//
// Enumeration happens before the main window is created - it has to, because
// the Input tab needs a device list the moment it opens - so the devices are
// first bound to nothing. This re-binds them to the real window and is also
// what a rescan uses, so the binding can never drift onto a dialog.
void set_dinput_owner(HWND owner) {
    auto& devices = dinput_devices();
    if (owner == nullptr || devices.owner == owner) {
        return;
    }
    devices.owner = owner;
    for (auto* device : devices.device) {
        if (device == nullptr) {
            continue;
        }
        // Unacquire first: the cooperative level cannot be changed on an
        // acquired device, and failing silently here is how this went wrong in
        // the first place.
        device->Unacquire();
        if (FAILED(device->SetCooperativeLevel(owner, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
            ++devices.cooperative_failures;
        }
        if (FAILED(device->Acquire())) {
            ++devices.acquire_failures;
        }
    }
}

// Look for controllers again.
//
// XInput needs none of this - it is four fixed slots polled every frame, so a
// pad plugged in mid-game simply starts answering. DirectInput is the opposite:
// devices are found by an enumeration, and one that happens at startup can only
// ever see what was already plugged in. Without a rescan, a DirectInput pad
// connected after launch does not exist as far as the emulator is concerned.
//
// Slots are rebuilt in enumeration order, which is the same order they would
// have had at startup, so a player's chosen slot keeps meaning the same device
// as long as the set of attached pads is the same.
void rescan_dinput() {
    auto& devices = dinput_devices();
    if (!devices.tried || devices.api == nullptr) {
        return;
    }
    for (auto*& device : devices.device) {
        if (device != nullptr) {
            device->Unacquire();
            device->Release();
            device = nullptr;
        }
    }
    for (auto& name : devices.name) {
        name.clear();
    }
    devices.count = 0;
    devices.api->EnumDevices(DI8DEVCLASS_GAMECTRL, dinput_enum_callback, &devices,
                             DIEDFL_ATTACHEDONLY);
}

void close_dinput() {
    auto& devices = dinput_devices();
    for (auto*& device : devices.device) {
        if (device != nullptr) {
            device->Unacquire();
            device->Release();
            device = nullptr;
        }
    }
    if (devices.api != nullptr) {
        devices.api->Release();
        devices.api = nullptr;
    }
    devices.count = 0;
}

bool read_dinput(int slot, DIJOYSTATE2& out) {
    auto& devices = dinput_devices();
    if (slot < 0 || slot >= static_cast<int>(devices.device.size())) {
        return false;
    }
    IDirectInputDevice8W* device = devices.device[slot];
    if (device == nullptr) {
        return false;
    }
    // A device can be lost when another application takes it; re-acquiring is
    // the documented recovery and costs nothing when it is not needed.
    if (FAILED(device->Poll())) {
        if (FAILED(device->Acquire())) {
            return false;
        }
        device->Poll();
    }
    if (FAILED(device->GetDeviceState(sizeof(DIJOYSTATE2), &out))) {
        return false;
    }
    return true;
}

// DirectInput axes are reported over the range this asks for; half scale is the
// centre, so "pressed" is a good way off it.
constexpr LONG kDiAxisRange = 1000;
constexpr LONG kDiAxisThreshold = 500;

LONG dinput_axis(const DIJOYSTATE2& state, int axis) {
    switch (axis) {
        case 0: return state.lX;
        case 1: return state.lY;
        case 2: return state.lZ;
        case 3: return state.lRx;
        case 4: return state.lRy;
        default: return state.lRz;
    }
}

bool dinput_active(const DIJOYSTATE2& state, PadInput code) {
    if (code == kPadInputNone) {
        return false;
    }
    if (code >= kDiAxisBase) {
        const int index = static_cast<int>(code - kDiAxisBase);
        const int axis = index / 2;
        if (axis >= kDiAxisCount) {
            return false;
        }
        const LONG value = dinput_axis(state, axis);
        return (index % 2) != 0 ? value > kDiAxisThreshold : value < -kDiAxisThreshold;
    }
    if (code >= kDiPovBase) {
        const DWORD pov = state.rgdwPOV[0];
        // A centred hat reports every bit set in its low word.
        if (LOWORD(pov) == 0xFFFF) {
            return false;
        }
        // Hundredths of a degree, clockwise from north. A direction counts when
        // the hat is within 45 degrees of it, so diagonals press both.
        const int degrees = static_cast<int>(pov / 100);
        switch (code - kDiPovBase) {
            case 0: return degrees > 315 || degrees < 45;
            case 1: return degrees > 45 && degrees < 135;
            case 2: return degrees > 135 && degrees < 225;
            default: return degrees > 225 && degrees < 315;
        }
    }
    const int button = static_cast<int>(code - kDiButtonBase);
    if (button < 0 || button >= 128) {
        return false;
    }
    return (state.rgbButtons[button] & 0x80) != 0;
}

std::wstring dinput_input_name(PadInput code) {
    if (code == kPadInputNone) {
        return L"(none)";
    }
    if (code >= kDiAxisBase) {
        static const wchar_t* axes[kDiAxisCount] = {L"X", L"Y", L"Z", L"Rx", L"Ry", L"Rz"};
        const int index = static_cast<int>(code - kDiAxisBase);
        if (index / 2 >= kDiAxisCount) {
            return L"axis?";
        }
        return std::wstring(axes[index / 2]) + ((index % 2) != 0 ? L"+" : L"-");
    }
    if (code >= kDiPovBase) {
        static const wchar_t* hats[4] = {L"Hat Up", L"Hat Right", L"Hat Down", L"Hat Left"};
        return hats[(code - kDiPovBase) & 3];
    }
    return L"Button " + std::to_wstring(code - kDiButtonBase + 1);
}

// The first input on a DirectInput device that is pressed now and was not
// pressed in the baseline. Used for binding.
PadInput first_dinput_press(const DIJOYSTATE2& now, const DIJOYSTATE2& baseline) {
    for (int button = 0; button < 128; ++button) {
        if ((now.rgbButtons[button] & 0x80) != 0 && (baseline.rgbButtons[button] & 0x80) == 0) {
            return kDiButtonBase + static_cast<PadInput>(button);
        }
    }
    for (PadInput direction = 0; direction < 4; ++direction) {
        const PadInput code = kDiPovBase + direction;
        if (dinput_active(now, code) && !dinput_active(baseline, code)) {
            return code;
        }
    }
    for (int index = 0; index < kDiAxisCount * 2; ++index) {
        const PadInput code = kDiAxisBase + static_cast<PadInput>(index);
        if (dinput_active(now, code) && !dinput_active(baseline, code)) {
            return code;
        }
    }
    return kPadInputNone;
}

std::wstring pad_input_name(PadInput input) {
    if (input == kPadInputNone) {
        return L"(none)";
    }
    switch (input) {
        case XINPUT_GAMEPAD_DPAD_UP: return L"D-pad Up";
        case XINPUT_GAMEPAD_DPAD_DOWN: return L"D-pad Down";
        case XINPUT_GAMEPAD_DPAD_LEFT: return L"D-pad Left";
        case XINPUT_GAMEPAD_DPAD_RIGHT: return L"D-pad Right";
        case XINPUT_GAMEPAD_START: return L"Start";
        case XINPUT_GAMEPAD_BACK: return L"Back";
        case XINPUT_GAMEPAD_LEFT_THUMB: return L"L3";
        case XINPUT_GAMEPAD_RIGHT_THUMB: return L"R3";
        case XINPUT_GAMEPAD_LEFT_SHOULDER: return L"LB";
        case XINPUT_GAMEPAD_RIGHT_SHOULDER: return L"RB";
        case XINPUT_GAMEPAD_A: return L"A";
        case XINPUT_GAMEPAD_B: return L"B";
        case XINPUT_GAMEPAD_X: return L"X";
        case XINPUT_GAMEPAD_Y: return L"Y";
        case kPadInputStickUp: return L"Stick Up";
        case kPadInputStickDown: return L"Stick Down";
        case kPadInputStickLeft: return L"Stick Left";
        case kPadInputStickRight: return L"Stick Right";
        case kPadInputLeftTrigger: return L"LT";
        case kPadInputRightTrigger: return L"RT";
        default: return L"button " + std::to_wstring(input);
    }
}

// A Mega Drive pad laid over an Xbox one. The three-button row A/B/C sits on
// the bottom row of the face buttons the way it does on every other emulator,
// so muscle memory carries over; X/Y/Z go on the shoulders and the top face
// button, which is the only sensible place left for them.
void set_default_pad_bindings(PlayerInput& input) {
    input.pad = {};
    input.pad[static_cast<int>(PadButton::up)] = XINPUT_GAMEPAD_DPAD_UP;
    input.pad[static_cast<int>(PadButton::down)] = XINPUT_GAMEPAD_DPAD_DOWN;
    input.pad[static_cast<int>(PadButton::left)] = XINPUT_GAMEPAD_DPAD_LEFT;
    input.pad[static_cast<int>(PadButton::right)] = XINPUT_GAMEPAD_DPAD_RIGHT;
    input.pad[static_cast<int>(PadButton::a)] = XINPUT_GAMEPAD_X;
    input.pad[static_cast<int>(PadButton::b)] = XINPUT_GAMEPAD_A;
    input.pad[static_cast<int>(PadButton::c)] = XINPUT_GAMEPAD_B;
    input.pad[static_cast<int>(PadButton::x)] = XINPUT_GAMEPAD_LEFT_SHOULDER;
    input.pad[static_cast<int>(PadButton::y)] = XINPUT_GAMEPAD_Y;
    input.pad[static_cast<int>(PadButton::z)] = XINPUT_GAMEPAD_RIGHT_SHOULDER;
    input.pad[static_cast<int>(PadButton::start)] = XINPUT_GAMEPAD_START;
    input.pad[static_cast<int>(PadButton::mode)] = XINPUT_GAMEPAD_BACK;
}

// DirectInput has no agreed button order, so this is a starting point rather
// than a layout: the hat and the first buttons in the order the device reports
// them. Anything with an unusual arrangement gets rebound by hand, which is
// what the binding boxes are for.
void set_default_dinput_bindings(PlayerInput& input) {
    input.dpad = {};
    input.dpad[static_cast<int>(PadButton::up)] = kDiPovBase + 0;
    input.dpad[static_cast<int>(PadButton::right)] = kDiPovBase + 1;
    input.dpad[static_cast<int>(PadButton::down)] = kDiPovBase + 2;
    input.dpad[static_cast<int>(PadButton::left)] = kDiPovBase + 3;
    input.dpad[static_cast<int>(PadButton::a)] = kDiButtonBase + 2;
    input.dpad[static_cast<int>(PadButton::b)] = kDiButtonBase + 0;
    input.dpad[static_cast<int>(PadButton::c)] = kDiButtonBase + 1;
    input.dpad[static_cast<int>(PadButton::x)] = kDiButtonBase + 4;
    input.dpad[static_cast<int>(PadButton::y)] = kDiButtonBase + 3;
    input.dpad[static_cast<int>(PadButton::z)] = kDiButtonBase + 5;
    input.dpad[static_cast<int>(PadButton::start)] = kDiButtonBase + 9;
    input.dpad[static_cast<int>(PadButton::mode)] = kDiButtonBase + 8;
}

// A gun's trigger and reload, which are only ever reachable when that player is
// holding a gun, so both players get them regardless of what they are set to.
// Also used as a migration: a config written before these rows existed has them
// unbound, and a gun with no trigger is a gun that does nothing.
void set_default_gun_bindings(PlayerInput& input) {
    if (input.keys[static_cast<int>(PadButton::gun_trigger)] == 0) {
        input.keys[static_cast<int>(PadButton::gun_trigger)] = VK_LBUTTON;
    }
    if (input.keys[static_cast<int>(PadButton::gun_reload)] == 0) {
        input.keys[static_cast<int>(PadButton::gun_reload)] = VK_RBUTTON;
    }
}

// The bindings the runtime has always used, now as data rather than a switch
// so the Input tab can change them. Player 2 is left unbound.
void set_default_input(RuntimeState& state) {
    auto& p1 = state.players[0];
    p1.kind = PadKind::six_button;
    p1.device = InputDevice::keyboard;
    p1.keys = {};
    p1.keys[static_cast<int>(PadButton::up)] = VK_UP;
    p1.keys[static_cast<int>(PadButton::down)] = VK_DOWN;
    p1.keys[static_cast<int>(PadButton::left)] = VK_LEFT;
    p1.keys[static_cast<int>(PadButton::right)] = VK_RIGHT;
    p1.keys[static_cast<int>(PadButton::a)] = 'Z';
    p1.keys[static_cast<int>(PadButton::b)] = 'X';
    p1.keys[static_cast<int>(PadButton::c)] = 'C';
    p1.keys[static_cast<int>(PadButton::x)] = 'A';
    p1.keys[static_cast<int>(PadButton::y)] = 'S';
    p1.keys[static_cast<int>(PadButton::z)] = 'D';
    p1.keys[static_cast<int>(PadButton::start)] = VK_RETURN;
    p1.keys[static_cast<int>(PadButton::mode)] = VK_TAB;

    set_default_pad_bindings(p1);
    set_default_dinput_bindings(p1);
    p1.pad_slot = 0;

    auto& p2 = state.players[1];
    p2.kind = PadKind::three_button;
    p2.device = InputDevice::keyboard;
    p2.keys = {};
    // Player 2's gamepad bindings are the standard ones from the start: unlike
    // the keyboard, two pads cannot collide with each other.
    set_default_pad_bindings(p2);
    set_default_dinput_bindings(p2);
    p2.pad_slot = 1;

    set_default_gun_bindings(p1);
    set_default_gun_bindings(p2);
}

// Two people, one Game Gear - but only where a game actually works that way.
//
// The console has no controller port: the pad is moulded into the machine, so
// a two-player game on ONE unit has to split the buttons that are there. Micro
// Machines 1 and 2 do exactly that: one player steers on the d-pad while the
// other drives with buttons 1 and 2. Every other Game Gear game expects one
// pair of hands, and merging a second pad into them would mean a stray press
// on player two's controller firing player one's weapon.
//
// Identified by the Codemasters header checksum rather than by the file name,
// because a checksum is the same in every dump of a release and a file name is
// whatever someone typed. Micro Machines 2 is matched by name as well: its
// checksum is not in here because no copy of that cartridge has been measured,
// and a guessed constant that silently matches nothing is worse than an honest
// fallback.
bool shared_pad_game(const RuntimeState& state);

void apply_pad_button(RuntimeState& state, int player, PadButton button, bool pressed);
mercury::genesis::ControllerPort& port_for_player(RuntimeState& state, int player);
bool two_gun_mode(const RuntimeState& state);
void forget_gun_devices(RuntimeState& state);

// A PAD THAT MOVES. A player's controller is remembered by slot, and slots are
// not stable. When another program takes a pad over - Steam Input, DS4Windows
// and the like hide the real device and put a virtual one in its place - or a
// wireless pad drops and comes back, it can reappear in a different slot, and
// reading the old one looks exactly like the pad "disconnecting from GenX" (the
// user's report, 2026-10-03). DirectInput is worse: every device change
// re-enumerates, and slots are handed out again in whatever order Windows lists
// the devices at that moment - so the old slot can even hold a DIFFERENT pad.
//
// So: a DirectInput pad is followed by NAME, wherever it is listed. And while a
// player's pad is missing altogether, other slots are checked (twice a second -
// asking XInput about an empty slot is slow) for a connected pad the other
// player is not using. DirectInput only takes a pad with the remembered name,
// since another model's buttons are numbered differently; XInput's layout is
// the same for every pad, so any free one will do. The slot found is kept, and
// saved with the settings.
int find_moved_pad(const RuntimeState& state, int player) {
    const auto& input = state.players[player];
    const auto& other = state.players[1 - player];
    const auto taken = [&](int slot) {
        return other.device == input.device && other.pad_slot == slot;
    };
    if (input.device == InputDevice::xinput) {
        for (int slot = 0; slot < 4; ++slot) {
            if (slot == input.pad_slot || taken(slot)) {
                continue;
            }
            bool connected = false;
            (void)read_gamepad(slot, connected);
            if (connected) {
                return slot;
            }
        }
        return -1;
    }
    if (input.device == InputDevice::dinput) {
        const auto& devices = dinput_devices();
        const auto& name = state.dinput_pad_name[player];
        if (name.empty()) {
            return -1;
        }
        for (int slot = 0; slot < devices.count; ++slot) {
            if (slot == input.pad_slot || taken(slot) || devices.name[slot] != name) {
                continue;
            }
            DIJOYSTATE2 probe{};
            if (read_dinput(slot, probe)) {
                return slot;
            }
        }
    }
    return -1;
}

// Read both players' pads and push any change through to the machine. Called
// once a frame; edge-detected against the previous mask so a held button is not
// re-sent every frame.
void poll_gamepads(RuntimeState& state) {
    for (int player = 0; player < 2; ++player) {
        auto& input = state.players[player];
        if (!device_is_gamepad(input.device)) {
            // If the player switched away from a pad, let go of anything the
            // pad was still holding.
            if (state.pad_previous[player] != kPadInputNone) {
                for (int index = 0; index < kPadButtonCount; ++index) {
                    const auto button = static_cast<PadButton>(index);
                    if (kind_has_button(input.kind, button)) {
                        apply_pad_button(state, player, button, false);
                    }
                }
                state.pad_previous[player] = kPadInputNone;
            }
            continue;
        }

        if (input.device == InputDevice::dinput) {
            // The remembered pad listed somewhere else now (a device change
            // re-numbered them): follow it at once - a name check costs
            // nothing - rather than read whatever took its old slot.
            const auto& devices = dinput_devices();
            const auto& remembered = state.dinput_pad_name[player];
            if (!remembered.empty() && input.pad_slot >= 0 && input.pad_slot < devices.count &&
                devices.name[input.pad_slot] != remembered) {
                if (const int moved = find_moved_pad(state, player); moved >= 0) {
                    input.pad_slot = moved;
                }
            }
            DIJOYSTATE2 now{};
            bool connected = read_dinput(input.pad_slot, now);
            if (!connected && --state.pad_search_countdown[player] <= 0) {
                state.pad_search_countdown[player] = 30;
                if (const int moved = find_moved_pad(state, player); moved >= 0) {
                    input.pad_slot = moved;
                    connected = read_dinput(input.pad_slot, now);
                }
            }
            if (connected && input.pad_slot < devices.count) {
                state.dinput_pad_name[player] = devices.name[input.pad_slot];
            }
            const bool was_connected = state.pad_connected[player];
            state.pad_connected[player] = connected;
            if (!connected) {
                // A pad unplugged mid-press used to leave whatever it was
                // holding held for ever, because the reading loop below is
                // what releases a button and this path skipped it. Let go of
                // everything and forget the last reading, so reconnecting
                // starts from a clean slate rather than diffing against a
                // state from before the cable came out.
                if (was_connected) {
                    for (int index = 0; index < kPadButtonCount; ++index) {
                        const auto button = static_cast<PadButton>(index);
                        if (kind_has_button(input.kind, button)) {
                            apply_pad_button(state, player, button, false);
                        }
                    }
                    state.dinput_previous[player] = DIJOYSTATE2{};
                }
                continue;
            }
            // DirectInput state is too wide for a mask, so each binding is
            // tested directly against the previous frame's reading.
            for (int index = 0; index < kPadButtonCount; ++index) {
                const auto binding = input.dpad[index];
                if (binding == kPadInputNone) {
                    continue;
                }
                const auto button = static_cast<PadButton>(index);
                if (!kind_has_button(input.kind, button)) {
                    continue;
                }
                const bool is = dinput_active(now, binding);
                const bool was = dinput_active(state.dinput_previous[player], binding);
                if (was != is) {
                    apply_pad_button(state, player, button, is);
                }
            }
            state.dinput_previous[player] = now;
            continue;
        }

        bool connected = false;
        PadInput now = read_gamepad(input.pad_slot, connected);
        if (!connected && --state.pad_search_countdown[player] <= 0) {
            // Gone from its slot: see whether it turned up in another one.
            state.pad_search_countdown[player] = 30;
            if (const int moved = find_moved_pad(state, player); moved >= 0) {
                input.pad_slot = moved;
                now = read_gamepad(input.pad_slot, connected);
            }
        }
        state.pad_connected[player] = connected;
        const PadInput before = state.pad_previous[player];
        if (now == before) {
            continue;
        }
        state.pad_previous[player] = now;
        for (int index = 0; index < kPadButtonCount; ++index) {
            const auto binding = input.pad[index];
            if (binding == kPadInputNone) {
                continue;
            }
            const auto button = static_cast<PadButton>(index);
            if (!kind_has_button(input.kind, button)) {
                continue;
            }
            const bool was = (before & binding) != 0;
            const bool is = (now & binding) != 0;
            if (was != is) {
                apply_pad_button(state, player, button, is);
            }
        }
    }
}

// Push one button through to whichever machine is running. The Game Gear has a
// single port and only three of the twelve buttons, so player 2 and the pad's
// extra buttons simply have nowhere to go there.
// Netplay is driving the machine's inputs: this window's, or - for the second
// window of a Gear Link - its partner's session.
bool netplay_owns_input(RuntimeState& state) {
    if (state.netplay.active()) {
        return true;
    }
    RuntimeState* partner = link_partner(state);
    return partner != nullptr && partner->netplay.active();
}

void apply_pad_button(RuntimeState& state, int player, PadButton button, bool pressed) {
    // Player one's presses are recorded here whatever else happens to them,
    // because netplay needs to know what this person is holding and cannot ask
    // the port - see netplay_local_mask.
    if (player == 0 && static_cast<int>(button) < 16) {
        const auto bit = static_cast<std::uint16_t>(1u << static_cast<int>(button));
        if (pressed) {
            state.netplay_local_mask |= bit;
        } else {
            state.netplay_local_mask = static_cast<std::uint16_t>(state.netplay_local_mask & ~bit);
        }
    }
    // ...and during netplay that record is ALL that happens. The ports are
    // set once a frame, from the agreed inputs, at both ends alike. A press
    // written straight to a port here lands whenever the key happens to go
    // down - between frames at best, part-way through one if the frame pumps
    // messages - and only on THIS computer, which is a desynchronisation.
    if (netplay_owns_input(state)) {
        return;
    }
    // The gun rows are not pad buttons and have no Button to cast to. The
    // trigger is the port's A line; the reload is the same pull with the gun
    // pointed away from the screen, which is the gesture the games watch for.
    if (button == PadButton::gun_trigger || button == PadButton::gun_reload) {
        if (state.machine != Machine::genesis) {
            return;
        }
        if (button == PadButton::gun_reload) {
            if (two_gun_mode(state) && player >= 0 && player < 2) {
                state.gun_pointers[player].off_screen_shot = pressed;
            } else {
                state.off_screen_held = pressed;
            }
        }
        if (player == 1 && two_gun_mode(state)) {
            state.genesis.port2().set_gun2_trigger(pressed);
        } else {
            port_for_player(state, player).set_button(mercury::genesis::Button::a, pressed);
        }
        return;
    }
    if (state.machine == Machine::genesis) {
        // Player two's Justifier is not a second port - it is the second gun on
        // the same one - so its buttons go to the gun rather than to the port,
        // or both players would be pulling the same trigger.
        if (player == 1 && two_gun_mode(state)) {
            if (button == PadButton::a) {
                state.genesis.port2().set_gun2_trigger(pressed);
            } else if (button == PadButton::start) {
                state.genesis.port2().set_gun2_start(pressed);
            }
            return;
        }
        // Through port_for_player, so a light gun's own buttons reach port 2
        // rather than the port its player number would suggest.
        port_for_player(state, player)
            .set_button(static_cast<mercury::genesis::Button>(button), pressed);
        return;
    }
    if (player < 0 || player > 1) {
        return;
    }
    if (player == 1) {
        // Player two only exists on the games that share one console.
        if (!shared_pad_game(state)) {
            return;
        }
        // ...and their d-pad is dropped even there: the console has one, and
        // two people steering the same car helps nobody. Buttons 1, 2 and
        // Start are what they get, which is the split the games expect.
        if (button == PadButton::up || button == PadButton::down ||
            button == PadButton::left || button == PadButton::right) {
            return;
        }
    }

    const auto index = static_cast<std::size_t>(button);
    if (index >= state.shared_pad_held[0].size()) {
        return;
    }
    state.shared_pad_held[static_cast<std::size_t>(player)][index] = pressed;
    // The OR is the point. Last-writer-wins would let player one letting go of
    // button 1 cancel player two's press of the same button, which on a real
    // shared console is not something that can happen.
    const bool held = state.shared_pad_held[0][index] || state.shared_pad_held[1][index];

    switch (button) {
        case PadButton::up: state.game_gear.input_state().set_button(Button::up, held); break;
        case PadButton::down: state.game_gear.input_state().set_button(Button::down, held); break;
        case PadButton::left: state.game_gear.input_state().set_button(Button::left, held); break;
        case PadButton::right: state.game_gear.input_state().set_button(Button::right, held); break;
        case PadButton::a: state.game_gear.input_state().set_button(Button::button1, held); break;
        case PadButton::b: state.game_gear.input_state().set_button(Button::button2, held); break;
        case PadButton::start:
            // START is a real button, not a tap: it has to be holdable so a
            // game can read its level, and it has to release on key-up. In
            // Master System mode it also drives the pause NMI, which is edge
            // triggered in GameGear, so holding it here cannot spam the CPU.
            // Either player can reach it - it is on the console, not the pad.
            state.game_gear.input_state().set_button(Button::start, held);
            state.start_pulse_frames_remaining = 0;
            break;
        default:
            break;
    }
}

// Write everything that decides what a scanline shows: the VDP registers, the
// derived plane and window geometry, and the live scroll values.
std::filesystem::path log_directory();
std::wstring log_file_name(const RuntimeState& state, const wchar_t* suffix);
// Matches the definition below exactly - by value, not by const reference. A
// near-miss here is a second overload rather than a declaration, and every call
// in the file then becomes ambiguous.
void set_status(RuntimeState& state, std::wstring message);

void dump_video_state(RuntimeState& state) {
    const auto& vdp = state.genesis.vdp();
    // Into the log folder under the cartridge's name and the time, NOT a fixed
    // name in the working directory. It used to write "mercury_video_state.txt"
    // next to wherever GenX happened to be launched from, which meant every
    // dump overwrote the last one and none of them were where the other logs
    // are.
    const auto path = log_directory() / log_file_name(state, L"-video.txt");
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        set_status(state, L"could not write the video state log");
        return;
    }
    out << "VDP registers:\n";
    for (std::size_t i = 0; i < 24; ++i) {
        out << "  reg" << std::dec << i << " = 0x" << std::hex << int(vdp.reg(i)) << "\n";
    }
    const int r17 = vdp.reg(0x11);
    const int r18 = vdp.reg(0x12);
    out << std::dec;
    out << "\nwindow: reg17=0x" << std::hex << r17 << " reg18=0x" << r18 << std::dec << "\n";
    out << "  horizontal: from_right=" << ((r17 & 0x80) ? 1 : 0)
        << " HP=" << (r17 & 0x1F) << " -> cell " << ((r17 & 0x1F) * 2) << "\n";
    out << "  vertical:   from_bottom=" << ((r18 & 0x80) ? 1 : 0)
        << " VP=" << (r18 & 0x1F) << " -> line " << ((r18 & 0x1F) * 8) << "\n";
    out << "\nplanes: A base=0x" << std::hex << vdp.plane_a_base_addr()
        << " B base=0x" << vdp.plane_b_base_addr()
        << " window base=0x" << ((vdp.reg(0x03) & 0x3E) << 10) << std::dec << "\n";
    out << "  size: " << vdp.plane_a_row_cells() << " x " << (vdp.plane_h_px() / 8)
        << " cells (" << vdp.plane_h_px() << " px tall)\n";
    out << "  vscrollA=" << vdp.vsram_entry(0) << " vscrollB=" << vdp.vsram_entry(1) << "\n";
    {
        const auto& e = vdp.edge_debug();
        out << "\nedge-blank decision (from line 0 of the last frame):\n";
        out << "  plane height px   = " << e[0] << "\n";
        out << "  plane A wrap line = " << e[1] << "\n";
        out << "  plane B wrap line = " << e[2] << "\n";
        out << "  top_leftover      = " << e[3] << "\n";
        out << "  bottom_leftover   = " << e[4] << "\n";
        out << "  bottom_window     = " << e[5] << "\n";
        out << "  quirk enabled     = " << e[6] << "\n";
    }
    out << "\nreg18 (window V) values written:\n";
    {
        const auto& v = vdp.reg18_values();
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i]) out << "  0x" << std::hex << i << std::dec
                          << "  down=" << ((i & 0x80) ? 1 : 0) << " VP=" << (i & 0x1F)
                          << " -> line " << ((i & 0x1F) * 8) << "   x" << v[i] << "\n";
        }
        out << "  written on lines:";
        const auto& l = vdp.reg18_lines();
        for (std::size_t i = 0; i < l.size(); ++i) {
            if (l[i]) out << " " << i << "(x" << l[i] << ")";
        }
        out << "\n";
    }
    out << "\nreg10 values written:\n";
    const auto& rv = vdp.reg10_values();
    for (std::size_t i = 0; i < rv.size(); ++i) {
        if (rv[i]) out << "  0x" << std::hex << i << std::dec << " (" << (i + 1)
                       << " lines) x" << rv[i] << "\n";
    }
    out << "\ndisplay blanked starting on line:\n";
    const auto& bl = vdp.blank_start_lines();
    for (std::size_t i = 0; i < bl.size(); ++i) {
        if (bl[i]) out << "  line " << i << " x" << bl[i] << "\n";
    }
    out << "\nH-ints LOST (raised while one was still pending):\n";
    {
        const auto& ll = vdp.hint_lost_lines();
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < ll.size(); ++i) {
            if (ll[i]) { out << "  line " << i << " x" << ll[i] << "\n"; total += ll[i]; }
        }
        out << "  total lost = " << total << "\n";
    }
    out << "\nH-ints raised on lines (>=20):\n";
    const auto& rl = vdp.hint_raise_lines();
    for (std::size_t i = 0; i < rl.size(); ++i) {
        if (rl[i] >= 20) out << "  line " << i << " x" << rl[i] << "\n";
    }

    // The window replaces plane A only, so a transparent window tile lets the
    // scroll plane show through underneath. Which of those two is happening is
    // decided by the nametable entries and the pixels of the tiles they name.
    const auto& vram = vdp.vram();
    const auto wbase = static_cast<std::uint32_t>((vdp.reg(0x03) & 0x3E) << 10);
    const auto read16 = [&](std::uint32_t a) {
        return static_cast<std::uint32_t>((vram[a & 0xFFFF] << 8) | vram[(a + 1) & 0xFFFF]);
    };
    out << "\nwindow nametable (base 0x" << std::hex << wbase << std::dec
        << "), 14 cells per row:\n";
    for (std::uint32_t r = 20; r < 28; ++r) {
        const auto row_addr = wbase + r * 64u * 2u;
        out << "  row " << r << " (lines " << (r * 8) << "-" << (r * 8 + 7) << ") @0x"
            << std::hex << row_addr << std::dec << ":";
        for (std::uint32_t c = 0; c < 14; ++c) {
            out << " " << std::hex << read16(row_addr + c * 2u) << std::dec;
        }
        out << "\n";
    }
    out << "\ntile pixels for the entries on window rows 25-27, cell 0:\n";
    for (std::uint32_t r = 25; r < 28; ++r) {
        const auto entry = read16(wbase + r * 64u * 2u);
        const auto tile = entry & 0x07FFu;
        std::uint32_t nonzero = 0;
        for (std::uint32_t i = 0; i < 32; ++i) {
            if (vram[(tile * 32u + i) & 0xFFFF] != 0) ++nonzero;
        }
        out << "  row " << r << " entry=0x" << std::hex << entry << " tile=0x" << tile
            << std::dec << " -> " << nonzero << " of 32 bytes non-zero"
            << (nonzero == 0 ? "  (TRANSPARENT)" : "") << "\n";
    }
    out.flush();
    out.close();
    set_status(state, L"wrote " + path.filename().wstring());
}

// A key can be bound on both pads at once - nothing stops it - so every player
// is checked rather than stopping at the first match.
// Returns how many buttons the key actually reached, so the caller can record
// "delivered to nothing" separately from "delivered".
int set_button_from_key(RuntimeState& state, WPARAM key, bool pressed) {
    int delivered = 0;
    for (int player = 0; player < 2; ++player) {
        const auto& input = state.players[player];
        // A player on a controller plays on the controller. While a pad is
        // chosen the Input tab shows the PAD's bindings, so keyboard keys that
        // still worked behind them were invisible - Enter, player one's default
        // Start, started games for a player whose Start was on the pad (the
        // user's report, 2026-10-03). Keyboard players keep their keys, and so
        // do Mouse players (light guns, the piano), whose trigger and reload
        // are mouse buttons in this same list.
        if (device_is_gamepad(input.device)) {
            continue;
        }
        for (int index = 0; index < kPadButtonCount; ++index) {
            if (input.keys[index] == 0 || input.keys[index] != static_cast<UINT>(key)) {
                continue;
            }
            // A binding left over from switching the dropdown - X on a pad that
            // is now three-button, or A on a Menacer - must not still fire.
            const auto button = static_cast<PadButton>(index);
            if (!kind_has_button(input.kind, button)) {
                continue;
            }
            apply_pad_button(state, player, button, pressed);
            ++delivered;
        }
    }
    return delivered;
}

void note_key(RuntimeState& state, UINT key, const char* outcome, int delivered) {
    ++state.keys_seen;
    state.key_history[state.key_history_next] = {key, outcome, delivered};
    state.key_history_next = (state.key_history_next + 1) % state.key_history.size();
}

// Tell the machine what is plugged into each port. The six-button pad answers a
// different number of TH edges than the three-button one, so a game that probes
// for six buttons has to see the right answer - this is not cosmetic.
mercury::genesis::ControllerPort::Kind port_kind_of(PadKind kind) {
    using Kind = mercury::genesis::ControllerPort::Kind;
    switch (kind) {
        case PadKind::six_button: return Kind::pad6;
        case PadKind::justifier: return Kind::justifier;
        case PadKind::menacer: return Kind::menacer;
        case PadKind::miracle_piano: return Kind::miracle_piano;
        default: return Kind::pad3;
    }
}

void apply_port_kinds_for(RuntimeState& state, PadKind first, PadKind second);

void apply_port_kinds(RuntimeState& state) {
    apply_port_kinds_for(state, state.players[0].kind, state.players[1].kind);
}

// The two seats' devices, onto the two ports. Normally this computer's own
// Input tab (apply_port_kinds); during netplay, the host's, at both ends.
void apply_port_kinds_for(RuntimeState& state, PadKind first, PadKind second) {
    using Kind = mercury::genesis::ControllerPort::Kind;
    const auto to_kind = [](PadKind kind) { return port_kind_of(kind); };
    const PadKind seat[2] = {first, second};
    // The Menacer does not plug into a console at all: its infrared receiver
    // box does, and that box goes in PORT 2. Every game looks for it there and
    // nowhere else. So whichever player the gun is assigned to, it is wired to
    // port 2 - assigning it to player 1 and putting it in port 1 is a setup
    // that cannot exist on hardware, and the game simply never finds it.
    //
    // Port 1 then keeps a pad, because these games are navigated with one.
    // A second gun exists only when both players have one. Telling the port
    // otherwise makes it multiplex on TR for a gun nobody is holding, and the
    // absent gun then swallows the light pulse on any frame the game happened
    // to have it selected.
    state.genesis.port2().set_gun2_present(seat[0] == PadKind::justifier &&
                                           seat[1] == PadKind::justifier);
    // The Miracle Piano goes in PORT 2, for the same reason the Menacer does:
    // that is where the game looks. Measured on the cartridge - it writes the
    // port-2 control register and bit-bangs there, and never touches port 1
    // for anything but a pad. Assigned to either player, it is wired to 2.
    if (seat[0] == PadKind::miracle_piano || seat[1] == PadKind::miracle_piano) {
        state.genesis.port1().set_kind(Kind::pad3);
        state.genesis.port2().set_kind(Kind::miracle_piano);
        return;
    }
    if (kind_is_light_gun(seat[0])) {
        state.genesis.port1().set_kind(Kind::pad3);
        state.genesis.port2().set_kind(to_kind(seat[0]));
        return;
    }
    state.genesis.port1().set_kind(to_kind(seat[0]));
    state.genesis.port2().set_kind(to_kind(seat[1]));
}

// Which port a player's device is actually wired to. Normally player 1 is port
// 1, but a light gun is always port 2 - see apply_port_kinds.
mercury::genesis::ControllerPort& port_for_player(RuntimeState& state, int player) {
    if (kind_is_light_gun(state.players[player].kind)) {
        return state.genesis.port2();
    }
    return player == 0 ? state.genesis.port1() : state.genesis.port2();
}

bool shared_pad_game(const RuntimeState& state) {
    if (state.machine != Machine::game_gear || !state.rom_loaded) {
        return false;
    }
    // Measured from the cartridge, so it holds for any dump of that release.
    switch (state.game_gear.cartridge().codemasters_checksum()) {
        case 0xAA9D:  // Micro Machines (USA, Europe)
            return true;
        default:
            break;
    }
    // The name is the only thing left for a release that has not been measured
    // here - Micro Machines 2 among them. Loose on purpose: it is a match for
    // a two-player convenience, not for anything that could corrupt a save.
    auto name = state.rom_path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name.find("micro machines") != std::string::npos ||
           name.find("micromachines") != std::string::npos;
}

void clear_all_buttons(RuntimeState& state) {
    state.turbo_held = false;
    state.fast_forward_held = false;
    state.rewind_held = false;
    // What this person is holding, as netplay sends it - including the mouse.
    // Left set, a button held while focus went elsewhere would be held on BOTH
    // computers until it was pressed and released again.
    state.netplay_local_mask = 0;
    state.mouse_left_down = false;
    state.mouse_right_down = false;
    state.mouse_middle_down = false;
    // The ports themselves are netplay's to set during a session.
    if (netplay_owns_input(state)) {
        return;
    }
    if (state.machine == Machine::genesis) {
        using mercury::genesis::Button;
        for (int i = 0; i < static_cast<int>(Button::count); ++i) {
            state.genesis.port1().set_button(static_cast<Button>(i), false);
            state.genesis.port2().set_button(static_cast<Button>(i), false);
        }
        return;
    }
    state.game_gear.input_state().set_button(Button::up, false);
    state.game_gear.input_state().set_button(Button::down, false);
    state.game_gear.input_state().set_button(Button::left, false);
    state.game_gear.input_state().set_button(Button::right, false);
    state.game_gear.input_state().set_button(Button::button1, false);
    state.game_gear.input_state().set_button(Button::button2, false);
    state.game_gear.input_state().set_button(Button::start, false);
    // Both players' contributions, or a button held by the other pad when the
    // window lost focus would come back still held.
    for (auto& player : state.shared_pad_held) {
        player.fill(false);
    }
    state.start_pulse_frames_remaining = 0;
}

int effective_scale(const RuntimeState& state);

// An interlace mode 2 frame carries 448 lines but stands the same height on a
// screen as a 224-line one. This is the height to reason about when sizing.
int logical_height(const RuntimeState& state) {
    const bool interlaced =
        state.machine == Machine::genesis && state.genesis.vdp().interlace_double();
    const int height = static_cast<int>(state.presented_height);
    return interlaced ? (std::max)(1, height / 2) : height;
}

// Width divided by height for the chosen shape, or 0 when the picture should
// simply fill whatever it is given.
double aspect_ratio(const RuntimeState& state) {
    switch (state.aspect) {
        case Aspect::four_three: return 4.0 / 3.0;
        case Aspect::sixteen_nine: return 16.0 / 9.0;
        case Aspect::custom:
            if (state.custom_aspect_width > 0 && state.custom_aspect_height > 0) {
                return static_cast<double>(state.custom_aspect_width) /
                       static_cast<double>(state.custom_aspect_height);
            }
            return 0.0;
        default: return 0.0;
    }
}

// Where the picture goes inside the client area. Anything left over is a
// letterbox bar, which the paint handler fills.
RECT picture_rect(const RuntimeState& state, const RECT& client) {
    const int available_width = client.right - client.left;
    const int available_height = client.bottom - client.top;
    const double ratio = aspect_ratio(state);
    if (ratio <= 0.0 || available_width <= 0 || available_height <= 0) {
        return client;
    }
    int width = available_width;
    int height = static_cast<int>(width / ratio);
    if (height > available_height) {
        height = available_height;
        width = static_cast<int>(height * ratio);
    }
    // No cap from the 1x-4x setting. That was tried and it is wrong: it left
    // 4:3 and 16:9 as a small rectangle in the middle of a fullscreen display
    // instead of filling it. The scale options size the WINDOW; fullscreen
    // fills the screen with the chosen shape.
    const int left = client.left + (available_width - width) / 2;
    const int top = client.top + (available_height - height) / 2;
    return RECT{left, top, left + width, top + height};
}

// An interlace mode 2 frame is 448 lines but fills the same height on a screen
// as a 224-line one - the extra lines are detail, not size. Halving the vertical
// scale keeps the window the size the player expects and the picture the right
// shape.
SIZE desired_client_size(const RuntimeState& state) {
    const int scale = effective_scale(state);
    const int height = logical_height(state) * scale;
    const double ratio = aspect_ratio(state);
    // A chosen shape decides the width; stretch keeps the source's own pixels.
    const int width = ratio > 0.0 ? static_cast<int>(height * ratio)
                                  : static_cast<int>(state.presented_width) * scale;
    return SIZE{static_cast<LONG>((std::max)(1, width)), static_cast<LONG>((std::max)(1, height))};
}

// Grow or shrink the client area to match the VDP's current display size.
// Only fires when the size actually changed, so a normal frame costs nothing.
void apply_pending_resize(RuntimeState& state) {
    HWND hwnd = state.window;
    if (!state.resize_pending || hwnd == nullptr) {
        return;
    }
    state.resize_pending = false;
    // Never while fullscreen. Sizing the window to a cartridge's dimensions
    // there drags it straight back out of fullscreen - which is what happened
    // when a game was opened from the fullscreen logo screen, and again
    // whenever a game changed video mode mid-run. A Gear Link is the same
    // situation: the two halves of the screen are the layout, not a default to
    // be overridden by whatever the cartridge asks for.
    if (state.fullscreen || gear_link_active()) {
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    const auto client = desired_client_size(state);
    RECT desired{0, 0, client.cx, client.cy};
    // TRUE for the menu: the bar eats client height, and sizing without telling
    // AdjustWindowRect about it crops the bottom of every frame.
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, TRUE);
    SetWindowPos(hwnd, nullptr, 0, 0,
                 desired.right - desired.left,
                 desired.bottom - desired.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// The Mega Drive VDP always renders into a 320-wide buffer regardless of the
// mode in use, so an H32 frame has to be copied out row by row rather than
// taken whole - the same "the source stride is not the display width" trap the
// Game Gear path hit.
void update_framebuffer_genesis(RuntimeState& state) {
    const auto& vdp = state.genesis.vdp();
    const auto width = static_cast<std::size_t>(vdp.screen_width());
    const auto height = static_cast<std::size_t>(vdp.screen_height());
    const auto& source = vdp.frame_pixels();
    constexpr std::size_t stride = mercury::genesis::Vdp::kMaxWidth;

    if (width != state.presented_width || height != state.presented_height) {
        state.presented_width = width;
        state.presented_height = height;
        state.resize_pending = true;
    }
    state.bgra_frame.resize(width * height);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const auto src = (y * stride + x) * 3;
            state.bgra_frame[y * width + x] =
                0xFF000000u |
                (static_cast<std::uint32_t>(source[src + 0]) << 16) |
                (static_cast<std::uint32_t>(source[src + 1]) << 8) |
                static_cast<std::uint32_t>(source[src + 2]);
        }
    }
    state.has_visible_frame = true;
}

void update_framebuffer(RuntimeState& state) {
    if (state.machine == Machine::genesis) {
        update_framebuffer_genesis(state);
        return;
    }
    const auto& presented_pixels = state.game_gear.vdp().presented_frame_pixels();
    if (!presented_pixels.empty()) {
        state.rgb_frame = presented_pixels;
    } else if (!state.has_visible_frame) {
        state.rgb_frame = state.game_gear.vdp().visible_frame_pixels(true);
    } else {
        return;
    }
    // Size from the VDP, not from the Game Gear constants: a Master System
    // cartridge puts it in 256x192 mode, and Mode 4's 224-line variant can be
    // selected at any point after boot - Fantastic Dizzy switches once its
    // logo is done. The window has to follow or the taller frame is squashed.
    const auto width = state.game_gear.vdp().visible_width();
    const auto height = state.game_gear.vdp().visible_height();
    if (width != state.presented_width || height != state.presented_height) {
        state.presented_width = width;
        state.presented_height = height;
        state.resize_pending = true;
    }
    state.bgra_frame.resize(width * height);

    for (std::size_t i = 0; i < state.bgra_frame.size(); ++i) {
        const auto r = state.rgb_frame[i * 3 + 0];
        const auto g = state.rgb_frame[i * 3 + 1];
        const auto b = state.rgb_frame[i * 3 + 2];
        state.bgra_frame[i] =
            0xFF000000u |
            (static_cast<std::uint32_t>(r) << 16) |
            (static_cast<std::uint32_t>(g) << 8) |
            static_cast<std::uint32_t>(b);
    }

    state.has_visible_frame = true;
}

std::int16_t amplify_sample(const RuntimeState& state, std::int16_t sample) {
    // `hidden_by_view` is a Gear Link showing one console of the pair. The
    // other one is still RUNNING - the cable needs it - but nobody is looking
    // at it, and two consoles playing the same game a few cycles apart is not
    // stereo, it is a flanged mess.
    if (state.muted || state.hidden_by_view) {
        return 0;
    }
    // The volume setting rides on top of the per-machine gain rather than
    // replacing it: the gain exists to bring a quiet chip up to a usable level,
    // and folding the two together would make 100% mean something different on
    // the Game Gear than on the Mega Drive.
    const auto amplified =
        static_cast<int>(sample) * state.audio_gain * std::clamp(state.volume, 0, 100) / 100;
    return static_cast<std::int16_t>(std::clamp(amplified, -32768, 32767));
}

void fault(RuntimeState& state, const std::string& text) {
    state.faulted = true;
    state.running = false;
    state.status_text = text;
}

bool initialize_audio(RuntimeState& state) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = mercury::gamegear::Psg::kDefaultSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    if (waveOutOpen(&state.wave_out, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        state.wave_out = nullptr;
        return false;
    }

    for (auto& buffer : state.audio_buffers) {
        buffer.samples.resize(kAudioChunkSamples, 0);
        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
        buffer.header.dwBufferLength = static_cast<DWORD>(buffer.samples.size() * sizeof(std::int16_t));
        buffer.header.dwFlags = 0;
    }

    return true;
}

void shutdown_audio(RuntimeState& state) {
    if (state.wave_out == nullptr) {
        return;
    }

    waveOutReset(state.wave_out);
    for (auto& buffer : state.audio_buffers) {
        if (buffer.prepared) {
            waveOutUnprepareHeader(state.wave_out, &buffer.header, sizeof(WAVEHDR));
            buffer.prepared = false;
            buffer.submitted = false;
        }
    }
    waveOutClose(state.wave_out);
    state.wave_out = nullptr;
}

// The Mega Drive's analogue output stage is a low-pass, and a strong one: the
// console does not put the chips' raw square edges on the speaker. Emulators
// that sound "right" all model it - BlastEm ships lowpass_cutoff 3390 in its
// default config, and Kega has had an equivalent filter for years. Without it
// everything carries treble the hardware never produced, which reads as tinny
// on exactly the sounds that are mostly high harmonics: engines, wind, noise.
//
// One pole, applied after both chips are summed, because it models the thing
// that comes after the summing node on the board.
constexpr double kConsoleLowPassHz = 3390.0;

// Defined below, in the shell section; used here by the mixer and the recorder.
void record_samples(RuntimeState& state, const std::vector<std::int16_t>& samples);
void set_status(RuntimeState& state, std::wstring message);
std::filesystem::path recent_file_path();

void apply_console_filter(RuntimeState& state, std::vector<std::int16_t>& samples) {
    if (!state.console_filter) {
        return;
    }
    static const double alpha =
        1.0 - std::exp(-2.0 * 3.14159265358979323846 * kConsoleLowPassHz /
                       static_cast<double>(mercury::gamegear::Psg::kDefaultSampleRate));
    for (auto& sample : samples) {
        state.lowpass_state += alpha * (static_cast<double>(sample) - state.lowpass_state);
        sample = static_cast<std::int16_t>(
            std::clamp(static_cast<int>(state.lowpass_state), -32768, 32767));
    }
}

// The Mega Drive has two sound chips at unrelated rates: the YM2612 emits
// stereo frames at clock/144, the SN76489 mono at the device rate. Both are
// folded down to the mono 44.1kHz stream the output device was opened with.
void collect_audio_genesis(RuntimeState& state) {
    auto& ym = state.genesis.ym2612();
    auto& psg = state.genesis.psg();
    const auto& fm = ym.sample_buffer();
    const auto& psg_samples = psg.sample_buffer();

    const double ratio = mercury::genesis::Ym2612::kSampleRateHz /
                         static_cast<double>(mercury::gamegear::Psg::kDefaultSampleRate);
    const std::size_t frames = fm.size() / 2;

    // Downmix to mono with one sample of history at the front, so the
    // interpolation below can reach across the frame boundary instead of
    // restarting from silence every sixtieth of a second.
    std::vector<int> mono;
    mono.reserve(frames + 1);
    mono.push_back(state.ym_previous_mono);
    for (std::size_t i = 0; i < frames; ++i) {
        mono.push_back((static_cast<int>(fm[i * 2]) + static_cast<int>(fm[i * 2 + 1])) / 2);
    }

    // Resample 53.3kHz to 44.1kHz. Picking the nearest sample instead of
    // interpolating drops roughly one sample in six, and at this ratio that
    // lands as aliasing spread across the whole spectrum - audible as a gritty
    // edge on every note rather than as a pitch error.
    std::vector<std::int16_t> mixed;
    mixed.reserve(static_cast<std::size_t>(static_cast<double>(frames) / ratio) + 2);
    while (state.ym_resample_phase < static_cast<double>(frames)) {
        const auto index = static_cast<std::size_t>(state.ym_resample_phase);
        const double fraction = state.ym_resample_phase - static_cast<double>(index);
        const int a = mono[index];
        const int b = mono[index + 1];
        const int value = a + static_cast<int>((b - a) * fraction);
        mixed.push_back(static_cast<std::int16_t>(std::clamp(value, -32768, 32767)));
        state.ym_resample_phase += ratio;
    }
    state.ym_resample_phase -= static_cast<double>(frames);
    state.ym_previous_mono = mono.back();
    ym.clear_sample_buffer();

    // Sum the two chips with headroom. Both can reach most of the sixteen-bit
    // range on their own, so adding them at full weight clips on any passage
    // where the FM and the PSG are both busy - a title screen, typically.
    //
    // The PSG divisor is not a taste decision. At full scale our FM reaches
    // 32767 and our PSG reaches 32767 too, so a 3:4 / 1:3 split puts the PSG at
    // 0.444 of the FM. BlastEm's numbers give the real ratio: its FM tops out
    // at 6 * 8191 * 79/120 = 32347 and its PSG at 4 * 32767/14 = 9362, which is
    // 0.289. Ours had the PSG about half again too loud, and the PSG is the
    // bright, square-edged half of the mix - too much of it reads as thin.
    const auto common = std::min(mixed.size(), psg_samples.size());
    const auto scale_fm = [](std::int16_t sample) { return (static_cast<int>(sample) * 3) / 4; };
    const auto scale_psg = [&state](std::int16_t sample) {
        if (state.mute_psg) {
            return 0;
        }
        // 0.75 * 0.289 = 0.217, near enough 7/32.
        return (static_cast<int>(sample) * 7) / 32;
    };
    for (std::size_t i = 0; i < common; ++i) {
        mixed[i] = static_cast<std::int16_t>(
            std::clamp(scale_fm(mixed[i]) + scale_psg(psg_samples[i]), -32768, 32767));
    }
    for (std::size_t i = common; i < mixed.size(); ++i) {
        mixed[i] = static_cast<std::int16_t>(scale_fm(mixed[i]));
    }
    for (std::size_t i = common; i < psg_samples.size(); ++i) {
        mixed.push_back(static_cast<std::int16_t>(scale_psg(psg_samples[i])));
    }
    psg.clear_sample_buffer();

    // The 32X's PWM, which is a THIRD source and on some cartridges the only
    // one. Star Wars Arcade drives its music, its lasers and its speech
    // entirely from here and never touches the FM chip or the PSG, so leaving
    // this out of the mix is the difference between the game having sound and
    // running silent.
    //
    // Already at the output rate, mixed at the same weight as the FM: on the
    // hardware the adapter's output and the Mega Drive's are summed at
    // comparable level, and a cartridge that uses only one of them should not
    // be quieter than one that uses the other.
    auto& pwm = state.genesis.thirtytwox().adapter().pwm();
    const auto& pwm_samples = pwm.sample_buffer();
    if (!pwm_samples.empty()) {
        const auto scale_pwm = [&state](std::int16_t sample) {
            return state.mute_pwm ? 0 : (static_cast<int>(sample) * 3) / 4;
        };
        const auto shared = std::min(mixed.size(), pwm_samples.size());
        for (std::size_t i = 0; i < shared; ++i) {
            mixed[i] = static_cast<std::int16_t>(
                std::clamp(static_cast<int>(mixed[i]) + scale_pwm(pwm_samples[i]), -32768, 32767));
        }
        for (std::size_t i = shared; i < pwm_samples.size(); ++i) {
            mixed.push_back(static_cast<std::int16_t>(scale_pwm(pwm_samples[i])));
        }
        pwm.clear_sample_buffer();
    }

    // The Sega CD: its PCM chip and the disc's own audio tracks, already
    // mixed together at the output rate, stereo. Folded to mono like
    // everything else here and weighted like the FM - a CD game's music is
    // very often ONLY the disc's audio track, so it must not sit under the
    // Mega Drive's chips.
    if (state.genesis.cd_mode()) {
        auto& cd = state.genesis.segacd();
        const auto& cd_samples = cd.audio();
        const std::size_t frames = cd_samples.size() / 2;
        const auto scale_cd = [&state](int left, int right) {
            return state.mute_cd ? 0 : (((left + right) / 2) * 3) / 4;
        };
        const auto shared = std::min(mixed.size(), frames);
        for (std::size_t i = 0; i < shared; ++i) {
            mixed[i] = static_cast<std::int16_t>(std::clamp(
                static_cast<int>(mixed[i]) + scale_cd(cd_samples[i * 2], cd_samples[i * 2 + 1]),
                -32768, 32767));
        }
        for (std::size_t i = shared; i < frames; ++i) {
            mixed.push_back(static_cast<std::int16_t>(scale_cd(cd_samples[i * 2], cd_samples[i * 2 + 1])));
        }
        cd.clear_audio();
    }

    apply_console_filter(state, mixed);
    // The Miracle Piano last, and deliberately AFTER the console's output
    // filter: the keyboard is a separate instrument on the end of a cable,
    // not something coming out of the Mega Drive's sound chip, so the
    // console's own low-pass has no business shaping it. Before the WAV
    // capture, though, so a recording of a lesson has the lesson in it.
    mix_piano_audio(state, mixed);
    record_samples(state, mixed);
    state.pending_audio.insert(state.pending_audio.end(), mixed.begin(), mixed.end());
}

// --- WAV capture -----------------------------------------------------------
// A plain 16-bit mono RIFF file at the output rate. The header carries two
// lengths that are not known until the recording stops, so they go out as zero
// and are patched in afterwards.
void write_wav_header(std::ofstream& out, std::uint32_t sample_count) {
    const std::uint32_t rate = mercury::gamegear::Psg::kDefaultSampleRate;
    const std::uint32_t data_bytes = sample_count * 2;
    const auto put32 = [&out](std::uint32_t value) {
        out.write(reinterpret_cast<const char*>(&value), 4);
    };
    const auto put16 = [&out](std::uint16_t value) {
        out.write(reinterpret_cast<const char*>(&value), 2);
    };
    out.write("RIFF", 4);
    put32(36 + data_bytes);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);          // PCM
    put16(1);          // mono
    put32(rate);
    put32(rate * 2);   // bytes per second
    put16(2);          // block align
    put16(16);         // bits
    out.write("data", 4);
    put32(data_bytes);
}

void stop_recording(RuntimeState& state) {
    if (!state.recording.is_open()) {
        return;
    }
    state.recording.seekp(0, std::ios::beg);
    write_wav_header(state.recording, state.recorded_samples);
    state.recording.close();
    set_status(state, L"saved " + state.recording_path.filename().wstring());
}

void start_recording(RuntimeState& state) {
    // The log folder, like everything else the Tools menu produces. It used to
    // have a "recordings" folder of its own, which meant two places to look.
    auto directory = log_directory();
    auto name = state.rom_path.stem().wstring();
    if (name.empty()) {
        name = L"genx";
    }
    // Seconds since the epoch keeps successive takes apart without needing to
    // ask the filesystem what already exists.
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    state.recording_path = directory / (name + L"-" + std::to_wstring(stamp) + L".wav");
    state.recording.open(state.recording_path, std::ios::binary | std::ios::trunc);
    if (!state.recording) {
        set_status(state, L"could not open the recording file");
        return;
    }
    state.recorded_samples = 0;
    write_wav_header(state.recording, 0);
    set_status(state, L"recording...");
}

// --- where GenX keeps its own files ----------------------------------------

// The settings, the recent list, the profiles and the logs. Made on demand so
// it exists the first time anything is written.
std::filesystem::path genx_directory() {
    auto directory = recent_file_path().parent_path();
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

// Every log lands here, whichever tool wrote it, so "send me the log" names one
// folder rather than a hunt through AppData. Created at startup rather than on
// first write, so it is there to be opened even before anything has logged.
std::filesystem::path log_directory() {
    auto directory = genx_directory() / L"logs";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

// A file name that sorts by time and cannot collide with the previous take.
std::wstring log_file_name(const RuntimeState& state, const wchar_t* suffix) {
    auto name = state.rom_path.stem().wstring();
    if (name.empty()) {
        name = L"genx";
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return name + L"-" + std::to_wstring(stamp) + suffix;
}

// --- FM register log -------------------------------------------------------
// The FM and DAC logs share one chip-side log, so the chip keeps recording
// while EITHER is open and the drain decides who gets which line.
void drain_fm_log(RuntimeState& state) {
    if (state.machine != Machine::genesis) {
        return;
    }
    const bool want_fm = state.fm_log.is_open();
    const bool want_dac = state.dac_log.is_open();
    if (!want_fm && !want_dac) {
        return;
    }
    auto& ym = state.genesis.ym2612();
    for (const auto& entry : ym.write_log()) {
        // Seconds are what a waveform is measured in, so the log is timestamped
        // in them rather than in chip samples.
        const double seconds =
            static_cast<double>(entry.sample) / mercury::genesis::Ym2612::kSampleRateHz;
        if (want_fm) {
            state.fm_log << seconds << "\tbank" << static_cast<int>(entry.bank) << "\t0x"
                         << std::hex << static_cast<int>(entry.address) << "\t0x"
                         << static_cast<int>(entry.value) << std::dec << "\n";
        }
        // Register 0x2A of bank 0 IS the DAC - every sample the driver feeds
        // channel six goes through it and nothing else does.
        if (want_dac && entry.bank == 0 && entry.address == 0x2A) {
            state.dac_log << seconds << "\t0x" << std::hex << static_cast<int>(entry.value)
                          << std::dec << "\t" << static_cast<int>(entry.value) << "\n";
        }
    }
    ym.clear_write_log();
}

void drain_psg_log(RuntimeState& state) {
    if (!state.psg_log.is_open()) {
        return;
    }
    auto& psg = state.machine == Machine::genesis ? state.genesis.psg() : state.game_gear.psg();
    for (const auto& entry : psg.write_log()) {
        const double seconds = static_cast<double>(entry.sample) /
                               static_cast<double>(mercury::gamegear::Psg::kDefaultSampleRate);
        // Decoded as well as raw: a latch byte names its channel and a data byte
        // does not, and reading that off a column of hex by hand is exactly the
        // sort of work a log should have already done.
        const bool latch = (entry.value & 0x80) != 0;
        const int channel = latch ? ((entry.value >> 5) & 0x03) : -1;
        const bool volume = latch && (entry.value & 0x10) != 0;
        state.psg_log << seconds << "\t0x" << std::hex << static_cast<int>(entry.value) << std::dec
                      << "\t" << (latch ? "latch" : "data ");
        if (latch) {
            state.psg_log << "\tch" << channel << "\t" << (volume ? "volume" : "tone")
                          << "\t" << (entry.value & 0x0F);
        }
        state.psg_log << "\n";
    }
    psg.clear_write_log();
}

void drain_logs(RuntimeState& state) {
    drain_fm_log(state);
    drain_psg_log(state);
}

void stop_fm_log(RuntimeState& state) {
    if (!state.fm_log.is_open()) {
        return;
    }
    drain_logs(state);
    if (!state.dac_log.is_open()) {
        state.genesis.ym2612().set_write_logging(false);
    }
    state.fm_log.close();
    set_status(state, L"saved " + state.fm_log_path.filename().wstring());
}

void stop_dac_log(RuntimeState& state) {
    if (!state.dac_log.is_open()) {
        return;
    }
    drain_logs(state);
    if (!state.fm_log.is_open()) {
        state.genesis.ym2612().set_write_logging(false);
    }
    state.dac_log.close();
    set_status(state, L"saved " + state.dac_log_path.filename().wstring());
}

void stop_psg_log(RuntimeState& state) {
    if (!state.psg_log.is_open()) {
        return;
    }
    drain_psg_log(state);
    auto& psg = state.machine == Machine::genesis ? state.genesis.psg() : state.game_gear.psg();
    psg.set_write_logging(false);
    state.psg_log.close();
    set_status(state, L"saved " + state.psg_log_path.filename().wstring());
}

void start_fm_log(RuntimeState& state) {
    state.fm_log_path = log_directory() / log_file_name(state, L"-fm.txt");
    state.fm_log.open(state.fm_log_path, std::ios::trunc);
    if (!state.fm_log) {
        set_status(state, L"could not open the FM log");
        return;
    }
    state.fm_log << "seconds\tbank\taddress\tvalue\n";
    state.genesis.ym2612().clear_write_log();
    state.genesis.ym2612().set_write_logging(true);
    set_status(state, L"logging FM writes...");
}

void start_dac_log(RuntimeState& state) {
    state.dac_log_path = log_directory() / log_file_name(state, L"-dac.txt");
    state.dac_log.open(state.dac_log_path, std::ios::trunc);
    if (!state.dac_log) {
        set_status(state, L"could not open the DAC log");
        return;
    }
    state.dac_log << "seconds\thex\tvalue\n";
    if (!state.fm_log.is_open()) {
        state.genesis.ym2612().clear_write_log();
    }
    state.genesis.ym2612().set_write_logging(true);
    set_status(state, L"logging DAC writes...");
}

void start_psg_log(RuntimeState& state) {
    state.psg_log_path = log_directory() / log_file_name(state, L"-psg.txt");
    state.psg_log.open(state.psg_log_path, std::ios::trunc);
    if (!state.psg_log) {
        set_status(state, L"could not open the PSG log");
        return;
    }
    state.psg_log << "seconds\thex\tkind\tchannel\tregister\tdata\n";
    auto& psg = state.machine == Machine::genesis ? state.genesis.psg() : state.game_gear.psg();
    psg.clear_write_log();
    psg.set_write_logging(true);
    set_status(state, L"logging PSG writes...");
}

// --- netplay ----------------------------------------------------------------

// THIS PROGRAM IS THE JOINING END OF A SESSION, and its machine is running on
// the host's save data - the host's cartridge RAM, or for a Sega CD the host's
// whole memory card, every CD game's saves at once. Writing that out would
// replace this player's own files with the host's, so while this is set
// flush_battery writes nothing, and end_netplay reads this player's own files
// back in. Cleared there, or when a different game is loaded.
bool g_netplay_joined = false;

// What the machine looks like to the netplay layer. It knows nothing about a
// Mega Drive; it asks for a fingerprint and a blob and hands back buttons.
mercury::net::Hooks netplay_hooks(RuntimeState& state) {
    mercury::net::Hooks hooks;
    hooks.rom_fingerprint = [&state]() {
        // The cartridge itself, not its filename: two people with the same game
        // under different names must be allowed to play, and the same name over
        // different dumps must not.
        //
        // Whichever machine is loaded. Fingerprinting the Mega Drive
        // unconditionally meant two Game Gear players both hashed an empty
        // cartridge, matched, and were allowed to start two different games.
        std::uint64_t hash = 1469598103934665603ULL;
        const auto& rom = state.machine == Machine::genesis ? state.genesis.cartridge().rom()
                                                            : state.game_gear.cartridge().rom();
        for (const auto byte : rom) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        // A linked pair is a different machine from a lone console running
        // the same cartridge: one end would send two consoles and the other
        // expect one. Folded into the fingerprint so the mismatch is caught
        // at the handshake, with a message that names it, instead of as a
        // state that will not load.
        if (gear_link_active()) {
            hash ^= 0x4745415220204C4EULL;  // "GEAR  LN"
            hash *= 1099511628211ULL;
        }
        // A Sega CD has no cartridge bytes to hash - every disc would match
        // every other - so the disc and the BIOS go in here, and the 32X
        // firmware for a 32X cartridge. The state's own header would refuse
        // a mismatch too, but only after the host had sent it; here it is
        // caught at the handshake.
        if (state.machine == Machine::genesis) {
            const auto media = state.genesis.media_id();
            for (int i = 0; i < 8; ++i) {
                hash ^= static_cast<std::uint8_t>(media >> (i * 8));
                hash *= 1099511628211ULL;
            }
        }
        return hash;
    };
    // A LINKED PAIR GOES OVER AS BOTH CONSOLES AND THE SESSION BETWEEN THEM.
    // Sending only the first left the second wherever it happened to be at
    // each end - opened a few seconds apart, reset at different moments - so
    // the two ends were never the same machine. And the session's two clock
    // baselines decide which console steps next, so without them the joiner
    // holds the same two consoles and still interleaves them differently:
    // measured, it parts on the very first frame. See save_pair_state.
    hooks.capture_state = [&state]() {
        if (state.machine == Machine::game_gear && gear_link_active()) {
            return gear_link_save_pair();
        }
        return state.machine == Machine::genesis ? state.genesis.save_state_blob()
                                                 : state.game_gear.save_state_blob();
    };
    // EXACT, including a Sega CD's backup RAM: both ends must be the same
    // machine, and a game reads its saves while it runs. That leaves this
    // player's memory card holding the host's saves for the session - which
    // is why nothing is written to this player's save files until it ends,
    // and why their own are read back then. See g_netplay_joined.
    hooks.restore_state = [&state](const std::vector<std::uint8_t>& blob, std::string& error) {
        const bool is_pair = mercury::gamegear::GearLinkSession::is_pair_state(blob);
        if (is_pair != (state.machine == Machine::game_gear && gear_link_active())) {
            // The fingerprint should have caught this; refuse, never half-load.
            error = is_pair ? "the host sent a Gear Link pair and no link is open here"
                            : "a Gear Link is open here and the host sent one console";
            return false;
        }
        if (is_pair) {
            return gear_link_load_pair(blob, error);
        }
        return state.machine == Machine::genesis ? state.genesis.load_state_blob(blob, error)
                                                 : state.game_gear.load_state_blob(blob, error);
    };
    return hooks;
}

std::uint64_t netplay_state_hash(RuntimeState& state) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto fold = [&hash](const std::vector<std::uint8_t>& blob) {
        for (const auto byte : blob) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
    };
    // Whichever machine is actually running. Hashing the Mega Drive
    // unconditionally made the drift check useless for a Game Gear session: it
    // hashed an idle machine that both ends agreed about no matter how far
    // apart the games they were playing had drifted.
    if (state.machine == Machine::genesis) {
        fold(state.genesis.save_state_blob());
    } else {
        fold(state.game_gear.save_state_blob());
    }
    return hash;
}

// One frame of the agreement. Returns false if the frame must not run yet
// because the other player's buttons for it have not arrived.
//
// Both ends read their OWN controls as player one and let the roles decide
// which port that becomes, so neither player has to configure anything
// differently from single-player.
std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text);
void update_title(const RuntimeState& state);
bool capturing();
bool kind_has_button(PadKind kind, PadButton button);
bool kind_is_light_gun(PadKind kind);

// Keep the title telling the truth about the session, which runs on its own
// thread and would otherwise never reach the window.
void refresh_netplay_status(RuntimeState& state) {
    std::wstring text;
    switch (state.netplay.status()) {
        case mercury::net::Status::idle:
            break;
        case mercury::net::Status::running: {
            const auto peer = state.netplay.peer().nickname;
            text = L"netplay with " + widen(peer.empty() ? std::string("player") : peer) +
                   L" - you are player " +
                   std::to_wstring(state.netplay.local_port_index() + 1);
            if (state.netplay.desynced()) {
                text += L" - THE GAMES HAVE DRIFTED APART";
            } else if (state.netplay_stalled) {
                text += L" - waiting";
            }
            break;
        }
        default:
            text = L"netplay: " + widen(state.netplay.message());
            break;
    }
    if (text != state.netplay_label) {
        state.netplay_label = text;
        update_title(state);
    }
}

mercury::net::PlayerInput capture_netplay_input(RuntimeState& state, PadKind seat_kind);

// A seat kind as it crosses the wire: a PadKind's value, refused back to a
// three-button pad if it is not one this build knows.
PadKind seat_kind_from_byte(std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(PadKind::miracle_piano) ? static_cast<PadKind>(value)
                                                                      : PadKind::three_button;
}

bool netplay_step(RuntimeState& state) {
    const auto wire = state.netplay.seat_kinds();
    const PadKind seats[2] = {seat_kind_from_byte(wire[0]), seat_kind_from_byte(wire[1])};
    const int mine = state.netplay.local_port_index();

    // This person's own controls, whatever their Input tab calls them, as the
    // device in THEIR seat - with a gun's aim if that seat holds one.
    const auto local = capture_netplay_input(state, seats[mine]);
    mercury::net::PlayerInput first;
    mercury::net::PlayerInput second;
    if (!state.netplay.exchange(local, first, second)) {
        return false;
    }
    // THE HOST'S CONTROLLERS, AT BOTH ENDS. Plugged in on the first frame the
    // session runs - the same frame at both ends, before anything has read a
    // port - so a joiner whose Input tab says something else plays on the
    // host's setup and gets its own back when the session ends.
    if (!state.netplay_seats_applied) {
        apply_port_kinds_for(state, seats[0], seats[1]);
        state.netplay_seat_kinds = {seats[0], seats[1]};
        state.netplay_seats_applied = true;
    }
    mercury::net::apply_genesis_seats(state.genesis, {port_kind_of(seats[0]), port_kind_of(seats[1])},
                                      {first, second});
    state.netplay_seat_inputs = {first, second};
    if (state.netplay_frame % mercury::net::Session::kStateCheckInterval == 0) {
        state.netplay.report_state_hash(state.netplay_frame, netplay_state_hash(state));
    }
    ++state.netplay_frame;
    return true;
}

// One console's buttons, straight from a netplay mask.
//
// Not through apply_pad_button: that folds two local players onto one shared
// console, which is the right thing for Micro Machines on one machine and the
// wrong thing here. Over a cable each console has exactly one pair of hands.
void apply_mask_to_game_gear(RuntimeState& state, std::uint16_t mask) {
    using mercury::gamegear::Button;
    const auto held = [mask](PadButton button) {
        return (mask & (1u << static_cast<int>(button))) != 0;
    };
    auto& input = state.game_gear.input_state();
    input.set_button(Button::up, held(PadButton::up));
    input.set_button(Button::down, held(PadButton::down));
    input.set_button(Button::left, held(PadButton::left));
    input.set_button(Button::right, held(PadButton::right));
    input.set_button(Button::button1, held(PadButton::a));
    input.set_button(Button::button2, held(PadButton::b));
    input.set_button(Button::start, held(PadButton::start));
    state.start_pulse_frames_remaining = 0;
}

// A linked pair, played over the network.
//
// The cable is NOT tunnelled. Both consoles run on BOTH machines, and netplay
// carries the same two button masks it already carries for a shared Mega Drive
// pad. The cable traffic then stays local at each end, and because the pair is
// deterministic - same ROMs, same reset, the same fixed head start - the two
// simulations stay identical without a single serial byte crossing the
// network. Tunnelling the cable instead would mean making network latency
// match cable latency, which is a much harder problem for no gain.
//
// Returns false when the other player's buttons have not arrived, exactly like
// the single-console path: neither console may run until they have.
bool gear_link_netplay_step(RuntimeState& primary, RuntimeState& second) {
    const std::uint16_t local = primary.netplay_local_mask;
    std::uint16_t first = 0;
    std::uint16_t partner = 0;
    if (!primary.netplay.exchange(local, first, partner)) {
        return false;
    }
    apply_mask_to_game_gear(primary, first);
    apply_mask_to_game_gear(second, partner);
    if (primary.netplay_frame % mercury::net::Session::kStateCheckInterval == 0) {
        // BOTH machines folded into one number. Hashing only the first console
        // would miss a drift that happened entirely on the second.
        std::uint64_t hash = netplay_state_hash(primary);
        const auto other = netplay_state_hash(second);
        hash ^= other + 0x9E3779B97F4A7C15ULL + (hash << 6) + (hash >> 2);
        primary.netplay.report_state_hash(primary.netplay_frame, hash);
    }
    ++primary.netplay_frame;
    return true;
}

// --- the diagnostic log ----------------------------------------------------
//
// One shot: everything the machine can say about itself at this instant,
// written as text. The point is to answer "why is this game's picture or sound
// wrong, or why did it stop" without the person reporting it having to run a
// probe tool or describe what they saw.
//
// It is deliberately made of NUMBERS THAT MEAN SOMETHING rather than a register
// dump alone - a raw dump tells you what the chip holds, but not that the game
// has never turned the display on, or that the CPU is sitting on four addresses,
// or that the sound driver has not written a note in ten seconds. Those are the
// lines that end an investigation early. The raw registers are there too, at the
// bottom, in hex, for when they are the thing that is actually needed.
void write_diagnostic_log(RuntimeState& state) {
    const auto path = log_directory() / log_file_name(state, L"-diagnostic.txt");
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        set_status(state, L"could not write the diagnostic log");
        return;
    }
    const auto hex = [&out](unsigned value, int digits) {
        const auto flags = out.flags();
        out << "0x" << std::uppercase << std::hex << std::setw(digits) << std::setfill('0') << value;
        out.flags(flags);
        out << std::dec;
    };

    out << "GenX diagnostic log\n";
    out << "===================\n\n";
    out << "cartridge : " << state.rom_path.filename().string() << "\n";
    out << "machine   : " << (state.machine == Machine::genesis ? "Mega Drive / Genesis"
                                                                : "Game Gear / Master System")
        << "\n";
    out << "loaded    : " << (state.rom_loaded ? "yes" : "NO - nothing is running") << "\n";
    out << "paused    : " << (state.paused ? "yes" : "no") << "\n\n";

    // The Miracle Piano, if one is plugged in. Reported before the
    // cartridge check because "is my keyboard reaching the game" is a
    // question worth answering even when nothing is loaded, and because the
    // counters say exactly where a note stopped: the window sets keys, the
    // device queues bytes, and the console clocks them out. Whichever of
    // those three is zero is the one that is broken.
    if (state.machine == Machine::genesis &&
        state.genesis.port2().kind() ==
            mercury::genesis::ControllerPort::Kind::miracle_piano) {
        const auto& piano = state.genesis.port2().piano().activity();
        out << "-- Miracle Piano (port 2) -----------------------------------\n";
        out << "window open      : " << (piano_window_open() ? "yes" : "no") << "\n";
        out << "bytes from game  : " << piano.bytes_from_console << "\n";
        out << "bytes to game    : " << piano.bytes_to_console << "\n";
        out << "polls            : " << piano.polls << "\n";
        out << "sysex handled    : " << piano.sysex_handled << "\n";
        if (piano.polls == 0) {
            out << "The game has not polled the keyboard at all. It is not looking for\n"
                   "one, which usually means the cartridge is not the Miracle Piano.\n";
        } else if (piano.bytes_to_console == 0) {
            out << "The game is polling but the keyboard has sent nothing back.\n";
        }
        out << "\n";
    }

    // Input, before the cartridge check, because "nothing I press does
    // anything" is a question worth answering with no game running. Every way
    // input can be silently dropped is listed here, because from the outside
    // they all look the same: a dead pad, a device bound to a window that no
    // longer exists, a controller kind that has no buttons, an unbound row,
    // and a key-capture hook that outlived its dialog and is eating the lot.
    {
        const auto& devices = dinput_devices();
        out << "-- Input ----------------------------------------------------\n";
        out << "key capture hook : " << (capturing() ? "INSTALLED - THIS SWALLOWS EVERY KEY"
                                                     : "not installed")
            << "\n";
        // Windows sends a key only to the focused window, so a keyboard that
        // does nothing is very often a keyboard going somewhere else.
        const HWND focused = GetForegroundWindow();
        out << "foreground window: "
            << (focused == state.window ? "this one"
                : focused == nullptr    ? "NONE"
                : piano_window_open() && focused == piano_window()
                                        ? "the Miracle Piano"
                : gear_link_partner_window(state) != nullptr &&
                          focused == gear_link_partner_window(state)
                                        ? "the other Gear Link console"
                                        : "SOMETHING ELSE - keys are going there")
            << "\n";
        out << "DirectInput      : " << devices.count << " device(s), bound to "
            << (devices.owner == nullptr ? "NOTHING" : "the main window") << "\n";
        if (devices.cooperative_failures != 0 || devices.acquire_failures != 0) {
            out << "  setup failures : " << devices.cooperative_failures << " cooperative, "
                << devices.acquire_failures << " acquire\n";
        }
        for (int slot = 0; slot < devices.count; ++slot) {
            out << "  slot " << slot << "         : " << narrow(devices.name[slot]) << "\n";
        }
        for (int player = 0; player < 2; ++player) {
            const auto& input = state.players[player];
            static const char* kDeviceNames[] = {"keyboard", "XInput", "DirectInput", "mouse"};
            static const char* kKindNames[] = {"3-button", "6-button", "Justifier", "Menacer",
                                               "Miracle Piano"};
            int bound = 0;
            int usable = 0;
            for (int index = 0; index < kPadButtonCount; ++index) {
                const auto button = static_cast<PadButton>(index);
                const bool has_binding = input.device == InputDevice::dinput
                                             ? input.dpad[index] != kPadInputNone
                                         : input.device == InputDevice::xinput
                                             ? input.pad[index] != kPadInputNone
                                             : input.keys[index] != 0;
                if (!has_binding) {
                    continue;
                }
                // The trigger and reload rows carry a default in every profile
                // ever saved, gun or not. Counting those as bindings on a pad
                // made an ordinary unbound player two read as "2 bound, 0
                // reaching the game" and fire the warning below at nobody.
                const bool gun_row =
                    button == PadButton::gun_trigger || button == PadButton::gun_reload;
                if (gun_row && !kind_is_light_gun(input.kind)) {
                    continue;
                }
                ++bound;
                if (kind_has_button(input.kind, button)) {
                    ++usable;
                }
            }
            out << "player " << (player + 1) << "         : "
                << kDeviceNames[static_cast<int>(input.device)] << ", "
                << kKindNames[static_cast<int>(input.kind)] << ", slot " << input.pad_slot << ", "
                << bound << " bound, " << usable << " reaching the game\n";
            if (device_is_gamepad(input.device)) {
                out << "                   " << (state.pad_connected[player] ? "pad answering"
                                                                             : "NO PAD ANSWERING")
                    << "\n";
            }
            if (bound != 0 && usable == 0) {
                out << "                   Every binding is being discarded. The controller\n"
                       "                   kind above has no room for them - the Miracle Piano\n"
                       "                   has no buttons at all, and a light gun has only a\n"
                       "                   few. Change the kind on the Input tab.\n";
            }
        }

        // What actually became of the last few keys. This is the line that
        // should end a "my keyboard stopped working" report.
        out << "keys seen        : " << state.keys_seen << "\n";
        if (state.keys_seen == 0) {
            out << "  No key has reached the window at all. The keys are going somewhere\n"
                   "  else entirely - check the foreground window above.\n";
        } else {
            out << "  most recent first:\n";
            for (std::size_t step = 0; step < state.key_history.size(); ++step) {
                const auto index =
                    (state.key_history_next + state.key_history.size() - 1 - step) %
                    state.key_history.size();
                const auto& event = state.key_history[index];
                if (event.outcome == nullptr) {
                    continue;
                }
                out << "    key 0x" << std::uppercase << std::hex << event.key << std::dec
                    << "  " << event.outcome;
                if (event.delivered > 0) {
                    out << " (" << event.delivered << " button"
                        << (event.delivered == 1 ? "" : "s") << ")";
                }
                out << "\n";
            }
        }
        out << "\n";
    }

    if (!state.rom_loaded) {
        out << "No cartridge is loaded, so there is nothing to report on.\n";
        set_status(state, L"wrote " + path.filename().wstring());
        return;
    }

    if (state.machine != Machine::genesis) {
        const auto& gg = state.game_gear;
        out << "-- Game Gear ------------------------------------------------\n";
        out << gg.describe() << "\n";
        out << "PSG: " << gg.psg().describe() << "\n";
        out << gg.psg().activity_summary() << "\n";
        set_status(state, L"wrote " + path.filename().wstring());
        return;
    }

    // Non-const: the chip accessors are, even though nothing here writes.
    auto& machine = state.genesis;
    const auto& vdp = machine.vdp();

    // --- picture -----------------------------------------------------------
    out << "-- picture --------------------------------------------------\n";
    out << "display enabled : " << (vdp.display_enabled() ? "yes" : "NO")
        << (vdp.display_enabled() ? "\n" : "   <-- a blank screen starts here\n");
    out << "resolution      : " << vdp.screen_width() << "x" << vdp.screen_height()
        << (vdp.wide_mode() ? "  (H40)\n" : "  (H32)\n");
    out << "frames drawn    : " << vdp.frames_elapsed() << "\n";
    out << "VRAM writes     : " << vdp.vram_write_count() << "\n";
    out << "data port writes: " << vdp.data_write_count() << "\n";
    out << "DMA transfers   : " << vdp.dma_count() << "\n";
    if (vdp.vram_write_count() == 0) {
        out << "  NOTHING has been written to VRAM. The game is not drawing at all,\n"
               "  which is a boot or a bus problem rather than a rendering one.\n";
    }

    // --- sound -------------------------------------------------------------
    const auto& ym = machine.ym2612();
    out << "\n-- sound ----------------------------------------------------\n";
    out << "FM key-ons      : " << ym.key_on_count() << "\n";
    out << "DAC writes      : " << ym.dac_write_count() << "\n";
    out << "SSG-EG enables  : " << ym.ssg_enable_count() << "\n";
    out << "LFO enables     : " << ym.lfo_enable_count() << "\n";
    out << "CSM enables     : " << ym.csm_enable_count() << "\n";
    out << "PSG writes      : " << machine.psg().write_count() << "\n";
    out << "PSG: " << machine.psg().activity_summary() << "\n";
    if (ym.key_on_count() == 0 && machine.psg().write_count() == 0) {
        out << "  Neither sound chip has been written to. Silence here is the game\n"
               "  never asking for a note, not the chips playing it wrongly.\n";
    }

    // --- processors --------------------------------------------------------
    out << "\n-- processors -----------------------------------------------\n";
    const auto& regs = machine.m68k().registers();
    out << "68000 PC        : ";
    hex(regs.pc, 6);
    out << "\n";
    out << "68000 SR        : ";
    hex(regs.sr, 4);
    out << "\n";
    out << "unknown opcodes : " << machine.m68k_unimplemented_count();
    if (machine.m68k_unimplemented_count() > 0) {
        out << "   <-- the CPU has hit instructions GenX does not implement";
    }
    out << "\n";
    out << "Z80 instructions: " << machine.z80_instructions() << "\n";
    out << "Z80 unknown ops : " << machine.z80_unimplemented_count() << "\n";
    out << "Z80 interrupts  : " << machine.z80_interrupts() << "\n";
    out << "\nD0-D7 :";
    for (int i = 0; i < 8; ++i) {
        out << " ";
        hex(regs.d[i], 8);
    }
    out << "\nA0-A7 :";
    for (int i = 0; i < 8; ++i) {
        out << " ";
        hex(regs.a[i], 8);
    }
    out << "\n";

    // --- interrupts --------------------------------------------------------
    out << "\n-- interrupts -----------------------------------------------\n";
    out << "vertical taken  : " << machine.m68k_vint_count() << "\n";
    out << "horizontal taken: " << machine.m68k_hint_count() << "\n";
    out << "external taken  : " << machine.m68k_ext_count() << "\n";
    const auto frames = vdp.frames_elapsed();
    if (frames > 0) {
        const double vint_rate = static_cast<double>(machine.m68k_vint_count()) / frames;
        out << "vertical / frame: " << vint_rate;
        if (vint_rate > 4.0) {
            out << "   <-- far too many: an interrupt is being raised and never retired";
        } else if (vint_rate < 0.1 && frames > 60) {
            out << "   <-- the game is not taking vertical interrupts at all";
        }
        out << "\n";
    }

    // --- raw registers -----------------------------------------------------
    out << "\n-- VDP registers --------------------------------------------\n";
    const auto& vdp_regs = vdp.registers();
    for (std::size_t i = 0; i < vdp_regs.size(); ++i) {
        if (i % 8 == 0) {
            out << "\n  ";
            hex(static_cast<unsigned>(i), 2);
            out << ": ";
        }
        hex(vdp_regs[i], 2);
        out << " ";
    }
    out << "\n";

    // The palette, in full. Small enough to always include and the single most
    // common reason a picture is wrong while every other number reads healthy:
    // a correct tilemap drawn in an all-black palette looks exactly like a dead
    // renderer until you look here.
    out << "\n-- CRAM (palette, 4 lines of 16 colours) --------------------\n";
    const auto& cram = vdp.cram();
    int non_black = 0;
    for (int line = 0; line < 4; ++line) {
        out << "  line " << line << ":";
        for (int slot = 0; slot < 16; ++slot) {
            const std::size_t index = static_cast<std::size_t>(line * 16 + slot);
            const std::uint16_t colour = index < cram.size() ? cram[index] : 0;
            if ((colour & 0x0EEE) != 0) {
                ++non_black;
            }
            out << " ";
            hex(colour & 0x0FFF, 3);
        }
        out << "\n";
    }
    // Distinct as well as non-black. All 64 slots holding the SAME colour is a
    // flat screen, and reads as healthy on a "not black" count alone - Sonic 3
    // sits at 64 of 64 non-black during its white logo flash, which is correct
    // and would otherwise look like a fault.
    int distinct = 0;
    for (std::size_t i = 0; i < 64 && i < cram.size(); ++i) {
        bool seen_before = false;
        for (std::size_t j = 0; j < i; ++j) {
            if ((cram[j] & 0x0FFF) == (cram[i] & 0x0FFF)) {
                seen_before = true;
                break;
            }
        }
        if (!seen_before) {
            ++distinct;
        }
    }
    out << "  not black: " << non_black << " of 64      distinct colours: " << distinct;
    if (non_black == 0) {
        out << "\n  The whole palette is black, so nothing can be seen whatever the\n"
               "  tilemap holds.";
    } else if (distinct == 1) {
        out << "\n  Every slot is the same colour, so the screen is one flat field.\n"
               "  Normal during a fade or a flash, a fault if it persists.";
    }
    out << "\n";

    out << "\n-- VSRAM (vertical scroll) ----------------------------------\n";
    out << "  plane A: " << vdp.vsram_entry(0) << "   plane B: " << vdp.vsram_entry(1) << "\n";

    out << "\n-- notable VDP bits -----------------------------------------\n";
    out << "H40 wide mode   : " << (vdp.wide_mode() ? "on" : "off") << "\n";
    out << "vertical int    : " << ((vdp_regs[1] & 0x20) != 0 ? "enabled" : "disabled") << "\n";
    out << "horizontal int  : " << ((vdp_regs[0] & 0x10) != 0 ? "enabled" : "disabled") << "\n";
    out << "external int    : " << (vdp.external_interrupt_enabled() ? "enabled" : "disabled")
        << "   (light gun)\n";
    out << "HV counter latch: " << (vdp.hv_latch_enabled() ? "enabled" : "disabled")
        << "   (light gun)\n";
    out << "line counter    : " << static_cast<int>(vdp_regs[10]) << "\n";

    out << "\n-- light guns -----------------------------------------------\n";
    out << "beam seen       : " << vdp.light_gun_latches() << "\n";
    out << "  with latch on : " << vdp.light_gun_hv_latches() << "\n";
    out << "  with int on   : " << vdp.light_gun_interrupts() << "\n";
    out << "port 1          : " << machine.port1().describe() << "\n";
    out << "port 2          : " << machine.port2().describe() << "\n";

    out << "\n-- how to read this -----------------------------------------\n";
    out << "A blank picture with VRAM writes at zero is a boot problem.\n"
           "A blank picture with the display disabled is the game's own choice,\n"
           "  usually because it is waiting for something.\n"
           "Silence with no key-ons and no PSG writes is the driver, not the chips.\n"
           "Unknown opcodes above zero means the CPU met something unimplemented,\n"
           "  and everything after that point is unreliable.\n"
           "Vertical interrupts per frame far above one means an interrupt is\n"
           "  being raised and never retired, which presents as a freeze.\n";

    out.close();
    set_status(state, L"wrote " + path.filename().wstring());
}

// VRAM and work RAM as hex, on demand.
//
// Kept out of the diagnostic log deliberately: these are 64KB each and come to
// several hundred kilobytes of text, which is fine as a thing you ask for and
// wrong as a thing that happens every time you press a key. The palette, which
// is the small one that explains most picture faults, is in the diagnostic log
// instead.
void dump_memory(RuntimeState& state) {
    if (!state.rom_loaded || state.machine != Machine::genesis) {
        set_status(state, L"load a Mega Drive cartridge first");
        return;
    }
    const auto path = log_directory() / log_file_name(state, L"-memory.txt");
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        set_status(state, L"could not write the memory dump");
        return;
    }
    // Sixteen bytes a row with the offset at the front, which is what every
    // other hex dump in the world looks like and so needs no explaining.
    const auto dump = [&out](const char* title, const std::vector<std::uint8_t>& bytes) {
        out << "\n-- " << title << " (" << bytes.size() << " bytes) "
            << std::string(40, '-') << "\n";
        std::size_t blank_run = 0;
        for (std::size_t offset = 0; offset < bytes.size(); offset += 16) {
            bool all_zero = true;
            for (std::size_t i = 0; i < 16 && offset + i < bytes.size(); ++i) {
                if (bytes[offset + i] != 0) {
                    all_zero = false;
                    break;
                }
            }
            // Runs of zeroes are collapsed. A mostly empty 64KB region is the
            // normal case and printing four thousand identical rows buries the
            // part that matters.
            if (all_zero) {
                ++blank_run;
                continue;
            }
            if (blank_run > 0) {
                out << "  ... " << (blank_run * 16) << " bytes of zero ...\n";
                blank_run = 0;
            }
            char line[16];
            std::snprintf(line, sizeof(line), "  %04X: ", static_cast<unsigned>(offset));
            out << line;
            for (std::size_t i = 0; i < 16; ++i) {
                if (offset + i < bytes.size()) {
                    std::snprintf(line, sizeof(line), "%02X ",
                                  static_cast<unsigned>(bytes[offset + i]));
                    out << line;
                } else {
                    out << "   ";
                }
            }
            out << " |";
            for (std::size_t i = 0; i < 16 && offset + i < bytes.size(); ++i) {
                const auto byte = bytes[offset + i];
                out << (byte >= 0x20 && byte < 0x7F ? static_cast<char>(byte) : '.');
            }
            out << "|\n";
        }
        if (blank_run > 0) {
            out << "  ... " << (blank_run * 16) << " bytes of zero ...\n";
        }
    };

    out << "GenX memory dump\n";
    out << "================\n";
    out << "cartridge : " << state.rom_path.filename().string() << "\n";
    out << "\nRuns of sixteen zero bytes are collapsed to a single line.\n";
    dump("VRAM", state.genesis.vdp().vram());
    dump("work RAM", state.genesis.work_ram());
    out.close();
    set_status(state, L"wrote " + path.filename().wstring());
}

void record_samples(RuntimeState& state, const std::vector<std::int16_t>& samples) {
    if (!state.recording.is_open() || samples.empty()) {
        return;
    }
    state.recording.write(reinterpret_cast<const char*>(samples.data()),
                          static_cast<std::streamsize>(samples.size() * sizeof(std::int16_t)));
    state.recorded_samples += static_cast<std::uint32_t>(samples.size());
}

void collect_audio(RuntimeState& state) {
    if (state.machine == Machine::genesis) {
        collect_audio_genesis(state);
        return;
    }
    const auto& source = state.game_gear.psg().sample_buffer();
    if (!source.empty()) {
        std::vector<std::int16_t> mixed(source.begin(), source.end());
        // The keyboard can be played over anything, so it is mixed here too.
        // It has no cartridge to talk to on a Game Gear, but it still makes
        // its own sound - which is what the instrument does.
        mix_piano_audio(state, mixed);
        record_samples(state, mixed);
        state.pending_audio.insert(state.pending_audio.end(), mixed.begin(), mixed.end());
        state.game_gear.psg().clear_sample_buffer();
    }
}

void service_audio(RuntimeState& state) {
    if (state.wave_out == nullptr) {
        return;
    }

    // The device queue is bounded - four buffers - but pending_audio is not, and
    // it only drains a chunk per call. Any stretch where frames are produced
    // faster than real time piles samples up here and the sound then trails the
    // picture by however much has accumulated, permanently. Dropping the oldest
    // costs one click; carrying them costs seconds of lag that never recovers.
    constexpr std::size_t kMaxPendingSamples = kAudioChunkSamples * 6;
    if (state.pending_audio.size() > kMaxPendingSamples) {
        state.pending_audio.erase(
            state.pending_audio.begin(),
            state.pending_audio.end() - static_cast<std::ptrdiff_t>(kMaxPendingSamples));
    }

    std::size_t active_buffers = 0;
    for (auto& buffer : state.audio_buffers) {
        if (buffer.submitted && (buffer.header.dwFlags & WHDR_DONE) == 0) {
            ++active_buffers;
            continue;
        }

        if (buffer.prepared) {
            waveOutUnprepareHeader(state.wave_out, &buffer.header, sizeof(WAVEHDR));
            buffer.prepared = false;
            buffer.submitted = false;
            buffer.header.dwFlags = 0;
        }
    }

    for (auto& buffer : state.audio_buffers) {
        if (buffer.prepared || buffer.submitted) {
            continue;
        }

        if (state.pending_audio.empty()) {
            break;
        }

        if (active_buffers != 0 && state.pending_audio.size() < kAudioChunkSamples) {
            break;
        }

        const auto copy_count = std::min<std::size_t>(kAudioChunkSamples, state.pending_audio.size());
        if (copy_count == 0) {
            break;
        }

        for (std::size_t index = 0; index < copy_count; ++index) {
            buffer.samples[index] = amplify_sample(state, state.pending_audio[index]);
        }
        state.pending_audio.erase(
            state.pending_audio.begin(),
            state.pending_audio.begin() + static_cast<std::ptrdiff_t>(copy_count));

        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
        buffer.header.dwBufferLength = static_cast<DWORD>(copy_count * sizeof(std::int16_t));
        buffer.header.dwFlags = 0;
        if (waveOutPrepareHeader(state.wave_out, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            continue;
        }
        buffer.prepared = true;
        if (waveOutWrite(state.wave_out, &buffer.header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            buffer.submitted = true;
            ++active_buffers;
        } else {
            waveOutUnprepareHeader(state.wave_out, &buffer.header, sizeof(WAVEHDR));
            buffer.prepared = false;
            buffer.submitted = false;
        }
    }
}

void step_until_next_frame_genesis(RuntimeState& state) {
    state.genesis.run_frame();
    // A SEGA CD 32X DISC WITH NO 32X FIRMWARE IS A BLACK SCREEN AND NO CLUE.
    //
    // Six discs use both add-ons at once - Night Trap, Supreme Warrior,
    // Corpse Killer, Fahrenheit's second disc. A cartridge does not need the
    // 32X BIOS, because GenX synthesises its startup from the cartridge
    // header; a disc has no such header, so the real firmware is the only way
    // in and without it the SH-2s never run. The disc looks broken.
    //
    // There is no way to know a disc is one of those before it says so, and
    // it says so by switching the adapter on. That is the moment to speak,
    // and only once - the flag is cleared when a game is loaded.
    if (!state.warned_no_32x_bios && state.genesis.cd_mode() &&
        state.genesis.thirtytwox().adapter().enabled() &&
        !state.genesis.thirtytwox().bios_loaded()) {
        state.warned_no_32x_bios = true;
        set_status(state, L"This disc uses the 32X, and no 32X firmware was found - "
                          L"set its folder on the BIOS tab in Configure.");
    }
    state.last_presented_frame = state.genesis.vdp().frames_elapsed();
    update_framebuffer(state);
    apply_pending_resize(state);
    collect_audio(state);
    service_audio(state);
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "Frame %u  PC=%06X  %ux%u",
                  state.last_presented_frame,
                  state.genesis.m68k().registers().pc,
                  state.genesis.vdp().screen_width(),
                  state.genesis.vdp().screen_height());
    state.status_text = buffer;
}

// Everything that happens once the Game Gear's VDP says a frame is finished:
// put the picture up, hand the samples to the audio device, refresh the status
// line. Split out because a Gear Link pair has the frame itself run by the link
// session - the two consoles have to be interleaved instruction by instruction,
// which cannot be done from inside either one's own stepping loop - but still
// needs everything after it.
void present_game_gear_frame(RuntimeState& state) {
    state.last_presented_frame = state.game_gear.vdp().frames_elapsed();
    if (state.start_pulse_frames_remaining != 0) {
        --state.start_pulse_frames_remaining;
        if (state.start_pulse_frames_remaining == 0) {
            state.game_gear.input_state().set_button(Button::start, false);
        }
    }
    update_framebuffer(state);
    apply_pending_resize(state);
    collect_audio(state);
    service_audio(state);
    state.status_text = "Frame " + std::to_string(state.last_presented_frame);
}

void step_until_next_frame(RuntimeState& state) {
    if (state.machine == Machine::genesis) {
        step_until_next_frame_genesis(state);
        return;
    }
    const auto start_frame = state.game_gear.vdp().frames_elapsed();

    for (int steps = 0; steps < kFrameBudgetSteps; ++steps) {
        const auto result = state.game_gear.step();
        if (!result.executed || result.unimplemented) {
            std::string message = "Stopped at PC=";
            char buffer[64];
            std::snprintf(
                buffer,
                sizeof(buffer),
                "%04X OP=%s%02X %s",
                result.pc_before,
                result.prefix != 0x00 ? "" : "",
                result.opcode,
                result.message.c_str());
            message += buffer;
            fault(state, message);
            return;
        }

        if (state.game_gear.vdp().frames_elapsed() != start_frame) {
            present_game_gear_frame(state);
            return;
        }
    }

    fault(state, "Frame budget exceeded while stepping.");
}

// --- shell -----------------------------------------------------------------

const Theme& theme_of(const RuntimeState& state) {
    return state.dark_theme ? kDarkTheme : kLightTheme;
}

std::wstring widen(const std::string& text);

// Decode an embedded PNG into a premultiplied 32-bit DIB. Premultiplied because
// that is the only format AlphaBlend accepts, and every one of these images -
// the logo and the four controllers - has to sit on both a dark and a white
// background without a halo.
HBITMAP decode_png_resource(HINSTANCE instance, int id, int& width, int& height) {
    // MAKEINTRESOURCEW(10) rather than RT_RCDATA: this translation unit does not
    // define UNICODE, so the RT_ macros expand to their ANSI form and will not
    // pass to the W entry point.
    HRSRC found = FindResourceW(instance, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (found == nullptr) {
        return nullptr;
    }
    const DWORD size = SizeofResource(instance, found);
    HGLOBAL resource = LoadResource(instance, found);
    if (resource == nullptr || size == 0) {
        return nullptr;
    }
    const void* bytes = LockResource(resource);
    if (bytes == nullptr) {
        return nullptr;
    }
    // GDI+ wants an IStream, and the stream wants memory it owns - a resource
    // pointer is not moveable memory, so it has to be copied.
    HGLOBAL owned = GlobalAlloc(GMEM_MOVEABLE, size);
    if (owned == nullptr) {
        return nullptr;
    }
    if (void* copy = GlobalLock(owned)) {
        std::memcpy(copy, bytes, size);
        GlobalUnlock(owned);
    }
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(owned, TRUE, &stream) != S_OK || stream == nullptr) {
        GlobalFree(owned);
        return nullptr;
    }

    HBITMAP bitmap = nullptr;
    {
        Gdiplus::Bitmap image(stream);
        if (image.GetLastStatus() == Gdiplus::Ok) {
            width = static_cast<int>(image.GetWidth());
            height = static_cast<int>(image.GetHeight());
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;  // top-down, to match GDI+
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (bitmap != nullptr && pixels != nullptr) {
                Gdiplus::Rect rect(0, 0, width, height);
                Gdiplus::BitmapData locked{};
                locked.Width = static_cast<UINT>(width);
                locked.Height = static_cast<UINT>(height);
                locked.Stride = width * 4;
                locked.PixelFormat = PixelFormat32bppPARGB;
                locked.Scan0 = pixels;
                const auto status = image.LockBits(
                    &rect, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeUserInputBuf,
                    PixelFormat32bppPARGB, &locked);
                if (status == Gdiplus::Ok) {
                    image.UnlockBits(&locked);
                } else {
                    DeleteObject(bitmap);
                    bitmap = nullptr;
                }
            }
        }
    }
    stream->Release();
    return bitmap;
}

// GDI+, kept alive for the whole run.
//
// The logo and the pad artwork each start and stop it around their own decode,
// which is right for a one-off: the DIB that comes out is an ordinary GDI
// object and outlives the library. Pixel smoothing is different - it runs on
// every painted frame, and starting a graphics library sixty times a second
// is not a thing to do. Nested startups are legal, so the scoped ones are left
// exactly as they are.
struct GdiPlusSession {
    ULONG_PTR token = 0;
    bool running = false;

    void start() {
        if (running) {
            return;
        }
        Gdiplus::GdiplusStartupInput input;
        running = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }
    void stop() {
        if (running) {
            Gdiplus::GdiplusShutdown(token);
            running = false;
        }
    }
};

GdiPlusSession& gdiplus_session() {
    static GdiPlusSession instance;
    return instance;
}

// --- pixel smoothing on the graphics card ----------------------------------
//
// One factory for the process; render targets and bitmaps are per window,
// because a Gear Link has two of them and a target belongs to its HWND.
ID2D1Factory* d2d_factory() {
    static ID2D1Factory* factory = []() -> ID2D1Factory* {
        ID2D1Factory* made = nullptr;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &made))) {
            return nullptr;
        }
        return made;
    }();
    return factory;
}

void release_d2d(RuntimeState& state) {
    if (state.d2d_scanlines != nullptr) {
        state.d2d_scanlines->Release();
        state.d2d_scanlines = nullptr;
    }
    if (state.d2d_picture != nullptr) {
        state.d2d_picture->Release();
        state.d2d_picture = nullptr;
    }
    if (state.d2d_target != nullptr) {
        state.d2d_target->Release();
        state.d2d_target = nullptr;
    }
    state.d2d_target_width = 0;
    state.d2d_target_height = 0;
    state.d2d_picture_width = 0;
    state.d2d_picture_height = 0;
}

// The scanline overlay as a two-pixel-tall tile: one transparent row and one
// dark one, wrapped across the picture. Drawn at device resolution like the
// GDI version, so the bands stay one screen pixel thick however far the frame
// is blown up.
bool ensure_d2d_scanlines(RuntimeState& state) {
    if (state.d2d_scanlines != nullptr) {
        return true;
    }
    const std::uint32_t rows[2] = {0x00000000u, 0x73000000u};  // premultiplied
    ID2D1Bitmap* tile = nullptr;
    const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    if (FAILED(state.d2d_target->CreateBitmap(D2D1::SizeU(1, 2), rows,
                                              static_cast<UINT32>(sizeof(std::uint32_t)),
                                              D2D1::BitmapProperties(format), &tile)) ||
        tile == nullptr) {
        return false;
    }
    const auto brush_properties =
        D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    const bool ok = SUCCEEDED(state.d2d_target->CreateBitmapBrush(tile, brush_properties,
                                                                  &state.d2d_scanlines));
    tile->Release();
    return ok && state.d2d_scanlines != nullptr;
}

// Put the frame up through the graphics card. Returns false for any reason at
// all, and the caller falls back to the GDI path for that frame - a machine
// with no Direct2D, a lost device, a window mid-resize.
bool present_smooth_d2d(RuntimeState& state, HWND hwnd, const RECT& client, const RECT& picture,
                        std::size_t frame_width, std::size_t frame_height) {
    ID2D1Factory* factory = d2d_factory();
    if (factory == nullptr || frame_width == 0 || frame_height == 0) {
        return false;
    }
    const auto width = static_cast<unsigned>(client.right - client.left);
    const auto height = static_cast<unsigned>(client.bottom - client.top);
    if (width == 0 || height == 0) {
        return false;
    }

    // A Direct2D window target presents ON THE VERTICAL BLANK by default, so
    // EndDraw below BLOCKS until the monitor is ready. That is what is wanted
    // for normal play - it is where the tear-free picture comes from - but it
    // also puts a hard ceiling at the refresh rate on the whole loop, and fast
    // forward works by shortening the frame budget. The budget can say 240Hz
    // all it likes; EndDraw still waits, and the emulator runs at 60.
    //
    // THIS IS WHY FAST FORWARD STOPPED WORKING when the scale moved from GDI to
    // Direct2D: the GDI blit never waited for anything.
    //
    // The present option is fixed when the target is made, so the target is
    // rebuilt when the mode changes. That is once per key press, not per frame.
    const bool present_immediately = state.fast_forward_held;
    if (state.d2d_target != nullptr && state.d2d_present_immediately != present_immediately) {
        release_d2d(state);
    }
    if (state.d2d_target != nullptr &&
        (state.d2d_target_width != width || state.d2d_target_height != height)) {
        // Resize in place where possible; the bitmaps belong to the target, so
        // a failure means starting the lot again.
        if (FAILED(state.d2d_target->Resize(D2D1::SizeU(width, height)))) {
            release_d2d(state);
        } else {
            state.d2d_target_width = width;
            state.d2d_target_height = height;
        }
    }
    if (state.d2d_target == nullptr) {
        auto properties = D2D1::RenderTargetProperties();
        auto hwnd_properties = D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(width, height));
        hwnd_properties.presentOptions = present_immediately
                                             ? D2D1_PRESENT_OPTIONS_IMMEDIATELY
                                             : D2D1_PRESENT_OPTIONS_NONE;
        if (FAILED(factory->CreateHwndRenderTarget(properties, hwnd_properties, &state.d2d_target)) ||
            state.d2d_target == nullptr) {
            state.d2d_target = nullptr;
            return false;
        }
        state.d2d_target_width = width;
        state.d2d_target_height = height;
        state.d2d_present_immediately = present_immediately;
    }

    if (state.d2d_picture != nullptr &&
        (state.d2d_picture_width != frame_width || state.d2d_picture_height != frame_height)) {
        state.d2d_picture->Release();
        state.d2d_picture = nullptr;
    }
    if (state.d2d_picture == nullptr) {
        const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
        if (FAILED(state.d2d_target->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(frame_width), static_cast<UINT32>(frame_height)),
                D2D1::BitmapProperties(format), &state.d2d_picture)) ||
            state.d2d_picture == nullptr) {
            state.d2d_picture = nullptr;
            return false;
        }
        state.d2d_picture_width = static_cast<unsigned>(frame_width);
        state.d2d_picture_height = static_cast<unsigned>(frame_height);
    }

    if (FAILED(state.d2d_picture->CopyFromMemory(
            nullptr, state.bgra_frame.data(),
            static_cast<UINT32>(frame_width * sizeof(std::uint32_t))))) {
        return false;
    }

    const auto to_f = [](LONG value) { return static_cast<float>(value); };
    const D2D1_RECT_F target = D2D1::RectF(to_f(picture.left - client.left),
                                           to_f(picture.top - client.top),
                                           to_f(picture.right - client.left),
                                           to_f(picture.bottom - client.top));

    state.d2d_target->BeginDraw();
    // The whole client area every time, which also paints the letterbox bars
    // the GDI path draws separately. A present replaces everything anyway, so
    // there is nothing to preserve and nothing to flash.
    state.d2d_target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    state.d2d_target->DrawBitmap(state.d2d_picture, target, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (state.crt_filter && ensure_d2d_scanlines(state)) {
        state.d2d_target->FillRectangle(target, state.d2d_scanlines);
    }
    const HRESULT ended = state.d2d_target->EndDraw();
    if (ended == D2DERR_RECREATE_TARGET) {
        // The device was lost - a driver update, a machine waking up. Throw
        // everything away and let the next frame build it again.
        release_d2d(state);
        return false;
    }
    return SUCCEEDED(ended);
}

void release_smooth_surface(RuntimeState& state) {
    if (state.smooth_dc != nullptr) {
        if (state.smooth_previous != nullptr) {
            SelectObject(state.smooth_dc, state.smooth_previous);
            state.smooth_previous = nullptr;
        }
        DeleteDC(state.smooth_dc);
        state.smooth_dc = nullptr;
    }
    if (state.smooth_bitmap != nullptr) {
        DeleteObject(state.smooth_bitmap);
        state.smooth_bitmap = nullptr;
    }
    state.smooth_width = 0;
    state.smooth_height = 0;
}

// A memory surface the size of the area being drawn into, reused until that
// size changes.
bool ensure_smooth_surface(RuntimeState& state, HDC reference, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (state.smooth_dc != nullptr && state.smooth_width == width &&
        state.smooth_height == height) {
        return true;
    }
    release_smooth_surface(state);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down, same as the frame buffer
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    state.smooth_bitmap =
        CreateDIBSection(reference, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (state.smooth_bitmap == nullptr) {
        return false;
    }
    state.smooth_dc = CreateCompatibleDC(reference);
    if (state.smooth_dc == nullptr) {
        DeleteObject(state.smooth_bitmap);
        state.smooth_bitmap = nullptr;
        return false;
    }
    state.smooth_previous =
        static_cast<HBITMAP>(SelectObject(state.smooth_dc, state.smooth_bitmap));
    state.smooth_width = width;
    state.smooth_height = height;
    return true;
}

void release_compose_surface(RuntimeState& state) {
    if (state.compose_dc != nullptr) {
        if (state.compose_previous != nullptr) {
            SelectObject(state.compose_dc, state.compose_previous);
            state.compose_previous = nullptr;
        }
        DeleteDC(state.compose_dc);
        state.compose_dc = nullptr;
    }
    if (state.compose_bitmap != nullptr) {
        DeleteObject(state.compose_bitmap);
        state.compose_bitmap = nullptr;
    }
    state.compose_width = 0;
    state.compose_height = 0;
}

// The client-sized surface the CRT path composes in, reused until the window
// changes size.
bool ensure_compose_surface(RuntimeState& state, HDC reference, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (state.compose_dc != nullptr && state.compose_width == width && state.compose_height == height) {
        return true;
    }
    release_compose_surface(state);
    state.compose_bitmap = CreateCompatibleBitmap(reference, width, height);
    if (state.compose_bitmap == nullptr) {
        return false;
    }
    state.compose_dc = CreateCompatibleDC(reference);
    if (state.compose_dc == nullptr) {
        DeleteObject(state.compose_bitmap);
        state.compose_bitmap = nullptr;
        return false;
    }
    state.compose_previous = static_cast<HBITMAP>(SelectObject(state.compose_dc, state.compose_bitmap));
    state.compose_width = width;
    state.compose_height = height;
    return true;
}

void load_logo(RuntimeState& state) {
    // GDI+ is only needed for the decode. The DIB it produces is an ordinary
    // GDI object and outlives the shutdown, so the library does not have to be
    // kept alive for the whole run.
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        return;
    }
    state.logo_bitmap =
        decode_png_resource(state.instance, IDR_LOGO_PNG, state.logo_width, state.logo_height);
    Gdiplus::GdiplusShutdown(token);
}

// The controller artwork, decoded on first use rather than at startup: someone
// who never opens the Input tab should not pay for four PNG decodes. GDI+ is
// started and stopped around the decode, the same way the logo does it, because
// the DIB that comes out is an ordinary GDI object that outlives the library.
struct PadArt {
    HBITMAP bitmap = nullptr;
    int width = 0;
    int height = 0;
};

const PadArt& pad_art(HINSTANCE instance, PadKind kind) {
    static PadArt cache[5];
    static bool loaded[5] = {};
    const int index = static_cast<int>(kind);
    if (index < 0 || index >= 5) {
        return cache[0];
    }
    if (!loaded[index]) {
        loaded[index] = true;
        static const int resources[5] = {IDR_PAD_3BUTTON, IDR_PAD_6BUTTON, IDR_PAD_JUSTIFIER,
                                         IDR_PAD_MENACER, IDR_PAD_PIANO};
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
            cache[index].bitmap = decode_png_resource(instance, resources[index],
                                                      cache[index].width, cache[index].height);
            Gdiplus::GdiplusShutdown(token);
        }
    }
    return cache[index];
}

// What the Input tab's dropdown calls each controller, and what the outline
// says when there is no picture of it.
const wchar_t* pad_kind_name(PadKind kind) {
    switch (kind) {
        case PadKind::three_button: return L"3 button";
        case PadKind::six_button: return L"6 button";
        case PadKind::justifier: return L"Justifier";
        case PadKind::menacer: return L"Menacer";
        case PadKind::miracle_piano: return L"Miracle Piano";
    }
    return L"";
}

// Title bar and menus follow the app's own theme rather than the system's.
// The background brush each menu is currently using.
//
// Kept per menu rather than as one process-wide handle, because a Gear Link
// puts two menus on screen at once and a menu holds its brush until something
// replaces it. Deleting another menu's brush leaves that menu drawing with a
// freed GDI object.
std::vector<std::pair<HMENU, HBRUSH>>& menu_brushes() {
    static std::vector<std::pair<HMENU, HBRUSH>> brushes;
    return brushes;
}

void set_menu_brush(HMENU menu, HBRUSH brush) {
    auto& brushes = menu_brushes();
    for (auto& entry : brushes) {
        if (entry.first == menu) {
            if (entry.second != nullptr) {
                DeleteObject(entry.second);
            }
            entry.second = brush;
            return;
        }
    }
    brushes.emplace_back(menu, brush);
}

// Called before a menu is destroyed, so its brush goes with it rather than
// leaking for the life of the program.
void release_menu_brush(HMENU menu) {
    auto& brushes = menu_brushes();
    for (auto entry = brushes.begin(); entry != brushes.end(); ++entry) {
        if (entry->first == menu) {
            if (entry->second != nullptr) {
                DeleteObject(entry->second);
            }
            brushes.erase(entry);
            return;
        }
    }
}

void apply_window_theme(HWND hwnd, bool dark) {
    const BOOL value = dark ? TRUE : FALSE;
    // 20 is the documented attribute; 19 is what the builds that shipped it
    // first used, so try the current one and fall back.
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value)))) {
        DwmSetWindowAttribute(hwnd, 19, &value, sizeof(value));
    }
    // Undocumented, and looked up by ordinal for that reason: this is what
    // makes the menu bar and its popups dark instead of leaving a bright strip
    // across the top of a dark window. If the export is not there - an older
    // Windows - the menu simply stays system-drawn and nothing breaks.
    if (HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using SetPreferredAppMode = int(WINAPI*)(int);
        using FlushMenuThemes = void(WINAPI*)();
        const auto set_mode =
            reinterpret_cast<SetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        const auto flush =
            reinterpret_cast<FlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
        if (set_mode != nullptr) {
            set_mode(dark ? 2 : 3);  // 2 = force dark, 3 = force light
        }
        if (flush != nullptr) {
            flush();
        }
        FreeLibrary(uxtheme);
    }
    // The owner-draw callbacks only cover the items themselves. The strip of
    // menu bar to the right of the last item, and the body of each popup, are
    // filled by the menu, so they need a brush of their own or they stay the
    // system colour and the dark bar has a light margin.
    if (HMENU bar = GetMenu(hwnd)) {
        HBRUSH replacement = CreateSolidBrush(dark ? kDarkTheme.background : kLightTheme.background);
        MENUINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
        info.hbrBack = replacement;
        SetMenuInfo(bar, &info);
        // One brush PER MENU. This used to be a single static shared by the
        // whole process, which worked for exactly as long as there was one
        // window. Opening a Gear Link gave the second console its own menu,
        // and theming it deleted the brush the FIRST window's menu was still
        // pointing at - so that menu drew itself with a freed object and its
        // drop-downs and the strip beside Tools went white, and stayed white
        // after the link was closed, because the dead handle was still in
        // place.
        set_menu_brush(bar, replacement);
    }
    DrawMenuBar(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// The folder genx.exe is running from.
std::filesystem::path executable_directory() {
    std::wstring module(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (length == 0) {
            return {};
        }
        if (length < module.size()) {
            module.resize(length);
            return std::filesystem::path(module).parent_path();
        }
        // Truncated: a path longer than MAX_PATH. Try again with more room.
        module.resize(module.size() * 2);
    }
}

// Where everything GenX writes is kept: settings, the recent list, saves,
// states, profiles, cheats and logs. %APPDATA% rather than beside the
// executable: the build directory is wiped and regenerated regularly, and a
// list that disappears with a rebuild is not a list.
//
// Portable mode is the exception, and it is switched on by the folder itself:
// a folder named User beside genx.exe is used instead, so a copy on a USB
// stick carries its own settings and saves with it. Decided once, at the first
// call, so one run never writes to both places.
std::filesystem::path data_directory() {
    static const std::filesystem::path directory = [] {
        std::error_code ignored;
        const auto beside = executable_directory();
        if (!beside.empty() && std::filesystem::is_directory(beside / L"User", ignored)) {
            return beside / L"User";
        }
        wchar_t* base = nullptr;
        std::filesystem::path appdata;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base)) &&
            base != nullptr) {
            appdata = std::filesystem::path(base) / L"GenX";
        } else {
            appdata = std::filesystem::temp_directory_path(ignored) / L"GenX";
        }
        if (base != nullptr) {
            CoTaskMemFree(base);
        }
        return appdata;
    }();
    return directory;
}

std::filesystem::path recent_file_path() {
    const auto directory = data_directory();
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / L"recent.txt";
}

// Preferences, next to the recent list. Plain key=value lines: the file is
// small enough that a person can read and fix it, which a binary blob is not.
//
// The name is per-window rather than fixed. The second console of a Gear Link
// pair is a whole separate machine with its own controller bound to it, and it
// has to be able to remember that without overwriting the first window's
// bindings every time it closes.
std::filesystem::path settings_file_path(const std::wstring& name) {
    return recent_file_path().parent_path() / (name.empty() ? L"settings.txt" : name);
}

// The stored window rectangle is width/height on disk but a right/bottom in a
// RECT, so it is converted after reading rather than in the middle of parsing.
void finish_loading_window_rect(RuntimeState& state) {
    auto& frame = state.windowed_placement.rcNormalPosition;
    if (frame.right <= 0 || frame.bottom <= 0) {
        frame = RECT{};
        return;
    }
    frame.right += frame.left;
    frame.bottom += frame.top;
    // Refuse a rectangle that is not on any monitor any more - a display that
    // has been unplugged since would otherwise open the window off-screen.
    const RECT probe = frame;
    if (MonitorFromRect(&probe, MONITOR_DEFAULTTONULL) == nullptr) {
        frame = RECT{};
    }
}

void load_settings(RuntimeState& state) {
    std::ifstream in(settings_file_path(state.settings_name));
    if (!in) {
        return;
    }
    // What shape the file is in. Absent means it predates versioning, which is
    // the case this exists to catch.
    int settings_version = 1;
    std::string line;
    while (std::getline(in, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (key == "settings_version") {
            settings_version = std::atoi(value.c_str());
        } else if (key == "dark_theme") {
            state.dark_theme = value != "0";
        } else if (key == "hide_cursor") {
            state.hide_cursor = value != "0";
        } else if (key == "check_for_updates") {
            state.check_for_updates = value != "0";
        } else if (key == "view_scale") {
            const int scale = std::atoi(value.c_str());
            if (scale >= 0 && scale <= 4) {
                state.view_scale = scale;
            }
        } else if (key == "aspect") {
            const int mode = std::atoi(value.c_str());
            if (mode >= 0 && mode <= static_cast<int>(Aspect::custom)) {
                state.aspect = static_cast<Aspect>(mode);
            }
        } else if (key == "custom_aspect_width") {
            state.custom_aspect_width = (std::max)(1, std::atoi(value.c_str()));
        } else if (key == "custom_aspect_height") {
            state.custom_aspect_height = (std::max)(1, std::atoi(value.c_str()));
        } else if (key == "fullscreen") {
            state.fullscreen = value != "0";
        } else if (key == "crt_filter") {
            state.crt_filter = value != "0";
        } else if (key == "smooth_scaling") {
            state.smooth_scaling = value != "0";
        } else if (key == "show_fps") {
            state.show_fps = value != "0";
        } else if (key == "window_x") {
            state.windowed_placement.rcNormalPosition.left = std::atol(value.c_str());
        } else if (key == "window_y") {
            state.windowed_placement.rcNormalPosition.top = std::atol(value.c_str());
        } else if (key == "window_w") {
            state.windowed_placement.rcNormalPosition.right = std::atol(value.c_str());
        } else if (key == "window_h") {
            state.windowed_placement.rcNormalPosition.bottom = std::atol(value.c_str());
        } else if (key == "window_maximized") {
            state.start_maximized = value != "0";
        } else if (key == "volume") {
            state.volume = std::clamp(std::atoi(value.c_str()), 0, 100);
        } else if (key == "muted") {
            state.muted = value != "0";
        } else if (key == "console_filter") {
            state.console_filter = value != "0";
        } else if (key == "profile_1") {
            state.profile_names[0] = widen(value);
        } else if (key == "profile_2") {
            state.profile_names[1] = widen(value);
        } else if (key == "netplay_nickname") {
            state.netplay_nickname = widen(value);
        } else if (key == "netplay_address") {
            state.netplay_address = widen(value);
        } else if (key == "netplay_port") {
            const int port = std::atoi(value.c_str());
            if (port > 0 && port < 65536) {
                state.netplay_port = port;
            }
        } else if (key == "netplay_delay") {
            const int delay = std::atoi(value.c_str());
            if (delay >= 1 && delay <= 20) {
                state.netplay_delay = delay;
            }
        } else if (key == "games_folder") {
            // The old single setting. Kept so an existing install does not lose
            // its library on upgrade: it seeds every system that has not been
            // given a folder of its own yet.
            for (auto& folder : state.games_folders) {
                if (folder.empty()) {
                    folder = std::filesystem::u8path(value);
                }
            }
        } else if (key.rfind("games_folder_", 0) == 0) {
            const int which = std::atoi(key.c_str() + 13);
            if (which >= 0 && which < kSaveFolderCount) {
                state.games_folders[which] = std::filesystem::u8path(value);
            }
        } else if (key.rfind("save_folder_", 0) == 0) {
            const int which = std::atoi(key.c_str() + 12);
            if (which >= 0 && which < kSaveFolderCount) {
                state.save_folders[which] = std::filesystem::u8path(value);
            }
        } else if (key == "bios_folder_32x") {
            state.bios_folder_32x = std::filesystem::u8path(value);
        } else if (key == "bios_folder_segacd") {
            state.bios_folder_segacd = std::filesystem::u8path(value);
        } else if (key == "cheats_on") {
            state.cheats_on = value != "0";
        } else if (key.rfind("pad", 0) == 0 && key.find("_dname") != std::string::npos) {
            // The DirectInput pad this player was last reading, by name, so it
            // can be found again if Windows lists it in a different slot.
            const int player = key[3] - '1';
            if (player >= 0 && player < 2) {
                state.dinput_pad_name[player] = widen(value);
            }
        } else if (key.rfind("pad", 0) == 0 && key.find("_kind") != std::string::npos) {
            const int player = key[3] - '1';
            const int kind = std::atoi(value.c_str());
            if (player >= 0 && player < 2 && kind >= 0 &&
                kind <= static_cast<int>(PadKind::miracle_piano)) {
                state.players[player].kind = static_cast<PadKind>(kind);
            }
        } else if (key.rfind("pad", 0) == 0 && key.find("_device") != std::string::npos) {
            const int player = key[3] - '1';
            const int device = std::atoi(value.c_str());
            if (player >= 0 && player < 2 && device >= 0 &&
                device <= static_cast<int>(InputDevice::mouse)) {
                state.players[player].device = static_cast<InputDevice>(device);
            }
        } else if (key.rfind("pad", 0) == 0 && key.find("_slot") != std::string::npos) {
            const int player = key[3] - '1';
            const int slot = std::atoi(value.c_str());
            if (player >= 0 && player < 2 && slot >= 0 && slot < 4) {
                state.players[player].pad_slot = slot;
            }
        } else if (key.rfind("pad", 0) == 0 && key.find("_btn") != std::string::npos) {
            // padN_btnM=mask
            const int player = key[3] - '1';
            const int button = std::atoi(key.c_str() + key.find("_btn") + 4);
            if (player >= 0 && player < 2 && button >= 0 && button < kPadButtonCount) {
                state.players[player].pad[button] =
                    static_cast<PadInput>(std::strtoul(value.c_str(), nullptr, 10));
            }
        } else if (key.rfind("pad", 0) == 0 && key.find("_dib") != std::string::npos) {
            const int player = key[3] - '1';
            const int button = std::atoi(key.c_str() + key.find("_dib") + 4);
            if (player >= 0 && player < 2 && button >= 0 && button < kPadButtonCount) {
                state.players[player].dpad[button] =
                    static_cast<PadInput>(std::strtoul(value.c_str(), nullptr, 10));
            }
        } else if (key.rfind("pad", 0) == 0 && key.find("_key") != std::string::npos) {
            // padN_keyM=vk
            const int player = key[3] - '1';
            const int button = std::atoi(key.c_str() + key.find("_key") + 4);
            if (player >= 0 && player < 2 && button >= 0 && button < kPadButtonCount) {
                state.players[player].keys[button] = static_cast<UINT>(std::atoi(value.c_str()));
            }
        } else if (key.rfind("hotkey", 0) == 0) {
            // hotkeyN=vk,ctrl,shift,alt - one line each so a hand edit of the
            // file cannot half-describe a chord.
            const int action = std::atoi(key.c_str() + 6);
            int vk = 0, ctrl = 0, shift = 0, alt = 0;
            if (action >= 0 && action < kHkCount &&
                std::sscanf(value.c_str(), "%d,%d,%d,%d", &vk, &ctrl, &shift, &alt) == 4) {
                state.hotkeys[action] =
                    Hotkey{static_cast<UINT>(vk), ctrl != 0, shift != 0, alt != 0};
            }
        }
    }
    // Open ROM used to be one action on Ctrl+O and is now one per system, so an
    // older file still holds Ctrl+O under what is now the Genesis action.
    //
    // Keyed off an explicit version rather than off whether the new actions
    // appear in the file. That was the first attempt and it is not reliable: a
    // build that had the new actions but not this migration writes them out,
    // and the next run then sees them and concludes there is nothing to do.
    if (settings_version < 2) {
        std::array<Hotkey, kHkCount> fresh{};
        set_default_hotkeys(fresh);
        for (const int action :
             {kHkOpenGenesis, kHkOpenGameGear, kHkOpen32X, kHkOpenCd, kHkDumpDiagnostics}) {
            state.hotkeys[action] = fresh[action];
        }
    }
    // A settings file written before the gun rows existed leaves them unbound,
    // which would be a gun with no trigger. Fill only what is still empty, so a
    // deliberate binding is never overwritten.
    for (auto& player : state.players) {
        set_default_gun_bindings(player);
    }
    finish_loading_window_rect(state);
}

void save_settings(const RuntimeState& state) {
    std::ofstream out(settings_file_path(state.settings_name), std::ios::trunc);
    if (!out) {
        return;
    }
    // 2: Open ROM became one action per system, so the hotkey at index 0 means
    // "open a Genesis ROM" rather than "open a ROM".
    out << "settings_version=2\n";
    out << "dark_theme=" << (state.dark_theme ? 1 : 0) << "\n";
    out << "hide_cursor=" << (state.hide_cursor ? 1 : 0) << "\n";
    out << "check_for_updates=" << (state.check_for_updates ? 1 : 0) << "\n";
    out << "view_scale=" << state.view_scale << "\n";
    out << "aspect=" << static_cast<int>(state.aspect) << "\n";
    out << "custom_aspect_width=" << state.custom_aspect_width << "\n";
    out << "custom_aspect_height=" << state.custom_aspect_height << "\n";
    out << "fullscreen=" << (state.fullscreen ? 1 : 0) << "\n";
    out << "crt_filter=" << (state.crt_filter ? 1 : 0) << "\n";
    out << "smooth_scaling=" << (state.smooth_scaling ? 1 : 0) << "\n";
    out << "show_fps=" << (state.show_fps ? 1 : 0) << "\n";
    // The WINDOWED placement, even while fullscreen: it is what the window goes
    // back to, and what the next run should open at.
    RECT frame = state.windowed_placement.rcNormalPosition;
    if (!state.fullscreen && state.window != nullptr) {
        WINDOWPLACEMENT now{sizeof(WINDOWPLACEMENT)};
        if (GetWindowPlacement(state.window, &now)) {
            frame = now.rcNormalPosition;
        }
    }
    if (frame.right > frame.left && frame.bottom > frame.top) {
        out << "window_x=" << frame.left << "\n";
        out << "window_y=" << frame.top << "\n";
        out << "window_w=" << (frame.right - frame.left) << "\n";
        out << "window_h=" << (frame.bottom - frame.top) << "\n";
    }
    // Maximized is not a position - rcNormalPosition holds where the window
    // would go if restored, so saving that alone reopens a maximized window at
    // its small size. The show state has to be carried separately.
    bool maximized = state.windowed_placement.showCmd == SW_SHOWMAXIMIZED;
    if (!state.fullscreen && state.window != nullptr) {
        maximized = IsZoomed(state.window) != 0;
    }
    out << "window_maximized=" << (maximized ? 1 : 0) << "\n";

    out << "volume=" << state.volume << "\n";
    out << "muted=" << (state.muted ? 1 : 0) << "\n";
    out << "console_filter=" << (state.console_filter ? 1 : 0) << "\n";
    const auto utf8_of = [](const std::filesystem::path& path) {
        const auto bytes = path.u8string();
        return std::string(bytes.begin(), bytes.end());
    };
    // Everything but the passphrase, which is a session secret and is generated
    // fresh each time hosting is offered.
    out << "profile_1=" << narrow(state.profile_names[0]) << "\n";
    out << "profile_2=" << narrow(state.profile_names[1]) << "\n";
    out << "netplay_nickname=" << narrow(state.netplay_nickname) << "\n";
    out << "netplay_address=" << narrow(state.netplay_address) << "\n";
    out << "netplay_port=" << state.netplay_port << "\n";
    out << "netplay_delay=" << state.netplay_delay << "\n";
    for (int which = 0; which < kSaveFolderCount; ++which) {
        out << "games_folder_" << which << "=" << utf8_of(state.games_folders[which]) << "\n";
    }
    for (int which = 0; which < kSaveFolderCount; ++which) {
        out << "save_folder_" << which << "=" << utf8_of(state.save_folders[which]) << "\n";
    }
    out << "bios_folder_32x=" << utf8_of(state.bios_folder_32x) << "\n";
    out << "bios_folder_segacd=" << utf8_of(state.bios_folder_segacd) << "\n";
    // The Game Genie master switch. The codes themselves are per game and
    // live in their own files; this is the one "are cheats on at all" bit.
    out << "cheats_on=" << (state.cheats_on ? 1 : 0) << "\n";
    for (int player = 0; player < 2; ++player) {
        const auto prefix = "pad" + std::to_string(player + 1);
        // What the player CHOSE, not what a window has temporarily plugged in.
        // Opening the Miracle Piano makes player two a piano on a mouse for as
        // long as its window is up; any unrelated save while it is open - a
        // theme change, going fullscreen - would write that into the settings
        // file, and it would still be there on the next launch, with every one
        // of that player's buttons silently discarded.
        const auto saved = piano_saved_input(state, player);
        out << prefix << "_kind=" << static_cast<int>(saved.kind) << "\n";
        out << prefix << "_device=" << static_cast<int>(saved.device) << "\n";
        out << prefix << "_slot=" << state.players[player].pad_slot << "\n";
        out << prefix << "_dname=" << narrow(state.dinput_pad_name[player]) << "\n";
        for (int button = 0; button < kPadButtonCount; ++button) {
            out << prefix << "_key" << button << "=" << state.players[player].keys[button] << "\n";
            out << prefix << "_btn" << button << "=" << state.players[player].pad[button] << "\n";
            out << prefix << "_dib" << button << "=" << state.players[player].dpad[button] << "\n";
        }
    }
    for (int action = 0; action < kHkCount; ++action) {
        const auto& binding = state.hotkeys[action];
        out << "hotkey" << action << "=" << binding.key << "," << (binding.ctrl ? 1 : 0) << ","
            << (binding.shift ? 1 : 0) << "," << (binding.alt ? 1 : 0) << "\n";
    }
}

// The cursor is hidden only while a game is actually running - not on the logo
// screen, not while paused, and never when a light gun is plugged in, because a
// light gun is aimed with the pointer.
bool crosshair_wanted(const RuntimeState& state);
bool pointer_hidden_for_gun(const RuntimeState& state);

bool should_hide_cursor(const RuntimeState& state) {
    if (!state.hide_cursor || !state.rom_loaded || state.paused || state.in_modal_loop) {
        return false;
    }
    // Still visible for a moment after the last movement.
    if (state.cursor_idle_frames > 0) {
        return false;
    }
    // A gun that needs OUR crosshair keeps the pointer whatever this setting
    // says - hiding it would leave the player with no sight at all. A gun whose
    // game draws its own is handled the other way round, in WM_SETCURSOR.
    if (crosshair_wanted(state)) {
        return false;
    }
    return true;
}

void load_recent(RuntimeState& state) {
    state.recent.clear();
    std::ifstream in(recent_file_path());
    if (!in) {
        return;
    }
    std::string line;
    while (state.recent.size() < kRecentSlots && std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        // Stored as UTF-8 so a path with non-ASCII characters survives the round
        // trip; std::filesystem::u8path is what reads it back as one.
        state.recent.emplace_back(std::filesystem::u8path(line));
    }
}

void save_recent(const RuntimeState& state) {
    std::ofstream out(recent_file_path(), std::ios::trunc);
    if (!out) {
        return;
    }
    for (const auto& path : state.recent) {
        const auto utf8 = path.u8string();
        out << std::string(utf8.begin(), utf8.end()) << "\n";
    }
}

// Defined here rather than forward-declared: an opaque enum has no size, so a
// declaration alone is not enough to pass one by value.
enum class LabelBucket { fixed, recent, states };
// Labels are keyed by WINDOW as well as by bucket. The three buckets keep one
// window's menus from clearing each other's labels; the owner keeps one
// window from clearing ANOTHER window's, which is what a Gear Link made
// possible. The second console rebuilding its recent list was freeing the
// strings the first window's live menu items still pointed at, and those
// items then had nothing to draw - which is why the recent files vanished
// after a link and did not come back.
void make_menu_owner_drawn(HMENU menu, LabelBucket bucket, const void* owner);
std::vector<std::unique_ptr<std::wstring>>& menu_labels(LabelBucket bucket, const void* owner);
void release_menu_labels(const void* owner);
void update_title(const RuntimeState& state);

void rebuild_recent_menu(RuntimeState& state) {
    if (state.recent_menu == nullptr) {
        return;
    }
    while (GetMenuItemCount(state.recent_menu) > 0) {
        DeleteMenu(state.recent_menu, 0, MF_BYPOSITION);
    }
    if (state.recent.empty()) {
        AppendMenuW(state.recent_menu, MF_STRING | MF_GRAYED, IDM_FILE_RECENT_EMPTY,
                    L"(no recent files)");
    } else {
        for (std::size_t i = 0; i < state.recent.size(); ++i) {
            // "&1" ... "&9" then "1&0": the mnemonic is the digit, and the tenth
            // slot uses 0 so all ten are reachable from the keyboard.
            std::wstring label = i < 9 ? (L"&" + std::to_wstring(i + 1)) : L"1&0";
            label += L"  " + state.recent[i].filename().wstring();
            AppendMenuW(state.recent_menu, MF_STRING,
                        static_cast<UINT_PTR>(IDM_FILE_RECENT_FIRST + i), label.c_str());
        }
    }
    AppendMenuW(state.recent_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(state.recent_menu, MF_STRING | (state.recent.empty() ? MF_GRAYED : MF_ENABLED),
                IDM_FILE_RECENT_CLEAR, L"&Clear recent files");
    // The items are new, so they need switching to owner-draw again or they
    // draw in the system colours - black on a dark popup. The labels from the
    // previous rebuild are unreferenced now that the items are gone.
    menu_labels(LabelBucket::recent, &state).clear();
    make_menu_owner_drawn(state.recent_menu, LabelBucket::recent, &state);
    if (state.window != nullptr) {
        DrawMenuBar(state.window);
    }
}

// %APPDATA%\GenX\states\<rom name>.st<slot>. Named after the cartridge so
// states for different games never collide, and kept out of the build tree for
// the same reason as the recent list.
std::filesystem::path states_directory() {
    auto directory = recent_file_path().parent_path() / L"states";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

// Every state file, across every game. ".st" followed by digits is the naming
// this writes, and matching on it means a stray file that happens to be sitting
// in the folder is left alone rather than deleted with the rest.
std::vector<std::filesystem::path> all_state_files() {
    std::vector<std::filesystem::path> found;
    std::error_code failed;
    for (const auto& entry : std::filesystem::directory_iterator(states_directory(), failed)) {
        if (failed || !entry.is_regular_file()) {
            continue;
        }
        const auto extension = entry.path().extension().wstring();
        if (extension.size() < 4 || extension.compare(0, 3, L".st") != 0) {
            continue;
        }
        if (std::all_of(extension.begin() + 3, extension.end(),
                        [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
            found.push_back(entry.path());
        }
    }
    return found;
}

std::filesystem::path state_slot_path(const RuntimeState& state, int slot) {
    auto directory = states_directory();
    auto name = state.rom_path.stem().wstring();
    if (name.empty()) {
        name = L"cartridge";
    }
    return directory / (name + L".st" + std::to_wstring(slot));
}

// --- battery-backed cartridge saves ----------------------------------------
// Kept apart from save states: a state is the whole machine at an instant, this
// is the handful of bytes the cartridge itself keeps across a power cycle. The
// file is raw, with no header, which is the shape every other emulator's .srm
// has.
std::filesystem::path battery_path(const RuntimeState& state) {
    // The Sega CD's backup RAM is inside the console, not on the disc: every
    // game saves into the same 8KB, and the BIOS's memory manager lists them
    // all together. So it is one file per console region, whichever disc is
    // in - keeping it per disc would hide each game's saves from the others.
    if (state.machine == Machine::genesis && state.genesis.cd_mode()) {
        auto directory = state.save_folders[kSaveCd].empty()
                             ? recent_file_path().parent_path() / L"saves"
                             : state.save_folders[kSaveCd];
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        const char region = state.genesis.segacd().bios_region();
        std::wstring name = L"Sega CD backup RAM (";
        name += region == 'E' ? L"Europe" : region == 'J' ? L"Japan" : L"USA";
        name += L").brm";
        return directory / name;
    }
    // The General tab can point each machine's saves somewhere else - a shared
    // folder with another emulator, most usefully. Empty means "keep them with
    // the rest of GenX's data", which is where they have always gone.
    const int which = state.machine == Machine::genesis ? kSaveGenesis : kSaveGameGear;
    auto directory = state.save_folders[which].empty()
                         ? recent_file_path().parent_path() / L"saves"
                         : state.save_folders[which];
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    auto name = state.rom_path.stem().wstring();
    if (name.empty()) {
        name = L"cartridge";
    }
    const bool eeprom =
        state.machine == Machine::genesis && state.genesis.battery_is_eeprom();
    return directory / (name + (eeprom ? L".eep" : L".srm"));
}

bool cartridge_has_battery(const RuntimeState& state) {
    if (!state.rom_loaded) {
        return false;
    }
    if (state.machine == Machine::genesis) {
        return state.genesis.battery_backed();
    }
    // The Game Gear side has no header flag for it, so the tell is whether the
    // game has actually written to cartridge RAM.
    return state.game_gear.cartridge().battery_dirty() ||
           std::filesystem::exists(battery_path(state));
}

void flush_battery(RuntimeState& state, bool force) {
    if (!state.rom_loaded || g_netplay_joined) {
        return;
    }
    std::vector<std::uint8_t> bytes;
    if (state.machine == Machine::genesis) {
        if (!state.genesis.battery_backed() || (!force && !state.genesis.battery_dirty())) {
            return;
        }
        bytes = state.genesis.battery_snapshot();
        state.genesis.clear_battery_dirty();
    } else {
        const auto& cart = state.game_gear.cartridge();
        if (!force && !cart.battery_dirty()) {
            return;
        }
        if (!cart.battery_dirty() && !std::filesystem::exists(battery_path(state))) {
            return;
        }
        const auto& ram = cart.battery_bytes();
        bytes.assign(ram.begin(), ram.end());
        state.game_gear.mutable_cartridge().clear_battery_dirty();
    }
    if (bytes.empty()) {
        return;
    }
    std::ofstream out(battery_path(state), std::ios::binary | std::ios::trunc);
    if (out) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
}

void restore_battery(RuntimeState& state) {
    const auto path = battery_path(state);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return;
    }
    if (state.machine == Machine::genesis) {
        state.genesis.restore_battery(bytes);
    } else {
        state.game_gear.mutable_cartridge().restore_battery(bytes.data(), bytes.size());
    }
}

void set_status(RuntimeState& state, std::wstring message) {
    state.status_message = std::move(message);
    // Roughly two seconds at 60Hz.
    state.status_frames_remaining = 120;
    update_title(state);
}

// Put a saved state back into THIS player's machine - a state slot or a rewind
// step, never netplay, which needs the state exact.
//
// A SEGA CD KEEPS ITS MEMORY CARD. The backup RAM is not one game's save, it
// is the console's, shared by every disc. A state carries it (a game reads
// its saves while it runs, and netplay needs both ends identical), but putting
// that copy back here would roll the card back to the moment the state was
// taken - and the next save would write that old card over the file, losing
// whatever was saved in any CD game since. So the card as it stands now is
// put back over the state's copy, the way a real memory card stays in the
// slot while the console is reset.
bool load_machine_state(RuntimeState& state, const std::vector<std::uint8_t>& blob,
                        std::string& error) {
    if (state.machine != Machine::genesis) {
        return state.game_gear.load_state_blob(blob, error);
    }
    if (!state.genesis.cd_mode()) {
        return state.genesis.load_state_blob(blob, error);
    }
    // Anything saved and not yet written goes out first: putting the card
    // back below also clears the "needs writing" flag.
    flush_battery(state, false);
    const auto card = state.genesis.battery_snapshot();
    if (!state.genesis.load_state_blob(blob, error)) {
        return false;
    }
    state.genesis.restore_battery(card);
    return true;
}

void save_state_slot(RuntimeState& state, int slot) {
    if (!state.rom_loaded) {
        return;
    }
    const auto blob = state.machine == Machine::genesis ? state.genesis.save_state_blob()
                                                        : state.game_gear.save_state_blob();
    const auto path = state_slot_path(state, slot);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out ||
        !out.write(reinterpret_cast<const char*>(blob.data()),
                   static_cast<std::streamsize>(blob.size()))) {
        MessageBoxW(state.window, L"Could not write the save state.", L"GenX",
                    MB_OK | MB_ICONERROR);
        return;
    }
    set_status(state, L"state " + std::to_wstring(slot) + L" saved");
}

void load_state_slot(RuntimeState& state, int slot) {
    if (!state.rom_loaded) {
        return;
    }
    const auto path = state_slot_path(state, slot);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_status(state, L"state " + std::to_wstring(slot) + L" is empty");
        return;
    }
    std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    std::string error;
    if (!load_machine_state(state, blob, error)) {
        MessageBoxW(state.window, widen(error).c_str(), L"GenX", MB_OK | MB_ICONERROR);
        return;
    }
    // The restored machine may be in a different video mode than the one that
    // was running, so the picture has to be re-read rather than assumed.
    state.pending_audio.clear();
    update_framebuffer(state);
    set_status(state, L"state " + std::to_wstring(slot) + L" loaded");
    if (state.window != nullptr) {
        InvalidateRect(state.window, nullptr, FALSE);
    }
}

// Deleting a state cannot be undone, so it asks first. Save and load do not,
// because neither destroys anything the user cannot get back by playing on.
void delete_state_slot(RuntimeState& state, int slot) {
    if (!state.rom_loaded) {
        return;
    }
    const auto path = state_slot_path(state, slot);
    if (!std::filesystem::exists(path)) {
        return;
    }
    std::wstring prompt = L"Delete save state " + std::to_wstring(slot) + L" for " +
                          state.rom_path.filename().wstring() + L"?\n\nThis cannot be undone.";
    if (MessageBoxW(state.window, prompt.c_str(), L"GenX - Delete State",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    std::error_code failed;
    std::filesystem::remove(path, failed);
    if (failed) {
        MessageBoxW(state.window, L"Could not delete the save state.", L"GenX",
                    MB_OK | MB_ICONERROR);
        return;
    }
    set_status(state, L"state " + std::to_wstring(slot) + L" deleted");
}

// Every state for every game, not just the cartridge in the machine. It needs no
// ROM loaded, and it says how many games are affected before doing anything -
// this is the one action here that can throw away work the user cannot see from
// the menu they are standing in.
void delete_all_states(RuntimeState& state) {
    const auto files = all_state_files();
    if (files.empty()) {
        set_status(state, L"no save states to delete");
        return;
    }
    std::vector<std::wstring> games;
    for (const auto& file : files) {
        auto game = file.stem().wstring();
        if (std::find(games.begin(), games.end(), game) == games.end()) {
            games.push_back(game);
        }
    }
    std::wstring prompt = L"Delete ALL " + std::to_wstring(files.size()) + L" save state" +
                          (files.size() == 1 ? L"" : L"s") + L", across " +
                          std::to_wstring(games.size()) + L" game" +
                          (games.size() == 1 ? L"" : L"s") + L"?\n\n";
    // Name them, up to a point: agreeing to "12 states" without knowing which
    // games they belong to is not really agreeing to anything.
    for (std::size_t i = 0; i < games.size() && i < 12; ++i) {
        prompt += L"    " + games[i] + L"\n";
    }
    if (games.size() > 12) {
        prompt += L"    ... and " + std::to_wstring(games.size() - 12) + L" more\n";
    }
    prompt += L"\nThis cannot be undone.";
    if (MessageBoxW(state.window, prompt.c_str(), L"GenX - Delete All Save States",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    std::size_t removed = 0;
    for (const auto& file : files) {
        std::error_code failed;
        if (std::filesystem::remove(file, failed)) {
            ++removed;
        }
    }
    set_status(state, std::to_wstring(removed) + L" states deleted");
}

// Rebuilt whenever the Emulation menu opens, so a slot that holds a state says
// so instead of every slot looking alike.
void rebuild_state_menus(RuntimeState& state) {
    struct Column {
        HMENU menu;
        UINT first;
        // Whether an EMPTY slot should be selectable. Only saving is useful on
        // an empty slot; loading or deleting one is not.
        bool empty_usable;
        const wchar_t* accelerator;
    };
    const Column columns[3] = {
        {state.load_state_menu, IDM_EMU_LOAD_STATE_FIRST, false, L"\t"},
        {state.save_state_menu, IDM_EMU_SAVE_STATE_FIRST, true, L"\tShift+"},
        {state.delete_state_menu, IDM_EMU_DELETE_STATE_FIRST, false, nullptr},
    };

    for (const auto& column : columns) {
        if (column.menu == nullptr) {
            continue;
        }
        while (GetMenuItemCount(column.menu) > 0) {
            DeleteMenu(column.menu, 0, MF_BYPOSITION);
        }
        bool any = false;
        for (int slot = 1; slot <= 10; ++slot) {
            // Digit 1-9 then 0 for the tenth, matching the hotkeys.
            const wchar_t digit = slot < 10 ? static_cast<wchar_t>(L'0' + slot) : L'0';
            std::wstring label = L"Slot &";
            label += digit;
            const bool occupied =
                state.rom_loaded && std::filesystem::exists(state_slot_path(state, slot));
            any = any || occupied;
            label += occupied ? L"  (in use)" : L"  (empty)";
            if (column.accelerator != nullptr) {
                label += column.accelerator;
                label += digit;
            }
            UINT flags = MF_STRING;
            if (!state.rom_loaded || (!occupied && !column.empty_usable)) {
                flags |= MF_GRAYED;
            }
            AppendMenuW(column.menu, flags, column.first + static_cast<UINT>(slot - 1),
                        label.c_str());
        }
        if (column.first == IDM_EMU_DELETE_STATE_FIRST) {
            // Not gated on `any`, and not on a cartridge being loaded: this one
            // covers every game, so it is available whenever anything is there
            // to delete.
            AppendMenuW(column.menu, MF_SEPARATOR, 0, nullptr);
            const bool anywhere = !all_state_files().empty();
            AppendMenuW(column.menu, MF_STRING | (anywhere ? MF_ENABLED : MF_GRAYED),
                        IDM_EMU_DELETE_ALL_STATES, L"Delete &all save states (every game)");
        }
    }
    // Every slot menu was emptied above, so their old labels are unreferenced.
    menu_labels(LabelBucket::states, &state).clear();
    for (const auto& column : columns) {
        make_menu_owner_drawn(column.menu, LabelBucket::states, &state);
    }
}

void remember_recent(RuntimeState& state, const std::filesystem::path& path) {
    std::error_code ignored;
    auto full = std::filesystem::absolute(path, ignored);
    if (ignored) {
        full = path;
    }
    // Windows paths are case-insensitive, so the same ROM opened with different
    // casing must not take two slots.
    const auto same = [&](const std::filesystem::path& other) {
        return _wcsicmp(other.native().c_str(), full.native().c_str()) == 0;
    };
    state.recent.erase(std::remove_if(state.recent.begin(), state.recent.end(), same),
                       state.recent.end());
    state.recent.insert(state.recent.begin(), full);
    if (state.recent.size() > kRecentSlots) {
        state.recent.resize(kRecentSlots);
    }
    save_recent(state);
    rebuild_recent_menu(state);
}

HMENU build_menu(RuntimeState& state) {
    HMENU bar = CreateMenu();

    state.recent_menu = CreatePopupMenu();
    HMENU file = CreatePopupMenu();
    // Open ROM is a list of systems rather than one dialog, because each system
    // keeps its own library folder and its own file types.
    HMENU open = CreatePopupMenu();
    AppendMenuW(open, MF_STRING, IDM_FILE_OPEN, L"&Genesis / Mega Drive...");
    AppendMenuW(open, MF_STRING, IDM_FILE_OPEN_GG, L"Game Gear / &Master System...");
    AppendMenuW(open, MF_STRING, IDM_FILE_OPEN_32X, L"&32X...");
    AppendMenuW(open, MF_STRING, IDM_FILE_OPEN_CD, L"Sega &CD...");
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(open), L"&Open ROM");
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(state.recent_menu), L"&Recent Files");
    AppendMenuW(file, MF_STRING, IDM_FILE_CLOSE, L"&Close ROM");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_FILE_OPEN_GENX_FOLDER, L"Open Gen&X folder");
    AppendMenuW(file, MF_STRING, IDM_FILE_OPEN_LOG_FOLDER, L"Open &log folder");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    HMENU emulation = CreatePopupMenu();
    AppendMenuW(emulation, MF_STRING, IDM_EMU_PAUSE, L"&Pause\tCtrl+P");
    AppendMenuW(emulation, MF_STRING, IDM_EMU_RESET_SOFT, L"&Soft Reset\tCtrl+R");
    AppendMenuW(emulation, MF_STRING, IDM_EMU_RESET_HARD, L"&Hard Reset\tCtrl+Shift+R");
    AppendMenuW(emulation, MF_STRING, IDM_EMU_POWER_OFF, L"Power &Off\tCtrl+W");
    AppendMenuW(emulation, MF_SEPARATOR, 0, nullptr);
    state.load_state_menu = CreatePopupMenu();
    state.save_state_menu = CreatePopupMenu();
    state.delete_state_menu = CreatePopupMenu();
    AppendMenuW(emulation, MF_POPUP, reinterpret_cast<UINT_PTR>(state.load_state_menu),
                L"&Load State");
    AppendMenuW(emulation, MF_POPUP, reinterpret_cast<UINT_PTR>(state.save_state_menu),
                L"Sa&ve State");
    AppendMenuW(emulation, MF_POPUP, reinterpret_cast<UINT_PTR>(state.delete_state_menu),
                L"&Delete State");
    AppendMenuW(emulation, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(emulation, MF_STRING, IDM_EMU_CONFIGURE, L"&Configure...");
    AppendMenuW(emulation, MF_STRING, IDM_EMU_REPAIR_GUNS, L"Re-pair &light guns");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(emulation), L"&Emulation");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_VIEW_SCALE_1X, L"&1x");
    AppendMenuW(view, MF_STRING, IDM_VIEW_SCALE_2X, L"&2x");
    AppendMenuW(view, MF_STRING, IDM_VIEW_SCALE_3X, L"&3x");
    AppendMenuW(view, MF_STRING, IDM_VIEW_SCALE_4X, L"&4x");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_VIEW_FULLSCREEN, L"&Fullscreen\tAlt+Enter");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    HMENU aspect = CreatePopupMenu();
    AppendMenuW(aspect, MF_STRING, IDM_VIEW_ASPECT_STRETCH, L"&Stretch to fit");
    AppendMenuW(aspect, MF_STRING, IDM_VIEW_ASPECT_4_3, L"&4:3");
    AppendMenuW(aspect, MF_STRING, IDM_VIEW_ASPECT_16_9, L"1&6:9");
    AppendMenuW(aspect, MF_STRING, IDM_VIEW_ASPECT_CUSTOM, L"&Custom...");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(aspect), L"&Aspect Ratio");
    AppendMenuW(view, MF_STRING, IDM_VIEW_CRT, L"CR&T filter");
    AppendMenuW(view, MF_STRING, IDM_VIEW_SMOOTH, L"&Pixel smoothing");
    AppendMenuW(view, MF_STRING, IDM_VIEW_FPS, L"FP&S counter");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_VIEW_DARK, L"&Dark theme");
    AppendMenuW(view, MF_STRING, IDM_VIEW_LIGHT, L"&Light theme");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

    // The disc drive. On the bar rather than inside Tools because swapping
    // discs happens DURING a game and more than once - Night Trap, Fahrenheit
    // and Supreme Warrior all ask for the other disc partway through - and
    // because booting the unit with no disc is how the BIOS's own front end,
    // CD player and backup RAM manager are reached.
    HMENU disc = CreatePopupMenu();
    AppendMenuW(disc, MF_STRING, IDM_CD_SWAP, L"&Swap disc...");
    AppendMenuW(disc, MF_STRING, IDM_CD_BOOT, L"&Boot Sega CD");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(disc), L"Sega C&D");

    HMENU cheats = CreatePopupMenu();
    AppendMenuW(cheats, MF_STRING, IDM_CHEAT_GAME_GENIE, L"&Game Genie codes...");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(cheats), L"C&heats");

    HMENU multiplayer = CreatePopupMenu();
    AppendMenuW(multiplayer, MF_STRING, IDM_MP_NETPLAY_START, L"&Start netplay...");
    AppendMenuW(multiplayer, MF_STRING, IDM_MP_NETPLAY_JOIN, L"&Join netplay...");
    AppendMenuW(multiplayer, MF_STRING, IDM_MP_NETPLAY_EXIT, L"&Exit netplay");
    AppendMenuW(multiplayer, MF_SEPARATOR, 0, nullptr);
    // Two Game Gears on one screen, joined by a cable. Nothing to do with
    // netplay: both consoles are here, on this machine.
    AppendMenuW(multiplayer, MF_STRING, IDM_MP_GEAR_LINK, L"&Gear Link");
    AppendMenuW(multiplayer, MF_STRING, IDM_MP_GEAR_LINK_CLOSE, L"&Close Gear Link");
    AppendMenuW(multiplayer, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(multiplayer, MF_STRING, IDM_MP_LINK_VIEW_BOTH, L"Show &both consoles");
    AppendMenuW(multiplayer, MF_STRING, IDM_MP_LINK_VIEW_ONE, L"Show console &1 only");
    AppendMenuW(multiplayer, MF_STRING, IDM_MP_LINK_VIEW_TWO, L"Show console &2 only");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(multiplayer), L"&Multiplayer");

    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_DUMP_VIDEO, L"Dump &Video State\tF12");
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    // Silence one source at a time to find out which one a sound belongs to.
    // Faster and more certain than reasoning about it from the outside.
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_MUTE_FM, L"Mute &FM channels");
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_MUTE_PSG, L"Mute &PSG");
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_MUTE_DAC, L"Mute &DAC samples");
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_MUTE_PWM, L"Mute 32X P&WM");
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_MUTE_CD, L"Mute Sega &CD audio");
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_RECORD_WAV, L"&Record audio to WAV");
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_LOG_FM, L"&Log FM register writes");
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_LOG_PSG, L"Log PS&G register writes");
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_LOG_DAC, L"Log D&AC register writes");
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_DUMP_LOG, L"Write &diagnostic log");
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_DUMP_MEMORY, L"Dump &memory to hex");
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    // The Miracle Piano game needs its keyboard, and nobody has one.
    AppendMenuW(tools, MF_STRING, IDM_TOOLS_MIRACLE_PIANO, L"&Miracle Piano keyboard\tM");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"&Tools");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_HELP_CHECK_UPDATES, L"Check for &updates...");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");

    return bar;
}

// Windows draws the classic menu bar itself, in the system colours, and none of
// the dark-mode APIs reach it - the bar stays a bright strip across the top of a
// dark window. The only way to colour it is to own the drawing, so every item is
// switched to owner-draw here. The item's STRING is deliberately left in place
// as well as the label being copied into dwItemData: the string is what Windows
// matches Alt-key mnemonics against, and dropping it would cost Alt+F.
// Owner-drawn items point at their label through dwItemData, so a label has to
// outlive its menu item. The menus that get rebuilt - the recent list, the state
// slots - each own a separate bucket, cleared only by their own rebuild. One
// shared store would mean clearing the state slots' labels out from under the
// recent list's live menu items.
struct MenuLabelStore {
    const void* owner = nullptr;
    LabelBucket bucket = LabelBucket::fixed;
    std::vector<std::unique_ptr<std::wstring>> labels;
};

std::vector<MenuLabelStore>& menu_label_stores() {
    static std::vector<MenuLabelStore> stores;
    return stores;
}

std::vector<std::unique_ptr<std::wstring>>& menu_labels(LabelBucket bucket, const void* owner) {
    auto& stores = menu_label_stores();
    for (auto& store : stores) {
        if (store.owner == owner && store.bucket == bucket) {
            return store.labels;
        }
    }
    stores.push_back(MenuLabelStore{owner, bucket, {}});
    return stores.back().labels;
}

void release_menu_labels(const void* owner) {
    auto& stores = menu_label_stores();
    for (auto store = stores.begin(); store != stores.end();) {
        store = store->owner == owner ? stores.erase(store) : store + 1;
    }
}

void make_menu_owner_drawn(HMENU menu, LabelBucket bucket, const void* owner) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_SUBMENU | MIIM_STRING;
        info.dwTypeData = nullptr;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) {
            continue;
        }
        if ((info.fType & MFT_SEPARATOR) != 0) {
            continue;
        }
        std::wstring text(info.cch + 1, L'\0');
        info.dwTypeData = text.data();
        info.cch += 1;
        GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info);
        text.resize(std::wcslen(text.c_str()));

        HMENU submenu = info.hSubMenu;
        menu_labels(bucket, owner).push_back(std::make_unique<std::wstring>(std::move(text)));

        MENUITEMINFOW set{};
        set.cbSize = sizeof(set);
        set.fMask = MIIM_FTYPE | MIIM_DATA;
        set.fType = info.fType | MFT_OWNERDRAW;
        set.dwItemData = reinterpret_cast<ULONG_PTR>(menu_labels(bucket, owner).back().get());
        SetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &set);

        // The dynamic submenus own their own labels and are rebuilt separately,
        // so the recursive walk must not claim them for this bucket.
        if (submenu != nullptr && bucket == LabelBucket::fixed) {
            make_menu_owner_drawn(submenu, bucket, owner);
        }
    }
}

HFONT menu_font() {
    static HFONT font = [] {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
            return CreateFontIndirectW(&metrics.lfMenuFont);
        }
        return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }();
    return font;
}

// Split "Open ROM...\tCtrl+O" into the label and the shortcut, which are drawn
// in two columns the way a system menu does it.
void split_menu_label(const std::wstring& text, std::wstring& label, std::wstring& accel) {
    const auto tab = text.find(L'\t');
    if (tab == std::wstring::npos) {
        label = text;
        accel.clear();
        return;
    }
    label = text.substr(0, tab);
    accel = text.substr(tab + 1);
}

void measure_menu_item(const RuntimeState& state, MEASUREITEMSTRUCT* item) {
    const auto* text = reinterpret_cast<const std::wstring*>(item->itemData);
    if (text == nullptr) {
        return;
    }
    std::wstring label;
    std::wstring accel;
    split_menu_label(*text, label, accel);

    HDC hdc = GetDC(state.window);
    HGDIOBJ previous = SelectObject(hdc, menu_font());
    SIZE label_size{};
    SIZE accel_size{};
    GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()), &label_size);
    if (!accel.empty()) {
        GetTextExtentPoint32W(hdc, accel.c_str(), static_cast<int>(accel.size()), &accel_size);
    }
    TEXTMETRICW metrics{};
    GetTextMetricsW(hdc, &metrics);
    SelectObject(hdc, previous);
    ReleaseDC(state.window, hdc);

    // The ampersand is a mnemonic marker, not a glyph, so it must not be
    // measured - every item would come out one character too wide.
    const int ampersands = static_cast<int>(std::count(label.begin(), label.end(), L'&'));
    const int ampersand_width = ampersands * (label_size.cx / (std::max)(1, static_cast<int>(label.size())));

    item->itemWidth = static_cast<UINT>(label_size.cx - ampersand_width + 28 +
                                        (accel.empty() ? 0 : accel_size.cx + 32));
    item->itemHeight = static_cast<UINT>((std::max)(static_cast<int>(metrics.tmHeight) + 8, 22));
}

void draw_menu_item(const RuntimeState& state, DRAWITEMSTRUCT* item) {
    const auto* text = reinterpret_cast<const std::wstring*>(item->itemData);
    if (text == nullptr) {
        return;
    }
    std::wstring label;
    std::wstring accel;
    split_menu_label(*text, label, accel);

    const Theme& theme = theme_of(state);
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    const bool checked = (item->itemState & ODS_CHECKED) != 0;

    const COLORREF highlight = state.dark_theme ? RGB(64, 64, 70) : RGB(222, 222, 228);
    HBRUSH back = CreateSolidBrush(selected ? highlight : theme.background);
    FillRect(item->hDC, &item->rcItem, back);
    DeleteObject(back);

    COLORREF ink = theme.text;
    if (disabled) {
        ink = state.dark_theme ? RGB(120, 120, 126) : RGB(160, 160, 166);
    }
    SetTextColor(item->hDC, ink);
    SetBkMode(item->hDC, TRANSPARENT);
    HGDIOBJ previous = SelectObject(item->hDC, menu_font());

    RECT text_rect = item->rcItem;
    text_rect.left += 22;
    text_rect.right -= 12;
    DrawTextW(item->hDC, label.c_str(), static_cast<int>(label.size()), &text_rect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    if (!accel.empty()) {
        DrawTextW(item->hDC, accel.c_str(), static_cast<int>(accel.size()), &text_rect,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    }
    if (checked) {
        RECT tick = item->rcItem;
        tick.left += 6;
        tick.right = tick.left + 14;
        DrawTextW(item->hDC, L"•", 1, &tick, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    SelectObject(item->hDC, previous);
}

// The menus advertise their shortcuts in a second column. Now that those are
// rebindable the text has to follow, and the way to do that is to rewrite the
// owner-draw label in place: calling ModifyMenu would replace the item and drop
// the dwItemData pointer the drawing code reads the label through.
void set_menu_accel(HMENU menu, UINT id, const std::wstring& accel) {
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_DATA;
    if (!GetMenuItemInfoW(menu, id, FALSE, &info) || info.dwItemData == 0) {
        return;
    }
    auto* label = reinterpret_cast<std::wstring*>(info.dwItemData);
    const auto tab = label->find(L'\t');
    const std::wstring head = tab == std::wstring::npos ? *label : label->substr(0, tab);
    *label = accel.empty() ? head : head + L"\t" + accel;
}

void refresh_menu_hotkeys(const RuntimeState& state) {
    if (state.menu == nullptr) {
        return;
    }
    struct Row {
        UINT id;
        int action;
    };
    static constexpr Row rows[] = {
        {IDM_FILE_OPEN, kHkOpenGenesis},    {IDM_FILE_OPEN_GG, kHkOpenGameGear},
        {IDM_FILE_OPEN_32X, kHkOpen32X},    {IDM_FILE_OPEN_CD, kHkOpenCd},
        {IDM_TOOLS_DUMP_LOG, kHkDumpDiagnostics},
        {IDM_TOOLS_MIRACLE_PIANO, kHkMiraclePiano},
        {IDM_FILE_EXIT, kHkQuit},
        {IDM_EMU_PAUSE, kHkPause},          {IDM_EMU_RESET_SOFT, kHkSoftReset},
        {IDM_EMU_RESET_HARD, kHkHardReset}, {IDM_EMU_POWER_OFF, kHkPowerOff},
        {IDM_VIEW_FULLSCREEN, kHkFullscreen}, {IDM_VIEW_CRT, kHkCrtFilter},
        {IDM_TOOLS_DUMP_VIDEO, kHkDumpVideo},
    };
    for (const auto& row : rows) {
        const auto& binding = state.hotkeys[row.action];
        set_menu_accel(state.menu, row.id,
                       hotkey_bound(binding) ? hotkey_display_name(binding) : std::wstring());
    }
    if (state.window != nullptr && !state.fullscreen) {
        DrawMenuBar(state.window);
    }
}

void sync_menu_state(const RuntimeState& state) {
    if (state.menu == nullptr) {
        return;
    }
    const UINT loaded = state.rom_loaded ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(state.menu, IDM_FILE_CLOSE, MF_BYCOMMAND | loaded);
    EnableMenuItem(state.menu, IDM_EMU_PAUSE, MF_BYCOMMAND | loaded);
    EnableMenuItem(state.menu, IDM_EMU_RESET_SOFT, MF_BYCOMMAND | loaded);
    EnableMenuItem(state.menu, IDM_EMU_RESET_HARD, MF_BYCOMMAND | loaded);
    EnableMenuItem(state.menu, IDM_EMU_POWER_OFF, MF_BYCOMMAND | loaded);
    EnableMenuItem(state.menu, IDM_TOOLS_DUMP_VIDEO, MF_BYCOMMAND | loaded);
    CheckMenuItem(state.menu, IDM_EMU_PAUSE,
                  MF_BYCOMMAND | (state.paused ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(state.menu, IDM_VIEW_DARK, IDM_VIEW_LIGHT,
                       state.dark_theme ? IDM_VIEW_DARK : IDM_VIEW_LIGHT, MF_BYCOMMAND);
    CheckMenuItem(state.menu, IDM_VIEW_FULLSCREEN,
                  MF_BYCOMMAND | (state.fullscreen ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_VIEW_CRT,
                  MF_BYCOMMAND | (state.crt_filter ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_VIEW_SMOOTH,
                  MF_BYCOMMAND | (state.smooth_scaling ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_VIEW_FPS,
                  MF_BYCOMMAND | (state.show_fps ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_TOOLS_MUTE_FM,
                  MF_BYCOMMAND | (state.mute_fm ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_TOOLS_MUTE_PSG,
                  MF_BYCOMMAND | (state.mute_psg ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_TOOLS_MUTE_DAC,
                  MF_BYCOMMAND | (state.mute_dac ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_TOOLS_MUTE_PWM,
                  MF_BYCOMMAND | (state.mute_pwm ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_TOOLS_MUTE_CD,
                  MF_BYCOMMAND | (state.mute_cd ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_TOOLS_RECORD_WAV,
                  MF_BYCOMMAND | (state.recording.is_open() ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_TOOLS_LOG_FM,
                  MF_BYCOMMAND | (state.fm_log.is_open() ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_TOOLS_LOG_PSG,
                  MF_BYCOMMAND | (state.psg_log.is_open() ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(state.menu, IDM_TOOLS_LOG_DAC,
                  MF_BYCOMMAND | (state.dac_log.is_open() ? MF_CHECKED : MF_UNCHECKED));
    const UINT aspect_id = state.aspect == Aspect::four_three     ? IDM_VIEW_ASPECT_4_3
                           : state.aspect == Aspect::sixteen_nine ? IDM_VIEW_ASPECT_16_9
                           : state.aspect == Aspect::custom       ? IDM_VIEW_ASPECT_CUSTOM
                                                                  : IDM_VIEW_ASPECT_STRETCH;
    // The range has to span every aspect id, and these are not numbered in the
    // order they appear on the menu: 4:3 and 16:9 sit BELOW stretch. Passing
    // stretch as the first id left them outside the range, so only stretch and
    // custom could ever show the mark.
    CheckMenuRadioItem(state.menu, IDM_VIEW_ASPECT_4_3, IDM_VIEW_ASPECT_CUSTOM, aspect_id,
                       MF_BYCOMMAND);
    const int scale = state.view_scale;
    for (int i = 1; i <= 4; ++i) {
        CheckMenuItem(state.menu, static_cast<UINT>(IDM_VIEW_SCALE_1X + i - 1),
                      MF_BYCOMMAND | (scale == i ? MF_CHECKED : MF_UNCHECKED));
    }

    CheckMenuItem(state.menu, IDM_TOOLS_MIRACLE_PIANO,
                  MF_BYCOMMAND | (piano_window_open() ? MF_CHECKED : MF_UNCHECKED));

    // Gear Link belongs to the first window. The second console's own menu
    // offers neither item: it did not start the link and closing it is not its
    // decision.
    const bool linked = gear_link_active();
    EnableMenuItem(state.menu, IDM_MP_GEAR_LINK,
                   MF_BYCOMMAND | ((linked || state.link_secondary) ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(state.menu, IDM_MP_GEAR_LINK_CLOSE,
                   MF_BYCOMMAND | ((linked && !state.link_secondary) ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(state.menu, IDM_MP_GEAR_LINK,
                  MF_BYCOMMAND | (linked ? MF_CHECKED : MF_UNCHECKED));
    // Which console is on screen. Offered in BOTH windows, because whichever
    // one is still showing is the only one a person can reach.
    {
        const LinkView view = gear_link_view();
        const struct {
            UINT id;
            LinkView view;
        } items[] = {{IDM_MP_LINK_VIEW_BOTH, LinkView::both},
                     {IDM_MP_LINK_VIEW_ONE, LinkView::first},
                     {IDM_MP_LINK_VIEW_TWO, LinkView::second}};
        for (const auto& item : items) {
            EnableMenuItem(state.menu, item.id, MF_BYCOMMAND | (linked ? MF_ENABLED : MF_GRAYED));
            CheckMenuItem(state.menu, item.id,
                          MF_BYCOMMAND | (linked && view == item.view ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    // Swapping a disc needs a disc in the drive. Booting the unit with an
    // empty tray does not need anything at all, so it stays available even
    // with no game loaded - that is rather the point of it.
    const bool disc_in = state.rom_loaded && state.machine == Machine::genesis &&
                         state.genesis.cd_mode();
    EnableMenuItem(state.menu, IDM_CD_SWAP, MF_BYCOMMAND | (disc_in ? MF_ENABLED : MF_GRAYED));

    // Both consoles of a link run the same cartridge, and the first window is
    // where it is chosen. Letting the second open a different game would leave
    // two machines talking a protocol neither of them shares.
    if (state.link_secondary) {
        for (const UINT item : {static_cast<UINT>(IDM_FILE_OPEN), static_cast<UINT>(IDM_FILE_OPEN_GG),
                                static_cast<UINT>(IDM_FILE_OPEN_32X),
                                static_cast<UINT>(IDM_FILE_OPEN_CD),
                                static_cast<UINT>(IDM_FILE_CLOSE)}) {
            EnableMenuItem(state.menu, item, MF_BYCOMMAND | MF_GRAYED);
        }
    }
}


// The window with no cartridge in it. Everything goes into a memory DC and is
// blitted once: the logo is alpha-blended over the background, and compositing
// that straight onto the window would show the bare fill for a moment first.
void draw_idle_screen(RuntimeState& state, HDC hdc, const RECT& client) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    HDC memory = CreateCompatibleDC(hdc);
    HBITMAP surface = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ previous = SelectObject(memory, surface);

    HBRUSH background = CreateSolidBrush(theme_of(state).background);
    FillRect(memory, &client, background);
    DeleteObject(background);

    if (state.logo_bitmap != nullptr && state.logo_width > 0 && state.logo_height > 0) {
        const double fit = (std::min)(
            (static_cast<double>(width) * kLogoFillFraction) / state.logo_width,
            (static_cast<double>(height) * kLogoFillFraction) / state.logo_height);
        const int draw_width = (std::max)(1, static_cast<int>(state.logo_width * fit));
        const int draw_height = (std::max)(1, static_cast<int>(state.logo_height * fit));
        HDC logo_dc = CreateCompatibleDC(memory);
        HGDIOBJ previous_logo = SelectObject(logo_dc, state.logo_bitmap);
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        SetStretchBltMode(memory, HALFTONE);
        SetBrushOrgEx(memory, 0, 0, nullptr);
        AlphaBlend(memory, (width - draw_width) / 2, (height - draw_height) / 2, draw_width,
                   draw_height, logo_dc, 0, 0, state.logo_width, state.logo_height, blend);
        SelectObject(logo_dc, previous_logo);
        DeleteDC(logo_dc);
    }

    // No theme slider here any more: View carries the dark/light choice, and
    // two controls for one setting is one too many.
    BitBlt(hdc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, previous);
    DeleteObject(surface);
    DeleteDC(memory);
}

int effective_scale(const RuntimeState& state) {
    return state.view_scale > 0 ? state.view_scale : window_scale(state);
}

void resize_for_content(RuntimeState& state) {
    if (state.window == nullptr || !state.rom_loaded) {
        return;
    }
    // A Gear Link owns both windows' geometry: half the screen each, which is
    // the whole point of the mode. Sizing to the cartridge here would pull one
    // of them off its half the moment a game was loaded.
    if (gear_link_active()) {
        InvalidateRect(state.window, nullptr, TRUE);
        return;
    }
    // Same reason as apply_pending_resize: in fullscreen there is no window to
    // size, and doing it anyway leaves fullscreen. Repaint instead so a new
    // cartridge or a new aspect ratio takes effect on the screen it is filling.
    if (state.fullscreen) {
        InvalidateRect(state.window, nullptr, TRUE);
        return;
    }
    const auto client = desired_client_size(state);
    RECT desired{0, 0, client.cx, client.cy};
    // TRUE for the menu: the bar eats client height, and sizing without telling
    // AdjustWindowRect about it crops the bottom of every frame.
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, TRUE);
    SetWindowPos(state.window, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);
}

void update_title(const RuntimeState& state) {
    if (state.window == nullptr) {
        return;
    }
    std::wstring title = L"GenX";
    // Which console is this? Both halves of a Gear Link run the same cartridge
    // and would otherwise carry identical titles, which is no help at all when
    // binding a pad to one of them.
    if (gear_link_active()) {
        title += state.link_secondary ? L" - Gear Link 2" : L" - Gear Link 1";
    }
    if (state.rom_loaded) {
        title += L" - " + state.rom_path.filename().wstring();
    }
    // Netplay is a MODE, not an event, so it belongs in the title rather than
    // in the status line - which clears itself after two seconds and left a
    // live session looking like it had stopped saying anything.
    if (state.show_fps) {
        wchar_t reading[32];
        std::swprintf(reading, 32, L"   %.1f fps", state.fps_value);
        title += reading;
    }
    if (!state.netplay_label.empty()) {
        title += L"   {" + state.netplay_label + L"}";
    }
    if (!state.status_message.empty()) {
        title += L"   [" + state.status_message + L"]";
    }
    SetWindowTextW(state.window, title.c_str());
}

void flush_battery(RuntimeState& state, bool force);

void unload_rom(RuntimeState& state) {
    // Before anything else: the cartridge is about to go, and its battery is
    // the one thing that has to outlive it.
    flush_battery(state, false);
    state.rom_loaded = false;
    state.paused = false;
    state.faulted = false;
    // The history belongs to the cartridge that is going away.
    state.rewind_history.clear();
    state.rewind_countdown = 0;
    state.has_visible_frame = false;
    state.presented_width = 0;
    state.presented_height = 0;
    state.bgra_frame.clear();
    state.status_text = "No cartridge";
    sync_menu_state(state);
    update_title(state);
    if (state.window != nullptr) {
        InvalidateRect(state.window, nullptr, TRUE);
    }
    gear_link_mirror_unload(state);
}

// Why a load failed, when the shell can do more about it than say so.
enum class LoadProblem {
    other,
    // A Sega CD disc with no BIOS to boot it, or none for its region. The
    // person almost certainly has the images somewhere; what they do not have
    // is any reason to know that the setting lives on a tab called BIOS.
    segacd_bios,
};

std::filesystem::path browse_for_folder(HWND owner, const wchar_t* prompt,
                                        const std::filesystem::path& current);

// --- Game Genie ------------------------------------------------------------
//
// A Mega Drive code and a Master System / Game Gear code are applied by two
// completely different mechanisms, because the two devices work differently
// - see mercury/core/game_genie.hpp. A Mega Drive code is written into the
// cartridge image, the way the device answers for the slot; a Game Gear code
// is watched for on the bus, because its address is a processor address that
// a bank switch can repoint.
//
// Both are undone without reloading: the Mega Drive kind remembers the word
// it replaced, and the Game Gear kind stops being in the map.

// Whether the machine that is loaded takes Mega Drive codes.
bool cheats_are_mega_drive(const RuntimeState& state) {
    return state.machine == Machine::genesis;
}

// What a game is called in the library. The file's own name rather than the
// header's title: it is what the person sees in the Open dialog and in
// Recent Files, and two dumps of one game share a title but not a name.
std::string current_game_key(const RuntimeState& state) {
    if (state.rom_path.empty()) {
        return {};
    }
    const auto stem = state.rom_path.stem().u8string();
    return std::string(stem.begin(), stem.end());
}

bool cheat_applies_here(const LibraryCheat& entry, const std::string& game) {
    if (entry.games.empty()) {
        return true;  // written with no game in mind: it is for all of them
    }
    return std::find(entry.games.begin(), entry.games.end(), game) != entry.games.end();
}

// Put the list into effect. Called after every change to it, and after a
// cartridge is loaded.
void apply_cheats(RuntimeState& state) {
    state.cheat_map.clear();
    const bool mega = cheats_are_mega_drive(state);
    const auto game = current_game_key(state);
    for (auto& entry : state.cheats) {
        auto& cheat = entry.cheat;
        const bool want = state.cheats_on && cheat.enabled && state.rom_loaded &&
                          cheat_applies_here(entry, game);
        if (!cheat.word) {
            // Game Gear / Master System: nothing to undo, it is simply in the
            // map or it is not.
            if (want && !mega) {
                state.cheat_map.add(static_cast<std::uint16_t>(cheat.address),
                                    static_cast<std::uint8_t>(cheat.value), cheat.compare);
            }
            continue;
        }
        // A word patch, so the odd bit of the address is dropped here rather
        // than in the decoder - the decoder's job is to say what the code
        // says, and every real code names an even address anyway.
        const std::uint32_t at = cheat.address & ~std::uint32_t{1};
        if (want && mega && !cheat.applied) {
            cheat.original = state.genesis.cartridge().patch16(at, cheat.value);
            cheat.applied = true;
        } else if ((!want || !mega) && cheat.applied) {
            state.genesis.cartridge().patch16(at, cheat.original);
            cheat.applied = false;
        }
    }
    state.game_gear.attach_cheats(state.cheat_map.empty() ? nullptr : &state.cheat_map);
}

// ONE LIBRARY, NOT A FILE PER GAME.
//
// Codes used to be kept beside the game they were typed against, which had
// two faults the user hit at once: with no game loaded there was no file to
// write to, so anything typed before opening one was lost; and loading a game
// cleared the list to read that game's file, which threw away whatever had
// just been entered.
//
// So there is one file, always writable, and each code carries the list of
// games it belongs to. A code with an empty list applies to ANY game, which
// is what a code typed with nothing loaded gets. The same code can sit
// against several games at once - some codes really do work across a series -
// and the dialog adds and removes the current game from that list.
//
// One code a line, readable and editable by hand on purpose:
//   <code>|<on|off>|<game>,<game>,...|<description>
std::filesystem::path cheat_library_path() {
    return recent_file_path().parent_path() / L"cheats.txt";
}

void save_cheats(const RuntimeState& state) {
    const auto path = cheat_library_path();
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    if (state.cheats.empty()) {
        std::filesystem::remove(path, ignored);
        return;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "# GenX Game Genie codes. One a line:\n"
        << "#   code|on or off|games it is for, separated by *|what it does\n"
        << "# An empty game list means the code is used for every game.\n";
    for (const auto& entry : state.cheats) {
        out << entry.cheat.code << "|" << (entry.cheat.enabled ? "on" : "off") << "|";
        for (std::size_t i = 0; i < entry.games.size(); ++i) {
            if (i != 0) {
                // A STAR, NOT A COMMA. Game names are file names and file
                // names are full of commas - "Sonic The Hedgehog (USA,
                // Europe)" is the normal shape of one - so a comma-separated
                // list silently tore every such name in half and the code
                // then matched nothing. A star cannot appear in a Windows
                // file name at all, so it can never be part of one.
                out << "*";
            }
            out << entry.games[i];
        }
        out << "|" << entry.cheat.description << "\n";
    }
}

// Read the library. Called once at startup, NOT when a game is loaded -
// loading a game only re-applies what is already here.
void load_cheat_library(RuntimeState& state) {
    state.cheats.clear();
    std::ifstream in(cheat_library_path());
    if (!in) {
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        // Split into at most four fields; the description may hold anything,
        // including the separator, so it takes the rest of the line.
        std::vector<std::string> fields;
        std::size_t at = 0;
        while (fields.size() < 3) {
            const auto bar = line.find('|', at);
            if (bar == std::string::npos) {
                break;
            }
            fields.push_back(line.substr(at, bar - at));
            at = bar + 1;
        }
        fields.push_back(line.substr(at));
        if (fields.empty()) {
            continue;
        }
        LibraryCheat entry;
        std::string error;
        if (!mercury::core::decode_cheat(fields[0], mercury::core::CheatFormat::automatic,
                                         entry.cheat, error)) {
            continue;  // a typo someone saved by hand
        }
        entry.cheat.enabled = fields.size() < 2 || fields[1].rfind("on", 0) == 0;
        if (fields.size() > 2 && !fields[2].empty()) {
            std::size_t from = 0;
            while (from <= fields[2].size()) {
                const auto star = fields[2].find('*', from);
                auto name =
                    fields[2].substr(from, star == std::string::npos ? star : star - from);
                if (!name.empty()) {
                    entry.games.push_back(name);
                }
                if (star == std::string::npos) {
                    break;
                }
                from = star + 1;
            }
        }
        if (fields.size() > 3) {
            entry.cheat.description = fields[3];
        }
        state.cheats.push_back(entry);
    }
}

// Called when a game is loaded. The library stays; only what is in force
// changes - and every Mega Drive patch is forgotten, because the cartridge
// image it was written into has gone with the last game.
void load_cheats(RuntimeState& state) {
    for (auto& entry : state.cheats) {
        entry.cheat.applied = false;
    }
    apply_cheats(state);
}

// Everything that belongs to the MEDIUM rather than to the window, once it is
// in and running. The window, menu, theme, logo and the open audio device are
// deliberately left alone so a second Open does not rebuild the shell.
//
// Its own function because two things load a machine: a file that was opened,
// and the Sega CD booted with an empty tray - and the second one has no path
// at all. Duplicating twenty lines of bookkeeping is how one of them ends up
// forgetting to clear the rewind history.
void finish_loading_machine(RuntimeState& state, Machine machine,
                            const std::filesystem::path& path, int gain) {
    state.machine = machine;
    state.audio_gain = gain;
    state.rom_path = path;
    state.rom_loaded = true;
    state.paused = false;
    state.faulted = false;
    state.status_text = "Booting";
    state.has_visible_frame = false;
    state.last_presented_frame = 0;
    state.start_pulse_frames_remaining = 0;
    state.resize_pending = false;
    state.ym_resample_phase = 0.0;
    state.ym_previous_mono = 0;
    state.pending_audio.clear();
    state.rgb_frame.clear();
    state.bgra_frame.clear();
    // Snapshots of the cartridge that was in a moment ago.
    state.rewind_history.clear();
    state.rewind_countdown = 0;
    state.warned_no_32x_bios = false;
    if (machine == Machine::genesis) {
        apply_port_kinds(state);
    }
    update_framebuffer(state);
    if (state.machine == Machine::genesis) {
        state.presented_width = state.genesis.vdp().screen_width();
        state.presented_height = state.genesis.vdp().screen_height();
    } else {
        state.presented_width = state.game_gear.vdp().visible_width();
        state.presented_height = state.game_gear.vdp().visible_height();
    }
    // A different game is not the host's any more. Left set, this game's own
    // saves would never be written.
    if (!state.netplay.active()) {
        RuntimeState* partner = link_partner(state);
        if (partner == nullptr || !partner->netplay.active()) {
            g_netplay_joined = false;
        }
    }
    restore_battery(state);
    // Codes are kept per game, so the new cartridge gets its own list - and
    // the last one's Mega Drive patches went out with its ROM image.
    load_cheats(state);
    resize_for_content(state);
    sync_menu_state(state);
    update_title(state);
    // Say it without a dialog to dismiss. Two people sharing one Game Gear is
    // not something anyone would guess is available, and the alternative is
    // that it only ever gets found by someone reading the Input tab.
    if (shared_pad_game(state)) {
        set_status(state, L"Two players on one Game Gear: P1 d-pad, P2 buttons A and B");
    }
    // Both halves of a Gear Link run the same cartridge, so opening one here
    // opens it on the other console too. Nothing to mirror without a file.
    if (!path.empty()) {
        gear_link_mirror_rom(state, path);
    }
}

// Load a cartridge, choosing the machine from the extension and then from the
// Mega Drive header - a .bin could be either, and the "SEGA" signature at $100
// settles it.
//
// `problem` says whether the shell can do anything about a failure. Only one
// kind can be fixed from here: a disc with no BIOS to boot it - see
// report_load_failure.
bool load_rom_from_path(RuntimeState& state, const std::filesystem::path& path,
                        std::string& error, LoadProblem* problem = nullptr) {
    if (problem != nullptr) {
        *problem = LoadProblem::other;
    }
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // .bin and .zip are ambiguous - either machine's cartridge can arrive under
    // them - so both are offered to the Mega Drive loader first and fall through
    // to the Game Gear when its header check rejects them.
    const bool ambiguous = extension == ".bin" || extension == ".zip";
    // .32x is a Mega Drive cartridge as far as loading goes. The adapter is
    // always attached and switches itself on from the header, so there is no
    // separate machine to pick - see ThirtyTwoX::attach_rom.
    const bool genesis_extension = extension == ".md" || extension == ".gen" ||
                                   extension == ".smd" || extension == ".32x" || ambiguous;

    // The outgoing cartridge's battery goes to disk before the new one takes
    // its place - swapping games must not cost the first one's save.
    flush_battery(state, false);

    // Loaded IN PLACE, never by building a fresh RuntimeState and moving it in.
    // Genesis wires [this] lambdas into the 68000 and Z80 bus structs and into
    // the VDP's memory reader in its constructor; moving the object copies those
    // lambdas with the OLD this still captured, so every bus access afterwards
    // runs against a destroyed temporary. It looks like "no game loads" and it
    // is a use-after-free, not a loader fault.
    Machine machine = Machine::game_gear;
    int gain = kRuntimeAudioGain;
    // A Sega CD disc. Checked before the cartridge extensions because a .bin
    // can be either, and a CD .bin must never be handed to the cartridge
    // loader - it would "load" as a very large, very broken cartridge.
    if (mercury::segacd::Disc::is_disc_image(path)) {
        // The disc's region picks the BIOS: a disc only boots on its own.
        mercury::segacd::Disc peek;
        if (!peek.open(path, error)) {
            return false;
        }
        std::vector<std::uint8_t> bios;
        if (!mercury::segacd::load_bios_for(peek.region(), state.bios_folder_segacd, path, bios,
                                            error)) {
            // The one load failure the shell can offer to fix rather than
            // just report - see report_load_failure.
            if (problem != nullptr) {
                *problem = LoadProblem::segacd_bios;
            }
            return false;
        }
        const auto result = state.genesis.load_cd(path, bios);
        if (!result.loaded) {
            error = result.message;
            return false;
        }
        machine = Machine::genesis;
        gain = 1;
    } else if (genesis_extension) {
        const auto result = state.genesis.load_rom(path);
        if (result.loaded) {
            machine = Machine::genesis;
            gain = 1;
        } else if (!ambiguous) {
            // A .md that is not a Mega Drive image is an error, not something to
            // retry as a Game Gear cartridge.
            error = result.message;
            return false;
        }
    }
    if (machine == Machine::game_gear) {
        const auto result = state.game_gear.load_rom(path);
        if (!result.ok) {
            error = result.error;
            return false;
        }
        state.game_gear.reset();
    }

    finish_loading_machine(state, machine, path, gain);
    return true;
}

// Push the configured firmware folders into the machine. Called at startup and
// again whenever Configure is accepted.
//
// It does NOT reload whatever is running. The firmware is read once, when a
// cartridge is loaded, so changing this while a game is playing can only take
// effect on the next one - and resetting someone's game out from under them to
// apply a settings change is worse than waiting. The status line says so
// instead, but only when it actually matters: with no 32X cartridge in, the
// setting has nothing to be late for.
void apply_bios_folders(RuntimeState& state) {
    auto& adapter = state.genesis.thirtytwox();
    const bool changed = adapter.bios_folder() != state.bios_folder_32x;
    adapter.set_bios_folder(state.bios_folder_32x);
    if (changed && state.rom_loaded && state.machine == Machine::genesis &&
        adapter.adapter().enabled()) {
        set_status(state, L"32X firmware folder saved - it applies to the next game you open");
    }
}

// Soft reset is the console's RESET button - RAM and battery saves survive.
// Hard reset is a power cycle. The Game Gear only has the one entry point, so
// both do the same thing there; the menu still offers both rather than hiding
// an item depending on which cartridge happens to be in.
void restart_machine(RuntimeState& state, bool hard) {
    if (!state.rom_loaded) {
        return;
    }
    if (state.machine == Machine::genesis) {
        if (hard) {
            state.genesis.reset();
        } else {
            state.genesis.soft_reset();
        }
    } else {
        state.game_gear.reset();
    }
    state.paused = false;
    state.faulted = false;
    state.status_text = "Booting";
    state.has_visible_frame = false;
    state.last_presented_frame = 0;
    state.start_pulse_frames_remaining = 0;
    state.ym_resample_phase = 0.0;
    state.ym_previous_mono = 0;
    state.pending_audio.clear();
    // Snapshots from before the reset would rewind straight back past it.
    state.rewind_history.clear();
    state.rewind_countdown = 0;
    if (state.machine == Machine::genesis) {
        // A reset clears the port back to its own default kind.
        apply_port_kinds(state);
    }
    update_framebuffer(state);
    sync_menu_state(state);
    if (state.window != nullptr) {
        InvalidateRect(state.window, nullptr, FALSE);
    }
}

void soft_reset(RuntimeState& state) { restart_machine(state, false); }
void hard_reset(RuntimeState& state) { restart_machine(state, true); }

// A LOAD THAT FAILED FOR WANT OF A BIOS IS NOT REALLY AN ERROR, it is a
// question. Everything else here just gets reported.
//
// The first Sega CD disc someone opens fails, because the BIOS is a separate
// download and GenX cannot ship it. Saying so and stopping leaves them to
// discover Configure, the BIOS tab and which of the three folder boxes is the
// right one. So the message asks instead: point at the folder, and the disc
// they were already opening is retried on the spot. The answer is saved, so
// the question is asked once ever.
//
// Returns true when the game ended up loaded.
bool report_load_failure(RuntimeState& state, const std::filesystem::path& path,
                         const std::string& error, LoadProblem problem) {
    if (problem != LoadProblem::segacd_bios) {
        MessageBoxW(state.window, widen(error).c_str(), L"GenX", MB_OK | MB_ICONERROR);
        return false;
    }
    const std::wstring question =
        widen(error) +
        L"\n\nGenX cannot include the Sega CD BIOS - it is Sega's, not ours - so it has to be "
        L"pointed at your own copy. It is a folder of 128KB images, usually one per region.\n\n"
        L"Choose that folder now?";
    if (MessageBoxW(state.window, question.c_str(), L"GenX - Sega CD BIOS needed",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return false;
    }
    const auto chosen = browse_for_folder(state.window, L"Where your Sega CD BIOS images are",
                                          state.bios_folder_segacd);
    if (chosen.empty()) {
        return false;
    }
    state.bios_folder_segacd = chosen;
    save_settings(state);
    std::string retry_error;
    LoadProblem retry_problem = LoadProblem::other;
    if (load_rom_from_path(state, path, retry_error, &retry_problem)) {
        return true;
    }
    // Once only. A second folder that also has nothing in it is a plain error,
    // or the loop never ends.
    MessageBoxW(state.window, widen(retry_error).c_str(), L"GenX", MB_OK | MB_ICONERROR);
    return false;
}

void open_recent(RuntimeState& state, std::size_t index) {
    if (index >= state.recent.size()) {
        return;
    }
    // Copied out first: loading rewrites the list this reference would point at.
    const auto path = state.recent[index];
    std::string error;
    LoadProblem problem = LoadProblem::other;
    if (!load_rom_from_path(state, path, error, &problem) &&
        !report_load_failure(state, path, error, problem)) {
        return;
    }
    remember_recent(state, path);
    InvalidateRect(state.window, nullptr, TRUE);
}

// --- Configure -------------------------------------------------------------
// A frame holding a tab control and three child dialogs, one per page. Every
// page edits a draft rather than the live settings, so Cancel really does undo
// everything - except the volume, which has to be audible while it is being
// dragged and is therefore applied live and put back if the dialog is cancelled.

// General, Input, Hotkeys, BIOS. Named rather than written as a literal in the
// four loops that walk the pages: adding the BIOS tab meant finding every bare
// "3" in the frame, and the next page should not repeat that.
constexpr int kConfigPageCount = 4;

struct ConfigDraft {
    RuntimeState* state = nullptr;
    bool hide_cursor = false;
    bool check_for_updates = true;
    int volume = 100;
    bool muted = false;
    bool console_filter = true;
    int original_volume = 100;
    bool original_muted = false;
    bool original_console_filter = true;
    std::array<std::filesystem::path, kSaveFolderCount> games_folders{};
    std::array<std::filesystem::path, kSaveFolderCount> save_folders{};
    std::filesystem::path bios_folder_32x{};
    std::filesystem::path bios_folder_segacd{};
    std::array<PlayerInput, 2> players{};
    // Which profile each player's dropdown names. Kept here with the bindings
    // so Cancel throws it away with them.
    std::array<std::wstring, 2> profile_names{};
    std::array<Hotkey, kHkCount> hotkeys{};
    HWND pages[kConfigPageCount]{};
    int current_page = 0;
    // Which binding box the pointer is over, and which one is waiting for a
    // key. -1 for neither. Held on the draft rather than in the drawing code so
    // both picture controls can be repainted consistently.
    int hover_player = -1;
    int hover_row = -1;
    int capture_player = -1;
    int capture_row = -1;
    // A gamepad capture cannot use the keyboard hook, so it polls on a timer
    // and takes the first button that was NOT already held when the box was
    // clicked - otherwise a trigger resting slightly down binds itself.
    bool capture_from_pad = false;
    PadInput capture_baseline = kPadInputNone;
    DIJOYSTATE2 capture_di_baseline{};
    // Whether each player's XInput slot currently answers. Cached rather than
    // polled during painting: asking about an empty slot is slow.
    std::array<bool, 2> pad_present{};
};

constexpr UINT_PTR kPadCaptureTimer = 2;
constexpr UINT_PTR kPadStatusTimer = 3;

void refresh_pad_presence(ConfigDraft& draft) {
    for (int player = 0; player < 2; ++player) {
        bool connected = false;
        const auto& input = draft.players[player];
        if (input.device == InputDevice::xinput) {
            read_gamepad(input.pad_slot, connected);
        } else if (input.device == InputDevice::dinput) {
            DIJOYSTATE2 ignored{};
            connected = read_dinput(input.pad_slot, ignored);
        }
        draft.pad_present[player] = connected;
    }
}

// Capturing a key has to happen BEFORE the dialog manager sees it. Tab, Enter,
// Escape and the arrows are all navigation to IsDialogMessage, and Mode is
// bound to Tab out of the box - so binding any of them through a normal control
// message is impossible. A thread keyboard hook runs inside GetMessage, which is
// earlier than that. Hooks are a callback with no user parameter, hence the file
// statics; only one capture can be in progress at a time by construction.
constexpr UINT WM_APP_CAPTURE_DONE = WM_APP + 1;

HHOOK g_capture_hook = nullptr;
HWND g_capture_owner = nullptr;
UINT g_capture_key = 0;

bool is_modifier_key(UINT key) {
    switch (key) {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU:
        case VK_LSHIFT: case VK_RSHIFT:
        case VK_LCONTROL: case VK_RCONTROL:
        case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK capture_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code != HC_ACTION) {
        return CallNextHookEx(g_capture_hook, code, wparam, lparam);
    }
    // Fail open, never closed. This hook swallows EVERY key on the thread, so
    // if it ever outlives the page that asked for it the emulator loses its
    // keyboard entirely and says nothing about why. Nothing known does that
    // today, but the cost of the check is one IsWindow call per keystroke and
    // the cost of being wrong is a dead keyboard.
    if (g_capture_owner == nullptr || IsWindow(g_capture_owner) == FALSE) {
        const HHOOK stale = g_capture_hook;
        g_capture_hook = nullptr;
        g_capture_owner = nullptr;
        if (stale != nullptr) {
            UnhookWindowsHookEx(stale);
        }
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    const bool key_up = (lparam & (1u << 31)) != 0;
    const auto key = static_cast<UINT>(wparam);
    if (!key_up && !is_modifier_key(key)) {
        g_capture_key = key;
        PostMessageW(g_capture_owner, WM_APP_CAPTURE_DONE, 0, 0);
    }
    // Swallow everything while capturing, key-ups included: a released Alt that
    // reached the dialog would open its system menu.
    return 1;
}

void begin_capture(HWND page) {
    if (g_capture_hook != nullptr) {
        return;
    }
    g_capture_owner = page;
    g_capture_key = 0;
    g_capture_hook = SetWindowsHookExW(WH_KEYBOARD, capture_hook_proc, nullptr, GetCurrentThreadId());
}

void end_capture() {
    if (g_capture_hook != nullptr) {
        UnhookWindowsHookEx(g_capture_hook);
        g_capture_hook = nullptr;
    }
    g_capture_owner = nullptr;
}

bool capturing() { return g_capture_hook != nullptr; }

// Themed background for a dialog or one of its pages. Static because the brush
// has to outlive the message that returns it, and one brush serves every call
// at a given theme.
HBRUSH theme_brush(const RuntimeState& state, HDC dc) {
    const Theme& theme = theme_of(state);
    static HBRUSH brush = nullptr;
    static COLORREF made_for = 0xFFFFFFFF;
    if (brush == nullptr || made_for != theme.background) {
        if (brush != nullptr) {
            DeleteObject(brush);
        }
        brush = CreateSolidBrush(theme.background);
        made_for = theme.background;
    }
    SetTextColor(dc, theme.text);
    SetBkColor(dc, theme.background);
    return brush;
}

// An edit box does not take its colours from the dialog: it asks separately,
// and left alone it stays white with black text in the middle of a dark page.
HBRUSH theme_field_brush(const RuntimeState& state, HDC dc) {
    const bool dark = state.dark_theme;
    const COLORREF fill = dark ? RGB(30, 30, 34) : RGB(255, 255, 255);
    const COLORREF text = dark ? RGB(220, 220, 226) : RGB(30, 30, 34);
    static HBRUSH brush = nullptr;
    static COLORREF made_for = 0xFFFFFFFF;
    if (brush == nullptr || made_for != fill) {
        if (brush != nullptr) {
            DeleteObject(brush);
        }
        brush = CreateSolidBrush(fill);
        made_for = fill;
    }
    SetTextColor(dc, text);
    SetBkColor(dc, fill);
    return brush;
}

// The tab control paints its own strip in the system colours, so the band above
// the pages stays bright on a dark dialog unless the erase is taken over. The
// items themselves are owner-drawn, which is the other half of the same job.
WNDPROC g_config_tab_original = nullptr;

LRESULT CALLBACK config_tab_proc(HWND control, UINT message, WPARAM wparam, LPARAM lparam);

std::filesystem::path browse_for_folder(HWND owner, const wchar_t* prompt,
                                        const std::filesystem::path& current) {
    // The path is handed to the callback so the dialog opens on the folder that
    // is already set rather than at the desktop.
    std::wstring start = current.wstring();
    BROWSEINFOW info{};
    wchar_t chosen[MAX_PATH] = L"";
    info.hwndOwner = owner;
    info.pszDisplayName = chosen;
    info.lpszTitle = prompt;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    info.lParam = reinterpret_cast<LPARAM>(start.c_str());
    info.lpfn = [](HWND dialog, UINT message, LPARAM, LPARAM data) -> int {
        if (message == BFFM_INITIALIZED && data != 0) {
            SendMessageW(dialog, BFFM_SETSELECTIONW, TRUE, data);
        }
        return 0;
    };
    LPITEMIDLIST id = SHBrowseForFolderW(&info);
    if (id == nullptr) {
        return {};
    }
    wchar_t path[MAX_PATH] = L"";
    const bool ok = SHGetPathFromIDListW(id, path) != FALSE;
    CoTaskMemFree(id);
    return ok ? std::filesystem::path(path) : std::filesystem::path();
}

// --- General page ----------------------------------------------------------

void set_path_field(HWND page, int control, const std::filesystem::path& path) {
    SetDlgItemTextW(page, control, path.empty() ? L"" : path.wstring().c_str());
}

std::filesystem::path get_path_field(HWND page, int control) {
    wchar_t text[MAX_PATH] = L"";
    GetDlgItemTextW(page, control, text, MAX_PATH);
    return text[0] == L'\0' ? std::filesystem::path() : std::filesystem::path(text);
}

void update_volume_label(HWND page, int volume) {
    SetDlgItemTextW(page, IDC_GEN_VOLUME_VALUE, (std::to_wstring(volume) + L"%").c_str());
}

// The slider is applied to the running machine as it moves - a volume control
// you cannot hear while dragging is not a volume control.
void apply_live_volume(ConfigDraft& draft) {
    draft.state->volume = draft.volume;
    draft.state->muted = draft.muted;
    // Live too, and for the same reason: the filter is the sort of thing you
    // judge by flipping it back and forth while a game is playing.
    draft.state->console_filter = draft.console_filter;
}

INT_PTR CALLBACK general_page_proc(HWND page, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* draft = reinterpret_cast<ConfigDraft*>(GetWindowLongPtrW(page, GWLP_USERDATA));
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(page, GWLP_USERDATA, static_cast<LONG_PTR>(lparam));
            auto* init = reinterpret_cast<ConfigDraft*>(lparam);
            set_path_field(page, IDC_GEN_GAMES_GENESIS, init->games_folders[kSaveGenesis]);
            set_path_field(page, IDC_GEN_GAMES_GG, init->games_folders[kSaveGameGear]);
            set_path_field(page, IDC_GEN_GAMES_32X, init->games_folders[kSave32X]);
            set_path_field(page, IDC_GEN_GAMES_CD, init->games_folders[kSaveCd]);
            set_path_field(page, IDC_GEN_SAVE_GENESIS, init->save_folders[kSaveGenesis]);
            set_path_field(page, IDC_GEN_SAVE_GG, init->save_folders[kSaveGameGear]);
            set_path_field(page, IDC_GEN_SAVE_32X, init->save_folders[kSave32X]);
            set_path_field(page, IDC_GEN_SAVE_CD, init->save_folders[kSaveCd]);
            CheckDlgButton(page, IDC_GEN_HIDE_CURSOR,
                           init->hide_cursor ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(page, IDC_GEN_CHECK_UPDATES,
                           init->check_for_updates ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(page, IDC_GEN_MUTE, init->muted ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(page, IDC_GEN_CONSOLE_FILTER,
                           init->console_filter ? BST_CHECKED : BST_UNCHECKED);
            HWND slider = GetDlgItem(page, IDC_GEN_VOLUME);
            SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(slider, TBM_SETTICFREQ, 10, 0);
            SendMessageW(slider, TBM_SETPOS, TRUE, init->volume);
            update_volume_label(page, init->volume);
            return TRUE;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            if (draft != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_brush(*draft->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (draft != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_field_brush(*draft->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_HSCROLL:
            if (draft != nullptr &&
                reinterpret_cast<HWND>(lparam) == GetDlgItem(page, IDC_GEN_VOLUME)) {
                draft->volume = static_cast<int>(
                    SendMessageW(GetDlgItem(page, IDC_GEN_VOLUME), TBM_GETPOS, 0, 0));
                update_volume_label(page, draft->volume);
                apply_live_volume(*draft);
                return TRUE;
            }
            break;
        case WM_COMMAND: {
            if (draft == nullptr) {
                break;
            }
            const int control = LOWORD(wparam);
            struct Browse {
                int button;
                int field;
                const wchar_t* prompt;
                int folder;
            };
            static const Browse browses[] = {
                {IDC_GEN_GAMES_GENESIS_BROWSE, IDC_GEN_GAMES_GENESIS,
                 L"Where your Genesis games are", kSaveGenesis},
                {IDC_GEN_GAMES_GG_BROWSE, IDC_GEN_GAMES_GG, L"Where your Game Gear games are",
                 kSaveGameGear},
                {IDC_GEN_GAMES_32X_BROWSE, IDC_GEN_GAMES_32X, L"Where your 32X games are",
                 kSave32X},
                {IDC_GEN_GAMES_CD_BROWSE, IDC_GEN_GAMES_CD, L"Where your Sega CD games are",
                 kSaveCd},
                {IDC_GEN_SAVE_GENESIS_BROWSE, IDC_GEN_SAVE_GENESIS,
                 L"Where Genesis cartridge saves go", kSaveGenesis},
                {IDC_GEN_SAVE_GG_BROWSE, IDC_GEN_SAVE_GG,
                 L"Where Game Gear cartridge saves go", kSaveGameGear},
                {IDC_GEN_SAVE_32X_BROWSE, IDC_GEN_SAVE_32X, L"Where 32X saves go", kSave32X},
                {IDC_GEN_SAVE_CD_BROWSE, IDC_GEN_SAVE_CD, L"Where Sega CD saves go", kSaveCd},
            };
            for (const auto& browse : browses) {
                if (control != browse.button) {
                    continue;
                }
                const auto current = get_path_field(page, browse.field);
                const auto chosen = browse_for_folder(page, browse.prompt, current);
                if (!chosen.empty()) {
                    set_path_field(page, browse.field, chosen);
                }
                return TRUE;
            }
            if ((control == IDC_GEN_MUTE || control == IDC_GEN_CONSOLE_FILTER) &&
                HIWORD(wparam) == BN_CLICKED) {
                draft->muted = IsDlgButtonChecked(page, IDC_GEN_MUTE) == BST_CHECKED;
                draft->console_filter =
                    IsDlgButtonChecked(page, IDC_GEN_CONSOLE_FILTER) == BST_CHECKED;
                apply_live_volume(*draft);
                return TRUE;
            }
            break;
        }
        default:
            break;
    }
    return FALSE;
}

// Read the page's controls back into the draft. Called on OK, and only then:
// the edit fields are free-typed, so there is nothing to validate until the
// user says they are finished.
void commit_general_page(HWND page, ConfigDraft& draft) {
    if (page == nullptr) {
        return;
    }
    draft.games_folders[kSaveGenesis] = get_path_field(page, IDC_GEN_GAMES_GENESIS);
    draft.games_folders[kSaveGameGear] = get_path_field(page, IDC_GEN_GAMES_GG);
    draft.games_folders[kSave32X] = get_path_field(page, IDC_GEN_GAMES_32X);
    draft.games_folders[kSaveCd] = get_path_field(page, IDC_GEN_GAMES_CD);
    draft.save_folders[kSaveGenesis] = get_path_field(page, IDC_GEN_SAVE_GENESIS);
    draft.save_folders[kSaveGameGear] = get_path_field(page, IDC_GEN_SAVE_GG);
    draft.save_folders[kSave32X] = get_path_field(page, IDC_GEN_SAVE_32X);
    draft.save_folders[kSaveCd] = get_path_field(page, IDC_GEN_SAVE_CD);
    draft.hide_cursor = IsDlgButtonChecked(page, IDC_GEN_HIDE_CURSOR) == BST_CHECKED;
    draft.check_for_updates = IsDlgButtonChecked(page, IDC_GEN_CHECK_UPDATES) == BST_CHECKED;
    draft.muted = IsDlgButtonChecked(page, IDC_GEN_MUTE) == BST_CHECKED;
    draft.console_filter = IsDlgButtonChecked(page, IDC_GEN_CONSOLE_FILTER) == BST_CHECKED;
    draft.volume =
        static_cast<int>(SendMessageW(GetDlgItem(page, IDC_GEN_VOLUME), TBM_GETPOS, 0, 0));
}

// --- BIOS page -------------------------------------------------------------

// Say what is in the folder, as it is typed. The alternative - accepting any
// path and finding out at the next Open ROM - makes a typo look like a broken
// emulator, and the failure is silent because a missing BIOS is not an error
// here: the cartridge just boots the synthesised way instead.
//
// The scan is ThirtyTwoX's own, so "found" here and "used" at load time cannot
// disagree.
void update_bios_status(HWND page) {
    const auto folder = get_path_field(page, IDC_BIOS_32X);
    if (folder.empty()) {
        SetDlgItemTextW(page, IDC_BIOS_32X_STATUS,
                        L"Not set - GenX will look in your game folder, then boot without it.");
        return;
    }
    std::error_code ignored;
    if (!std::filesystem::is_directory(folder, ignored)) {
        SetDlgItemTextW(page, IDC_BIOS_32X_STATUS, L"That folder does not exist.");
        return;
    }
    const auto scan = mercury::thirtytwox::ThirtyTwoX::scan_bios_folder(folder);
    if (scan.complete()) {
        SetDlgItemTextW(page, IDC_BIOS_32X_STATUS, L"All three images found.");
        return;
    }
    // Name what is MISSING rather than what was found: the whole point of the
    // line is to say what to go and fetch.
    std::wstring missing;
    const auto add = [&missing](const wchar_t* name) {
        if (!missing.empty()) {
            missing += L", ";
        }
        missing += name;
    };
    if (!scan.genesis_page) add(L"Mega Drive page (256 bytes)");
    if (!scan.master_rom) add(L"master boot ROM (2048 bytes)");
    if (!scan.slave_rom) add(L"slave boot ROM (1024 bytes)");
    SetDlgItemTextW(page, IDC_BIOS_32X_STATUS, (L"Missing: " + missing).c_str());
}

// The Sega CD folder's status NAMES THE FILE each region will actually boot
// from, one line per region.
//
// It used to say only which regions were covered - "Found: USA, Europe,
// Japan" - and that is not enough to debug with. A BIOS pack routinely holds
// several images for one region, only some of which work: one "Model 2 v2.00"
// image in the user's own pack never starts its sub CPU at all. Knowing a USA
// BIOS was found does not say WHICH, so a disc that hangs looks like an
// emulator fault rather than a bad image that can simply be moved out of the
// folder. The scan and the pick are the loader's own, so this line cannot
// disagree with what happens at load time.
void update_segacd_bios_status(HWND page) {
    const auto folder = get_path_field(page, IDC_BIOS_SEGACD);
    if (folder.empty()) {
        // The breaks are written out rather than left to word wrap: the
        // control does not wrap, because the per-region list below is a
        // tab-aligned table and wrapping a long file name would fold the
        // table in half.
        SetDlgItemTextW(page, IDC_BIOS_SEGACD_STATUS,
                        L"Not set - GenX will look for BIOS files in your game folder,\r\n"
                        L"then in any folder with BIOS in its name near your discs.");
        return;
    }
    std::error_code ignored;
    if (!std::filesystem::is_directory(folder, ignored)) {
        SetDlgItemTextW(page, IDC_BIOS_SEGACD_STATUS, L"That folder does not exist.");
        return;
    }
    const auto images = mercury::segacd::scan_bios_folder(folder);
    if (images.empty()) {
        SetDlgItemTextW(page, IDC_BIOS_SEGACD_STATUS,
                        L"No Sega CD BIOS here. GenX looks for 128KB files carrying a\r\n"
                        L"Mega Drive header, subfolders included, two deep.");
        return;
    }
    // scan_bios_folder returns them in the order the loader prefers, so the
    // first of a region is the one that region's discs will get.
    std::wstring text;
    int missing = 0;
    const auto line = [&](char region, const wchar_t* name) {
        const mercury::segacd::BiosImage* pick = nullptr;
        for (const auto& image : images) {
            if (image.region == region) {
                pick = &image;
                break;
            }
        }
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += name;
        text += L"\t";
        if (pick == nullptr) {
            text += L"none - discs from this region will not boot";
            ++missing;
            return;
        }
        text += pick->path.filename().wstring();
    };
    line('U', L"USA");
    line('E', L"Europe");
    line('J', L"Japan");
    // An image whose header does not name a region is still usable; say so
    // rather than leaving it out of a count that then looks wrong.
    std::size_t unplaced = 0;
    for (const auto& image : images) {
        unplaced += image.region == 0 ? 1 : 0;
    }
    if (unplaced != 0) {
        text += L"\r\n" + std::to_wstring(unplaced) +
                (unplaced == 1 ? L" more image names no region" : L" more images name no region");
    }
    SetDlgItemTextW(page, IDC_BIOS_SEGACD_STATUS, text.c_str());
}

INT_PTR CALLBACK bios_page_proc(HWND page, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* draft = reinterpret_cast<ConfigDraft*>(GetWindowLongPtrW(page, GWLP_USERDATA));
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(page, GWLP_USERDATA, static_cast<LONG_PTR>(lparam));
            auto* init = reinterpret_cast<ConfigDraft*>(lparam);
            set_path_field(page, IDC_BIOS_32X, init->bios_folder_32x);
            update_bios_status(page);
            set_path_field(page, IDC_BIOS_SEGACD, init->bios_folder_segacd);
            update_segacd_bios_status(page);
            return TRUE;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            if (draft != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_brush(*draft->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (draft != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_field_brush(*draft->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_COMMAND: {
            if (draft == nullptr) {
                break;
            }
            const int control = LOWORD(wparam);
            if (control == IDC_BIOS_32X_BROWSE) {
                const auto current = get_path_field(page, IDC_BIOS_32X);
                const auto chosen =
                    browse_for_folder(page, L"Where your 32X firmware images are", current);
                if (!chosen.empty()) {
                    set_path_field(page, IDC_BIOS_32X, chosen);
                    update_bios_status(page);
                }
                return TRUE;
            }
            if (control == IDC_BIOS_32X && HIWORD(wparam) == EN_CHANGE) {
                update_bios_status(page);
                return TRUE;
            }
            if (control == IDC_BIOS_SEGACD_BROWSE) {
                const auto current = get_path_field(page, IDC_BIOS_SEGACD);
                const auto chosen =
                    browse_for_folder(page, L"Where your Sega CD BIOS images are", current);
                if (!chosen.empty()) {
                    set_path_field(page, IDC_BIOS_SEGACD, chosen);
                    update_segacd_bios_status(page);
                }
                return TRUE;
            }
            if (control == IDC_BIOS_SEGACD && HIWORD(wparam) == EN_CHANGE) {
                update_segacd_bios_status(page);
                return TRUE;
            }
            break;
        }
        default:
            break;
    }
    return FALSE;
}

void commit_bios_page(HWND page, ConfigDraft& draft) {
    if (page == nullptr) {
        return;
    }
    draft.bios_folder_32x = get_path_field(page, IDC_BIOS_32X);
    draft.bios_folder_segacd = get_path_field(page, IDC_BIOS_SEGACD);
}

// --- Hotkeys page ----------------------------------------------------------

// This translation unit does not define UNICODE, so every ListView_ and
// TabCtrl_ convenience macro expands to the ANSI message - and the ones that
// take a struct cast it rather than failing to compile, which would send a
// wide string to a message expecting a narrow one. The W messages are therefore
// sent by hand.
void set_list_subitem(HWND list, int row, int column, const wchar_t* text) {
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = const_cast<wchar_t*>(text);
    SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
                 reinterpret_cast<LPARAM>(&item));
}

void fill_hotkey_list(HWND page, const ConfigDraft& draft) {
    HWND list = GetDlgItem(page, IDC_HK_LIST);
    SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
    for (int action = 0; action < kHkCount; ++action) {
        const auto name = hotkey_action_name(action);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = action;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        item.lParam = action;
        SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        set_list_subitem(list, action, 1, hotkey_display_name(draft.hotkeys[action]).c_str());
    }
}

void update_hotkey_row(HWND page, const ConfigDraft& draft, int action) {
    set_list_subitem(GetDlgItem(page, IDC_HK_LIST), action, 1,
                     hotkey_display_name(draft.hotkeys[action]).c_str());
}

int selected_hotkey(HWND page) {
    return static_cast<int>(SendMessageW(GetDlgItem(page, IDC_HK_LIST), LVM_GETNEXTITEM,
                                         static_cast<WPARAM>(-1), LVNI_SELECTED));
}

void start_hotkey_capture(HWND page) {
    const int action = selected_hotkey(page);
    if (action < 0 || capturing()) {
        return;
    }
    HWND list = GetDlgItem(page, IDC_HK_LIST);
    set_list_subitem(list, action, 1, L"press a key...");
    SetFocus(list);
    begin_capture(page);
}

INT_PTR CALLBACK hotkeys_page_proc(HWND page, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* draft = reinterpret_cast<ConfigDraft*>(GetWindowLongPtrW(page, GWLP_USERDATA));
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(page, GWLP_USERDATA, static_cast<LONG_PTR>(lparam));
            auto* init = reinterpret_cast<ConfigDraft*>(lparam);
            HWND list = GetDlgItem(page, IDC_HK_LIST);
            SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                         LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            // Widths from the list's own client area, less the vertical
            // scrollbar it will need, so the two columns fill it exactly rather
            // than leaving a dead strip on the right at whatever DPI is in use.
            RECT list_client{};
            GetClientRect(list, &list_client);
            const int usable = (list_client.right - list_client.left) -
                               GetSystemMetrics(SM_CXVSCROLL) - 4;
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.cx = usable * 3 / 5;
            column.pszText = const_cast<wchar_t*>(L"Action");
            column.iSubItem = 0;
            SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
            column.cx = usable - usable * 3 / 5;
            column.pszText = const_cast<wchar_t*>(L"Key");
            column.iSubItem = 1;
            SendMessageW(list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
            // A list view keeps its own colours and ignores WM_CTLCOLOR, so it
            // has to be told the theme directly or it stays a white panel.
            {
                const bool dark = init->state->dark_theme;
                const COLORREF back = dark ? RGB(30, 30, 34) : RGB(255, 255, 255);
                const COLORREF text = dark ? RGB(220, 220, 226) : RGB(30, 30, 34);
                SendMessageW(list, LVM_SETBKCOLOR, 0, static_cast<LPARAM>(back));
                SendMessageW(list, LVM_SETTEXTBKCOLOR, 0, static_cast<LPARAM>(back));
                SendMessageW(list, LVM_SETTEXTCOLOR, 0, static_cast<LPARAM>(text));
            }
            fill_hotkey_list(page, *init);
            return TRUE;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            if (draft != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_brush(*draft->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_APP_CAPTURE_DONE: {
            if (draft == nullptr) {
                break;
            }
            const UINT key = g_capture_key;
            end_capture();
            const int action = selected_hotkey(page);
            if (action < 0) {
                break;
            }
            if (key == VK_ESCAPE) {
                // Escape is the way out of a capture, so it cannot also be a
                // binding. Quit's default is Escape and stays that way; it just
                // cannot be re-assigned to Escape from here.
                update_hotkey_row(page, *draft, action);
                break;
            }
            const Hotkey chord{key, (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                               (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                               (GetKeyState(VK_MENU) & 0x8000) != 0};
            // One chord, one action. Taking it from whoever had it and saying so
            // is better than two actions quietly fighting over the same keys.
            for (int other = 0; other < kHkCount; ++other) {
                if (other != action && hotkey_same(draft->hotkeys[other], chord) &&
                    hotkey_bound(draft->hotkeys[other])) {
                    const auto message_text = hotkey_display_name(chord) + L" was " +
                                              hotkey_action_name(other) + L".\nIt is now " +
                                              hotkey_action_name(action) + L".";
                    MessageBoxW(page, message_text.c_str(), L"GenX", MB_OK | MB_ICONINFORMATION);
                    draft->hotkeys[other] = Hotkey{};
                    update_hotkey_row(page, *draft, other);
                }
            }
            draft->hotkeys[action] = chord;
            update_hotkey_row(page, *draft, action);
            break;
        }
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (header != nullptr && header->idFrom == IDC_HK_LIST &&
                header->code == NM_DBLCLK) {
                start_hotkey_capture(page);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            if (draft == nullptr) {
                break;
            }
            switch (LOWORD(wparam)) {
                case IDC_HK_ASSIGN:
                    start_hotkey_capture(page);
                    return TRUE;
                case IDC_HK_CLEAR: {
                    const int action = selected_hotkey(page);
                    if (action >= 0) {
                        draft->hotkeys[action] = Hotkey{};
                        update_hotkey_row(page, *draft, action);
                    }
                    return TRUE;
                }
                case IDC_HK_DEFAULTS:
                    set_default_hotkeys(draft->hotkeys);
                    fill_hotkey_list(page, *draft);
                    return TRUE;
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }
    return FALSE;
}

// --- Input page ------------------------------------------------------------

// Where each button sits on the artwork, as a fraction of the image. Measured
// off a percentage grid drawn over the actual PNGs, not estimated: a marker
// that is visibly off the button it names is worse than no marker at all.
// Buttons with no entry have no marker - a three-button pad has no X, Y or Z,
// and neither picture shows a Mode button. Re-measure whenever the pictures in
// res/ change (oracles/genx_art makes them); the two pads share one body, so
// the d-pad, Start, A, B and C are the same on both.
struct PadSpot {
    PadButton button;
    double x;
    double y;
};

constexpr PadSpot kThreeButtonSpots[] = {
    {PadButton::up, 0.235, 0.380},   {PadButton::down, 0.235, 0.580},
    {PadButton::left, 0.174, 0.480}, {PadButton::right, 0.296, 0.480},
    {PadButton::a, 0.680, 0.585},    {PadButton::b, 0.775, 0.500},
    {PadButton::c, 0.873, 0.446},    {PadButton::start, 0.489, 0.513},
};

constexpr PadSpot kSixButtonSpots[] = {
    {PadButton::up, 0.235, 0.380},   {PadButton::down, 0.235, 0.580},
    {PadButton::left, 0.174, 0.480}, {PadButton::right, 0.296, 0.480},
    {PadButton::start, 0.489, 0.513},
    {PadButton::x, 0.656, 0.387},    {PadButton::y, 0.731, 0.328},
    {PadButton::z, 0.805, 0.292},    {PadButton::a, 0.680, 0.587},
    {PadButton::b, 0.775, 0.500},    {PadButton::c, 0.873, 0.451},
};

bool pad_spot(PadKind kind, PadButton button, double& x, double& y) {
    const PadSpot* spots = nullptr;
    std::size_t count = 0;
    if (kind == PadKind::three_button) {
        spots = kThreeButtonSpots;
        count = std::size(kThreeButtonSpots);
    } else if (kind == PadKind::six_button) {
        spots = kSixButtonSpots;
        count = std::size(kSixButtonSpots);
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (spots[i].button == button) {
            x = spots[i].x;
            y = spots[i].y;
            return true;
        }
    }
    return false;
}

// The binding boxes sit in one column down the right of the picture area, with
// the artwork filling what is left. One column rather than two so a row is wide
// enough for a button name and a key name without either being clipped.
constexpr int kInputColumnWidth = 210;
constexpr int kInputRowGap = 2;

struct InputLayout {
    RECT art{};
    // Indexed by PadButton, so a row can be looked up without searching.
    RECT rows[kPadButtonCount]{};
    bool row_used[kPadButtonCount]{};
};

InputLayout input_layout(const RECT& client, PadKind kind, const PadArt& art) {
    InputLayout out{};
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const bool gun = kind_is_light_gun(kind);
    const int column = (std::min)(kInputColumnWidth, width / 2);
    const int art_width = (std::max)(40, width - column - 10);

    // Fit the artwork into its half, preserving its shape and centring it.
    int draw_w = art_width;
    int draw_h = height;
    if (art.width > 0 && art.height > 0) {
        const double scale =
            (std::min)(static_cast<double>(art_width) / art.width,
                       static_cast<double>(height) / art.height);
        draw_w = (std::max)(1, static_cast<int>(art.width * scale));
        draw_h = (std::max)(1, static_cast<int>(art.height * scale));
    }
    out.art.left = client.left + (art_width - draw_w) / 2;
    out.art.top = client.top + (height - draw_h) / 2;
    out.art.right = out.art.left + draw_w;
    out.art.bottom = out.art.top + draw_h;

    int rows = 0;
    for (const auto button : kPadButtonDisplayOrder) {
        if (kind_has_button(kind, button)) {
            ++rows;
        }
    }
    if (rows == 0) {
        return out;
    }
    // A pad's twelve rows fill the height; a gun's four would be stretched to
    // fill it too and look nothing like the pad beside it, so the row height is
    // capped and the short list sits at the top.
    const int row_height =
        (std::clamp)((height - (rows - 1) * kInputRowGap) / rows, 12, 26);
    int y = client.top;
    for (const auto button : kPadButtonDisplayOrder) {
        if (!kind_has_button(kind, button)) {
            continue;
        }
        const int index = static_cast<int>(button);
        out.row_used[index] = true;
        out.rows[index] = RECT{client.right - column, y, client.right, y + row_height};
        y += row_height + kInputRowGap;
    }
    return out;
}

// --- input profiles --------------------------------------------------------

std::filesystem::path profiles_directory() {
    auto directory = recent_file_path().parent_path() / L"profiles";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

std::vector<std::wstring> list_profiles() {
    std::vector<std::wstring> names;
    std::error_code failed;
    for (const auto& entry : std::filesystem::directory_iterator(profiles_directory(), failed)) {
        if (failed || !entry.is_regular_file() || entry.path().extension() != L".txt") {
            continue;
        }
        names.push_back(entry.path().stem().wstring());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// A profile is ONE player's bindings - not both players', and not the hotkeys
// or the paths, which are one machine's setup rather than one player's.
//
// It used to hold both players at once, which is why it read as a player 1
// feature: saving player 1's pad also wrote player 2's, and loading it replaced
// both. Now each player has their own list, so a controller can be set up once
// and given to whoever is holding it.
//
// Returns false if the file could not be written, so that a failure says so
// instead of looking like nothing happened.
bool save_profile(const std::wstring& name, const PlayerInput& input) {
    std::ofstream out(profiles_directory() / (name + L".txt"), std::ios::trunc);
    if (!out) {
        return false;
    }
    // Written under the pad1 prefix whichever player it came from, since the
    // file describes a player rather than a port.
    const std::string prefix = "pad1";
    out << prefix << "_kind=" << static_cast<int>(input.kind) << "\n";
    out << prefix << "_device=" << static_cast<int>(input.device) << "\n";
    out << prefix << "_slot=" << input.pad_slot << "\n";
    for (int button = 0; button < kPadButtonCount; ++button) {
        out << prefix << "_key" << button << "=" << input.keys[button] << "\n";
        out << prefix << "_btn" << button << "=" << input.pad[button] << "\n";
        out << prefix << "_dib" << button << "=" << input.dpad[button] << "\n";
    }
    return out.good();
}

bool load_profile(const std::wstring& name, PlayerInput& input) {
    std::ifstream in(profiles_directory() / (name + L".txt"));
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos || line.rfind("pad", 0) != 0) {
            continue;
        }
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        // A profile written by the old two-player format still loads: its pad1
        // half is taken and its pad2 half ignored, which is the right reading
        // of "this file describes a player".
        if (key[3] != '1') {
            continue;
        }
        if (key.find("_kind") != std::string::npos) {
            const int kind = std::atoi(value.c_str());
            if (kind >= 0 && kind <= static_cast<int>(PadKind::miracle_piano)) {
                input.kind = static_cast<PadKind>(kind);
            }
        } else if (key.find("_device") != std::string::npos) {
            const int device = std::atoi(value.c_str());
            if (device >= 0 && device <= static_cast<int>(InputDevice::mouse)) {
                input.device = static_cast<InputDevice>(device);
            }
        } else if (key.find("_slot") != std::string::npos) {
            const int slot = std::atoi(value.c_str());
            if (slot >= 0 && slot < 4) {
                input.pad_slot = slot;
            }
        } else if (key.find("_btn") != std::string::npos) {
            const int button = std::atoi(key.c_str() + key.find("_btn") + 4);
            if (button >= 0 && button < kPadButtonCount) {
                input.pad[button] = static_cast<PadInput>(std::strtoul(value.c_str(), nullptr, 10));
            }
        } else if (key.find("_dib") != std::string::npos) {
            const int button = std::atoi(key.c_str() + key.find("_dib") + 4);
            if (button >= 0 && button < kPadButtonCount) {
                input.dpad[button] = static_cast<PadInput>(std::strtoul(value.c_str(), nullptr, 10));
            }
        } else if (key.find("_key") != std::string::npos) {
            const int button = std::atoi(key.c_str() + key.find("_key") + 4);
            if (button >= 0 && button < kPadButtonCount) {
                input.keys[button] = static_cast<UINT>(std::atoi(value.c_str()));
            }
        }
    }
    // Same migration as the settings file: a profile saved before the gun rows
    // existed has no trigger in it.
    set_default_gun_bindings(input);
    return true;
}

// A one-line text prompt, for naming a profile.
struct NamePrompt {
    const RuntimeState* state = nullptr;
    std::wstring prompt;
    std::wstring value;
};

INT_PTR CALLBACK name_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* prompt = reinterpret_cast<NamePrompt*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, static_cast<LONG_PTR>(lparam));
            auto* init = reinterpret_cast<NamePrompt*>(lparam);
            SetDlgItemTextW(dialog, IDC_NAME_PROMPT, init->prompt.c_str());
            SetDlgItemTextW(dialog, IDC_NAME_EDIT, init->value.c_str());
            const BOOL dark = init->state->dark_theme ? TRUE : FALSE;
            if (FAILED(DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark)))) {
                DwmSetWindowAttribute(dialog, 19, &dark, sizeof(dark));
            }
            return TRUE;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
            if (prompt != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_brush(*prompt->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_CTLCOLOREDIT:
            if (prompt != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_field_brush(*prompt->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK && prompt != nullptr) {
                wchar_t text[128] = L"";
                GetDlgItemTextW(dialog, IDC_NAME_EDIT, text, 128);
                prompt->value = text;
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

bool prompt_for_name(HWND owner, const RuntimeState& state, const wchar_t* text,
                     std::wstring& value) {
    NamePrompt prompt;
    prompt.state = &state;
    prompt.prompt = text;
    prompt.value = value;
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_NAME), owner, name_proc,
                        reinterpret_cast<LPARAM>(&prompt)) != IDOK) {
        return false;
    }
    // A name becomes a filename, so the characters a filename cannot hold are
    // refused here rather than producing a profile that will not save.
    if (prompt.value.empty() ||
        prompt.value.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        MessageBoxW(owner, L"That name cannot be used as a file name.", L"GenX",
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    value = prompt.value;
    return true;
}

// --- the netplay dialog -----------------------------------------------------

struct NetplayPrompt {
    RuntimeState* state = nullptr;
    bool hosting = false;
    std::wstring nickname;
    std::wstring address;
    int port = 47215;
    std::wstring passphrase;
    int delay = 4;
    bool play_as_two = false;
};

INT_PTR CALLBACK netplay_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* prompt = reinterpret_cast<NetplayPrompt*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, static_cast<LONG_PTR>(lparam));
            auto* init = reinterpret_cast<NetplayPrompt*>(lparam);
            SetWindowTextW(dialog, init->hosting ? L"GenX - Start netplay" : L"GenX - Join netplay");
            SetDlgItemTextW(
                dialog, IDC_NET_HEADING,
                init->hosting
                    ? L"Others join you using your address, the port and the passphrase below. "
                      L"Share all three."
                    : L"Enter the address, port and passphrase the host gave you.");
            SetDlgItemTextW(dialog, IDC_NET_NICKNAME, init->nickname.c_str());
            SetDlgItemTextW(dialog, IDC_NET_ADDRESS, init->address.c_str());
            SetDlgItemInt(dialog, IDC_NET_PORT, static_cast<UINT>(init->port), FALSE);
            SetDlgItemTextW(dialog, IDC_NET_PASSPHRASE, init->passphrase.c_str());
            SetDlgItemInt(dialog, IDC_NET_DELAY, static_cast<UINT>(init->delay), FALSE);
            if (init->hosting) {
                // The three things the other player needs, in the one place the
                // host is already looking.
                std::wstring note = L"Your address: ";
                const auto addresses = mercury::net::local_addresses();
                if (addresses.empty()) {
                    note += L"(unknown - check your network settings)";
                } else {
                    for (std::size_t i = 0; i < addresses.size(); ++i) {
                        if (i != 0) {
                            note += L" or ";
                        }
                        note += widen(addresses[i]);
                    }
                }
                note += L"\nGive them that, the port and the passphrase. Over the internet the "
                        L"port must be forwarded to this machine.";
                SetDlgItemTextW(dialog, IDC_NET_NOTE, note.c_str());
            } else {
                SetDlgItemTextW(
                    dialog, IDC_NET_NOTE,
                    L"Input delay is in frames. Higher rides out a worse connection at the "
                    L"cost of feel; lower is sharper and stutters sooner.");
            }
            // The host has no address to type - it IS the address. And only the
            // host chooses the sides; the joiner is told at the handshake.
            ShowWindow(GetDlgItem(dialog, IDC_NET_ADDRESS), init->hosting ? SW_HIDE : SW_SHOW);
            ShowWindow(GetDlgItem(dialog, IDC_NET_ADDRESS_LABEL),
                       init->hosting ? SW_HIDE : SW_SHOW);
            ShowWindow(GetDlgItem(dialog, IDC_NET_PLAY_AS_TWO),
                       init->hosting ? SW_SHOW : SW_HIDE);
            CheckDlgButton(dialog, IDC_NET_PLAY_AS_TWO,
                           init->play_as_two ? BST_CHECKED : BST_UNCHECKED);
            const BOOL dark = init->state->dark_theme ? TRUE : FALSE;
            if (FAILED(DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark)))) {
                DwmSetWindowAttribute(dialog, 19, &dark, sizeof(dark));
            }
            return TRUE;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
            if (prompt != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_brush(*prompt->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_CTLCOLOREDIT:
            if (prompt != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_field_brush(*prompt->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK && prompt != nullptr) {
                wchar_t text[256] = L"";
                GetDlgItemTextW(dialog, IDC_NET_NICKNAME, text, 256);
                prompt->nickname = text;
                GetDlgItemTextW(dialog, IDC_NET_ADDRESS, text, 256);
                prompt->address = text;
                GetDlgItemTextW(dialog, IDC_NET_PASSPHRASE, text, 256);
                prompt->passphrase = text;
                BOOL ok = FALSE;
                const auto port = GetDlgItemInt(dialog, IDC_NET_PORT, &ok, FALSE);
                if (ok && port > 0 && port < 65536) {
                    prompt->port = static_cast<int>(port);
                }
                const auto delay = GetDlgItemInt(dialog, IDC_NET_DELAY, &ok, FALSE);
                if (ok && delay >= 1 && delay <= 20) {
                    prompt->delay = static_cast<int>(delay);
                }
                prompt->play_as_two =
                    IsDlgButtonChecked(dialog, IDC_NET_PLAY_AS_TWO) == BST_CHECKED;
                if (prompt->passphrase.empty()) {
                    MessageBoxW(dialog, L"A passphrase is required - it is what encrypts the "
                                        L"session.",
                                L"GenX", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                if (!prompt->hosting && prompt->address.empty()) {
                    MessageBoxW(dialog, L"Enter the host's address.", L"GenX",
                                MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

// The counterpart to widen(), which already exists further down. Nicknames and
// passphrases come from wide edit fields and go out as UTF-8 bytes.
std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr,
                                          nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

void end_netplay(RuntimeState& state);

void begin_netplay(RuntimeState& state, bool hosting) {
    // WHAT CAN GO OVER THE NETWORK, and why each refusal is what it is.
    //
    // This used to refuse everything that was not a Mega Drive cartridge, but
    // the machinery underneath already handled a Game Gear - the fingerprint,
    // the state transfer and the drift hash all branch on it - so the guard
    // was the only thing in the way of a linked pair.
    if (state.link_secondary) {
        MessageBoxW(state.window,
                    L"Start or join netplay from the first Gear Link window - that is the one "
                    L"that carries the session for both consoles.",
                    L"GenX", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!state.rom_loaded) {
        MessageBoxW(state.window, L"Load a cartridge first. Both players need the same one.",
                    L"GenX", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // A GAME GEAR IS ONE PERSON'S MACHINE. Two people play by linking two of
    // them, so that is what goes over the network: each player's console,
    // with the cable between them kept local at both ends. One Game Gear on
    // its own has only one pair of hands to give the other player.
    if (state.machine == Machine::game_gear && !gear_link_active()) {
        MessageBoxW(state.window,
                    L"Game Gear netplay plays a Gear Link: each of you gets your own "
                    L"console, joined by the cable.\n\n"
                    L"Both players: open Multiplayer > Gear Link and load the same game, then "
                    L"one of you starts netplay and the other joins. You will each see your own "
                    L"console.",
                    L"GenX", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // 32X and Sega CD games are allowed: since state version 5 the state the
    // host sends carries both, and the handshake compares the disc and the
    // BIOS as well as the cartridge (see netplay_hooks).
    if (state.netplay.active()) {
        MessageBoxW(state.window, L"A netplay session is already running.", L"GenX",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    NetplayPrompt prompt;
    prompt.state = &state;
    prompt.hosting = hosting;
    prompt.nickname = state.netplay_nickname;
    prompt.address = state.netplay_address;
    prompt.port = state.netplay_port;
    prompt.delay = state.netplay_delay;
    // A fresh one every time it is offered, so a session key is never reused
    // just because nobody thought to change it.
    prompt.passphrase = hosting ? widen(mercury::net::generate_passphrase()) : L"";

    state.in_modal_loop = true;
    const auto result = DialogBoxParamW(state.instance, MAKEINTRESOURCEW(IDD_NETPLAY),
                                        state.window, netplay_proc,
                                        reinterpret_cast<LPARAM>(&prompt));
    state.in_modal_loop = false;
    if (result != IDOK) {
        return;
    }
    state.netplay_nickname = prompt.nickname;
    state.netplay_address = prompt.address;
    state.netplay_port = prompt.port;
    state.netplay_delay = prompt.delay;
    // A session that failed earlier is still holding the host's save data
    // until it is ended properly; end it, which puts this player's own back.
    if (state.netplay.status() != mercury::net::Status::idle || g_netplay_joined) {
        end_netplay(state);
    }
    // Anything the game has saved and not yet written goes out now, while
    // the save data in the machine is still this player's own.
    flush_battery(state, false);
    if (RuntimeState* partner = link_partner(state); partner != nullptr) {
        flush_battery(*partner, false);
    }
    state.netplay_frame = 0;
    state.netplay.set_delay(prompt.delay);
    state.netplay.set_host_plays_second(prompt.play_as_two);
    // What this computer has plugged in. The host's is the session's; a
    // joiner's is sent only so both greetings have the same shape.
    state.netplay.set_seat_kinds(static_cast<std::uint8_t>(state.players[0].kind),
                                 static_cast<std::uint8_t>(state.players[1].kind));
    state.netplay_seats_applied = false;
    state.netplay_seat_inputs = {};

    std::string error;
    const bool started =
        hosting ? state.netplay.host(static_cast<std::uint16_t>(prompt.port),
                                     narrow(prompt.nickname), narrow(prompt.passphrase),
                                     netplay_hooks(state), error)
                : state.netplay.join(narrow(prompt.address),
                                     static_cast<std::uint16_t>(prompt.port),
                                     narrow(prompt.nickname), narrow(prompt.passphrase),
                                     netplay_hooks(state), error);
    if (!started) {
        MessageBoxW(state.window, widen(error).c_str(), L"GenX netplay", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!hosting) {
        g_netplay_joined = true;
    }
    // A linked pair switches to this player's own console once the session
    // knows which that is - see gear_link_follow_netplay.
    if (gear_link_active()) {
        gear_link_follow_netplay();
    }
    // No box to dismiss. The host's address is shown in the dialog itself,
    // where they were already reading the port and the passphrase, and a modal
    // box here also stopped the emulator from running - so the joiner sat
    // waiting until somebody clicked OK.
    refresh_netplay_status(state);
}

void end_netplay(RuntimeState& state) {
    if (!state.netplay.active() && state.netplay.status() == mercury::net::Status::idle) {
        return;
    }
    state.netplay.close();
    state.netplay_frame = 0;
    state.netplay_stalled = false;
    state.netplay_label.clear();
    // This computer's own controllers back, in place of the host's.
    if (state.netplay_seats_applied) {
        state.netplay_seats_applied = false;
        state.netplay_seat_inputs = {};
        if (state.machine == Machine::genesis) {
            apply_port_kinds(state);
        }
        clear_all_buttons(state);
    }
    // This player's own save data back in place of the host's. Nothing was
    // written while it was the host's, so the files still hold it.
    if (g_netplay_joined) {
        g_netplay_joined = false;
        restore_battery(state);
        if (RuntimeState* partner = link_partner(state); partner != nullptr) {
            restore_battery(*partner);
        }
    }
    update_title(state);
    set_status(state, L"netplay ended");
}

// --- the Input page proc ---------------------------------------------------

// Both players' lists show the same profiles - a saved controller setup belongs
// to whoever picks it, not to a fixed seat.
void fill_profile_combo(HWND page, const std::wstring& select, int select_player = -1) {
    const auto names = list_profiles();
    for (int player = 0; player < 2; ++player) {
        const int id = player == 0 ? IDC_IN_PROFILE : IDC_IN_P2_PROFILE;
        HWND combo = GetDlgItem(page, id);
        if (combo == nullptr) {
            continue;
        }
        // The selection only moves for the player who asked. Refilling after a
        // save used to be able to drag the other player onto a profile they had
        // not chosen.
        const bool mine = select_player < 0 || select_player == player;
        // What was showing, so refilling after a save does not silently move
        // the OTHER player onto a different profile.
        wchar_t previous[128] = L"";
        const auto was = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (was > 0) {
            SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(was),
                         reinterpret_cast<LPARAM>(previous));
        }
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(current)"));
        for (const auto& name : names) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        const std::wstring wanted =
            (mine && !select.empty()) ? select : std::wstring(previous);
        const auto found = wanted.empty()
                               ? CB_ERR
                               : SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                              reinterpret_cast<LPARAM>(wanted.c_str()));
        SendMessageW(combo, CB_SETCURSEL, found == CB_ERR ? 0 : static_cast<WPARAM>(found), 0);
    }
}

// A light gun is a mouse and nothing else, so the device dropdown is forced to
// Mouse and disabled rather than offering three devices that could never aim
// one. Switching back to a pad puts the choice back to the keyboard.
void sync_player_controls(HWND page, ConfigDraft& draft, int player) {
    const int kind_id = player == 0 ? IDC_IN_P1_KIND : IDC_IN_P2_KIND;
    const int device_id = player == 0 ? IDC_IN_P1_DEVICE : IDC_IN_P2_DEVICE;
    const int pic_id = player == 0 ? IDC_IN_P1_PIC : IDC_IN_P2_PIC;
    auto& input = draft.players[player];
    const bool gun = kind_is_light_gun(input.kind);
    // The keyboard is played in its own window with a pointer or a
    // fingertip, so none of the four devices describes it. Left as Keyboard
    // it reads as though it were being typed on, which is not what happens
    // and not something anyone could bind. Forced to Mouse and greyed out,
    // the same way a light gun is, because that is the truth about both.
    const bool piano = input.kind == PadKind::miracle_piano;
    if (gun || piano) {
        input.device = InputDevice::mouse;
    } else if (input.device == InputDevice::mouse) {
        input.device = InputDevice::keyboard;
    }
    SendDlgItemMessageW(page, kind_id, CB_SETCURSEL, static_cast<WPARAM>(input.kind), 0);
    SendDlgItemMessageW(page, device_id, CB_SETCURSEL, static_cast<WPARAM>(input.device), 0);
    EnableWindow(GetDlgItem(page, device_id), (gun || piano) ? FALSE : TRUE);
    InvalidateRect(GetDlgItem(page, pic_id), nullptr, TRUE);
}

// The picture areas are ordinary statics with a subclass on top: SS_NOTIFY
// gives a click with no coordinates and no hover at all, and both are needed to
// know which box the pointer is over.
WNDPROC g_pad_pic_original = nullptr;

int pad_pic_player(HWND control) {
    return GetDlgCtrlID(control) == IDC_IN_P2_PIC ? 1 : 0;
}

int pad_pic_hit(HWND control, const ConfigDraft& draft, POINT at) {
    RECT client{};
    GetClientRect(control, &client);
    const int player = pad_pic_player(control);
    const auto kind = draft.players[player].kind;
    const auto& art = pad_art(draft.state->instance, kind);
    const auto layout = input_layout(client, kind, art);
    for (int index = 0; index < kPadButtonCount; ++index) {
        if (layout.row_used[index] && PtInRect(&layout.rows[index], at)) {
            return index;
        }
    }
    return -1;
}

// A mouse button pressed while a row is waiting for an input IS the input,
// rather than a cancel. That is the whole point of it: a Wii remote in a gun
// holder arrives as a mouse, its trigger is the B button underneath, and a
// Dolphin Bar sends that as a RIGHT click - so a gun whose trigger is welded to
// the left button is a gun that cannot be held properly. Escape still cancels.
bool capture_mouse_button(ConfigDraft& draft, HWND control, UINT key) {
    if (!capturing() || draft.capture_row < 0) {
        return false;
    }
    const int player = draft.capture_player;
    if (player < 0 || player > 1) {
        return false;
    }
    end_capture();
    // Into keys[], because a mouse button has a virtual-key code like any other
    // and so needs no storage of its own.
    draft.players[player].keys[draft.capture_row] = key;
    draft.capture_player = -1;
    draft.capture_row = -1;
    InvalidateRect(control, nullptr, FALSE);
    return true;
}

LRESULT CALLBACK pad_pic_proc(HWND control, UINT message, WPARAM wparam, LPARAM lparam) {
    HWND page = GetParent(control);
    auto* draft = reinterpret_cast<ConfigDraft*>(GetWindowLongPtrW(page, GWLP_USERDATA));
    if (draft == nullptr) {
        return CallWindowProcW(g_pad_pic_original, control, message, wparam, lparam);
    }
    const int player = pad_pic_player(control);
    switch (message) {
        case WM_MOUSEMOVE: {
            const POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int row = pad_pic_hit(control, *draft, at);
            if (draft->hover_player != player || draft->hover_row != row) {
                // The other panel only needs repainting if IT was the one
                // showing a highlight. Repainting both on every mouse move was
                // twice the work for nothing.
                const bool other_was_hovered = draft->hover_player == (1 - player);
                draft->hover_player = row >= 0 ? player : -1;
                draft->hover_row = row;
                InvalidateRect(control, nullptr, FALSE);
                if (other_was_hovered) {
                    InvalidateRect(GetDlgItem(page, player == 0 ? IDC_IN_P2_PIC : IDC_IN_P1_PIC),
                                   nullptr, FALSE);
                }
            }
            TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, control, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (draft->hover_player == player) {
                draft->hover_player = -1;
                draft->hover_row = -1;
                InvalidateRect(control, nullptr, FALSE);
            }
            return 0;
        case WM_MBUTTONDOWN:
            capture_mouse_button(*draft, control, VK_MBUTTON);
            return 0;
        case WM_LBUTTONDOWN: {
            if (capture_mouse_button(*draft, control, VK_LBUTTON)) {
                return 0;
            }
            if (draft->capture_from_pad) {
                // A click while waiting for a PAD input means "never mind" -
                // there is no mouse button to bind on a gamepad.
                KillTimer(page, kPadCaptureTimer);
                draft->capture_from_pad = false;
                draft->capture_player = -1;
                draft->capture_row = -1;
                InvalidateRect(control, nullptr, FALSE);
                return 0;
            }
            const POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int row = pad_pic_hit(control, *draft, at);
            if (row >= 0) {
                draft->capture_player = player;
                draft->capture_row = row;
                draft->capture_from_pad = device_is_gamepad(draft->players[player].device);
                InvalidateRect(control, nullptr, FALSE);
                if (draft->capture_from_pad) {
                    bool connected = false;
                    draft->capture_baseline =
                        read_gamepad(draft->players[player].pad_slot, connected);
                    draft->capture_di_baseline = DIJOYSTATE2{};
                    read_dinput(draft->players[player].pad_slot, draft->capture_di_baseline);
                    SetTimer(page, kPadCaptureTimer, 16, nullptr);
                } else {
                    begin_capture(page);
                }
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (capture_mouse_button(*draft, control, VK_RBUTTON)) {
                return 0;
            }
            const POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int row = pad_pic_hit(control, *draft, at);
            if (row >= 0) {
                if (draft->players[player].device == InputDevice::dinput) {
                    draft->players[player].dpad[row] = kPadInputNone;
                } else if (draft->players[player].device == InputDevice::xinput) {
                    draft->players[player].pad[row] = kPadInputNone;
                } else {
                    draft->players[player].keys[row] = 0;
                }
                InvalidateRect(control, nullptr, FALSE);
            }
            return 0;
        }
        default:
            break;
    }
    return CallWindowProcW(g_pad_pic_original, control, message, wparam, lparam);
}

void draw_pad_picture(const ConfigDraft& draft, DRAWITEMSTRUCT* item) {
    const int player = item->CtlID == IDC_IN_P2_PIC ? 1 : 0;
    const auto& input = draft.players[player];
    const auto& art = pad_art(draft.state->instance, input.kind);
    const Theme& theme = theme_of(*draft.state);
    const bool dark = draft.state->dark_theme;
    RECT client = item->rcItem;

    // Everything below goes into an off-screen bitmap and reaches the screen as
    // one blit. Drawing straight to the control meant every mouse move repainted
    // the panel in stages - fill, then artwork, then twelve boxes - and the
    // display caught it mid-sequence, which is the flicker.
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC screen = item->hDC;
    HDC buffer = CreateCompatibleDC(screen);
    HBITMAP canvas = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old_canvas = SelectObject(buffer, canvas);
    // So the drawing below can keep using the control's own coordinates even
    // though the bitmap starts at zero.
    SetWindowOrgEx(buffer, client.left, client.top, nullptr);
    HDC dc = buffer;

    HBRUSH background = CreateSolidBrush(theme.background);
    FillRect(dc, &client, background);
    DeleteObject(background);

    const auto layout = input_layout(client, input.kind, art);

    if (art.bitmap != nullptr) {
        HDC memory = CreateCompatibleDC(dc);
        HGDIOBJ previous = SelectObject(memory, art.bitmap);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(dc, layout.art.left, layout.art.top, layout.art.right - layout.art.left,
                   layout.art.bottom - layout.art.top, memory, 0, 0, art.width, art.height,
                   blend);
        SelectObject(memory, previous);
        DeleteDC(memory);
    }

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dc, menu_font());

    const COLORREF box_fill = dark ? RGB(58, 58, 64) : RGB(246, 246, 248);
    const COLORREF box_edge = dark ? RGB(96, 96, 106) : RGB(170, 170, 178);
    const COLORREF live_edge = RGB(96, 160, 230);
    const COLORREF dim_text = dark ? RGB(150, 150, 158) : RGB(120, 120, 128);

    // No picture of this controller in the build - they are optional, see
    // CMakeLists.txt. A labelled outline holds its place. The bottom inset
    // leaves room for the connected/not found line drawn under the artwork.
    if (art.bitmap == nullptr) {
        RECT frame = layout.art;
        frame.left += 10;
        frame.top += 10;
        frame.right -= 10;
        frame.bottom -= 22;
        HPEN outline = CreatePen(PS_SOLID, 1, box_edge);
        HGDIOBJ old_outline = SelectObject(dc, outline);
        HGDIOBJ old_fill = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, frame.left, frame.top, frame.right, frame.bottom, 18, 18);
        SelectObject(dc, old_fill);
        SelectObject(dc, old_outline);
        DeleteObject(outline);
        SetTextColor(dc, dim_text);
        DrawTextW(dc, pad_kind_name(input.kind), -1, &frame,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // Whether the chosen controller is actually there, said plainly under the
    // artwork. XInput reports no difference between a pad on a cable and one on
    // Bluetooth, so the wording does not claim to know which it is.
    if (device_is_gamepad(input.device)) {
        RECT status = client;
        status.left = layout.art.left;
        status.right = layout.art.right;
        status.top = layout.art.bottom - 14;
        const bool present = draft.pad_present[player];
        SetTextColor(dc, present ? theme.text : dim_text);
        const std::wstring text =
            present ? (L"Controller " + std::to_wstring(input.pad_slot + 1) + L": connected")
                    : (L"Controller " + std::to_wstring(input.pad_slot + 1) + L": not found");
        DrawTextW(dc, text.c_str(), -1, &status, DT_CENTER | DT_SINGLELINE);
    }

    // The aim is the mouse and is not up for negotiation, but the buttons are.
    if (kind_is_light_gun(input.kind)) {
        RECT note = client;
        note.left = client.right -
                    (std::min)(kInputColumnWidth, static_cast<int>(client.right - client.left) / 2);
        note.top = client.top;
        // Below whatever rows this gun has.
        for (int index = 0; index < kPadButtonCount; ++index) {
            if (layout.row_used[index] && layout.rows[index].bottom + 4 > note.top) {
                note.top = layout.rows[index].bottom + 4;
            }
        }
        SetTextColor(dc, dim_text);
        DrawTextW(dc,
                  input.kind == PadKind::menacer
                      ? L"Aimed with the mouse; Trigger and Reload bind to any mouse button "
                        L"or key. Always wired to port 2, where the real receiver box plugs "
                        L"in - port 1 keeps a pad for the menus. The pointer is hidden: "
                        L"every Menacer game draws its own crosshair."
                      : L"Aimed with the mouse; Trigger and Reload bind to any mouse button "
                        L"or key. Both guns share port 2, as the real pair does. Set BOTH "
                        L"players to Justifier for two-player: each gets its own mouse, "
                        L"claimed in the order they are first moved, and Emulation > "
                        L"Re-pair light guns starts that over.",
                  -1, &note, DT_LEFT | DT_WORDBREAK);
    }

    for (int index = 0; index < kPadButtonCount; ++index) {
        if (!layout.row_used[index]) {
            continue;
        }
        const auto button = static_cast<PadButton>(index);
        const bool hovered = draft.hover_player == player && draft.hover_row == index;
        const bool waiting = draft.capture_player == player && draft.capture_row == index;
        const bool live = hovered || waiting;

        HBRUSH fill = CreateSolidBrush(box_fill);
        HPEN pen = CreatePen(PS_SOLID, 1, live ? live_edge : box_edge);
        HGDIOBJ old_brush = SelectObject(dc, fill);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        const RECT& row = layout.rows[index];
        Rectangle(dc, row.left, row.top, row.right, row.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(fill);
        DeleteObject(pen);

        RECT text = row;
        text.left += 6;
        text.right -= 6;
        SetTextColor(dc, theme.text);
        DrawTextW(dc, pad_button_name(button), -1, &text,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        const bool on_pad = device_is_gamepad(input.device);
        const bool on_di = input.device == InputDevice::dinput;
        const PadInput binding = on_di ? input.dpad[index] : input.pad[index];
        const bool bound = on_pad ? binding != kPadInputNone : input.keys[index] != 0;
        const auto name = waiting ? std::wstring(on_pad ? L"press a button..." : L"press a key...")
                          : !on_pad ? key_display_name(input.keys[index])
                          : on_di   ? dinput_input_name(binding)
                                    : pad_input_name(binding);
        SetTextColor(dc, waiting ? live_edge : (!bound ? dim_text : theme.text));
        DrawTextW(dc, name.c_str(), -1, &text, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

        // One leader line at a time, to whichever row the pointer is on. Drawing
        // all twelve at once would be a cat's cradle across the pad. Not with
        // the outline stand-in: the spots are places on the picture.
        double fx = 0.0;
        double fy = 0.0;
        if (live && art.bitmap != nullptr && pad_spot(input.kind, button, fx, fy)) {
            const int spot_x =
                layout.art.left + static_cast<int>((layout.art.right - layout.art.left) * fx);
            const int spot_y =
                layout.art.top + static_cast<int>((layout.art.bottom - layout.art.top) * fy);
            HPEN line = CreatePen(PS_SOLID, 1, live_edge);
            HGDIOBJ old_line = SelectObject(dc, line);
            MoveToEx(dc, row.left, (row.top + row.bottom) / 2, nullptr);
            LineTo(dc, spot_x, spot_y);
            HGDIOBJ hollow = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, spot_x - 7, spot_y - 7, spot_x + 7, spot_y + 7);
            SelectObject(dc, hollow);
            SelectObject(dc, old_line);
            DeleteObject(line);
        }
    }
    SelectObject(dc, old_font);

    BitBlt(screen, client.left, client.top, width, height, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, old_canvas);
    DeleteObject(canvas);
    DeleteDC(buffer);
}

INT_PTR CALLBACK input_page_proc(HWND page, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* draft = reinterpret_cast<ConfigDraft*>(GetWindowLongPtrW(page, GWLP_USERDATA));
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(page, GWLP_USERDATA, static_cast<LONG_PTR>(lparam));
            auto* init = reinterpret_cast<ConfigDraft*>(lparam);
            // Named after the driver model, which is the thing that actually
            // differs. Whether a pad is on a cable or on Bluetooth changes
            // nothing about how it is read.
            static const wchar_t* devices[] = {L"Keyboard", L"XInput", L"DirectInput", L"Mouse"};
            for (const int id : {IDC_IN_P1_KIND, IDC_IN_P2_KIND}) {
                // In PadKind order: the selection index IS the kind.
                for (const auto kind : {PadKind::three_button, PadKind::six_button,
                                        PadKind::justifier, PadKind::menacer,
                                        PadKind::miracle_piano}) {
                    SendDlgItemMessageW(page, id, CB_ADDSTRING, 0,
                                        reinterpret_cast<LPARAM>(pad_kind_name(kind)));
                }
            }
            for (const int id : {IDC_IN_P1_DEVICE, IDC_IN_P2_DEVICE}) {
                for (const auto* name : devices) {
                    SendDlgItemMessageW(page, id, CB_ADDSTRING, 0,
                                        reinterpret_cast<LPARAM>(name));
                }
            }
            for (const int id : {IDC_IN_P1_PIC, IDC_IN_P2_PIC}) {
                HWND pic = GetDlgItem(page, id);
                g_pad_pic_original = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(pic, GWLP_WNDPROC,
                                      reinterpret_cast<LONG_PTR>(pad_pic_proc)));
            }
            // What player two actually does, which depends on the machine and
            // on the cartridge. Worth saying here rather than leaving someone
            // to bind a second pad and wonder why nothing happens.
            {
                // One line each. The box is a single line high, and a string
                // that wraps is a string whose second half is invisible.
                const wchar_t* note =
                    L"Player 2 is the Mega Drive's second controller port.";
                if (init->state != nullptr && init->state->machine == Machine::game_gear) {
                    note = shared_pad_game(*init->state)
                               ? L"For Micro Machines games, player 1 steers on the d-pad and "
                                 L"player 2 drives with A and B. Active now."
                               : L"For Micro Machines games, player 1 steers on the d-pad and "
                                 L"player 2 drives with A and B.";
                }
                SetDlgItemTextW(page, IDC_IN_P2_NOTE, note);
            }
            // Each player's dropdown opens on the profile they last picked, not
            // on "(current)" every time - the user's report, 2026-10-03: P1
            // chosen, OK pressed, and the dialog reopened saying (current).
            // A remembered profile whose file has since gone falls back to
            // (current) and is forgotten.
            for (int player = 0; player < 2; ++player) {
                fill_profile_combo(page, init->profile_names[player], player);
                const int combo_id = player == 0 ? IDC_IN_PROFILE : IDC_IN_P2_PROFILE;
                if (SendDlgItemMessageW(page, combo_id, CB_GETCURSEL, 0, 0) <= 0) {
                    init->profile_names[player].clear();
                }
            }
            refresh_pad_presence(*init);
            // Half a second is often enough to notice a pad being plugged in
            // while the dialog is open, and rare enough that polling an empty
            // slot costs nothing worth measuring.
            SetTimer(page, kPadStatusTimer, 500, nullptr);
            sync_player_controls(page, *init, 0);
            sync_player_controls(page, *init, 1);
            return TRUE;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            if (draft != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_brush(*draft->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (draft != nullptr && item != nullptr &&
                (item->CtlID == IDC_IN_P1_PIC || item->CtlID == IDC_IN_P2_PIC)) {
                draw_pad_picture(*draft, item);
                return TRUE;
            }
            break;
        }
        case WM_TIMER: {
            if (draft == nullptr) {
                break;
            }
            if (wparam == kPadStatusTimer) {
                const auto before = draft->pad_present;
                refresh_pad_presence(*draft);
                if (before != draft->pad_present) {
                    InvalidateRect(GetDlgItem(page, IDC_IN_P1_PIC), nullptr, FALSE);
                    InvalidateRect(GetDlgItem(page, IDC_IN_P2_PIC), nullptr, FALSE);
                }
                return TRUE;
            }
            if (wparam != kPadCaptureTimer || !draft->capture_from_pad) {
                break;
            }
            const int player = draft->capture_player;
            const int row = draft->capture_row;
            if (player < 0 || row < 0) {
                KillTimer(page, kPadCaptureTimer);
                draft->capture_from_pad = false;
                break;
            }
            const bool on_di = draft->players[player].device == InputDevice::dinput;
            PadInput chosen = kPadInputNone;
            if (on_di) {
                DIJOYSTATE2 now{};
                if (!read_dinput(draft->players[player].pad_slot, now)) {
                    return TRUE;
                }
                chosen = first_dinput_press(now, draft->capture_di_baseline);
                if (chosen == kPadInputNone) {
                    // Anything released since the click becomes bindable.
                    draft->capture_di_baseline = now;
                    return TRUE;
                }
            } else {
                bool connected = false;
                const PadInput now = read_gamepad(draft->players[player].pad_slot, connected);
                // Only inputs that were released when the box was clicked count,
                // so a resting trigger or a held button cannot bind itself.
                const PadInput fresh = now & ~draft->capture_baseline;
                if (fresh == kPadInputNone) {
                    // Anything let go of since is fair game from now on.
                    draft->capture_baseline &= now;
                    return TRUE;
                }
                // The lowest set bit, so one press binds one thing even if the
                // pad reports two at the same instant.
                chosen = fresh & (~fresh + 1u);
            }
            KillTimer(page, kPadCaptureTimer);
            draft->capture_from_pad = false;
            draft->capture_player = -1;
            draft->capture_row = -1;
            // One input, one button, on this player's pad.
            auto& bindings = on_di ? draft->players[player].dpad : draft->players[player].pad;
            for (int index = 0; index < kPadButtonCount; ++index) {
                if (index != row && bindings[index] == chosen) {
                    bindings[index] = kPadInputNone;
                }
            }
            bindings[row] = chosen;
            InvalidateRect(GetDlgItem(page, IDC_IN_P1_PIC), nullptr, FALSE);
            InvalidateRect(GetDlgItem(page, IDC_IN_P2_PIC), nullptr, FALSE);
            return TRUE;
        }
        case WM_APP_CAPTURE_DONE: {
            if (draft == nullptr) {
                break;
            }
            const UINT key = g_capture_key;
            end_capture();
            const int player = draft->capture_player;
            const int row = draft->capture_row;
            draft->capture_player = -1;
            draft->capture_row = -1;
            if (player >= 0 && row >= 0 && key != VK_ESCAPE) {
                // A key already used by the OTHER player's pad is taken away
                // from it: two pads on one key would both fire, which is never
                // what was meant.
                const int other = 1 - player;
                for (int index = 0; index < kPadButtonCount; ++index) {
                    if (draft->players[other].keys[index] == key) {
                        draft->players[other].keys[index] = 0;
                    }
                    if (index != row && draft->players[player].keys[index] == key) {
                        draft->players[player].keys[index] = 0;
                    }
                }
                draft->players[player].keys[row] = key;
            }
            InvalidateRect(GetDlgItem(page, IDC_IN_P1_PIC), nullptr, FALSE);
            InvalidateRect(GetDlgItem(page, IDC_IN_P2_PIC), nullptr, FALSE);
            break;
        }
        case WM_COMMAND: {
            if (draft == nullptr) {
                break;
            }
            const int control = LOWORD(wparam);
            const int notification = HIWORD(wparam);
            if (notification == CBN_SELCHANGE &&
                (control == IDC_IN_P1_KIND || control == IDC_IN_P2_KIND)) {
                const int player = control == IDC_IN_P2_KIND ? 1 : 0;
                const auto chosen = SendDlgItemMessageW(page, control, CB_GETCURSEL, 0, 0);
                if (chosen != CB_ERR) {
                    draft->players[player].kind = static_cast<PadKind>(chosen);
                    sync_player_controls(page, *draft, player);
                }
                return TRUE;
            }
            if (notification == CBN_SELCHANGE &&
                (control == IDC_IN_P1_DEVICE || control == IDC_IN_P2_DEVICE)) {
                const int player = control == IDC_IN_P2_DEVICE ? 1 : 0;
                const auto chosen = SendDlgItemMessageW(page, control, CB_GETCURSEL, 0, 0);
                if (chosen != CB_ERR) {
                    draft->players[player].device = static_cast<InputDevice>(chosen);
                    // Whether a pad is actually there is shown in the panel
                    // rather than in a message box: a modal interruption every
                    // time the dropdown changes is worse than a line of text
                    // that is always visible and updates when one is plugged in.
                    refresh_pad_presence(*draft);
                    sync_player_controls(page, *draft, player);
                }
                return TRUE;
            }
            // Both players' profile rows, which differ only by which player
            // they read and write.
            for (int player = 0; player < 2; ++player) {
                const int combo_id = player == 0 ? IDC_IN_PROFILE : IDC_IN_P2_PROFILE;
                const int save_id = player == 0 ? IDC_IN_PROFILE_SAVE : IDC_IN_P2_PROFILE_SAVE;
                const int delete_id =
                    player == 0 ? IDC_IN_PROFILE_DELETE : IDC_IN_P2_PROFILE_DELETE;
                const auto selected_name = [&]() {
                    const auto chosen = SendDlgItemMessageW(page, combo_id, CB_GETCURSEL, 0, 0);
                    wchar_t name[128] = L"";
                    if (chosen > 0) {
                        SendDlgItemMessageW(page, combo_id, CB_GETLBTEXT,
                                            static_cast<WPARAM>(chosen),
                                            reinterpret_cast<LPARAM>(name));
                    }
                    return std::wstring(name);
                };
                if (notification == CBN_SELCHANGE && control == combo_id) {
                    const auto name = selected_name();
                    if (!name.empty()) {
                        load_profile(name, draft->players[player]);
                        sync_player_controls(page, *draft, player);
                    }
                    // Remembered for next time, (current) included.
                    draft->profile_names[player] = name;
                    return TRUE;
                }
                if (control == save_id && notification == BN_CLICKED) {
                    std::wstring name;
                    if (prompt_for_name(page, *draft->state,
                                        player == 0 ? L"Name for player 1's controller setup:"
                                                    : L"Name for player 2's controller setup:",
                                        name)) {
                        // Say so on failure. This used to return silently, so a
                        // profile that did not save looked exactly like one
                        // that did.
                        if (save_profile(name, draft->players[player])) {
                            fill_profile_combo(page, name, player);
                            draft->profile_names[player] = name;
                        } else {
                            MessageBoxW(page, L"That profile could not be saved.", L"GenX",
                                        MB_OK | MB_ICONWARNING);
                        }
                    }
                    return TRUE;
                }
                if (control == delete_id && notification == BN_CLICKED) {
                    const auto name = selected_name();
                    if (!name.empty()) {
                        std::error_code ignored;
                        std::filesystem::remove(profiles_directory() / (name + L".txt"), ignored);
                        fill_profile_combo(page, L"(current)", player);
                        draft->profile_names[player].clear();
                        // The other player may have had the same one selected;
                        // its dropdown has just lost it too.
                        const int other = 1 - player;
                        const int other_combo = other == 0 ? IDC_IN_PROFILE : IDC_IN_P2_PROFILE;
                        if (SendDlgItemMessageW(page, other_combo, CB_GETCURSEL, 0, 0) <= 0) {
                            draft->profile_names[other].clear();
                        }
                    }
                    return TRUE;
                }
            }
            break;
        }
        default:
            break;
    }
    return FALSE;
}

// --- the frame -------------------------------------------------------------

INT_PTR CALLBACK configure_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* draft = reinterpret_cast<ConfigDraft*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, static_cast<LONG_PTR>(lparam));
            auto* init = reinterpret_cast<ConfigDraft*>(lparam);
            HWND tabs = GetDlgItem(dialog, IDC_CONFIG_TABS);
            const wchar_t* titles[kConfigPageCount] = {L"General", L"Input", L"Hotkeys",
                                                       L"BIOS"};
            for (int i = 0; i < kConfigPageCount; ++i) {
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = const_cast<wchar_t*>(titles[i]);
                // TCM_INSERTITEMW by hand: the TabCtrl_ macro sends the ANSI
                // message, which would read this wide string as bytes.
                SendMessageW(tabs, TCM_INSERTITEMW, static_cast<WPARAM>(i),
                             reinterpret_cast<LPARAM>(&item));
            }
            const int templates[kConfigPageCount] = {IDD_PAGE_GENERAL, IDD_PAGE_INPUT,
                                                     IDD_PAGE_HOTKEYS, IDD_PAGE_BIOS};
            DLGPROC procs[kConfigPageCount] = {general_page_proc, input_page_proc,
                                               hotkeys_page_proc, bios_page_proc};
            // The display area is whatever the tab strip leaves; asking the tab
            // control for it is the only way to place a page that stays right
            // when the strip's height changes with the system font.
            RECT area{};
            GetWindowRect(tabs, &area);
            MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&area), 2);
            TabCtrl_AdjustRect(tabs, FALSE, &area);
            for (int i = 0; i < kConfigPageCount; ++i) {
                init->pages[i] = CreateDialogParamW(GetModuleHandleW(nullptr),
                                                    MAKEINTRESOURCEW(templates[i]), dialog,
                                                    procs[i], reinterpret_cast<LPARAM>(init));
                if (init->pages[i] == nullptr) {
                    continue;
                }
                SetWindowPos(init->pages[i], HWND_TOP, area.left, area.top,
                             area.right - area.left, area.bottom - area.top, SWP_NOACTIVATE);
                ShowWindow(init->pages[i], i == 0 ? SW_SHOW : SW_HIDE);
            }
            init->current_page = 0;
            g_config_tab_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                tabs, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(config_tab_proc)));
            const BOOL dark = init->state->dark_theme ? TRUE : FALSE;
            if (FAILED(DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark)))) {
                DwmSetWindowAttribute(dialog, 19, &dark, sizeof(dark));
            }
            return TRUE;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            if (draft != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_brush(*draft->state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (draft == nullptr || item == nullptr || item->CtlType != ODT_TAB) {
                break;
            }
            const Theme& theme = theme_of(*draft->state);
            const bool dark = draft->state->dark_theme;
            const bool selected = (item->itemState & ODS_SELECTED) != 0;
            const COLORREF fill =
                selected ? theme.background : (dark ? RGB(34, 34, 38) : RGB(232, 232, 236));
            HBRUSH brush = CreateSolidBrush(fill);
            FillRect(item->hDC, &item->rcItem, brush);
            DeleteObject(brush);
            wchar_t label[32] = L"";
            TCITEMW query{};
            query.mask = TCIF_TEXT;
            query.pszText = label;
            query.cchTextMax = 32;
            SendMessageW(item->hwndItem, TCM_GETITEMW, item->itemID,
                         reinterpret_cast<LPARAM>(&query));
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, theme.text);
            HGDIOBJ old_font = SelectObject(item->hDC, menu_font());
            RECT text = item->rcItem;
            DrawTextW(item->hDC, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(item->hDC, old_font);
            return TRUE;
        }
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (draft != nullptr && header != nullptr && header->idFrom == IDC_CONFIG_TABS &&
                header->code == TCN_SELCHANGE) {
                const int chosen = TabCtrl_GetCurSel(GetDlgItem(dialog, IDC_CONFIG_TABS));
                for (int i = 0; i < kConfigPageCount; ++i) {
                    if (draft->pages[i] != nullptr) {
                        ShowWindow(draft->pages[i], i == chosen ? SW_SHOW : SW_HIDE);
                    }
                }
                draft->current_page = chosen;
                return TRUE;
            }
            break;
        }
        case WM_COMMAND:
            if (draft == nullptr) {
                break;
            }
            // A capture in progress swallows every key, so the only way OK or
            // Cancel can arrive is a mouse click. Take the click as cancelling
            // the capture rather than acting on a half-finished binding.
            if (capturing()) {
                end_capture();
                if (draft->pages[2] != nullptr) {
                    fill_hotkey_list(draft->pages[2], *draft);
                }
                return TRUE;
            }
            switch (LOWORD(wparam)) {
                case IDOK: {
                    commit_general_page(draft->pages[0], *draft);
                    commit_bios_page(draft->pages[3], *draft);
                    RuntimeState& state = *draft->state;
                    state.hide_cursor = draft->hide_cursor;
                    state.check_for_updates = draft->check_for_updates;
                    state.volume = draft->volume;
                    state.muted = draft->muted;
                    state.console_filter = draft->console_filter;
                    state.games_folders = draft->games_folders;
                    state.save_folders = draft->save_folders;
                    state.bios_folder_32x = draft->bios_folder_32x;
                    state.bios_folder_segacd = draft->bios_folder_segacd;
                    apply_bios_folders(state);
                    // A player who picked a different pad (or a different kind of
                    // pad) in the dialog chose THAT pad: forget the one being
                    // followed by name, or the poll would follow it straight back.
                    for (int player = 0; player < 2; ++player) {
                        if (draft->players[player].device != state.players[player].device ||
                            draft->players[player].pad_slot != state.players[player].pad_slot) {
                            state.dinput_pad_name[player].clear();
                        }
                    }
                    state.players = draft->players;
                    state.profile_names = draft->profile_names;
                    state.hotkeys = draft->hotkeys;
                    // Whatever the players were, they may not be that now, and
                    // a mouse claimed by a gun that no longer exists would keep
                    // its slot. Pairing starts again from the next movement.
                    forget_gun_devices(state);
                    if (state.rom_loaded && state.machine == Machine::genesis) {
                        apply_port_kinds(state);
                    }
                    save_settings(state);
                    refresh_menu_hotkeys(state);
                    // Any binding that was down when the dialog opened is not
                    // down any more as far as the user is concerned.
                    clear_all_buttons(state);
                    if (state.window != nullptr) {
                        // Take the cursor setting now rather than on the next
                        // mouse move.
                        POINT at{};
                        GetCursorPos(&at);
                        SetCursorPos(at.x, at.y);
                    }
                    EndDialog(dialog, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    // Volume and the filter were applied live; put them back.
                    draft->state->volume = draft->original_volume;
                    draft->state->muted = draft->original_muted;
                    draft->state->console_filter = draft->original_console_filter;
                    EndDialog(dialog, IDCANCEL);
                    return TRUE;
                default:
                    break;
            }
            break;
        case WM_DESTROY:
            end_capture();
            // The pages are children of this dialog and go with it; their
            // timers have to be stopped explicitly.
            if (draft != nullptr && draft->pages[1] != nullptr) {
                KillTimer(draft->pages[1], kPadStatusTimer);
                KillTimer(draft->pages[1], kPadCaptureTimer);
            }
            break;
        default:
            break;
    }
    return FALSE;
}

// Borderless on the monitor the window is currently on - not a mode change.
// Changing the display mode risks leaving the desktop rearranged if the process
// dies, and gains nothing when the picture is being scaled anyway.
void set_fullscreen(RuntimeState& state, bool on) {
    HWND hwnd = state.window;
    if (hwnd == nullptr || state.fullscreen == on) {
        return;
    }
    if (on) {
        state.windowed_placement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hwnd, &state.windowed_placement);
        state.windowed_style = GetWindowLongPtrW(hwnd, GWL_STYLE);

        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        // The menu bar goes with it: a menu strip across a fullscreen picture is
        // not what anyone means by fullscreen. Alt still opens it.
        SetMenu(hwnd, nullptr);
        state.fullscreen_menu_shown = false;
        SetWindowLongPtrW(hwnd, GWL_STYLE,
                          (state.windowed_style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) |
                              WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, state.windowed_style);
        SetMenu(hwnd, state.menu);
        state.fullscreen_menu_shown = false;
        SetWindowPlacement(hwnd, &state.windowed_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                         SWP_FRAMECHANGED);
    }
    state.fullscreen = on;
    sync_menu_state(state);
    save_settings(state);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// --- the Game Genie dialog -------------------------------------------------

// REBUILDING THE LIST IS NOT THE USER TICKING ANYTHING.
//
// A list view sends LVN_ITEMCHANGED for every row it is given, including the
// state image that is its tick box, so the handler that reads a tick back
// into a cheat also hears every insertion - and an inserted row starts
// unticked. Without this the act of showing the list turned every code off.
bool g_game_genie_filling = false;
// Which library entry each visible row is, because the list can be filtered
// down to one game and a row number is then not an index into the library.
// Getting this wrong deletes the wrong code, so the two are never assumed
// to be the same.
std::vector<std::size_t> g_game_genie_rows;

void game_genie_fill_list(HWND dialog, const RuntimeState& state) {
    HWND list = GetDlgItem(dialog, IDC_GG_LIST);
    const bool only_this = IsDlgButtonChecked(dialog, IDC_GG_ONLY_THIS) == BST_CHECKED;
    const auto game = current_game_key(state);
    g_game_genie_filling = true;
    g_game_genie_rows.clear();
    ListView_DeleteAllItems(list);
    for (std::size_t i = 0; i < state.cheats.size(); ++i) {
        const auto& entry = state.cheats[i];
        if (only_this && !game.empty() && !cheat_applies_here(entry, game)) {
            continue;
        }
        const auto& cheat = entry.cheat;
        const int row = static_cast<int>(g_game_genie_rows.size());
        g_game_genie_rows.push_back(i);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        auto code = widen(cheat.code);
        item.pszText = code.data();
        SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        // What it patches, in the machine's own terms. A person checking a
        // code against a printed list wants to see the address, and a code
        // that decoded to somewhere absurd is obvious at a glance.
        wchar_t effect[64];
        if (cheat.word) {
            std::swprintf(effect, 64, L"%06X = %04X", cheat.address, cheat.value);
        } else if (cheat.compare >= 0) {
            std::swprintf(effect, 64, L"%04X = %02X if %02X", cheat.address, cheat.value,
                          static_cast<unsigned>(cheat.compare));
        } else {
            std::swprintf(effect, 64, L"%04X = %02X", cheat.address, cheat.value);
        }
        set_list_subitem(list, row, 1, effect);
        // Which games it is for. "every game" is not a placeholder - a code
        // with no game named really does apply to whatever is loaded.
        std::wstring games;
        if (entry.games.empty()) {
            games = L"every game";
        } else {
            for (const auto& name : entry.games) {
                if (!games.empty()) {
                    games += L", ";
                }
                games += widen(name);
            }
        }
        set_list_subitem(list, row, 2, games.c_str());
        set_list_subitem(list, row, 3, widen(cheat.description).c_str());
        ListView_SetCheckState(list, row, cheat.enabled ? TRUE : FALSE);
    }
    g_game_genie_filling = false;
}

// The library entry the selected row stands for, or -1.
std::ptrdiff_t game_genie_selected(HWND dialog) {
    HWND list = GetDlgItem(dialog, IDC_GG_LIST);
    const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (row < 0 || static_cast<std::size_t>(row) >= g_game_genie_rows.size()) {
        return -1;
    }
    return static_cast<std::ptrdiff_t>(g_game_genie_rows[static_cast<std::size_t>(row)]);
}

void game_genie_say(HWND dialog, const std::wstring& text) {
    SetDlgItemTextW(dialog, IDC_GG_STATUS, text.c_str());
}

INT_PTR CALLBACK game_genie_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<RuntimeState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, static_cast<LONG_PTR>(lparam));
            auto* init = reinterpret_cast<RuntimeState*>(lparam);
            // BOTH FORMATS, ALWAYS. The format is read off the code itself,
            // so naming only the loaded machine's would be describing a
            // restriction that is not there - and with no game loaded there
            // is no machine to name.
            SetDlgItemTextW(dialog, IDC_GG_NOTE,
                            L"Mega Drive codes are eight letters (ABCD-EFGH); Master System and "
                            L"Game Gear codes are six or nine hex digits (00A-000 or "
                            L"00A-000-000). Paste a multi-part cheat whole.");
            HWND list = GetDlgItem(dialog, IDC_GG_LIST);
            ListView_SetExtendedListViewStyle(
                list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const struct {
                const wchar_t* title;
                int width;
            } columns[] = {
                {L"Code", 105}, {L"Effect", 125}, {L"Games", 175}, {L"What it does", 180}};
            for (int i = 0; i < 4; ++i) {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(columns[i].title);
                column.cx = columns[i].width;
                SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
                             reinterpret_cast<LPARAM>(&column));
            }
            CheckDlgButton(dialog, IDC_GG_ENABLE, init->cheats_on ? BST_CHECKED : BST_UNCHECKED);
            game_genie_fill_list(dialog, *init);
            if (!init->rom_loaded) {
                game_genie_say(dialog, L"No game is loaded, so nothing is being patched yet - "
                                       L"codes added now take effect when a game is loaded.");
            }
            const BOOL dark = init->dark_theme ? TRUE : FALSE;
            if (FAILED(DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark)))) {
                DwmSetWindowAttribute(dialog, 19, &dark, sizeof(dark));
            }
            return TRUE;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            if (state != nullptr) {
                return reinterpret_cast<INT_PTR>(theme_brush(*state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (state != nullptr) {
                return reinterpret_cast<INT_PTR>(
                    theme_field_brush(*state, reinterpret_cast<HDC>(wparam)));
            }
            break;
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lparam);
            // The tick boxes are the switch for one code. Changing one takes
            // effect at once rather than on Close, so a code can be tried
            // against the game playing behind the dialog.
            if (state != nullptr && header != nullptr && !g_game_genie_filling &&
                header->idFrom == IDC_GG_LIST && header->code == LVN_ITEMCHANGED) {
                auto* changed = reinterpret_cast<NMLISTVIEW*>(lparam);
                if ((changed->uChanged & LVIF_STATE) != 0 &&
                    ((changed->uOldState ^ changed->uNewState) & LVIS_STATEIMAGEMASK) != 0) {
                    const auto row = static_cast<std::size_t>(changed->iItem);
                    if (row < g_game_genie_rows.size()) {
                        const auto index = g_game_genie_rows[row];
                        state->cheats[index].cheat.enabled =
                            ListView_GetCheckState(header->hwndFrom, changed->iItem) != FALSE;
                        apply_cheats(*state);
                        save_cheats(*state);
                    }
                }
            }
            break;
        }
        case WM_COMMAND: {
            if (state == nullptr) {
                break;
            }
            const int control = LOWORD(wparam);
            if (control == IDC_GG_ADD) {
                wchar_t typed[64] = L"";
                GetDlgItemTextW(dialog, IDC_GG_CODE, typed, 64);
                wchar_t note[256] = L"";
                GetDlgItemTextW(dialog, IDC_GG_DESCRIPTION, note, 256);
                // A published multi-part cheat is several codes joined with
                // " + " and pasting the whole line should add all of them.
                const auto pieces = mercury::core::split_codes(narrow(typed));
                if (pieces.empty()) {
                    game_genie_say(dialog, L"Type a code first.");
                    return TRUE;
                }
                std::vector<mercury::core::Cheat> decoded;
                std::string error;
                for (const auto& piece : pieces) {
                    mercury::core::Cheat cheat;
                    if (!mercury::core::decode_cheat(piece, mercury::core::CheatFormat::automatic,
                                                     cheat, error)) {
                        game_genie_say(dialog, widen(error));
                        return TRUE;
                    }
                    cheat.description = narrow(note);
                    decoded.push_back(cheat);
                }
                // A code added while a game is open is for THAT game. Added
                // with nothing open it is left for every game, which is what
                // makes typing a list before loading anything work.
                const auto game = current_game_key(*state);
                // The format is the code's own, so a code for the other
                // machine decodes perfectly and then does nothing. Say so
                // rather than leaving a ticked row that has no effect.
                const bool mega = cheats_are_mega_drive(*state);
                std::wstring note_text;
                if (state->rom_loaded && decoded.front().word != mega) {
                    note_text = mega ? L" - but that is a Master System / Game Gear code and a "
                                       L"Mega Drive game is loaded, so it will not do anything"
                                     : L" - but that is a Mega Drive code and a Game Gear game "
                                       L"is loaded, so it will not do anything";
                }
                for (auto& cheat : decoded) {
                    LibraryCheat entry;
                    entry.cheat = cheat;
                    if (!game.empty()) {
                        entry.games.push_back(game);
                    }
                    state->cheats.push_back(entry);
                }
                apply_cheats(*state);
                save_cheats(*state);
                game_genie_fill_list(dialog, *state);
                SetDlgItemTextW(dialog, IDC_GG_CODE, L"");
                SetDlgItemTextW(dialog, IDC_GG_DESCRIPTION, L"");
                std::wstring said = widen(decoded.front().code);
                if (decoded.size() > 1) {
                    said += L" and " + std::to_wstring(decoded.size() - 1) + L" more";
                }
                game_genie_say(dialog, said + L" added." + note_text);
                return TRUE;
            }
            if (control == IDC_GG_REMOVE) {
                const auto index = game_genie_selected(dialog);
                if (index < 0) {
                    game_genie_say(dialog, L"Pick a code in the list first.");
                    return TRUE;
                }
                // Turn it off before it goes, or a Mega Drive code's patch
                // stays in the cartridge with nothing left to undo it.
                state->cheats[static_cast<std::size_t>(index)].cheat.enabled = false;
                apply_cheats(*state);
                state->cheats.erase(state->cheats.begin() + index);
                apply_cheats(*state);
                save_cheats(*state);
                game_genie_fill_list(dialog, *state);
                game_genie_say(dialog, L"Removed.");
                return TRUE;
            }
            // Adding a game to a code, but not taking one away: a code that
            // should not fire is simply unticked, which is one control doing
            // one obvious thing rather than two that overlap.
            if (control == IDC_GG_USE_HERE) {
                const auto index = game_genie_selected(dialog);
                if (index < 0) {
                    game_genie_say(dialog, L"Pick a code in the list first.");
                    return TRUE;
                }
                const auto game = current_game_key(*state);
                if (game.empty()) {
                    game_genie_say(dialog, L"Open a game first - this says which game a code "
                                           L"is for.");
                    return TRUE;
                }
                auto& entry = state->cheats[static_cast<std::size_t>(index)];
                if (std::find(entry.games.begin(), entry.games.end(), game) ==
                    entry.games.end()) {
                    entry.games.push_back(game);
                }
                apply_cheats(*state);
                save_cheats(*state);
                game_genie_fill_list(dialog, *state);
                game_genie_say(dialog,
                               widen(entry.cheat.code) + L" is now used for " + widen(game) + L".");
                return TRUE;
            }
            if (control == IDC_GG_ONLY_THIS) {
                game_genie_fill_list(dialog, *state);
                return TRUE;
            }
            if (control == IDC_GG_ENABLE) {
                state->cheats_on = IsDlgButtonChecked(dialog, IDC_GG_ENABLE) == BST_CHECKED;
                apply_cheats(*state);
                save_settings(*state);
                game_genie_say(dialog, state->cheats_on ? L"Codes enabled." : L"Codes disabled.");
                return TRUE;
            }
            if (control == IDOK || control == IDCANCEL) {
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            break;
        }
        default:
            break;
    }
    return FALSE;
}

void show_game_genie(RuntimeState& state) {
    DialogBoxParamW(state.instance, MAKEINTRESOURCEW(IDD_GAME_GENIE), state.window,
                    game_genie_proc, reinterpret_cast<LPARAM>(&state));
}

INT_PTR CALLBACK aspect_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<RuntimeState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, static_cast<LONG_PTR>(lparam));
            auto* init = reinterpret_cast<RuntimeState*>(lparam);
            SetDlgItemInt(dialog, IDC_ASPECT_WIDTH, static_cast<UINT>(init->custom_aspect_width),
                          FALSE);
            SetDlgItemInt(dialog, IDC_ASPECT_HEIGHT, static_cast<UINT>(init->custom_aspect_height),
                          FALSE);
            const BOOL dark = init->dark_theme ? TRUE : FALSE;
            if (FAILED(DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark)))) {
                DwmSetWindowAttribute(dialog, 19, &dark, sizeof(dark));
            }
            return TRUE;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC: {
            if (state == nullptr) {
                break;
            }
            const Theme& theme = theme_of(*state);
            static HBRUSH brush = nullptr;
            if (brush != nullptr) {
                DeleteObject(brush);
            }
            brush = CreateSolidBrush(theme.background);
            SetTextColor(reinterpret_cast<HDC>(wparam), theme.text);
            SetBkColor(reinterpret_cast<HDC>(wparam), theme.background);
            return reinterpret_cast<INT_PTR>(brush);
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK && state != nullptr) {
                BOOL ok_width = FALSE;
                BOOL ok_height = FALSE;
                const UINT width = GetDlgItemInt(dialog, IDC_ASPECT_WIDTH, &ok_width, FALSE);
                const UINT height = GetDlgItemInt(dialog, IDC_ASPECT_HEIGHT, &ok_height, FALSE);
                if (!ok_width || !ok_height || width == 0 || height == 0) {
                    MessageBoxW(dialog, L"Width and height both have to be greater than zero.",
                                L"GenX", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                state->custom_aspect_width = static_cast<int>(width);
                state->custom_aspect_height = static_cast<int>(height);
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

LRESULT CALLBACK config_tab_proc(HWND control, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) {
        auto* draft =
            reinterpret_cast<ConfigDraft*>(GetWindowLongPtrW(GetParent(control), GWLP_USERDATA));
        if (draft != nullptr) {
            RECT client{};
            GetClientRect(control, &client);
            HBRUSH brush = CreateSolidBrush(theme_of(*draft->state).background);
            FillRect(reinterpret_cast<HDC>(wparam), &client, brush);
            DeleteObject(brush);
            return 1;
        }
    }
    return CallWindowProcW(g_config_tab_original, control, message, wparam, lparam);
}

void show_configure(RuntimeState& state) {
    ConfigDraft draft;
    draft.state = &state;
    draft.hide_cursor = state.hide_cursor;
    draft.check_for_updates = state.check_for_updates;
    draft.volume = state.volume;
    draft.muted = state.muted;
    draft.console_filter = state.console_filter;
    draft.original_volume = state.volume;
    draft.original_muted = state.muted;
    draft.original_console_filter = state.console_filter;
    draft.games_folders = state.games_folders;
    draft.save_folders = state.save_folders;
    draft.bios_folder_32x = state.bios_folder_32x;
    draft.bios_folder_segacd = state.bios_folder_segacd;
    draft.players = state.players;
    draft.profile_names = state.profile_names;
    draft.hotkeys = state.hotkeys;
    DialogBoxParamW(state.instance, MAKEINTRESOURCEW(IDD_CONFIGURE), state.window, configure_proc,
                    reinterpret_cast<LPARAM>(&draft));
}

// Open ROM, for one system. Each has its own library folder and its own file
// types, so the dialog opens where that system's games actually are instead of
// wherever the last file dialog in the process happened to end up.
void prompt_open_rom(RuntimeState& state, int system) {
    struct SystemFiles {
        const wchar_t* title;
        const wchar_t* filter;
        bool emulated;
    };
    // The order matches the SaveFolder enum, so one index reaches the folder,
    // the filter and the title together.
    static const SystemFiles files[kSaveFolderCount] = {
        {L"Open Genesis / Mega Drive ROM",
         L"Genesis / Mega Drive\0*.md;*.gen;*.smd;*.bin;*.zip\0All files\0*.*\0\0", true},
        {L"Open Game Gear / Master System ROM",
         L"Game Gear / Master System\0*.gg;*.sms;*.zip\0All files\0*.*\0\0", true},
        {L"Open 32X ROM", L"32X\0*.32x;*.bin;*.zip\0All files\0*.*\0\0", true},
        {L"Open Sega CD image", L"Sega CD\0*.cue;*.iso;*.bin;*.chd\0All files\0*.*\0\0", true},
    };
    if (system < 0 || system >= kSaveFolderCount) {
        return;
    }
    const auto& chosen_system = files[system];
    // Said before the file dialog rather than after the pick: choosing a game
    // and only then being told the machine does not exist is a worse way to
    // find out.
    if (!chosen_system.emulated) {
        MessageBoxW(state.window,
                    L"That system is not emulated yet.\n\nIts folder is here so the setup is "
                    L"ready, but GenX cannot run these games.",
                    L"GenX", MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.window;
    dialog.lpstrFilter = chosen_system.filter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = chosen_system.title;
    std::wstring initial;
    const auto& folder = state.games_folders[system];
    if (!folder.empty() && std::filesystem::is_directory(folder)) {
        initial = folder.wstring();
        dialog.lpstrInitialDir = initial.c_str();
    }
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) {
        return;
    }
    std::string error;
    LoadProblem problem = LoadProblem::other;
    if (!load_rom_from_path(state, path, error, &problem) &&
        !report_load_failure(state, path, error, problem)) {
        return;
    }
    remember_recent(state, path);
    InvalidateRect(state.window, nullptr, TRUE);
}

// --- the disc drive --------------------------------------------------------

// Put a different disc in WITHOUT restarting anything.
//
// This is not Open: opening disc 2 of Night Trap reboots the console and
// drops the player back at the title with their progress gone. A swap leaves
// the game running - which is the whole reason a multi-disc game asks for the
// next disc instead of just ending.
//
// The file dialog starts in the folder the current disc came from, because
// the other discs of a set are nearly always its siblings.
void swap_disc(RuntimeState& state) {
    if (!state.rom_loaded || state.machine != Machine::genesis ||
        !state.genesis.cd_mode()) {
        MessageBoxW(state.window,
                    L"There is no disc in the drive.\n\nOpen a Sega CD game first, or use "
                    L"Boot Sega CD to start the console with an empty tray.",
                    L"GenX", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.window;
    dialog.lpstrFilter = L"Sega CD\0*.cue;*.iso;*.bin;*.chd\0All files\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"Which disc goes in?";
    std::wstring initial;
    if (!state.rom_path.empty()) {
        initial = state.rom_path.parent_path().wstring();
    }
    if (initial.empty() || !std::filesystem::is_directory(initial)) {
        const auto& folder = state.games_folders[kSaveCd];
        if (!folder.empty() && std::filesystem::is_directory(folder)) {
            initial = folder.wstring();
        }
    }
    if (!initial.empty()) {
        dialog.lpstrInitialDir = initial.c_str();
    }
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) {
        return;
    }
    std::string error;
    if (!state.genesis.segacd().swap_disc(path, error)) {
        MessageBoxW(state.window, widen(error).c_str(), L"GenX", MB_OK | MB_ICONERROR);
        return;
    }
    // The window title follows the disc, not the session: a save state or a
    // screenshot taken after the swap belongs to the disc that is in now.
    state.rom_path = path;
    remember_recent(state, state.rom_path);
    update_title(state);
    set_status(state, L"Disc swapped - " + widen(state.genesis.segacd().disc().title()));
}

// The console with an empty tray, which is a real machine doing a real thing:
// the Sega CD's own front end. From there it plays audio CDs and, more to the
// point, formats and erases the backup RAM that every disc saves into. There
// was no way to reach any of that without a game in the drive.
void boot_sega_cd(RuntimeState& state) {
    std::vector<std::uint8_t> bios;
    std::string error;
    // No disc, so no region to match: load_bios_for takes any image and
    // prefers USA - the front end is the same program in every region.
    if (!mercury::segacd::load_bios_for(0, state.bios_folder_segacd, {}, bios, error)) {
        report_load_failure(state, {}, error, LoadProblem::segacd_bios);
        // The retry inside report_load_failure loads a PATH, and there is no
        // path here, so try again by hand with whatever folder was chosen.
        if (!mercury::segacd::load_bios_for(0, state.bios_folder_segacd, {}, bios, error)) {
            return;
        }
    }
    flush_battery(state, false);
    const auto result = state.genesis.load_cd({}, bios);
    if (!result.loaded) {
        MessageBoxW(state.window, widen(result.message).c_str(), L"GenX", MB_OK | MB_ICONERROR);
        return;
    }
    finish_loading_machine(state, Machine::genesis, {}, 1);
    set_status(state, L"Sega CD - no disc");
}

// --- rewind ----------------------------------------------------------------
// The whole machine, snapshotted every few frames into a ring. Rewinding is
// then loading the previous snapshot back, which walks the game backwards in
// the increments it was recorded in. Bounded on purpose: a history that grows
// with session length would eventually cost more memory than the emulator.
constexpr int kRewindIntervalFrames = 10;
constexpr std::size_t kRewindMaxStates = 120;
// How much faster than real time the fast-forward key runs.
constexpr int kFastForwardMultiplier = 4;

std::vector<std::uint8_t> capture_state_blob(const RuntimeState& state) {
    return state.machine == Machine::genesis ? state.genesis.save_state_blob()
                                             : state.game_gear.save_state_blob();
}

bool apply_state_blob(RuntimeState& state, const std::vector<std::uint8_t>& blob) {
    std::string error;
    return load_machine_state(state, blob, error);
}

void record_rewind_point(RuntimeState& state) {
    // The Sega CD and the 32X included, now that a state carries them.
    if (!state.rom_loaded) {
        return;
    }
    if (--state.rewind_countdown > 0) {
        return;
    }
    state.rewind_countdown = kRewindIntervalFrames;
    state.rewind_history.push_back(capture_state_blob(state));
    while (state.rewind_history.size() > kRewindMaxStates) {
        state.rewind_history.pop_front();
    }
}

// The newest entry is roughly where the machine already is, so it is dropped
// and the one before it loaded. Reloading the newest would put the game back
// where it started and read as a freeze rather than as rewinding.
bool step_rewind(RuntimeState& state) {
    if (state.rewind_history.size() < 2) {
        return false;
    }
    state.rewind_history.pop_back();
    const bool loaded = apply_state_blob(state, state.rewind_history.back());
    state.rewind_countdown = kRewindIntervalFrames;
    return loaded;
}

// --- hotkey dispatch -------------------------------------------------------

int find_hotkey(const RuntimeState& state, UINT key, bool ctrl, bool shift, bool alt) {
    const Hotkey pressed{key, ctrl, shift, alt};
    for (int action = 0; action < kHkCount; ++action) {
        if (hotkey_bound(state.hotkeys[action]) && hotkey_same(state.hotkeys[action], pressed)) {
            return action;
        }
    }
    return -1;
}

// True when the key was a hotkey, in which case it must not also reach the pads.
bool handle_hotkey_down(RuntimeState& state, UINT key, bool ctrl, bool shift, bool alt) {
    const int action = find_hotkey(state, key, ctrl, shift, alt);
    if (action < 0) {
        return false;
    }
    // Bounded at BOTH ends. These used to test only the lower bound, which
    // silently swallowed every action numbered above the state slots: the
    // four Open ROM hotkeys, the diagnostic log and the piano all landed in
    // load_state_slot instead. Pressing M reported "state 15 is empty",
    // which is action 36 minus the first load slot plus one - the arithmetic
    // of a range check with no ceiling.
    if (action >= kHkLoadStateFirst && action < kHkStateEnd) {
        load_state_slot(state, action - kHkLoadStateFirst + 1);
        return true;
    }
    if (action >= kHkSaveStateFirst && action < kHkLoadStateFirst) {
        save_state_slot(state, action - kHkSaveStateFirst + 1);
        return true;
    }
    switch (action) {
        case kHkOpenGenesis:
            prompt_open_rom(state, kSaveGenesis);
            return true;
        case kHkOpenGameGear:
            prompt_open_rom(state, kSaveGameGear);
            return true;
        case kHkOpen32X:
            prompt_open_rom(state, kSave32X);
            return true;
        case kHkOpenCd:
            prompt_open_rom(state, kSaveCd);
            return true;
        case kHkMiraclePiano:
            toggle_piano_window(state);
            return true;
        case kHkDumpDiagnostics:
            write_diagnostic_log(state);
            return true;
        case kHkQuit:
            PostMessageW(state.window, WM_CLOSE, 0, 0);
            return true;
        case kHkPause:
            if (state.rom_loaded) {
                state.paused = !state.paused;
                sync_menu_state(state);
            }
            return true;
        case kHkSoftReset:
            restart_machine(state, false);
            return true;
        case kHkHardReset:
            restart_machine(state, true);
            return true;
        case kHkPowerOff:
            unload_rom(state);
            return true;
        case kHkFullscreen:
            set_fullscreen(state, !state.fullscreen);
            return true;
        case kHkCrtFilter:
            state.crt_filter = !state.crt_filter;
            sync_menu_state(state);
            save_settings(state);
            if (state.window != nullptr) {
                InvalidateRect(state.window, nullptr, TRUE);
            }
            return true;
        case kHkTurbo:
            state.turbo_held = true;
            return true;
        case kHkFastForward:
            state.fast_forward_held = true;
            return true;
        case kHkRewind:
            state.rewind_held = true;
            return true;
        case kHkDumpVideo:
            if (state.rom_loaded) {
                dump_video_state(state);
            }
            return true;
        default:
            return false;
    }
}

// Key-up matters only for the held actions, and is matched on the key alone.
// Letting go of Ctrl before the key would otherwise leave one stuck down.
bool handle_hotkey_up(RuntimeState& state, UINT key) {
    bool consumed = false;
    for (int action = 0; action < kHkCount; ++action) {
        if (!hotkey_is_held(action) || state.hotkeys[action].key != key) {
            continue;
        }
        if (action == kHkTurbo) {
            state.turbo_held = false;
            // Leave nothing pressed behind on the phase it happened to stop on.
            if (!kind_is_light_gun(state.players[0].kind)) {
                apply_pad_button(state, 0, PadButton::a, false);
                apply_pad_button(state, 0, PadButton::b, false);
                apply_pad_button(state, 0, PadButton::c, false);
            }
        }
        if (action == kHkFastForward) {
            state.fast_forward_held = false;
        }
        if (action == kHkRewind) {
            state.rewind_held = false;
        }
        consumed = true;
    }
    return consumed;
}

// --- the light-gun crosshair -----------------------------------------------

// Turn the shared pixel design into a cursor. A 32-bit DIB section with an
// alpha mask is what gives a cursor real transparency; the monochrome mask has
// to exist but is ignored once there is an alpha channel.
HCURSOR create_crosshair_cursor(int size) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;  // top-down, matching the generator
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                      DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (colour == nullptr || bits == nullptr) {
        return nullptr;
    }
    const auto pixels = mercury::core::make_crosshair(size);
    std::memcpy(bits, pixels.data(), pixels.size() * sizeof(std::uint32_t));

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = FALSE;
    // The aim point is the centre of the image, not its corner.
    info.xHotspot = static_cast<DWORD>(size / 2);
    info.yHotspot = static_cast<DWORD>(size / 2);
    info.hbmMask = mask;
    info.hbmColor = colour;
    HCURSOR cursor = CreateIconIndirect(&info);
    DeleteObject(colour);
    DeleteObject(mask);
    return cursor;
}

// Built once per size and kept. Four sizes rather than a continuous range so
// the crosshair does not change shape while a window is being dragged bigger.
HCURSOR crosshair_cursor(int size) {
    struct Cached {
        int size;
        HCURSOR cursor;
    };
    static Cached cache[4] = {{24, nullptr}, {32, nullptr}, {48, nullptr}, {64, nullptr}};
    for (auto& entry : cache) {
        if (entry.size != size) {
            continue;
        }
        if (entry.cursor == nullptr) {
            entry.cursor = create_crosshair_cursor(entry.size);
        }
        return entry.cursor;
    }
    return nullptr;
}

// Scaled with the picture, so the crosshair covers about the same number of
// GAME pixels however big the window is - which is what makes it feel the same
// at 1x and in fullscreen.
int crosshair_size_for(const RuntimeState& state) {
    if (state.window == nullptr || state.presented_width == 0) {
        return 32;
    }
    RECT client{};
    GetClientRect(state.window, &client);
    const RECT picture = picture_rect(state, client);
    const double scale = static_cast<double>(picture.right - picture.left) /
                         static_cast<double>(state.presented_width);
    const long wanted = std::lround(16.0 * scale);
    if (wanted <= 28) return 24;
    if (wanted <= 40) return 32;
    if (wanted <= 56) return 48;
    return 64;
}

bool light_gun_selected(const RuntimeState& state) {
    return kind_is_light_gun(state.players[0].kind) || kind_is_light_gun(state.players[1].kind);
}

// A gun is plugged in AND it is one that needs a sight drawn for it. See
// kind_draws_crosshair: a Menacer game already has one and does not want two.
bool crosshair_wanted(const RuntimeState& state) {
    return kind_draws_crosshair(state.players[0].kind) ||
           kind_draws_crosshair(state.players[1].kind);
}

// A gun is plugged in and the game draws the sight, so the pointer goes away.
bool pointer_hidden_for_gun(const RuntimeState& state) {
    return kind_hides_pointer(state.players[0].kind) || kind_hides_pointer(state.players[1].kind);
}

// --- two guns, two mice ------------------------------------------------------

// Two guns at once is a pair of Justifiers and nothing else. That is the only
// arrangement the hardware allows: the second gun daisy-chains off the first
// and both answer on port 2. Two Menacers is not a thing - one infrared
// receiver box, one gun - so this is deliberately not "any two light guns".
//
// NOT DURING NETPLAY. Each computer then has one person and one mouse, and the
// other gun arrives over the wire - so the two-mice machinery (per-device raw
// input, merged clicks ignored, both sights from local mice) stands aside.
// Left on, it swallowed the clicks that should have been this player's
// trigger: a Lethal Enforcers session where neither player could fire.
bool two_gun_mode(const RuntimeState& state) {
    return !state.netplay.active() && state.players[0].kind == PadKind::justifier &&
           state.players[1].kind == PadKind::justifier;
}

// Ask Windows to report mouse input per device as well as merged.
//
// No RIDEV_NOLEGACY: the ordinary WM_MOUSEMOVE stream has to keep working,
// because it is what drives the menus, the single-gun path and every dialog.
// Raw input is added ALONGSIDE it, not instead of it.
void register_raw_mice(RuntimeState& state) {
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;  // generic desktop
    device.usUsage = 0x02;      // mouse
    // RIDEV_DEVNOTIFY so a mouse being unplugged is reported. Without it a gun
    // keeps the slot belonging to a device that is no longer there, and its
    // crosshair sits frozen on the picture for the rest of the session.
    device.dwFlags = RIDEV_DEVNOTIFY;
    device.hwndTarget = state.window;
    state.raw_input_ready = RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
}

// At least one mouse has been claimed, so there is a painted sight on screen.
bool any_gun_paired(const RuntimeState& state) {
    return state.gun_pointers[0].seen || state.gun_pointers[1].seen;
}

// Which gun a physical mouse belongs to, claiming a free slot for one that has
// not been seen before. First device to move or fire is player one, second is
// player two - there is no way to ask a mouse who is holding it, so order of
// use is the only thing available. A third mouse is ignored rather than made to
// fight one of the two for a slot.
int gun_for_device(RuntimeState& state, HANDLE device) {
    for (int gun = 0; gun < 2; ++gun) {
        if (state.gun_pointers[gun].device == device) {
            return gun;
        }
    }
    for (int gun = 0; gun < 2; ++gun) {
        if (state.gun_pointers[gun].device == nullptr) {
            state.gun_pointers[gun].device = device;
            // Start in the middle of the window. A relative mouse reports only
            // movement, so it needs somewhere to move FROM; starting at the
            // origin would put player two's sight in the top-left corner until
            // they had dragged it out.
            RECT client{};
            if (state.window != nullptr) {
                GetClientRect(state.window, &client);
            }
            state.gun_pointers[gun].x = (client.right - client.left) / 2;
            state.gun_pointers[gun].y = (client.bottom - client.top) / 2;
            return gun;
        }
    }
    return -1;
}

void forget_gun_devices(RuntimeState& state) {
    for (auto& pointer : state.gun_pointers) {
        pointer = RuntimeState::GunPointer{};
    }
}

// One mouse has been unplugged. Its gun goes back to having no device and no
// sight, rather than leaving a crosshair parked wherever the mouse last was.
// The slot is freed too, so the next mouse to arrive can take it.
void release_gun_device(RuntimeState& state, HANDLE device) {
    for (auto& pointer : state.gun_pointers) {
        if (pointer.device == device) {
            pointer = RuntimeState::GunPointer{};
        }
    }
}

void handle_raw_mouse(RuntimeState& state, HRAWINPUT handle) {
    UINT size = 0;
    if (GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0) {
        return;
    }
    static std::vector<std::uint8_t> buffer;
    buffer.resize(size);
    if (GetRawInputData(handle, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
        return;
    }
    const auto* input = reinterpret_cast<const RAWINPUT*>(buffer.data());
    if (input->header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    const int gun = gun_for_device(state, input->header.hDevice);
    if (gun < 0) {
        return;
    }
    auto& pointer = state.gun_pointers[gun];
    const RAWMOUSE& mouse = input->data.mouse;

    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        // What a Wii remote through a Dolphin Bar sends: a POINT it is aimed
        // at, not a movement. The pair is a fraction of 65535 across either the
        // whole desktop or the primary monitor, and the flag beside it says
        // which - guessing wrong puts the aim on the wrong screen entirely on a
        // multi-monitor desk.
        const bool desktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int origin_x = desktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
        const int origin_y = desktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
        const int span_x = GetSystemMetrics(desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const int span_y = GetSystemMetrics(desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        POINT at{origin_x + MulDiv(mouse.lLastX, span_x, 65535),
                 origin_y + MulDiv(mouse.lLastY, span_y, 65535)};
        if (state.window != nullptr) {
            ScreenToClient(state.window, &at);
        }
        pointer.x = at.x;
        pointer.y = at.y;
    } else if (mouse.lLastX != 0 || mouse.lLastY != 0) {
        pointer.x += mouse.lLastX;
        pointer.y += mouse.lLastY;
    }
    // Kept inside the window, so a gun that has been swung too far still comes
    // back. Outside the PICTURE is still allowed and still means an off-screen
    // shot; this only stops a relative mouse running away to infinity.
    RECT client{};
    if (state.window != nullptr) {
        GetClientRect(state.window, &client);
        pointer.x = std::clamp(pointer.x, 0, std::max(0, static_cast<int>(client.right) - 1));
        pointer.y = std::clamp(pointer.y, 0, std::max(0, static_cast<int>(client.bottom) - 1));
    }
    pointer.seen = true;

    if ((mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) pointer.left = true;
    if ((mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) != 0) pointer.left = false;
    if ((mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) pointer.right = true;
    if ((mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) != 0) pointer.right = false;
    if ((mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0) pointer.middle = true;
    if ((mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP) != 0) pointer.middle = false;
}

// Is the button this binding names held on this particular mouse? Only mouse
// bindings can be answered here - a gun driven by raw input is driven by one
// device, and a keyboard key is not part of it.
bool pointer_button_down(const RuntimeState::GunPointer& pointer, UINT key) {
    switch (key) {
        case VK_LBUTTON: return pointer.left;
        case VK_RBUTTON: return pointer.right;
        case VK_MBUTTON: return pointer.middle;
        default: return false;
    }
}

// Client pixels to the VDP's own, for one gun. Shared by both input paths so a
// shot from a raw device lands in the same place as one from the pointer.
bool client_to_gun(const RuntimeState& state, POINT at, std::uint32_t& gun_x,
                   std::uint32_t& gun_y) {
    RECT client{};
    GetClientRect(state.window, &client);
    const RECT picture = picture_rect(state, client);
    const int width = picture.right - picture.left;
    const int height = picture.bottom - picture.top;
    if (width <= 0 || height <= 0 || at.x < picture.left || at.x >= picture.right ||
        at.y < picture.top || at.y >= picture.bottom) {
        return false;
    }
    const auto source_width = static_cast<std::uint32_t>(state.presented_width);
    // logical_height, not the raw one: an interlaced frame is twice as tall in
    // the buffer but the same height on the screen.
    const auto source_height = static_cast<std::uint32_t>(logical_height(state));
    if (source_width == 0 || source_height == 0) {
        return false;
    }
    gun_x = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(at.x - picture.left) * source_width / width);
    gun_y = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(at.y - picture.top) * source_height / height);
    return true;
}

// The other way: a gun's VDP pixel back to client pixels, at the middle of the
// screen pixel it names. For drawing the OTHER netplay player's sight, whose
// aim arrives as the game sees it rather than as their window did.
bool gun_to_client(const RuntimeState& state, std::uint32_t gun_x, std::uint32_t gun_y,
                   POINT& at) {
    RECT client{};
    GetClientRect(state.window, &client);
    const RECT picture = picture_rect(state, client);
    const int width = picture.right - picture.left;
    const int height = picture.bottom - picture.top;
    const auto source_width = static_cast<std::int64_t>(state.presented_width);
    const auto source_height = static_cast<std::int64_t>(logical_height(state));
    if (width <= 0 || height <= 0 || source_width <= 0 || source_height <= 0) {
        return false;
    }
    at.x = picture.left + static_cast<LONG>((static_cast<std::int64_t>(gun_x) * 2 + 1) * width /
                                            (source_width * 2));
    at.y = picture.top + static_cast<LONG>((static_cast<std::int64_t>(gun_y) * 2 + 1) * height /
                                           (source_height * 2));
    return true;
}

// Is a binding held right now? A mouse button answers from what Windows last
// told this window (logical buttons, tracked in WM_xBUTTON*); a key from the
// keyboard - but only while this window has the focus, or a click in another
// program would fire the gun here.
bool binding_held(const RuntimeState& state, UINT key) {
    switch (key) {
        case 0: return false;
        case VK_LBUTTON: return state.mouse_left_down;
        case VK_RBUTTON: return state.mouse_right_down;
        case VK_MBUTTON: return state.mouse_middle_down;
        default:
            return GetForegroundWindow() == state.window &&
                   (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    }
}

// WHAT THIS PERSON IS DOING, for netplay to send - in the terms of the device
// in their seat, which is the host's choice and may not be what this
// computer's own Input tab says.
//
// The pad rows come from netplay_local_mask as always. A gun seat adds the
// mouse: the aim from the system pointer, the trigger and reload from this
// player's own bindings (the mouse buttons unless they chose otherwise). Each
// computer has one person, so there is no second mouse to tell apart and no
// raw input to ask - the other gun arrives over the wire.
mercury::net::PlayerInput capture_netplay_input(RuntimeState& state, PadKind seat_kind) {
    mercury::net::PlayerInput input;
    input.buttons = state.netplay_local_mask;
    if (!kind_is_light_gun(seat_kind) || state.machine != Machine::genesis ||
        state.window == nullptr) {
        return input;
    }
    const auto& keys = state.players[0].keys;
    const auto bound = [&keys](PadButton button, UINT fallback) {
        const UINT key = keys[static_cast<int>(button)];
        return key != 0 ? key : fallback;
    };
    constexpr auto trigger_bit = static_cast<std::uint16_t>(1u << mercury::net::PlayerInput::kTriggerBit);
    constexpr auto reload_bit = static_cast<std::uint16_t>(1u << mercury::net::PlayerInput::kReloadBit);
    const bool reload = binding_held(state, bound(PadButton::gun_reload, VK_RBUTTON)) ||
                        (input.buttons & reload_bit) != 0;
    const bool trigger = binding_held(state, bound(PadButton::gun_trigger, VK_LBUTTON)) ||
                         (input.buttons & trigger_bit) != 0;
    if (trigger) input.buttons |= trigger_bit;
    if (reload) input.buttons |= reload_bit;

    POINT at{};
    GetCursorPos(&at);
    ScreenToClient(state.window, &at);
    std::uint32_t gun_x = 0;
    std::uint32_t gun_y = 0;
    // A reload is the gun pointed away: off screen, whatever the pointer says.
    input.on_screen = !reload && client_to_gun(state, at, gun_x, gun_y);
    input.gun_x = static_cast<std::uint16_t>(std::min<std::uint32_t>(gun_x, 0xFFFF));
    input.gun_y = static_cast<std::uint16_t>(std::min<std::uint32_t>(gun_y, 0xFFFF));
    return input;
}

// Both Justifiers, each from its own mouse, both onto port 2.
void update_two_gun_aim(RuntimeState& state) {
    auto& port = state.genesis.port2();
    for (int gun = 0; gun < 2; ++gun) {
        auto& pointer = state.gun_pointers[gun];
        const auto& keys = state.players[gun].keys;
        // Resolved from this player's own bindings, so the two guns need not
        // agree about which button is the trigger.
        const bool reload =
            pointer_button_down(pointer, keys[static_cast<int>(PadButton::gun_reload)]);
        const bool trigger =
            pointer_button_down(pointer, keys[static_cast<int>(PadButton::gun_trigger)]) || reload;
        const bool start = pointer_button_down(pointer, keys[static_cast<int>(PadButton::start)]);
        pointer.off_screen_shot = reload;

        std::uint32_t gun_x = 0;
        std::uint32_t gun_y = 0;
        bool on_screen = false;
        if (pointer.seen && !reload) {
            on_screen = client_to_gun(state, POINT{pointer.x, pointer.y}, gun_x, gun_y);
        }
        const bool trigger_changed = trigger != pointer.trigger_applied;
        const bool start_changed = start != pointer.start_applied;
        pointer.trigger_applied = trigger;
        pointer.start_applied = start;
        if (gun == 0) {
            port.set_aim(gun_x, gun_y, on_screen);
            if (trigger_changed) {
                port.set_button(mercury::genesis::Button::a, trigger);
            }
            if (start_changed) {
                port.set_button(mercury::genesis::Button::start, start);
            }
        } else {
            port.set_gun2_aim(gun_x, gun_y, on_screen);
            if (trigger_changed) {
                port.set_gun2_trigger(trigger);
            }
            if (start_changed) {
                port.set_gun2_start(start);
            }
        }
    }
}

// The sight as a bitmap rather than a cursor. Two-gun play cannot use the mouse
// cursor for this - there is one system pointer and two guns - so both sights
// are painted into the picture instead.
HBITMAP crosshair_bitmap(int size, int gun) {
    struct Cached {
        int size;
        int gun;
        HBITMAP bitmap;
    };
    static std::vector<Cached> cache;
    for (const auto& entry : cache) {
        if (entry.size == size && entry.gun == gun) {
            return entry.bitmap;
        }
    }
    mercury::core::CrosshairStyle style;
    if (gun == 1) {
        // Player two is told apart by COLOUR, not by shape. A different shape
        // would have to be learned; a colour is read at a glance, which is all
        // the attention anyone has spare while aiming.
        style.core = 0xFFD6ECFF;
        style.centre = 0xFF2E9BFF;
    }
    const auto pixels = mercury::core::make_crosshair(size, style);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;  // top-down, matching the generator
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        return nullptr;
    }
    // AlphaBlend wants the colour PREMULTIPLIED by its alpha. The cursor path
    // wants it straight, which is why these two cannot share one bitmap - and
    // why a crosshair that looked right as a cursor would come out with a pale
    // halo if this step were skipped.
    auto* out = static_cast<std::uint32_t*>(bits);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const std::uint32_t pixel = pixels[i];
        const std::uint32_t alpha = (pixel >> 24) & 0xFF;
        const std::uint32_t r = ((pixel >> 16) & 0xFF) * alpha / 255;
        const std::uint32_t g = ((pixel >> 8) & 0xFF) * alpha / 255;
        const std::uint32_t b = (pixel & 0xFF) * alpha / 255;
        out[i] = (alpha << 24) | (r << 16) | (g << 8) | b;
    }
    cache.push_back({size, gun, bitmap});
    return bitmap;
}

// Netplay: one sight per Justifier seat, in that seat's colour. This player's
// own follows their pointer live, so aiming feels the same as at home; the
// other player's sits where their gun was pointed in the frame that just ran,
// which is where the game saw it. A Menacer seat gets none - its game draws
// its own.
void draw_netplay_crosshairs(const RuntimeState& state, HDC hdc) {
    if (!state.rom_loaded || state.machine != Machine::genesis || !state.netplay_seats_applied) {
        return;
    }
    const int size = crosshair_size_for(state);
    HDC memory = CreateCompatibleDC(hdc);
    if (memory == nullptr) {
        return;
    }
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const int mine = state.netplay.local_port_index();
    for (int seat = 0; seat < 2; ++seat) {
        if (state.netplay_seat_kinds[static_cast<std::size_t>(seat)] != PadKind::justifier) {
            continue;
        }
        POINT at{};
        if (seat == mine) {
            GetCursorPos(&at);
            ScreenToClient(state.window, &at);
            RECT client{};
            GetClientRect(state.window, &client);
            const RECT picture = picture_rect(state, client);
            if (!PtInRect(&picture, at)) {
                continue;
            }
        } else {
            const auto& input = state.netplay_seat_inputs[static_cast<std::size_t>(seat)];
            if (!input.on_screen || !gun_to_client(state, input.gun_x, input.gun_y, at)) {
                continue;
            }
        }
        HBITMAP bitmap = crosshair_bitmap(size, seat);
        if (bitmap == nullptr) {
            continue;
        }
        HGDIOBJ previous = SelectObject(memory, bitmap);
        AlphaBlend(hdc, at.x - size / 2, at.y - size / 2, size, size, memory, 0, 0, size, size,
                   blend);
        SelectObject(memory, previous);
    }
    DeleteDC(memory);
}

void draw_gun_crosshairs(const RuntimeState& state, HDC hdc) {
    if (state.netplay.active()) {
        draw_netplay_crosshairs(state, hdc);
        return;
    }
    if (!state.rom_loaded || !two_gun_mode(state) || !state.raw_input_ready) {
        return;
    }
    const int size = crosshair_size_for(state);
    HDC memory = CreateCompatibleDC(hdc);
    if (memory == nullptr) {
        return;
    }
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    for (int gun = 0; gun < 2; ++gun) {
        const auto& pointer = state.gun_pointers[gun];
        if (!pointer.seen) {
            continue;
        }
        HBITMAP bitmap = crosshair_bitmap(size, gun);
        if (bitmap == nullptr) {
            continue;
        }
        HGDIOBJ previous = SelectObject(memory, bitmap);
        AlphaBlend(hdc, pointer.x - size / 2, pointer.y - size / 2, size, size, memory, 0, 0, size,
                   size, blend);
        SelectObject(memory, previous);
    }
    DeleteDC(memory);
}

// Where the pointer is, in the VDP's own pixel coordinates. The picture is
// letterboxed and scaled, so this undoes exactly the transform WM_PAINT
// applies - anything else and the shot lands somewhere other than the
// crosshair, which is the one thing a light gun must not do.
void update_light_gun_aim(RuntimeState& state) {
    if (!state.rom_loaded || state.machine != Machine::genesis || state.window == nullptr) {
        return;
    }
    // During netplay the aim is part of the agreed input - see
    // capture_netplay_input and apply_genesis_seats - so the live mouse must
    // not reach the port directly.
    if (state.netplay.active()) {
        return;
    }
    const bool any_gun = light_gun_selected(state);
    if (!any_gun) {
        return;
    }
    // Two guns cannot share the system pointer - there is only one of it - so
    // that case is driven entirely by raw input instead, and this path stands
    // aside rather than fighting it for player one's aim.
    if (two_gun_mode(state) && state.raw_input_ready) {
        update_two_gun_aim(state);
        return;
    }
    POINT at{};
    GetCursorPos(&at);
    ScreenToClient(state.window, &at);
    std::uint32_t gun_x = 0;
    std::uint32_t gun_y = 0;
    // While the reload button is held the gun is pointed away, whatever the
    // pointer says. This has to be re-applied every frame: the aim is rebuilt
    // from the pointer each time round, so a flag set once by the click was
    // being wiped before the machine could read it.
    const bool on_screen = !state.off_screen_held && client_to_gun(state, at, gun_x, gun_y);
    for (int player = 0; player < 2; ++player) {
        if (!kind_is_light_gun(state.players[player].kind)) {
            continue;
        }
        port_for_player(state, player).set_aim(gun_x, gun_y, on_screen);
    }
}

// One frame of emulation, shared by the main loop and by the timer that keeps
// the machine running while Windows has a modal loop of its own open. Holding a
// menu down used to freeze the game; nothing pauses now except the Pause item
// and its hotkey.
constexpr UINT_PTR kModalFrameTimer = 1;

void after_frame(RuntimeState& state);

void step_one_frame(RuntimeState& state) {
    if (state.stepping_frame || !state.rom_loaded || state.paused) {
        return;
    }
    // Pads are polled once per frame, before the machine runs, so a press lands
    // on the same frame the picture is drawn for. The same goes for where a
    // light gun is pointed: the beam sweeps past the aim point during the frame
    // that is about to run, so the position has to be current before it starts.
    poll_gamepads(state);
    update_light_gun_aim(state);

    // A frame can pump messages (the audio device, a fault box), and a timer
    // firing inside that would step the machine twice from one frame's budget.
    state.stepping_frame = true;
    if (state.rewind_held) {
        // Rewinding replaces the frame rather than running one: stepping and
        // then loading an older state would waste the work and, worse, let the
        // sound for a frame that is about to be undone reach the speakers.
        if (!step_rewind(state)) {
            state.rewind_held = false;
        }
        update_framebuffer(state);
    } else {
        // Auto-fire, on player 1's pad only - there is one turbo key, and
        // silently making player 2 fire as well is not what it says.
        if (state.turbo_held && !kind_is_light_gun(state.players[0].kind)) {
            state.turbo_phase = !state.turbo_phase;
            apply_pad_button(state, 0, PadButton::a, state.turbo_phase);
            apply_pad_button(state, 0, PadButton::b, state.turbo_phase);
            apply_pad_button(state, 0, PadButton::c, state.turbo_phase);
        }
        // Netplay decides whether this frame may run at all. Neither machine
        // advances until both players' buttons for it are in hand, which is
        // what keeps them together - and is also why a bad connection shows up
        // as the picture pausing rather than as the two games diverging.
        if (state.netplay.active()) {
            if (!netplay_step(state)) {
                state.netplay_stalled = true;
                refresh_netplay_status(state);
                state.stepping_frame = false;
                return;
            }
            state.netplay_stalled = false;
            refresh_netplay_status(state);
        } else if (state.netplay.status() != mercury::net::Status::idle) {
            // Failed or ended: say so once, then stop asking.
            refresh_netplay_status(state);
        }
        step_until_next_frame(state);
        record_rewind_point(state);
    }
    state.stepping_frame = false;
    after_frame(state);
}

// Frames presented, counted over a rolling second.
//
// Measured at the point a frame is finished rather than derived from the
// target rate, which is the whole reason to have it: a number that always says
// 60 answers nothing. Averaged over a second because a per-frame reciprocal
// jitters far too much to read.
void tick_fps(RuntimeState& state) {
    if (!state.show_fps) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (state.fps_window_start.time_since_epoch().count() == 0) {
        state.fps_window_start = now;
        return;
    }
    ++state.fps_frames;
    const std::chrono::duration<double> elapsed = now - state.fps_window_start;
    if (elapsed.count() < 1.0) {
        return;
    }
    state.fps_value = static_cast<double>(state.fps_frames) / elapsed.count();
    state.fps_frames = 0;
    state.fps_window_start = now;
    update_title(state);
}

// Housekeeping that follows any frame, however it was run. Shared with the
// Gear Link pair, where two consoles each need it.
void after_frame(RuntimeState& state) {
    tick_fps(state);
    drain_logs(state);
    // Flush the cartridge battery a few seconds after the last write rather
    // than only on exit: a crash or a pulled plug should not cost a save the
    // game already thinks it has made.
    if (++state.battery_flush_countdown >= 300) {
        state.battery_flush_countdown = 0;
        flush_battery(state, false);
    }
    if (state.cursor_idle_frames > 0 && --state.cursor_idle_frames == 0) {
        // Hide it now rather than waiting for a mouse message that may never
        // come. This used to call SetCursorPos to the pointer's own position to
        // provoke one, which was self-defeating twice over: moving the pointer
        // to where it already is still posts WM_MOUSEMOVE, and that handler
        // resets this very countdown - so the cursor never actually hid - and
        // the teleport yanked the pointer backwards if it happened to be moving
        // at the time.
        if (should_hide_cursor(state)) {
            SetCursor(nullptr);
        }
    }
    if (state.window != nullptr) {
        InvalidateRect(state.window, nullptr, FALSE);
    }
    if (state.status_frames_remaining > 0 && --state.status_frames_remaining == 0) {
        state.status_message.clear();
        update_title(state);
    }
}

// --- Miracle Piano window ---------------------------------------------------
//
// The Miracle Piano Teaching System shipped with a real MIDI keyboard, and
// without one the game is unusable: it exists to teach you to play, and it
// listens to the instrument to find out whether you did. This is that
// keyboard, on screen.
//
// Drawn rather than blitted from a photograph, so the keys stay sharp at any
// window size and can light up as they are pressed. The photograph of the real
// instrument is used for the Input tab, where it identifies the device.
//
// Touch as well as mouse: a piano that can only sound one note at a time is
// not much of a piano, and a touchscreen is the whole reason this is worth
// having.

constexpr wchar_t kPianoClassName[] = L"GenXMiraclePianoWindow";

// How far the instrument's own level can be pushed. Four times full scale,
// because the keyboard has to be heard over a game that is also playing
// music, and a lesson that says you played it well while nobody could hear
// the note is not teaching anyone anything.
constexpr int kMaxPianoVolume = 400;

struct PianoWindow {
    HWND window = nullptr;
    RuntimeState* owner = nullptr;

    // Geometry, recomputed on resize. White and black keys are hit-tested
    // separately because they overlap: a black key sits on top of two whites
    // and has to win wherever it covers them.
    struct KeyRect {
        int note = 0;    // index into the 49, not a MIDI number
        RECT area{};
        bool black = false;
    };
    std::vector<KeyRect> keys;
    std::array<RECT, 8> buttons{};
    RECT sustain_area{};
    // The instrument's own level, separate from the emulator's. A lesson is
    // the game and the keyboard playing together, and being able to balance
    // the two is the difference between following along and guessing.
    RECT volume_area{};   // the whole row, which is what accepts a click
    RECT volume_track{};  // the groove the knob slides along
    RECT volume_label{};  // the reading, to the right of it
    // Per cent, and it runs past one hundred on purpose - see kMaxVolume.
    // Defaults above full so the instrument is audible over the game
    // without anyone having to find this control first.
    int volume = 150;
    bool dragging_volume = false;

    // Which of the 49 are down, and why. A key held by the mouse and the same
    // key held by a finger must not release early when one of them lifts.
    std::array<int, MiraclePianoKeyCount> holds{};
    int mouse_key = -1;
    std::array<int, 16> touch_key{};   // per touch id, the key it is holding
    std::array<DWORD, 16> touch_id{};
    bool sustain = false;
    int pressed_button = -1;

    // Opening the window plugs the keyboard in, and closing it puts back
    // whatever was in that port before. Without this, someone presses M, gets
    // a piano, plays it and nothing happens - because the port is still a
    // gamepad and the note has nowhere to go.
    bool connected = false;
    PadKind previous_kind = PadKind::three_button;
    InputDevice previous_device = InputDevice::keyboard;
};

PianoWindow g_piano;

// The 49 keys run from C two octaves below middle C. Whether a semitone is a
// black key is a property of where it sits in the octave, nothing more.
bool piano_key_is_black(int key) {
    switch ((key + 0) % 12) {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

int piano_white_index(int key) {
    // How many white keys come before this one.
    int count = 0;
    for (int i = 0; i < key; ++i) {
        if (!piano_key_is_black(i)) {
            ++count;
        }
    }
    return count;
}

int piano_white_count() {
    int count = 0;
    for (int i = 0; i < MiraclePianoKeyCount; ++i) {
        if (!piano_key_is_black(i)) {
            ++count;
        }
    }
    return count;
}

// The instrument's own voice.
//
// The Miracle Piano makes its own sound - it is a real synthesiser, not a
// silent controller - so an on-screen one has to make it too, or a lesson is
// a game asking you to listen to nothing. Two things play through it: the
// keys you press, and the notes the GAME sends back down the cable, which is
// how it demonstrates a piece before asking you to copy it.
//
// Mixed into the emulator's own output rather than opened as a second sound
// device. That keeps one clock and one volume control, and means a WAV
// capture of a lesson contains the lesson.
class PianoSynth {
public:
    static constexpr int kVoices = 24;

    void reset() noexcept {
        for (auto& voice : voices_) {
            voice = Voice{};
        }
    }

    // Up to four times, not one. The keyboard is competing with the game's
    // own music and sound effects, and a lesson is no use if the note you
    // are being asked to copy is quieter than the applause.
    void set_volume(double volume) noexcept { volume_ = std::clamp(volume, 0.0, 4.0); }
    void set_instrument(int instrument) noexcept {
        instrument_ = std::clamp(instrument, 0, 5);
    }
    [[nodiscard]] int instrument() const noexcept { return instrument_; }

    void note_on(int note, int velocity, int instrument) noexcept {
        if (note < 0 || note > 127) {
            return;
        }
        // Retrigger the same note rather than stacking a second voice on it.
        Voice* slot = find(note);
        if (slot == nullptr) {
            slot = steal();
        }
        slot->note = note;
        slot->active = true;
        slot->released = false;
        slot->phase = 0.0;
        slot->lfo = 0.0;
        slot->level = 0.0;
        slot->target = std::clamp(velocity / 127.0, 0.0, 1.0);
        slot->instrument = std::clamp(instrument, 0, 5);
        slot->step = frequency(note) / static_cast<double>(kRate);
    }

    void note_off(int note) noexcept {
        if (Voice* slot = find(note)) {
            slot->released = true;
            // Caught by the pedal if it is down. This is the line that was
            // missing: `pedalled` was declared and tested in the envelope
            // but never once set, so the pedal lit up and changed nothing.
            slot->pedalled = sustain_;
        }
    }

    void set_sustain(bool held) noexcept {
        sustain_ = held;
        if (held) {
            // Pressing the pedal catches whatever is already sounding, the
            // way lifting the dampers on a real piano does.
            for (auto& voice : voices_) {
                if (voice.active) {
                    voice.pedalled = true;
                }
            }
            return;
        }
        // Lifting it drops the dampers: everything the pedal was holding
        // starts releasing now.
        for (auto& voice : voices_) {
            voice.pedalled = false;
        }
    }

    void all_off() noexcept {
        for (auto& voice : voices_) {
            voice.released = true;
        }
    }

    [[nodiscard]] bool idle() const noexcept {
        for (const auto& voice : voices_) {
            if (voice.active) {
                return false;
            }
        }
        return true;
    }

    // Add this many samples' worth of instrument on top of whatever the
    // console is already producing.
    void mix(std::vector<std::int16_t>& out) noexcept {
        if (volume_ <= 0.0 || idle()) {
            return;
        }
        for (auto& sample : out) {
            double total = 0.0;
            for (auto& voice : voices_) {
                if (!voice.active) {
                    continue;
                }
                total += step_voice(voice);
            }
            // Soft limit rather than hard clip. Turning this up to four
            // times is meant to make the instrument carry over the game, and
            // a plain multiply would simply square off the peaks and turn a
            // loud piano into a buzz. tanh leans on the loudest part of the
            // waveform and leaves the rest alone, so the note gets stronger
            // instead of harsher.
            const double driven = total * volume_ * kAmplitude;
            const double limited = 32767.0 * std::tanh(driven / 32767.0);
            const int mixed = static_cast<int>(sample) + static_cast<int>(limited);
            sample = static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
        }
    }

private:
    static constexpr int kRate = static_cast<int>(mercury::gamegear::Psg::kDefaultSampleRate);
    // Full-scale for one voice at 100%. Raised from 9000: the game reported
    // a lesson played well while being too quiet for anyone in the room to
    // hear it, which is the one thing a teaching game cannot afford.
    static constexpr double kAmplitude = 12000.0;

    struct Voice {
        double phase = 0.0;
        double step = 0.0;
        double level = 0.0;
        double target = 0.0;
        double lfo = 0.0;
        int note = -1;
        int instrument = 0;
        bool active = false;
        bool released = false;
        bool pedalled = false;
    };

    static double frequency(int note) noexcept {
        return 440.0 * std::pow(2.0, (note - 69) / 12.0);
    }

    Voice* find(int note) noexcept {
        for (auto& voice : voices_) {
            if (voice.active && voice.note == note && !voice.released) {
                return &voice;
            }
        }
        return nullptr;
    }

    Voice* steal() noexcept {
        Voice* quietest = &voices_[0];
        for (auto& voice : voices_) {
            if (!voice.active) {
                return &voice;
            }
            if (voice.level < quietest->level) {
                quietest = &voice;
            }
        }
        return quietest;
    }

    // The six voices the instrument offers, as waveform plus envelope. Not
    // sampled recordings of the real thing - this is a shape that reads as
    // the right instrument, which is what the games need to be playable.
    static double waveform(double phase, int instrument) noexcept {
        const double t = phase - std::floor(phase);
        const double angle = t * 6.283185307179586;
        switch (instrument) {
            case 1:  // harpsichord: bright and buzzy
                return (2.0 * t - 1.0) * 0.6 + std::sin(angle * 2.0) * 0.25;
            case 2:  // organ: stacked octaves, no decay
                return std::sin(angle) * 0.5 + std::sin(angle * 2.0) * 0.3 +
                       std::sin(angle * 4.0) * 0.2;
            case 3:  // vibraphone: near-pure with a strong second partial
                return std::sin(angle) * 0.8 + std::sin(angle * 4.0) * 0.2;
            case 4:  // electric piano: a bell-like upper partial over a sine
                return std::sin(angle) * 0.7 + std::sin(angle * 3.0) * 0.3;
            case 5:  // synth: square
                return t < 0.5 ? 0.7 : -0.7;
            default:  // piano
                return std::sin(angle) * 0.7 + std::sin(angle * 2.0) * 0.2 +
                       std::sin(angle * 3.0) * 0.1;
        }
    }

    // How fast a note fades once struck, and how fast it goes when let go.
    //
    // The release figures are deliberately gentle. The first version damped a
    // note in under a fifth of a second, which sounded worse than the bug it
    // replaced - notes that never stopped at all were nicer to listen to than
    // notes chopped off. A struck string keeps ringing for a moment after the
    // key comes up, and the ear notices when it does not.
    static void envelope_rates(int instrument, double& decay, double& release) noexcept {
        switch (instrument) {
            case 1: decay = 0.999920; release = 0.99940; break;  // harpsichord: short by nature
            case 2: decay = 1.0;      release = 0.99900; break;  // organ: stops when you stop
            case 3: decay = 0.999975; release = 0.99975; break;  // vibraphone: rings on
            case 4: decay = 0.999965; release = 0.99955; break;  // electric piano
            case 5: decay = 1.0;      release = 0.99920; break;  // synth holds
            default: decay = 0.999970; release = 0.99960; break;  // piano
        }
    }

    double step_voice(Voice& voice) noexcept {
        double decay = 1.0;
        double release = 0.999;
        envelope_rates(voice.instrument, decay, release);

        // A short attack rather than an instant one: a note that starts at
        // full amplitude on the first sample clicks.
        constexpr double kAttack = 0.004;
        if (!voice.released && voice.level < voice.target) {
            voice.level = (std::min)(voice.target, voice.level + kAttack);
        } else if (voice.released && !(sustain_ && voice.pedalled)) {
            voice.level *= release;
        } else {
            voice.level *= decay;
        }

        if (voice.level < 0.0005) {
            voice.active = false;
            voice.level = 0.0;
            return 0.0;
        }

        double sample = waveform(voice.phase, voice.instrument) * voice.level;
        voice.phase += voice.step;
        if (voice.phase >= 1.0) {
            voice.phase -= std::floor(voice.phase);
        }

        // The vibraphone's tremolo, which is most of what makes it one.
        if (voice.instrument == 3) {
            voice.lfo += 5.5 / static_cast<double>(kRate);
            if (voice.lfo >= 1.0) {
                voice.lfo -= 1.0;
            }
            sample *= 0.75 + 0.25 * std::sin(voice.lfo * 6.283185307179586);
        }
        return sample;
    }

    std::array<Voice, kVoices> voices_{};
    double volume_ = 0.7;
    int instrument_ = 0;
    bool sustain_ = false;
};

PianoSynth g_piano_synth;

void mix_piano_audio(RuntimeState& state, std::vector<std::int16_t>& samples) {
    // Whatever the GAME asked the instrument to sound. This is how a lesson
    // demonstrates a piece: it plays the notes through the keyboard rather
    // than through the console's own sound chip, so without this half the
    // game is silent.
    if (state.machine == Machine::genesis &&
        state.genesis.port2().kind() ==
            mercury::genesis::ControllerPort::Kind::miracle_piano) {
        mercury::genesis::MiraclePiano::SoundEvent events[32];
        auto& device = state.genesis.port2().piano();
        // A reset silences the instrument. This is the escape hatch for a
        // note that somehow got stuck: the game resets the keyboard often,
        // so nothing can ring for ever.
        if (device.take_silence_request()) {
            g_piano_synth.all_off();
        }
        while (const auto count = device.take_sound_events(events, 32)) {
            for (std::size_t i = 0; i < count; ++i) {
                const auto& event = events[i];
                if (event.velocity != 0) {
                    g_piano_synth.note_on(event.note, event.velocity,
                                          g_piano_synth.instrument());
                } else {
                    g_piano_synth.note_off(event.note);
                }
            }
            if (count < 32) {
                break;
            }
        }
    }
    // The keyboard's own level on top of the emulator's, so the instrument
    // can be balanced against the game without turning both down.
    const double volume =
        state.muted ? 0.0 : (state.volume / 100.0) * (g_piano.volume / 100.0);
    g_piano_synth.set_volume(volume);
    g_piano_synth.mix(samples);
}

// The keyboard the game is actually talking to, or nothing if the port does
// not have one plugged in. Everything the window does is a no-op without it.
mercury::genesis::MiraclePiano* piano_device(RuntimeState* state) {
    if (state == nullptr || state->machine != Machine::genesis) {
        return nullptr;
    }
    if (state->genesis.port2().kind() !=
        mercury::genesis::ControllerPort::Kind::miracle_piano) {
        return nullptr;
    }
    return &state->genesis.port2().piano();
}

const wchar_t* kPianoButtonLabels[8] = {L"Piano",  L"Harpsi", L"Organ", L"Vibes",
                                        L"E.Piano", L"Synth", L"Up",    L"Down"};

void layout_piano(PianoWindow& piano) {
    RECT client{};
    GetClientRect(piano.window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    // A row of voice buttons, a sustain toggle, then the keyboard filling
    // whatever is left. The keyboard gets the space because it is the part
    // being played.
    const int button_h = (std::max)(28, height / 8);
    const int gap = 4;
    const int button_w = (width - gap * 9) / 8;
    for (int i = 0; i < 8; ++i) {
        piano.buttons[static_cast<std::size_t>(i)] =
            RECT{gap + i * (button_w + gap), gap, gap + i * (button_w + gap) + button_w,
                 gap + button_h};
    }
    // Centred under the voice buttons, the way the pedal sits under the
    // middle of a real instrument rather than off to one side.
    const int sustain_top = gap * 2 + button_h;
    const int sustain_w = button_w * 2 + gap;
    const int sustain_left = (width - sustain_w) / 2;
    piano.sustain_area =
        RECT{sustain_left, sustain_top, sustain_left + sustain_w, sustain_top + button_h};

    // The level, directly under the pedal and the same width, so the two read
    // as one block of controls rather than two stray widgets.
    const int volume_top = sustain_top + button_h + gap;
    const int volume_h = (std::max)(20, button_h * 2 / 3);
    piano.volume_area =
        RECT{sustain_left, volume_top, sustain_left + sustain_w, volume_top + volume_h};
    // The reading sits to the RIGHT of the groove rather than on top of it,
    // so the knob never covers the number it is setting.
    const int label_w = 48;
    piano.volume_label = RECT{piano.volume_area.right - label_w, volume_top,
                              piano.volume_area.right, volume_top + volume_h};
    // Inset by the knob's half-width at each end, so the knob's CENTRE can
    // reach 0% and 100% without the knob itself hanging off the groove.
    const int knob_half = volume_h / 2;
    piano.volume_track = RECT{piano.volume_area.left + knob_half, volume_top,
                              piano.volume_label.left - 10 - knob_half, volume_top + volume_h};

    const int keys_top = volume_top + volume_h + gap * 2;
    const int keys_bottom = height - gap;
    if (keys_bottom <= keys_top) {
        return;
    }

    piano.keys.clear();
    const int whites = piano_white_count();
    const int white_w = (std::max)(6, (width - gap * 2) / whites);
    const int keyboard_w = white_w * whites;
    const int left = (width - keyboard_w) / 2;
    const int white_h = keys_bottom - keys_top;
    const int black_h = (white_h * 3) / 5;
    const int black_w = (std::max)(4, (white_w * 3) / 5);

    // Whites first so the blacks, added after, are hit-tested before them.
    for (int key = 0; key < MiraclePianoKeyCount; ++key) {
        if (piano_key_is_black(key)) {
            continue;
        }
        const int index = piano_white_index(key);
        piano.keys.push_back(PianoWindow::KeyRect{
            key,
            RECT{left + index * white_w, keys_top, left + index * white_w + white_w, keys_bottom},
            false});
    }
    for (int key = 0; key < MiraclePianoKeyCount; ++key) {
        if (!piano_key_is_black(key)) {
            continue;
        }
        // A black key straddles the boundary between the white below it and
        // the next one up.
        const int index = piano_white_index(key);
        const int centre = left + index * white_w;
        piano.keys.push_back(PianoWindow::KeyRect{
            key, RECT{centre - black_w / 2, keys_top, centre + black_w / 2, keys_top + black_h},
            true});
    }
}

void set_piano_volume_from(POINT point) {
    // Against the TRACK, not the whole row: the row includes the reading on
    // the right, and measuring against that would put the knob's 100% short
    // of the end of its own groove.
    const int width = g_piano.volume_track.right - g_piano.volume_track.left;
    if (width <= 0) {
        return;
    }
    const int offset = std::clamp(static_cast<int>(point.x - g_piano.volume_track.left), 0, width);
    g_piano.volume = (offset * kMaxPianoVolume) / width;
    InvalidateRect(g_piano.window, &g_piano.volume_area, FALSE);
}

int piano_hit_test(const PianoWindow& piano, POINT point) {
    // Backwards: the blacks were appended last and sit on top.
    for (auto entry = piano.keys.rbegin(); entry != piano.keys.rend(); ++entry) {
        if (PtInRect(&entry->area, point)) {
            return entry->note;
        }
    }
    return -1;
}

// How hard the key was struck, from where on it the pointer landed.
//
// The real instrument has weighted, velocity-sensitive keys, and the game
// reads that velocity - it is what "play more forcefully" means. There is no
// microphone involved and never was. A pointer has no weight behind it, so
// the striking force has to come from somewhere else, and the nearest thing
// to a real keyboard is how far down the key you hit: near the pivot at the
// back is a weak press, out at the front edge is a firm one.
//
// Almost the whole key strikes at FULL force, with only the top sliver
// softening. Both reference emulators send a flat 100 and their users play
// these lessons, so 100 is known to be playable - but this game was still
// asking to be played harder, and the one number it can be asking about is
// this one. Anything but a deliberately feathered touch now sends 127.
int piano_strike_velocity(const PianoWindow& piano, int key, POINT point) {
    for (const auto& entry : piano.keys) {
        if (entry.note != key) {
            continue;
        }
        const int height = entry.area.bottom - entry.area.top;
        if (height <= 0) {
            break;
        }
        const int depth = std::clamp(static_cast<int>(point.y - entry.area.top), 0, height);
        const double fraction = static_cast<double>(depth) / static_cast<double>(height);
        return std::clamp(static_cast<int>(90.0 + fraction * 150.0), 1, 127);
    }
    return 127;
}

// A key is down while ANY pointer is holding it. Counted rather than flagged
// so a finger lifting does not release a key the mouse is still on.
void piano_hold(PianoWindow& piano, int key, bool down, int velocity = 127) {
    if (key < 0 || key >= MiraclePianoKeyCount) {
        return;
    }
    auto& count = piano.holds[static_cast<std::size_t>(key)];
    const int before = count;
    count = down ? count + 1 : (std::max)(0, count - 1);
    if ((before > 0) == (count > 0)) {
        return;
    }
    if (auto* device = piano_device(g_piano.owner)) {
        device->set_key(key, count > 0, static_cast<std::uint8_t>(velocity));
    }
    // Sound it whether or not a cartridge is listening. The instrument makes
    // its own noise on real hardware, and a keyboard that is silent until a
    // game is loaded would look broken.
    const int note = mercury::genesis::MiraclePiano::kBaseNote + key;
    if (count > 0) {
        // Struck with the same force the game is told about, so what you
        // hear matches what it is judging.
        g_piano_synth.note_on(note, velocity, g_piano_synth.instrument());
    } else {
        g_piano_synth.note_off(note);
    }
    InvalidateRect(piano.window, nullptr, FALSE);
}

void piano_release_all(PianoWindow& piano) {
    for (int key = 0; key < MiraclePianoKeyCount; ++key) {
        if (piano.holds[static_cast<std::size_t>(key)] > 0) {
            piano.holds[static_cast<std::size_t>(key)] = 0;
            if (auto* device = piano_device(g_piano.owner)) {
                device->set_key(key, false);
            }
            // The synthesiser has to be told too. Leaving this out is what
            // made every note ring for ever and read as a stuck sustain
            // pedal: this path runs on EVERY mouse-up, because releasing the
            // capture is itself what sends WM_CAPTURECHANGED, and it cleared
            // the hold count before the ordinary release could act on it.
            g_piano_synth.note_off(mercury::genesis::MiraclePiano::kBaseNote + key);
        }
    }
    piano.mouse_key = -1;
    piano.touch_key.fill(-1);
    piano.touch_id.fill(0);
    InvalidateRect(piano.window, nullptr, FALSE);
}

void paint_piano(PianoWindow& piano, HDC hdc) {
    RECT client{};
    GetClientRect(piano.window, &client);
    const bool dark = piano.owner == nullptr || piano.owner->dark_theme;
    const Theme& theme = dark ? kDarkTheme : kLightTheme;

    // Everything is drawn into a memory bitmap and blitted once. Painting a
    // hundred rectangles straight onto the window flickers.
    HDC memory = CreateCompatibleDC(hdc);
    HBITMAP surface = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    auto* previous = static_cast<HBITMAP>(SelectObject(memory, surface));

    HBRUSH background = CreateSolidBrush(theme.background);
    FillRect(memory, &client, background);
    DeleteObject(background);

    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, theme.text);

    const bool have_device = piano_device(piano.owner) != nullptr;

    // The voice buttons.
    HBRUSH face = CreateSolidBrush(dark ? RGB(70, 70, 76) : RGB(225, 225, 232));
    HBRUSH lit = CreateSolidBrush(RGB(90, 130, 200));
    for (int i = 0; i < 8; ++i) {
        const RECT& area = piano.buttons[static_cast<std::size_t>(i)];
        FillRect(memory, &area, i == piano.pressed_button ? lit : face);
        FrameRect(memory, &area, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        DrawTextW(memory, kPianoButtonLabels[i], -1, const_cast<RECT*>(&area),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    FillRect(memory, &piano.sustain_area, piano.sustain ? lit : face);
    FrameRect(memory, &piano.sustain_area, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    DrawTextW(memory, L"Sustain", -1, &piano.sustain_area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // The level: a groove with a knob on it, drawn rather than borrowed from
    // the system, because everything else in this window is drawn and one
    // themed control among them would be the odd one out.
    const int track_mid = (piano.volume_track.top + piano.volume_track.bottom) / 2;
    RECT groove{piano.volume_track.left, track_mid - 3, piano.volume_track.right, track_mid + 3};
    FillRect(memory, &groove, face);
    const int track_w = piano.volume_track.right - piano.volume_track.left;
    const int knob_x = piano.volume_track.left + (track_w * piano.volume) / kMaxPianoVolume;
    RECT done{groove.left, groove.top, knob_x, groove.bottom};
    FillRect(memory, &done, lit);
    FrameRect(memory, &groove, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    // A notch at 100%, so a control that runs to four hundred still shows
    // where the instrument's natural level is.
    const int unity_x = piano.volume_track.left + track_w / (kMaxPianoVolume / 100);
    RECT notch{unity_x - 1, groove.top - 4, unity_x + 1, groove.bottom + 4};
    FillRect(memory, &notch, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    const int knob_r = (piano.volume_area.bottom - piano.volume_area.top) / 2;
    HBRUSH knob_fill = CreateSolidBrush(dark ? RGB(190, 190, 198) : RGB(250, 250, 252));
    auto* knob_pen = static_cast<HPEN>(SelectObject(
        memory, CreatePen(PS_SOLID, 1, RGB(120, 120, 128))));
    auto* knob_old = static_cast<HBRUSH>(SelectObject(memory, knob_fill));
    Ellipse(memory, knob_x - knob_r, track_mid - knob_r, knob_x + knob_r, track_mid + knob_r);
    SelectObject(memory, knob_old);
    DeleteObject(SelectObject(memory, knob_pen));
    DeleteObject(knob_fill);

    wchar_t level[16];
    std::swprintf(level, 16, L"%d%%", piano.volume);
    DrawTextW(memory, level, -1, &piano.volume_label,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(face);
    DeleteObject(lit);

    // The keys. Whites first, then blacks over them, matching the hit test.
    HBRUSH white = CreateSolidBrush(RGB(248, 248, 246));
    HBRUSH white_down = CreateSolidBrush(RGB(150, 190, 240));
    HBRUSH black = CreateSolidBrush(RGB(24, 24, 26));
    HBRUSH black_down = CreateSolidBrush(RGB(70, 110, 170));
    HBRUSH edge = CreateSolidBrush(RGB(90, 90, 95));
    for (const auto& entry : piano.keys) {
        const bool down = piano.holds[static_cast<std::size_t>(entry.note)] > 0;
        HBRUSH fill = entry.black ? (down ? black_down : black) : (down ? white_down : white);
        FillRect(memory, &entry.area, fill);
        FrameRect(memory, &entry.area, edge);
    }
    DeleteObject(white);
    DeleteObject(white_down);
    DeleteObject(black);
    DeleteObject(black_down);
    DeleteObject(edge);

    (void)have_device;

    BitBlt(hdc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, previous);
    DeleteObject(surface);
    DeleteDC(memory);
}

LRESULT CALLBACK piano_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_SIZE:
            layout_piano(g_piano);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const auto hdc = BeginPaint(hwnd, &paint);
            paint_piano(g_piano, hdc);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetCapture(hwnd);
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            for (int i = 0; i < 8; ++i) {
                if (PtInRect(&g_piano.buttons[static_cast<std::size_t>(i)], point)) {
                    g_piano.pressed_button = i;
                    if (auto* device = piano_device(g_piano.owner)) {
                        device->set_control(
                            static_cast<mercury::genesis::MiraclePiano::Control>(i), true);
                    }
                    // The first six pick a voice, so the on-screen instrument
                    // has to change with them or the buttons do nothing
                    // audible until a game is running.
                    if (i < 6) {
                        g_piano_synth.set_instrument(i);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (PtInRect(&g_piano.volume_area, point)) {
                g_piano.dragging_volume = true;
                set_piano_volume_from(point);
                return 0;
            }
            if (PtInRect(&g_piano.sustain_area, point)) {
                g_piano.sustain = !g_piano.sustain;
                if (auto* device = piano_device(g_piano.owner)) {
                    device->set_sustain(g_piano.sustain);
                }
                g_piano_synth.set_sustain(g_piano.sustain);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const int key = piano_hit_test(g_piano, point);
            if (key >= 0) {
                g_piano.mouse_key = key;
                piano_hold(g_piano, key, true, piano_strike_velocity(g_piano, key, point));
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_piano.dragging_volume && (wparam & MK_LBUTTON) != 0) {
                set_piano_volume_from(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                return 0;
            }
            if ((wparam & MK_LBUTTON) == 0 || g_piano.mouse_key < 0) {
                return 0;
            }
            // Sliding along the keys plays them, the way a finger does on the
            // real thing.
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int key = piano_hit_test(g_piano, point);
            if (key != g_piano.mouse_key) {
                piano_hold(g_piano, g_piano.mouse_key, false);
                g_piano.mouse_key = key;
                if (key >= 0) {
                    piano_hold(g_piano, key, true,
                               piano_strike_velocity(g_piano, key, point));
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            g_piano.dragging_volume = false;
            ReleaseCapture();
            if (g_piano.pressed_button >= 0) {
                if (auto* device = piano_device(g_piano.owner)) {
                    device->set_control(static_cast<mercury::genesis::MiraclePiano::Control>(
                                            g_piano.pressed_button),
                                        false);
                }
                g_piano.pressed_button = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (g_piano.mouse_key >= 0) {
                piano_hold(g_piano, g_piano.mouse_key, false);
                g_piano.mouse_key = -1;
            }
            return 0;
        }
        case WM_TOUCH: {
            // Several fingers at once, which is the entire point of putting
            // this on a touchscreen.
            const UINT count = LOWORD(wparam);
            std::vector<TOUCHINPUT> inputs(count);
            if (GetTouchInputInfo(reinterpret_cast<HTOUCHINPUT>(lparam), count, inputs.data(),
                                  sizeof(TOUCHINPUT))) {
                for (const auto& touch : inputs) {
                    POINT point{TOUCH_COORD_TO_PIXEL(touch.x), TOUCH_COORD_TO_PIXEL(touch.y)};
                    ScreenToClient(hwnd, &point);
                    std::size_t slot = g_piano.touch_id.size();
                    for (std::size_t i = 0; i < g_piano.touch_id.size(); ++i) {
                        if (g_piano.touch_id[i] == touch.dwID) {
                            slot = i;
                            break;
                        }
                    }
                    if ((touch.dwFlags & TOUCHEVENTF_DOWN) != 0) {
                        for (std::size_t i = 0; i < g_piano.touch_id.size(); ++i) {
                            if (g_piano.touch_key[i] < 0) {
                                slot = i;
                                break;
                            }
                        }
                        if (slot < g_piano.touch_id.size()) {
                            const int key = piano_hit_test(g_piano, point);
                            g_piano.touch_id[slot] = touch.dwID;
                            g_piano.touch_key[slot] = key;
                            piano_hold(g_piano, key, true,
                                       piano_strike_velocity(g_piano, key, point));
                        }
                    } else if ((touch.dwFlags & TOUCHEVENTF_UP) != 0) {
                        if (slot < g_piano.touch_id.size()) {
                            piano_hold(g_piano, g_piano.touch_key[slot], false);
                            g_piano.touch_key[slot] = -1;
                            g_piano.touch_id[slot] = 0;
                        }
                    } else if ((touch.dwFlags & TOUCHEVENTF_MOVE) != 0 &&
                               slot < g_piano.touch_id.size()) {
                        const int key = piano_hit_test(g_piano, point);
                        if (key != g_piano.touch_key[slot]) {
                            piano_hold(g_piano, g_piano.touch_key[slot], false);
                            g_piano.touch_key[slot] = key;
                            piano_hold(g_piano, key, true,
                                       piano_strike_velocity(g_piano, key, point));
                        }
                    }
                }
                CloseTouchInputHandle(reinterpret_cast<HTOUCHINPUT>(lparam));
            }
            return 0;
        }
        case WM_KILLFOCUS:
        case WM_CAPTURECHANGED:
            // A key held when the window loses the pointer would sound for
            // ever otherwise.
            piano_release_all(g_piano);
            return 0;
        case WM_CLOSE:
            close_piano_window();
            return 0;
        case WM_DESTROY:
            g_piano.window = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool piano_window_open() { return g_piano.window != nullptr; }
HWND piano_window() { return g_piano.window; }

bool piano_owner_is(const RuntimeState* state) {
    return state != nullptr && g_piano.owner == state;
}

SavedInputChoice piano_saved_input(const RuntimeState& state, int player) {
    SavedInputChoice choice{state.players[player].kind, state.players[player].device};
    if (player == 1 && g_piano.connected && g_piano.owner == &state) {
        choice.kind = g_piano.previous_kind;
        choice.device = g_piano.previous_device;
    }
    return choice;
}

void close_piano_window() {
    if (g_piano.window == nullptr) {
        return;
    }
    piano_release_all(g_piano);
    HWND doomed = g_piano.window;
    g_piano.window = nullptr;
    DestroyWindow(doomed);
    if (g_piano.owner != nullptr) {
        // Put the port back the way it was found.
        if (g_piano.connected) {
            g_piano.owner->players[1].kind = g_piano.previous_kind;
            g_piano.owner->players[1].device = g_piano.previous_device;
            g_piano.connected = false;
            if (g_piano.owner->machine == Machine::genesis) {
                apply_port_kinds(*g_piano.owner);
            }
            save_settings(*g_piano.owner);
        }
        sync_menu_state(*g_piano.owner);
    }
}

void toggle_piano_window(RuntimeState& state) {
    if (g_piano.window != nullptr) {
        close_piano_window();
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = piano_proc;
        window_class.hInstance = state.instance;
        window_class.lpszClassName = kPianoClassName;
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.hbrBackground = nullptr;
        RegisterClassExW(&window_class);
        registered = true;
    }

    g_piano.owner = &state;
    // Plug the keyboard in. Player 2's slot, because the instrument lives in
    // port 2 and player 1 keeps the pad the menus are driven with.
    if (state.players[1].kind != PadKind::miracle_piano) {
        g_piano.previous_kind = state.players[1].kind;
        g_piano.previous_device = state.players[1].device;
        state.players[1].kind = PadKind::miracle_piano;
        // Mouse, not Keyboard: the instrument is pointed at, not typed on,
        // and leaving it as a keyboard device makes the Input tab claim
        // something that is not true.
        state.players[1].device = InputDevice::mouse;
        g_piano.connected = true;
        if (state.machine == Machine::genesis) {
            apply_port_kinds(state);
        }
    }
    g_piano.holds.fill(0);
    g_piano.touch_key.fill(-1);
    g_piano.touch_id.fill(0);
    g_piano.mouse_key = -1;
    g_piano.pressed_button = -1;

    // Wide and short, like the instrument. 49 keys need the width far more
    // than they need the height.
    RECT desired{0, 0, 1000, 320};
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
    g_piano.window = CreateWindowExW(
        0, kPianoClassName, L"GenX - Miracle Piano", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, desired.right - desired.left, desired.bottom - desired.top, nullptr,
        nullptr, state.instance, nullptr);
    if (g_piano.window == nullptr) {
        return;
    }
    // Ask for real touch messages rather than the single emulated mouse
    // Windows would otherwise synthesise, which cannot express a chord.
    RegisterTouchWindow(g_piano.window, 0);
    apply_window_theme(g_piano.window, state.dark_theme);
    layout_piano(g_piano);
    ShowWindow(g_piano.window, SW_SHOW);
    UpdateWindow(g_piano.window);
    sync_menu_state(state);
}

// --- Gear Link -------------------------------------------------------------
//
// Two Game Gears side by side on one screen, joined by an emulated Gear-to-Gear
// cable. The second console is a whole second machine - its own window, its own
// controller bindings, its own sound - living in the same process as the first.
//
// Same process on purpose. The cable is a shared object both consoles write
// their pins into, and the two machines have to be interleaved instruction by
// instruction for a serial bit put on the wire by one to be seen by the other
// at the right moment. Two processes would need shared memory and a
// synchronisation scheme to achieve what a single-threaded loop gets for free.

struct GearLink {
    std::unique_ptr<RuntimeState> second;
    mercury::gamegear::GearLinkSession session;
    LinkView view = LinkView::both;
    // Set when netplay starts on a linked pair: switch to this player's own
    // console the first time the session is running. Not at start, because a
    // joiner does not know which side it is until the host's hello arrives.
    bool view_follows_netplay = false;
    // Where the first window was before the split, so closing the link puts it
    // back rather than leaving it as a half-screen strip.
    WINDOWPLACEMENT saved_placement{sizeof(WINDOWPLACEMENT)};
    bool saved_placement_valid = false;
    // Whether the first window was fullscreen when the link opened, so closing
    // the link puts it back there rather than leaving it windowed.
    bool restore_fullscreen = false;
    bool active = false;
    // The first window. The second console cannot tear the pair down from
    // inside its own window procedure - that would free the state the
    // procedure is running against - so it asks this one to do it.
    HWND primary_window = nullptr;
    // The first console, so a key arriving at either window can be handed to
    // both. Kept as a pointer rather than looked up from the window each time,
    // because the lookup is what breaks if the window is mid-teardown.
    RuntimeState* primary_state = nullptr;
};

GearLink g_gear_link;

bool gear_link_active() {
    return g_gear_link.active && g_gear_link.second != nullptr;
}

LinkView gear_link_view() {
    return gear_link_active() ? g_gear_link.view : LinkView::both;
}

void gear_link_follow_netplay() {
    g_gear_link.view_follows_netplay = true;
}

std::vector<std::uint8_t> gear_link_save_pair() {
    return g_gear_link.session.save_pair_state();
}

bool gear_link_load_pair(const std::vector<std::uint8_t>& blob, std::string& error) {
    return g_gear_link.session.load_pair_state(blob, error);
}

// The console at the OTHER window of a linked pair, or nothing when there is
// no pair.
RuntimeState* link_partner(RuntimeState& state) {
    if (!gear_link_active() || g_gear_link.primary_state == nullptr) {
        return nullptr;
    }
    if (&state == g_gear_link.primary_state) {
        return g_gear_link.second.get();
    }
    if (&state == g_gear_link.second.get()) {
        return g_gear_link.primary_state;
    }
    return nullptr;
}

HWND gear_link_partner_window(const RuntimeState& state) {
    const RuntimeState* partner = link_partner(const_cast<RuntimeState&>(state));
    return partner != nullptr ? partner->window : nullptr;
}

// A key press goes to BOTH consoles of a linked pair, whichever window is
// focused.
//
// Two windows is a presentation choice; two people sitting at one keyboard is
// the situation. Windows sends a key only to the focused window, so without
// this the player whose console is not on top presses keys into nothing, and
// clicking the other half to fix it breaks it for the first player instead.
// Each console applies the key through its OWN bindings, which is what makes
// two separate controller configurations mean anything.
//
// Hotkeys are deliberately NOT broadcast. Save state, pause and the rest act
// on the console being looked at, which is the focused one.
int deliver_key(RuntimeState& state, WPARAM key, bool pressed) {
    int delivered = set_button_from_key(state, key, pressed);
    if (RuntimeState* partner = link_partner(state); partner != nullptr) {
        delivered += set_button_from_key(*partner, key, pressed);
    }
    return delivered;
}

void end_gear_link(RuntimeState& primary);

// Split the first window's monitor down the middle. Both consoles get an equal
// half of the working area, which is what "two Game Gears side by side" means
// on a single screen.
void gear_link_halves(HWND anchor, RECT& left, RECT& right) {
    MONITORINFO monitor{sizeof(MONITORINFO)};
    HMONITOR handle = MonitorFromWindow(anchor, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(handle, &monitor)) {
        monitor.rcWork = RECT{0, 0, 1280, 720};
    }
    const LONG middle = (monitor.rcWork.left + monitor.rcWork.right) / 2;
    left = RECT{monitor.rcWork.left, monitor.rcWork.top, middle, monitor.rcWork.bottom};
    right = RECT{middle, monitor.rcWork.top, monitor.rcWork.right, monitor.rcWork.bottom};
}

void place_window(HWND hwnd, const RECT& rect) {
    if (hwnd == nullptr) {
        return;
    }
    // A window's VISIBLE edge is not where SetWindowPos puts it. Since Aero,
    // several pixels of invisible resize border sit outside the rectangle on
    // the left, right and bottom, so two windows given exact halves of the
    // screen show a gap down the middle and a strip at each outer edge - which
    // is precisely what they did. DwmGetWindowAttribute reports where the
    // painted frame actually lands; the difference between that and the window
    // rectangle is how much to grow by so the visible edges meet.
    RECT target = rect;
    RECT outer{};
    RECT painted{};
    if (GetWindowRect(hwnd, &outer) != 0 &&
        SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &painted,
                                        sizeof(painted)))) {
        target.left -= painted.left - outer.left;
        target.top -= painted.top - outer.top;
        target.right += outer.right - painted.right;
        target.bottom += outer.bottom - painted.bottom;
    }
    SetWindowPos(hwnd, nullptr, target.left, target.top, target.right - target.left,
                 target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Put the chosen view into effect: place and show the console or consoles
// that should be visible, hide and silence the one that should not.
//
// The hidden console is NOT stopped. The cable is a conversation and half a
// conversation deadlocks - which is the whole reason this hides a window
// rather than closing it.
void apply_gear_link_view(LinkView view) {
    if (!gear_link_active()) {
        return;
    }
    auto& primary = *g_gear_link.primary_state;
    auto& second = *g_gear_link.second;
    g_gear_link.view = view;

    RECT left{};
    RECT right{};
    gear_link_halves(primary.window, left, right);
    // The whole area either half came out of, for a console on its own.
    RECT whole{left.left, left.top, right.right, right.bottom};

    const bool show_first = view != LinkView::second;
    const bool show_second = view != LinkView::first;
    primary.hidden_by_view = !show_first;
    second.hidden_by_view = !show_second;

    // Placement before visibility: a window moved while hidden does not
    // flicker across the desktop on its way to the right place.
    if (view == LinkView::both) {
        place_window(primary.window, left);
        place_window(second.window, right);
    } else if (view == LinkView::first) {
        place_window(primary.window, whole);
    } else {
        place_window(second.window, whole);
    }
    ShowWindow(primary.window, show_first ? SW_SHOW : SW_HIDE);
    ShowWindow(second.window, show_second ? SW_SHOW : SW_HIDE);
    if (show_first && !show_second) {
        SetForegroundWindow(primary.window);
    } else if (show_second && !show_first) {
        SetForegroundWindow(second.window);
    }
    // Whatever the hidden one had queued would play out of the speakers after
    // it went away otherwise.
    if (primary.hidden_by_view) {
        primary.pending_audio.clear();
    }
    if (second.hidden_by_view) {
        second.pending_audio.clear();
    }
    sync_menu_state(primary);
    sync_menu_state(second);
}

// Which console the person at THIS machine is playing, over a network. The
// session's own answer, not a second copy of the rule: it is the same one
// Session::exchange uses to decide whose buttons reach which console, so the
// screen shown can never disagree with the pad being read.
LinkView local_console_view(const RuntimeState& state) {
    return state.netplay.local_port_index() == 1 ? LinkView::second : LinkView::first;
}

void begin_gear_link(RuntimeState& primary) {
    if (gear_link_active()) {
        return;
    }
    // The cable is Game Gear hardware. A Mega Drive cartridge in the first
    // window has nothing to plug into.
    if (primary.rom_loaded && primary.machine != Machine::game_gear) {
        MessageBoxW(primary.window,
                    L"Gear Link joins two Game Gears. Close the Mega Drive cartridge first.",
                    L"GenX", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // THE LINK COMES FIRST, THEN THE NETWORK. Netplay makes the two computers
    // identical by having the host send its machine at the start - and for a
    // linked pair that now means both consoles and the cable between them.
    // A link opened after the session started has a second console that was
    // never sent: it resets at a different moment at each end and the pair
    // drifts from its first frame. So this refuses and says the order,
    // instead of warning and letting it go wrong.
    if (primary.netplay.active()) {
        MessageBoxW(primary.window,
                    L"Open the Gear Link before starting netplay, not during it.\n\n"
                    L"Exit netplay, then on BOTH computers: Multiplayer > Gear Link and load "
                    L"the same game. Then one of you starts netplay and the other joins - the "
                    L"host sends both consoles across, so the two ends start identical, and "
                    L"each of you sees your own console.",
                    L"GenX", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Neither of these survives a split screen: a fullscreen or maximized
    // first window cannot be moved to half the desktop until it is an ordinary
    // window again.
    //
    // Leaving fullscreen comes FIRST, and the placement is captured after it.
    // A fullscreen window is a borderless popup covering the monitor, so its
    // placement describes that shape; saving it and applying it later to an
    // ordinary window produced the not-quite-maximized window that came back
    // when the link was closed. Dropping out of fullscreen restores the real
    // windowed shape, and that is the one worth remembering.
    g_gear_link.restore_fullscreen = primary.fullscreen;
    if (primary.fullscreen) {
        set_fullscreen(primary, false);
    }

    // Still before un-maximizing, though. Capturing after it records the
    // restore rectangle with a normal show state, so a window that had been
    // maximized came back at its old restore size instead of maximized.
    g_gear_link.saved_placement.length = sizeof(WINDOWPLACEMENT);
    g_gear_link.saved_placement_valid =
        GetWindowPlacement(primary.window, &g_gear_link.saved_placement) != 0;

    if (IsZoomed(primary.window)) {
        ShowWindow(primary.window, SW_RESTORE);
    }

    auto second = std::make_unique<RuntimeState>();
    second->instance = primary.instance;
    // Its own preferences file, which is the whole point of a second window:
    // a pad bound to console two must not overwrite console one's bindings.
    second->settings_name = L"settings-gearlink2.txt";
    second->link_secondary = true;
    set_default_input(*second);
    set_default_hotkeys(second->hotkeys);
    load_settings(*second);
    // The first window's look, not whatever the second file happened to say -
    // two consoles side by side in different themes looks like a fault.
    second->dark_theme = primary.dark_theme;
    load_logo(*second);
    initialize_audio(*second);

    second->menu = build_menu(*second);
    make_menu_owner_drawn(second->menu, LabelBucket::fixed, second.get());
    refresh_menu_hotkeys(*second);
    rebuild_recent_menu(*second);

    RECT left{};
    RECT right{};
    gear_link_halves(primary.window, left, right);

    HWND hwnd = CreateWindowExW(0, kWindowClassName, L"GenX - Gear Link 2", WS_OVERLAPPEDWINDOW,
                                right.left, right.top, right.right - right.left,
                                right.bottom - right.top, nullptr, second->menu,
                                primary.instance, second.get());
    if (hwnd == nullptr) {
        shutdown_audio(*second);
        MessageBoxW(primary.window, L"Could not open the second Gear Link window.", L"GenX",
                    MB_OK | MB_ICONERROR);
        return;
    }
    second->window = hwnd;
    apply_window_theme(hwnd, second->dark_theme);
    sync_menu_state(*second);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    place_window(primary.window, left);

    g_gear_link.second = std::move(second);
    g_gear_link.primary_window = primary.window;
    g_gear_link.primary_state = &primary;
    g_gear_link.active = true;
    g_gear_link.session.connect(primary.game_gear, g_gear_link.second->game_gear);
    // Now that the link counts as live, both windows can say which console
    // they are and hide the items that belong to the other one.
    sync_menu_state(*g_gear_link.second);
    update_title(*g_gear_link.second);

    // Whatever is already in the first console goes into the second, so a link
    // started with a game running does not need the cartridge chosen twice.
    if (primary.rom_loaded && primary.machine == Machine::game_gear) {
        std::string error;
        if (!load_rom_from_path(*g_gear_link.second, primary.rom_path, error)) {
            MessageBoxW(hwnd, widen(error).c_str(), L"GenX", MB_OK | MB_ICONWARNING);
        }
    }

    sync_menu_state(primary);
    update_title(primary);
}

void end_gear_link(RuntimeState& primary) {
    if (!gear_link_active()) {
        return;
    }
    // A network session built on the pair has nothing left to run once the
    // pair is gone - the far end still has two consoles and this end would
    // have one, and the single-console path drives Mega Drive ports.
    if (primary.netplay.active()) {
        end_netplay(primary);
    }
    g_gear_link.session.disconnect();
    // The first window may be the one that was hidden - "show console 2 only"
    // and then closing console 2's window is the ordinary way to get here.
    // It has to come back, or the program is running with no window at all.
    g_gear_link.view = LinkView::both;
    g_gear_link.view_follows_netplay = false;
    primary.hidden_by_view = false;
    ShowWindow(primary.window, SW_SHOW);

    auto second = std::move(g_gear_link.second);
    g_gear_link.active = false;
    g_gear_link.primary_window = nullptr;
    g_gear_link.primary_state = nullptr;

    // The piano holds a bare pointer to whichever console it is plugged into.
    // If that is the one about to be destroyed, shut the piano first: putting
    // its port back afterwards would be writing through a dangling pointer.
    if (piano_window_open() && piano_owner_is(second.get())) {
        close_piano_window();
    }

    // The second console's own preferences, including whatever pad was bound
    // to it, before its state goes away.
    flush_battery(*second, true);
    save_settings(*second);
    stop_recording(*second);
    stop_fm_log(*second);
    stop_dac_log(*second);
    stop_psg_log(*second);
    shutdown_audio(*second);
    if (second->window != nullptr) {
        // Its menu's brush and its labels go with it. Leaving them behind
        // leaks a GDI object per link and keeps strings alive for menus that
        // no longer exist.
        if (second->menu != nullptr) {
            release_menu_brush(second->menu);
        }
        release_menu_labels(second.get());
        // Destroyed while its RuntimeState is still alive. DestroyWindow sends
        // messages synchronously and the window procedure reads that state, so
        // it has to outlive the window rather than the other way round.
        DestroyWindow(second->window);
        second->window = nullptr;
    }
    if (second->logo_bitmap != nullptr) {
        DeleteObject(second->logo_bitmap);
        second->logo_bitmap = nullptr;
    }
    second.reset();

    if (g_gear_link.saved_placement_valid) {
        SetWindowPlacement(primary.window, &g_gear_link.saved_placement);
        g_gear_link.saved_placement_valid = false;
    }
    // Back to fullscreen last, and only if that is where it came from. Doing it
    // after the placement means set_fullscreen saves the right windowed shape
    // to come back to when fullscreen is left later.
    if (g_gear_link.restore_fullscreen) {
        g_gear_link.restore_fullscreen = false;
        set_fullscreen(primary, true);
    }
    sync_menu_state(primary);
    update_title(primary);
}

// The first window opened a cartridge. Both consoles have to be running the
// same game or there is nothing for the cable to carry.
void gear_link_mirror_rom(RuntimeState& source, const std::filesystem::path& path) {
    if (!gear_link_active() || source.link_secondary) {
        return;
    }
    if (source.machine != Machine::game_gear) {
        // A Mega Drive cartridge in the first window ends the link rather than
        // leaving a second Game Gear window attached to nothing.
        end_gear_link(source);
        return;
    }
    std::string error;
    if (!load_rom_from_path(*g_gear_link.second, path, error)) {
        MessageBoxW(g_gear_link.second->window, widen(error).c_str(), L"GenX",
                    MB_OK | MB_ICONWARNING);
    }
}

void gear_link_mirror_unload(RuntimeState& source) {
    if (!gear_link_active() || source.link_secondary) {
        return;
    }
    unload_rom(*g_gear_link.second);
}

// One frame on both consoles.
//
// The two machines are stepped by the link session rather than by either one's
// own loop, so that neither ever runs more than a single instruction ahead of
// the other. Everything before and after the frame - reading the pads, putting
// the picture up, feeding the audio device - is each window's own business and
// happens twice.
void step_gear_link_frame(RuntimeState& primary) {
    auto& second = *g_gear_link.second;
    if (primary.stepping_frame || second.stepping_frame) {
        return;
    }
    if (!primary.rom_loaded || !second.rom_loaded) {
        // One console without a cartridge: run the one that has it, on its own.
        step_one_frame(primary);
        return;
    }
    if (primary.paused || second.paused) {
        return;
    }

    // Over the network, the two masks come from netplay rather than from two
    // sets of hands at this machine: the far player is console two here and
    // console one at their end. Locally, both consoles read their own pads.
    if (primary.netplay.active()) {
        poll_gamepads(primary);
        if (!gear_link_netplay_step(primary, second)) {
            primary.netplay_stalled = true;
            refresh_netplay_status(primary);
            return;
        }
        primary.netplay_stalled = false;
        // The first frame that actually ran is the first moment both ends are
        // sure which console is whose: a joiner learns it from the host's
        // hello. Switch to this player's own console now, once, and leave the
        // menu free to change it afterwards.
        if (g_gear_link.view_follows_netplay) {
            g_gear_link.view_follows_netplay = false;
            apply_gear_link_view(local_console_view(primary));
        }
    } else {
        for (RuntimeState* console : {&primary, &second}) {
            poll_gamepads(*console);
            update_light_gun_aim(*console);
        }
    }
    for (RuntimeState* console : {&primary, &second}) {
        console->stepping_frame = true;
    }

    const bool completed = g_gear_link.session.run_frame();

    for (RuntimeState* console : {&primary, &second}) {
        console->stepping_frame = false;
    }

    if (!completed) {
        // One of the two stopped producing frames. Say which is not something
        // the session can tell us, so say what is certain and put both windows
        // back under their own control rather than freezing the pair.
        fault(primary, "A Gear Link console stopped producing frames. Link closed.");
        end_gear_link(primary);
        return;
    }

    for (RuntimeState* console : {&primary, &second}) {
        present_game_gear_frame(*console);
        after_frame(*console);
    }
}

// --- new versions ------------------------------------------------------------

// This build's version, from CMakeLists.txt.
constexpr wchar_t kGenxVersion[] = L"" GENX_VERSION;
// Where a new version is downloaded from. Fixed here rather than taken from
// GitHub's reply, so the button can only ever open GenX's own release page.
constexpr wchar_t kReleasesPage[] = L"https://github.com/GamesExObscura/GenX/releases/latest";
// Well clear of WM_APP + 1, which the binding capture already uses.
constexpr UINT WM_APP_UPDATE_CHECKED = WM_APP + 20;

// genx.exe -v or --version. Front ends such as LaunchBox run it to find out
// which version is installed, so it must print one line and leave without a
// window, a sound device or a single file touched.
bool is_version_flag(const wchar_t* argument) {
    return _wcsicmp(argument, L"-v") == 0 || _wcsicmp(argument, L"--version") == 0;
}

// GenX is a windowed program, so normally there is nowhere to print. A caller
// that wants the answer connects a pipe or a file (which is what a .NET
// front end's redirected output is), and that is used first. Failing that,
// the line goes to the console GenX was started from, if there is one.
void print_version() {
    const std::string line = std::string("GenX ") + GENX_VERSION + "\r\n";
    DWORD written = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        const DWORD type = GetFileType(out);
        if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) {
            WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            return;
        }
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
        if (console != INVALID_HANDLE_VALUE) {
            WriteFile(console, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            CloseHandle(console);
        }
        FreeConsole();
    }
}

// What came back from GitHub, handed from the checking thread to the window.
struct UpdateCheck {
    std::wstring latest;  // the newest release's tag, "v1.2.0"; empty if unknown
    bool asked = false;   // Help > Check for updates, rather than at startup
};

// "v1.2.3" or "1.2.3" as three numbers. Anything missing counts as 0, so
// "v1.2" is 1.2.0.
std::array<int, 3> parse_version(const std::wstring& text) {
    std::array<int, 3> parts{};
    std::size_t at = 0;
    if (at < text.size() && (text[at] == L'v' || text[at] == L'V')) {
        ++at;
    }
    for (auto& part : parts) {
        const std::size_t first = at;
        while (at < text.size() && text[at] >= L'0' && text[at] <= L'9' && at - first < 6) {
            part = part * 10 + (text[at] - L'0');
            ++at;
        }
        if (at == first || at >= text.size() || text[at] != L'.') {
            break;
        }
        ++at;
    }
    return parts;
}

// The newest release's tag from GitHub, or empty on any failure - offline, a
// firewall, GitHub down - because none of those is worth more than a shrug at
// startup. Drafts and pre-releases are never "latest", so they never prompt.
std::wstring fetch_latest_release_tag() {
    std::wstring tag;
    const std::wstring agent = std::wstring(L"GenX/") + kGenxVersion;
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        return tag;
    }
    // Short: nobody should wait on this, and it runs off the window's thread.
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET connection =
        WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection == nullptr
                            ? nullptr
                            : WinHttpOpenRequest(connection, L"GET",
                                                 L"/repos/GamesExObscura/GenX/releases/latest",
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (request != nullptr &&
        WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n",
                           static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX) &&
        status == 200) {
        // The reply is a few kilobytes. The cap is only there so a broken
        // proxy cannot feed this forever.
        constexpr std::size_t kMaxReply = 1 << 20;
        std::string body;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0 &&
               body.size() + available <= kMaxReply) {
            const std::size_t old_size = body.size();
            body.resize(old_size + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, body.data() + old_size, available, &read) || read == 0) {
                body.resize(old_size);
                break;
            }
            body.resize(old_size + read);
        }
        // One field is needed, so it is found by hand rather than by a JSON
        // library. tag_name comes before the release notes in the reply, so
        // the first match is the field and never text quoted in the notes.
        const auto key = body.find("\"tag_name\"");
        const auto open = key == std::string::npos ? key : body.find('"', key + 10);
        const auto close = open == std::string::npos ? open : body.find('"', open + 1);
        if (close != std::string::npos && close - open - 1 <= 32) {
            const auto value = body.substr(open + 1, close - open - 1);
            const bool plain = std::all_of(value.begin(), value.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       c == '.' || c == '-' || c == '_';
            });
            if (plain) {
                tag = widen(value);
            }
        }
    }
    if (request != nullptr) {
        WinHttpCloseHandle(request);
    }
    if (connection != nullptr) {
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return tag;
}

// Asks on a thread of its own, so a slow or absent network never holds up the
// window, and posts the answer back to it.
void start_update_check(RuntimeState& state, bool asked) {
    if (state.update_check_running || state.window == nullptr) {
        return;
    }
    state.update_check_running = true;
    HWND window = state.window;
    std::thread([window, asked] {
        auto* result = new UpdateCheck{fetch_latest_release_tag(), asked};
        if (!PostMessageW(window, WM_APP_UPDATE_CHECKED, 0, reinterpret_cast<LPARAM>(result))) {
            delete result;  // the window closed while the request was out
        }
    }).detach();
}

void show_update_result(RuntimeState& state, const UpdateCheck& check) {
    const std::wstring running = std::wstring(L"v") + kGenxVersion;
    if (check.latest.empty()) {
        if (check.asked) {
            MessageBoxW(state.window,
                        L"GenX couldn't reach GitHub to check for a new version. Check your "
                        L"internet connection and try again.",
                        L"GenX", MB_OK | MB_ICONWARNING);
        }
        return;
    }
    if (parse_version(check.latest) <= parse_version(kGenxVersion)) {
        if (check.asked) {
            MessageBoxW(state.window,
                        (L"You have the latest version of GenX (" + running + L").").c_str(),
                        L"GenX", MB_OK | MB_ICONINFORMATION);
        }
        return;
    }
    // The startup check never interrupts a game. One opened before the answer
    // came back is left alone, and the next start asks again.
    if (!check.asked && (state.rom_loaded || state.fullscreen)) {
        return;
    }
    const std::wstring text = L"You have GenX " + running + L".\nGenX " + check.latest +
                              L" is available.\n\nOpen the download page?";
    if (MessageBoxW(state.window, text.c_str(), L"GenX - New version",
                    MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        ShellExecuteW(state.window, L"open", kReleasesPage, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<RuntimeState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            const auto* create_struct = reinterpret_cast<LPCREATESTRUCTW>(lparam);
            auto* create_state = reinterpret_cast<RuntimeState*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create_state));
            return TRUE;
        }
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (state != nullptr && measure != nullptr && measure->CtlType == ODT_MENU) {
                measure_menu_item(*state, measure);
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (state != nullptr && draw != nullptr && draw->CtlType == ODT_MENU) {
                draw_menu_item(*state, draw);
                return TRUE;
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        case WM_COMMAND: {
            if (state == nullptr) {
                return 0;
            }
            const UINT command = LOWORD(wparam);
            if (command >= IDM_FILE_RECENT_FIRST && command <= IDM_FILE_RECENT_LAST) {
                open_recent(*state, static_cast<std::size_t>(command - IDM_FILE_RECENT_FIRST));
                return 0;
            }
            if (command >= IDM_EMU_SAVE_STATE_FIRST && command <= IDM_EMU_SAVE_STATE_LAST) {
                save_state_slot(*state, static_cast<int>(command - IDM_EMU_SAVE_STATE_FIRST) + 1);
                return 0;
            }
            if (command >= IDM_EMU_LOAD_STATE_FIRST && command <= IDM_EMU_LOAD_STATE_LAST) {
                load_state_slot(*state, static_cast<int>(command - IDM_EMU_LOAD_STATE_FIRST) + 1);
                return 0;
            }
            if (command >= IDM_EMU_DELETE_STATE_FIRST && command <= IDM_EMU_DELETE_STATE_LAST) {
                delete_state_slot(*state,
                                  static_cast<int>(command - IDM_EMU_DELETE_STATE_FIRST) + 1);
                return 0;
            }
            if (command == IDM_EMU_DELETE_ALL_STATES) {
                delete_all_states(*state);
                return 0;
            }
            switch (command) {
                case IDM_FILE_OPEN:
                    prompt_open_rom(*state, kSaveGenesis);
                    return 0;
                case IDM_FILE_OPEN_GG:
                    prompt_open_rom(*state, kSaveGameGear);
                    return 0;
                case IDM_FILE_OPEN_32X:
                    prompt_open_rom(*state, kSave32X);
                    return 0;
                case IDM_FILE_OPEN_CD:
                    prompt_open_rom(*state, kSaveCd);
                    return 0;
                case IDM_CD_SWAP:
                    swap_disc(*state);
                    return 0;
                case IDM_CD_BOOT:
                    boot_sega_cd(*state);
                    return 0;
                case IDM_CHEAT_GAME_GENIE:
                    show_game_genie(*state);
                    return 0;
                case IDM_FILE_CLOSE:
                    unload_rom(*state);
                    return 0;
                case IDM_FILE_EXIT:
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                case IDM_EMU_PAUSE:
                    if (state->rom_loaded) {
                        state->paused = !state->paused;
                        sync_menu_state(*state);
                    }
                    return 0;
                case IDM_EMU_RESET_SOFT:
                    soft_reset(*state);
                    return 0;
                case IDM_EMU_RESET_HARD:
                    hard_reset(*state);
                    return 0;
                case IDM_EMU_POWER_OFF:
                    unload_rom(*state);
                    return 0;
                case IDM_EMU_CONFIGURE:
                    show_configure(*state);
                    return 0;
                case IDM_MP_LINK_VIEW_BOTH:
                    apply_gear_link_view(LinkView::both);
                    return 0;
                case IDM_MP_LINK_VIEW_ONE:
                    apply_gear_link_view(LinkView::first);
                    return 0;
                case IDM_MP_LINK_VIEW_TWO:
                    apply_gear_link_view(LinkView::second);
                    return 0;
                case IDM_MP_NETPLAY_START:
                    begin_netplay(*state, true);
                    return 0;
                case IDM_MP_NETPLAY_JOIN:
                    begin_netplay(*state, false);
                    return 0;
                case IDM_MP_NETPLAY_EXIT:
                    end_netplay(*state);
                    return 0;
                case IDM_TOOLS_MIRACLE_PIANO:
                    toggle_piano_window(*state);
                    return 0;
                case IDM_MP_GEAR_LINK:
                    if (!state->link_secondary) {
                        begin_gear_link(*state);
                    }
                    return 0;
                case IDM_MP_GEAR_LINK_CLOSE:
                    // Always run against the FIRST window's state. The second
                    // console's own state is destroyed by this, so it must
                    // never be the object the call is made on.
                    if (!state->link_secondary) {
                        end_gear_link(*state);
                    }
                    return 0;
                case IDM_EMU_REPAIR_GUNS:
                    // Both guns forget which mouse they were. The next one to
                    // move or fire becomes player one, so re-pairing is done by
                    // whoever moves first rather than by picking from a list of
                    // device names nobody can match to a remote in their hand.
                    forget_gun_devices(*state);
                    set_status(*state,
                               L"Light guns unpaired - move or fire player one's mouse first.");
                    return 0;
                case IDM_FILE_RECENT_CLEAR:
                    state->recent.clear();
                    save_recent(*state);
                    rebuild_recent_menu(*state);
                    return 0;
                case IDM_VIEW_SCALE_1X:
                case IDM_VIEW_SCALE_2X:
                case IDM_VIEW_SCALE_3X:
                case IDM_VIEW_SCALE_4X:
                    state->view_scale = static_cast<int>(LOWORD(wparam) - IDM_VIEW_SCALE_1X) + 1;
                    resize_for_content(*state);
                    sync_menu_state(*state);
                    save_settings(*state);
                    return 0;
                case IDM_VIEW_FULLSCREEN:
                    set_fullscreen(*state, !state->fullscreen);
                    return 0;
                case IDM_VIEW_ASPECT_STRETCH:
                case IDM_VIEW_ASPECT_4_3:
                case IDM_VIEW_ASPECT_16_9:
                    state->aspect = LOWORD(wparam) == IDM_VIEW_ASPECT_4_3    ? Aspect::four_three
                                    : LOWORD(wparam) == IDM_VIEW_ASPECT_16_9 ? Aspect::sixteen_nine
                                                                             : Aspect::stretch;
                    resize_for_content(*state);
                    sync_menu_state(*state);
                    save_settings(*state);
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                case IDM_VIEW_ASPECT_CUSTOM:
                    if (DialogBoxParamW(state->instance, MAKEINTRESOURCEW(IDD_ASPECT), hwnd,
                                        aspect_proc,
                                        reinterpret_cast<LPARAM>(state)) == IDOK) {
                        state->aspect = Aspect::custom;
                        resize_for_content(*state);
                        sync_menu_state(*state);
                        save_settings(*state);
                        InvalidateRect(hwnd, nullptr, TRUE);
                    }
                    return 0;
                case IDM_VIEW_CRT:
                    state->crt_filter = !state->crt_filter;
                    sync_menu_state(*state);
                    save_settings(*state);
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                case IDM_VIEW_SMOOTH:
                    state->smooth_scaling = !state->smooth_scaling;
                    sync_menu_state(*state);
                    save_settings(*state);
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                case IDM_VIEW_FPS:
                    state->show_fps = !state->show_fps;
                    // Start the count from now rather than reporting a rate
                    // measured over a window that began before anyone asked.
                    state->fps_frames = 0;
                    state->fps_value = 0.0;
                    state->fps_window_start = std::chrono::steady_clock::now();
                    sync_menu_state(*state);
                    save_settings(*state);
                    update_title(*state);
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                case IDM_VIEW_DARK:
                case IDM_VIEW_LIGHT:
                    state->dark_theme = LOWORD(wparam) == IDM_VIEW_DARK;
                    apply_window_theme(hwnd, state->dark_theme);
                    sync_menu_state(*state);
                    save_settings(*state);
                    return 0;
                case IDM_TOOLS_RECORD_WAV:
                    if (state->recording.is_open()) {
                        stop_recording(*state);
                    } else {
                        start_recording(*state);
                    }
                    sync_menu_state(*state);
                    return 0;
                case IDM_TOOLS_LOG_FM:
                    if (state->fm_log.is_open()) {
                        stop_fm_log(*state);
                    } else {
                        start_fm_log(*state);
                    }
                    sync_menu_state(*state);
                    return 0;
                case IDM_TOOLS_LOG_PSG:
                    if (state->psg_log.is_open()) {
                        stop_psg_log(*state);
                    } else {
                        start_psg_log(*state);
                    }
                    sync_menu_state(*state);
                    return 0;
                case IDM_TOOLS_LOG_DAC:
                    if (state->dac_log.is_open()) {
                        stop_dac_log(*state);
                    } else {
                        start_dac_log(*state);
                    }
                    sync_menu_state(*state);
                    return 0;
                case IDM_TOOLS_DUMP_LOG:
                    write_diagnostic_log(*state);
                    return 0;
                case IDM_TOOLS_DUMP_MEMORY:
                    dump_memory(*state);
                    return 0;
                case IDM_FILE_OPEN_GENX_FOLDER:
                    ShellExecuteW(hwnd, L"open", genx_directory().c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                    return 0;
                case IDM_FILE_OPEN_LOG_FOLDER:
                    ShellExecuteW(hwnd, L"open", log_directory().c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                    return 0;
                case IDM_HELP_CHECK_UPDATES:
                    start_update_check(*state, true);
                    return 0;
                case IDM_TOOLS_MUTE_FM:
                case IDM_TOOLS_MUTE_PSG:
                case IDM_TOOLS_MUTE_PWM:
                case IDM_TOOLS_MUTE_CD:
                case IDM_TOOLS_MUTE_DAC: {
                    const UINT which = LOWORD(wparam);
                    if (which == IDM_TOOLS_MUTE_FM) {
                        state->mute_fm = !state->mute_fm;
                        state->genesis.ym2612().set_fm_muted(state->mute_fm);
                    } else if (which == IDM_TOOLS_MUTE_DAC) {
                        state->mute_dac = !state->mute_dac;
                        state->genesis.ym2612().set_dac_muted(state->mute_dac);
                    } else if (which == IDM_TOOLS_MUTE_PWM) {
                        state->mute_pwm = !state->mute_pwm;
                    } else if (which == IDM_TOOLS_MUTE_CD) {
                        state->mute_cd = !state->mute_cd;
                    } else {
                        state->mute_psg = !state->mute_psg;
                    }
                    sync_menu_state(*state);
                    return 0;
                }
                case IDM_TOOLS_DUMP_VIDEO:
                    if (state->rom_loaded) {
                        dump_video_state(*state);
                    }
                    return 0;
                default:
                    return 0;
            }
        }
        case WM_ENTERMENULOOP:
        case WM_ENTERSIZEMOVE:
            // Windows is about to run its own message loop - a menu, or a window
            // drag - and will not return until the user is done. The main loop
            // is stuck for the duration, so a timer takes over driving frames.
            if (state != nullptr) {
                state->in_modal_loop = true;
                SetTimer(hwnd, kModalFrameTimer, 1, nullptr);
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        case WM_EXITMENULOOP:
        case WM_EXITSIZEMOVE:
            if (state != nullptr) {
                state->in_modal_loop = false;
                KillTimer(hwnd, kModalFrameTimer);
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        case WM_TIMER:
            if (wparam == kModalFrameTimer && state != nullptr) {
                // Paced against the same deadline the main loop uses, so time
                // spent in a menu runs at speed rather than as fast as the timer
                // will fire.
                const auto now = std::chrono::steady_clock::now();
                if (now >= state->next_frame_deadline) {
                    // A Gear Link is driven from the first window only: the two
                    // consoles are one interleaved frame, and the second window
                    // running its own would step a machine outside the session.
                    if (gear_link_active() && !state->link_secondary) {
                        step_gear_link_frame(*state);
                    } else if (!gear_link_active()) {
                        step_one_frame(*state);
                    }
                    state->next_frame_deadline +=
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            kFrameDuration);
                    const auto limit =
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            kFrameDuration * 2);
                    if (now > state->next_frame_deadline + limit) {
                        state->next_frame_deadline = now;
                    }
                }
                return 0;
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            // The trigger, and the off-screen shot a game uses as reload. Both
            // are bindable rows on the Input tab rather than fixed buttons.
            if (state != nullptr) {
                // Recorded before anything else decides to ignore the click:
                // netplay reads the buttons from here (binding_held), and the
                // two-gun path below drops these messages on purpose.
                const bool pressed = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                                     message == WM_MBUTTONDOWN;
                if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
                    state->mouse_left_down = pressed;
                } else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) {
                    state->mouse_right_down = pressed;
                } else {
                    state->mouse_middle_down = pressed;
                }
            }
            if (state != nullptr && state->rom_loaded && state->machine == Machine::genesis) {
                // Two guns are driven per device from WM_INPUT; these merged
                // messages cannot say which mouse was clicked, so acting on
                // them here would fire both players' guns at once.
                if (two_gun_mode(*state) && state->raw_input_ready) {
                    return 0;
                }
                const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                                  message == WM_MBUTTONDOWN;
                const UINT key = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)
                                     ? VK_LBUTTON
                                     : (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
                                           ? VK_RBUTTON
                                           : VK_MBUTTON;
                // Through the ordinary binding lookup, so which button fires and
                // which reloads is whatever the player assigned. A mouse button
                // has a virtual-key code like any other, so this is the same
                // path a keyboard binding takes.
                set_button_from_key(*state, key, down);
                if (down) {
                    SetCapture(hwnd);
                } else {
                    ReleaseCapture();
                }
                return 0;
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        case WM_INPUT_DEVICE_CHANGE:
            // A mouse arrived or left. Only removal matters: a gun holding a
            // slot for a device that has been unplugged would keep a crosshair
            // on screen that nothing can move.
            if (state != nullptr && wparam == GIDC_REMOVAL) {
                release_gun_device(*state, reinterpret_cast<HANDLE>(lparam));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_INPUT:
            // Per-device mouse reports. Only consumed while two guns are
            // configured; the rest of the time the ordinary pointer messages
            // are both sufficient and better behaved, and claiming devices we
            // are not using would assign player two to whichever mouse happened
            // to twitch first.
            if (state != nullptr && two_gun_mode(*state)) {
                handle_raw_mouse(*state, reinterpret_cast<HRAWINPUT>(lparam));
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        case WM_MOUSEMOVE:
            if (state != nullptr) {
                // Any movement brings the pointer back for a couple of seconds,
                // so a fullscreen game is never left without one.
                state->cursor_idle_frames = kCursorIdleFrames;
                if (state->fullscreen) {
                    // The menu bar is not there in fullscreen; pushing the
                    // pointer against the top edge brings it back, and moving
                    // away again takes it away.
                    const int y = GET_Y_LPARAM(lparam);
                    const bool want_menu = y <= kFullscreenMenuReveal;
                    if (want_menu && !state->fullscreen_menu_shown) {
                        state->fullscreen_menu_shown = true;
                        SetMenu(hwnd, state->menu);
                        DrawMenuBar(hwnd);
                        InvalidateRect(hwnd, nullptr, TRUE);
                    } else if (!want_menu && state->fullscreen_menu_shown &&
                               !state->in_modal_loop && y > GetSystemMetrics(SM_CYMENU) + 8) {
                        state->fullscreen_menu_shown = false;
                        SetMenu(hwnd, nullptr);
                        DrawMenuBar(hwnd);
                        InvalidateRect(hwnd, nullptr, TRUE);
                    }
                }
            }
            return 0;
        case WM_SETCURSOR:
            // Only over the client area: the menu bar, the borders and the title
            // bar keep their normal pointers.
            if (state != nullptr && LOWORD(lparam) == HTCLIENT) {
                // A Justifier is aimed, so the pointer becomes the sight. This
                // is checked before the hide rule, which already refuses to
                // hide the pointer while any gun is plugged in - a Menacer
                // still gets an ordinary arrow, and still gets to keep it.
                // Two guns are drawn into the picture, not carried by the
                // pointer, so the pointer itself goes away - leaving it there
                // would put a third mark on screen belonging to neither player.
                // ...but only once a mouse has actually been claimed. Before
                // that there is no painted sight either, and hiding the pointer
                // as well would leave the screen with nothing on it at all
                // until someone happened to move something.
                // Netplay with a gun in this player's seat: both sights are
                // drawn into the picture (draw_netplay_crosshairs), each in its
                // seat's colour, so the pointer goes - for a Menacer as well,
                // whose game draws its own.
                if (state->rom_loaded && state->netplay_seats_applied && state->netplay.active() &&
                    kind_is_light_gun(state->netplay_seat_kinds[static_cast<std::size_t>(
                        state->netplay.local_port_index())])) {
                    SetCursor(nullptr);
                    return TRUE;
                }
                if (state->rom_loaded && two_gun_mode(*state) && state->raw_input_ready &&
                    any_gun_paired(*state)) {
                    SetCursor(nullptr);
                    return TRUE;
                }
                if (state->rom_loaded && crosshair_wanted(*state)) {
                    if (HCURSOR sight = crosshair_cursor(crosshair_size_for(*state))) {
                        SetCursor(sight);
                        return TRUE;
                    }
                }
                // A Menacer game draws its own sight, so ours goes away
                // immediately - not on the idle timer, and regardless of the
                // hide-cursor setting, which is about a pad game being played
                // with the pointer left on screen.
                if (state->rom_loaded && !state->paused && !state->in_modal_loop &&
                    pointer_hidden_for_gun(*state)) {
                    SetCursor(nullptr);
                    return TRUE;
                }
                if (should_hide_cursor(*state)) {
                    SetCursor(nullptr);
                    return TRUE;
                }
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        case WM_INITMENUPOPUP:
            // Which slots hold a state can change while the program runs, so the
            // two slot menus are rebuilt at the moment they are opened rather
            // than once at startup.
            if (state != nullptr) {
                rebuild_state_menus(*state);
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        case WM_SIZE:
            // The logo is centred and scaled to the client area, so a resize
            // has to repaint the whole of it, not just the exposed strip.
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (state != nullptr) {
                const bool repeated = (lparam & (1u << 30)) != 0;
                const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                // Hotkeys first, and they win: a chord that is bound to an
                // action must not also reach a pad bound to the same key. The
                // held ones (turbo, fast forward, rewind) are allowed to repeat
                // harmlessly; the rest would fire over and over on auto-repeat.
                const int action = find_hotkey(*state, static_cast<UINT>(wparam), control, shift, alt);
                if (action >= 0) {
                    if (!repeated || hotkey_is_held(action)) {
                        handle_hotkey_down(*state, static_cast<UINT>(wparam), control, shift, alt);
                    }
                    note_key(*state, static_cast<UINT>(wparam), "taken by a hotkey", 0);
                    return 0;
                }
                // Alt is a menu modifier, not a pad one. Letting a plain pad
                // key through while Alt is down would fire a button as the menu
                // opened.
                if (alt) {
                    note_key(*state, static_cast<UINT>(wparam), "ignored, Alt was down", 0);
                    break;
                }
                if (!state->rom_loaded) {
                    note_key(*state, static_cast<UINT>(wparam), "ignored, no cartridge", 0);
                    return 0;
                }
                if (repeated) {
                    // A held pad button is already down; the auto-repeat stream
                    // tells the machine nothing it does not know.
                    return 0;
                }
                const int reached = deliver_key(*state, wparam, true);
                note_key(*state, static_cast<UINT>(wparam),
                         reached > 0 ? "reached the machine" : "NO BINDING MATCHED", reached);
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (state != nullptr) {
                if (handle_hotkey_up(*state, static_cast<UINT>(wparam))) {
                    return 0;
                }
                deliver_key(*state, wparam, false);
            }
            return 0;
        case WM_KILLFOCUS:
            // Focus moving between the two halves of a Gear Link is not a
            // player letting go of the keyboard - both consoles are being
            // played at once - so releasing their buttons would drop whatever
            // was held at the instant somebody clicked the other window. Focus
            // leaving GenX altogether still releases everything.
            if (state != nullptr) {
                const HWND taking = reinterpret_cast<HWND>(wparam);
                const bool sibling =
                    gear_link_active() && taking != nullptr &&
                    (taking == g_gear_link.primary_window ||
                     (g_gear_link.second != nullptr && taking == g_gear_link.second->window));
                if (!sibling) {
                    clear_all_buttons(*state);
                    if (RuntimeState* partner = link_partner(*state); partner != nullptr) {
                        clear_all_buttons(*partner);
                    }
                }
            }
            return 0;
        case WM_ERASEBKGND:
            // The frame blit below covers the whole client area, so letting
            // Windows erase first would just flash a blank window between the
            // erase and the blit. Claim the erase and do nothing.
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const auto hdc = BeginPaint(hwnd, &paint);
            RECT client_rect{};
            GetClientRect(hwnd, &client_rect);

            // Describe the bitmap with the size bgra_frame was actually BUILT
            // with, never the VDP's live size. A game can change display height
            // mid-run (Mode 4's 224-line variant), and WM_PAINT can land
            // between that change and the next update_framebuffer. Reading the
            // live size here asks GDI for 256x224 out of a buffer still holding
            // 256x192, which walks off the end and repeats the top of the
            // picture along the bottom.
            const std::size_t frame_width = state != nullptr ? state->presented_width : 0;
            const std::size_t frame_height = state != nullptr ? state->presented_height : 0;
            const bool frame_is_consistent =
                state != nullptr && state->rom_loaded && frame_width > 0 && frame_height > 0 &&
                state->bgra_frame.size() >= frame_width * frame_height;

            if (!frame_is_consistent) {
                // No cartridge, or none far enough along to have produced a
                // frame: the themed background with the logo on it. This is the
                // only case that paints a background at all. Filling
                // unconditionally (as this used to) blanked the window on every
                // repaint, and a monitor refresh landing between the fill and
                // the blit is exactly the random black "blink" that shows up
                // during play.
                if (state != nullptr) {
                    draw_idle_screen(*state, hdc, client_rect);
                } else {
                    HBRUSH brush = CreateSolidBrush(kDarkTheme.background);
                    FillRect(hdc, &client_rect, brush);
                    DeleteObject(brush);
                }
            }

            if (frame_is_consistent) {
                const RECT destination = picture_rect(*state, client_rect);

                // Smoothing goes to the graphics card, which paints the bars,
                // the picture and the scanlines in one pass. Measured at
                // 0.14ms a frame against 85ms for the same scale on the
                // processor, and flat however large the window gets.
                if (state->smooth_scaling &&
                    present_smooth_d2d(*state, hwnd, client_rect, destination,
                                       frame_width, frame_height)) {
                    EndPaint(hwnd, &paint);
                    return 0;
                }

                // WITH THE CRT FILTER ON, the picture and the scanlines over
                // it are two separate draws, and drawn straight onto the window
                // the compositor can show the window BETWEEN them: the picture
                // without its scanlines for a refresh, which is the filter
                // flickering on and off (the user's report, 2026-10-03 - the
                // same class of fault as the old erase-then-blit black frame).
                // So with the filter on, everything goes into one off-screen
                // surface and reaches the window in a single copy at the end.
                // With it off there is only one draw, and nothing changes.
                // (The Direct2D path above was never affected: it presents the
                // picture and the scanlines together.)
                HDC canvas = hdc;
                if (state->crt_filter &&
                    ensure_compose_surface(*state, hdc, client_rect.right - client_rect.left,
                                           client_rect.bottom - client_rect.top)) {
                    canvas = state->compose_dc;
                }

                // With a shape chosen the picture no longer covers the client
                // area, so the bars around it have to be painted. Only the bars
                // - painting the whole area first and then the picture over it
                // is the erase-then-draw flash this window has always avoided.
                if (!EqualRect(&destination, &client_rect)) {
                    HBRUSH bar = CreateSolidBrush(RGB(0, 0, 0));
                    const RECT bars[4] = {
                        {client_rect.left, client_rect.top, client_rect.right, destination.top},
                        {client_rect.left, destination.bottom, client_rect.right,
                         client_rect.bottom},
                        {client_rect.left, destination.top, destination.left, destination.bottom},
                        {destination.right, destination.top, client_rect.right,
                         destination.bottom},
                    };
                    for (const auto& strip : bars) {
                        if (strip.right > strip.left && strip.bottom > strip.top) {
                            FillRect(canvas, &strip, bar);
                        }
                    }
                    DeleteObject(bar);
                }

                // Scanlines, drawn over the picture after it lands rather than
                // baked into the frame: the darkening has to be at DISPLAY
                // resolution, not the source's, or a 224-line frame blown up to
                // 900 gives eight-pixel-thick black bands instead of a CRT.
                const auto crt_overlay = [&]() {
                    if (!state->crt_filter) {
                        return;
                    }
                    const int height_px = destination.bottom - destination.top;
                    const int width_px = destination.right - destination.left;
                    if (height_px <= 0 || width_px <= 0) {
                        return;
                    }
                    // Rebuilt only when the size changes; it is the same picture
                    // every frame otherwise.
                    static HBITMAP overlay = nullptr;
                    static int overlay_width = 0;
                    static int overlay_height = 0;
                    if (overlay == nullptr || overlay_width != width_px ||
                        overlay_height != height_px) {
                        if (overlay != nullptr) {
                            DeleteObject(overlay);
                        }
                        BITMAPINFO mask{};
                        mask.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        mask.bmiHeader.biWidth = width_px;
                        mask.bmiHeader.biHeight = -height_px;
                        mask.bmiHeader.biPlanes = 1;
                        mask.bmiHeader.biBitCount = 32;
                        mask.bmiHeader.biCompression = BI_RGB;
                        void* bits = nullptr;
                        overlay = CreateDIBSection(nullptr, &mask, DIB_RGB_COLORS, &bits, nullptr, 0);
                        if (overlay != nullptr && bits != nullptr) {
                            auto* pixels = static_cast<std::uint32_t*>(bits);
                            // Premultiplied: a dark row is black at ~45% alpha,
                            // the rows between it are fully transparent.
                            for (int y = 0; y < height_px; ++y) {
                                const std::uint32_t value = (y & 1) ? 0x73000000u : 0x00000000u;
                                for (int x = 0; x < width_px; ++x) {
                                    pixels[static_cast<std::size_t>(y) * width_px + x] = value;
                                }
                            }
                        }
                        overlay_width = width_px;
                        overlay_height = height_px;
                    }
                    if (overlay == nullptr) {
                        return;
                    }
                    HDC scan_dc = CreateCompatibleDC(hdc);
                    HGDIOBJ previous = SelectObject(scan_dc, overlay);
                    BLENDFUNCTION blend{};
                    blend.BlendOp = AC_SRC_OVER;
                    blend.SourceConstantAlpha = 255;
                    blend.AlphaFormat = AC_SRC_ALPHA;
                    AlphaBlend(canvas, destination.left, destination.top, width_px, height_px, scan_dc,
                               0, 0, width_px, height_px, blend);
                    SelectObject(scan_dc, previous);
                    DeleteDC(scan_dc);
                };

                BITMAPINFO bmi{};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = static_cast<LONG>(frame_width);
                bmi.bmiHeader.biHeight = -static_cast<LONG>(frame_height);
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;

                // Pixel smoothing is a GDI+ job. GDI has no bilinear upscale:
                // its best stretch mode averages source pixels, which does
                // nothing at all when scaling UP, because each destination
                // pixel falls inside a single source pixel. So the smooth path
                // goes through GDI+ and the sharp one stays on the faster GDI
                // blit that has always drawn it.
                const int target_w = destination.right - destination.left;
                const int target_h = destination.bottom - destination.top;
                bool drawn = false;
                if (state->smooth_scaling && gdiplus_session().running &&
                    ensure_smooth_surface(*state, hdc, target_w, target_h)) {
                    Gdiplus::Bitmap source(
                        static_cast<INT>(frame_width), static_cast<INT>(frame_height),
                        static_cast<INT>(frame_width * sizeof(std::uint32_t)),
                        PixelFormat32bppRGB,
                        reinterpret_cast<BYTE*>(state->bgra_frame.data()));
                    if (source.GetLastStatus() == Gdiplus::Ok) {
                        // Into the memory surface, never straight onto the
                        // window: interpolating onto a screen DC takes GDI+
                        // down a path that runs at single-figure frame rates.
                        Gdiplus::Graphics graphics(state->smooth_dc);
                        // Bilinear rather than HighQualityBilinear: the high
                        // quality mode adds a prefilter that only earns its
                        // cost when shrinking, and this only ever enlarges.
                        graphics.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
                        // Without this the sampler is half a pixel out and the
                        // top and left edges pick up a smeared border.
                        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                        // Clamped, so the outermost pixels are not blended
                        // against whatever lies past the edge of the frame.
                        Gdiplus::ImageAttributes attributes;
                        attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
                        const Gdiplus::Rect target(0, 0, target_w, target_h);
                        if (graphics.DrawImage(&source, target, 0, 0,
                                               static_cast<INT>(frame_width),
                                               static_cast<INT>(frame_height),
                                               Gdiplus::UnitPixel, &attributes) == Gdiplus::Ok) {
                            drawn = BitBlt(canvas, destination.left, destination.top, target_w,
                                           target_h, state->smooth_dc, 0, 0, SRCCOPY) != 0;
                        }
                    }
                }
                if (!drawn) {
                    SetStretchBltMode(canvas, COLORONCOLOR);
                    StretchDIBits(
                        canvas,
                        destination.left,
                        destination.top,
                        destination.right - destination.left,
                        destination.bottom - destination.top,
                        0,
                        0,
                        static_cast<int>(frame_width),
                        static_cast<int>(frame_height),
                        state->bgra_frame.data(),
                        &bmi,
                        DIB_RGB_COLORS,
                        SRCCOPY);
                }
                crt_overlay();
                // After the scanline overlay, so the sights sit on top of
                // everything the player is aiming at rather than under it.
                draw_gun_crosshairs(*state, canvas);
                // The one copy to the window, when the frame was put together
                // off screen.
                if (canvas != hdc) {
                    BitBlt(hdc, 0, 0, client_rect.right - client_rect.left,
                           client_rect.bottom - client_rect.top, canvas, 0, 0, SRCCOPY);
                }
            }

            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_APP_UPDATE_CHECKED: {
            std::unique_ptr<UpdateCheck> check(reinterpret_cast<UpdateCheck*>(lparam));
            if (state != nullptr && check != nullptr) {
                state->update_check_running = false;
                show_update_result(*state, *check);
            }
            return 0;
        }
        case WM_DEVICECHANGE:
            // Windows broadcasts this to every top-level window when the
            // device tree changes, with no registration needed for the
            // node-change flavour. That is exactly the moment a controller was
            // plugged in or pulled out, so it is the moment to look again.
            // XInput needs nothing - it polls fixed slots - but a DirectInput
            // pad only exists if an enumeration found it.
            if (wparam == DBT_DEVNODES_CHANGED) {
                rescan_dinput();
            }
            return TRUE;
        case WM_CLOSE:
            if (state != nullptr && state->link_secondary) {
                // Closing the second console closes the pair, but it cannot do
                // that itself: ending the link frees the very state this
                // procedure is running against. Hand it to the first window.
                if (g_gear_link.primary_window != nullptr) {
                    PostMessageW(g_gear_link.primary_window, WM_COMMAND, IDM_MP_GEAR_LINK_CLOSE, 0);
                }
                return 0;
            }
            break;
        case WM_DESTROY:
            // Only the first window's closing ends the program. The second
            // console goes away on its own whenever a Gear Link is closed.
            if (state == nullptr || !state->link_secondary) {
                PostQuitMessage(0);
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    // Reached only by a case that breaks out on purpose - a plain key pressed
    // while Alt is down, which has to go to Windows so the menu still opens.
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    // A path on the command line still works - that is how the sweep scripts and
    // every test launcher start a game - but it is optional now. With no
    // argument the window opens on the logo screen and waits for File > Open.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path startup_rom;
    if (argv != nullptr) {
        // Before anything else, settings included: asking the version must
        // not create the data folder or write a log.
        if (argc >= 2 && is_version_flag(argv[1])) {
            LocalFree(argv);
            print_version();
            return 0;
        }
        if (argc >= 2) {
            startup_rom = argv[1];
        }
        LocalFree(argv);
    }

    RuntimeState state;
    state.instance = instance;
    // Defaults first, then the settings file on top: a file written by an older
    // build has no bindings in it, and the result should be the bindings the
    // runtime has always had rather than a pad that does nothing.
    set_default_input(state);
    set_default_hotkeys(state.hotkeys);
    load_settings(state);
    // Before any cartridge is loaded, including the one that may be on the
    // command line: the firmware is read during load_rom, so a folder applied
    // afterwards would be a load too late.
    apply_bios_folders(state);
    // Before the logo, so pixel smoothing has a live GDI+ from the first
    // painted frame rather than from whenever something else happened to
    // start it.
    gdiplus_session().start();
    load_logo(state);
    initialize_audio(state);

    // DirectInput devices are enumerated once at startup. XInput needs no such
    // step - it is four fixed slots - which is why only this side has an open.
    open_dinput(instance);

    // The tab control, the hotkey list and the volume slider all live in
    // comctl32 and none of them create without this.
    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX),
                                  ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);

    HICON icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               0, 0, LR_DEFAULTSIZE | LR_SHARED));
    HICON icon_small = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), LR_SHARED));

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClassName;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hIcon = icon;
    window_class.hIconSm = icon_small;
    // No background brush: whichever branch of WM_PAINT runs paints every pixel
    // of the client area itself, and a class background would only add an
    // erase-then-draw flash.
    window_class.hbrBackground = nullptr;
    RegisterClassExW(&window_class);

    state.menu = build_menu(state);
    make_menu_owner_drawn(state.menu, LabelBucket::fixed, &state);
    // The shortcut column is written from the live bindings, not from the
    // literals in build_menu - those are only the shape of the label.
    refresh_menu_hotkeys(state);
    load_recent(state);
    rebuild_recent_menu(state);

    RECT desired_rect{0, 0, kIdleWindowWidth, kIdleWindowHeight};
    AdjustWindowRect(&desired_rect, WS_OVERLAPPEDWINDOW, TRUE);
    // A remembered position from the last run, if it is still on a monitor.
    const RECT& saved = state.windowed_placement.rcNormalPosition;
    const bool restore = saved.right > saved.left && saved.bottom > saved.top;
    const int start_x = restore ? saved.left : CW_USEDEFAULT;
    const int start_y = restore ? saved.top : CW_USEDEFAULT;
    const int start_w = restore ? (saved.right - saved.left) : (desired_rect.right - desired_rect.left);
    const int start_h = restore ? (saved.bottom - saved.top) : (desired_rect.bottom - desired_rect.top);

    HWND hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        L"GenX",
        WS_OVERLAPPEDWINDOW,
        start_x,
        start_y,
        start_w,
        start_h,
        nullptr,
        state.menu,
        instance,
        &state);
    if (hwnd == nullptr) {
        MessageBoxW(nullptr, L"Failed to create the runtime window.", L"GenX",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    state.window = hwnd;
    // The Game Genie library, read once. It is not per game and must not be
    // re-read when one is loaded - that is what used to throw away codes
    // typed before a game was opened.
    load_cheat_library(state);
    // Now that there is a window, give it to DirectInput. Until this point the
    // devices are bound to nothing, because the enumeration has to run before
    // the window is created.
    set_dinput_owner(hwnd);
    // Made at startup rather than on first write, so File > Open log folder
    // always has somewhere to go and a fresh install has the folder waiting.
    (void)log_directory();
    // Registered once for the life of the window whether or not two guns are
    // configured, because a failure here has to be known BEFORE the player
    // picks two Justifiers and finds nothing responds.
    register_raw_mice(state);
    apply_window_theme(hwnd, state.dark_theme);
    sync_menu_state(state);

    if (!startup_rom.empty()) {
        std::string error;
        LoadProblem problem = LoadProblem::other;
        // A disc opened from the shell ("Open with GenX") is the likeliest
        // first Sega CD anyone runs, so this asks for the BIOS too.
        if (load_rom_from_path(state, startup_rom, error, &problem) ||
            report_load_failure(state, startup_rom, error, problem)) {
            remember_recent(state, startup_rom);
        }
    }

    ShowWindow(hwnd, state.start_maximized ? SW_SHOWMAXIMIZED : show_command);
    if (state.start_maximized) {
        // Keep the restore size the one that was saved, so un-maximizing goes
        // back to the window's old shape rather than to whatever it was created
        // with.
        state.windowed_placement.showCmd = SW_SHOWMAXIMIZED;
    }
    UpdateWindow(hwnd);
    // Restored last, so entering fullscreen captures a real windowed placement
    // to come back to rather than an empty one.
    if (state.fullscreen) {
        state.fullscreen = false;
        set_fullscreen(state, true);
    }
    // Not when a game was named on the command line: that is a front end such
    // as LaunchBox starting a game, and nothing should pop up over it.
    if (state.check_for_updates && startup_rom.empty()) {
        start_update_check(state, false);
    }

    timeBeginPeriod(1);
    state.next_frame_deadline = std::chrono::steady_clock::now();
    // How far the standalone keyboard's audio has been generated up to. Reset
    // whenever nothing is being generated, so a long pause does not come back
    // owing a minute of sound.
    std::chrono::steady_clock::time_point piano_audio_clock{};

    while (state.running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                state.running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!state.running) {
            break;
        }

        const bool linked = gear_link_active();
        const bool anything_running =
            (state.rom_loaded && !state.paused) ||
            (linked && g_gear_link.second->rom_loaded && !g_gear_link.second->paused);
        if (!anything_running) {
            // The keyboard is playable with no cartridge in, so it needs
            // samples produced for it. Nothing else does: the idle path
            // exists precisely so a logo screen costs no CPU.
            if (piano_window_open()) {
                // Produce exactly as much sound as time has passed for.
                //
                // Two earlier attempts got this wrong. A chunk per pass with
                // a fixed sleep ran at three times real time; gating on the
                // queue length still ran at 1.3, because the queue drains
                // into the device's own buffers faster than the device plays
                // them. Either way the surplus was discarded mid-note, which
                // measured as clean audio - the recording is taken before the
                // discard - and sounded broken. The clock is the only honest
                // source of how many samples are owed.
                const auto now = std::chrono::steady_clock::now();
                if (piano_audio_clock.time_since_epoch().count() == 0) {
                    piano_audio_clock = now;
                }
                const auto owed_time = now - piano_audio_clock;
                auto owed = static_cast<std::int64_t>(
                    std::chrono::duration<double>(owed_time).count() *
                    mercury::gamegear::Psg::kDefaultSampleRate);
                // A window that was dragged or a machine that stalled should
                // not try to catch up with a second of sound in one go.
                constexpr std::int64_t kMaxCatchUp = kAudioChunkSamples * 4;
                if (owed > kMaxCatchUp) {
                    owed = kMaxCatchUp;
                    piano_audio_clock = now;
                } else if (owed > 0) {
                    piano_audio_clock +=
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(
                                static_cast<double>(owed) /
                                mercury::gamegear::Psg::kDefaultSampleRate));
                }
                if (owed > 0) {
                    std::vector<std::int16_t> samples(static_cast<std::size_t>(owed), 0);
                    mix_piano_audio(state, samples);
                    // Recorded like any other output, so a WAV capture of
                    // someone playing the keyboard on its own has the playing
                    // in it, at the right speed.
                    record_samples(state, samples);
                    state.pending_audio.insert(state.pending_audio.end(), samples.begin(),
                                               samples.end());
                }
                service_audio(state);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            piano_audio_clock = {};
            // Idle: nothing to step and nothing to repaint until the user does
            // something. Blocking here keeps the logo screen off the CPU
            // instead of spinning a 60Hz loop over a static picture.
            WaitMessage();
            state.next_frame_deadline = std::chrono::steady_clock::now();
            continue;
        }

        if (linked) {
            // One call runs both consoles. They have to be interleaved
            // instruction by instruction for the cable to carry anything, so
            // neither window may step its own machine.
            step_gear_link_frame(state);
        } else {
            step_one_frame(state);
        }

        if (state.faulted) {
            // A fault used to end the process. In a window with a menu it ends
            // the cartridge instead: report it, drop back to the logo screen,
            // and let the user open something else.
            MessageBoxW(hwnd, widen(state.status_text).c_str(), L"GenX", MB_OK | MB_ICONWARNING);
            unload_rom(state);
            continue;
        }

        // Fast forward shortens the frame budget rather than running extra
        // frames per tick, so the picture still updates every frame instead of
        // showing one in four. The audio queue is capped, so the surplus the
        // chips produce at this speed is dropped rather than backing up.
        const auto budget = state.fast_forward_held ? kFrameDuration / kFastForwardMultiplier
                                                    : kFrameDuration;
        state.next_frame_deadline +=
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(budget);
        // A modal loop the timer could not cover - the Open ROM dialog runs its
        // own pump - still leaves the deadline in the past. Without this,
        // sleep_until returns instantly for hundreds of iterations and the
        // emulator sprints to catch up, producing audio far faster than the
        // device can play it and leaving the sound permanently behind the
        // picture. Past a couple of frames behind, give up the lost time rather
        // than trying to reclaim it.
        const auto now = std::chrono::steady_clock::now();
        const auto catch_up_limit =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(kFrameDuration * 2);
        if (now > state.next_frame_deadline + catch_up_limit) {
            state.next_frame_deadline = now;
        }
        std::this_thread::sleep_until(state.next_frame_deadline);
    }

    // The second console goes first: it has its own battery, its own logs and
    // its own preferences file, and closing the program must not lose them.
    if (gear_link_active()) {
        end_gear_link(state);
    }

    // Position and fullscreen are remembered across runs, so they are written
    // out here as well as when they change - a window that was only ever moved
    // has nothing else to trigger a save.
    flush_battery(state, false);
    // A half-written WAV has a zeroed length field and most players refuse it,
    // so the header is patched even when the program is closed mid-recording.
    stop_recording(state);
    stop_fm_log(state);
    stop_dac_log(state);
    stop_psg_log(state);
    close_dinput();
    save_settings(state);
    shutdown_audio(state);
    timeEndPeriod(1);
    if (state.logo_bitmap != nullptr) {
        DeleteObject(state.logo_bitmap);
    }
    release_smooth_surface(state);
    release_compose_surface(state);
    release_d2d(state);
    // After every GDI+ object made against it has gone.
    gdiplus_session().stop();

    return 0;
}





