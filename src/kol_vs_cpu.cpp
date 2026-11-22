// kol_vs_cpu.cpp — VS Mode against the computer (kol_vs_cpu.h).
//
// The parent (the player's game):
//   1. VS Mode's "Connecting..." (world state 0x08246D18 outside a battle,
//      no link, the serial port in Multi-Player mode: the game is looking for
//      the other GBA) asks the runtime for a state save into its own slot, at
//      the next dispatch boundary.
//   2. The save written (vs_cpu_state_saved): the link starts as the parent
//      on a free local port at the snapshot's time (GbaPpu::guest_cycle), the
//      copy is started from that state, and the guest is held
//      (g_runtime_pause_request: the window stays alive) until the copy says
//      hello. Both games then go on linked from the same point, as two games
//      loading the same state do (the pairs tested 2026-11-22).
//   3. The game's port out of Multi-Player mode for 3 s (it closed its link),
//      or the link lost: the link stops and the copy ends.
// The copy (KOL_VS_OPPONENT=1): the AI army on from the start, no window, no
// sound, files in a session folder of its own; it ends when the link drops,
// when its parent goes, or when no link comes within 20 s.
//
// Desktop only (Mac, Linux): gbarecomp's link has no Windows transport yet,
// and Android would need a process of its own (a service).

#include "kol_vs_cpu.h"

#include "kol_settings.h"

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
#include <signal.h>
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
#include <mutex>
#include <thread>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "sio_link.h"

extern char** environ;

namespace kol {
namespace {

constexpr int kSlot = 0x5653;                       // "VS": the snapshot's slot (its path is ours)
constexpr std::uint32_t kWorld = 0x29BD0;           // EWRAM: the world state's pointer
constexpr std::uint32_t kConnecting = 0x08246D18u;  // ... VS Mode's connecting / attack order / roulette
constexpr std::uint32_t kMode = 0x12E0;             // EWRAM: 4 from the battle's preparation on
constexpr int kOffTicks = 180;                      // the port out of Multi-Player mode this long: link closed
constexpr auto kLinkWait = std::chrono::seconds(20);

enum Phase : int { kIdle, kSaving, kLinking, kLinked };

std::atomic<int> g_phase{kIdle};
std::mutex g_mu;                 // the session (check_linking runs from the key filter too)
std::vector<std::string> g_args;
std::string g_dir;               // the session's folder: the snapshot, the copy's save and log
std::string g_state;             // g_dir/vs.state
pid_t g_pid = -1;
bool g_failed_here = false;      // a try failed on this Connecting screen: not again until it is left
int g_wait = 0;
int g_off = 0;
bool g_multi_was = false;
std::chrono::steady_clock::time_point g_link_start;
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

// The settings that change how the game itself runs: the copy takes the
// player's, so the two games run alike (pictures are left at the copy's
// defaults: no one sees them).
const Setting kShared[] = {Setting::FastLoading, Setting::FastActions, Setting::FastWalk,  Setting::QuickEnemy,
                           Setting::FastAttack,  Setting::FastSpells,  Setting::SmoothPacing};

bool spawn(int port) {
    const std::string exe = self_exe();
    const std::string rom = arg_value("--rom");
    if (exe.empty() || rom.empty()) {
        std::fprintf(stderr, "vs-cpu: no %s to start the computer's game with\n", exe.empty() ? "program path" : "ROM");
        return false;
    }
    std::error_code ec;
    const std::string save = arg_value("--save-path");
    if (!save.empty() && std::filesystem::exists(save, ec))
        std::filesystem::copy_file(save, g_dir + "/totkol.sav", std::filesystem::copy_options::overwrite_existing, ec);

    std::vector<std::string> argv = {exe, "--rom", rom};
    if (const std::string b = arg_value("--bios"); !b.empty()) { argv.push_back("--bios"); argv.push_back(b); }
    else if (arg_flag("--no-bios")) argv.push_back("--no-bios");
    if (const std::string m = arg_value("--rom-patch"); !m.empty()) { argv.push_back("--rom-patch"); argv.push_back(m); }
    for (const char* a : {"--save-path"}) { argv.push_back(a); argv.push_back(g_dir + "/totkol.sav"); }
    argv.insert(argv.end(), {"--load-state", g_state, "--window", "--scale", "1"});

    // Its environment: the player's, less every game, runtime and SDL switch,
    // then the copy's own. KOL_VS_CHILD_ENV="A=1 B=2" adds more (tests).
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string v = *e;
        if (v.rfind("KOL_", 0) == 0 || v.rfind("GBARECOMP_", 0) == 0 || v.rfind("SDL_", 0) == 0) continue;
        env.push_back(v);
    }
    std::string on, off;
    for (const Setting s : kShared) {
        std::string& to = setting_on(s) ? on : off;
        to += (to.empty() ? "" : ",") + std::string(setting_key(s));
    }
    env.insert(env.end(), {"KOL_VS_OPPONENT=1", "GBARECOMP_LINK=child:" + std::to_string(port),
                           "GBARECOMP_SELFHEAL_RECOMPILE=0", "SDL_VIDEODRIVER=dummy",
                           "SDL_RENDER_DRIVER=software", "SDL_AUDIODRIVER=dummy",
                           "KOL_SETTINGS_ON=" + on, "KOL_SETTINGS_OFF=" + off});
    if (const char* extra = std::getenv("KOL_VS_CHILD_ENV"); extra && *extra) {
        std::string s = extra;
        for (std::size_t at = 0; at < s.size();) {
            const std::size_t sp = s.find(' ', at);
            const std::string t = s.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
            if (!t.empty()) env.push_back(t);
            if (sp == std::string::npos) break;
            at = sp + 1;
        }
    }

    std::vector<char*> av, ev;
    for (auto& a : argv) av.push_back(a.data());
    av.push_back(nullptr);
    for (auto& e : env) ev.push_back(e.data());
    ev.push_back(nullptr);
    const std::string log = g_dir + "/opponent.log";
    const int log_fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const std::string dir = g_dir;

    const pid_t pid = ::fork();
    if (pid < 0) {
        if (log_fd >= 0) ::close(log_fd);
        std::fprintf(stderr, "vs-cpu: could not start the computer's game\n");
        return false;
    }
    if (pid == 0) {   // (only async-signal-safe calls from here)
        if (log_fd >= 0) {
            ::dup2(log_fd, 1);
            ::dup2(log_fd, 2);
        }
        const int null_fd = ::open("/dev/null", O_RDONLY);
        if (null_fd >= 0) ::dup2(null_fd, 0);
        if (::chdir(dir.c_str()) != 0) ::_exit(126);
        ::execve(av[0], av.data(), ev.data());
        ::_exit(127);
    }
    if (log_fd >= 0) ::close(log_fd);
    g_pid = pid;
    std::fprintf(stderr, "vs-cpu: the computer's game started (pid %d, link port %d, log %s)\n", static_cast<int>(pid),
                 port, log.c_str());
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

// The session over (under g_mu): the link stopped, the copy gone, the game
// let go if it was held.
void end_session(const char* why) {
    std::fprintf(stderr, "vs-cpu: session ends: %s\n", why);
    gba::SioLink::stop();
    reap_child();
    gbarecomp::g_runtime_pause_request = 0;
    g_phase.store(kIdle);
}

// While the game is held: linked, or given up on.
void check_linking() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_phase.load() != kLinking) return;
    gba::SioLink* l = gba::SioLink::instance();
    if (l && l->connected()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                              g_link_start).count();
        std::fprintf(stderr, "vs-cpu: linked to the computer's game after %lld ms\n", static_cast<long long>(ms));
        g_phase.store(kLinked);
        g_off = 0;
        gbarecomp::g_runtime_pause_request = 0;
        return;
    }
    int st = 0;
    const bool gone = g_pid > 0 && ::waitpid(g_pid, &st, WNOHANG) == g_pid;
    if (gone) g_pid = -1;
    if (gone || std::chrono::steady_clock::now() - g_link_start > kLinkWait) {
        end_session(gone ? "the computer's game ended before linking" : "no link in 20 s");
        g_failed_here = true;
    }
}

std::uint16_t filter(std::uint16_t keys) {
    if (g_prev_filter) keys = g_prev_filter(keys);
    if (g_phase.load(std::memory_order_relaxed) == kLinking) check_linking();
    return keys;
}

void at_exit() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_phase.load() != kIdle || g_pid > 0) {
        gba::SioLink::stop();
        reap_child();
    }
}

// The copy: ends with its link, its parent, or no link at all.
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
            std::fprintf(stderr, "vs-cpu: the computer's game ends: %s\n", why);
            std::fflush(stderr);
            std::fflush(stdout);
            std::_Exit(code);
        }
    }
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
    if (g_args.empty()) return;   // not the desktop's main (no command line to give the copy)
    g_prev_filter = gbarecomp::g_host_keyinput_filter;
    gbarecomp::g_host_keyinput_filter = &filter;
    std::atexit(at_exit);
}

void vs_cpu_tick() {
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
    const bool connecting = rd32(ew + kWorld) == kConnecting && ew[kMode] != 4u;
    switch (g_phase.load()) {
    case kIdle: {
        if (!connecting) {
            g_failed_here = false;
            break;
        }
        if (g_failed_here || !multi || !setting_on(Setting::VsComputer) || gba::SioLink::instance()) break;
        std::lock_guard<std::mutex> lock(g_mu);
        std::error_code ec;
        static int session = 0;
        const auto dir = std::filesystem::temp_directory_path(ec) /
                         ("kol-vs-" + std::to_string(::getpid()) + "-" + std::to_string(++session));
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::fprintf(stderr, "vs-cpu: no folder for the computer's game (%s)\n", ec.message().c_str());
            g_failed_here = true;
            break;
        }
        g_dir = dir.string();
        g_state = g_dir + "/vs.state";
        g_wait = 0;
        g_phase.store(kSaving);
        std::fprintf(stderr, "vs-cpu: Connecting: a snapshot for the computer's game (%s)\n", g_dir.c_str());
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
    case kLinking:
        check_linking();
        break;
    case kLinked: {
        std::lock_guard<std::mutex> lock(g_mu);
        gba::SioLink* l = gba::SioLink::instance();
        if (!l || !l->connected()) {
            end_session("the link to the computer's game was lost");
            g_failed_here = connecting;
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
    if (slot != kSlot || !save || g_phase.load() != kSaving) return nullptr;
    return g_state.c_str();
}

bool vs_cpu_state_saved(const char* path) {
    if (g_phase.load() != kSaving || !path || g_state != path) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    const int port = ppu ? gba::SioLink::start_parent(ppu->guest_cycle()) : -1;
    if (port < 0 || !spawn(port)) {
        end_session(port < 0 ? "no link could be started" : "the computer's game could not be started");
        g_failed_here = true;
        return true;
    }
    g_link_start = std::chrono::steady_clock::now();
    g_phase.store(kLinking);
    gbarecomp::g_runtime_pause_request = 1;   // held until the copy says hello (check_linking)
    return true;
}

}  // namespace kol

#endif
