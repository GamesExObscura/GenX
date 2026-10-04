#pragma once

// Netplay: two machines, one game, buttons over the wire.
//
// WHAT IS SENT. Only the players' buttons, two bytes a frame each way. No
// video, no audio, no memory. Both ends run the same cartridge on the same
// build and are relied on to reach the same state from the same inputs -
// which determinism_check confirms this emulator does, byte for byte, over
// nine hundred frames on every game tried including the SVP one.
//
// LOCKSTEP, NOT ROLLBACK. RetroArch replays: it accepts input late, rewinds and
// re-simulates, which hides all network latency but re-runs frames constantly
// and fails as a silent desynchronisation. This runs the simpler scheme older
// emulators used - a fixed delay, and a frame does not run until both players'
// buttons for it have arrived. It costs a few frames of input lag and it cannot
// drift, because neither side can advance without the other. Rollback can be
// layered on this same transport later; the reverse is much harder.
//
// SYSTEM AGNOSTIC ON PURPOSE. Nothing here knows what a Mega Drive is. It moves
// a sixteen-bit button mask and a blob of machine state, so a system that does
// not exist yet gets netplay the day its core does. (Since protocol 3 the mask
// travels with a light gun's aim - see PlayerInput - and the host's choice of
// what is plugged into each seat goes over in the greeting.)

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mercury::net {

enum class Role : std::uint8_t { none, host, client };

enum class Status : std::uint8_t {
    idle,
    listening,   // host, waiting for someone to arrive
    connecting,  // client, dialling
    syncing,     // connected; agreeing on the cartridge and copying the state
    running,     // playing
    failed,      // gave up; message says why
    closed       // ended on purpose
};

// What the session needs from the machine it is driving. Supplied by the
// runtime so this file never has to know which machine that is.
struct Hooks {
    // Identifies the cartridge. Both ends must agree or the session is refused
    // rather than allowed to desynchronise five minutes in.
    std::function<std::uint64_t()> rom_fingerprint;
    // The whole machine, for handing a joiner an identical starting point.
    std::function<std::vector<std::uint8_t>()> capture_state;
    // False to refuse, with the reason in `error` - it goes into the message
    // both players see.
    std::function<bool(const std::vector<std::uint8_t>&, std::string& error)> restore_state;
};

struct Peer {
    std::string nickname;
    std::uint32_t round_trip_ms = 0;
};

// ONE PLAYER'S INPUT FOR ONE FRAME - what crosses the wire.
//
// The buttons are the shell's pad rows: bits 0-11 in the Mega Drive's own
// button order, bit 12 a light gun's trigger and bit 13 its reload. A gun's
// aim travels with them, already turned into the VDP's pixel coordinates at
// the end that holds the mouse, so both machines apply exactly the same
// numbers. It used to be the buttons alone, and the aim was read from each
// computer's OWN mouse - so the other player's gun never reached this machine,
// and the two games parted the moment either gun moved.
struct PlayerInput {
    std::uint16_t buttons = 0;
    std::uint16_t gun_x = 0;
    std::uint16_t gun_y = 0;
    bool on_screen = false;

    static constexpr int kTriggerBit = 12;
    static constexpr int kReloadBit = 13;
    [[nodiscard]] bool operator==(const PlayerInput&) const = default;
};

class Session {
  public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Listen on a port and wait for one joiner. Returns false only if the
    // socket could not be opened; waiting happens in the background.
    bool host(std::uint16_t port, const std::string& nickname, const std::string& passphrase,
              Hooks hooks, std::string& error);
    // Dial a host. Also returns immediately.
    bool join(const std::string& address, std::uint16_t port, const std::string& nickname,
              const std::string& passphrase, Hooks hooks, std::string& error);
    void close();

    [[nodiscard]] Role role() const noexcept { return role_; }
    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    // Safe to call from the UI thread at any time.
    [[nodiscard]] std::string message() const;
    [[nodiscard]] Peer peer() const;
    [[nodiscard]] std::uint32_t frame() const noexcept;
    [[nodiscard]] int delay() const noexcept { return delay_; }
    void set_delay(int frames) noexcept;

    // A PRETEND NETWORK, for testing on one computer. Every message this end
    // sends is held back `ms` milliseconds plus up to `jitter_ms` more, in
    // order, the way a real connection delivers them. Loopback delivers in
    // microseconds, so a bug that only shows when one end is waiting on the
    // other - which over the internet is every few frames - never showed on
    // one machine. Off (0) by default; also switched on for both ends by
    // MERCURY_NETPLAY_LATENCY="ms[,jitter]" when a session starts.
    void set_simulated_latency(std::uint32_t ms, std::uint32_t jitter_ms) noexcept {
        latency_ms_ = ms;
        jitter_ms_ = jitter_ms;
    }

    // Which controller port the HOST takes. Set before hosting; the joiner
    // takes the other one and is told which at the handshake, so the two can
    // never disagree about who is holding which pad.
    void set_host_plays_second(bool second) noexcept { host_second_ = second; }
    [[nodiscard]] bool host_plays_second() const noexcept { return host_second_; }
    // Which port THIS end is playing on, once connected. 0 or 1.
    [[nodiscard]] int local_port_index() const noexcept;

    // Once a frame, before the machine runs.
    //
    // Hands in what this player is pressing NOW and gets back what BOTH players
    // pressed `delay` frames ago, which is what the machine should actually be
    // given. Returns false when the peer's buttons for that frame have not
    // arrived yet: the caller must not run the frame, and should come back
    // shortly. That stall is the whole reason this cannot drift.
    bool exchange(std::uint16_t local_now, std::uint16_t& host_input, std::uint16_t& client_input);
    // The same, with a light gun's aim alongside the buttons. `first` and
    // `second` are the two SEATS - port one's player and port two's - not
    // host and joiner.
    bool exchange(const PlayerInput& local_now, PlayerInput& first, PlayerInput& second);

    // WHAT IS PLUGGED INTO EACH SEAT, decided by the host. Two computers whose
    // Input tabs disagree - a six-button pad at one end, a three-button at the
    // other, or a Justifier left selected from the last gun game - are two
    // different consoles running the same cartridge, and they part as surely
    // as two different cartridges would. So the host's setup goes over in the
    // greeting and both ends plug in the same thing. Opaque bytes here: the
    // shell owns what the values mean. Set before host(); read once running.
    void set_seat_kinds(std::uint8_t first, std::uint8_t second) noexcept {
        seat_kinds_[0] = first;
        seat_kinds_[1] = second;
    }
    [[nodiscard]] std::array<std::uint8_t, 2> seat_kinds() const noexcept;

    // Every so often both ends compare a hash of their whole machine. Cheap,
    // and it turns "the games slowly stopped agreeing" into a message that says
    // so on the frame it happens.
    void report_state_hash(std::uint32_t frame, std::uint64_t hash);
    [[nodiscard]] bool desynced() const noexcept;

    // How often the caller should hash its state for the check above.
    static constexpr std::uint32_t kStateCheckInterval = 300;

  private:
    struct Impl;
    Impl* impl_ = nullptr;
    Role role_ = Role::none;
    int delay_ = 4;
    bool host_second_ = false;
    std::uint32_t latency_ms_ = 0;
    std::uint32_t jitter_ms_ = 0;
    std::array<std::uint8_t, 2> seat_kinds_{};
    // Applies the pretend network (see set_simulated_latency) to a session
    // that is starting.
    void start_simulated_latency();
};

// A readable address for the host to pass to the other player. Best effort:
// returns every non-loopback IPv4 address the machine has, since which one is
// reachable is not something this can know.
[[nodiscard]] std::vector<std::string> local_addresses();

// Something short, memorable enough to read down a phone, and random enough to
// be worth using as a key. Used when the host does not supply a passphrase.
[[nodiscard]] std::string generate_passphrase();

}  // namespace mercury::net
