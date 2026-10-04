#include "mercury/net/netplay.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace mercury::net {
namespace {

constexpr std::uint32_t kMagic = 0x584E4547;  // "GENX"
// 2: each frame's input is sent exactly once (see Session::exchange). A
// version 1 end still re-sends while it waits, and paired with a version 2
// end the two would part as surely as before - so they are refused at the
// door instead.
// 3: an input carries a light gun's aim beside the buttons (PlayerInput), and
// the greeting carries the host's choice of what is plugged into each seat.
constexpr std::uint32_t kProtocolVersion = 3;
constexpr std::size_t kKeyBytes = 32;   // AES-256
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kTagBytes = 16;
constexpr std::size_t kSaltBytes = 16;
constexpr std::uint32_t kMaxMessageBytes = 8u * 1024u * 1024u;  // a state blob, with room
constexpr std::size_t kRingSize = 1024;  // frames of input kept; far more than any delay

// Message kinds, inside the encrypted envelope.
enum class Kind : std::uint8_t {
    hello = 1,       // nickname, rom fingerprint, delay
    state = 2,       // host -> client, the whole machine
    ready = 3,       // client -> host, state restored, start counting frames
    input = 4,       // frame, buttons
    state_hash = 5,  // frame, hash
    bye = 6,
};

// --- winsock lifetime -------------------------------------------------------

struct WinsockGuard {
    WinsockGuard() {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockGuard() {
        if (ok) {
            WSACleanup();
        }
    }
    bool ok = false;
};

WinsockGuard& winsock() {
    static WinsockGuard guard;
    return guard;
}

// --- crypto -----------------------------------------------------------------
//
// AES-256-GCM through the operating system's own provider rather than anything
// written here. The key comes from the passphrase the two players share, so the
// session is private from anyone who does not have it and tampering is
// detected rather than played. What it is NOT is protection against someone who
// HAS the passphrase - there is no certificate and no identity here, so a
// passphrase shared carelessly is a session shared carelessly.

bool random_bytes(void* out, std::size_t count) {
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(count),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

bool derive_key(const std::string& passphrase, const std::uint8_t* salt,
                std::array<std::uint8_t, kKeyBytes>& key) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return false;
    }
    // A hundred thousand rounds: the passphrase is short and typed by a person,
    // so the only thing standing between it and a guessing attack is how long
    // each guess takes.
    const auto status = BCryptDeriveKeyPBKDF2(
        algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(passphrase.data())),
        static_cast<ULONG>(passphrase.size()), const_cast<PUCHAR>(salt),
        static_cast<ULONG>(kSaltBytes), 100000, key.data(), static_cast<ULONG>(key.size()), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0;
}

class Cipher {
  public:
    bool open(const std::array<std::uint8_t, kKeyBytes>& key) {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
            return false;
        }
        if (BCryptSetProperty(algorithm_, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
            return false;
        }
        return BCryptGenerateSymmetricKey(algorithm_, &key_, nullptr, 0,
                                          const_cast<PUCHAR>(key.data()),
                                          static_cast<ULONG>(key.size()), 0) == 0;
    }
    ~Cipher() {
        if (key_ != nullptr) {
            BCryptDestroyKey(key_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    bool encrypt(const std::uint8_t* nonce, const std::vector<std::uint8_t>& plain,
                 std::vector<std::uint8_t>& out) const {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = static_cast<ULONG>(kNonceBytes);
        std::array<std::uint8_t, kTagBytes> tag{};
        info.pbTag = tag.data();
        info.cbTag = static_cast<ULONG>(tag.size());
        out.resize(plain.size() + kTagBytes);
        ULONG written = 0;
        const auto status = BCryptEncrypt(
            key_, const_cast<PUCHAR>(plain.data()), static_cast<ULONG>(plain.size()), &info,
            nullptr, 0, out.data(), static_cast<ULONG>(plain.size()), &written, 0);
        if (status != 0) {
            return false;
        }
        std::memcpy(out.data() + plain.size(), tag.data(), tag.size());
        return true;
    }

    bool decrypt(const std::uint8_t* nonce, const std::vector<std::uint8_t>& sealed,
                 std::vector<std::uint8_t>& out) const {
        if (sealed.size() < kTagBytes) {
            return false;
        }
        const std::size_t body = sealed.size() - kTagBytes;
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = static_cast<ULONG>(kNonceBytes);
        info.pbTag = const_cast<PUCHAR>(sealed.data() + body);
        info.cbTag = static_cast<ULONG>(kTagBytes);
        out.resize(body);
        ULONG written = 0;
        // A failure here is a wrong passphrase or a tampered packet, and both
        // are refusals rather than something to carry on through.
        return BCryptDecrypt(key_, const_cast<PUCHAR>(sealed.data()), static_cast<ULONG>(body),
                             &info, nullptr, 0, out.data(), static_cast<ULONG>(body), &written,
                             0) == 0;
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

// --- little serialisation helpers -------------------------------------------

void put8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }
void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
    }
}
void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
    }
}
void put_string(std::vector<std::uint8_t>& out, const std::string& s) {
    const auto size = static_cast<std::uint16_t>(s.size() > 64 ? 64 : s.size());
    put16(out, size);
    out.insert(out.end(), s.begin(), s.begin() + size);
}

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;
    bool ok = true;
    std::uint8_t u8() {
        if (at + 1 > size) { ok = false; return 0; }
        return data[at++];
    }
    std::uint16_t u16() {
        if (at + 2 > size) { ok = false; return 0; }
        const std::uint16_t v = static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8));
        at += 2;
        return v;
    }
    std::uint32_t u32() {
        if (at + 4 > size) { ok = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) { v |= static_cast<std::uint32_t>(data[at + i]) << (i * 8); }
        at += 4;
        return v;
    }
    std::uint64_t u64() {
        if (at + 8 > size) { ok = false; return 0; }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) { v |= static_cast<std::uint64_t>(data[at + i]) << (i * 8); }
        at += 8;
        return v;
    }
    std::string str() {
        const auto n = u16();
        if (!ok || at + n > size) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(data + at), n);
        at += n;
        return s;
    }
    std::vector<std::uint8_t> rest() {
        std::vector<std::uint8_t> out(data + at, data + size);
        at = size;
        return out;
    }
};

bool send_all(SOCKET sock, const std::uint8_t* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const auto n = ::send(sock, reinterpret_cast<const char*>(data + sent),
                              static_cast<int>(size - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_all(SOCKET sock, std::uint8_t* data, std::size_t size) {
    std::size_t got = 0;
    while (got < size) {
        const auto n = ::recv(sock, reinterpret_cast<char*>(data + got),
                              static_cast<int>(size - got), 0);
        if (n <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

// --- the session ------------------------------------------------------------

struct Session::Impl {
    Hooks hooks;
    std::string nickname;
    std::string passphrase;
    Cipher cipher;

    SOCKET listener = INVALID_SOCKET;
    SOCKET peer_sock = INVALID_SOCKET;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<Status> status{Status::idle};
    std::atomic<bool> desync{false};
    std::atomic<std::uint32_t> frame{0};

    mutable std::mutex lock;
    std::string message;
    std::string peer_nickname;

    // Send and receive counters, so no nonce is ever used twice under one key.
    std::atomic<std::uint64_t> send_counter{0};
    std::uint64_t recv_counter = 0;
    std::uint8_t send_direction = 0;
    std::uint8_t recv_direction = 1;

    // Buttons, indexed by frame. Both sides keep both rings so the applied
    // inputs are looked up the same way on each.
    std::array<PlayerInput, kRingSize> local_ring{};
    std::array<PlayerInput, kRingSize> remote_ring{};
    std::array<bool, kRingSize> remote_present{};
    // What is plugged into each seat: the host's own choice, and at the
    // joiner the host's as read from its greeting. Written by the worker
    // before the session reaches `running`, read only after.
    std::array<std::uint8_t, 2> seat_kinds{};
    std::mutex ring_lock;

    // Host's state, waiting to be restored by the client.
    std::vector<std::uint8_t> pending_state;
    std::atomic<bool> have_pending_state{false};
    std::atomic<bool> peer_ready{false};

    // "No hash recorded yet" needs a value that cannot be a frame number.
    // Zero-initialising these made the very first check - which is at frame
    // ZERO - read the empty slot as a recorded hash of zero and report a
    // desynchronisation on two machines that were perfectly in step.
    static constexpr std::uint64_t kNoHash = ~0ULL;
    std::array<std::uint64_t, 64> hash_frames{};
    std::array<std::uint64_t, 64> hash_values{};

    // The last frame whose local input has been recorded and sent, and the
    // last frame of the other player's input received. -1 before the first.
    // See Session::exchange for why these exist.
    std::int64_t local_sent_through = -1;
    std::int64_t remote_received_through = -1;

    // --- the pretend network (Session::set_simulated_latency) ---------------
    // Sealed messages wait here until they are due, then a thread of their own
    // puts them on the wire in order. Only ever used when switched on.
    std::uint32_t sim_latency_ms = 0;
    std::uint32_t sim_jitter_ms = 0;
    std::uint64_t sim_random = 0;
    std::chrono::steady_clock::time_point sim_last_due{};
    struct Delayed {
        std::chrono::steady_clock::time_point due;
        std::vector<std::uint8_t> bytes;
    };
    std::deque<Delayed> sim_queue;
    std::mutex sim_lock;
    std::condition_variable sim_wake;
    std::thread sim_worker;
    bool sim_stop = false;

    void run_sim_worker() {
        for (;;) {
            std::unique_lock<std::mutex> guard(sim_lock);
            sim_wake.wait(guard, [this]() { return sim_stop || !sim_queue.empty(); });
            if (sim_stop) {
                return;
            }
            // The front is always the earliest due: due times never go
            // backwards (see send_message), which is what keeps the order.
            const auto due = sim_queue.front().due;
            if (sim_wake.wait_until(guard, due, [this]() { return sim_stop; })) {
                return;
            }
            auto item = std::move(sim_queue.front());
            sim_queue.pop_front();
            guard.unlock();
            if (!send_all(peer_sock, item.bytes.data(), item.bytes.size())) {
                return;
            }
        }
    }

    void set_message(std::string text) {
        std::lock_guard<std::mutex> guard(lock);
        message = std::move(text);
    }

    void fail(std::string text) {
        set_message(std::move(text));
        status = Status::failed;
    }

    bool send_message(Kind kind, const std::vector<std::uint8_t>& body) {
        if (peer_sock == INVALID_SOCKET) {
            return false;
        }
        std::vector<std::uint8_t> plain;
        plain.reserve(body.size() + 1);
        plain.push_back(static_cast<std::uint8_t>(kind));
        plain.insert(plain.end(), body.begin(), body.end());

        // Numbering, sealing and sending under ONE lock. The receiver expects
        // the counters in order and rejects anything else as tampering, so two
        // threads that took numbers 5 and 6 and then raced to the socket would
        // end the session. Nothing sends from two threads at once today, but
        // that is an accident of who talks when, not a guarantee.
        std::lock_guard<std::mutex> guard(send_lock);
        const std::uint64_t counter = send_counter.fetch_add(1);
        std::array<std::uint8_t, kNonceBytes> nonce{};
        nonce[0] = send_direction;
        std::memcpy(nonce.data() + 4, &counter, sizeof(counter));

        std::vector<std::uint8_t> sealed;
        if (!cipher.encrypt(nonce.data(), plain, sealed)) {
            return false;
        }
        std::array<std::uint8_t, 4> header{};
        const auto size = static_cast<std::uint32_t>(sealed.size());
        for (int i = 0; i < 4; ++i) {
            header[i] = static_cast<std::uint8_t>(size >> (i * 8));
        }
        if (sim_latency_ms == 0 && sim_jitter_ms == 0) {
            return send_all(peer_sock, header.data(), header.size()) &&
                   send_all(peer_sock, sealed.data(), sealed.size());
        }
        // The pretend network. Each message is due `latency` plus a random
        // share of `jitter` from now - but never before the one sent ahead of
        // it, because a real connection delivers in order and a burst of late
        // messages arriving together is exactly what a real one looks like.
        sim_random = sim_random * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto extra = sim_jitter_ms == 0 ? 0u
                                              : static_cast<std::uint32_t>((sim_random >> 33) %
                                                                           (sim_jitter_ms + 1));
        auto due = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(sim_latency_ms + extra);
        if (due < sim_last_due) {
            due = sim_last_due;
        }
        sim_last_due = due;
        Delayed item;
        item.due = due;
        item.bytes.assign(header.begin(), header.end());
        item.bytes.insert(item.bytes.end(), sealed.begin(), sealed.end());
        {
            std::lock_guard<std::mutex> queue_guard(sim_lock);
            sim_queue.push_back(std::move(item));
        }
        sim_wake.notify_one();
        return true;
    }
    std::mutex send_lock;

    bool receive_message(Kind& kind, std::vector<std::uint8_t>& body) {
        std::array<std::uint8_t, 4> header{};
        if (!recv_all(peer_sock, header.data(), header.size())) {
            return false;
        }
        std::uint32_t size = 0;
        for (int i = 0; i < 4; ++i) {
            size |= static_cast<std::uint32_t>(header[i]) << (i * 8);
        }
        if (size == 0 || size > kMaxMessageBytes) {
            return false;
        }
        std::vector<std::uint8_t> sealed(size);
        if (!recv_all(peer_sock, sealed.data(), sealed.size())) {
            return false;
        }
        std::array<std::uint8_t, kNonceBytes> nonce{};
        nonce[0] = recv_direction;
        std::memcpy(nonce.data() + 4, &recv_counter, sizeof(recv_counter));
        ++recv_counter;

        std::vector<std::uint8_t> plain;
        if (!cipher.decrypt(nonce.data(), sealed, plain) || plain.empty()) {
            return false;
        }
        kind = static_cast<Kind>(plain[0]);
        body.assign(plain.begin() + 1, plain.end());
        return true;
    }

    // Everything after the socket is connected: agree on the cartridge, copy
    // the machine across, then read buttons until told to stop.
    // Which port the host takes. The host decides and the joiner is told, so
    // the two cannot end up both thinking they are player one.
    std::atomic<bool> host_second{false};

    void run_connected(bool as_host, int delay) {
        // Hello, both ways.
        std::vector<std::uint8_t> hello;
        put_string(hello, nickname);
        put64(hello, hooks.rom_fingerprint ? hooks.rom_fingerprint() : 0);
        put32(hello, static_cast<std::uint32_t>(delay));
        put8(hello, host_second ? 1 : 0);
        put8(hello, seat_kinds[0]);
        put8(hello, seat_kinds[1]);
        if (!send_message(Kind::hello, hello)) {
            fail("could not greet the other player");
            return;
        }
        Kind kind{};
        std::vector<std::uint8_t> body;
        if (!receive_message(kind, body) || kind != Kind::hello) {
            fail("the other side did not answer, or the passphrase does not match");
            return;
        }
        {
            Reader reader{body.data(), body.size()};
            const auto their_name = reader.str();
            const auto their_rom = reader.u64();
            (void)reader.u32();
            const auto their_host_second = reader.u8();
            const auto their_first_seat = reader.u8();
            const auto their_second_seat = reader.u8();
            if (!reader.ok) {
                fail("the greeting was malformed");
                return;
            }
            if (!as_host) {
                // The host's choice wins - it is the one that was made. Both
                // which seat it sits in and what is plugged into each.
                host_second = their_host_second != 0;
                seat_kinds = {their_first_seat, their_second_seat};
            }
            const auto our_rom = hooks.rom_fingerprint ? hooks.rom_fingerprint() : 0;
            if (their_rom != our_rom) {
                // Refused here rather than five minutes in, when it would look
                // like a network fault instead of a different cartridge. The
                // fingerprint also says whether a Gear Link is open, because
                // a pair and a lone console are different machines even with
                // the same cartridge in - so the message names both causes.
                fail("you are not both running the same thing - it has to be the same "
                     "cartridge, and for Game Gear both of you need a Gear Link open");
                return;
            }
            std::lock_guard<std::mutex> guard(lock);
            peer_nickname = their_name.empty() ? std::string("player") : their_name;
        }

        // The host hands over its whole machine so the two start identical. It
        // removes every "we started from slightly different places" failure,
        // and means the joiner does not have to be at the same point in the
        // game - or even have started it.
        if (as_host) {
            status = Status::syncing;
            set_message("sending the game state...");
            auto blob = hooks.capture_state ? hooks.capture_state() : std::vector<std::uint8_t>{};
            if (!send_message(Kind::state, blob)) {
                fail("could not send the game state");
                return;
            }
            if (!receive_message(kind, body) || kind != Kind::ready) {
                fail("the other player could not load the game state");
                return;
            }
        } else {
            status = Status::syncing;
            set_message("receiving the game state...");
            if (!receive_message(kind, body) || kind != Kind::state) {
                fail("no game state arrived");
                return;
            }
            {
                std::lock_guard<std::mutex> guard(ring_lock);
                pending_state = body;
            }
            have_pending_state = true;
            // The main thread restores it - touching the machine from here
            // would be a race against the frame currently running.
            while (!stop && have_pending_state) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (stop) {
                return;
            }
            if (!send_message(Kind::ready, {})) {
                fail("could not confirm the game state");
                return;
            }
        }

        {
            std::lock_guard<std::mutex> guard(ring_lock);
            local_ring.fill(PlayerInput{});
            remote_ring.fill(PlayerInput{});
            remote_present.fill(false);
            hash_frames.fill(kNoHash);
            hash_values.fill(0);
            local_sent_through = -1;
            remote_received_through = -1;
        }
        frame = 0;
        peer_ready = true;
        status = Status::running;
        set_message("connected");

        while (!stop) {
            if (!receive_message(kind, body)) {
                if (!stop) {
                    fail("the connection dropped");
                }
                return;
            }
            if (kind == Kind::input) {
                Reader reader{body.data(), body.size()};
                const auto at = reader.u32();
                PlayerInput input;
                input.buttons = reader.u16();
                input.gun_x = reader.u16();
                input.gun_y = reader.u16();
                input.on_screen = (reader.u8() & 1) != 0;
                if (!reader.ok) {
                    continue;
                }
                std::lock_guard<std::mutex> guard(ring_lock);
                // A frame already received is never taken twice. The first
                // copy may already have been played, and a second one would
                // also leave the slot marked present for the frame that
                // reuses it a whole ring later. Since protocol 2 no sender
                // repeats a frame; this keeps that true whatever arrives.
                if (static_cast<std::int64_t>(at) <= remote_received_through) {
                    continue;
                }
                remote_received_through = at;
                remote_ring[at % kRingSize] = input;
                remote_present[at % kRingSize] = true;
            } else if (kind == Kind::state_hash) {
                Reader reader{body.data(), body.size()};
                const auto at = reader.u32();
                const auto value = reader.u64();
                if (!reader.ok) {
                    continue;
                }
                std::lock_guard<std::mutex> guard(ring_lock);
                const auto slot = (at / Session::kStateCheckInterval) % hash_frames.size();
                if (hash_frames[slot] == at && hash_values[slot] != value) {
                    desync = true;
                    set_message("the two games have drifted apart");
                } else {
                    hash_frames[slot] = at;
                    hash_values[slot] = value;
                }
            } else if (kind == Kind::bye) {
                set_message("the other player left");
                status = Status::closed;
                return;
            }
        }
    }
};

Session::Session() : impl_(new Impl()) {}

Session::~Session() {
    close();
    delete impl_;
}

Status Session::status() const noexcept { return impl_->status.load(); }
bool Session::active() const noexcept {
    const auto s = impl_->status.load();
    return s == Status::listening || s == Status::connecting || s == Status::syncing ||
           s == Status::running;
}
std::string Session::message() const {
    std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->message;
}
Peer Session::peer() const {
    std::lock_guard<std::mutex> guard(impl_->lock);
    Peer out;
    out.nickname = impl_->peer_nickname;
    return out;
}
std::uint32_t Session::frame() const noexcept { return impl_->frame.load(); }
bool Session::desynced() const noexcept { return impl_->desync.load(); }

void Session::set_delay(int frames) noexcept {
    delay_ = frames < 1 ? 1 : (frames > 20 ? 20 : frames);
}

void Session::start_simulated_latency() {
    auto& impl = *impl_;
    std::uint32_t latency = latency_ms_;
    std::uint32_t jitter = jitter_ms_;
    // "ms" or "ms,jitter" - so two copies of the GUI on one computer can be
    // made to behave like two computers on a real connection.
    if (latency == 0 && jitter == 0) {
        if (const char* e = std::getenv("MERCURY_NETPLAY_LATENCY"); e != nullptr && *e != '\0') {
            char* end = nullptr;
            latency = static_cast<std::uint32_t>(std::strtoul(e, &end, 10));
            if (end != nullptr && *end == ',') {
                jitter = static_cast<std::uint32_t>(std::strtoul(end + 1, nullptr, 10));
            }
        }
    }
    impl.sim_latency_ms = latency;
    impl.sim_jitter_ms = jitter;
    impl.sim_random = 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(impl.send_direction) << 32);
    impl.sim_last_due = std::chrono::steady_clock::time_point{};
    {
        std::lock_guard<std::mutex> guard(impl.sim_lock);
        impl.sim_queue.clear();
        impl.sim_stop = false;
    }
    if (latency != 0 || jitter != 0) {
        impl.sim_worker = std::thread([&impl]() { impl.run_sim_worker(); });
    }
}

bool Session::host(std::uint16_t port, const std::string& nickname, const std::string& passphrase,
                   Hooks hooks, std::string& error) {
    if (!winsock().ok) {
        error = "Windows networking failed to start";
        return false;
    }
    close();
    role_ = Role::host;
    impl_->hooks = std::move(hooks);
    impl_->nickname = nickname;
    impl_->passphrase = passphrase;
    impl_->send_direction = 0;
    impl_->recv_direction = 1;

    impl_->listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listener == INVALID_SOCKET) {
        error = "could not open a socket";
        return false;
    }
    BOOL reuse = TRUE;
    ::setsockopt(impl_->listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse),
                 sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (::bind(impl_->listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(impl_->listener, 1) != 0) {
        ::closesocket(impl_->listener);
        impl_->listener = INVALID_SOCKET;
        error = "could not listen on port " + std::to_string(port) +
                " - something else may already be using it";
        return false;
    }

    impl_->host_second = host_second_;
    impl_->seat_kinds = seat_kinds_;
    impl_->status = Status::listening;
    impl_->set_message("waiting for the other player...");
    impl_->stop = false;
    start_simulated_latency();
    const int delay = delay_;
    impl_->worker = std::thread([this, delay]() {
        auto& impl = *impl_;
        SOCKET accepted = ::accept(impl.listener, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            if (!impl.stop) {
                impl.fail("nobody arrived");
            }
            return;
        }
        impl.peer_sock = accepted;
        BOOL nodelay = TRUE;
        ::setsockopt(impl.peer_sock, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<char*>(&nodelay), sizeof(nodelay));

        // The host picks the salt and sends it in the clear. A salt is not a
        // secret - it exists so the same passphrase does not produce the same
        // key twice - and the passphrase itself never crosses the wire.
        std::array<std::uint8_t, kSaltBytes> salt{};
        if (!random_bytes(salt.data(), salt.size())) {
            impl.fail("could not generate a session key");
            return;
        }
        std::array<std::uint8_t, 12> header{};
        std::memcpy(header.data(), &kMagic, 4);
        std::memcpy(header.data() + 4, &kProtocolVersion, 4);
        const std::uint32_t salt_size = kSaltBytes;
        std::memcpy(header.data() + 8, &salt_size, 4);
        if (!send_all(impl.peer_sock, header.data(), header.size()) ||
            !send_all(impl.peer_sock, salt.data(), salt.size())) {
            impl.fail("the connection dropped during setup");
            return;
        }
        std::array<std::uint8_t, kKeyBytes> key{};
        if (!derive_key(impl.passphrase, salt.data(), key) || !impl.cipher.open(key)) {
            impl.fail("could not set up encryption");
            return;
        }
        impl.run_connected(true, delay);
    });
    return true;
}

bool Session::join(const std::string& address, std::uint16_t port, const std::string& nickname,
                   const std::string& passphrase, Hooks hooks, std::string& error) {
    if (!winsock().ok) {
        error = "Windows networking failed to start";
        return false;
    }
    close();
    role_ = Role::client;
    impl_->hooks = std::move(hooks);
    impl_->nickname = nickname;
    impl_->passphrase = passphrase;
    impl_->send_direction = 1;
    impl_->recv_direction = 0;
    // Replaced by the host's from its greeting; sent only so the greeting
    // has one shape both ways.
    impl_->seat_kinds = seat_kinds_;
    impl_->status = Status::connecting;
    impl_->set_message("connecting...");
    impl_->stop = false;
    start_simulated_latency();

    const int delay = delay_;
    const std::string host_text = address;
    impl_->worker = std::thread([this, host_text, port, delay]() {
        auto& impl = *impl_;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* found = nullptr;
        if (::getaddrinfo(host_text.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 ||
            found == nullptr) {
            impl.fail("could not find " + host_text);
            return;
        }
        SOCKET sock = ::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
        if (sock == INVALID_SOCKET) {
            ::freeaddrinfo(found);
            impl.fail("could not open a socket");
            return;
        }
        if (::connect(sock, found->ai_addr, static_cast<int>(found->ai_addrlen)) != 0) {
            ::freeaddrinfo(found);
            ::closesocket(sock);
            impl.fail("could not reach " + host_text + " on port " + std::to_string(port));
            return;
        }
        ::freeaddrinfo(found);
        impl.peer_sock = sock;
        BOOL nodelay = TRUE;
        ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nodelay),
                     sizeof(nodelay));

        std::array<std::uint8_t, 12> header{};
        if (!recv_all(sock, header.data(), header.size())) {
            impl.fail("the host did not answer");
            return;
        }
        std::uint32_t magic = 0, version = 0, salt_size = 0;
        std::memcpy(&magic, header.data(), 4);
        std::memcpy(&version, header.data() + 4, 4);
        std::memcpy(&salt_size, header.data() + 8, 4);
        if (magic != kMagic) {
            impl.fail("that is not a GenX netplay host");
            return;
        }
        if (version != kProtocolVersion) {
            impl.fail("the two sides are running different versions of GenX");
            return;
        }
        if (salt_size != kSaltBytes) {
            impl.fail("the host's setup message was malformed");
            return;
        }
        std::array<std::uint8_t, kSaltBytes> salt{};
        if (!recv_all(sock, salt.data(), salt.size())) {
            impl.fail("the connection dropped during setup");
            return;
        }
        std::array<std::uint8_t, kKeyBytes> key{};
        if (!derive_key(impl.passphrase, salt.data(), key) || !impl.cipher.open(key)) {
            impl.fail("could not set up encryption");
            return;
        }
        impl.run_connected(false, delay);
    });
    return true;
}

void Session::close() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->status == Status::running) {
        impl_->send_message(Kind::bye, {});
    }
    impl_->stop = true;
    // The pretend network's sender first: it may be about to write to the
    // socket that is closed below.
    {
        std::lock_guard<std::mutex> guard(impl_->sim_lock);
        impl_->sim_stop = true;
    }
    impl_->sim_wake.notify_all();
    if (impl_->sim_worker.joinable()) {
        impl_->sim_worker.join();
    }
    if (impl_->listener != INVALID_SOCKET) {
        ::closesocket(impl_->listener);
        impl_->listener = INVALID_SOCKET;
    }
    if (impl_->peer_sock != INVALID_SOCKET) {
        ::shutdown(impl_->peer_sock, SD_BOTH);
        ::closesocket(impl_->peer_sock);
        impl_->peer_sock = INVALID_SOCKET;
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    if (impl_->status != Status::failed) {
        impl_->status = Status::closed;
    }
    impl_->have_pending_state = false;
    impl_->peer_ready = false;
    role_ = Role::none;
}

std::array<std::uint8_t, 2> Session::seat_kinds() const noexcept { return impl_->seat_kinds; }

// Buttons only - the Game Gear and every pad game. The same frames and the same
// wire as the full form, with no gun on either side.
bool Session::exchange(std::uint16_t local_now, std::uint16_t& host_input,
                       std::uint16_t& client_input) {
    PlayerInput local;
    local.buttons = local_now;
    PlayerInput first;
    PlayerInput second;
    if (!exchange(local, first, second)) {
        return false;
    }
    host_input = first.buttons;
    client_input = second.buttons;
    return true;
}

bool Session::exchange(const PlayerInput& local_now, PlayerInput& host_input,
                       PlayerInput& client_input) {
    auto& impl = *impl_;

    // A joiner has a machine waiting to be swapped in. Done on this thread,
    // where nothing else is touching the machine.
    if (impl.have_pending_state) {
        std::vector<std::uint8_t> blob;
        {
            std::lock_guard<std::mutex> guard(impl.ring_lock);
            blob.swap(impl.pending_state);
        }
        std::string why;
        const bool ok = impl.hooks.restore_state && impl.hooks.restore_state(blob, why);
        impl.have_pending_state = false;
        if (!ok) {
            // The machine's own reason, when it has one: "a different Sega CD
            // BIOS" is something two people can fix, "could not be loaded" is
            // not.
            impl.fail(why.empty() ? "the game state from the host could not be loaded"
                                  : "the game state from the host could not be loaded: " + why);
            return false;
        }
        return false;
    }

    if (impl.status != Status::running) {
        return false;
    }

    const std::uint32_t now = impl.frame.load();
    // ONE INPUT PER FRAME, RECORDED AND SENT ONCE.
    //
    // This used to happen on every call. A call that has to wait (below)
    // returns false and the caller comes straight back with the SAME frame -
    // and whatever the player is holding by then. So a frame's input was
    // re-recorded and re-sent, and the other end, which can be up to `delay`
    // frames ahead, had often already played the first copy: one machine
    // pressed jump on frame 100 and the other on frame 101. Over a real
    // connection that is every few frames, and Sonic 3 ended up in two places
    // with the same moves on both screens - the user's report, 2026-09-24.
    // The late copies also re-marked a slot the other end had already used,
    // which came back as a stale input when the ring wrapped. On loopback
    // nothing waits, so none of it ever showed on one computer.
    //
    // Now the first answer for a frame is the answer. Anything the player
    // does while this end waits lands on the next frame, as it would have
    // on the hardware had the game been running slower.
    if (static_cast<std::int64_t>(now) > impl.local_sent_through) {
        {
            std::lock_guard<std::mutex> guard(impl.ring_lock);
            impl.local_ring[now % kRingSize] = local_now;
        }
        std::vector<std::uint8_t> body;
        put32(body, now);
        put16(body, local_now.buttons);
        put16(body, local_now.gun_x);
        put16(body, local_now.gun_y);
        put8(body, local_now.on_screen ? 1 : 0);
        if (!impl.send_message(Kind::input, body)) {
            impl.fail("the connection dropped");
            return false;
        }
        impl.local_sent_through = now;
    }

    // Nothing runs until both players' buttons for this frame are in hand. For
    // the first few frames there is nothing to wait for, since the delay has
    // not been filled yet.
    const auto want = static_cast<std::int64_t>(now) - delay_;
    PlayerInput local_applied;
    PlayerInput remote_applied;
    if (want >= 0) {
        const auto slot = static_cast<std::uint32_t>(want) % kRingSize;
        std::lock_guard<std::mutex> guard(impl.ring_lock);
        if (!impl.remote_present[slot]) {
            return false;  // the caller comes back in a moment
        }
        local_applied = impl.local_ring[slot];
        remote_applied = impl.remote_ring[slot];
        impl.remote_present[slot] = false;
    }

    // Whose buttons go to which port. Both ends compute this the same way from
    // the same flag, so port one always gets the same player's input on both
    // machines - which is what stops them diverging.
    const bool local_is_second =
        (role_ == Role::host) ? impl.host_second.load() : !impl.host_second.load();
    if (local_is_second) {
        host_input = remote_applied;
        client_input = local_applied;
    } else {
        host_input = local_applied;
        client_input = remote_applied;
    }
    impl.frame = now + 1;
    return true;
}

int Session::local_port_index() const noexcept {
    const bool local_is_second =
        (role_ == Role::host) ? impl_->host_second.load() : !impl_->host_second.load();
    return local_is_second ? 1 : 0;
}

void Session::report_state_hash(std::uint32_t at, std::uint64_t hash) {
    auto& impl = *impl_;
    if (impl.status != Status::running) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(impl.ring_lock);
        const auto slot = (at / kStateCheckInterval) % impl.hash_frames.size();
        if (impl.hash_frames[slot] == at && impl.hash_values[slot] != hash) {
            impl.desync = true;
            impl.set_message("the two games have drifted apart");
            return;
        }
        impl.hash_frames[slot] = at;
        impl.hash_values[slot] = hash;
    }
    std::vector<std::uint8_t> body;
    put32(body, at);
    put64(body, hash);
    impl.send_message(Kind::state_hash, body);
}

std::vector<std::string> local_addresses() {
    std::vector<std::string> out;
    if (!winsock().ok) {
        return out;
    }
    char name[256] = {};
    if (::gethostname(name, sizeof(name)) != 0) {
        return out;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (::getaddrinfo(name, nullptr, &hints, &found) != 0) {
        return out;
    }
    for (auto* entry = found; entry != nullptr; entry = entry->ai_next) {
        char text[INET_ADDRSTRLEN] = {};
        auto* v4 = reinterpret_cast<sockaddr_in*>(entry->ai_addr);
        if (::inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text)) != nullptr) {
            const std::string address(text);
            if (address.rfind("127.", 0) != 0) {
                out.push_back(address);
            }
        }
    }
    ::freeaddrinfo(found);
    return out;
}

std::string generate_passphrase() {
    // Read aloud down a phone without ambiguity: no letters that sound alike
    // and no digits that look like letters.
    static const char* const alphabet = "abcdefghjkmnpqrstuvwxyz23456789";
    const std::size_t span = std::strlen(alphabet);
    std::array<std::uint8_t, 16> bytes{};
    if (!random_bytes(bytes.data(), bytes.size())) {
        return "genx-netplay";
    }
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 8 || i == 12) {
            out.push_back('-');
        }
        out.push_back(alphabet[bytes[i] % span]);
    }
    return out;
}

}  // namespace mercury::net
