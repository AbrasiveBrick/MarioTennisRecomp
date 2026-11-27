// Mario Tennis recomp: deterministic frame stepping, input record/replay, per-frame RDRAM hashing and
// two-player delay-based lockstep netplay over UDP.
//
// The game runs its logic once per call of its frame-wait routine func_80031F40 (30 fps). Three hooks
// from mt.us.toml tie into it:
//   - mt_net_frame (end of func_80031F40, just before it reads the pads with func_800364C4): one tick.
//     The inputs for that frame are fixed here, from the network, a replay file or local controllers.
//   - mt_net_pad (end of func_80033A00, the game's "read one pad" routine): replaces what the game
//     reads with the frame's input, so the pad poll thread's timing doesn't matter.
//   - mt_net_stir (func_80031F40 at 0x80031FF0): the frame wait advances the game's Mersenne Twister
//     (osGetCount() & 7) times on every loop iteration, which depends on host timing. In deterministic
//     mode it is advanced a seeded pseudo-random amount once per frame instead.
//
// Environment variables (all off by default; nothing changes unless one is set):
//   MT_NET_HOST=<port>           host a netplay session (you are player 1)
//   MT_NET_JOIN=<address>:<port> join a session (you are player 2)
//   MT_NET_DELAY=<frames>        input delay in game frames (30 per second), host decides, default 2
//   MT_REPLAY_RECORD=<file>      record each frame's inputs
//   MT_REPLAY_PLAY=<file>        play inputs back from a recording (ignores controllers)
//   MT_HASH_LOG=<file>           write per-frame hashes of every 64 KiB block of RDRAM (tools/hashdiff.py)
//   MT_DETERMINISTIC=1           deterministic RNG stirring without any of the above
//   MT_DET_SEED=<n>              seed for the stirring outside netplay (default 0)
//   MT_STATE_DUMP=<dir>:<f,f,..> write all of RDRAM (32-bit words in host byte order) at these frames
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <filesystem>
#include <vector>

#include "recomp_input.h"
#include "zelda_config.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t bad_socket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t bad_socket = -1;
static void close_socket(socket_t s) { close(s); }
#endif

namespace ultramodern {
    void change_save_file(const std::u8string& subfolder, const std::u8string& name);
}

namespace {

using clk = std::chrono::steady_clock;

struct Pad {
    uint16_t buttons = 0;
    int8_t x = 0;
    int8_t y = 0;
};
static_assert(sizeof(Pad) == 4);

constexpr int max_ports = 4;
constexpr uint32_t rdram_base = 0x80000000u;
constexpr uint32_t rdram_size = 8u * 1024 * 1024;
constexpr uint32_t hash_block = 64u * 1024;
constexpr uint32_t num_blocks = rdram_size / hash_block;

uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint64_t hash_bytes(const uint8_t* p, size_t n) {
    // Simple, fast 64-bit word hash; only needs to agree between peers running the same build.
    uint64_t h = 0x84222325CBF29CE4ull;
    const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
    for (size_t i = 0; i < n / 8; i++) {
        h = (h ^ w[i]) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

// ---------------------------------------------------------------------------------------------------
// Configuration

enum class Mode { Off, Local, Replay, Host, Join };

struct Config {
    Mode mode = Mode::Off;
    bool deterministic = false;
    std::string net_address;
    int net_port = 0;
    int delay = 2;
    uint64_t seed = 0;
    FILE* record = nullptr;
    FILE* replay = nullptr;
    FILE* hash_log = nullptr;
    int players = 0;    // ports forced present (0 = leave to local multiplayer settings)
};

Config cfg;

// The save data and cheat settings both players use (the host's).
std::vector<uint8_t> session_save;
uint8_t session_cheats[(int)recomp::Cheat::Count]{};
bool session_save_applied = false;
std::once_flag cfg_once;

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? v : nullptr;
}

// Replay files: "MTRP" + u32 version + u32 players + u32 reserved, then 16 bytes per frame (4 pads).
constexpr uint32_t replay_version = 1;

void load_config() {
    if (const char* host = env("MT_NET_HOST")) {
        cfg.mode = Mode::Host;
        cfg.net_port = std::atoi(host);
    }
    else if (const char* join = env("MT_NET_JOIN")) {
        cfg.mode = Mode::Join;
        std::string s = join;
        size_t colon = s.rfind(':');
        if (colon == std::string::npos) {
            fprintf(stderr, "[net] MT_NET_JOIN must be <address>:<port>\n");
            cfg.mode = Mode::Off;
        }
        else {
            cfg.net_address = s.substr(0, colon);
            cfg.net_port = std::atoi(s.c_str() + colon + 1);
        }
    }
    if (const char* d = env("MT_NET_DELAY")) cfg.delay = std::max(0, std::min(30, std::atoi(d)));
    if (const char* s = env("MT_DET_SEED")) cfg.seed = std::strtoull(s, nullptr, 0);

    if (const char* path = env("MT_REPLAY_PLAY"); path != nullptr && cfg.mode == Mode::Off) {
        cfg.replay = std::fopen(path, "rb");
        uint32_t header[4]{};
        if (cfg.replay == nullptr || std::fread(header, sizeof(header), 1, cfg.replay) != 1 ||
            std::memcmp(header, "MTRP", 4) != 0 || header[1] != replay_version) {
            fprintf(stderr, "[net] can't read replay %s\n", path);
            if (cfg.replay) std::fclose(cfg.replay);
            cfg.replay = nullptr;
        }
        else {
            cfg.mode = Mode::Replay;
            cfg.players = (int)header[2];
            fprintf(stderr, "[net] playing back %s (%d players)\n", path, cfg.players);
        }
    }
    if (cfg.mode == Mode::Host || cfg.mode == Mode::Join) {
        cfg.players = 2;
    }
    if (const char* path = env("MT_REPLAY_RECORD")) {
        cfg.record = std::fopen(path, "wb");
        if (cfg.record == nullptr) {
            fprintf(stderr, "[net] can't write replay %s\n", path);
        }
        else if (cfg.mode == Mode::Off) {
            cfg.mode = Mode::Local;
            if (const char* p = env("MT_PLAYERS")) cfg.players = std::atoi(p);
        }
        // The header is written once the player count is final (first frame).
    }
    if (const char* path = env("MT_HASH_LOG")) {
        cfg.hash_log = std::fopen(path, "wb");
        if (cfg.hash_log == nullptr) {
            fprintf(stderr, "[net] can't write hash log %s\n", path);
        }
        else {
            uint32_t header[4] = { 0, 1, hash_block, num_blocks };
            std::memcpy(header, "MTHL", 4);
            std::fwrite(header, sizeof(header), 1, cfg.hash_log);
            if (cfg.mode == Mode::Off) cfg.mode = Mode::Local;
        }
    }
    if (env("MT_DETERMINISTIC") && cfg.mode == Mode::Off) cfg.mode = Mode::Local;
    cfg.deterministic = cfg.mode != Mode::Off;
}

void ensure_config() {
    std::call_once(cfg_once, load_config);
}

// ---------------------------------------------------------------------------------------------------
// Network

constexpr uint32_t net_magic = 0x504E544Du; // "MTNP"
constexpr uint8_t net_version = 1;

enum PacketType : uint8_t { PktHello = 1, PktWelcome = 2, PktInput = 3, PktBye = 4, PktSaveReq = 5, PktSaveData = 6 };

constexpr int num_cheats = (int)recomp::Cheat::Count;
constexpr uint32_t save_chunk = 1024;

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint8_t type;
    uint8_t version;
    uint16_t reserved;
};
struct HelloPacket {
    PacketHeader h;
    uint32_t nonce;
};
struct WelcomePacket {
    PacketHeader h;
    uint32_t nonce;
    uint64_t seed;
    uint8_t delay;
    uint8_t cheats[num_cheats];  // the host's cheat settings, used by both players for the session
    uint32_t save_size;          // the host's save data, fetched with PktSaveReq
    uint64_t save_hash;
};
struct SaveReqPacket {
    PacketHeader h;
    uint32_t offset;
};
struct SaveDataPacket {
    PacketHeader h;
    uint32_t offset;
    uint32_t size;
    uint8_t data[save_chunk];
};
constexpr int max_pads_per_packet = 64;
struct InputPacket {
    PacketHeader h;
    uint32_t ack;          // the sender has every remote frame below this
    uint32_t hash_frame;   // a frame the sender has a state hash for (0xFFFFFFFF = none)
    uint64_t hash;
    uint32_t first_frame;  // frame of pads[0]
    uint8_t count;
    Pad pads[max_pads_per_packet];
};
#pragma pack(pop)

struct Net {
    socket_t sock = bad_socket;
    sockaddr_storage peer{};
    socklen_t peer_len = 0;
    bool connected = false;
    std::atomic<bool> failed{ false };
    std::atomic<bool> stop{ false };
    std::thread thread;

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Pad> local;    // local inputs by frame (frames below `delay` are implicit zeros)
    std::vector<Pad> remote;   // remote inputs by frame
    std::vector<bool> remote_have;
    uint32_t remote_contiguous = 0;  // every remote frame below this has arrived
    uint32_t peer_ack = 0;           // the peer has every local frame below this
    clk::time_point last_recv{};
    clk::time_point last_send{};

    static constexpr size_t hash_ring = 1024;
    uint64_t local_hash[hash_ring]{};
    uint32_t local_hash_frame[hash_ring]{};
    uint64_t remote_hash[hash_ring]{};
    uint32_t remote_hash_frame[hash_ring]{};
    uint32_t latest_local_hash_frame = 0xFFFFFFFFu;
    bool desync_reported = false;
    uint32_t last_compared_frame = 0xFFFFFFFFu;
    uint32_t mismatch_run = 0;     // consecutive compared frames whose hashes differ
    uint32_t mismatch_start = 0;
};

Net net;

void net_send_raw(const void* data, size_t size) {
    sendto(net.sock, reinterpret_cast<const char*>(data), (int)size, 0,
        reinterpret_cast<const sockaddr*>(&net.peer), net.peer_len);
}

PacketHeader make_header(uint8_t type) {
    return PacketHeader{ net_magic, type, net_version, 0 };
}

// Called with net.mutex held.
void net_send_inputs_locked() {
    InputPacket p{};
    p.h = make_header(PktInput);
    p.ack = net.remote_contiguous;
    p.hash_frame = net.latest_local_hash_frame;
    p.hash = (p.hash_frame != 0xFFFFFFFFu) ? net.local_hash[p.hash_frame % Net::hash_ring] : 0;
    uint32_t first = net.peer_ack;
    uint32_t end = (uint32_t)net.local.size();
    if (end > first + max_pads_per_packet) first = end - max_pads_per_packet;
    if (first > end) first = end;
    p.first_frame = first;
    p.count = (uint8_t)(end - first);
    for (uint32_t i = 0; i < p.count; i++) p.pads[i] = net.local[first + i];
    net_send_raw(&p, offsetof(InputPacket, pads) + p.count * sizeof(Pad));
    net.last_send = clk::now();
}

WelcomePacket make_welcome(uint32_t nonce) {
    WelcomePacket w{};
    w.h = make_header(PktWelcome);
    w.nonce = nonce;
    w.seed = cfg.seed;
    w.delay = (uint8_t)cfg.delay;
    std::memcpy(w.cheats, session_cheats, sizeof(w.cheats));
    w.save_size = (uint32_t)session_save.size();
    w.save_hash = session_save.empty() ? 0 : hash_bytes(session_save.data(), session_save.size());
    return w;
}

void check_hash_locked(uint32_t frame) {
    size_t slot = frame % Net::hash_ring;
    if (net.local_hash_frame[slot] != frame || net.remote_hash_frame[slot] != frame) return;
    // Each frame counts once (the peer repeats its latest hash in every packet).
    if (net.last_compared_frame != 0xFFFFFFFFu && frame <= net.last_compared_frame) return;
    net.last_compared_frame = frame;
    // Single frames can differ while a streaming load is still landing in RAM, so only a mismatch that
    // persists counts as a desync.
    if (net.local_hash[slot] == net.remote_hash[slot]) {
        net.mismatch_run = 0;
        return;
    }
    if (net.mismatch_run++ == 0) net.mismatch_start = frame;
    if (net.mismatch_run >= 15 && !net.desync_reported) {
        net.desync_reported = true;
        fprintf(stderr, "[net] DESYNC: game state differs from the other player's since frame %u. "
            "Run both sides with MT_HASH_LOG to find out where.\n", net.mismatch_start);
    }
}

void handle_packet(const uint8_t* data, size_t size) {
    if (size < sizeof(PacketHeader)) return;
    PacketHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.magic != net_magic || h.version != net_version) return;
    std::lock_guard lock{ net.mutex };
    net.last_recv = clk::now();
    if (h.type == PktInput && size >= offsetof(InputPacket, pads)) {
        InputPacket p{};
        std::memcpy(&p, data, std::min(size, sizeof(p)));
        if (offsetof(InputPacket, pads) + p.count * sizeof(Pad) > size || p.count > max_pads_per_packet) return;
        net.peer_ack = std::max(net.peer_ack, p.ack);
        for (uint32_t i = 0; i < p.count; i++) {
            uint32_t f = p.first_frame + i;
            if (f >= net.remote.size()) {
                net.remote.resize(f + 1);
                net.remote_have.resize(f + 1, false);
            }
            if (!net.remote_have[f]) {
                net.remote[f] = p.pads[i];
                net.remote_have[f] = true;
            }
        }
        while (net.remote_contiguous < net.remote_have.size() && net.remote_have[net.remote_contiguous]) {
            net.remote_contiguous++;
        }
        if (p.hash_frame != 0xFFFFFFFFu) {
            size_t slot = p.hash_frame % Net::hash_ring;
            net.remote_hash[slot] = p.hash;
            net.remote_hash_frame[slot] = p.hash_frame;
            check_hash_locked(p.hash_frame);
        }
        net.cv.notify_all();
    }
    else if (h.type == PktBye) {
        if (!net.failed.exchange(true)) {
            fprintf(stderr, "[net] the other player left the session\n");
        }
        net.cv.notify_all();
    }
    else if (h.type == PktHello && cfg.mode == Mode::Host && size >= sizeof(HelloPacket)) {
        // The client didn't get our welcome yet; send it again.
        HelloPacket hello;
        std::memcpy(&hello, data, sizeof(hello));
        WelcomePacket w = make_welcome(hello.nonce);
        net_send_raw(&w, sizeof(w));
    }
    else if (h.type == PktSaveReq && cfg.mode == Mode::Host && size >= sizeof(SaveReqPacket)) {
        SaveReqPacket req;
        std::memcpy(&req, data, sizeof(req));
        if (req.offset < session_save.size()) {
            SaveDataPacket d{};
            d.h = make_header(PktSaveData);
            d.offset = req.offset;
            d.size = std::min<uint32_t>(save_chunk, (uint32_t)session_save.size() - req.offset);
            std::memcpy(d.data, session_save.data() + req.offset, d.size);
            net_send_raw(&d, offsetof(SaveDataPacket, data) + d.size);
        }
    }
}

void net_thread_func() {
    uint8_t buf[2048];
    while (!net.stop.load()) {
#ifdef _WIN32
        WSAPOLLFD pfd{ net.sock, POLLRDNORM, 0 };
        int r = WSAPoll(&pfd, 1, 4);
#else
        pollfd pfd{ net.sock, POLLIN, 0 };
        int r = poll(&pfd, 1, 4);
#endif
        if (r > 0) {
            sockaddr_storage from{};
            socklen_t from_len = sizeof(from);
            int n = (int)recvfrom(net.sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n > 0) handle_packet(buf, (size_t)n);
        }
        std::lock_guard lock{ net.mutex };
        auto now = clk::now();
        if (now - net.last_send > std::chrono::milliseconds(10)) {
            net_send_inputs_locked();
        }
        if (!net.failed.load() && now - net.last_recv > std::chrono::seconds(15)) {
            net.failed = true;
            fprintf(stderr, "[net] no packets from the other player for 15 seconds; continuing without them\n");
            net.cv.notify_all();
        }
    }
}

bool net_open_socket(int family) {
#ifdef _WIN32
    static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    if (!wsa) return false;
#endif
    net.sock = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (net.sock == bad_socket) return false;
    if (family == AF_INET6) {
        int off = 0;  // accept IPv4 peers too
        setsockopt(net.sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    }
    return true;
}

int wait_readable(int ms) {
#ifdef _WIN32
    WSAPOLLFD pfd{ net.sock, POLLRDNORM, 0 };
    return WSAPoll(&pfd, 1, ms);
#else
    pollfd pfd{ net.sock, POLLIN, 0 };
    return poll(&pfd, 1, ms);
#endif
}

// Host: the save file and cheat settings the session will use.
void load_host_session_data() {
    for (int c = 0; c < num_cheats; c++) {
        session_cheats[c] = (uint8_t)recomp::get_cheat((recomp::Cheat)c);
    }
    std::filesystem::path path = zelda64::get_app_folder_path() / "saves" / "mt.n64.us.bin";
    session_save.assign(0x800, 0);  // 16 Kbit EEPROM; all zeroes if there is no save yet
    if (FILE* f = std::fopen(path.string().c_str(), "rb")) {
        size_t n = std::fread(session_save.data(), 1, session_save.size(), f);
        (void)n;
        std::fclose(f);
    }
}

// Client: download the host's save data (before the network thread starts).
bool fetch_host_save(uint64_t expected_hash) {
    std::vector<bool> have((session_save.size() + save_chunk - 1) / save_chunk, false);
    auto start = clk::now();
    uint8_t buf[2048];
    while (true) {
        bool done = true;
        for (size_t i = 0; i < have.size(); i++) {
            if (have[i]) continue;
            done = false;
            SaveReqPacket req{ make_header(PktSaveReq), (uint32_t)(i * save_chunk) };
            net_send_raw(&req, sizeof(req));
        }
        if (done) break;
        if (clk::now() - start > std::chrono::seconds(20)) {
            fprintf(stderr, "[net] couldn't get the host's save data\n");
            return false;
        }
        // The host's input packets arrive here too (and are dropped; they're resent later), so listen
        // for a bounded time before asking again for what is still missing.
        auto round_end = clk::now() + std::chrono::milliseconds(150);
        while (clk::now() < round_end && wait_readable(20) > 0) {
            int n = (int)recv(net.sock, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            SaveDataPacket d{};
            if (n < (int)offsetof(SaveDataPacket, data)) continue;
            std::memcpy(&d, buf, std::min<size_t>(n, sizeof(d)));
            if (d.h.magic != net_magic || d.h.type != PktSaveData) continue;
            if (d.offset % save_chunk != 0 || d.offset + d.size > session_save.size() ||
                offsetof(SaveDataPacket, data) + d.size > (size_t)n) continue;
            std::memcpy(session_save.data() + d.offset, d.data, d.size);
            have[d.offset / save_chunk] = true;
        }
    }
    if (!session_save.empty() && hash_bytes(session_save.data(), session_save.size()) != expected_hash) {
        fprintf(stderr, "[net] the host's save data arrived damaged\n");
        return false;
    }
    return true;
}

// Blocks until the session is set up. Runs on the game thread before the game boots.
void net_connect() {
    if (cfg.mode == Mode::Host) {
        // Dual-stack IPv6 socket where available (accepts IPv4 players too), plain IPv4 otherwise.
        bool bound = false;
        if (net_open_socket(AF_INET6)) {
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_addr = in6addr_any;
            addr.sin6_port = htons((uint16_t)cfg.net_port);
            bound = bind(net.sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
            if (!bound) close_socket(net.sock);
        }
        if (!bound && net_open_socket(AF_INET)) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons((uint16_t)cfg.net_port);
            bound = bind(net.sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        }
        if (!bound) {
            fprintf(stderr, "[net] can't listen on UDP port %d\n", cfg.net_port);
            net.failed = true;
            return;
        }
        if (cfg.seed == 0) {
            cfg.seed = splitmix64((uint64_t)clk::now().time_since_epoch().count());
        }
        load_host_session_data();
        fprintf(stderr, "[net] hosting on UDP port %d, waiting for player 2 (input delay %d frames)\n",
            cfg.net_port, cfg.delay);
        uint8_t buf[2048];
        while (true) {
            sockaddr_storage from{};
            socklen_t from_len = sizeof(from);
            int n = (int)recvfrom(net.sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                reinterpret_cast<sockaddr*>(&from), &from_len);
            PacketHeader h{};
            if (n < (int)sizeof(HelloPacket)) continue;
            std::memcpy(&h, buf, sizeof(h));
            if (h.magic != net_magic || h.type != PktHello) continue;
            if (h.version != net_version) {
                fprintf(stderr, "[net] player 2 runs a different netplay version (%u, we have %u)\n", h.version, net_version);
                continue;
            }
            HelloPacket hello;
            std::memcpy(&hello, buf, sizeof(hello));
            net.peer = from;
            net.peer_len = from_len;
            WelcomePacket w = make_welcome(hello.nonce);
            net_send_raw(&w, sizeof(w));
            break;
        }
    }
    else {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        std::string port = std::to_string(cfg.net_port);
        if (getaddrinfo(cfg.net_address.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
            fprintf(stderr, "[net] can't resolve %s\n", cfg.net_address.c_str());
            net.failed = true;
            return;
        }
        std::memcpy(&net.peer, res->ai_addr, res->ai_addrlen);
        net.peer_len = (socklen_t)res->ai_addrlen;
        int family = res->ai_family;
        freeaddrinfo(res);
        if (!net_open_socket(family)) {
            fprintf(stderr, "[net] can't open a UDP socket\n");
            net.failed = true;
            return;
        }
        uint32_t nonce = (uint32_t)splitmix64((uint64_t)clk::now().time_since_epoch().count());
        fprintf(stderr, "[net] joining %s:%d\n", cfg.net_address.c_str(), cfg.net_port);
        uint8_t buf[2048];
        auto start = clk::now();
        while (true) {
            HelloPacket hello{ make_header(PktHello), nonce };
            net_send_raw(&hello, sizeof(hello));
            if (wait_readable(250) > 0) {
                int n = (int)recv(net.sock, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                WelcomePacket w{};
                if (n >= (int)sizeof(WelcomePacket)) {
                    std::memcpy(&w, buf, sizeof(w));
                    if (w.h.magic == net_magic && w.h.type == PktWelcome && w.nonce == nonce) {
                        if (w.h.version != net_version) {
                            fprintf(stderr, "[net] the host runs a different netplay version (%u, we have %u)\n", w.h.version, net_version);
                            net.failed = true;
                            return;
                        }
                        cfg.seed = w.seed;
                        cfg.delay = w.delay;
                        std::memcpy(session_cheats, w.cheats, sizeof(session_cheats));
                        session_save.assign(std::min<uint32_t>(w.save_size, 0x20000), 0);
                        if (!fetch_host_save(w.save_hash)) {
                            net.failed = true;
                            return;
                        }
                        break;
                    }
                }
            }
            if (clk::now() - start > std::chrono::seconds(60)) {
                fprintf(stderr, "[net] no answer from the host after 60 seconds; playing alone\n");
                net.failed = true;
                return;
            }
        }
    }
    net.connected = true;
    net.last_recv = clk::now();
    fprintf(stderr, "[net] connected: you are player %d, input delay %d frames\n", cfg.mode == Mode::Host ? 1 : 2, cfg.delay);
    net.thread = std::thread(net_thread_func);
}

// ---------------------------------------------------------------------------------------------------
// Frame state

uint32_t frame = 0;          // game frames completed (ticks)
uint32_t stir_frame = 0xFFFFFFFFu;
Pad frame_pads[max_ports]{};
bool frame_pads_valid = false;

Pad sample_local(int port) {
    uint16_t buttons = 0;
    float x = 0, y = 0;
    Pad p{};
    if (recomp::get_n64_input(port, &buttons, &x, &y)) {
        p.buttons = buttons;
        p.x = (int8_t)(127 * x);  // same conversion as ultramodern's osContGetReadData
        p.y = (int8_t)(127 * y);
    }
    return p;
}

void write_hash_log(uint8_t* rdram) {
    uint64_t hashes[num_blocks];
    for (uint32_t b = 0; b < num_blocks; b++) {
        hashes[b] = hash_bytes(rdram + b * hash_block, hash_block);
    }
    std::fwrite(&frame, sizeof(frame), 1, cfg.hash_log);
    std::fwrite(hashes, sizeof(hashes), 1, cfg.hash_log);
    if ((frame & 63) == 0) std::fflush(cfg.hash_log);
}

// Blocks that hold no game logic state (framebuffers, audio, the renderer's scratch) and would make
// the netplay state hash differ between machines. Filled in from MT_HASH_LOG comparisons.
bool hash_block_ignored(uint32_t block) {
    static std::vector<bool> ignored = [] {
        std::vector<bool> v(num_blocks, false);
        // Default: audio driver and heap, scheduler/timing globals (0x80040000-0x800CFFFF), the
        // audio overlay (0x802F0000) and frame/display-list buffers (0x80380000-0x803FFFFF), which
        // differ between machines even when the game itself plays out identically.
        const char* s = env("MT_NET_HASH_SKIP");
        if (s == nullptr) s = "4-12,47,56-63";
        {
            // Comma-separated block indices or ranges, e.g. "0-3,40,41".
            std::string list = s;
            size_t p = 0;
            while (p < list.size()) {
                size_t e = list.find(',', p);
                std::string item = list.substr(p, e == std::string::npos ? std::string::npos : e - p);
                int a = std::atoi(item.c_str()), b = a;
                if (size_t dash = item.find('-'); dash != std::string::npos) b = std::atoi(item.c_str() + dash + 1);
                for (int i = a; i <= b && i < (int)num_blocks; i++) if (i >= 0) v[i] = true;
                if (e == std::string::npos) break;
                p = e + 1;
            }
        }
        return v;
    }();
    return ignored[block];
}

void dump_state(uint8_t* rdram) {
    static const std::string spec = env("MT_STATE_DUMP") ? env("MT_STATE_DUMP") : "";
    if (spec.empty()) return;
    size_t colon = spec.rfind(':');
    std::string list = "," + spec.substr(colon + 1) + ",";
    if (list.find("," + std::to_string(frame) + ",") == std::string::npos) return;
    std::string path = spec.substr(0, colon) + "/frame" + std::to_string(frame) + ".bin";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(rdram, rdram_size, 1, f);
        std::fclose(f);
    }
}

uint64_t state_hash(uint8_t* rdram) {
    uint64_t h = 0;
    for (uint32_t b = 0; b < num_blocks; b++) {
        if (hash_block_ignored(b)) continue;
        h = splitmix64(h ^ hash_bytes(rdram + b * hash_block, hash_block));
    }
    return h;
}

void net_tick(uint8_t* rdram) {
    const int me = (cfg.mode == Mode::Host) ? 0 : 1;
    Pad local = sample_local(0);
    if (!net.connected) {
        // The session never started: play on alone as player 1.
        for (int p = 0; p < max_ports; p++) frame_pads[p] = Pad{};
        frame_pads[0] = local;
        return;
    }

    std::unique_lock lock{ net.mutex };
    if (net.connected) {
        // Our input for this tick applies `delay` frames from now; earlier frames are zeros on both sides.
        while (net.local.size() < (size_t)frame + cfg.delay) net.local.push_back(Pad{});
        if (net.local.size() == (size_t)frame + cfg.delay) net.local.push_back(local);
        // State hash of the frame about to run, for desync detection.
        size_t slot = frame % Net::hash_ring;
        net.local_hash[slot] = state_hash(rdram);
        net.local_hash_frame[slot] = frame;
        net.latest_local_hash_frame = frame;
        check_hash_locked(frame);
        net_send_inputs_locked();
    }
    Pad mine = (frame < net.local.size()) ? net.local[frame] : Pad{};
    Pad theirs{};
    if (net.connected) {
        auto wait_start = clk::now();
        bool warned = false;
        while (!net.failed.load() && !(frame < net.remote_have.size() && net.remote_have[frame])) {
            net.cv.wait_for(lock, std::chrono::milliseconds(100));
            if (!warned && clk::now() - wait_start > std::chrono::seconds(2)) {
                warned = true;
                fprintf(stderr, "[net] waiting for the other player (frame %u)\n", frame);
            }
        }
        if (frame < net.remote_have.size() && net.remote_have[frame]) theirs = net.remote[frame];
    }
    lock.unlock();

    for (int p = 0; p < max_ports; p++) frame_pads[p] = Pad{};
    frame_pads[me] = mine;
    frame_pads[1 - me] = theirs;
}

} // namespace

// Ports the game sees as connected, or -1 to leave it to the local multiplayer setting.
int mt_net_forced_players() {
    ensure_config();
    return cfg.players > 0 ? cfg.players : -1;
}

bool mt_net_session_active() {
    ensure_config();
    return cfg.mode == Mode::Host || cfg.mode == Mode::Join;
}

void save_write_ptr(const void* in, uint32_t offset, uint32_t count);  // librecomp

// Game start (mt::on_init, before any game code runs): set up the netplay session, so both players
// boot with the same save data and settings.
void mt_net_on_init() {
    ensure_config();
    static bool done = false;
    if (done || !(cfg.mode == Mode::Host || cfg.mode == Mode::Join)) return;
    done = true;
    net_connect();
    if (net.connected && cfg.mode == Mode::Join) {
        for (int c = 0; c < num_cheats; c++) {
            recomp::set_cheat((recomp::Cheat)c, (recomp::CheatMode)session_cheats[c]);
        }
    }
}

// Entry of the game's EEPROM probe and read routines: the guest plays on a copy of the host's save,
// kept in saves/netplay/guest.bin so their own save isn't touched.
extern "C" void mt_net_save_access() {
    if (session_save_applied || cfg.mode != Mode::Join || !net.connected) return;
    session_save_applied = true;
    ultramodern::change_save_file(u8"netplay", u8"guest");
    save_write_ptr(session_save.data(), 0, (uint32_t)session_save.size());
}

extern "C" uint32_t mt_net_stir(uint32_t count) {
    ensure_config();
    static const bool test_nostir = env("MT_TEST_HOST_STIR") != nullptr;  // testing: keep the host-timed stirring
    if (!cfg.deterministic || test_nostir) return count;
    if (stir_frame == frame) return 0;
    stir_frame = frame;
    return (uint32_t)splitmix64(cfg.seed ^ ((uint64_t)frame << 32));
}

extern "C" void mt_net_frame(uint8_t* rdram) {
    ensure_config();
    if (cfg.mode == Mode::Off) return;

    static const bool frame_log = env("MT_FRAME_LOG") != nullptr;
    if (frame_log && frame % 30 == 0) fprintf(stderr, "[net] frame %u\n", frame);
    if (cfg.hash_log != nullptr) write_hash_log(rdram);
    dump_state(rdram);

    switch (cfg.mode) {
    case Mode::Host:
    case Mode::Join:
        net_tick(rdram);
        break;
    case Mode::Replay: {
        Pad pads[max_ports]{};
        if (cfg.replay != nullptr && std::fread(pads, sizeof(pads), 1, cfg.replay) != 1) {
            fprintf(stderr, "[net] replay ended at frame %u\n", frame);
            std::fclose(cfg.replay);
            cfg.replay = nullptr;
        }
        std::memcpy(frame_pads, pads, sizeof(pads));
        break;
    }
    default:
        for (int p = 0; p < max_ports; p++) frame_pads[p] = sample_local(p);
        break;
    }
    frame_pads_valid = true;

    if (cfg.record != nullptr) {
        if (frame == 0) {
            uint32_t header[4] = { 0, replay_version, (uint32_t)std::max(cfg.players, 0), 0 };
            std::memcpy(header, "MTRP", 4);
            std::fwrite(header, sizeof(header), 1, cfg.record);
        }
        std::fwrite(frame_pads, sizeof(frame_pads), 1, cfg.record);
        if ((frame & 63) == 0) std::fflush(cfg.record);
    }
    frame++;
}

// End of func_80033A00(pad_t* out, int port): out = {u16 button; s8 x; s8 y; u8 errno; u8; u16 pressed},
// not_prev = ~(button before this read).
extern "C" void mt_net_pad(uint8_t* rdram, uint32_t out, uint32_t port, uint32_t not_prev) {
    if (cfg.mode == Mode::Off || !frame_pads_valid || port >= max_ports) return;
    auto h = [&](uint32_t a) -> uint16_t& { return *reinterpret_cast<uint16_t*>(rdram + ((a ^ 2) - rdram_base)); };
    auto b = [&](uint32_t a) -> uint8_t& { return *(rdram + ((a ^ 3) - rdram_base)); };
    const Pad& p = frame_pads[port];
    h(out + 0) = p.buttons;
    b(out + 2) = (uint8_t)p.x;
    b(out + 3) = (uint8_t)p.y;
    b(out + 4) = 0;
    h(out + 6) = (uint16_t)(p.buttons & not_prev);
}

void mt_net_shutdown() {
    if (net.connected) {
        PacketHeader bye = make_header(PktBye);
        for (int i = 0; i < 3; i++) net_send_raw(&bye, sizeof(bye));
        net.stop = true;
        if (net.thread.joinable()) net.thread.join();
        close_socket(net.sock);
        net.connected = false;
    }
    if (cfg.record) std::fclose(cfg.record);
    if (cfg.hash_log) std::fclose(cfg.hash_log);
    cfg.record = cfg.hash_log = nullptr;
}
