// kol_vs_net.cpp — VS Mode over the local network (kol_vs_net.h).

#include "kol_vs_net.h"

#if defined(_WIN32) || defined(__ANDROID__)

namespace kol {
bool vs_mirror_child() { return false; }
namespace vsnet {
std::string file_fingerprint(const std::string&) { return "none"; }
bool start(Side) { return false; }
Status status() { return Status::Idle; }
const std::string& failure() { static const std::string s; return s; }
const Side& peer() { static const Side s; return s; }
void set_key_sink(int) {}
void send_keys(std::uint64_t, std::uint16_t) {}
void send_hash(std::uint64_t, std::uint64_t) {}
bool take_peer_hash(std::uint64_t, std::uint64_t*) { return false; }
bool peer_gone(std::string*) { return false; }
void stop(const char*) {}
void set_on_gone(void (*)()) {}
void play_begin(std::uint16_t, const std::string&) {}
void play_reset_calls() {}
void play_attach_hash_fd(int) {}
void play_start_keys() {}
void play_tick(std::uint64_t) {}
std::uint16_t play_filter(std::uint16_t keys) { return keys; }
bool play_active() { return false; }
bool play_desync(std::string*) { return false; }
void play_end() {}
void mirror_settings() {}
void mirror_install() {}
void mirror_tick(std::uint64_t) {}
std::uint64_t game_hash() { return 0; }
}  // namespace vsnet
}  // namespace kol

#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "kol_settings.h"
#include "mod_function_hooks.h"
#include "runtime.h"
#include "runtime_arm.h"   // g_cpu (the hash log)
#include "runtime_bus_bridge.h"

namespace kol {

bool vs_mirror_child() {
    static const bool on = [] {
        const char* e = std::getenv("KOL_VS_MIRROR");
        return e && *e == '1';
    }();
    return on;
}

namespace vsnet {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBeaconPort = 19873;
constexpr int kProto = 1;
constexpr std::uint64_t kHashEvery = 64;
constexpr auto kSilence = std::chrono::seconds(15);

// Pad reads between a button pressed and the game seeing it, on both
// machines (KOL_VS_NET_DELAY=n for tests). The game reads the pad once a
// frame in play: 4 reads (67 ms) cover this network's round trip of ~15 ms,
// 32 at worst (2026-11-22, the Mac to the Deck).
std::uint64_t delay_frames() {
    static const std::uint64_t d = [] {
        const char* e = std::getenv("KOL_VS_NET_DELAY");
        const long v = e && *e ? std::atol(e) : 4;
        return static_cast<std::uint64_t>(v < 1 ? 1 : v > 60 ? 60 : v);
    }();
    return d;
}

// One message on the session's socket: type, 3 spare bytes, the payload's
// length, then the payload.
enum : std::uint8_t { mHello = 1, mState, mSave, mCfg, mKeys, mHash, mBye, mPing };
struct Header {
    std::uint8_t type = 0;
    std::uint8_t pad[3] = {0, 0, 0};
    std::uint32_t len = 0;
};
static_assert(sizeof(Header) == 8, "header is 8 bytes");
// A key or hash record (the pipes to and from the copy use it too).
struct Rec {
    std::uint64_t frame = 0;   // the pad read's number
    std::uint64_t value = 0;
};
static_assert(sizeof(Rec) == 16, "record is 16 bytes");

void cloexec(int fd) {
    if (fd >= 0) ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

bool write_all(int fd, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    while (n > 0) {
        const ssize_t w = ::send(fd, b, n, 0);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        b += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}
bool read_all(int fd, void* p, std::size_t n) {
    auto* b = static_cast<std::uint8_t*>(p);
    while (n > 0) {
        const ssize_t r = ::recv(fd, b, n, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        b += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}
// A pipe's whole record (the copy's ends): write() on a pipe.
bool pipe_write(int fd, const Rec& r) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&r);
    std::size_t n = sizeof r;
    while (n > 0) {
        const ssize_t w = ::write(fd, b, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        b += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

std::string hex64(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

std::uint64_t fnv(const std::uint8_t* p, std::size_t n, std::uint64_t h = 1469598103934665603ull) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// ── The session's socket and its thread ──────────────────────────────────

struct Net {
    std::mutex mu;
    std::atomic<Status> status{Status::Searching};
    std::string failure;
    Side mine, peer;
    std::atomic<bool> stopping{false};
    std::atomic<int> fd{-1};
    std::mutex send_mu;
    Clock::time_point last_tx = Clock::now();
    // (under mu)
    int sink = -1;
    std::vector<Rec> pending;
    std::map<std::uint64_t, std::uint64_t> peer_hashes;
    bool gone = false;
    std::string gone_why;
    Clock::time_point last_rx = Clock::now();

    bool send_msg(std::uint8_t type, const void* p, std::size_t n) {
        const int s = fd.load();
        if (s < 0) return false;
        Header h;
        h.type = type;
        h.len = static_cast<std::uint32_t>(n);
        std::lock_guard<std::mutex> lock(send_mu);
        last_tx = Clock::now();
        return write_all(s, &h, sizeof h) && (n == 0 || write_all(s, p, n));
    }
    void fail(std::string why) {
        {
            std::lock_guard<std::mutex> lock(mu);
            failure = std::move(why);
        }
        std::fprintf(stderr, "vs-net: %s\n", failure.c_str());
        status.store(Status::Failed);
    }
};

std::mutex g_cur_mu;
std::shared_ptr<Net> g_cur;   // the session (kept by its thread too)
std::atomic<void (*)()> g_on_gone{nullptr};

std::shared_ptr<Net> cur() {
    std::lock_guard<std::mutex> lock(g_cur_mu);
    return g_cur;
}

std::string hello_text(const Side& s) {
    char b[1024];
    std::snprintf(b, sizeof b,
                  "proto=%d\nnonce=%s\nname=%s\nrom=%s\nbios=%s\nmod=%s\nframe=%llu\nclock=%llu\npanel=%ux%u\n", kProto,
                  hex64(s.nonce).c_str(), s.name.c_str(), s.rom.c_str(), s.bios.c_str(), s.mod.c_str(),
                  static_cast<unsigned long long>(s.frame), static_cast<unsigned long long>(s.clock), s.panel_w,
                  s.panel_h);
    return b;
}

bool parse_hello(const std::string& t, Side* s, int* proto) {
    *proto = -1;
    std::size_t at = 0;
    while (at < t.size()) {
        std::size_t nl = t.find('\n', at);
        if (nl == std::string::npos) nl = t.size();
        const std::string line = t.substr(at, nl - at);
        at = nl + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "proto") *proto = std::atoi(v.c_str());
        else if (k == "nonce") s->nonce = std::strtoull(v.c_str(), nullptr, 16);
        else if (k == "name") s->name = v;
        else if (k == "rom") s->rom = v;
        else if (k == "bios") s->bios = v;
        else if (k == "mod") s->mod = v;
        else if (k == "frame") s->frame = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "clock") s->clock = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "panel") std::sscanf(v.c_str(), "%ux%u", &s->panel_w, &s->panel_h);
    }
    return *proto >= 0;
}

// A TCP connection to ip:port within 2 s, or -1.
int connect_to(const sockaddr_in& to) {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    cloexec(s);
    const int fl = ::fcntl(s, F_GETFL);
    ::fcntl(s, F_SETFL, fl | O_NONBLOCK);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0 && errno != EINPROGRESS) {
        ::close(s);
        return -1;
    }
    pollfd p{s, POLLOUT, 0};
    int err = 0;
    socklen_t len = sizeof err;
    if (::poll(&p, 1, 2000) <= 0 || ::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        ::close(s);
        return -1;
    }
    ::fcntl(s, F_SETFL, fl);
    return s;
}

// Finds the other game: beacons out, theirs in; the smaller number connects.
int find_peer(Net& n) {
    const int udp = ::socket(AF_INET, SOCK_DGRAM, 0);
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    cloexec(udp);
    cloexec(ls);
    int one = 1;
    if (udp >= 0) {
        ::setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#if defined(SO_REUSEPORT)
        ::setsockopt(udp, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);   // two games on one machine (tests)
#endif
        ::setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    }
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    any.sin_port = htons(kBeaconPort);
    sockaddr_in tcp_any = any;
    tcp_any.sin_port = 0;
    if (ls >= 0) ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (udp < 0 || ls < 0 || ::bind(udp, reinterpret_cast<sockaddr*>(&any), sizeof any) != 0 ||
        ::bind(ls, reinterpret_cast<sockaddr*>(&tcp_any), sizeof tcp_any) != 0 || ::listen(ls, 4) != 0) {
        if (udp >= 0) ::close(udp);
        if (ls >= 0) ::close(ls);
        n.fail("no network: could not open the local network's ports");
        return -1;
    }
    sockaddr_in mine{};
    socklen_t mlen = sizeof mine;
    ::getsockname(ls, reinterpret_cast<sockaddr*>(&mine), &mlen);
    const unsigned tcp_port = ntohs(mine.sin_port);
    char beacon[160];
    std::snprintf(beacon, sizeof beacon, "KOLVS%d %s %u %s", kProto, hex64(n.mine.nonce).c_str(), tcp_port,
                  n.mine.rom.c_str());
    std::fprintf(stderr, "vs-net: looking for a player (beacons on UDP %d, TCP %u, number %s)\n", kBeaconPort,
                 tcp_port, hex64(n.mine.nonce).c_str());

    // KOL_VS_NET_PEER=a.b.c.d: beacons to that address too (a network that
    // drops broadcasts).
    sockaddr_in bcast{};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(kBeaconPort);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sockaddr_in extra{};
    bool have_extra = false;
    if (const char* e = std::getenv("KOL_VS_NET_PEER"); e && *e) {
        extra = bcast;
        have_extra = ::inet_pton(AF_INET, e, &extra.sin_addr) == 1;
    }

    int fd = -1;
    auto next_beacon = Clock::now();
    while (fd < 0 && !n.stopping.load()) {
        if (Clock::now() >= next_beacon) {
            ::sendto(udp, beacon, std::strlen(beacon), 0, reinterpret_cast<sockaddr*>(&bcast), sizeof bcast);
            if (have_extra)
                ::sendto(udp, beacon, std::strlen(beacon), 0, reinterpret_cast<sockaddr*>(&extra), sizeof extra);
            next_beacon = Clock::now() + std::chrono::milliseconds(500);
        }
        pollfd p[2] = {{udp, POLLIN, 0}, {ls, POLLIN, 0}};
        if (::poll(p, 2, 100) <= 0) continue;
        if (p[1].revents & POLLIN) {
            fd = ::accept(ls, nullptr, nullptr);
            if (fd >= 0) {
                cloexec(fd);
                std::fprintf(stderr, "vs-net: a player connected\n");
            }
            continue;
        }
        if (p[0].revents & POLLIN) {
            char buf[200];
            sockaddr_in from{};
            socklen_t flen = sizeof from;
            const ssize_t r = ::recvfrom(udp, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr*>(&from), &flen);
            if (r <= 0) continue;
            buf[r] = '\0';
            int proto = 0;
            char nonce_hex[40] = {0}, rom[80] = {0};
            unsigned port = 0;
            if (std::sscanf(buf, "KOLVS%d %39s %u %79s", &proto, nonce_hex, &port, rom) != 4 || proto != kProto)
                continue;
            const std::uint64_t nonce = std::strtoull(nonce_hex, nullptr, 16);
            if (nonce == n.mine.nonce || n.mine.rom != rom || port == 0 || port > 65535) continue;
            if (n.mine.nonce > nonce) continue;   // the other one connects
            sockaddr_in to = from;
            to.sin_port = htons(static_cast<std::uint16_t>(port));
            char ip[64];
            ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            fd = connect_to(to);
            std::fprintf(stderr, "vs-net: a player at %s:%u: %s\n", ip, port, fd >= 0 ? "connected" : "no connection");
        }
    }
    ::close(udp);
    ::close(ls);
    return fd;
}

// Hello, snapshot, save and settings both ways; then the match's messages.
void run(std::shared_ptr<Net> np) {
    Net& n = *np;
    const int fd = find_peer(n);
    if (fd < 0) {
        if (!n.stopping.load() && n.status.load() != Status::Failed) n.fail("no player found");
        return;
    }
    n.fd.store(fd);
    if (n.stopping.load()) ::shutdown(fd, SHUT_RDWR);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    n.status.store(Status::Exchanging);

    // Ours out on a thread of its own (both send ~700 KB at once: neither
    // may wait for the other to read first).
    std::thread sender([&n] {
        const std::string h = hello_text(n.mine);
        (void)(n.send_msg(mHello, h.data(), h.size()) && n.send_msg(mState, n.mine.state.data(), n.mine.state.size()) &&
               n.send_msg(mSave, n.mine.save.data(), n.mine.save.size()) &&
               n.send_msg(mCfg, n.mine.cfg.data(), n.mine.cfg.size()));
    });
    Side peer;
    int proto = -1;
    bool have[4] = {false, false, false, false};
    std::string bye;
    while (!(have[0] && have[1] && have[2] && have[3])) {
        Header h;
        if (!read_all(fd, &h, sizeof h) || h.len > (64u << 20)) break;
        std::vector<std::uint8_t> body(h.len);
        if (h.len && !read_all(fd, body.data(), h.len)) break;
        const std::string text(body.begin(), body.end());
        if (h.type == mHello) have[0] = parse_hello(text, &peer, &proto);
        else if (h.type == mState) peer.state = std::move(body), have[1] = true;
        else if (h.type == mSave) peer.save = std::move(body), have[2] = true;
        else if (h.type == mCfg) peer.cfg = text, have[3] = true;
        else if (h.type == mBye) {
            bye = text;
            break;
        }
    }
    sender.join();
    if (!(have[0] && have[1] && have[2] && have[3])) {
        if (!n.stopping.load())
            n.fail(bye.empty() ? "the other player's game left before the start" : "the other player: " + bye);
        ::shutdown(fd, SHUT_RDWR);
        return;
    }
    std::string bad;
    if (proto != kProto) bad = "another version of this program";
    else if (peer.rom != n.mine.rom) bad = "another ROM";
    else if (peer.mod != n.mine.mod) bad = "another mod";
    else if (peer.bios != n.mine.bios) bad = "another BIOS";
    if (!bad.empty()) {
        const std::string why = "not the same game: " + bad;
        n.send_msg(mBye, why.data(), why.size());
        n.fail("the other player's game has " + bad);
        ::shutdown(fd, SHUT_RDWR);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(n.mu);
        n.peer = std::move(peer);
        n.last_rx = Clock::now();
    }
    std::fprintf(stderr, "vs-net: playing %s (frame %llu; this game is the link's %s)\n", n.peer.name.c_str(),
                 static_cast<unsigned long long>(n.peer.frame), n.mine.nonce < n.peer.nonce ? "parent" : "child");
    n.status.store(Status::Ready);

    // The match: the peer's buttons to the copy, its hashes kept. A ping
    // each second this side sends nothing (held, or paused).
    std::string why = "the connection closed";
    while (!n.stopping.load()) {
        pollfd p{fd, POLLIN, 0};
        const int pr = ::poll(&p, 1, 500);
        {
            std::lock_guard<std::mutex> lock(n.mu);
            if (Clock::now() - n.last_rx > kSilence) {
                why = "nothing from the other player for 15 s";
                break;
            }
        }
        if (pr == 0 || !(p.revents & (POLLIN | POLLHUP | POLLERR))) {
            bool ping = false;
            {
                std::lock_guard<std::mutex> lock(n.send_mu);
                ping = Clock::now() - n.last_tx > std::chrono::seconds(1);
            }
            if (ping) n.send_msg(mPing, nullptr, 0);
            continue;
        }
        Header h;
        if (!read_all(fd, &h, sizeof h) || h.len > (1u << 20)) break;
        std::vector<std::uint8_t> body(h.len);
        if (h.len && !read_all(fd, body.data(), h.len)) break;
        std::lock_guard<std::mutex> lock(n.mu);
        n.last_rx = Clock::now();
        if (h.type == mKeys && h.len == sizeof(Rec)) {
            Rec r;
            std::memcpy(&r, body.data(), sizeof r);
            if (n.sink >= 0) pipe_write(n.sink, r);
            else n.pending.push_back(r);
        } else if (h.type == mHash && h.len == sizeof(Rec)) {
            Rec r;
            std::memcpy(&r, body.data(), sizeof r);
            n.peer_hashes[r.frame] = r.value;
            while (n.peer_hashes.size() > 256) n.peer_hashes.erase(n.peer_hashes.begin());
        } else if (h.type == mBye) {
            why = "the other player: " + std::string(body.begin(), body.end());
            break;
        }
    }
    bool tell = false;
    {
        std::lock_guard<std::mutex> lock(n.mu);
        if (!n.stopping.load() && !n.gone) {
            n.gone = true;
            n.gone_why = why;
            tell = true;
            std::fprintf(stderr, "vs-net: %s\n", why.c_str());
        }
        // No more buttons: the copy's pipe closed, its wait ends.
        if (n.sink >= 0) ::close(n.sink);
        n.sink = -1;
    }
    ::shutdown(fd, SHUT_RDWR);
    // The seen game may be held in the link, waiting on the copy, which waits
    // for buttons that will not come: the copy goes now, the link closes, and
    // the game goes on to end the match (it waited the copy's 30 s before).
    if (tell) {
        if (void (*fn)() = g_on_gone.load()) fn();
    }
}

// ── The button timeline ──────────────────────────────────────────────────
//
// Numbered by the game's own pad reads, not by frames: the main loop calls
// 0x08000A04 and then reads KEYINPUT (0x0800061C) and hands the pressed
// buttons to its key state (0x08002504, IWRAM 0x03000820). A hook on that
// call sets KEYINPUT for read n, so every copy of a game gets read n's
// buttons at the same instruction. (Set from the per-frame hook they landed
// a few instructions apart in the two copies; once the camera then eased a
// frame later in one, 2026-11-22. That hook also runs twice in a frame when
// the view changes size.) The memory hash is taken there too, every 64th
// read: the same instruction in both copies.

constexpr std::uint32_t kBeforePad = 0x08000A04u;        // called right before the pad is read
constexpr std::uint32_t kBeforePadReturn = 0x08000618u;  // ... by the main loop's bl at 0x08000614
constexpr char kPadHookId[] = "kol.vs-net-pad";

std::atomic<bool> g_play{false};
std::atomic<bool> g_counting{false};                     // reads counted (from the match's snapshot loaded)
std::atomic<std::uint16_t> g_applied{0x03FFu};
std::atomic<std::uint16_t> g_physical{0x03FFu};
std::uint64_t g_calls = 0;                               // pad reads since the snapshot
std::uint64_t g_sent_to = 0;                             // the last read whose buttons were sent
std::atomic<std::uint64_t> g_last_call{0};
std::atomic<long long> g_last_call_ms{0};
std::map<std::uint64_t, std::uint16_t> g_local;          // this game's buttons by read
std::map<std::uint64_t, std::uint64_t> g_mirror_hashes;  // the copy's hashes by read
int g_hash_fd = -1;
int g_rewind_was = 0;
bool g_desync = false;
std::string g_desync_what;
unsigned long long g_hash_checks = 0;

void dump_memory(const char* kind, std::uint64_t n);
// The last few hash points' memory, kept in the session's folder (the seen
// game's) or the copy's own folder (its working directory): <kind>_<read>.bin,
// the one four points older removed. A desync keeps the folders, so both
// sides of the difference can be compared (ring_dump).
std::string g_ring_dir;
void ring_dump(const char* kind, const std::string& dir, std::uint64_t n);

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void apply_keys(std::uint16_t v) {
    if (gba::GbaBus* bus = gbarecomp::active_bus()) bus->io().set_keyinput(v);
    g_applied.store(v);
}

// ── The copy ─────────────────────────────────────────────────────────────

std::mutex g_m_mu;
std::condition_variable g_m_cv;
std::map<std::uint64_t, std::uint16_t> g_m_keys;
bool g_m_closed = false;
int g_m_hash_fd = -1;
std::uint64_t g_m_frame0 = 0;   // its snapshot's frame: reads before it are the boot's, before the load
std::uint64_t g_m_calls = 0;

std::uint16_t mirror_filter(std::uint16_t) { return g_applied.load(); }

void mirror_reader(int fd) {
    for (;;) {
        Rec r;
        auto* b = reinterpret_cast<std::uint8_t*>(&r);
        std::size_t got = 0;
        while (got < sizeof r) {
            const ssize_t n = ::read(fd, b + got, sizeof r - got);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                std::lock_guard<std::mutex> lock(g_m_mu);
                g_m_closed = true;
                g_m_cv.notify_all();
                return;
            }
            got += static_cast<std::size_t>(n);
        }
        std::lock_guard<std::mutex> lock(g_m_mu);
        g_m_keys[r.frame] = static_cast<std::uint16_t>(r.value);
        g_m_cv.notify_all();
    }
}

// The seen game's read n: its hash every 64th, its player's buttons now for
// read n + the delay (here and there), read n's own applied.
void play_input() {
    const std::uint64_t n = ++g_calls;
    g_last_call.store(n);
    g_last_call_ms.store(now_ms());
    if (n % kHashEvery == 0) {
        dump_memory("own", n);
        ring_dump("own", g_ring_dir, n);
        send_hash(n, game_hash());
    }
    const std::uint16_t now = g_physical.load();
    for (std::uint64_t g = std::max(g_sent_to, n) + 1; g <= n + delay_frames(); ++g) {
        g_local[g] = now;
        send_keys(g, now);
    }
    g_sent_to = std::max(g_sent_to, n + delay_frames());
    std::uint16_t v = 0x03FFu;
    if (const auto it = g_local.find(n); it != g_local.end()) v = it->second;
    g_local.erase(g_local.begin(), g_local.upper_bound(n));
    apply_keys(v);
}

// The copy's read n: its hash every 64th, then read n's buttons as the other
// machine sent them (waiting for them: the other player's game is held here).
void mirror_input() {
    const std::uint64_t n = ++g_m_calls;
    if (n % kHashEvery == 0 && g_m_hash_fd >= 0) {
        dump_memory("copy", n);
        ring_dump("copy", ".", n);
        pipe_write(g_m_hash_fd, Rec{n, game_hash()});
    }
    std::uint16_t v = 0x03FFu;
    {
        std::unique_lock<std::mutex> lock(g_m_mu);
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        auto note_at = Clock::now() + std::chrono::seconds(1);
        while (!g_m_closed && !g_m_keys.count(n)) {
            if (g_m_cv.wait_until(lock, std::min(deadline, note_at)) == std::cv_status::timeout) {
                if (Clock::now() >= deadline) break;
                std::fprintf(stderr, "vs-net: the copy waits for read %llu's buttons (it has %llu..%llu)\n",
                             static_cast<unsigned long long>(n),
                             g_m_keys.empty() ? 0ull : static_cast<unsigned long long>(g_m_keys.begin()->first),
                             g_m_keys.empty() ? 0ull : static_cast<unsigned long long>(g_m_keys.rbegin()->first));
                note_at = Clock::now() + std::chrono::seconds(5);
            }
        }
        const auto it = g_m_keys.find(n);
        if (it == g_m_keys.end()) {
            std::fprintf(stderr, "vs-net: the copy ends: no buttons for read %llu%s\n",
                         static_cast<unsigned long long>(n), g_m_closed ? " (the session closed)" : " in 30 s");
            std::fflush(stderr);
            std::_Exit(3);
        }
        v = it->second;
        g_m_keys.erase(g_m_keys.begin(), g_m_keys.upper_bound(n));
    }
    apply_keys(v);
}

int on_before_pad(std::uint32_t, int, ArmCpuState* cpu) {
    if ((cpu->R[14] & ~1u) != kBeforePadReturn) return 0;
    if (vs_mirror_child()) {
        gba::GbaPpu* ppu = gbarecomp::active_ppu();
        if (ppu && ppu->frame_count() >= g_m_frame0) mirror_input();
    } else if (g_play.load() && g_counting.load()) {
        play_input();
    }
    return 0;   // declined: the game's own routine runs (KEYINPUT is set)
}

void install_pad_hook() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!gba_mod_register_function_entry_plugin(kPadHookId, kBeforePad, 1, &on_before_pad) ||
        !gba_mod_set_function_hook_enabled(kPadHookId, 1))
        std::fprintf(stderr, "vs-net: the pad hook NOT installed (game.toml's [[mod_function_hook]] 0x08000A04?)\n");
}

}  // namespace

std::string file_fingerprint(const std::string& path) {
    if (path.empty()) return "none";
    std::ifstream f(path, std::ios::binary);
    if (!f) return "unreadable";
    std::uint64_t h = 1469598103934665603ull;
    std::vector<char> buf(1 << 16);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        h = fnv(reinterpret_cast<const std::uint8_t*>(buf.data()), static_cast<std::size_t>(f.gcount()), h);
    }
    return hex64(h);
}

void set_on_gone(void (*fn)()) { g_on_gone.store(fn); }

bool start(Side mine) {
    stop("a new try");
    auto n = std::make_shared<Net>();
    n->mine = std::move(mine);
    {
        std::lock_guard<std::mutex> lock(g_cur_mu);
        g_cur = n;
    }
    std::thread(run, n).detach();
    return true;
}

Status status() {
    const auto n = cur();
    return n ? n->status.load() : Status::Idle;
}

const std::string& failure() {
    static std::string s;
    const auto n = cur();
    if (n) {
        std::lock_guard<std::mutex> lock(n->mu);
        s = n->failure;
    } else {
        s.clear();
    }
    return s;
}

const Side& peer() {
    static const Side none;
    const auto n = cur();
    return n && n->status.load() == Status::Ready ? n->peer : none;
}

void set_key_sink(int fd) {
    const auto n = cur();
    if (!n) {
        if (fd >= 0) ::close(fd);
        return;
    }
    std::lock_guard<std::mutex> lock(n->mu);
    if (n->sink >= 0 && n->sink != fd) ::close(n->sink);   // (the session's: it owns the pipe's end)
    n->sink = fd;
    if (fd >= 0) {
        for (const Rec& r : n->pending) pipe_write(fd, r);
        n->pending.clear();
    }
}

void send_keys(std::uint64_t frame, std::uint16_t keys) {
    if (const auto n = cur()) {
        const Rec r{frame, keys};
        n->send_msg(mKeys, &r, sizeof r);
    }
}

void send_hash(std::uint64_t frame, std::uint64_t hash) {
    if (const auto n = cur()) {
        const Rec r{frame, hash};
        n->send_msg(mHash, &r, sizeof r);
    }
}

bool take_peer_hash(std::uint64_t frame, std::uint64_t* hash) {
    const auto n = cur();
    if (!n) return false;
    std::lock_guard<std::mutex> lock(n->mu);
    const auto it = n->peer_hashes.find(frame);
    if (it == n->peer_hashes.end()) return false;
    *hash = it->second;
    n->peer_hashes.erase(it);
    return true;
}

bool peer_gone(std::string* why) {
    const auto n = cur();
    if (!n || n->status.load() != Status::Ready) return false;
    std::lock_guard<std::mutex> lock(n->mu);
    if (!n->gone && Clock::now() - n->last_rx > kSilence) {
        n->gone = true;
        n->gone_why = "nothing from the other player for 15 s";
    }
    if (n->gone && why) *why = n->gone_why;
    return n->gone;
}

void stop(const char* why) {
    std::shared_ptr<Net> n;
    {
        std::lock_guard<std::mutex> lock(g_cur_mu);
        n.swap(g_cur);
    }
    if (!n) return;
    n->stopping.store(true);
    if (n->status.load() == Status::Ready || n->status.load() == Status::Exchanging) {
        const std::string w = why ? why : "";
        n->send_msg(mBye, w.data(), w.size());
    }
    const int fd = n->fd.load();
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    std::lock_guard<std::mutex> lock(n->mu);
    if (n->sink >= 0) ::close(n->sink);
    n->sink = -1;
}

// ── The button timeline ──────────────────────────────────────────────────

void play_begin(std::uint16_t keyinput, const std::string& dir) {
    install_pad_hook();
    g_ring_dir = dir;
    g_calls = 0;
    g_sent_to = 0;
    g_counting.store(false);
    g_local.clear();
    g_mirror_hashes.clear();
    g_desync = false;
    g_desync_what.clear();
    g_hash_checks = 0;
    g_hash_fd = -1;
    g_applied.store(keyinput);
    g_physical.store(keyinput);
    g_rewind_was = gbarecomp::g_runtime_rewind_blocked;
    gbarecomp::g_runtime_rewind_blocked = 1;   // no going back in time in a match
    settings_net_match(true);
    g_play.store(true);
}

void play_attach_hash_fd(int fd) { g_hash_fd = fd; }

void play_start_keys() {
    for (std::uint64_t n = 1; n <= delay_frames(); ++n) {
        g_local[n] = 0x03FFu;
        send_keys(n, 0x03FFu);
    }
    g_sent_to = std::max<std::uint64_t>(g_sent_to, delay_frames());
}

void play_reset_calls() {
    g_calls = 0;
    g_counting.store(true);
    g_last_call_ms.store(now_ms());
}

void play_tick(std::uint64_t frame) {
    if (!g_play.load()) return;
    static bool watching = false;
    if (!watching) {   // a line when the game's pad reads stop for a second (a stall)
        watching = true;
        std::thread([] {
            std::uint64_t told = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!g_play.load() || !g_counting.load()) continue;
                const std::uint64_t n = g_last_call.load();
                if (now_ms() - g_last_call_ms.load() > 1000 && n != told) {
                    told = n;
                    std::fprintf(stderr, "vs-net: this game stalls after read %llu (buttons sent to %llu)\n",
                                 static_cast<unsigned long long>(n),
                                 static_cast<unsigned long long>(n + delay_frames()));
                }
            }
        }).detach();
    }
    // The copy's hashes of the other game against the other machine's own.
    if (g_hash_fd >= 0) {
        Rec r;
        while (::read(g_hash_fd, &r, sizeof r) == static_cast<ssize_t>(sizeof r)) g_mirror_hashes[r.frame] = r.value;
    }
    for (auto it = g_mirror_hashes.begin(); it != g_mirror_hashes.end();) {
        std::uint64_t theirs = 0;
        if (!take_peer_hash(it->first, &theirs)) {
            ++it;
            continue;
        }
        ++g_hash_checks;
        if (theirs != it->second && !g_desync) {
            g_desync = true;
            char b[160];
            std::snprintf(b, sizeof b, "the other game's read %llu: %s there, %s here",
                          static_cast<unsigned long long>(it->first), hex64(theirs).c_str(),
                          hex64(it->second).c_str());
            g_desync_what = b;
            std::fprintf(stderr, "vs-net: DESYNC: %s\n", b);
        }
        it = g_mirror_hashes.erase(it);
    }
    while (g_mirror_hashes.size() > 256) g_mirror_hashes.erase(g_mirror_hashes.begin());
    if (frame % 600 == 0)
        std::fprintf(stderr, "vs-net: frame %llu, read %llu, %llu hashes matched%s\n",
                     static_cast<unsigned long long>(frame), static_cast<unsigned long long>(g_calls), g_hash_checks,
                     g_desync ? " (DESYNC)" : "");
}

std::uint16_t play_filter(std::uint16_t keys) {
    g_physical.store(keys);
    return g_applied.load();
}

bool play_active() { return g_play.load(); }

bool play_desync(std::string* what) {
    if (g_desync && what) *what = g_desync_what;
    return g_desync;
}

void play_end() {
    if (!g_play.exchange(false)) return;
    g_counting.store(false);
    gbarecomp::g_runtime_rewind_blocked = g_rewind_was;
    settings_net_match(false);
    if (g_hash_fd >= 0) ::close(g_hash_fd);
    g_hash_fd = -1;
    std::fprintf(stderr, "vs-net: the match's buttons end (read %llu, %llu hashes matched%s)\n",
                 static_cast<unsigned long long>(g_calls), g_hash_checks, g_desync ? ", desync" : "");
}

// ── The copy ─────────────────────────────────────────────────────────────

void mirror_settings() {
    const char* dir = std::getenv("KOL_VS_MIRROR_CFG");
    unsigned w = 0, h = 0;
    if (const char* p = std::getenv("KOL_VS_MIRROR_PANEL")) std::sscanf(p, "%ux%u", &w, &h);
    settings_init(dir && *dir ? dir : ".", 240u, w, h);
    sound_choice_set(0);   // no one hears it: the game's own sound (HQ/HD only add work, outside the game)
}

void mirror_install() {
    const char* k = std::getenv("KOL_VS_MIRROR_KEYS_FD");
    const char* h = std::getenv("KOL_VS_MIRROR_HASH_FD");
    const char* f0 = std::getenv("KOL_VS_MIRROR_FRAME0");
    const int kfd = k && *k ? std::atoi(k) : -1;
    g_m_hash_fd = h && *h ? std::atoi(h) : -1;
    g_m_frame0 = f0 && *f0 ? std::strtoull(f0, nullptr, 10) : 1ull;
    if (kfd >= 0) std::thread(mirror_reader, kfd).detach();
    gbarecomp::g_host_keyinput_filter = &mirror_filter;   // the other player's buttons only
    gbarecomp::g_runtime_rewind_blocked = 1;
    settings_net_match(true);
    install_pad_hook();
    std::fprintf(stderr,
                 "vs-net: this is the other player's game (keys fd %d, hashes fd %d, from frame %llu, delay %llu)\n",
                 kfd, g_m_hash_fd, static_cast<unsigned long long>(g_m_frame0),
                 static_cast<unsigned long long>(delay_frames()));
}

void mirror_tick(std::uint64_t) {}

namespace {
// KOL_VS_NET_DUMP=<dir> (tests): the memory hashed, at the first 4 hash
// reads from KOL_VS_NET_DUMP_FROM (default 1), to
// <dir>/<own|copy>_<read>_<pid>.bin (EWRAM then IWRAM).
void dump_memory(const char* kind, std::uint64_t n) {
    static const char* dir = std::getenv("KOL_VS_NET_DUMP");
    static int left = 4;
    static const std::uint64_t from = [] {
        const char* e = std::getenv("KOL_VS_NET_DUMP_FROM");
        return e && *e ? std::strtoull(e, nullptr, 10) : 1ull;
    }();
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!dir || !*dir || !bus || left <= 0 || n == 0 || n < from) return;
    --left;
    char path[512];
    std::snprintf(path, sizeof path, "%s/%s_%llu_%d.bin", dir, kind, static_cast<unsigned long long>(n),
                  static_cast<int>(::getpid()));
    if (std::FILE* f = std::fopen(path, "wb")) {
        std::fwrite(bus->ewram_ptr(), 1, 0x40000, f);
        std::fwrite(bus->iwram_ptr(), 1, 0x8000, f);
        std::fclose(f);
    }
}
}  // namespace

namespace {
void ring_dump(const char* kind, const std::string& dir, std::uint64_t n) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (dir.empty() || !bus) return;
    char path[1024];
    std::snprintf(path, sizeof path, "%s/%s_%llu.bin", dir.c_str(), kind, static_cast<unsigned long long>(n));
    if (std::FILE* f = std::fopen(path, "wb")) {
        std::fwrite(bus->ewram_ptr(), 1, 0x40000, f);
        std::fwrite(bus->iwram_ptr(), 1, 0x8000, f);
        std::fclose(f);
    }
    if (n > 4 * kHashEvery) {
        std::snprintf(path, sizeof path, "%s/%s_%llu.bin", dir.c_str(), kind,
                      static_cast<unsigned long long>(n - 4 * kHashEvery));
        std::remove(path);
    }
}
}  // namespace

std::uint64_t game_hash() {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return 0;
    // All of it, stack included: taken at a pad read, the same instruction
    // in both copies. (Two copies differed in the stack and in a stored stack
    // pointer until a state save brought the devices up to the CPU and a load
    // dropped the loader's own unflushed cycles: gbarecomp
    // runtime_snapshot_flush / runtime_snapshot_loaded, 2026-11-22.)
    std::uint64_t h = fnv(bus->ewram_ptr(), 0x40000);
    h = fnv(bus->iwram_ptr(), 0x8000, h);
    if (std::getenv("KOL_VS_NET_HASH_LOG")) {
        std::fprintf(stderr, "vs-net: hash %s pc=%08X cpsr=%08X\n", hex64(h).c_str(), g_cpu.R[15], g_cpu.cpsr);
    }
    return h;
}

}  // namespace vsnet
}  // namespace kol

#endif
