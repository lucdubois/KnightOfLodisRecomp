// kol_vs_cpu.cpp — VS Mode's other GBA: the computer, or a player on the
// local network (kol_vs_cpu.h, kol_vs_net.h).
//
// The player's game:
//   1. VS Mode's "Connecting..." asks the runtime for a state save into its
//      own slot, at the next dispatch boundary. The screen: the menu's screen
//      0x08246C18 (0x02029B80), its cursor on VS (0x03000ED1 1; Exchange's
//      Connecting is the same screen with 2), 0x02002830 0x24, outside a
//      battle, no link; its first frame, the port in any mode. Not VS's world
//      state (0x02029BD0 0x08246D18): that is NAVI's, set only when VS's
//      eight pages of rules are shown, which a save that has seen them skips
//      (the first trigger never fired there, 2026-11-22); and set from the
//      first page, where a copy started waited for an A no one pressed.
//   2. The save written (vs_cpu_state_saved), the game is held
//      (g_runtime_pause_request: the window stays alive) and:
//      - Computer: the link starts as the parent on a free local port at the
//        snapshot's time (GbaPpu::guest_cycle) and a copy of the game starts
//        from that state, played by the game's AI (KOL_VS_OPPONENT=1).
//      - Network: from the snapshot on the game sees only the match's button
//        timeline (kol_vs_net); it looks for another game held on Connecting
//        on the local network (B gives up), and the two swap snapshots. Then
//        the other player's game starts here, unseen, from its snapshot
//        (KOL_VS_MIRROR=1), its buttons from the network; the smaller random
//        number's game is the link's parent on both machines.
//      Either way the game is let go once its copy says hello on the link.
//   3. The game's port out of Multi-Player mode for 3 s (it closed its link),
//      the link lost, or the other player gone: the link stops and the copy
//      ends.
// A copy: no window, no sound, files in a session folder of its own; it ends
// when the link drops, when its parent goes, or when no link comes in 20 s.
//
// Desktop only (Mac, Linux): gbarecomp's link has no Windows transport yet,
// and Android would need a process of its own (a service).

#include "kol_vs_cpu.h"

#include "kol_settings.h"
#include "kol_vs_net.h"

#if defined(_WIN32) || defined(__ANDROID__)

namespace kol {
bool vs_cpu_child() { return false; }
void vs_cpu_args(const std::vector<std::string>&) {}
void vs_cpu_install() {}
void vs_cpu_tick() {}
const char* vs_cpu_state_path(int, int) { return nullptr; }
bool vs_cpu_state_saved(const char*) { return false; }
}  // namespace kol

#else

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "kol_cheevos.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "sio_link.h"

extern char** environ;

namespace kol {
namespace {

constexpr int kSlot = 0x5653;                       // "VS": the snapshot's slot (its path is ours)
constexpr std::uint32_t kWorld = 0x29BD0;           // EWRAM: the world state's pointer (VS's NAVI pages on: logs only)
constexpr std::uint32_t kScreen = 0x29B80;          // EWRAM: the Quest/VS/Exchange menu's screen
constexpr std::uint32_t kConnecting = 0x08246C18u;  // ... "Connecting..." (0x08246CA8 the menu itself)
constexpr std::uint32_t kCursor = 0xED1;            // IWRAM: the menu's cursor, kept on Connecting: 0 Quest, 1 VS, 2 Exchange
constexpr std::uint32_t kMode = 0x12E0;             // EWRAM: 4 from the battle's preparation on
constexpr std::uint32_t kScene = 0x2830;            // EWRAM: 0x24 and +6 0x1A on Connecting, 0x46 VS's NAVI pages
                                                    // (the screen pointer is 0x08246C18 from the menu's A on)
constexpr int kOffTicks = 180;                      // the port out of Multi-Player mode this long: link closed
constexpr auto kLinkWait = std::chrono::seconds(20);
constexpr auto kSearchWait = std::chrono::minutes(2);

enum Phase : int { kIdle, kSaving, kSearching, kLinking, kLinked };

std::atomic<int> g_phase{kIdle};
std::mutex g_mu;                 // the session (the key filter drives it while the game is held)
std::vector<std::string> g_args;
std::string g_dir;               // the session's folder: the snapshot(s), the copy's save and log
std::string g_state;             // g_dir/vs.state
bool g_net = false;              // this session is over the network
pid_t g_pid = -1;
bool g_failed_here = false;      // a try failed on this Connecting screen: not again until it is left
int g_wait = 0;
int g_off = 0;
bool g_multi_was = false;
std::uint64_t g_snap_clock = 0;  // the snapshot's guest cycle and frame
std::uint64_t g_snap_frame = 0;
std::uint64_t g_my_nonce = 0;    // this try's random number (network)
bool g_link_parent = false;      // (network) this game is the local link's parent
int g_link_port = -1;            // (network) the local link's port
std::chrono::steady_clock::time_point g_link_start, g_search_start, g_notice_at;
bool g_b_was = true;             // B held when the search began (a new press gives up)
std::uint16_t (*g_prev_filter)(std::uint16_t) = nullptr;

std::uint32_t rd32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// The game's serial port in Multi-Player mode (RCNT bit 15 clear, SIOCNT
// mode 2): it is talking to, or looking for, the other GBA.
bool port_multi(const std::uint8_t* io) {
    const unsigned siocnt = io[0x128] | (io[0x129] << 8);
    const unsigned rcnt = io[0x134] | (io[0x135] << 8);
    return (rcnt & 0x8000u) == 0 && (siocnt & 0x3000u) == 0x2000u;
}

std::string self_exe() {
#if defined(__APPLE__)
    std::uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);
    std::string s(n, '\0');
    if (_NSGetExecutablePath(s.data(), &n) != 0) return {};
    s.resize(std::strlen(s.c_str()));
    return s;
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return buf;
#endif
}

// The value after `flag` in the run's arguments, made absolute (the copy runs
// in its session folder), or "".
std::string arg_value(const char* flag) {
    for (std::size_t i = 0; i + 1 < g_args.size(); ++i)
        if (g_args[i] == flag) {
            std::error_code ec;
            const auto abs = std::filesystem::absolute(g_args[i + 1], ec);
            return ec ? g_args[i + 1] : abs.string();
        }
    return {};
}
bool arg_flag(const char* flag) {
    for (const auto& a : g_args)
        if (a == flag) return true;
    return false;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool write_file(const std::string& path, const void* p, std::size_t n) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

// The settings that change how the game itself runs: the computer's copy
// takes the player's, so the two games run alike (pictures are left at the
// copy's defaults: no one sees them). A network copy takes all of its
// player's (their enhancements.cfg).
const Setting kShared[] = {Setting::FastLoading, Setting::FastActions, Setting::FastWalk,  Setting::QuickEnemy,
                           Setting::FastAttack,  Setting::FastSpells,  Setting::SmoothPacing};

struct Spawn {
    std::string link;                 // GBARECOMP_LINK: "child:<port>" or "parent:<port>"
    std::string state, save;          // what it starts from
    std::vector<std::string> env;     // its own switches
    int keys_fd = -1, hash_fd = -1;   // passed as fds 3 and 4 (a network copy)
    const char* who = "the computer's game";
};

bool spawn(const Spawn& sp) {
    const std::string exe = self_exe();
    const std::string rom = arg_value("--rom");
    if (exe.empty() || rom.empty()) {
        std::fprintf(stderr, "vs-cpu: no %s to start %s with\n", exe.empty() ? "program path" : "ROM", sp.who);
        return false;
    }
    std::vector<std::string> argv = {exe, "--rom", rom};
    if (const std::string b = arg_value("--bios"); !b.empty()) { argv.push_back("--bios"); argv.push_back(b); }
    else if (arg_flag("--no-bios")) argv.push_back("--no-bios");
    if (const std::string m = arg_value("--rom-patch"); !m.empty()) { argv.push_back("--rom-patch"); argv.push_back(m); }
    argv.insert(argv.end(), {"--save-path", sp.save, "--load-state", sp.state, "--window", "--scale", "1"});

    // Its environment: the player's, less every game, runtime and SDL switch,
    // then the copy's own. KOL_VS_CHILD_ENV="A=1 B=2" adds more (tests).
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string v = *e;
        if (v.rfind("KOL_", 0) == 0 || v.rfind("GBARECOMP_", 0) == 0 || v.rfind("SDL_", 0) == 0) continue;
        env.push_back(v);
    }
    env.insert(env.end(), {"GBARECOMP_LINK=" + sp.link, "GBARECOMP_SELFHEAL_RECOMPILE=0", "SDL_VIDEODRIVER=dummy",
                           "SDL_RENDER_DRIVER=software", "SDL_AUDIODRIVER=dummy"});
    env.insert(env.end(), sp.env.begin(), sp.env.end());
    if (const char* extra = std::getenv("KOL_VS_CHILD_ENV"); extra && *extra) {
        std::string s = extra;
        for (std::size_t at = 0; at < s.size();) {
            const std::size_t sp_at = s.find(' ', at);
            const std::string t = s.substr(at, sp_at == std::string::npos ? std::string::npos : sp_at - at);
            if (!t.empty()) env.push_back(t);
            if (sp_at == std::string::npos) break;
            at = sp_at + 1;
        }
    }

    std::vector<char*> av, ev;
    for (auto& a : argv) av.push_back(a.data());
    av.push_back(nullptr);
    for (auto& e : env) ev.push_back(e.data());
    ev.push_back(nullptr);
    const std::string log = g_dir + (g_net ? "/other_player.log" : "/opponent.log");
    const int log_fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const std::string dir = g_dir;
    const int kfd = sp.keys_fd, hfd = sp.hash_fd;

    const pid_t pid = ::fork();
    if (pid < 0) {
        if (log_fd >= 0) ::close(log_fd);
        std::fprintf(stderr, "vs-cpu: could not start %s\n", sp.who);
        return false;
    }
    if (pid == 0) {   // (only async-signal-safe calls from here)
        if (log_fd >= 0) {
            ::dup2(log_fd, 1);
            ::dup2(log_fd, 2);
        }
        const int null_fd = ::open("/dev/null", O_RDONLY);
        if (null_fd >= 0) ::dup2(null_fd, 0);
        // The pipes to fds 3 and 4, by way of high numbers (either may be 3
        // or 4 already); dup2 clears close-on-exec.
        const int k = kfd >= 0 ? ::fcntl(kfd, F_DUPFD, 20) : -1;
        const int h = hfd >= 0 ? ::fcntl(hfd, F_DUPFD, 20) : -1;
        if (k >= 0) ::dup2(k, 3);
        if (h >= 0) ::dup2(h, 4);
        if (::chdir(dir.c_str()) != 0) ::_exit(126);
        ::execve(av[0], av.data(), ev.data());
        ::_exit(127);
    }
    if (log_fd >= 0) ::close(log_fd);
    g_pid = pid;
    std::fprintf(stderr, "vs-cpu: %s started (pid %d, link %s, log %s)\n", sp.who, static_cast<int>(pid),
                 sp.link.c_str(), log.c_str());
    return true;
}

// The copy stopped and the session's folder gone (under g_mu).
void reap_child() {
    if (g_pid > 0) {
        int st = 0;
        pid_t r = ::waitpid(g_pid, &st, WNOHANG);
        if (r == 0) {
            ::kill(g_pid, SIGTERM);
            for (int i = 0; i < 50 && r == 0; ++i) {   // up to 0.5 s, then for sure
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                r = ::waitpid(g_pid, &st, WNOHANG);
            }
            if (r == 0) {
                ::kill(g_pid, SIGKILL);
                ::waitpid(g_pid, &st, 0);
            }
        }
        g_pid = -1;
    }
    if (!g_dir.empty() && !std::getenv("KOL_VS_KEEP")) {   // KOL_VS_KEEP=1: keep it (its log)
        std::error_code ec;
        std::filesystem::remove_all(g_dir, ec);
    }
    g_dir.clear();
    g_state.clear();
}

void notice(const char* line) {
    std::fprintf(stderr, "vs-cpu: notice: %s\n", line);
    cheevos_notice("VS Mode", line);
}

// The session over (under g_mu): the network and the link stopped, the copy
// gone, the game let go if it was held.
void end_session(const char* why) {
    std::fprintf(stderr, "vs-cpu: session ends: %s\n", why);
    if (g_net) {
        vsnet::stop(why);
        vsnet::set_key_sink(-1);
        vsnet::play_end();
    }
    gba::SioLink::stop();
    reap_child();
    gbarecomp::g_runtime_pause_request = 0;
    g_phase.store(kIdle);
}

// A free local TCP port (the network copy listens on it as the link's
// parent), or -1.
int free_port() {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    int port = -1;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0)
        port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

// Network: the other player found and their snapshot here. Their game
// starts, unseen, linked to this one (under g_mu). False: given up.
bool start_other_game() {
    const vsnet::Side& peer = vsnet::peer();
    const std::string state = g_dir + "/other.state", save = g_dir + "/other.sav", cfg_dir = g_dir + "/other";
    std::error_code ec;
    std::filesystem::create_directories(cfg_dir, ec);
    if (!write_file(state, peer.state.data(), peer.state.size()) || !write_file(save, peer.save.data(), peer.save.size()) ||
        (!peer.cfg.empty() && !write_file(cfg_dir + "/enhancements.cfg", peer.cfg.data(), peer.cfg.size()))) {
        end_session("the other player's game could not be written here");
        return false;
    }
    int kp[2] = {-1, -1}, hp[2] = {-1, -1};
    if (::pipe(kp) != 0 || ::pipe(hp) != 0) {
        end_session("no pipes to the other player's game");
        return false;
    }
    for (int fd : {kp[0], kp[1], hp[0], hp[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    ::fcntl(hp[0], F_SETFL, ::fcntl(hp[0], F_GETFL) | O_NONBLOCK);
    ::signal(SIGPIPE, SIG_IGN);   // a copy gone: its pipe's write fails, not the game

    Spawn sp;
    sp.state = state;
    sp.save = save;
    sp.who = "the other player's game";
    sp.keys_fd = kp[0];
    sp.hash_fd = hp[1];
    char panel[32];
    std::snprintf(panel, sizeof panel, "%ux%u", peer.panel_w, peer.panel_h);
    sp.env = {"KOL_VS_MIRROR=1", "KOL_VS_MIRROR_KEYS_FD=3", "KOL_VS_MIRROR_HASH_FD=4", "KOL_VS_MIRROR_CFG=" + cfg_dir,
              std::string("KOL_VS_MIRROR_PANEL=") + panel, "KOL_VS_MIRROR_FRAME0=" + std::to_string(peer.frame)};
    if (const char* d = std::getenv("KOL_VS_NET_DELAY")) sp.env.push_back(std::string("KOL_VS_NET_DELAY=") + d);

    // The smaller number's game is the link's parent, on both machines. This
    // game's end of the local link is made as its snapshot is loaded back
    // (vs_cpu_state_path), so the link knows it only from there, as the copy
    // over there knows its own.
    g_link_parent = g_my_nonce < peer.nonce;
    g_link_port = free_port();
    bool ok = false;
    if (g_link_port > 0) {
        sp.link = (g_link_parent ? "child:" : "parent:") + std::to_string(g_link_port);
        ok = spawn(sp);
    }
    ::close(kp[0]);
    ::close(hp[1]);
    if (!ok) {
        ::close(kp[1]);
        ::close(hp[0]);
        end_session("the other player's game could not be started");
        return false;
    }
    vsnet::set_key_sink(kp[1]);
    vsnet::play_attach_hash_fd(hp[0]);
    // This game from its snapshot loaded too, the way its copy over there
    // starts (what ran here since the snapshot is undone, and its pad reads
    // are counted from the load on: vs_cpu_state_path).
    gbarecomp::runtime_request_state_load(kSlot);
    vsnet::play_start_keys();
    g_link_start = std::chrono::steady_clock::now();
    g_phase.store(kLinking);
    std::string line = "Playing " + (peer.name.empty() ? std::string("a player") : peer.name);
    notice(line.c_str());
    return true;
}

// While the game is held: linked, or given up on.
void check_linking() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_phase.load() != kLinking) return;
    gba::SioLink* l = gba::SioLink::instance();
    if (l && l->connected()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                              g_link_start).count();
        std::fprintf(stderr, "vs-cpu: linked to %s after %lld ms\n",
                     g_net ? "the other player's game" : "the computer's game", static_cast<long long>(ms));
        g_phase.store(kLinked);
        g_off = 0;
        gbarecomp::g_runtime_pause_request = 0;
        return;
    }
    int st = 0;
    const bool gone = g_pid > 0 && ::waitpid(g_pid, &st, WNOHANG) == g_pid;
    if (gone) g_pid = -1;
    std::string why;
    if (g_net && vsnet::peer_gone(&why)) {
        end_session(why.c_str());
        g_failed_here = true;
        return;
    }
    if (gone || std::chrono::steady_clock::now() - g_link_start > kLinkWait) {
        end_session(gone ? "the copy ended before linking" : "no link in 20 s");
        g_failed_here = true;
    }
}

// Network, while the game is held on Connecting: found, failed, or given up.
void check_search(std::uint16_t keys) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_phase.load() != kSearching) return;
    const auto now = std::chrono::steady_clock::now();
    const bool b = (keys & 0x0002u) == 0;
    const bool b_press = b && !g_b_was;
    g_b_was = b;
    switch (vsnet::status()) {
    case vsnet::Status::Ready:
        start_other_game();
        return;
    case vsnet::Status::Failed: {
        const std::string why = vsnet::failure();
        notice(why.c_str());
        end_session(why.c_str());
        g_failed_here = true;
        return;
    }
    default:
        break;
    }
    if (b_press || now - g_search_start > kSearchWait) {
        notice(b_press ? "Stopped looking." : "No player found on this network.");
        end_session(b_press ? "the player stopped looking" : "no player found in 2 minutes");
        g_failed_here = true;
        return;
    }
    if (now >= g_notice_at) {
        notice("Looking for a player on this network... B: stop");
        g_notice_at = now + std::chrono::seconds(5);
    }
}

std::uint16_t filter(std::uint16_t keys) {
    if (g_prev_filter) keys = g_prev_filter(keys);
    const int ph = g_phase.load(std::memory_order_relaxed);
    if (ph == kSearching) check_search(keys);
    else if (ph == kLinking) check_linking();
    if (vsnet::play_active()) return vsnet::play_filter(keys);   // the match's timeline, not the pad's now
    return keys;
}

void at_exit() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_phase.load() != kIdle || g_pid > 0) {
        if (g_net) vsnet::stop("the other player quit");
        gba::SioLink::stop();
        reap_child();
    }
}

// A copy: ends with its link, its parent, or no link at all.
void child_watchdog() {
    const pid_t parent = ::getppid();
    const auto t0 = std::chrono::steady_clock::now();
    bool was = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const char* why = nullptr;
        int code = 0;
        gba::SioLink* l = gba::SioLink::instance();
        const bool on = l && l->connected();
        if (::getppid() != parent) why = "the player's game is gone";
        else if (on) was = true;
        else if (was) why = "the link closed";
        else if (std::chrono::steady_clock::now() - t0 > kLinkWait) why = "no link in 20 s", code = 2;
        if (why) {
            std::fprintf(stderr, "vs-cpu: the copy ends: %s\n", why);
            std::fflush(stderr);
            std::fflush(stdout);
            std::_Exit(code);
        }
    }
}

std::string host_name() {
    char b[256] = {0};
    if (::gethostname(b, sizeof b - 1) != 0) return {};
    std::string s = b;
    if (const auto dot = s.find(".local"); dot != std::string::npos) s.resize(dot);
    return s;
}

}  // namespace

bool vs_cpu_child() {
    static const bool on = [] {
        const char* e = std::getenv("KOL_VS_OPPONENT");
        return e && *e == '1';
    }();
    return on;
}

void vs_cpu_args(const std::vector<std::string>& args) { g_args = args; }

void vs_cpu_install() {
    static bool done = false;
    if (done) return;
    done = true;
    if (vs_cpu_child()) {
        std::fprintf(stderr, "vs-cpu: this is the computer's game (link child, AI army)\n");
        std::thread(child_watchdog).detach();
        return;
    }
    if (vs_mirror_child()) {
        vsnet::mirror_install();
        std::thread(child_watchdog).detach();
        return;
    }
    if (g_args.empty()) return;   // not the desktop's main (no command line to give the copy)
    g_prev_filter = gbarecomp::g_host_keyinput_filter;
    gbarecomp::g_host_keyinput_filter = &filter;
    std::atexit(at_exit);
}

void vs_cpu_tick() {
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    const std::uint64_t frame = ppu ? ppu->frame_count() : 0;
    if (vs_mirror_child()) {
        vsnet::mirror_tick(frame);
        return;
    }
    if (vs_cpu_child() || g_args.empty()) return;
    if (std::getenv("GBARECOMP_LINK")) return;   // a test pair: its own link
    gba::GbaBus* bus = gbarecomp::active_bus();
    const std::uint8_t* ew = bus ? bus->ewram_ptr() : nullptr;
    if (!ew) return;
    const bool multi = port_multi(bus->io().raw());
    if (multi != g_multi_was) {
        g_multi_was = multi;
        std::fprintf(stderr, "vs-cpu: the game's port %s Multi-Player mode (world %08X, mode %u)\n",
                     multi ? "enters" : "leaves", rd32(ew + kWorld), ew[kMode]);
    }
    const std::uint8_t* iw = bus->iwram_ptr();
    // VS's "Connecting..." (Exchange's is the same screen, cursor 2).
    const bool connecting = rd32(ew + kScreen) == kConnecting && iw[kCursor] == 1u && ew[kMode] != 4u;
    const bool connecting_screen = connecting && ew[kScene] == 0x24u && ew[kScene + 6] == 0x1Au;
    // KOL_VS_LOG=1: the bytes the Connecting screen is told by, when they change.
    if (static const bool log = std::getenv("KOL_VS_LOG") != nullptr; log) {
        char now[128];
        std::snprintf(now, sizeof now, "screen=%08X cursor=%u world=%08X ef0=%02X 2830=%02X 2836=%02X multi=%d",
                      rd32(ew + kScreen), iw[kCursor], rd32(ew + kWorld), iw[0xEF0], ew[0x2830], ew[0x2836],
                      multi ? 1 : 0);
        static std::string last;
        if (last != now) {
            std::fprintf(stderr, "vs-log: frame %llu %s\n", static_cast<unsigned long long>(frame), now);
            last = now;
        }
    }
    switch (g_phase.load()) {
    case kIdle: {
        if (!connecting) {
            g_failed_here = false;
            break;
        }
        // On the screen's first frame, whatever the port: a Connecting after
        // one that timed out puts the port in Multi-Player mode only ~32
        // frames in, and a snapshot taken there had the game decide with no
        // link (no transfer ever started on either side, 2026-11-22).
        if (g_failed_here || !connecting_screen || gba::SioLink::instance()) break;
        std::lock_guard<std::mutex> lock(g_mu);
        std::error_code ec;
        static int session = 0;
        const auto dir = std::filesystem::temp_directory_path(ec) /
                         ("kol-vs-" + std::to_string(::getpid()) + "-" + std::to_string(++session));
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::fprintf(stderr, "vs-cpu: no folder for the session (%s)\n", ec.message().c_str());
            g_failed_here = true;
            break;
        }
        g_net = vs_mode_choice_current() == 1u;
        g_dir = dir.string();
        g_state = g_dir + "/vs.state";
        g_wait = 0;
        g_phase.store(kSaving);
        std::fprintf(stderr, "vs-cpu: Connecting (%s): a snapshot (%s)\n", g_net ? "network" : "computer",
                     g_dir.c_str());
        gbarecomp::runtime_request_state_save(kSlot);
        break;
    }
    case kSaving:
        if (++g_wait > 300) {   // 5 s and no save: give up here
            std::lock_guard<std::mutex> lock(g_mu);
            end_session("the snapshot was not written");
            g_failed_here = true;
        }
        break;
    case kSearching:
        break;   // (held: the key filter runs it)
    case kLinking:
        check_linking();
        break;
    case kLinked: {
        if (g_net) vsnet::play_tick(frame);
        std::lock_guard<std::mutex> lock(g_mu);
        gba::SioLink* l = gba::SioLink::instance();
        std::string why;
        if (!l || !l->connected()) {
            end_session(g_net ? "the link to the other player's game was lost" : "the link to the computer's game was lost");
            g_failed_here = connecting;
            break;
        }
        if (g_net && vsnet::peer_gone(&why)) {
            notice("The other player left.");
            end_session(why.c_str());
            break;
        }
        if (g_net && vsnet::play_desync(&why) && !std::getenv("KOL_VS_NET_DESYNC_GO_ON")) {
            notice("The two games went out of step.");
            end_session(("desync: " + why).c_str());
            break;
        }
        g_off = multi ? 0 : g_off + 1;
        if (g_off > kOffTicks) end_session("the game closed its link");
        break;
    }
    default:
        break;
    }
}

const char* vs_cpu_state_path(int slot, int save) {
    if (slot != kSlot) return nullptr;
    if (save) return g_phase.load() == kSaving ? g_state.c_str() : nullptr;
    // Network: the game starts the match from its snapshot loaded, as its copy
    // on the other machine does; its pad reads are counted from here.
    if (!g_net || g_phase.load() != kLinking) return nullptr;
    vsnet::play_reset_calls();
    if (g_link_parent) gba::SioLink::start_parent(g_snap_clock, g_link_port);
    else gba::SioLink::start_child(g_link_port, g_snap_clock);
    return g_state.c_str();
}

bool vs_cpu_state_saved(const char* path) {
    if (g_phase.load() != kSaving || !path || g_state != path) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    gba::GbaBus* bus = gbarecomp::active_bus();
    g_snap_clock = ppu ? ppu->guest_cycle() : 0;
    g_snap_frame = ppu ? ppu->frame_count() : 0;
    if (!g_net) {
        const int port = ppu ? gba::SioLink::start_parent(g_snap_clock) : -1;
        Spawn sp;
        sp.state = g_state;
        sp.save = g_dir + "/totkol.sav";
        std::error_code ec;
        const std::string own = arg_value("--save-path");
        if (!own.empty() && std::filesystem::exists(own, ec))
            std::filesystem::copy_file(own, sp.save, std::filesystem::copy_options::overwrite_existing, ec);
        std::string on, off;
        for (const Setting s : kShared) {
            std::string& to = setting_on(s) ? on : off;
            to += (to.empty() ? "" : ",") + std::string(setting_key(s));
        }
        sp.env = {"KOL_VS_OPPONENT=1", "KOL_SETTINGS_ON=" + on, "KOL_SETTINGS_OFF=" + off};
        if (port > 0) sp.link = "child:" + std::to_string(port);
        if (port < 0 || !spawn(sp)) {
            end_session(port < 0 ? "no link could be started" : "the computer's game could not be started");
            g_failed_here = true;
            return true;
        }
        g_link_start = std::chrono::steady_clock::now();
        g_phase.store(kLinking);
        gbarecomp::g_runtime_pause_request = 1;   // held until the copy says hello (check_linking)
        return true;
    }
    // Network: this game's side, then the search.
    vsnet::Side mine;
    std::random_device rd;
    mine.nonce = (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
                 static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    if (mine.nonce == 0) mine.nonce = 1;
    g_my_nonce = mine.nonce;
    mine.name = host_name();
    mine.rom = vsnet::file_fingerprint(arg_value("--rom"));
    mine.bios = arg_flag("--no-bios") ? "no-bios" : vsnet::file_fingerprint(arg_value("--bios"));
    mine.mod = vsnet::file_fingerprint(arg_value("--rom-patch"));
    mine.frame = g_snap_frame;
    mine.clock = g_snap_clock;
    settings_panel(&mine.panel_w, &mine.panel_h);
    mine.state = read_file(g_state);
    const std::string own_save = arg_value("--save-path");
    if (!own_save.empty()) mine.save = read_file(own_save);
    const std::string cfg = settings_file_path();
    if (!cfg.empty()) {
        const auto c = read_file(cfg);
        mine.cfg.assign(c.begin(), c.end());
    }
    const std::uint16_t keyinput = bus ? static_cast<std::uint16_t>(bus->io().raw()[0x130] |
                                                                    (bus->io().raw()[0x131] << 8))
                                       : 0x03FFu;
    if (mine.state.empty() || !vsnet::start(std::move(mine))) {
        end_session("the network could not be started");
        g_failed_here = true;
        return true;
    }
    vsnet::play_begin(keyinput);
    g_search_start = std::chrono::steady_clock::now();
    g_notice_at = g_search_start;
    g_b_was = true;
    g_phase.store(kSearching);
    gbarecomp::g_runtime_pause_request = 1;   // held on Connecting while it looks (check_search)
    return true;
}

}  // namespace kol

#endif
