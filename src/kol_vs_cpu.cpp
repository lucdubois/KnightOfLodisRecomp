// kol_vs_cpu.cpp — VS Mode's other GBA: the computer, or a player on the
// local network (kol_vs_cpu.h, kol_vs_net.h).
//
// The player's game:
//   1. VS Mode's "Connecting..." asks the runtime for a state save into its
//      own slot, taken at the game's next pad read (kol_vs_net's pad hook,
//      before the frame's logic: the same instruction on every machine; from
//      the present it landed wherever each host next yielded, after the link
//      routines' decision on the Deck and the Thor, 2026-11-23). The screen: the menu's screen
//      0x08246C18 (0x02029B80), its cursor on VS (0x03000ED1 1), 0x02002830
//      0x24, outside a battle, no link; its first frame, the port in any mode.
//      Exchange's Connecting is the same screen with the cursor on 2 (the
//      same bytes, logged 2026-11-23): taken over the network only (asked:
//      an exchange with the computer makes no sense), and paired only with a
//      game that chose Exchange too (vsnet::Side::kind). Not VS's world
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


#include "net_compat.h"   // POSIX sockets, or Winsock (2026-11-23: Windows too)
#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <algorithm>

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
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "sio_link.h"

#if !defined(_WIN32)
extern char** environ;
#endif

namespace kol {
namespace net = gba::net;
namespace {

constexpr int kSlot = 0x5653;                       // "VS": the snapshot's slot (its path is ours)
constexpr std::uint32_t kWorld = 0x29BD0;           // EWRAM: the world state's pointer (VS's NAVI pages on: logs only)
constexpr std::uint32_t kScreen = 0x29B80;          // EWRAM: the Quest/VS/Exchange menu's screen
constexpr std::uint32_t kConnecting = 0x08246C18u;  // ... "Connecting..." (0x08246CA8 the menu itself)
constexpr std::uint32_t kCursor = 0xED1;            // IWRAM: the menu's cursor, kept on Connecting: 0 Quest, 1 VS, 2 Exchange
constexpr std::uint32_t kMode = 0x12E0;             // EWRAM: 4 from the battle's preparation on
constexpr std::uint32_t kScene = 0x2830;            // EWRAM: 0x24 and +6 0x1A on Connecting, 0x46 VS's NAVI pages
                                                    // (the screen pointer is 0x08246C18 from the menu's A on)
constexpr std::uint32_t kLinkInit = 0x0800601Cu;     // the game's link routines set up (struct 0x03002A10 cleared,
constexpr std::uint32_t kLinkInitFromConnecting = 0x0814FB2Cu;   // the port to Multi-Player) by Connecting's code
constexpr std::uint32_t kLinkState = 0x2A11;       // IWRAM: the game's link routines' state byte (0 until their
                                                    // first call, which decides parent or child once: 0x08006210)
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
bool g_exchange = false;         // ... and is Exchange's, not VS's (always over the network)
std::atomic<bool> g_save_asked{false};   // kSaving: the save asked for at a pad read (on_pad_read)
bool g_keep_dir = false;         // the session's folder outlives it (a desync)
long g_pid = -1;                 // the copy's process id (-1: none, or the platform's)
std::atomic<long> g_copy_pid{-1};   // the copy, for the session thread (vsnet::set_on_gone)
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
VsPlatform g_platform;           // Android: how a copy starts, the Wi-Fi's multicast lock

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
#if defined(_WIN32)
    wchar_t buf[32768];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, 32768);
    if (n == 0 || n >= 32768) return {};
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), s.data(), len, nullptr, nullptr);
    return s;
#elif defined(__APPLE__)
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

// ── The copy's process (desktop): fork + execve, or CreateProcess ────────
// proc_start: the copy running with `argv`, the player's environment less
// every game, runtime and SDL switch plus `own`, its output to `log`, in
// `dir`; its process id, or -1. proc_exited: it has ended (reaped).
// proc_stop: ended for sure (asked first where the system can ask).
// proc_kill: ended at once, from any thread. self_pid: this process's.
#if defined(_WIN32)
HANDLE g_proc = nullptr;   // the copy's process (under g_mu)

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
// One argument as the C runtime reads it back (CommandLineToArgvW's rules).
std::wstring quoted(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') ++i, ++slashes;
        if (i == a.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        out.push_back(a[i]);
    }
    out.push_back(L'"');
    return out;
}
// An environment entry's name ("=C:" entries keep their leading '=').
std::wstring var_name(const std::wstring& v) {
    const std::size_t eq = v.find(L'=', 1);
    return eq == std::wstring::npos ? v : v.substr(0, eq);
}
bool switch_var(const std::wstring& v) {
    const std::wstring n = var_name(v);
    for (const wchar_t* p : {L"KOL_", L"GBARECOMP_", L"SDL_"})
        if (::_wcsnicmp(n.c_str(), p, std::wcslen(p)) == 0) return true;
    return false;
}

long proc_start(const std::vector<std::string>& argv, const std::vector<std::string>& own, const std::string& log,
                const std::string& dir) {
    std::wstring cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quoted(wide(a));
    }
    std::vector<std::wstring> vars;
    if (wchar_t* env = ::GetEnvironmentStringsW()) {
        for (const wchar_t* e = env; *e; e += std::wcslen(e) + 1)
            if (!switch_var(e)) vars.emplace_back(e);
        ::FreeEnvironmentStringsW(env);
    }
    for (const auto& o : own) vars.push_back(wide(o));
    // (A block sorted by name, as Windows keeps its own.)
    std::stable_sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        return ::_wcsicmp(var_name(a).c_str(), var_name(b).c_str()) < 0;
    });
    std::wstring block;
    for (const auto& v : vars) block.append(v).push_back(L'\0');
    block.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = ::CreateFileW(wide(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = out;
    // Those two handles only, not every inheritable one this process holds.
    HANDLE list[2];
    DWORD listed = 0;
    if (in != INVALID_HANDLE_VALUE) list[listed++] = in;
    if (out != INVALID_HANDLE_VALUE) list[listed++] = out;
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attr(size ? size : 1);
    auto* al = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr.data());
    bool have_list = listed > 0 && size > 0 && ::InitializeProcThreadAttributeList(al, 1, 0, &size);
    if (have_list && !::UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                                  listed * sizeof(HANDLE), nullptr, nullptr)) {
        ::DeleteProcThreadAttributeList(al);
        have_list = false;
    }
    if (have_list) si.lpAttributeList = al;
    PROCESS_INFORMATION pi{};
    const std::wstring exe = wide(argv[0]), wdir = wide(dir);
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, listed > 0 ? TRUE : FALSE,
                                     CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                         (have_list ? EXTENDED_STARTUPINFO_PRESENT : 0),
                                     block.data(), wdir.c_str(), &si.StartupInfo, &pi);
    if (have_list) ::DeleteProcThreadAttributeList(al);
    if (out != INVALID_HANDLE_VALUE) ::CloseHandle(out);
    if (in != INVALID_HANDLE_VALUE) ::CloseHandle(in);
    if (!ok) {
        std::fprintf(stderr, "vs-cpu: CreateProcess failed (error %lu)\n", static_cast<unsigned long>(::GetLastError()));
        return -1;
    }
    ::CloseHandle(pi.hThread);
    if (g_proc) ::CloseHandle(g_proc);
    g_proc = pi.hProcess;
    return static_cast<long>(pi.dwProcessId);
}
bool proc_exited(long) {
    if (!g_proc || ::WaitForSingleObject(g_proc, 0) != WAIT_OBJECT_0) return false;
    ::CloseHandle(g_proc);
    g_proc = nullptr;
    return true;
}
void proc_stop(long) {
    if (!g_proc) return;
    // (Windows has no polite ask a windowless program reads: ended at once.)
    if (::WaitForSingleObject(g_proc, 0) != WAIT_OBJECT_0) {
        ::TerminateProcess(g_proc, 1);
        ::WaitForSingleObject(g_proc, 2000);
    }
    ::CloseHandle(g_proc);
    g_proc = nullptr;
}
void proc_kill(long pid) {
    if (HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid))) {
        ::TerminateProcess(h, 1);
        ::CloseHandle(h);
    }
}
long self_pid() { return static_cast<long>(::GetCurrentProcessId()); }
#elif !defined(__ANDROID__)
long proc_start(const std::vector<std::string>& argv, const std::vector<std::string>& own, const std::string& log,
                const std::string& dir) {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string v = *e;
        if (v.rfind("KOL_", 0) == 0 || v.rfind("GBARECOMP_", 0) == 0 || v.rfind("SDL_", 0) == 0) continue;
        env.push_back(v);
    }
    env.insert(env.end(), own.begin(), own.end());
    std::vector<std::string> args = argv;
    std::vector<char*> av, ev;
    for (auto& a : args) av.push_back(a.data());
    av.push_back(nullptr);
    for (auto& e : env) ev.push_back(e.data());
    ev.push_back(nullptr);
    const int log_fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const pid_t pid = ::fork();
    if (pid < 0) {
        if (log_fd >= 0) ::close(log_fd);
        return -1;
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
    return static_cast<long>(pid);
}
bool proc_exited(long pid) {
    int st = 0;
    return pid > 0 && ::waitpid(static_cast<pid_t>(pid), &st, WNOHANG) == static_cast<pid_t>(pid);
}
void proc_stop(long pid) {
    const pid_t p = static_cast<pid_t>(pid);
    int st = 0;
    pid_t r = ::waitpid(p, &st, WNOHANG);
    if (r == 0) {
        ::kill(p, SIGTERM);
        for (int i = 0; i < 50 && r == 0; ++i) {   // up to 0.5 s, then for sure
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            r = ::waitpid(p, &st, WNOHANG);
        }
        if (r == 0) {
            ::kill(p, SIGKILL);
            ::waitpid(p, &st, 0);
        }
    }
}
void proc_kill(long pid) { ::kill(static_cast<pid_t>(pid), SIGKILL); }
long self_pid() { return static_cast<long>(::getpid()); }
#else
// (Android: the platform's service runs the copy; there is no process of
// ours to wait for or end.)
bool proc_exited(long) { return false; }
void proc_stop(long) {}
void proc_kill(long) {}
long self_pid() { return static_cast<long>(::getpid()); }
#endif

struct Spawn {
    std::string link;                 // GBARECOMP_LINK: "child:<port>" or "parent:<port>"
    std::string state, save;          // what it starts from
    std::vector<std::string> env;     // its own switches
    const char* who = "the computer's game";
};

bool spawn(const Spawn& sp) {
#if defined(__ANDROID__)
    const std::string exe = "KnightOfLodisRecomp";   // (argv[0] only: the platform starts the copy)
#else
    const std::string exe = self_exe();
#endif
    const std::string rom = arg_value("--rom");
    if (exe.empty() || rom.empty()) {
        std::fprintf(stderr, "vs-cpu: no %s to start %s with\n", exe.empty() ? "program path" : "ROM", sp.who);
        return false;
    }
    std::vector<std::string> argv = {exe, "--rom", rom};
    if (const std::string b = arg_value("--bios"); !b.empty()) { argv.push_back("--bios"); argv.push_back(b); }
    else if (arg_flag("--no-bios")) argv.push_back("--no-bios");
    if (const std::string m = arg_value("--rom-patch"); !m.empty()) { argv.push_back("--rom-patch"); argv.push_back(m); }
    argv.insert(argv.end(), {"--save-path", sp.save, "--load-state", sp.state});
    // No window at all (as an Android service must run it; in step with a
    // seen game, 142 hashes, 2026-11-23), and a frame count, or a headless
    // run stops after one: the copy's own rules end it.
    // KOL_VS_COPY_WINDOW=1: a window SDL never shows (dummy video), as before.
    if (const char* w = std::getenv("KOL_VS_COPY_WINDOW"); w && *w == '1')
        argv.insert(argv.end(), {"--window", "--scale", "1"});
    else
        argv.insert(argv.end(), {"--no-window", "--frames", "2000000000"});

    // Its own switches: unseen (gbarecomp GBARECOMP_UNSEEN: no frame
    // composed or presented, no pacing, no audio device; it drew every
    // picture before, in software, at its owner's effects: a network copy
    // took more of the Deck than the game played on it), and the copy's
    // own. KOL_VS_CHILD_ENV="A=1 B=2" adds more (tests).
    std::vector<std::string> own = {"GBARECOMP_LINK=" + sp.link, "GBARECOMP_SELFHEAL_RECOMPILE=0", "GBARECOMP_UNSEEN=1",
                                    "SDL_VIDEODRIVER=dummy", "SDL_RENDER_DRIVER=software", "SDL_AUDIODRIVER=dummy",
                                    "KOL_VS_PARENT_PID=" + std::to_string(self_pid())};
    own.insert(own.end(), sp.env.begin(), sp.env.end());
    if (const char* extra = std::getenv("KOL_VS_CHILD_ENV"); extra && *extra) {
        std::string s = extra;
        for (std::size_t at = 0; at < s.size();) {
            const std::size_t sp_at = s.find(' ', at);
            const std::string t = s.substr(at, sp_at == std::string::npos ? std::string::npos : sp_at - at);
            if (!t.empty()) own.push_back(t);
            if (sp_at == std::string::npos) break;
            at = sp_at + 1;
        }
    }
    const std::string log = g_dir + (g_net ? "/other_player.log" : "/opponent.log");

#if defined(__ANDROID__)
    // An app's process cannot start itself again: the platform runs the copy
    // in a service of its own process (main_android, VsCopyService.java),
    // with these arguments and switches. Its process id is not ours to wait
    // for: it ends itself when its link or its channel closes.
    if (!g_platform.spawn || !g_platform.spawn(argv, own, log, g_dir)) {
        std::fprintf(stderr, "vs-cpu: could not start %s (no platform service)\n", sp.who);
        return false;
    }
    g_pid = -1;
    std::fprintf(stderr, "vs-cpu: %s started (service, link %s, log %s)\n", sp.who, sp.link.c_str(), log.c_str());
    return true;
#else
    const long pid = proc_start(argv, own, log, g_dir);
    if (pid < 0) {
        std::fprintf(stderr, "vs-cpu: could not start %s\n", sp.who);
        return false;
    }
    g_pid = pid;
    g_copy_pid.store(pid);
    std::fprintf(stderr, "vs-cpu: %s started (pid %ld, link %s, log %s)\n", sp.who, pid, sp.link.c_str(),
                 log.c_str());
    return true;
#endif
}

// The copy stopped and the session's folder gone (under g_mu).
void reap_child() {
    if (g_pid > 0) {
        proc_stop(g_pid);
        g_pid = -1;
        g_copy_pid.store(-1);
    }
    if (!g_dir.empty() && g_keep_dir)
        std::fprintf(stderr, "vs-cpu: the session's folder is kept (the games went out of step): %s\n", g_dir.c_str());
    if (!g_dir.empty() && !std::getenv("KOL_VS_KEEP") && !g_keep_dir) {   // KOL_VS_KEEP=1: keep it (its log)
        std::error_code ec;
        std::filesystem::remove_all(g_dir, ec);
    }
    g_dir.clear();
    g_state.clear();
}

void notice(const char* line) {
    std::fprintf(stderr, "vs-cpu: notice: %s\n", line);
    cheevos_notice(g_exchange ? "Exchange" : "VS Mode", line);
}

// The session over (under g_mu): the network and the link stopped, the copy
// gone, the game let go if it was held.
void wifi_lock(bool on) {
    static bool held = false;
    if (held == on || !g_platform.wifi_lock) return;
    held = on;
    g_platform.wifi_lock(on);
}

void end_session(const char* why) {
    std::fprintf(stderr, "vs-cpu: session ends: %s\n", why);
    wifi_lock(false);
    // Out of step, here or there: both sides keep their last memory
    // snapshots (vsnet ring) and logs, to be compared.
    if (why && std::strstr(why, "desync")) g_keep_dir = true;
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
    const int s = net::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    int port = -1;
    if (net::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 &&
        net::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0)
        port = ntohs(a.sin_port);
    net::close(s);
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
    // The channel to it (kol_vs_net): this game listens on a local port,
    // the copy connects once it runs; taken on a thread of its own.
    const int ls = net::socket(AF_INET, SOCK_STREAM, 0);
    int channel_port = -1;
    if (ls >= 0) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof a;
        if (net::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && net::listen(ls, 1) == 0 &&
            net::getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len) == 0)
            channel_port = ntohs(a.sin_port);
    }
    if (channel_port <= 0) {
        if (ls >= 0) net::close(ls);
        end_session("no channel to the other player's game");
        return false;
    }
#if !defined(_WIN32)
    ::signal(SIGPIPE, SIG_IGN);   // a copy gone: a write to its channel fails, not the game
#endif
    std::thread([ls] {
        pollfd p = net::poll_entry(ls, POLLIN);
        int fd = -1;
        if (net::poll(&p, 1, 20000) > 0) fd = net::accept(ls);
        net::close(ls);
        if (fd < 0) {
            std::fprintf(stderr, "vs-cpu: the other player's game never took its channel\n");
            return;
        }
        int one = 1;
        net::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        net::no_sigpipe(fd);
        vsnet::set_key_sink(fd);
    }).detach();

    Spawn sp;
    sp.state = state;
    sp.save = save;
    sp.who = "the other player's game";
    char panel[32];
    std::snprintf(panel, sizeof panel, "%ux%u", peer.panel_w, peer.panel_h);
    sp.env = {"KOL_VS_MIRROR=1", "KOL_VS_MIRROR_CHANNEL=" + std::to_string(channel_port), "KOL_VS_MIRROR_CFG=" + cfg_dir,
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
    if (!ok) {
        end_session("the other player's game could not be started");
        return false;
    }
    // This game from its snapshot loaded too, the way its copy over there
    // starts (what ran here since the snapshot is undone, and its pad reads
    // are counted from the load on: vs_cpu_state_path).
    gbarecomp::runtime_request_state_load(kSlot);
    vsnet::play_start_keys();
    g_link_start = std::chrono::steady_clock::now();
    g_phase.store(kLinking);
    std::string line = (g_exchange ? "Exchanging with " : "Playing ") +
                       (peer.name.empty() ? std::string("a player") : peer.name);
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
    const bool gone = g_pid > 0 && proc_exited(g_pid);
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
        wifi_lock(false);
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

// Under g_mu: a session's folder and phase; its snapshot is asked for by the
// caller (VS: at the next pad read; Exchange: at once).
bool begin_session(bool net, bool exchange) {
    std::error_code ec;
    static int session = 0;
    const auto dir = std::filesystem::temp_directory_path(ec) /
                     ("kol-vs-" + std::to_string(self_pid()) + "-" + std::to_string(++session));
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "vs-cpu: no folder for the session (%s)\n", ec.message().c_str());
        return false;
    }
    g_net = net;
    g_exchange = exchange;
    g_keep_dir = false;
    g_dir = dir.string();
    g_state = g_dir + "/vs.state";
    g_wait = 0;
    g_save_asked.store(false);
    g_phase.store(kSaving);
    std::fprintf(stderr, "vs-cpu: %s's Connecting (%s): a snapshot (%s)\n", exchange ? "Exchange" : "VS",
                 net ? "network" : "computer", g_dir.c_str());
    return true;
}

// Exchange starts where the game sets up its link routines (0x0800601C,
// called by Connecting's code 0x0814FB2C: it clears their struct at
// 0x03002A10 and puts the port in Multi-Player mode), on the player's A,
// first time and after a time-out alike. The routines decide parent or child
// once, at their first call a frame or two later; the snapshot asked for here
// is taken at this frame's end (the guest's next yield), before it. Keyed on
// the set-up, not the screen: Connecting's screen pointer (0x02029B80) is also
// set by the title's menu, with the cursor byte still on Exchange, which
// started a search on the title (2026-11-23, the Thor), and it does not change
// when Exchange is chosen again after a time-out.
void (*g_prev_entry)(std::uint32_t) = nullptr;
void on_fn_entry(std::uint32_t pc) {
    if (g_prev_entry) g_prev_entry(pc);
    if (pc != kLinkInit || (g_cpu.R[14] & ~1u) != kLinkInitFromConnecting) return;
    if (vs_mode_choice_current() != 1u || g_phase.load() != kIdle || gba::SioLink::instance()) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    const std::uint8_t* ew = bus ? bus->ewram_ptr() : nullptr;
    const std::uint8_t* iw = bus ? bus->iwram_ptr() : nullptr;
    if (!ew || !iw || rd32(ew + kScreen) != kConnecting || iw[kCursor] != 2u || ew[kMode] == 4u) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!begin_session(true, true)) return;
    g_save_asked.store(true);
    gbarecomp::runtime_request_state_save_now(kSlot);
}

// Installed once VS Mode is Network (a call at every function entry).
void entry_hook_install() {
    static bool done = false;
    if (done || g_args.empty()) return;
    done = true;
    g_prev_entry = g_runtime_fn_entry_hook;
    g_runtime_fn_entry_hook = &on_fn_entry;
}

// The game's pad read (kol_vs_net's hook, on the guest thread), before the
// frame's logic: Connecting's snapshot is asked for here. The runtime takes it
// at the guest's next yield, which compiled code reaches at the end of the
// frame's logic (the VBlank wait, 0x080007A4; measured 2026-11-23): a point of
// the game's own, the same on every machine. (Asked from the present, it
// landed wherever each host next yielded.)
void on_pad_read() {
    if (g_phase.load(std::memory_order_relaxed) != kSaving || g_save_asked.load()) return;
    g_save_asked.store(true);
    gbarecomp::runtime_request_state_save_now(kSlot);
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
    // The player's game: gone when its process ends (Windows: the process
    // it named, KOL_VS_PARENT_PID; elsewhere: this one's parent changes).
#if defined(_WIN32)
    const char* pp = std::getenv("KOL_VS_PARENT_PID");
    const DWORD parent_pid = pp && *pp ? static_cast<DWORD>(std::strtoul(pp, nullptr, 10)) : 0;
    HANDLE parent = parent_pid ? ::OpenProcess(SYNCHRONIZE, FALSE, parent_pid) : nullptr;
    auto parent_gone = [&] { return parent && ::WaitForSingleObject(parent, 0) == WAIT_OBJECT_0; };
#else
    const pid_t parent = ::getppid();
    auto parent_gone = [&] { return ::getppid() != parent; };
#endif
    const auto t0 = std::chrono::steady_clock::now();
    bool was = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const char* why = nullptr;
        int code = 0;
        gba::SioLink* l = gba::SioLink::instance();
        const bool on = l && l->connected();
        if (parent_gone()) why = "the player's game is gone";
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
    net::init();   // (Winsock's gethostname)
    if (::gethostname(b, sizeof b - 1) != 0) return {};
    std::string s = b;
    if (const auto dot = s.find(".local"); dot != std::string::npos) s.resize(dot);
    return s;
}

}  // namespace

void vs_set_platform(const VsPlatform& p) { g_platform = p; }

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
    vsnet::set_pad_read_hook(&on_pad_read);
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
    // VS's "Connecting..." (Exchange's is the same screen, cursor 2: over the
    // network only, VS Mode = Network).
    const unsigned cursor = iw[kCursor];
    const bool net_chosen = vs_mode_choice_current() == 1u;
    const std::uint32_t screen = rd32(ew + kScreen);
    const bool connecting = screen == kConnecting && (cursor == 1u || (cursor == 2u && net_chosen)) &&
                            ew[kMode] != 4u;
    if (net_chosen) entry_hook_install();   // Exchange's start (on_fn_entry)
    // VS's snapshot comes at 0x2836 0x1A, before its link routines are set
    // up. Exchange's starts at their set-up instead (on_fn_entry).
    const bool connecting_screen = connecting && cursor == 1u && ew[kScene] == 0x24u && ew[kScene + 6] == 0x1Au;
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
        if (!begin_session(net_chosen, false)) g_failed_here = true;   // the save at the next pad read
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
        if (g_net && vsnet::peer_gone(&why)) {
            notice("The other player left.");
            end_session(why.c_str());
            break;
        }
        if (!l || !l->connected()) {
            end_session(g_net ? "the link to the other player's game was lost" : "the link to the computer's game was lost");
            g_failed_here = connecting;
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
    // Exchange: its link routines must not have decided yet (see the trigger).
    const std::uint8_t* iw = bus ? bus->iwram_ptr() : nullptr;
    if (std::getenv("KOL_VS_LOG") && iw)
        std::fprintf(stderr, "vs-log: snapshot at frame %llu: link state %u, 2830=%02X\n",
                     static_cast<unsigned long long>(g_snap_frame), iw[kLinkState], bus->ewram_ptr()[kScene]);
    if (g_exchange && iw && iw[kLinkState] != 0u) {
        notice("The exchange could not start. Try again.");
        end_session("Exchange's snapshot came after its link routines had decided");
        g_failed_here = true;
        return true;
    }
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
    // KOL_VS_NET_NONCE=n (tests): this side's number, so which one connects
    // (the smaller) is known (an emulator behind its own NAT can only connect out).
    if (const char* n = std::getenv("KOL_VS_NET_NONCE"); n && *n) mine.nonce = std::strtoull(n, nullptr, 0);
    g_my_nonce = mine.nonce;
    mine.kind = g_exchange ? "exchange" : "vs";
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
    vsnet::set_on_gone([] {
        // SIGKILL: SDL makes SIGTERM a quit event, which a copy held in its
        // wait for buttons never reads.
        if (const long p = g_copy_pid.load(); p > 0) proc_kill(p);
    });
    if (mine.state.empty() || !vsnet::start(std::move(mine))) {
        end_session("the network could not be started");
        g_failed_here = true;
        return true;
    }
    vsnet::play_begin(keyinput, g_dir);
    wifi_lock(true);   // (Android: broadcasts reach the game only while it is held)
    g_search_start = std::chrono::steady_clock::now();
    g_notice_at = g_search_start;
    g_b_was = true;
    g_phase.store(kSearching);
    gbarecomp::g_runtime_pause_request = 1;   // held on Connecting while it looks (check_search)
    return true;
}

}  // namespace kol
